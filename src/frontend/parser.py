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

    # Procedure del programma, per l'inferenza dei tipi di sessione: una
    # chiamata dentro un ramo di `par` contribuisce il protocollo del corpo
    # della callee, quindi serve poterla risolvere dal punto in cui si controlla.
    program_procedures: tuple = ()


# ── Builtin ─────────────────────────────────────────────────────────────────
# Chiamate tipo foo(x) senza `call`: opcode VM diretto, non CALL nome_proc.
# (Usato anche da bytecode.py, che importa questo nome da qui.)
_BUILTIN_CALL_OPCODES = frozenset({'show', 'push', 'pop', 'ssend', 'srecv'})

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
# Volutamente assenti:
#   show                      — sola lettura;
#   swap                      — gestito a parte e deliberatamente escluso dal
#                               fixpoint (vedi _walk_proc_int_param_mutations_and_calls).
_BUILTIN_WRITTEN_ARGS = {
    'push':       (0,),
    'pop':        (0,),
    'ssend':      _BUILTIN_ARGS_ALL_BUT_LAST,
    'srecv':      _BUILTIN_ARGS_ALL_BUT_LAST,
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
    ('left', 'OROR'),
    ('left', 'ANDAND'),
    ('nonassoc', 'EQEQ', 'NEQ', 'GEQ', 'LEQ', 'GT', 'LT'),
    ('left', 'PLUS', 'MINUS'),
    ('left', 'TIMES', 'DIVIDE', 'MOD'),
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
def p_param(p):
    '''param : type ID
             | type ID LBRACKET RBRACKET'''
    # Un array passato come parametro ha tipo 'array' (per riferimento, come
    # gli stack); la lunghezza e' quella dell'array dichiarato dal chiamante.
    if len(p) == 5:
        if p[1] != 'int':
            raise KairosCompileError("PARSER", f"riga {p.lineno(2)}: solo gli int possono essere array")
        p[0] = ('array', p[2])
    else:
        p[0] = (p[1], p[2])

def p_param_list(p):
    '''param_list : param
                  | param_list COMMA param'''
    p[0] = [p[1]] if len(p) == 2 else p[1] + [p[3]]

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

def p_opt_body_skip(p):
    '''opt_body : opt_body SKIP'''
    p[0] = p[1]          # skip: comando vuoto, non genera nulla

# ── Tipi ────────────────────────────────────────────────────────────────────
def p_type(p):
    '''type : INT
            | STACK
            | CHANNEL'''
    p[0] = p[1]

# ── Espressioni ─────────────────────────────────────────────────────────────
def p_expr_binop(p):
    '''expr : expr PLUS expr
            | expr MINUS expr
            | expr TIMES expr
            | expr DIVIDE expr
            | expr MOD expr
            | expr EQEQ expr
            | expr NEQ  expr
            | expr GEQ  expr
            | expr LEQ  expr
            | expr GT   expr
            | expr LT   expr
            | expr ANDAND expr
            | expr OROR expr'''
    # Come in Janus, confronti e connettivi sono operatori delle espressioni:
    # valgono 1 (vero) o 0 (falso).
    p[0] = ('binop', p[2], p[1], p[3])

def p_expr_paren(p):
    '''expr : LPAREN expr RPAREN'''
    p[0] = p[2]

def p_expr_index(p):
    '''expr : ID LBRACKET expr RBRACKET'''
    p[0] = ('index', p[1], p[3])

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
_COMPARE_OPS = ('==', '!=', '>=', '<=', '>', '<')

def p_condition(p):
    '''condition : expr'''
    # Una guardia e' un'espressione, vera se diversa da zero. Se in cima c'e'
    # un confronto la si tiene nella forma `lhs op rhs`, come prima; altrimenti
    # (connettivi, o un'espressione qualsiasi) diventa `e != 0`.
    e = p[1]
    if isinstance(e, tuple) and e and e[0] == 'binop' and e[1] in _COMPARE_OPS:
        p[0] = ('cond', e[1], e[2], e[3])
    else:
        p[0] = ('cond', '!=', e, 0)

# ── Dichiarazioni di tipo ───────────────────────────────────────────────────
def p_type_decl(p):
    '''statement : type ID'''
    # Usa la linea dell'identificatore: il non-terminale "type" non ha sempre lineno affidabile.
    p[0] = ('decl', p[1], p[2], p.lineno(2))
    if VERBOSE: print(f"dichiarazione: {p[2]} ({p[1]})")

# ── Assegnamenti reversibili ────────────────────────────────────────────────
def p_assign(p):
    '''statement : lvalue PLUSEQUALS expr
                 | lvalue MINUSEQUALS expr
                 | lvalue XOREQUALS expr
                 | lvalue SWAP expr'''
    if p[2] == '<=>' and not (isinstance(p[3], str) or
                              (isinstance(p[3], tuple) and p[3][0] == 'index')):
        raise KairosCompileError(
            "PARSER", f"riga {p.lineno(2)}: '<=>' scambia due luoghi (variabile o cella)")
    p[0] = ('assign', p[1], p[2], p[3], p.lineno(2))

def p_lvalue(p):
    '''lvalue : ID
              | ID LBRACKET expr RBRACKET'''
    p[0] = p[1] if len(p) == 2 else ('index', p[1], p[3])

# ── Local / Delocal ─────────────────────────────────────────────────────────
def p_local(p):
    '''statement : LOCAL type ID EQUALS value'''
    p[0] = ('local', p[2], p[3], p[5], p.lineno(1))
    if VERBOSE: print(f"local: {p[3]} ({p[2]}) = {p[5]}")

def p_local_array(p):
    '''statement : LOCAL type ID LBRACKET NUMBER RBRACKET EQUALS value'''
    if p[2] != 'int':
        raise KairosCompileError("PARSER", f"riga {p.lineno(1)}: solo gli int possono essere array")
    if p[5] <= 0:
        raise KairosCompileError("PARSER", f"riga {p.lineno(1)}: lunghezza array non valida {p[5]}")
    # La lunghezza viaggia nel tipo ('array:N'): la tupla resta di 5 elementi.
    p[0] = ('local', f'array:{p[5]}', p[3], p[8], p.lineno(1))

def p_delocal_array(p):
    '''statement : DELOCAL type ID LBRACKET NUMBER RBRACKET EQUALS value'''
    if p[2] != 'int':
        raise KairosCompileError("PARSER", f"riga {p.lineno(1)}: solo gli int possono essere array")
    p[0] = ('delocal', f'array:{p[5]}', p[3], p[8], p.lineno(1))

def p_delocal(p):
    '''statement : DELOCAL type ID EQUALS value
                 | DELOCAL type ID'''
    if len(p) == 6:
        p[0] = ('delocal', p[2], p[3], p[5], p.lineno(1))
        if VERBOSE: print(f"delocal: {p[3]} ({p[2]}) = {p[5]}")
    else:
        # Come in Janus il delocal dice il valore atteso: e' cio' che rende
        # l'allocazione invertibile.
        raise KairosCompileError(
            "PARSER", f"riga {p.lineno(1)}: delocal {p[2]} {p[3]} senza il valore atteso")
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
def p_opt_do(p):
    '''opt_do : DO opt_body
              | '''
    p[0] = p[2] if len(p) == 3 else []

def p_opt_loop(p):
    '''opt_loop : LOOP opt_body
                | '''
    p[0] = p[2] if len(p) == 3 else []

def p_from(p):
    '''statement : FROM condition opt_do opt_loop UNTIL condition'''
    # Ciclo a due corpi (Janus):  from b1 do c1 loop c2 until b2  → traccia  c1 [c2 c1]*
    # Le clausole `do` e `loop` sono facoltative, come in Janus: una assente vale skip.
    # Salviamo sia la linea di FROM che quella di UNTIL per il mapping breakpoint.
    # Il secondo corpo sta IN CODA alla tupla: i visitatori che destrutturano
    # ('from', b1, c1, b2, *_rest) restano validi.
    p[0] = ('from', p[2], p[3], p[6], p.lineno(1), p.lineno(5), p[4])
    if VERBOSE: print(f"from: {p[2]} until: {p[6]}")

# ── IF / ELSE ───────────────────────────────────────────────────────────────
def p_opt_then(p):
    '''opt_then : THEN opt_body
                | '''
    p[0] = p[2] if len(p) == 3 else []

def p_opt_else(p):
    '''opt_else : ELSE opt_body
                | '''
    p[0] = p[2] if len(p) == 3 else []

def p_if(p):
    '''statement : IF condition opt_then opt_else FI condition'''
    # `then` ed `else` sono facoltativi come in Janus: un ramo assente vale skip.
    p[0] = ('if', p[2], p[3], p[4], p[6], p.lineno(1))
    if VERBOSE: print(f"if: {p[2]} fi: {p[6]}")

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

def _lv_base(lv):
    """Nome della variabile bersaglio: `x` per `x`, `a` per la cella `a[e]`."""
    return lv[1] if isinstance(lv, tuple) else lv


def _lv_text(lv):
    """Il bersaglio come lo ha scritto il programmatore: `x` oppure `a[...]`."""
    return f"{lv[1]}[...]" if isinstance(lv, tuple) else lv


def _intlike(t):
    """Tipi che si comportano come int per i controlli di race e di mutazione."""
    return t == 'int' or (isinstance(t, str) and t.startswith('array'))


def _expr_contains_var(expr, var_name):
    """`var_name` compare in `expr`? Per una cella `a[e]` come bersaglio conta il
    nome dell'array: `a[i] += a[j]` e' rifiutato come `x += x`."""
    base = _lv_base(var_name)
    if isinstance(expr, tuple) and expr and expr[0] == 'binop':
        return _expr_contains_var(expr[2], var_name) or _expr_contains_var(expr[3], var_name)
    if isinstance(expr, tuple) and expr and expr[0] == 'index':
        return expr[1] == base or _expr_contains_var(expr[2], var_name)
    return isinstance(expr, str) and expr == base


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
    if isinstance(expr, tuple) and expr and expr[0] == 'index':
        out.add(expr[1])
        _collect_ids_in_expr(expr[2], out)
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
        if isinstance(var_name, tuple):
            _collect_ids_in_expr(var_name, out)       # cella: array + variabili dell'indice
        else:
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
        var_name = _lv_base(var_name)
        if op == '<=>':
            if _intlike(declared_types.get(var_name)):
                out.add(var_name)
            other = _lv_base(expr) if isinstance(expr, (str, tuple)) else None
            if isinstance(other, str) and _intlike(declared_types.get(other)):
                out.add(other)
            return
        if _intlike(declared_types.get(var_name)):
            out.add(var_name)
        return
    if tag == 'local':
        _, tipo, name, _val, _lineno = stmt
        if _intlike(tipo):
            out.add(name)
        return
    if tag == 'delocal':
        _, tipo, name, _val, _lineno = stmt
        if _intlike(tipo):
            out.add(name)
        return
    if tag == 'call_direct':
        _, name, args, lineno = stmt
        lname = name.lower()
        if lname == 'swap' and isinstance(args, list) and len(args) >= 2:
            for vid in args[:2]:
                if isinstance(vid, str) and _intlike(declared_types.get(vid)):
                    out.add(vid)
            return
        # Builtin che scrivono i propri argomenti (pop, srecv, push, ...): sono
        # scritture quanto un 'assign', anche se nell'AST sono un 'call_direct'.
        if lname in _BUILTIN_WRITTEN_ARGS:
            for vid in _builtin_written_arg_names(name, args):
                if _intlike(declared_types.get(vid)):
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
                if not _intlike(ptype) or formal not in mf:
                    continue
                if isinstance(actual, str) and _intlike(declared_types.get(actual)):
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
                if not _intlike(ptype) or formal not in mf:
                    continue
                if isinstance(actual, str) and _intlike(declared_types.get(actual)):
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
        _, var_name, op, expr, _lineno = stmt
        sides = [var_name] + ([expr] if op == '<=>' else [])   # lo scambio scrive entrambi
        for side in sides:
            base = _lv_base(side)
            if base in int_formal_set:
                mutated_out.add(base)
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
        name: {pn for pt, pn in param_lists[name] if _intlike(pt)}
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
                    if not _intlike(pt) or formal not in callee_mut:
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
        # Ogni argomento conta (push/pop, payload e destinazioni di ssend/srecv,
        # show): il chiamante tiene solo quelli dichiarati stack.
        for a in args or []:
            if isinstance(a, str):
                out.add(a)
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
        # L'indice di una cella scritta non puo' dipendere dalle celle scritte:
        # `a[a[0]] += 1` cambierebbe a[0] e l'inverso agirebbe su un'altra cella.
        sides = [var_name] + ([expr] if op == '<=>' else [])
        written = {_lv_base(lv) for lv in sides}
        for lv in sides:
            if isinstance(lv, tuple) and lv and lv[0] == 'index':
                if any(_expr_contains_var(lv[2], w) for w in written):
                    raise KairosCompileError(
                        "STATIC",
                        f"riga {lineno}: l'indice di '{_lv_text(lv)}' dipende da una variabile "
                        f"scritta dalla stessa istruzione (operazione non reversibile)")
        if op in ('+=', '-=', '^=') and _expr_contains_var(expr, var_name):
            raise KairosCompileError(
                "STATIC",
                (
                    f"riga {lineno}: operazione non reversibile '{_lv_text(var_name)} {op} ...' "
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
        for body in (then_body, else_body):
            _check_list_reversibility(
                body, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
        return

    if tag == 'from':
        for body in (stmt[2], from_second_body(stmt)):
            _check_list_reversibility(
                body, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
        return

    if tag == 'par':
        _, branches, lineno = stmt
        stack_usage_by_var = {}
        branch_access = []
        branch_direct_int_writes = []
        # Tipi: quelli in scope al par, piu' i nomi che ciascun ramo introduce
        # (due rami che aprono lo stesso local userebbero lo stesso slot del
        # frame, quindi contano anch'essi).
        branch_envs = []
        for branch in branches:
            benv = _KindEnv(declared_types)
            for name, t in _all_declarations(branch):
                benv.add(name, t)
            branch_envs.append(benv)
        for branch_idx, branch in enumerate(branches):
            benv = branch_envs[branch_idx]
            used_in_branch = set()
            par_ids = set()
            writes = set()
            for nested in branch:
                _collect_stack_endpoints_in_stmt(nested, used_in_branch)
                _collect_stack_args_from_call(nested, proc_signatures, used_in_branch)
                _collect_par_branch_var_uses(nested, par_ids)
                _collect_par_branch_int_writes(
                    nested, benv, proc_param_lists, proc_int_mutated_formals, writes
                )
            _check_list_reversibility(
                branch, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
            for var_name in used_in_branch:
                if benv.is_stack(var_name):
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
            {v for v in acc if _intlike(branch_envs[k].get(v))}
            for k, acc in enumerate(branch_access)
        ]
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

class _KindEnv:
    """Tipi dei nomi visti da un ramo di par. Un nome dichiarato piu' volte nel
    ramo con tipi diversi (blocchi disgiunti) li ha tutti: per i controlli
    sulle corse conta ogni uso, quindi `get` risponde con un tipo intero se il
    nome lo e' mai stato, poi stack, poi l'ultimo visto."""
    def __init__(self, base):
        self.kinds = {k: [v] for k, v in base.items()}

    def add(self, name, t):
        self.kinds.setdefault(name, []).append(t)

    def get(self, name, default=None):
        ts = self.kinds.get(name)
        if not ts:
            return default
        for t in ts:
            if _intlike(t):
                return t
        return 'stack' if 'stack' in ts else ts[-1]

    def is_stack(self, name):
        return 'stack' in self.kinds.get(name, [])


def _all_declarations(stmts):
    """Coppie (nome, tipo) di ogni decl/local in `stmts`, a ogni profondita'."""
    out = []
    for st in stmts or []:
        if not isinstance(st, tuple) or not st:
            continue
        if st[0] in ('decl', 'local'):
            out.append((st[2], st[1]))
        elif st[0] == 'if':
            out += _all_declarations(st[2]) + _all_declarations(st[3])
        elif st[0] == 'from':
            out += _all_declarations(st[2]) + _all_declarations(from_second_body(st))
        elif st[0] == 'par':
            for b in st[1]:
                out += _all_declarations(b)
    return out


def _check_list_reversibility(
    stmts, env, proc_signatures, proc_param_lists, proc_int_mutated_formals
):
    """Controlla una sequenza di comandi con i tipi dello scope: ogni local o
    dichiarazione vale per i comandi che seguono, fino al suo delocal."""
    env = dict(env)
    for st in stmts or []:
        _check_stmt_reversibility(
            st, env, proc_signatures, proc_param_lists, proc_int_mutated_formals
        )
        if isinstance(st, tuple) and st and st[0] in ('local', 'decl'):
            env[st[2]] = st[1]
        elif isinstance(st, tuple) and st and st[0] == 'delocal':
            env.pop(st[2], None)


def run_static_checks(program_ast):
    if not isinstance(program_ast, tuple) or not program_ast or program_ast[0] != 'program':
        raise KairosCompileError("PARSER", "AST del programma non valido")

    prev_procs = _ParStaticConfig.program_procedures
    _ParStaticConfig.program_procedures = tuple(program_ast[1] or [])
    try:
        _run_static_checks_body(program_ast)
    finally:
        _ParStaticConfig.program_procedures = prev_procs


def _stmt_own_ids(s):
    """Identificatori letti o scritti da `s` stesso (non dai corpi annidati)."""
    used = set()
    tag = s[0]
    if tag == 'local':
        _collect_ids_in_expr(s[3], used)
    elif tag == 'delocal':
        used.add(s[2])
        if s[3] is not None:
            _collect_ids_in_expr(s[3], used)
    elif tag == 'assign':
        if isinstance(s[1], tuple):
            _collect_ids_in_expr(s[1], used)
        else:
            used.add(s[1])
        _collect_ids_in_expr(s[3], used)
    elif tag in ('call', 'uncall', 'call_direct'):
        for a in s[2]:
            if a not in ('char', 'nil', 'empty'):
                _collect_ids_in_expr(a, used)
    elif tag == 'if':
        _collect_cond_var_ids(s[1], used)
        _collect_cond_var_ids(s[4], used)
    elif tag == 'from':
        _collect_cond_var_ids(s[1], used)
        _collect_cond_var_ids(s[3], used)
    return used


def _check_ids_declared(s, proc_name, scope):
    """Ogni identificatore usato da `s` dev'essere nello store: parametro,
    variabile dichiarata o local aperto."""
    for name in sorted(_stmt_own_ids(s)):
        if name not in scope:
            raise KairosCompileError(
                "STATIC", f"riga {s[-1]}: variabile '{name}' in {proc_name} non dichiarata")


def _check_local_blocks(stmts, proc_name, in_scope=()):
    """Come in Janus, `local x … delocal x` e' un blocco: ogni delocal chiude
    il local aperto piu' di recente nella stessa lista di istruzioni, e a fine
    lista non resta nessun local aperto. Un local non puo' ridichiarare un nome
    gia' presente nello store dell'attivazione (parametro o local ancora
    aperto): e' Local-Err."""
    open_ = []
    for s in stmts or []:
        if not isinstance(s, tuple) or not s:
            continue
        tag = s[0]
        _check_ids_declared(s, proc_name, tuple(in_scope) + tuple(o[0] for o in open_))
        if tag == 'decl':
            in_scope = tuple(in_scope) + (s[2],)
        elif tag == 'local':
            if s[2] in in_scope or any(o[0] == s[2] for o in open_):
                raise KairosCompileError(
                    "LOCAL",
                    f"riga {s[-1]}: local '{s[2]}' in {proc_name}: il nome e' gia' in uso "
                    f"(parametro o local aperto) nella stessa attivazione (Local-Err)")
            open_.append((s[2], s[-1], s[1]))
        elif tag == 'delocal':
            if open_ and open_[-1][0] == s[2] and open_[-1][2] != s[1]:
                raise KairosCompileError(
                    "LOCAL",
                    f"riga {s[-1]}: delocal {s[1]} '{s[2]}' in {proc_name} chiude un "
                    f"local {open_[-1][2]}")
            if not open_ or open_[-1][0] != s[2]:
                atteso = f"'{open_[-1][0]}'" if open_ else "nessuno"
                raise KairosCompileError(
                    "LOCAL",
                    f"riga {s[-1]}: delocal '{s[2]}' in {proc_name} non chiude il local "
                    f"aperto più di recente nello stesso blocco (atteso: {atteso})")
            open_.pop()
        else:
            inner = tuple(in_scope) + tuple(o[0] for o in open_)
            if tag == 'if':
                _check_local_blocks(s[2], proc_name, inner)
                _check_local_blocks(s[3], proc_name, inner)
            elif tag == 'from':
                _check_local_blocks(s[2], proc_name, inner)
                if len(s) >= 7:
                    _check_local_blocks(s[6], proc_name, inner)
            elif tag == 'par':
                for b in s[1]:
                    _check_local_blocks(b, proc_name, inner)
    if open_:
        name, ln, _t = open_[-1]
        raise KairosCompileError(
            "LOCAL", f"riga {ln}: local '{name}' in {proc_name} senza delocal nello stesso blocco")


def _check_callees_defined(stmts, procs):
    """Ogni `call`/`uncall` deve nominare una procedura del programma."""
    for s in stmts or []:
        if not isinstance(s, tuple) or not s:
            continue
        tag = s[0]
        if tag in ('call', 'uncall') and s[1] not in procs:
            raise KairosCompileError(
                "STATIC", f"riga {s[-1]}: {tag} di '{s[1]}', procedura non definita")
        if (tag in ('call', 'uncall') or (tag == 'call_direct' and s[1] in procs)) \
                and len(s[2]) != len(procs[s[1]]):
            raise KairosCompileError(
                "STATIC", f"riga {s[-1]}: {s[1]} vuole {len(procs[s[1]])} argomenti, "
                f"ne riceve {len(s[2])}")
        if tag in ('call', 'uncall') or (tag == 'call_direct' and s[1] in procs):
            # Come in Janus gli argomenti sono passati per riferimento e devono
            # essere distinti: con f(x, x) il corpo vedrebbe `a += b` diventare
            # `x += x`, che non e' invertibile.
            seen = set()
            for a in s[2]:
                if isinstance(a, str) and a in seen:
                    raise KairosCompileError(
                        "STATIC", f"riga {s[-1]}: '{a}' passato due volte a '{s[1]}': "
                        "gli argomenti di una chiamata devono essere distinti")
                seen.add(a)
        if tag == 'if':
            _check_callees_defined(s[2], procs)
            _check_callees_defined(s[3], procs)
        elif tag == 'from':
            _check_callees_defined(s[2], procs)
            if len(s) >= 7:
                _check_callees_defined(s[6], procs)
        elif tag == 'par':
            for b in s[1]:
                _check_callees_defined(b, procs)


def _kind(t):
    """Categoria di un tipo dichiarato: int, array, stack o channel."""
    if t is None:
        return None
    return 'array' if t.startswith('array') else t


def _check_expr_types(e, declared, ln):
    """In un'espressione una variabile e' un intero; un array si legge solo
    cella per cella."""
    if isinstance(e, tuple) and e and e[0] == 'binop':
        _check_expr_types(e[2], declared, ln)
        _check_expr_types(e[3], declared, ln)
    elif isinstance(e, tuple) and e and e[0] == 'index':
        if _kind(declared.get(e[1])) != 'array':
            raise KairosCompileError("STATIC", f"riga {ln}: '{e[1]}' non e' un array")
        _check_expr_types(e[2], declared, ln)
    elif isinstance(e, str) and not _is_number_literal(e):
        if e.lower() in ('nil', 'empty'):
            raise KairosCompileError(
                "STATIC", f"riga {ln}: '{e}' e' il valore di uno stack o di un canale, "
                "qui serve un intero")
        k = _kind(declared.get(e))
        if k not in (None, 'int'):
            raise KairosCompileError(
                "STATIC", f"riga {ln}: '{e}' e' {k}, in un'espressione serve un intero")


def _check_types(stmts, env, proc_name, signatures):
    """Tipi degli operandi, con gli scope veri (un nome puo' essere int in un
    blocco e stack in un altro): espressioni intere, argomenti di una call
    dello stesso tipo dei parametri, push/pop fra un intero e uno stack,
    ssend/srecv su un canale con payload distinti, valore di local/delocal
    del tipo della variabile; le dichiarazioni senza local valgono solo in
    main. `env` mappa i nomi in scope al loro tipo."""
    env = dict(env)
    for x in stmts or []:
        if not isinstance(x, tuple) or not x:
            continue
        tag, ln = x[0], x[-1]
        if tag == 'decl':
            if proc_name != 'main':
                raise KairosCompileError(
                    "STATIC", f"riga {ln}: '{x[1]} {x[2]}' in {proc_name}: fuori da main le "
                    "variabili si introducono con local/delocal")
            env[x[2]] = x[1]
        elif tag == 'assign':
            _, lv, op, e, _ = x
            for side in [lv] + ([e] if op == '<=>' else []):
                if isinstance(side, str) and _kind(env.get(side)) not in (None, 'int'):
                    raise KairosCompileError(
                        "STATIC", f"riga {ln}: '{side}' non e' un intero")
                _check_expr_types(side, env, ln)
            if op != '<=>':
                _check_expr_types(e, env, ln)
        elif tag in ('local', 'delocal'):
            k, val = _kind(x[1]), x[3]
            if k in ('int', 'array'):
                _check_expr_types(val, env, ln)
            elif k == 'stack' and val != 'nil':
                raise KairosCompileError(
                    "STATIC", f"riga {ln}: {tag} stack {x[2]}: il valore e' nil (stack vuoto)")
            elif k == 'channel' and val != 'empty':
                raise KairosCompileError(
                    "STATIC", f"riga {ln}: {tag} channel {x[2]}: il valore e' empty")
            if tag == 'local':
                env[x[2]] = x[1]
            else:
                env.pop(x[2], None)
        elif tag in ('call', 'uncall'):
            params = signatures.get(x[1], [])
            for a, pt in zip(x[2], params):
                ka, kp = _kind(env.get(a)), _kind(pt)
                if ka is not None and ka != kp:
                    raise KairosCompileError(
                        "STATIC", f"riga {ln}: {x[1]}: '{a}' e' {ka}, il parametro e' {kp}")
        elif tag == 'call_direct':
            name, args = x[1].lower(), x[2]
            if name in ('push', 'pop'):
                if len(args) != 2:
                    raise KairosCompileError(
                        "STATIC", f"riga {ln}: {name} vuole due argomenti, una variabile e uno stack")
                if _kind(env.get(args[0])) != 'int' or _kind(env.get(args[1])) != 'stack':
                    raise KairosCompileError(
                        "STATIC", f"riga {ln}: {name}({args[0]}, {args[1]}): il primo argomento "
                        "e' una variabile intera, il secondo uno stack")
            elif name in ('ssend', 'srecv'):
                payload, ch = args[:-1], args[-1] if args else None
                if _kind(env.get(ch)) != 'channel':
                    raise KairosCompileError(
                        "STATIC", f"riga {ln}: {name}: '{ch}' non e' un canale")
                if not payload:
                    raise KairosCompileError(
                        "STATIC", f"riga {ln}: {name}: il messaggio e' vuoto")
                if len(set(payload)) != len(payload) or ch in payload:
                    raise KairosCompileError(
                        "STATIC", f"riga {ln}: {name}: le variabili del messaggio devono essere "
                        "distinte fra loro e dal canale")
                for v in payload:
                    if _kind(env.get(v)) not in ('int', 'stack', 'channel'):
                        raise KairosCompileError(
                            "STATIC", f"riga {ln}: {name}: '{v}' non si puo' spedire "
                            "(un messaggio porta interi, stack o canali)")
        elif tag == 'if':
            for c in (x[1], x[4]):
                _check_expr_types(c[2], env, ln)
                _check_expr_types(c[3], env, ln)
            _check_types(x[2], env, proc_name, signatures)
            _check_types(x[3], env, proc_name, signatures)
        elif tag == 'from':
            for c in (x[1], x[3]):
                _check_expr_types(c[2], env, ln)
                _check_expr_types(c[3], env, ln)
            _check_types(x[2], env, proc_name, signatures)
            _check_types(from_second_body(x), env, proc_name, signatures)
        elif tag == 'par':
            for br in x[1]:
                _check_types(br, env, proc_name, signatures)


def _run_static_checks_body(program_ast):
    procedures = program_ast[1] if len(program_ast) > 1 else []
    proc_signatures = {}
    for proc in procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        proc_name = proc[1]
        params = proc[2] or []
        if proc_name in proc_signatures:
            raise KairosCompileError(
                "STATIC", f"procedura '{proc_name}' definita due volte")
        if proc_name == 'main' and params:
            raise KairosCompileError(
                "STATIC", "main non ha parametri: le sue variabili si dichiarano nel corpo")
        proc_signatures[proc_name] = [ptype for ptype, _pname in params]

    proc_param_lists, proc_int_mutated_formals = _compute_proc_int_mutated_formals(procedures)

    for proc in procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        params_env = {pn: pt for pt, pn in (proc[2] or [])}
        body = proc[3] or []
        _check_local_blocks(body, proc[1], tuple(pn for _pt, pn in (proc[2] or [])))
        _check_callees_defined(body, proc_signatures)
        _check_types(body, params_env, proc[1], proc_signatures)
        _check_list_reversibility(
            body, params_env, proc_signatures, proc_param_lists, proc_int_mutated_formals
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
# procedura sintetica muta le celle del chiamante in-place. Il risultato è un
# `if` qualunque: la condizione `yrt` è la sua asserzione d'uscita e, come in
# Janus, dev'essere vera dopo il commit e falsa dopo il rollback.

_FORBIDDEN_IN_TRY = ('par',)  # v1: niente par/canali dentro il body del try


def _proc_contains_ssend_srecv(name, proc_bodies, cache, visiting):
    """par, ssend o srecv raggiungibili dal corpo della procedura `name`, anche
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
        if tag == 'call_direct' and s[1].lower() in ('ssend', 'srecv'):
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
            return True   # anche un par raggiunto tramite call e' vietato nel try
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
            if isinstance(s[1], tuple):
                _collect_ids_in_expr(s[1], used)
            else:
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
        if s[0] in _FORBIDDEN_IN_TRY or s[0] == 'call_direct' and s[1].lower() in ('ssend', 'srecv'):
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
                f"'{s[1]}' contiene par o ssend/srecv, anche indirettamente — un try/rollback non può "
                "disfare un rendez-vous già consumato dal partner senza la sua cooperazione",
            )
        if s[0] == 'if':
            _try_assert_no_forbidden(s[2], lineno, proc_bodies, ssend_srecv_cache)
            _try_assert_no_forbidden(s[3], lineno, proc_bodies, ssend_srecv_cache)
        elif s[0] == 'from':
            _try_assert_no_forbidden(s[2], lineno, proc_bodies, ssend_srecv_cache)
            _try_assert_no_forbidden(from_second_body(s), lineno, proc_bodies, ssend_srecv_cache)


def _desugar_stmts(stmts, new_procs, counter, proc_bodies, ssend_srecv_cache):
    # I tipi in scope seguono la sequenza: un local vale fino al suo delocal,
    # e una sottosequenza (ramo, corpo) non cambia lo scope di chi la contiene.
    global _try_declared
    saved = _try_declared
    _try_declared = dict(saved)
    out = []
    for s in stmts:
        out.extend(_desugar_one(s, new_procs, counter, proc_bodies, ssend_srecv_cache))
        if isinstance(s, tuple) and s and s[0] in ('local', 'decl'):
            _try_declared[s[2]] = s[1]
        elif isinstance(s, tuple) and s and s[0] == 'delocal':
            _try_declared.pop(s[2], None)
    _try_declared = saved
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
                          [(_try_param_type(v), v) for v in fv_body], body, ln))
        else_body = [('uncall', try_name, list(fv_body), ln)]
        if rb_body is not None:
            rb_body = _desugar_stmts(rb_body, new_procs, counter, proc_bodies, ssend_srecv_cache)
            _try_assert_no_forbidden(rb_body, ln, proc_bodies, ssend_srecv_cache)
            rb_name = f"__try_rb_{uid}"
            fv_rb = sorted(_try_free_vars(rb_body))
            new_procs.append(('procedure', rb_name,
                              [(_try_param_type(v), v) for v in fv_rb], rb_body, ln))
            else_body.append(('call', rb_name, list(fv_rb), ln))
        # entry_cond decide il commit (valutata dopo il body); exit_cond è
        # l'asserzione del `fi`: vera dopo il commit, falsa dopo il rollback.
        return [
            ('call', try_name, list(fv_body), ln),
            ('if', entry_cond, [], else_body, exit_cond, ln),
        ]
    return [s]


# Tipi delle variabili della procedura che contiene il try in corso di
# riscrittura: i parametri delle procedure sintetiche hanno il tipo vero
# delle variabili libere (uno stack resta uno stack per i controlli sul par).
_try_declared = {}


def _try_param_type(v):
    t = _try_declared.get(v, 'int')
    return 'array' if t.startswith('array') else t


def _normalize_calls(procedures):
    """`f(x)` verso una procedura del programma diventa `call f(x)`, cosi' ogni
    controllo su call vale anche per la forma abbreviata; un nome che non e'
    ne' procedura ne' builtin e' un errore. I nomi delle builtin e i suffissi
    usati dal compilatore (`__inv`, `__try_`) non sono nomi di procedura."""
    names = set()
    for proc in procedures:
        if isinstance(proc, tuple) and proc and proc[0] == 'procedure':
            n = proc[1]
            if n.lower() in _BUILTIN_CALL_OPCODES or n.endswith('__inv') or n.startswith('__try_'):
                raise KairosCompileError(
                    "STATIC", f"'{n}' non e' un nome di procedura ammesso "
                    "(builtin o nome riservato al compilatore)")
            names.add(n)

    def stmts(lst):
        return [stmt(x) for x in lst] if lst is not None else lst

    def stmt(x):
        if not isinstance(x, tuple) or not x:
            return x
        tag = x[0]
        if tag == 'call_direct':
            if x[1] in names:
                return ('call', x[1], x[2], x[3])
            if x[1].lower() not in _BUILTIN_CALL_OPCODES:
                raise KairosCompileError(
                    "STATIC", f"riga {x[3]}: '{x[1]}' non e' una procedura del programma "
                    "ne' una builtin")
            return x
        if tag == 'if':
            return ('if', x[1], stmts(x[2]), stmts(x[3]), x[4], x[5])
        if tag == 'from':
            y = list(x)
            y[2] = stmts(x[2])
            if len(x) >= 7:
                y[6] = stmts(x[6])
            return tuple(y)
        if tag == 'par':
            return ('par', [stmts(b) for b in x[1]], x[2])
        if tag == 'try':
            return ('try', x[1], stmts(x[2]), stmts(x[3]), x[4], x[5])
        return x

    return [('procedure', p[1], p[2], stmts(p[3]), *p[4:])
            if isinstance(p, tuple) and p and p[0] == 'procedure' else p
            for p in procedures]


def desugar_try(program_ast):
    """Riscrive tutti i nodi `try` del programma in call/uncall/if-fi.

    Ritorna un nuovo AST `('program', procedure_list)` con le procedure
    sintetiche del body/rollback appese in coda. Idempotente sui programmi
    senza `try`.
    """
    if not program_ast or program_ast[0] != 'program':
        return program_ast
    procedures = _normalize_calls(program_ast[1] if len(program_ast) > 1 else [])
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
        # Le procedure sintetiche del try si emettono accanto alla procedura
        # che le usa (l'ordine nel file non conta: main parte dopo la prima
        # passata).
        new_procs = []
        global _try_declared
        _try_declared = {pn: pt for pt, pn in (params or [])}
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