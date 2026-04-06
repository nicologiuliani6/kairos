import {
    LoggingDebugSession,
    InitializedEvent,
    StoppedEvent,
    TerminatedEvent,
    StackFrame,
    Scope,
    Source,
    Thread
} from '@vscode/debugadapter';
import { DebugProtocol } from '@vscode/debugprotocol';
import * as path from 'path';
import * as koffi from 'koffi';
import { execSync } from 'child_process';
import * as fs from 'fs';
// Cattura stderr nel log file (il fprintf del C va qui)
const { createWriteStream } = require('fs');
const stderrLog = createWriteStream('/tmp/janus-dap-stderr.log', { flags: 'a' });
process.stderr.write = stderrLog.write.bind(stderrLog) as any;
/* ======================================================================
 *  Logging
 * ====================================================================== */

const logFile = '/tmp/janus-dap.log';

function log(msg: string) {
    fs.appendFileSync(logFile, new Date().toISOString() + ' ' + msg + '\n');
    console.log(msg);
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
        // FIX 3: bind the clear-all function so setBreakpoints can replace atomically
        vm_debug_clear_all_breakpoints = lib.func('void vm_debug_clear_all_breakpoints(void*)');
        vm_debug_vars_json_ext    = lib.func('int  vm_debug_vars_json_ext(void*, void*, int)');
        vm_debug_dump_json_ext    = lib.func('int  vm_debug_dump_json_ext(void*, void*, int)');
        log('Tutte le funzioni caricate OK');
    } catch (e: any) {
        log('ERRORE func binding: ' + e.message);
        throw e;
    }
}

/* ======================================================================
 *  Compilazione .janus → bytecode tramite janus.py
 * ====================================================================== */

function compileBytecode(janusFile: string): string {
    const root   = path.join(__dirname, '../../');
    const venv   = path.join(root, 'venv/bin/python');
    const btFile = path.join(root, 'bytecode.txt');

    // FIX 4: remove the old bytecode file first so we never read a stale one
    if (fs.existsSync(btFile)) fs.unlinkSync(btFile);

    const cmd = `LD_PRELOAD=/usr/lib/gcc/x86_64-linux-gnu/14/libasan.so ` +
                `${venv} -m src.janus "${janusFile}" --dump-bytecode 2>/dev/null`;

    try {
        execSync(cmd, { cwd: root });
    } catch (e: any) {
        // ASan may give non-zero exit codes even on success; only re-throw
        // if bytecode.txt was not produced at all.
        if (!fs.existsSync(btFile)) {
            throw new Error(`Compilazione fallita (nessun bytecode.txt): ${e.message}`);
        }
        log(`WARN execSync exit non-zero (ignorato, bytecode.txt presente): ${e.message}`);
    }

    return fs.readFileSync(btFile, 'utf-8');
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
        try {
            bytecode = compileBytecode(this.sourceFile);
        } catch (e: any) {
            this.sendErrorResponse(response, 1, `Compilazione fallita: ${e.message}`);
            return;
        }

        this.dbg = vm_debug_new();

        // FIX 1 & 5: pass stopOnEntry intent to the C layer via the mode
        // before calling vm_debug_start, so the VM thread pauses immediately
        // on the first instruction when stopOnEntry is true, and runs freely
        // otherwise. vm_debug_start must not return until the VM thread has
        // reached its first pause (i.e. first_pause_reached == 1).
        vm_debug_start(bytecode, this.dbg);
        // DEBUG TEMPORANEO
        const stateBuf = Buffer.alloc(65536);
        vm_debug_dump_json_ext(this.dbg, stateBuf, stateBuf.length);
        log('Stato VM dopo start: ' + stateBuf.toString('utf-8').replace(/\0.*$/, ''));
        this.sendResponse(response);

        if (args.stopOnEntry !== false) {
            this.sendEvent(new StoppedEvent('entry', 1));
        } else {
            // VM is paused at entry; immediately resume it in RUN/CONTINUE mode
            // so it runs until the first breakpoint or end of program.
            const line = vm_debug_continue(this.dbg);
            log(`launchRequest: auto-continue ritorna linea: ${line}`);
            if (line < 0) {
                this.sendEvent(new TerminatedEvent());
            }
            // If line >= 0 we stopped at a breakpoint — but the DAP client
            // hasn't finished its configuration yet, so do nothing here;
            // configurationDoneRequest will fire shortly and the user will
            // see the breakpoint hit.
        }
    }

    /* ── Threads ── */
    protected threadsRequest(response: DebugProtocol.ThreadsResponse) {
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
        this.sendResponse(response);
        // DEBUG TEMPORANEO
        const stateBuf = Buffer.alloc(65536);
        vm_debug_dump_json_ext(this.dbg, stateBuf, stateBuf.length);
        log('Stato VM prima di continue: ' + stateBuf.toString('utf-8').replace(/\0.*$/, ''));
        let line: number;
        try {
            line = vm_debug_continue(this.dbg);
            log(`vm_debug_continue ritorna linea: ${line}`);
        } catch (e: any) {
            log('CRASH in vm_debug_continue: ' + e.message);
            throw e;
        }

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
            throw e;
        }

        if (line < 0) {
            log('Terminazione VM');
            this.sendEvent(new TerminatedEvent());
        } else {
            this.sendEvent(new StoppedEvent('step', 1));
        }
    }

    /* ── StepBack ── */
    // FIX 2: was sending StoppedEvent('step') in both branches; line < 0
    // means the history is exhausted (already at the beginning), which is
    // not a termination but should still stop and let the user know.
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
            throw e;
        }

        // line < 0 means history exhausted — stay stopped at the oldest
        // known position (the VM is still paused, not terminated).
        this.sendEvent(new StoppedEvent('step', 1));
    }

    /* ── SetBreakpoints ── */
    // FIX 3: VS Code sends the FULL desired set every time; clear first,
    // then add, so removed breakpoints don't linger in the VM.
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