#ifndef VM_OPS_H
#define VM_OPS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"
#include "vm_helpers.h"
#include "vm_channel.h"

#ifdef DAP_MODE
extern VM *g_current_vm; 
  #define vm_printf(...) do { \
      VMDebugState *_d = g_current_vm ? g_current_vm->dbg : NULL; \
      if (_d) { \
          if (_d->suppress_output) break; \
          int _avail = DBG_OUTPUT_BUF_SIZE - _d->out_len - 1; \
          if (_avail > 0) \
              _d->out_len += snprintf(_d->out_buf + _d->out_len, _avail, __VA_ARGS__); \
      } \
  } while(0)
#else
  #define vm_printf(...) printf(__VA_ARGS__)
#endif

/* ======================================================================
 *  SWAP
 * ====================================================================== */

void op_swap(VM *vm, const char *frame_name)
{
    char *ID1 = strtok(NULL, " \t"), *ID2 = strtok(NULL, " \t");
    uint  fi  = get_findex(frame_name);
    Var  *v1  = get_var(vm, fi, ID1, "SWAP");
    Var  *v2  = get_var(vm, fi, ID2, "SWAP");
    int   tmp = *(v1->value);
    *(v1->value) = *(v2->value);
    *(v2->value) = tmp;
}

#include "ops_arith.h"

/* ======================================================================
 *  PUSH / POP (con supporto channel)
 * ====================================================================== */

static inline void op_push(VM *vm, const char *frame_name);
static inline void op_pop (VM *vm, const char *frame_name);

static inline void op_push(VM *vm, const char *frame_name)
{
    char *C_val   = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_debug_panic("[VM] PUSH: troppi parametri!\n");

    uint fi  = get_findex(frame_name);
    int  val;

    if (char_id_map_exists(&vm->frames[fi].VarIndexer, C_val)) {
        Var *src = get_var(vm, fi, C_val, "PUSH");
        val = *(src->value);
        *(src->value) = 0;
    } else {
        val = (int)strtoul(C_val, NULL, 10);
    }

    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, C_stack))
        vm_debug_panic("[VM] PUSH: stack destinazione non trovato!\n");

    uint  si = char_id_map_get(&vm->frames[fi].VarIndexer, C_stack);
    Var  *sv = vm->frames[fi].vars[si];
    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL)
        vm_debug_panic("[VM] PUSH: destinazione non è stack/channel!\n");

    sv->value = realloc(sv->value, (sv->stack_len + 1) * sizeof(int));
    if (!sv->value) vm_debug_panic("realloc failed\n");
    sv->value[sv->stack_len++] = val;

    if (sv->T == TYPE_CHANNEL) {
        //fprintf(stderr, "[CHANNEL] %s is_send=%d stack_len=%zu\n", sv->name, sv->T==TYPE_CHANNEL?1:0, sv->stack_len);
        int was_queued = op_wait(sv->channel, 1);
        if (was_queued)
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

    if (sv->T == TYPE_CHANNEL) {
        pthread_mutex_lock(&sv->channel->mtx);
        if (sv->channel->send_q_head)
            sender_to_wake = sv->channel->send_q_head->thread_args;
        pthread_mutex_unlock(&sv->channel->mtx);

        op_wait(sv->channel, 0);

        if (!sender_to_wake) {
            pthread_mutex_lock(&sv->channel->mtx);
            sender_to_wake = sv->channel->sender_args;
            sv->channel->sender_args = NULL;
            pthread_mutex_unlock(&sv->channel->mtx);
        }
    }

    if (sv->T == TYPE_CHANNEL && sv->stack_len == 0) {
        vm_debug_panic("[VM] POP: channel vuoto dopo op_wait!\n");
    }

    int popped = sv->value[--sv->stack_len];
    if (sv->stack_len > 0)
        sv->value = realloc(sv->value, sv->stack_len * sizeof(int));

    Var *dest = get_var(vm, fi, C_dest, "POP");
    *(dest->value) += popped;

    if (sv->T == TYPE_CHANNEL && sender_to_wake && current_thread_args)
        current_thread_args->sender_to_notify = sender_to_wake;
}

/* ======================================================================
 *  SHOW
 * ====================================================================== */

static inline void op_show(VM *vm, const char *frame_name)
{
    char *ID = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_debug_panic("[VM] SHOW: troppi parametri!\n");
    uint fi = get_findex(frame_name);
    Var *v  = get_var(vm, fi, ID, "SHOW");

    if (v->T == TYPE_INT) {
        vm_printf("%s: %d\n", ID, *(v->value));
    } else if (v->T == TYPE_STACK || v->T == TYPE_CHANNEL) {
        char open = (v->T == TYPE_STACK) ? '[' : '<';
        char clos = (v->T == TYPE_STACK) ? ']' : '>';
        vm_printf("%s: %c", ID, open);
        for (size_t k = 0; k < v->stack_len; k++) {
            vm_printf("%d", v->value[k]);
            if (k + 1 < v->stack_len) vm_printf(", ");
        }
        vm_printf("%c\n", clos);
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
    if (vm->frames[fi].vars[vi]) {
        free(vm->frames[fi].vars[vi]->value);
        if (vm->frames[fi].vars[vi]->channel) {
            pthread_mutex_destroy(&vm->frames[fi].vars[vi]->channel->mtx);
            free(vm->frames[fi].vars[vi]->channel);
        }
        free(vm->frames[fi].vars[vi]);
        vm->frames[fi].vars[vi] = NULL;
    }

    vm->frames[fi].vars[vi] = malloc(sizeof(Var));
    if (!vm->frames[fi].vars[vi]) vm_debug_panic("[VM] LOCAL: malloc fallita\n");
    alloc_var(vm->frames[fi].vars[vi], Vtype, Vname);

    if (vi >= (uint)vm->frames[fi].var_count)
        vm->frames[fi].var_count = vi + 1;

    Var *dst = vm->frames[fi].vars[vi];

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
            uint src_vi = char_id_map_get(&vm->frames[fi].VarIndexer, c_val);
            Var *src    = vm->frames[fi].vars[src_vi];
            Vvalue = (src && src->T == TYPE_INT) ? *(src->value) : 0;
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
    else if (V->T == TYPE_CHANNEL) ok = (V->stack_len == 0 && c_val && strcmp(c_val, "empty") == 0);

    if (!ok) {
        if (V->T == TYPE_INT)
            vm_debug_panic( "[VM] DELOCAL: valore finale errato! (var=%s, atteso=%d, trovato=%d)\n",Vname, Vvalue, *(V->value));
        else
            vm_debug_panic("[VM] DELOCAL: %s non è nil/empty!\n", Vname);
        
    }

    /* ── 6. Distruggi ── */
    pthread_mutex_lock(&var_indexer_mtx);
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, Vname);
    pthread_mutex_unlock(&var_indexer_mtx);

    delete_var(vm->frames[fi].vars, &vm->frames[fi].var_count, (int)vi);
}

#endif /* VM_OPS_H */