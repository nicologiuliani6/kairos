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
#include "mn_native_arith.h"
#include "Kairos_core.h"


/* Puntatore alla VM corrente — usato da vm_printf in DAP_MODE */
VM *g_current_vm = NULL;
/* 1 = esegui procedure Mnemo mul/div/bitwise in C O(1) (KAIROS_NATIVE_ARITH=1 / vm_set_native_arith). */
int g_vm_native_arith = 0;

void vm_dump_active(VM *vm, const char *frame_name);  /* fwd: opcode DUMP (dump mid-run) */
void vm_print_stats(VM *vm);          /* fwd: --vm-stats (def più sotto) */
static int g_vm_stats_enabled;        /* tentative decl (def con =0 più sotto) */

void vm_set_native_arith(int enabled)
{
    g_vm_native_arith = enabled ? 1 : 0;
}
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

/* Dopo CALL __mn_hist_floor_snap il Mnemo emette subito CALL <proc> (coppia XOR+uncall). */
static inline char *mn_skip_bytecode_lineno_prefix(char *line)
{
    char *p = line;
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '@') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    return p;
}

static int mn_bytecode_next_token(char **scan, char *buf, size_t bufsz)
{
    char *s = *scan;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (*s == '\0' || *s == '\n') {
        *scan = s;
        return 0;
    }
    size_t i = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && i + 1 < bufsz)
        buf[i++] = *s++;
    buf[i] = '\0';
    *scan = s;
    return 1;
}

static void mn_hist_floor_snap_peek_next_call_callee(char *cursor_after_snap_line_nl,
                                                     char *out_callee,
                                                     size_t callee_sz)
{
    for (; *cursor_after_snap_line_nl;) {
        char *nline = strchr(cursor_after_snap_line_nl, '\n');
        size_t L    = nline ? (size_t)(nline - cursor_after_snap_line_nl)
                           : strlen(cursor_after_snap_line_nl);
        char  linebuf[16384];

        if (L >= sizeof(linebuf))
            vm_debug_panic("[VM] __mn_hist_floor_snap: riga bytecode troppo lunga\n");
        memcpy(linebuf, cursor_after_snap_line_nl, L);
        linebuf[L] = '\0';

        cursor_after_snap_line_nl = nline ? nline + 1 : cursor_after_snap_line_nl + L;

        char *past = mn_skip_bytecode_lineno_prefix(linebuf);
        if (*past == '\0')
            continue;

        char *wk       = past;
        char  tok_op[VAR_NAME_LENGTH], callee[VAR_NAME_LENGTH];
        if (!mn_bytecode_next_token(&wk, tok_op, sizeof(tok_op)))
            continue;
        if (strcmp(tok_op, "CALL") != 0) {
            vm_debug_panic(
                "[VM] __mn_hist_floor_snap: dopo lo snap attendevo CALL <proc>, trovato '%s'\n",
                tok_op);
        }
        if (!mn_bytecode_next_token(&wk, callee, sizeof(callee)))
            vm_debug_panic("[VM] __mn_hist_floor_snap: CALL senza nome procedura\n");
        if (!strcmp(callee, "__mn_hist_floor_snap"))
            vm_debug_panic("[VM] __mn_hist_floor_snap: snapshot Mnemo duplicati consecutivi\n");
        strncpy(out_callee, callee, callee_sz - 1);
        out_callee[callee_sz - 1] = '\0';
        return;
    }
    vm_debug_panic("[VM] __mn_hist_floor_snap: nessuna CALL dopo snapshot\n");
}

/* ======================================================================
 *  vm_run_BT — loop principale di esecuzione
 * ====================================================================== */

void vm_stats_sample(VM *vm);

/* Native interception di `call __mn_pool_load(slot, __mn_mem0..N, out, __mn_hist,
 * __mn_scratch)` (dispatch statico binary-search delle celle nominate per
 * `tbl[i]` a indice runtime). Il bytecode lega 917 parametri e poi fa, sul leaf
 * `slot==k`: `t=mem[k]; push(out,hist); out=t; push(t,hist)`. Qui lo eseguiamo
 * in C sul frame CHIAMANTE — out = mem[slot] spingendo gli STESSI 2 valori su
 * __mn_hist (push old-out, push mem[slot]) → l'inverse bytecode (uncall) resta
 * coerente, niente interception inverse. Salta il binding dei 917 param (= il
 * collo: ~4x di des). Ritorna 1 se gestito, 0 = fallback al bytecode (NB: in tal
 * caso strtok è già consumato, quindi gestiamo o paniciamo — gli arg di
 * pool_load sono sempre ben formati). Solo `g_vm_native_arith`. */
static int mn_native_pool_load_fwd(VM *vm, uint cfi_cur)
{
    Frame *f = vm->frames[cfi_cur];
    char *a; char first[VAR_NAME_LENGTH] = {0};
    char w0[VAR_NAME_LENGTH] = {0}, w1[VAR_NAME_LENGTH] = {0}, w2[VAR_NAME_LENGTH] = {0};
    int n = 0;
    while ((a = strtok(NULL, " \t"))) {
        if (n == 0) { strncpy(first, a, VAR_NAME_LENGTH - 1); first[VAR_NAME_LENGTH-1]='\0'; }
        strncpy(w0, w1, VAR_NAME_LENGTH - 1); w0[VAR_NAME_LENGTH-1]='\0';
        strncpy(w1, w2, VAR_NAME_LENGTH - 1); w1[VAR_NAME_LENGTH-1]='\0';
        strncpy(w2, a,  VAR_NAME_LENGTH - 1); w2[VAR_NAME_LENGTH-1]='\0';
        n++;
    }
    /* arg layout: slot=first, mem0..memK, out=w0, __mn_hist=w1, __mn_scratch=w2 */
    if (n < 4) vm_debug_panic("[VM] native __mn_pool_load: troppi pochi arg (%d)\n", n);
    int si = char_id_map_lookup(&f->VarIndexer, first);
    if (si < 0 || !f->vars[si]) vm_debug_panic("[VM] native __mn_pool_load: slot '%s'\n", first);
    int64_t slot = *(f->vars[si]->value);
    char cell[40]; snprintf(cell, sizeof(cell), "__mn_mem%lld", (long long)slot);
    int ci = char_id_map_lookup(&f->VarIndexer, cell);
    int oi = char_id_map_lookup(&f->VarIndexer, w0);
    int hi = char_id_map_lookup(&f->VarIndexer, w1);
    if (ci < 0 || oi < 0 || hi < 0 || !f->vars[ci] || !f->vars[oi] || !f->vars[hi]
        || f->vars[hi]->T != TYPE_STACK)
        vm_debug_panic("[VM] native __mn_pool_load: cella/out/hist non risolti (slot=%lld)\n",
                       (long long)slot);
    int64_t cellval = *(f->vars[ci]->value);
    int64_t *outp   = f->vars[oi]->value;
    mn_hist_push(f->vars[hi], *outp);    /* push(out): vecchio out */
    *outp = cellval;                     /* out := mem[slot] */
    mn_hist_push(f->vars[hi], cellval);  /* push(t): t = mem[slot] */
    return 1;
}

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
        Var  *saved_params[MAX_PROC_PARAMS];
        int   saved_param_count, callee_findex;
        Stack saved_local_vars;
        int   is_recursive_clone;
        int   base_findex;   /* frame-base della proc chiamata: per decremento `active` a END_PROC */
    } CallRecord;
    /* Call stack dinamico: cresce on-demand (raddoppia). Nessun hard cap. */
    uint cs_cap = VM_FRAMES_INIT_CAP;
    CallRecord *cs = malloc(sizeof(CallRecord) * cs_cap);
    int cs_top = -1;
#define VM_CS_ENSURE(needed) do { \
    if ((uint)(needed) >= cs_cap) { \
        uint _nc = cs_cap; while (_nc <= (uint)(needed)) _nc *= 2; \
        CallRecord *_nb = (CallRecord *)realloc(cs, sizeof(CallRecord) * _nc); \
        if (!_nb) vm_debug_panic("[VM] CALL: realloc(%u) fallita\n", _nc); \
        cs = _nb; cs_cap = _nc; \
    } \
} while (0)
    uint  si  = char_id_map_get(&FrameIndexer, fname);
    char *ptr = go_to_line(orig, vm->frames[si]->addr + 1);
    if (!ptr) { fprintf(stderr, "ERROR: '%s' non trovato\n", fname); free(cs); free(orig); return; }

    while (*ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[16384]; strncpy(lb, ptr, sizeof(lb) - 1);
        lb[sizeof(lb)-1] = '\0';
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
                vm->frames[cfi]->LocalVariables = cs[cs_top].saved_local_vars;
                /* Non liberare i Var PARAM dei cloni ricorsivi qui: sono guscio allocato
                 * in init_clone_frame; free() tra restore e ripresa del chiamante può
                 * corrompere l'heap o interferire con alias ancora attivi. vm_free() a
                 * fine esecuzione deduplica i Var* condivisi. */
                ptr = cs[cs_top].return_ptr;
                strncpy(fname, cs[cs_top].caller_frame, VAR_NAME_LENGTH - 1);
                fname[VAR_NAME_LENGTH - 1] = '\0';
                cs_top--;
            } else break;
            continue;
        }
        else if (!strcmp(fw, "CALL")) {
            vm_if_mark_call();
            char *pn      = strtok(NULL, " \t");
            /* Mnemo: registra len(__mn_hist) prima di call ottimizzata + uncall. */
            if (pn && !strcmp(pn, "__mn_hist_floor_snap")) {
                char *hn = strtok(NULL, " \t");
                if (!hn) vm_debug_panic("[VM] __mn_hist_floor_snap: manca stack\n");
                uint  cfi_snap = get_findex(fname);
                uint  si = char_id_map_get(&vm->frames[cfi_snap]->VarIndexer, hn);
                Var  *hv = vm->frames[cfi_snap]->vars[si];
                if (!hv || hv->T != TYPE_STACK)
                    vm_debug_panic("[VM] __mn_hist_floor_snap: non stack\n");
                if ((uint)vm->mn_hist_floor_snap_sp >= vm->mn_hist_floor_snaps_cap) {
                    uint new_cap = vm->mn_hist_floor_snaps_cap
                        ? vm->mn_hist_floor_snaps_cap * 2 : MNEMO_HIST_SNAP_INIT_CAP;
                    MnemoHistFloorSnapEntry *ns = (MnemoHistFloorSnapEntry *)realloc(
                        vm->mn_hist_floor_snaps, sizeof(MnemoHistFloorSnapEntry) * new_cap);
                    if (!ns) vm_debug_panic("[VM] __mn_hist_floor_snap realloc %u fallita\n", new_cap);
                    memset(ns + vm->mn_hist_floor_snaps_cap, 0,
                           sizeof(MnemoHistFloorSnapEntry) * (new_cap - vm->mn_hist_floor_snaps_cap));
                    vm->mn_hist_floor_snaps = ns;
                    vm->mn_hist_floor_snaps_cap = new_cap;
                }
                MnemoHistFloorSnapEntry *ent =
                    &vm->mn_hist_floor_snaps[vm->mn_hist_floor_snap_sp];
                ent->hist_len_floor = hv->stack_len;
                ent->frame_indexer_count_at_snap = FrameIndexer.count;
                mn_hist_floor_snap_peek_next_call_callee(nl + 1, ent->opt_call_callee,
                                                        sizeof(ent->opt_call_callee));
                vm->mn_hist_floor_snap_sp++;
                /* Fix P3: attiva execution trace per il subtree opt-uncall.
                 * op_jmpf forward push trace mentre active>0 E proc match.
                 * Inverse JMPF_ELSE pop quando trace non vuota.
                 * Decremento dopo UNCALL match. */
                vm->branch_trace_active++;
                strncpy(vm->branch_trace_proc, ent->opt_call_callee, VAR_NAME_LENGTH - 1);
                vm->branch_trace_proc[VAR_NAME_LENGTH - 1] = '\0';
                /* Cache line-range dei from-loop del callee: la forward op_jmpf
                 * NON deve pushare su branch_trace gli IF dentro un loop (il loro
                 * inverse usa recompute, non consuma il cursor → la window LIFO si
                 * disallineerebbe e gli IF top-level leggerebbero entry sbagliate
                 * → DELOCAL loop-counter / branch errati sotto opt-uncall). */
                {
                    LoopDescriptor _ld[VM_BT_LOOP_MAX];
                    int _nlp = collect_loops(vm, vm->branch_trace_proc, orig,
                                             _ld, VM_BT_LOOP_MAX);
                    vm->bt_loop_n = 0;
                    for (int _li = 0; _li < _nlp && vm->bt_loop_n < VM_BT_LOOP_MAX; _li++) {
                        vm->bt_loop_lo[vm->bt_loop_n] = _ld[_li].from_start_line;
                        vm->bt_loop_hi[vm->bt_loop_n] = _ld[_li].eval_exit_line;
                        vm->bt_loop_n++;
                    }
                    strncpy(vm->bt_loops_cached_proc, vm->branch_trace_proc,
                            VAR_NAME_LENGTH - 1);
                    vm->bt_loops_cached_proc[VAR_NAME_LENGTH - 1] = '\0';
                }
                *nl = '\n'; ptr = nl + 1;
                continue;
            }
            uint  cfi_cur = get_findex(fname);
            /* Native pool_load: salta i 917 param-bind, esegue out=mem[slot] in C
             * sul frame chiamante (hist coerente col bytecode → inverse OK). */
            if (g_vm_native_arith && !strcmp(pn, "__mn_pool_load")) {
                mn_native_pool_load_fwd(vm, cfi_cur);
                *nl = '\n'; ptr = nl + 1; continue;
            }
            char  base[VAR_NAME_LENGTH]; strncpy(base, fname, VAR_NAME_LENGTH - 1);
            char *at = strchr(base, '@'); if (at) *at = '\0';
            int   is_rec    = !strcmp(pn, base);
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
            strncpy(cs[cs_top].caller_frame, fname, VAR_NAME_LENGTH - 1);
            cs[cs_top].caller_frame[VAR_NAME_LENGTH - 1] = '\0';
            int  pc = vm->frames[cfi]->param_count, *pi = vm->frames[cfi]->param_indices;
            cs[cs_top].saved_param_count = pc;
            cs[cs_top].saved_local_vars  = vm->frames[cfi]->LocalVariables;
            stack_init(&vm->frames[cfi]->LocalVariables);
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
            /* Fix P3 trace: push trace_top corrente sullo stack del clone.
             * Inverse INVOP_CALL/UNCALL pop e setta come trace_window_start
             * corrente. Stack necessario perché clone reused tra siblings.
             * Solo se proc base matches trace_proc (procs altre = skip). */
            if (vm->branch_trace_active > 0) {
                char p_base[VAR_NAME_LENGTH];
                strncpy(p_base, pn, VAR_NAME_LENGTH - 1);
                p_base[VAR_NAME_LENGTH - 1] = '\0';
                char *pb_at2 = strchr(p_base, '@');
                if (pb_at2) *pb_at2 = '\0';
                if (!strcmp(p_base, vm->branch_trace_proc)) {
                    frame_ensure_trace(vm->frames[cfi], vm->frames[cfi]->trace_window_top);
                    vm->frames[cfi]->trace_window_stack[vm->frames[cfi]->trace_window_top++] =
                        vm->branch_trace_top;
                }
            }
            char nfname[VAR_NAME_LENGTH];
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
                else {
                    strncpy(nfname, pn, VAR_NAME_LENGTH - 1);
                    nfname[VAR_NAME_LENGTH - 1] = '\0';
                }
            }
            strncpy(fname, nfname, VAR_NAME_LENGTH - 1);
            fname[VAR_NAME_LENGTH - 1] = '\0';
            if (pn && mn_native_arith_call_forward(vm, pn, cfi)) {
                if (vm->frames[cs[cs_top].base_findex]->active > 0)
                    vm->frames[cs[cs_top].base_findex]->active--;
                for (int k = 0; k < cs[cs_top].saved_param_count; k++)
                    vm->frames[cfi]->vars[vm->frames[cfi]->param_indices[k]] =
                        cs[cs_top].saved_params[k];
                vm->frames[cfi]->LocalVariables = cs[cs_top].saved_local_vars;
                ptr = cs[cs_top].return_ptr;
                strncpy(fname, cs[cs_top].caller_frame, VAR_NAME_LENGTH - 1);
                fname[VAR_NAME_LENGTH - 1] = '\0';
                cs_top--;
                *nl = '\n';
                continue;
            }
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
            Var  *sv[MAX_PROC_PARAMS]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi]->vars[pi[k]];
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
            char inv_name[VAR_NAME_LENGTH];
            if (current_thread_args)
                make_thread_frame_key(pn, inv_name, sizeof(inv_name));
            else {
                strncpy(inv_name, pn, sizeof(inv_name) - 1);
                inv_name[sizeof(inv_name) - 1] = '\0';
            }
            Var *histv = NULL;
            for (int hk = 0; hk < pc; hk++) {
                Var *cv = vm->frames[cfi]->vars[pi[hk]];
                if (cv && cv->T == TYPE_STACK) {
                    histv = cv;
                    break;
                }
            }
            vm->invert_hist_guard_var   = NULL;
            vm->invert_hist_floor_min   = 0;
            vm->mn_hist_floor_pop_guard_anchor[0] = '\0';
            int matched_opt_uncall = 0;
            int frame_indexer_floor_to_restore = -1;
            if (histv && vm->mn_hist_floor_snap_sp > 0 &&
                !strcmp(vm->mn_hist_floor_snaps[vm->mn_hist_floor_snap_sp - 1].opt_call_callee, pn)) {
                MnemoHistFloorSnapEntry *top =
                    &vm->mn_hist_floor_snaps[--vm->mn_hist_floor_snap_sp];
                vm->invert_hist_floor_min = top->hist_len_floor;
                vm->invert_hist_guard_var = histv;
                strncpy(vm->mn_hist_floor_pop_guard_anchor, inv_name, VAR_NAME_LENGTH - 1);
                vm->mn_hist_floor_pop_guard_anchor[VAR_NAME_LENGTH - 1] = '\0';
                matched_opt_uncall = 1;
                frame_indexer_floor_to_restore = top->frame_indexer_count_at_snap;
            }
            /* Fix P3 trace: pop callee clone trace_window stack, imposta
             * trace_window_start corrente. Cursor reset = 0. Copia anche
             * sul BASE frame perché invert_op_to_line riceve frame_name
             * base e get_findex(base) → base fi. */
            int uncall_win_start = -1;
            if (vm->branch_trace_active > 0 && vm->frames[cfi]->trace_window_top > 0) {
                int win = vm->frames[cfi]->trace_window_stack[--vm->frames[cfi]->trace_window_top];
                vm->frames[cfi]->trace_window_start = win;
                vm->frames[cfi]->trace_window_cursor = 0;
                uint base_fi_t = char_id_map_get(&FrameIndexer, pn);
                vm->frames[base_fi_t]->trace_window_start = win;
                vm->frames[base_fi_t]->trace_window_cursor = 0;
                uncall_win_start = win;
            }
            /* Restore '\n' su orig prima del recursive scan: invert_op_to_line ->
               collect_ifs/collect_loops scansionano `orig` cercando '\n', con '\0'
               ancora attivo qui la scan si fermerebbe prematuramente. */
            *nl = '\n';
            if (!pn || !mn_native_arith_uncall_inverse(vm, pn, cfi))
                invert_op_to_line(vm, inv_name, orig, vm->frames[cfi]->end_addr - 1,
                                  vm->frames[cfi]->addr + 1, 1);
            vm->invert_hist_guard_var = NULL;
            vm->invert_hist_floor_min   = 0;
            vm->mn_hist_floor_pop_guard_anchor[0] = '\0';
            /* Fix P3: chiudi modalità trace e azzera trace residua (sanity).
             * Tronca branch_trace_top allo start della finestra consumata: il
             * consume inverso legge LIFO (top-1-cursor), quindi dopo l'uncall di
             * questa finestra il top deve scendere al suo start perché le
             * finestre esterne (call ricorsivi dello stesso opt-proc) leggano
             * il proprio range corretto. */
            if (matched_opt_uncall && vm->branch_trace_active > 0) {
                vm->branch_trace_active--;
                if (vm->branch_trace_active == 0) {
                    vm->branch_trace_top = 0;
                } else if (uncall_win_start >= 0 &&
                           uncall_win_start <= vm->branch_trace_top) {
                    vm->branch_trace_top = uncall_win_start;
                }
            }
            /* Rilascia frames generati durante il pattern opt-uncall (forward +
             * inverse). Permette riuso slot vm->frames per cicli call+uncall
             * consecutivi. Necessario per `printer(5); printer(10);` con
             * `__mn_putd_uint@N` auto-ricorsivo che cresce indefinitamente
             * tra cicli. */
            if (matched_opt_uncall && frame_indexer_floor_to_restore >= 0 &&
                frame_indexer_floor_to_restore < FrameIndexer.count) {
                pthread_mutex_lock(&var_indexer_mtx);
                /* Sanity: non scendere sotto floor. */
                if (frame_indexer_floor_to_restore < FrameIndexer.count) {
                    int old_count = FrameIndexer.count;
                    FrameIndexer.count = frame_indexer_floor_to_restore;
                    /* I frame [floor..old_count) restano allocati con Var*
                     * dangling. Reset name slot per evitare match futuri su
                     * chiave stale. */
                    for (int fi = frame_indexer_floor_to_restore;
                         fi < old_count; fi++) {
                        FrameIndexer.names[fi][0] = '\0';
                    }
                }
                pthread_mutex_unlock(&var_indexer_mtx);
            }
            VMLOG("[UNCALL] invert_op_to_line completata\n");
            for (int k = 0; k < pc; k++) vm->frames[cfi]->vars[pi[k]] = sv[k];
            vm->frames[cfi]->LocalVariables = slv;
            ptr = nl + 1; continue;
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
        else if (!strcmp(fw, "DUMP"))    vm_dump_active(vm, fname);
        else if (!strcmp(fw, "PUSHEQ"))  op_pusheq (vm, fname);
        else if (!strcmp(fw, "MINEQ"))   op_mineq  (vm, fname);
        else if (!strcmp(fw, "XOREQ"))   op_xoreq  (vm, fname);
        else if (!strcmp(fw, "MNHALVE")) op_mnhalve(vm, fname);
        else if (!strcmp(fw, "MNSPLIT32")) op_mnsplit32(vm, fname);
        else if (!strcmp(fw, "SWAP"))    op_swap   (vm, fname);
        else if (!strcmp(fw, "PUSH"))  op_push (vm, fname);
        else if (!strcmp(fw, "POP"))   op_pop  (vm, fname);
        else if (!strcmp(fw, "POOLADD"))    op_pooladd   (vm, fname);
        else if (!strcmp(fw, "POOLSUB"))    op_poolsub   (vm, fname);
        else if (!strcmp(fw, "POOLGETNEG")) op_poolgetneg(vm, fname);
        else if (!strcmp(fw, "POOLGET"))    op_poolget   (vm, fname);
        else if (!strcmp(fw, "POOLPUSH"))   op_poolpush  (vm, fname);
        else if (!strcmp(fw, "POOLPOP"))    op_poolpop   (vm, fname);
        else if (!strcmp(fw, "SSEND")) op_ssend(vm, fname);
        else if (!strcmp(fw, "SRECV")) op_srecv(vm, fname);
        else if (!strcmp(fw, "EVAL"))    op_eval   (vm, fname);
        else if (!strcmp(fw, "ASSERT"))  op_assert (vm, fname);
        else if (!strcmp(fw, "JMPF")) {
            int jmpf_line = atoi(ptr);
            *nl = '\n';
            char *np = op_jmpf(vm, fname, orig, jmpf_line);
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
    free(cs);
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
                vm_ensure_frame_cap(vm, idx);
                vm->frame_top = idx;
                char_id_map_init(&vm->frames[idx]->VarIndexer);
                stack_init(&vm->frames[idx]->LocalVariables);
                strncpy(vm->frames[idx]->name, name, VAR_NAME_LENGTH - 1);
                vm->frames[idx]->addr = line;
                VMLOG("[EXEC] PROC '%s' addr=%u\n", name, line);

            } else if (!strcmp(fw, "END_PROC")) {
                char *name = strtok(NULL, " \t");
                vm->frames[vm->frame_top]->end_addr = line;
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
                strncpy(vm->frames[vm->frame_top]->vars[vi]->name, vn, VAR_NAME_LENGTH - 1);
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

void vm_free(VM *vm)
{
    if (!vm) return;
    size_t freed_cap = (size_t)vm->frames_cap * (size_t)MAX_VARS;
    if (freed_cap == 0) freed_cap = (size_t)VM_FRAMES_INIT_CAP * (size_t)MAX_VARS;
    Var **freed = (Var **)calloc(freed_cap, sizeof(Var *));
    if (!freed) return;
    int freed_count = 0;
    for (int i = 0; i <= vm->frame_top; i++) {
        Frame *f = vm->frames[i];
        if (!f) continue;
        for (int j = 0; j < f->var_count; j++) {
            Var *v = f->vars[j];
            if (v) {
                int already_freed = 0;
                for (int k = 0; k < freed_count; k++) {
                    if (freed[k] == v) {
                        already_freed = 1;
                        break;
                    }
                }
                if (!already_freed) {
                    Var *to_free = v;
                    /* Evita double-free quando piu` slot puntano alla stessa Var
                       (es. alias parametri durante call/uncall in debug rebuild). */
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
                    free(to_free);
                    if (freed_count < (int)freed_cap) freed[freed_count++] = to_free;
                }
                f->vars[j] = NULL;
            }
        }
        f->var_count = 0;
    }
    free(freed);
    if (vm->frames) {
        for (uint i = 0; i < vm->frames_cap; i++) {
            if (vm->frames[i]) {
                /* buffer heap dinamici per-Frame */
                free(vm->frames[i]->vars);
                free(vm->frames[i]->label);
                free(vm->frames[i]->param_indices);
                free(vm->frames[i]->trace_window_stack);
                free(vm->frames[i]);
                vm->frames[i] = NULL;
            }
        }
        free(vm->frames);
        vm->frames = NULL;
        vm->frames_cap = 0;
    }
    if (vm->branch_trace) {
        free(vm->branch_trace);
        vm->branch_trace = NULL;
        vm->branch_trace_cap = 0;
    }
    if (vm->mn_hist_floor_snaps) {
        free(vm->mn_hist_floor_snaps);
        vm->mn_hist_floor_snaps = NULL;
        vm->mn_hist_floor_snaps_cap = 0;
    }
    if (vm->mn_pool) {
        free(vm->mn_pool);
        vm->mn_pool = NULL;
        vm->mn_pool_len = 0;
        vm->mn_pool_cap = 0;
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
            size_t n = (v->T == TYPE_STACK) ? v->stack_len : v->channel->buf_len;
            int64_t *arr = (v->T == TYPE_STACK) ? v->value : v->channel->buf;
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

/* op `dump` (MNDUMP): dump dello stato del frame attivo a metà esecuzione.
 * Usato da --check-invertibility: lo stato forward viene stampato PRIMA
 * dell'uncall (che reverte tutto), così il dump esce sempre — anche se
 * l'inverso fallisce (es. ssend/channel) o se l'uncall azzera la memoria.
 * Soppresso durante il replay inverso (come op_show) per non duplicare. */
void vm_dump_active(VM *vm, const char *frame_name)
{
    if (vm->suppress_show) return;
    vm_printf("=== VM dump ===\n");
    /* get_findex: stesso indice usato da op_local per allocare le celle. */
    uint fi = get_findex(frame_name);
    vm_dump_frame(vm->frames[fi]);
    /* Stats sullo stato forward (live cells qui, prima che uncall reverta/liberi).
     * Stampate qui perché un uncall che fallisce (es. ssend) abortisce prima
     * del vm_print_stats finale. */
    if (g_vm_stats_enabled)
        vm_print_stats(vm);
    vm->mn_dumped = 1;  /* salta dump+stats finali post-uncall */
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
            else if (v->T == TYPE_STACK) count += (uint64_t)v->stack_len;
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
    const char *na = getenv("KAIROS_NATIVE_ARITH");
    if (na && (na[0] == '1' || na[0] == 'y' || na[0] == 'Y' || na[0] == 't' || na[0] == 'T'))
        g_vm_native_arith = 1;
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
    /* Init frames dinamico (cresce on-demand via vm_ensure_frame_cap). */
    vm->frames = (Frame **)calloc(VM_FRAMES_INIT_CAP, sizeof(Frame *));
    if (!vm->frames) { fprintf(stderr, "VM frames alloc failed\n"); free(ast); free(vm); return; }
    vm->frames_cap = VM_FRAMES_INIT_CAP;
    vm->branch_trace = (int *)calloc(VM_BRANCH_TRACE_INIT_CAP, sizeof(int));
    if (!vm->branch_trace) { fprintf(stderr, "VM branch_trace alloc failed\n"); free(vm->frames); free(ast); free(vm); return; }
    vm->branch_trace_cap = VM_BRANCH_TRACE_INIT_CAP;
    vm->mn_hist_floor_snaps = (MnemoHistFloorSnapEntry *)calloc(
        MNEMO_HIST_SNAP_INIT_CAP, sizeof(MnemoHistFloorSnapEntry));
    if (!vm->mn_hist_floor_snaps) {
        fprintf(stderr, "VM mn_hist_floor_snaps alloc failed\n");
        free(vm->branch_trace); free(vm->frames); free(ast); free(vm); return;
    }
    vm->mn_hist_floor_snaps_cap = MNEMO_HIST_SNAP_INIT_CAP;
    vm_exec(vm, ast);
    if (dump_after && !vm->mn_dumped)
        vm_dump(vm);
    if (!vm->mn_dumped)
        vm_print_stats(vm);
    vm_free(vm);
    free(ast);
    free(vm);
}

void vm_run_from_string(const char *bytecode)
{
    vm_run_from_string_impl(bytecode, 1);
}

/* Esecuzione senza dump finale (es. binari Mnemo standalone). */
void vm_run_from_string_quiet(const char *bytecode)
{
    vm_run_from_string_impl(bytecode, 0);
}