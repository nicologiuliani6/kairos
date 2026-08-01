#!/usr/bin/env python3
"""Esperimenti sul convertitore reversibile.

    python3 lossless/esperimenti.py costo        # call contro uncall a parita' di lavoro
    python3 lossless/esperimenti.py validazione  # cosa accetta l'inverso fuori dall'immagine
    python3 lossless/esperimenti.py peggiore     # dati incomprimibili
    python3 lossless/esperimenti.py tutti
"""

import pathlib
import random
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent
KAIROS = ROOT.parent
PY = KAIROS / "venv" / "bin" / "python"
SORGENTE = ROOT / "converti.kairos"
TMP = pathlib.Path("/tmp/kairos_esp")
TMP.mkdir(exist_ok=True)

sys.path.insert(0, str(ROOT))
from converti import blocco_dati, leggi_uscita  # noqa: E402


def scrivi(nome, righe_push, contatore, tipo):
    src = SORGENTE.read_text().replace(
        "    // DATI", blocco_dati(righe_push, contatore, tipo)
    )
    p = TMP / nome
    p.write_text(src)
    return p


def tempo_parse(path):
    """Secondi spesi dal frontend, per poterli sottrarre dal totale."""
    codice = (
        "import time,sys; sys.path.insert(0,'.');"
        "from src.frontend.parser import parser;"
        "from src.frontend.lexer import lexer;"
        f"s=open({str(path)!r}).read();"
        "t=time.time(); parser.parse(s, lexer=lexer); print(time.time()-t)"
    )
    r = subprocess.run(
        [str(PY), "-c", codice], cwd=KAIROS, capture_output=True, text=True
    )
    return float(r.stdout.strip().splitlines()[-1])


def esegui(path, limite=60):
    t = time.time()
    try:
        r = subprocess.run(
            [str(PY), "-m", "src.kairos", str(path)],
            cwd=KAIROS,
            capture_output=True,
            text=True,
            timeout=limite,
        )
    except subprocess.TimeoutExpired:
        return None, time.time() - t
    return r, time.time() - t


def push_grezzo(byte):
    return [f"        t += {b}   push(t, a)" for b in reversed(byte)]


def _carica(x, pila):
    """Kairos non ha letterali negativi: un valore < 0 si costruisce sottraendo."""
    seg = f"t += {x}" if x >= 0 else f"t -= {-x}"
    return f"        {seg}   push(t, {pila})"


def push_coppie(coppie):
    fuori = []
    for v, run in coppie:
        fuori.append(_carica(v, "b"))
        fuori.append(_carica(run, "b"))
    return fuori


def corse_di(byte):
    coppie = []
    for b in byte:
        if coppie and coppie[-1][0] == b:
            coppie[-1][1] += 1
        else:
            coppie.append([b, 1])
    return [tuple(c) for c in coppie]


def immagine(w):
    px = [
        30 if (x - (w - 1) / 2) ** 2 + (y - (w - 1) / 2) ** 2 < (w * 0.32) ** 2 else 220
        for y in range(w)
        for x in range(w)
    ]
    return b"P5\n%d %d\n255\n" % (w, w) + bytes(px)


# --- 1. costo dell'inverso ---------------------------------------------------


def esp_costo():
    """Sola conversione, isolata per differenza.

    I due programmi non fanno lo stesso lavoro: il diretto carica n `push` di
    dati, l'inverso solo 2k. Per confrontare `call` e `uncall` va tolto tutto
    il resto. Si esegue quindi ogni programma due volte, una com'e' e una con
    il contatore azzerato: con nsrc = 0 (o npair = 0) la conversione non parte,
    ma parse e caricamento restano identici. La differenza e' la conversione.
    """
    print("Costo della sola conversione, isolata per differenza.")
    print(f"{'byte':>7} {'corse':>6} {'call':>9} {'uncall':>9} {'rapporto':>9}")
    for w in (32, 64, 128):
        dati = immagine(w)
        coppie = corse_di(dati)
        gf, gi = push_grezzo(dati), push_coppie(coppie)
        ef = esegui(scrivi("cf.kairos", gf, f"na += {len(dati)}", 0))[1]
        ef0 = esegui(scrivi("cf0.kairos", gf, "na += 0", 0))[1]
        ei = esegui(scrivi("ci.kairos", gi, f"np += {len(coppie)}", 1))[1]
        ei0 = esegui(scrivi("ci0.kairos", gi, "np += 0", 1))[1]
        cf, ci = ef - ef0, ei - ei0
        print(
            f"{len(dati):7d} {len(coppie):6d} {cf:8.2f}s {ci:8.2f}s {ci / cf:8.2f}x"
        )


# --- 2. validazione ----------------------------------------------------------


def esp_validazione():
    """Ingressi in formato B che nessuna esecuzione diretta produrrebbe."""
    buone = [(7, 3), (3, 2), (9, 1)]
    casi = {
        "riferimento, immagine del diretto": (buone, len(buone)),
        "corsa di lunghezza 0": ([(7, 3), (3, 0), (9, 1)], 3),
        "due corse adiacenti uguali": ([(7, 3), (7, 2), (9, 1)], 3),
        "npair troppo piccolo": (buone, 2),
        "npair troppo grande": (buone, 4),
        "lunghezza negativa": ([(7, 3), (3, -2), (9, 1)], 3),
        "valore che collide con la sentinella": ([(7, 3), (-1, 2), (9, 1)], 3),
    }
    print("Validazione: cosa accetta l'inverso fuori dall'immagine del diretto.\n")
    for nome, (coppie, np) in casi.items():
        p = scrivi("val.kairos", push_coppie(coppie), f"np += {np}", 1)
        r, _ = esegui(p, limite=30)
        if r is None:
            print(f"  {nome:38s} NON TERMINA (30 s)")
            continue
        if r.returncode != 0:
            diag = (r.stdout + r.stderr).strip().splitlines()
            msg = next((l for l in diag if "[" in l), diag[-1] if diag else "?")
            esito = "RIFIUTA   " + msg[:70]
        else:
            fuori = leggi_uscita(r.stdout)
            pila = fuori["a"]
            byte = list(reversed(pila[1:])) if pila and pila[0] == -1 else pila
            riesce = corse_di(byte) == [tuple(c) for c in coppie]
            esito = f"accetta   -> {byte}"
            if not riesce:
                esito += "   (ricodificato NON torna all'ingresso)"
        print(f"  {nome:38s} {esito}")


# --- 3. caso peggiore --------------------------------------------------------


def esp_peggiore():
    print("Dati incomprimibili: ogni corsa lunga 1, il formato B raddoppia.\n")
    random.seed(7)
    for n in (200, 800):
        dati = bytes(random.randrange(256) for _ in range(n))
        coppie = corse_di(dati)
        pf = scrivi("peg_f.kairos", push_grezzo(dati), f"na += {n}", 0)
        r, tf = esegui(pf)
        fuori = leggi_uscita(r.stdout)
        piatto = fuori["b"]
        ottenute = [(piatto[i], piatto[i + 1]) for i in range(0, len(piatto), 2)]
        pi = scrivi("peg_i.kairos", push_coppie(ottenute), f"np += {len(ottenute)}", 1)
        r2, ti = esegui(pi)
        f2 = leggi_uscita(r2.stdout)
        indietro = bytes(reversed(f2["a"][1:]))
        print(
            f"  n={n:4d}  corse {len(ottenute):4d}  B/A = {2 * len(ottenute) / n:.2f}"
            f"  call {tf:.1f}s  uncall {ti:.1f}s  giro "
            + ("identico" if indietro == dati else "DIVERSO")
        )


if __name__ == "__main__":
    quale = sys.argv[1] if len(sys.argv) > 1 else "tutti"
    for nome, fn in (
        ("costo", esp_costo),
        ("validazione", esp_validazione),
        ("peggiore", esp_peggiore),
    ):
        if quale in (nome, "tutti"):
            fn()
            print()
