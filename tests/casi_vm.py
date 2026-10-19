#!/usr/bin/env python3
"""Casi di bytecode scritti a mano per gli errori a tempo d'esecuzione della VM.

Ogni caso si esegue con due o piu' libvm e se ne confronta l'esito (codice
d'uscita o segnale) e l'uscita (stdout e stderr insieme, dump finale
compreso). Serve a controllare che una VM ottimizzata dia gli stessi errori,
con lo stesso testo e nello stesso ordine, della VM di riferimento, anche
dove il frontend non arriva: errori che si vedono solo eseguendo
(Deloc-Err, Pop-Err, Idx-Err, divisione per zero, ...), righe malformate,
etichette inesistenti, uncall che falliscono durante l'inversione.

Uso:
    python tests/casi_vm.py LIB_RIFERIMENTO.so LIB_NUOVA.so [ALTRE.so ...]

Con una libvm compilata con -DVM_DEC_SUBITO le righe si decodificano gia'
alla prima esecuzione: e' il modo di provare la via delle righe decodificate
(src/vm/vm_dec.h) anche su questi programmi, che eseguono ogni riga una volta.
Esce con 1 se almeno un caso e' diverso.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from regressione_vm import esegui, _alza_stack      # noqa: E402

CASI = {}


def caso(nome, corpo, procedure=''):
    righe = (['START'] + procedure.strip().splitlines() + ['PROC main']
             + corpo.strip().splitlines() + ['END_PROC main', 'HALT'])
    CASI[nome] = ''.join('@1      ' + r.strip() + '\n' for r in righe if r.strip())


caso('deloc_val', 'LOCAL int x 5\nPUSHEQ x 1\nDELOCAL int x 5')
caso('deloc_tipo', 'LOCAL int x 0\nDELOCAL stack x nil')
caso('deloc_ordine', 'LOCAL int x 0\nLOCAL int y 0\nDELOCAL int x 0')
caso('pop_vuota', 'LOCAL stack s nil\nLOCAL int x 0\nPOP x s')
caso('pop_err2', 'LOCAL stack s nil\nLOCAL int x 4\nLOCAL int y 3\nPUSH y s\nPOP x s')
caso('push_non_stack', 'LOCAL int s 0\nLOCAL int x 1\nPUSH x s')
caso('push_numero', 'LOCAL stack s nil\nPUSH 7 s\nSHOW s\nLOCAL int x 0\nPOP x s\nSHOW x')
caso('local_doppio', 'LOCAL int x 0\nLOCAL int x 0')
caso('assert_fuori_if', 'LOCAL int x 0\nASSERT x == 1')
caso('mineq_stesso', 'LOCAL int x 3\nMINEQ x x')
caso('xoreq_stesso', 'LOCAL int x 3\nXOREQ x x')
caso('idx_err', 'LOCAL array a 0 4\nLOCAL int i 7\nPUSHEQ a[i] 1')
caso('array_da_var', 'LOCAL int v 3\nLOCAL array a v 4\nSHOW a\nDELOCAL array a v 4\nSHOW v')
caso('array_lunghezza', 'LOCAL array a 0 4\nDELOCAL array a 0 5')
caso('array_valori', 'LOCAL array a 0 4\nPUSHEQ a[2] 1\nDELOCAL array a 0 4')
caso('stack_copia', 'LOCAL stack s nil\nLOCAL int x 5\nPUSH x s\nLOCAL stack t s\nSHOW t\nSHOW s')
caso('stack_non_nil', 'LOCAL stack s 3')
caso('eval_op_ignoto', 'LOCAL int x 0\nEVAL x <> 1')
caso('var_indefinita', 'LOCAL int x 0\nPUSHEQ x y')
caso('fuori_blocco', 'LOCAL int z 0\nLOCAL int x 0\nDELOCAL int x 0\nPUSHEQ z x')
caso('div_zero', 'LOCAL int x 0\nLOCAL int y 0\nPUSHEQ x (1 / y)')
caso('mod_meno_uno', 'LOCAL int x 0\nLOCAL int y -1\nPUSHEQ x (7 % y)\nSHOW x')
caso('swap_celle', 'LOCAL array a 0 3\nPUSHEQ a[0] 5\nPUSHEQ a[2] 9\nSWAP a[0] a[2]\nSHOW a')
caso('swap_indefinita', 'LOCAL int x 0\nSWAP x q')
caso('cella_malformata', 'LOCAL int x 0\nPUSHEQ x a[1')
caso('non_array', 'LOCAL int a 0\nLOCAL int x 0\nPUSHEQ x a[0]')
caso('luogo_non_int', 'LOCAL stack s nil\nPUSHEQ s 1')
caso('jmp_ignota', 'JMP NIENTE_1')
caso('jmpf_ignota', 'LOCAL int x 0\nEVAL x == 1\nJMPF NIENTE_2')
caso('pop_troppi', 'LOCAL stack s nil\nLOCAL int x 0\nPOP x s y')
caso('push_troppi', 'LOCAL stack s nil\nLOCAL int x 0\nPUSH x s y')
caso('eval_malformata', 'LOCAL int x 0\nEVAL x ==')
caso('pusheq_vuota', 'LOCAL int x 0\nPUSHEQ x')
caso('tipo_ignoto', 'LOCAL foo x 0')
caso('espr_lunghe', 'LOCAL int x 2\nLOCAL int y 5\n'
     'PUSHEQ x (((x * y) + (y - x)) * ((x + 1) / (y - 3)))\nSHOW x\n'
     'PUSHEQ x ((x = 21) + (y # 5))\nSHOW x\nPUSHEQ x ((x { 3) | (y } 9))\nSHOW x')
caso('negativi', 'LOCAL int x -5\nPUSHEQ x -3\nSHOW x\nMINEQ x (0 - 9223372036854775807)\nSHOW x')
caso('grande', 'LOCAL int x 18446744073709551615\nSHOW x\nPUSHEQ x 1\nSHOW x')
caso('show_char', 'LOCAL int x 65\nSHOW x char\nSHOW x')
caso('canale', 'LOCAL channel c empty\nSHOW c\nDELOCAL channel c empty')
caso('canale_non_vuoto', 'LOCAL channel c empty\nDELOCAL channel c nil')

P_DELOC = """PROC p
PARAM int a
LOCAL int t 0
PUSHEQ t a
PUSHEQ a t
MINEQ t a
PUSHEQ t (a / 2)
MINEQ t (a / 2)
DELOCAL int t 0
END_PROC p"""
caso('uncall_semplice', 'LOCAL int a 6\nUNCALL p a\nSHOW a\nCALL p a\nSHOW a', P_DELOC)

P_ERR = """PROC q
PARAM int a
LOCAL int t 0
PUSHEQ t 1
DELOCAL int t 1
PUSHEQ a 1
END_PROC q"""
caso('uncall_errore', 'LOCAL int a 0\nUNCALL q a\nSHOW a\nLOCAL int b 3\nUNCALL q b\nUNCALL q b\nSHOW b', P_ERR)

P_STACK = """PROC r
PARAM stack s
PARAM int a
PUSH a s
LOCAL int z 0
POP z s
PUSH z s
DELOCAL int z 0
END_PROC r"""
caso('uncall_stack', 'LOCAL stack s nil\nLOCAL int a 4\nCALL r s a\nSHOW s\nSHOW a\n'
     'UNCALL r s a\nSHOW s\nSHOW a\nUNCALL r s a', P_STACK)

P_SWAP = """PROC w
PARAM array v
SWAP v[0] v[1]
XOREQ v[2] v[0]
PUSHEQ v[1] (v[0] * 3)
END_PROC w"""
caso('uncall_swap', 'LOCAL array v 0 3\nPUSHEQ v[0] 2\nPUSHEQ v[1] 7\nCALL w v\nSHOW v\n'
     'UNCALL w v\nSHOW v\nUNCALL w v\nUNCALL w v\nSHOW v', P_SWAP)


def main(libs):
    if len(libs) < 2:
        print(__doc__.split('\n\n')[2], file=sys.stderr)
        return 2
    libs = [os.path.abspath(l) for l in libs]
    _alza_stack()
    diversi = 0
    for nome, testo in sorted(CASI.items()):
        esiti = [esegui(testo.encode(), lib) for lib in libs]
        uguali = all((e.stato, e.uscita) == (esiti[0].stato, esiti[0].uscita) for e in esiti)
        righe = esiti[0].uscita.decode(errors='replace').strip().splitlines()
        ultima = next((r for r in reversed(righe) if not r.startswith('===')), '')
        print(f"{'OK  ' if uguali else 'DIFF'} {nome:18s} {esiti[0].stato:24s} {ultima[:70]}")
        if not uguali:
            diversi += 1
            for lib, e in zip(libs, esiti):
                print(f'     {os.path.basename(lib)}: {e.stato}: '
                      + e.uscita.decode(errors='replace')[-300:].replace('\n', ' | '))
    print(f'{len(CASI) - diversi} uguali, {diversi} diversi')
    return 1 if diversi else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
