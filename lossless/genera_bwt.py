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


def genera(n, nb, modo, libreria):
    L = [libreria]
    R = range(nb)

    # Sorgente di ogni blocco: l'identita' sugli offset, riempita e disfatta da
    # una sola procedura (call in un verso, uncall nell'altro).
    L.append('procedure identita(int a[], int n)')
    L.append('    local int i = 0')
    L.append('    from i == 0 do')
    L.append('        a[i] += i')
    L.append('        i += 1')
    L.append('    loop until i == n')
    L.append('    delocal int i = n')
    L.append('')

    # lavoratore: trasforma il blocco e consegna (indice, colonna) per canale.
    # Il canale trasporta stack, non array: la colonna viene copiata in uno stack.
    L.append('procedure lavoratore(int blocco[], int offs[], int out[], stack scarto,')
    L.append('                     stack colonna, channel c, int n)')
    L.append('    local int idx = 0')
    L.append('        call ordina(offs, blocco, scarto, n)')
    L.append('        call estrai(offs, blocco, out, n, idx)')
    L.append('        local int i = 0')
    L.append('        from i == 0 do')
    L.append('            local int t = 0')
    L.append('                t += out[i]')
    L.append('                push(t, colonna)')
    L.append('            delocal int t = 0')
    L.append('            i += 1')
    L.append('        loop until i == n')
    L.append('        delocal int i = n')
    L.append('        ssend(<idx, colonna>, c)')
    L.append('    delocal int idx = 0')
    L.append('')

    # raccoglitore: riceve da tutti i canali
    ch = ', '.join(f'channel c{i}' for i in R)
    co = ', '.join(f'stack col{i}' for i in R)
    L.append(f'procedure raccoglitore({ch}, {co}, stack indici)')
    for i in R:
        L.append(f'    local int i{i} = 0')
    for i in R:
        L.append(f'        srecv(<i{i}, col{i}>, c{i})')
    for i in R:
        L.append(f'        push(i{i}, indici)')
    for i in reversed(R):
        L.append(f'    delocal int i{i} = 0')
    L.append('')

    # comprimi
    par = ', '.join(
        [f'int b{i}[]' for i in R] + [f'int offs{i}[]' for i in R] +
        [f'int out{i}[]' for i in R] + [f'stack sc{i}' for i in R] +
        [f'stack st{i}' for i in R] + [f'stack col{i}' for i in R] +
        ['stack indici', 'int n'])
    L.append(f'procedure comprimi({par})')
    for i in R:
        L.append(f'    local channel c{i} = empty')
    if modo == 'par':
        L.append('    par')
        rami = [f'        call lavoratore(b{i}, offs{i}, out{i}, sc{i}, st{i}, c{i}, n)'
                for i in R]
        cargs = ', '.join(f'c{i}' for i in R)
        colargs = ', '.join(f'col{i}' for i in R)
        rami.append(f'        call raccoglitore({cargs}, {colargs}, indici)')
        L.append('\n    and\n'.join(rami))
        L.append('    rap')
    else:
        # sequenziale: ogni blocco viene trasformato e consegnato, uno per volta.
        # Il rendez-vous vuole comunque due rami, quindi il par c'e' ma contiene
        # un solo lavoratore per volta: nessun parallelismo fra blocchi.
        for i in R:
            L.append('    par')
            L.append(f'        call lavoratore(b{i}, offs{i}, out{i}, sc{i}, st{i}, c{i}, n)')
            L.append('    and')
            L.append(f'        local int i{i} = 0')
            L.append(f'            srecv(<i{i}, col{i}>, c{i})')
            L.append(f'            push(i{i}, indici)')
            L.append(f'        delocal int i{i} = 0')
            L.append('    rap')
    for i in reversed(R):
        L.append(f'    delocal channel c{i} = empty')
    L.append('')

    # main
    L.append('procedure main()')
    for p in ('sc', 'st', 'col'):
        L.append('    ' + '  '.join(f'stack {p}{i}' for i in R))
    L.append('    stack indici')
    L.append(f'    local int n = {n}')
    arrays = [f'{p}{i}' for p in ('b', 'offs', 'out') for i in R]
    for a in arrays:
        L.append(f'    local int {a}[{n}] = 0')
    for i in R:
        L.append(f'    call identita(offs{i}, n)')
        for k, v in enumerate(blocco(i, n)):
            L.append(f'    b{i}[{k}] += {v}')
    args = ', '.join(
        [f'b{i}' for i in R] + [f'offs{i}' for i in R] + [f'out{i}' for i in R] +
        [f'sc{i}' for i in R] + [f'st{i}' for i in R] + [f'col{i}' for i in R] +
        ['indici', 'n'])
    L.append(f'    call comprimi({args})')
    for i in R:
        L.append(f'    show(col{i})')
    L.append('    show(indici)')
    L.append(f'    uncall comprimi({args})')
    for i in R:
        for k, v in reversed(list(enumerate(blocco(i, n)))):
            L.append(f'    b{i}[{k}] -= {v}')
        L.append(f'    uncall identita(offs{i}, n)')
    for a in reversed(arrays):
        L.append(f'    delocal int {a}[{n}] = 0')
    L.append(f'    delocal int n = {n}')
    return '\n'.join(L) + '\n'


if __name__ == '__main__':
    n, nb, modo = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    src = open('lossless/bwt.kairos', encoding='utf-8').read()
    lib = src[:src.index('procedure main()')]
    sys.stdout.write(genera(n, nb, modo, lib))
