#!/usr/bin/env python3
"""Genera programmi Kairos per lo studio sulla BWT reversibile.

Produce, per un dato numero di blocchi e una data lunghezza, due varianti dello
stesso calcolo: una sequenziale e una con i blocchi come rami di un `par`. Il
corpo delle procedure e' identico, quindi il confronto dei tempi misura solo
l'effetto della concorrenza.

    python3 lossless/genera_bwt.py <n> <blocchi> <seq|par>  > file.kairos
"""
import sys

ALFABETO = {1: 'a', 2: 'b', 3: 'n'}


def blocco(i, n):
    """Blocco i-esimo: una sequenza periodica sull'alfabeto ridotto, scelta in
    modo che le rotazioni siano tutte distinte (serve al confronto: rotazioni
    uguali renderebbero l'ordinamento non deterministico)."""
    base = [2, 1, 3, 1, 3, 1]
    return [base[(k + i) % len(base)] for k in range(n)]


def spingi(nome, valori, ind='    '):
    out = []
    for v in valori:
        if v:
            out.append(f'{ind}t += {v}   push(t, {nome})')
        else:
            out.append(f'{ind}push(t, {nome})')
    return out


def genera(n, nb, modo, libreria):
    L = [libreria]
    L.append('procedure lavoratore(stack blocco, stack aux, stack offs, stack scarto,')
    L.append('                     stack out, channel c, int n)')
    L.append('    local int idx = 0')
    L.append('        call ordina(offs, blocco, aux, scarto, n)')
    L.append('        call estrai(offs, blocco, aux, scarto, out, n, idx)')
    L.append('        ssend(<idx, out>, c)')
    L.append('    delocal int idx = 0')
    L.append('')

    # raccoglitore: riceve da tutti i canali
    ch = ', '.join(f'channel c{i}' for i in range(nb))
    co = ', '.join(f'stack col{i}' for i in range(nb))
    L.append(f'procedure raccoglitore({ch}, {co}, stack indici)')
    for i in range(nb):
        L.append(f'    local int i{i} = 0')
    for i in range(nb):
        L.append(f'        srecv(<i{i}, col{i}>, c{i})')
    for i in range(nb):
        L.append(f'        push(i{i}, indici)')
    for i in reversed(range(nb)):
        L.append(f'    delocal int i{i} = 0')
    L.append('')

    # comprimi
    par = ', '.join(
        [f'stack b{i}' for i in range(nb)] + [f'stack aux{i}' for i in range(nb)] +
        [f'stack offs{i}' for i in range(nb)] + [f'stack sc{i}' for i in range(nb)] +
        [f'stack out{i}' for i in range(nb)] + [f'stack col{i}' for i in range(nb)] +
        ['stack indici', 'int n'])
    L.append(f'procedure comprimi({par})')
    for i in range(nb):
        L.append(f'    local channel c{i} = empty')
    if modo == 'par':
        L.append('    par')
        rami = [f'        call lavoratore(b{i}, aux{i}, offs{i}, sc{i}, out{i}, c{i}, n)'
                for i in range(nb)]
        cargs = ', '.join(f'c{i}' for i in range(nb))
        colargs = ', '.join(f'col{i}' for i in range(nb))
        rami.append(f'        call raccoglitore({cargs}, {colargs}, indici)')
        L.append('\n    and\n'.join(rami))
        L.append('    rap')
    else:
        # sequenziale: ogni blocco viene trasformato e consegnato, uno per volta.
        # Il rendez-vous vuole comunque due rami, quindi il par c'e' ma contiene
        # un solo lavoratore per volta: nessun parallelismo fra blocchi.
        for i in range(nb):
            L.append('    par')
            L.append(f'        call lavoratore(b{i}, aux{i}, offs{i}, sc{i}, out{i}, c{i}, n)')
            L.append('    and')
            L.append(f'        local int i{i} = 0')
            L.append(f'            srecv(<i{i}, col{i}>, c{i})')
            L.append(f'            push(i{i}, indici)')
            L.append(f'        delocal int i{i} = 0')
            L.append('    rap')
    for i in reversed(range(nb)):
        L.append(f'    delocal channel c{i} = empty')
    L.append('')

    # main
    L.append('procedure main()')
    for p in ('b', 'aux', 'offs', 'sc', 'out', 'col'):
        L.append('    ' + '  '.join(f'stack {p}{i}' for i in range(nb)))
    L.append('    stack indici')
    L.append(f'    local int n = {n}')
    L.append('    local int t = 0')
    for i in range(nb):
        L.extend(spingi(f'b{i}', reversed(blocco(i, n))))
    for i in range(nb):
        L.extend(spingi(f'offs{i}', list(range(n - 1, -1, -1))))
    L.append('    delocal int t = 0')
    args = ', '.join(
        [f'b{i}' for i in range(nb)] + [f'aux{i}' for i in range(nb)] +
        [f'offs{i}' for i in range(nb)] + [f'sc{i}' for i in range(nb)] +
        [f'out{i}' for i in range(nb)] + [f'col{i}' for i in range(nb)] +
        ['indici', 'n'])
    L.append(f'    call comprimi({args})')
    for i in range(nb):
        L.append(f'    show(col{i})')
    L.append('    show(indici)')
    L.append(f'    uncall comprimi({args})')
    for i in range(nb):
        L.append(f'    show(b{i})')
    L.append(f'    delocal int n = {n}')
    return '\n'.join(L) + '\n'


if __name__ == '__main__':
    n, nb, modo = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    src = open('lossless/bwt.kairos', encoding='utf-8').read()
    lib = src[:src.index('procedure main()')]
    sys.stdout.write(genera(n, nb, modo, lib))
