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
import { spawnSync } from 'child_process';
import * as fs from 'fs';
import * as net from 'net';
import { Worker } from 'worker_threads';

/* ======================================================================
 *  IMPORTANTE: stdout è il canale DAP — NON scriverci mai direttamente.
 *  Tutto l'output (log, VM output) va su file o come DAP OutputEvent.
 * ====================================================================== */

const logFile = '/tmp/janus-dap.log';

function log(msg: string) {
    fs.appendFileSync(logFile, new Date().toISOString() + ' ' + msg + '\n');
}

log('DAP adapter avviato');

/* ======================================================================
 *  Caricamento libvm_dap.so tramite koffi
 * ====================================================================== */

let LIB_PATH = path.join(__dirname, '../../build/libvm_dap.so');

let lib: any;
let vm_debug_new:                   ()                                    => any;
let vm_debug_free:                  (dbg: any)                            => void;
let vm_debug_start:                 (bytecode: string, dbg: any)          => void;
let vm_debug_stop:                  (dbg: any)                            => void;
let vm_debug_step:                  (dbg: any)                            => number;
let vm_debug_step_back:             (dbg: any)                            => number;
let vm_debug_continue:              (dbg: any)                            => number;
let vm_debug_set_breakpoint:        (dbg: any, line: number)              => void;
let vm_debug_clear_breakpoint:      (dbg: any, line: number)              => void;
let vm_debug_clear_all_breakpoints: (dbg: any)                            => void;
let vm_debug_vars_json_ext:         (dbg: any, out: Buffer, sz: number)   => number;
let vm_debug_dump_json_ext:         (dbg: any, out: Buffer, sz: number)   => number;
let vm_debug_get_output_fd:         (dbg: any)                            => number;

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
        vm_debug_new                   = lib.func('void* vm_debug_new()');
        vm_debug_free                  = lib.func('void  vm_debug_free(void*)');
        vm_debug_start                 = lib.func('void  vm_debug_start(str, void*)');
        vm_debug_stop                  = lib.func('void  vm_debug_stop(void*)');
        vm_debug_step                  = lib.func('int   vm_debug_step(void*)');
        vm_debug_step_back             = lib.func('int   vm_debug_step_back(void*)');
        vm_debug_continue              = lib.func('int   vm_debug_continue(void*)');
        vm_debug_set_breakpoint        = lib.func('void  vm_debug_set_breakpoint(void*, int)');
        vm_debug_clear_breakpoint      = lib.func('void  vm_debug_clear_breakpoint(void*, int)');
        vm_debug_clear_all_breakpoints = lib.func('void  vm_debug_clear_all_breakpoints(void*)');
        vm_debug_vars_json_ext         = lib.func('int   vm_debug_vars_json_ext(void*, void*, int)');
        vm_debug_dump_json_ext         = lib.func('int   vm_debug_dump_json_ext(void*, void*, int)');
        vm_debug_get_output_fd         = lib.func('int   vm_debug_get_output_fd(void*)');
        log('Funzioni caricate OK');
    } catch (e: any) {
        log('ERRORE func binding: ' + e.message);
        throw e;
    }
}

/* ======================================================================
 *  Compilazione .janus → bytecode
 * ====================================================================== */

function compileBytecode(
    janusFile: string,
    janusApp:  string,
): { bytecode: string; compileOutput: string } {

    const appDir = path.dirname(janusApp);
    const btFile = path.join(appDir, 'bytecode.txt');

    log(`compileBytecode: app=${janusApp}, file=${janusFile}`);

    if (fs.existsSync(btFile)) fs.unlinkSync(btFile);

    const result = spawnSync(
        janusApp,
        [janusFile, '--dap', '--dump-bytecode'],
        { cwd: appDir, encoding: 'utf-8', maxBuffer: 10 * 1024 * 1024 }
    );

    const compileOutput = [result.stdout || '', result.stderr || '']
        .filter(Boolean)
        .join('\n')
        .trim();

    if (!fs.existsSync(btFile)) {
        throw new Error(`Compilazione fallita (nessun bytecode.txt):\n${compileOutput}`);
    }

    return { bytecode: fs.readFileSync(btFile, 'utf-8'), compileOutput };
}

/* ======================================================================
 *  vm_debug_continue in un Worker thread separato
 * ====================================================================== */

function runContinueInWorker(dbgPtr: bigint): Promise<number> {
    return new Promise((resolve, reject) => {
        const workerCode = `
const { workerData, parentPort } = require('worker_threads');
const koffi = require(workerData.koffiPath);

try {
    const lib = koffi.load(workerData.libPath);
    const vm_debug_continue = lib.func('int vm_debug_continue(void*)');
    const line = vm_debug_continue(workerData.dbgPtr);
    parentPort.postMessage({ ok: true, line });
} catch (e) {
    parentPort.postMessage({ ok: false, error: e.message });
}
        `;

        const worker = new Worker(workerCode, {
            eval: true,
            workerData: {
                libPath:   LIB_PATH,
                dbgPtr,
                koffiPath: require.resolve('koffi'),
            }
        });

        worker.on('message', (msg) => {
            if (msg.ok) resolve(msg.line);
            else        reject(new Error(msg.error));
        });
        worker.on('error', reject);
    });
}

/* ======================================================================
 *  Lettura dalla pipe C → Debug Console
 * ====================================================================== */

class PipeReader {
    private fd:       number;
    private socket:   net.Socket | null      = null;
    private interval: NodeJS.Timeout | null  = null;
    private onData:   (text: string) => void;
    private usedFallback = false;

    constructor(fd: number, onData: (text: string) => void) {
        this.fd     = fd;
        this.onData = onData;
    }

    start() {
        log(`PipeReader.start fd=${this.fd}`);

        try {
            const sock = new net.Socket({
                fd:            this.fd,
                readable:      true,
                writable:      false,
                allowHalfOpen: true,
            });
            sock.setEncoding('utf-8');

            let socketOk = false;

            sock.on('data', (chunk: string) => {
                socketOk = true;
                log(`pipe[socket] data: ${JSON.stringify(chunk)}`);
                this.onData(chunk);
            });

            sock.on('error', (err) => {
                log(`pipe[socket] errore: ${err.message} — passo al polling`);
                sock.destroy();
                this.socket = null;
                if (!socketOk) this._startPolling();
            });

            sock.on('end',   () => log('pipe[socket] EOF'));
            sock.on('close', () => log('pipe[socket] chiusa'));

            sock.resume();
            this.socket = sock;

        } catch (e: any) {
            log(`PipeReader socket fallito: ${e.message} — uso polling`);
            this._startPolling();
        }
    }

    private _startPolling() {
        if (this.usedFallback) return;
        this.usedFallback = true;
        log(`PipeReader: avvio polling fd=${this.fd}`);

        this.interval = setInterval(() => {
            const buf = Buffer.alloc(4096);
            try {
                const n = fs.readSync(this.fd, buf, 0, buf.length, null);
                if (n > 0) {
                    const text = buf.toString('utf-8', 0, n);
                    log(`pipe[poll] data: ${JSON.stringify(text)}`);
                    this.onData(text);
                }
            } catch (_) { /* EAGAIN / EWOULDBLOCK — normale */ }
        }, 50);
    }

    stop() {
        if (this.socket)   { this.socket.destroy(); this.socket = null; }
        if (this.interval) { clearInterval(this.interval); this.interval = null; }
    }
}

/* ======================================================================
 *  Interfaccia LaunchArgs
 * ====================================================================== */

interface LaunchArgs extends DebugProtocol.LaunchRequestArguments {
    program:      string;
    stopOnEntry?: boolean;
    janusApp?:    string;
    janusLib?:    string;
}

/* ======================================================================
 *  Sessione DAP principale
 * ====================================================================== */

class JanusDebugSession extends LoggingDebugSession {

    private dbg:        any    = null;
    private dbgPtr:     bigint = 0n;
    private sourceFile: string = '';
    private pipeReader: PipeReader | null = null;

    constructor() {
        super();
        // loadLib() viene chiamato in launchRequest dopo aver impostato LIB_PATH
        this.setDebuggerLinesStartAt1(true);
        this.setDebuggerColumnsStartAt1(true);
    }

    /* ── Initialize ── */
    protected initializeRequest(
        response: DebugProtocol.InitializeResponse,
        _args:    DebugProtocol.InitializeRequestArguments
    ) {
        response.body = response.body || {};
        response.body.supportsStepBack                 = true;
        response.body.supportsConfigurationDoneRequest = true;
        this.sendResponse(response);
        this.sendEvent(new InitializedEvent());
    }

    /* ── Helper output ── */
    private sendOutput(text: string, cat: string = 'console') {
        this.sendEvent(new OutputEvent(text, cat));
    }

    /* ── Pipe output dalla VM ── */
    private startOutputPipe() {
        if (!this.dbg) return;
        const fd = vm_debug_get_output_fd(this.dbg);
        log(`startOutputPipe: fd=${fd}`);
        if (fd < 0) return;

        this.pipeReader = new PipeReader(fd, (text) => {
            this.sendOutput(text, 'stdout');
        });
        this.pipeReader.start();
    }

    private stopOutputPipe() {
        if (this.pipeReader) { this.pipeReader.stop(); this.pipeReader = null; }
    }

    /* ── Launch ── */
    protected launchRequest(
        response: DebugProtocol.LaunchResponse,
        args:     LaunchArgs
    ) {
        // Imposta LIB_PATH prima di loadLib()
        if (args.janusLib) {
            LIB_PATH = args.janusLib;
        }

        log('launchRequest chiamato, program=' + args.program);
        log('janusApp=' + (args.janusApp || 'NON IMPOSTATO'));
        log('janusLib=' + LIB_PATH);

        // Carica la libreria ora che LIB_PATH è aggiornato
        try {
            loadLib();
        } catch (e: any) {
            this.sendErrorResponse(response, 1, `Impossibile caricare libvm_dap.so: ${e.message}`);
            return;
        }

        this.sourceFile = args.program;

        let bytecode:      string;
        let compileOutput: string;
        try {
            const appPath = args.janusApp || '';
            if (!appPath) {
                this.sendErrorResponse(response, 1, 'janusApp non impostato nel launch.json');
                return;
            }
            ({ bytecode, compileOutput } = compileBytecode(this.sourceFile, appPath));
        } catch (e: any) {
            this.sendOutput(`Errore di compilazione:\n${e.message}`, 'stderr');
            this.sendErrorResponse(response, 1, `Compilazione fallita: ${e.message}`);
            return;
        }

        if (compileOutput) {
            this.sendOutput(`[LEXER->PARSER->AST->BYTECODE]\n${compileOutput}\n`, 'console');
        }

        this.dbg = vm_debug_new();
        try {
            this.dbgPtr = BigInt(koffi.address(this.dbg));
        } catch {
            this.dbgPtr = BigInt(this.dbg);
        }

        vm_debug_start(bytecode, this.dbg);
        this.startOutputPipe();
        this.sendResponse(response);

        if (args.stopOnEntry === true) {
            this.sendEvent(new StoppedEvent('entry', 1));
        } else {
            this._doContinue();
        }
    }

    /* ── Continue (F5) ── */
    protected continueRequest(
        response: DebugProtocol.ContinueResponse,
        _args:    DebugProtocol.ContinueArguments
    ) {
        log('continueRequest');
        this.sendResponse(response);
        this._doContinue();
    }

    private _doContinue() {
        runContinueInWorker(this.dbgPtr)
            .then((line) => {
                log(`_doContinue: line=${line}`);
                if (line < 0) {
                    log('VM terminata');
                    this.sendEvent(new TerminatedEvent());
                } else {
                    this.sendEvent(new StoppedEvent('breakpoint', 1));
                }
            })
            .catch((e) => {
                log(`_doContinue error: ${e.message}`);
                this.sendOutput(`Errore VM: ${e.message}`, 'stderr');
                this.sendEvent(new TerminatedEvent());
            });
    }

    /* ── Next (F10) ── */
    protected nextRequest(
        response: DebugProtocol.NextResponse,
        _args:    DebugProtocol.NextArguments
    ) {
        this.sendResponse(response);
        log('vm_debug_step');
        let line: number;
        try {
            line = vm_debug_step(this.dbg);
            log(`step ritorna: ${line}`);
        } catch (e: any) {
            log('CRASH step: ' + e.message);
            this.sendOutput(`CRASH step: ${e.message}`, 'stderr');
            return;
        }

        if (line < 0) {
            this.sendEvent(new TerminatedEvent());
        } else {
            this.sendEvent(new StoppedEvent('step', 1));
        }
    }

    /* ── StepBack ── */
    protected stepBackRequest(
        response: DebugProtocol.StepBackResponse,
        _args:    DebugProtocol.StepBackArguments
    ) {
        this.sendResponse(response);
        log('vm_debug_step_back');
        let line: number;
        try {
            line = vm_debug_step_back(this.dbg);
            log(`step_back ritorna: ${line}`);
        } catch (e: any) {
            log('CRASH step_back: ' + e.message);
            this.sendOutput(`CRASH step_back: ${e.message}`, 'stderr');
            return;
        }
        this.sendEvent(new StoppedEvent('step', 1));
    }

    /* ── Threads ── */
    protected threadsRequest(response: DebugProtocol.ThreadsResponse) {
        response.body = { threads: [new Thread(1, 'main')] };
        this.sendResponse(response);
    }

    /* ── StackTrace ── */
    protected stackTraceRequest(
        response: DebugProtocol.StackTraceResponse,
        _args:    DebugProtocol.StackTraceArguments
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
        _args:    DebugProtocol.ScopesArguments
    ) {
        response.body = { scopes: [new Scope('Locals', 1, false)] };
        this.sendResponse(response);
    }

    /* ── Variables ── */
    protected variablesRequest(
        response: DebugProtocol.VariablesResponse,
        _args:    DebugProtocol.VariablesArguments
    ) {
        const vars = this.getVariables();
        response.body = {
            variables: vars.map(v => ({
                name:               v.name,
                value:              JSON.stringify(v.value),
                variablesReference: 0,
            }))
        };
        this.sendResponse(response);
    }

    /* ── SetBreakpoints ── */
    protected setBreakPointsRequest(
        response: DebugProtocol.SetBreakpointsResponse,
        args:     DebugProtocol.SetBreakpointsArguments
    ) {
        if (this.dbg) vm_debug_clear_all_breakpoints(this.dbg);

        const bps = (args.breakpoints || []).map(bp => {
            if (this.dbg) vm_debug_set_breakpoint(this.dbg, bp.line);
            log(`Breakpoint @ ${bp.line}`);
            return { verified: true, line: bp.line };
        });

        response.body = { breakpoints: bps };
        this.sendResponse(response);
    }

    /* ── Disconnect ── */
    protected disconnectRequest(
        response: DebugProtocol.DisconnectResponse,
        _args:    DebugProtocol.DisconnectArguments
    ) {
        this.stopOutputPipe();
        if (this.dbg) {
            vm_debug_stop(this.dbg);
            vm_debug_free(this.dbg);
            this.dbg = null;
        }
        this.sendResponse(response);
    }

    /* ── Helper: stato corrente ── */
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

    /* ── Helper: variabili ── */
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