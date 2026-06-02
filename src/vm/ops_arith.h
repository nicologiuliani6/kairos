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
 *  POOL ops — heap puntatori Mnemo dinamico (vm->mn_pool).
 *  Sostituisce le celle statiche __mn_mem* del pool: array int64 che cresce
 *  on-demand (zero-filled, doubling), indicizzato a runtime → malloc-in-loop
 *  a bound runtime senza free funziona senza --ptr-pool-size.
 *  Operandi idx/val risolti come var-or-const via resolve_value.
 *  Coppie reversibili:
 *    POOLADD  pool[idx] += val        ↔ POOLSUB     pool[idx] -= val
 *    POOLGET  dst      += pool[idx]   ↔ POOLGETNEG  dst      -= pool[idx]
 *    POOLPUSH stk <- pool[idx]; pool[idx]=0  ↔ POOLPOP  pool[idx] += stk.pop()
 *  Store Mnemo = POOLPUSH(slot,hist) + POOLADD(slot,val); Load = POOLGET.
 * ====================================================================== */

static pthread_mutex_t mn_pool_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Ritorna &pool[idx], crescendo l'array zero-filled se serve. Chiamare sotto
 * mn_pool_mtx: realloc può spostare il buffer, quindi non tenere il puntatore
 * oltre il lock. */
static inline int64_t *mn_pool_at(VM *vm, int64_t idx)
{
    if (idx < 0) vm_debug_panic("[VM] POOL: indice negativo %lld\n", (long long)idx);
    if (idx >= vm->mn_pool_len) {
        long need = idx + 1;
        if (need > vm->mn_pool_cap) {
            long ncap = vm->mn_pool_cap ? vm->mn_pool_cap : 64;
            while (ncap < need) ncap *= 2;
            int64_t *np = (int64_t *)realloc(vm->mn_pool, (size_t)ncap * sizeof(int64_t));
            if (!np) vm_debug_panic("[VM] POOL: realloc fallita (%ld celle)\n", ncap);
            vm->mn_pool = np;
            vm->mn_pool_cap = ncap;
        }
        for (long k = vm->mn_pool_len; k < need; k++) vm->mn_pool[k] = 0;
        vm->mn_pool_len = need;
    }
    return &vm->mn_pool[idx];
}

static inline void op_pooladd(VM *vm, const char *frame_name)
{
    char *C_idx = strtok(NULL, " \t");
    char *C_val = strtok(NULL, " \t");
    if (!C_idx || !C_val) vm_debug_panic("[VM] POOLADD: 2 args (idx val)\n");
    uint fi = get_findex(frame_name);
    int64_t idx = resolve_value(vm, fi, C_idx);
    int64_t val = resolve_value(vm, fi, C_val);
    pthread_mutex_lock(&mn_pool_mtx);
    *mn_pool_at(vm, idx) += val;
    pthread_mutex_unlock(&mn_pool_mtx);
}

static inline void op_poolsub(VM *vm, const char *frame_name)
{
    char *C_idx = strtok(NULL, " \t");
    char *C_val = strtok(NULL, " \t");
    if (!C_idx || !C_val) vm_debug_panic("[VM] POOLSUB: 2 args (idx val)\n");
    uint fi = get_findex(frame_name);
    int64_t idx = resolve_value(vm, fi, C_idx);
    int64_t val = resolve_value(vm, fi, C_val);
    pthread_mutex_lock(&mn_pool_mtx);
    *mn_pool_at(vm, idx) -= val;
    pthread_mutex_unlock(&mn_pool_mtx);
}

static inline void op_poolget(VM *vm, const char *frame_name)
{
    char *C_idx = strtok(NULL, " \t");
    char *C_dst = strtok(NULL, " \t");
    if (!C_idx || !C_dst) vm_debug_panic("[VM] POOLGET: 2 args (idx dst)\n");
    uint fi = get_findex(frame_name);
    int64_t idx = resolve_value(vm, fi, C_idx);
    Var *dst = get_var(vm, fi, C_dst, "POOLGET");
    if (dst->T != TYPE_INT) vm_debug_panic("[VM] POOLGET: dst non INT\n");
    pthread_mutex_lock(&mn_pool_mtx);
    int64_t v = *mn_pool_at(vm, idx);
    pthread_mutex_unlock(&mn_pool_mtx);
    var_par_mut_acquire(dst);
    *(dst->value) += v;
    var_par_mut_release(dst);
}

static inline void op_poolgetneg(VM *vm, const char *frame_name)
{
    char *C_idx = strtok(NULL, " \t");
    char *C_dst = strtok(NULL, " \t");
    if (!C_idx || !C_dst) vm_debug_panic("[VM] POOLGETNEG: 2 args (idx dst)\n");
    uint fi = get_findex(frame_name);
    int64_t idx = resolve_value(vm, fi, C_idx);
    Var *dst = get_var(vm, fi, C_dst, "POOLGETNEG");
    if (dst->T != TYPE_INT) vm_debug_panic("[VM] POOLGETNEG: dst non INT\n");
    pthread_mutex_lock(&mn_pool_mtx);
    int64_t v = *mn_pool_at(vm, idx);
    pthread_mutex_unlock(&mn_pool_mtx);
    var_par_mut_acquire(dst);
    *(dst->value) -= v;
    var_par_mut_release(dst);
}

/* Ritorna la Var stack dato il suo nome, validando il tipo. */
static inline Var *mn_pool_stack_var(VM *vm, uint fi, const char *name, const char *op)
{
    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, name))
        vm_debug_panic("[VM] %s: stack '%s' non trovato!\n", op, name ? name : "(null)");
    uint si = char_id_map_get(&vm->frames[fi]->VarIndexer, name);
    Var *sv = vm->frames[fi]->vars[si];
    if (sv->T != TYPE_STACK)
        vm_debug_panic("[VM] %s: '%s' non e' stack (T=%d)\n", op, name, sv->T);
    return sv;
}

static inline void op_poolpush(VM *vm, const char *frame_name)
{
    char *C_idx = strtok(NULL, " \t");
    char *C_stk = strtok(NULL, " \t");
    if (!C_idx || !C_stk) vm_debug_panic("[VM] POOLPUSH: 2 args (idx stack)\n");
    uint fi = get_findex(frame_name);
    int64_t idx = resolve_value(vm, fi, C_idx);
    Var *sv = mn_pool_stack_var(vm, fi, C_stk, "POOLPUSH");
    pthread_mutex_lock(&mn_pool_mtx);
    int64_t *cell = mn_pool_at(vm, idx);
    int64_t v = *cell;
    *cell = 0;
    pthread_mutex_unlock(&mn_pool_mtx);
    sv->value = realloc(sv->value, (sv->stack_len + 1) * sizeof(int64_t));
    if (!sv->value) vm_debug_panic("realloc failed\n");
    sv->value[sv->stack_len++] = v;
}

static inline void op_poolpop(VM *vm, const char *frame_name)
{
    char *C_idx = strtok(NULL, " \t");
    char *C_stk = strtok(NULL, " \t");
    if (!C_idx || !C_stk) vm_debug_panic("[VM] POOLPOP: 2 args (idx stack)\n");
    uint fi = get_findex(frame_name);
    int64_t idx = resolve_value(vm, fi, C_idx);
    Var *sv = mn_pool_stack_var(vm, fi, C_stk, "POOLPOP");
    if (sv->stack_len == 0) vm_debug_panic("[VM] POOLPOP: stack vuoto!\n");
    int64_t v = sv->value[--sv->stack_len];
    if (sv->stack_len > 0)
        sv->value = realloc(sv->value, sv->stack_len * sizeof(int64_t));
    pthread_mutex_lock(&mn_pool_mtx);
    *mn_pool_at(vm, idx) += v;
    pthread_mutex_unlock(&mn_pool_mtx);
}

static inline void op_pooladd_inv(VM *vm, const char *frame_name) { op_poolsub(vm, frame_name); }
static inline void op_poolsub_inv(VM *vm, const char *frame_name) { op_pooladd(vm, frame_name); }
static inline void op_poolget_inv(VM *vm, const char *frame_name) { op_poolgetneg(vm, frame_name); }
static inline void op_poolgetneg_inv(VM *vm, const char *frame_name) { op_poolget(vm, frame_name); }
static inline void op_poolpush_inv(VM *vm, const char *frame_name) { op_poolpop(vm, frame_name); }
static inline void op_poolpop_inv(VM *vm, const char *frame_name) { op_poolpush(vm, frame_name); }

/* ======================================================================
 *  Inverse per UNCALL
 * ====================================================================== */
static inline void op_pusheq_inv(VM *vm, const char *frame_name) { op_mineq (vm, frame_name); }
static inline void op_mineq_inv (VM *vm, const char *frame_name) { op_pusheq(vm, frame_name); }
static inline void op_swap_inv  (VM *vm, const char *frame_name) { op_swap  (vm, frame_name); }
static inline void op_xoreq_inv(VM *vm, const char *frame_name) { op_xoreq(vm, frame_name); }