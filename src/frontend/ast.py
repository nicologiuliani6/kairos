import sys
import pprint
from src.frontend.parser import parser, lexer

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python Jast.py <file>")
        sys.exit(1)
    with open(sys.argv[1], 'r') as f:
        source = f.read()
    ast = parser.parse(source, lexer=lexer)
    pprint.pprint(ast)
