# Conversione fra formati lossless con `call` e `uncall`: analisi

## L'idea

Due rappresentazioni lossless dello stesso contenuto, e un solo programma che
le mette in relazione. Il formato A è la sequenza grezza dei byte; il formato B
è la stessa informazione codificata a corse. Passare da A a B è una biiezione,
quindi il decodificatore non è un secondo programma da scrivere e mantenere
allineato: è il codificatore letto al contrario.

Il programma riconosce il tipo del file in ingresso e sceglie la direzione:

```kairos
if tipo == 0 then
    call rle(a, b, na, np)
else
    uncall rle(a, b, na, np)
fi tipo == 0
```

Sotto quel bivio c'è una sola procedura, `rle`. Nessuna riga di decodifica
esiste nel sorgente. Questo è il punto: in un linguaggio reversibile la
decodifica non si implementa, si ottiene.

## Perché serve che `rle` sia pulita

`rle` consuma interamente l'ingresso e lascia solo l'uscita:

```
pre   src = [sentinella, byte_n-1, ..., byte_0]   nsrc = n   dst vuoto   npair = 0
post  src = [sentinella]                          nsrc = 0   dst = coppie  npair = k
```

È la nozione di *clean* di Lyngby, Nylandsted, Glück e Yokoyama. Se `rle`
lasciasse dietro di sé stati intermedi, l'inverso non sarebbe una decodifica:
per girare avrebbe bisogno anche di quella spazzatura, cioè di informazione che
un file in formato B non contiene. La pulizia non è eleganza, è la condizione
che rende l'inverso utilizzabile su un file vero.

## Due dettagli di linguaggio che hanno cambiato il codice

**Le guardie di Kairos sono confronti semplici.** Non c'è disgiunzione. La
condizione naturale per la scansione di una corsa è "resta qualcosa in `src`,
oppure ho appena consumato un elemento", e non si scrive. Il tentativo di
aggirarla con `(nsrc + cont) > 0` supera il parser ma non la VM, che spezza la
riga `EVAL` sugli spazi e non ricompone la parentesi.

La soluzione non è stata piegare il linguaggio ma cambiare i dati: una
sentinella `-1` in fondo allo stack. Nessun byte vale -1, quindi il confronto
si interrompe sempre da solo e il test di vuotezza sparisce dal codice. La
guardia torna a essere `cont == 1`, un confronto semplice.

**Ogni `if` deve dichiarare in uscita quale ramo ha preso.** Dopo aver
consumato la cima di `src` non è più possibile verificare che fosse uguale a
`v`. Serve un testimone: `cont` vale 1 quando l'ultimo elemento letto era
uguale, 0 quando la corsa è finita. È lo stesso schema del `pref` usato per il
confronto lessicografico nella trasformata di Burrows e Wheeler, e sembra
essere il modo standard di scrivere condizionali non banali in Kairos.

## Risultati

Immagini PGM binarie, cerchio pieno su fondo chiaro. Giro completo A → B → A
con confronto byte a byte del file finale contro l'originale.

| immagine | A (byte) | corse | B (byte) | giro |
|---|---|---|---|---|
| 16x16 | 269 | 33 | 103 | identico |
| 32x32 | 1.037 | 53 | 163 | identico |
| 64x64 | 4.109 | 93 | 283 | identico |
| 128x128 | 16.399 | 179 | 541 | identico |
| 256x256 | 65.551 | 343 | 1.033 | identico |

Il contenitore del formato B spende tre byte per corsa (valore su un byte,
lunghezza su due). Con la lunghezza su un byte e corse spezzate a 255 sarebbero
due byte per corsa, ma il conto non cambia natura.

## Efficienza

Tempi sul giro completo, separando il frontend dall'esecuzione:

| immagine | righe del programma generato | parse | esecuzione |
|---|---|---|---|
| 32x32 | 1.132 | 0,1 s | 0,2 s |
| 64x64 | 4.204 | 0,6 s | 0,2 s |
| 128x128 | 16.494 | 6,3 s | 0,8 s |
| 256x256 | 65.646 | 87,3 s | 2,0 s |

**L'esecuzione è lineare.** 65 KB di immagine si comprimono e si ricostruiscono
in circa 2 s. L'RLE su stack tocca solo la cima, non chiede mai accesso
casuale, e la VM reversibile lo esegue al costo che ci si aspetta.

**Il parse non lo è.** Cresce circa con il quadrato della lunghezza del
programma, e a 65.646 righe costa 87,3 s, cioè quaranta volte l'esecuzione.

La ragione è che la VM non ha filesystem: l'ingresso non può essere letto a
runtime, quindi i byte del file finiscono nel sorgente come sequenza di `push`.
Il programma ha la dimensione del dato. Il muro che si incontra non è quello
della macchina reversibile, è quello di un frontend LALR in Python messo a
digerire un sorgente da 65 mila righe.

La stessa asimmetria si vede fra le due direzioni. Il `call` parte da 65.551
byte e paga 87 s di parse; l'`uncall` parte da 343 coppie, cioè 686 `push`, e
chiude in 1,9 s. Non è che l'inverso sia più veloce dell'inverso di sé stesso:
è che il suo ingresso è più piccolo, quindi lo è anche il suo programma.

## Quanto costa davvero l'inverso

Il decodificatore è gratuito da scrivere. Resta da chiedersi se sia gratuito da
eseguire, perché un inverso lento sarebbe un decodificatore inutile.

I tempi della tabella precedente non rispondono: i due programmi non fanno lo
stesso lavoro, il diretto esegue n `push` di caricamento dati e l'inverso solo
2k. Per isolare la sola conversione ogni programma si esegue due volte, una
com'è e una con il contatore azzerato: con `nsrc = 0` (o `npair = 0`) la
conversione non parte, mentre parse e caricamento restano identici. La
differenza è la conversione e nient'altro.

| byte | corse | `call` | `uncall` | rapporto |
|---|---|---|---|---|
| 1.037 | 53 | 0,02 s | 0,03 s | 2,06x |
| 4.109 | 93 | 0,06 s | 0,11 s | 1,73x |
| 16.399 | 179 | 0,20 s | 0,43 s | 2,18x |
| 65.551 | 343 | 0,97 s | 1,68 s | 1,73x |

**L'inverso costa fra 1,7 e 2,2 volte il diretto, e il rapporto non cresce con
la taglia.** È un fattore costante, non un fattore n: l'inversione è
un'operazione locale sull'istruzione, non una rielaborazione del programma. Un
decodificatore ottenuto per inversione è quindi utilizzabile, non solo
dimostrabile.

## L'inverso è anche un decodificatore che valida?

Questa è la domanda che decide se l'idea regge fuori dal laboratorio. Un file
in formato B arriva da disco e non c'è nessuna garanzia che sia nell'immagine
del codificatore. Se lo si dà all'inverso, cosa succede?

L'esperimento (`esperimenti.py validazione`) costruisce sette ingressi in
formato B, uno legittimo e sei che nessuna esecuzione diretta produrrebbe.

**Il risultato di partenza è negativo.** L'inverso accetta in silenzio ingressi
fuori dall'immagine e risponde con dati sbagliati, oppure diverge.

| ingresso | esito |
|---|---|
| riferimento, immagine del diretto | accetta, corretto |
| corsa di lunghezza 0 | **non termina** |
| due corse adiacenti con lo stesso valore | **accetta**, e il risultato ricodificato non torna all'ingresso |
| `npair` più piccolo del contenuto | **accetta**, risultato troncato |
| `npair` più grande del contenuto | rifiuta, ma per `POP` su stack vuoto |
| lunghezza negativa | **non termina** |
| valore che collide con la sentinella | **accetta** |

### La causa: le due guardie sono controllate una sola

Il condizionale reversibile ha due guardie e la semantica ne chiede due
controlli, uno per verso. Andando in avanti si controlla l'asserzione d'uscita.
Andando all'indietro l'asserzione d'uscita sceglie il ramo, e resta da
verificare che la guardia d'ingresso sia coerente con la scelta.

Il caso minimo che lo mostra:

```kairos
procedure p(int x, int y)
    if x > 0 then
        y += 1
    fi y == 1

procedure main()
    int x
    int y
    x -= 5          // la guardia d'ingresso e' falsa
    y += 1          // l'asserzione d'uscita e' vera
    uncall p(x, y)
    show(y)
```

Lo stato di partenza non è raggiungibile da nessuna esecuzione diretta di `p`.
La VM invertiva il ramo `then`, portava `y` a 0 e terminava con successo. Lo
stesso programma percorso in avanti con l'asserzione d'uscita falsa produceva
invece l'errore `IF/FI non reversibile`: il controllo c'era in un verso e
mancava nell'altro. Ora entrambi i versi lo fanno.

### La conseguenza vera: le asserzioni non funzionano all'indietro

In Janus un'asserzione si scrive con un condizionale a due guardie:

```kairos
local int positiva = 0
    if run > 0 then
        positiva += 1
    fi positiva == 1
delocal int positiva = 1
```

In avanti funziona: se `run <= 0` la variabile resta a 0 e la `delocal`
fallisce. All'indietro dovrebbe funzionare allo stesso modo, e invece **non
faceva assolutamente nulla**, perché il ramo si sceglieva dall'asserzione
d'uscita e la guardia `run > 0` non veniva mai riletta.

Questo è il punto che rende la mancanza grave e non cosmetica: non riguarda un
angolo del linguaggio, riguarda l'unico modo che si ha di scrivere una
precondizione. In un decodificatore ottenuto per inversione tutte le
precondizioni sull'ingresso sono esattamente asserzioni di questa forma.

### Il controllo mancante, e cosa cambia

Il controllo è stato aggiunto in `src/vm/vm_invert.h`
(`check_if_entry_inverse`): dopo aver invertito il ramo, quando lo store è
tornato allo stato che precedeva l'if e la guardia torna valutabile, si verifica
che valga se e solo se il ramo preso era il `then`. Le due eccezioni sono quelle
che il controllo in avanti ha già: nessun controllo dove manca l'asserzione
d'uscita (il ramo è stato dedotto dalla guardia stessa, riverificarla sarebbe
una tautologia) e nessun controllo nei rami di `par`, dove altri thread possono
mutare gli int condivisi fra la valutazione e la verifica.

Con il controllo e l'asserzione `run > 0` scritta nel sorgente:

| ingresso | prima della correzione | dopo |
|---|---|---|
| riferimento | accetta, corretto | accetta, corretto |
| corsa di lunghezza 0 | non termina | **rifiuta** |
| due corse adiacenti uguali | accetta, sbagliato | **rifiuta** |
| `npair` troppo piccolo | accetta, troncato | accetta, troncato |
| `npair` troppo grande | rifiuta (`POP` su stack vuoto) | rifiuta |
| lunghezza negativa | non termina | **rifiuta** |
| valore = sentinella | accetta | accetta |

Tre casi passano da errore silenzioso o divergenza a rifiuto esplicito. Va
sottolineato che l'asserzione `run > 0` è presente nel sorgente in entrambe le
colonne: la colonna di sinistra non è "senza asserzione", è "con l'asserzione
che non viene controllata".

Il costo della correzione, misurato prima di renderla definitiva:
**nessuna regressione**. La suite Kairos dà 58 PASS / 4 FAIL identici, con gli
stessi quattro fallimenti di prima. Tutti gli 87 programmi C di Mnemo, che è il
generatore vero di `uncall`, danno uscita identica con e senza il controllo. Le
due sole differenze osservate sono risultate artefatti: `PC.c` è
nondeterministico di suo, perché lo scheduling di `par` cambia l'ordine
dell'uscita da un'esecuzione all'altra; `maze_backtrack.c` impiega circa cinque
minuti e il timeout dello sweep lo troncava in punti diversi, mentre eseguito
fino in fondo dà lo stesso esito nelle due modalità. I due programmi
`try_rollback.c` e `try_backtracking.c` falliscono sotto
`--check-invertibility`, ma falliscono allo stesso modo anche sulla VM
precedente alla modifica.

### Cosa resta fuori portata, e perché

**Il campo di conteggio.** `npair` è un ingresso dell'inverso, e nulla nel
programma lo lega al contenuto di `dst`. Un conteggio troppo piccolo lascia
coppie inutilizzate e nessuno se ne accorge. Verrebbe da concludere che i
formati auto-delimitati siano preferibili, ma la conclusione è l'opposta: il
ciclo di Kairos ha bisogno di un'asserzione d'ingresso che sia falsa a ogni
rientro, e per un ciclo guidato dai dati quell'asserzione è naturalmente un
contatore. **Il linguaggio spinge verso formati contati, non terminati**, e il
prezzo è che il contatore va convalidato fuori dal programma. Nel driver lo si
fa, controllando che `dst` finisca vuoto.

**La sentinella in banda.** Il valore -1 che ha semplificato le guardie è
rappresentabile in formato B, quindi un file costruito ad arte può contenerlo e
confondersi con il fondo dello stack. La pipeline reale non lo produce mai,
perché i byte stanno fra 0 e 255, ma è una superficie d'attacco creata dalla
scelta di codifica, non dal linguaggio.

**La terminazione.** Nessun controllo di guardia rende decidibile se un inverso
termina. Qui i due casi divergenti si sono chiusi perché l'asserzione li
intercetta prima del ciclo, ma è una fortuna del caso specifico, non un
teorema.

## Il caso peggiore

Dati incomprimibili, ogni corsa lunga 1, quindi il formato B è grande il doppio
del formato A.

| n | corse | B/A | `call` | `uncall` | giro |
|---|---|---|---|---|---|
| 200 | 199 | 1,99 | 0,1 s | 0,2 s | identico |
| 800 | 797 | 1,99 | 0,2 s | 0,3 s | identico |

Il giro chiude comunque. Vale la pena dirlo esplicitamente perché è il punto
che distingue questo lavoro da un compressore: **B non è "il file compresso",
è "il file nell'altro formato"**, e la biiezione non dipende dal fatto che
comprima. Un compressore che raddoppia i dati è inutile; un convertitore che
raddoppia i dati è ancora un convertitore corretto.

## Confronto con la via C

Lo stesso identico algoritmo, sullo stesso identico file da 269 byte, scritto in
C e passato per Mnemo, costa 290,5 s. Scritto in Kairos su stack costa 0,1 s.
Tremila volte.

La differenza non è nella VM, che è la stessa. È nella struttura dati. In C
l'RLE si scrive su array con indice a runtime, e Mnemo abbassa ogni accesso
indicizzato in una catena di confronti lunga quanto l'array: una scansione
lineare diventa O(n²). Su stack la stessa scansione tocca solo la cima e resta
O(n).

Il confronto va letto con onestà: non dice che Kairos è meglio di C, dice che
l'RLE è un algoritmo che scorre e non salta, e che scritto contro la struttura
dati giusta non paga nulla. Un algoritmo che salta davvero, come la trasformata
di Burrows e Wheeler, paga il fattore n su entrambe le strade.

## Sviluppi

**Lettura e scrittura di file dalla VM.** È l'intervento con il rapporto
migliore fra costo e resa, e vale per entrambi gli studi. Stacca la dimensione
del programma da quella del dato, elimina gli 87 s di parse, e porta il caso da
65 KB da 90 s a 2 s. Non richiede di toccare la semantica reversibile: leggere
e scrivere sono effetti al bordo, come `show`.

**Riconoscimento del formato dentro il programma.** Oggi il tipo arriva come
variabile scritta dal generatore. Con la lettura da file, il programma
leggerebbe da sé i byte di magia e sceglierebbe la direzione, che è la forma
compiuta dell'idea.

**Più di due formati.** Con tre rappresentazioni e le conversioni A→B e B→C si
ottengono tutti e sei i percorsi orientati componendo `call` e `uncall`. Il
guadagno però va dichiarato per quello che è: anche un convertitore
convenzionale organizzato a stella scrive O(k) conversioni, non O(k²). Quello
che la reversibilità aggiunge sono due cose, entrambe misurabili: **metà del
codice**, perché di ogni conversione se ne scrive una direzione sola, e
**l'impossibilità che le due direzioni divergano**, perché non sono due
programmi che qualcuno deve tenere allineati. È un argomento più stretto di
quello quadratico, ed è quello vero.

**Convalida del conteggio dentro il linguaggio.** Oggi il fatto che `dst` finisca
vuoto lo controlla il driver Python, fuori dal programma. Una primitiva che
asserisca una pila vuota porterebbe il controllo dentro, e chiuderebbe l'ultima
classe di ingressi malformati che l'inverso accetta in silenzio.

**Corse in parallelo.** L'immagine si divide in bande orizzontali indipendenti;
i confini non si toccano perché una corsa non attraversa una banda se le bande
si tagliano sulle righe. Ogni banda diventa un ramo di `par`, ed è il punto in
cui reversibilità e concorrenza si vedono insieme su un file vero.

**Codifiche più forti.** LZW e la trasformata di Burrows e Wheeler comprimono
molto meglio dell'RLE su dati non sintetici, ma chiedono accesso casuale, che
su stack costa un fattore n. Restano subordinate a una memoria indicizzata
reversibile a costo costante.
