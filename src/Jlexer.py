"""
LEXER basilare per janus concorrenziale
Uso: python Jlexer.py <file>
"""
import sys
import ply.lex as lex

# parole riservate del linguaggio
reserved = {
    'procedure': 'PROCEDURE',
    'int'      : 'INT',   'stack'   : 'STACK', 'nil'   : 'NIL',
    'channel'  : 'CHANNEL', 'empty' : 'EMPT',
    'local'    : 'LOCAL', 'delocal' : 'DELOCAL',
    'call'     : 'CALL',  'uncall'  : 'UNCALL',
    'if'       : 'IF',    'then'    : 'THEN',  'else'  : 'ELSE', 'fi': 'FI',
    'from'     : 'FROM',  'loop'    : 'LOOP',  'until' : 'UNTIL',
    'par'      : 'PAR',   'and'     : 'AND',   'rap'   : 'RAP',
}

tokens = (
    'PROCEDURE',
    'INT', 'STACK', 'CHANNEL',
    'ID',
    'NUMBER', 'NIL', 'EMPT',
    'EQEQ',                                # == confronto nelle condition
    'EQUALS',                              # =  assegnamento in local/delocal
    'PLUSEQUALS', 'MINUSEQUALS', 'SWAP',   # operatori composti
    'PLUS', 'MINUS',                       # operatori semplici
    'LPAREN', 'RPAREN',
    'LOCAL', 'DELOCAL',
    'CALL', 'UNCALL',
    'IF', 'THEN', 'ELSE', 'FI',
    'FROM', 'LOOP', 'UNTIL',
    'PAR', 'AND', 'RAP',
    'COMMA',
)

# identificatori e parole riservate
def t_ID(t):
    r'[a-zA-Z_][a-zA-Z0-9_]*'
    t.type = reserved.get(t.value, 'ID')
    return t

def t_NUMBER(t):
    r'\d+'
    t.value = int(t.value)
    return t

# operatori composti PRIMA di quelli semplici
t_EQEQ        = r'=='       # deve stare PRIMA di t_EQUALS
t_PLUSEQUALS  = r'[+]='
t_MINUSEQUALS = r'[-]='
t_SWAP        = r'<=>'

# operatori semplici
t_EQUALS = r'='
t_PLUS   = r'[+]'
t_MINUS  = r'[-]'

# parentesi e virgole
t_LPAREN = r'[(]'
t_RPAREN = r'[)]'
t_COMMA  = r'[,]'

# ignora commenti
t_ignore_COMMENT = r'//.*'

# conta le righe
def t_newline(t):
    r'\n+'
    t.lexer.lineno += len(t.value)

# ignora spazi e tab
t_ignore = ' \t'

def t_error(t):
    print(f"[LEXER] riga {t.lexer.lineno}: carattere non riconosciuto '{t.value[0]}'")
    t.lexer.skip(1)
    exit(1)

lexer = lex.lex()

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python Jlexer.py <file>")
        sys.exit(1)
    with open(sys.argv[1], 'r') as f:
        source = f.read()
    lexer.input(source)
    for tok in lexer:
        print(tok.type, tok.value)