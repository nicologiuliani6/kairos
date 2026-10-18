#ifndef VM_PAR_H
#define VM_PAR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include "vm_types.h"
#include "vm_session.h"
#include "vm_helpers.h"
#include "vm_frames.h"
#include "vm_ops.h"
#include "vm_debug.h"

static inline int par_extract_srcline(const char *raw_line)
{
    const char *at = strchr(raw_line, '@');
    if (!at) return 0;
    return atoi(at + 1);
}

/* Ramo di par che si sta invertendo (righe [lo, hi] del frame base
 * g_inv_branch_frame); lo = 0: nessuno. L'inverter non salta il par che lo
 * racchiude e non svuota lo stack dei local di quel frame, condiviso con gli
 * altri rami. Vale per il solo thread del ramo. */
static __thread uint g_inv_branch_lo = 0;
static __thread uint g_inv_branch_hi = 0;
static __thread uint g_inv_branch_frame = 0;

/* Forward declarations — definite rispettivamente in vm_par.h (thread_entry)
   e in Kairos.c (vm_run_BT, invert_op_to_line). */
static void *thread_entry(void *arg);
static void vm_invert_free_cache(void);   /* vm_invert.h: cache __thread dell'inversione */
void vm_run_BT(VM *vm, char *buffer, char *frame_name_init);
void invert_op_to_line(VM *vm, const char *frame_name, char *buffer,
                       uint start, uint stop, int honor_if_line_skip);

/* ======================================================================
 *  PAR — struttura di un blocco parallelo
 * ====================================================================== */

/* Inizio di ciascun ramo: quanti sono i rami, tanti sono i thread. Il vettore
 * cresce (prima ne teneva 16 e i rami in più venivano ignorati in silenzio);
 * lo libera par_block_free. */
typedef struct {
    char **starts;
    int    count;
    int    cap;
    char  *after_end;   /* puntatore dopo PAR_END + '\n' */
} ParBlock;

static inline void par_block_free(ParBlock *pb)
{
    free(pb->starts);
    pb->starts = NULL;
    pb->count = pb->cap = 0;
}

static inline ParBlock scan_par_block(char *par_ptr)
{
    ParBlock pb = { .starts = NULL, .count = 0, .cap = 0, .after_end = NULL };
    int   depth = 1;
    char *scan  = par_ptr;

    while (*scan && depth > 0) {
        char *nl = strchr(scan, '\n');
        if (!nl) break;
        VM_LINE_DUP(tmp, scan, nl);
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw) {
            if      (strcmp(fw, "PAR_START") == 0) depth++;
            else if (strcmp(fw, "PAR_END")   == 0) {
                depth--;
                if (depth == 0) { pb.after_end = nl + 1; break; }
            } else if (strncmp(fw, "THREAD_", 7) == 0 && depth == 1) {
                if (pb.count == pb.cap) {
                    pb.cap = pb.cap ? pb.cap * 2 : 8;
                    char **n = (char **)realloc(pb.starts, sizeof(char *) * (size_t)pb.cap);
                    if (!n) vm_debug_panic("[VM] PAR: out of memory\n");
                    pb.starts = n;
                }
                pb.starts[pb.count++] = nl + 1;
            }
        }
        scan = nl + 1;
    }
    return pb;
}

static inline void exec_par_threads(VM *vm, char *buffer, const char *frame_name,
                                    ParBlock *pb, int is_inverse)
{
    pthread_mutex_t done_mtx  = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  done_cond = PTHREAD_COND_INITIALIZER;

    ThreadArgs **args = (ThreadArgs **)calloc((size_t)(pb->count ? pb->count : 1),
                                              sizeof(ThreadArgs *));
    if (!args) vm_debug_panic("[VM] PAR: out of memory\n");
    for (int t = 0; t < pb->count; t++) {
        args[t] = calloc(1, sizeof(ThreadArgs));
        args[t]->vm        = vm;
        args[t]->buffer    = buffer;          /* bytecode condiviso, sola lettura */
        args[t]->start_ptr = pb->starts[t];
        args[t]->done_mtx    = &done_mtx;
        args[t]->done_cond   = &done_cond;
        args[t]->is_inverse  = is_inverse;
        args[t]->frame_name  = char_id_strdup(frame_name);
        size_t pl = strlen(g_branch_path) + 24;
        args[t]->branch_path = (char *)malloc(pl);
        if (!args[t]->branch_path) vm_debug_panic("[VM] PAR: out of memory\n");
        snprintf(args[t]->branch_path, pl, "%s/%d", g_branch_path, t);
        args[t]->inversion_depth = g_inversion_depth;
    }

    /* Tutti i thread partono insieme; canali e lock sui parametri serializzano
       gli accessi condivisi dove serve. */
    /* i rami partono eseguibili; chi li lancia smette di esserlo finche' aspetta */
    __atomic_add_fetch(&g_runnable, pb->count, __ATOMIC_SEQ_CST);
    run_dec();
    for (int t = 0; t < pb->count; t++)
        if (pthread_create(&args[t]->tid, NULL, thread_entry, args[t]) != 0)
            vm_debug_panic("[VM] PAR: cannot create the thread of branch %d of %d "
                           "(system out of memory)\n", t, pb->count);

    pthread_mutex_lock(&done_mtx);
    for (;;) {
        int done = 0, blocked = 0;
        for (int t = 0; t < pb->count; t++) {
            if (args[t]->finished) done++;
            else if (args[t]->blocked) blocked++;
        }
        if (done == pb->count) break;

        /* Diagnosi di stallo. Se ogni thread del blocco o ha finito o e' fermo
           in attesa su un canale, nessuno potra' piu' sbloccare nessun altro:
           il blocco e' definitivo. Sostituisce il vecchio controllo statico
           "canale con un solo endpoint", che indovinava dal testo del programma
           cio' che qui si osserva. Si conferma dopo un'attesa, perche' lo stato
           "tutti fermi" e' anche transitorio durante un rendez-vous. */
        if (done + blocked == pb->count) {
            unsigned prog = __atomic_load_n(&g_progress, __ATOMIC_SEQ_CST);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200L * 1000L * 1000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&done_cond, &done_mtx, &ts) == ETIMEDOUT) {
                int done2 = 0, blocked2 = 0;
                for (int t = 0; t < pb->count; t++) {
                    if (args[t]->finished) done2++;
                    else if (args[t]->blocked) blocked2++;
                }
                if (done2 == done && done2 + blocked2 == pb->count && blocked2 > 0 &&
                    __atomic_load_n(&g_runnable, __ATOMIC_SEQ_CST) <= 0 &&
                    __atomic_load_n(&g_progress, __ATOMIC_SEQ_CST) == prog) {
                    pthread_mutex_unlock(&done_mtx);
                    vm_debug_panic(
                        "[VM] SESSION: deadlock, every thread of the par is waiting "
                        "on a channel and no partner can arrive any more; the thread to synchronize "
                        "with is missing, or both sides offer the same direction\n");
                }
            }
            continue;
        }
        pthread_cond_wait(&done_cond, &done_mtx);
    }
    pthread_mutex_unlock(&done_mtx);
    run_inc();   /* i rami sono finiti: chi li ha lanciati riparte */

    for (int t = 0; t < pb->count; t++) {
        pthread_join(args[t]->tid, NULL);
        free(args[t]->frame_name);
        free(args[t]->branch_path);
        free(args[t]);
    }
    free(args);
}

/* ======================================================================
 *  thread_entry
 * ====================================================================== */

static void *thread_entry(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    //fprintf(stderr, "[THREAD_ENTRY] ptr=%p is_inverse=%d\n", (void*)args, args->is_inverse);
    VM         *vm   = args->vm;
    const char *fname = args->frame_name;   /* vive quanto il thread */
    current_thread_args = args;
    g_branch_path = args->branch_path;
    g_inversion_depth = args->inversion_depth;
    session_thread_begin();   /* vm_session.h */
    //fprintf(stderr, "[THREAD] avviato is_inverse=%d\n", args->is_inverse);
    Stack branch_locals;
    stack_init(&branch_locals);
    g_branch_locals_fi = get_findex(fname);
    g_branch_locals = &branch_locals;

    if (args->is_inverse) {
        /* Le righe del ramo, quante sono: il vettore cresce su richiesta. */
        int    lines_cap = 64;
        char **lines = malloc(sizeof(char *) * (size_t)lines_cap);
        int nlines = 0;
        int has_complex = 0;
        char *scan = args->start_ptr;
        while (scan && *scan) {
            char *nl = strchr(scan, '\n');
            if (!nl) break;
            VM_LINE_DUP(lb, scan, nl);
            char *fw = strtok(skip_lineno(lb), " \t");
            if (!fw || strncmp(fw, "THREAD_", 7) == 0 || !strcmp(fw, "PAR_END")) break;
            if (!strcmp(fw, "CALL") || !strcmp(fw, "UNCALL") ||
                !strcmp(fw, "JMP") || !strcmp(fw, "JMPF") ||
                !strcmp(fw, "EVAL") || !strcmp(fw, "ASSERT") ||
                !strcmp(fw, "PAR_START")) {
                has_complex = 1;
            }
            if (nlines == lines_cap) {
                lines_cap *= 2;
                lines = realloc(lines, sizeof(char *) * (size_t)lines_cap);
            }
            lines[nlines++] = strndup(scan, (size_t)(nl - scan));
            scan = nl + 1;
        }

        if (!has_complex) {
            for (int i = nlines - 1; i >= 0; i--) {
                VM_LINE_COPY(lb, lines[i]);
                char *fw = strtok(skip_lineno(lb), " \t");
                if (!fw) continue;
                if      (!strcmp(fw, "PUSHEQ")) op_pusheq_inv(vm, fname);
                else if (!strcmp(fw, "MINEQ"))  op_mineq_inv (vm, fname);
                else if (!strcmp(fw, "XOREQ"))  op_xoreq_inv (vm, fname);
                else if (!strcmp(fw, "SWAP"))   op_swap_inv  (vm, fname);
                else if (!strcmp(fw, "PUSH"))   op_pop       (vm, fname);
                else if (!strcmp(fw, "POP"))    op_push      (vm, fname);
                else if (!strcmp(fw, "SSEND"))  op_srecv     (vm, fname);
                else if (!strcmp(fw, "SRECV"))  op_ssend     (vm, fname);
                else if (!strcmp(fw, "LOCAL"))  op_delocal   (vm, fname);
                else if (!strcmp(fw, "DELOCAL"))op_local     (vm, fname);
                else if (!strcmp(fw, "SHOW"))   op_show(vm, fname);
                else if (!strcmp(fw, "START") || !strcmp(fw, "PARAM") ||
                         !strcmp(fw, "LABEL") || !strcmp(fw, "DECL")) { /* skip */ }
                else { vm_debug_panic("[THREAD-INV] unknown op: '%s'\n", fw); }
            }
            for (int k = 0; k < nlines; k++) free(lines[k]);
            free(lines);
            goto thread_exit;
        }
        /* Ramo con chiamate, condizionali, cicli o par annidati: lo si inverte
         * come qualunque sequenza, I(c1;...;cn) = I(cn);...;I(c1), con
         * l'inverter sull'intervallo di righe del ramo. */
        uint lo = (uint)atoi(args->start_ptr);
        uint hi = nlines ? (uint)atoi(lines[nlines - 1]) : 0;
        for (int k = 0; k < nlines; k++) free(lines[k]);
        free(lines);
        if (nlines) {
            VM_FRAME_BASE(bbase, fname);
            g_inv_branch_frame = char_id_map_get(&FrameIndexer, bbase);
            g_inv_branch_lo = lo;
            g_inv_branch_hi = hi;
            invert_op_to_line(vm, fname, args->buffer, hi, lo - 1, 1);
            g_inv_branch_lo = g_inv_branch_hi = 0;
        }
        goto thread_exit;
    }

    char *ptr = args->start_ptr;

    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break;
        VM_LINE_DUP(lb, ptr, nl);
        char *fw = strtok(skip_lineno(lb), " \t");

        if (!fw || strncmp(fw, "THREAD_", 7) == 0 || !strcmp(fw, "PAR_END"))
            break;

        if (vm->dbg && vm->dbg->initialized) {
            if (!strcmp(fw, "DECL") || !strcmp(fw, "LABEL")) {
                vm->dbg->current_line = par_extract_srcline(ptr);
            } else if (strcmp(fw, "PARAM") != 0) {
                dbg_hook(vm->dbg, par_extract_srcline(ptr), fname, lb, 0);
            }
        }
        if (!strcmp(fw, "PAR_START")) {
            ParBlock pb = scan_par_block(nl + 1);
            exec_par_threads(vm, args->buffer, fname, &pb, args->is_inverse);
            ptr = pb.after_end ? pb.after_end : nl + 1;
            par_block_free(&pb);
            continue;
        }
        else if (!strcmp(fw, "SHOW"))   op_show(vm, fname);
        else if (!strcmp(fw, "PUSHEQ")) op_pusheq (vm, fname);
        else if (!strcmp(fw, "MINEQ"))  op_mineq  (vm, fname);
        else if (!strcmp(fw, "XOREQ"))  op_xoreq  (vm, fname);
        else if (!strcmp(fw, "SWAP"))   op_swap   (vm, fname);
        else if (!strcmp(fw, "PUSH"))
            { if (args->is_inverse) op_pop(vm, fname); else op_push(vm, fname); }
        else if (!strcmp(fw, "POP"))
            { if (args->is_inverse) op_push(vm, fname); else op_pop(vm, fname); }
        else if (!strcmp(fw, "SSEND"))
            { if (args->is_inverse) op_srecv(vm, fname); else op_ssend(vm, fname); }
        else if (!strcmp(fw, "SRECV"))
            { if (args->is_inverse) op_ssend(vm, fname); else op_srecv(vm, fname); }
        else if (!strcmp(fw, "LOCAL"))
            { if (args->is_inverse) op_delocal(vm, fname); else op_local(vm, fname); }
        else if (!strcmp(fw, "DELOCAL"))
            { if (args->is_inverse) op_local(vm, fname); else op_delocal(vm, fname); }
        else if (!strcmp(fw, "EVAL"))    op_eval   (vm, fname);
        else if (!strcmp(fw, "ASSERT"))  op_assert (vm, fname);
        else if (!strcmp(fw, "JMPF")) {
            char *np = op_jmpf(vm, fname, args->buffer);
            ptr = np ? np : nl + 1;
            continue;
        }
        else if (!strcmp(fw, "JMP")) {
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
            uint cfi = clone_frame_for_thread(vm, pn);
            int  pc = vm->frames[cfi]->param_count, *pi = vm->frames[cfi]->param_indices;
            VM_PARAM_SAVE(sv, pc); for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi]->vars[pi[k]];
            Stack slv = vm->frames[cfi]->LocalVariables; stack_init(&vm->frames[cfi]->LocalVariables);
            VM_FRAME_KEY_BUF(thread_key, pn); make_thread_frame_key(pn, thread_key, sizeof(thread_key));
            char *p = NULL; int ii = 0;
            while ((p = strtok(NULL, " \t")) && ii < pc) {
                int si = char_id_map_get(&vm->frames[cfi_cur]->VarIndexer, p);
                vm->frames[cfi]->vars[pi[ii++]] = vm->frames[cfi_cur]->vars[si];
            }
            /* Restore '\n' before any recursive scan on args->buffer (collect_ifs /
               vm_run_BT scan it line-by-line; an active '\0' would short-circuit strchr). */
            vm->frames[cfi]->active++;   /* per la rientranza mutua (Janus.c CALL) */
            if (do_invert)
                invert_op_to_line(vm, thread_key, args->buffer,
                                  vm->frames[cfi]->end_addr - 1, vm->frames[cfi]->addr + 1, 1);
            else {
                vm_run_BT(vm, args->buffer, thread_key);
            }
            vm->frames[cfi]->active--;
            for (int k = 0; k < pc; k++) vm->frames[cfi]->vars[pi[k]] = sv[k];
            stack_restore(&vm->frames[cfi]->LocalVariables, slv);
            VM_PARAM_SAVE_FREE(sv);
        }
        else if (!strcmp(fw, "LABEL"))  op_label(vm, fname, args->buffer);
        else if (!strcmp(fw, "START") ||
                 !strcmp(fw, "DECL") || !strcmp(fw, "PARAM")) { /* skip */ }
        else { vm_debug_panic("[THREAD] unknown op: '%s'\n", fw); }

        ptr = nl + 1;
    }
thread_exit:
    session_thread_end();
    g_branch_locals = NULL;
    stack_release(&branch_locals);
    vm_if_free_branch_stack();   /* vettori __thread di questo thread */
    vm_invert_free_cache();
    vm_cexpr_free_cache();
    /* Sveglia sender pendente prima di terminare */
    if (args->sender_to_notify) {
        ThreadArgs *s = args->sender_to_notify;
        args->sender_to_notify = NULL;
        notify_sender_turn_done(s);
    }
    pthread_mutex_lock(args->done_mtx);
    args->finished = 1;
    run_dec();
    __atomic_add_fetch(&g_progress, 1, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(args->done_cond);
    pthread_mutex_unlock(args->done_mtx);
    return NULL;
}


#endif /* VM_PAR_H */