import sys
import ply.yacc as yacc
from .lexer import lexer, tokens
from .errors import KairosCompileError
import logging
import tempfile

VERBOSE = False

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
                 | ID LPAREN arg_list RPAREN'''
    if len(p) == 4:
        p[0] = ('call_direct', p[1], [], p.lineno(1))
        if VERBOSE: print(f"call diretto: {p[1]}()")
    else:
        p[0] = ('call_direct', p[1], p[3], p.lineno(1))
        if VERBOSE: print(f"call diretto: {p[1]}({p[3]})")

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
    '''statement : FROM condition LOOP opt_body UNTIL condition'''
    # Salviamo sia la linea di FROM che quella di UNTIL per il mapping breakpoint.
    p[0] = ('from', p[2], p[4], p[6], p.lineno(1), p.lineno(5))
    if VERBOSE: print(f"from: {p[2]} until: {p[6]}")

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

def _check_stmt_reversibility(stmt):
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

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        for nested in then_body:
            _check_stmt_reversibility(nested)
        for nested in else_body:
            _check_stmt_reversibility(nested)
        return

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        for nested in body:
            _check_stmt_reversibility(nested)
        return

    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _check_stmt_reversibility(nested)

def run_static_checks(program_ast):
    if not isinstance(program_ast, tuple) or not program_ast or program_ast[0] != 'program':
        raise KairosCompileError("PARSER", "AST del programma non valido")

    procedures = program_ast[1] if len(program_ast) > 1 else []
    for proc in procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        body = proc[3] or []
        for stmt in body:
            _check_stmt_reversibility(stmt)


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