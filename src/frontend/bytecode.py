import sys
from queue import Queue

from src.frontend.errors import KairosCompileError
from src.frontend.parser import (
    lexer, parser, run_static_checks, desugar_try, from_second_body,
    _BUILTIN_CALL_OPCODES,
)

_KAIROS_ALLOW_PAR_SHARED_INT = "// KAIROS_ALLOW_PAR_SHARED_INT"


def _strip_mnemo_par_shared_pragma(source: str) -> tuple[str, bool]:
    lines = source.splitlines()
    if lines and lines[0].strip() == _KAIROS_ALLOW_PAR_SHARED_INT:
        body = lines[1:]
        return ("\n".join(body) + ("\n" if body else ""), True)
    return source, False

_ASSIGN_OPS = {
    '+=':  'PUSHEQ',
    '-=':  'MINEQ',
    '^=':  'XOREQ',
    '<=>': 'SWAP'
}

# _BUILTIN_CALL_OPCODES vive in parser.py, accanto alla tabella degli argomenti
# scritti da ciascuna builtin (_BUILTIN_WRITTEN_ARGS): le due liste devono
# restare allineate, e parser.py non può importare da qui (dipendenza circolare).

class ByteCode_Compiler:
    def __init__(self):
        self.queue = Queue()
        self.addr  = 0
        self.current_lineno = 0
        self.bytecode_line = 0      # ← AGGIUNGI

    def emit(self, instr, lineno=None):
        self.bytecode_line += 1
        src_tag = f"@{lineno}" if lineno is not None else "@0"
        self.queue.put((self.bytecode_line, src_tag, instr))
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
                    raise KairosCompileError("BYTECODE", f"operatore aritmetico non supportato: {op}")
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
                if name.lower() in _BUILTIN_CALL_OPCODES:
                    self.emit(f"{name.upper()} {args_str}".rstrip(), lineno)
                else:
                    self.emit(f"CALL {name} {args_str}".rstrip(), lineno)

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
                # ('from', b1, c1, b2, from_ln, until_ln[, c2])
                second_body = from_second_body(ast)
                if len(ast) >= 6:
                    entry_cond, body, until_cond = ast[1], ast[2], ast[3]
                    from_lineno, until_lineno = ast[4], ast[5]
                else:
                    # Compatibilita' con AST vecchi: una sola linea per tutto il loop.
                    _, entry_cond, body, until_cond, from_lineno = ast
                    until_lineno = from_lineno
                uid         = self.addr
                start_label = f"FROM_START_{uid}"
                err_label   = f"FROM_ERR_{uid}"
                back_label  = f"FROM_BACK_{uid}"

                lhs, op, rhs = self.cond_to_str(entry_cond)
                self.emit(f"EVAL {lhs} {op} {rhs}", from_lineno)
                self.emit(f"JMPF {err_label}", from_lineno)
                if not second_body:
                    # Ciclo a un corpo (c2 vuoto): layout storico, invariato byte per byte.
                    self.emit(f"LABEL {start_label}", from_lineno)
                    for stmt in body:
                        self.process(stmt)
                    lhs_u, op_u, rhs_u = self.cond_to_str(until_cond)
                    self.emit(f"EVAL {lhs_u} {op_u} {rhs_u}", until_lineno)
                    self.emit(f"JMPF {start_label}", until_lineno)
                else:
                    # Ciclo a due corpi (Janus): traccia  c1 [c2 c1]*.
                    # Il JMP iniziale salta c2 al primo giro; il back-edge ci rientra.
                    self.emit(f"JMP {start_label}", from_lineno)
                    self.emit(f"LABEL {back_label}", from_lineno)
                    for stmt in second_body:
                        self.process(stmt)
                    self.emit(f"LABEL {start_label}", from_lineno)
                    for stmt in body:
                        self.process(stmt)
                    lhs_u, op_u, rhs_u = self.cond_to_str(until_cond)
                    self.emit(f"EVAL {lhs_u} {op_u} {rhs_u}", until_lineno)
                    self.emit(f"JMPF {back_label}", until_lineno)
                self.emit(f"LABEL FROM_END_{uid}", until_lineno)
                self.emit(f"LABEL {err_label}", until_lineno)

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
                raise KairosCompileError("BYTECODE", f"nodo AST non gestito: {ast[0]}  ->  {ast}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python JBytecode.py <file>")
        sys.exit(1)

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    source, skip_par_int_race = _strip_mnemo_par_shared_pragma(source)

    try:
        ast = parser.parse(source, lexer=lexer)
        if ast is None:
            raise KairosCompileError("PARSER", "compilazione interrotta: AST non generato")
        ast = desugar_try(ast)
        run_static_checks(ast, check_par_int_race=not skip_par_int_race)
        compiler = ByteCode_Compiler()
        compiler.process(ast)
    except KairosCompileError as exc:
        print(exc)
        sys.exit(1)
    except Exception as exc:
        print(f"[COMPILER] errore interno: {exc}")
        sys.exit(1)

    with open("bytecode.txt", "w") as f:
        while not compiler.queue.empty():
            _phys, src_tag, instr = compiler.queue.get()
            line = f"{src_tag:<8}  {instr}\n"
            f.write(line)