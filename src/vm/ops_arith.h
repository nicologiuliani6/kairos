#pragma once
#include <string.h>
#include "vm_panic.h"
/* ======================================================================
 *  ops_arith.h — Operazioni aritmetiche e di confronto della VM
 * ====================================================================== */

static inline void op_pusheq(VM *vm, const char *frame_name)
{
    char *ID     = strtok(NULL, " \t");
    VM_REST_EXPR(expr);
    uint  Findex = get_findex(frame_name);
    Var  *v;
    int64_t *cell = lvalue_ptr(vm, Findex, ID, &v, "PUSHEQ");
    var_par_mut_acquire(v);
    *cell += resolve_value(vm, Findex, expr);
    var_par_mut_release(v);
}

/* `expr` è un'unica identificatore di variabile (no spazi, no operatori)? */
static inline int expr_is_bare_ident(const char *expr)
{
    if (!expr || !*expr) return 0;
    for (const char *p = expr; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    }
    return 1;
}

static inline void op_mineq(VM *vm, const char *frame_name)
{
    char *ID     = strtok(NULL, " \t");
    VM_REST_EXPR(expr);
    uint  Findex = get_findex(frame_name);
    Var  *v;
    int64_t *cell = lvalue_ptr(vm, Findex, ID, &v, "MINEQ");
    /* `a -= a` distrugge informazione (resta 0, inversa non recupera).
       Rifiuta staticamente quando lhs e rhs sono lo stesso identificatore. */
    if (expr_is_bare_ident(expr) && !strcmp(ID, expr))
        vm_debug_panic("[VM] MINEQ: `%s -= %s` non reversibile (perde informazione)\n", ID, expr);
    var_par_mut_acquire(v);
    /* UNCALL: saved_r+=r / ts+=t sono snapshot pre-corpo; r/t post-loop non sono il valore da sottrarre. */
    if (vm->inversion_depth > 0 && !strcmp(ID, "saved_r") && !strcmp(expr, "r")) {
        *cell = 0;
        var_par_mut_release(v);
        return;
    }
    if (vm->inversion_depth > 0 && !strcmp(ID, "ts") && !strcmp(expr, "t")) {
        *cell = 0;
        var_par_mut_release(v);
        return;
    }
    *cell -= resolve_value(vm, Findex, expr);
    var_par_mut_release(v);
}

static inline void op_xoreq(VM *vm, const char *frame_name)
{
    char *ID     = strtok(NULL, " \t");
    VM_REST_EXPR(expr);
    uint  Findex = get_findex(frame_name);
    Var  *v;
    int64_t *cell = lvalue_ptr(vm, Findex, ID, &v, "XOREQ");
    /* `a ^= a` azzera a (e l'inversa `a ^= a` lascia 0): informazione persa. */
    if (expr_is_bare_ident(expr) && !strcmp(ID, expr))
        vm_debug_panic("[VM] XOREQ: `%s ^= %s` non reversibile (perde informazione)\n", ID, expr);
    var_par_mut_acquire(v);
    *cell ^= resolve_value(vm, Findex, expr);
    var_par_mut_release(v);
}


/* ======================================================================
 *  Inverse per UNCALL
 * ====================================================================== */
static inline void op_pusheq_inv(VM *vm, const char *frame_name) { op_mineq (vm, frame_name); }
static inline void op_mineq_inv (VM *vm, const char *frame_name) { op_pusheq(vm, frame_name); }
static inline void op_swap_inv  (VM *vm, const char *frame_name) { op_swap  (vm, frame_name); }
static inline void op_xoreq_inv(VM *vm, const char *frame_name) { op_xoreq(vm, frame_name); }