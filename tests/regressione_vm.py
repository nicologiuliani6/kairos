#!/usr/bin/env python3
"""Regressione della VM di Kairos sull'uscita registrata (golden output).

Si registra una volta l'uscita di tutti i programmi di prova eseguiti con una
libvm di riferimento, poi la si confronta con quella di un'altra libvm (per
esempio una versione ottimizzata). Si confronta la VM, non il frontend: il
bytecode si traduce una sola volta, alla registrazione, e il confronto
riesegue proprio quel bytecode.

Uso:
    python tests/regressione_vm.py registra CARTELLA LIB.so
    python tests/regressione_vm.py confronta CARTELLA LIB.so
    python tests/regressione_vm.py confronta CARTELLA LIB.so --compiled-uncall

Con --compiled-uncall il confronto ritraduce ogni programma con gli inversi
calcolati in compilazione (ByteCode_Compiler(compiled_uncall=True): ogni
`uncall f` diventa `CALL f__inv`) e controlla che l'uscita sia la stessa
registrata con l'uncall della VM.

Programmi: tests/*.kairos, examples/**/*.kairos, lossless/*.kairos e
test_error/*.kairos, in ordine di percorso. Il nome di un programma e' il suo
percorso relativo senza `.kairos`, con '/' sostituito da '__'. In CARTELLA:

    <nome>.bc     bytecode passato alla VM (manca se la compilazione fallisce)
    <nome>.out    prima riga `# esito: ...`, poi l'uscita della VM byte per
                  byte (stdout e stderr insieme, nello stesso file); per un
                  errore di compilazione, il messaggio che stamperebbe kairos
    <nome>.2.out  un'altra esecuzione, diversa dalla prima: solo per i
                  programmi nondeterministici
    nondeterministici.txt  i programmi con esecuzioni diverse tra loro
    info.txt      con che cosa e quando e' stata fatta la registrazione

Ogni esecuzione avviene in un processo figlio (fork) che carica la libreria
indicata con ctypes, con un tempo massimo di 20 secondi.

Registrazione: ogni programma si esegue due volte. Quelli con `par` (thread)
si eseguono in tutto RIPETIZIONI_PAR volte, a giri su tutti i programmi con
par, e meta' dei giri in piu' con il figlio legato a una sola CPU: l'intreccio
dei thread cambia con il carico della macchina e nel tempo, e due esecuzioni
di fila spesso coincidono anche quando l'uscita dipende dallo scheduler.
L'uscita registrata (.out) e' sempre quella della prima esecuzione, normale.

Confronto: PASS se esito e uscita sono identici alla registrazione, DIFF se
no (con un diff breve), TIMEOUT se l'esecuzione non finisce. Per i programmi
nondeterministici contano solo l'esito e la sezione finale `=== VM dump ===`,
se le esecuzioni registrate la hanno e sono d'accordo; altrimenti il
programma si salta. Un programma con par che risulta diverso si riprova fino
a TENTATIVI_PAR volte: se un tentativo coincide e' PASS; se nessuno coincide
ma uno ha lo stesso esito, lo stesso dump e le stesse righe in un altro
ordine (show e' atomica, i thread si intrecciano riga per riga) e' PASS a
meno dell'ordine. Entrambi i casi sono elencati a parte nel riepilogo.
Codice di uscita 1 se almeno un programma e' diverso o va in timeout.
"""
import ctypes
import difflib
import hashlib
import os
import pathlib
import platform
import re
import select
import signal
import subprocess
import sys
import tempfile
import time
import traceback

RADICE = pathlib.Path(__file__).resolve().parent.parent   # cartella di kairos
MODELLI = ('tests/*.kairos', 'examples/**/*.kairos',
           'lossless/*.kairos', 'test_error/*.kairos')
TIMEOUT = 20.0                    # secondi per ogni esecuzione della VM
TIMEOUT_COMPILAZIONE = 120.0      # secondi per tradurre un programma
RIPETIZIONI_PAR = 20              # esecuzioni registrate dei programmi con par
TENTATIVI_PAR = 5                 # tentativi al confronto per i programmi con par
MARCA_DUMP = b'=== VM dump ==='
ESITO = b'# esito: '
COMPILAZIONE_FALLITA = 'errore di compilazione'
FILE_ND = 'nondeterministici.txt'
FILE_INFO = 'info.txt'
RIGHE_DIFF = 40                   # righe di diff mostrate al massimo
# Buffer di stdout della VM nel figlio: pieno, di 4096 byte, come quello che
# glibc sceglie per un file normale. Fissarlo rende l'intreccio di stdout e
# stderr nell'uscita catturata indipendente da `python -u`/PYTHONUNBUFFERED
# (che rendono stdout del C senza buffer) e dal filesystem del file temporaneo.
BUFFER_STDOUT = 4096
_IOFBF = 0
# Il figlio non e' riuscito a far partire la VM (eccezione Python, il
# traceback finisce nell'uscita catturata).
CODICE_FIGLIO_ROTTO = 99
# Variabili d'ambiente lette dalla VM che aggiungono righe all'uscita.
VARIABILI_VM = ('KAIROS_VM_STATS', 'KAIROS_VM_TRACE')
_PAR = re.compile(rb'^\S+[ \t]+PAR_START\b', re.M)


def _errore(msg):
    print(f'errore: {msg}', file=sys.stderr)
    sys.exit(2)


def _alza_stack():
    """Come src/kairos.py: stack senza limite (l'UNCALL della VM e il
    frontend sono ricorsivi). Vale anche per i figli."""
    try:
        import resource
        try:
            resource.setrlimit(resource.RLIMIT_STACK,
                               (resource.RLIM_INFINITY, resource.RLIM_INFINITY))
        except (ValueError, OSError):
            _, massimo = resource.getrlimit(resource.RLIMIT_STACK)
            resource.setrlimit(resource.RLIMIT_STACK, (massimo, massimo))
    except (ImportError, ValueError, OSError):
        pass


def programmi():
    """Percorsi relativi (con '/') dei programmi da provare, ordinati."""
    trovati = set()
    for modello in MODELLI:
        for p in RADICE.glob(modello):
            if p.is_file():
                trovati.add(p.relative_to(RADICE).as_posix())
    return sorted(trovati)


def nome_di(rel):
    return rel[:-len('.kairos')].replace('/', '__')


def usa_par(bytecode):
    """Il programma apre thread (istruzione PAR_START)?"""
    return _PAR.search(bytecode) is not None


# ----------------------------------------------------------------------
#  Processi figli
# ----------------------------------------------------------------------

def _svuota_flussi():
    """Prima di una fork: niente testo di Python in sospeso che il figlio
    possa riscrivere nel proprio file."""
    sys.stdout.flush()
    sys.stderr.flush()


def _uccidi(pid):
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except (ProcessLookupError, ChildProcessError):
        pass


def _attendi(pid, scadenza):
    """Aspetta il figlio fino all'istante `scadenza` (perf_counter). Restituisce
    lo stato di waitpid, o None se il tempo e' scaduto (il figlio e' ucciso)."""
    try:
        pidfd = os.pidfd_open(pid)
    except (AttributeError, OSError):
        pidfd = None                  # senza pidfd: si controlla a intervalli
    try:
        while True:
            fatto, stato = os.waitpid(pid, os.WNOHANG)
            if fatto:
                return stato
            resto = scadenza - time.perf_counter()
            if resto <= 0:
                _uccidi(pid)
                return None
            if pidfd is not None:
                select.select([pidfd], [], [], resto)
            else:
                time.sleep(min(0.002, resto))
    except BaseException:
        _uccidi(pid)                  # anche per Ctrl-C: nessun figlio orfano
        raise
    finally:
        if pidfd is not None:
            os.close(pidfd)


def descrivi(stato):
    """Stato di waitpid -> 'uscita N', 'segnale N (NOME)' o 'timeout'."""
    if stato is None:
        return 'timeout'
    if os.WIFEXITED(stato):
        return f'uscita {os.WEXITSTATUS(stato)}'
    if os.WIFSIGNALED(stato):
        n = os.WTERMSIG(stato)
        try:
            nome = signal.Signals(n).name
        except ValueError:
            nome = '?'
        return f'segnale {n} ({nome})'
    return f'stato {stato}'


def _figlio_vm(bytecode, lib, fd, una_cpu):
    """Nel figlio: stdout e stderr sul file `fd`, poi la VM. Non ritorna."""
    codice = CODICE_FIGLIO_ROTTO
    try:
        if una_cpu:                   # i thread della VM ereditano il legame
            os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})
        nullo = os.open(os.devnull, os.O_RDONLY)
        os.dup2(nullo, 0)
        os.dup2(fd, 1)
        os.dup2(fd, 2)
        for v in VARIABILI_VM:
            os.environ.pop(v, None)
        libc = ctypes.CDLL(None)
        libc.setvbuf.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.c_int, ctypes.c_size_t]
        buffer = ctypes.create_string_buffer(BUFFER_STDOUT)   # vive fino a _exit
        c_stdout = ctypes.c_void_p.in_dll(libc, 'stdout')
        libc.setvbuf(c_stdout, ctypes.addressof(buffer), _IOFBF, BUFFER_STDOUT)
        vm = ctypes.CDLL(lib)
        vm.vm_run_from_string.argtypes = [ctypes.c_char_p]
        vm.vm_run_from_string.restype = None
        vm.vm_run_from_string(bytecode)
        libc.fflush(None)
        codice = 0
    except BaseException:
        traceback.print_exc()
        sys.stderr.flush()
    finally:
        os._exit(codice)


class Esito:
    """Una esecuzione: stato ('uscita 0', 'segnale 11 (SIGSEGV)', 'timeout'),
    uscita catturata (byte) e secondi di orologio."""

    def __init__(self, stato, uscita, secondi):
        self.stato, self.uscita, self.secondi = stato, uscita, secondi

    def testo(self):
        """Contenuto del file .out."""
        return intestazione(self.stato) + self.uscita

    def testo_dump(self):
        """Esito e dump finale: quello che si confronta se il programma e'
        nondeterministico."""
        return intestazione(self.stato) + (dump(self.uscita) or b'')


def intestazione(stato):
    return ESITO + stato.encode() + b'\n'


def dump(uscita):
    """La sezione finale '=== VM dump ===' (fino alla fine), o None."""
    k = uscita.rfind(MARCA_DUMP)
    return None if k < 0 else uscita[k:]


def esegui(bytecode, lib, timeout=TIMEOUT, una_cpu=False):
    """Esegue il bytecode (byte) con la libreria `lib` in un processo figlio,
    legato a una sola CPU se `una_cpu`."""
    with tempfile.TemporaryFile() as cattura:
        _svuota_flussi()
        t0 = time.perf_counter()
        pid = os.fork()
        if pid == 0:
            _figlio_vm(bytecode, lib, cattura.fileno(), una_cpu)
        stato = _attendi(pid, t0 + timeout)
        secondi = time.perf_counter() - t0
        cattura.seek(0)
        uscita = cattura.read()
    return Esito(descrivi(stato), uscita, secondi)


def prova_libreria(lib):
    """Controlla, in un figlio, che la libreria si carichi e abbia
    vm_run_from_string: meglio fermarsi subito che registrare errori di
    caricamento come uscita attesa."""
    if not os.path.isfile(lib):
        _errore(f'libreria non trovata: {lib}')
    _svuota_flussi()
    pid = os.fork()
    if pid == 0:
        codice = 1
        try:
            ctypes.CDLL(lib).vm_run_from_string
            codice = 0
        except BaseException as exc:
            print(f'errore: libreria non utilizzabile: {exc}', file=sys.stderr)
            sys.stderr.flush()
        finally:
            os._exit(codice)
    stato = _attendi(pid, time.perf_counter() + 30)
    if stato is None or not os.WIFEXITED(stato) or os.WEXITSTATUS(stato) != 0:
        _errore(f'la libreria {lib} non si carica ({descrivi(stato)})')
    with open(lib, 'rb') as f:
        impronta = hashlib.sha256(f.read()).hexdigest()
    print(f'libreria: {lib}  (sha256 {impronta[:16]})', flush=True)
    return impronta


# ----------------------------------------------------------------------
#  Compilazione (solo per registra)
# ----------------------------------------------------------------------

def _carica_frontend():
    """Importa il frontend di kairos (serve ply). Solo registra lo usa."""
    sys.setrecursionlimit(200000)     # come src/kairos.py
    if str(RADICE) not in sys.path:
        sys.path.insert(0, str(RADICE))
    from src.frontend.lexer import lexer
    from src.frontend.parser import parser, run_static_checks, desugar_try
    from src.frontend.bytecode import ByteCode_Compiler
    from src.frontend.errors import KairosCompileError
    return lexer, parser, run_static_checks, desugar_try, ByteCode_Compiler, KairosCompileError


COMPILED_UNCALL = False      # --compiled-uncall nel confronto


def _traduci(frontend, sorgente):
    """Stessi passi e stesso formato di src/kairos.py."""
    lexer, parser, run_static_checks, desugar_try, ByteCode_Compiler, KairosCompileError = frontend
    lexer.lineno = 1
    albero = parser.parse(sorgente, lexer=lexer)
    if albero is None:
        raise KairosCompileError('PARSER', 'compilation stopped: no AST produced')
    albero = desugar_try(albero)
    run_static_checks(albero)
    bc = ByteCode_Compiler(compiled_uncall=COMPILED_UNCALL)
    bc.process(albero)
    righe = []
    while not bc.queue.empty():
        _fisica, tag, istruzione = bc.queue.get()
        righe.append(f'{tag:<6}  {istruzione}')
    return '\n'.join(righe) + '\n'


def compila(frontend, sorgente):
    """Traduce in un processo figlio, cosi' ogni programma parte dallo stato
    del frontend appena importato, come con `kairos file`. Restituisce
    (bytecode in byte, None) oppure (None, messaggio d'errore)."""
    KairosCompileError = frontend[-1]
    with tempfile.TemporaryFile() as risultato:
        _svuota_flussi()
        t0 = time.perf_counter()
        pid = os.fork()
        if pid == 0:
            codice = 3
            try:
                try:
                    dati = _traduci(frontend, sorgente).encode('utf-8')
                    codice = 0
                except KairosCompileError as exc:
                    dati = f'{exc}\n'.encode('utf-8')
                    codice = 1
                except Exception as exc:          # come src/kairos.py
                    dati = f'[COMPILER] internal error: {exc}\n'.encode('utf-8')
                    codice = 1
                with open(risultato.fileno(), 'wb', closefd=False) as f:
                    f.write(dati)
            except BaseException:
                traceback.print_exc()
                codice = 3
            finally:
                os._exit(codice)
        stato = _attendi(pid, t0 + TIMEOUT_COMPILAZIONE)
        risultato.seek(0)
        dati = risultato.read()
    if stato is not None and os.WIFEXITED(stato):
        if os.WEXITSTATUS(stato) == 0:
            return dati, None
        if os.WEXITSTATUS(stato) == 1:
            return None, dati.decode('utf-8', 'replace')
    return None, f'[COMPILER] compilazione interrotta: {descrivi(stato)}\n'


# ----------------------------------------------------------------------
#  registra
# ----------------------------------------------------------------------

def _prepara_cartella(cartella):
    """Crea la cartella, o la svuota dei file di una registrazione precedente.
    Una cartella non vuota che non viene da qui non si tocca."""
    if cartella.exists() and not cartella.is_dir():
        _errore(f'{cartella} esiste e non e\' una cartella')
    cartella.mkdir(parents=True, exist_ok=True)
    presenti = list(cartella.iterdir())
    if presenti and not (cartella / FILE_INFO).exists():
        _errore(f'{cartella} non e\' vuota e non contiene {FILE_INFO}: '
                'scegli una cartella nuova')
    for p in presenti:
        if p.is_file() and (p.suffix in ('.bc', '.out') or p.name in (FILE_ND, FILE_INFO)):
            p.unlink()


def _git(*argomenti):
    try:
        r = subprocess.run(['git', '-C', str(RADICE), *argomenti],
                           capture_output=True, text=True, timeout=30)
        return r.stdout.strip() if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def _stesso_risultato(a, b):
    """Due timeout sono lo stesso risultato (l'uscita parziale dipende da
    quando arriva il segnale); altrimenti contano esito e uscita esatti."""
    if a.stato == 'timeout' and b.stato == 'timeout':
        return True
    return a.testo() == b.testo()


class Voce:
    """Un programma compilato, con le esecuzioni registrate che servono:
    la prima, la prima diversa da lei e la prima con esito o dump diversi."""

    def __init__(self, nome, bytecode):
        self.nome, self.bytecode = nome, bytecode
        self.par = usa_par(bytecode)
        self.prima = self.diversa = self.diversa_dump = None
        self.esecuzioni = 0

    def aggiungi(self, e):
        self.esecuzioni += 1
        if self.prima is None:
            self.prima = e
        elif not _stesso_risultato(self.prima, e):
            if self.diversa is None:
                self.diversa = e
            if self.diversa_dump is None and e.testo_dump() != self.prima.testo_dump():
                self.diversa_dump = e

    def nondeterministico(self):
        return self.diversa is not None

    def seconda(self):
        """L'esecuzione da salvare in .2.out: se ce n'e' una con esito o dump
        diversi si salva quella, cosi' il confronto sa che non sono stabili."""
        return self.diversa_dump or self.diversa


def registra(cartella, lib):
    t0 = time.perf_counter()
    impronta = prova_libreria(lib)
    frontend = _carica_frontend()     # prima di svuotare la cartella
    _prepara_cartella(cartella)
    elenco = programmi()
    nomi = [nome_di(r) for r in elenco]
    if len(set(nomi)) != len(nomi):
        _errore('due programmi hanno lo stesso nome nella cartella')
    larghezza = max(len(n) for n in nomi)
    print(f'registrazione di {len(elenco)} programmi in {cartella}', flush=True)

    # Primo passaggio: traduzione e due esecuzioni per programma.
    voci, falliti = [], []
    for i, (rel, nome) in enumerate(zip(elenco, nomi), 1):
        testa = f'[{i:3}/{len(elenco)}] {nome:<{larghezza}}'
        try:
            sorgente = (RADICE / rel).read_text(encoding='utf-8')
            bytecode, errore = compila(frontend, sorgente)
        except (OSError, UnicodeDecodeError) as exc:
            bytecode, errore = None, f'[COMPILER] sorgente illeggibile: {exc}\n'
        if bytecode is None:
            falliti.append(nome)
            (cartella / f'{nome}.out').write_bytes(
                intestazione(COMPILAZIONE_FALLITA) + errore.encode('utf-8'))
            print(f'{testa}  {COMPILAZIONE_FALLITA}: {errore.strip()[:70]}', flush=True)
            continue
        (cartella / f'{nome}.bc').write_bytes(bytecode)
        v = Voce(nome, bytecode)
        v.aggiungi(esegui(bytecode, lib))
        v.aggiungi(esegui(bytecode, lib))
        voci.append(v)
        nota = '  NONDETERMINISTICO' if v.nondeterministico() else ''
        print(f'{testa}  {v.prima.stato:<10} {v.prima.secondi:7.3f} s{nota}', flush=True)

    # Giri in piu' sui programmi con par, uno dopo l'altro a rotazione; un
    # giro su due con tutti i thread su una sola CPU.
    con_par = [v for v in voci if v.par]
    giri = RIPETIZIONI_PAR - 2
    if con_par and giri > 0:
        print(f'\naltre {giri} esecuzioni per ciascuno dei {len(con_par)} programmi con par '
              '(la meta\' su una sola CPU)...', flush=True)
        prima_nd = {v.nome for v in voci if v.nondeterministico()}
        for giro in range(giri):
            for v in con_par:
                v.aggiungi(esegui(v.bytecode, lib, una_cpu=giro % 2 == 1))
        nuovi = [v.nome for v in con_par if v.nondeterministico() and v.nome not in prima_nd]
        print('nondeterministici trovati in questi giri: ' + (', '.join(nuovi) or 'nessuno'))

    nondet, esiti = [], {}
    for v in voci:
        (cartella / f'{v.nome}.out').write_bytes(v.prima.testo())
        esiti[v.prima.stato] = esiti.get(v.prima.stato, 0) + 1
        if v.nondeterministico():
            nondet.append(v.nome)
            (cartella / f'{v.nome}.2.out').write_bytes(v.seconda().testo())
    (cartella / FILE_ND).write_text(''.join(f'{n}\n' for n in nondet))
    instabili = [v.nome for v in voci if v.diversa_dump is not None]

    commit = _git('rev-parse', 'HEAD') or 'sconosciuto'
    modifiche = _git('status', '--porcelain', '--', 'src/frontend')
    riepilogo_esiti = ', '.join(f'{k}: {n}' for k, n in sorted(esiti.items()))
    info = [
        'Registrazione per tests/regressione_vm.py',
        f'data: {time.strftime("%Y-%m-%d %H:%M:%S")}',
        f'libreria: {lib}',
        f'sha256 libreria: {impronta}',
        f'kairos: {RADICE}',
        f'commit: {commit}' + ('  (src/frontend con modifiche locali)' if modifiche else ''),
        f'python: {platform.python_version()}',
        f'timeout: {TIMEOUT:g} s',
        f'esecuzioni: 2 per programma, {max(RIPETIZIONI_PAR, 2)} per quelli con par '
        '(i giri in piu\' alternati: normale, una sola CPU)',
        f'programmi: {len(elenco)}',
        f'compilati: {len(voci)} (con par: {len(con_par)})',
        f'errori di compilazione: {len(falliti)}',
        f'nondeterministici: {len(nondet)} (esito o dump instabili: {len(instabili)})',
        f'esiti della prima esecuzione: {riepilogo_esiti}',
    ]
    (cartella / FILE_INFO).write_text('\n'.join(info) + '\n')

    print()
    print(f'registrati {len(elenco)} programmi: {len(voci)} compilati '
          f'({len(con_par)} con par), {len(falliti)} errori di compilazione, '
          f'{len(nondet)} nondeterministici')
    print(f'esiti della VM: {riepilogo_esiti}')
    if nondet:
        print('nondeterministici: ' + ', '.join(nondet))
    if instabili:
        print('  di cui con esito o dump instabili: ' + ', '.join(instabili))
    print(f'tempo totale: {time.perf_counter() - t0:.2f} s')
    return 0


# ----------------------------------------------------------------------
#  confronta
# ----------------------------------------------------------------------

def _separa(testo):
    """Contenuto di un .out -> (stato, uscita)."""
    riga, _, uscita = testo.partition(b'\n')
    if not riga.startswith(ESITO):
        raise ValueError('prima riga senza "# esito:"')
    return riga[len(ESITO):].decode(), uscita


def _a_meno_dell_ordine(e, stato, uscita):
    """Stesso esito, stesso dump finale e stesse righe, in un altro ordine."""
    return (e.stato == stato and dump(e.uscita) == dump(uscita)
            and sorted(e.uscita.splitlines()) == sorted(uscita.splitlines()))


def _mostra_diff(atteso, ottenuto):
    a = atteso.decode('utf-8', 'backslashreplace').splitlines(keepends=True)
    b = ottenuto.decode('utf-8', 'backslashreplace').splitlines(keepends=True)
    righe = list(difflib.unified_diff(a, b, 'registrato', 'ottenuto', n=2))
    if not righe:
        print('    (differenza non visibile riga per riga)')
    for r in righe[:RIGHE_DIFF]:
        print('    ' + (r[:-1] if r.endswith('\n') else r + '  [senza a capo finale]'))
    if len(righe) > RIGHE_DIFF:
        print(f'    ... altre {len(righe) - RIGHE_DIFF} righe di diff')


def confronta(cartella, lib):
    t0 = time.perf_counter()
    prova_libreria(lib)
    if not (cartella / FILE_INFO).exists():
        _errore(f'{cartella} non contiene una registrazione ({FILE_INFO} assente)')
    nd_file = cartella / FILE_ND
    nondet = set()
    if nd_file.exists():
        nondet = {r.strip() for r in nd_file.read_text().splitlines()
                  if r.strip() and not r.startswith('#')}
    seconde = {f'{n}.2.out' for n in nondet}
    nomi = sorted(p.name[:-len('.out')] for p in cartella.glob('*.out')
                  if p.name not in seconde)
    if not nomi:
        _errore(f'nessun file .out in {cartella}')
    larghezza = max(len(n) for n in nomi)
    print(f'confronto di {len(nomi)} programmi con {cartella}', flush=True)

    uguali, diversi, saltati, ritentati, riordinati = [], [], [], [], []
    motivi_salto = {}

    def riga(etichetta, nome, resto=''):
        print(f'{etichetta:<8} {nome:<{larghezza}}  {resto}'.rstrip(), flush=True)

    def salta(nome, motivo):
        saltati.append(nome)
        motivi_salto[motivo] = motivi_salto.get(motivo, 0) + 1
        riga('SALTATO', nome, f'({motivo})')

    sorgenti = {nome_di(rel): rel for rel in programmi()}
    frontend = _carica_frontend() if COMPILED_UNCALL else None
    for nome in nomi:
        stato_atteso, uscita_attesa = _separa((cartella / f'{nome}.out').read_bytes())
        bc_file = cartella / f'{nome}.bc'
        if not bc_file.exists():
            salta(nome, 'errore di compilazione registrato, niente bytecode')
            continue
        bytecode = bc_file.read_bytes()
        if COMPILED_UNCALL:
            nuovo, err = compila(frontend, (RADICE / sorgenti[nome]).read_text())
            if nuovo is None:
                diversi.append(nome)
                riga('DIFF', nome, f'(non si traduce con --compiled-uncall: {err.strip()[:120]})')
                continue
            bytecode = nuovo
        nd = nome in nondet

        if nd:
            # Contano solo l'esito e il dump finale, e solo se le due
            # esecuzioni registrate li hanno uguali.
            seconda = cartella / f'{nome}.2.out'
            stato2, uscita2 = (_separa(seconda.read_bytes()) if seconda.exists()
                               else (stato_atteso, uscita_attesa))
            d1, d2 = dump(uscita_attesa), dump(uscita2)
            if d1 is None or d2 is None:
                salta(nome, 'nondeterministico senza dump finale')
                continue
            if d1 != d2 or stato_atteso != stato2:
                salta(nome, 'nondeterministico, esito o dump diversi tra le registrazioni')
                continue
            atteso = intestazione(stato_atteso) + d1
            forma = Esito.testo_dump
        elif stato_atteso == 'timeout':
            e = esegui(bytecode, lib)
            if e.stato == 'timeout':
                salta(nome, 'timeout anche nella registrazione')
            else:
                diversi.append(nome)
                riga('DIFF', nome, f'({e.secondi:.3f} s)  (registrato: timeout, ora {e.stato})')
            continue
        else:
            atteso = intestazione(stato_atteso) + uscita_attesa
            forma = Esito.testo

        # Fino a TENTATIVI_PAR esecuzioni se ci sono thread, una sola se no.
        tentativi = TENTATIVI_PAR if usa_par(bytecode) else 1
        primo = trovato = riordinato = None
        scaduto = False
        for k in range(1, tentativi + 1):
            e = esegui(bytecode, lib)
            primo = primo or e
            if e.stato == 'timeout':
                scaduto = True
                break
            if forma(e) == atteso:
                trovato = k
                break
            if not nd and riordinato is None and _a_meno_dell_ordine(e, stato_atteso,
                                                                    uscita_attesa):
                riordinato = k
        tempo = f'({primo.secondi:.3f} s)'
        note = ['nondeterministico: solo esito e dump'] if nd else []
        if scaduto:
            diversi.append(nome)
            riga('TIMEOUT', nome, f'(oltre {TIMEOUT:g} s)')
        elif trovato is not None or riordinato is not None:
            uguali.append(nome)
            if trovato is None:
                note.append(f'stesse righe in un altro ordine, tentativo {riordinato}')
                riordinati.append(nome)
            elif trovato > 1:
                note.append(f'uguale solo al tentativo {trovato}')
                ritentati.append(f'{nome} ({trovato})')
            riga('PASS', nome, tempo + (f'  ({"; ".join(note)})' if note else ''))
        else:
            diversi.append(nome)
            if nd:
                note = ['nondeterministico: esito o dump diversi']
            if tentativi > 1:
                note.append(f'diverso in {tentativi} tentativi')
            riga('DIFF', nome, tempo + (f'  ({"; ".join(note)})' if note else ''))
            _mostra_diff(atteso, forma(primo))

    print()
    if diversi:
        print('diversi: ' + ', '.join(diversi))
    if ritentati:
        print('uguali solo dopo piu\' tentativi: ' + ', '.join(ritentati))
    if riordinati:
        print('uguali a meno dell\'ordine delle righe: ' + ', '.join(riordinati))
    if motivi_salto:
        print('saltati: ' + ', '.join(f'{n} {m}' for m, n in sorted(motivi_salto.items())))
    print(f'{len(uguali)} uguali, {len(diversi)} diversi, {len(saltati)} saltati')
    print(f'tempo totale: {time.perf_counter() - t0:.2f} s')
    return 1 if diversi else 0


def main(argv):
    global COMPILED_UNCALL
    if argv[3:] == ['--compiled-uncall'] and argv[:1] == ['confronta']:
        COMPILED_UNCALL = True
        argv = argv[:3]
    if len(argv) != 3 or argv[0] not in ('registra', 'confronta'):
        uso = next(p for p in __doc__.split('\n\n') if p.startswith('Uso:'))
        print(uso, file=sys.stderr)
        return 2
    modo, cartella, lib = argv
    cartella = pathlib.Path(cartella).resolve()
    lib = os.path.abspath(lib)
    _alza_stack()
    if modo == 'registra':
        return registra(cartella, lib)
    return confronta(cartella, lib)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
