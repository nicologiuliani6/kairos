import os
import sys
import ctypes

# Kairos frontend (parser/bytecode) walks AST ricorsivamente. Programmi
# Mnemo con array grandi unrollati possono produrre file .kairos da decine
# di migliaia di righe → AST nodes ricorsivi → eccede recursion limit
# default 1000. Bump a 200k così files fino a ~10MB compilano.
sys.setrecursionlimit(200000)

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
from src.frontend.parser import parser, run_static_checks, desugar_try
from src.frontend.errors import KairosCompileError

_KAIROS_ALLOW_PAR_SHARED_INT = "// KAIROS_ALLOW_PAR_SHARED_INT"

# Stessi nomi di src/vm/mn_native_arith.h (MN_NATIVE_PROCS) — se il bytecode
# ne chiama uno, native-arith ha effetto reale (bypass per-nome della CALL).
_MN_NATIVE_PROC_NAMES = (
    "__mn_move_int", "__mn_mul_into", "__mn_mul_signed_into",
    "__mn_divmod_nonneg", "__mn_divmod_signed", "__mn_mod_nonneg",
    "__mn_mod_signed", "__mn_and_into", "__mn_or_into",
    "__mn_bit_k_signed", "__mn_floor_div2_signed",
    "__mn_divmod_nonneg_div2", "__mn_shl_into",
)


def _bytecode_wants_native_arith(bytecode_str: str) -> bool:
    """--auto: il bytecode chiama procedure che native-arith intercetta per nome?"""
    return any(name in bytecode_str for name in _MN_NATIVE_PROC_NAMES)


def _strip_mnemo_par_shared_pragma(source: str) -> tuple[str, bool]:
    """
    Mnemo può premettere questa riga: disattiva il check STATIC «race su int nel PAR»
    (variabili file-scope condivise + mutex nel modello C).
    """
    lines = source.splitlines()
    if lines and lines[0].strip() == _KAIROS_ALLOW_PAR_SHARED_INT:
        body = lines[1:]
        return ("\n".join(body) + ("\n" if body else ""), True)
    return source, False


if __name__ == '__main__':
    # invert_op_to_line (UNCALL) è ricorsiva sulle CALL; uno stack POSIX stretto sul main thread può dare SIGSEGV.
    try:
        import resource

        resource.setrlimit(
            resource.RLIMIT_STACK,
            (resource.RLIM_INFINITY, resource.RLIM_INFINITY),
        )
    except (ValueError, AttributeError, OSError):
        pass

    if len(sys.argv) < 2:
        print("Uso: python Kairos.py <file> [--dump-bytecode] [--dap] [--native-arith] [--vm-stats] [--auto]")
        sys.exit(1)

    dap_mode = "--dap" in sys.argv

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    source, skip_par_int_race = _strip_mnemo_par_shared_pragma(source)

    try:
        ast = parser.parse(source, lexer=lexer)
        if ast is None:
            raise KairosCompileError("PARSER", "compilazione interrotta: AST non generato")
        ast = desugar_try(ast)
        run_static_checks(ast, check_par_int_race=not skip_par_int_race)
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
    if hasattr(lib, 'vm_set_native_arith'):
        lib.vm_set_native_arith.argtypes = [ctypes.c_int]
        lib.vm_set_native_arith.restype = None
        na = os.environ.get('KAIROS_NATIVE_ARITH', '')
        want_native = ("--native-arith" in sys.argv) or (na and na[0] in '1yYtT')
        if (not want_native) and "--auto" in sys.argv:
            want_native = _bytecode_wants_native_arith(bytecode_str)
            print(
                f"[Kairos] --auto: native-arith {'ON' if want_native else 'off'} "
                f"({'trovate' if want_native else 'nessuna'} chiamata a procedure __mn_* accelerabili)",
                file=sys.stderr,
            )
        if want_native:
            lib.vm_set_native_arith(1)
    if "--vm-stats" in sys.argv:
        os.environ["KAIROS_VM_STATS"] = "1"
    lib.vm_run_from_string(bytecode_str.encode('utf-8'))
