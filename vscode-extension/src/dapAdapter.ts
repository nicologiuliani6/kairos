import {
    LoggingDebugSession,
    InitializedEvent,
    StoppedEvent,
    TerminatedEvent,
    StackFrame,
    Scope,
    Source,
    Thread,
    OutputEvent
} from '@vscode/debugadapter';
import { DebugProtocol } from '@vscode/debugprotocol';
import * as path from 'path';
import * as koffi from 'koffi';
import { execSync, spawnSync } from 'child_process';
import * as fs from 'fs';

/* ======================================================================
 *  IMPORTANTE: stdout è il canale DAP — NON scriverci mai direttamente.
 *  Tutto l'output (log, VM output) va su file o come DAP OutputEvent.
 * ====================================================================== */

const logFile = '/tmp/janus-dap.log';

function log(msg: string) {
    fs.appendFileSync(logFile, new Date().toISOString() + ' ' + msg + '\n');
    // NON usare console.log (→ stdout → corrompe DAP)
}

log('DAP adapter avviato');

/* ======================================================================
 *  Caricamento libvm_dap.so tramite koffi
 * ====================================================================== */

const LIB_PATH = path.join(__dirname, '../../build/libvm_dap.so');

let lib: any;
let vm_debug_new: () => any;
let vm_debug_free: (dbg: any) => void;
let vm_debug_start: (bytecode: string, dbg: any) => void;
let vm_debug_stop: (dbg: any) => void;
let vm_debug_step: (dbg: any) => number;
let vm_debug_step_back: (dbg: any) => number;
let vm_debug_continue: (dbg: any) => number;
let vm_debug_set_breakpoint: (dbg: any, line: number) => void;
let vm_debug_clear_breakpoint: (dbg: any, line: number) => void;
let vm_debug_clear_all_breakpoints: (dbg: any) => void;
let vm_debug_vars_json_ext: (dbg: any, out: Buffer, outsz: number) => number;
let vm_debug_dump_json_ext: (dbg: any, out: Buffer, outsz: number) => number;
// Funzione C che restituisce l'output della VM (stdout della VM) accumulato
// Firma attesa: int vm_debug_output_ext(void* dbg, void* out, int outsz)
// Se non esiste nella libreria, verrà usato il fallback file-based (vedi sotto).
let vm_debug_output_ext: ((dbg: any, out: Buffer, outsz: number) => number) | null = null;

function loadLib() {
    log('Carico libvm_dap.so da: ' + LIB_PATH);
    try {
        lib = koffi.load(LIB_PATH);
        log('libvm_dap.so caricata OK');
    } catch (e: any) {
        log('ERRORE koffi.load: ' + e.message);
        throw e;
    }
    try {
        vm_debug_new              = lib.func('void* vm_debug_new()');
        vm_debug_free             = lib.func('void vm_debug_free(void*)');
        vm_debug_start            = lib.func('void vm_debug_start(str, void*)');
        vm_debug_stop             = lib.func('void vm_debug_stop(void*)');
        vm_debug_step             = lib.func('int  vm_debug_step(void*)');
        vm_debug_step_back        = lib.func('int  vm_debug_step_back(void*)');
        vm_debug_continue         = lib.func('int  vm_debug_continue(void*)');
        vm_debug_set_breakpoint   = lib.func('void vm_debug_set_breakpoint(void*, int)');
        vm_debug_clear_breakpoint = lib.func('void vm_debug_clear_breakpoint(void*, int)');
        vm_debug_clear_all_breakpoints = lib.func('void vm_debug_clear_all_breakpoints(void*)');
        vm_debug_vars_json_ext    = lib.func('int  vm_debug_vars_json_ext(void*, void*, int)');
        vm_debug_dump_json_ext    = lib.func('int  vm_debug_dump_json_ext(void*, void*, int)');
        log('Funzioni principali caricate OK');
    } catch (e: any) {
        log('ERRORE func binding: ' + e.message);
        throw e;
    }

    // Prova a caricare vm_debug_output_ext (opzionale — non tutte le versioni
    // della libreria la espongono)
    try {
        vm_debug_output_ext = lib.func('int vm_debug_output_ext(void*, void*, int)');
        log('vm_debug_output_ext caricata OK');
    } catch {
        log('vm_debug_output_ext non disponibile — uso fallback file-based');
        vm_debug_output_ext = null;
    }
}

/* ======================================================================
 *  Compilazione .janus → bytecode tramite janus.py
 *
 *  USA spawnSync invece di execSync così stderr della VM non va a finire
 *  nel file di log ma viene catturato e poi inviato come OutputEvent DAP.
 * ====================================================================== */

const VM_OUTPUT_FILE = '/tmp/janus-vm-output.txt';

function compileBytecode(janusFile: string): { bytecode: string; compileOutput: string } {
    const root   = path.join(__dirname, '../../');
    const venv   = path.join(root, 'venv/bin/python');
    const btFile = path.join(root, 'bytecode.txt');

    if (fs.existsSync(btFile)) fs.unlinkSync(btFile);

    // spawnSync cattura stdout+stderr invece di lasciarli passare al processo
    const result = spawnSync(
        'env',
        [
            `LD_PRELOAD=/usr/lib/gcc/x86_64-linux-gnu/14/libasan.so`,
            venv,
            '-m', 'src.frontend.bytecode',   // ← usa il __main__ già in bytecode.py
            janusFile,
        ],
        { cwd: root, encoding: 'utf-8', maxBuffer: 10 * 1024 * 1024 }
    );

    const compileOutput = [result.stdout || '', result.stderr || '']
        .filter(Boolean)
        .join('\n')
        .trim();

    if (!fs.existsSync(btFile)) {
        throw new Error(
            `Compilazione fallita (nessun bytecode.txt):\n${compileOutput}`
        );
    }

    if (result.status !== 0) {
        log(`WARN compilazione exit ${result.status} (ignorato, bytecode.txt presente)`);
    }

    return { bytecode: fs.readFileSync(btFile, 'utf-8'), compileOutput };
}

/* ======================================================================
 *  Lettura output VM
 *
 *  Strategia 1 (preferita): vm_debug_output_ext() nella libreria C —
 *    la VM accumula il suo stdout in un buffer interno e lo espone qui.
 *
 *  Strategia 2 (fallback): la VM scrive su VM_OUTPUT_FILE; lo leggiamo
 *    dopo ogni step/continue e lo svuotiamo (truncate).
 *
 *  In entrambi i casi il risultato viene inviato come DAP OutputEvent
 *  ("stdout" category) così appare nel pannello DEBUG CONSOLE di VS Code.
 * ====================================================================== */

function readVmOutput(dbg: any): string {
    // Strategia 1
    if (vm_debug_output_ext && dbg) {
        try {
            const buf = Buffer.alloc(1024 * 1024); // 1 MB
            const n = vm_debug_output_ext(dbg, buf, buf.length);
            if (n > 0) return buf.toString('utf-8', 0, n);
            return '';
        } catch (e: any) {
            log('WARN vm_debug_output_ext fallito: ' + e.message);
        }
    }

    // Strategia 2 — file-based
    try {
        if (!fs.existsSync(VM_OUTPUT_FILE)) return '';
        const content = fs.readFileSync(VM_OUTPUT_FILE, 'utf-8');
        if (!content) return '';
        // Svuota il file dopo la lettura
        fs.writeFileSync(VM_OUTPUT_FILE, '');
        return content;
    } catch {
        return '';
    }
}

/* ======================================================================
 *  JanusDebugSession
 * ====================================================================== */

interface LaunchArgs extends DebugProtocol.LaunchRequestArguments {
    program: string;
    stopOnEntry?: boolean;
}

class JanusDebugSession extends LoggingDebugSession {

    private dbg: any = null;
    private sourceFile: string = '';

    constructor() {
        super();
        this.setDebuggerLinesStartAt1(true);
        loadLib();
    }

    /* ── Helper: invia testo come OutputEvent nella Debug Console ── */
    private sendOutput(text: string, category: 'stdout' | 'stderr' | 'console' = 'stdout') {
        if (!text) return;
        // Assicura newline finale
        const body = text.endsWith('\n') ? text : text + '\n';
        this.sendEvent(new OutputEvent(body, category));
    }

    /* ── Helper: legge output VM e lo invia come OutputEvent ── */
    private flushVmOutput() {
        const out = readVmOutput(this.dbg);
        log('flushVmOutput: ' + JSON.stringify(out));  // ← aggiungi
        if (out) this.sendOutput(out, 'console');
    }

    /* ── Initialize ── */
    protected initializeRequest(
        response: DebugProtocol.InitializeResponse,
        _args: DebugProtocol.InitializeRequestArguments
    ) {
        response.body = response.body || {};
        response.body.supportsStepBack = true;
        response.body.supportsConfigurationDoneRequest = true;
        this.sendResponse(response);
        this.sendEvent(new InitializedEvent());
    }

    /* ── Launch ── */
    protected launchRequest(
        response: DebugProtocol.LaunchResponse,
        args: LaunchArgs
    ) {
        this.sourceFile = args.program;

        let bytecode: string;
        let compileOutput: string;
        try {
            ({ bytecode, compileOutput } = compileBytecode(this.sourceFile));
        } catch (e: any) {
            this.sendOutput(`Errore di compilazione:\n${e.message}`, 'stderr');
            this.sendErrorResponse(response, 1, `Compilazione fallita: ${e.message}`);
            return;
        }

        // Mostra l'output del compilatore nella Debug Console (es. warning)
        if (compileOutput) {
            this.sendOutput(`[compilatore]\n${compileOutput}\n`, 'console');
        }

        this.dbg = vm_debug_new();
        vm_debug_start(bytecode, this.dbg);

        // Eventuale output prodotto durante l'avvio (es. print all'inizio del programma)
        this.flushVmOutput();

        this.sendResponse(response);

        if (args.stopOnEntry !== true) {
            this.sendEvent(new StoppedEvent('entry', 1));
        } else {
            const line = vm_debug_continue(this.dbg);
            this.flushVmOutput();
            log(`launchRequest: auto-continue ritorna linea: ${line}`);
            if (line < 0) {
                log('Terminazione VM');
                this.flushVmOutput();          // flush esplicito prima di terminare
                this.sendEvent(new TerminatedEvent());
            } else {
                this.sendEvent(new StoppedEvent('breakpoint', 1));
            }
        }
    }

    /* ── Threads ── */
   protected threadsRequest(response: DebugProtocol.ThreadsResponse) {
        log('threadsRequest chiamato');
        response.body = { threads: [new Thread(1, 'main')] };
        this.sendResponse(response);
    }

    /* ── StackTrace ── */
    protected stackTraceRequest(
        response: DebugProtocol.StackTraceResponse,
        _args: DebugProtocol.StackTraceArguments
    ) {
        const state = this.getState();
        const src   = new Source(path.basename(this.sourceFile), this.sourceFile);
        const frame = new StackFrame(0, state.frame, src, state.line);
        response.body = { stackFrames: [frame], totalFrames: 1 };
        this.sendResponse(response);
    }

    /* ── Scopes ── */
    protected scopesRequest(
        response: DebugProtocol.ScopesResponse,
        _args: DebugProtocol.ScopesArguments
    ) {
        response.body = { scopes: [new Scope('Locals', 1, false)] };
        this.sendResponse(response);
    }

    /* ── Variables ── */
    protected variablesRequest(
        response: DebugProtocol.VariablesResponse,
        _args: DebugProtocol.VariablesArguments
    ) {
        const vars = this.getVariables();
        response.body = {
            variables: vars.map(v => ({
                name: v.name,
                value: JSON.stringify(v.value),
                variablesReference: 0
            }))
        };
        this.sendResponse(response);
    }

    /* ── Continue ── */
    protected continueRequest(
        response: DebugProtocol.ContinueResponse,
        _args: DebugProtocol.ContinueArguments
    ) {
        log('continueRequest CHIAMATO');   // ← primissima riga
        this.sendResponse(response);
        log('prima di vm_debug_continue');   // ← aggiungi
        let line: number;
        try {
            line = vm_debug_continue(this.dbg);
             log(`dopo vm_debug_continue, line=${line}`);   
        } catch (e: any) {
            log('CRASH in vm_debug_continue: ' + e.message);
            this.sendOutput(`CRASH vm_debug_continue: ${e.message}`, 'stderr');
            throw e;
        }

        // ← aggiungi questo
        const raw = readVmOutput(this.dbg);
        log(`continueRequest: raw output = ${JSON.stringify(raw)}, len = ${raw.length}`);
        if (raw) this.sendOutput(raw, 'console');

        if (line < 0) {
            log('Terminazione VM');
            this.sendEvent(new TerminatedEvent());
        } else {
            this.sendEvent(new StoppedEvent('breakpoint', 1));
        }
    }

    /* ── Next (step over) ── */
    protected nextRequest(
        response: DebugProtocol.NextResponse,
        _args: DebugProtocol.NextArguments
    ) {
        this.sendResponse(response);
        log('Chiamo vm_debug_step()');
        let line: number;
        try {
            line = vm_debug_step(this.dbg);
            log(`vm_debug_step ritorna linea: ${line}`);
        } catch (e: any) {
            log('CRASH in vm_debug_step: ' + e.message);
            this.sendOutput(`CRASH vm_debug_step: ${e.message}`, 'stderr');
            throw e;
        }
        this.flushVmOutput();

        if (line < 0) {
            log('Terminazione VM');
            this.sendEvent(new TerminatedEvent());
        } else {
            this.sendEvent(new StoppedEvent('step', 1));
        }
    }

    /* ── StepBack ── */
    protected stepBackRequest(
        response: DebugProtocol.StepBackResponse,
        _args: DebugProtocol.StepBackArguments
    ) {
        this.sendResponse(response);
        log('Chiamo vm_debug_step_back()');
        let line: number;
        try {
            line = vm_debug_step_back(this.dbg);
            log(`vm_debug_step_back ritorna linea: ${line}`);
        } catch (e: any) {
            log('CRASH in vm_debug_step_back: ' + e.message);
            this.sendOutput(`CRASH vm_debug_step_back: ${e.message}`, 'stderr');
            throw e;
        }
        // line < 0 = history esaurita, non è terminazione
        this.sendEvent(new StoppedEvent('step', 1));
    }

    /* ── SetBreakpoints ── */
    protected setBreakPointsRequest(
        response: DebugProtocol.SetBreakpointsResponse,
        args: DebugProtocol.SetBreakpointsArguments
    ) {
        if (this.dbg) {
            vm_debug_clear_all_breakpoints(this.dbg);
        }

        const bps = (args.breakpoints || []).map(bp => {
            if (this.dbg) vm_debug_set_breakpoint(this.dbg, bp.line);
            log(`Breakpoint set @ line ${bp.line}`);
            return { verified: true, line: bp.line };
        });

        response.body = { breakpoints: bps };
        this.sendResponse(response);
    }

    /* ── Disconnect ── */
    protected disconnectRequest(
        response: DebugProtocol.DisconnectResponse,
        _args: DebugProtocol.DisconnectArguments
    ) {
        if (this.dbg) {
            this.flushVmOutput();   // ← aggiungi questa riga
            vm_debug_stop(this.dbg);
            vm_debug_free(this.dbg);
            this.dbg = null;
        }
        this.sendResponse(response);
    }

    /* ── Helper: stato corrente dalla VM ── */
    private getState(): { line: number; frame: string } {
        if (!this.dbg) return { line: 0, frame: 'main' };
        const buf = Buffer.alloc(65536);
        vm_debug_dump_json_ext(this.dbg, buf, buf.length);
        try {
            const json = JSON.parse(buf.toString('utf-8').replace(/\0.*$/, ''));
            return { line: json.line, frame: json.frame };
        } catch {
            return { line: 0, frame: 'main' };
        }
    }

    /* ── Helper: variabili del frame corrente ── */
    private getVariables(): any[] {
        if (!this.dbg) return [];
        const buf = Buffer.alloc(65536);
        vm_debug_vars_json_ext(this.dbg, buf, buf.length);
        try {
            return JSON.parse(buf.toString('utf-8').replace(/\0.*$/, ''));
        } catch {
            return [];
        }
    }
}

/* ── Avvia la sessione DAP ── */
const session = new JanusDebugSession();
session.start(process.stdin, process.stdout);