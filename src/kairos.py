import os
import sys
import ctypes
from src.frontend.bytecode import ByteCode_Compiler


def _warn_if_libvm_stale(lib_path: str, project_root: str) -> None:
    """Avvisa se si usa una VM vecchia rispetto ai sorgenti C (causa tipica di bug già corretti nel repo)."""
    vm_dir = os.path.join(project_root, 'src', 'vm')
    try:
        if not os.path.isfile(lib_path):
            return
        if lib_path.startswith('/opt/') or lib_path.startswith('/usr/'):
            print(
                f"[Kairos] VM di sistema: {lib_path}\n"
                "         Per la build del repository: make build-release",
                file=sys.stderr,
            )
            return
        if not os.path.isdir(vm_dir):
            return
        lib_m = os.path.getmtime(lib_path)
        for fn in os.listdir(vm_dir):
            if fn.endswith(('.c', '.h')):
                p = os.path.join(vm_dir, fn)
                if os.path.isfile(p) and os.path.getmtime(p) > lib_m:
                    print(
                        "[Kairos] build/libvm.so è più vecchio di src/vm — esegui: make build-release",
                        file=sys.stderr,
                    )
                    break
    except OSError:
        pass
from src.frontend.lexer import lexer
from src.frontend.parser import parser, run_static_checks
from src.frontend.errors import KairosCompileError

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python Kairos.py <file> [--dump-bytecode] [--dap]")
        sys.exit(1)

    dap_mode = "--dap" in sys.argv

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    try:
        ast = parser.parse(source, lexer=lexer)
        if ast is None:
            raise KairosCompileError("PARSER", "compilazione interrotta: AST non generato")
        run_static_checks(ast)
        BT_Compiler = ByteCode_Compiler()
        BT_Compiler.process(ast)
    except KairosCompileError as exc:
        print(exc)
        sys.exit(1)
    except Exception as exc:
        print(f"[COMPILER] errore interno: {exc}")
        sys.exit(1)

    lines = []
    while not BT_Compiler.queue.empty():
        _phys, src_tag, instr = BT_Compiler.queue.get()
        lines.append(f"{src_tag:<6}  {instr}")

    bytecode_str = "\n".join(lines) + "\n"

    if "--dump-bytecode" in sys.argv:
        with open("bytecode.txt", "w") as f:
            f.write(bytecode_str)

    # In modalità --dap non scrivere su disco: il DAP legge il bytecode da stdout.
    if dap_mode:
        print("<<<KAIROS_BYTECODE_BEGIN>>>")
        print(bytecode_str, end="")
        print("<<<KAIROS_BYTECODE_END>>>")
        sys.exit(0)

    # Ricava la root del progetto (da qui cerchiamo build/)
    if getattr(sys, 'frozen', False):
        lib_path = os.path.join(sys._MEIPASS, 'libvm.so')
    else:
        build_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'build'))
        lib_path = os.path.join(build_dir, 'libvm.so')

    if not os.path.exists(lib_path):
        lib_path = '/opt/kairosapp/libvm.so'

    if not os.path.exists(lib_path):
        print("Errore: libreria non trovata in nessun percorso.")
        sys.exit(1)

    project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    _warn_if_libvm_stale(lib_path, project_root)

    lib = ctypes.CDLL(lib_path)
    lib.vm_run_from_string.argtypes = [ctypes.c_char_p]
    lib.vm_run_from_string.restype  = None
    lib.vm_run_from_string(bytecode_str.encode('utf-8'))
