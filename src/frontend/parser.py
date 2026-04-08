import sys
import ply.yacc as yacc
from .lexer import lexer, tokens
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
    p[0] = ('decl', p[1], p[2], p.lineno(1))
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
    p[0] = ('from', p[2], p[4], p[6], p.lineno(1))
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
        print(f"[PARSER] riga {p.lineno}: token non atteso '{p.value}'")
    else:
        print("[PARSER] errore sintattico: fine file inattesa")


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