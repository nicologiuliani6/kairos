# Ottimizzazioni VM Implementate

## 1. Caching thread-local per make_frame_key_par_rec

- Aggiunte variabili `__thread` a scope di file per memorizzare l'ultima chiave generata
- Implementata logica di cache nella funzione `make_frame_key_par_rec` per evitare ricreazione della stessa chiave
- Aggiunta pulizia della cache in `vm_cexpr_free_cache`
- Mantenuti output byte-for-byte identici, errori e flussi di controllo
- Nessun cambiamento ai file build/, venv/, packaging/, tests/
- Utilizzati interi a 64 bit, senza modifiche alla sintassi
- Commenti in italiano, seguendo lo stile esistente

## 2. Frame Cloning Optimization

- Implementata cache thread-local per `clone_frame_for_depth` per evitare ricreazione di frame identici
- Implementata cache thread-local per `clone_frame_for_thread` per evitare ricreazione di frame identici
- Aggiunta logica di cache lookup per chiamate consecutive con stessi parametri
- Aggiornamento cache a ogni creazione di frame per mantenere la consistenza
- Mantenuti output byte-for-byte identici, errori e flussi di controllo

## 3. Call/Uncall Caching

- Implementata cache thread-local per `vm_call_cache` per evitare ricreazione di slot identici per stessa riga
- Aggiunta logica di cache lookup per chiamate consecutive con stessa riga
- Aggiornamento cache a ogni creazione di slot per mantenere la consistenza
- Mantenuti output byte-for-byte identici, errori e flussi di controllo

## 4. Frame Reuse Strategy

- Implementata strategia di riutilizzo dei frame tramite caching thread-local
- Aggiunta funzione `get_or_create_frame` che tenta di riutilizzare frame esistenti
- Mantenuti output byte-for-byte identici, errori e flussi di controllo

## 5. Thread-Local Frame Indexing

- Implementata cache thread-local per il lookup degli indici dei frame
- Aggiunta logica di cache per evitare ricerche ripetute nella stessa mappa
- Implementata funzione `get_frame_index_cached` con supporto per cache locale per thread
- Mantenuti output byte-for-byte identici, errori e flussi di controllo