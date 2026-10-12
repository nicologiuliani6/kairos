# Kairos

Kairos is a **reversible, concurrent** programming language in the [Janus](https://en.wikipedia.org/wiki/Janus_%28time-reversible_computing_programming_language%29) family. Every program runs forwards and backwards: each command has a well-defined inverse, and `uncall` runs a procedure in reverse. Kairos follows Janus one-to-one on the reversible core (assignments, `if … fi`, `from … until`, `local … delocal`, `call`/`uncall`) and adds explicit parallelism (`par … and … rap`), synchronous channels with inferred protocols and channel passing (π-calculus style), stacks, arrays and a `try … rollback … yrt` construct.

The frontend is written in Python (PLY) and produces a textual bytecode; the VM is written in C and interprets it both forwards and backwards.

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

## Contents

1. [Quick start](#quick-start)
2. [Linux packages](#linux-packages)
3. [Make targets](#make-targets)
4. [Repository layout](#repository-layout)
5. [Architecture](#architecture)
6. [The language](#the-language)
7. [Reversibility: rules and checks](#reversibility-rules-and-checks)
8. [The bytecode](#the-bytecode)
9. [Common errors](#common-errors)
10. [Debugger and VS Code](#debugger-and-vs-code)

---

## Quick start

Requirements: `gcc`, `make`, Python ≥ 3.10 with the `venv` module (Debian/Ubuntu: `sudo apt install build-essential python3 python3-venv`).

```bash
git clone https://github.com/nicologiuliani6/kairos.git
cd kairos
make install-deps                     # venv + ply (+ pyinstaller)
make                                  # builds build/libvm.so
make run FILE=examples/fib.kairos     # runs a program
```

Or directly:

```bash
./venv/bin/python -m src.kairos examples/fib.kairos [--dump-bytecode] [--vm-stats]
```

`--dump-bytecode` writes the bytecode to `bytecode.txt`; `--vm-stats` (or `KAIROS_VM_STATS=1`) prints how many cells are still alive at the end. After editing the VM's C sources run `make build-release` again, otherwise the old `libvm.so` stays in use (the frontend warns about it on stderr).

---

## Linux packages

Every [release](https://github.com/nicologiuliani6/kairos/releases) ships ready-made packages of **KairosApp** (standalone frontend + VM, no Python needed) built by the `release` workflow:

| Distribution | File | Install |
|---|---|---|
| Debian / Ubuntu | `kairosapp_<version>_amd64.deb` | `sudo apt install ./kairosapp_<version>_amd64.deb` |
| Fedora / RHEL | `kairosapp-<version>-1.<dist>.x86_64.rpm` | `sudo dnf install ./kairosapp-<version>-1.<dist>.x86_64.rpm` |
| Arch Linux | `kairosapp-<version>-1-x86_64.pkg.tar.zst` | `sudo pacman -U ./kairosapp-<version>-1-x86_64.pkg.tar.zst` |

The packages install `/usr/local/bin/kairosapp`, `/opt/kairosapp/KairosApp` and the debugger library `/usr/local/lib/kairosapp/dap.so`. Then:

```bash
kairosapp program.kairos
```

To build a package locally use `packaging/linux/build-deb.sh`, `build-rpm.sh` or `build-arch.sh` (details in `packaging/linux/README.md`). Pushing a tag `vX.Y.Z` builds all three and attaches them to the release.

---

## Make targets

| Target | Effect |
|---|---|
| `make` / `make build-release` | optimised `build/libvm.so` (`-O3 -march=native -flto`, not redistributable) |
| `make build` | portable `libvm.so` (`-O2`) |
| `make build-dap` | `build/libvm_dap.so` for the debugger |
| `make run FILE=f.kairos` | runs a program with `--dump-bytecode` |
| `make test` | runs `tests/`, `examples/` and `lossless/` (5 s timeout each; `KAIROS_EXCLUDE` in the makefile excludes files) |
| `make test-error` | every program in `test_error/` must stop with its tagged error (`*_ok.kairos` must run) |
| `make test-dap` | debugger smoke test (step, step back, streamed output) |
| `make release-app` | standalone app `build/dist/KairosApp` (PyInstaller) |
| `make install-deps` / `make check-system` | venv and dependencies / system requirements check |
| `make clean` | removes build artefacts |

Programs that must be rejected (expected static and runtime errors) live in `test_error/`, one per rule.

---

## Repository layout

```
src/
  kairos.py            entry point: compile and run
  frontend/
    lexer.py, parser.py   lexing and parsing (PLY) + static checks + try desugaring
    sessions.py           channel protocols inside par blocks
    inverse.py            compiled inverse body of recursive procedures
    bytecode.py           AST → bytecode
  vm/
    Janus.c               forward execution (vm_run_BT), first pass, dump
    vm_invert.h           backward execution (uncall)
    vm_ops.h, ops_arith.h instructions and their inverses
    vm_par.h, vm_channel.h, vm_session.h, vm_ref_lock.h   par, channels, sessions, locks
    vm_frames.h           cloned frames (recursion, threads)
    Janus_dap.c           debugger API (step, step back, breakpoints)
examples/   examples (fib, producer/consumer, π-calculus, try, SAT, janus1982/ …)
lossless/   reversible compression (BWT) and measurements
tests/      programs that must run without errors
test_error/ programs that must be rejected
packaging/  .deb / .rpm / Arch packages
```

---

## Architecture

```
file.kairos ─▶ lexer ─▶ parser (+ static checks, try desugaring)
            ─▶ inverse.py (p__inv for recursive procedures)
            ─▶ bytecode.py ─▶ bytecode string ─▶ libvm.so (ctypes)
                                                  ├─ vm_exec            first pass: frames, DECL, PARAM, LABEL
                                                  ├─ vm_run_BT          forward execution
                                                  └─ invert_op_to_line  backward execution
```

The bytecode reaches the VM in memory, with no intermediate file, and there is **a single copy** of it: every thread of a `par` reads the same read-only string, copying only the line it is executing. At the end the VM prints the variables of `main` (`=== VM dump ===`).

---

## The language

### Types

| Type | Initial value | Notes |
|---|---|---|
| `int` | `0` | signed 64-bit integer, two's complement: arithmetic wraps modulo 2^64 |
| `int a[n]` | `n` cells set to `m` | constant length fixed at declaration |
| `stack` | `nil` | LIFO list of integers |
| `channel` | `empty` | synchronous (rendez-vous) channel |

### Declarations, `local` and `delocal`

In `main` a declaration without `local` (`int x`, `stack s`, `channel c`) creates a frame variable, initially empty. Elsewhere (the compiler enforces it) use `local`/`delocal`:

```kairos
local int x = 0
local int y = x          // y starts from the current value of x (ints only)
local stack s = nil
local channel c = empty
...
delocal channel c = empty
delocal stack s = nil    // s must be empty
delocal int y = x        // y must still equal x
delocal int x = 0
```

As in Janus, `local x … delocal x` is a **block**: each `delocal` closes the most recent open `local` *of the same statement list*, and no `local` may stay open at the end of the list (a `then` branch, a loop body, a `par` branch, a procedure body). A `local` may not redeclare a parameter or a still-open local of the same activation (Local-Err). The frontend rejects anything else. At run time `delocal` checks the declared value. The inverse of `local` is `delocal` and vice versa.

### Reversible assignments

| Command | Inverse |
|---|---|
| `x += e` | `x -= e` |
| `x -= e` | `x += e` |
| `x ^= e` | itself |
| `x <=> y` | itself |

The variable (or array) on the left **must not occur** in `e`, nor in the index of the cell being written: `x += x` and `a[a[0]] += 1` would lose information and are rejected.

### Expressions

`+ - * / %` (division truncates towards zero and the remainder takes the sign of the dividend, as in C), comparisons `== != < > <= >=` that evaluate to 1 or 0, connectives `&& ||`, parentheses and array cells. A guard is true when non-zero. `* / %` only exist inside expressions; division by zero is an error (`Div-Err`). `&&` and `||` do not short-circuit: both operands are always evaluated. Reading a variable that is not declared is a compile error.

```kairos
x += ((a * b) % c)
x += v[(i + 1)]
if (a == 0) && (b > 1) then
    r += 1
fi r == 1
```

### Arrays

```kairos
procedure fill(int a[], int n)
    local int i = 0
    from i == 0 do
        a[i] += (i * 2)
        i += 1
    loop until i == n
    delocal int i = n

procedure main()
    local int v[4] = 0
    local int n = 4
    call fill(v, n)
    v[0] <=> v[3]
    show(v)
    v[0] <=> v[3]
    uncall fill(v, n)
    delocal int n = 4
    delocal int v[4] = 0     // every cell must be 0
```

- A cell `a[i]` (index = expression) is a location like a variable: `+= -= ^= <=>`.
- An out-of-bounds index is an error (`Idx-Err`).
- Arrays are passed to procedures by reference (`int a[]`); they cannot travel on channels.
- Where the syntax expects an identifier (`push`, `show`, `call` arguments, channel payloads) a variable is needed, not a cell.

### Procedures, `call` and `uncall`

Parameters are passed **by reference**, so, as in Janus, the arguments of one call must be distinct variables and match the parameters in number. `f(x)` without `call` is the same as `call f(x)`. Every program has a `procedure main()` with no parameters; it runs once all procedures are registered, so the order of procedures in the file does not matter.

```kairos
procedure increment(int x)
    x += 5

procedure main()
    local int a = 0
    call increment(a)     // a = 5
    uncall increment(a)   // a back to 0
    delocal int a = 0
```

`uncall p` runs `p` backwards: commands in reverse order, each replaced by its inverse. For **recursive** procedures (mutual recursion included) the frontend compiles the inverse body `p__inv` by induction on the syntax (`inverse.py`): `uncall p` becomes an ordinary forward call to `p__inv`, and the VM does the same when it inverts a `call p` inside another inversion.

### `if … fi`

```kairos
if <entry guard> then
    ...
else
    ...
fi <exit assertion>
```

As in Janus, the exit assertion, evaluated after the branch, must be true after `then` and false after `else`; it is what tells the inverse which branch was taken. The VM checks it on every `if`, forwards (`IF/FI non reversibile`) and backwards (the entry guard must agree with the branch chosen by the assertion).

### `from … do … loop … until`

```kairos
from <b1> do
    c1
loop
    c2
until <b2>
```

Trace `c1 [c2 c1]*`. As in Janus, `b1` must be true on entry (**LoopError1** otherwise) and false every time the loop is re-entered after `c2` (**LoopError2**); `b2` is evaluated after each `c1`. The inverse is `from b2 do I(c1) loop I(c2) until b1`, with the same checks on `b2`.

```kairos
local int i = 0
from i == 0 do
    i += 1
loop until i == n
delocal int i = n
```

### Optional clauses and `skip`

As in Janus, `then`, `else`, `do` and `loop` may be omitted (they mean `skip`); `skip` is the empty command. The `fi` and `until` conditions are mandatory.

### `try … rollback … yrt`

```kairos
try x == 7          // commit condition, evaluated after the body
    x += 5
rollback            // optional
    x += 99
yrt x == 7          // exit assertion
```

If the condition holds after the body, the body is kept; otherwise the body is undone (run backwards) and the rollback runs. It is syntactic sugar: body and rollback become procedures `__try_body_N` / `__try_rb_N` over their free variables, and the block becomes a `call` followed by an ordinary `if` whose `else` does the `uncall`. The `yrt` condition is that `if`'s exit assertion, so the Janus rule applies: it must be true after a commit and false after a rollback (see `examples/try_backtracking.kairos`). No `par` and no channels inside a `try`, not even through a called procedure.

### Stacks

```kairos
push(x, s)   // x on top of s, x reset to 0
pop(x, s)    // top of s into x; x must be 0 (Pop-Err2)
// push/pop work on stacks only; channels use ssend/srecv
```

Each is the inverse of the other. Popping an empty stack is an error.

### Channels

```kairos
ssend(<v1, v2, …>, c)   // send, zeroing/emptying the sources
srecv(<d1, d2, …>, c)   // receive; int destinations must be 0, stacks empty (Srecv-Err)
```

Synchronous rendez-vous: `ssend` waits for an `srecv` and vice versa, and the receiver takes the whole message (an `srecv` with fewer destinations than the values sent is an error); nothing stays in the channel between two exchanges. The payload can hold `int`, `stack` (emptied and concatenated) and `channel` (endpoint passing, as in the π-calculus). `ssend` and `srecv` are each other's inverse. Inside a `par` each channel is a **binary session**: exactly two branches use it, with complementary protocols (static check in `sessions.py`, dynamic check in the VM).

### `par … and … rap`

```kairos
par
    ssend(<x>, c)
and
    srecv(<y>, c)
rap
```

One `pthread` per branch, all started together; the block ends when all of them end. `par` blocks can be nested. The inverse runs the inverse of every branch (swapping `ssend`/`srecv` and `call`/`uncall`).

Branches share the frame but not writable memory; each branch has its own stack of open `local`s, so `local … delocal` blocks in different branches never interleave: the frontend rejects a stack used by more than one branch (even only by `show`) and any `int` or array **written** in one branch and **used** in another, even indirectly (writes made by called procedures, fixed-point analysis over the call graph; `pop`/`srecv` count as writes). An `int` only read by several branches is allowed. At run time the VM also reports two simultaneous updates of the same `int` when it sees them (`vm_ref_lock.h`); this is a best-effort check, the guarantee is the static one.

### `show`

Output is not part of the store, so it does not affect reversibility, but it is observable: `show` is its own inverse, so `uncall` prints again, in reverse order (`n_k … n_1` for a forward `n_1 … n_k`). Each `show` prints its whole line even when two `par` branches print at the same time.

```kairos
show(x)          // "x: 42"  (int, stack, channel, array)
show(x, char)    // a single character: the low byte of x, no newline
```

### Comments

`// to end of line`. There are no multi-line comments.

---

## Reversibility: rules and checks

The frontend rejects with `[STATIC]` / `[LOCAL]`:

- `x += e`, `x -= e`, `x ^= e` with `x` in `e` (arrays included), or with an index that reads the array being written;
- `delocal int x = x`;
- a `delocal` that does not close the most recent `local` of the same block, a `local` left open, or a `local` that redeclares a parameter or an open local;
- two procedures with the same name, a `main` with parameters, or a procedure named like a builtin (`show`, `push`, `pop`, `ssend`, `srecv`) or with a name the compiler reserves (`…__inv`, `__try_…`);
- a declaration without `local` (`int x`, `stack s`, `channel c`) outside `main`;
- a `delocal` without its expected value;
- operands of the wrong type, following the declaration in scope (a name may be an `int` in one block and a stack in another): a stack, channel or whole array inside an expression; a `call`/`uncall` argument whose type differs from the parameter; `push`/`pop` not between an `int` variable and a stack; `ssend`/`srecv` on something that is not a channel, with a payload that is not made of distinct `int`s, stacks or channels; a `local`/`delocal` value that is not an integer for `int` and arrays, `nil` for a stack, `empty` for a channel;
- a literal that does not fit in 64 bits;
- a `delocal` whose type differs from its `local`;
- a variable used but not declared (parameter, declaration or open local);
- a `call`/`uncall` (or the short form `f(x)`) of a procedure that does not exist, with the wrong number of arguments, or with the same variable passed twice;
- in a `par`: the same `stack` in several branches; an `int`/array written in one branch and used in another; a `call` to a name that is neither a procedure nor a builtin (its writes would be unknown); two branches using a channel with incompatible protocols (a channel used by one branch only, or by more than two, is caught at run time);
- a `par`, `ssend` or `srecv` inside a `try` body or rollback, also through called procedures.

At run time the VM checks, exactly as Janus does: the value of every `delocal`, the exit assertion of every `if` (forwards) and its entry guard (backwards), the `from` guard on entry (LoopError1) and on re-entry (LoopError2), and the same for `until` when running backwards; and beyond Janus: `pop` and `srecv` into non-zero or non-empty destinations, empty stacks, indices and divisions, channel sessions (including `ssend`/`srecv` outside any `par`, which can never meet a partner). Every rule has an example in `test_error/` (`make test-error`).

---

## The bytecode

One line per instruction, `@SRC OPCODE arguments`, where `@SRC` is the source line (used by the debugger).

| Instruction | Meaning |
|---|---|
| `START` / `HALT` | program start / end |
| `PROC p` / `END_PROC p` | procedure |
| `PARAM type name`, `DECL type name` | parameter, frame variable |
| `LOCAL type name val` / `DELOCAL type name val` | open / close with check |
| `PUSHEQ v e` / `MINEQ v e` / `XOREQ v e` / `SWAP a b` | assignments |
| `PUSH v s` / `POP v s` | stacks |
| `SSEND … c` / `SRECV … c` | channels |
| `EVAL l op r`, `JMPF lbl`, `JMP lbl`, `LABEL lbl`, `ASSERT l op r` | control flow and assertions |
| `CALL p args` / `UNCALL p args` | forward / backward call |
| `SHOW v` / `SHOW v char` | output |
| `PAR_START`, `THREAD_N`, `PAR_END` | parallel block |

---

## Common errors

| Message | Cause |
|---|---|
| `DELOCAL: valore finale errato` | the variable does not hold the value declared in `delocal` |
| `[LOCAL] … non chiude il local aperto più di recente` | `local`/`delocal` are not a block (Janus rule) |
| `DELOCAL: … non è nil/empty` | stack or channel not empty when closed |
| `IF/FI non reversibile` | the exit assertion does not match the branch taken |
| `FROM: … (LoopError1)` / `(LoopError2)` | loop entry guard false on entry / true on re-entry |
| `POP: destinazione … non è zero (Pop-Err2)` | `pop` into a non-zero variable |
| `Idx-Err` / `Div-Err` | index out of bounds / division by zero |
| `[STATIC] race su int nel PAR` | written in one branch, used in another: use distinct variables or a channel |
| `[VM] SESSIONE: …` | channel used by more than two branches, or a stuck protocol |
| `cannot open shared object file: libvm.so` | run `make build-release` |

Diagnostics are printed in Italian.

---

## Debugger and VS Code

- **Debugger extension**: [kairos-vscode-debugger](https://github.com/nicologiuliani6/kairos-vscode-debugger), with forward and backward stepping (`Step Back`, `Reverse Continue`). A step back runs the inverse of the previous instruction inside the interpreter: a completed call or `par` is undone as a whole with its inverse, and a `show` is undone by printing again (it is its own inverse); the debugger keeps only the trace of positions, no data. Stepping back from the start of a procedure into its caller, inside the branches of a running `par`, or while stopped inside an `uncall` in progress, is not supported and is reported. With the package installed: `kairos.appPath = /usr/local/bin/kairosapp`, `kairos.libPath = /usr/local/lib/kairosapp/dap.so`.
- **Standalone app**: `make release-app`.
