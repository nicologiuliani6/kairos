#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>    /* pipe, close */

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
#include "check_if_reversibility.h"


/* Puntatore alla VM corrente — usato da vm_printf in DAP_MODE */
VM *g_current_vm = NULL;
/* ── thread-local state (dichiarate extern in vm_types.h) ── */
__thread ThreadArgs *current_thread_args = NULL;
__thread char       *strtok_saveptr      = NULL;
__thread uint        thread_val_IF       = 0;

pthread_mutex_t var_indexer_mtx = PTHREAD_MUTEX_INITIALIZER;
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
    /* Estrae il numero fisico di riga (i primi 4 digit) */
    char tmp[8] = {0};
    int  i;
    for (i = 0; i < 4 && raw_line[i] >= '0' && raw_line[i] <= '9'; i++)
        tmp[i] = raw_line[i];
    return atoi(tmp);
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

void vm_run_BT(VM *vm, char *buffer, char *frame_name_init)
{
    g_current_vm = vm;
    char *orig = strdup(buffer);
    char  fname[VAR_NAME_LENGTH];
    strncpy(fname, frame_name_init, VAR_NAME_LENGTH - 1);
    fname[VAR_NAME_LENGTH - 1] = '\0';

    typedef struct {
        char *return_ptr;
        char  caller_frame[VAR_NAME_LENGTH];
        Var  *saved_params[64];
        int   saved_param_count, callee_findex;
        Stack saved_local_vars;
        int   is_recursive_clone;
    } CallRecord;

    CallRecord cs[MAX_FRAMES]; int cs_top = -1;
    uint  si  = char_id_map_get(&FrameIndexer, fname);
    char *ptr = go_to_line(orig, vm->frames[si].addr + 1);
    if (!ptr) { fprintf(stderr, "ERROR: '%s' non trovato\n", fname); free(orig); return; }

    while (*ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[512]; strncpy(lb, ptr, sizeof(lb) - 1);
        lb[sizeof(lb)-1] = '\0';
        char *fw = strtok(skip_lineno(lb), " \t");

        if (!fw) { *nl = '\n'; ptr = nl + 1; continue; }

        /* ── DEBUG HOOK ── chiamato prima di ogni istruzione reale ── */
        if (strcmp(fw, "PROC") != 0 && strcmp(fw, "PARAM") != 0 &&
            strcmp(fw, "HALT") != 0) {
            if (strcmp(fw, "DECL") == 0 || strcmp(fw, "LABEL") == 0) {
                if (vm->dbg && vm->dbg->initialized)
                    vm->dbg->current_line = extract_srcline(ptr);
            } else {
                DEBUG_HOOK(ptr, lb);
            }
        }

        if (!strcmp(fw, "END_PROC")) {
            uint fi = get_findex(fname);
            if (stack_size(&vm->frames[fi].LocalVariables) > -1)
                vm_debug_panic("[VM] END_PROC: variabili LOCAL non chiuse!\n");
            *nl = '\n';
            if (cs_top >= 0) {
                int cfi = cs[cs_top].callee_findex;
                for (int k = 0; k < cs[cs_top].saved_param_count; k++)
                    vm->frames[cfi].vars[vm->frames[cfi].param_indices[k]] = cs[cs_top].saved_params[k];
                vm->frames[cfi].LocalVariables = cs[cs_top].saved_local_vars;
                if (cs[cs_top].is_recursive_clone)
                    for (int k = 0; k < vm->frames[cfi].param_count; k++) {
                        int pidx = vm->frames[cfi].param_indices[k];
                        free(vm->frames[cfi].vars[pidx]);
                        vm->frames[cfi].vars[pidx] = NULL;
                    }
                ptr = cs[cs_top].return_ptr;
                strncpy(fname, cs[cs_top].caller_frame, VAR_NAME_LENGTH - 1);
                cs_top--;
            } else break;
            continue;
        }
        else if (!strcmp(fw, "CALL")) {
            char *pn      = strtok(NULL, " \t");
            uint  cfi_cur = get_findex(fname);
            char  base[VAR_NAME_LENGTH]; strncpy(base, fname, VAR_NAME_LENGTH - 1);
            char *at = strchr(base, '@'); if (at) *at = '\0';
            int   is_rec    = !strcmp(pn, base);
            int   new_depth = 0;
            if (is_rec) {
                char *at2 = strchr(fname, '@');
                int   cd  = at2 ? atoi(at2 + 1) : 0;
                new_depth = cd + 1;
            }
            uint cfi = is_rec ? clone_frame_for_depth(vm, pn, new_depth)
                              : char_id_map_get(&FrameIndexer, pn);
            if (cs_top + 1 >= MAX_FRAMES) vm_debug_panic("[VM] CALL: stack overflow!\n");
            cs_top++;
            *nl = '\n';
            cs[cs_top].return_ptr         = nl + 1;
            cs[cs_top].is_recursive_clone = is_rec;
            cs[cs_top].callee_findex      = cfi;
            strncpy(cs[cs_top].caller_frame, fname, VAR_NAME_LENGTH - 1);
            int  pc = vm->frames[cfi].param_count, *pi = vm->frames[cfi].param_indices;
            cs[cs_top].saved_param_count = pc;
            cs[cs_top].saved_local_vars  = vm->frames[cfi].LocalVariables;
            stack_init(&vm->frames[cfi].LocalVariables);
            for (int k = 0; k < pc; k++) cs[cs_top].saved_params[k] = vm->frames[cfi].vars[pi[k]];
            char *p = NULL; int ii = 0;
            while ((p = strtok(NULL, " \t")) && ii < pc) {
                if (!char_id_map_exists(&vm->frames[cfi_cur].VarIndexer, p))
                    { vm_debug_panic("[VM] CALL: '%s' non def\n", p);}
                int src = char_id_map_get(&vm->frames[cfi_cur].VarIndexer, p);
                if (!vm->frames[cfi_cur].vars[src])
                    { vm_debug_panic("[VM] CALL: '%s' NULL\n", p);}
                vm->frames[cfi].vars[pi[ii++]] = vm->frames[cfi_cur].vars[src];
            }
            if (ii != pc) { 
                vm_debug_panic("ERROR: params mismatch UNCALL '%s'\n", pn); 
            }
            if (is_rec) {
                uint bfi = char_id_map_get(&FrameIndexer, pn);
                vm->frames[bfi].recursion_depth = new_depth;
            }
            char nfname[VAR_NAME_LENGTH];
            if (is_rec) make_frame_key(pn, new_depth, nfname, sizeof(nfname));
            else        strncpy(nfname, pn, VAR_NAME_LENGTH - 1);
            strncpy(fname, nfname, VAR_NAME_LENGTH - 1);
            ptr = go_to_line(orig, vm->frames[cfi].addr + 1);
            if (!ptr) vm_debug_panic("[VM] CALL: indirizzo non trovato!\n");
            continue;
        }
        else if (!strcmp(fw, "UNCALL")) {
            char *pn  = strtok(NULL, " \t");
            VMLOG("[UNCALL] chiamato per '%s'\n", pn ? pn : "NULL");
            uint  cfi = char_id_map_get(&FrameIndexer, pn);
            uint  curi = get_findex(fname);
            int   pc  = vm->frames[cfi].param_count, *pi = vm->frames[cfi].param_indices;
            Var  *sv[64]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi].vars[pi[k]];
            char *p = NULL; int ii = 0;
            while ((p = strtok(NULL, " \t")) && ii < pc) {
                int src = char_id_map_get(&vm->frames[curi].VarIndexer, p);
                vm->frames[cfi].vars[pi[ii++]] = vm->frames[curi].vars[src];
            }
            VMLOG("[UNCALL] param linkati: %d, end_addr=%u addr=%u\n",
                    ii, vm->frames[cfi].end_addr, vm->frames[cfi].addr);
            invert_op_to_line(vm, pn, orig, vm->frames[cfi].end_addr - 1, vm->frames[cfi].addr + 1);
            VMLOG("[UNCALL] invert_op_to_line completata\n");
            for (int k = 0; k < pc; k++) vm->frames[cfi].vars[pi[k]] = sv[k];
            *nl = '\n'; ptr = nl + 1; continue;
        }
        else if (!strcmp(fw, "PAR_START")) {
            *nl = '\n';
            ParBlock pb = scan_par_block(nl + 1);
            exec_par_threads(vm, orig, fname, &pb, 1, 0);
            ptr = pb.after_end ? pb.after_end : nl + 1;
            continue;
        }
        else if (!strcmp(fw, "LOCAL"))   op_local  (vm, fname);
        else if (!strcmp(fw, "DELOCAL")) op_delocal(vm, fname);
        else if (!strcmp(fw, "SHOW"))    op_show   (vm, fname);
        else if (!strcmp(fw, "PUSHEQ"))  op_pusheq (vm, fname);
        else if (!strcmp(fw, "MINEQ"))   op_mineq  (vm, fname);
        else if (!strcmp(fw, "XOREQ"))   op_xoreq  (vm, fname);
        else if (!strcmp(fw, "SWAP"))    op_swap   (vm, fname);
        else if (!strcmp(fw, "PUSH") || !strcmp(fw, "SSEND")) op_push(vm, fname);
        else if (!strcmp(fw, "POP")  || !strcmp(fw, "SRECV")) op_pop (vm, fname);
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
        else if (!strcmp(fw, "PROC") || !strcmp(fw, "PARAM") || !strcmp(fw, "LABEL") ||
                 !strcmp(fw, "DECL") || !strcmp(fw, "HALT"))  { /* skip */ }
        else { vm_debug_panic("[VM] op sconosciuta: '%s'\n", fw); }

        *nl = '\n'; ptr = nl + 1;
    }

done:
    free(orig);
}

/* ======================================================================
 *  vm_exec — prima passata (raccolta frame/dichiarazioni)
 * ====================================================================== */

void vm_exec(VM *vm, char *buffer)
{
    char *orig = strdup(buffer);
    char *ptr  = buffer;
    int   line = 1;

    while (*ptr) {
        char *nl = strchr(ptr, '\n');
        if (!nl) break;
        *nl = '\0';

        if (*skip_lineno(ptr)) {
            char *_skipped = skip_lineno(ptr);
            char *fw = strtok(_skipped, " \t");

            if (!strcmp(fw, "START")) {
                char_id_map_init(&FrameIndexer);
                vm->frame_top = -1;

            } else if (!strcmp(fw, "PROC")) {
                char *name = strtok(NULL, " \t");
                uint  idx  = char_id_map_get(&FrameIndexer, name);
                vm->frame_top = idx;
                char_id_map_init(&vm->frames[idx].VarIndexer);
                stack_init(&vm->frames[idx].LocalVariables);
                strncpy(vm->frames[idx].name, name, VAR_NAME_LENGTH - 1);
                vm->frames[idx].addr = line;
                VMLOG("[EXEC] PROC '%s' addr=%u\n", name, line);

            } else if (!strcmp(fw, "END_PROC")) {
                char *name = strtok(NULL, " \t");
                vm->frames[vm->frame_top].end_addr = line;
                VMLOG("[EXEC] END_PROC '%s' addr=%u end_addr=%u\n",
                    name,
                    vm->frames[vm->frame_top].addr,
                    vm->frames[vm->frame_top].end_addr);
                if (!strcmp(name, "main"))
                    vm_run_BT(vm, orig, "main");

            } else if (!strcmp(fw, "DECL")) {
                char *type = strtok(NULL, " \t"), *vn = strtok(NULL, " \t");
                int   vi   = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, vn);
                if (vm->frames[vm->frame_top].vars[vi]) vm_debug_panic("[VM] Variabile già definita!\n");
                vm->frames[vm->frame_top].vars[vi] = malloc(sizeof(Var));
                alloc_var(vm->frames[vm->frame_top].vars[vi], type, vn);
                vm->frames[vm->frame_top].vars[vi]->is_local = 0;
                if (vi >= vm->frames[vm->frame_top].var_count)
                    vm->frames[vm->frame_top].var_count = vi + 1;

            } else if (!strcmp(fw, "PARAM")) {
                char *vtype = strtok(NULL, " \t"), *vn = strtok(NULL, " \t");
                int   vi    = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, vn);
                if (vm->frames[vm->frame_top].vars[vi]) vm_debug_panic("[VM] PARAM già definito!\n");
                vm->frames[vm->frame_top].vars[vi]          = calloc(1, sizeof(Var));
                vm->frames[vm->frame_top].vars[vi]->T        = TYPE_PARAM;
                vm->frames[vm->frame_top].vars[vi]->is_local = 0;
                strncpy(vm->frames[vm->frame_top].vars[vi]->name, vn, VAR_NAME_LENGTH - 1);
                (void)vtype;
                if (vi >= vm->frames[vm->frame_top].var_count)
                    vm->frames[vm->frame_top].var_count = vi + 1;
                vm->frames[vm->frame_top].param_indices[vm->frames[vm->frame_top].param_count++] = vi;

            } else if (!strcmp(fw, "LABEL")) {
                char *ln = strtok(NULL, " \t");
                uint  li = char_id_map_get(&vm->frames[vm->frame_top].LabelIndexer, ln);
                vm->frames[vm->frame_top].label[li] = line;

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

static void vm_free(VM *vm)
{
    if (!vm) return;
    for (int i = 0; i <= vm->frame_top; i++) {
        Frame *f = &vm->frames[i];
        for (int j = 0; j < f->var_count; j++) {
            if (f->vars[j]) {
                /* Il campo value è allocato da alloc_var (calloc in vm_helpers.h) */
                if (f->vars[j]->value)
                    free(f->vars[j]->value);
                free(f->vars[j]);
                f->vars[j] = NULL;
            }
        }
        f->var_count = 0;
    }
}

/* ======================================================================
 *  vm_dump
 * ====================================================================== */

void vm_dump(VM *vm)
{
    printf("=== VM dump ===\n");
    for (int i = 0; i <= vm->frame_top; i++) {
        Frame *f = &vm->frames[i];
        if (strcmp(f->name, "main") != 0) continue;
        for (int j = 0; j < f->var_count; j++) {
            Var *v = f->vars[j]; if (!v) continue;
            printf("%s: ", v->name);
            if (v->T == TYPE_INT) {
                printf("%d", *(v->value));
            } else {
                printf("[");
                for (size_t k = 0; k < v->stack_len; k++) {
                    printf("%d", v->value[k]);
                    if (k + 1 < v->stack_len) printf(", ");
                }
                printf("]");
            }
            printf("\n");
        }
    }
}

/* ======================================================================
 *  Entry point — esecuzione normale (invariato)
 * ====================================================================== */

#define AST_BUFFER (1024 * 10)

void vm_run_from_string(const char *bytecode)
{
    char ast[AST_BUFFER];
    ast[0] = '\0';
    strncat(ast, bytecode, sizeof(ast) - 1);

    if (vm_check_if_reversibility(ast) > 0)
        fprintf(stderr, "Warning: il bytecode potrebbe non essere completamente reversibile.\n");

    VM vm; memset(&vm, 0, sizeof(VM));
    vm.dbg = NULL;   /* modalità normale: nessun debugger */
    vm_exec(&vm, ast);
    vm_free(&vm);
}

/* ======================================================================
 *  API di debug pubblica
 *
 *  Struttura di utilizzo dal DAP adapter (Node.js via ffi-napi):
 *
 *    VMDebugState *dbg = vm_debug_new();
 *    vm_debug_set_breakpoint(dbg, 12);
 *    vm_debug_set_breakpoint(dbg, 25);
 *    dbg->on_pause = my_callback;
 *    dbg->userdata = &my_context;
 *    vm_debug_start(bytecode, dbg);   // avvia in un thread separato
 *    // ... nel thread principale:
 *    vm_debug_step(dbg);
 *    vm_debug_continue(dbg);
 *    char buf[65536]; vm_debug_dump_json_ext(dbg, buf, sizeof(buf));
 *    vm_debug_free(dbg);
 * ====================================================================== */

/* Stato globale del debugger (usato dall'API esterna).
   In un processo normale ci sarà una sola sessione di debug alla volta. */
static VM            *g_debug_vm  = NULL;
VMDebugState  *g_debug_dbg = NULL;
static char          *g_debug_buf = NULL;   /* copia del bytecode */
static char          *g_debug_buf_orig = NULL;   
static pthread_t      g_debug_tid;

/* Thread che esegue vm_exec (e quindi vm_run_BT) in background */
static void *debug_exec_thread(void *arg)
{
    FILE *f = fopen("/tmp/janus-vm.log", "a");
    if (f) { fprintf(f, "debug_exec_thread AVVIATO\n"); fclose(f); }

    (void)arg;
    vm_exec(g_debug_vm, g_debug_buf);

    if (g_debug_dbg) {
        pthread_mutex_lock(&g_debug_dbg->pause_mtx);
        g_debug_dbg->mode = VM_MODE_DONE;
        pthread_cond_broadcast(&g_debug_dbg->pause_cond);
        pthread_mutex_unlock(&g_debug_dbg->pause_mtx);

        /* Aspetta che il controllore abbia preso atto del DONE
           prima di tornare — evita che la .so venga scaricata
           mentre vm_debug_continue sta ancora girando */
        pthread_mutex_lock(&g_debug_dbg->pause_mtx);
        while (g_debug_dbg->mode == VM_MODE_DONE)
            pthread_cond_wait(&g_debug_dbg->pause_cond, &g_debug_dbg->pause_mtx);
        pthread_mutex_unlock(&g_debug_dbg->pause_mtx);

        if (g_debug_dbg->on_pause)
            g_debug_dbg->on_pause(-1, "done", g_debug_dbg->userdata);
    }
    return NULL;
}

/* Alloca e inizializza uno stato di debug */
VMDebugState *vm_debug_new(void)
{
    VMDebugState *dbg = calloc(1, sizeof(VMDebugState));
    dbg_init(dbg);
    return dbg;
}

/* Libera le risorse */
void vm_debug_free(VMDebugState *dbg)
{
    if (!dbg) return;
    dbg_destroy(dbg);
    free(dbg);
}

/*
 * vm_debug_start — avvia l'esecuzione in modalità debug in un thread
 *                  separato. La VM si fermerà subito alla prima istruzione
 *                  (mode = VM_MODE_STEP all'avvio) oppure al primo
 *                  breakpoint se mode = VM_MODE_CONTINUE.
 */
void vm_debug_start(const char *bytecode, VMDebugState *dbg)
{
    /* Cleanup eventuale sessione precedente */
    if (g_debug_vm) {
        if (g_debug_dbg) dbg_resume(g_debug_dbg, VM_MODE_DONE);
        pthread_join(g_debug_tid, NULL);
        free(g_debug_buf);
        vm_free(g_debug_vm);
        free(g_debug_vm);
        g_debug_vm  = NULL;
        g_debug_buf = NULL;
    }

    g_debug_dbg = dbg;
    g_debug_vm  = calloc(1, sizeof(VM));
    g_debug_vm->dbg = dbg;
     /* Crea la pipe per lo streaming dell'output */
    int fds[2];
    if (pipe(fds) == 0) {
        dbg->output_pipe_rd = fds[0];
        dbg->output_pipe_fd = fds[1];
    } else {
        dbg->output_pipe_rd = -1;
        dbg->output_pipe_fd = -1;
    }

    /* Copia del bytecode */
    size_t blen = strlen(bytecode) + 1;
    g_debug_buf = malloc(blen);
    memcpy(g_debug_buf, bytecode, blen);
    g_debug_buf_orig = malloc(blen);
    memcpy(g_debug_buf_orig, bytecode, blen);

    /* Verifica reversibilità (non bloccante, solo warning) */
    if (vm_check_if_reversibility(g_debug_buf) > 0)
        fprintf(stderr, "Warning: il bytecode potrebbe non essere completamente reversibile.\n");

    /* Di default parte in modalità STEP: si ferma alla prima istruzione */
    if (dbg->mode == VM_MODE_RUN)
        dbg->mode = VM_MODE_STEP;

    pthread_create(&g_debug_tid, NULL, debug_exec_thread, NULL);
        /* Aspetta che la VM raggiunga la prima PAUSE (o termini) */
    pthread_mutex_lock(&dbg->pause_mtx);
    while (!dbg->first_pause_reached && dbg->mode != VM_MODE_DONE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);
    pthread_mutex_unlock(&dbg->pause_mtx);

}

/*
 * vm_debug_step — avanza di una istruzione poi si ferma.
 * Ritorna il numero di riga corrente dopo lo step, -1 se terminato.
 */
int vm_debug_step(VMDebugState *dbg)
{
    if (!dbg || dbg->mode == VM_MODE_DONE) return -1;
    pthread_mutex_lock(&dbg->pause_mtx);
    dbg->mode = VM_MODE_STEP;
    pthread_cond_broadcast(&dbg->pause_cond);

    while (dbg->mode != VM_MODE_PAUSE && dbg->mode != VM_MODE_DONE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);
    pthread_mutex_unlock(&dbg->pause_mtx);

    return (dbg->mode == VM_MODE_DONE) ? -1 : dbg->current_line;
}

/*
 * vm_debug_step_back — inverte l'ultima istruzione eseguita.
 *
 * Grazie alla reversibilità di Janus possiamo re-eseguire all'indietro
 * usando invert_op_to_line sul record in cima alla history.
 * Per ora: segnala STEP_BACK alla VM e riprende; la VM esegue
 * l'inversione e torna in PAUSE.
 *
 * NOTA: l'inversione vera di singola istruzione richiede che vm_run_BT
 * riconosca VM_MODE_STEP_BACK e chiami il codice di inversione.
 * Questa è la base su cui costruire — l'implementazione completa
 * dipende dal fatto che non tutte le istruzioni hanno un inverso
 * semplice (CALL→UNCALL, PUSHEQ→MINEQ, ecc.). Vedere vm_invert.h.
 */
int vm_debug_step_back(VMDebugState *dbg)
{
    if (!dbg || dbg->history_top < 0) return -1;
    
    /* Pop del record corrente (la riga su cui siamo fermi) */
    ExecRecord *rec = dbg_pop_history(dbg);
    if (!rec) return -1;
    //fprintf(stderr, "[STEP_BACK] invertendo riga %d: '%s'\n", rec->line, rec->instr);
    /* Esegui l'inversione della riga che abbiamo appena lasciato */
    if (g_debug_vm) {
        invert_op_to_line(g_debug_vm, rec->frame, g_debug_buf_orig,
                  rec->line, rec->line + 1);
    }

    /* Aggiorna posizione al record precedente */
    int prev_line = (dbg->history_top >= 0) ? dbg->history[dbg->history_top].line : 0;
    dbg->current_line = prev_line;
    if (dbg->history_top >= 0)
        strncpy(dbg->current_frame, dbg->history[dbg->history_top].frame, VAR_NAME_LENGTH - 1);

    if (dbg->on_pause)
        dbg->on_pause(prev_line, dbg->current_frame, dbg->userdata);
    return prev_line;
}

/*
 * vm_debug_continue — riprendi fino al prossimo breakpoint (o fine).
 * Ritorna la riga del breakpoint raggiunto, -1 se terminato.
 */
int vm_debug_continue(VMDebugState *dbg)
{
    if (!dbg || dbg->mode == VM_MODE_DONE) return -1;
    pthread_mutex_lock(&dbg->pause_mtx);
    dbg->mode = VM_MODE_CONTINUE;
    pthread_cond_broadcast(&dbg->pause_cond);

    while (dbg->mode != VM_MODE_PAUSE && dbg->mode != VM_MODE_DONE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);

    int result = (dbg->mode == VM_MODE_DONE) ? -1 : dbg->current_line;

    /* Segnala al thread VM che abbiamo ricevuto il DONE
       così può uscire da debug_exec_thread in modo pulito */
    if (dbg->mode == VM_MODE_DONE) {
        dbg->mode = VM_MODE_IDLE;   // qualsiasi mode != DONE va bene
        pthread_cond_broadcast(&dbg->pause_cond);
    }

    pthread_mutex_unlock(&dbg->pause_mtx);
    return result;
}
/*
 * vm_debug_continue_inverse — continua all'indietro fino al prossimo
 * breakpoint. Utilizza invert_op_to_line iterativamente sulla history.
 */
int vm_debug_continue_inverse(VMDebugState *dbg)
{
    if (!dbg || dbg->history_top < 0) return -1;
    while (dbg->history_top >= 0) {
        ExecRecord *rec = &dbg->history[dbg->history_top];
        if (dbg_is_breakpoint(dbg, rec->line)) {
            dbg->current_line = rec->line;
            strncpy(dbg->current_frame, rec->frame, VAR_NAME_LENGTH - 1);
            dbg->history_top--;
            if (dbg->on_pause)
                dbg->on_pause(rec->line, rec->frame, dbg->userdata);
            return rec->line;
        }
        dbg->history_top--;
    }
    /* Arrivati all'inizio della history */
    if (dbg->on_pause)
        dbg->on_pause(0, "start", dbg->userdata);
    return 0;
}

/*
 * vm_debug_goto_line — continua fino a una riga specifica.
 * Equivalente a mettere un breakpoint temporaneo su quella riga
 * e chiamare continue, poi rimuoverlo.
 */
int vm_debug_goto_line(VMDebugState *dbg, int target_line)
{
    if (!dbg) return -1;
    _vm_debug_set_breakpoint(dbg, target_line);      // ← underscore
    int result = vm_debug_continue(dbg);
    _vm_debug_clear_breakpoint(dbg, target_line);    // ← underscore
    return result;
}

/*
 * vm_debug_dump_json_ext — serializza lo stato completo della VM in JSON.
 * Il buffer deve essere pre-allocato dal chiamante.
 * Ritorna il numero di byte scritti.
 */
int vm_debug_dump_json_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!g_debug_vm || !out) return 0;
    (void)dbg;
    return vm_debug_dump_json(g_debug_vm, out, outsz);
}

/*
 * vm_debug_vars_json_ext — solo le variabili del frame corrente.
 */
int vm_debug_vars_json_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!g_debug_vm || !out || !dbg) return 0;
    return vm_debug_vars_json(g_debug_vm, dbg->current_frame, out, outsz);
}

/*
 * vm_debug_stop — termina la sessione di debug forzatamente.
 */
void vm_debug_stop(VMDebugState *dbg)
{
    if (!dbg) return;
    dbg_resume(dbg, VM_MODE_DONE);
    pthread_join(g_debug_tid, NULL);

    /* Chiudi la pipe */
    if (dbg->output_pipe_fd > 0) { close(dbg->output_pipe_fd); dbg->output_pipe_fd = -1; }
    if (dbg->output_pipe_rd > 0) { close(dbg->output_pipe_rd); dbg->output_pipe_rd = -1; }

    free(g_debug_buf);
    g_debug_buf = NULL;
    free(g_debug_buf_orig);
    g_debug_buf_orig = NULL;  
    vm_free(g_debug_vm);
    free(g_debug_vm);  
    g_debug_vm  = NULL;
    g_debug_dbg = NULL;
}

int vm_debug_get_output_fd(VMDebugState *dbg)
{
    return dbg ? dbg->output_pipe_rd : -1;
}

void vm_debug_set_breakpoint(VMDebugState *dbg, int line)
{
    _vm_debug_set_breakpoint(dbg, line);
}

void vm_debug_clear_breakpoint(VMDebugState *dbg, int line)
{
    _vm_debug_clear_breakpoint(dbg, line);
}

void vm_debug_clear_all_breakpoints(VMDebugState *dbg)
{
    if (dbg) dbg->bp_count = 0;
}
int vm_debug_output_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!dbg || dbg->out_len == 0) return 0;
    int n = dbg->out_len < outsz - 1 ? dbg->out_len : outsz - 1;
    memcpy(out, dbg->out_buf, n);
    out[n] = '\0';
    dbg->out_len = 0;
    return n;
}
int vm_debug_error_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!dbg || dbg->last_error[0] == '\0') return 0;
    int n = (int)strlen(dbg->last_error);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, dbg->last_error, n);
    out[n] = '\0';
    return n;
}