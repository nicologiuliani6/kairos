#!/usr/bin/env python3
"""Convertitore fra due formati lossless, in una direzione o nell'altra.

Il programma Kairos `converti.kairos` contiene una sola procedura, `rle`, che
va dal formato grezzo a quello a corse. La direzione la sceglie il tipo del
file: grezzo in ingresso e si fa `call`, codificato e si fa `uncall`. Il
decodificatore non e' scritto da nessuna parte, e' l'inverso del codificatore.

    python3 lossless/converti.py cerchio.pgm            # grezzo  -> .rle1
    python3 lossless/converti.py cerchio.pgm.rle1       # .rle1   -> grezzo
    python3 lossless/converti.py cerchio.pgm --giro     # A -> B -> A e confronto

Il contenitore del formato B: magia `RLE1`, poi per ogni corsa un byte di
valore e due byte di lunghezza, big endian.
"""

import argparse
import pathlib
import re
import struct
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent
KAIROS = ROOT.parent
PY = KAIROS / "venv" / "bin" / "python"
SORGENTE = ROOT / "converti.kairos"
MAGIA = b"RLE1"


# --- il formato B su disco ---------------------------------------------------


def impacchetta(coppie):
    """[(valore, lunghezza), ...] -> byte del contenitore."""
    out = bytearray(MAGIA)
    for v, run in coppie:
        out.append(v)
        out += struct.pack(">H", run)
    return bytes(out)


def spacchetta(dati):
    """byte del contenitore -> [(valore, lunghezza), ...]."""
    corpo = dati[len(MAGIA) :]
    if len(corpo) % 3:
        raise SystemExit("contenitore RLE1 malformato")
    return [
        (corpo[i], struct.unpack(">H", corpo[i + 1 : i + 3])[0])
        for i in range(0, len(corpo), 3)
    ]


# --- generazione ed esecuzione del programma Kairos --------------------------


def blocco_dati(righe_push, contatore, tipo):
    corpo = ["local int t = 0", "        t -= 1   push(t, a)   // sentinella"]
    corpo += righe_push
    corpo.append("    delocal int t = 0")
    corpo.append(f"    {contatore}")
    corpo.append(f"    tipo += {tipo}")
    return "    " + "\n".join(corpo)


def esegui(dati_kairos, tenere=None):
    src = SORGENTE.read_text().replace("    // DATI", dati_kairos)
    path = pathlib.Path(tenere) if tenere else (ROOT / "generato_converti.kairos")
    path.write_text(src)
    t0 = time.time()
    proc = subprocess.run(
        [str(PY), "-m", "src.kairos", str(path)],
        cwd=KAIROS,
        capture_output=True,
        text=True,
    )
    dt = time.time() - t0
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit(f"kairos fallito ({proc.returncode})")
    return leggi_uscita(proc.stdout), dt


def leggi_uscita(testo):
    """Prende le prime occorrenze di a, b, na, np dalle righe di `show`."""
    fuori = {}
    for riga in testo.splitlines():
        if riga.startswith("=== VM dump ==="):
            break
        m = re.match(r"^(\w+): (.*)$", riga)
        if not m or m.group(1) in fuori:
            continue
        nome, val = m.group(1), m.group(2).strip()
        if val.startswith("["):
            interno = val[1:-1].strip()
            fuori[nome] = [int(x) for x in interno.split(",")] if interno else []
        else:
            fuori[nome] = int(val)
    for k in ("a", "b", "na", "np"):
        if k not in fuori:
            raise SystemExit(f"uscita Kairos senza '{k}':\n{testo[:400]}")
    return fuori


# --- le due direzioni --------------------------------------------------------


def codifica(byte, tenere=None):
    """Formato A -> formato B, con `call`."""
    push = [f"        t += {b}   push(t, a)" for b in reversed(byte)]
    dati = blocco_dati(push, f"na += {len(byte)}", 0)
    fuori, dt = esegui(dati, tenere)
    if fuori["a"] != [-1] or fuori["na"] != 0:
        raise SystemExit("il codificatore non ha consumato tutto l'ingresso")
    piatto = fuori["b"]
    coppie = [(piatto[i], piatto[i + 1]) for i in range(0, len(piatto), 2)]
    if len(coppie) != fuori["np"]:
        raise SystemExit("numero di coppie incoerente")
    return coppie, dt


def decodifica(coppie, tenere=None):
    """Formato B -> formato A, con `uncall`. Nessun decodificatore scritto."""
    push = []
    for v, run in coppie:
        push.append(f"        t += {v}   push(t, b)")
        push.append(f"        t += {run}   push(t, b)")
    dati = blocco_dati(push, f"np += {len(coppie)}", 1)
    fuori, dt = esegui(dati, tenere)
    if fuori["b"] != [] or fuori["np"] != 0:
        raise SystemExit("il decodificatore non ha consumato tutte le coppie")
    pila = fuori["a"]
    if not pila or pila[0] != -1:
        raise SystemExit("sentinella mancante")
    return bytes(reversed(pila[1:])), dt


# --- riga di comando ---------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("file")
    ap.add_argument("-o", "--out")
    ap.add_argument("-k", "--keep", help="dove tenere il .kairos generato")
    ap.add_argument(
        "--giro", action="store_true", help="A -> B -> A e confronto byte a byte"
    )
    args = ap.parse_args()

    path = pathlib.Path(args.file)
    dati = path.read_bytes()
    if not dati:
        raise SystemExit("file vuoto")

    if args.giro:
        coppie, t1 = codifica(dati, args.keep)
        indietro, t2 = decodifica(coppie)
        pack = impacchetta(coppie)
        print(f"A  {path}  {len(dati)} byte")
        print(f"B  {len(pack)} byte in {len(coppie)} corse   call    {t1:.1f} s")
        print(f"A' {len(indietro)} byte                      uncall  {t2:.1f} s")
        ok = indietro == dati
        print("giro completo " + ("identico" if ok else "DIVERSO"))
        return 0 if ok else 1

    if dati.startswith(MAGIA):
        fuori, tempo = decodifica(spacchetta(dati), args.keep)
        dest = pathlib.Path(args.out or str(path) + ".grezzo")
        verso = "B -> A  (uncall)"
    else:
        coppie, tempo = codifica(dati, args.keep)
        fuori = impacchetta(coppie)
        dest = pathlib.Path(args.out or str(path) + ".rle1")
        verso = "A -> B  (call)"

    dest.write_bytes(fuori)
    print(f"{verso}   {len(dati)} -> {len(fuori)} byte   {tempo:.1f} s")
    print(f"scritto {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
