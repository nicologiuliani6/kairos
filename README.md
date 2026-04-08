# Kairos

Kairos è un linguaggio di programmazione **reversibile e concorrente**. Ogni programma Kairos può essere eseguito sia in avanti che all'indietro: l'inverso di qualunque computazione è sempre ben definito e calcolabile. Il linguaggio supporta parallelismo esplicito tramite blocchi `par/rap` e comunicazione sincrona tra thread tramite canali tipizzati.

---

## Indice

1. [Struttura del progetto](#struttura-del-progetto)
2. [Installazione e compilazione](#installazione-e-compilazione)
3. [Toolchain — comandi make](#toolchain--comandi-make)
4. [Architettura interna](#architettura-interna)
5. [Il linguaggio Kairos](#il-linguaggio-kairos)
   - [Tipi](#tipi)
   - [Dichiarazioni globali](#dichiarazioni-globali)
   - [Variabili locali — local/delocal](#variabili-locali--localdelocal)
   - [Operatori reversibili](#operatori-reversibili)
   - [Espressioni](#espressioni)
   - [Procedure e parametri](#procedure-e-parametri)
   - [call e uncall](#call-e-uncall)
   - [Blocco if-fi](#blocco-if-fi)
   - [Ciclo from-loop-until](#ciclo-from-loop-until)
   - [Stack — push e pop](#stack--push-e-pop)
   - [Canali — ssend e srecv](#canali--ssend-e-srecv)
   - [Parallelismo — par/and/rap](#parallelismo--parandarap)
   - [show](#show)
   - [Commenti](#commenti)
6. [Reversibilità — regole e vincoli](#reversibilità--regole-e-vincoli)
7. [Esempi completi](#esempi-completi)
8. [Il bytecode Kairos](#il-bytecode-kairos)
9. [Errori comuni](#errori-comuni)

---

## Struttura del progetto

```
kairos/
├── makefile
├── README.md
├── .gitignore
├── src/
│   ├── kairos.py            ← entry point: compila ed esegue
│   ├── __init__.py
│   └── frontend/
│       ├── lexer.py        ← analisi lessicale (PLY)
│       ├── parser.py       ← analisi sintattica (PLY)
│       ├── ast.py          ← utility: stampa AST
│       └── bytecode.py     ← compilatore AST → bytecode
├── src/vm/
│   ├── Kairos.c             ← core runtime VM (exec/run/dump)
│   ├── Kairos_dap.c         ← API debug/DAP (step, continue, JSON/output)
│   ├── Kairos_core.h        ← interfaccia condivisa tra core e DAP
│   ├── vm_types.h          ← strutture dati (VM, Frame, Var, Channel)
│   ├── vm_helpers.h        ← funzioni di supporto
│   ├── vm_ops.h            ← istruzioni runtime (LOCAL, PUSH, EVAL…)
│   ├── ops_arith.h         ← operatori aritmetici e loro inversi
│   ├── vm_invert.h         ← motore di inversione (UNCALL)
│   ├── vm_par.h            ← parallelismo (PAR, thread_entry)
│   ├── vm_frames.h         ← gestione frame (ricorsione, thread-clone)
│   ├── vm_channel.h        ← canali sincroni (rendezvous)
│   ├── stack.h             ← stack di puntatori a Var
│   ├── char_id_map.h       ← mappa stringa→indice
│   └── check_if_reversibility.h ← analisi statica reversibilità
├── build/
│   └── libvm.so            ← generato da make
├── tests/
│   ├── expected/           ← output attesi (opzionale)
│   ├── ...
└── examples/
    ├── ...
```

---

## Installazione e compilazione

### Requisiti

| Componente | Versione minima |
|-----------|----------------|
| Python    | 3.10           |
| GCC       | 11             |
| PLY       | 3.11           |
| PyInstaller (opzionale) | 5.0 |

### Setup da zero

```bash
# 1. Clona il repository
git clone <url> kairos
cd kairos

# 2. Crea il virtualenv e installa le dipendenze Python
make install-deps

# 3. Compila la VM (debug, con AddressSanitizer)
make

# oppure solo la build ottimizzata
make build-release
```

Dopo `make` troverai `build/libvm.so`.

---

## Toolchain — comandi make

| Comando | Descrizione |
|---------|-------------|
| `make` | Compila la VM in modalità debug (`-g -fsanitize=address,undefined`) |
| `make build-debug` | Esplicita build debug con AddressSanitizer |
| `make build-release` | Compila con `-O2 -DNDEBUG` |
| `make build-dap` | Compila `libvm_dap.so` (senza ASan, per adapter DAP) |
| `make run FILE=<f>` | Esegue un singolo file `.kairos` |
| `make test` | Esegue tutti i `.kairos` in `tests/` e `examples/` |
| `make test-one FILE=<f>` | Esegue un singolo test con output verboso |
| `make release` | Build ottimizzata + pacchetto standalone con PyInstaller |
| `make install-deps` | Crea il venv e installa `ply` e `pyinstaller` |
| `make clean` | Rimuove tutti gli artefatti generati |
| `make help` | Mostra il riepilogo dei comandi |

Esempi d'uso:

```bash
make run FILE=examples/fib.kairos
make test
make test-one FILE=tests/test_uncall.kairos
make release
```

---

## Architettura interna

```
file.kairos
    │
    ▼
[ lexer.py ]  ──  analisi lessicale con PLY
    │
    ▼
[ parser.py ]  ── analisi sintattica LALR(1), produce AST come tuple Python
    │
    ▼
[ bytecode.py ] ── visita l'AST e produce il bytecode testuale
    │
    ▼  (stringa in memoria)
[ Kairos.c + Kairos_dap.c / libvm.so ] ── VM in C che interpreta il bytecode
    │
    ├── vm_exec()      prima passata: raccoglie frame, DECL, PARAM, LABEL
    ├── vm_run_BT()    loop principale di esecuzione forward
    ├── invert_op_to_line()  esecuzione inversa (UNCALL)
    └── API DAP/debug (step/continue/output pipe)
```

Il frontend Python compila il sorgente in una stringa bytecode che viene passata direttamente alla VM tramite `ctypes` — nessun file intermedio su disco (a meno di `--dump-bytecode`).

---

## Il linguaggio Kairos

### Tipi

Kairos ha tre tipi primitivi:

| Tipo | Descrizione |
|------|-------------|
| `int` | Intero con segno a 32 bit, inizializzato a `0` |
| `stack` | Lista LIFO di interi, inizialmente vuota (`nil`) |
| `channel` | Canale sincrono per comunicazione tra thread, inizialmente vuoto (`empty`) |

---

### Dichiarazioni globali

Le variabili dichiarate nel corpo di `main` senza `local` sono variabili globali del frame. Vengono allocate nella prima passata della VM.

```kairos
procedure main()
    int x          // dichiara x, vale 0
    channel ch     // dichiara ch
    x += 5
```

> **Nota:** fuori da `main`, le variabili si dichiarano obbligatoriamente con `local/delocal`.

---

### Variabili locali — local/delocal

`local` alloca una variabile con un valore iniziale. `delocal` la dealloca verificando che il valore finale corrisponda al valore atteso. Questa coppia garantisce la reversibilità: la VM può ricostruire esattamente lo stato precedente.

```kairos
local int x = 0        // alloca x, inizializza a 0
local int y = x        // alloca y, copia il valore corrente di x
local stack s = nil    // stack vuoto
local channel ch = empty

// ... uso di x, y, s, ch ...

delocal channel ch = empty
delocal stack s = nil  // verifica che s sia vuoto
delocal int y = x      // verifica che y == valore corrente di x
delocal int x = 0      // verifica che x == 0 prima di deallocare


```

**Regole:**
- La `delocal` deve specificare il valore che la variabile ha in quel punto — se il valore non corrisponde, la VM termina con errore.
- `local` e `delocal` devono essere in ordine LIFO: l'ultima variabile dichiarata con `local` deve essere la prima a essere chiusa con `delocal`.
- All'interno di una procedura (non `main`) si usano solo `local/delocal`, mai dichiarazioni globali.

---

### Operatori reversibili

Kairos ammette solo operatori che sono invertibili per costruzione:

| Operatore | Sintassi | Inverso |
|-----------|----------|---------|
| Incremento | `x += expr` | `x -= expr` |
| Decremento | `x -= expr` | `x += expr` |
| XOR | `x ^= expr` | `x ^= expr` (è il proprio inverso) |
| Swap | `x <=> y` | `x <=> y` (è il proprio inverso) |

```kairos
x += 5        // x = x + 5
x -= y        // x = x - y
x ^= 42       // x = x XOR 42
x <=> y       // scambia x e y
```

> **Vincolo fondamentale:** la variabile a sinistra **non deve comparire** nell'espressione a destra. `x += x` non è reversibile.

---

### Espressioni

Le espressioni supportano addizione, sottrazione e parentesi:

```kairos
x += (y + 1)
x -= (a + b)
x += ((a + b) - c)
```

I letterali numerici sono interi. Non sono supportate moltiplicazione o divisione come operatori di espressione.

---

### Procedure e parametri

```kairos
procedure nome(tipo param1, tipo param2)
    // corpo
```

I parametri sono passati **per riferimento**: le modifiche ai parametri all'interno della procedura si riflettono sulle variabili del chiamante.

```kairos
procedure increment(int x)
    x += 5

procedure main()
    local int a = 3
    call increment(a)   // a diventa 8
    delocal int a = 8
```

Ogni programma Kairos deve avere una procedura `main()` senza parametri. L'esecuzione parte da `main`.

---

### call e uncall

`call` esegue una procedura normalmente. `uncall` esegue la procedura **al contrario**: le istruzioni vengono eseguite in ordine inverso e ogni operazione viene sostituita dalla sua inversa (`+=` diventa `-=`, `push` diventa `pop`, ecc.).

```kairos
procedure increment(int x)
    x += 5

procedure main()
    local int a = 0
    call increment(a)    // a = 5
    show(a)              // stampa 5
    uncall increment(a)  // a torna 0
    show(a)              // stampa 0
    delocal int a = 0
```

`uncall` è la primitiva chiave della reversibilità: permette di "annullare" qualunque computazione senza doverla riscrivere manualmente.

---

### Blocco if-fi

Il blocco condizionale in Kairos richiede una **condizione di entrata** e una **condizione di uscita**:

```kairos
if <condizione_entrata> then
    // ramo then
else
    // ramo else (opzionale)
fi <condizione_uscita>
```

La condizione di uscita viene valutata **dopo** il corpo ed è ciò che rende il blocco reversibile: l'inverso sa quale ramo è stato eseguito leggendo la condizione di uscita.

```kairos
procedure check_boolean(int flag)
    if flag == 1 then
        show(flag)
    else
        show(flag)
    fi flag == 1
```

**Regola di reversibilità:** la variabile usata nella condizione **non deve essere modificata** all'interno del blocco `if-fi`. Il checker statico segnala questa violazione come warning.

---

### Ciclo from-loop-until

```kairos
from <condizione_entrata> loop
    // corpo
until <condizione_uscita>
```

- `from`: la condizione deve essere vera all'ingresso del ciclo (prima iterazione) e falsa per tutte le iterazioni successive.
- `until`: la condizione deve essere falsa durante il ciclo e vera quando il ciclo termina.

```kairos
// Esempio: somma da 1 a n
local int i = 0
from i == 0 loop
    i += 1
until i == n
delocal int i = n
```

Il ciclo è reversibile: eseguito al contrario, la condizione `until` diventa la condizione di entrata e `from` quella di uscita, e il corpo viene eseguito all'indietro.

---

### Stack — push e pop

```kairos
push(var, stack)    // sposta il valore di var in cima allo stack, azzera var
pop(var, stack)     // preleva dalla cima dello stack e lo aggiunge a var
```

`push` azzera la variabile sorgente dopo aver copiato il valore (per preservare la biettività). `pop` **aggiunge** il valore prelevato alla variabile destinazione (non sovrascrive).

```kairos
local stack s = nil
local int x = 5
push(x, s)          // s = [5], x = 0
local int y = 0
pop(y, s)           // s = [], y = 5
delocal int y = 5
delocal int x = 0
delocal stack s = nil
```

L'inverso di `push` è `pop` e viceversa.

---

### Canali — ssend e srecv

I canali sono code sincrone (rendezvous): `ssend` blocca finché un `srecv` non è pronto a ricevere, e viceversa.

```kairos
ssend(var, ch)      // invia var sul canale ch, azzera var
srecv(var, ch)      // riceve dal canale ch e aggiunge il valore a var
```

Come `push/pop`, `ssend` azzera la sorgente dopo l'invio e `srecv` somma (non sovrascrive) alla destinazione. L'inverso di `ssend` è `srecv` e viceversa.

I canali sono pensati per essere usati esclusivamente all'interno di blocchi `par/rap`.

---

### Parallelismo — par/and/rap

```kairos
par
    // thread 0
and
    // thread 1
and
    // thread 2
rap
```

`par/rap` avvia i thread elencati in parallelo. I thread condividono tutte le variabili del frame corrente. La sincronizzazione tra thread avviene esclusivamente tramite canali.

I blocchi `par` possono essere annidati:

```kairos
par
    ssend(x, c)
and
    par
        ssend(y, c)
    and
        srecv(a, c)
        srecv(b, c)
    rap
rap
```

**Inversione di par:** `uncall` su una procedura contenente `par` inverte l'ordine dei thread e scambia `ssend↔srecv` e `call↔uncall` all'interno di ogni thread.

---

### show

`show(var)` stampa il valore corrente di una variabile su stdout. Non è reversibile — nell'inversione viene semplicemente saltata.
Al termine dell'esecuzione, la VM stampa sempre anche un dump finale (`=== VM dump ===`), sia in run standard sia in modalità DAP.

```kairos
show(x)        // stampa: x: 42
show(result)   // stampa: result: [1, 2, 3, 4, 5]
```

---

### Commenti

```kairos
// questo è un commento su riga singola
```

I commenti si estendono fino alla fine della riga. Non esistono commenti multiriga.

---

## Reversibilità — regole e vincoli

Kairos garantisce la reversibilità a patto che il programma rispetti alcune regole. Il compilatore effettua un'analisi statica e segnala le violazioni prima dell'esecuzione.

### 1. Assegnamento: niente autoriflessività

```kairos
x += x    // ERRORE: x compare su entrambi i lati
x += y    // OK
```

### 2. if-fi: la variabile di controllo non deve cambiare nel corpo

```kairos
if x == 0 then
    x += 1    // WARNING: x è la variabile di controllo
fi x == 0
```

### 3. local/delocal: la sorgente non deve essere modificata tra local e delocal

```kairos
local int y = x     // y inizializzata con x
x += 1              // WARNING: x viene modificata prima del delocal di y
delocal int y = x   // y non può essere ricostruita in UNCALL
```

### 4. DELOCAL verifica il valore a runtime

Se il valore finale della variabile non corrisponde a quello dichiarato nella `delocal`, la VM termina:

```
[VM] DELOCAL: valore finale errato! (var=x, atteso=0, trovato=3)
```

### 5. Stack e channel devono essere vuoti alla delocal

```kairos
delocal stack s = nil     // s deve essere vuoto
delocal channel ch = empty // ch deve essere vuoto
```

---

## Il bytecode Kairos

Il compilatore produce un bytecode testuale che la VM interpreta. Ogni riga ha il formato:

```
NNNN  @SRC   ISTRUZIONE [argomenti...]
```

dove:
- `NNNN` è la riga fisica del bytecode (4 cifre, crescente).
- `@SRC` è la riga del sorgente Kairos da cui l'istruzione proviene (usata anche dal debugger DAP).

Esempio reale:

```
0052  @33     PAR_START
0053  @33     THREAD_0
0054  @34     CALL producer buffer n
```

Le istruzioni principali:

| Istruzione | Descrizione |
|-----------|-------------|
| `START` | Inizio programma |
| `HALT` | Fine programma |
| `PROC name` | Inizio procedura |
| `END_PROC name` | Fine procedura |
| `PARAM type name` | Dichiarazione parametro |
| `DECL type name` | Dichiarazione variabile globale |
| `LOCAL type name val` | Alloca variabile locale |
| `DELOCAL type name val` | Dealloca e verifica variabile locale |
| `PUSHEQ var expr` | `var += expr` |
| `MINEQ var expr` | `var -= expr` |
| `XOREQ var expr` | `var ^= expr` |
| `SWAP var1 var2` | Scambia var1 e var2 |
| `PUSH var stack` | Sposta var in cima allo stack |
| `POP var stack` | Preleva dalla cima e aggiunge a var |
| `SSEND var ch` | Invia var sul canale (alias di PUSH su channel) |
| `SRECV var ch` | Riceve dal canale e aggiunge a var |
| `EVAL lhs op rhs` | Valuta condizione, risultato in flag interno |
| `ASSERT lhs op rhs` | Verifica condizione, termina se falsa |
| `JMPF label` | Salta a label se EVAL è falso |
| `JMP label` | Salta incondizionato |
| `LABEL name` | Definisce un'etichetta |
| `CALL proc args...` | Chiama procedura |
| `UNCALL proc args...` | Chiama procedura in inverso |
| `SHOW var` | Stampa variabile |
| `PAR_START` | Inizio blocco parallelo |
| `THREAD_N` | Inizio thread N |
| `PAR_END` | Fine blocco parallelo |

Per vedere il bytecode generato da un programma:

```bash
make run FILE=examples/fib.kairos
# oppure
./venv/bin/python -m src.kairos examples/fib.kairos --dump-bytecode
# il bytecode viene scritto in bytecode.txt
```

Nota:
- con `--dap` il frontend scrive sempre `bytecode.txt` e non esegue direttamente la VM (l'esecuzione è gestita dall'adapter DAP).

---

## Errori comuni

### `DELOCAL: valore finale errato`

```
[VM] DELOCAL: valore finale errato! (var=x, atteso=0, trovato=3)
```

La variabile non ha il valore atteso al momento della `delocal`. Controlla che il corpo tra `local` e `delocal` riporti la variabile al valore iniziale.

### `DELOCAL: ordine errato`

```
[VM] DELOCAL: ordine errato! atteso 'x', trovato 'y'
```

Le `delocal` non sono in ordine LIFO rispetto alle `local`. L'ultima variabile dichiarata con `local` deve essere la prima a essere chiusa.

### `DELOCAL: stack/channel non è nil/empty`

```
[VM] DELOCAL: result non è nil/empty!
```

Lo stack o il canale non è vuoto al momento della `delocal`. Nel caso di `uncall`, significa che la procedura inversa non ha svuotato completamente la struttura.

### `cannot open shared object file: libvm.so`

```
OSError: .../libvm.so: cannot open shared object file
```

La VM non è stata compilata. Esegui `make build-release` prima di `make test`.

### `[PARSER] token non atteso`

Errore sintattico nel sorgente. La riga indicata contiene un token non riconosciuto dalla grammatica.

### Warning di reversibilità

```
[WARNING] La procedura "f" dentro un blocco if-fi ha la variabile di controllo
          'x' modificata da istruzione: PUSHEQ (riga 5)
```

Il checker statico ha trovato una potenziale violazione. Il programma continua a essere eseguito ma potrebbe non essere correttamente invertibile.