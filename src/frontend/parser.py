import sys
import ply.yacc as yacc
from .lexer import lexer, tokens
from .errors import KairosCompileError
from .sessions import check_sessions
import logging
import tempfile

VERBOSE = False


class _ParStaticConfig:
    """Controlli statici su `par` (modificato da `run_static_checks`)."""

    check_int_race: bool = True
    # Procedure del programma, per l'inferenza dei tipi di sessione: una
    # chiamata dentro un ramo di `par` contribuisce il protocollo del corpo
    # della callee, quindi serve poterla risolvere dal punto in cui si controlla.
    program_procedures: tuple = ()


# ── Builtin ─────────────────────────────────────────────────────────────────
# Chiamate tipo foo(x) senza `call`: opcode VM diretto, non CALL nome_proc.
# (Usato anche da bytecode.py, che importa questo nome da qui.)
_BUILTIN_CALL_OPCODES = frozenset({'show', 'push', 'pop', 'ssend', 'srecv', 'mnhalve', 'mnsplit32', 'dump',
                                   'pooladd', 'poolsub', 'poolget', 'poolgetneg', 'poolpush', 'poolpop'})

# Marcatore per le builtin variadiche ssend/srecv: `nome(<a1 ... ak>, c)` arriva
# come lista di argomenti `[a1, ..., ak, c]`, l'ultimo è il channel e tutti gli
# altri sono destinazioni/sorgenti scritte.
_BUILTIN_ARGS_ALL_BUT_LAST = object()

# Posizioni degli argomenti che ogni builtin SCRIVE. Serve ai controlli statici
# sul `par`: una builtin che scrive il proprio argomento è una scrittura a tutti
# gli effetti, anche se nell'AST è un 'call_direct' e non un 'assign'.
# Evidenza, riga per riga, dall'implementazione della VM:
#   push       src/vm/vm_ops.h op_push       — `*(src->value) = 0` (azzeramento sorgente)
#   pop        src/vm/vm_ops.h op_pop        — `*(dest->value) += popped` / `= popped`
#   ssend      src/vm/vm_ops.h op_ssend      — per ogni payload int `*(src->value) = 0`
#   srecv      src/vm/vm_ops.h op_srecv      — per ogni destinazione `*(dest->value) += popped`
#   mnhalve    src/vm/ops_arith.h op_mnhalve   — `*(vq) += q; *(vp) += par; *(vs) = 0` (tutti e 3)
#   mnsplit32  src/vm/ops_arith.h op_mnsplit32 — `*(vh) += hi; *(vl) += lo; *(vs) = 0` (tutti e 3)
#   poolget    src/vm/ops_arith.h op_poolget    — `*(dst->value) += pool[idx]` (2° arg)
#   poolgetneg src/vm/ops_arith.h op_poolgetneg — `*(dst->value) -= pool[idx]` (2° arg)
# Volutamente assenti:
#   show, dump                — sola lettura;
#   pooladd, poolsub          — scrivono la cella del pool, non una variabile int;
#   poolpush, poolpop         — scrivono lo stack (2° arg), coperto dal controllo
#                               separato sugli stack condivisi nel `par`;
#   swap                      — gestito a parte e deliberatamente escluso dal
#                               fixpoint (vedi _walk_proc_int_param_mutations_and_calls).
_BUILTIN_WRITTEN_ARGS = {
    'push':       (0,),
    'pop':        (0,),
    'ssend':      _BUILTIN_ARGS_ALL_BUT_LAST,
    'srecv':      _BUILTIN_ARGS_ALL_BUT_LAST,
    'mnhalve':    (0, 1, 2),
    'mnsplit32':  (0, 1, 2),
    'poolget':    (1,),
    'poolgetneg': (1,),
}


def _builtin_written_arg_names(name, args):
    """Nomi passati a una builtin nelle posizioni che la builtin scrive.

    Lista vuota se `name` non è una builtin che scrive i propri argomenti.
    """
    spec = _BUILTIN_WRITTEN_ARGS.get((name or '').lower())
    if spec is None:
        return []
    args = list(args or [])
    idxs = range(max(0, len(args) - 1)) if spec is _BUILTIN_ARGS_ALL_BUT_LAST else spec
    return [args[i] for i in idxs if i < len(args) and isinstance(args[i], str)]


# ── Precedenza operatori ────────────────────────────────────────────────────
precedence = (
    ('left', 'PLUS', 'MINUS'),
)

# ── Programma ───────────────────────────────────────────────────────────────
def p_program(p):
    '''program : procedure_list'''
    p[0] = ('program', p[1])

def p_procedure_list(p):
    '''procedure_list : procedure
                      | procedure_list procedure'''
    p[0] = [p[1]] if len(p) == 2 else p[1] + [p[2]]

# ── Procedure ───────────────────────────────────────────────────────────────
def p_param_list(p):
    '''param_list : type ID
                  | param_list COMMA type ID'''
    if len(p) == 3:
        p[0] = [(p[1], p[2])]
    else:
        p[0] = p[1] + [(p[3], p[4])]

def p_procedure(p):
    '''procedure : PROCEDURE ID LPAREN RPAREN opt_body
                 | PROCEDURE ID LPAREN param_list RPAREN opt_body'''
    if len(p) == 6:
        p[0] = ('procedure', p[2], [], p[5], p.lineno(1))
        if VERBOSE: print(f"procedure: {p[2]}()")
    else:
        p[0] = ('procedure', p[2], p[4], p[6], p.lineno(1))
        if VERBOSE: print(f"procedure: {p[2]}({p[4]})")

# ── Body ────────────────────────────────────────────────────────────────────
def p_opt_body_empty(p):
    '''opt_body : '''
    p[0] = []

def p_opt_body_nonempty(p):
    '''opt_body : opt_body statement'''
    p[0] = p[1] + [p[2]]

# ── Tipi ────────────────────────────────────────────────────────────────────
def p_type(p):
    '''type : INT
            | STACK
            | CHANNEL'''
    p[0] = p[1]

# ── Espressioni ─────────────────────────────────────────────────────────────
def p_expr_binop(p):
    '''expr : expr PLUS expr
            | expr MINUS expr'''
    p[0] = ('binop', p[2], p[1], p[3])

def p_expr_paren(p):
    '''expr : LPAREN expr RPAREN'''
    p[0] = p[2]

def p_expr_atom(p):
    '''expr : NUMBER
            | ID'''
    p[0] = p[1]

# ── Valori letterali (per local/delocal) ────────────────────────────────────
def p_value(p):
    '''value : NUMBER
             | NIL
             | EMPT
             | ID'''
    p[0] = p[1]

def p_value_negative(p):
    '''value : MINUS NUMBER'''
    # Literal intero negativo per local/delocal (es. `delocal int a = -5`).
    p[0] = -p[2]

# ── Operatori di confronto ──────────────────────────────────────────────────
def p_condition(p):
    '''condition : expr EQEQ expr
                 | expr NEQ  expr
                 | expr GEQ  expr
                 | expr LEQ  expr
                 | expr GT   expr
                 | expr LT   expr'''
    p[0] = ('cond', p[2], p[1], p[3])

# ── Dichiarazioni di tipo ───────────────────────────────────────────────────
def p_type_decl(p):
    '''statement : type ID'''
    # Usa la linea dell'identificatore: il non-terminale "type" non ha sempre lineno affidabile.
    p[0] = ('decl', p[1], p[2], p.lineno(2))
    if VERBOSE: print(f"dichiarazione: {p[2]} ({p[1]})")

# ── Assegnamenti reversibili ────────────────────────────────────────────────
def p_assign(p):
    '''statement : ID PLUSEQUALS expr
                 | ID MINUSEQUALS expr
                 | ID XOREQUALS expr
                 | ID SWAP expr'''
    p[0] = ('assign', p[1], p[2], p[3], p.lineno(1))

# ── Local / Delocal ─────────────────────────────────────────────────────────
def p_local(p):
    '''statement : LOCAL type ID EQUALS value'''
    p[0] = ('local', p[2], p[3], p[5], p.lineno(1))
    if VERBOSE: print(f"local: {p[3]} ({p[2]}) = {p[5]}")

def p_delocal(p):
    '''statement : DELOCAL type ID EQUALS value
                 | DELOCAL type ID'''
    if len(p) == 6:
        p[0] = ('delocal', p[2], p[3], p[5], p.lineno(1))
        if VERBOSE: print(f"delocal: {p[3]} ({p[2]}) = {p[5]}")
    else:
        p[0] = ('delocal', p[2], p[3], None, p.lineno(1))
        if VERBOSE: print(f"delocal: {p[3]} ({p[2]})")

# ── Liste argomenti ─────────────────────────────────────────────────────────
def p_arg_list(p):
    '''arg_list : ID
                | arg_list COMMA ID'''
    p[0] = [p[1]] if len(p) == 2 else p[1] + [p[3]]

def p_sendrecv_payload(p):
    '''sendrecv_payload : LT arg_list GT'''
    p[0] = p[2]

# ── Call / Uncall ───────────────────────────────────────────────────────────
def p_call(p):
    '''statement : CALL ID LPAREN RPAREN
                 | CALL ID LPAREN arg_list RPAREN'''
    if len(p) == 5:
        p[0] = ('call', p[2], [], p.lineno(1))
        if VERBOSE: print(f"call: {p[2]}()")
    else:
        p[0] = ('call', p[2], p[4], p.lineno(1))
        if VERBOSE: print(f"call: {p[2]}({p[4]})")

def p_call_direct(p):
    '''statement : ID LPAREN RPAREN
                 | ID LPAREN arg_list RPAREN
                 | ID LPAREN sendrecv_payload COMMA ID RPAREN'''
    if len(p) == 4:
        p[0] = ('call_direct', p[1], [], p.lineno(1))
        if VERBOSE: print(f"call diretto: {p[1]}()")
    elif len(p) == 5:
        p[0] = ('call_direct', p[1], p[3], p.lineno(1))
        if VERBOSE: print(f"call diretto: {p[1]}({p[3]})")
    else:
        name = p[1]
        if name.lower() not in ('ssend', 'srecv'):
            raise KairosCompileError(
                "PARSER",
                f"riga {p.lineno(1)}: sintassi '<...>' supportata solo per ssend/srecv",
            )
        args = p[3] + [p[5]]
        p[0] = ('call_direct', name, args, p.lineno(1))
        if VERBOSE: print(f"call diretto: {name}(<...>, {p[5]})")

def p_uncall(p):
    '''statement : UNCALL ID LPAREN RPAREN
                 | UNCALL ID LPAREN arg_list RPAREN'''
    if len(p) == 5:
        p[0] = ('uncall', p[2], [], p.lineno(1))
        if VERBOSE: print(f"uncall: {p[2]}()")
    else:
        p[0] = ('uncall', p[2], p[4], p.lineno(1))
        if VERBOSE: print(f"uncall: {p[2]}({p[4]})")

# ── FROM loop ───────────────────────────────────────────────────────────────
def p_from(p):
    '''statement : FROM condition DO opt_body LOOP opt_body UNTIL condition'''
    # Ciclo a due corpi (Janus):  from b1 do c1 loop c2 until b2  → traccia  c1 [c2 c1]*
    # Salviamo sia la linea di FROM che quella di UNTIL per il mapping breakpoint.
    # Il secondo corpo sta IN CODA alla tupla: i visitatori che destrutturano
    # ('from', b1, c1, b2, *_rest) restano validi.
    p[0] = ('from', p[2], p[4], p[8], p.lineno(1), p.lineno(7), p[6])
    if VERBOSE: print(f"from: {p[2]} until: {p[8]}")

# ── IF / ELSE ───────────────────────────────────────────────────────────────
def p_if(p):
    '''statement : IF condition THEN opt_body FI condition
                 | IF condition THEN opt_body ELSE opt_body FI condition'''
    if len(p) == 7:
        p[0] = ('if', p[2], p[4], [], p[6], p.lineno(1))
        if VERBOSE: print(f"if: {p[2]} fi: {p[6]}")
    else:
        p[0] = ('if', p[2], p[4], p[6], p[8], p.lineno(1))
        if VERBOSE: print(f"if: {p[2]} else fi: {p[8]}")

# ── PAR ─────────────────────────────────────────────────────────────────────
def p_par_branch_list(p):
    '''par_branch_list : opt_body
                       | par_branch_list AND opt_body'''
    p[0] = [p[1]] if len(p) == 2 else p[1] + [p[3]]

def p_par(p):
    '''statement : PAR par_branch_list RAP'''
    p[0] = ('par', p[2], p.lineno(1))
    if VERBOSE: print(f"par: {p[2]}")

# ── TRY / ROLLBACK / YRT ─────────────────────────────────────────────────────
# Stile if-fi: condizione di commit dopo `try`, ripetuta dopo `yrt`.
#     try <cond>           rollback opzionale:   try <cond>
#         <body>                                     <body>
#     rollback                                   yrt <cond>
#         <rb_body>
#     yrt <cond>
def p_try(p):
    '''statement : TRY condition opt_body YRT condition
                 | TRY condition opt_body ROLLBACK opt_body YRT condition'''
    if len(p) == 6:
        # try <entry_cond> <body> yrt <exit_cond>   (nessun rollback)
        p[0] = ('try', p[2], p[3], None, p[5], p.lineno(1))
    else:
        # try <entry_cond> <body> rollback <rb_body> yrt <exit_cond>
        p[0] = ('try', p[2], p[3], p[5], p[7], p.lineno(1))
    if VERBOSE: print(f"try: {p[2]}")

# ── Errore ───────────────────────────────────────────────────────────────────
def p_error(p):
    if p:
        raise KairosCompileError("PARSER", f"riga {p.lineno}: token non atteso '{p.value}'")
    else:
        raise KairosCompileError("PARSER", "errore sintattico: fine file inattesa")

def _expr_contains_var(expr, var_name):
    if isinstance(expr, tuple) and len(expr) == 4 and expr[0] == 'binop':
        return _expr_contains_var(expr[2], var_name) or _expr_contains_var(expr[3], var_name)
    return isinstance(expr, str) and expr == var_name


def _is_number_literal(expr_atom):
    if isinstance(expr_atom, int):
        return True
    if not isinstance(expr_atom, str) or not expr_atom:
        return False
    s = expr_atom
    if s[0] == '-':
        s = s[1:]
    return bool(s) and s.isdigit()


def _collect_ids_in_expr(expr, out):
    if isinstance(expr, int):
        return
    if isinstance(expr, tuple) and expr and expr[0] == 'binop':
        _collect_ids_in_expr(expr[2], out)
        _collect_ids_in_expr(expr[3], out)
        return
    if isinstance(expr, str):
        if _is_number_literal(expr):
            return
        if expr.lower() in ('nil', 'empty'):
            return
        out.add(expr)


def _collect_cond_var_ids(cond, out):
    _, _op, lhs, rhs = cond
    _collect_ids_in_expr(lhs, out)
    _collect_ids_in_expr(rhs, out)


def from_second_body(stmt):
    """Secondo corpo (`c2`) di un nodo ('from', b1, c1, b2, from_ln, until_ln, c2).

    Sta in coda alla tupla: i nodi a un corpo (c2 vuoto) e gli AST costruiti
    programmaticamente senza settimo campo restituiscono [].
    """
    return stmt[6] if len(stmt) >= 7 and stmt[6] else []


def _collect_par_branch_var_uses(stmt, out):
    """Identificatori usati (lettura/scrittura) in un branch PAR, per analisi conflitti."""
    if not isinstance(stmt, tuple) or not stmt:
        return
    tag = stmt[0]
    if tag == 'decl':
        return
    if tag == 'assign':
        _, var_name, _op, expr, _lineno = stmt
        out.add(var_name)
        _collect_ids_in_expr(expr, out)
        return
    if tag == 'local':
        _, _t, name, val, _lineno = stmt
        out.add(name)
        if isinstance(val, str) and not _is_number_literal(val) and val.lower() not in ('nil', 'empty'):
            out.add(val)
        return
    if tag == 'delocal':
        _, _t, name, val, _lineno = stmt
        out.add(name)
        if val is not None and isinstance(val, str) and not _is_number_literal(val):
            if val.lower() not in ('nil', 'empty'):
                out.add(val)
        return
    if tag in ('call', 'uncall'):
        _, _name, args, _lineno = stmt
        for a in args or []:
            if isinstance(a, str):
                out.add(a)
        return
    if tag == 'call_direct':
        _, name, args, _lineno = stmt
        if name.lower() == 'show' and isinstance(args, list) and len(args) == 2:
            a0, a1 = args[0], args[1]
            if isinstance(a0, str):
                out.add(a0)
            if isinstance(a1, str) and a1 != 'char':
                out.add(a1)
            return
        for a in args or []:
            if isinstance(a, str):
                out.add(a)
        return
    if tag == 'if':
        _, entry_cond, then_body, else_body, fi_cond, _lineno = stmt
        _collect_cond_var_ids(entry_cond, out)
        for nested in then_body:
            _collect_par_branch_var_uses(nested, out)
        for nested in else_body:
            _collect_par_branch_var_uses(nested, out)
        _collect_cond_var_ids(fi_cond, out)
        return
    if tag == 'from':
        _, entry_cond, body, until_cond, *_rest = stmt
        _collect_cond_var_ids(entry_cond, out)
        for nested in list(body) + from_second_body(stmt):
            _collect_par_branch_var_uses(nested, out)
        _collect_cond_var_ids(until_cond, out)
        return
    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _collect_par_branch_var_uses(nested, out)
        return


def _raise_unresolved_par_callee(name, lineno):
    """Callee non risolvibile dentro un `par`: senza il corpo della procedura non
    si può sapere quali int scrive, e la race passerebbe inosservata."""
    raise KairosCompileError(
        "STATIC",
        (
            f"riga {lineno}: chiamata a '{name}' non risolvibile in blocco PAR "
            f"(non è né una procedura dichiarata né una builtin): "
            f"impossibile stabilire quali int '{name}' scrive; "
            "correggi il nome o dichiara la procedura"
        ),
    )


def _collect_par_branch_int_writes(
    stmt, declared_types, proc_param_lists, proc_int_mutated_formals, out
):
    """Scritture su int nel branch: dirette + tramite call a procedure che mutano parametri int."""
    if not isinstance(stmt, tuple) or not stmt:
        return
    tag = stmt[0]
    if tag == 'assign':
        _, var_name, op, expr, _lineno = stmt
        if op == '<=>':
            if declared_types.get(var_name) == 'int':
                out.add(var_name)
            if isinstance(expr, str) and declared_types.get(expr) == 'int':
                out.add(expr)
            return
        if declared_types.get(var_name) == 'int':
            out.add(var_name)
        return
    if tag == 'local':
        _, tipo, name, _val, _lineno = stmt
        if tipo == 'int':
            out.add(name)
        return
    if tag == 'delocal':
        _, tipo, name, _val, _lineno = stmt
        if tipo == 'int':
            out.add(name)
        return
    if tag == 'call_direct':
        _, name, args, lineno = stmt
        lname = name.lower()
        if lname == 'swap' and isinstance(args, list) and len(args) >= 2:
            for vid in args[:2]:
                if isinstance(vid, str) and declared_types.get(vid) == 'int':
                    out.add(vid)
            return
        # Builtin che scrivono i propri argomenti (pop, srecv, push, ...): sono
        # scritture quanto un 'assign', anche se nell'AST sono un 'call_direct'.
        if lname in _BUILTIN_WRITTEN_ARGS:
            for vid in _builtin_written_arg_names(name, args):
                if declared_types.get(vid) == 'int':
                    out.add(vid)
            return
        if lname in _BUILTIN_CALL_OPCODES:
            return
        pl = proc_param_lists.get(name)
        if pl is None:
            _raise_unresolved_par_callee(name, lineno)
        mf = proc_int_mutated_formals.get(name)
        if pl and mf:
            for i, actual in enumerate(args or []):
                if i >= len(pl):
                    break
                ptype, formal = pl[i]
                if ptype != 'int' or formal not in mf:
                    continue
                if isinstance(actual, str) and declared_types.get(actual) == 'int':
                    out.add(actual)
        return
    if tag in ('call', 'uncall'):
        _, proc_name, args, lineno = stmt
        pl = proc_param_lists.get(proc_name)
        if pl is None:
            _raise_unresolved_par_callee(proc_name, lineno)
        mf = proc_int_mutated_formals.get(proc_name) if pl else None
        if pl and mf:
            for i, actual in enumerate(args or []):
                if i >= len(pl):
                    break
                ptype, formal = pl[i]
                if ptype != 'int' or formal not in mf:
                    continue
                if isinstance(actual, str) and declared_types.get(actual) == 'int':
                    out.add(actual)
        return
    if tag == 'if':
        _, _ec, then_body, else_body, _fc, _lineno = stmt
        for nested in then_body:
            _collect_par_branch_int_writes(
                nested, declared_types, proc_param_lists, proc_int_mutated_formals, out
            )
        for nested in else_body:
            _collect_par_branch_int_writes(
                nested, declared_types, proc_param_lists, proc_int_mutated_formals, out
            )
        return
    if tag == 'from':
        _, _ec, body, _uc, *_rest = stmt
        for nested in list(body) + from_second_body(stmt):
            _collect_par_branch_int_writes(
                nested, declared_types, proc_param_lists, proc_int_mutated_formals, out
            )
        return
    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _collect_par_branch_int_writes(
                    nested, declared_types, proc_param_lists, proc_int_mutated_formals, out
                )
        return


def _walk_proc_int_param_mutations_and_calls(stmt, int_formal_set, mutated_out, calls_out):
    """Raccoglie assegnamenti diretti ai parametri int e le call (per fixpoint)."""
    if not isinstance(stmt, tuple) or not stmt:
        return
    tag = stmt[0]
    if tag == 'assign':
        _, var_name, _op, _expr, _lineno = stmt
        if var_name in int_formal_set:
            mutated_out.add(var_name)
        return
    if tag in ('call', 'uncall'):
        _, proc_name, args, _lineno = stmt
        calls_out.append((proc_name, list(args or [])))
        return
    if tag == 'call_direct':
        _, name, args, _lineno = stmt
        if name.lower() == 'swap':
            return
        # Le builtin non hanno entry in param_lists: il fixpoint le scarterebbe.
        # Quelle che scrivono un argomento (pop, srecv, push, ...) devono comunque
        # marcare come mutato il parametro int che ricevono.
        for vid in _builtin_written_arg_names(name, args):
            if vid in int_formal_set:
                mutated_out.add(vid)
        calls_out.append((name, list(args or [])))
        return
    if tag == 'if':
        _, _ec, then_body, else_body, _fc, _lineno = stmt
        for nested in then_body:
            _walk_proc_int_param_mutations_and_calls(
                nested, int_formal_set, mutated_out, calls_out
            )
        for nested in else_body:
            _walk_proc_int_param_mutations_and_calls(
                nested, int_formal_set, mutated_out, calls_out
            )
        return
    if tag == 'from':
        _, _ec, body, _uc, *_rest = stmt
        for nested in list(body) + from_second_body(stmt):
            _walk_proc_int_param_mutations_and_calls(
                nested, int_formal_set, mutated_out, calls_out
            )
        return
    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _walk_proc_int_param_mutations_and_calls(
                    nested, int_formal_set, mutated_out, calls_out
                )
        return


def _compute_proc_int_mutated_formals(program_procedures):
    """
    Per ogni procedura, insieme dei nomi di parametri int che possono essere mutati
    (assegnamento diretto nel corpo o tramite call che mutano parametri int).
    """
    proc_by_name = {}
    for proc in program_procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        proc_by_name[proc[1]] = proc

    param_lists = {name: (proc_by_name[name][2] or []) for name in proc_by_name}
    int_formals = {
        name: {pn for pt, pn in param_lists[name] if pt == 'int'}
        for name in param_lists
    }

    mutated = {name: set() for name in param_lists}
    call_edges = {name: [] for name in param_lists}

    for name, p in proc_by_name.items():
        body = p[3] or []
        for stmt in body:
            _walk_proc_int_param_mutations_and_calls(
                stmt, int_formals[name], mutated[name], call_edges[name]
            )

    changed = True
    while changed:
        changed = False
        for p_name in param_lists:
            for callee, args in call_edges[p_name]:
                pl = param_lists.get(callee)
                if not pl:
                    continue
                callee_mut = mutated.get(callee)
                if not callee_mut:
                    continue
                for i, actual in enumerate(args):
                    if i >= len(pl):
                        break
                    pt, formal = pl[i]
                    if pt != 'int' or formal not in callee_mut:
                        continue
                    if not isinstance(actual, str):
                        continue
                    if actual not in int_formals[p_name]:
                        continue
                    if actual not in mutated[p_name]:
                        mutated[p_name].add(actual)
                        changed = True

    return param_lists, mutated


def _collect_declared_types(proc):
    declared = {}
    if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
        return declared

    params = proc[2] or []
    body = proc[3] or []
    for ptype, pname in params:
        declared[pname] = ptype

    def walk_stmt(stmt):
        if not isinstance(stmt, tuple) or not stmt:
            return
        tag = stmt[0]
        if tag in ('decl', 'local', 'delocal'):
            if len(stmt) >= 4:
                declared[stmt[2]] = stmt[1]
            return
        if tag == 'if':
            _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
            for nested in then_body:
                walk_stmt(nested)
            for nested in else_body:
                walk_stmt(nested)
            return
        if tag == 'from':
            _, _entry_cond, body, _until_cond, *_rest = stmt
            for nested in list(body) + from_second_body(stmt):
                walk_stmt(nested)
            return
        if tag == 'par':
            _, branches, _lineno = stmt
            for branch in branches:
                for nested in branch:
                    walk_stmt(nested)

    for stmt in body:
        walk_stmt(stmt)
    return declared

def _collect_stack_endpoints_in_stmt(stmt, out):
    if not isinstance(stmt, tuple) or not stmt:
        return

    tag = stmt[0]
    if tag == 'call_direct':
        _, name, args, _lineno = stmt
        lname = name.lower()
        if lname in ('push', 'pop') and isinstance(args, list) and len(args) >= 2:
            out.add(args[1])
        return

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        for nested in then_body:
            _collect_stack_endpoints_in_stmt(nested, out)
        for nested in else_body:
            _collect_stack_endpoints_in_stmt(nested, out)
        return

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        for nested in list(body) + from_second_body(stmt):
            _collect_stack_endpoints_in_stmt(nested, out)
        return

    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _collect_stack_endpoints_in_stmt(nested, out)

def _collect_stack_args_from_call(stmt, proc_signatures, out):
    if not isinstance(stmt, tuple) or not stmt:
        return

    tag = stmt[0]
    if tag in ('call', 'uncall'):
        _, proc_name, args, _lineno = stmt
        param_types = proc_signatures.get(proc_name, [])
        for idx, actual in enumerate(args or []):
            if idx < len(param_types) and param_types[idx] == 'stack' and isinstance(actual, str):
                out.add(actual)
        return

    if tag == 'call_direct':
        _, proc_name, args, _lineno = stmt
        lname = proc_name.lower()
        if lname in ('push', 'pop'):
            return
        param_types = proc_signatures.get(proc_name, [])
        for idx, actual in enumerate(args or []):
            if idx < len(param_types) and param_types[idx] == 'stack' and isinstance(actual, str):
                out.add(actual)
        return

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        for nested in then_body:
            _collect_stack_args_from_call(nested, proc_signatures, out)
        for nested in else_body:
            _collect_stack_args_from_call(nested, proc_signatures, out)
        return

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        for nested in list(body) + from_second_body(stmt):
            _collect_stack_args_from_call(nested, proc_signatures, out)
        return

    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _collect_stack_args_from_call(nested, proc_signatures, out)


def _check_stmt_reversibility(
    stmt, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
):
    if not isinstance(stmt, tuple) or not stmt:
        return

    tag = stmt[0]
    if tag == 'assign':
        _, var_name, op, expr, lineno = stmt
        if op in ('+=', '-=', '^=') and _expr_contains_var(expr, var_name):
            raise KairosCompileError(
                "STATIC",
                (
                    f"riga {lineno}: operazione non reversibile '{var_name} {op} ...' "
                    f"(la variabile a sinistra compare anche nell'espressione a destra)"
                ),
            )
        return

    if tag == 'delocal':
        _, _tipo, name, val, lineno = stmt
        if isinstance(val, str) and val == name:
            raise KairosCompileError(
                "STATIC",
                (
                    f"riga {lineno}: DELOCAL non ammesso '{name} = {val}': il valore atteso "
                    f"non può essere lo stesso identificatore (vincolo banale, inversione non "
                    f"determinata). Usa un letterale o un altro nome di variabile."
                ),
            )
        return

    if tag == 'call_direct':
        _, name, args, lineno = stmt
        if name.lower() == 'show':
            if not args:
                raise KairosCompileError(
                    "STATIC",
                    f"riga {lineno}: show richiede almeno un argomento (variabile int)",
                )
            if len(args) > 2:
                raise KairosCompileError(
                    "STATIC",
                    f"riga {lineno}: show accetta al massimo due argomenti",
                )
            if len(args) == 2 and args[1] != 'char':
                raise KairosCompileError(
                    "STATIC",
                    (
                        f"riga {lineno}: secondo argomento di show deve essere il "
                        f"formato letterale 'char' (es. show(x, char))"
                    ),
                )
        return

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        for nested in then_body:
            _check_stmt_reversibility(
                nested, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
        for nested in else_body:
            _check_stmt_reversibility(
                nested, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
        return

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        for nested in list(body) + from_second_body(stmt):
            _check_stmt_reversibility(
                nested, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
        return

    if tag == 'par':
        _, branches, lineno = stmt
        stack_usage_by_var = {}
        branch_access = []
        branch_direct_int_writes = []
        for branch_idx, branch in enumerate(branches):
            used_in_branch = set()
            par_ids = set()
            writes = set()
            for nested in branch:
                _collect_stack_endpoints_in_stmt(nested, used_in_branch)
                _collect_stack_args_from_call(nested, proc_signatures, used_in_branch)
                _collect_par_branch_var_uses(nested, par_ids)
                _collect_par_branch_int_writes(
                    nested, declared_types, proc_param_lists, proc_int_mutated_formals, writes
                )
                _check_stmt_reversibility(
                    nested, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
                )
            for var_name in used_in_branch:
                if declared_types.get(var_name) == 'stack':
                    stack_usage_by_var.setdefault(var_name, set()).add(branch_idx)
            branch_access.append(par_ids)
            branch_direct_int_writes.append(writes)

        for var_name, branch_ids in stack_usage_by_var.items():
            if len(branch_ids) > 1:
                branches_txt = ", ".join(str(i) for i in sorted(branch_ids))
                raise KairosCompileError(
                    "STATIC",
                    (
                        f"riga {lineno}: uso non reversibile di stack condiviso '{var_name}' "
                        f"in blocco PAR (branch: {branches_txt}); "
                        "usa channel/ssend/srecv o separa le strutture tra i branch"
                    ),
                )

        int_access_sets = [
            {v for v in acc if declared_types.get(v) == 'int'}
            for acc in branch_access
        ]
        if _ParStaticConfig.check_int_race:
            for i in range(len(branch_direct_int_writes)):
                for j in range(i + 1, len(branch_direct_int_writes)):
                    w_i = branch_direct_int_writes[i]
                    w_j = branch_direct_int_writes[j]
                    acc_i = int_access_sets[i]
                    acc_j = int_access_sets[j]
                    shared = (w_i & acc_j) | (w_j & acc_i)
                    if shared:
                        names = ", ".join(sorted(shared))
                        raise KairosCompileError(
                            "STATIC",
                            (
                                f"riga {lineno}: race su int nel PAR (scrittura vs accesso, anche tramite call): "
                                f"{names} (usa channel/ssend/srecv o variabili distinte per branch)"
                            ),
                        )

        # La disciplina di sessione sui canali sta tutta altrove: la garanzia
        # (al più due titolari per canale, in ogni istante) la dà il controllo
        # dinamico nella VM, src/vm/vm_session.h; qui resta solo l'anticipo dei
        # conflitti di protocollo visibili già dal sorgente. Una riga sola, per
        # poterla togliere senza toccare nient'altro.
        check_sessions(
            branches, proc_signatures, _ParStaticConfig.program_procedures, lineno
        )

def run_static_checks(program_ast, *, check_par_int_race: bool = True):
    if not isinstance(program_ast, tuple) or not program_ast or program_ast[0] != 'program':
        raise KairosCompileError("PARSER", "AST del programma non valido")

    prev_race = _ParStaticConfig.check_int_race
    prev_procs = _ParStaticConfig.program_procedures
    _ParStaticConfig.check_int_race = check_par_int_race
    _ParStaticConfig.program_procedures = tuple(program_ast[1] or [])
    try:
        _run_static_checks_body(program_ast)
    finally:
        _ParStaticConfig.check_int_race = prev_race
        _ParStaticConfig.program_procedures = prev_procs


def _run_static_checks_body(program_ast):
    procedures = program_ast[1] if len(program_ast) > 1 else []
    proc_signatures = {}
    for proc in procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        proc_name = proc[1]
        params = proc[2] or []
        proc_signatures[proc_name] = [ptype for ptype, _pname in params]

    proc_param_lists, proc_int_mutated_formals = _compute_proc_int_mutated_formals(procedures)

    for proc in procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        declared_types = _collect_declared_types(proc)
        body = proc[3] or []
        for stmt in body:
            _check_stmt_reversibility(
                stmt, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )


# ── Desugaring di try/commit/rollback ────────────────────────────────────────
# Il costrutto
#     try <body> commit <cond> rollback <rb> yrt
# viene riscritto, prima dei controlli statici e del bytecode, in costrutti
# esistenti (call / uncall / if-fi). Il body diventa una procedura sintetica
# `__try_body_N(freevars)`; il rollback (se presente) `__try_rb_N(freevars)`:
#
#     call __try_body_N(fv_body)
#     if <cond> then
#         // commit: il body resta applicato
#     else
#         uncall __try_body_N(fv_body)   // annulla il body (inversione)
#         call  __try_rb_N(fv_rb)        // esegue il rollback
#     fi <cond>
#
# I parametri sono le *free variable* del blocco (id usati ma non dichiarati
# `local` al suo interno): CALL le linka per riferimento (aliasing), quindi la
# procedura sintetica muta le celle del chiamante in-place. L'`else` contiene
# call/uncall → la VM imposta has_call e salta il check di reversibilità del
# `fi` (vm_ops.h:op_assert), così la condizione del `fi` resta solo sintattica.

_FORBIDDEN_IN_TRY = ('par',)  # v1: niente par/canali dentro il body del try


def _proc_contains_ssend_srecv(name, proc_bodies, cache, visiting):
    """ssend/srecv raggiungibili dal corpo della procedura `name`, anche
    attraverso call/uncall annidate (per il divieto try/rollback: vedi
    _try_assert_no_forbidden). Memoizzato; `visiting` evita loop su
    ricorsione/mutua ricorsione tra procedure."""
    if name in cache:
        return cache[name]
    if name in visiting:
        return False
    body = proc_bodies.get(name)
    if body is None:
        return False
    visiting.add(name)
    found = _stmts_contain_ssend_srecv(body, proc_bodies, cache, visiting)
    visiting.discard(name)
    cache[name] = found
    return found


def _stmts_contain_ssend_srecv(stmts, proc_bodies, cache, visiting):
    for s in stmts:
        if not isinstance(s, tuple) or not s:
            continue
        tag = s[0]
        if tag == 'call_direct' and s[1] in ('ssend', 'srecv'):
            return True
        if tag in ('call', 'uncall'):
            if _proc_contains_ssend_srecv(s[1], proc_bodies, cache, visiting):
                return True
        elif tag == 'if':
            if _stmts_contain_ssend_srecv(s[2], proc_bodies, cache, visiting):
                return True
            if _stmts_contain_ssend_srecv(s[3], proc_bodies, cache, visiting):
                return True
        elif tag == 'from':
            if _stmts_contain_ssend_srecv(s[2], proc_bodies, cache, visiting):
                return True
            if _stmts_contain_ssend_srecv(from_second_body(s), proc_bodies, cache, visiting):
                return True
        elif tag == 'par':
            for branch in s[1]:
                if _stmts_contain_ssend_srecv(branch, proc_bodies, cache, visiting):
                    return True
    return False


def _try_free_vars(stmts):
    """Id usati ma non dichiarati `local`/`decl` dentro `stmts` (ricorsivo)."""
    used, bound = set(), set()
    _try_collect_vars(stmts, used, bound)
    return used - bound


def _try_collect_vars(stmts, used, bound):
    for s in stmts:
        if not isinstance(s, tuple) or not s:
            continue
        tag = s[0]
        if tag == 'decl':            # ('decl', tipo, name, ln)
            bound.add(s[2])
        elif tag == 'local':         # ('local', tipo, name, val, ln)
            bound.add(s[2])
            _collect_ids_in_expr(s[3], used)
        elif tag == 'delocal':       # ('delocal', tipo, name, val|None, ln)
            bound.add(s[2])
            if s[3] is not None:
                _collect_ids_in_expr(s[3], used)
        elif tag == 'assign':        # ('assign', var, op, expr, ln)
            used.add(s[1])
            _collect_ids_in_expr(s[3], used)
        elif tag in ('call', 'uncall', 'call_direct'):  # (_, name, args, ln)
            for a in s[2]:
                # `char` è il flag di formato di `show(x, char)`, non una
                # variabile; `nil`/`empty` sono letterali stack/channel.
                if isinstance(a, str) and a not in ('char', 'nil', 'empty'):
                    used.add(a)
        elif tag == 'if':            # ('if', ec, then, else, fc, ln)
            _collect_cond_var_ids(s[1], used)
            _collect_cond_var_ids(s[4], used)
            _try_collect_vars(s[2], used, bound)
            _try_collect_vars(s[3], used, bound)
        elif tag == 'from':          # ('from', ec, c1, uc, from_ln, until_ln, c2)
            _collect_cond_var_ids(s[1], used)
            _collect_cond_var_ids(s[3], used)
            _try_collect_vars(s[2], used, bound)
            _try_collect_vars(from_second_body(s), used, bound)
        elif tag == 'par':
            for branch in s[1]:
                _try_collect_vars(branch, used, bound)


def _try_assert_no_forbidden(stmts, lineno, proc_bodies, ssend_srecv_cache):
    for s in stmts:
        if not isinstance(s, tuple) or not s:
            continue
        if s[0] in _FORBIDDEN_IN_TRY or s[0] == 'call_direct' and s[1] in ('ssend', 'srecv'):
            raise KairosCompileError(
                "PARSER",
                f"riga {lineno}: '{s[0] if s[0]!='call_direct' else s[1]}' non ammesso "
                "nel body di un blocco try (v1)",
            )
        if s[0] in ('call', 'uncall') and _proc_contains_ssend_srecv(
            s[1], proc_bodies, ssend_srecv_cache, set()
        ):
            raise KairosCompileError(
                "PARSER",
                f"riga {lineno}: '{s[0]} {s[1]}' non ammesso nel body di un blocco try (v1): "
                f"'{s[1]}' contiene ssend/srecv, anche indirettamente — un try/rollback non può "
                "disfare un rendez-vous già consumato dal partner senza la sua cooperazione",
            )
        if s[0] == 'if':
            _try_assert_no_forbidden(s[2], lineno, proc_bodies, ssend_srecv_cache)
            _try_assert_no_forbidden(s[3], lineno, proc_bodies, ssend_srecv_cache)
        elif s[0] == 'from':
            _try_assert_no_forbidden(s[2], lineno, proc_bodies, ssend_srecv_cache)
            _try_assert_no_forbidden(from_second_body(s), lineno, proc_bodies, ssend_srecv_cache)


def _desugar_stmts(stmts, new_procs, counter, proc_bodies, ssend_srecv_cache):
    out = []
    for s in stmts:
        out.extend(_desugar_one(s, new_procs, counter, proc_bodies, ssend_srecv_cache))
    return out


def _desugar_one(s, new_procs, counter, proc_bodies, ssend_srecv_cache):
    if not isinstance(s, tuple) or not s:
        return [s]
    tag = s[0]
    if tag == 'if':
        _, ec, tb, eb, fc, ln = s
        return [('if', ec, _desugar_stmts(tb, new_procs, counter, proc_bodies, ssend_srecv_cache),
                 _desugar_stmts(eb, new_procs, counter, proc_bodies, ssend_srecv_cache), fc, ln)]
    if tag == 'from':
        body = _desugar_stmts(s[2], new_procs, counter, proc_bodies, ssend_srecv_cache)
        rest = list(s[4:])
        if len(s) >= 7:
            # settimo campo = secondo corpo: va desugarato anch'esso.
            rest[2] = _desugar_stmts(s[6], new_procs, counter, proc_bodies, ssend_srecv_cache)
        return [('from', s[1], body, s[3], *rest)]
    if tag == 'par':
        _, branches, ln = s
        return [('par', [_desugar_stmts(b, new_procs, counter, proc_bodies, ssend_srecv_cache)
                          for b in branches], ln)]
    if tag == 'try':
        _, entry_cond, body, rb_body, exit_cond, ln = s
        # innermost-first: i try annidati sono già riscritti qui sotto.
        body = _desugar_stmts(body, new_procs, counter, proc_bodies, ssend_srecv_cache)
        _try_assert_no_forbidden(body, ln, proc_bodies, ssend_srecv_cache)
        uid = counter[0]; counter[0] += 1
        try_name = f"__try_body_{uid}"
        fv_body = sorted(_try_free_vars(body))
        new_procs.append(('procedure', try_name,
                          [('int', v) for v in fv_body], body, ln))
        else_body = [('uncall', try_name, list(fv_body), ln)]
        if rb_body is not None:
            rb_body = _desugar_stmts(rb_body, new_procs, counter, proc_bodies, ssend_srecv_cache)
            _try_assert_no_forbidden(rb_body, ln, proc_bodies, ssend_srecv_cache)
            rb_name = f"__try_rb_{uid}"
            fv_rb = sorted(_try_free_vars(rb_body))
            new_procs.append(('procedure', rb_name,
                              [('int', v) for v in fv_rb], rb_body, ln))
            else_body.append(('call', rb_name, list(fv_rb), ln))
        # entry_cond decide il commit (valutata dopo il body); exit_cond è il
        # mirror del `fi` (soppresso da has_call perché l'else contiene call).
        return [
            ('call', try_name, list(fv_body), ln),
            ('if', entry_cond, [], else_body, exit_cond, ln),
        ]
    return [s]


def desugar_try(program_ast):
    """Riscrive tutti i nodi `try` del programma in call/uncall/if-fi.

    Ritorna un nuovo AST `('program', procedure_list)` con le procedure
    sintetiche del body/rollback appese in coda. Idempotente sui programmi
    senza `try`.
    """
    if not program_ast or program_ast[0] != 'program':
        return program_ast
    procedures = program_ast[1] if len(program_ast) > 1 else []
    # Mappa nome->corpo ORIGINALE (pre-desugar) di tutte le procedure del
    # programma: serve a _try_assert_no_forbidden per seguire call/uncall
    # transitivamente e vietare ssend/srecv raggiunti indirettamente dentro
    # un try/rollback. Il desugar di `try` non introduce né rimuove
    # ssend/srecv, quindi il corpo pre-desugar è equivalente ai fini di
    # questo controllo, ed è disponibile per intero fin da subito (a
    # differenza di out_procs, costruito incrementalmente sotto).
    proc_bodies = {
        proc[1]: proc[3]
        for proc in procedures
        if isinstance(proc, tuple) and len(proc) >= 5 and proc[0] == 'procedure'
    }
    ssend_srecv_cache = {}
    counter = [0]
    out_procs = []
    for proc in procedures:
        if not isinstance(proc, tuple) or len(proc) < 5 or proc[0] != 'procedure':
            out_procs.append(proc)
            continue
        _, name, params, body, ln = proc
        # Le procedure sintetiche del try vanno emesse PRIMA della procedura che
        # le usa: in Kairos `main` (o un'altra proc) viene eseguita appena lo
        # scan del prepass incontra il suo PROC, quindi i callee devono essere
        # già registrati (definiti più in alto nel file).
        new_procs = []
        new_body = _desugar_stmts(body, new_procs, counter, proc_bodies, ssend_srecv_cache)
        out_procs.extend(new_procs)
        out_procs.append(('procedure', name, params, new_body, ln))
    return ('program', out_procs)


_nulllog = logging.getLogger('ply.nulllog')
_nulllog.addHandler(logging.NullHandler())

parser = yacc.yacc(
    errorlog=_nulllog,
    debuglog=_nulllog,
    outputdir=tempfile.gettempdir(),
    debug=False,
)
if __name__ == '__main__':
    VERBOSE = True
    if len(sys.argv) < 2:
        print("Uso: python Jparser.py <file>")
        sys.exit(1)
    with open(sys.argv[1], 'r') as f:
        source = f.read()
    result = parser.parse(source, lexer=lexer)
    print(result)