import sys
from Jast import parser, lexer
from queue import Queue

# ───────────────────────────────────────────
#  Indirizzo per backpatching dei salti
# ───────────────────────────────────────────
class ByteCode_Compiler:
    def __init__(self):
        self.queue   = Queue()
        self.addr    = 0          # contatore istruzioni
        self.labels  = {}         # label → indirizzo

    def emit(self, instr):
        self.queue.put((self.addr, instr))
        self.addr += 1

    # ── entry point ──────────────────────────
    def process(self, ast):
        if not ast:
            return
        head = ast[0]

        match head:

            case 'program':
                # ('program', [procedure, ...])
                self.emit("START")
                procs = ast[1] if len(ast) > 1 else []
                for child in procs:
                    if isinstance(child, (list, tuple)):
                        self.process(child)
                self.emit("HALT")

            case 'procedure':
                # ('procedure', nome, [(tipo1, id1), (tipo2, id2), ...], body)
                name   = ast[1]
                params = ast[2] if len(ast) > 2 else []
                body   = ast[3] if len(ast) > 3 else []

                self.labels[name] = self.addr
                self.emit(f"PROC {name}")

                # ── un'istruzione PARAM per ogni coppia (tipo, id) ──
                for param in params:
                    tipo  = param[0]
                    pname = param[1]
                    self.emit(f"PARAM {tipo} {pname}")

                for stmt in body:
                    self.process(stmt)

                self.emit(f"END_PROC {name}")

            case 'decl':
                # (DECL, type, ID)
                type = ast[1]
                ID = ast[2]
                self.emit(f"DECL {type} {ID}")
            case 'local':
                #(LOCAL, 'type', ID, value)
                type = ast[1]
                ID = ast[2]
                value = ast[3]
                self.emit(f"LOCAL {type} {ID} {value}")
            case 'delocal':
                #(DELOCAL, 'type', ID, value)
                type = ast[1]
                ID = ast[2]
                value = ast[3]
                self.emit(f"DELOCAL {type} {ID} {value}")
            case 'assign':
                #operator ID VALUE
                ID = ast[1]
                operator = ast[2]
                value = ast[3]
                #capiamo che operazione abbiamo
                match operator:
                    case '+=':
                        operator = 'PUSHEQ'
                    case '-=':
                        operator = 'MINEQ'
                    case '<=>':
                        operator = 'SWAP'
                    case _:
                        print(f"[BYTECODE] operatore artimetico non supportato")
                        exit(1)
                self.emit(f"{operator} {ID} {value}")
            case 'call':
                # ('call', nome, [arg1, arg2, ...])
                proc_name = ast[1]
                args      = ast[2] if len(ast) > 2 else []

                args_str = " ".join(str(a) for a in args)
                self.emit(f"CALL {proc_name} {args_str}")

            case 'uncall':
                # ('uncall', nome, [arg1, arg2, ...])
                proc_name = ast[1]
                args      = ast[2] if len(ast) > 2 else []
                args_str = " ".join(str(a) for a in args)
                self.emit(f"UNCALL {proc_name} {args_str}")
            case 'call_direct':
                proc_name = ast[1]
                args      = ast[2] if len(ast) > 2 else []
                args_str = " ".join(str(a) for a in args)
                self.emit(f"{proc_name.upper()} {args_str}")
            case 'if':
                # AST: ('if', entry_cond, then_body, else_body, fi_cond)
                entry_cond = ast[1]
                then_body  = ast[2]
                else_body  = ast[3]
                fi_cond    = ast[4]

                uid        = self.addr
                else_label = f"ELSE_{uid}"
                fi_label   = f"FI_{uid}"

                lhs, rhs = entry_cond[1], entry_cond[2]

                # ── 1) ENTRY CONDITION
                self.emit(f"EVAL {lhs} {rhs}")
                self.emit(f"JMPF {else_label}")

                # ── 2) THEN
                for stmt in then_body:
                    self.process(stmt)
                self.emit(f"JMP {fi_label}")

                # ── 3) ELSE
                self.labels[else_label] = self.addr
                self.emit(f"LABEL {else_label}")
                for stmt in else_body:
                    self.process(stmt)

                # ── 4) FI (REVERSIBILITY CHECK)
                self.labels[fi_label] = self.addr
                self.emit(f"LABEL {fi_label}")

                lhs_fi, rhs_fi = fi_cond[1], fi_cond[2]

                # 🔥 QUESTA È LA PARTE IMPORTANTE
                self.emit(f"EVAL {lhs_fi} {rhs_fi}")
                self.emit(f"ASSERT {lhs} {rhs}")   # deve matchare la entry condition 
            case 'from':
                entry_cond = ast[1]
                body       = ast[2]
                until_cond = ast[3]

                uid = self.addr

                start_label = f"FROM_START_{uid}"
                end_label   = f"FROM_END_{uid}"
                err_label   = f"FROM_ERR_{uid}"

                # ─────────────────────────────
                # ENTRY CHECK (OK)
                # ─────────────────────────────
                lhs_e, rhs_e = entry_cond[1], entry_cond[2]
                self.emit(f"EVAL {lhs_e} {rhs_e}")
                self.emit(f"JMPF {err_label}")

                # ─────────────────────────────
                # LOOP START
                # ─────────────────────────────
                self.labels[start_label] = self.addr
                self.emit(f"LABEL {start_label}")

                # ─────────────────────────────
                # BODY
                # ─────────────────────────────
                for stmt in body:
                    self.process(stmt)

                # ─────────────────────────────
                # UNTIL CHECK (POST-BODY BUT SYMMETRIC)
                # ─────────────────────────────
                lhs_u, rhs_u = until_cond[1], until_cond[2]

                self.emit(f"EVAL {lhs_u} {rhs_u}")

                # ⚠️ FIX CRUCIALE: EXIT FIRST, NOT AFTER SIDE EFFECT CHAOS
                self.emit(f"JMPF {start_label}")

                # ─────────────────────────────
                # EXIT
                # ─────────────────────────────
                self.labels[end_label] = self.addr
                self.emit(f"LABEL {end_label}")

                # ─────────────────────────────
                # ERROR
                # ─────────────────────────────
                self.labels[err_label] = self.addr
                self.emit(f"LABEL {err_label}")
                #self.emit("HALT")
            case 'par':
                # ('par', [[stmt, ...], [stmt, ...], ...])
                branches = ast[1]
                self.emit("PAR_START")
                for i, branch in enumerate(branches):
                    self.emit(f"THREAD_{i}")
                    for stmt in branch:          # branch is a body (list of stmts)
                        if stmt is not None:
                            self.process(stmt)
                self.emit("PAR_END")
            case _:
                print(f"[BYTECODE] nodo AST non gestito: {head}  →  {ast}")
                exit(1)


# ───────────────────────────────────────────
#  Main
# ───────────────────────────────────────────
if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python JBytecode.py <file>")
        sys.exit(1)

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    ast = list(parser.parse(source, lexer=lexer))

    BT_Compiler= ByteCode_Compiler()
    BT_Compiler.process(ast)

    #print("=== Bytecode generato ===")
    with open("bytecode.txt", "w") as f:
        while not BT_Compiler.queue.empty():
            addr, instr = BT_Compiler.queue.get()
            line = f"{addr:04d}  {instr}\n"
            f.write(line)
            print(line, end="")  # opzionale: stampa anche a video
    
