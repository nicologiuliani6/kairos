import os
import sys
import ctypes
from src.frontend.bytecode import ByteCode_Compiler
from src.frontend.lexer import lexer
from src.frontend.parser import parser

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python Janus.py <file> [--dump-bytecode]")
        sys.exit(1)

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    ast = list(parser.parse(source, lexer=lexer))
    BT_Compiler = ByteCode_Compiler()
    BT_Compiler.process(ast)

    # Costruisci la stringa bytecode in memoria invece di scrivere su file
    lines = []
    while not BT_Compiler.queue.empty():
        addr, instr = BT_Compiler.queue.get()
        lines.append(f"{addr:04d}  {instr}")
    
    bytecode_str = "\n".join(lines) + "\n"
    if "--dump-bytecode" in sys.argv:
        with open("bytecode.txt", "w") as f:
            f.write(bytecode_str)
            
    # Carica la VM e passagli la stringa direttamente
    
    # percorso della root del package src
    base_path = os.path.dirname(os.path.dirname(__file__))  # sempre src/
    lib_path = os.path.join(base_path, "build", "libvm.so")
    lib = ctypes.CDLL(lib_path)
    lib.vm_run_from_string.argtypes = [ctypes.c_char_p]
    lib.vm_run_from_string.restype  = None

    lib.vm_run_from_string(bytecode_str.encode('utf-8'))