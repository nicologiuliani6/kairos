# Kairos

Kairos è un linguaggio **reversibile e concorrente** nella famiglia di [Janus](https://en.wikipedia.org/wiki/Janus_%28time-reversible_computing_programming_language%29). Ogni programma si può eseguire in avanti e all'indietro: ogni comando ha un inverso definito, e `uncall` esegue una procedura al contrario. A Janus aggiunge il parallelismo esplicito (`par … and … rap`), i canali sincroni tipizzati con passaggio di canali (stile π-calcolo), gli stack, gli array e il costrutto `try … rollback … yrt`.

Il frontend è in Python (PLY) e produce un bytecode testuale; la VM è in C e lo interpreta, sia in avanti sia all'indietro.

```kairos
procedure fib(int n, int a, int b)
    if n == 0 then
        a += 1
        b += 1
    else
        n -= 1
        call fib(n, a, b)
        a += b
        a <=> b
    fi a == b

procedure main()
    local int n = 10
    local int a = 0
    local int b = 0
    call fib(n, a, b)
    show(b)
    uncall fib(n, a, b)
    delocal int b = 0
    delocal int a = 0
    delocal int n = 10
```

---

## Indice

1. [Avvio rapido](#avvio-rapido)
2. [Comandi make](#comandi-make)
3. [Struttura del repository](#struttura-del-repository)
4. [Architettura](#architettura)
5. [Il linguaggio](#il-linguaggio)
6. [Reversibilità: regole e controlli](#reversibilità-regole-e-controlli)
7. [Il bytecode](#il-bytecode)
8. [Errori comuni](#errori-comuni)
9. [Pacchetti, app e VS Code](#pacchetti-app-e-vs-code)

---

## Avvio rapido

Requisiti: `gcc`, `make`, Python ≥ 3.10 con il modulo `venv` (su Debian/Ubuntu: `sudo apt install build-essential python3 python3-venv`).

```bash
git clone https://github.com/nicologiuliani6/kairos.git
cd kairos
make install-deps                     # venv + ply (+ pyinstaller)
make                                  # compila build/libvm.so
make run FILE=examples/fib.kairos     # esegue un programma
```

Oppure direttamente:

```bash
./venv/bin/python -m src.kairos examples/fib.kairos [--dump-bytecode] [--vm-stats]
```

`--dump-bytecode` scrive il bytecode in `bytecode.txt`; `--vm-stats` (o `KAIROS_VM_STATS=1`) stampa a fine esecuzione quante celle sono rimaste vive. Dopo aver modificato i sorgenti C della VM serve `make build-release`, altrimenti resta in uso il `libvm.so` precedente (il frontend lo segnala su stderr).

---

## Comandi make

| Comando | Effetto |
|---|---|
| `make` / `make build-release` | `build/libvm.so` ottimizzata (`-O3 -march=native -flto`, non redistribuibile) |
| `make build` | `libvm.so` portabile (`-O2`) |
| `make build-dap` | `build/libvm_dap.so` per il debugger |
| `make run FILE=f.kairos` | esegue un programma con `--dump-bytecode` |
| `make test` | esegue `tests/`, `examples/` e `lossless/` (timeout 5 s ciascuno; `KAIROS_EXCLUDE` nel makefile per escludere file) |
| `make release-app` | app standalone `build/dist/KairosApp` con PyInstaller |
| `make install-deps` / `make check-system` | venv e dipendenze / controllo dei requisiti |
| `make clean` | rimuove gli artefatti |

I programmi che devono fallire (errori statici e di runtime attesi) sono in `test_error/`, uno per regola.

---

## Struttura del repository

```
src/
  kairos.py            entry point: compila ed esegue
  frontend/
    lexer.py, parser.py   analisi lessicale e sintattica (PLY) + controlli statici
    sessions.py           protocolli dei canali dentro i par
    inverse.py            corpo inverso compilato delle procedure ricorsive
    bytecode.py           AST → bytecode
  vm/
    Janus.c               esecuzione in avanti (vm_run_BT), prima passata, dump
    vm_invert.h           esecuzione inversa (uncall)
    vm_ops.h, ops_arith.h istruzioni e loro inversi
    vm_par.h, vm_channel.h, vm_session.h, vm_ref_lock.h   par, canali, sessioni, lock
    vm_frames.h           frame clonati (ricorsione, thread)
    Janus_dap.c           API per il debugger (step, step back, breakpoint)
examples/   esempi (fib, producer/consumer, π-calcolo, try, SAT, janus1982/ …)
lossless/   compressione reversibile (BWT) e misure
tests/      programmi che devono girare senza errori
test_error/ programmi che devono essere rifiutati
packaging/  pacchetti .deb / .rpm / Arch
```

---

## Architettura

```
file.kairos ─▶ lexer ─▶ parser (+ controlli statici, desugar di try)
            ─▶ inverse.py (p__inv per le procedure ricorsive)
            ─▶ bytecode.py ─▶ stringa bytecode ─▶ libvm.so (ctypes)
                                                   ├─ vm_exec        prima passata: frame, DECL, PARAM, LABEL
                                                   ├─ vm_run_BT      esecuzione in avanti
                                                   └─ invert_op_to_line   esecuzione inversa
```

Il bytecode passa alla VM in memoria, senza file intermedi. A fine esecuzione la VM stampa il dump delle variabili di `main` (`=== VM dump ===`).

---

## Il linguaggio

### Tipi

| Tipo | Valore iniziale | Note |
|---|---|---|
| `int` | `0` | intero con segno a 64 bit |
| `int a[n]` | `n` celle a `m` | lunghezza costante fissata alla dichiarazione |
| `stack` | `nil` | lista LIFO di interi |
| `channel` | `empty` | canale sincrono (rendez-vous) |

### Dichiarazioni, `local` e `delocal`

In `main` una dichiarazione senza `local` (`int x`, `stack s`, `channel c`) crea una variabile del frame, inizialmente vuota. Altrove si usa la coppia `local`/`delocal`:

```kairos
local int x = 0
local int y = x          // y parte dal valore corrente di x
local stack s = nil
local channel c = empty
...
delocal channel c = empty
delocal stack s = nil    // s deve essere vuoto
delocal int y = x        // y deve valere ancora x
delocal int x = 0
```

`delocal` verifica a runtime il valore dichiarato (altrimenti errore) e le chiusure seguono l'ordine **LIFO** delle aperture. L'inverso di `local` è `delocal` e viceversa.

### Assegnamenti reversibili

| Comando | Inverso |
|---|---|
| `x += e` | `x -= e` |
| `x -= e` | `x += e` |
| `x ^= e` | se stesso |
| `x <=> y` | se stesso |

La variabile (o l'array) a sinistra **non può comparire** in `e`: `x += x` perderebbe l'informazione ed è rifiutato.

### Espressioni

`+ - * / %` (divisione troncata verso zero e resto col segno del dividendo, come in C), confronti `== != < > <= >=` che valgono 1 o 0, connettivi `&& ||`, parentesi e celle di array. Una guardia è vera se diversa da zero. `* / %` esistono solo nelle espressioni; dividere per zero è un errore (`Div-Err`).

```kairos
x += ((a * b) % c)
x += v[(i + 1)]
if (a == 0) && (b > 1) then
    r += 1
fi r == 1
```

### Array

```kairos
procedure riempi(int a[], int n)
    local int i = 0
    from i == 0 do
        a[i] += (i * 2)
        i += 1
    loop until i == n
    delocal int i = n

procedure main()
    local int v[4] = 0
    local int n = 4
    call riempi(v, n)
    v[0] <=> v[3]
    show(v)
    v[0] <=> v[3]
    uncall riempi(v, n)
    delocal int n = 4
    delocal int v[4] = 0     // tutte le celle devono valere 0
```

- Una cella `a[i]` (indice = espressione) è un luogo come una variabile: `+= -= ^= <=>`.
- Un indice fuori dai limiti è un errore (`Idx-Err`).
- Gli array si passano alle procedure per riferimento (`int a[]`); non viaggiano sui canali.
- Dove la sintassi vuole un identificatore (`push`, `show`, argomenti di `call`, payload dei canali) serve una variabile, non una cella.

### Procedure, `call` e `uncall`

I parametri sono passati **per riferimento**. Ogni programma ha una `procedure main()` senza parametri.

```kairos
procedure increment(int x)
    x += 5

procedure main()
    local int a = 0
    call increment(a)     // a = 5
    uncall increment(a)   // a torna 0
    delocal int a = 0
```

`uncall p` esegue `p` al contrario: comandi in ordine inverso, ciascuno sostituito dal suo inverso. Per le procedure **ricorsive** (anche mutuamente) il frontend compila il corpo inverso `p__inv` per induzione sulla sintassi (`inverse.py`): `uncall p` diventa una normale chiamata in avanti a `p__inv`, e così fa la VM quando inverte una `call p` dentro un'altra inversione.

### `if … fi`

```kairos
if <guardia d'ingresso> then
    ...
else
    ...
fi <guardia d'uscita>
```

La guardia d'uscita, valutata dopo il ramo, deve essere vera dopo il `then` e falsa dopo l'`else`: è ciò che permette all'inverso di sapere quale ramo era stato preso. La VM lo verifica (`IF/FI non reversibile`).

### `from … do … loop … until`

```kairos
from <b1> do
    c1
loop
    c2
until <b2>
```

Traccia `c1 [c2 c1]*`: `b1` deve essere vera all'ingresso e falsa alle iterazioni successive; `b2` è valutata dopo ogni `c1`. L'inverso è `from b2 do I(c1) loop I(c2) until b1`.

```kairos
local int i = 0
from i == 0 do
    i += 1
loop until i == n
delocal int i = n
```

### Clausole facoltative e `skip`

Come in Janus, `then`, `else`, `do` e `loop` si possono omettere (valgono `skip`); `skip` è il comando vuoto. Le guardie `fi` e `until` sono obbligatorie.

### `try … rollback … yrt`

```kairos
try x == 7          // condizione di commit, valutata dopo il body
    x += 5
rollback            // facoltativo
    x += 99
yrt x == 7
```

Se dopo il body la condizione è vera il body resta; altrimenti il body viene annullato (eseguito al contrario) e si esegue il rollback. È zucchero sintattico: body e rollback diventano procedure `__try_body_N` / `__try_rb_N` sulle variabili libere, e il blocco un `call` seguito da un `if` con `uncall`. Niente `par` né canali dentro un `try`.

### Stack

```kairos
push(x, s)   // x in cima a s, x azzerato
pop(x, s)    // cima di s in x; x deve valere 0 (Pop-Err2)
```

Uno è l'inverso dell'altro. Un `pop` da uno stack vuoto è un errore.

### Canali

```kairos
ssend(<v1, v2, …>, c)   // invia, azzerando/svuotando le sorgenti
srecv(<d1, d2, …>, c)   // riceve; le destinazioni int devono valere 0 (Srecv-Err)
```

Rendez-vous sincrono: `ssend` attende un `srecv` e viceversa; i valori in transito sono FIFO. Il payload può contenere `int`, `stack` (svuotato e concatenato) e `channel` (passaggio dell'endpoint, come nel π-calcolo). `ssend` e `srecv` sono l'uno l'inverso dell'altro. Dentro un `par` ogni canale è una **sessione binaria**: esattamente due rami lo usano, con protocolli complementari (controllo statico in `sessions.py` e dinamico nella VM).

### `par … and … rap`

```kairos
par
    ssend(<x>, c)
and
    srecv(<y>, c)
rap
```

Un `pthread` per ramo, tutti avviati insieme; il blocco termina quando terminano tutti. I `par` si possono annidare. L'inverso esegue l'inverso di ogni ramo (scambiando `ssend`/`srecv` e `call`/`uncall`).

I rami condividono il frame ma non la memoria scrivibile: il frontend rifiuta stack usati da più rami e ogni `int` o array **scritto** in un ramo e **usato** in un altro, anche indirettamente (scritture fatte da procedure chiamate, analisi a punto fisso sul grafo delle chiamate; `pop`/`srecv` contano come scritture). Un `int` letto da più rami è ammesso. A runtime, mutazioni concorrenti della stessa cella vengono comunque intercettate (`vm_ref_lock.h`).

### `show`

L'output non fa parte del modello reversibile: è un supporto allo sviluppo, ignorato quando si esegue all'indietro.

```kairos
show(x)          // "x: 42"  (int, stack, channel, array)
show(x, char)    // un solo carattere: il byte basso di x, senza newline
```

### Commenti

`// fino a fine riga`. Non ci sono commenti multiriga.

---

## Reversibilità: regole e controlli

Il frontend rifiuta con `[STATIC]`:

- `x += e`, `x -= e`, `x ^= e` con `x` in `e` (anche per gli array);
- `delocal int x = x`;
- in un `par`: lo stesso `stack` in più rami; un `int`/array scritto in un ramo e usato in un altro; una `call` a un nome che non è né una procedura né una builtin (non se ne conoscerebbero le scritture); canali usati da un solo ramo, da più di due o con protocolli incompatibili.

Emette un warning (il programma gira, ma l'inverso può fallire) quando una guardia di `if` è modificata nel suo corpo o quando la sorgente di un `local int y = x` cambia prima del `delocal`.

A runtime la VM verifica: valore e ordine LIFO dei `delocal`, la guardia d'uscita di `if`, `from`/`until`, `pop` e `srecv` su destinazioni non nulle, stack vuoti, indici e divisioni, sessioni dei canali. Ogni regola ha un esempio in `test_error/`.

---

## Il bytecode

Una riga per istruzione, `NNNN @SRC OPCODE argomenti`, dove `@SRC` è la riga sorgente (usata dal debugger).

| Istruzione | Significato |
|---|---|
| `START` / `HALT` | inizio / fine programma |
| `PROC p` / `END_PROC p` | procedura |
| `PARAM tipo nome`, `DECL tipo nome` | parametro, variabile di frame |
| `LOCAL tipo nome val` / `DELOCAL tipo nome val` | apertura / chiusura con verifica |
| `PUSHEQ v e` / `MINEQ v e` / `XOREQ v e` / `SWAP a b` | assegnamenti |
| `PUSH v s` / `POP v s` | stack |
| `SSEND … c` / `SRECV … c` | canali |
| `EVAL l op r`, `JMPF lbl`, `JMP lbl`, `LABEL lbl`, `ASSERT l op r` | controllo |
| `CALL p args` / `UNCALL p args` | chiamata in avanti / inversa |
| `SHOW v` / `SHOW v char` | output |
| `PAR_START`, `THREAD_N`, `PAR_END` | blocco parallelo |

---

## Errori comuni

| Messaggio | Causa |
|---|---|
| `DELOCAL: valore finale errato` | la variabile non vale quanto dichiarato nella `delocal` |
| `DELOCAL: ordine errato` | `local`/`delocal` non annidati LIFO |
| `DELOCAL: … non è nil/empty` | stack o canale non vuoto alla chiusura |
| `IF/FI non reversibile` | la guardia d'uscita non corrisponde al ramo eseguito |
| `POP: destinazione … non è zero (Pop-Err2)` | `pop` su una variabile non nulla |
| `Idx-Err` / `Div-Err` | indice fuori dai limiti / divisione per zero |
| `[STATIC] race su int nel PAR` | scrittura in un ramo, accesso in un altro: usa variabili distinte o un canale |
| `[VM] SESSIONE: …` | canale usato da più di due rami o protocollo bloccato |
| `cannot open shared object file: libvm.so` | manca `make build-release` |

---

## Pacchetti, app e VS Code

- **Pacchetti Linux**: `packaging/linux/build-deb.sh` (Debian/Ubuntu), `build-rpm.sh`, `build-arch.sh`; dettagli in `packaging/linux/README.md`. Il `.deb` installa `/usr/local/bin/kairosapp`, `/opt/kairosapp/KairosApp` e `/usr/local/lib/kairosapp/dap.so`.
- **App standalone**: `make release-app`.
- **Debugger VS Code**: estensione in [kairos-vscode-debugger](https://github.com/nicologiuliani6/kairos-vscode-debugger), con step avanti e indietro (`Step Back`, `Reverse Continue`). Con il pacchetto installato: `kairos.appPath = /usr/local/bin/kairosapp`, `kairos.libPath = /usr/local/lib/kairosapp/dap.so`.
