#ifndef VM_PAR_H
#define VM_PAR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "vm_types.h"
#include "vm_helpers.h"
#include "vm_frames.h"
#include "vm_ops.h"

/* Forward declarations — definite rispettivamente in vm_par.h (thread_entry)
   e in Janus.c (vm_run_BT, invert_op_to_line). */
static void *thread_entry(void *arg);
void vm_run_BT(VM *vm, char *buffer, char *frame_name_init);
void invert_op_to_line(VM *vm, const char *frame_name, char *buffer,
                       uint start, uint stop);

/* ======================================================================
 *  PAR — struttura di un blocco parallelo
 * ====================================================================== */

typedef struct {
    char *starts[16];
    int   count;
    char *after_end;   /* puntatore dopo PAR_END + '\n' */
} ParBlock;

static inline ParBlock scan_par_block(char *par_ptr)
{
    ParBlock pb = { .count = 0, .after_end = NULL };
    int   depth = 1;
    char *scan  = par_ptr;

    while (*scan && depth > 0) {
        char *nl = strchr(scan, '\n');
        if (!nl) break;
        *nl = '\0';
        char tmp[512];
        strncpy(tmp, scan, sizeof(tmp) - 1);
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw) {
            if      (strcmp(fw, "PAR_START") == 0) depth++;
            else if (strcmp(fw, "PAR_END")   == 0) {
                depth--;
                if (depth == 0) { *nl = '\n'; pb.after_end = nl + 1; break; }
            } else if (strncmp(fw, "THREAD_", 7) == 0 && depth == 1 && pb.count < 16) {
                pb.starts[pb.count++] = nl + 1;
            }
        }
        *nl = '\n';
        scan = nl + 1;
    }
    return pb;
}

static inline void exec_par_threads(VM *vm, char *buffer, const char *frame_name,
                                    ParBlock *pb, int dup_buffer, int is_inverse)
{
    pthread_mutex_t done_mtx  = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  done_cond = PTHREAD_COND_INITIALIZER;

    ThreadArgs *args[16];
    for (int t = 0; t < pb->count; t++) {
        args[t] = calloc(1, sizeof(ThreadArgs));
        args[t]->vm        = vm;
        args[t]->buffer    = dup_buffer ? strdup(buffer) : buffer;
        args[t]->start_ptr = pb->starts[t];
        args[t]->done_mtx  = &done_mtx;
        args[t]->done_cond = &done_cond;
        args[t]->is_inverse = is_inverse;
        //fprintf(stderr, "[EXEC_PAR] t=%d is_inverse=%d ptr=%p\n", t, args[t]->is_inverse, (void*)args[t]);
        strncpy(args[t]->frame_name, frame_name, VAR_NAME_LENGTH - 1);
    }

    /* Avvia i thread uno alla volta, attendendo che si blocchino o terminino */
    for (int t = 0; t < pb->count; t++) {
        pthread_create(&args[t]->tid, NULL, thread_entry, args[t]);
        pthread_mutex_lock(&done_mtx);
        while (!args[t]->finished && !args[t]->blocked)
            pthread_cond_wait(&done_cond, &done_mtx);
        pthread_mutex_unlock(&done_mtx);
    }

    /* Attendi tutti */
    pthread_mutex_lock(&done_mtx);
    for (;;) {
        int done = 0;
        for (int t = 0; t < pb->count; t++) done += args[t]->finished;
        if (done == pb->count) break;
        pthread_cond_wait(&done_cond, &done_mtx);
    }
    pthread_mutex_unlock(&done_mtx);

    for (int t = 0; t < pb->count; t++) {
        pthread_join(args[t]->tid, NULL);
        if (dup_buffer) free(args[t]->buffer);
        free(args[t]);
    }
}

/* ======================================================================
 *  thread_entry
 * ====================================================================== */

static void *thread_entry(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    //fprintf(stderr, "[THREAD_ENTRY] ptr=%p is_inverse=%d\n", (void*)args, args->is_inverse);
    VM         *vm   = args->vm;
    char        fname[VAR_NAME_LENGTH];
    strncpy(fname, args->frame_name, VAR_NAME_LENGTH - 1);
    fname[VAR_NAME_LENGTH - 1] = '\0';
    current_thread_args = args;
    //fprintf(stderr, "[THREAD] avviato is_inverse=%d\n", args->is_inverse);

    char *ptr = args->start_ptr;

    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[512]; strncpy(lb, ptr, sizeof(lb) - 1);
        char *fw = strtok(skip_lineno(lb), " \t");

        if (!fw || strncmp(fw, "THREAD_", 7) == 0 || !strcmp(fw, "PAR_END"))
            { *nl = '\n'; break; }
        else if (!strcmp(fw, "PAR_START")) {
            *nl = '\n';
            ParBlock pb = scan_par_block(nl + 1);
            exec_par_threads(vm, args->buffer, fname, &pb, 0, args->is_inverse);
            ptr = pb.after_end ? pb.after_end : nl + 1;
            continue;
        }
        else if (!strcmp(fw, "SHOW"))   op_show   (vm, fname);
        else if (!strcmp(fw, "PUSHEQ")) op_pusheq (vm, fname);
        else if (!strcmp(fw, "MINEQ"))  op_mineq  (vm, fname);
        else if (!strcmp(fw, "XOREQ"))  op_xoreq  (vm, fname);
        else if (!strcmp(fw, "SWAP"))   op_swap   (vm, fname);
        else if (!strcmp(fw, "PUSH") || !strcmp(fw, "SSEND"))
            { if (args->is_inverse) op_pop(vm, fname); else op_push(vm, fname); }
        else if (!strcmp(fw, "POP")  || !strcmp(fw, "SRECV"))
            { if (args->is_inverse) op_push(vm, fname); else op_pop(vm, fname); }
        else if (!strcmp(fw, "LOCAL"))   op_local  (vm, fname);
        else if (!strcmp(fw, "DELOCAL")) op_delocal(vm, fname);
        else if (!strcmp(fw, "EVAL"))    op_eval   (vm, fname);
        else if (!strcmp(fw, "ASSERT"))  op_assert (vm, fname);
        else if (!strcmp(fw, "JMPF")) {
            *nl = '\n';
            char *np = op_jmpf(vm, fname, args->buffer);
            ptr = np ? np : nl + 1;
            continue;
        }
        else if (!strcmp(fw, "JMP")) {
            *nl = '\n';
            ptr = op_jmp(vm, fname, args->buffer);
            continue;
        }
        else if (!strcmp(fw, "CALL") || !strcmp(fw, "UNCALL")) {
            /* Quando is_inverse=1, CALL e UNCALL si scambiano di ruolo:
               CALL  → esegue invert_op_to_line  (come UNCALL)
               UNCALL→ esegue vm_run_BT           (come CALL)
               Quando is_inverse=0 il comportamento è quello canonico. */
            int do_invert = ((!strcmp(fw, "CALL")   &&  args->is_inverse) ||
                             (!strcmp(fw, "UNCALL")  && !args->is_inverse));
            char *pn      = strtok(NULL, " \t");
            uint  cfi_cur = get_findex(fname);
            pthread_mutex_lock(&var_indexer_mtx);
            uint cfi = clone_frame_for_thread(vm, pn);
            pthread_mutex_unlock(&var_indexer_mtx);
            int  pc = vm->frames[cfi].param_count, *pi = vm->frames[cfi].param_indices;
            Var *sv[64]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi].vars[pi[k]];
            Stack slv = vm->frames[cfi].LocalVariables; stack_init(&vm->frames[cfi].LocalVariables);
            char thread_key[VAR_NAME_LENGTH]; make_thread_frame_key(pn, thread_key, sizeof(thread_key));
            char *p = NULL; int ii = 0;
            while ((p = strtok(NULL, " \t")) && ii < pc) {
                int si = char_id_map_get(&vm->frames[cfi_cur].VarIndexer, p);
                vm->frames[cfi].vars[pi[ii++]] = vm->frames[cfi_cur].vars[si];
            }
            if (do_invert)
                invert_op_to_line(vm, thread_key, args->buffer,
                                  vm->frames[cfi].end_addr - 1, vm->frames[cfi].addr + 1);
            else
                vm_run_BT(vm, args->buffer, thread_key);
            for (int k = 0; k < pc; k++) vm->frames[cfi].vars[pi[k]] = sv[k];
            vm->frames[cfi].LocalVariables = slv;
        }
        else if (!strcmp(fw, "DECL") || !strcmp(fw, "PARAM") || !strcmp(fw, "LABEL")) { /* skip */ }
        else { fprintf(stderr, "[THREAD] op sconosciuta: '%s'\n", fw); exit(EXIT_FAILURE); }

        *nl = '\n'; ptr = nl + 1;
    }

    /* Sveglia sender pendente prima di terminare */
    if (args->sender_to_notify) {
        ThreadArgs *s = args->sender_to_notify;
        args->sender_to_notify = NULL;
        notify_sender_turn_done(s);
    }
    pthread_mutex_lock(args->done_mtx);
    args->finished = 1;
    pthread_cond_signal(args->done_cond);
    pthread_mutex_unlock(args->done_mtx);
    return NULL;
}

#endif /* VM_PAR_H */