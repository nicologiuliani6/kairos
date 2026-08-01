# Compressione lossless reversibile

Studio su un caso d'uso in cui reversibilità e concorrenza servono entrambe:
comprimere e decomprimere sono per costruzione l'una l'inversa dell'altra,
quindi un unico programma reversibile le realizza entrambe, e i blocchi in cui
si divide l'ingresso sono indipendenti, quindi si trasformano in parallelo.

Riferimento: Lyngby, Nylandsted, Glück, Yokoyama, *Towards Clean Reversible
Lossless Compression: A Reversible Programming Experiment with Zip*, RC 2024,
LNCS 14680, pp. 94–102. Da lì vengono la struttura e il vettore di prova; quel
lavoro è però interamente sequenziale.

## File

| file | contenuto |
|---|---|
| `indice_su_stack.kairos` | accesso per indice reversibile sopra gli stack, che Kairos non ha nativamente |
| `bwt.kairos` | trasformata di Burrows–Wheeler su un blocco, e la sua inversa |
| `bwt_parallelo.kairos` | i blocchi come rami di un `par`, colonne e indici consegnati per canale |
| `compress_par.kairos` | trasformata delta a blocchi: lo scheletro concorrente, più semplice da leggere |
| `genera_bwt.py` | genera i casi di misura, in variante sequenziale e concorrente |
| `converti.kairos` | conversione fra due formati lossless: una procedura sola, `call` in un verso e `uncall` nell'altro |
| `converti.py` | driver del convertitore: file in ingresso, formato riconosciuto, file in uscita |
| `esperimenti.py` | costo dell'inverso, validazione, caso peggiore |
| `analisi_converti.md` | risultati e misure del convertitore |
| `casi/guardia_inversa.kairos` | caso minimo: la guardia d'ingresso non verificata invertendo |
| `cerchio.pgm` | immagine di prova, 16x16 in scala di grigi |

Tutti i `.kairos` fanno parte di `make test`.

## Conversione fra formati

Un file grezzo e la sua codifica a corse sono due rappresentazioni lossless
dello stesso contenuto. Il decodificatore non è scritto: è il codificatore
invertito, e il programma sceglie la direzione dal tipo del file.

```bash
python3 lossless/converti.py lossless/cerchio.pgm --giro   # A -> B -> A, confronto byte a byte
python3 lossless/converti.py lossless/cerchio.pgm          # scrive cerchio.pgm.rle1
python3 lossless/converti.py cerchio.pgm.rle1              # torna al grezzo, con uncall
```

Giro completo verificato fino a 256x256, cioè 65.551 byte, ricostruiti
identici. L'esecuzione è lineare, circa 2 s per 65 KB; il costo che cresce è il
parse del sorgente generato, perché la VM non legge file e i byte finiscono nel
programma. L'inverso costa fra 1,7 e 2,2 volte il diretto, e il rapporto non
cresce con la taglia.

```bash
python3 lossless/esperimenti.py tutti   # costo, validazione, caso peggiore
```

Questo studio ha portato a una correzione della VM: invertendo un `if`, la
guardia d'ingresso non veniva riletta, quindi l'inverso accettava in silenzio
stati che nessuna esecuzione diretta produce e le asserzioni scritte
nell'idioma di Janus non avevano effetto all'indietro. Il caso minimo è
[`casi/guardia_inversa.kairos`](casi/guardia_inversa.kairos). Dettagli e misure
in [`analisi_converti.md`](analisi_converti.md).

## Riprodurre le misure

```
python3 lossless/genera_bwt.py <n> <blocchi> <seq|par> > /tmp/caso.kairos
./venv/bin/python -m src.kairos /tmp/caso.kairos
```

Le due varianti hanno corpo identico: il confronto dei tempi misura solo
l'effetto della concorrenza.

## Risultati

Correttezza, sul vettore di prova del lavoro di riferimento (`banana`, con
alfabeto ridotto a=1, b=2, n=3):

```
blocco                   [2, 1, 3, 1, 3, 1]     banana
rotazioni ordinate       [5, 3, 1, 0, 4, 2]
ultima colonna           [3, 3, 2, 1, 1, 1]     nnbaaa
indice                   3
ricostruzione da (colonna, indice)              banana
```

Costo: l'esponente misurato passa da 3,29 a 3,57 fra n=6 e n=12, in
avvicinamento a n^4. Il lavoro di riferimento dichiara n^3 per la BWT ingenua
con gli array; il fattore in più è l'accesso per indice emulato sugli stack.

Concorrenza, su 24 nuclei:

```
blocchi   sequenziale   concorrente   guadagno
   2         4,53 s        3,77 s      1,20x
   4         9,01 s        6,86 s      1,31x
   6        13,60 s        7,68 s      1,77x
   8        18,21 s       10,68 s      1,71x
```

Il guadagno si ferma attorno a 1,75x pur con nuclei liberi: il raccoglitore è un
ramo solo che riceve dai canali uno per volta, e l'ingresso in un `par` costa una
copia dell'intero bytecode per ramo.
