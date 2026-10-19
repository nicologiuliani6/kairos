# Kairos

Linguaggio reversibile e concorrente (famiglia Janus) creato dall'utente. Ogni programma gira avanti e indietro.
Frontend Python (PLY) -> bytecode testuale -> VM in C che lo interpreta in entrambe le direzioni.

## Struttura
- `src/kairos.py` entry point del frontend
- `src/frontend/`: `lexer.py`, `parser.py`, `ast.py`, `inverse.py` (inversione), `sessions.py` (protocolli dei canali), `bytecode.py`, `errors.py`
- `src/vm/`: VM in C (`Janus.c`, `vm_*.h`: par, canali, sessioni, inversione), `Janus_dap.c` per il debugger
- `examples/` programmi di esempio (`fib.kairos`, `janus1982/`...), `tests/` test `.kairos`, `lossless/` caso di studio Burrows-Wheeler

## Comandi
- `make install-deps` prima volta (venv + ply)
- `make` compila `build/libvm.so`
- `make run FILE=examples/fib.kairos` esegue un programma
- `./venv/bin/python -m src.kairos file.kairos [--dump-bytecode] [--vm-stats]`
- `make test` tutta la suite (tests/, examples/, lossless/)

## Regole
- Dopo aver modificato i sorgenti C della VM rilancia `make build-release`, altrimenti resta in uso la vecchia `libvm.so`.
- Ogni costrutto nuovo deve avere un inverso ben definito: aggiorna anche `src/frontend/inverse.py` e la parte inversa nella VM.
- Non toccare `build/`, `venv/`, `packaging/` se non richiesto.

## Ottimizzazioni Implementate

### 1. Caching thread-local per make_frame_key_par_rec
- Aggiunte variabili `__thread` a scope di file per memorizzare l'ultima chiave generata
- Implementata logica di cache nella funzione `make_frame_key_par_rec` per evitare ricreazione della stessa chiave
- Aggiunta pulizia della cache in `vm_cexpr_free_cache`
- Mantenuti output byte-for-byte identici, errori e flussi di controllo
- Nessun cambiamento ai file build/, venv/, packaging/, tests/
- Utilizzati interi a 64 bit, senza modifiche alla sintassi
- Commenti in italiano, seguendo lo stile esistente
