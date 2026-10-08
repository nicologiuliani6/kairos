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
| `indice_su_stack.kairos` | accesso per indice sopra gli stack (il ponte che serviva prima degli array; resta come esempio) |
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
ricostruzione da (colonna, indice)  [2, 1, 3, 1, 3, 1]   banana
```

Costo di un blocco, `genera_bwt.py <n> 1 seq` (minimo di 3 esecuzioni):

```
 n      6     8    12    16    20    24    28    32
 s   0,13  0,17  0,38  0,76  1,42  2,79  3,97  5,63
```

La pendenza della regressione log-log fra n=12 e n=32 e' 2,82: il costo e' n^3,
come nel lavoro di riferimento (il tempo fisso di avvio pesa sui blocchi piccoli).

Concorrenza, n=20 (mediana di 3 esecuzioni, 10 per 6 blocchi; 6 nuclei / 12 thread):

```
blocchi   sequenziale   concorrente   guadagno
   2         2,29 s        1,27 s       1,80x
   4         4,49 s        1,41 s       3,19x
   6         6,77 s        1,58 s       4,29x
  12        13,62 s        2,95 s       4,62x
```

Riferimento hardware: lo stesso blocco lanciato come processi indipendenti da'
un throughput di 3,50x (4), 4,59x (6), 5,10x (12). Il `par` ne raggiunge il 91%.

Il guadagno e' quasi lineare fino a 4 blocchi e poi si appiattisce: oltre i sei
nuclei fisici i thread condividono le unita' di calcolo, e resta la parte
sequenziale (il raccoglitore riceve dai canali uno per volta).

Le misure hanno richiesto tre correzioni della VM, tutte sul percorso dei rami
di `par`: i lock globali sull'indice dei nomi a ogni chiamata e a ogni
`local`/`delocal`, la copia di 16 KB per istruzione, e un risveglio perso sulla
variabile di condizione condivisa del `par` (con `pthread_cond_signal` il
mittente di un rendez-vous poteva restare fermo finche' un altro thread non
terminava, e l'inversione di un `par` girava di fatto un ramo alla volta).
