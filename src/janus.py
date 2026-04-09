import os
import sys
import ctypes
from src.frontend.bytecode import ByteCode_Compiler
from src.frontend.lexer import lexer
from src.frontend.parser import parser

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python Kairos.py <file> [--dump-bytecode] [--dap]")
        sys.exit(1)

    dap_mode = "--dap" in sys.argv

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    ast = list(parser.parse(source, lexer=lexer))
    BT_Compiler = ByteCode_Compiler()
    BT_Compiler.process(ast)

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
        exe_dir = os.path.dirname(sys.executable)
        root_dir = os.path.abspath(os.path.join(exe_dir, '..'))
    else:
        root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

    # Percorso corretto della libreria
    lib_path = os.path.join(root_dir, 'build', 'libvm.so')
    lib_path = os.path.normpath(lib_path)

    if not os.path.exists(lib_path):
        print(f"Errore: libreria non trovata: {lib_path}")
        sys.exit(1)

    print(f"Caricamento libreria: {lib_path}")
    lib = ctypes.CDLL(lib_path)
    lib.vm_run_from_string.argtypes = [ctypes.c_char_p]
    lib.vm_run_from_string.restype  = None
    lib.vm_run_from_string(bytecode_str.encode('utf-8'))