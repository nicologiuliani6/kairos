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
 * compresa: nessun massimo, il vettore cresce raddoppiando. */
static __thread int *if_branch_stack          = NULL;
static __thread int  if_branch_cap            = 0;
static __thread int  if_branch_top            = -1;

static inline void vm_if_reset_branch_stack(void)
{
    if_branch_top = -1;
}

static inline void vm_if_push_branch(int took_then)
{
    if (if_branch_top + 1 >= if_branch_cap) {
        int nc = if_branch_cap ? if_branch_cap * 2 : 64;
        int *a = (int *)realloc(if_branch_stack, sizeof(int) * (size_t)nc);
        if (!a) vm_debug_panic("[VM] IF: out of memory\n");
        if_branch_stack = a;
        if_branch_cap = nc;
    }
    if_branch_stack[++if_branch_top] = took_then;
}

/* Alla fine di un thread: il vettore è __thread, lo libera chi l'ha creato. */
static inline void vm_if_free_branch_stack(void)
{
    free(if_branch_stack);
    if_branch_stack = NULL;
    if_branch_cap = 0;
    if_branch_top = -1;
}

/* Pila dei local di un ramo di par. I rami girano nel frame della procedura
 * che contiene il par, ma ognuno apre e chiude i propri local: ogni thread di
 * ramo ha la sua pila (impostata da thread_entry), le procedure chiamate dal
 * ramo usano quella del loro frame. */
static __thread Stack *g_branch_locals    = NULL;
static __thread uint   g_branch_locals_fi = 0;

static inline Stack *frame_locals(VM *vm, uint fi)
{
    if (__atomic_load_n(&g_par_attivi, __ATOMIC_RELAXED) &&   /* g_branch_locals: solo nei rami */
        g_branch_locals && fi == g_branch_locals_fi) return g_branch_locals;
    return &vm->frames[fi]->LocalVariables;
}

#define CHANNEL_REF_MARKER (-1001)

/* Posizione del thread nell'albero dei par: "/1/0" e' il ramo 0 di un par
 * dentro il ramo 1 del par piu' esterno; "" il thread principale. E' la stessa
 * all'andata e all'inversione, e due rami concorrenti ne hanno di diverse. */
static __thread const char *g_branch_path = "";

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

/* Chiave di un'attivazione: posizione del ramo e procedura. Due attivazioni
 * concorrenti della stessa procedura stanno in rami diversi; quelle di uno
 * stesso ramo si disfano in ordine inverso, e la ricerca dall'ultima voce le
 * accoppia giuste. */
static inline char *channel_restore_key(const char *frame_name)
{
    VM_FRAME_BASE(proc, frame_name);
    size_t a = strlen(g_branch_path), b = strlen(proc);
    char *k = (char *)malloc(a + b + 2);
    if (!k) vm_debug_panic("[VM] channel restore: out of memory\n");
    memcpy(k, g_branch_path, a); k[a] = '|'; memcpy(k + a + 1, proc, b + 1);
    return k;
}

static inline void channel_restore_push(const char *frame_name, const char *var_name, Channel *ch)
{
    if (!ch) return;
    char *p = channel_restore_key(frame_name), *v = char_id_strdup(var_name);
    pthread_mutex_lock(&g_channel_restore_mtx);
    if (g_channel_restore_top >= g_channel_restore_cap) {
        int nc = g_channel_restore_cap ? g_channel_restore_cap * 2 : 64;
        ChannelRestoreEntry *n = (ChannelRestoreEntry *)realloc(
            g_channel_restore, sizeof(ChannelRestoreEntry) * (size_t)nc);
        if (!n) {
            pthread_mutex_unlock(&g_channel_restore_mtx);
            vm_debug_panic("[VM] channel restore: out of memory\n");
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
    char *proc = channel_restore_key(frame_name);
    pthread_mutex_lock(&g_channel_restore_mtx);
    for (int i = g_channel_restore_top - 1; i >= 0; i--) {
        if (strcmp(g_channel_restore[i].proc, proc) == 0 &&
            strcmp(g_channel_restore[i].var, var_name) == 0) {
            Channel *ch = g_channel_restore[i].channel;
            free(g_channel_restore[i].proc);
            free(g_channel_restore[i].var);
            /* si chiude il buco tenendo l'ordine: le altre chiavi restano LIFO */
            memmove(&g_channel_restore[i], &g_channel_restore[i + 1],
                    sizeof(ChannelRestoreEntry) * (size_t)(g_channel_restore_top - 1 - i));
            g_channel_restore_top--;
            pthread_mutex_unlock(&g_channel_restore_mtx);
            free(proc);
            return ch;
        }
    }
    pthread_mutex_unlock(&g_channel_restore_mtx);
    free(proc);
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
    if (strtok(NULL, " \t")) vm_debug_panic("[VM] PUSH: too many arguments\n");

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
        vm_debug_panic("[VM] PUSH: destination stack not found\n");

    uint  si = char_id_map_get(&vm->frames[fi]->VarIndexer, C_stack);
    Var  *sv = vm->frames[fi]->vars[si];
    if (sv->T != TYPE_STACK)
        vm_debug_panic("[VM] PUSH: '%s' is not a stack (channels use ssend/srecv) frame=%s\n",
            C_stack, frame_name);
    var_stack_push(sv, val);
}

static inline void op_pop(VM *vm, const char *frame_name)
{
    char *C_dest  = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_debug_panic("[VM] POP: too many arguments\n");

    uint fi = get_findex(frame_name);
    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, C_stack))
        vm_debug_panic("[VM] POP: stack not found\n");

    uint  si = char_id_map_get(&vm->frames[fi]->VarIndexer, C_stack);
    Var  *sv = vm->frames[fi]->vars[si];

    if (sv->T != TYPE_STACK)
        vm_debug_panic("[VM] POP: '%s' is not a stack (channels use ssend/srecv) frame=%s\n",
                       C_stack, frame_name);
    if (sv->stack_len == 0)
        vm_debug_panic("[VM] POP: empty stack (Pop-Err, frame=%s dest=%s stack=%s inv=%d)\n",
                       frame_name, C_dest, C_stack, g_inversion_depth);
    int64_t popped = sv->value[--sv->stack_len];   /* la capacità resta per i push */

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
            "[VM] POP: destination '%s' is not zero before the pop (Pop-Err2, current value=%lld) frame=%s\n",
            C_dest, (long long)*(dest->value), frame_name);
    }
    *(dest->value) += popped;
    var_par_mut_release(dest);
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
            if (!_n) vm_debug_panic("[VM] payload: out of memory\n"); \
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
        vm_debug_panic("[VM] SSEND: bad format (expected SSEND <v1 ...> <channel>)\n");

    char *ch_name = tokv[ntok - 1];
    int payload_count = ntok - 1;
    uint fi = get_findex(frame_name);

    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, ch_name))
        vm_debug_panic("[VM] SSEND: channel not found\n");
    uint chi = char_id_map_get(&vm->frames[fi]->VarIndexer, ch_name);
    Var *chv = vm->frames[fi]->vars[chi];
    if (!chv || chv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] SSEND: destination is not a channel\n");
    session_acquire(chv->channel, ch_name);

    int encoded_len = 0;
    int encoded_cap = 16;
    int64_t *encoded = malloc((size_t)encoded_cap * sizeof(int64_t));
    if (!encoded)
        vm_debug_panic("[VM] SSEND: malloc failed\n");

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
                vm_debug_panic("[VM] SSEND: a channel cannot be sent over itself\n");
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
                ENC_PUSH((int)(uint32_t)(((uint64_t)p >> 32) & 0xffffffffu));
            } else {
                free(encoded);
                vm_debug_panic("[VM] SSEND: unlinked parameter\n");
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
        vm_debug_panic("[VM] SRECV: bad format (expected SRECV <v1 ...> <channel>)\n");

    char *ch_name = tokv[ntok - 1];
    int recv_count = ntok - 1;
    uint fi = get_findex(frame_name);

    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, ch_name))
        vm_debug_panic("[VM] SRECV: channel not found\n");
    uint chi = char_id_map_get(&vm->frames[fi]->VarIndexer, ch_name);
    Var *chv = vm->frames[fi]->vars[chi];
    if (!chv || chv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] SRECV: source is not a channel\n");
    session_acquire(chv->channel, ch_name);

    /* Srecv-Err prima del rendez-vous: una destinazione già piena è un errore
       del ricevente e va segnalata anche quando il mittente non arriva mai. */
    for (int i = 0; i < recv_count; i++) {
        Var *dest = get_var(vm, fi, tokv[i], "SRECV");
        if (dest->T == TYPE_INT && *(dest->value) != 0)
            vm_debug_panic(
                "[VM] SRECV: destination '%s' is not zero before receiving (Srecv-Err, current value=%lld)\n",
                tokv[i], (long long)*(dest->value));
        if (dest->T == TYPE_STACK && dest->stack_len != 0)
            vm_debug_panic(
                "[VM] SRECV: stack '%s' is not empty before receiving (Srecv-Err)\n", tokv[i]);
    }

    op_wait(chv->channel, 0);

    pthread_mutex_lock(&chv->channel->mtx);
    ThreadArgs *sender_to_wake = chv->channel->sender_args;
    chv->channel->sender_args = NULL;
    if (!sender_to_wake) {
        pthread_mutex_unlock(&chv->channel->mtx);
        vm_debug_panic("[VM] SRECV: null sender_args after the rendezvous\n");
    }
    size_t read_idx = 0;
    for (int i = 0; i < recv_count; i++) {
        Var *dest = get_var(vm, fi, tokv[i], "SRECV");
        if (read_idx >= chv->channel->buf_len) {
            pthread_mutex_unlock(&chv->channel->mtx);
            vm_debug_panic("[VM] SRECV: not enough values on the channel\n");
        }
        int marker = chv->channel->buf[read_idx++];
        if (marker == (int)TYPE_INT) {
            if (read_idx >= chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: incomplete int payload\n");
            }
            int64_t popped = chv->channel->buf[read_idx++];   /* era int: troncava i valori a 32 bit */
            if (dest->T != TYPE_INT) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: an int payload needs an int destination\n");
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
                    "[VM] SRECV: destination '%s' is not zero before receiving (Srecv-Err, current value=%lld)\n",
                    tokv[i], (long long)*(dest->value));
            }
            *(dest->value) += popped;
            var_par_mut_release(dest);
        } else if (marker == CHANNEL_REF_MARKER) {
            if (read_idx + 1 >= chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: incomplete channel payload\n");
            }
            uint32_t lo = (uint32_t)chv->channel->buf[read_idx++];
            uint32_t hi = (uint32_t)chv->channel->buf[read_idx++];
            uintptr_t p = (uintptr_t)(((uint64_t)hi << 32) | (uint64_t)lo);
            Channel *shared = (Channel *)p;
            if (dest->T != TYPE_CHANNEL || !shared) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: a channel payload needs a channel destination\n");
            }
            if (dest->channel == chv->channel || shared == chv->channel) {
                /* la destinazione e' il canale stesso su cui si riceve */
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: '%s' is the channel being received on: it cannot "
                               "also be the destination\n", tokv[i]);
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
        } else if (marker == (int)TYPE_STACK) {
            if (read_idx >= chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: incomplete stack payload\n");
            }
            int64_t n = chv->channel->buf[read_idx++];
            if (n < 0 || read_idx + (size_t)n > chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: invalid payload length\n");
            }
            if (dest->T != TYPE_STACK) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: a stack payload needs a stack destination ('%s' is not)\n",
                               tokv[i]);
            }
            /* Srecv-Err anche per le collezioni: la destinazione dev'essere
               vuota, altrimenti il contenuto ricevuto si accoderebbe al vecchio
               e ssend, l'inverso, rispedirebbe anche quello. */
            if (dest->stack_len != 0) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic(
                    "[VM] SRECV: stack '%s' is not empty before receiving (Srecv-Err)\n", tokv[i]);
            }
            if (n > 0) {
                var_stack_reserve(dest, dest->stack_len + (size_t)n);
                memcpy(dest->value + dest->stack_len, chv->channel->buf + read_idx, (size_t)n * sizeof(int64_t));
                dest->stack_len += (size_t)n;
            }
            read_idx += (size_t)n;
        } else {
            pthread_mutex_unlock(&chv->channel->mtx);
            vm_debug_panic("[VM] SRECV: unknown payload marker\n");
        }
    }

    size_t remaining = chv->channel->buf_len - read_idx;
    if (remaining > 0) {
        /* Un rendez-vous scambia un messaggio intero: il ricevente deve
           prendere tutti i valori che il mittente spedisce. */
        pthread_mutex_unlock(&chv->channel->mtx);
        vm_debug_panic("[VM] SRECV: the sender sent more values than '%s' receives\n",
                       ch_name);
    }
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

static inline void op_show_unlocked(VM *vm, const char *frame_name);

/* Uno show scrive la sua riga per intero anche quando due rami di par
 * stampano insieme: stdout resta del thread per tutta l'istruzione. */
static inline void op_show(VM *vm, const char *frame_name)
{
#ifndef DAP_MODE
    flockfile(stdout);
    op_show_unlocked(vm, frame_name);
    funlockfile(stdout);
#else
    static pthread_mutex_t show_mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&show_mtx);
    op_show_unlocked(vm, frame_name);
    pthread_mutex_unlock(&show_mtx);
#endif
}

static inline void op_show_unlocked(VM *vm, const char *frame_name)
{
    char *ID = strtok(NULL, " \t");
    if (!ID) vm_debug_panic("[VM] SHOW: missing variable name\n");
    char *fmt = strtok(NULL, " \t");
    char *more = strtok(NULL, " \t");
    if (more) vm_debug_panic("[VM] SHOW: too many arguments\n");

    int as_char = 0;
    if (fmt) {
        if (strcmp(fmt, "char") != 0) {
            vm_debug_panic(
                "[VM] SHOW: unknown second argument "
                "(only 'char', or omit it for the plain format)\n"
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
            vm_debug_panic("[VM] SHOW char: only for int\n");
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
            vm_debug_panic("[VM] SHOW char: only for int\n");
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
        vm_debug_panic("[VM] SHOW of an unlinked PARAM variable\n");
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
    vm_debug_panic("[VM] unknown comparison operator: '%s'\n", op);
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
        vm_debug_panic("[VM] EVAL: bad format (expected: EVAL <lhs> <op> <rhs>)\n");
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
        vm_debug_panic("[VM] ASSERT: bad format (expected: ASSERT <lhs> <op> <rhs>)\n");
    }

    /* thread_val_IF contiene il risultato dell'ultimo EVAL, che nel caso IF/ELSE
       è la condizione FI appena valutata. */
    int fi_result = (int)thread_val_IF;

    /* Come in Janus: dopo il ramo then la condizione FI dev'essere vera, dopo il
       ramo else falsa. */
    if (if_branch_top >= 0) {
        int took_then = if_branch_stack[if_branch_top--];
        if (!took_then && fi_result) {
            vm_debug_panic(
                "[VM] IF/FI not reversible: else branch taken but the fi assertion is true (IfError, frame=%s lhs=%s op=%s rhs=%s)\n",
                frame_name, lhs_tok, op_tok, rhs);
        }
        if (took_then && !fi_result) {
            vm_debug_panic(
                "[VM] IF/FI not reversible: then branch taken but the fi assertion is false (IfError, frame=%s lhs=%s op=%s rhs=%s)\n",
                frame_name, lhs_tok, op_tok, rhs);
        }
        return;
    }

    /* Fallback conservativo fuori dal contesto IF. */
    uint fi = get_findex(frame_name);
    int64_t lval = resolve_value(vm, fi, lhs_tok);
    int64_t rval = resolve_value(vm, fi, rhs);
    if (!eval_cond(lval, op_tok, rval)) {
        vm_debug_panic("[VM] ASSERT failed: %s %s %s\n", lhs_tok, op_tok, rhs);
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
    if (!newptr) vm_debug_panic("[VM] JMP: label '%s' not found (frame='%s' fi=%u li=%u line=%u)\n",
                                lbl ? lbl : "(null)", fname, fi, li, vm->frames[fi]->label[li]);
    return newptr;
}

/* Guardia d'ingresso b1 del ciclo `from b1 do c1 loop c2 until b2` di etichette
 * `_<uid>`, valutata nello stato corrente. La riga `EVAL b1` ha posizione fissa
 * rispetto alle etichette del ciclo: due righe prima di LABEL FROM_START nel
 * layout a un corpo, tre righe prima di LABEL FROM_BACK in quello a due. */
static inline int loop_entry_guard(VM *vm, const char *fname, char *buf, const char *uid)
{
    uint fi = get_findex(fname);
    size_t ul = strlen(uid);
    char lbl[ul + sizeof("FROM_START_")];
    memcpy(lbl, "FROM_BACK_", sizeof("FROM_BACK_") - 1);
    memcpy(lbl + sizeof("FROM_BACK_") - 1, uid, ul + 1);
    uint back = 3;
    if (!char_id_map_exists(&vm->frames[fi]->LabelIndexer, lbl)) {
        memcpy(lbl, "FROM_START_", sizeof("FROM_START_") - 1);
        memcpy(lbl + sizeof("FROM_START_") - 1, uid, ul + 1);
        back = 2;
    }
    uint li  = char_id_map_get(&vm->frames[fi]->LabelIndexer, lbl);
    char *ln = go_to_line(buf, vm->frames[fi]->label[li] - back);
    char *nl = ln ? strchr(ln, '\n') : NULL;
    if (!nl) vm_debug_panic("[VM] FROM: entry guard of loop %s not found\n", uid);
    VM_LINE_DUP(line, ln, nl);
    char *fw = strtok(skip_lineno(line), " \t");
    if (!fw || strcmp(fw, "EVAL") != 0)
        vm_debug_panic("[VM] FROM: entry guard of loop %s not found\n", uid);
    uint saved = thread_val_IF;
    /* La copia e' quella di una riga del programma, divisa come la divide
     * op_eval: la si pubblica come riga corrente (cn_cerca, vm_helpers.h), e
     * le sue espressioni si ritrovano dall'indirizzo invece che dall'hash. */
    const char *rc = tl_riga_copia, *rf = tl_riga_fine, *ro = tl_riga_orig;
    tl_riga_copia = line; tl_riga_fine = line + line_len; tl_riga_orig = ln;
    op_eval(vm, fname);
    tl_riga_copia = rc; tl_riga_fine = rf; tl_riga_orig = ro;
    int b1 = thread_val_IF ? 1 : 0;
    thread_val_IF = saved;
    return b1;
}

/* LoopError2: al rientro nel ciclo (dopo c2) la guardia d'ingresso dev'essere
 * falsa, come in Janus; e' cio' che permette all'inverso di sapere quando
 * fermarsi. */
static inline void loop_check_reentry(VM *vm, const char *fname, char *buf, const char *uid)
{
    if (loop_entry_guard(vm, fname, buf, uid))
        vm_debug_panic("[VM] FROM: entry guard true on re-entering the loop "
                       "(LoopError2, frame=%s)\n", fname);
}

/* `LABEL FROM_START_<uid>` raggiunta in sequenza: nel layout a due corpi vuol
 * dire che si rientra nel ciclo dopo c2 (all'ingresso la si scavalca con JMP). */
static inline void op_label(VM *vm, const char *fname, char *buf)
{
    char *lbl = strtok(NULL, " \t");
    if (!lbl || strncmp(lbl, "FROM_START_", 11) != 0) return;
    const char *uid = lbl + 11;
    uint fi = get_findex(fname);
    size_t ul = strlen(uid);
    char back[ul + sizeof("FROM_BACK_")];
    memcpy(back, "FROM_BACK_", sizeof("FROM_BACK_") - 1);
    memcpy(back + sizeof("FROM_BACK_") - 1, uid, ul + 1);
    if (char_id_map_exists(&vm->frames[fi]->LabelIndexer, back))
        loop_check_reentry(vm, fname, buf, uid);
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
    /* LoopError1: la guardia d'ingresso di un ciclo dev'essere vera all'ingresso. */
    if (lbl && !strncmp(lbl, "FROM_ERR_", 9))
        vm_debug_panic("[VM] FROM: entry guard false on entering the loop "
                       "(LoopError1, frame=%s)\n", fname);
    /* Ciclo a un corpo: il back-edge e' il rientro, la guardia dev'essere falsa. */
    if (lbl && !strncmp(lbl, "FROM_START_", 11))
        loop_check_reentry(vm, fname, buf, lbl + 11);
    uint fi = get_findex(fname);
    if (!char_id_map_exists(&vm->frames[fi]->LabelIndexer, lbl)) vm_debug_panic("EXIT_FAILURE");
    uint  li     = char_id_map_get(&vm->frames[fi]->LabelIndexer, lbl);
    char *newptr = go_to_line(buf, vm->frames[fi]->label[li] + 1);
    if (!newptr) vm_debug_panic("[VM] JMPF: label not found\n");
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
                "[VM] LOCAL: variable '%s' already allocated (Local-Err: missing DELOCAL before this LOCAL?) frame=%s\n",
                Vname, frame_name);
        delete_var(vm->frames[fi]->vars, &vm->frames[fi]->var_count, (int)vi);
    }

    vm->frames[fi]->vars[vi] = vm_var_nuova(vm_tipo_cod(Vtype), Vname);
    if (!vm->frames[fi]->vars[vi]) vm_debug_panic("[VM] LOCAL: allocation failed\n");

    /* Massimo atomico: due rami di par possono aprire local nello stesso frame. */
    int vc = __atomic_load_n(&vm->frames[fi]->var_count, __ATOMIC_RELAXED);
    while ((int)vi >= vc &&
           !__atomic_compare_exchange_n(&vm->frames[fi]->var_count, &vc, (int)vi + 1, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;

    Var *dst = vm->frames[fi]->vars[vi];

    if (dst->T == TYPE_CHANNEL && g_inversion_depth > 0) {
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
        if (n <= 0) vm_debug_panic("[VM] LOCAL: array '%s' without a valid length\n", Vname);
        int64_t fill = 0;
        if (c_val) {
            if (char_id_map_exists(&vm->frames[fi]->VarIndexer, c_val))
                fill = resolve_value(vm, fi, c_val);
            else
                fill = (int64_t)strtoull(c_val, NULL, 10);
        }
        if ((unsigned long)n > SIZE_MAX / sizeof(int64_t))
            vm_debug_panic("[VM] LOCAL: array '%s' too large (%ld cells)\n", Vname, n);
        dst->value = malloc((size_t)n * sizeof(int64_t));
        if (!dst->value) vm_debug_panic("[VM] LOCAL: array allocation failed\n");
        for (long k = 0; k < n; k++) dst->value[k] = fill;
        dst->stack_len = (size_t)n;
    } else if (c_val && char_id_map_exists(&vm->frames[fi]->VarIndexer, c_val)) {
        uint  si  = char_id_map_get(&vm->frames[fi]->VarIndexer, c_val);
        Var  *src = vm->frames[fi]->vars[si];
        if (!src) vm_debug_panic("[VM] LOCAL: NULL source\n");
        if (src->T == TYPE_INT)
            *(dst->value) = *(src->value);
        else if (src->T == TYPE_STACK) {
            var_stack_reserve(dst, src->stack_len);
            dst->stack_len = src->stack_len;
            memcpy(dst->value, src->value, src->stack_len * sizeof(int64_t));
        } else {
            vm_debug_panic("[VM] LOCAL: copy from an unlinked PARAM\n");
        }
    } else {
        if (dst->T == TYPE_INT)
            *(dst->value) = c_val ? (int64_t)strtoull(c_val, NULL, 10) : 0;
        else if (dst->T == TYPE_STACK) {
            if (c_val && strcmp(c_val, "nil") != 0)
                vm_debug_panic("[VM] LOCAL: incompatible stack value\n");
        }
    }

    stack_push(frame_locals(vm, fi), dst);
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
    Var *V = stack_pop(frame_locals(vm, fi));

    /* ── 3. Ordine LIFO ── */
    if (strcmp(V->name, Vname) != 0) {
        vm_debug_panic("[VM] DELOCAL: wrong order, expected '%s', found '%s'\n", Vname, V->name);
    }

    /* ── 4. Tipo ── */
    const char *actual_type = (V->T == TYPE_INT)  ? "int"
                            : (V->T == TYPE_STACK) ? "stack"
                            : (V->T == TYPE_ARRAY) ? "array"
                                                   : "channel";
    if (strcmp(Vtype, actual_type) != 0) {
        vm_debug_panic("[VM] DELOCAL: wrong type, expected %s, found %s\n",
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
                "[VM] DELOCAL: array '%s' is not all %lld when closed (or has another length)\n",
                Vname, (long long)Vvalue);
        else if (V->T == TYPE_INT)
            vm_debug_panic(
                "[VM] DELOCAL: wrong final value (Deloc-Err, frame=%s var=%s, expected=%lld, found=%lld, c_val=%s)\n",
                frame_name, Vname, (long long)Vvalue, (long long)*(V->value), c_val ? c_val : "NULL");
        else
            vm_debug_panic("[VM] DELOCAL: %s is not nil/empty\n", Vname);
        
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

    if (V->T == TYPE_CHANNEL && g_inversion_depth == 0) {
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


#include "vm_dec.h"   /* righe decodificate: usa le op_* qui sopra */

#endif /* VM_OPS_H */