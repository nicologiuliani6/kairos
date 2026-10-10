#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── ordine di inclusione importante ── */
#define DEFINE_VM_DEBUG_PANIC
#include "vm_panic.h"   // ← deve venire prima di tutti gli altri
#include "vm_types.h"
#include "vm_helpers.h"
#include "vm_channel.h"
#include "vm_frames.h"
#include "vm_ops.h"
#include "vm_par.h"      /* deve venire prima: definisce ParBlock, scan_par_block, exec_par_threads */
#include "vm_invert.h"   /* usa ParBlock e exec_par_threads definiti sopra */
#include "vm_debug.h"    /* debug hook, dump JSON, breakpoint management  */
#include "Kairos_core.h"


/* Puntatore alla VM corrente — usato da vm_printf in DAP_MODE */
VM *g_current_vm = NULL;
void vm_print_stats(VM *vm);          /* fwd: --vm-stats (def più sotto) */
static int g_vm_stats_enabled;        /* tentative decl (def con =0 più sotto) */
/* ── thread-local state (dichiarate extern in vm_types.h) ── */
__thread ThreadArgs *current_thread_args = NULL;
__thread char       *strtok_saveptr      = NULL;
__thread uint        thread_val_IF       = 0;

pthread_mutex_t var_indexer_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t char_id_map_ins_mtx = PTHREAD_MUTEX_INITIALIZER;   /* char_id_map.h */
CharIdMap       FrameIndexer;

/* ======================================================================
 *  Macro DEBUG_HOOK — inserita prima di ogni istruzione in vm_run_BT.
 *
 *  Estrae il numero di riga dalla stringa corrente (i primi 4 caratteri
 *  del formato "NNNN  OP ...") e chiama dbg_hook se il debugger è attivo.
 *
 *  ptr punta all'inizio della riga (prima che venga modificata con \0).
 *  instr_text è lb (la copia della riga già disponibile).
 * ====================================================================== */

static inline int extract_lineno(const char *raw_line)
{
    /* Estrae il numero fisico di riga: tutte le cifre iniziali ("%04u" ne
       scrive almeno 4, oltre la riga 9999 di più). */
    return atoi(raw_line);
}

static inline int extract_srcline(const char *raw_line)
{
    /* Formato: "NNNN  @SRC   OP" — cerca '@' e legge il numero sorgente */
    const char *at = strchr(raw_line, '@');
    if (!at) return extract_lineno(raw_line); /* fallback al fisico */
    return atoi(at + 1);
}

#define DEBUG_HOOK(raw_ptr, instr_text)                                  \
    do {                                                                 \
        if (vm->dbg && vm->dbg->initialized) {                          \
            int _ln = extract_srcline(raw_ptr);                          \
            dbg_hook(vm->dbg, _ln, fname, instr_text);                  \
            if (vm->dbg->mode == VM_MODE_DONE &&                        \
                vm->inversion_depth == 0) { *nl='\n'; goto done; }      \
        }                                                                \
    } while(0)

/* ======================================================================
 *  vm_run_BT — loop principale di esecuzione
 * ====================================================================== */

void vm_stats_sample(VM *vm);

/* Nome del frame corrente di vm_run_BT, e nome del chiamante salvato in un
 * CallRecord: buffer che crescono solo quando il nome nuovo non ci sta. Nessuna
 * lunghezza massima e nessuna allocazione per chiamata a regime. */
static inline void vm_buf_set(char **buf, size_t *cap, const char *src)
{
    size_t l = strlen(src) + 1;
    if (l > *cap) {
        size_t nc = *cap ? *cap : 32;
        while (nc < l) nc *= 2;
        char *n = (char *)realloc(*buf, nc);
        if (!n) vm_debug_panic("[VM] nome di frame: memoria esaurita\n");
        *buf = n; *cap = nc;
    }
    memcpy(*buf, src, l);
}

void vm_run_BT(VM *vm, char *buffer, char *frame_name_init)
{
    g_current_vm = vm;
    char *orig = strdup(buffer);
    char  *fname = NULL;
    size_t fname_cap = 0;
    vm_buf_set(&fname, &fname_cap, frame_name_init);

    /* Un record per chiamata attiva. I parametri salvati e il nome del chiamante
     * sono buffer del record, riusati dalle chiamate successive alla stessa
     * profondità: crescono quando servono, nessun numero massimo di parametri
     * né lunghezza massima del nome. */
    typedef struct {
        char  *return_ptr;
        char  *caller_frame;
        size_t caller_frame_cap;
        Var  **saved_params;
        int    saved_params_cap;
        int    saved_param_count, callee_findex;
        Stack  saved_local_vars;
        int    is_recursive_clone;
        int    base_findex;   /* frame-base della proc chiamata: per decremento `active` a END_PROC */
    } CallRecord;
    /* Call stack dinamico: cresce on-demand (raddoppia). Nessun hard cap.
     * Gli slot nuovi sono azzerati: i loro buffer partono vuoti. */
    uint cs_cap = 64;
    CallRecord *cs = calloc(cs_cap, sizeof(CallRecord));
    if (!cs) vm_debug_panic("[VM] call stack: memoria esaurita\n");
    int cs_top = -1;
#define VM_CS_ENSURE(needed) do { \
    if ((uint)(needed) >= cs_cap) { \
        uint _nc = cs_cap; while (_nc <= (uint)(needed)) _nc *= 2; \
        CallRecord *_nb = (CallRecord *)realloc(cs, sizeof(CallRecord) * _nc); \
        if (!_nb) vm_debug_panic("[VM] CALL: realloc(%u) fallita\n", _nc); \
        memset(_nb + cs_cap, 0, sizeof(CallRecord) * (_nc - cs_cap)); \
        cs = _nb; cs_cap = _nc; \
    } \
} while (0)
    uint  si  = char_id_map_get(&FrameIndexer, fname);
    char *ptr = go_to_line(orig, vm->frames[si]->addr + 1);
    if (!ptr) { fprintf(stderr, "ERROR: '%s' non trovato\n", fname); goto done; }

    while (*ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        VM_LINE_COPY(lb, ptr);
        char *fw = strtok(skip_lineno(lb), " \t");

        if (!fw) { *nl = '\n'; ptr = nl + 1; continue; }

        /* ── DEBUG HOOK ── chiamato prima di ogni istruzione breakpointable ── */
        if (strcmp(fw, "PROC") != 0 && strcmp(fw, "PARAM") != 0 &&
            strcmp(fw, "HALT") != 0) {
            DEBUG_HOOK(ptr, lb);
            vm_stats_sample(vm);
        }

        if (!strcmp(fw, "END_PROC")) {
            uint fi = get_findex(fname);
            if (stack_size(&vm->frames[fi]->LocalVariables) > -1) {
                vm_debug_panic("[VM] END_PROC: variabili LOCAL non chiuse!\n");
            }
            *nl = '\n';
            if (cs_top >= 0) {
                int cfi = cs[cs_top].callee_findex;
                if (vm->frames[cs[cs_top].base_findex]->active > 0)
                    vm->frames[cs[cs_top].base_findex]->active--;
                for (int k = 0; k < cs[cs_top].saved_param_count; k++)
                    vm->frames[cfi]->vars[vm->frames[cfi]->param_indices[k]] = cs[cs_top].saved_params[k];
                stack_restore(&vm->frames[cfi]->LocalVariables, cs[cs_top].saved_local_vars);
                /* Non liberare i Var PARAM dei cloni ricorsivi qui: sono guscio allocato
                 * in init_clone_frame; free() tra restore e ripresa del chiamante può
                 * corrompere l'heap o interferire con alias ancora attivi. vm_free() a
                 * fine esecuzione deduplica i Var* condivisi. */
                ptr = cs[cs_top].return_ptr;
                vm_buf_set(&fname, &fname_cap, cs[cs_top].caller_frame);
                cs_top--;
            } else break;
            continue;
        }
        else if (!strcmp(fw, "CALL")) {
            vm_if_mark_call();
            char *pn      = strtok(NULL, " \t");
            uint  cfi_cur = get_findex(fname);
            int   is_rec    = vm_base_eq(fname, pn);
            int   new_depth = 0;
            if (is_rec) {
                char *at2 = strchr(fname, '@');
                int   cd  = 0;
                if (!at2)
                    cd = 0;
                else if (at2[1] >= '0' && at2[1] <= '9')
                    cd = atoi(at2 + 1);
                else {
                    uint cur_fi = get_findex(fname);
                    cd          = vm->frames[cur_fi]->recursion_depth;
                }
                new_depth = cd + 1;
            }
            /* Re-entrancy MUTUA: callee != caller (no self-rec) ma la sua
             * proc-base è già attiva sul call stack → clona come per la self-rec
             * (depth = #attivazioni correnti), così i LOCAL int del frame base
             * della call esterna non vengono liberati dal delocal di quella
             * interna. Solo path non-thread (i worker PAR usano clone_for_thread). */
            uint base_fi_pn = char_id_map_get(&FrameIndexer, pn);
            int  reentrant  = !is_rec && !current_thread_args
                              && vm->frames[base_fi_pn]->active > 0;
            int  reent_depth = reentrant ? vm->frames[base_fi_pn]->active + 1 : 0;
            uint cfi;
            if (is_rec) {
                cfi = clone_frame_for_depth(vm, pn, new_depth);
            } else if (reentrant) {
                cfi = clone_frame_for_depth(vm, pn, reent_depth);
            } else {
                cfi = current_thread_args ? clone_frame_for_thread(vm, pn)
                                          : char_id_map_get(&FrameIndexer, pn);
            }
            VM_CS_ENSURE((uint)(cs_top + 1));
            cs_top++;
            *nl = '\n';
            cs[cs_top].return_ptr         = nl + 1;
            cs[cs_top].is_recursive_clone = is_rec || reentrant;
            cs[cs_top].callee_findex      = cfi;
            cs[cs_top].base_findex        = (int)base_fi_pn;
            vm->frames[base_fi_pn]->active++;
            vm_buf_set(&cs[cs_top].caller_frame, &cs[cs_top].caller_frame_cap, fname);
            int  pc = vm->frames[cfi]->param_count, *pi = vm->frames[cfi]->param_indices;
            cs[cs_top].saved_param_count = pc;
            cs[cs_top].saved_local_vars  = vm->frames[cfi]->LocalVariables;
            stack_init(&vm->frames[cfi]->LocalVariables);
            if (pc > cs[cs_top].saved_params_cap) {
                int nc = cs[cs_top].saved_params_cap ? cs[cs_top].saved_params_cap : 8;
                while (nc < pc) nc *= 2;
                Var **np = (Var **)realloc(cs[cs_top].saved_params, sizeof(Var *) * (size_t)nc);
                if (!np) vm_debug_panic("[VM] CALL: memoria esaurita per i parametri\n");
                cs[cs_top].saved_params = np;
                cs[cs_top].saved_params_cap = nc;
            }
            for (int k = 0; k < pc; k++) cs[cs_top].saved_params[k] = vm->frames[cfi]->vars[pi[k]];
            char *p = NULL; int ii = 0;
            while ((p = strtok(NULL, " \t")) && ii < pc) {
                if (!char_id_map_exists(&vm->frames[cfi_cur]->VarIndexer, p))
                    { vm_debug_panic("[VM] CALL: '%s' non def\n", p);}
                int src = char_id_map_get(&vm->frames[cfi_cur]->VarIndexer, p);
                if (!vm->frames[cfi_cur]->vars[src])
                    { vm_debug_panic("[VM] CALL: '%s' NULL\n", p);}
                vm->frames[cfi]->vars[pi[ii++]] = vm->frames[cfi_cur]->vars[src];
            }
            if (ii != pc) { 
                vm_debug_panic("ERROR: params mismatch UNCALL '%s'\n", pn); 
            }
            if (is_rec) {
                vm->frames[cfi]->recursion_depth = new_depth;
                /* invert_op_to_line spesso riceve solo il nome base (UNCALL): il frame
                 * template deve riflettere la profondità corrente. Nei worker PAR non
                 * aggiorniamo il template condiviso (altri thread / altre proc). */
                if (!current_thread_args) {
                    uint bfi = char_id_map_get(&FrameIndexer, pn);
                    vm->frames[bfi]->recursion_depth = new_depth;
                }
            }
            VM_FRAME_KEY_BUF(nfname, pn);
            if (is_rec) {
                if (current_thread_args)
                    make_frame_key_par_rec(pn, new_depth, nfname, sizeof(nfname));
                else
                    make_frame_key(pn, new_depth, nfname, sizeof(nfname));
            } else if (reentrant) {
                /* clone mutuo: il body deve girare sul frame clonato (es. is_even@1),
                 * non sul base. */
                make_frame_key(pn, reent_depth, nfname, sizeof(nfname));
            } else {
                if (current_thread_args)
                    make_thread_frame_key(pn, nfname, sizeof(nfname));
                else
                    memcpy(nfname, pn, strlen(pn) + 1);
            }
            vm_buf_set(&fname, &fname_cap, nfname);
            ptr = go_to_line(orig, vm->frames[cfi]->addr + 1);
            if (!ptr) vm_debug_panic("[VM] CALL: indirizzo non trovato!\n");
            continue;
        }
        else if (!strcmp(fw, "UNCALL")) {
            vm_if_mark_call();
            char *pn  = strtok(NULL, " \t");
            VMLOG("[UNCALL] chiamato per '%s'\n", pn ? pn : "NULL");
            uint  cfi = current_thread_args ? clone_frame_for_thread(vm, pn)
                                            : char_id_map_get(&FrameIndexer, pn);
            uint  curi = get_findex(fname);
            int   pc  = vm->frames[cfi]->param_count, *pi = vm->frames[cfi]->param_indices;
            VM_PARAM_SAVE(sv, pc); for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi]->vars[pi[k]];
            Stack slv = vm->frames[cfi]->LocalVariables;
            stack_init(&vm->frames[cfi]->LocalVariables);
            char *p = NULL; int ii = 0;
            while ((p = strtok(NULL, " \t")) && ii < pc) {
                int src = char_id_map_get(&vm->frames[curi]->VarIndexer, p);
                vm->frames[cfi]->vars[pi[ii++]] = vm->frames[curi]->vars[src];
            }
            if (ii != pc) {
                vm_debug_panic("ERROR: params mismatch UNCALL '%s'\n", pn);
            }
            VMLOG("[UNCALL] param linkati: %d, end_addr=%u addr=%u\n",
                    ii, vm->frames[cfi]->end_addr, vm->frames[cfi]->addr);
            VM_FRAME_KEY_BUF(inv_name, pn);
            if (current_thread_args)
                make_thread_frame_key(pn, inv_name, sizeof(inv_name));
            else
                memcpy(inv_name, pn, strlen(pn) + 1);
            /* Restore '\n' su orig prima del recursive scan: invert_op_to_line ->
               collect_ifs/collect_loops scansionano `orig` cercando '\n', con '\0'
               ancora attivo qui la scan si fermerebbe prematuramente. */
            *nl = '\n';
            invert_op_to_line(vm, inv_name, orig, vm->frames[cfi]->end_addr - 1,
                              vm->frames[cfi]->addr + 1, 1);
            VMLOG("[UNCALL] invert_op_to_line completata\n");
            for (int k = 0; k < pc; k++) vm->frames[cfi]->vars[pi[k]] = sv[k];
            stack_restore(&vm->frames[cfi]->LocalVariables, slv);
            VM_PARAM_SAVE_FREE(sv);
            ptr = nl + 1; continue;
        }
        else if (!strcmp(fw, "PAR_START")) {
            *nl = '\n';
            ParBlock pb = scan_par_block(nl + 1);
            exec_par_threads(vm, orig, fname, &pb, 1, 0);
            ptr = pb.after_end ? pb.after_end : nl + 1;
            par_block_free(&pb);
            continue;
        }
        else if (!strcmp(fw, "LOCAL"))   op_local  (vm, fname);
        else if (!strcmp(fw, "DELOCAL")) op_delocal(vm, fname);
        else if (!strcmp(fw, "SHOW"))    op_show   (vm, fname);
        else if (!strcmp(fw, "PUSHEQ"))  op_pusheq (vm, fname);
        else if (!strcmp(fw, "MINEQ"))   op_mineq  (vm, fname);
        else if (!strcmp(fw, "XOREQ"))   op_xoreq  (vm, fname);
        else if (!strcmp(fw, "SWAP"))    op_swap   (vm, fname);
        else if (!strcmp(fw, "PUSH"))  op_push (vm, fname);
        else if (!strcmp(fw, "POP"))   op_pop  (vm, fname);
        else if (!strcmp(fw, "SSEND")) op_ssend(vm, fname);
        else if (!strcmp(fw, "SRECV")) op_srecv(vm, fname);
        else if (!strcmp(fw, "EVAL"))    op_eval   (vm, fname);
        else if (!strcmp(fw, "ASSERT"))  op_assert (vm, fname);
        else if (!strcmp(fw, "JMPF")) {
            *nl = '\n';
            char *np = op_jmpf(vm, fname, orig);
            ptr = np ? np : nl + 1; continue;
        }
        else if (!strcmp(fw, "JMP")) {
            *nl = '\n'; ptr = op_jmp(vm, fname, orig); continue;
        }
        else if (!strcmp(fw, "START") ||
                 !strcmp(fw, "PROC") || !strcmp(fw, "PARAM") || !strcmp(fw, "LABEL") ||
                 !strcmp(fw, "DECL") || !strcmp(fw, "HALT"))  { /* skip */ }
        /* Dopo END_PROC il return_ptr può cadere su THREAD_* o PAR_END: è la fine
         * del branch fisico nel bytecode. thread_entry salta così; qui vm_run_BT
         * annidato (CALL da worker) deve terminare allo stesso modo. */
        else if (strncmp(fw, "THREAD_", 7) == 0 || !strcmp(fw, "PAR_END")) {
            *nl = '\n';
            if (current_thread_args) {
                goto done;
            }
            vm_debug_panic("[VM] op sconosciuta: '%s'\n", fw);
        }
        else { vm_debug_panic("[VM] op sconosciuta: '%s'\n", fw); }

        *nl = '\n'; ptr = nl + 1;
    }

done:
    free(orig);
    for (uint k = 0; k < cs_cap; k++) {
        free(cs[k].caller_frame);
        free(cs[k].saved_params);
    }
    free(cs);
    free(fname);
}

/* ======================================================================
 *  vm_exec — prima passata (raccolta frame/dichiarazioni)
 * ====================================================================== */

void vm_exec(VM *vm, char *buffer)
{
    char *orig = strdup(buffer);
    char *ptr  = buffer;
    int   line = 1;
    /* Nomi che il testo della procedura corrente può introdurre (DECL, PARAM,
     * LOCAL, DELOCAL) e LOCAL: a END_PROC dimensionano vars[] e lo stack dei
     * local del frame (names_hint/locals_hint), così che in esecuzione non
     * debbano crescere. Sono stime per eccesso, non limiti. */
    int   proc_names = 0, proc_locals = 0;

    while (*ptr) {
        char *nl = strchr(ptr, '\n');
        if (!nl) break;
        *nl = '\0';

        if (*skip_lineno(ptr)) {
            char *_skipped = skip_lineno(ptr);
            char *fw = strtok(_skipped, " \t");

            if (!strcmp(fw, "DECL") || !strcmp(fw, "PARAM") || !strcmp(fw, "DELOCAL"))
                proc_names++;
            else if (!strcmp(fw, "LOCAL")) {
                proc_names++;
                proc_locals++;
            }

            if (!strcmp(fw, "START")) {
                char_id_map_init(&FrameIndexer);
                vm->frame_top = -1;

            } else if (!strcmp(fw, "PROC")) {
                char *name = strtok(NULL, " \t");
                uint  idx  = char_id_map_get(&FrameIndexer, name);
                vm_ensure_frame_cap(vm, idx);
                vm->frame_top = idx;
                char_id_map_init(&vm->frames[idx]->VarIndexer);
                stack_init(&vm->frames[idx]->LocalVariables);
                vm_str_replace(&vm->frames[idx]->name, name);
                vm->frames[idx]->addr = line;
                proc_names = proc_locals = 0;
                VMLOG("[EXEC] PROC '%s' addr=%u\n", name, line);

            } else if (!strcmp(fw, "END_PROC")) {
                char *name = strtok(NULL, " \t");
                vm->frames[vm->frame_top]->end_addr = line;
                {
                    Frame *pf = vm->frames[vm->frame_top];
                    pf->names_hint  = proc_names + FRAME_VARS_INIT_CAP;
                    pf->locals_hint = proc_locals;
                    frame_ensure_vars(pf, pf->names_hint - 1);
                }
                VMLOG("[EXEC] END_PROC '%s' addr=%u end_addr=%u\n",
                    name,
                    vm->frames[vm->frame_top]->addr,
                    vm->frames[vm->frame_top]->end_addr);
                if (!strcmp(name, "main"))
                    vm_run_BT(vm, orig, "main");

            } else if (!strcmp(fw, "DECL")) {
                char *type = strtok(NULL, " \t"), *vn = strtok(NULL, " \t");
                int   vi   = char_id_map_get(&vm->frames[vm->frame_top]->VarIndexer, vn);
                frame_ensure_vars(vm->frames[vm->frame_top], vi);
                if (vm->frames[vm->frame_top]->vars[vi]) vm_debug_panic("[VM] Variabile già definita!\n");
                vm->frames[vm->frame_top]->vars[vi] = malloc(sizeof(Var));
                alloc_var(vm->frames[vm->frame_top]->vars[vi], type, vn);
                vm->frames[vm->frame_top]->vars[vi]->is_local = 0;
                if (vi >= vm->frames[vm->frame_top]->var_count)
                    vm->frames[vm->frame_top]->var_count = vi + 1;

            } else if (!strcmp(fw, "PARAM")) {
                char *vtype = strtok(NULL, " \t"), *vn = strtok(NULL, " \t");
                int   vi    = char_id_map_get(&vm->frames[vm->frame_top]->VarIndexer, vn);
                frame_ensure_vars(vm->frames[vm->frame_top], vi);
                if (vm->frames[vm->frame_top]->vars[vi]) vm_debug_panic("[VM] PARAM già definito!\n");
                vm->frames[vm->frame_top]->vars[vi]          = calloc(1, sizeof(Var));
                vm->frames[vm->frame_top]->vars[vi]->T        = TYPE_PARAM;
                vm->frames[vm->frame_top]->vars[vi]->is_local = 0;
                vm->frames[vm->frame_top]->vars[vi]->name     = char_id_strdup(vn);
                (void)vtype;
                if (vi >= vm->frames[vm->frame_top]->var_count)
                    vm->frames[vm->frame_top]->var_count = vi + 1;
                frame_ensure_params(vm->frames[vm->frame_top], vm->frames[vm->frame_top]->param_count);
                vm->frames[vm->frame_top]->param_indices[vm->frames[vm->frame_top]->param_count++] = vi;

            } else if (!strcmp(fw, "LABEL")) {
                char *ln = strtok(NULL, " \t");
                uint  li = char_id_map_get(&vm->frames[vm->frame_top]->LabelIndexer, ln);
                frame_ensure_labels(vm->frames[vm->frame_top], (int)li);
                vm->frames[vm->frame_top]->label[li] = line;

            } else if (!strcmp(fw, "HALT")) { /* nop */
            }
        }
        *nl = '\n'; ptr = nl + 1; line++;
    }
    free(orig);
}

/* ======================================================================
 *  vm_free — libera tutte le variabili allocate da vm_exec
 * ====================================================================== */

static int vm_ptr_cmp(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)*(Var *const *)a, y = (uintptr_t)*(Var *const *)b;
    return (x > y) - (x < y);
}

static void vm_free_var(Var *to_free)
{
    if (to_free->T == TYPE_CHANNEL && to_free->channel) {
        pthread_mutex_lock(&to_free->channel->mtx);
        to_free->channel->refcount--;
        int do_free = (to_free->channel->refcount <= 0);
        pthread_mutex_unlock(&to_free->channel->mtx);
        if (do_free) {
            pthread_mutex_destroy(&to_free->channel->mtx);
            if (to_free->channel->buf) free(to_free->channel->buf);
            free(to_free->channel);
        }
    } else if (to_free->value) {
        free(to_free->value);
    }
    free(to_free->name);
    free(to_free);
}

void vm_free(VM *vm)
{
    if (!vm) return;
    /* Evita double-free quando piu` slot puntano alla stessa Var (es. alias
       parametri durante call/uncall in debug rebuild): si raccolgono tutti i
       puntatori, si ordinano e si libera ciascuno una volta sola. Il vettore è
       della misura delle variabili davvero presenti. */
    size_t nptr = 0;
    for (int i = 0; vm->frames && i <= vm->frame_top; i++)
        if (vm->frames[i]) nptr += (size_t)vm->frames[i]->var_count;
    Var **all = (Var **)malloc(sizeof(Var *) * (nptr ? nptr : 1));
    if (!all) return;
    size_t n = 0;
    for (int i = 0; vm->frames && i <= vm->frame_top; i++) {
        Frame *f = vm->frames[i];
        if (!f) continue;
        for (int j = 0; j < f->var_count; j++) {
            if (f->vars[j]) all[n++] = f->vars[j];
            f->vars[j] = NULL;
        }
        f->var_count = 0;
    }
    qsort(all, n, sizeof(Var *), vm_ptr_cmp);
    for (size_t k = 0; k < n; k++)
        if (k == 0 || all[k] != all[k - 1]) vm_free_var(all[k]);
    free(all);
    if (vm->frames) {
        for (uint i = 0; i < vm->frames_cap; i++) {
            if (vm->frames[i]) {
                /* buffer heap dinamici per-Frame */
                vm_grow_free(vm->frames[i]->vars);
                free(vm->frames[i]->label);
                free(vm->frames[i]->param_indices);
                free(vm->frames[i]->name);
                stack_release(&vm->frames[i]->LocalVariables);
                char_id_map_destroy(&vm->frames[i]->VarIndexer);
                char_id_map_destroy(&vm->frames[i]->LabelIndexer);
                free(vm->frames[i]);
                vm->frames[i] = NULL;
            }
        }
        vm_grow_free(vm->frames);
        vm->frames = NULL;
        vm->frames_cap = 0;
    }
}

/* ======================================================================
 *  vm_dump
 * ====================================================================== */

/* Dump dei var di un singolo frame (senza header). */
static void vm_dump_frame(Frame *f)
{
    if (!f) return;
    for (int j = 0; j < f->var_count; j++) {
        Var *v = f->vars[j]; if (!v) continue;
        vm_printf("%s: ", v->name);
        if (v->T == TYPE_INT) {
            vm_printf("%lld", (long long)*(v->value));
        } else {
            vm_printf("[");
            int seq = (v->T == TYPE_STACK || v->T == TYPE_ARRAY);
            size_t n = seq ? v->stack_len : v->channel->buf_len;
            int64_t *arr = seq ? v->value : v->channel->buf;
            for (size_t k = 0; k < n; k++) {
                vm_printf("%lld", (long long)arr[k]);
                if (k + 1 < n) vm_printf(", ");
            }
            vm_printf("]");
        }
        vm_printf("\n");
    }
}

void vm_dump(VM *vm)
{
    vm_printf("=== VM dump ===\n");
    for (int i = 0; i <= vm->frame_top; i++) {
        Frame *f = vm->frames[i];
        if (strcmp(f->name, "main") != 0) continue;
        vm_dump_frame(f);
    }
}

/* --vm-stats: post-execution stats su tutti gli int cell rimasti.
 * Mean abs e max abs value of int cells (across all frames).
 * Anche stack cells inclusi (ogni elemento conta).
 * Trigger via env KAIROS_VM_STATS=1 (no symbol export). */
static int g_vm_stats_enabled = 0;

/* Live cell count tracking. Aggiornato dal dispatch loop via vm_stats_sample().
 * Conta tutti gli int var + total stack elements vivi a quel tick. */
static uint64_t g_vm_cells_sample_sum = 0;
static uint64_t g_vm_cells_sample_count = 0;
static uint64_t g_vm_cells_sample_max = 0;
static uint64_t g_vm_tick_counter = 0;
#define VM_STATS_SAMPLE_EVERY 256

static uint64_t vm_count_live_cells(VM *vm)
{
    uint64_t count = 0;
    for (int i = 0; i <= vm->frame_top; i++) {
        Frame *f = vm->frames[i];
        for (int j = 0; j < f->var_count; j++) {
            Var *v = f->vars[j]; if (!v) continue;
            if (v->T == TYPE_INT) count++;
            else if (v->T == TYPE_STACK || v->T == TYPE_ARRAY) count += (uint64_t)v->stack_len;
        }
    }
    return count;
}

void vm_stats_sample(VM *vm)
{
    if (!g_vm_stats_enabled) return;
    g_vm_tick_counter++;
    if ((g_vm_tick_counter & (VM_STATS_SAMPLE_EVERY - 1)) != 0) return;
    uint64_t c = vm_count_live_cells(vm);
    g_vm_cells_sample_sum += c;
    g_vm_cells_sample_count++;
    if (c > g_vm_cells_sample_max) g_vm_cells_sample_max = c;
}

void vm_print_stats(VM *vm)
{
    if (!g_vm_stats_enabled) return;
    uint64_t final_cells = vm_count_live_cells(vm);
    /* Sample finale per garantire copertura anche su run brevissimi. */
    if (final_cells > g_vm_cells_sample_max) g_vm_cells_sample_max = final_cells;
    if (g_vm_cells_sample_count == 0) {
        g_vm_cells_sample_sum += final_cells;
        g_vm_cells_sample_count++;
    }
    double mean = g_vm_cells_sample_count
        ? (double)g_vm_cells_sample_sum / (double)g_vm_cells_sample_count
        : 0.0;
    vm_printf("=== VM stats ===\n");
    vm_printf("cells_final: %llu\n", (unsigned long long)final_cells);
    vm_printf("cells_mean:  %.2f\n", mean);
    vm_printf("cells_max:   %llu\n", (unsigned long long)g_vm_cells_sample_max);
}

/* ======================================================================
 *  Entry point — esecuzione normale (invariato)
 * ====================================================================== */

static void vm_run_from_string_impl(const char *bytecode, int dump_after)
{
    const char *st = getenv("KAIROS_VM_STATS");
    if (st && (st[0] == '1' || st[0] == 'y' || st[0] == 'Y' || st[0] == 't' || st[0] == 'T'))
        g_vm_stats_enabled = 1;

    char *ast = normalize_bytecode_physical_lines(bytecode);
    if (!ast) {
        fprintf(stderr, "Errore: normalizzazione bytecode fallita.\n");
        return;
    }

    VM *vm = calloc(1, sizeof(VM));
    if (!vm) { fprintf(stderr, "VM alloc failed\n"); free(ast); return; }
    vm->dbg = NULL;
    /* frames: lo crea vm_ensure_frame_cap al primo PROC e cresce on-demand. */
    vm_exec(vm, ast);
    if (dump_after)
        vm_dump(vm);
    vm_print_stats(vm);
    vm_free(vm);
    free(ast);
    free(vm);
    vm_if_free_branch_stack();    /* vettori __thread del thread chiamante */
    vm_invert_free_cache();
}

void vm_run_from_string(const char *bytecode)
{
    vm_run_from_string_impl(bytecode, 1);
}

/* Esecuzione senza dump finale. */
void vm_run_from_string_quiet(const char *bytecode)
{
    vm_run_from_string_impl(bytecode, 0);
}