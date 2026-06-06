#pragma once
/* Mnemo library procedures: O(1) forward + matching inverse when native arith is on. */

#include <stdint.h>
#include <string.h>
#include "vm_types.h"
#include "vm_helpers.h"
#include "vm_panic.h"

extern int g_vm_native_arith;

/* Risolve un parametro formale per nome (dst, a, …) dopo CALL: i Var* live sono
 * alias del caller e hanno nome __mn_mem*, non il formale — si usa il template. */
static inline int64_t *mn_formal_int(VM *vm, uint cfi, const char *formal)
{
    char proc[VAR_NAME_LENGTH];
    strncpy(proc, vm->frames[cfi]->name, sizeof(proc) - 1);
    proc[sizeof(proc) - 1] = '\0';
    char *at = strchr(proc, '@');
    if (at)
        *at = '\0';
    if (!char_id_map_exists(&FrameIndexer, proc))
        return NULL;
    uint bfi = char_id_map_get(&FrameIndexer, proc);
    if (!char_id_map_exists(&vm->frames[bfi]->VarIndexer, formal))
        return NULL;
    int formal_vi = char_id_map_get(&vm->frames[bfi]->VarIndexer, formal);
    for (int k = 0; k < vm->frames[cfi]->param_count; k++) {
        if (vm->frames[bfi]->param_indices[k] != formal_vi)
            continue;
        int live_vi = vm->frames[cfi]->param_indices[k];
        Var *v      = vm->frames[cfi]->vars[live_vi];
        if (!v || (v->T != TYPE_INT && v->T != TYPE_PARAM))
            return NULL;
        return v->value;
    }
    return NULL;
}

static inline Var *mn_formal_stack(VM *vm, uint cfi, const char *formal)
{
    char proc[VAR_NAME_LENGTH];
    strncpy(proc, vm->frames[cfi]->name, sizeof(proc) - 1);
    proc[sizeof(proc) - 1] = '\0';
    char *at = strchr(proc, '@');
    if (at)
        *at = '\0';
    if (!char_id_map_exists(&FrameIndexer, proc))
        return NULL;
    uint bfi = char_id_map_get(&FrameIndexer, proc);
    if (!char_id_map_exists(&vm->frames[bfi]->VarIndexer, formal))
        return NULL;
    int formal_vi = char_id_map_get(&vm->frames[bfi]->VarIndexer, formal);
    for (int k = 0; k < vm->frames[cfi]->param_count; k++) {
        if (vm->frames[bfi]->param_indices[k] != formal_vi)
            continue;
        Var *v = vm->frames[cfi]->vars[vm->frames[cfi]->param_indices[k]];
        if (!v || v->T != TYPE_STACK)
            return NULL;
        return v;
    }
    return NULL;
}

static inline void mn_hist_push(Var *hist, int64_t val)
{
    hist->value = realloc(hist->value, (hist->stack_len + 1) * sizeof(int64_t));
    if (!hist->value)
        vm_debug_panic("[VM] native arith: hist push OOM\n");
    hist->value[hist->stack_len++] = val;
}

static inline int64_t mn_hist_pop(Var *hist)
{
    if (!hist || hist->stack_len == 0)
        vm_debug_panic("[VM] native arith: hist pop su stack vuoto\n");
    int64_t v = hist->value[--hist->stack_len];
    if (hist->stack_len > 0)
        hist->value = realloc(hist->value, hist->stack_len * sizeof(int64_t));
    else {
        free(hist->value);
        hist->value = NULL;
    }
    return v;
}

static inline Var *mn_require_hist(VM *vm, uint cfi)
{
    Var *h = mn_formal_stack(vm, cfi, "__mn_hist");
    if (!h)
        vm_debug_panic("[VM] native arith: __mn_hist\n");
    return h;
}

/* Replay push(…, __mn_hist) from mnemo lib kairos (not delocal). O(1) math stays native;
 * hist must match interpreted CALL for uncall / vm_dump parity. */

static inline void mn_divmod_nonneg_hist_replay(Var *hist, int64_t a_orig, int64_t b)
{
  (void)b;
    mn_hist_push(hist, a_orig);
}

static inline void mn_divmod_nonneg_hist_undo(Var *hist)
{
    (void)mn_hist_pop(hist);
}

static inline void mn_floor_div2_signed_hist_replay(Var *hist, int64_t t_in)
{
    int64_t ts = t_in;
    if (ts >= 0) {
        mn_divmod_nonneg_hist_replay(hist, t_in, 2);
        mn_hist_push(hist, ts);
        mn_hist_push(hist, t_in % 2);
    } else {
        int64_t u = -t_in + 1;
        int64_t qv  = u / 2;
        int64_t rv  = u % 2;
        mn_divmod_nonneg_hist_replay(hist, u, 2);
        mn_hist_push(hist, qv);
        mn_hist_push(hist, 0);
        mn_hist_push(hist, -qv);
        mn_hist_push(hist, 0);
        mn_hist_push(hist, ts);
        mn_hist_push(hist, rv);
    }
}

/* `t_in` = il valore LIVE che il replay usò per decidere il ramo (NON il `ts`
 * poppato dalla hist). Il replay (mn_floor_div2_signed_hist_replay) sceglie
 * ramo >=0/<0 da `t_in`; l'undo DEVE usare lo stesso valore, altrimenti su
 * operandi int64 negativi il `ts` poppato può divergere dal `t_in` originale
 * (bug: 64-bit neg → ramo errato → push/pop count mismatch → hist underflow nel
 * successivo __mn_shr_into). I valori poppati sono scartati: conta solo il
 * NUMERO di pop per ramo (pos=3, neg=7), determinato da `t_in`. */
static inline void mn_floor_div2_signed_hist_undo(Var *hist, int64_t t_in)
{
    if (t_in >= 0) {
        mn_hist_pop(hist);   /* t_in % 2 */
        mn_hist_pop(hist);   /* ts       */
        mn_divmod_nonneg_hist_undo(hist);   /* a_orig */
    } else {
        mn_hist_pop(hist);   /* rv  */
        mn_hist_pop(hist);   /* ts  */
        mn_hist_pop(hist);   /* 0   */
        mn_hist_pop(hist);   /* -qv */
        mn_hist_pop(hist);   /* 0   */
        mn_hist_pop(hist);   /* qv  */
        mn_divmod_nonneg_hist_undo(hist);   /* u */
    }
}

static inline int64_t mn_floor_div2_q(int64_t t)
{
    if (t >= 0)
        return t / 2;
    return -(((-t) + 1) / 2);
}

static inline void mn_bit_k_signed_hist_replay(Var *hist, int64_t a_in, int64_t kk)
{
    int64_t t   = a_in;
    int64_t kp1 = kk + 1;
    for (int64_t i = 0; i < kp1; i++) {
        if (i != 0) {
            int64_t tb = t;
            mn_floor_div2_signed_hist_replay(hist, tb);
            int64_t q = mn_floor_div2_q(tb);
            t         = q;
            mn_hist_push(hist, q);
        }
    }
    int64_t ts = t;
    if (ts >= 0) {
        mn_divmod_nonneg_hist_replay(hist, ts, 2);
    } else {
        int64_t u = -ts;
        mn_divmod_nonneg_hist_replay(hist, u, 2);
        mn_hist_push(hist, u);
    }
    mn_hist_push(hist, ts);
    mn_hist_push(hist, (ts >= 0) ? (ts % 2) : (int64_t)((-ts) % 2));
    mn_hist_push(hist, (ts >= 0) ? (ts / 2) : (int64_t)((-ts) / 2));
    mn_hist_push(hist, kp1);
    mn_hist_push(hist, kp1);
    mn_hist_push(hist, 0);
}

static inline void mn_bit_k_signed_hist_undo(Var *hist, int64_t a_in, int64_t kk)
{
    int64_t t   = a_in;
    int64_t kp1 = kk + 1;
    for (int64_t i = 0; i < kp1; i++) {
        if (i != 0) {
            mn_hist_pop(hist);
            int64_t tb = t;
            mn_floor_div2_signed_hist_undo(hist, tb);
            t          = mn_floor_div2_q(tb);
        }
    }
    /* `t` ora == il `ts` LIVE che il replay usò (line 169). Decidi il ramo
     * neg col valore live, non col `ts` poppato (che su int64 neg diverge). */
    int64_t ts_live = t;
    mn_hist_pop(hist);
    mn_hist_pop(hist);
    mn_hist_pop(hist);
    mn_hist_pop(hist);
    mn_hist_pop(hist);
    mn_hist_pop(hist);   /* ts (sentinel, scartato) */
    if (ts_live < 0)
        mn_hist_pop(hist);
    mn_divmod_nonneg_hist_undo(hist);
}

static inline void mn_and_or_hist_replay(Var *hist, int64_t a, int64_t b, int is_or)
{
    int64_t pow = 1;
    for (int k = 0; k < 31; k++) {
        int64_t abit = (a >> k) & 1;
        int64_t bbit = (b >> k) & 1;
        mn_bit_k_signed_hist_replay(hist, a, k);
        mn_bit_k_signed_hist_replay(hist, b, k);
        int64_t s = abit + bbit;
        if (is_or) {
            if (s != 0)
                (void)0;
        } else if (s == 2)
            (void)0;
        mn_hist_push(hist, 0);
        int64_t next_pow = pow * 2;
        mn_hist_push(hist, pow);
        mn_hist_push(hist, 0);
        mn_hist_push(hist, bbit);
        mn_hist_push(hist, abit);
        pow = next_pow;
    }
    mn_hist_push(hist, pow);
    mn_hist_push(hist, 31);
}

static inline void mn_and_or_hist_undo(Var *hist, int64_t a, int64_t b)
{
    (void)a;
    (void)b;
    mn_hist_pop(hist);
    mn_hist_pop(hist);
    for (int k = 30; k >= 0; k--) {
        mn_hist_pop(hist);
        mn_hist_pop(hist);
        mn_hist_pop(hist);
        mn_hist_pop(hist);
        mn_hist_pop(hist);
        mn_bit_k_signed_hist_undo(hist, b, k);
        mn_bit_k_signed_hist_undo(hist, a, k);
    }
}

static inline void mn_shl_into_hist_replay(Var *hist, int64_t n)
{
    int64_t pow = 1;
    if (n > 0) {
        for (int64_t i = 0; i < n; i++) {
            int64_t next_pow = pow * 2;
            mn_hist_push(hist, pow);
            mn_hist_push(hist, 0);
            pow = next_pow;
        }
    }
    mn_hist_push(hist, n);
    mn_hist_push(hist, pow);
}

static inline void mn_shl_into_hist_undo(Var *hist, int64_t n)
{
    mn_hist_pop(hist);
    mn_hist_pop(hist);
    for (int64_t i = 0; i < n; i++) {
        mn_hist_pop(hist);
        mn_hist_pop(hist);
    }
}

static inline void mn_shr_into_hist_replay(Var *hist, int64_t x, int64_t n)
{
    int64_t t = x;
    if (n > 0) {
        for (int64_t i = 0; i < n; i++) {
            int64_t tb = t;
            mn_floor_div2_signed_hist_replay(hist, tb);
            int64_t q = mn_floor_div2_q(tb);
            t         = q;
            mn_hist_push(hist, q);
        }
    }
    mn_hist_push(hist, n);
    mn_hist_push(hist, t);
}

static inline void mn_shr_into_hist_undo(Var *hist, int64_t x, int64_t n)
{
    int64_t t = x;
    mn_hist_pop(hist);
    mn_hist_pop(hist);
    for (int64_t i = 0; i < n; i++) {
        mn_hist_pop(hist);
        int64_t tb = t;
        mn_floor_div2_signed_hist_undo(hist, tb);
        t          = mn_floor_div2_q(tb);
    }
}

static inline void mn_move_int_native(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *src = mn_formal_int(vm, cfi, "src");
    if (!dst || !src)
        vm_debug_panic("[VM] native __mn_move_int: param\n");
    *dst += *src;
    *src = 0;
}

static inline void mn_move_int_native_inv(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *src = mn_formal_int(vm, cfi, "src");
    if (!dst || !src)
        vm_debug_panic("[VM] native __mn_move_int inv: param\n");
    *src += *dst;
    *dst = 0;
}

static inline void mn_mul_into_native(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *a   = mn_formal_int(vm, cfi, "a");
    int64_t *b   = mn_formal_int(vm, cfi, "b");
    if (!dst || !a || !b)
        vm_debug_panic("[VM] native __mn_mul_into: param\n");
    if (*b < 0)
        vm_debug_panic("[VM] native __mn_mul_into: b < 0\n");
    *dst += (*a) * (*b);
}

static inline void mn_mul_into_native_inv(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *a   = mn_formal_int(vm, cfi, "a");
    int64_t *b   = mn_formal_int(vm, cfi, "b");
    if (!dst || !a || !b)
        vm_debug_panic("[VM] native __mn_mul_into inv: param\n");
    *dst -= (*a) * (*b);
}

static inline void mn_mul_signed_into_native(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *a   = mn_formal_int(vm, cfi, "a");
    int64_t *b   = mn_formal_int(vm, cfi, "b");
    if (!dst || !a || !b)
        vm_debug_panic("[VM] native __mn_mul_signed_into: param\n");
    *dst += (*a) * (*b);
}

static inline void mn_mul_signed_into_native_inv(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *a   = mn_formal_int(vm, cfi, "a");
    int64_t *b   = mn_formal_int(vm, cfi, "b");
    if (!dst || !a || !b)
        vm_debug_panic("[VM] native __mn_mul_signed_into inv: param\n");
    *dst -= (*a) * (*b);
}

static inline void mn_divmod_nonneg_native(VM *vm, uint cfi)
{
    int64_t *a = mn_formal_int(vm, cfi, "a");
    int64_t *b = mn_formal_int(vm, cfi, "b");
    int64_t *q = mn_formal_int(vm, cfi, "q");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    Var     *hist = mn_formal_stack(vm, cfi, "__mn_hist");
    if (!a || !b || !q || !r || !hist)
        vm_debug_panic("[VM] native __mn_divmod_nonneg: param\n");
    if (*b <= 0)
        vm_debug_panic("[VM] native __mn_divmod_nonneg: b <= 0\n");
    int64_t saved_r = *a;
    *r = *a;
    *a = 0;
    if (saved_r >= *b) {
        *q += *r / *b;
        *r = *r % *b;
    }
    mn_divmod_nonneg_hist_replay(hist, saved_r, *b);
}

static inline void mn_divmod_nonneg_native_inv(VM *vm, uint cfi)
{
    int64_t *a = mn_formal_int(vm, cfi, "a");
    int64_t *b = mn_formal_int(vm, cfi, "b");
    int64_t *q = mn_formal_int(vm, cfi, "q");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    Var     *hist = mn_formal_stack(vm, cfi, "__mn_hist");
    if (!a || !b || !q || !r || !hist)
        vm_debug_panic("[VM] native __mn_divmod_nonneg inv: param\n");
    (void)mn_hist_pop(hist);
    *a = (*q) * (*b) + (*r);
    *q = 0;
    *r = 0;
}

static inline void mn_divmod_signed_native(VM *vm, uint cfi)
{
    int64_t *a = mn_formal_int(vm, cfi, "a");
    int64_t *b = mn_formal_int(vm, cfi, "b");
    int64_t *q = mn_formal_int(vm, cfi, "q");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    if (!a || !b || !q || !r)
        vm_debug_panic("[VM] native __mn_divmod_signed: param\n");
    if (*b == 0)
        vm_debug_panic("[VM] native __mn_divmod_signed: b == 0\n");
    *q += *a / *b;
    *r += *a % *b;
    /* Kairos: solo abs_a in divmod_nonneg viene azzerato; il parametro a resta. */
}

static inline void mn_divmod_signed_native_inv(VM *vm, uint cfi)
{
    int64_t *q = mn_formal_int(vm, cfi, "q");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    if (!q || !r)
        vm_debug_panic("[VM] native __mn_divmod_signed inv: param\n");
    *q = 0;
    *r = 0;
}

static inline void mn_mod_nonneg_native(VM *vm, uint cfi)
{
    int64_t *a = mn_formal_int(vm, cfi, "a");
    int64_t *b = mn_formal_int(vm, cfi, "b");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    if (!a || !b || !r)
        vm_debug_panic("[VM] native __mn_mod_nonneg: param\n");
    if (*b <= 0)
        vm_debug_panic("[VM] native __mn_mod_nonneg: b <= 0\n");
    *r += *a;
    *a = 0;
    if (*r >= *b)
        *r %= *b;
}

static inline void mn_mod_nonneg_native_inv(VM *vm, uint cfi)
{
    int64_t *a = mn_formal_int(vm, cfi, "a");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    if (!a || !r)
        vm_debug_panic("[VM] native __mn_mod_nonneg inv: param\n");
    *a += *r;
    *r = 0;
}

static inline void mn_mod_signed_native(VM *vm, uint cfi)
{
    int64_t *a = mn_formal_int(vm, cfi, "a");
    int64_t *b = mn_formal_int(vm, cfi, "b");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    Var     *hist = mn_formal_stack(vm, cfi, "__mn_hist");
    if (!a || !b || !r || !hist)
        vm_debug_panic("[VM] native __mn_mod_signed: param\n");
    if (*b == 0)
        vm_debug_panic("[VM] native __mn_mod_signed: b == 0\n");
    int64_t rem = *a % *b;
    *r += rem;
    mn_hist_push(hist, rem);
}

static inline void mn_mod_signed_native_inv(VM *vm, uint cfi)
{
    int64_t *r = mn_formal_int(vm, cfi, "r");
    Var     *hist = mn_formal_stack(vm, cfi, "__mn_hist");
    if (!r || !hist)
        vm_debug_panic("[VM] native __mn_mod_signed inv: param\n");
    int64_t rem = mn_hist_pop(hist);
    *r -= rem;
}

static inline void mn_and_into_native(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *a   = mn_formal_int(vm, cfi, "a");
    int64_t *b   = mn_formal_int(vm, cfi, "b");
    if (!dst || !a || !b)
        vm_debug_panic("[VM] native __mn_and_into: param\n");
    *dst += (*a) & (*b);
    mn_and_or_hist_replay(mn_require_hist(vm, cfi), *a, *b, 0);
}

static inline void mn_and_into_native_inv(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *a   = mn_formal_int(vm, cfi, "a");
    int64_t *b   = mn_formal_int(vm, cfi, "b");
    if (!dst || !a || !b)
        vm_debug_panic("[VM] native __mn_and_into inv: param\n");
    mn_and_or_hist_undo(mn_require_hist(vm, cfi), *a, *b);
    *dst -= (*a) & (*b);
}

static inline void mn_or_into_native(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *a   = mn_formal_int(vm, cfi, "a");
    int64_t *b   = mn_formal_int(vm, cfi, "b");
    if (!dst || !a || !b)
        vm_debug_panic("[VM] native __mn_or_into: param\n");
    *dst += (*a) | (*b);
    mn_and_or_hist_replay(mn_require_hist(vm, cfi), *a, *b, 1);
}

static inline void mn_or_into_native_inv(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *a   = mn_formal_int(vm, cfi, "a");
    int64_t *b   = mn_formal_int(vm, cfi, "b");
    if (!dst || !a || !b)
        vm_debug_panic("[VM] native __mn_or_into inv: param\n");
    mn_and_or_hist_undo(mn_require_hist(vm, cfi), *a, *b);
    *dst -= (*a) | (*b);
}

static inline void mn_bit_k_signed_native(VM *vm, uint cfi)
{
    int64_t *a        = mn_formal_int(vm, cfi, "a");
    int64_t *kk       = mn_formal_int(vm, cfi, "kk");
    int64_t *bit_out  = mn_formal_int(vm, cfi, "bit_out");
    if (!a || !kk || !bit_out)
        vm_debug_panic("[VM] native __mn_bit_k_signed: param\n");
    if (*kk < 0)
        vm_debug_panic("[VM] native __mn_bit_k_signed: kk < 0\n");
    int64_t bit = (*a >> *kk) & 1;
    *bit_out += bit;
    mn_bit_k_signed_hist_replay(mn_require_hist(vm, cfi), *a, *kk);
}

static inline void mn_bit_k_signed_native_inv(VM *vm, uint cfi)
{
    int64_t *a       = mn_formal_int(vm, cfi, "a");
    int64_t *kk      = mn_formal_int(vm, cfi, "kk");
    int64_t *bit_out = mn_formal_int(vm, cfi, "bit_out");
    if (!a || !kk || !bit_out)
        vm_debug_panic("[VM] native __mn_bit_k_signed inv: param\n");
    mn_bit_k_signed_hist_undo(mn_require_hist(vm, cfi), *a, *kk);
    if (*bit_out != 0)
        *bit_out -= 1;
}

static inline void mn_floor_div2_signed_native(VM *vm, uint cfi)
{
    int64_t *t = mn_formal_int(vm, cfi, "t");
    int64_t *q = mn_formal_int(vm, cfi, "q");
    if (!t || !q)
        vm_debug_panic("[VM] native __mn_floor_div2_signed: param\n");
    int64_t tv = *t;
    int64_t qv;
    if (tv >= 0)
        qv = tv / 2;
    else
        qv = -(((-tv) + 1) / 2);
    *q += qv;
    *t = 0;
    mn_floor_div2_signed_hist_replay(mn_require_hist(vm, cfi), tv);
}

static inline void mn_floor_div2_signed_native_inv(VM *vm, uint cfi)
{
    int64_t *t = mn_formal_int(vm, cfi, "t");
    int64_t *q = mn_formal_int(vm, cfi, "q");
    if (!t || !q)
        vm_debug_panic("[VM] native __mn_floor_div2_signed inv: param\n");
    /* Ramo dell'undo deciso dal segno LIVE di t_in (come il replay), non da
     * valori poppati. q = floor_div2_q(t_in) ha lo stesso segno di t_in
     * (q>=0 ⟺ t_in>=0), quindi *q è un proxy di segno valido. */
    mn_floor_div2_signed_hist_undo(mn_require_hist(vm, cfi), *q);
    *t += (*q) * 2;
    *q = 0;
}

static inline void mn_divmod_nonneg_div2_native(VM *vm, uint cfi)
{
    int64_t *a = mn_formal_int(vm, cfi, "a");
    int64_t *q = mn_formal_int(vm, cfi, "q");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    if (!a || !q || !r)
        vm_debug_panic("[VM] native __mn_divmod_nonneg_div2: param\n");
    int64_t av = *a;
    *q += av / 2;
    *r += av % 2;
    *a = 0;
    mn_divmod_nonneg_hist_replay(mn_require_hist(vm, cfi), av, 2);
}

static inline void mn_divmod_nonneg_div2_native_inv(VM *vm, uint cfi)
{
    int64_t *a = mn_formal_int(vm, cfi, "a");
    int64_t *q = mn_formal_int(vm, cfi, "q");
    int64_t *r = mn_formal_int(vm, cfi, "r");
    if (!a || !q || !r)
        vm_debug_panic("[VM] native __mn_divmod_nonneg_div2 inv: param\n");
    mn_divmod_nonneg_hist_undo(mn_require_hist(vm, cfi));
    *a += (*q) * 2 + (*r);
    *q = 0;
    *r = 0;
}

static inline void mn_shl_into_native(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *x   = mn_formal_int(vm, cfi, "x");
    int64_t *n   = mn_formal_int(vm, cfi, "n");
    if (!dst || !x || !n)
        vm_debug_panic("[VM] native __mn_shl_into: param\n");
    if (*n < 0)
        vm_debug_panic("[VM] native __mn_shl_into: n < 0\n");
    if (*n > 63)
        vm_debug_panic("[VM] native __mn_shl_into: n troppo grande\n");
    /* n==0 → dst += x (x<<0). Il bytecode lascia pow=1 e fa mul_into(dst,x,1).
       Un guard `if n!=0` qui perdeva il termine `x<<0` (bug: `5<<0`→0). */
    *dst += (int64_t)((uint64_t)(*x) << *n);
    mn_shl_into_hist_replay(mn_require_hist(vm, cfi), *n);
}

static inline void mn_shl_into_native_inv(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *x   = mn_formal_int(vm, cfi, "x");
    int64_t *n   = mn_formal_int(vm, cfi, "n");
    if (!dst || !x || !n)
        vm_debug_panic("[VM] native __mn_shl_into inv: param\n");
    mn_shl_into_hist_undo(mn_require_hist(vm, cfi), *n);
    *dst -= (int64_t)((uint64_t)(*x) << *n);
}

static inline void mn_shr_into_native(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *x   = mn_formal_int(vm, cfi, "x");
    int64_t *n   = mn_formal_int(vm, cfi, "n");
    if (!dst || !x || !n)
        vm_debug_panic("[VM] native __mn_shr_into: param\n");
    if (*n < 0)
        vm_debug_panic("[VM] native __mn_shr_into: n < 0\n");
    if (*n > 63)
        vm_debug_panic("[VM] native __mn_shr_into: n > 63\n");
    /* Unsigned (logical) shift right: necessario per u32/u64 con high bit set.
     * Match con bytecode mnhalve-based __mn_shr_into in lib/bits.kairos. */
    int64_t shifted = (*n == 0) ? *x : (int64_t)((uint64_t)*x >> *n);
    *dst += shifted;
    mn_shr_into_hist_replay(mn_require_hist(vm, cfi), *x, *n);
}

static inline void mn_shr_into_native_inv(VM *vm, uint cfi)
{
    int64_t *dst = mn_formal_int(vm, cfi, "dst");
    int64_t *x   = mn_formal_int(vm, cfi, "x");
    int64_t *n   = mn_formal_int(vm, cfi, "n");
    if (!dst || !x || !n)
        vm_debug_panic("[VM] native __mn_shr_into inv: param\n");
    mn_shr_into_hist_undo(mn_require_hist(vm, cfi), *x, *n);
    int64_t shifted = (*n == 0) ? *x : (int64_t)((uint64_t)*x >> *n);
    *dst -= shifted;
}

typedef void (*mn_native_fn)(VM *, uint);

typedef struct {
    const char *name;
    mn_native_fn forward;
    mn_native_fn inverse;
} MnNativeProcEntry;

static const MnNativeProcEntry MN_NATIVE_PROCS[] = {
    {"__mn_move_int",           mn_move_int_native,           mn_move_int_native_inv},
    {"__mn_mul_into",           mn_mul_into_native,           mn_mul_into_native_inv},
    {"__mn_mul_signed_into",    mn_mul_signed_into_native,    mn_mul_signed_into_native_inv},
    {"__mn_divmod_nonneg",      mn_divmod_nonneg_native,      mn_divmod_nonneg_native_inv},
    {"__mn_divmod_signed",      mn_divmod_signed_native,      mn_divmod_signed_native_inv},
    {"__mn_mod_nonneg",         mn_mod_nonneg_native,         mn_mod_nonneg_native_inv},
    {"__mn_mod_signed",         mn_mod_signed_native,         mn_mod_signed_native_inv},
    {"__mn_and_into",           mn_and_into_native,           mn_and_into_native_inv},
    {"__mn_or_into",            mn_or_into_native,            mn_or_into_native_inv},
    {"__mn_bit_k_signed",       mn_bit_k_signed_native,       mn_bit_k_signed_native_inv},
    {"__mn_floor_div2_signed",  mn_floor_div2_signed_native,  mn_floor_div2_signed_native_inv},
    {"__mn_divmod_nonneg_div2", mn_divmod_nonneg_div2_native, mn_divmod_nonneg_div2_native_inv},
    {"__mn_shl_into",           mn_shl_into_native,           mn_shl_into_native_inv},
    /* __mn_shr_into rimosso da native: bytecode usa mnhalve (unsigned),
     * native_hist_replay esistente push valori del vecchio floor_div2_signed.
     * Mismatch → uncall corrompe hist. Bytecode loop n<=63 è veloce comunque. */
};

static inline const MnNativeProcEntry *mn_native_lookup(const char *proc)
{
    char base[VAR_NAME_LENGTH];
    strncpy(base, proc, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    char *at = strchr(base, '@');
    if (at) *at = '\0';
    for (size_t i = 0; i < sizeof(MN_NATIVE_PROCS) / sizeof(MN_NATIVE_PROCS[0]); i++) {
        if (!strcmp(base, MN_NATIVE_PROCS[i].name))
            return &MN_NATIVE_PROCS[i];
    }
    return NULL;
}

/* CALL fast path: 1 = handled (caller must pop frame), 0 = interpret bytecode */
static inline int mn_native_arith_call_forward(VM *vm, const char *proc, uint cfi)
{
    if (!g_vm_native_arith)
        return 0;
    const MnNativeProcEntry *e = mn_native_lookup(proc);
    if (!e || !e->forward)
        return 0;
    e->forward(vm, cfi);
    return 1;
}

/* UNCALL fast path: 1 = handled, 0 = invert_op_to_line */
static inline int mn_native_arith_uncall_inverse(VM *vm, const char *proc, uint cfi)
{
    if (!g_vm_native_arith)
        return 0;
    const MnNativeProcEntry *e = mn_native_lookup(proc);
    if (!e || !e->inverse)
        return 0;
    e->inverse(vm, cfi);
    return 1;
}
