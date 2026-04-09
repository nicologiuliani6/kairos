const path = require('path');
const { spawnSync } = require('child_process');
const fs = require('fs');
const koffi = require('koffi');

function compileBytecode(kairosFile, kairosApp) {
  const appDir = path.dirname(kairosApp);
  const result = spawnSync(kairosApp, [kairosFile, '--dap'], {
    cwd: appDir,
    encoding: 'utf-8',
    maxBuffer: 10 * 1024 * 1024,
  });
  const stdoutText = result.stdout || '';
  const begin = '<<<KAIROS_BYTECODE_BEGIN>>>';
  const end = '<<<KAIROS_BYTECODE_END>>>';
  const m = stdoutText.match(new RegExp(`${begin}\\n([\\s\\S]*?)\\n${end}`));
  if (!m) {
    const fallback = [stdoutText, result.stderr || ''].filter(Boolean).join('\n').trim();
    throw new Error(`Bytecode non ricevuto: ${fallback}`);
  }
  return m[1].endsWith('\n') ? m[1] : `${m[1]}\n`;
}

function readVmBuffer(vm_debug_output_ext, dbg) {
  let out = '';
  for (;;) {
    const buf = Buffer.alloc(65536);
    const n = vm_debug_output_ext(dbg, buf, buf.length);
    if (n <= 0) break;
    out += buf.toString('utf-8', 0, n);
  }
  return out;
}

async function main() {
  const root = path.resolve(__dirname, '../..');
  const libPath = path.join(root, 'build', 'libvm_dap.so');
  const kairosApp = path.join(root, 'build', 'dist', 'KairosApp');
  const program = path.join(root, 'tests', 'test_loop.kairos');

  const lib = koffi.load(libPath);
  const vm_debug_new = lib.func('void* vm_debug_new()');
  const vm_debug_free = lib.func('void vm_debug_free(void*)');
  const vm_debug_start = lib.func('void vm_debug_start(str, void*)');
  const vm_debug_stop = lib.func('void vm_debug_stop(void*)');
  const vm_debug_step = lib.func('int vm_debug_step(void*)');
  const vm_debug_step_back = lib.func('int vm_debug_step_back(void*)');
  const vm_debug_continue = lib.func('int vm_debug_continue(void*)');
  const vm_debug_set_breakpoint = lib.func('void vm_debug_set_breakpoint(void*, int)');
  const vm_debug_get_output_fd = lib.func('int vm_debug_get_output_fd(void*)');
  const vm_debug_output_ext = lib.func('int vm_debug_output_ext(void*, void*, int)');

  const bytecode = compileBytecode(program, kairosApp);
  const dbg = vm_debug_new();
  vm_debug_start(bytecode, dbg);

  const events = [];
  const received = [];
  const attachPipe = () => {
    const fd = vm_debug_get_output_fd(dbg);
    events.push(`attach fd=${fd}`);
    let inFlight = false;
    const timer = setInterval(() => {
      if (inFlight) return;
      inFlight = true;
      const buf = Buffer.alloc(4096);
      fs.read(fd, buf, 0, buf.length, null, (err, bytesRead, b) => {
        inFlight = false;
        if (err) return;
        if (bytesRead > 0) {
          const chunk = b.toString('utf-8', 0, bytesRead);
          received.push(chunk);
          events.push(`poll:data ${JSON.stringify(chunk)}`);
        }
      });
    }, 20);
    return { stop: () => clearInterval(timer) };
  };

  let reader = attachPipe();

  let line = -1;
  let guard = 0;
  while (guard++ < 10000) {
    line = vm_debug_step(dbg);
    if (line < 0 || line === 7) break;
  }
  events.push(`step->line ${line}`);

  line = vm_debug_step_back(dbg);
  events.push(`step_back->line ${line}`);

  // Simula esattamente il riattach del dapAdapter dopo stepBack
  reader.stop();
  reader = attachPipe();

  line = vm_debug_continue(dbg);
  events.push(`continue->line ${line}`);

  await new Promise((r) => setTimeout(r, 120));
  const vmBuf = readVmBuffer(vm_debug_output_ext, dbg);

  console.log('=== DAP-SIDE EVENTS ===');
  for (const e of events) console.log(e);
  console.log('=== DAP-SIDE PIPE OUTPUT ===');
  console.log(received.join(''));
  console.log('=== VM BUFFER FALLBACK ===');
  console.log(vmBuf);

  reader.stop();
  vm_debug_stop(dbg);
  vm_debug_free(dbg);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
