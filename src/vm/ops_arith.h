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
 *  MNHALVE  src  dst_q  dst_par
 *  Forward: dst_q += src/2 (floor toward zero); dst_par += src%2 (0 o 1); src = 0.
 *  Reversibile: inverse riempie src da dst_q*2+dst_par, azzera dst_q/dst_par.
 *  Permette halving O(1) per divmod sub-lineare (vs O(n) via sottrazione).
 * ====================================================================== */

static inline void op_mnhalve(VM *vm, const char *frame_name)
{
    char *ID_src = strtok(NULL, " \t");
    char *ID_q   = strtok(NULL, " \t");
    char *ID_p   = strtok(NULL, " \t");
    if (!ID_src || !ID_q || !ID_p)
        vm_debug_panic("[VM] MNHALVE: 3 args (src dst_q dst_par)\n");
    uint  Findex = get_findex(frame_name);
    Var  *vs = get_var(vm, Findex, ID_src, "MNHALVE");
    Var  *vq = get_var(vm, Findex, ID_q,   "MNHALVE");
    Var  *vp = get_var(vm, Findex, ID_p,   "MNHALVE");
    if (vs->T != TYPE_INT || vq->T != TYPE_INT || vp->T != TYPE_INT)
        vm_debug_panic("[VM] MNHALVE: tutti gli args devono essere INT\n");
    /* Semantica unsigned: necessario per stampa u64 (high bit set = neg signed)
     * e per estrarre bit pattern reale di int64. Per valori >= 0 unsigned e
     * signed coincidono, quindi safe per existing callers (divmod_fast,...). */
    uint64_t su = (uint64_t)*(vs->value);
    int64_t par = (int64_t)(su & 1ULL);
    int64_t qv = (int64_t)(su >> 1);
    *(vq->value) += qv;
    *(vp->value) += par;
    *(vs->value) = 0;
}

static inline void op_mnhalve_inv(VM *vm, const char *frame_name)
{
    char *ID_src = strtok(NULL, " \t");
    char *ID_q   = strtok(NULL, " \t");
    char *ID_p   = strtok(NULL, " \t");
    if (!ID_src || !ID_q || !ID_p)
        vm_debug_panic("[VM] MNHALVE_INV: 3 args\n");
    uint  Findex = get_findex(frame_name);
    Var  *vs = get_var(vm, Findex, ID_src, "MNHALVE_INV");
    Var  *vq = get_var(vm, Findex, ID_q,   "MNHALVE_INV");
    Var  *vp = get_var(vm, Findex, ID_p,   "MNHALVE_INV");
    uint64_t qu = (uint64_t)*(vq->value);
    uint64_t pu = (uint64_t)*(vp->value);
    *(vs->value) += (int64_t)((qu << 1) | (pu & 1ULL));
    *(vq->value) = 0;
    *(vp->value) = 0;
}

/* ======================================================================
 *  MNSPLIT32  src  dst_hi  dst_lo
 *  Forward: dst_hi += (u64(src) >> 32); dst_lo += (src & 0xFFFFFFFF); src = 0.
 *  Inverse: src += ((u64)dst_hi << 32) | (dst_lo & 0xFFFFFFFF); dst_hi = 0; dst_lo = 0.
 *  Permette stampa u64 hex via split + print(hi) + print(lo padded).
 * ====================================================================== */

static inline void op_mnsplit32(VM *vm, const char *frame_name)
{
    char *ID_src = strtok(NULL, " \t");
    char *ID_h   = strtok(NULL, " \t");
    char *ID_l   = strtok(NULL, " \t");
    if (!ID_src || !ID_h || !ID_l)
        vm_debug_panic("[VM] MNSPLIT32: 3 args (src dst_hi dst_lo)\n");
    uint  Findex = get_findex(frame_name);
    Var  *vs = get_var(vm, Findex, ID_src, "MNSPLIT32");
    Var  *vh = get_var(vm, Findex, ID_h,   "MNSPLIT32");
    Var  *vl = get_var(vm, Findex, ID_l,   "MNSPLIT32");
    if (vs->T != TYPE_INT || vh->T != TYPE_INT || vl->T != TYPE_INT)
        vm_debug_panic("[VM] MNSPLIT32: tutti gli args devono essere INT\n");
    uint64_t su = (uint64_t)*(vs->value);
    int64_t hi = (int64_t)(su >> 32);
    int64_t lo = (int64_t)(su & 0xFFFFFFFFULL);
    *(vh->value) += hi;
    *(vl->value) += lo;
    *(vs->value) = 0;
}

static inline void op_mnsplit32_inv(VM *vm, const char *frame_name)
{
    char *ID_src = strtok(NULL, " \t");
    char *ID_h   = strtok(NULL, " \t");
    char *ID_l   = strtok(NULL, " \t");
    if (!ID_src || !ID_h || !ID_l)
        vm_debug_panic("[VM] MNSPLIT32_INV: 3 args\n");
    uint  Findex = get_findex(frame_name);
    Var  *vs = get_var(vm, Findex, ID_src, "MNSPLIT32_INV");
    Var  *vh = get_var(vm, Findex, ID_h,   "MNSPLIT32_INV");
    Var  *vl = get_var(vm, Findex, ID_l,   "MNSPLIT32_INV");
    uint64_t hu = (uint64_t)*(vh->value);
    uint64_t lu = (uint64_t)*(vl->value);
    *(vs->value) += (int64_t)((hu << 32) | (lu & 0xFFFFFFFFULL));
    *(vh->value) = 0;
    *(vl->value) = 0;
}

/* ======================================================================
 *  Inverse per UNCALL
 * ====================================================================== */
static inline void op_pusheq_inv(VM *vm, const char *frame_name) { op_mineq (vm, frame_name); }
static inline void op_mineq_inv (VM *vm, const char *frame_name) { op_pusheq(vm, frame_name); }
static inline void op_swap_inv  (VM *vm, const char *frame_name) { op_swap  (vm, frame_name); }
static inline void op_xoreq_inv(VM *vm, const char *frame_name) { op_xoreq(vm, frame_name); }