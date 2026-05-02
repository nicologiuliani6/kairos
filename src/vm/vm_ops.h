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

#ifdef DAP_MODE
#include <unistd.h>
extern VM *g_current_vm;
#define vm_printf(...) do { \
    VMDebugState *_d = g_current_vm ? g_current_vm->dbg : NULL; \
    if (_d) { \
        if (_d->suppress_output) break; \
        char _tmp[1024]; \
        int _nw = snprintf(_tmp, sizeof(_tmp), __VA_ARGS__); \
        if (_nw > 0) { \
            if (_d->output_pipe_fd > 0) { \
                /* Canale real-time */ \
                (void)write(_d->output_pipe_fd, _tmp, (size_t)_nw); \
            } \
            /* Mantieni sempre anche il buffer interno come fallback/backup \
               (alcuni client leggono vm_debug_output_ext invece della pipe \
               in certi passaggi di stepback/revert). */ \
            int _avail = DBG_OUTPUT_BUF_SIZE - _d->out_len - 1; \
            if (_avail > 0) { \
                int _copy = _nw < _avail ? _nw : _avail; \
                memcpy(_d->out_buf + _d->out_len, _tmp, _copy); \
                _d->out_len += _copy; \
                _d->out_buf[_d->out_len] = '\0'; \
            } \
        } \
    } \
} while(0)
#else
  #define vm_printf(...) printf(__VA_ARGS__)
#endif

#define IF_BRANCH_STACK_MAX 256
static __thread int if_branch_stack[IF_BRANCH_STACK_MAX];
static __thread int if_branch_has_call_stack[IF_BRANCH_STACK_MAX];
static __thread int if_branch_top = -1;

static inline void vm_if_reset_branch_stack(void)
{
    if_branch_top = -1;
}

static inline void vm_if_mark_call(void)
{
    if (if_branch_top >= 0)
        if_branch_has_call_stack[if_branch_top] = 1;
}

#define CHANNEL_REF_MARKER (-1001)

typedef struct {
    char proc[VAR_NAME_LENGTH];
    char var[VAR_NAME_LENGTH];
    Channel *channel;
} ChannelRestoreEntry;

#define CHANNEL_RESTORE_MAX 4096
static ChannelRestoreEntry g_channel_restore[CHANNEL_RESTORE_MAX];
static int g_channel_restore_top = 0;
static pthread_mutex_t g_channel_restore_mtx = PTHREAD_MUTEX_INITIALIZER;

static inline void frame_base_name(const char *frame_name, char *out, size_t out_sz)
{
    size_t i = 0;
    while (frame_name[i] && frame_name[i] != '@' && i + 1 < out_sz) {
        out[i] = frame_name[i];
        i++;
    }
    out[i] = '\0';
}

static inline void channel_restore_push(const char *frame_name, const char *var_name, Channel *ch)
{
    if (!ch) return;
    char proc[VAR_NAME_LENGTH];
    frame_base_name(frame_name, proc, sizeof(proc));
    pthread_mutex_lock(&g_channel_restore_mtx);
    if (g_channel_restore_top < CHANNEL_RESTORE_MAX) {
        strncpy(g_channel_restore[g_channel_restore_top].proc, proc, VAR_NAME_LENGTH - 1);
        g_channel_restore[g_channel_restore_top].proc[VAR_NAME_LENGTH - 1] = '\0';
        strncpy(g_channel_restore[g_channel_restore_top].var, var_name, VAR_NAME_LENGTH - 1);
        g_channel_restore[g_channel_restore_top].var[VAR_NAME_LENGTH - 1] = '\0';
        g_channel_restore[g_channel_restore_top].channel = ch;
        g_channel_restore_top++;
    }
    pthread_mutex_unlock(&g_channel_restore_mtx);
}

static inline Channel *channel_restore_pop(const char *frame_name, const char *var_name)
{
    char proc[VAR_NAME_LENGTH];
    frame_base_name(frame_name, proc, sizeof(proc));
    pthread_mutex_lock(&g_channel_restore_mtx);
    for (int i = g_channel_restore_top - 1; i >= 0; i--) {
        if (strcmp(g_channel_restore[i].proc, proc) == 0 &&
            strcmp(g_channel_restore[i].var, var_name) == 0) {
            Channel *ch = g_channel_restore[i].channel;
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
    Var  *v1  = get_var(vm, fi, ID1, "SWAP");
    Var  *v2  = get_var(vm, fi, ID2, "SWAP");
    var_par_mut_acquire2(v1, v2);
    int   tmp = *(v1->value);
    *(v1->value) = *(v2->value);
    *(v2->value) = tmp;
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
    int  val;

    if (char_id_map_exists(&vm->frames[fi].VarIndexer, C_val)) {
        Var *src = get_var(vm, fi, C_val, "PUSH");
        var_par_mut_acquire(src);
        val = *(src->value);
        *(src->value) = 0;
        var_par_mut_release(src);
    } else {
        val = (int)strtoul(C_val, NULL, 10);
    }

    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, C_stack))
        vm_debug_panic("[VM] PUSH: stack destinazione non trovato!\n");

    uint  si = char_id_map_get(&vm->frames[fi].VarIndexer, C_stack);
    Var  *sv = vm->frames[fi].vars[si];
    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] PUSH: destinazione non è stack/channel!\n");

    if (sv->T == TYPE_STACK) {
        sv->value = realloc(sv->value, (sv->stack_len + 1) * sizeof(int));
        if (!sv->value) vm_debug_panic("realloc failed\n");
        sv->value[sv->stack_len++] = val;
    } else {
        pthread_mutex_lock(&sv->channel->mtx);
        sv->channel->buf = realloc(sv->channel->buf, (sv->channel->buf_len + 1) * sizeof(int));
        if (!sv->channel->buf) vm_debug_panic("realloc failed\n");
        sv->channel->buf[sv->channel->buf_len++] = val;
        pthread_mutex_unlock(&sv->channel->mtx);
        int w = op_wait(sv->channel, 1, 0);
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
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, C_stack))
        vm_debug_panic("[VM] POP: stack non trovato!\n");

    uint  si = char_id_map_get(&vm->frames[fi].VarIndexer, C_stack);
    Var  *sv = vm->frames[fi].vars[si];

    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL) vm_debug_panic("[VM] POP: sorgente non è stack/channel!\n");
    if (sv->T == TYPE_STACK && sv->stack_len == 0)    vm_debug_panic("[VM] POP: stack vuoto!\n");

    ThreadArgs *sender_to_wake = NULL;
    int           popped;

    if (sv->T == TYPE_CHANNEL) {
        /* op_wait abbina questo recv al sender giusto e imposta sender_args.
           Non leggere send_q_head prima: con più SSEND paralleli la coda dei
           waiter non coincide col FIFO del buffer; notificare il thread
           sbagliato corrompeva lo stato (es. malloc.kairos intermittente).
           Leggere sender_args e fare il pop FIFO sotto lo stesso mtx: altrimenti
           un altro SSEND può intercalare tra le due e far leggere una cella
           non ancora valorizzata (buffer malloc non azzerato). */
        op_wait(sv->channel, 0, 0);
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
            memmove(sv->channel->buf, sv->channel->buf + 1, sv->channel->buf_len * sizeof(int));
        if (sv->channel->buf_len > 0)
            sv->channel->buf = realloc(sv->channel->buf, sv->channel->buf_len * sizeof(int));
        pthread_mutex_unlock(&sv->channel->mtx);
    } else {
        popped = sv->value[--sv->stack_len];
        if (sv->stack_len > 0)
            sv->value = realloc(sv->value, sv->stack_len * sizeof(int));
    }

    Var *dest = get_var(vm, fi, C_dest, "POP");
    var_par_mut_acquire(dest);
    *(dest->value) += popped;
    var_par_mut_release(dest);

    if (sv->T == TYPE_CHANNEL && sender_to_wake)
        notify_sender_turn_done(sender_to_wake);
}

static inline void op_ssend(VM *vm, const char *frame_name)
{
    char *tokv[64];
    int ntok = 0;
    char *tok = NULL;
    while ((tok = strtok(NULL, " \t")) && ntok < 64)
        tokv[ntok++] = tok;

    if (ntok < 2)
        vm_debug_panic("[VM] SSEND: formato errato (atteso SSEND <v1 ...> <channel>)\n");

    char *ch_name = tokv[ntok - 1];
    int payload_count = ntok - 1;
    uint fi = get_findex(frame_name);

    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, ch_name))
        vm_debug_panic("[VM] SSEND: canale non trovato!\n");
    uint chi = char_id_map_get(&vm->frames[fi].VarIndexer, ch_name);
    Var *chv = vm->frames[fi].vars[chi];
    if (!chv || chv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] SSEND: destinazione non è channel!\n");

    int encoded_len = 0;
    int encoded_cap = 16;
    int *encoded = malloc((size_t)encoded_cap * sizeof(int));
    if (!encoded)
        vm_debug_panic("[VM] SSEND: malloc fallita\n");

#define ENC_PUSH(_v) do { \
        if (encoded_len >= encoded_cap) { \
            encoded_cap *= 2; \
            int *tmp = realloc(encoded, (size_t)encoded_cap * sizeof(int)); \
            if (!tmp) { free(encoded); vm_debug_panic("realloc failed\n"); } \
            encoded = tmp; \
        } \
        encoded[encoded_len++] = (_v); \
    } while (0)

    for (int i = 0; i < payload_count; i++) {
        char *src_tok = tokv[i];
        if (char_id_map_exists(&vm->frames[fi].VarIndexer, src_tok)) {
            Var *src = get_var(vm, fi, src_tok, "SSEND");
            if (src == chv) {
                free(encoded);
                vm_debug_panic("[VM] SSEND: non puoi inviare il canale su se stesso\n");
            }
            if (src->T == TYPE_INT) {
                int val;
                var_par_mut_acquire(src);
                val = *(src->value);
                *(src->value) = 0;
                var_par_mut_release(src);
                ENC_PUSH((int)TYPE_INT);
                ENC_PUSH(val);
            } else if (src->T == TYPE_STACK) {
                size_t n = src->stack_len;
                ENC_PUSH((int)TYPE_STACK);
                ENC_PUSH((int)n);
                for (size_t k = 0; k < n; k++)
                    ENC_PUSH(src->value[k]);
                src->stack_len = 0;
            } else if (src->T == TYPE_CHANNEL) {
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
            ENC_PUSH((int)strtoul(src_tok, NULL, 10));
        }
    }

    /* Solo canali mutex Mnemo (`__mn_mtx_*`): mailbox; altrimenti rendezvous stretto (π-calculus). */
    int mailbox_eligible = (
        encoded_len == 2 && encoded[0] == (int)TYPE_INT
        && strncmp(ch_name, "__mn_mtx_", (size_t)9) == 0
    );

    pthread_mutex_lock(&chv->channel->mtx);
    if (encoded_len > 0) {
        chv->channel->buf = realloc(chv->channel->buf, (chv->channel->buf_len + (size_t)encoded_len) * sizeof(int));
        if (!chv->channel->buf) {
            pthread_mutex_unlock(&chv->channel->mtx);
            free(encoded);
            vm_debug_panic("realloc failed\n");
        }
        memcpy(chv->channel->buf + chv->channel->buf_len, encoded, (size_t)encoded_len * sizeof(int));
        chv->channel->buf_len += (size_t)encoded_len;
    }
    pthread_mutex_unlock(&chv->channel->mtx);
    free(encoded);

#undef ENC_PUSH

    int w = op_wait(chv->channel, 1, mailbox_eligible);
    if (w == 1)
        wait_for_turn_done(current_thread_args);
}

/*
 * Verifica se buf contiene un messaggio ssend completo per recv_count destinazioni.
 * Ritorna la lunghezza in parole int consumate, o 0 se incompleto.
 */
static inline size_t peek_ssend_payload_words(Channel *ch, int recv_count)
{
    size_t read_idx = 0;
    size_t buf_len = ch->buf_len;

    for (int i = 0; i < recv_count; i++) {
        if (read_idx >= buf_len)
            return 0;
        int marker = ch->buf[read_idx++];
        if (marker == (int)TYPE_INT) {
            if (read_idx >= buf_len)
                return 0;
            read_idx++;
        } else if (marker == CHANNEL_REF_MARKER) {
            if (read_idx + 1 >= buf_len)
                return 0;
            read_idx += 2;
        } else if (marker == (int)TYPE_STACK || marker == (int)TYPE_CHANNEL) {
            if (read_idx >= buf_len)
                return 0;
            int n = ch->buf[read_idx++];
            if (n < 0 || read_idx + (size_t)n > buf_len)
                return 0;
            read_idx += (size_t)n;
        } else {
            return 0;
        }
    }
    return read_idx;
}

static inline void op_srecv(VM *vm, const char *frame_name)
{
    char *tokv[64];
    int ntok = 0;
    char *tok = NULL;
    while ((tok = strtok(NULL, " \t")) && ntok < 64)
        tokv[ntok++] = tok;

    if (ntok < 2)
        vm_debug_panic("[VM] SRECV: formato errato (atteso SRECV <v1 ...> <channel>)\n");

    char *ch_name = tokv[ntok - 1];
    int recv_count = ntok - 1;
    uint fi = get_findex(frame_name);

    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, ch_name))
        vm_debug_panic("[VM] SRECV: channel non trovato!\n");
    uint chi = char_id_map_get(&vm->frames[fi].VarIndexer, ch_name);
    Var *chv = vm->frames[fi].vars[chi];
    if (!chv || chv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] SRECV: sorgente non è channel!\n");

    pthread_mutex_lock(&chv->channel->mtx);
    size_t msg_words = 0;
    if (strncmp(ch_name, "__mn_mtx_", (size_t)9) == 0)
        msg_words = peek_ssend_payload_words(chv->channel, recv_count);
    if (msg_words > 0) {
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
                int popped = chv->channel->buf[read_idx++];
                if (dest->T != TYPE_INT) {
                    pthread_mutex_unlock(&chv->channel->mtx);
                    vm_debug_panic("[VM] SRECV: payload int richiede destinazione int\n");
                }
                var_par_mut_acquire(dest);
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
                int n = chv->channel->buf[read_idx++];
                if (n < 0 || read_idx + (size_t)n > chv->channel->buf_len) {
                    pthread_mutex_unlock(&chv->channel->mtx);
                    vm_debug_panic("[VM] SRECV: lunghezza payload non valida\n");
                }
                if (dest->T != TYPE_STACK && dest->T != TYPE_CHANNEL) {
                    pthread_mutex_unlock(&chv->channel->mtx);
                    vm_debug_panic("[VM] SRECV: payload stack/channel richiede destinazione stack o channel\n");
                }
                if (n > 0) {
                    dest->value = realloc(dest->value, (dest->stack_len + (size_t)n) * sizeof(int));
                    if (!dest->value) {
                        pthread_mutex_unlock(&chv->channel->mtx);
                        vm_debug_panic("realloc failed\n");
                    }
                    memcpy(dest->value + dest->stack_len, chv->channel->buf + read_idx, (size_t)n * sizeof(int));
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
            memmove(chv->channel->buf, chv->channel->buf + read_idx, remaining * sizeof(int));
        chv->channel->buf_len = remaining;
        if (remaining > 0) {
            chv->channel->buf = realloc(chv->channel->buf, remaining * sizeof(int));
            if (!chv->channel->buf) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("realloc failed\n");
            }
        }
        pthread_mutex_unlock(&chv->channel->mtx);
        return;
    }
    pthread_mutex_unlock(&chv->channel->mtx);

    op_wait(chv->channel, 0, 0);

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
            int popped = chv->channel->buf[read_idx++];
            if (dest->T != TYPE_INT) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: payload int richiede destinazione int\n");
            }
            var_par_mut_acquire(dest);
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
            int n = chv->channel->buf[read_idx++];
            if (n < 0 || read_idx + (size_t)n > chv->channel->buf_len) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: lunghezza payload non valida\n");
            }
            if (dest->T != TYPE_STACK && dest->T != TYPE_CHANNEL) {
                pthread_mutex_unlock(&chv->channel->mtx);
                vm_debug_panic("[VM] SRECV: payload stack/channel richiede destinazione stack o channel\n");
            }
            if (n > 0) {
                dest->value = realloc(dest->value, (dest->stack_len + (size_t)n) * sizeof(int));
                if (!dest->value) {
                    pthread_mutex_unlock(&chv->channel->mtx);
                    vm_debug_panic("realloc failed\n");
                }
                memcpy(dest->value + dest->stack_len, chv->channel->buf + read_idx, (size_t)n * sizeof(int));
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
        memmove(chv->channel->buf, chv->channel->buf + read_idx, remaining * sizeof(int));
    chv->channel->buf_len = remaining;
    if (remaining > 0) {
        chv->channel->buf = realloc(chv->channel->buf, remaining * sizeof(int));
        if (!chv->channel->buf) {
            pthread_mutex_unlock(&chv->channel->mtx);
            vm_debug_panic("realloc failed\n");
        }
    }
    pthread_mutex_unlock(&chv->channel->mtx);

    notify_sender_turn_done(sender_to_wake);
}

/* ======================================================================
 *  SHOW
 * ====================================================================== */

static inline void op_show(VM *vm, const char *frame_name)
{
    if (vm->suppress_show) return;
    char *ID = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_debug_panic("[VM] SHOW: troppi parametri!\n");
    uint fi = get_findex(frame_name);
    Var *v  = get_var(vm, fi, ID, "SHOW");

    if (v->T == TYPE_INT) {
        vm_printf("%s: %d\n", ID, *(v->value));
    } else if (v->T == TYPE_STACK) {
        char open = (v->T == TYPE_STACK) ? '[' : '<';
        char clos = (v->T == TYPE_STACK) ? ']' : '>';
        vm_printf("%s: %c", ID, open);
        for (size_t k = 0; k < v->stack_len; k++) {
            vm_printf("%d", v->value[k]);
            if (k + 1 < v->stack_len) vm_printf(", ");
        }
        vm_printf("%c\n", clos);
    } else if (v->T == TYPE_CHANNEL) {
        vm_printf("%s: <", ID);
        pthread_mutex_lock(&v->channel->mtx);
        for (size_t k = 0; k < v->channel->buf_len; k++) {
            vm_printf("%d", v->channel->buf[k]);
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

static inline int eval_cond(int lval, const char *op, int rval)
{
    if (!strcmp(op, "==")) return lval == rval;
    if (!strcmp(op, "!=")) return lval != rval;
    if (!strcmp(op, ">=")) return lval >= rval;
    if (!strcmp(op, "<=")) return lval <= rval;
    if (!strcmp(op, ">"))  return lval >  rval;
    if (!strcmp(op, "<"))  return lval <  rval;
    vm_debug_panic("[VM] operatore di confronto sconosciuto: '%s'\n", op);
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
    char  rhs[256]; read_rest_of_expr(rhs, sizeof(rhs)); /* espressione destra */

    if (!lhs_tok || !op_tok || rhs[0] == '\0') {
        vm_debug_panic("[VM] EVAL: formato errato (atteso: EVAL <lhs> <op> <rhs>)\n");
    }

    uint fi   = get_findex(frame_name);
    int  lval = resolve_value(vm, fi, lhs_tok);
    int  rval = resolve_value(vm, fi, rhs);

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
    char  rhs[256]; read_rest_of_expr(rhs, sizeof(rhs));

    if (!lhs_tok || !op_tok || rhs[0] == '\0') {
        vm_debug_panic("[VM] ASSERT: formato errato (atteso: ASSERT <lhs> <op> <rhs>)\n");
    }

    /* thread_val_IF contiene il risultato dell'ultimo EVAL, che nel caso IF/ELSE
       è la condizione FI appena valutata. */
    int fi_result = (int)thread_val_IF;

    /* Regola runtime richiesta: se abbiamo seguito il ramo ELSE e, a fine IF,
       la condizione FI risulta vera, segnaliamo errore.
       Nei casi con call/uncall nel blocco IF (es. ricorsione), manteniamo il
       comportamento storico per non introdurre regressioni. */
    if (if_branch_top >= 0) {
        int took_then = if_branch_stack[if_branch_top--];
        int has_call  = if_branch_has_call_stack[if_branch_top + 1];
        if (!has_call && !took_then && fi_result) {
            vm_debug_panic(
                "[VM] IF/FI non reversibile: ramo else ma condizione fi=vera (frame=%s lhs=%s op=%s rhs=%s fi=%d call=%d)\n",
                frame_name, lhs_tok, op_tok, rhs, fi_result, has_call);
        }
        if (!has_call && took_then && !fi_result) {
            vm_debug_panic(
                "[VM] IF/FI non reversibile: eseguito ramo then, ma guardia del FI falsa (frame=%s lhs=%s op=%s rhs=%s fi=%d call=%d)\n",
                frame_name, lhs_tok, op_tok, rhs, fi_result, has_call);
        }
        return;
    }

    /* Fallback conservativo fuori dal contesto IF. */
    uint fi = get_findex(frame_name);
    int  lval = resolve_value(vm, fi, lhs_tok);
    int  rval = resolve_value(vm, fi, rhs);
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
    uint  li     = char_id_map_get(&vm->frames[fi].LabelIndexer, lbl);
    char *newptr = go_to_line(buf, vm->frames[fi].label[li] + 1);
    if (!newptr) vm_debug_panic("[VM] JMP: label non trovata!\n");
    return newptr;
}

static inline char *op_jmpf(VM *vm, const char *fname, char *buf)
{
    char *lbl = strtok(NULL, " \t");

    /* JMPF con target ELSE_* identifica il branching di IF.
       Memorizziamo il ramo scelto per validare poi la condizione FI in ASSERT. */
    if (lbl && !strncmp(lbl, "ELSE_", 5)) {
        if (if_branch_top + 1 >= IF_BRANCH_STACK_MAX) {
            vm_debug_panic("[VM] IF stack overflow durante JMPF\n");
        }
        if_branch_stack[++if_branch_top] = thread_val_IF ? 1 : 0;
        if_branch_has_call_stack[if_branch_top] = 0;
    }

    if (thread_val_IF) return NULL;
    uint fi = get_findex(fname);
    if (!char_id_map_exists(&vm->frames[fi].LabelIndexer, lbl)) vm_debug_panic("EXIT_FAILURE");
    uint  li     = char_id_map_get(&vm->frames[fi].LabelIndexer, lbl);
    char *newptr = go_to_line(buf, vm->frames[fi].label[li] + 1);
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
    uint  fi    = get_findex(frame_name);

    pthread_mutex_lock(&var_indexer_mtx);
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, Vname);
    pthread_mutex_unlock(&var_indexer_mtx);

    /* Se vm_exec ha già allocato questa variabile tramite DECL, la
       liberiamo: LOCAL è l'allocazione runtime autorevole. */
    if (vm->frames[fi].vars[vi])
        delete_var(vm->frames[fi].vars, &vm->frames[fi].var_count, (int)vi);

    vm->frames[fi].vars[vi] = malloc(sizeof(Var));
    if (!vm->frames[fi].vars[vi]) vm_debug_panic("[VM] LOCAL: malloc fallita\n");
    alloc_var(vm->frames[fi].vars[vi], Vtype, Vname);

    if (vi >= (uint)vm->frames[fi].var_count)
        vm->frames[fi].var_count = vi + 1;

    Var *dst = vm->frames[fi].vars[vi];

    if (dst->T == TYPE_CHANNEL && vm->inversion_depth > 0) {
        Channel *restored = channel_restore_pop(frame_name, Vname);
        if (restored) {
            pthread_mutex_destroy(&dst->channel->mtx);
            free(dst->channel->buf);
            free(dst->channel);
            dst->channel = restored;
        }
    }

    if (c_val && char_id_map_exists(&vm->frames[fi].VarIndexer, c_val)) {
        uint  si  = char_id_map_get(&vm->frames[fi].VarIndexer, c_val);
        Var  *src = vm->frames[fi].vars[si];
        if (!src) vm_debug_panic("[VM] LOCAL: sorgente NULL\n");
        if (src->T == TYPE_INT)
            *(dst->value) = *(src->value);
        else if (src->T == TYPE_STACK) {
            dst->stack_len = src->stack_len;
            memcpy(dst->value, src->value, src->stack_len * sizeof(int));
        } else {
            vm_debug_panic("[VM] LOCAL: copia da PARAM non linkato\n");
        }
    } else {
        if (dst->T == TYPE_INT)
            *(dst->value) = c_val ? (int)strtol(c_val, NULL, 10) : 0;
        else if (dst->T == TYPE_STACK) {
            if (c_val && strcmp(c_val, "nil") != 0)
                vm_debug_panic("[VM] LOCAL: valore stack non compatibile\n");
        }
    }

    stack_push(&vm->frames[fi].LocalVariables, dst);
}

static inline void op_delocal(VM *vm, const char *frame_name)
{
    char *Vtype = strtok(NULL, " \t");
    char *Vname = strtok(NULL, " \t");
    char *c_val = strtok(NULL, " \t");
    uint  fi    = get_findex(frame_name);

    /* ── 1. Valore atteso ── */
    int Vvalue = 0;
    if (c_val) {
        if (char_id_map_exists(&vm->frames[fi].VarIndexer, c_val)) {
            Vvalue = resolve_value(vm, fi, c_val);
        } else {
            Vvalue = (int)strtol(c_val, NULL, 10);
        }
    }

    /* ── 2. Pop ── */
    Var *V = stack_pop(&vm->frames[fi].LocalVariables);

    /* ── 3. Ordine LIFO ── */
    if (strcmp(V->name, Vname) != 0) {
        vm_debug_panic("[VM] DELOCAL: ordine errato! atteso '%s', trovato '%s'\n", Vname, V->name);
    }

    /* ── 4. Tipo ── */
    const char *actual_type = (V->T == TYPE_INT)  ? "int"
                            : (V->T == TYPE_STACK) ? "stack"
                                                   : "channel";
    if (strcmp(Vtype, actual_type) != 0) {
        vm_debug_panic("[VM] DELOCAL: tipo errato! atteso %s, trovato %s\n",
                actual_type, Vtype);
    }

    /* ── 5. Valore finale ── */
    int ok = 0;
    if      (V->T == TYPE_INT)     ok = (*(V->value) == Vvalue);
    else if (V->T == TYPE_STACK)   ok = (V->stack_len == 0 && c_val && strcmp(c_val, "nil")   == 0);
    else if (V->T == TYPE_CHANNEL) ok = (V->channel->buf_len == 0 && c_val && strcmp(c_val, "empty") == 0);

    if (!ok) {
        if (V->T == TYPE_INT)
            vm_debug_panic(
                "[VM] DELOCAL: valore finale errato! (frame=%s var=%s, atteso=%d, trovato=%d, c_val=%s)\n",
                frame_name, Vname, Vvalue, *(V->value), c_val ? c_val : "NULL");
        else
            vm_debug_panic("[VM] DELOCAL: %s non è nil/empty!\n", Vname);
        
    }

    /* ── 6. Distruggi ── */
    pthread_mutex_lock(&var_indexer_mtx);
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, Vname);
    pthread_mutex_unlock(&var_indexer_mtx);

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

    delete_var(vm->frames[fi].vars, &vm->frames[fi].var_count, (int)vi);
}

#endif /* VM_OPS_H */