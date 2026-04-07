import sys
from src.frontend.parser import parser, lexer
from queue import Queue

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
        self.current_lineno = 0

    def emit(self, instr, lineno=None):
        src = lineno if lineno is not None else self.current_lineno
        self.queue.put((src, instr))
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
                self.emit("START", 0)
                for child in (ast[1] if len(ast) > 1 else []):
                    if isinstance(child, (list, tuple)):
                        self.process(child)
                self.emit("HALT", 0)

            case 'procedure':
                name, params, body, lineno = ast[1], ast[2], ast[3], ast[4]
                self.emit(f"PROC {name}", lineno)
                for tipo, pname in params:
                    self.emit(f"PARAM {tipo} {pname}", lineno)
                for stmt in body:
                    self.process(stmt)
                self.emit(f"END_PROC {name}", lineno)

            case 'decl':
                _, tipo, name, lineno = ast
                self.emit(f"DECL {tipo} {name}", lineno)

            case 'local':
                _, tipo, name, val, lineno = ast
                self.emit(f"LOCAL {tipo} {name} {val}", lineno)

            case 'delocal':
                _, tipo, name, val, lineno = ast
                self.emit(f"DELOCAL {tipo} {name} {val}", lineno)

            case 'assign':
                _, var, op, expr, lineno = ast
                opcode = _ASSIGN_OPS.get(op)
                if opcode is None:
                    print(f"[BYTECODE] operatore aritmetico non supportato: {op}")
                    sys.exit(1)
                self.emit(f"{opcode} {var} {self.expr_to_str(expr)}", lineno)

            case 'call':
                _, name, args, lineno = ast
                args_str = " ".join(str(a) for a in args)
                self.emit(f"CALL {name} {args_str}".rstrip(), lineno)

            case 'uncall':
                _, name, args, lineno = ast
                args_str = " ".join(str(a) for a in args)
                self.emit(f"UNCALL {name} {args_str}".rstrip(), lineno)

            case 'call_direct':
                _, name, args, lineno = ast
                args_str = " ".join(str(a) for a in args)
                self.emit(f"{name.upper()} {args_str}".rstrip(), lineno)

            case 'if':
                _, entry_cond, then_body, else_body, fi_cond, lineno = ast
                uid        = self.addr
                else_label = f"ELSE_{uid}"
                fi_label   = f"FI_{uid}"

                lhs, op, rhs = self.cond_to_str(entry_cond)
                self.emit(f"EVAL {lhs} {op} {rhs}", lineno)
                self.emit(f"JMPF {else_label}", lineno)
                for stmt in then_body:
                    self.process(stmt)
                self.emit(f"JMP {fi_label}", lineno)
                self.emit(f"LABEL {else_label}", lineno)
                for stmt in else_body:
                    self.process(stmt)
                self.emit(f"LABEL {fi_label}", lineno)
                lhs_fi, op_fi, rhs_fi = self.cond_to_str(fi_cond)
                self.emit(f"EVAL {lhs_fi} {op_fi} {rhs_fi}", lineno)
                lhs_e, op_e, rhs_e = self.cond_to_str(entry_cond)
                self.emit(f"ASSERT {lhs_e} {op_e} {rhs_e}", lineno)

            case 'from':
                _, entry_cond, body, until_cond, lineno = ast
                uid         = self.addr
                start_label = f"FROM_START_{uid}"
                err_label   = f"FROM_ERR_{uid}"

                lhs, op, rhs = self.cond_to_str(entry_cond)
                self.emit(f"EVAL {lhs} {op} {rhs}", lineno)
                self.emit(f"JMPF {err_label}", lineno)
                self.emit(f"LABEL {start_label}", lineno)
                for stmt in body:
                    self.process(stmt)
                lhs_u, op_u, rhs_u = self.cond_to_str(until_cond)
                self.emit(f"EVAL {lhs_u} {op_u} {rhs_u}", lineno)
                self.emit(f"JMPF {start_label}", lineno)
                self.emit(f"LABEL FROM_END_{uid}", lineno)
                self.emit(f"LABEL {err_label}", lineno)

            case 'par':
                _, branches, lineno = ast
                self.emit("PAR_START", lineno)
                for i, branch in enumerate(branches):
                    self.emit(f"THREAD_{i}", lineno)
                    for stmt in branch:
                        if stmt is not None:
                            self.process(stmt)
                self.emit("PAR_END", lineno)

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
            src_line, instr = compiler.queue.get()
            line = f"{src_line:04d}  {instr}\n"
            f.write(line)
            #print(line, end="")