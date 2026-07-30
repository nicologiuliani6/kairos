"""Disciplina di sessione sui canali — parte statica.

Tutto ciò che riguarda i canali e i tipi di sessione lato compilatore sta qui
dentro, e da fuori si vede una funzione sola: `check_sessions`. `parser.py`
importa questo modulo; questo modulo non importa `parser.py`. Se un giorno il
controllo statico va sostituito o tolto, si cancella questo file e la riga che
lo chiama.

Il controllo che conta è quello DINAMICO, nella VM (`src/vm/vm_session.h`): è
lui a garantire che un canale abbia in ogni istante al più due titolari, e a
seguire un canale che cambia titolare, cosa che un'analisi del testo non può
fare. Qui resta l'anticipo: i conflitti di protocollo che si vedono già dal
sorgente, segnalati in compilazione invece che come blocco a runtime.

I tipi di sessione sono INFERITI, non dichiarati:

    S ::= end | !k.S | ?k.S | mu X.S | X          (k = arità del payload)

Il controllo è SANO ma INCOMPLETO, per scelta: dove il frammento non basta a
descrivere l'uso di un canale l'inferenza produce ('any',) e il confronto si
ferma lì senza segnalare nulla. Così nessun programma corretto viene rifiutato
perché il sistema di tipi è piccolo.
"""

from .errors import KairosCompileError

# I nomi delle builtin servono a riconoscere ssend/srecv fra le call_direct.
# Import ritardato dentro le funzioni per non creare un ciclo con parser.py.


def _builtin_names():
    from .parser import _BUILTIN_CALL_OPCODES
    return _BUILTIN_CALL_OPCODES


def _from_second_body(stmt):
    from .parser import from_second_body
    return from_second_body(stmt)

def _collect_channel_args_from_call(stmt, proc_signatures, out):
    """Come _collect_stack_args_from_call, ma per 'channel'. In più, a
    differenza di call/uncall, ssend/srecv sono call_direct con il canale
    come ultimo argomento (non risolvibile via proc_signatures)."""
    if not isinstance(stmt, tuple) or not stmt:
        return

    tag = stmt[0]
    if tag in ('call', 'uncall'):
        _, proc_name, args, _lineno = stmt
        param_types = proc_signatures.get(proc_name, [])
        for idx, actual in enumerate(args or []):
            if idx < len(param_types) and param_types[idx] == 'channel' and isinstance(actual, str):
                out.add(actual)
        return

    if tag == 'call_direct':
        _, proc_name, args, _lineno = stmt
        lname = proc_name.lower()
        if lname in ('ssend', 'srecv'):
            if args and isinstance(args[-1], str):
                out.add(args[-1])
            return
        if lname in ('push', 'pop'):
            return
        param_types = proc_signatures.get(proc_name, [])
        for idx, actual in enumerate(args or []):
            if idx < len(param_types) and param_types[idx] == 'channel' and isinstance(actual, str):
                out.add(actual)
        return

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        for nested in then_body:
            _collect_channel_args_from_call(nested, proc_signatures, out)
        for nested in else_body:
            _collect_channel_args_from_call(nested, proc_signatures, out)
        return

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        for nested in list(body) + _from_second_body(stmt):
            _collect_channel_args_from_call(nested, proc_signatures, out)
        return

    # NOTA: a differenza di _collect_stack_args_from_call, qui NON si
    # ricorsa dentro 'par' annidati: un par annidato introduce nuovi thread
    # concorrenti (foglie separate), non ulteriori usi dello stesso thread.
    # È _par_leaf_channel_touches sotto a gestirli come foglie a parte.


def _par_leaf_bodies(branch_stmts):
    """Come _par_leaf_channel_touches, ma restituisce i CORPI delle foglie
    invece dei soli nomi di channel: serve all'inferenza dei tipi di sessione,
    che ha bisogno della sequenza ordinata degli statement e non di un insieme.
    Le due funzioni devono appiattire i `par` annidati allo stesso modo,
    altrimenti un canale contato su due foglie verrebbe poi tipato su altre."""
    own = []
    leaves = []
    for nested in branch_stmts:
        if not isinstance(nested, tuple) or not nested:
            continue
        if nested[0] == 'par':
            _, sub_branches, _lineno = nested
            for sub_branch in sub_branches:
                leaves.extend(_par_leaf_bodies(sub_branch))
        else:
            own.append(nested)
    if own or not leaves:
        leaves.append(own)
    return leaves


# ── Tipi di sessione ────────────────────────────────────────────────────────
# Il vincolo di sessione binaria (sopra) garantisce che ogni canale abbia
# esattamente due endpoint, ma non dice nulla su COME i due li usano: due rami
# che fanno entrambi ssend passano quel controllo e si bloccano a runtime.
# Il tipo di sessione descrive la sequenza degli scambi, e i due estremi devono
# essere duali.
#
#   S ::= end | !k.S | ?k.S | mu X.S | X          (k = arita' del payload)
#
# rappresentati come tuple: ('end',) ('!', k, S) ('?', k, S) ('mu', S) ('var',).
# I tipi sono INFERITI dal codice: nessuna annotazione nella sintassi.
# Frammento deliberatamente minimo: niente scelta etichettata, niente delega.
#
# Il controllo e' SANO ma INCOMPLETO, per scelta. Dove il frammento non basta a
# descrivere l'uso di un canale l'inferenza produce ('any',), "non determinato",
# e il confronto si ferma li' senza segnalare nulla. Cosi' nessun programma
# corretto viene rifiutato perche' il sistema di tipi e' piccolo: si segnalano
# solo i conflitti dimostrabili. Due esempi reali dal repo, entrambi corretti e
# entrambi non tipabili qui:
#   examples/sat_solver.kairos  invia da rami diversi di if annidati; senza
#                               etichette di scelta il tipo non e' unico
#   examples/malloc.kairos      un lato invia in un ciclo di n giri, l'altro
#                               riceve n volte srotolato; servirebbero tipi
#                               dipendenti dal valore di n
_S_END = ('end',)
_S_VAR = ('var',)
_S_ANY = ('any',)


def _session_dual(s):
    """Duale di un tipo di sessione: si scambiano invio e ricezione."""
    tag = s[0]
    if tag == '!':
        return ('?', s[1], _session_dual(s[2]))
    if tag == '?':
        return ('!', s[1], _session_dual(s[2]))
    if tag == 'mu':
        return ('mu', _session_dual(s[1]))
    return s                                    # end, var, any


def _session_concat(s, t):
    """s seguito da t. Dopo `mu` o `any` non si concatena nulla: nel primo caso
    il protocollo non termina, nel secondo non e' noto."""
    tag = s[0]
    if tag == 'end':
        return t
    if tag in ('var', 'mu', 'any'):
        return s
    return (tag, s[1], _session_concat(s[2], t))


def _session_str(s):
    """Resa leggibile, per i messaggi d'errore."""
    tag = s[0]
    if tag == 'end':
        return 'end'
    if tag == 'var':
        return 'X'
    if tag == 'any':
        return '?'
    if tag == 'mu':
        return f"mu X.{_session_str(s[1])}"
    return f"{tag}{s[1]}.{_session_str(s[2])}"


def _session_clash(s1, s2):
    """Confronta due tipi di sessione che dovrebbero essere duali e restituisce
    la descrizione del primo conflitto DIMOSTRABILE, oppure None. Dove non si
    puo' decidere (un lato non determinato, oppure un ciclo contro una sequenza
    srotolata di lunghezza non nota) si restituisce None: meglio un falso
    negativo che rifiutare un programma corretto."""
    t1, t2 = s1[0], s2[0]

    if t1 == 'any' or t2 == 'any':
        return None
    if t1 == 'var' or t2 == 'var':
        return None
    if t1 == 'mu' and t2 == 'mu':
        return _session_clash(s1[1], s2[1])
    if t1 == 'mu' or t2 == 'mu':
        return None                             # ciclo contro sequenza: giri non noti
    if t1 == 'end' and t2 == 'end':
        return None
    if t1 == 'end' or t2 == 'end':
        avanti = s2 if t1 == 'end' else s1
        resta = ("ha ancora un invio da fare" if avanti[0] == '!'
                 else "attende ancora una ricezione")
        return f"un thread {resta}, ma l'altro ha gia' finito"
    if t1 == t2:
        verso = 'inviano' if t1 == '!' else 'ricevono'
        return f"entrambi i thread {verso} nello stesso punto del protocollo"
    if s1[1] != s2[1]:
        return (f"scambio di ampiezza diversa: {s1[1]} "
                f"{'valore' if s1[1] == 1 else 'valori'} da un lato, {s2[1]} dall'altro")
    return _session_clash(s1[2], s2[2])


def _session_of_stmt(stmt, ch, ctx):
    """Tipo di sessione del singolo statement sul canale `ch`. `ctx` porta le
    procedure del programma, i loro parametri, la memo dei tipi gia' calcolati
    e la catena di chiamate aperte (per rilevare la ricorsione)."""
    if not isinstance(stmt, tuple) or not stmt:
        return _S_END

    tag = stmt[0]

    if tag == 'call_direct':
        _, name, args, _lineno = stmt
        lname = name.lower()
        if lname in ('ssend', 'srecv') and args and args[-1] == ch:
            return ('!' if lname == 'ssend' else '?', len(args) - 1, _S_END)
        if lname in _builtin_names():
            return _S_END
        return _session_of_call(name, args, ch, ctx)

    if tag == 'call':
        _, name, args, _lineno = stmt
        return _session_of_call(name, args, ch, ctx)

    if tag == 'uncall':
        # L'inversione scambia ssend e srecv: il tipo sarebbe il duale della
        # sequenza rovesciata. Non lo ricostruiamo.
        _, _name, args, _lineno = stmt
        return _S_ANY if ch in (args or []) else _S_END

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        s_then = _session_of_body(then_body, ch, ctx)
        s_else = _session_of_body(else_body, ch, ctx)
        if s_then == s_else:
            return s_then
        # Rami con protocolli diversi: senza etichette di scelta il tipo del
        # canale non e' unico. Non determinato.
        return _S_ANY

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        s_do = _session_of_body(body, ch, ctx)
        s_loop = _session_of_body(_from_second_body(stmt), ch, ctx)
        if s_loop != _S_END:
            # La traccia e' c1 [c2 c1]*: l'ultima iterazione esegue c1 ma non
            # c2, quindi il protocollo non e' regolare.
            return _S_ANY
        if s_do == _S_END:
            return _S_END
        if s_do[0] in ('any', 'mu'):
            return _S_ANY                       # cicli annidati sullo stesso canale
        return ('mu', _session_concat(s_do, _S_VAR))

    if tag == 'par':
        # I par annidati sono gia' stati appiattiti in foglie da
        # _par_leaf_bodies: qui non si arriva passando da li'.
        return _S_ANY

    return _S_END


def _session_of_body(stmts, ch, ctx):
    """Concatenazione dei tipi degli statement di un corpo, nell'ordine."""
    out = _S_END
    for stmt in stmts or []:
        out = _session_concat(out, _session_of_stmt(stmt, ch, ctx))
    return out


def _session_of_call(proc_name, args, ch, ctx):
    """Tipo di una chiamata vista sul canale `ch` del chiamante: si individua
    il parametro formale che riceve `ch` e si tipa il corpo della callee su
    quel formale."""
    proc = ctx['procedures'].get(proc_name)
    if proc is None:
        return _S_END
    params = ctx['proc_params'].get(proc_name, [])
    formals = [name for idx, (_t, name) in enumerate(params)
               if idx < len(args or []) and args[idx] == ch]
    if not formals:
        return _S_END
    if len(formals) > 1:
        return _S_ANY                           # stesso canale su piu' formali
    key = (proc_name, formals[0])
    if key in ctx['open']:
        return _S_ANY                           # procedura ricorsiva sul canale
    if key not in ctx['memo']:
        ctx['open'].add(key)
        try:
            ctx['memo'][key] = _session_of_body(proc[3] or [], formals[0], ctx)
        finally:
            ctx['open'].discard(key)
    return ctx['memo'][key]


def check_sessions(branches, proc_signatures, program_procedures, lineno):
    """Per ogni canale con esattamente due endpoint fra le foglie del `par`,
    inferisce il tipo di sessione dei due lati e segnala i conflitti che sa
    dimostrare."""
    ctx = {
        'procedures': {p[1]: p for p in program_procedures
                       if isinstance(p, tuple) and p and p[0] == 'procedure'},
        'proc_params': {p[1]: (p[2] or []) for p in program_procedures
                        if isinstance(p, tuple) and p and p[0] == 'procedure'},
        'memo': {},
        'open': set(),
    }

    leaves = []
    for branch in branches:
        leaves.extend(_par_leaf_bodies(branch))

    touched = []
    for leaf in leaves:
        names = set()
        for stmt in leaf:
            _collect_channel_args_from_call(stmt, proc_signatures, names)
        touched.append(names)

    all_names = set()
    for names in touched:
        all_names |= names

    for ch in sorted(all_names):
        sides = [leaf for leaf, names in zip(leaves, touched) if ch in names]
        if len(sides) != 2:
            continue                            # gia' trattato dal conteggio endpoint
        s1 = _session_of_body(sides[0], ch, ctx)
        s2 = _session_of_body(sides[1], ch, ctx)
        if s1 == _S_END or s2 == _S_END:
            continue                            # canale solo passato, mai usato qui
        why = _session_clash(s1, s2)
        if why:
            raise KairosCompileError(
                "STATIC",
                (
                    f"riga {lineno}: i due thread usano il channel '{ch}' con protocolli non "
                    f"duali ({_session_str(s1)} contro {_session_str(s2)}): {why}; "
                    "a ogni ssend di un lato deve corrispondere un srecv dell'altro, "
                    "nello stesso ordine e con lo stesso numero di valori"
                ),
            )


