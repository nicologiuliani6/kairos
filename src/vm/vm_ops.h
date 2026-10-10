#ifndef VM_OPS_H
#define VM_OPS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vm_types.h"
#include "vm_helpers.h"
#include "vm_channel.h"
#include "vm_ref_lock.h"
#include "vm_session.h"

#ifdef DAP_MODE
#include <unistd.h>
#include <stdarg.h>
extern VM *g_current_vm;
/* Output in DAP_MODE: il testo si formatta in un buffer della sua misura (prima
 * si misura, poi si scrive), quindi nessuna stampa viene troncata. */
static void vm_dap_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vm_dap_printf(const char *fmt, ...)
{
    VMDebugState *_d = g_current_vm ? g_current_vm->dbg : NULL;
    if (!_d || _d->suppress_output) return;
    va_list ap;
    va_start(ap, fmt);
    int _nw = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (_nw <= 0) return;
    char *_tmp = (char *)malloc((size_t)_nw + 1);
    if (!_tmp) return;
    va_start(ap, fmt);
    vsnprintf(_tmp, (size_t)_nw + 1, fmt, ap);
    va_end(ap);
    if (_d->output_pipe_fd > 0) {
        /* Canale real-time */
        (void)write(_d->output_pipe_fd, _tmp, (size_t)_nw);
    }
    /* Mantieni sempre anche il buffer interno come fallback/backup
       (alcuni client leggono vm_debug_output_ext invece della pipe
       in certi passaggi di stepback/revert). Cresce su richiesta. */
    pthread_mutex_lock(&_d->out_mtx);
    if (_d->out_len + _nw + 1 > _d->out_cap) {
        int nc = _d->out_cap ? _d->out_cap : DBG_OUT_INIT_CAP;
        while (nc < _d->out_len + _nw + 1) nc *= 2;
        char *nb = (char *)realloc(_d->out_buf, (size_t)nc);
        if (nb) { _d->out_buf = nb; _d->out_cap = nc; }
    }
    if (_d->out_len + _nw + 1 <= _d->out_cap) {
        memcpy(_d->out_buf + _d->out_len, _tmp, (size_t)_nw);
        _d->out_len += _nw;
        _d->out_buf[_d->out_len] = '\0';
    }
    pthread_mutex_unlock(&_d->out_mtx);
    free(_tmp);
}
#define vm_printf(...) vm_dap_printf(__VA_ARGS__)
#else
  #define vm_printf(...) printf(__VA_ARGS__)
#endif

/* Stack per-thread dei rami presi dagli IF in corso (per il controllo del FI
 * in op_assert). La profondità è quella di annidamento a run time, ricorsione
 * compresa: nessun massimo, i due vettori crescono raddoppiando. */
static __thread int *if_branch_stack          = NULL;
static __thread int *if_branch_has_call_stack = NULL;
static __thread int  if_branch_cap            = 0;
static __thread int  if_branch_top            = -1;

static inline void vm_if_reset_branch_stack(void)
{
    if_branch_top = -1;
}

static inline void vm_if_mark_call(void)
{
    if (if_branch_top >= 0)
        if_branch_has_call_stack[if_branch_top] = 1;
}

static inline void vm_if_push_branch(int took_then)
{
    if (if_branch_top + 1 >= if_branch_cap) {
        int nc = if_branch_cap ? if_branch_cap * 2 : 64;
        int *a = (int *)realloc(if_branch_stack, sizeof(int) * (size_t)nc);
        if (!a) vm_debug_panic("[VM] IF: memoria esaurita\n");
        if_branch_stack = a;
        int *b = (int *)realloc(if_branch_has_call_stack, sizeof(int) * (size_t)nc);
        if (!b) vm_debug_panic("[VM] IF: memoria esaurita\n");
        if_branch_has_call_stack = b;
        if_branch_cap = nc;
    }
    if_branch_stack[++if_branch_top] = took_then;
    if_branch_has_call_stack[if_branch_top] = 0;
}

/* Alla fine di un thread: i vettori sono __thread, li libera chi li ha creati. */
static inline void vm_if_free_branch_stack(void)
{
    free(if_branch_stack);
    free(if_branch_has_call_stack);
    if_branch_stack = if_branch_has_call_stack = NULL;
    if_branch_cap = 0;
    if_branch_top = -1;
}

#define CHANNEL_REF_MARKER (-1001)

/* Canali delocal'd ancora condivisi, da ripristinare se il LOCAL inverso li
 * ricrea. Vettore che cresce, nomi sull'heap: nessun numero massimo di voci
 * (prima oltre 4096 una voce si perdeva in silenzio). */
typedef struct {
    char    *proc;
    char    *var;
    Channel *channel;
} ChannelRestoreEntry;

static ChannelRestoreEntry *g_channel_restore     = NULL;
static int                  g_channel_restore_cap = 0;
static int                  g_channel_restore_top = 0;
static pthread_mutex_t g_channel_restore_mtx = PTHREAD_MUTEX_INITIALIZER;

static inline void channel_restore_push(const char *frame_name, const char *var_name, Channel *ch)
{
    if (!ch) return;
    VM_FRAME_BASE(proc, frame_name);
    char *p = char_id_strdup(proc), *v = char_id_strdup(var_name);
    pthread_mutex_lock(&g_channel_restore_mtx);
    if (g_channel_restore_top >= g_channel_restore_cap) {
        int nc = g_channel_restore_cap ? g_channel_restore_cap * 2 : 64;
        ChannelRestoreEntry *n = (ChannelRestoreEntry *)realloc(
            g_channel_restore, sizeof(ChannelRestoreEntry) * (size_t)nc);
        if (!n) {
            pthread_mutex_unlock(&g_channel_restore_mtx);
            vm_debug_panic("[VM] channel restore: memoria esaurita\n");
        }
        g_channel_restore = n;
        g_channel_restore_cap = nc;
    }
    g_channel_restore[g_channel_restore_top].proc    = p;
    g_channel_restore[g_channel_restore_top].var     = v;
    g_channel_restore[g_channel_restore_top].channel = ch;
    g_channel_restore_top++;
    pthread_mutex_unlock(&g_channel_restore_mtx);
}

static inline Channel *channel_restore_pop(const char *frame_name, const char *var_name)
{
    VM_FRAME_BASE(proc, frame_name);
    pthread_mutex_lock(&g_channel_restore_mtx);
    for (int i = g_channel_restore_top - 1; i >= 0; i--) {
        if (strcmp(g_channel_restore[i].proc, proc) == 0 &&
            strcmp(g_channel_restore[i].var, var_name) == 0) {
            Channel *ch = g_channel_restore[i].channel;
            free(g_channel_restore[i].proc);
            free(g_channel_restore[i].var);
            g_channel_restore[i] = g_channel_restore[g_channel_restore_top - 1];
            g_channel_restore_top--;
            pthread_mutex_unlock(&g_channel_restore_mtx);
            return ch;
        }
    }
    pthread_mutex_unlock(&g_channel_restore_mtx);
    return NULL;
}

static inline void lock_channel_pair(Channel *a, Channel *b)
{
    if (!a && !b) return;
    if (a == b) {
        if (a) pthread_mutex_lock(&a->mtx);
        return;
    }
    if (!a) { pthread_mutex_lock(&b->mtx); return; }
    if (!b) { pthread_mutex_lock(&a->mtx); return; }
    if ((uintptr_t)a < (uintptr_t)b) {
        pthread_mutex_lock(&a->mtx);
        pthread_mutex_lock(&b->mtx);
    } else {
        pthread_mutex_lock(&b->mtx);
        pthread_mutex_lock(&a->mtx);
    }
}

static inline void unlock_channel_pair(Channel *a, Channel *b)
{
    if (!a && !b) return;
    if (a == b) {
        if (a) pthread_mutex_unlock(&a->mtx);
        return;
    }
    if (!a) { pthread_mutex_unlock(&b->mtx); return; }
    if (!b) { pthread_mutex_unlock(&a->mtx); return; }
    if ((uintptr_t)a < (uintptr_t)b) {
        pthread_mutex_unlock(&b->mtx);
        pthread_mutex_unlock(&a->mtx);
    } else {
        pthread_mutex_unlock(&a->mtx);
        pthread_mutex_unlock(&b->mtx);
    }
}

/* ======================================================================
 *  SWAP
 * ====================================================================== */

void op_swap(VM *vm, const char *frame_name)
{
    char *ID1 = strtok(NULL, " \t"), *ID2 = strtok(NULL, " \t");
    uint  fi  = get_findex(frame_name);
    Var  *v1, *v2;
    int64_t *c1 = lvalue_ptr(vm, fi, ID1, &v1, "SWAP");
    int64_t *c2 = lvalue_ptr(vm, fi, ID2, &v2, "SWAP");
    var_par_mut_acquire2(v1, v2);
    int64_t tmp = *c1;
    *c1 = *c2;
    *c2 = tmp;
    var_par_mut_release2(v1, v2);
}

#include "ops_arith.h"

/* ======================================================================
 *  PUSH / POP (con supporto channel)
 * ====================================================================== */

static inline void op_push(VM *vm, const char *frame_name);
static inline void op_pop (VM *vm, const char *frame_name);
static inline void op_ssend(VM *vm, const char *frame_name);
static inline void op_srecv(VM *vm, const char *frame_name);

static inline void op_push(VM *vm, const char *frame_name)
{
    char *C_val   = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_debug_panic("[VM] PUSH: troppi parametri!\n");

    uint fi  = get_findex(frame_name);
    int64_t val;

    if (char_id_map_exists(&vm->frames[fi]->VarIndexer, C_val)) {
        Var *src = get_var(vm, fi, C_val, "PUSH");
        var_par_mut_acquire(src);
        val = *(src->value);
        *(src->value) = 0;
        var_par_mut_release(src);
    } else {
        val = (int64_t)strtoull(C_val, NULL, 10);
    }

    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, C_stack))
        vm_debug_panic("[VM] PUSH: stack destinazione non trovato!\n");

    uint  si = char_id_map_get(&vm->frames[fi]->VarIndexer, C_stack);
    Var  *sv = vm->frames[fi]->vars[si];
    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] PUSH: destinazione '%s' non e' stack/channel (T=%d) frame=%s\n",
            C_stack, sv->T, frame_name);

    if (sv->T == TYPE_STACK) {
        var_stack_push(sv, val);
    } else {
        pthread_mutex_lock(&sv->channel->mtx);
        sv->channel->buf = realloc(sv->channel->buf, (sv->channel->buf_len + 1) * sizeof(int64_t));
        if (!sv->channel->buf) vm_debug_panic("realloc failed\n");
        sv->channel->buf[sv->channel->buf_len++] = val;
        pthread_mutex_unlock(&sv->channel->mtx);
        int w = op_wait(sv->channel, 1);
        if (w == 1)
            wait_for_turn_done(current_thread_args);
    }
}

static inline void op_pop(VM *vm, const char *frame_name)
{
    char *C_dest  = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_debug_panic("[VM] POP: troppi parametri!\n");

    uint fi = get_findex(frame_name);
    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, C_stack))
        vm_debug_panic("[VM] POP: stack non trovato!\n");

    uint  si = char_id_map_get(&vm->frames[fi]->VarIndexer, C_stack);
    Var  *sv = vm->frames[fi]->vars[si];

    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL) vm_debug_panic("[VM] POP: sorgente non è stack/channel!\n");
    if (sv->T == TYPE_STACK && sv->stack_len == 0)
        vm_debug_panic("[VM] POP: stack vuoto! (frame=%s dest=%s stack=%s inv=%d)\n",
                       frame_name, C_dest, C_stack, vm->inversion_depth);

    ThreadArgs *sender_to_wake = NULL;
    int64_t       popped;

    if (sv->T == TYPE_CHANNEL) {
        /* op_wait abbina questo recv al sender giusto e imposta sender_args.
           Non leggere send_q_head prima: con più SSEND paralleli la coda dei
           waiter non coincide col FIFO del buffer; notificare il thread
           sbagliato corrompeva lo stato (es. malloc.kairos intermittente).
           Leggere sender_args e fare il pop FIFO sotto lo stesso mtx: altrimenti
           un altro SSEND può intercalare tra le due e far leggere una cella
           non ancora valorizzata (buffer malloc non azzerato). */
        op_wait(sv->channel, 0);
        pthread_mutex_lock(&sv->channel->mtx);
        sender_to_wake = sv->channel->sender_args;
        sv->channel->sender_args = NULL;
        if (!sender_to_wake) {
            pthread_mutex_unlock(&sv->channel->mtx);
            vm_debug_panic("[VM] POP: channel sender_args nullo dopo rendezvous\n");
        }
        if (sv->channel->buf_len == 0) {
            pthread_mutex_unlock(&sv->channel->mtx);
            vm_debug_panic("[VM] POP: channel vuoto dopo op_wait!\n");
        }
        popped = sv->channel->buf[0];
        sv->channel->buf_len--;
        if (sv->channel->buf_len > 0)
            memmove(sv->channel->buf, sv->channel->buf + 1, sv->channel->buf_len * sizeof(int64_t));
        if (sv->channel->buf_len > 0)
            sv->channel->buf = realloc(sv->channel->buf, sv->channel->buf_len * sizeof(int64_t));
        pthread_mutex_unlock(&sv->channel->mtx);
    } else {
        popped = sv->value[--sv->stack_len];   /* la capacità resta per i push */
    }

    Var *dest = get_var(vm, fi, C_dest, "POP");
    var_par_mut_acquire(dest);
    /* Pop-Err2 (convenzione zero-cleared): pop(v, s) richiede v == 0 prima
       dell'operazione — è l'esatto invariante che push(v, s) garantisce
       azzerando la sorgente al momento del push. Se v != 0 qui, += perderebbe
       silenziosamente il vecchio valore di v: l'operazione smetterebbe di
       essere invertibile (push, l'inverso di pop, non potrebbe più
       ricostruirlo). Vale anche dentro un'inversione. */
    if (dest->T == TYPE_INT && *(dest->value) != 0) {
        var_par_mut_release(dest);
        vm_debug_panic(
            "[VM] POP: destinazione '%s' non è zero prima del pop (Pop-Err2, valore attuale=%lld) frame=%s\n",
            C_dest, (long long)*(dest->value), frame_name);
    }
    *(dest->value) += popped;
    var_par_mut_release(dest);

    if (sv->T == TYPE_CHANNEL && sender_to_wake)
        notify_sender_turn_done(sender_to_wake);
}

/* Token di SSEND/SRECV (valori del payload più il canale): quanti sono. Sullo
 * stack del C finché sono pochi, sull'heap oltre. I token puntano nella riga
 * corrente, che resta viva per tutta l'istruzione. */
#define VM_TOKV_SMALL 16
#define VM_TOKV_COLLECT(tokv, ntok) \
    char *tokv##_small[VM_TOKV_SMALL]; char **tokv = tokv##_small; \
    int tokv##_cap = VM_TOKV_SMALL, ntok = 0; \
    for (char *_t; (_t = strtok(NULL, " \t")); ) { \
        if (ntok == tokv##_cap) { \
            char **_n = (char **)malloc(sizeof(char *) * (size_t)tokv##_cap * 2); \
            if (!_n) vm_debug_panic("[VM] payload: memoria esaurita\n"); \
            memcpy(_n, tokv, sizeof(char *) * (size_t)ntok); \
            if (tokv != tokv##_small) free(tokv); \
            tokv = _n; tokv##_cap *= 2; \
        } \
        tokv[ntok++] = _t; \
    }
#define VM_TOKV_FREE(tokv) do { if (tokv != tokv##_small) free(tokv); } while (0)

static inline void op_ssend(VM *vm, const char *frame_name)
{
    VM_TOKV_COLLECT(tokv, ntok);

    if (ntok < 2)
        vm_debug_panic("[VM] SSEND: formato errato (atteso SSEND <v1 ...> <channel>)\n");

    char *ch_name = tokv[ntok - 1];
    int payload_count = ntok - 1;
    uint fi = get_findex(frame_name);

    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, ch_name))
        vm_debug_panic("[VM] SSEND: canale non trovato!\n");
    uint chi = char_id_map_get(&vm->frames[fi]->VarIndexer, ch_name);
    Var *chv = vm->frames[fi]->vars[chi];
    if (!chv || chv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] SSEND: destinazione non è channel!\n");
    session_acquire(chv->channel, ch_name);

    int encoded_len = 0;
    int encoded_cap = 16;
    int64_t *encoded = malloc((size_t)encoded_cap * sizeof(int64_t));
    if (!encoded)
        vm_debug_panic("[VM] SSEND: malloc fallita\n");

#define ENC_PUSH(_v) do { \
        if (encoded_len >= encoded_cap) { \
            encoded_cap *= 2; \
            int64_t *tmp = realloc(encoded, (size_t)encoded_cap * sizeof(int64_t)); \
            if (!tmp) { free(encoded); vm_debug_panic("realloc failed\n"); } \
            encoded = tmp; \
        } \
        encoded[encoded_len++] = (_v); \
    } while (0)

    for (int i = 0; i < payload_count; i++) {
        char *src_tok = tokv[i];
        if (char_id_map_exists(&vm->frames[fi]->VarIndexer, src_tok)) {
            Var *src = get_var(vm, fi, src_tok, "SSEND");
            if (src == chv) {
                free(encoded);
                vm_debug_panic("[VM] SSEND: non puoi inviare il canale su se stesso\n");
            }
            if (src->T == TYPE_INT) {
                int64_t val;
                var_par_mut_acquire(src);
                val = *(src->value);
                *(src->value) = 0;
                var_par_mut_release(src);
                ENC_PUSH((int)TYPE_INT);
                ENC_PUSH(val);
            } else if (src->T == TYPE_STACK) {
                size_t n = src->stack_len;
                ENC_PUSH((int)TYPE_STACK);
                ENC_PUSH((int64_t)n);       /* lunghezza intera, non troncata a int */
                for (size_t k = 0; k < n; k++)
                    ENC_PUSH(src->value[k]);
                src->stack_len = 0;
            } else if (src->T == TYPE_CHANNEL) {
                session_delegate_release(src->channel, src_tok);
                uintptr_t p = (uintptr_t)src->channel;
                ENC_PUSH(CHANNEL_REF_MARKER);
                ENC_PUSH((int)(uint32_t)(p & 0xffffffffu));
                ENC_PUSH((int)(uint32_t)((p >> 32) & 0xffffffffu));
            } else {
                free(encoded);
                vm_debug_panic("[VM] SSEND: parametro non linkato\n");
            }
        } else {
            ENC_PUSH((int)TYPE_INT);
            /* Valore a 64 bit come nel ramo variabile: (int)strtoul lo troncava. */
            ENC_PUSH((int64_t)strtoull(src_tok, NULL, 10));
        }
    }

    pthread_mutex_lock(&chv->channel->mtx);
    if (encoded_len > 0) {
        chv->channel->buf = realloc(chv->channel->buf, (chv->channel->buf_len + (size_t)encoded_len) * sizeof(int64_t));
        if (!chv->channel->buf) {
            pthread_mutex_unlock(&chv->channel->mtx);
            free(encoded);
            vm_debug_panic("realloc failed\n");
        }
        memcpy(chv->channel->buf + chv->channel->buf_len, encoded, (size_t)encoded_len * sizeof(int64_t));
        chv->channel->buf_len += (size_t)encoded_len;
    }
    pthread_mutex_unlock(&chv->channel->mtx);
    free(encoded);
    VM_TOKV_FREE(tokv);

#undef ENC_PUSH

    int w = op_wait(chv->channel, 1);
    if (w == 1)
        wait_for_turn_done(current_thread_args);
}

static inline void op_srecv(VM *vm, const char *frame_name)
{
    VM_TOKV_COLLECT(tokv, ntok);

    if (ntok < 2)
        vm_debug_panic("[VM] SRECV: formato errato (atteso SRECV <v1 ...> <channel>)\n");

    char *ch_name = tokv[ntok - 1];
    int recv_count = ntok - 1;
    uint fi = get_findex(frame_name);

    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, ch_name))
        vm_debug_panic("[VM] SRECV: channel non trovato!\n");
    uint chi = char_id_map_get(&vm->frames[fi]->VarIndexer, ch_name);
    Var *chv = vm->frames[fi]->vars[chi];
    if (!chv || chv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] SRECV: sorgente non è channel!\n");
    session_acquire(chv->channel, ch_name);

    op_wait(chv->channel, 0);

    pthread_mutex_lock(&chv->channel->mtx);
    ThreadArgs *sender_to_wake = chv->channel->sender_args;
    chv->channel->sender_args = NULL;
    if (!sender_to_wake) {
        pthread_mutex_unlock(&chv->channel->mtx);
        vm_debug_panic("[VM] SRECV: sender_args nullo dopo rendezvous\n");
    }
    size_t read_idx = 0;
    for (int i = 0; i < recv_count; i++) {
        Var *dest = get_var(vm, fi, tokv[i], "SRECV");
        if (read_idx >= chv->channel->buf_len) {
            pthread_mutex_unlock(&chv->channel->mtx);
            vm_debug_panic("[VM] SRECV: payload insufficiente sul channel\n");
        }
        int marker = chv->channel->buf[read_idx++];
        if (marker == (int)TYPE_INT) {
            if (read_idx >= chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: payload int incompleto\n");
            }
            int64_t popped = chv->channel->buf[read_idx++];   /* era int: troncava i valori a 32 bit */
            if (dest->T != TYPE_INT) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: payload int richiede destinazione int\n");
            }
            var_par_mut_acquire(dest);
            /* Srecv-Err (convenzione zero-cleared): srecv(<w...>, ch) richiede
               ogni w già a 0 prima della ricezione — è l'invariante che ssend
               garantisce azzerando la sorgente al momento dell'invio. Se w != 0
               qui, += perderebbe silenziosamente il vecchio valore: l'operazione
               smetterebbe di essere invertibile (ssend, l'inverso di srecv, non
               potrebbe più ricostruirlo). */
            if (*(dest->value) != 0) {
                var_par_mut_release(dest);
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic(
                    "[VM] SRECV: destinazione '%s' non è zero prima della ricezione (Srecv-Err, valore attuale=%lld)\n",
                    tokv[i], (long long)*(dest->value));
            }
            *(dest->value) += popped;
            var_par_mut_release(dest);
        } else if (marker == CHANNEL_REF_MARKER) {
            if (read_idx + 1 >= chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: payload channel-ref incompleto\n");
            }
            uint32_t lo = (uint32_t)chv->channel->buf[read_idx++];
            uint32_t hi = (uint32_t)chv->channel->buf[read_idx++];
            uintptr_t p = ((uintptr_t)hi << 32) | (uintptr_t)lo;
            Channel *shared = (Channel *)p;
            if (dest->T != TYPE_CHANNEL || !shared) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: channel-ref richiede destinazione channel valida\n");
            }
            session_delegate_acquire(shared, tokv[i]);
            if (dest->channel != shared) {
                Channel *old = dest->channel;
                lock_channel_pair(old, shared);
                if (old) old->refcount--;
                shared->refcount++;
                unlock_channel_pair(old, shared);
                dest->channel = shared;
                if (old) {
                    int do_free = 0;
                    pthread_mutex_lock(&old->mtx);
                    do_free = (old->refcount <= 0);
                    pthread_mutex_unlock(&old->mtx);
                    if (do_free) {
                        pthread_mutex_destroy(&old->mtx);
                        free(old->buf);
                        free(old);
                    }
                }
            }
        } else if (marker == (int)TYPE_STACK || marker == (int)TYPE_CHANNEL) {
            if (read_idx >= chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: payload collezione incompleto\n");
            }
            int64_t n = chv->channel->buf[read_idx++];
            if (n < 0 || read_idx + (size_t)n > chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: lunghezza payload non valida\n");
            }
            if (dest->T != TYPE_STACK && dest->T != TYPE_CHANNEL) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: payload stack/channel richiede destinazione stack o channel\n");
            }
            if (n > 0) {
                var_stack_reserve(dest, dest->stack_len + (size_t)n);
                memcpy(dest->value + dest->stack_len, chv->channel->buf + read_idx, (size_t)n * sizeof(int64_t));
                dest->stack_len += (size_t)n;
            }
            read_idx += (size_t)n;
        } else {
            pthread_mutex_unlock(&chv->channel->mtx);
            vm_debug_panic("[VM] SRECV: marker payload sconosciuto\n");
        }
    }

    size_t remaining = chv->channel->buf_len - read_idx;
    if (remaining > 0)
        memmove(chv->channel->buf, chv->channel->buf + read_idx, remaining * sizeof(int64_t));
    chv->channel->buf_len = remaining;
    if (remaining > 0) {
        chv->channel->buf = realloc(chv->channel->buf, remaining * sizeof(int64_t));
        if (!chv->channel->buf) {
            pthread_mutex_unlock(&chv->channel->mtx);
            vm_debug_panic("realloc failed\n");
        }
    }
    pthread_mutex_unlock(&chv->channel->mtx);

    notify_sender_turn_done(sender_to_wake);
    VM_TOKV_FREE(tokv);
}

/* ======================================================================
 *  SHOW
 * ====================================================================== */

static inline void op_show_flush_char_line(VM *vm)
{
    if (vm->show_char_pending) {
        vm_printf("\n");
        vm->show_char_pending = 0;
    }
}

static inline void op_show(VM *vm, const char *frame_name)
{
    if (vm->suppress_show) return;
    char *ID = strtok(NULL, " \t");
    if (!ID) vm_debug_panic("[VM] SHOW: manca il nome della variabile\n");
    char *fmt = strtok(NULL, " \t");
    char *more = strtok(NULL, " \t");
    if (more) vm_debug_panic("[VM] SHOW: troppi parametri!\n");

    int as_char = 0;
    if (fmt) {
        if (strcmp(fmt, "char") != 0) {
            vm_debug_panic(
                "[VM] SHOW: secondo argomento non riconosciuto "
                "(solo 'char' oppure omettere per formato classico)\n"
            );
        }
        as_char = 1;
    }

    uint fi = get_findex(frame_name);
    Var *v  = get_var(vm, fi, ID, "SHOW");

    if (v->T == TYPE_INT) {
        if (as_char) {
            /* Un solo byte in stdout, senza nome variabile, apici né newline (base per printf). */
            unsigned char c = (unsigned char)(*(v->value) & 0xFF);
            vm_printf("%c", c);
            vm->show_char_pending = 1;
        } else {
            op_show_flush_char_line(vm);
            vm_printf("%s: %lld\n", ID, (long long)*(v->value));
        }
    } else if (v->T == TYPE_STACK || v->T == TYPE_ARRAY) {
        if (as_char)
            vm_debug_panic("[VM] SHOW char: supportato solo per int\n");
        op_show_flush_char_line(vm);
        char open = '[';
        char clos = ']';
        vm_printf("%s: %c", ID, open);
        for (size_t k = 0; k < v->stack_len; k++) {
            vm_printf("%lld", (long long)v->value[k]);
            if (k + 1 < v->stack_len) vm_printf(", ");
        }
        vm_printf("%c\n", clos);
    } else if (v->T == TYPE_CHANNEL) {
        if (as_char)
            vm_debug_panic("[VM] SHOW char: supportato solo per int\n");
        op_show_flush_char_line(vm);
        vm_printf("%s: <", ID);
        pthread_mutex_lock(&v->channel->mtx);
        for (size_t k = 0; k < v->channel->buf_len; k++) {
            vm_printf("%lld", (long long)v->channel->buf[k]);
            if (k + 1 < v->channel->buf_len) vm_printf(", ");
        }
        pthread_mutex_unlock(&v->channel->mtx);
        vm_printf(">\n");
    } else {
        vm_debug_panic("[VM] SHOW su variabile PARAM non linkata!\n");
    }
}

/* ======================================================================
 *  eval_cond — valuta  lval <op> rval  e ritorna 0 o 1
 *  Usato sia da op_eval che da op_assert.
 * ====================================================================== */

static inline int eval_cond(int64_t lval, const char *op, int64_t rval)
{
    if (!strcmp(op, "==")) return lval == rval;
    if (!strcmp(op, "!=")) return lval != rval;
    if (!strcmp(op, ">=")) return lval >= rval;
    if (!strcmp(op, "<=")) return lval <= rval;
    if (!strcmp(op, ">"))  return lval >  rval;
    if (!strcmp(op, "<"))  return lval <  rval;
    vm_debug_panic("[VM] operatore di confronto sconosciuto: '%s'\n", op);
    return 0;
}

/* ======================================================================
 *  EVAL  <lhs> <op> <rhs_expr>
 *
 *  Formato bytecode:   EVAL x >= 0
 *                      EVAL x == (y + 1)
 *  Imposta thread_val_IF = 1 se la condizione è vera, 0 altrimenti.
 * ====================================================================== */

static inline void op_eval(VM *vm, const char *frame_name)
{
    char *lhs_tok = strtok(NULL, " \t");   /* ID o numero a sinistra  */
    char *op_tok  = strtok(NULL, " \t");   /* operatore               */
    VM_REST_EXPR(rhs); /* espressione destra */

    if (!lhs_tok || !op_tok || rhs[0] == '\0') {
        vm_debug_panic("[VM] EVAL: formato errato (atteso: EVAL <lhs> <op> <rhs>)\n");
    }

    uint fi   = get_findex(frame_name);
    int64_t lval = resolve_value(vm, fi, lhs_tok);
    int64_t rval = resolve_value(vm, fi, rhs);

    thread_val_IF = eval_cond(lval, op_tok, rval);
}

/* ======================================================================
 *  ASSERT  <lhs> <op> <rhs_expr>
 *
 *  Formato bytecode:   ASSERT x == 0
 *  Termina la VM se la condizione è falsa (violazione di reversibilità).
 * ====================================================================== */

static inline void op_assert(VM *vm, const char *frame_name)
{
    char *lhs_tok = strtok(NULL, " \t");
    char *op_tok  = strtok(NULL, " \t");
    VM_REST_EXPR(rhs);

    if (!lhs_tok || !op_tok || rhs[0] == '\0') {
        vm_debug_panic("[VM] ASSERT: formato errato (atteso: ASSERT <lhs> <op> <rhs>)\n");
    }

    /* thread_val_IF contiene il risultato dell'ultimo EVAL, che nel caso IF/ELSE
       è la condizione FI appena valutata. */
    int fi_result = (int)thread_val_IF;

    /* Regola runtime richiesta: se abbiamo seguito il ramo ELSE e, a fine IF,
       la condizione FI risulta vera, segnaliamo errore.
       Nei casi con call/uncall nel blocco IF (es. ricorsione), manteniamo il
       comportamento storico per non introdurre regressioni.
       Nei thread worker di PAR_START, altri pthread possono mutare i PARAM/int
       condivisi tra i rami: la condizione può cambiare tra JMPF e ASSERT (race
       legittima con KAIROS_ALLOW_PAR_SHARED_INT / busy-wait). Non imporre
       allora il vincolo IF/FI single-thread. */
    if (if_branch_top >= 0) {
        int took_then = if_branch_stack[if_branch_top--];
        int has_call  = if_branch_has_call_stack[if_branch_top + 1];
        int par_worker = (current_thread_args != NULL);
        if (!has_call && !took_then && fi_result && !par_worker) {
            vm_debug_panic(
                "[VM] IF/FI non reversibile: ramo else ma condizione fi=vera (frame=%s lhs=%s op=%s rhs=%s fi=%d call=%d)\n",
                frame_name, lhs_tok, op_tok, rhs, fi_result, has_call);
        }
        if (!has_call && took_then && !fi_result && !par_worker) {
            vm_debug_panic(
                "[VM] IF/FI non reversibile: eseguito ramo then, ma guardia del FI falsa (frame=%s lhs=%s op=%s rhs=%s fi=%d call=%d)\n",
                frame_name, lhs_tok, op_tok, rhs, fi_result, has_call);
        }
        return;
    }

    /* Fallback conservativo fuori dal contesto IF. */
    uint fi = get_findex(frame_name);
    int64_t lval = resolve_value(vm, fi, lhs_tok);
    int64_t rval = resolve_value(vm, fi, rhs);
    if (!eval_cond(lval, op_tok, rval)) {
        vm_debug_panic("[VM] ASSERT fallita: %s %s %s\n", lhs_tok, op_tok, rhs);
    }
}

/* ======================================================================
 *  Salti
 * ====================================================================== */

static inline char *op_jmp(VM *vm, const char *fname, char *buf)
{
    char *lbl    = strtok(NULL, " \t");
    uint  fi     = get_findex(fname);
    uint  li     = char_id_map_get(&vm->frames[fi]->LabelIndexer, lbl);
    char *newptr = go_to_line(buf, vm->frames[fi]->label[li] + 1);
    if (!newptr) vm_debug_panic("[VM] JMP: label '%s' non trovata (frame='%s' fi=%u li=%u line=%u)!\n",
                                lbl ? lbl : "(null)", fname, fi, li, vm->frames[fi]->label[li]);
    return newptr;
}

static inline char *op_jmpf(VM *vm, const char *fname, char *buf)
{
    char *lbl = strtok(NULL, " \t");

    /* JMPF con target ELSE_* identifica il branching di IF.
       Memorizziamo il ramo scelto per validare poi la condizione FI in ASSERT. */
    if (lbl && !strncmp(lbl, "ELSE_", 5)) {
        vm_if_push_branch(thread_val_IF ? 1 : 0);
    }

    if (thread_val_IF) return NULL;
    uint fi = get_findex(fname);
    if (!char_id_map_exists(&vm->frames[fi]->LabelIndexer, lbl)) vm_debug_panic("EXIT_FAILURE");
    uint  li     = char_id_map_get(&vm->frames[fi]->LabelIndexer, lbl);
    char *newptr = go_to_line(buf, vm->frames[fi]->label[li] + 1);
    if (!newptr) vm_debug_panic("[VM] JMPF: label non trovata!\n");
    return newptr;
}

/* ======================================================================
 *  LOCAL / DELOCAL
 * ====================================================================== */

static inline void op_local(VM *vm, const char *frame_name)
{
    char *Vtype = strtok(NULL, " \t");
    char *Vname = strtok(NULL, " \t");
    char *c_val = strtok(NULL, " \t");
    char *c_len = (Vtype && !strcmp(Vtype, "array")) ? strtok(NULL, " \t") : NULL;
    uint  fi    = get_findex(frame_name);

    int vi_gia = char_id_map_lookup(&vm->frames[fi]->VarIndexer, Vname);
    uint vi;
    if (vi_gia >= 0) {
        vi = (uint)vi_gia;          /* nome gia' presente: nessun lock */
    } else {
        pthread_mutex_lock(&var_indexer_mtx);
        vi = char_id_map_get(&vm->frames[fi]->VarIndexer, Vname);
        pthread_mutex_unlock(&var_indexer_mtx);
    }

    frame_ensure_vars(vm->frames[fi], (int)vi);
    /* Local-Err: se lo slot esiste già ed è stato allocato da un LOCAL
       precedente (is_local=1, mai chiuso da un DELOCAL/UNCALL corrispondente),
       'v' è già nel dominio dello store: rialloc/overwrite silenzioso qui
       romperebbe l'invertibilità (l'inverso DELOCAL non saprebbe più
       ricostruire il valore perso). Deve fallire in modo esplicito, come gli
       altri errori di programma locali al ramo (stesso stile di vm_debug_panic
       usato da Pop-Err/DELOCAL sotto).
       Gli slot con is_local=0 (placeholder DECL della prima passata, o slot
       duplicati da init_clone_frame per la ricorsione) NON sono "v già
       locale": restano l'allocazione runtime autorevole di LOCAL, quindi
       vengono liberati e sovrascritti come sempre. */
    if (vm->frames[fi]->vars[vi]) {
        if (vm->frames[fi]->vars[vi]->is_local)
            vm_debug_panic(
                "[VM] LOCAL: variabile '%s' già allocata (Local-Err: manca un DELOCAL prima di questo LOCAL?) frame=%s\n",
                Vname, frame_name);
        delete_var(vm->frames[fi]->vars, &vm->frames[fi]->var_count, (int)vi);
    }

    vm->frames[fi]->vars[vi] = malloc(sizeof(Var));
    if (!vm->frames[fi]->vars[vi]) vm_debug_panic("[VM] LOCAL: malloc fallita\n");
    alloc_var(vm->frames[fi]->vars[vi], Vtype, Vname);

    if (vi >= (uint)vm->frames[fi]->var_count)
        vm->frames[fi]->var_count = vi + 1;

    Var *dst = vm->frames[fi]->vars[vi];

    if (dst->T == TYPE_CHANNEL && vm->inversion_depth > 0) {
        Channel *restored = channel_restore_pop(frame_name, Vname);
        if (restored) {
            pthread_mutex_destroy(&dst->channel->mtx);
            free(dst->channel->buf);
            free(dst->channel);
            dst->channel = restored;
        }
    }

    if (dst->T == TYPE_ARRAY) {
        long n = c_len ? strtol(c_len, NULL, 10) : 0;
        if (n <= 0) vm_debug_panic("[VM] LOCAL: array '%s' senza lunghezza valida\n", Vname);
        int64_t fill = 0;
        if (c_val) {
            if (char_id_map_exists(&vm->frames[fi]->VarIndexer, c_val))
                fill = resolve_value(vm, fi, c_val);
            else
                fill = (int64_t)strtoull(c_val, NULL, 10);
        }
        dst->value = malloc((size_t)n * sizeof(int64_t));
        if (!dst->value) vm_debug_panic("[VM] LOCAL: malloc array fallita\n");
        for (long k = 0; k < n; k++) dst->value[k] = fill;
        dst->stack_len = (size_t)n;
    } else if (c_val && char_id_map_exists(&vm->frames[fi]->VarIndexer, c_val)) {
        uint  si  = char_id_map_get(&vm->frames[fi]->VarIndexer, c_val);
        Var  *src = vm->frames[fi]->vars[si];
        if (!src) vm_debug_panic("[VM] LOCAL: sorgente NULL\n");
        if (src->T == TYPE_INT)
            *(dst->value) = *(src->value);
        else if (src->T == TYPE_STACK) {
            var_stack_reserve(dst, src->stack_len);
            dst->stack_len = src->stack_len;
            memcpy(dst->value, src->value, src->stack_len * sizeof(int64_t));
        } else {
            vm_debug_panic("[VM] LOCAL: copia da PARAM non linkato\n");
        }
    } else {
        if (dst->T == TYPE_INT)
            *(dst->value) = c_val ? (int64_t)strtoull(c_val, NULL, 10) : 0;
        else if (dst->T == TYPE_STACK) {
            if (c_val && strcmp(c_val, "nil") != 0)
                vm_debug_panic("[VM] LOCAL: valore stack non compatibile\n");
        }
    }

    stack_push(&vm->frames[fi]->LocalVariables, dst);
}

static inline void op_delocal(VM *vm, const char *frame_name)
{
    char *Vtype = strtok(NULL, " \t");
    char *Vname = strtok(NULL, " \t");
    char *c_val = strtok(NULL, " \t");
    char *c_len = (Vtype && !strcmp(Vtype, "array")) ? strtok(NULL, " \t") : NULL;
    uint  fi    = get_findex(frame_name);

    /* ── 1. Valore atteso ── */
    int64_t Vvalue = 0;
    if (c_val) {
        if (char_id_map_exists(&vm->frames[fi]->VarIndexer, c_val)) {
            Vvalue = resolve_value(vm, fi, c_val);
        } else {
            Vvalue = (int64_t)strtoull(c_val, NULL, 10);
        }
    }

    /* ── 2. Pop ── */
    Var *V = stack_pop(&vm->frames[fi]->LocalVariables);

    /* ── 3. Ordine LIFO ── */
    if (strcmp(V->name, Vname) != 0) {
        vm_debug_panic("[VM] DELOCAL: ordine errato! atteso '%s', trovato '%s'\n", Vname, V->name);
    }

    /* ── 4. Tipo ── */
    const char *actual_type = (V->T == TYPE_INT)  ? "int"
                            : (V->T == TYPE_STACK) ? "stack"
                            : (V->T == TYPE_ARRAY) ? "array"
                                                   : "channel";
    if (strcmp(Vtype, actual_type) != 0) {
        vm_debug_panic("[VM] DELOCAL: tipo errato! atteso %s, trovato %s\n",
                actual_type, Vtype);
    }

    /* ── 5. Valore finale ── */
    int ok = 0;
    if (V->T == TYPE_ARRAY) {
        long n = c_len ? strtol(c_len, NULL, 10) : -1;
        ok = (n == (long)V->stack_len);
        for (size_t k = 0; ok && k < V->stack_len; k++)
            ok = (V->value[k] == Vvalue);
    }
    else if (V->T == TYPE_INT)     ok = (*(V->value) == Vvalue);
    else if (V->T == TYPE_STACK)   ok = (V->stack_len == 0 && c_val && strcmp(c_val, "nil")   == 0);
    else if (V->T == TYPE_CHANNEL) ok = (V->channel->buf_len == 0 && c_val && strcmp(c_val, "empty") == 0);

    if (!ok) {
        if (V->T == TYPE_ARRAY)
            vm_debug_panic(
                "[VM] DELOCAL: array '%s' non e' tutto %lld alla chiusura (o lunghezza diversa)\n",
                Vname, (long long)Vvalue);
        else if (V->T == TYPE_INT)
            vm_debug_panic(
                "[VM] DELOCAL: valore finale errato! (frame=%s var=%s, atteso=%lld, trovato=%lld, c_val=%s)\n",
                frame_name, Vname, (long long)Vvalue, (long long)*(V->value), c_val ? c_val : "NULL");
        else
            vm_debug_panic("[VM] DELOCAL: %s non è nil/empty!\n", Vname);
        
    }

    /* ── 6. Distruggi ── */
    int vi_gia = char_id_map_lookup(&vm->frames[fi]->VarIndexer, Vname);
    uint vi;
    if (vi_gia >= 0) {
        vi = (uint)vi_gia;          /* nome gia' presente: nessun lock */
    } else {
        pthread_mutex_lock(&var_indexer_mtx);
        vi = char_id_map_get(&vm->frames[fi]->VarIndexer, Vname);
        pthread_mutex_unlock(&var_indexer_mtx);
    }

    if (V->T == TYPE_CHANNEL && vm->inversion_depth == 0) {
        int should_track = 0;
        pthread_mutex_lock(&V->channel->mtx);
        if (V->channel->refcount > 1) {
            V->channel->refcount++;
            should_track = 1;
        }
        pthread_mutex_unlock(&V->channel->mtx);
        if (should_track)
            channel_restore_push(frame_name, Vname, V->channel);
    }

    delete_var(vm->frames[fi]->vars, &vm->frames[fi]->var_count, (int)vi);
}


#endif /* VM_OPS_H */