"""
Corpo inverso calcolato in compilazione: l'operatore Inv della tesi sull'AST.

L'inverso di un comando e' un altro comando, quindi per una procedura si puo'
produrre una seconda procedura `<nome>__inv` che eseguita in avanti fa quel che
`uncall <nome>` farebbe. Per la ricorsione e' la strada che regge: il motore di
inversione dinamico deve dedurre a tempo di esecuzione quante volte la procedura
si e' richiamata, mentre il corpo inverso lo sa gia' per costruzione.

Inv e' definito per induzione sulla struttura:
    Inv(c1 ; c2)           = Inv(c2) ; Inv(c1)
    Inv(v += e)            = v -= e         (e viceversa; ^= e <=> sono involuzioni)
    Inv(local v = m)       = delocal v = m  (e viceversa)
    Inv(call f)            = uncall f       (e viceversa)
    Inv(push)/Inv(pop)     = pop/push
    Inv(ssend)/Inv(srecv)  = srecv/ssend
    Inv(if b then c1 else c2 fi b')      = if b' then Inv(c1) else Inv(c2) fi b
    Inv(from b1 do c1 loop c2 until b2)  = from b2 do Inv(c1) loop Inv(c2) until b1
    Inv(par c1 and ... cn rap)           = par Inv(c1) and ... Inv(cn) rap
"""
from src.frontend.parser import from_second_body

INV_SUFFIX = "__inv"

_ASSIGN_INV = {'+=': '-=', '-=': '+=', '^=': '^=', '<=>': '<=>'}
_DIRECT_INV = {'push': 'pop', 'pop': 'push', 'ssend': 'srecv', 'srecv': 'ssend',
               'show': 'show', 'swap': 'swap'}


class NoInverse(Exception):
    """Il corpo contiene un comando di cui non si sa calcolare l'inverso."""


def _calls_in(stmts, out):
    """Nomi delle procedure chiamate (call/uncall/chiamata diretta) in `stmts`."""
    for s in stmts or []:
        if not isinstance(s, tuple) or not s:
            continue
        tag = s[0]
        if tag in ('call', 'uncall', 'call_direct'):
            out.add(s[1])
        elif tag == 'if':
            _calls_in(s[2], out); _calls_in(s[3], out)
        elif tag == 'from':
            _calls_in(s[2], out); _calls_in(from_second_body(s), out)
        elif tag == 'par':
            for b in s[1]:
                _calls_in(b, out)


def cyclic_procedures(procs):
    """Procedure che stanno su un ciclo del grafo delle chiamate (ricorsione, anche mutua)."""
    graph = {}
    for p in procs:
        if isinstance(p, tuple) and p and p[0] == 'procedure':
            callees = set()
            _calls_in(p[3], callees)
            graph[p[1]] = {c for c in callees if c != p[1] or True}
    def reaches(src, dst):
        seen, stack = set(), list(graph.get(src, ()))
        while stack:
            n = stack.pop()
            if n == dst:
                return True
            if n in seen or n not in graph:
                continue
            seen.add(n)
            stack.extend(graph[n])
        return False
    return {n for n in graph if reaches(n, n)}


def _inv_stmt(s, cyclic):
    tag = s[0]
    if tag == 'assign':
        _, var, op, expr, ln = s
        return ('assign', var, _ASSIGN_INV[op], expr, ln)
    if tag == 'local':
        _, tipo, name, val, ln = s
        return ('delocal', tipo, name, val, ln)
    if tag == 'delocal':
        _, tipo, name, val, ln = s
        if val is None:
            raise NoInverse("delocal senza valore")
        return ('local', tipo, name, val, ln)
    if tag == 'call':
        return ('uncall',) + s[1:]
    if tag == 'uncall':
        return ('call',) + s[1:]
    if tag == 'call_direct':
        _, name, args, ln = s
        low = name.lower()
        if low in _DIRECT_INV:
            return ('call_direct', _DIRECT_INV[low], args, ln)
        # chiamata diretta a una procedura utente (senza `call`): equivale a call
        return ('uncall', name, args, ln)
    if tag == 'if':
        _, ec, then_b, else_b, fc, ln = s
        return ('if', fc, inv_body(then_b, cyclic), inv_body(else_b, cyclic), ec, ln)
    if tag == 'from':
        ec, c1, uc = s[1], s[2], s[3]
        from_ln, until_ln = s[4], s[5]
        c2 = from_second_body(s)
        return ('from', uc, inv_body(c1, cyclic), ec, from_ln, until_ln, inv_body(c2, cyclic))
    if tag == 'par':
        _, branches, ln = s
        return ('par', [inv_body(b, cyclic) for b in branches], ln)
    raise NoInverse(f"nodo {tag}")


def inv_body(stmts, cyclic):
    """Inv di una sequenza: ordine rovesciato, ogni comando invertito. Le
    dichiarazioni `decl` non sono comandi: restano all'inizio, nell'ordine dato."""
    decls = [s for s in stmts if isinstance(s, tuple) and s and s[0] == 'decl']
    rest = [s for s in stmts if not (isinstance(s, tuple) and s and s[0] == 'decl')]
    return decls + [_inv_stmt(s, cyclic) for s in reversed(rest) if s is not None]


def inverse_procedure(proc, cyclic):
    """('procedure', nome, params, body, ln) -> la procedura `<nome>__inv`, o None."""
    try:
        body = inv_body(proc[3], cyclic)
    except NoInverse:
        return None
    return ('procedure', proc[1] + INV_SUFFIX, proc[2], body, proc[4])
