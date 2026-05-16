#pragma once
#include <string.h>
#include "vm_panic.h"
/* ======================================================================
 *  ops_arith.h — Operazioni aritmetiche e di confronto della VM
 * ====================================================================== */

static inline void op_pusheq(VM *vm, const char *frame_name)
{
    char *ID     = strtok(NULL, " \t");
    char  expr[256]; read_rest_of_expr(expr, sizeof(expr));
    uint  Findex = get_findex(frame_name);
    Var  *v      = get_var(vm, Findex, ID, "PUSHEQ");
    if (v->T != TYPE_INT) { vm_debug_panic("[VM] PUSHEQ non su INT!\n");}
    var_par_mut_acquire(v);
    *(v->value) += resolve_value(vm, Findex, expr);
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
    char  expr[256]; read_rest_of_expr(expr, sizeof(expr));
    uint  Findex = get_findex(frame_name);
    Var  *v      = get_var(vm, Findex, ID, "MINEQ");
    if (v->T != TYPE_INT) { vm_debug_panic("[VM] MINEQ non su INT!\n"); }
    /* `a -= a` distrugge informazione (resta 0, inversa non recupera).
       Rifiuta staticamente quando lhs e rhs sono lo stesso identificatore. */
    if (expr_is_bare_ident(expr) && !strcmp(ID, expr))
        vm_debug_panic("[VM] MINEQ: `%s -= %s` non reversibile (perde informazione)\n", ID, expr);
    var_par_mut_acquire(v);
    /* UNCALL: saved_r+=r / ts+=t sono snapshot pre-corpo; r/t post-loop non sono il valore da sottrarre. */
    if (vm->inversion_depth > 0 && !strcmp(ID, "saved_r") && !strcmp(expr, "r")) {
        *(v->value) = 0;
        var_par_mut_release(v);
        return;
    }
    if (vm->inversion_depth > 0 && !strcmp(ID, "ts") && !strcmp(expr, "t")) {
        *(v->value) = 0;
        var_par_mut_release(v);
        return;
    }
    *(v->value) -= resolve_value(vm, Findex, expr);
    var_par_mut_release(v);
}

static inline void op_xoreq(VM *vm, const char *frame_name)
{
    char *ID     = strtok(NULL, " \t");
    char  expr[256]; read_rest_of_expr(expr, sizeof(expr));
    uint  Findex = get_findex(frame_name);
    Var  *v      = get_var(vm, Findex, ID, "XOREQ");
    if (v->T != TYPE_INT) { vm_debug_panic("[VM] XOREQ non su INT!\n"); }
    /* `a ^= a` azzera a (e l'inversa `a ^= a` lascia 0): informazione persa. */
    if (expr_is_bare_ident(expr) && !strcmp(ID, expr))
        vm_debug_panic("[VM] XOREQ: `%s ^= %s` non reversibile (perde informazione)\n", ID, expr);
    var_par_mut_acquire(v);
    *(v->value) ^= resolve_value(vm, Findex, expr);
    var_par_mut_release(v);
}


/* ======================================================================
 *  Inverse per UNCALL
 * ====================================================================== */
static inline void op_pusheq_inv(VM *vm, const char *frame_name) { op_mineq (vm, frame_name); }
static inline void op_mineq_inv (VM *vm, const char *frame_name) { op_pusheq(vm, frame_name); }
static inline void op_swap_inv  (VM *vm, const char *frame_name) { op_swap  (vm, frame_name); }
static inline void op_xoreq_inv(VM *vm, const char *frame_name) { op_xoreq(vm, frame_name); }