#ifndef VM_OPS_H
#define VM_OPS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"
#include "vm_helpers.h"
#include "vm_channel.h"

/* ======================================================================
 *  SWAP — deve stare prima di ops_arith.h (che usa op_swap_inv → op_swap)
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
    if (strtok(NULL, " \t")) vm_fatal("[VM] PUSH: troppi parametri!\n");

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
        vm_fatal("[VM] PUSH: stack destinazione non trovato!\n");

    uint  si = char_id_map_get(&vm->frames[fi].VarIndexer, C_stack);
    Var  *sv = vm->frames[fi].vars[si];
    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL)
        vm_fatal("[VM] PUSH: destinazione non è stack/channel!\n");

    sv->value = realloc(sv->value, (sv->stack_len + 1) * sizeof(int));
    if (!sv->value) vm_fatal("realloc failed\n");
    sv->value[sv->stack_len++] = val;

    if (sv->T == TYPE_CHANNEL) {
        int was_queued = op_wait(sv->channel, 1);
        if (was_queued)
            wait_for_turn_done(current_thread_args);
    }
}

static inline void op_pop(VM *vm, const char *frame_name)
{
    char *C_dest  = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_fatal("[VM] POP: troppi parametri!\n");

    uint fi = get_findex(frame_name);
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, C_stack))
        vm_fatal("[VM] POP: stack non trovato!\n");

    uint  si = char_id_map_get(&vm->frames[fi].VarIndexer, C_stack);
    Var  *sv = vm->frames[fi].vars[si];

    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL) vm_fatal("[VM] POP: sorgente non è stack/channel!\n");
    if (sv->T == TYPE_STACK && sv->stack_len == 0)    vm_fatal("[VM] POP: stack vuoto!\n");

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
        fprintf(stderr, "[VM] POP: channel vuoto dopo op_wait!\n");
        exit(EXIT_FAILURE);
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
 *  SHOW / EVAL / ASSERT
 * ====================================================================== */

static inline void op_show(VM *vm, const char *frame_name)
{
    char *ID = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_fatal("[VM] SHOW: troppi parametri!\n");
    uint fi = get_findex(frame_name);
    Var *v  = get_var(vm, fi, ID, "SHOW");

    if (v->T == TYPE_INT) {
        printf("%s: %d\n", ID, *(v->value));
    } else if (v->T == TYPE_STACK || v->T == TYPE_CHANNEL) {
        char open = (v->T == TYPE_STACK) ? '[' : '<';
        char clos = (v->T == TYPE_STACK) ? ']' : '>';
        printf("%s: %c", ID, open);
        for (size_t k = 0; k < v->stack_len; k++) {
            printf("%d", v->value[k]);
            if (k + 1 < v->stack_len) printf(", ");
        }
        printf("%c\n", clos);
    } else {
        vm_fatal("[VM] SHOW su variabile PARAM non linkata!\n");
    }
}

static inline void op_eval(VM *vm, const char *frame_name)
{
    char *ID = strtok(NULL, " \t"), *C_val = strtok(NULL, " \t");
    uint  fi = get_findex(frame_name);
    Var  *v  = get_var(vm, fi, ID, "EVAL");
    thread_val_IF = (*(v->value) == resolve_value(vm, fi, C_val));
}

static inline void op_assert(VM *vm, const char *frame_name)
{
    char *ID1 = strtok(NULL, " \t"), *ID2 = strtok(NULL, " \t");
    if (!ID1 || !ID2) { fprintf(stderr, "[VM] ASSERT: argomenti mancanti\n"); return; }
    uint fi = get_findex(frame_name);
    (void)(resolve_value(vm, fi, ID1) != resolve_value(vm, fi, ID2));
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
    if (!newptr) vm_fatal("[VM] JMP: label non trovata!\n");
    return newptr;
}

static inline char *op_jmpf(VM *vm, const char *fname, char *buf)
{
    char *lbl = strtok(NULL, " \t");
    if (thread_val_IF) return NULL;
    uint fi = get_findex(fname);
    if (!char_id_map_exists(&vm->frames[fi].LabelIndexer, lbl)) exit(EXIT_FAILURE);
    uint  li     = char_id_map_get(&vm->frames[fi].LabelIndexer, lbl);
    char *newptr = go_to_line(buf, vm->frames[fi].label[li] + 1);
    if (!newptr) vm_fatal("[VM] JMPF: label non trovata!\n");
    return newptr;
}

/* ======================================================================
 *  LOCAL / DELOCAL
 * ====================================================================== */

static inline void op_local(VM *vm, const char *frame_name)
{
    char *Vtype = strtok(NULL, " \t"), *Vname = strtok(NULL, " \t"), *c_val = strtok(NULL, " \t");
    uint  fi    = get_findex(frame_name);

    pthread_mutex_lock(&var_indexer_mtx);
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, Vname);
    pthread_mutex_unlock(&var_indexer_mtx);

    vm->frames[fi].vars[vi] = malloc(sizeof(Var));
    alloc_var(vm->frames[fi].vars[vi], Vtype, Vname);

    if (vi >= (uint)vm->frames[fi].var_count)
        vm->frames[fi].var_count = vi + 1;

    Var *dst = vm->frames[fi].vars[vi];

    if (char_id_map_exists(&vm->frames[fi].VarIndexer, c_val)) {
        int  si  = char_id_map_get(&vm->frames[fi].VarIndexer, c_val);
        Var *src = vm->frames[fi].vars[si];
        if (src->T == TYPE_INT)
            *(dst->value) = *(src->value);
        else if (src->T == TYPE_STACK) {
            dst->stack_len = src->stack_len;
            memcpy(dst->value, src->value, src->stack_len * sizeof(int));
        } else {
            vm_fatal("[VM] LOCAL: copia da PARAM non linkato\n");
        }
    } else {
        if (dst->T == TYPE_INT)
            *(dst->value) = (int)strtol(c_val, NULL, 10);
        else if (dst->T == TYPE_STACK) {
            if (strcmp(c_val, "nil") != 0) vm_fatal("[VM] LOCAL: valore stack non compatibile\n");
        }
    }

    stack_push(&vm->frames[fi].LocalVariables, dst);
}

static inline void op_delocal(VM *vm, const char *frame_name)
{
    char *Vtype = strtok(NULL, " \t"), *Vname = strtok(NULL, " \t"), *c_val = strtok(NULL, " \t");
    uint  fi    = get_findex(frame_name);

    int Vvalue = 0;
    if (c_val) {
        if (char_id_map_exists(&vm->frames[fi].VarIndexer, c_val))
            Vvalue = *(vm->frames[fi].vars[char_id_map_get(&vm->frames[fi].VarIndexer, c_val)]->value);
        else
            Vvalue = (int)strtoul(c_val, NULL, 10);
    }

    Var *V = stack_pop(&vm->frames[fi].LocalVariables);
    const char *actual_type = (V->T == TYPE_INT) ? "int"
                            : (V->T == TYPE_STACK ? "stack" : "channel");

    if (strcmp(Vtype, actual_type) != 0) {
        fprintf(stderr, "[VM] DELOCAL: tipo errato! atteso %s, trovato %s\n", actual_type, Vtype);
        exit(EXIT_FAILURE);
    }

    int ok = 0;
    if      (V->T == TYPE_INT)     ok = (Vvalue == *(V->value));
    else if (V->T == TYPE_STACK)   ok = (V->stack_len == 0 && strcmp(c_val, "nil")   == 0);
    else if (V->T == TYPE_CHANNEL) ok = (V->stack_len == 0 && strcmp(c_val, "empty") == 0);

    if (!ok) {
        if (V->T == TYPE_INT)
            fprintf(stderr, "[VM] DELOCAL: valore finale errato! (%s, %d, %d)\n", Vname, Vvalue, *(V->value));
        else
            fprintf(stderr, "[VM] DELOCAL: %s non è nil/empty!\n", Vname);
        exit(EXIT_FAILURE);
    }

    pthread_mutex_lock(&var_indexer_mtx);
    delete_var(vm->frames[fi].vars, &vm->frames[fi].var_count,
               char_id_map_get(&vm->frames[fi].VarIndexer, Vname));
    pthread_mutex_unlock(&var_indexer_mtx);
}

#endif /* VM_OPS_H */