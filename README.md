# Kairos

Kairos è un linguaggio di programmazione **reversibile e concorrente**, ispirato a [Janus](https://en.wikipedia.org/wiki/Janus_%28time-reversible_computing_programming_language%29) e modificato per ampliare a problemi concorrenziali deterministici. Ogni programma Kairos può essere eseguito sia in avanti che all'indietro: l'inverso di qualunque computazione è sempre ben definito e calcolabile. Il linguaggio supporta parallelismo esplicito e comunicazione sincrona tra thread tramite canali tipizzati.

---

## Indice


1. [Struttura del progetto](#struttura-del-progetto)
2. [Installazione](#installazione)
   - [Requisiti](#requisiti)
   - [Setup da zero](#setup-da-zero)
3. [Toolchain — comandi make](#toolchain--comandi-make)
   - [Pacchetti Linux](#pacchetti-linux)
   - [Integrazione VS Code](#integrazione-vs-code)
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
   - [Parallelismo — par/and/rap](#parallelismo-par-and-rap)
     - [Analisi statica e lock a runtime](#analisi-statica-par)
   - [show](#show)
   - [Commenti](#commenti)
6. [Reversibilità — regole e vincoli](#reversibilità--regole-e-vincoli)
   - [Controlli statici del compilatore (`parser.py`)](#controlli-statici-del-compilatore-parserpy)
7. [Il bytecode Kairos](#il-bytecode-kairos)
8. [Errori comuni](#errori-comuni)

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
│   ├── Janus.c              ← core runtime VM (exec/run/dump)
│   ├── Janus_dap.c          ← API debug/DAP (step, continue, JSON/output)
│   ├── Kairos_core.h        ← interfaccia condivisa tra core e DAP
│   ├── vm_types.h          ← strutture dati (VM, Frame, Var, Channel)
│   ├── vm_helpers.h        ← funzioni di supporto
│   ├── vm_ops.h            ← istruzioni runtime (LOCAL, PUSH, EVAL…)
│   ├── ops_arith.h         ← operatori aritmetici e loro inversi
│   ├── vm_invert.h         ← motore di inversione (UNCALL)
│   ├── vm_par.h            ← parallelismo (PAR, thread_entry)
│   ├── vm_ref_lock.h       ← lock su mutazioni int nei thread PAR
│   ├── vm_frames.h         ← gestione frame (ricorsione, thread-clone)
│   ├── vm_channel.h        ← canali sincroni (rendezvous)
│   ├── stack.h             ← stack di puntatori a Var
│   ├── char_id_map.h       ← mappa stringa→indice
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
Per l'installazione automatica guardare: [Pacchetti Linux](#pacchetti-linux)

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
git clone https://github.com/nicologiuliani6/kairos.git
cd kairos

# 2. Crea il virtualenv e installa le dipendenze Python
make install-deps

# 3. Compila la VM
make

# 4. Esegui un file
make run FILE=examples/fib.kairos

# Compila la App come file singolo 
make release
```

Dopo `make` troverai `build/libvm.so`.

---

## Toolchain — comandi make

I target sono definiti nel file `makefile` (minuscolo).

| Comando | Descrizione |
|---------|-------------|
| `make` | Compila la VM in modalità release (`-O2 -DNDEBUG`) |
| `make build-release` | Compila `build/libvm.so` con ottimizzazioni |
| `make build-dap` | Compila `build/libvm_dap.so` per il debugger DAP |
| `make run FILE=<f.kairos>` | Esegue un singolo file `.kairos` (con `--dump-bytecode`) |
| `make test` | Esegue i `.kairos` in `tests/` e `examples/` (vedi `KAIROS_EXCLUDE` nel makefile) |

Se invochi direttamente `./venv/bin/python -m src.kairos …` dopo aver modificato i sorgenti C della VM, esegui prima `make build-release`: altrimenti resta in uso un `build/libvm.so` obsoleto (o, se manca, la VM installata in `/opt/kairosapp/`). Il frontend stampa un avviso su *stderr* in questi casi.
| `make release` | Genera `build/dist/KairosApp` con PyInstaller |
| `make install-deps` | Crea il venv e installa `ply` e `pyinstaller` |
| `make clean` | Rimuove `.so`, artefatti PyInstaller e cache Python |
| `make help` | Mostra il riepilogo dei comandi |


---

### Pacchetti Linux

La toolchain packaging vive in `packaging/linux`.

### Build pacchetto Debian/Ubuntu 
(testato su Ubuntu)

```bash
cd packaging/linux
./build-deb.sh
```

Lo script esegue automaticamente:

- `make release`
- `make build-dap`
- bump versione patch automatico da `packaging/linux/VERSION`

Output: `packaging/linux/kairosapp_<versione>_<arch>.deb`

Installazione:

```bash
sudo dpkg -i ./packaging/linux/kairosapp_<versione>_amd64.deb
```

Disinstallazione:

```bash
sudo dpkg -r kairosapp
sudo dpkg -P kairosapp   # purge completa
```

Path installati dal pacchetto:

- `/usr/local/bin/kairosapp`
- `/opt/kairosapp/KairosApp`
- `/usr/local/lib/kairosapp/dap.so`

### Altre distro

- RPM (Fedora/RHEL): `./packaging/linux/build-rpm.sh` (richiede `rpmbuild`)
- Arch: `./packaging/linux/build-arch.sh` (richiede `makepkg`)

Per dettagli completi vedi `packaging/linux/README.md`.

---

### Integrazione VS Code

L'estensione è nel repository separato https://github.com/nicologiuliani6/kairos-vscode-debugger.

Con installazione `.deb`, i default runtime da usare sono:

- `kairos.appPath`: `/usr/local/bin/kairosapp`
- `kairos.libPath`: `/usr/local/lib/kairosapp/dap.so`

Workflow rapido estensione:

```bash
git clone https://github.com/nicologiuliani6/kairos-vscode-debugger.git
cd kairos-vscode-debugger
make reinstall
```

Poi in VS Code: `Developer: Reload Window`.

Se serve override per progetto, usa `launch.json` con:

- `kairosApp`
- `kairosLib`

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
[ Janus.c + Janus_dap.c / libvm.so ] ── VM in C che interpreta il bytecode
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

**Buffer del canale (VM).** A differenza degli stack Kairos, che restano **LIFO** (`push`/`pop`), i valori in transito su un canale sono accodati in ordine di invio e consumati in **FIFO** (primo inviato, primo ricevuto). Con più `ssend` concorrenti sullo stesso canale, l’append e il prelievo dal buffer interno sono serializzati con il mutex del canale, così ogni `srecv` abbinato al rendez-vous legge il messaggio coerente con l’ordine di accodamento. Senza questa disciplina, combinazioni tipo un solo thread che riceve in loop mentre più thread inviano potevano produrre valori errati e fallire le `delocal`.

I canali sono pensati per essere usati esclusivamente all'interno di blocchi `par/rap`.

---

<a id="parallelismo-par-and-rap"></a>

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

`par/rap` avvia i thread elencati in **parallelo reale** (un `pthread` per branch: tutti partono insieme). I thread condividono le variabili del frame corrente. La sincronizzazione logica tra thread resta basata sui **canali**; per gli `int` condivisi valgono in più i controlli descritti sotto. Per ogni `call` da un thread, la VM usa una chiave frame del tipo `nomeProc@t<thread_id>` (lunghezza massima 32 caratteri nella mappa interna): evita nomi di procedura troppo lunghi se la procedura è invocata solo da branch `par`.

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

<a id="analisi-statica-par"></a>

#### Analisi statica e lock a runtime nei PAR

**Compilatore.** Su ogni `par` il frontend applica i controlli descritti in [Controlli statici del compilatore (`parser.py`)](#controlli-statici-del-compilatore-parserpy): in sintesi, **vietati** stack condivisi tra branch e **race** tra scrittura e accesso sullo stesso `int` (anche se la scrittura avviene **dentro una procedura** chiamata dal branch). I **canali** possono invece essere usati dallo stesso nome in più branch (handshake `ssend`/`srecv`).

**VM (`vm_ref_lock.h`, operazioni su `Var`).** Nei worker PAR (`current_thread_args` attivo), ogni mutazione su una variabile `int` (`PUSHEQ`/`MINEQ`/`XOREQ`, `SWAP`, parte di `PUSH`/`POP` che azzera o somma un int) acquisisce un lock **re-entrante** sullo stesso `pthread`: la ricorsione sullo stesso thread non blocca; due thread che modificano la stessa cella int in conflitto ottengono:

```text
[VM] mutazione concorrente sulla variabile int 'x' da un altro thread
```

I parametri non vengono bloccati all’ingresso della `call` (evita falsi positivi quando lo stesso `int` è solo letto da più procedure in parallelo).

**Nota.** L’interleaving tra thread può restare non deterministico dove il programma non fissa un ordine (es. `push` concorrenti sullo stesso `stack`). Sui **canali**, i valori in transito sono ordinati in FIFO rispetto agli invii (vedi sopra). Per escludere file dalla suite `make test`, usare `KAIROS_EXCLUDE` nel `makefile`.

---
### show

Per definizione, l’I/O non è compatibile con la reversibilità.  
In Kairos, le operazioni di output tramite `show(var)` sono trattate come un’astrazione esterna al modello reversibile, pensata esclusivamente come supporto allo sviluppo e al debugging.

`show(var)` stampa il valore corrente di una variabile su `stdout`.
```
show(x)        // stampa: x: 42
show(result)   // stampa: result: [1, 2, 3, 4, 5]
```
Al termine dell’esecuzione, la VM produce sempre anche un dump finale dello stato:

```text
=== VM dump ===
x: 0
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

<a id="controlli-statici-del-compilatore-parserpy"></a>

### Controlli statici del compilatore (`parser.py`)

Oltre ai warning di reversibilità su `if-fi` e `local`/`delocal` (variabili di controllo / sorgenti modificate), il frontend rifiuta il programma con prefisso **`[STATIC]`** nei casi seguenti.

#### Assegnamenti `+=`, `-=`, `^=`

La variabile a sinistra **non può comparire** nell’espressione a destra (`x += x` è rifiutato). Messaggio: *operazione non reversibile … (la variabile a sinistra compare anche nell'espressione a destra)*. Esempi: `test_error/03_static_x_minus_eq_x.kairos`, `04_static_x_plus_eq_expr_contains_x.kairos`, `05_static_x_xor_eq_x.kairos`.

#### `delocal` e valore atteso

In `delocal tipo nome = valore`, se `valore` è un identificatore **uguale** a `nome` (`delocal int x = x`), la compilazione fallisce: l’inversione non può fissare un valore atteso non banale. Messaggio: *DELOCAL non ammesso … Usa un letterale o un altro nome di variabile.* Esempio: `test_error/10_static_delocal_self_ref.kairos`.

#### Blocco `par`: stesso `stack` in più branch

Per ogni branch viene raccolto l’insieme degli usi “strutturali” dello stack:

- secondo argomento di `push`/`pop` in sintassi diretta (`push(a, s)`);
- ogni argomento attuale di tipo `stack` in chiamate a procedure, sia con `call nome(...)` / `uncall nome(...)`, sia con **sintassi diretta** `nome(...)` (equivalente alla `call`).

Se lo **stesso** nome di variabile dichiarato `stack` compare in quella raccolta per **due branch distinti** dello stesso `par`, la compilazione fallisce. I **canali** non sono trattati come stack: passare lo stesso `channel` a più branch per sincronizzarli è consentito dal controllo statico (resta necessario un protocollo corretto di `ssend`/`srecv`). Messaggio: *uso non reversibile di stack condiviso … in blocco PAR*.

Esempio in `test_error/12_shared_stack_par_calls.kairos`.

#### Blocco `par`: race su `int` tra branch

Per ogni branch si calcolano:

- **Accessi** (`int`): tutti gli identificatori usati nel branch (assegnamenti, espressioni, argomenti di chiamate, condizioni, `show`, `push`/`pop`, `local`/`delocal`, ecc.), ristretti ai nomi dichiarati `int` nel frame corrente.
- **Scritture** (`int`): oltre alle scritture **dirette** (`+=`/`-=`/`^=` su un `int`, `x <=> y` con tipi `int`, `local`/`delocal` su `int`), anche gli **`int` passati come argomenti** a procedure che **mutano** quel parametro (analisi per punto fisso sul grafo delle chiamate: assegnamento al parametro nel corpo della procedura, propagazione attraverso `call` / `uncall` / chiamata diretta, con eccezione documentata per `swap`).

Per ogni coppia di branch distinti \(i, j\), se esiste un nome `int` che è **scritto** in un branch e **acceduto** nell’altro (in entrambe le direzioni: \(W_i \cap A_j\) o \(W_j \cap A_i\)), la compilazione fallisce. Messaggio: *race su int nel PAR (scrittura vs accesso, anche tramite call)*.

Così viene rifiutato sia il caso di due `x += …` in parallelo (`test_error/09_shared_int_par.kairos`), sia il caso in cui due branch chiamano procedure diverse su **`foo(x)`** e **`bar(x)`** con parametri `int` mutati nel callee (`test_error/11_shared_int_par_calls.kairos`). Restano ammessi scenari in cui lo stesso `int` è solo letto o passato a callees che **non** mutano quel parametro (es. limiti condivisi in stile producer/consumer).

#### Sintassi delle chiamate dirette

Nella grammatica attuale, le liste di argomenti delle chiamate `nome(...)` contengono solo **identificatori**, non letterali numerici (`push(1, s)` va scritto con una variabile `int` intermedia, come negli esempi in `tests/`).

---

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

### 5. PAR: stack condivisi e race su `int`

Nel blocco `par` il compilatore vieta **stack** usati in più branch e **race** su `int` (scrittura vs accesso, **anche tramite** chiamate a procedure che mutano parametri). I **canali** condivisi non attivano questi errori statici. Dettaglio formale: [Controlli statici del compilatore (`parser.py`)](#controlli-statici-del-compilatore-parserpy) e [Analisi statica e lock a runtime nei PAR](#analisi-statica-par).

### 6. Stack e channel devono essere vuoti alla delocal

```kairos
delocal stack s = nil     // s deve essere vuoto
delocal channel ch = empty // ch deve essere vuoto
```

---

## Il bytecode Kairos

Il compilatore produce un bytecode testuale che la VM interpreta. Il formato e':

```
@SRC   ISTRUZIONE [argomenti...]
```

dove:
- `@SRC` è la riga del sorgente Kairos da cui l'istruzione proviene (usata anche dal debugger DAP).


Le istruzioni principali:

| Istruzione | Descrizione |
|-----------|-------------|
| `START` | Inizio programma (emesso dal compilatore; in `vm_exec` reinizializza l’indicizzatore dei frame, in esecuzione normale è ignorato) |
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
- con `--dap` il frontend non esegue direttamente la VM: restituisce il bytecode via stdout all'adapter DAP (nessun file temporaneo su disco).
- in debug DAP, `Step Back` inverte una sola istruzione; `Reverse Continue` inverte finché trova il primo breakpoint a ritroso, oppure si ferma all'inizio del programma se non ci sono breakpoint precedenti.

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

### `[STATIC] race su int nel PAR`

Due branch dello stesso `par` hanno un conflitto **scrittura vs accesso** sullo stesso `int`: la scrittura può essere **diretta** nel branch o **indiretta** (es. `foo(x)` con `foo` che muta il parametro `int`). Usa variabili distinte per branch o comunica con `channel` / `ssend` / `srecv`.

### `[STATIC] stack condiviso nel PAR`

Lo stesso `stack` è usato (tramite `push`/`pop` o passaggio a procedure) da più branch dello stesso `par`. Usa stack separati o sposta i dati su canali.

### `[STATIC] DELOCAL … valore atteso = stesso nome`

`delocal int x = x` (o analogo) è rifiutato: il valore atteso non può coincidere con l’identificatore che stai chiudendo.

### `[VM] mutazione concorrente sulla variabile int`

Due thread PAR hanno tentato di modificare la stessa variabile `int` in un intervallo non protetto dal modello (il lock a runtime intercetta il conflitto). Rivedi il programma o la suddivisione tra branch.

### Warning di reversibilità

```
[WARNING] La procedura "f" dentro un blocco if-fi ha la variabile di controllo
          'x' modificata da istruzione: PUSHEQ (riga 5)
```

Il checker statico ha trovato una potenziale violazione. Il programma continua a essere eseguito ma potrebbe non essere correttamente invertibile.