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
#include "check_if_reversibility.h"
#include "Kairos_core.h"


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

void vm_free(VM *vm)
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
    vm_printf("=== VM dump ===\n");
    for (int i = 0; i <= vm->frame_top; i++) {
        Frame *f = &vm->frames[i];
        if (strcmp(f->name, "main") != 0) continue;
        for (int j = 0; j < f->var_count; j++) {
            Var *v = f->vars[j]; if (!v) continue;
            vm_printf("%s: ", v->name);
            if (v->T == TYPE_INT) {
                vm_printf("%d", *(v->value));
            } else {
                vm_printf("[");
                for (size_t k = 0; k < v->stack_len; k++) {
                    vm_printf("%d", v->value[k]);
                    if (k + 1 < v->stack_len) vm_printf(", ");
                }
                vm_printf("]");
            }
            vm_printf("\n");
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
    /* Dump finale sempre emesso in modalità run standard. */
    vm_dump(&vm);
    vm_free(&vm);
}