import sys
from src.Jast import parser, lexer
from queue import Queue

# in _ASSIGN_OPS aggiungi:
_ASSIGN_OPS = {
    '+=':  'PUSHEQ',
    '-=':  'MINEQ',
    '^=':  'XOREQ',
    '<=>': 'SWAP'
}

class ByteCode_Compiler:
    def __init__(self):
        self.queue = Queue()
        self.addr  = 0

    def emit(self, instr):
        self.queue.put((self.addr, instr))
        self.addr += 1

    def expr_to_str(self, expr):
        """Riduce ricorsivamente un nodo expr a una stringa piatta valutabile."""
        if not isinstance(expr, tuple):
            return str(expr)
        if expr[0] == 'binop':
            _, op, left, right = expr
            l = self.expr_to_str(left)
            r = self.expr_to_str(right)
            return f"({l} {op} {r})"
        return str(expr)

    def cond_to_str(self, cond):
        """
        Restituisce (lhs_str, op_str, rhs_str) da un nodo ('cond', op, l, r).
        Il bytecode EVAL usa il formato:  EVAL <lhs> <op> <rhs>
        """
        _, op, lhs, rhs = cond
        return self.expr_to_str(lhs), op, self.expr_to_str(rhs)

    def process(self, ast):
        if not ast:
            return
        match ast[0]:

            case 'program':
                self.emit("START")
                for child in (ast[1] if len(ast) > 1 else []):
                    if isinstance(child, (list, tuple)):
                        self.process(child)
                self.emit("HALT")

            case 'procedure':
                name   = ast[1]
                params = ast[2] if len(ast) > 2 else []
                body   = ast[3] if len(ast) > 3 else []
                self.emit(f"PROC {name}")
                for tipo, pname in params:
                    self.emit(f"PARAM {tipo} {pname}")
                for stmt in body:
                    self.process(stmt)
                self.emit(f"END_PROC {name}")

            case 'decl':
                self.emit(f"DECL {ast[1]} {ast[2]}")

            case 'local':
                self.emit(f"LOCAL {ast[1]} {ast[2]} {ast[3]}")

            case 'delocal':
                self.emit(f"DELOCAL {ast[1]} {ast[2]} {ast[3]}")

            case 'assign':
                op = _ASSIGN_OPS.get(ast[2])
                if op is None:
                    print(f"[BYTECODE] operatore aritmetico non supportato: {ast[2]}")
                    sys.exit(1)
                self.emit(f"{op} {ast[1]} {self.expr_to_str(ast[3])}")

            case 'call':
                args_str = " ".join(str(a) for a in (ast[2] if len(ast) > 2 else []))
                self.emit(f"CALL {ast[1]} {args_str}".rstrip())

            case 'uncall':
                args_str = " ".join(str(a) for a in (ast[2] if len(ast) > 2 else []))
                self.emit(f"UNCALL {ast[1]} {args_str}".rstrip())

            case 'call_direct':
                args_str = " ".join(str(a) for a in (ast[2] if len(ast) > 2 else []))
                self.emit(f"{ast[1].upper()} {args_str}".rstrip())

            case 'if':
                entry_cond, then_body, else_body, fi_cond = ast[1], ast[2], ast[3], ast[4]
                uid        = self.addr
                else_label = f"ELSE_{uid}"
                fi_label   = f"FI_{uid}"

                # EVAL <lhs> <op> <rhs>
                lhs, op, rhs = self.cond_to_str(entry_cond)
                self.emit(f"EVAL {lhs} {op} {rhs}")
                self.emit(f"JMPF {else_label}")
                for stmt in then_body:
                    self.process(stmt)
                self.emit(f"JMP {fi_label}")

                self.emit(f"LABEL {else_label}")
                for stmt in else_body:
                    self.process(stmt)

                self.emit(f"LABEL {fi_label}")
                lhs_fi, op_fi, rhs_fi = self.cond_to_str(fi_cond)
                self.emit(f"EVAL {lhs_fi} {op_fi} {rhs_fi}")
                lhs_e, op_e, rhs_e = self.cond_to_str(entry_cond)
                self.emit(f"ASSERT {lhs_e} {op_e} {rhs_e}")

            case 'from':
                entry_cond, body, until_cond = ast[1], ast[2], ast[3]
                uid         = self.addr
                start_label = f"FROM_START_{uid}"
                err_label   = f"FROM_ERR_{uid}"

                lhs, op, rhs = self.cond_to_str(entry_cond)
                self.emit(f"EVAL {lhs} {op} {rhs}")
                self.emit(f"JMPF {err_label}")

                self.emit(f"LABEL {start_label}")
                for stmt in body:
                    self.process(stmt)

                lhs_u, op_u, rhs_u = self.cond_to_str(until_cond)
                self.emit(f"EVAL {lhs_u} {op_u} {rhs_u}")
                self.emit(f"JMPF {start_label}")

                self.emit(f"LABEL FROM_END_{uid}")
                self.emit(f"LABEL {err_label}")

            case 'par':
                self.emit("PAR_START")
                for i, branch in enumerate(ast[1]):
                    self.emit(f"THREAD_{i}")
                    for stmt in branch:
                        if stmt is not None:
                            self.process(stmt)
                self.emit("PAR_END")

            case _:
                print(f"[BYTECODE] nodo AST non gestito: {ast[0]}  →  {ast}")
                sys.exit(1)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python JBytecode.py <file>")
        sys.exit(1)

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    ast = list(parser.parse(source, lexer=lexer))
    compiler = ByteCode_Compiler()
    compiler.process(ast)

    with open("bytecode.txt", "w") as f:
        while not compiler.queue.empty():
            addr, instr = compiler.queue.get()
            line = f"{addr:04d}  {instr}\n"
            f.write(line)
            print(line, end="")