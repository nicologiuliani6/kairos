"""
LEXER basilare per janus concorrenziale
Uso: python lexers.py <file>
"""
import sys
import ply.lex as lex
#definiamo le parole riservate del linguaggio
reserved = {
    'procedure': 'PROCEDURE',
    'int' : 'INT', 'stack' : 'STACK', 'nil' : 'NIL',
    'channel' : 'CHANNEL', 'empty' : 'EMPT',
    'local' : 'LOCAL', 'delocal' : 'DELOCAL',
    'call' : 'CALL', 'uncall' : 'UNCALL',
    'if' : 'IF', 'then' : 'THEN', 'else' : 'ELSE', 'fi' : 'FI',
    'from' : 'FROM', 'loop' : 'LOOP', 'until' : 'UNTIL',
    'par' : 'PAR', 'and' : 'AND', 'rap' : 'RAP'
    }
#definiamo i token del lexer
tokens = (
        'PROCEDURE',
        'INT', 'STACK', 'CHANNEL',
        'ID',
        'NUMBER', 'NIL', 'EMPT',
        'EQUALS',
        'PLUSEQUALS', 'MINUSEQUALS', 'SWAP', # operatori composti
        'PLUS', 'MINUS',                                     # operatori semplici
        'LPAREN', 'RPAREN',
        'LOCAL', 'DELOCAL',
        'CALL', 'UNCALL',
        'IF', 'THEN', 'ELSE', 'FI',
        'FROM', 'LOOP', 'UNTIL',
        'PAR', 'AND', 'RAP',
        'COMMA'
        )
#regex sintassi per identificare i token riservati o ID
def t_ID(t):
    r'[a-zA-Z_][a-zA-Z0-9_]*'
    t.type = reserved.get(t.value, 'ID')
    return t
#definiamo sintassi di un numero
def t_NUMBER(t):
    r'\d+'
    t.value = int(t.value)
    return t
# operatori composti PRIMA di quelli semplici!
t_PLUSEQUALS  = r'[+]='
t_MINUSEQUALS = r'[-]='
t_SWAP = r'<=>'
# operatori semplici
t_EQUALS = r'='
t_PLUS   = r'[+]'
t_MINUS  = r'[-]'
#parentesi e virgole
t_LPAREN = r'[(]'
t_RPAREN = r'[)]'
t_COMMA = r'[,]'
#ignoriamo i comenti 
t_ignore_COMMENT = r'//.*'
#ignoriamo il ritorno a capo
def t_newline(t):
    r'\n+'
#ignoriamo il fine file
t_ignore = ' \t'

#gestiamo gli errori
def t_error(t):
    print(f"[LEXER] riga {t.lexer.lineno}: carattere non riconosciuto '{t.value[0]}'")
    t.lexer.skip(1)
    exit(1)

lexer = lex.lex()
if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Uso: python lexers.py <file>")
        sys.exit(1)
    
    with open(sys.argv[1], 'r') as f:
        source = f.read()

    lexer.input(source)
    for tok in lexer:
        #print(tok)
        print(tok.type, tok.value)
