import sys
import ply.yacc as yacc
from .lexer import lexer, tokens
from .errors import KairosCompileError
import logging
import tempfile

VERBOSE = False

# ── Precedenza operatori ────────────────────────────────────────────────────
precedence = (
    ('left', 'PLUS', 'MINUS'),
)

# ── Programma ───────────────────────────────────────────────────────────────
def p_program(p):
    '''program : procedure_list'''
    p[0] = ('program', p[1])

def p_procedure_list(p):
    '''procedure_list : procedure
                      | procedure_list procedure'''
    p[0] = [p[1]] if len(p) == 2 else p[1] + [p[2]]

# ── Procedure ───────────────────────────────────────────────────────────────
def p_param_list(p):
    '''param_list : type ID
                  | param_list COMMA type ID'''
    if len(p) == 3:
        p[0] = [(p[1], p[2])]
    else:
        p[0] = p[1] + [(p[3], p[4])]

def p_procedure(p):
    '''procedure : PROCEDURE ID LPAREN RPAREN opt_body
                 | PROCEDURE ID LPAREN param_list RPAREN opt_body'''
    if len(p) == 6:
        p[0] = ('procedure', p[2], [], p[5], p.lineno(1))
        if VERBOSE: print(f"procedure: {p[2]}()")
    else:
        p[0] = ('procedure', p[2], p[4], p[6], p.lineno(1))
        if VERBOSE: print(f"procedure: {p[2]}({p[4]})")

# ── Body ────────────────────────────────────────────────────────────────────
def p_opt_body_empty(p):
    '''opt_body : '''
    p[0] = []

def p_opt_body_nonempty(p):
    '''opt_body : opt_body statement'''
    p[0] = p[1] + [p[2]]

# ── Tipi ────────────────────────────────────────────────────────────────────
def p_type(p):
    '''type : INT
            | STACK
            | CHANNEL'''
    p[0] = p[1]

# ── Espressioni ─────────────────────────────────────────────────────────────
def p_expr_binop(p):
    '''expr : expr PLUS expr
            | expr MINUS expr'''
    p[0] = ('binop', p[2], p[1], p[3])

def p_expr_paren(p):
    '''expr : LPAREN expr RPAREN'''
    p[0] = p[2]

def p_expr_atom(p):
    '''expr : NUMBER
            | ID'''
    p[0] = p[1]

# ── Valori letterali (per local/delocal) ────────────────────────────────────
def p_value(p):
    '''value : NUMBER
             | NIL
             | EMPT
             | ID'''
    p[0] = p[1]

# ── Operatori di confronto ──────────────────────────────────────────────────
def p_condition(p):
    '''condition : expr EQEQ expr
                 | expr NEQ  expr
                 | expr GEQ  expr
                 | expr LEQ  expr
                 | expr GT   expr
                 | expr LT   expr'''
    p[0] = ('cond', p[2], p[1], p[3])

# ── Dichiarazioni di tipo ───────────────────────────────────────────────────
def p_type_decl(p):
    '''statement : type ID'''
    # Usa la linea dell'identificatore: il non-terminale "type" non ha sempre lineno affidabile.
    p[0] = ('decl', p[1], p[2], p.lineno(2))
    if VERBOSE: print(f"dichiarazione: {p[2]} ({p[1]})")

# ── Assegnamenti reversibili ────────────────────────────────────────────────
def p_assign(p):
    '''statement : ID PLUSEQUALS expr
                 | ID MINUSEQUALS expr
                 | ID XOREQUALS expr
                 | ID SWAP expr'''
    p[0] = ('assign', p[1], p[2], p[3], p.lineno(1))

# ── Local / Delocal ─────────────────────────────────────────────────────────
def p_local(p):
    '''statement : LOCAL type ID EQUALS value'''
    p[0] = ('local', p[2], p[3], p[5], p.lineno(1))
    if VERBOSE: print(f"local: {p[3]} ({p[2]}) = {p[5]}")

def p_delocal(p):
    '''statement : DELOCAL type ID EQUALS value
                 | DELOCAL type ID'''
    if len(p) == 6:
        p[0] = ('delocal', p[2], p[3], p[5], p.lineno(1))
        if VERBOSE: print(f"delocal: {p[3]} ({p[2]}) = {p[5]}")
    else:
        p[0] = ('delocal', p[2], p[3], None, p.lineno(1))
        if VERBOSE: print(f"delocal: {p[3]} ({p[2]})")

# ── Liste argomenti ─────────────────────────────────────────────────────────
def p_arg_list(p):
    '''arg_list : ID
                | arg_list COMMA ID'''
    p[0] = [p[1]] if len(p) == 2 else p[1] + [p[3]]

def p_sendrecv_payload(p):
    '''sendrecv_payload : LT arg_list GT'''
    p[0] = p[2]

# ── Call / Uncall ───────────────────────────────────────────────────────────
def p_call(p):
    '''statement : CALL ID LPAREN RPAREN
                 | CALL ID LPAREN arg_list RPAREN'''
    if len(p) == 5:
        p[0] = ('call', p[2], [], p.lineno(1))
        if VERBOSE: print(f"call: {p[2]}()")
    else:
        p[0] = ('call', p[2], p[4], p.lineno(1))
        if VERBOSE: print(f"call: {p[2]}({p[4]})")

def p_call_direct(p):
    '''statement : ID LPAREN RPAREN
                 | ID LPAREN arg_list RPAREN
                 | ID LPAREN sendrecv_payload COMMA ID RPAREN'''
    if len(p) == 4:
        p[0] = ('call_direct', p[1], [], p.lineno(1))
        if VERBOSE: print(f"call diretto: {p[1]}()")
    elif len(p) == 5:
        p[0] = ('call_direct', p[1], p[3], p.lineno(1))
        if VERBOSE: print(f"call diretto: {p[1]}({p[3]})")
    else:
        name = p[1]
        if name.lower() not in ('ssend', 'srecv'):
            raise KairosCompileError(
                "PARSER",
                f"riga {p.lineno(1)}: sintassi '<...>' supportata solo per ssend/srecv",
            )
        args = p[3] + [p[5]]
        p[0] = ('call_direct', name, args, p.lineno(1))
        if VERBOSE: print(f"call diretto: {name}(<...>, {p[5]})")

def p_uncall(p):
    '''statement : UNCALL ID LPAREN RPAREN
                 | UNCALL ID LPAREN arg_list RPAREN'''
    if len(p) == 5:
        p[0] = ('uncall', p[2], [], p.lineno(1))
        if VERBOSE: print(f"uncall: {p[2]}()")
    else:
        p[0] = ('uncall', p[2], p[4], p.lineno(1))
        if VERBOSE: print(f"uncall: {p[2]}({p[4]})")

# ── FROM loop ───────────────────────────────────────────────────────────────
def p_from(p):
    '''statement : FROM condition LOOP opt_body UNTIL condition'''
    # Salviamo sia la linea di FROM che quella di UNTIL per il mapping breakpoint.
    p[0] = ('from', p[2], p[4], p[6], p.lineno(1), p.lineno(5))
    if VERBOSE: print(f"from: {p[2]} until: {p[6]}")

# ── IF / ELSE ───────────────────────────────────────────────────────────────
def p_if(p):
    '''statement : IF condition THEN opt_body FI condition
                 | IF condition THEN opt_body ELSE opt_body FI condition'''
    if len(p) == 7:
        p[0] = ('if', p[2], p[4], [], p[6], p.lineno(1))
        if VERBOSE: print(f"if: {p[2]} fi: {p[6]}")
    else:
        p[0] = ('if', p[2], p[4], p[6], p[8], p.lineno(1))
        if VERBOSE: print(f"if: {p[2]} else fi: {p[8]}")

# ── PAR ─────────────────────────────────────────────────────────────────────
def p_par_branch_list(p):
    '''par_branch_list : opt_body
                       | par_branch_list AND opt_body'''
    p[0] = [p[1]] if len(p) == 2 else p[1] + [p[3]]

def p_par(p):
    '''statement : PAR par_branch_list RAP'''
    p[0] = ('par', p[2], p.lineno(1))
    if VERBOSE: print(f"par: {p[2]}")

# ── Errore ───────────────────────────────────────────────────────────────────
def p_error(p):
    if p:
        raise KairosCompileError("PARSER", f"riga {p.lineno}: token non atteso '{p.value}'")
    else:
        raise KairosCompileError("PARSER", "errore sintattico: fine file inattesa")

def _expr_contains_var(expr, var_name):
    if isinstance(expr, tuple) and len(expr) == 4 and expr[0] == 'binop':
        return _expr_contains_var(expr[2], var_name) or _expr_contains_var(expr[3], var_name)
    return isinstance(expr, str) and expr == var_name


def _is_number_literal(expr_atom):
    if isinstance(expr_atom, int):
        return True
    if not isinstance(expr_atom, str) or not expr_atom:
        return False
    s = expr_atom
    if s[0] == '-':
        s = s[1:]
    return bool(s) and s.isdigit()


def _collect_ids_in_expr(expr, out):
    if isinstance(expr, int):
        return
    if isinstance(expr, tuple) and expr and expr[0] == 'binop':
        _collect_ids_in_expr(expr[2], out)
        _collect_ids_in_expr(expr[3], out)
        return
    if isinstance(expr, str):
        if _is_number_literal(expr):
            return
        if expr.lower() in ('nil', 'empty'):
            return
        out.add(expr)


def _collect_cond_var_ids(cond, out):
    _, _op, lhs, rhs = cond
    _collect_ids_in_expr(lhs, out)
    _collect_ids_in_expr(rhs, out)


def _collect_par_branch_var_uses(stmt, out):
    """Identificatori usati (lettura/scrittura) in un branch PAR, per analisi conflitti."""
    if not isinstance(stmt, tuple) or not stmt:
        return
    tag = stmt[0]
    if tag == 'decl':
        return
    if tag == 'assign':
        _, var_name, _op, expr, _lineno = stmt
        out.add(var_name)
        _collect_ids_in_expr(expr, out)
        return
    if tag == 'local':
        _, _t, name, val, _lineno = stmt
        out.add(name)
        if isinstance(val, str) and not _is_number_literal(val) and val.lower() not in ('nil', 'empty'):
            out.add(val)
        return
    if tag == 'delocal':
        _, _t, name, val, _lineno = stmt
        out.add(name)
        if val is not None and isinstance(val, str) and not _is_number_literal(val):
            if val.lower() not in ('nil', 'empty'):
                out.add(val)
        return
    if tag in ('call', 'uncall'):
        _, _name, args, _lineno = stmt
        for a in args or []:
            if isinstance(a, str):
                out.add(a)
        return
    if tag == 'call_direct':
        _, _name, args, _lineno = stmt
        for a in args or []:
            if isinstance(a, str):
                out.add(a)
        return
    if tag == 'if':
        _, entry_cond, then_body, else_body, fi_cond, _lineno = stmt
        _collect_cond_var_ids(entry_cond, out)
        for nested in then_body:
            _collect_par_branch_var_uses(nested, out)
        for nested in else_body:
            _collect_par_branch_var_uses(nested, out)
        _collect_cond_var_ids(fi_cond, out)
        return
    if tag == 'from':
        _, entry_cond, body, until_cond, *_rest = stmt
        _collect_cond_var_ids(entry_cond, out)
        for nested in body:
            _collect_par_branch_var_uses(nested, out)
        _collect_cond_var_ids(until_cond, out)
        return
    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _collect_par_branch_var_uses(nested, out)
        return


def _collect_par_branch_int_writes(
    stmt, declared_types, proc_param_lists, proc_int_mutated_formals, out
):
    """Scritture su int nel branch: dirette + tramite call a procedure che mutano parametri int."""
    if not isinstance(stmt, tuple) or not stmt:
        return
    tag = stmt[0]
    if tag == 'assign':
        _, var_name, op, expr, _lineno = stmt
        if op == '<=>':
            if declared_types.get(var_name) == 'int':
                out.add(var_name)
            if isinstance(expr, str) and declared_types.get(expr) == 'int':
                out.add(expr)
            return
        if declared_types.get(var_name) == 'int':
            out.add(var_name)
        return
    if tag == 'local':
        _, tipo, name, _val, _lineno = stmt
        if tipo == 'int':
            out.add(name)
        return
    if tag == 'delocal':
        _, tipo, name, _val, _lineno = stmt
        if tipo == 'int':
            out.add(name)
        return
    if tag == 'call_direct':
        _, name, args, _lineno = stmt
        lname = name.lower()
        if lname == 'swap' and isinstance(args, list) and len(args) >= 2:
            for vid in args[:2]:
                if isinstance(vid, str) and declared_types.get(vid) == 'int':
                    out.add(vid)
            return
        pl = proc_param_lists.get(name)
        mf = proc_int_mutated_formals.get(name) if pl else None
        if pl and mf:
            for i, actual in enumerate(args or []):
                if i >= len(pl):
                    break
                ptype, formal = pl[i]
                if ptype != 'int' or formal not in mf:
                    continue
                if isinstance(actual, str) and declared_types.get(actual) == 'int':
                    out.add(actual)
        return
    if tag in ('call', 'uncall'):
        _, proc_name, args, _lineno = stmt
        pl = proc_param_lists.get(proc_name)
        mf = proc_int_mutated_formals.get(proc_name) if pl else None
        if pl and mf:
            for i, actual in enumerate(args or []):
                if i >= len(pl):
                    break
                ptype, formal = pl[i]
                if ptype != 'int' or formal not in mf:
                    continue
                if isinstance(actual, str) and declared_types.get(actual) == 'int':
                    out.add(actual)
        return
    if tag == 'if':
        _, _ec, then_body, else_body, _fc, _lineno = stmt
        for nested in then_body:
            _collect_par_branch_int_writes(
                nested, declared_types, proc_param_lists, proc_int_mutated_formals, out
            )
        for nested in else_body:
            _collect_par_branch_int_writes(
                nested, declared_types, proc_param_lists, proc_int_mutated_formals, out
            )
        return
    if tag == 'from':
        _, _ec, body, _uc, *_rest = stmt
        for nested in body:
            _collect_par_branch_int_writes(
                nested, declared_types, proc_param_lists, proc_int_mutated_formals, out
            )
        return
    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _collect_par_branch_int_writes(
                    nested, declared_types, proc_param_lists, proc_int_mutated_formals, out
                )
        return


def _walk_proc_int_param_mutations_and_calls(stmt, int_formal_set, mutated_out, calls_out):
    """Raccoglie assegnamenti diretti ai parametri int e le call (per fixpoint)."""
    if not isinstance(stmt, tuple) or not stmt:
        return
    tag = stmt[0]
    if tag == 'assign':
        _, var_name, _op, _expr, _lineno = stmt
        if var_name in int_formal_set:
            mutated_out.add(var_name)
        return
    if tag in ('call', 'uncall'):
        _, proc_name, args, _lineno = stmt
        calls_out.append((proc_name, list(args or [])))
        return
    if tag == 'call_direct':
        _, name, args, _lineno = stmt
        if name.lower() == 'swap':
            return
        calls_out.append((name, list(args or [])))
        return
    if tag == 'if':
        _, _ec, then_body, else_body, _fc, _lineno = stmt
        for nested in then_body:
            _walk_proc_int_param_mutations_and_calls(
                nested, int_formal_set, mutated_out, calls_out
            )
        for nested in else_body:
            _walk_proc_int_param_mutations_and_calls(
                nested, int_formal_set, mutated_out, calls_out
            )
        return
    if tag == 'from':
        _, _ec, body, _uc, *_rest = stmt
        for nested in body:
            _walk_proc_int_param_mutations_and_calls(
                nested, int_formal_set, mutated_out, calls_out
            )
        return
    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _walk_proc_int_param_mutations_and_calls(
                    nested, int_formal_set, mutated_out, calls_out
                )
        return


def _compute_proc_int_mutated_formals(program_procedures):
    """
    Per ogni procedura, insieme dei nomi di parametri int che possono essere mutati
    (assegnamento diretto nel corpo o tramite call che mutano parametri int).
    """
    proc_by_name = {}
    for proc in program_procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        proc_by_name[proc[1]] = proc

    param_lists = {name: (proc_by_name[name][2] or []) for name in proc_by_name}
    int_formals = {
        name: {pn for pt, pn in param_lists[name] if pt == 'int'}
        for name in param_lists
    }

    mutated = {name: set() for name in param_lists}
    call_edges = {name: [] for name in param_lists}

    for name, p in proc_by_name.items():
        body = p[3] or []
        for stmt in body:
            _walk_proc_int_param_mutations_and_calls(
                stmt, int_formals[name], mutated[name], call_edges[name]
            )

    changed = True
    while changed:
        changed = False
        for p_name in param_lists:
            for callee, args in call_edges[p_name]:
                pl = param_lists.get(callee)
                if not pl:
                    continue
                callee_mut = mutated.get(callee)
                if not callee_mut:
                    continue
                for i, actual in enumerate(args):
                    if i >= len(pl):
                        break
                    pt, formal = pl[i]
                    if pt != 'int' or formal not in callee_mut:
                        continue
                    if not isinstance(actual, str):
                        continue
                    if actual not in int_formals[p_name]:
                        continue
                    if actual not in mutated[p_name]:
                        mutated[p_name].add(actual)
                        changed = True

    return param_lists, mutated


def _collect_declared_types(proc):
    declared = {}
    if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
        return declared

    params = proc[2] or []
    body = proc[3] or []
    for ptype, pname in params:
        declared[pname] = ptype

    def walk_stmt(stmt):
        if not isinstance(stmt, tuple) or not stmt:
            return
        tag = stmt[0]
        if tag in ('decl', 'local', 'delocal'):
            if len(stmt) >= 4:
                declared[stmt[2]] = stmt[1]
            return
        if tag == 'if':
            _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
            for nested in then_body:
                walk_stmt(nested)
            for nested in else_body:
                walk_stmt(nested)
            return
        if tag == 'from':
            _, _entry_cond, body, _until_cond, *_rest = stmt
            for nested in body:
                walk_stmt(nested)
            return
        if tag == 'par':
            _, branches, _lineno = stmt
            for branch in branches:
                for nested in branch:
                    walk_stmt(nested)

    for stmt in body:
        walk_stmt(stmt)
    return declared

def _collect_stack_endpoints_in_stmt(stmt, out):
    if not isinstance(stmt, tuple) or not stmt:
        return

    tag = stmt[0]
    if tag == 'call_direct':
        _, name, args, _lineno = stmt
        lname = name.lower()
        if lname in ('push', 'pop') and isinstance(args, list) and len(args) >= 2:
            out.add(args[1])
        return

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        for nested in then_body:
            _collect_stack_endpoints_in_stmt(nested, out)
        for nested in else_body:
            _collect_stack_endpoints_in_stmt(nested, out)
        return

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        for nested in body:
            _collect_stack_endpoints_in_stmt(nested, out)
        return

    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _collect_stack_endpoints_in_stmt(nested, out)

def _collect_stack_args_from_call(stmt, proc_signatures, out):
    if not isinstance(stmt, tuple) or not stmt:
        return

    tag = stmt[0]
    if tag in ('call', 'uncall'):
        _, proc_name, args, _lineno = stmt
        param_types = proc_signatures.get(proc_name, [])
        for idx, actual in enumerate(args or []):
            if idx < len(param_types) and param_types[idx] == 'stack' and isinstance(actual, str):
                out.add(actual)
        return

    if tag == 'call_direct':
        _, proc_name, args, _lineno = stmt
        lname = proc_name.lower()
        if lname in ('push', 'pop'):
            return
        param_types = proc_signatures.get(proc_name, [])
        for idx, actual in enumerate(args or []):
            if idx < len(param_types) and param_types[idx] == 'stack' and isinstance(actual, str):
                out.add(actual)
        return

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        for nested in then_body:
            _collect_stack_args_from_call(nested, proc_signatures, out)
        for nested in else_body:
            _collect_stack_args_from_call(nested, proc_signatures, out)
        return

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        for nested in body:
            _collect_stack_args_from_call(nested, proc_signatures, out)
        return

    if tag == 'par':
        _, branches, _lineno = stmt
        for branch in branches:
            for nested in branch:
                _collect_stack_args_from_call(nested, proc_signatures, out)

def _check_stmt_reversibility(
    stmt, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
):
    if not isinstance(stmt, tuple) or not stmt:
        return

    tag = stmt[0]
    if tag == 'assign':
        _, var_name, op, expr, lineno = stmt
        if op in ('+=', '-=', '^=') and _expr_contains_var(expr, var_name):
            raise KairosCompileError(
                "STATIC",
                (
                    f"riga {lineno}: operazione non reversibile '{var_name} {op} ...' "
                    f"(la variabile a sinistra compare anche nell'espressione a destra)"
                ),
            )
        return

    if tag == 'delocal':
        _, _tipo, name, val, lineno = stmt
        if isinstance(val, str) and val == name:
            raise KairosCompileError(
                "STATIC",
                (
                    f"riga {lineno}: DELOCAL non ammesso '{name} = {val}': il valore atteso "
                    f"non può essere lo stesso identificatore (vincolo banale, inversione non "
                    f"determinata). Usa un letterale o un altro nome di variabile."
                ),
            )
        return

    if tag == 'if':
        _, _entry_cond, then_body, else_body, _fi_cond, _lineno = stmt
        for nested in then_body:
            _check_stmt_reversibility(
                nested, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
        for nested in else_body:
            _check_stmt_reversibility(
                nested, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
        return

    if tag == 'from':
        _, _entry_cond, body, _until_cond, *_rest = stmt
        for nested in body:
            _check_stmt_reversibility(
                nested, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )
        return

    if tag == 'par':
        _, branches, lineno = stmt
        stack_usage_by_var = {}
        branch_access = []
        branch_direct_int_writes = []
        for branch_idx, branch in enumerate(branches):
            used_in_branch = set()
            par_ids = set()
            writes = set()
            for nested in branch:
                _collect_stack_endpoints_in_stmt(nested, used_in_branch)
                _collect_stack_args_from_call(nested, proc_signatures, used_in_branch)
                _collect_par_branch_var_uses(nested, par_ids)
                _collect_par_branch_int_writes(
                    nested, declared_types, proc_param_lists, proc_int_mutated_formals, writes
                )
                _check_stmt_reversibility(
                    nested, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
                )
            for var_name in used_in_branch:
                if declared_types.get(var_name) == 'stack':
                    stack_usage_by_var.setdefault(var_name, set()).add(branch_idx)
            branch_access.append(par_ids)
            branch_direct_int_writes.append(writes)

        for var_name, branch_ids in stack_usage_by_var.items():
            if len(branch_ids) > 1:
                branches_txt = ", ".join(str(i) for i in sorted(branch_ids))
                raise KairosCompileError(
                    "STATIC",
                    (
                        f"riga {lineno}: uso non reversibile di stack condiviso '{var_name}' "
                        f"in blocco PAR (branch: {branches_txt}); "
                        "usa channel/ssend/srecv o separa le strutture tra i branch"
                    ),
                )

        int_access_sets = [
            {v for v in acc if declared_types.get(v) == 'int'}
            for acc in branch_access
        ]
        for i in range(len(branch_direct_int_writes)):
            for j in range(i + 1, len(branch_direct_int_writes)):
                w_i = branch_direct_int_writes[i]
                w_j = branch_direct_int_writes[j]
                acc_i = int_access_sets[i]
                acc_j = int_access_sets[j]
                shared = (w_i & acc_j) | (w_j & acc_i)
                if shared:
                    names = ", ".join(sorted(shared))
                    raise KairosCompileError(
                        "STATIC",
                        (
                            f"riga {lineno}: race su int nel PAR (scrittura vs accesso, anche tramite call): "
                            f"{names} (usa channel/ssend/srecv o variabili distinte per branch)"
                        ),
                    )

def run_static_checks(program_ast):
    if not isinstance(program_ast, tuple) or not program_ast or program_ast[0] != 'program':
        raise KairosCompileError("PARSER", "AST del programma non valido")

    procedures = program_ast[1] if len(program_ast) > 1 else []
    proc_signatures = {}
    for proc in procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        proc_name = proc[1]
        params = proc[2] or []
        proc_signatures[proc_name] = [ptype for ptype, _pname in params]

    proc_param_lists, proc_int_mutated_formals = _compute_proc_int_mutated_formals(procedures)

    for proc in procedures:
        if not isinstance(proc, tuple) or len(proc) < 4 or proc[0] != 'procedure':
            continue
        declared_types = _collect_declared_types(proc)
        body = proc[3] or []
        for stmt in body:
            _check_stmt_reversibility(
                stmt, declared_types, proc_signatures, proc_param_lists, proc_int_mutated_formals
            )


_nulllog = logging.getLogger('ply.nulllog')
_nulllog.addHandler(logging.NullHandler())

parser = yacc.yacc(
    errorlog=_nulllog,
    debuglog=_nulllog,
    outputdir=tempfile.gettempdir(),
    debug=False,
)
if __name__ == '__main__':
    VERBOSE = True
    if len(sys.argv) < 2:
        print("Uso: python Jparser.py <file>")
        sys.exit(1)
    with open(sys.argv[1], 'r') as f:
        source = f.read()
    result = parser.parse(source, lexer=lexer)
    print(result)