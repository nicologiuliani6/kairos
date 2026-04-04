#pragma once
/* ======================================================================
 *  ops_arith.h — Operazioni aritmetiche e di confronto della VM
 * ====================================================================== */

static inline void op_pusheq(VM *vm, const char *frame_name)
{
    char *ID     = strtok(NULL, " \t");
    char  expr[256]; read_rest_of_expr(expr, sizeof(expr));
    uint  Findex = get_findex(frame_name);
    Var  *v      = get_var(vm, Findex, ID, "PUSHEQ");
    if (v->T != TYPE_INT) { fprintf(stderr, "[VM] PUSHEQ non su INT!\n"); exit(EXIT_FAILURE); }
    *(v->value) += resolve_value(vm, Findex, expr);
}

static inline void op_mineq(VM *vm, const char *frame_name)
{
    char *ID     = strtok(NULL, " \t");
    char  expr[256]; read_rest_of_expr(expr, sizeof(expr));
    uint  Findex = get_findex(frame_name);
    Var  *v      = get_var(vm, Findex, ID, "MINEQ");
    if (v->T != TYPE_INT) { fprintf(stderr, "[VM] MINEQ non su INT!\n"); exit(EXIT_FAILURE); }
    *(v->value) -= resolve_value(vm, Findex, expr);
}

/* ======================================================================
 *  Inverse per UNCALL
 * ====================================================================== */
static inline void op_pusheq_inv(VM *vm, const char *frame_name) { op_mineq (vm, frame_name); }
static inline void op_mineq_inv (VM *vm, const char *frame_name) { op_pusheq(vm, frame_name); }
static inline void op_swap_inv  (VM *vm, const char *frame_name) { op_swap  (vm, frame_name); }