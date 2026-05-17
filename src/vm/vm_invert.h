#ifndef VM_INVERT_H
#define VM_INVERT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vm_types.h"
#include "vm_helpers.h"
#include "vm_ops.h"
#include "vm_debug.h"

/* Forward — vm_run_BT è definita in Kairos.c */
void vm_run_BT(VM *vm, char *buffer, char *frame_name_init);

static inline int invert_extract_srcline(const char *raw_line)
{
    const char *at = strchr(raw_line, '@');
    if (!at) return 0;
    return atoi(at + 1);
}

/* ======================================================================
 *  Descrittori strutturali per loop e if (usati dall'inversore)
 * ====================================================================== */

typedef struct {
    uint eval_entry_line;
    char eval_entry_id[64], eval_entry_val[64];
    char eval_entry_op[8]; /* bytecode EVAL memorizza !=, <, … — obbligatorio per invert */
    uint jmpf_err_line, from_start_line, from_end_line, from_err_line;
    uint eval_exit_line;
    char eval_exit_id[64], eval_exit_val[64];
    char eval_exit_op[8];
    uint jmpf_start_line;
} LoopDescriptor;

typedef struct {
    uint eval_entry_line;
    char eval_entry_id[64], eval_entry_val[64];
    char eval_entry_op[8];
    uint jmpf_else_line, jmp_fi_line, else_label_line, fi_label_line;
    uint eval_exit_line;
    char eval_exit_id[64], eval_exit_val[64];
    char eval_exit_op[8];
    uint assert_line;
} IfDescriptor;

typedef enum {
    LOOP_ZONE_NONE, LOOP_ZONE_EVAL_ENTRY, LOOP_ZONE_JMPF_ERR,
    LOOP_ZONE_START_LABEL, LOOP_ZONE_EVAL_EXIT, LOOP_ZONE_JMPF_START,
    LOOP_ZONE_END_LABEL, LOOP_ZONE_ERR_LABEL
} LoopZone;

typedef enum {
    IF_ZONE_NONE, IF_ZONE_EVAL_ENTRY, IF_ZONE_JMPF_ELSE, IF_ZONE_JMP_FI,
    IF_ZONE_ELSE_LABEL, IF_ZONE_FI_LABEL, IF_ZONE_EVAL_EXIT, IF_ZONE_ASSERT
} IfZone;

/* ======================================================================
 *  collect_loops / collect_ifs
 * ====================================================================== */

static inline void _copy_compare_op(char *dst8, const char *op_raw)
{
    if (op_raw && op_raw[0])
        strncpy(dst8, op_raw, 7);
    else
        strncpy(dst8, "==", 7);
    dst8[7] = '\0';
}

static inline int collect_loops(VM *vm, const char *frame_name, char *buf,
                                 LoopDescriptor *out, int max)
{
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi].addr + 1);
    int n = 0, in_loop = 0;
    uint peval = 0; char pid[64] = {0}, pval[64] = {0}, pop[8] = {'=', '=', '\0'};

    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[2048]; strncpy(lb, ptr, sizeof(lb) - 1);
        lb[sizeof(lb) - 1] = '\0';
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { *nl = '\n'; ptr = nl + 1; continue; }

        if (!strcmp(fw, "EVAL")) {
            peval = cur;
            char *a = strtok(NULL, " \t");  /* lhs */
            char *op = strtok(NULL, " \t");
            char rhs[256]; read_rest_of_expr(rhs, sizeof(rhs));
            strncpy(pid,  a   ? a   : "", 63);
            strncpy(pval, rhs,             63);
            _copy_compare_op(pop, op);
        } else if (!strcmp(fw, "JMPF") && !in_loop) {
            char *ln = strtok(NULL, " \t");
            if (ln && !strncmp(ln, "FROM_ERR", 8)) {
                out[n].eval_entry_line = peval;
                strncpy(out[n].eval_entry_id,  pid,  63);
                strncpy(out[n].eval_entry_val, pval, 63);
                _copy_compare_op(out[n].eval_entry_op, pop);
                out[n].jmpf_err_line = cur; in_loop = 1;
            }
        } else if (!strcmp(fw, "LABEL") && in_loop) {
            char *ln = strtok(NULL, " \t"); if (!ln) { *nl = '\n'; ptr = nl + 1; continue; }
            if      (!strncmp(ln, "FROM_START", 10)) out[n].from_start_line = cur;
            else if (!strncmp(ln, "FROM_END",   8))  out[n].from_end_line   = cur;
            else if (!strncmp(ln, "FROM_ERR",   8))  { out[n].from_err_line = cur; in_loop = 0; n++; }
        } else if (!strcmp(fw, "JMPF") && in_loop) {
            char *ln = strtok(NULL, " \t");
            if (ln && !strncmp(ln, "FROM_START", 10)) {
                out[n].eval_exit_line = peval;
                strncpy(out[n].eval_exit_id,  pid,  63);
                strncpy(out[n].eval_exit_val, pval, 63);
                _copy_compare_op(out[n].eval_exit_op, pop);
                out[n].jmpf_start_line = cur;
            }
        } else if (!strcmp(fw, "END_PROC")) { *nl = '\n'; break; }
        *nl = '\n'; ptr = nl + 1;
    }
    return n;
}

static inline int collect_ifs(VM *vm, const char *frame_name, char *buf,
                               IfDescriptor *out, int max)
{
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi].addr + 1);
    int n = 0;

    /* Stack di IF aperti — match label per uid (ELSE_<uid>, FI_<uid>) per gestire
     * nested IF (es. Mnemo loop con IF di guard dentro body). Top dello stack =
     * IF correntemente in costruzione; chiuso da ASSERT (con sentinel handling
     * per EVAL-FI seguita o no da ASSERT). */
    int   stack_idx[64];          /* indice in out[] */
    char  stack_uid[64][32];      /* uid dell'IF aperto (es. "42" da "ELSE_42") */
    int   stack_eval_exit_set[64];/* 1 se EVAL FI già visto, attendiamo ASSERT */
    int   top = -1;

    uint peval = 0; char pid[64] = {0}, pval[64] = {0};
    char pfi_op[8] = {'=', '=', '\0'};

    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[2048]; strncpy(lb, ptr, sizeof(lb) - 1);
        lb[sizeof(lb) - 1] = '\0';
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { *nl = '\n'; ptr = nl + 1; continue; }

        if (!strcmp(fw, "EVAL")) {
            peval = cur;
            char *a = strtok(NULL, " \t");  /* lhs */
            char *iop = strtok(NULL, " \t");
            char rhs[256]; read_rest_of_expr(rhs, sizeof(rhs));
            strncpy(pid,  a   ? a   : "", 63);
            strncpy(pval, rhs,             63);
            _copy_compare_op(pfi_op, iop);

            /* EVAL FI: se top dello stack è in_then-completato (jmp_fi visto) e
             * aspettiamo l'EVAL/ASSERT di chiusura, registra eval_exit qui. */
            if (top >= 0 && out[stack_idx[top]].fi_label_line && !stack_eval_exit_set[top]) {
                int ti = stack_idx[top];
                out[ti].eval_exit_line = cur;
                strncpy(out[ti].eval_exit_id,  pid,  63);
                strncpy(out[ti].eval_exit_val, pval, 63);
                _copy_compare_op(out[ti].eval_exit_op, pfi_op);
                stack_eval_exit_set[top] = 1;

                /* Peek rigo dopo: se ASSERT, sentinel; altrimenti EVAL=ASSERT collassati. */
                char *n2 = strchr(nl + 1, '\n');
                int  nx_asrt = 0;
                if (n2 && (size_t)(n2 - (nl + 1)) < sizeof(lb)) {
                    char peekb[2048];
                    memcpy(peekb, nl + 1, (size_t)(n2 - (nl + 1)));
                    peekb[(size_t)(n2 - (nl + 1))] = '\0';
                    char ptmp[2048];
                    strncpy(ptmp, peekb, sizeof(ptmp) - 1);
                    ptmp[sizeof(ptmp) - 1] = '\0';
                    char *p1 = strtok(skip_lineno(ptmp), " \t");
                    nx_asrt = (p1 && !strcmp(p1, "ASSERT"));
                }
                if (!nx_asrt) {
                    /* Collassato: EVAL fa anche da ASSERT. Chiudi qui. */
                    out[ti].assert_line = cur;
                    top--;
                }
            }
        } else if (!strcmp(fw, "JMPF")) {
            char *ln = strtok(NULL, " \t");
            if (ln && !strncmp(ln, "ELSE_", 5)) {
                if (top + 1 >= 64 || n >= max) { *nl = '\n'; ptr = nl + 1; continue; }
                int idx = n++;
                top++;
                stack_idx[top] = idx;
                strncpy(stack_uid[top], ln + 5, 31);
                stack_uid[top][31] = '\0';
                stack_eval_exit_set[top] = 0;
                memset(&out[idx], 0, sizeof(IfDescriptor));
                out[idx].eval_entry_line = peval;
                strncpy(out[idx].eval_entry_id,  pid,  63);
                strncpy(out[idx].eval_entry_val, pval, 63);
                _copy_compare_op(out[idx].eval_entry_op, pfi_op);
                out[idx].jmpf_else_line = cur;
            }
        } else if (!strcmp(fw, "JMP")) {
            char *ln = strtok(NULL, " \t");
            if (ln && !strncmp(ln, "FI_", 3)) {
                /* Match per uid sullo stack — tipicamente è il top, ma se più
                 * IF chiusi annidati incompleti, cerca tutto lo stack. */
                for (int s = top; s >= 0; s--) {
                    if (!strcmp(stack_uid[s], ln + 3)) {
                        out[stack_idx[s]].jmp_fi_line = cur;
                        break;
                    }
                }
            }
        } else if (!strcmp(fw, "LABEL")) {
            char *ln = strtok(NULL, " \t");
            if (!ln) { *nl = '\n'; ptr = nl + 1; continue; }
            if (!strncmp(ln, "ELSE_", 5)) {
                for (int s = top; s >= 0; s--) {
                    if (!strcmp(stack_uid[s], ln + 5)) {
                        out[stack_idx[s]].else_label_line = cur;
                        break;
                    }
                }
            } else if (!strncmp(ln, "FI_", 3)) {
                for (int s = top; s >= 0; s--) {
                    if (!strcmp(stack_uid[s], ln + 3)) {
                        out[stack_idx[s]].fi_label_line = cur;
                        break;
                    }
                }
            }
        } else if (!strcmp(fw, "ASSERT")) {
            if (top >= 0) {
                int ti = stack_idx[top];
                if (!out[ti].assert_line && out[ti].eval_exit_line) {
                    out[ti].assert_line = cur;
                } else {
                    out[ti].eval_exit_line = peval;
                    strncpy(out[ti].eval_exit_id,  pid,  63);
                    strncpy(out[ti].eval_exit_val, pval, 63);
                    _copy_compare_op(out[ti].eval_exit_op, pfi_op);
                    out[ti].assert_line = cur;
                }
                top--;
            }
        } else if (!strcmp(fw, "END_PROC")) { *nl = '\n'; break; }
        *nl = '\n'; ptr = nl + 1;
    }
    return n;
}

/* ======================================================================
 *  Zone classifiers
 * ====================================================================== */

static inline LoopZone line_loop_zone(uint line, LoopDescriptor *L, int n, int *idx)
{
    for (int i = 0; i < n; i++) {
        if (line == L[i].eval_entry_line)  { *idx = i; return LOOP_ZONE_EVAL_ENTRY; }
        if (line == L[i].jmpf_err_line)    { *idx = i; return LOOP_ZONE_JMPF_ERR;   }
        if (line == L[i].from_start_line)  { *idx = i; return LOOP_ZONE_START_LABEL; }
        if (line == L[i].eval_exit_line)   { *idx = i; return LOOP_ZONE_EVAL_EXIT;  }
        if (line == L[i].jmpf_start_line)  { *idx = i; return LOOP_ZONE_JMPF_START; }
        if (line == L[i].from_end_line)    { *idx = i; return LOOP_ZONE_END_LABEL;  }
        if (line == L[i].from_err_line)    { *idx = i; return LOOP_ZONE_ERR_LABEL;  }
    }
    *idx = -1; return LOOP_ZONE_NONE;
}

static inline IfZone line_if_zone(uint line, IfDescriptor *I, int n, int *idx)
{
    for (int i = 0; i < n; i++) {
        if (line == I[i].eval_entry_line) { *idx = i; return IF_ZONE_EVAL_ENTRY; }
        if (line == I[i].jmpf_else_line)  { *idx = i; return IF_ZONE_JMPF_ELSE;  }
        if (line == I[i].jmp_fi_line)     { *idx = i; return IF_ZONE_JMP_FI;     }
        if (line == I[i].else_label_line) { *idx = i; return IF_ZONE_ELSE_LABEL; }
        if (line == I[i].fi_label_line)   { *idx = i; return IF_ZONE_FI_LABEL;   }
        if (line == I[i].eval_exit_line)  { *idx = i; return IF_ZONE_EVAL_EXIT;  }
        if (line == I[i].assert_line)     { *idx = i; return IF_ZONE_ASSERT;     }
    }
    *idx = -1; return IF_ZONE_NONE;
}

/* bytecode.py può ripetere lo stesso numero di riga @N su più record: la sola lookup per
 * linea collide (EVAL prima di JMPF/LABEL). Classifica per opcode/etichetta. */

static inline LoopZone line_loop_zone_for_instr(uint line, const char *fw, const char *arg1,
                                                LoopDescriptor *L, int n, int *idx)
{
    if (fw && arg1) {
        for (int i = 0; i < n; i++) {
            if (!strcmp(fw, "JMPF")) {
                if (!strncmp(arg1, "FROM_ERR", 8) && line == L[i].jmpf_err_line) {
                    *idx = i; return LOOP_ZONE_JMPF_ERR;
                }
                if (!strncmp(arg1, "FROM_START", 10) && line == L[i].jmpf_start_line) {
                    *idx = i; return LOOP_ZONE_JMPF_START;
                }
            }
            if (!strcmp(fw, "LABEL")) {
                if (!strncmp(arg1, "FROM_START", 10) && line == L[i].from_start_line) {
                    *idx = i; return LOOP_ZONE_START_LABEL;
                }
                if (!strncmp(arg1, "FROM_END", 8) && line == L[i].from_end_line) {
                    *idx = i; return LOOP_ZONE_END_LABEL;
                }
                if (!strncmp(arg1, "FROM_ERR", 8) && line == L[i].from_err_line) {
                    *idx = i; return LOOP_ZONE_ERR_LABEL;
                }
            }
        }
    }
    return line_loop_zone(line, L, n, idx);
}

static inline IfZone line_if_zone_for_instr(uint line, const char *fw, const char *arg1,
                                            IfDescriptor *I, int n, int *idx)
{
    if (fw && arg1) {
        for (int i = 0; i < n; i++) {
            if (!strcmp(fw, "JMPF") && !strncmp(arg1, "ELSE_", 5) &&
                line == I[i].jmpf_else_line) {
                *idx = i; return IF_ZONE_JMPF_ELSE;
            }
            if (!strcmp(fw, "JMP") && !strncmp(arg1, "FI_", 3) &&
                line == I[i].jmp_fi_line) {
                *idx = i; return IF_ZONE_JMP_FI;
            }
            if (!strcmp(fw, "LABEL")) {
                if (!strncmp(arg1, "ELSE_", 5) && line == I[i].else_label_line) {
                    *idx = i; return IF_ZONE_ELSE_LABEL;
                }
                if (!strncmp(arg1, "FI_", 3) && line == I[i].fi_label_line) {
                    *idx = i; return IF_ZONE_FI_LABEL;
                }
            }
        }
    }
    if (fw && !strcmp(fw, "ASSERT")) {
        for (int i = 0; i < n; i++) {
            if (line == I[i].assert_line) {
                *idx = i; return IF_ZONE_ASSERT;
            }
        }
    }
    return line_if_zone(line, I, n, idx);
}

/* Primo record @line con opcode atteso — necessario perché più record condividono @line. */

static inline int lp_row_first_jmpf_from_start(uint line, char **lp, uint *ln, int nl)
{
    for (int j = 0; j < nl; j++) {
        if (ln[j] != line) continue;
        char buf[2048];
        strncpy(buf, lp[j], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char scan[2048];
        strncpy(scan, skip_lineno(buf), sizeof(scan) - 1);
        scan[sizeof(scan) - 1] = '\0';
        char *ff = strtok(scan, " \t");
        char *a1 = strtok(NULL, " \t");
        if (ff && !strcmp(ff, "JMPF") && a1 && !strncmp(a1, "FROM_START", 10)) return j;
    }
    return -1;
}

static inline int lp_row_first_eval_at_line(uint line, char **lp, uint *ln, int nl)
{
    for (int j = 0; j < nl; j++) {
        if (ln[j] != line) continue;
        char buf[2048];
        strncpy(buf, lp[j], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char scan[2048];
        strncpy(scan, skip_lineno(buf), sizeof(scan) - 1);
        scan[sizeof(scan) - 1] = '\0';
        char *ff = strtok(scan, " \t");
        if (ff && !strcmp(ff, "EVAL")) return j;
    }
    return -1;
}

static inline void do_eval(VM *vm, uint fi, const char *id, const char *op,
                           const char *val)
{
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, id);
    int  lval = *(vm->frames[fi].vars[vi]->value);
    int  rval = resolve_expr(vm, fi, val);
    thread_val_IF = eval_cond(lval, op, rval);
}

/* IF su snapshot local (saved_r, ts, …): in inversa il delocal può azzerare la copia
   prima del JMPF; usare il parametro sorgente ancora intatto (a, t, …). */
static inline int invert_if_entry_lval(VM *vm, uint fi, const char *id, int current)
{
    if (!strcmp(id, "saved_r")) {
        if (char_id_map_exists(&vm->frames[fi].VarIndexer, "saved_r")) {
            uint si = char_id_map_get(&vm->frames[fi].VarIndexer, "saved_r");
            Var *sv = vm->frames[fi].vars[si];
            if (sv)
                return *(sv->value);
        }
        if (char_id_map_exists(&vm->frames[fi].VarIndexer, "a")) {
            uint ai = char_id_map_get(&vm->frames[fi].VarIndexer, "a");
            return *(vm->frames[fi].vars[ai]->value);
        }
    }
    if (!strcmp(id, "ts")) {
        if (char_id_map_exists(&vm->frames[fi].VarIndexer, "ts")) {
            uint tsi = char_id_map_get(&vm->frames[fi].VarIndexer, "ts");
            Var *tsv = vm->frames[fi].vars[tsi];
            if (tsv)
                return *(tsv->value);
        }
        if (char_id_map_exists(&vm->frames[fi].VarIndexer, "t")) {
            uint ti = char_id_map_get(&vm->frames[fi].VarIndexer, "t");
            return *(vm->frames[fi].vars[ti]->value);
        }
    }
    return current;
}

static inline void do_eval_if_entry(VM *vm, uint fi, const char *id, const char *op,
                                    const char *val)
{
    /* `id` può essere un letterale numerico (es. `from 0 == 0 loop ...`): in tal caso
       lval = atoi(id), niente lookup in VarIndexer (eviterebbe SEGV). */
    int lval;
    if (id && (id[0] == '-' || (id[0] >= '0' && id[0] <= '9'))) {
        lval = (int)strtol(id, NULL, 10);
    } else {
        uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, id);
        lval = invert_if_entry_lval(vm, fi, id, *(vm->frames[fi].vars[vi]->value));
    }
    int  rval = resolve_expr(vm, fi, val);
    thread_val_IF = eval_cond(lval, op, rval);
}

/* `from id == 0`: la guardia d'ingresso vale solo alla prima iterata forward.
   In inversa: ripetere il corpo finché id>0; uscire a JMPF_ERR e JMPF_START quando id<=0. */
static inline int loop_entry_eq_zero_guard(const LoopDescriptor *L, int li)
{
    return !strcmp(L[li].eval_entry_op, "==") && !strcmp(L[li].eval_entry_val, "0");
}

static inline int loop_entry_counter_val(VM *vm, uint fi, const LoopDescriptor *L, int li)
{
    const char *eid = L[li].eval_entry_id;
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, eid)) return 0;
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, eid);
    return *(vm->frames[fi].vars[vi]->value);
}

/* __mn_divmod_nonneg: IF saved_r >= b — se forward non entrò nel loop, non invertire il corpo. */
static inline int divmod_saved_r_loop_skipped_forward(VM *vm, uint fi)
{
    int sr = 0;
    if (char_id_map_exists(&vm->frames[fi].VarIndexer, "saved_r")) {
        uint si = char_id_map_get(&vm->frames[fi].VarIndexer, "saved_r");
        Var *sv = vm->frames[fi].vars[si];
        if (sv)
            sr = *(sv->value);
    } else if (char_id_map_exists(&vm->frames[fi].VarIndexer, "a")) {
        uint ai = char_id_map_get(&vm->frames[fi].VarIndexer, "a");
        sr = *(vm->frames[fi].vars[ai]->value);
    } else {
        return 0;
    }
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, "b"))
        return 0;
    uint bi = char_id_map_get(&vm->frames[fi].VarIndexer, "b");
    Var *bv = vm->frames[fi].vars[bi];
    if (!bv || !bv->value)
        return 0;
    return sr < *(bv->value);
}

/* __mn_bit_k_signed: if k == 0 il from i==0 non gira in avanti. */
static inline int bit_k_loop_skipped_forward(VM *vm, uint fi)
{
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, "k"))
        return 0;
    uint ki = char_id_map_get(&vm->frames[fi].VarIndexer, "k");
    Var *kv = vm->frames[fi].vars[ki];
    if (!kv || !kv->value)
        return 0;
    return *(kv->value) == 0;
}

static inline int loop_peel_more_at_until(VM *vm, uint fi, LoopDescriptor *L, int li,
                                          int exit_cond_true)
{
    if (loop_entry_eq_zero_guard(L, li))
        return loop_entry_counter_val(vm, fi, L, li) > 0;
    return exit_cond_true;
}

// #region agent log
static inline int mn_dbg_read_int(VM *vm, uint fi, const char *name)
{
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, name)) return -9999;
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, name);
    Var *v = vm->frames[fi].vars[vi];
    if (!v || v->T != TYPE_INT) return -9998;
    return *(v->value);
}

static inline void mn_dbg_log_loop(const char *hypothesisId, const char *zone,
                                   const char *frame, long long iter, int i, int nl,
                                   int tif, int q, int r, int b, int peel_more)
{
    if (!strstr(frame, "divmod_nonneg")) return;
    if (iter > 500 && iter % 100000 != 0) return;
    FILE *f = fopen("/home/nico/Desktop/mnemo/.cursor/debug-acb76d.log", "a");
    if (!f) return;
    fprintf(f,
            "{\"sessionId\":\"acb76d\",\"hypothesisId\":\"%s\",\"location\":\"vm_invert.h:loop\","
            "\"message\":\"%s\",\"data\":{\"frame\":\"%s\",\"iter\":%lld,\"i\":%d,\"nl\":%d,"
            "\"tif\":%d,\"q\":%d,\"r\":%d,\"b\":%d,\"peel_more\":%d},\"timestamp\":%lld}\n",
            hypothesisId, zone, frame, iter, i, nl, tif, q, r, b, peel_more,
            (long long)time(NULL) * 1000);
    fclose(f);
}
// #endregion

/* IF il cui bytecode è contenuto nel corpo del from/until (non il test pre-loop).
   Serve a sapere quando applicare jmp_start_deep (solo corpi puri Mnemo-ish). */
static inline int loop_body_has_nested_if(uint from_sl, uint from_el, IfDescriptor *ifs, int nifs)
{
    for (int i = 0; i < nifs; i++) {
        if (ifs[i].eval_entry_line > from_sl && ifs[i].eval_entry_line < from_el)
            return 1;
    }
    return 0;
}

/* CALL/UNCALL nel corpo: peeling profondo rompe pipeline tipo example_login/bruteforce. */
static inline int loop_body_has_call_or_uncall(char *buf, uint from_sl, uint from_el)
{
    char *ptr = go_to_line(buf, from_sl);
    if (!ptr) return 0;
    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        uint cur = (uint)atoi(ptr);
        if (cur > from_el) { *nl = '\n'; break; }
        if (cur >= from_sl) {
            char lb[2048]; strncpy(lb, ptr, sizeof(lb) - 1);
            lb[sizeof(lb) - 1] = '\0';
            char *fw = strtok(skip_lineno(lb), " \t");
            if (fw && (!strcmp(fw, "CALL") || !strcmp(fw, "UNCALL"))) {
                *nl = '\n'; return 1;
            }
        }
        *nl = '\n'; ptr = nl + 1;
    }
    return 0;
}

static inline int loop_body_use_deep_peel(char *buf, LoopDescriptor *L, int ili,
                                           IfDescriptor *ifs, int nifs)
{
    if (!loop_body_has_nested_if(L[ili].from_start_line, L[ili].from_end_line, ifs, nifs))
        return 0;
    if (loop_body_has_call_or_uncall(buf, L[ili].from_start_line, L[ili].from_end_line))
        return 0;
    return 1;
}

/* PUSH <id> <stack> nel corpo from/until (per riga bytecode). */
static inline int loop_body_push_count_to_stack(char *buf, uint from_sl, uint from_el,
                                                const char *stack_name)
{
    int n = 0;
    char *ptr = go_to_line(buf, from_sl);
    if (!ptr) return 0;
    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        uint cur = (uint)atoi(ptr);
        if (cur > from_el) { *nl = '\n'; break; }
        if (cur >= from_sl) {
            char lb[2048]; strncpy(lb, ptr, sizeof(lb) - 1);
            lb[sizeof(lb) - 1] = '\0';
            char scan[2048];
            strncpy(scan, skip_lineno(lb), sizeof(scan) - 1);
            scan[sizeof(scan) - 1] = '\0';
            char *tok = strtok(scan, " \t"); /* PUSH */
            if (tok && !strcmp(tok, "PUSH")) {
                strtok(NULL, " \t"); /* id */
                char *st = strtok(NULL, " \t");
                if (st && !strcmp(st, stack_name))
                    n++;
            }
        }
        *nl = '\n'; ptr = nl + 1;
    }
    return n;
}

/* Bump jmp_start_deep: una unità per PUSH su hist/scratch nel corpo + margine IF interno Mnemo.
   Es. Mnemo ladder: 4 PUSH hist + 2 scratch + 3 ≡ 9. */
static inline int loop_mnemo_deep_increment(char *buf, LoopDescriptor *L, int ili,
                                            IfDescriptor *ifs, int nifs)
{
    if (!loop_body_use_deep_peel(buf, L, ili, ifs, nifs)) return 0;
    int nh = loop_body_push_count_to_stack(buf, L[ili].from_start_line, L[ili].from_end_line,
                                              "__mn_hist");
    int ns = loop_body_push_count_to_stack(buf, L[ili].from_start_line, L[ili].from_end_line,
                                              "__mn_scratch");
    /* Un IF nel corpo duplica un PUSH e0 (then+else) nel sorgente: una sola push per iter. */
    int inner_ifs = 0;
    for (int j = 0; j < nifs; j++)
        if (ifs[j].eval_entry_line > L[ili].from_start_line &&
            ifs[j].eval_entry_line < L[ili].from_end_line)
            inner_ifs++;
    nh -= inner_ifs;
    if (nh < 1) nh = 1;
    return nh + ns + 3;
}

static inline int line_is_inside_if(uint line, IfDescriptor *ifs, int nifs)
{
    /* THEN: (jmpf_else, jmp_fi); ELSE: (else_label, fi_label). Prima si usava fi_label
       per il THEN e il corpo loop veniva ri-eseguito in invert_op_to_line principale. */
    for (int i = 0; i < nifs; i++) {
        if (line > ifs[i].jmpf_else_line && line < ifs[i].jmp_fi_line) return 1;
        if (line > ifs[i].else_label_line && line < ifs[i].fi_label_line) return 1;
    }
    return 0;
}

/* ======================================================================
 *  ParRange — intervallo [par_start_line .. par_end_line]
 *
 *  Raccogliamo tutti i blocchi PAR_START/PAR_END della procedura prima
 *  di entrare nel loop inverso, in modo da poter skippare le istruzioni
 *  che si trovano *dentro* un blocco PAR. Quelle istruzioni vengono
 *  gestite da exec_par_threads(is_inverse=1) quando il loop incontra
 *  PAR_START, e non devono essere eseguite una seconda volta dal loop.
 * ====================================================================== */

typedef struct { uint start_line, end_line; } ParRange;

static inline int collect_par_ranges(char *buf, uint proc_start, uint proc_end,
                                     ParRange *out, int max)
{
    int   n   = 0;
    char *ptr = go_to_line(buf, proc_start);
    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        uint cur = (uint)atoi(ptr);
        if (cur >= proc_end) { *nl = '\n'; break; }
        char tmp[2048]; strncpy(tmp, ptr, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw && !strcmp(fw, "PAR_START")) {
            out[n].start_line = cur;
            int   depth = 1;
            uint  max_inner = cur;
            char *scan  = nl + 1;
            *nl = '\n';
            while (scan && *scan && depth > 0) {
                char *nl2 = strchr(scan, '\n'); if (!nl2) break; *nl2 = '\0';
                uint  cur2 = (uint)atoi(scan);
                if (cur2 > max_inner) max_inner = cur2;
                char tmp2[2048]; strncpy(tmp2, scan, sizeof(tmp2) - 1);
                tmp2[sizeof(tmp2) - 1] = '\0';
                char *fw2  = strtok(skip_lineno(tmp2), " \t");
                if (fw2) {
                    if      (!strcmp(fw2, "PAR_START")) depth++;
                    else if (!strcmp(fw2, "PAR_END")) {
                        depth--;
                        if (depth == 0) {
                            out[n].end_line = max_inner;
                            n++;
                            *nl2 = '\n';
                            ptr = nl2 + 1;
                            goto next;
                        }
                    }
                }
                *nl2 = '\n'; scan = nl2 + 1;
            }
            break; /* PAR_END non trovato */
        }
        *nl = '\n'; ptr = nl + 1;
        next:;
    }
    return n;
}

static inline int line_is_inside_par(uint line, ParRange *pars, int npars)
{
    for (int i = 0; i < npars; i++)
        if (line > pars[i].start_line && line <= pars[i].end_line) return 1;
    return 0;
}

/* ======================================================================
 *  exec_branch_inverse — forward declaration (mutua ricorsione con
 *  invert_op_to_line)
 * ====================================================================== */

static void exec_branch_inverse(VM *vm, char *original_buffer,
                                const char *frame_name,
                                uint from_line, uint to_line,
                                uint caller_fi);


/* ======================================================================
 *  invert_op_to_line
 * ====================================================================== */

void invert_op_to_line(VM *vm, const char *frame_name, char *buffer,
                       uint start, uint stop, int honor_if_line_skip)
{
    //fprintf(stderr, "[INVERT] frame='%s' start=%u stop=%u depth=%d\n", frame_name, start, stop, vm->inversion_depth);
    char *orig = strdup(buffer);
    if (!orig) { vm_debug_panic("[UNCALL] strdup fallita\n"); }
    VMLOG("[INVERT] frame='%s' start=%u stop=%u\n", frame_name, start, stop);
    vm->inversion_depth++;   
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi_reset = char_id_map_get(&FrameIndexer, base);
    // #region agent log
    if (strstr(frame_name, "divmod_nonneg")) {
        FILE *_df = fopen("/home/nico/Desktop/mnemo/.cursor/debug-acb76d.log", "a");
        if (_df) {
            fprintf(_df,
                    "{\"sessionId\":\"acb76d\",\"hypothesisId\":\"C\",\"location\":\"invert_entry\","
                    "\"message\":\"invert_op_to_line\",\"data\":{\"frame\":\"%s\",\"a\":%d,\"b\":%d,"
                    "\"q\":%d,\"r\":%d,\"depth\":%d},\"timestamp\":%lld}\n",
                    frame_name, mn_dbg_read_int(vm, fi_reset, "a"),
                    mn_dbg_read_int(vm, fi_reset, "b"), mn_dbg_read_int(vm, fi_reset, "q"),
                    mn_dbg_read_int(vm, fi_reset, "r"), vm->inversion_depth,
                    (long long)time(NULL) * 1000);
            fclose(_df);
        }
    }
    // #endregion
    stack_init(&vm->frames[fi_reset].LocalVariables);

#define MAX_LOOPS 32
#define MAX_IFS   32
#define MAX_LINES 1024
#define MAX_PARS  32
    int jmp_start_deep[MAX_LOOPS];
    memset(jmp_start_deep, 0, sizeof(jmp_start_deep));

    LoopDescriptor loops[MAX_LOOPS]; int nloops = collect_loops(vm, frame_name, orig, loops, MAX_LOOPS);
    IfDescriptor   ifs  [MAX_IFS];   int nifs   = collect_ifs  (vm, frame_name, orig, ifs,   MAX_IFS);

    char cur_frame[VAR_NAME_LENGTH]; strncpy(cur_frame, frame_name, VAR_NAME_LENGTH - 1);
    uint fi       = get_findex(cur_frame);
    uint start_ln = vm->frames[fi_reset].addr + 1;

    /* Raccoglie i range PAR della procedura per skippare le istruzioni
       interne durante il loop inverso (vengono gestite da exec_par_threads). */
    ParRange pars[MAX_PARS];
    int npars = collect_par_ranges(orig, start_ln, vm->frames[fi_reset].end_addr, pars, MAX_PARS);

    char *lp[MAX_LINES]; uint ln[MAX_LINES]; int nl = 0;
    char *ptr = go_to_line(orig, stop + 1);   // ← CAMBIA: parti da dopo PROC
    while (ptr && *ptr && nl < MAX_LINES) {
        char *newline = strchr(ptr, '\n'); if (!newline) break;
        *newline = '\0';
        uint cur_ln = (uint)atoi(ptr);
        char tmp[2048]; strncpy(tmp, ptr, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw && !strcmp(fw, "END_PROC")) { *newline = '\n'; break; }
        if (cur_ln <= start && cur_ln > stop) {   // ← già corretto
            lp[nl] = strdup(ptr); ln[nl] = cur_ln; nl++;
        }
        *newline = '\n'; ptr = newline + 1;
    }
    //fprintf(stderr, "[INVERT] start=%u stop=%u righe_raccolte=%d\n", start, stop, nl);
    // #region agent log
    if (strstr(frame_name, "move_int")) {
        FILE *_mf = fopen("/home/nico/Desktop/mnemo/.cursor/debug-acb76d.log", "a");
        if (_mf) {
            fprintf(_mf,
                    "{\"sessionId\":\"acb76d\",\"hypothesisId\":\"F\",\"location\":\"invert_lp\","
                    "\"message\":\"lp_collect\",\"data\":{\"frame\":\"%s\",\"nl\":%d,\"start\":%u,"
                    "\"stop\":%u,\"depth\":%d},\"timestamp\":%lld}\n",
                    frame_name, nl, start, stop, vm->inversion_depth,
                    (long long)time(NULL) * 1000);
            for (int _j = 0; _j < nl && _j < 16; _j++) {
                char _ob[256];
                strncpy(_ob, lp[_j], sizeof(_ob) - 1);
                _ob[sizeof(_ob) - 1] = '\0';
                char *_fw = strtok(skip_lineno(_ob), " \t");
                fprintf(_mf,
                        "{\"sessionId\":\"acb76d\",\"hypothesisId\":\"F\",\"location\":\"invert_lp_line\","
                        "\"message\":\"lp_row\",\"data\":{\"ln\":%u,\"op\":\"%s\"},\"timestamp\":%lld}\n",
                        ln[_j], _fw ? _fw : "?", (long long)time(NULL) * 1000);
            }
            fclose(_mf);
        }
    }
    // #endregion

    int i = nl - 1;
    long long _iter_count = 0;
    while (i >= 0) {
        if (++_iter_count % 100000 == 0)
            fprintf(stderr, "[INVERT_LOOP] frame='%s' iter=%lld i=%d nl=%d\n",
                    frame_name, _iter_count, i, nl);
        char ob[2048]; strncpy(ob, lp[i], sizeof(ob) - 1);
        ob[sizeof(ob) - 1] = '\0';
        uint  cur   = ln[i];
        char *clean = skip_lineno(ob);
        /* exe_line: catena strtok per CALL/UNCALL/… senza toccherla prima con fw_cls */
        char exe_line[2048];
        strncpy(exe_line, clean, sizeof(exe_line) - 1);
        exe_line[sizeof(exe_line) - 1] = '\0';
        char zbuf[2048];
        strncpy(zbuf, exe_line, sizeof(zbuf) - 1);
        zbuf[sizeof(zbuf) - 1] = '\0';
        char *fw_cls = strtok(zbuf, " \t");
        char *arg1_cls = strtok(NULL, " \t");
        if (!fw_cls) { i--; continue; }
        if (!strcmp(frame_name, "__mn_divmod_nonneg"))
            VMLOG("[INV_LOOP] frame='%s' i=%d cur=%u fw='%s'\n", frame_name, i, cur, fw_cls ? fw_cls : "NULL");
        /* Nel ramo THEN già invertito da exec_branch_inverse (honor_if_line_skip=0 lì). */
        if (honor_if_line_skip && line_is_inside_if(cur, ifs, nifs)) { i--; continue; }
        if (line_is_inside_par(cur, pars, npars)) { i--; continue; }
        int li = -1;
        LoopZone lz = line_loop_zone_for_instr(cur, fw_cls, arg1_cls, loops, nloops, &li);
        if (lz == LOOP_ZONE_EVAL_ENTRY || lz == LOOP_ZONE_EVAL_EXIT  ||
            lz == LOOP_ZONE_START_LABEL|| lz == LOOP_ZONE_END_LABEL  ||
            lz == LOOP_ZONE_ERR_LABEL)  { i--; continue; }

        if (lz == LOOP_ZONE_JMPF_ERR) {
            if (strstr(frame_name, "divmod_nonneg") &&
                loop_entry_eq_zero_guard(loops, li) &&
                divmod_saved_r_loop_skipped_forward(vm, fi)) {
                i--;
                continue;
            }
            if (strstr(frame_name, "bit_k_signed") &&
                loop_entry_eq_zero_guard(loops, li) &&
                !strcmp(loops[li].eval_entry_id, "i") &&
                bit_k_loop_skipped_forward(vm, fi)) {
                i--;
                continue;
            }
            do_eval(vm, fi, loops[li].eval_entry_id, loops[li].eval_entry_op,
                    loops[li].eval_entry_val);
            // #region agent log
            mn_dbg_log_loop("B", "JMPF_ERR", frame_name, _iter_count, i, nl,
                            (int)thread_val_IF,
                            loop_entry_eq_zero_guard(loops, li)
                                ? loop_entry_counter_val(vm, fi, loops, li)
                                : mn_dbg_read_int(vm, fi, "q"),
                            mn_dbg_read_int(vm, fi, "r"), mn_dbg_read_int(vm, fi, "b"), -1);
            // #endregion
            if (loop_entry_eq_zero_guard(loops, li)) {
                if (loop_entry_counter_val(vm, fi, loops, li) <= 0) {
                    i--;
                    continue;
                }
                int tt = lp_row_first_jmpf_from_start(loops[li].jmpf_start_line, lp, ln, nl);
                if (tt < 0) { vm_debug_panic("[UNCALL] jmpf_start\n"); }
                i = tt - 1;
                continue;
            }
            int deep_peel =
                loop_body_use_deep_peel(orig, loops, li, ifs, nifs);
            if (deep_peel) {
                if (thread_val_IF) {
                    if (jmp_start_deep[li] > 0) {
                        jmp_start_deep[li]--;
                        int tt = lp_row_first_jmpf_from_start(loops[li].jmpf_start_line, lp, ln, nl);
                        if (tt < 0) { vm_debug_panic("[UNCALL] jmpf_start\n"); }
                        i = tt - 1;
                    } else {
                        i--;
                    }
                } else {
                    int tt = lp_row_first_jmpf_from_start(loops[li].jmpf_start_line, lp, ln, nl);
                    if (tt < 0) { vm_debug_panic("[UNCALL] jmpf_start\n"); }
                    i = tt - 1;
                }
            } else {
                if (thread_val_IF) {
                    i--;
                } else {
                    int tt = lp_row_first_jmpf_from_start(loops[li].jmpf_start_line, lp, ln, nl);
                    if (tt < 0) { vm_debug_panic("[UNCALL] jmpf_start\n"); }
                    i = tt - 1;
                }
            }
            continue;
        }
        if (lz == LOOP_ZONE_JMPF_START) {
            if (strstr(frame_name, "divmod_nonneg") &&
                loop_entry_eq_zero_guard(loops, li) &&
                divmod_saved_r_loop_skipped_forward(vm, fi)) {
                i--;
                continue;
            }
            if (strstr(frame_name, "bit_k_signed") &&
                loop_entry_eq_zero_guard(loops, li) &&
                !strcmp(loops[li].eval_entry_id, "i") &&
                bit_k_loop_skipped_forward(vm, fi)) {
                i--;
                continue;
            }
            do_eval(vm, fi, loops[li].eval_exit_id, loops[li].eval_exit_op,
                    loops[li].eval_exit_val);
            /* Forward: exit loop se exit-cond vera (senza jmp). Inverse: mentre il corpo da
               questa iterazione non è stato completamente inverted, ripeti da prima
               dell'EVAL until; quando la guardia coincide con uscita inversa, solo i--.
               I ramif erano scambiati: con e0==0 al primo incontr prendevamo i-- e mai il corpo. */
            int peel_more = loop_peel_more_at_until(vm, fi, loops, li, thread_val_IF);
            // #region agent log
            mn_dbg_log_loop("A", "JMPF_START", frame_name, _iter_count, i, nl,
                            (int)thread_val_IF, mn_dbg_read_int(vm, fi, "q"),
                            mn_dbg_read_int(vm, fi, "r"), mn_dbg_read_int(vm, fi, "b"),
                            peel_more);
            // #endregion
            if (!peel_more) { i--; }
            else {
                /* Uscita-until: caricare jmp_start_deep per le iterazioni residue (Mnemo). */
                int dinc = loop_mnemo_deep_increment(orig, loops, li, ifs, nifs);
                if (dinc > 0)
                    jmp_start_deep[li] += dinc;
                int t = lp_row_first_eval_at_line(loops[li].eval_exit_line, lp, ln, nl);
                if (t < 0) { vm_debug_panic("[UNCALL] loop_eval_exit\n"); }
                i = t - 1;
            }
            continue;
        }

        int ii = -1;
        IfZone iz = line_if_zone_for_instr(cur, fw_cls, arg1_cls, ifs, nifs, &ii);
        if (iz == IF_ZONE_EVAL_ENTRY || iz == IF_ZONE_EVAL_EXIT || iz == IF_ZONE_ELSE_LABEL ||
            iz == IF_ZONE_FI_LABEL   || iz == IF_ZONE_ASSERT    || iz == IF_ZONE_JMP_FI)
            { i--; continue; }

        if (iz == IF_ZONE_JMPF_ELSE) {
            int depth = vm->frames[fi_reset].recursion_depth;
            if (depth > 0) {
                /* Recursive procedure: the entry guard can be overwritten by nested calls.
                   Use recorded recursion depth to replay ELSE inversions, then base THEN. */
                for (int d = 0; d < depth; d++) {
                    uint else_from = ifs[ii].else_label_line + 1;
                    uint else_to = ifs[ii].fi_label_line;
                    if (else_from >= else_to) break;
                    Stack sv = vm->frames[fi].LocalVariables;
                    stack_init(&vm->frames[fi].LocalVariables);
                    exec_branch_inverse(vm, orig, cur_frame, else_from, else_to, fi);
                    vm->frames[fi].LocalVariables = sv;
                }
                uint then_from = ifs[ii].jmpf_else_line + 1;
                uint then_to = ifs[ii].jmp_fi_line;
                if (then_from < then_to) {
                    Stack sv = vm->frames[fi].LocalVariables;
                    stack_init(&vm->frames[fi].LocalVariables);
                    exec_branch_inverse(vm, orig, cur_frame, then_from, then_to, fi);
                    vm->frames[fi].LocalVariables = sv;
                }
            } else {
                do_eval_if_entry(vm, fi, ifs[ii].eval_entry_id, ifs[ii].eval_entry_op,
                                 ifs[ii].eval_entry_val);
                uint branch_from = 0, branch_to = 0;
                if (thread_val_IF) {
                    /* Forward IF condition true: invert only THEN branch. */
                    branch_from = ifs[ii].jmpf_else_line + 1;
                    branch_to = ifs[ii].jmp_fi_line;
                } else {
                    /* Forward IF condition false: invert only ELSE branch. */
                    branch_from = ifs[ii].else_label_line + 1;
                    branch_to = ifs[ii].fi_label_line;
                }
                if (branch_from >= branch_to) {
                    int t = -1;
                    for (int j = i - 1; j >= 0; j--) if (ln[j] == ifs[ii].eval_entry_line) { t = j; break; }
                    i = (t >= 0) ? t - 1 : i - 1;
                    continue;
                }
                Stack sv = vm->frames[fi].LocalVariables;
                stack_init(&vm->frames[fi].LocalVariables);
                exec_branch_inverse(vm, orig, cur_frame, branch_from, branch_to, fi);
                vm->frames[fi].LocalVariables = sv;
            }
            int t = -1;
            for (int j = i - 1; j >= 0; j--) if (ln[j] == ifs[ii].eval_entry_line) { t = j; break; }
            i = (t >= 0) ? t - 1 : i - 1;
            continue;
        }

        char *fw = strtok(exe_line, " \t");
        if (!fw) { i--; continue; }

        if (!strcmp(fw, "PAR_END")) { i--; continue; }

        if (strstr(frame_name, "divmod_nonneg") &&
            divmod_saved_r_loop_skipped_forward(vm, fi) && fw_cls && arg1_cls &&
            ((!strcmp(fw_cls, "MINEQ") && !strcmp(arg1_cls, "r")) ||
             (!strcmp(fw_cls, "PUSHEQ") && !strcmp(arg1_cls, "q")))) {
            i--;
            continue;
        }
        if (strstr(frame_name, "bit_k_signed") &&
            bit_k_loop_skipped_forward(vm, fi) && fw_cls && arg1_cls &&
            ((!strcmp(fw_cls, "MINEQ") && !strcmp(arg1_cls, "i")) ||
             (!strcmp(fw_cls, "PUSHEQ") && !strcmp(arg1_cls, "i")))) {
            i--;
            continue;
        }

        if (!strcmp(fw, "CALL")) {
            if (vm->dbg && vm->dbg->initialized)
                dbg_hook(vm->dbg, invert_extract_srcline(lp[i]), cur_frame, lp[i]);
            char *pn = strtok(NULL, " \t");
            char base_cur[VAR_NAME_LENGTH];
            strncpy(base_cur, frame_name, VAR_NAME_LENGTH - 1);
            base_cur[VAR_NAME_LENGTH - 1] = '\0';
            char *at_cur = strchr(base_cur, '@');
            if (at_cur) *at_cur = '\0';
            int is_rec = (strcmp(pn, base_cur) == 0);
            int new_depth = 0;
            if (is_rec) {
                const char *atf = strchr(frame_name, '@');
                if (atf) {
                    const char *us = strrchr(frame_name, '_');
                    if (us && us > atf) new_depth = atoi(us + 1);
                    else new_depth = atoi(atf + 1);
                }
                new_depth++;
            }
            uint cfi = is_rec ? clone_frame_for_depth(vm, pn, new_depth)
                              : (current_thread_args ? clone_frame_for_thread(vm, pn)
                                                     : char_id_map_get(&FrameIndexer, pn));
            uint curi = get_findex(frame_name);
            int  pc = vm->frames[cfi].param_count, *pi = vm->frames[cfi].param_indices;
            Var *sv[64]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi].vars[pi[k]];
            char *p = NULL; int j = 0;
            while ((p = strtok(NULL, " \t")) && j < pc) {
                int si = char_id_map_get(&vm->frames[curi].VarIndexer, p);
                vm->frames[cfi].vars[pi[j++]] = vm->frames[curi].vars[si];
            }
            char target[VAR_NAME_LENGTH];
            if (is_rec && current_thread_args) {
                make_frame_key_par_rec(pn, new_depth, target, sizeof(target));
            } else if (is_rec) {
                make_frame_key(pn, new_depth, target, sizeof(target));
            } else if (current_thread_args) {
                make_thread_frame_key(pn, target, sizeof(target));
            } else {
                strncpy(target, pn, sizeof(target) - 1);
                target[sizeof(target) - 1] = '\0';
            }
            invert_op_to_line(vm, target, orig, vm->frames[cfi].end_addr - 1,
                              vm->frames[cfi].addr + 1, 1);
            for (int k = 0; k < pc; k++) vm->frames[cfi].vars[pi[k]] = sv[k];
            i--; continue;
        }
        if (!strcmp(fw, "UNCALL")) {
            if (vm->dbg && vm->dbg->initialized)
                dbg_hook(vm->dbg, invert_extract_srcline(lp[i]), cur_frame, lp[i]);
            char *pn = strtok(NULL, " \t");
            char base_cur[VAR_NAME_LENGTH];
            strncpy(base_cur, frame_name, VAR_NAME_LENGTH - 1);
            base_cur[VAR_NAME_LENGTH - 1] = '\0';
            char *at_cur = strchr(base_cur, '@');
            if (at_cur) *at_cur = '\0';
            int is_rec = (strcmp(pn, base_cur) == 0);
            int new_depth = 0;
            if (is_rec) {
                const char *atf = strchr(frame_name, '@');
                if (atf) {
                    const char *us = strrchr(frame_name, '_');
                    if (us && us > atf) new_depth = atoi(us + 1);
                    else new_depth = atoi(atf + 1);
                }
                new_depth++;
            }
            uint cfi = is_rec ? clone_frame_for_depth(vm, pn, new_depth)
                              : (current_thread_args ? clone_frame_for_thread(vm, pn)
                                                     : char_id_map_get(&FrameIndexer, pn));
            uint curi = fi;
            int  pc = vm->frames[cfi].param_count, *pi = vm->frames[cfi].param_indices;
            Var *sv[64]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi].vars[pi[k]];
            char *p = NULL; int j = 0;
            while ((p = strtok(NULL, " \t")) && j < pc) {
                int si = char_id_map_get(&vm->frames[curi].VarIndexer, p);
                vm->frames[cfi].vars[pi[j++]] = vm->frames[curi].vars[si];
            }
            char cn[VAR_NAME_LENGTH];
            if (is_rec && current_thread_args) {
                make_frame_key_par_rec(pn, new_depth, cn, sizeof(cn));
            } else if (is_rec) {
                make_frame_key(pn, new_depth, cn, sizeof(cn));
            } else if (current_thread_args) {
                make_thread_frame_key(pn, cn, sizeof(cn));
            } else {
                strncpy(cn, pn, sizeof(cn) - 1);
                cn[sizeof(cn) - 1] = '\0';
            }
            int saved_inv = vm->inversion_depth;
            int ss = vm->suppress_show;
            Var *saved_g = vm->invert_hist_guard_var;
            size_t saved_fm = vm->invert_hist_floor_min;
            vm->inversion_depth          = 0;
            vm->invert_hist_guard_var    = NULL;
            vm->suppress_show = 1;
            // #region agent log
            {
                FILE *_uf = fopen("/home/nico/Desktop/mnemo/.cursor/debug-acb76d.log", "a");
                if (_uf) {
                    fprintf(_uf,
                            "{\"sessionId\":\"acb76d\",\"hypothesisId\":\"G\",\"location\":\"invert_uncall_replay\","
                            "\"message\":\"vm_run_BT\",\"data\":{\"parent\":\"%s\",\"callee\":\"%s\","
                            "\"loc_sz\":%d},\"timestamp\":%lld}\n",
                            frame_name, cn, stack_size(&vm->frames[cfi].LocalVariables),
                            (long long)time(NULL) * 1000);
                    fclose(_uf);
                }
            }
            // #endregion
            vm_run_BT(vm, orig, cn);
            vm->inversion_depth       = saved_inv;
            vm->invert_hist_guard_var = saved_g;
            vm->invert_hist_floor_min = saved_fm;
            vm->suppress_show = ss;
            for (int k = 0; k < pc; k++) vm->frames[cfi].vars[pi[k]] = sv[k];
            i--; continue;
        }

        /* PAR_START nell'inversione: rilancia i thread con is_inverse=1,
           in modo che SSEND e SRECV vengano scambiati dentro thread_entry.
           Le istruzioni interne sono già skippate da line_is_inside_par. */
        if (!strcmp(fw, "PAR_START")) {
            if (vm->dbg && vm->dbg->initialized)
                dbg_hook(vm->dbg, invert_extract_srcline(lp[i]), cur_frame, lp[i]);
            /* cur è il numero-riga stampato nel bytecode (es. 52 per "0052 PAR_START").
               Il blocco PAR da scansionare inizia dalla riga successiva
               (THREAD_0), quindi fisicamente cur+1. */
            char *par_ptr = go_to_line(orig, cur + 1);
            if (par_ptr) {
                ParBlock pb = scan_par_block(par_ptr);
                exec_par_threads(vm, orig, cur_frame, &pb, 1, 1);
            }
            i--; continue;
        }

        if (vm->dbg && vm->dbg->initialized) {
            if (!strcmp(fw, "DECL") || !strcmp(fw, "LABEL")) {
                vm->dbg->current_line = invert_extract_srcline(lp[i]);
            } else if (strcmp(fw, "PARAM") != 0) {
                dbg_hook(vm->dbg, invert_extract_srcline(lp[i]), cur_frame, lp[i]);
            }
        }

        if (!strcmp(frame_name, "__mn_divmod_nonneg"))
            VMLOG("[INV_OP] frame='%s' cur=%u op='%s'\n", frame_name, cur, fw);
        if      (!strcmp(fw, "PUSHEQ")) op_pusheq_inv(vm, cur_frame);
        else if (!strcmp(fw, "MINEQ"))  op_mineq_inv (vm, cur_frame);
        else if (!strcmp(fw, "XOREQ"))  op_xoreq_inv (vm, cur_frame);
        else if (!strcmp(fw, "SWAP"))   op_swap_inv  (vm, cur_frame);
        else if (!strcmp(fw, "PUSH"))  op_pop  (vm, cur_frame);
        else if (!strcmp(fw, "POP"))   op_push (vm, cur_frame);
        else if (!strcmp(fw, "SSEND")) op_srecv(vm, cur_frame);
        else if (!strcmp(fw, "SRECV")) op_ssend(vm, cur_frame);
        else if (!strcmp(fw, "LOCAL"))  op_delocal   (vm, cur_frame);
        else if (!strcmp(fw, "DELOCAL"))op_local     (vm, cur_frame);
        else if (!strcmp(fw, "SHOW"))   { /* no-op in inverse */ }
        else if (!strcmp(fw, "START")   || !strcmp(fw, "PARAM")   || !strcmp(fw, "LABEL")   ||
                 !strcmp(fw, "EVAL")    || !strcmp(fw, "JMPF")    || !strcmp(fw, "JMP")     ||
                 !strcmp(fw, "ASSERT")   || !strcmp(fw, "DECL")    || !strcmp(fw, "HALT")    ||
                 strncmp(fw, "THREAD_", 7) == 0) { /* skip */ }
        else { vm_debug_panic("[UNCALL] op sconosciuta: '%s'\n", fw); }
        i--;
    }

    for (int j = 0; j < nl; j++) free(lp[j]);
    // #region agent log
    if (strstr(frame_name, "move_int")) {
        FILE *_df = fopen("/home/nico/Desktop/mnemo/.cursor/debug-acb76d.log", "a");
        if (_df) {
            fprintf(_df,
                    "{\"sessionId\":\"acb76d\",\"hypothesisId\":\"F\",\"location\":\"invert_done\","
                    "\"message\":\"invert_exit\",\"data\":{\"frame\":\"%s\",\"loc_sz\":%d},"
                    "\"timestamp\":%lld}\n",
                    frame_name, stack_size(&vm->frames[fi_reset].LocalVariables),
                    (long long)time(NULL) * 1000);
            fclose(_df);
        }
    }
    // #endregion
    VMLOG("[INVERT] completata, righe processate=%d\n", nl);
    vm->inversion_depth--;
    free(orig);
#undef MAX_LOOPS
#undef MAX_IFS
#undef MAX_LINES
#undef MAX_PARS
}

/* ======================================================================
 *  exec_branch_inverse
 * ====================================================================== */

/* Rami THEN/ELSE corti (solo PUSHEQ/PUSH, nessun from/until e nessun CALL):
 * inversione lineare come prima. Se nel ramo c’è un ciclo o una CALL/UNCALL,
 * serve il loop di invert_op_to_line. */

static inline int branch_span_has_from_loop(char *buf, uint from_line, uint to_line)
{
    char *ptr = go_to_line(buf, from_line);
    if (!ptr) return 0;
    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        uint cur = (uint)atoi(ptr);
        if (cur >= to_line) { *nl = '\n'; break; }
        char lb[2048]; strncpy(lb, ptr, sizeof(lb) - 1);
        lb[sizeof(lb) - 1] = '\0';
        char *fw = strtok(skip_lineno(lb), " \t");
        if (fw) {
            char *a1 = strtok(NULL, " \t");
            if (!strcmp(fw, "LABEL") && a1 && !strncmp(a1, "FROM_", 5)) {
                *nl = '\n'; return 1;
            }
            if (!strcmp(fw, "JMPF") && a1 && !strncmp(a1, "FROM_", 5)) {
                *nl = '\n'; return 1;
            }
        }
        *nl = '\n'; ptr = nl + 1;
    }
    return 0;
}

/* Branch contiene IF nested (JMPF ELSE_*). exec_branch_inverse linear non rispetta
 * if_branch_stack → invertirebbe entrambi rami. Fall back a invert_op_to_line. */
static inline int branch_span_has_nested_if(char *buf, uint from_line, uint to_line)
{
    char *ptr = go_to_line(buf, from_line);
    if (!ptr) return 0;
    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        uint cur = (uint)atoi(ptr);
        if (cur >= to_line) { *nl = '\n'; break; }
        char lb[2048]; strncpy(lb, ptr, sizeof(lb) - 1);
        lb[sizeof(lb) - 1] = '\0';
        char *fw = strtok(skip_lineno(lb), " \t");
        if (fw) {
            char *a1 = strtok(NULL, " \t");
            if (!strcmp(fw, "JMPF") && a1 && !strncmp(a1, "ELSE_", 5)) {
                *nl = '\n'; return 1;
            }
        }
        *nl = '\n'; ptr = nl + 1;
    }
    return 0;
}

static inline int branch_span_has_call(char *buf, uint from_line, uint to_line)
{
    char *ptr = go_to_line(buf, from_line);
    if (!ptr) return 0;
    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        uint cur = (uint)atoi(ptr);
        if (cur >= to_line) { *nl = '\n'; break; }
        char lb[2048]; strncpy(lb, ptr, sizeof(lb) - 1);
        lb[sizeof(lb) - 1] = '\0';
        char *fw = strtok(skip_lineno(lb), " \t");
        if (fw && (!strcmp(fw, "CALL") || !strcmp(fw, "UNCALL"))) {
            *nl = '\n'; return 1;
        }
        *nl = '\n'; ptr = nl + 1;
    }
    return 0;
}

static void exec_branch_inverse(VM *vm, char *original_buffer,
                                const char *frame_name,
                                uint from_line, uint to_line,
                                uint caller_fi)
{
    if (from_line >= to_line) return;

    uint cfi = get_findex(frame_name);
    Var *saved[MAX_VARS]; memcpy(saved, vm->frames[cfi].vars, sizeof(Var *) * MAX_VARS);
    Stack saved_lv = vm->frames[cfi].LocalVariables;
    stack_init(&vm->frames[cfi].LocalVariables);

    for (int p = 0; p < vm->frames[cfi].param_count; p++) {
        int   pidx  = vm->frames[cfi].param_indices[p];
        char *pname = saved[pidx]->name;
        if (char_id_map_exists(&vm->frames[caller_fi].VarIndexer, pname)) {
            int src = char_id_map_get(&vm->frames[caller_fi].VarIndexer, pname);
            vm->frames[cfi].vars[pidx] = vm->frames[caller_fi].vars[src];
        }
    }

    Var *tmp_alloc[MAX_VARS]; memset(tmp_alloc, 0, sizeof(tmp_alloc));
    for (int v = 0; v < vm->frames[cfi].var_count; v++) {
        if (!vm->frames[cfi].vars[v]) {
            vm->frames[cfi].vars[v]        = calloc(1, sizeof(Var));
            vm->frames[cfi].vars[v]->T     = TYPE_INT;
            vm->frames[cfi].vars[v]->value = calloc(1, sizeof(int));
            if (saved[v]) strncpy(vm->frames[cfi].vars[v]->name, saved[v]->name, VAR_NAME_LENGTH - 1);
            tmp_alloc[v] = vm->frames[cfi].vars[v];
        }
    }

    if (branch_span_has_from_loop(original_buffer, from_line, to_line)) {
        if (strstr(frame_name, "divmod_nonneg") &&
            divmod_saved_r_loop_skipped_forward(vm, cfi)) {
            /* forward non entrò nel from q==0: il ramo THEN va saltato */
        } else {
            invert_op_to_line(vm, frame_name, original_buffer, to_line - 1, from_line - 1, 0);
        }
    } else {
        char *lines[512]; int count = 0;
        char *p2 = go_to_line(original_buffer, from_line);
        if (p2 && *p2) {
            while (p2 && *p2 && count < 512) {
                char *nl2 = strchr(p2, '\n'); if (!nl2) break; *nl2 = '\0';
                if ((uint)atoi(p2) >= to_line) { *nl2 = '\n'; break; }
                lines[count++] = strdup(p2);
                *nl2 = '\n'; p2 = nl2 + 1;
            }
        }
        for (int i = count - 1; i >= 0; i--) {
            char ob[2048]; strncpy(ob, lines[i], sizeof(ob) - 1);
            ob[sizeof(ob) - 1] = '\0';
            char *clean = skip_lineno(ob);
            char *fw = strtok(clean, " \t");
            if (!fw) continue;

            if (!strcmp(fw, "CALL")) {
                /* Inverti la procedura chiamata (non-ricorsiva) come fa il loop principale
                   di invert_op_to_line: chiama invert_op_to_line sul callee. */
                vm_if_mark_call();
                char *pn = strtok(NULL, " \t");
                //fprintf(stderr, "[BRANCH_CALL] frame='%s' callee='%s'\n", frame_name, pn ? pn : "NULL");
                char base_cur_c[VAR_NAME_LENGTH];
                strncpy(base_cur_c, frame_name, VAR_NAME_LENGTH - 1);
                base_cur_c[VAR_NAME_LENGTH - 1] = '\0';
                char *at_cur_c = strchr(base_cur_c, '@');
                if (at_cur_c) *at_cur_c = '\0';
                int is_rec_c = (strcmp(pn, base_cur_c) == 0);
                if (is_rec_c) continue; /* ricorsiva: salta (non supportato qui) */
                uint callee_fi_c = current_thread_args ? clone_frame_for_thread(vm, pn)
                                                       : char_id_map_get(&FrameIndexer, pn);
                int  pc_c = vm->frames[callee_fi_c].param_count;
                int *pi_c = vm->frames[callee_fi_c].param_indices;
                Var *sv_c[64];
                for (int k = 0; k < pc_c; k++) sv_c[k] = vm->frames[callee_fi_c].vars[pi_c[k]];
                Stack slv_c = vm->frames[callee_fi_c].LocalVariables;
                stack_init(&vm->frames[callee_fi_c].LocalVariables);
                char *p3c = NULL; int jjc = 0;
                while ((p3c = strtok(NULL, " \t")) && jjc < pc_c) {
                    if (!char_id_map_exists(&vm->frames[cfi].VarIndexer, p3c)) { jjc++; continue; }
                    int si = char_id_map_get(&vm->frames[cfi].VarIndexer, p3c);
                    vm->frames[callee_fi_c].vars[pi_c[jjc++]] = vm->frames[cfi].vars[si];
                }
                /* Sotto thread PAR la chiamata annidata deve usare la chiave threaded
                   (`pn@t<tid>`), altrimenti get_findex risolve sul frame template e i
                   suoi vars non sono linkati → SEGV su `vars[vi]->value` in invert. */
                char target_c[VAR_NAME_LENGTH];
                if (current_thread_args) {
                    make_thread_frame_key(pn, target_c, sizeof(target_c));
                } else {
                    strncpy(target_c, pn, sizeof(target_c) - 1);
                    target_c[sizeof(target_c) - 1] = '\0';
                }
                invert_op_to_line(vm, target_c, original_buffer,
                                  vm->frames[callee_fi_c].end_addr - 1,
                                  vm->frames[callee_fi_c].addr + 1, 1);
                for (int k = 0; k < pc_c; k++) vm->frames[callee_fi_c].vars[pi_c[k]] = sv_c[k];
                vm->frames[callee_fi_c].LocalVariables = slv_c;
                continue;
            }

            if (!strcmp(fw, "UNCALL")) {
                vm_if_mark_call();
                char *pn = strtok(NULL, " \t");
                char base_cur[VAR_NAME_LENGTH];
                strncpy(base_cur, frame_name, VAR_NAME_LENGTH - 1);
                base_cur[VAR_NAME_LENGTH - 1] = '\0';
                char *at_cur = strchr(base_cur, '@');
                if (at_cur) *at_cur = '\0';
                int is_rec = (strcmp(pn, base_cur) == 0);
                int new_depth = 0;
                if (is_rec) {
                    const char *atf = strchr(frame_name, '@');
                    if (atf) {
                        const char *us = strrchr(frame_name, '_');
                        if (us && us > atf) new_depth = atoi(us + 1);
                        else new_depth = atoi(atf + 1);
                    }
                    new_depth++;
                }
                uint callee_fi = is_rec ? clone_frame_for_depth(vm, pn, new_depth)
                                        : (current_thread_args ? clone_frame_for_thread(vm, pn)
                                                               : char_id_map_get(&FrameIndexer, pn));
                int  pc = vm->frames[callee_fi].param_count, *pi = vm->frames[callee_fi].param_indices;
                Var *sv[64]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[callee_fi].vars[pi[k]];
                Stack slv = vm->frames[callee_fi].LocalVariables;
                stack_init(&vm->frames[callee_fi].LocalVariables);
                char *p3 = NULL; int jj = 0;
                while ((p3 = strtok(NULL, " \t")) && jj < pc) {
                    int si = char_id_map_get(&vm->frames[cfi].VarIndexer, p3);
                    vm->frames[callee_fi].vars[pi[jj++]] = vm->frames[cfi].vars[si];
                }
                char cn[VAR_NAME_LENGTH];
                if (is_rec && current_thread_args) {
                    make_frame_key_par_rec(pn, new_depth, cn, sizeof(cn));
                } else if (is_rec) {
                    make_frame_key(pn, new_depth, cn, sizeof(cn));
                } else if (current_thread_args) {
                    make_thread_frame_key(pn, cn, sizeof(cn));
                } else {
                    strncpy(cn, pn, sizeof(cn) - 1);
                    cn[sizeof(cn) - 1] = '\0';
                }
                int saved_inv = vm->inversion_depth;
                int ss = vm->suppress_show;
                Var *saved_g = vm->invert_hist_guard_var;
                size_t saved_fm = vm->invert_hist_floor_min;
                vm->inversion_depth       = 0;
                vm->invert_hist_guard_var = NULL;
                vm->suppress_show = 1;
                vm_run_BT(vm, original_buffer, cn);
                vm->inversion_depth       = saved_inv;
                vm->invert_hist_guard_var = saved_g;
                vm->invert_hist_floor_min = saved_fm;
                vm->suppress_show = ss;
                for (int k = 0; k < pc; k++) vm->frames[callee_fi].vars[pi[k]] = sv[k];
                vm->frames[callee_fi].LocalVariables = slv;
                continue;
            }

            if      (!strcmp(fw, "PUSHEQ")) op_pusheq_inv(vm, frame_name);
            else if (!strcmp(fw, "MINEQ"))  op_mineq_inv (vm, frame_name);
            else if (!strcmp(fw, "XOREQ"))  op_xoreq_inv (vm, frame_name);
            else if (!strcmp(fw, "SWAP"))   op_swap_inv  (vm, frame_name);
            else if (!strcmp(fw, "PUSH"))   op_pop       (vm, frame_name);
            else if (!strcmp(fw, "POP"))    op_push      (vm, frame_name);
            else if (!strcmp(fw, "SSEND"))  op_srecv     (vm, frame_name);
            else if (!strcmp(fw, "SRECV"))  op_ssend     (vm, frame_name);
            else if (!strcmp(fw, "LOCAL"))  op_delocal   (vm, frame_name);
            else if (!strcmp(fw, "DELOCAL"))op_local     (vm, frame_name);
            else if (!strcmp(fw, "SHOW"))   { /* no-op in inverse */ }
        }
        for (int i = 0; i < count; i++) free(lines[i]);
    }

    for (int v = 0; v < vm->frames[cfi].var_count; v++)
        if (tmp_alloc[v] && vm->frames[cfi].vars[v] == tmp_alloc[v]) {
            free(tmp_alloc[v]->value); free(tmp_alloc[v]); vm->frames[cfi].vars[v] = NULL;
        }

    memcpy(vm->frames[cfi].vars, saved, sizeof(Var *) * MAX_VARS);
    vm->frames[cfi].LocalVariables = saved_lv;
}

#endif /* VM_INVERT_H */