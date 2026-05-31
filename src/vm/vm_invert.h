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
#include "mn_native_arith.h"

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
 *  Opcode classification (cache to avoid strcmp chain in hot loop)
 * ====================================================================== */

enum InvOpTag {
    INVOP_UNKNOWN = 0,
    INVOP_PUSHEQ, INVOP_MINEQ, INVOP_XOREQ, INVOP_SWAP, INVOP_MNHALVE, INVOP_MNSPLIT32,
    INVOP_PUSH, INVOP_POP, INVOP_SSEND, INVOP_SRECV,
    INVOP_LOCAL, INVOP_DELOCAL, INVOP_SHOW, INVOP_DUMP,
    INVOP_CALL, INVOP_UNCALL,
    INVOP_PAR_START, INVOP_PAR_END,
    INVOP_JMP, INVOP_JMPF, INVOP_EVAL, INVOP_LABEL, INVOP_ASSERT, INVOP_DECL,
    INVOP_HALT, INVOP_START, INVOP_PARAM, INVOP_THREAD, INVOP_END_PROC,
};

static inline uint8_t classify_op(const char *fw)
{
    if (!fw || !*fw) return INVOP_UNKNOWN;
    switch (fw[0]) {
        case 'P':
            if (!strcmp(fw, "PUSHEQ"))    return INVOP_PUSHEQ;
            if (!strcmp(fw, "PUSH"))      return INVOP_PUSH;
            if (!strcmp(fw, "POP"))       return INVOP_POP;
            if (!strcmp(fw, "PARAM"))     return INVOP_PARAM;
            if (!strcmp(fw, "PAR_START")) return INVOP_PAR_START;
            if (!strcmp(fw, "PAR_END"))   return INVOP_PAR_END;
            break;
        case 'M':
            if (!strcmp(fw, "MINEQ")) return INVOP_MINEQ;
            if (!strcmp(fw, "MNHALVE")) return INVOP_MNHALVE;
            if (!strcmp(fw, "MNSPLIT32")) return INVOP_MNSPLIT32;
            break;
        case 'X': if (!strcmp(fw, "XOREQ")) return INVOP_XOREQ; break;
        case 'S':
            if (!strcmp(fw, "SWAP"))  return INVOP_SWAP;
            if (!strcmp(fw, "SSEND")) return INVOP_SSEND;
            if (!strcmp(fw, "SRECV")) return INVOP_SRECV;
            if (!strcmp(fw, "SHOW"))  return INVOP_SHOW;
            if (!strcmp(fw, "START")) return INVOP_START;
            break;
        case 'L':
            if (!strcmp(fw, "LOCAL")) return INVOP_LOCAL;
            if (!strcmp(fw, "LABEL")) return INVOP_LABEL;
            break;
        case 'D':
            if (!strcmp(fw, "DELOCAL")) return INVOP_DELOCAL;
            if (!strcmp(fw, "DECL"))    return INVOP_DECL;
            if (!strcmp(fw, "DUMP"))    return INVOP_DUMP;
            break;
        case 'C': if (!strcmp(fw, "CALL"))   return INVOP_CALL;   break;
        case 'U': if (!strcmp(fw, "UNCALL")) return INVOP_UNCALL; break;
        case 'J':
            if (!strcmp(fw, "JMPF")) return INVOP_JMPF;
            if (!strcmp(fw, "JMP"))  return INVOP_JMP;
            break;
        case 'E':
            if (!strcmp(fw, "EVAL"))     return INVOP_EVAL;
            if (!strcmp(fw, "END_PROC")) return INVOP_END_PROC;
            break;
        case 'A': if (!strcmp(fw, "ASSERT")) return INVOP_ASSERT; break;
        case 'H': if (!strcmp(fw, "HALT"))   return INVOP_HALT;   break;
        case 'T': if (!strncmp(fw, "THREAD_", 7)) return INVOP_THREAD; break;
    }
    return INVOP_UNKNOWN;
}

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

/* Estrae l'UID da una label tipo "FROM_START_1140" → "1140". Ritorna 0 se non match. */
static inline const char *_loop_label_uid(const char *label, const char *prefix, size_t plen)
{
    if (strncmp(label, prefix, plen) != 0) return NULL;
    if (label[plen] != '_') return NULL;
    return label + plen + 1;
}

static inline int collect_loops(VM *vm, const char *frame_name, char *buf,
                                 LoopDescriptor *out, int max)
{
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi]->addr + 1);
    int n = 0;
    /* Stack di loop aperti per UID. Layout Kairos:
     *   LOCAL → EVAL → JMPF FROM_ERR_<UID> → LABEL FROM_START_<UID> → body
     *   → EVAL → JMPF FROM_START_<UID> → LABEL FROM_END_<UID> → LABEL FROM_ERR_<UID>
     * UID coerente tra JMPF FROM_ERR / LABEL FROM_START / JMPF FROM_START / LABEL FROM_END
     * / LABEL FROM_ERR. Slot aperto da prima reference, chiuso da LABEL FROM_ERR_<UID>.
     */
    int stack_slot[32];
    char stack_uid[32][32];
    int top = -1;

    uint peval = 0; char pid[64] = {0}, pval[64] = {0}, pop[8] = {'=', '=', '\0'};

    /* find_or_open: cerca slot per uid; se non aperto, alloca. */
    #define LOOP_FIND_OR_OPEN(uid, slot_out) do { \
        slot_out = -1; \
        for (int _s = top; _s >= 0; _s--) { \
            if (!strcmp(stack_uid[_s], uid)) { slot_out = stack_slot[_s]; break; } \
        } \
        if (slot_out < 0 && n < max && \
            top + 1 < (int)(sizeof(stack_slot)/sizeof(stack_slot[0]))) { \
            slot_out = n++; \
            memset(&out[slot_out], 0, sizeof(LoopDescriptor)); \
            top++; \
            stack_slot[top] = slot_out; \
            strncpy(stack_uid[top], uid, sizeof(stack_uid[0]) - 1); \
            stack_uid[top][sizeof(stack_uid[0]) - 1] = '\0'; \
        } \
    } while (0)

    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n'); if (!nl) break;
        size_t llen = (size_t)(nl - ptr);
        if (llen >= 16383) llen = 16383;
        char lb[16384]; memcpy(lb, ptr, llen); lb[llen] = '\0';
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { ptr = nl + 1; continue; }

        if (!strcmp(fw, "EVAL")) {
            peval = cur;
            char *a = strtok(NULL, " \t");  /* lhs */
            char *op = strtok(NULL, " \t");
            char rhs[256]; read_rest_of_expr(rhs, sizeof(rhs));
            strncpy(pid,  a   ? a   : "", 63);
            strncpy(pval, rhs,             63);
            _copy_compare_op(pop, op);
        } else if (!strcmp(fw, "LABEL")) {
            char *ln = strtok(NULL, " \t");
            if (!ln) { ptr = nl + 1; continue; }
            const char *uid;
            if ((uid = _loop_label_uid(ln, "FROM_START", 10)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) out[slot].from_start_line = cur;
            } else if ((uid = _loop_label_uid(ln, "FROM_END", 8)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) out[slot].from_end_line = cur;
            } else if ((uid = _loop_label_uid(ln, "FROM_ERR", 8)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) out[slot].from_err_line = cur;
                /* Chiude loop, pop stack. */
                for (int s = top; s >= 0; s--) {
                    if (!strcmp(stack_uid[s], uid)) {
                        for (int k = s; k < top; k++) {
                            stack_slot[k] = stack_slot[k + 1];
                            strncpy(stack_uid[k], stack_uid[k + 1], sizeof(stack_uid[0]) - 1);
                        }
                        top--;
                        break;
                    }
                }
            }
        } else if (!strcmp(fw, "JMPF")) {
            char *ln = strtok(NULL, " \t");
            if (!ln) { ptr = nl + 1; continue; }
            const char *uid;
            if ((uid = _loop_label_uid(ln, "FROM_ERR", 8)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) {
                    out[slot].eval_entry_line = peval;
                    strncpy(out[slot].eval_entry_id,  pid,  63);
                    strncpy(out[slot].eval_entry_val, pval, 63);
                    _copy_compare_op(out[slot].eval_entry_op, pop);
                    out[slot].jmpf_err_line = cur;
                }
            } else if ((uid = _loop_label_uid(ln, "FROM_START", 10)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) {
                    out[slot].eval_exit_line = peval;
                    strncpy(out[slot].eval_exit_id,  pid,  63);
                    strncpy(out[slot].eval_exit_val, pval, 63);
                    _copy_compare_op(out[slot].eval_exit_op, pop);
                    out[slot].jmpf_start_line = cur;
                }
            }
        } else if (!strcmp(fw, "END_PROC")) { break; }
        ptr = nl + 1;
    }
    #undef LOOP_FIND_OR_OPEN
    return n;
}

static inline int collect_ifs(VM *vm, const char *frame_name, char *buf,
                               IfDescriptor *out, int max)
{
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi]->addr + 1);
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
        char *nl = strchr(ptr, '\n');
        if (!nl) break;
        size_t llen = (size_t)(nl - ptr);
        if (llen >= 16383) llen = 16383;
        char lb[16384]; memcpy(lb, ptr, llen); lb[llen] = '\0';
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { ptr = nl + 1; continue; }

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
                    char peekb[16384];
                    memcpy(peekb, nl + 1, (size_t)(n2 - (nl + 1)));
                    peekb[(size_t)(n2 - (nl + 1))] = '\0';
                    char ptmp[16384];
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
                if (top + 1 >= 64 || n >= max) { ptr = nl + 1; continue; }
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
            if (!ln) { ptr = nl + 1; continue; }
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
        } else if (!strcmp(fw, "END_PROC")) {
            break;
        }
        ptr = nl + 1;
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
        char buf[16384];
        strncpy(buf, lp[j], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char scan[16384];
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
        char buf[16384];
        strncpy(buf, lp[j], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char scan[16384];
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
    uint vi = char_id_map_get(&vm->frames[fi]->VarIndexer, id);
    int64_t lval = *(vm->frames[fi]->vars[vi]->value);
    int64_t rval = resolve_expr(vm, fi, val);
    thread_val_IF = eval_cond(lval, op, rval);
}

/* IF su snapshot local (saved_r, ts, …): in inversa il delocal può azzerare la copia
   prima del JMPF; usare il parametro sorgente ancora intatto (a, t, …). */
static inline int64_t invert_if_entry_lval(VM *vm, uint fi, const char *id, int64_t current)
{
    if (!strcmp(id, "saved_r")) {
        if (char_id_map_exists(&vm->frames[fi]->VarIndexer, "saved_r")) {
            uint si = char_id_map_get(&vm->frames[fi]->VarIndexer, "saved_r");
            Var *sv = vm->frames[fi]->vars[si];
            if (sv)
                return *(sv->value);
        }
        if (char_id_map_exists(&vm->frames[fi]->VarIndexer, "a")) {
            uint ai = char_id_map_get(&vm->frames[fi]->VarIndexer, "a");
            return *(vm->frames[fi]->vars[ai]->value);
        }
    }
    if (!strcmp(id, "ts")) {
        if (char_id_map_exists(&vm->frames[fi]->VarIndexer, "ts")) {
            uint tsi = char_id_map_get(&vm->frames[fi]->VarIndexer, "ts");
            Var *tsv = vm->frames[fi]->vars[tsi];
            if (tsv)
                return *(tsv->value);
        }
        if (char_id_map_exists(&vm->frames[fi]->VarIndexer, "t")) {
            uint ti = char_id_map_get(&vm->frames[fi]->VarIndexer, "t");
            return *(vm->frames[fi]->vars[ti]->value);
        }
    }
    return current;
}

static inline void do_eval_if_entry(VM *vm, uint fi, const char *id, const char *op,
                                    const char *val)
{
    /* `id` può essere un letterale numerico (es. `from 0 == 0 loop ...`): in tal caso
       lval = atoi(id), niente lookup in VarIndexer (eviterebbe SEGV). */
    int64_t lval;
    if (id && (id[0] == '-' || (id[0] >= '0' && id[0] <= '9'))) {
        lval = (int64_t)strtoll(id, NULL, 10);
    } else {
        uint vi = char_id_map_get(&vm->frames[fi]->VarIndexer, id);
        lval = invert_if_entry_lval(vm, fi, id, *(vm->frames[fi]->vars[vi]->value));
    }
    int64_t rval = resolve_expr(vm, fi, val);
    thread_val_IF = eval_cond(lval, op, rval);
}

/* `from id == 0`: la guardia d'ingresso vale solo alla prima iterata forward.
   In inversa: ripetere il corpo finché id>0; uscire a JMPF_ERR e JMPF_START quando id<=0. */
static inline int loop_entry_eq_zero_guard(const LoopDescriptor *L, int li)
{
    return !strcmp(L[li].eval_entry_op, "==") && !strcmp(L[li].eval_entry_val, "0");
}

static inline int64_t loop_entry_counter_val(VM *vm, uint fi, const LoopDescriptor *L, int li)
{
    const char *eid = L[li].eval_entry_id;
    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, eid)) return 0;
    uint vi = char_id_map_get(&vm->frames[fi]->VarIndexer, eid);
    return *(vm->frames[fi]->vars[vi]->value);
}

/* __mn_divmod_nonneg: IF saved_r >= b — se forward non entrò nel loop, non invertire il corpo. */
static inline int divmod_saved_r_loop_skipped_forward(VM *vm, uint fi)
{
    int64_t sr = 0;
    if (char_id_map_exists(&vm->frames[fi]->VarIndexer, "saved_r")) {
        uint si = char_id_map_get(&vm->frames[fi]->VarIndexer, "saved_r");
        Var *sv = vm->frames[fi]->vars[si];
        if (sv)
            sr = *(sv->value);
    } else if (char_id_map_exists(&vm->frames[fi]->VarIndexer, "a")) {
        uint ai = char_id_map_get(&vm->frames[fi]->VarIndexer, "a");
        sr = *(vm->frames[fi]->vars[ai]->value);
    } else {
        return 0;
    }
    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, "b"))
        return 0;
    uint bi = char_id_map_get(&vm->frames[fi]->VarIndexer, "b");
    Var *bv = vm->frames[fi]->vars[bi];
    if (!bv || !bv->value)
        return 0;
    return sr < *(bv->value);
}

/* __mn_bit_k_signed: if k == 0 il from i==0 non gira in avanti. */
static inline int bit_k_loop_skipped_forward(VM *vm, uint fi)
{
    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, "k"))
        return 0;
    uint ki = char_id_map_get(&vm->frames[fi]->VarIndexer, "k");
    Var *kv = vm->frames[fi]->vars[ki];
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

#ifdef MNEMO_AGENT_LOG
static inline int mn_dbg_read_int(VM *vm, uint fi, const char *name)
{
    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, name)) return -9999;
    uint vi = char_id_map_get(&vm->frames[fi]->VarIndexer, name);
    Var *v = vm->frames[fi]->vars[vi];
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
#endif


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

/* Range-scoped variant: skip only IFs strictly nested in (rfrom, rto).
 * Usata da invert_op_to_line quando viene invocata da exec_branch_inverse su un
 * sotto-range (es. corpo loop dentro IIf outer wrap di counter-loop): l'IF outer
 * NON va filtrata (è il branch attuale), ma IFs annidate dentro il body sì. */
static inline int line_is_inside_if_subrange(uint line, IfDescriptor *ifs, int nifs,
                                              uint rfrom, uint rto)
{
    for (int i = 0; i < nifs; i++) {
        if (ifs[i].jmpf_else_line <= rfrom) continue;
        if (ifs[i].fi_label_line  >= rto)   continue;
        if (line > ifs[i].jmpf_else_line && line < ifs[i].jmp_fi_line) return 1;
        if (line > ifs[i].else_label_line && line < ifs[i].fi_label_line) return 1;
    }
    return 0;
}

/* Static state per range-scoped skip filter. Set da exec_branch_inverse prima di
 * chiamare invert_op_to_line(honor=0) sul FROM-loop fallback, restored dopo. */
static __thread uint g_invert_nested_filter_from = 0;
static __thread uint g_invert_nested_filter_to   = 0;

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
        char tmp[16384]; strncpy(tmp, ptr, sizeof(tmp) - 1);
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
                char tmp2[16384]; strncpy(tmp2, scan, sizeof(tmp2) - 1);
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
    /* buffer è già thread-locale (vm_par.h:77 dup_buffer per worker, top-level
       vm_run_BT alloca dal chiamante). Mutations qui (newline='\0' poi restore)
       sono transienti → no strdup, riduciamo malloc pressure. */
    char *orig = buffer;
    VMLOG("[INVERT] frame='%s' start=%u stop=%u\n", frame_name, start, stop);
    vm->inversion_depth++;   
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi_reset = char_id_map_get(&FrameIndexer, base);
    #ifdef MNEMO_AGENT_LOG
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
    #endif
    stack_init(&vm->frames[fi_reset]->LocalVariables);

#define MAX_LOOPS 32
#define MAX_IFS   32
#define MAX_LINES 1024
#define MAX_PARS  32

    /* Per-frame analysis cache. collect_loops/ifs/par_ranges scan ~50KB
       bytecode per invocation. Encrypt opt-uncall fa molte UNCALL su
       stesse procedure (divmod, putd, bit_k_signed, …) → cache per
       base name evita N rescan. */
    typedef struct {
        char base[VAR_NAME_LENGTH];
        int nloops, nifs, npars;
        LoopDescriptor loops[MAX_LOOPS];
        IfDescriptor   ifs  [MAX_IFS];
        ParRange       pars [MAX_PARS];
    } FrameAnalysisCache;
    /* Thread-local: due thread par (es. fib_left/fib_right) chiamano
       invert_op_to_line in parallelo; senza __thread la cache (n++ + array
       writes) è una race → descriptor partial-write → DELOCAL `__mn_e<N>`
       value errato a fine inverse. */
    static __thread FrameAnalysisCache _fa_cache[64];
    static __thread int _fa_cache_n = 0;
    LoopDescriptor *loops;
    IfDescriptor   *ifs;
    ParRange       *pars;
    int nloops, nifs, npars;
    int _fa_hit = -1;
    for (int _c = 0; _c < _fa_cache_n; _c++) {
        if (!strcmp(_fa_cache[_c].base, base)) { _fa_hit = _c; break; }
    }
    if (_fa_hit < 0 && _fa_cache_n < (int)(sizeof(_fa_cache)/sizeof(_fa_cache[0]))) {
        _fa_hit = _fa_cache_n++;
        strncpy(_fa_cache[_fa_hit].base, base, VAR_NAME_LENGTH - 1);
        _fa_cache[_fa_hit].base[VAR_NAME_LENGTH - 1] = '\0';
        _fa_cache[_fa_hit].nloops = collect_loops(vm, frame_name, orig,
                                                  _fa_cache[_fa_hit].loops, MAX_LOOPS);
        _fa_cache[_fa_hit].nifs   = collect_ifs  (vm, frame_name, orig,
                                                  _fa_cache[_fa_hit].ifs,   MAX_IFS);
        _fa_cache[_fa_hit].npars  = collect_par_ranges(orig,
                                                       vm->frames[fi_reset]->addr + 1,
                                                       vm->frames[fi_reset]->end_addr,
                                                       _fa_cache[_fa_hit].pars, MAX_PARS);
    }
    if (_fa_hit >= 0) {
        loops  = _fa_cache[_fa_hit].loops;  nloops = _fa_cache[_fa_hit].nloops;
        ifs    = _fa_cache[_fa_hit].ifs;    nifs   = _fa_cache[_fa_hit].nifs;
        pars   = _fa_cache[_fa_hit].pars;   npars  = _fa_cache[_fa_hit].npars;
    } else {
        /* Cache piena: stack arrays di fallback */
        static LoopDescriptor _fb_loops[MAX_LOOPS];
        static IfDescriptor   _fb_ifs  [MAX_IFS];
        static ParRange       _fb_pars [MAX_PARS];
        loops = _fb_loops; ifs = _fb_ifs; pars = _fb_pars;
        nloops = collect_loops(vm, frame_name, orig, loops, MAX_LOOPS);
        nifs   = collect_ifs  (vm, frame_name, orig, ifs,   MAX_IFS);
        npars  = collect_par_ranges(orig, vm->frames[fi_reset]->addr + 1,
                                    vm->frames[fi_reset]->end_addr, pars, MAX_PARS);
    }

    char cur_frame[VAR_NAME_LENGTH]; strncpy(cur_frame, frame_name, VAR_NAME_LENGTH - 1);
    uint fi       = get_findex(cur_frame);
    uint start_ln = vm->frames[fi_reset]->addr + 1;
    (void)start_ln;
    /* Cache: strstr(frame_name, X) chiamato in più punti dell'hot loop interno.
       frame_name è invariante per chiamata di invert_op_to_line. */
    /* Match esatto: NON `divmod_nonneg_fast` (struttura diversa, no saved_r,
     * usa MNHALVE invece di sottrazione lineare). */
    int _is_divmod_nonneg = (!strncmp(frame_name, "__mn_divmod_nonneg", 18) &&
                             strncmp(frame_name, "__mn_divmod_nonneg_fast", 23) != 0);
    int _is_bit_k_signed  = (strstr(frame_name, "bit_k_signed")  != NULL);

    char *lp[MAX_LINES]; uint ln[MAX_LINES]; uint8_t lp_op[MAX_LINES]; int nl = 0;
    /* Arena: one malloc per invert call instead of N strdups.
       Upper bound = size of remaining buffer from stop+1 to END_PROC. */
    size_t _arena_cap = strlen(orig) + 1;
    char  *_arena = (char *)malloc(_arena_cap);
    if (!_arena) vm_debug_panic("[UNCALL] arena malloc fallita\n");
    char  *_arena_p = _arena;
    char *ptr = go_to_line(orig, stop + 1);   // ← CAMBIA: parti da dopo PROC
    while (ptr && *ptr && nl < MAX_LINES) {
        char *newline = strchr(ptr, '\n'); if (!newline) break;
        *newline = '\0';
        uint cur_ln = (uint)atoi(ptr);
        char *op_start = skip_lineno(ptr);
        if (!strncmp(op_start, "END_PROC", 8) &&
            (op_start[8] == ' ' || op_start[8] == '\t' || op_start[8] == '\0')) {
            *newline = '\n'; break;
        }
        if (cur_ln <= start && cur_ln > stop) {
            size_t _line_len = (size_t)(newline - ptr);
            memcpy(_arena_p, ptr, _line_len);
            _arena_p[_line_len] = '\0';
            /* Precompute op_tag al collection: classify_op richiede null-term
               sul fw token. Trova fine-token come space/tab/null. */
            char *_a_op = _arena_p + (op_start - ptr);
            char *_tok_end = _a_op;
            while (*_tok_end && *_tok_end != ' ' && *_tok_end != '\t') _tok_end++;
            char _save_c = *_tok_end;
            *_tok_end = '\0';
            lp_op[nl] = classify_op(_a_op);
            *_tok_end = _save_c;
            lp[nl] = _arena_p;
            ln[nl] = cur_ln;
            _arena_p += _line_len + 1;
            nl++;
        }
        *newline = '\n'; ptr = newline + 1;
    }
    //fprintf(stderr, "[INVERT] start=%u stop=%u righe_raccolte=%d\n", start, stop, nl);
    #ifdef MNEMO_AGENT_LOG
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
    #endif

    int i = nl - 1;
#ifdef MNEMO_AGENT_LOG
    long long _iter_count = 0;
#endif
    while (i >= 0) {
#ifdef MNEMO_AGENT_LOG
        if (++_iter_count % 100000 == 0)
            fprintf(stderr, "[INVERT_LOOP] frame='%s' iter=%lld i=%d nl=%d\n",
                    frame_name, _iter_count, i, nl);
#endif
        uint  cur   = ln[i];
        /* skip_lineno è puro arithmetic, può operare su lp[i] direttamente (immutato). */
        char *clean = skip_lineno(lp[i]);
        size_t _clean_len = strlen(clean);
        /* exe_line: copia indipendente per strtok finale (dopo dispatch early-skip).
           zbuf: copia indipendente per strtok early (fw_cls/arg1_cls). */
        char exe_line[16384];
        if (_clean_len >= sizeof(exe_line)) _clean_len = sizeof(exe_line) - 1;
        memcpy(exe_line, clean, _clean_len); exe_line[_clean_len] = '\0';
        char zbuf[16384];
        memcpy(zbuf, clean, _clean_len); zbuf[_clean_len] = '\0';
        char *fw_cls = strtok(zbuf, " \t");
        char *arg1_cls = strtok(NULL, " \t");
        if (!fw_cls) { i--; continue; }
        uint8_t op_tag = lp_op[i];
        if (!strcmp(frame_name, "__mn_divmod_nonneg"))
            VMLOG("[INV_LOOP] frame='%s' i=%d cur=%u fw='%s'\n", frame_name, i, cur, fw_cls ? fw_cls : "NULL");
        /* Nel ramo THEN già invertito da exec_branch_inverse (honor_if_line_skip=0 lì).
         * Con range globals attivi (FROM-loop fallback su sub-range), filtra IFs
         * annidate dentro (g_from, g_to) ANCHE quando honor=0. */
        if (honor_if_line_skip && line_is_inside_if(cur, ifs, nifs)) { i--; continue; }
        if (!honor_if_line_skip && g_invert_nested_filter_to > g_invert_nested_filter_from &&
            line_is_inside_if_subrange(cur, ifs, nifs,
                                        g_invert_nested_filter_from,
                                        g_invert_nested_filter_to)) { i--; continue; }
        if (line_is_inside_par(cur, pars, npars)) { i--; continue; }
        int li = -1;
        /* fast path: line_loop_zone_for_instr controlla JMPF/LABEL/JMP via arg1.
           Per altri op (XOREQ/PUSHEQ/etc) basta confronto sui line numbers. */
        LoopZone lz;
        if (op_tag == INVOP_JMPF || op_tag == INVOP_LABEL || op_tag == INVOP_JMP) {
            lz = line_loop_zone_for_instr(cur, fw_cls, arg1_cls, loops, nloops, &li);
        } else {
            lz = line_loop_zone(cur, loops, nloops, &li);
        }
        if (lz == LOOP_ZONE_EVAL_ENTRY || lz == LOOP_ZONE_EVAL_EXIT  ||
            lz == LOOP_ZONE_START_LABEL|| lz == LOOP_ZONE_END_LABEL  ||
            lz == LOOP_ZONE_ERR_LABEL)  { i--; continue; }

        if (lz == LOOP_ZONE_JMPF_ERR) {
            if (_is_divmod_nonneg &&
                loop_entry_eq_zero_guard(loops, li) &&
                divmod_saved_r_loop_skipped_forward(vm, fi)) {
                i--;
                continue;
            }
            if (_is_bit_k_signed &&
                loop_entry_eq_zero_guard(loops, li) &&
                !strcmp(loops[li].eval_entry_id, "i") &&
                bit_k_loop_skipped_forward(vm, fi)) {
                i--;
                continue;
            }
            do_eval(vm, fi, loops[li].eval_entry_id, loops[li].eval_entry_op,
                    loops[li].eval_entry_val);
            #ifdef MNEMO_AGENT_LOG
            mn_dbg_log_loop("B", "JMPF_ERR", frame_name, _iter_count, i, nl,
                            (int)thread_val_IF,
                            loop_entry_eq_zero_guard(loops, li)
                                ? loop_entry_counter_val(vm, fi, loops, li)
                                : mn_dbg_read_int(vm, fi, "q"),
                            mn_dbg_read_int(vm, fi, "r"), mn_dbg_read_int(vm, fi, "b"), -1);
            #endif
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
            if (thread_val_IF) {
                i--;
            } else {
                int tt = lp_row_first_jmpf_from_start(loops[li].jmpf_start_line, lp, ln, nl);
                if (tt < 0) { vm_debug_panic("[UNCALL] jmpf_start\n"); }
                i = tt - 1;
            }
            continue;
        }
        if (lz == LOOP_ZONE_JMPF_START) {
            if (_is_divmod_nonneg &&
                loop_entry_eq_zero_guard(loops, li) &&
                divmod_saved_r_loop_skipped_forward(vm, fi)) {
                i--;
                continue;
            }
            if (_is_bit_k_signed &&
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
            #ifdef MNEMO_AGENT_LOG
            mn_dbg_log_loop("A", "JMPF_START", frame_name, _iter_count, i, nl,
                            (int)thread_val_IF, mn_dbg_read_int(vm, fi, "q"),
                            mn_dbg_read_int(vm, fi, "r"), mn_dbg_read_int(vm, fi, "b"),
                            peel_more);
            #endif
            if (!peel_more) { i--; }
            else {
                int t = lp_row_first_eval_at_line(loops[li].eval_exit_line, lp, ln, nl);
                if (t < 0) { vm_debug_panic("[UNCALL] loop_eval_exit\n"); }
                i = t - 1;
            }
            continue;
        }

        int ii = -1;
        IfZone iz;
        if (op_tag == INVOP_JMPF || op_tag == INVOP_LABEL || op_tag == INVOP_JMP) {
            iz = line_if_zone_for_instr(cur, fw_cls, arg1_cls, ifs, nifs, &ii);
        } else {
            iz = line_if_zone(cur, ifs, nifs, &ii);
        }
        if (iz == IF_ZONE_EVAL_ENTRY || iz == IF_ZONE_EVAL_EXIT || iz == IF_ZONE_ELSE_LABEL ||
            iz == IF_ZONE_FI_LABEL   || iz == IF_ZONE_ASSERT    || iz == IF_ZONE_JMP_FI)
            { i--; continue; }

        if (iz == IF_ZONE_JMPF_ELSE) {
            /* Fix P3 trace: legge branch dal clone frame trace window.
             * Forward CALL salva trace_window_start sul clone. Inverse
             * JMPF_ELSE legge window-relative (start+cursor) ed avanza
             * cursor. Attivo solo se proc base matches trace_proc. */
            int trace_path_active = 0;
            if (vm->branch_trace_active > 0) {
                char fb[VAR_NAME_LENGTH];
                strncpy(fb, cur_frame, VAR_NAME_LENGTH - 1);
                fb[VAR_NAME_LENGTH - 1] = '\0';
                char *fb_at = strchr(fb, '@');
                if (fb_at) *fb_at = '\0';
                if (!strcmp(fb, vm->branch_trace_proc)) trace_path_active = 1;
            }
            if (trace_path_active) {
                int win_start = vm->frames[fi]->trace_window_start;
                int win_cursor = vm->frames[fi]->trace_window_cursor;
                int trace_idx = win_start + win_cursor;
                if (trace_idx < vm->branch_trace_top) {
                    int branch_was_then = vm->branch_trace[trace_idx];
                    vm->frames[fi]->trace_window_cursor = win_cursor + 1;
                    uint branch_from = 0, branch_to = 0;
                    if (branch_was_then) {
                        branch_from = ifs[ii].jmpf_else_line + 1;
                        branch_to   = ifs[ii].jmp_fi_line;
                    } else {
                        branch_from = ifs[ii].else_label_line + 1;
                        branch_to   = ifs[ii].fi_label_line;
                    }
                    if (branch_from < branch_to) {
                        Stack sv = vm->frames[fi]->LocalVariables;
                        stack_init(&vm->frames[fi]->LocalVariables);
                        exec_branch_inverse(vm, orig, cur_frame, branch_from, branch_to, fi);
                        vm->frames[fi]->LocalVariables = sv;
                    }
                    int t = -1;
                    for (int j = i - 1; j >= 0; j--) if (ln[j] == ifs[ii].eval_entry_line) { t = j; break; }
                    i = (t >= 0) ? t - 1 : i - 1;
                    continue;
                }
            }
            int depth = vm->frames[fi_reset]->recursion_depth;
            if (depth > 0) {
                /* Recursive procedure: the entry guard can be overwritten by nested calls.
                   Use recorded recursion depth to replay ELSE inversions, then base THEN. */
                for (int d = 0; d < depth; d++) {
                    uint else_from = ifs[ii].else_label_line + 1;
                    uint else_to = ifs[ii].fi_label_line;
                    if (else_from >= else_to) break;
                    Stack sv = vm->frames[fi]->LocalVariables;
                    stack_init(&vm->frames[fi]->LocalVariables);
                    exec_branch_inverse(vm, orig, cur_frame, else_from, else_to, fi);
                    vm->frames[fi]->LocalVariables = sv;
                }
                uint then_from = ifs[ii].jmpf_else_line + 1;
                uint then_to = ifs[ii].jmp_fi_line;
                if (then_from < then_to) {
                    Stack sv = vm->frames[fi]->LocalVariables;
                    stack_init(&vm->frames[fi]->LocalVariables);
                    exec_branch_inverse(vm, orig, cur_frame, then_from, then_to, fi);
                    vm->frames[fi]->LocalVariables = sv;
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
                Stack sv = vm->frames[fi]->LocalVariables;
                stack_init(&vm->frames[fi]->LocalVariables);
                exec_branch_inverse(vm, orig, cur_frame, branch_from, branch_to, fi);
                vm->frames[fi]->LocalVariables = sv;
            }
            int t = -1;
            for (int j = i - 1; j >= 0; j--) if (ln[j] == ifs[ii].eval_entry_line) { t = j; break; }
            i = (t >= 0) ? t - 1 : i - 1;
            continue;
        }

        char *fw = strtok(exe_line, " \t");
        if (!fw) { i--; continue; }

        if (op_tag == INVOP_PAR_END) { i--; continue; }

        if (_is_divmod_nonneg &&
            divmod_saved_r_loop_skipped_forward(vm, fi) && arg1_cls &&
            ((op_tag == INVOP_MINEQ  && !strcmp(arg1_cls, "r")) ||
             (op_tag == INVOP_PUSHEQ && !strcmp(arg1_cls, "q")))) {
            i--;
            continue;
        }
        if (_is_bit_k_signed &&
            bit_k_loop_skipped_forward(vm, fi) && arg1_cls &&
            ((op_tag == INVOP_MINEQ  && !strcmp(arg1_cls, "i")) ||
             (op_tag == INVOP_PUSHEQ && !strcmp(arg1_cls, "i")))) {
            i--;
            continue;
        }

        if (op_tag == INVOP_CALL) {
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
            int  pc = vm->frames[cfi]->param_count, *pi = vm->frames[cfi]->param_indices;
            Var *sv[MAX_PROC_PARAMS]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi]->vars[pi[k]];
            char *p = NULL; int j = 0;
            while ((p = strtok(NULL, " \t")) && j < pc) {
                int si = char_id_map_get(&vm->frames[curi]->VarIndexer, p);
                vm->frames[cfi]->vars[pi[j++]] = vm->frames[curi]->vars[si];
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
            /* Fix P3 trace: pop callee clone trace_window stack, imposta
             * window_start corrente per invert_op_to_line. Save/restore
             * base frame window_state attorno all'invert ricorsivo per
             * non sovrascrivere il context outer. */
            int saved_base_win_start_x = 0;
            int saved_base_win_cursor_x = 0;
            int trace_did_pop_x = 0;
            uint base_fi_x = 0;
            if (vm->branch_trace_active > 0 && vm->frames[cfi]->trace_window_top > 0) {
                char pb_c[VAR_NAME_LENGTH];
                strncpy(pb_c, pn, VAR_NAME_LENGTH - 1);
                pb_c[VAR_NAME_LENGTH - 1] = '\0';
                char *pbc_at = strchr(pb_c, '@');
                if (pbc_at) *pbc_at = '\0';
                if (!strcmp(pb_c, vm->branch_trace_proc)) {
                    int win = vm->frames[cfi]->trace_window_stack[--vm->frames[cfi]->trace_window_top];
                    vm->frames[cfi]->trace_window_start = win;
                    vm->frames[cfi]->trace_window_cursor = 0;
                    base_fi_x = char_id_map_get(&FrameIndexer, pn);
                    saved_base_win_start_x = vm->frames[base_fi_x]->trace_window_start;
                    saved_base_win_cursor_x = vm->frames[base_fi_x]->trace_window_cursor;
                    vm->frames[base_fi_x]->trace_window_start = win;
                    vm->frames[base_fi_x]->trace_window_cursor = 0;
                    trace_did_pop_x = 1;
                }
            }
            /* Forward CALL may have used native O(1) (no __mn_hist pushes). Invert
             * must use the matching native inverse, not full bytecode inversion. */
            if (!mn_native_arith_uncall_inverse(vm, pn, cfi)) {
                invert_op_to_line(vm, target, orig, vm->frames[cfi]->end_addr - 1,
                                  vm->frames[cfi]->addr + 1, 1);
            }
            if (trace_did_pop_x) {
                vm->frames[base_fi_x]->trace_window_start = saved_base_win_start_x;
                vm->frames[base_fi_x]->trace_window_cursor = saved_base_win_cursor_x;
            }
            for (int k = 0; k < pc; k++) vm->frames[cfi]->vars[pi[k]] = sv[k];
            i--; continue;
        }
        if (op_tag == INVOP_UNCALL) {
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
            int  pc = vm->frames[cfi]->param_count, *pi = vm->frames[cfi]->param_indices;
            Var *sv[MAX_PROC_PARAMS]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi]->vars[pi[k]];
            char *p = NULL; int j = 0;
            while ((p = strtok(NULL, " \t")) && j < pc) {
                int si = char_id_map_get(&vm->frames[curi]->VarIndexer, p);
                vm->frames[cfi]->vars[pi[j++]] = vm->frames[curi]->vars[si];
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
            #ifdef MNEMO_AGENT_LOG
            {
                FILE *_uf = fopen("/home/nico/Desktop/mnemo/.cursor/debug-acb76d.log", "a");
                if (_uf) {
                    fprintf(_uf,
                            "{\"sessionId\":\"acb76d\",\"hypothesisId\":\"G\",\"location\":\"invert_uncall_replay\","
                            "\"message\":\"vm_run_BT\",\"data\":{\"parent\":\"%s\",\"callee\":\"%s\","
                            "\"loc_sz\":%d},\"timestamp\":%lld}\n",
                            frame_name, cn, stack_size(&vm->frames[cfi]->LocalVariables),
                            (long long)time(NULL) * 1000);
                    fclose(_uf);
                }
            }
            #endif
            vm_run_BT(vm, orig, cn);
            vm->inversion_depth       = saved_inv;
            vm->invert_hist_guard_var = saved_g;
            vm->invert_hist_floor_min = saved_fm;
            vm->suppress_show = ss;
            for (int k = 0; k < pc; k++) vm->frames[cfi]->vars[pi[k]] = sv[k];
            i--; continue;
        }

        /* PAR_START nell'inversione: rilancia i thread con is_inverse=1,
           in modo che SSEND e SRECV vengano scambiati dentro thread_entry.
           Le istruzioni interne sono già skippate da line_is_inside_par. */
        if (op_tag == INVOP_PAR_START) {
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
            if (op_tag == INVOP_DECL || op_tag == INVOP_LABEL) {
                vm->dbg->current_line = invert_extract_srcline(lp[i]);
            } else if (op_tag != INVOP_PARAM) {
                dbg_hook(vm->dbg, invert_extract_srcline(lp[i]), cur_frame, lp[i]);
            }
        }

        if (!strcmp(frame_name, "__mn_divmod_nonneg"))
            VMLOG("[INV_OP] frame='%s' cur=%u op='%s'\n", frame_name, cur, fw);
        switch (op_tag) {
            case INVOP_PUSHEQ:  op_pusheq_inv(vm, cur_frame); break;
            case INVOP_MINEQ:   op_mineq_inv (vm, cur_frame); break;
            case INVOP_XOREQ:   op_xoreq_inv (vm, cur_frame); break;
            case INVOP_MNHALVE: op_mnhalve_inv(vm, cur_frame); break;
            case INVOP_MNSPLIT32: op_mnsplit32_inv(vm, cur_frame); break;
            case INVOP_SWAP:    op_swap_inv  (vm, cur_frame); break;
            case INVOP_PUSH:    op_pop       (vm, cur_frame); break;
            case INVOP_POP:     op_push      (vm, cur_frame); break;
            case INVOP_SSEND:   op_srecv     (vm, cur_frame); break;
            case INVOP_SRECV:   op_ssend     (vm, cur_frame); break;
            case INVOP_LOCAL:   op_delocal   (vm, cur_frame); break;
            case INVOP_DELOCAL: op_local     (vm, cur_frame); break;
            case INVOP_SHOW:    /* no-op in inverse */ break;
            case INVOP_DUMP:    /* no-op in inverse */ break;
            case INVOP_START: case INVOP_PARAM: case INVOP_LABEL:
            case INVOP_EVAL:  case INVOP_JMPF:  case INVOP_JMP:
            case INVOP_ASSERT: case INVOP_DECL: case INVOP_HALT:
            case INVOP_THREAD: /* skip */ break;
            default: vm_debug_panic("[UNCALL] op sconosciuta: '%s'\n", fw);
        }
        i--;
    }

    free(_arena);
    /* orig non strduped → niente free(orig) */
    #ifdef MNEMO_AGENT_LOG
    if (strstr(frame_name, "move_int")) {
        FILE *_df = fopen("/home/nico/Desktop/mnemo/.cursor/debug-acb76d.log", "a");
        if (_df) {
            fprintf(_df,
                    "{\"sessionId\":\"acb76d\",\"hypothesisId\":\"F\",\"location\":\"invert_done\","
                    "\"message\":\"invert_exit\",\"data\":{\"frame\":\"%s\",\"loc_sz\":%d},"
                    "\"timestamp\":%lld}\n",
                    frame_name, stack_size(&vm->frames[fi_reset]->LocalVariables),
                    (long long)time(NULL) * 1000);
            fclose(_df);
        }
    }
    #endif
    VMLOG("[INVERT] completata, righe processate=%d\n", nl);
    vm->inversion_depth--;
    /* orig = buffer (no strdup), niente free */
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
        char lb[16384]; strncpy(lb, ptr, sizeof(lb) - 1);
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
        char lb[16384]; strncpy(lb, ptr, sizeof(lb) - 1);
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
        char lb[16384]; strncpy(lb, ptr, sizeof(lb) - 1);
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
    Var *saved[MAX_VARS]; memcpy(saved, vm->frames[cfi]->vars, sizeof(Var *) * MAX_VARS);
    Stack saved_lv = vm->frames[cfi]->LocalVariables;
    stack_init(&vm->frames[cfi]->LocalVariables);

    for (int p = 0; p < vm->frames[cfi]->param_count; p++) {
        int   pidx  = vm->frames[cfi]->param_indices[p];
        char *pname = saved[pidx]->name;
        if (char_id_map_exists(&vm->frames[caller_fi]->VarIndexer, pname)) {
            int src = char_id_map_get(&vm->frames[caller_fi]->VarIndexer, pname);
            vm->frames[cfi]->vars[pidx] = vm->frames[caller_fi]->vars[src];
        }
    }

    Var *tmp_alloc[MAX_VARS]; memset(tmp_alloc, 0, sizeof(tmp_alloc));
    for (int v = 0; v < vm->frames[cfi]->var_count; v++) {
        if (!vm->frames[cfi]->vars[v]) {
            vm->frames[cfi]->vars[v]        = calloc(1, sizeof(Var));
            vm->frames[cfi]->vars[v]->T     = TYPE_INT;
            vm->frames[cfi]->vars[v]->value = calloc(1, sizeof(int64_t));
            if (saved[v]) strncpy(vm->frames[cfi]->vars[v]->name, saved[v]->name, VAR_NAME_LENGTH - 1);
            tmp_alloc[v] = vm->frames[cfi]->vars[v];
        }
    }

    if (branch_span_has_from_loop(original_buffer, from_line, to_line)) {
        if (!strncmp(frame_name, "__mn_divmod_nonneg", 18) &&
            strncmp(frame_name, "__mn_divmod_nonneg_fast", 23) != 0 &&
            divmod_saved_r_loop_skipped_forward(vm, cfi)) {
            /* forward non entrò nel from q==0: il ramo THEN va saltato */
        } else {
            /* Attiva range-scoped IF skip: invert_op_to_line(honor=0) altrimenti
             * processa linearmente sia il body del FROM-loop che i corpi degli
             * IF annidati (gestiti via JMPF_ELSE dispatch) → double-processing. */
            uint saved_ff = g_invert_nested_filter_from;
            uint saved_ft = g_invert_nested_filter_to;
            g_invert_nested_filter_from = from_line;
            g_invert_nested_filter_to   = to_line;
            invert_op_to_line(vm, frame_name, original_buffer, to_line - 1, from_line - 1, 0);
            g_invert_nested_filter_from = saved_ff;
            g_invert_nested_filter_to   = saved_ft;
        }
    } else if (branch_span_has_nested_if(original_buffer, from_line, to_line)) {
        /* Branch contiene nested IF: linear-reverse standard processerebbe entrambi
         * THEN+ELSE come atomic ops (non riconosce JMPF/LABEL/EVAL) corrompendo
         * hist. Collect_ifs sull'intero frame, itera righe del branch in reverse,
         * skippa quelle interne a nested IF e dispatch ai loro JMPF ELSE_<uid>
         * con recurse exec_branch_inverse sul ramo che era vero in forward. */
        IfDescriptor   ifs[32];
        int nifs2 = collect_ifs(vm, frame_name, original_buffer, ifs, 32);

        char *lp[512]; uint ln[512]; int nl = 0;
        char *p3 = go_to_line(original_buffer, from_line);
        while (p3 && *p3 && nl < 512) {
            char *nl3 = strchr(p3, '\n'); if (!nl3) break; *nl3 = '\0';
            uint cur_ln = (uint)atoi(p3);
            if (cur_ln >= to_line) { *nl3 = '\n'; break; }
            lp[nl] = strdup(p3); ln[nl] = cur_ln; nl++;
            *nl3 = '\n'; p3 = nl3 + 1;
        }
        int idx = nl - 1;
        while (idx >= 0) {
            uint cur = ln[idx];
            char ob[16384]; strncpy(ob, lp[idx], sizeof(ob) - 1); ob[sizeof(ob) - 1] = '\0';
            char *fw = strtok(skip_lineno(ob), " \t");
            if (!fw) { idx--; continue; }

            /* JMPF ELSE_<uid> di un nested IF interamente nel branch: dispatch.
             * Filtro: jmpf_else > from_line && assert < to_line (IF interno, non sibling). */
            int matched = -1;
            for (int k = 0; k < nifs2; k++) {
                if (ifs[k].jmpf_else_line <= from_line) continue;
                if (ifs[k].assert_line >= to_line) continue;
                if (cur == ifs[k].jmpf_else_line) { matched = k; break; }
            }
            if (matched >= 0) {
                do_eval_if_entry(vm, cfi, ifs[matched].eval_entry_id,
                                 ifs[matched].eval_entry_op, ifs[matched].eval_entry_val);
                uint bf = 0, bt = 0;
                if (thread_val_IF) {
                    bf = ifs[matched].jmpf_else_line + 1;
                    bt = ifs[matched].jmp_fi_line;
                } else {
                    bf = ifs[matched].else_label_line + 1;
                    bt = ifs[matched].fi_label_line;
                }
                if (bf < bt) {
                    Stack sv2 = vm->frames[cfi]->LocalVariables;
                    stack_init(&vm->frames[cfi]->LocalVariables);
                    exec_branch_inverse(vm, original_buffer, frame_name, bf, bt, caller_fi);
                    vm->frames[cfi]->LocalVariables = sv2;
                }
                /* Jump idx a prima dell'EVAL del nested IF. */
                int t = -1;
                for (int j = idx - 1; j >= 0; j--)
                    if (ln[j] == ifs[matched].eval_entry_line) { t = j; break; }
                idx = (t >= 0) ? t - 1 : idx - 1;
                continue;
            }

            /* Cerca se cur è meta-line o body-line di un nested IF: skip. */
            int inside_nested = 0;
            for (int k = 0; k < nifs2; k++) {
                if (ifs[k].jmpf_else_line <= from_line) continue;
                if (ifs[k].assert_line >= to_line) continue;
                if ((cur > ifs[k].jmpf_else_line && cur < ifs[k].jmp_fi_line) ||
                    (cur > ifs[k].else_label_line && cur < ifs[k].fi_label_line)) {
                    inside_nested = 1; break;
                }
                if (cur == ifs[k].jmp_fi_line || cur == ifs[k].else_label_line ||
                    cur == ifs[k].fi_label_line || cur == ifs[k].eval_exit_line ||
                    cur == ifs[k].assert_line  || cur == ifs[k].eval_entry_line) {
                    inside_nested = 1; break;
                }
            }
            if (inside_nested) { idx--; continue; }

            /* Linear inverse della op come prima. */
            if (!strcmp(fw, "CALL")) {
                vm_if_mark_call();
                char *pn = strtok(NULL, " \t");
                char base_cur_c[VAR_NAME_LENGTH];
                strncpy(base_cur_c, frame_name, VAR_NAME_LENGTH - 1);
                base_cur_c[VAR_NAME_LENGTH - 1] = '\0';
                char *at_cur_c = strchr(base_cur_c, '@'); if (at_cur_c) *at_cur_c = '\0';
                int is_rec_c = (strcmp(pn, base_cur_c) == 0);
                int new_depth_c = 0;
                if (is_rec_c) {
                    const char *atf = strchr(frame_name, '@');
                    if (atf) {
                        const char *us = strrchr(frame_name, '_');
                        if (us && us > atf) new_depth_c = atoi(us + 1);
                        else new_depth_c = atoi(atf + 1);
                    }
                    new_depth_c++;
                }
                uint callee_fi_c = is_rec_c ? clone_frame_for_depth(vm, pn, new_depth_c)
                                            : (current_thread_args ? clone_frame_for_thread(vm, pn)
                                                                   : char_id_map_get(&FrameIndexer, pn));
                int pc_c = vm->frames[callee_fi_c]->param_count;
                int *pi_c = vm->frames[callee_fi_c]->param_indices;
                Var *sv_c[64];
                for (int k = 0; k < pc_c; k++) sv_c[k] = vm->frames[callee_fi_c]->vars[pi_c[k]];
                Stack slv_c = vm->frames[callee_fi_c]->LocalVariables;
                stack_init(&vm->frames[callee_fi_c]->LocalVariables);
                char *p3c = NULL; int jjc = 0;
                while ((p3c = strtok(NULL, " \t")) && jjc < pc_c) {
                    if (!char_id_map_exists(&vm->frames[cfi]->VarIndexer, p3c)) { jjc++; continue; }
                    int si = char_id_map_get(&vm->frames[cfi]->VarIndexer, p3c);
                    vm->frames[callee_fi_c]->vars[pi_c[jjc++]] = vm->frames[cfi]->vars[si];
                }
                char target_c[VAR_NAME_LENGTH];
                if (is_rec_c && current_thread_args) make_frame_key_par_rec(pn, new_depth_c, target_c, sizeof(target_c));
                else if (is_rec_c) make_frame_key(pn, new_depth_c, target_c, sizeof(target_c));
                else if (current_thread_args) make_thread_frame_key(pn, target_c, sizeof(target_c));
                else { strncpy(target_c, pn, sizeof(target_c) - 1); target_c[sizeof(target_c) - 1] = '\0'; }
                /* Fix P3 trace: pop callee clone trace_window stack, set
                 * window_start corrente per invert_op_to_line. Save/restore
                 * base attorno per non polluire outer context. */
                int saved_bws_eb = 0, saved_bwc_eb = 0, popped_eb = 0;
                uint base_fi_eb = 0;
                if (vm->branch_trace_active > 0 &&
                    vm->frames[callee_fi_c]->trace_window_top > 0) {
                    char pb_eb[VAR_NAME_LENGTH];
                    strncpy(pb_eb, pn, VAR_NAME_LENGTH - 1);
                    pb_eb[VAR_NAME_LENGTH - 1] = '\0';
                    char *pbeb_at = strchr(pb_eb, '@');
                    if (pbeb_at) *pbeb_at = '\0';
                    if (!strcmp(pb_eb, vm->branch_trace_proc)) {
                        int win = vm->frames[callee_fi_c]->trace_window_stack
                                  [--vm->frames[callee_fi_c]->trace_window_top];
                        vm->frames[callee_fi_c]->trace_window_start = win;
                        vm->frames[callee_fi_c]->trace_window_cursor = 0;
                        base_fi_eb = char_id_map_get(&FrameIndexer, pn);
                        saved_bws_eb = vm->frames[base_fi_eb]->trace_window_start;
                        saved_bwc_eb = vm->frames[base_fi_eb]->trace_window_cursor;
                        vm->frames[base_fi_eb]->trace_window_start = win;
                        vm->frames[base_fi_eb]->trace_window_cursor = 0;
                        popped_eb = 1;
                    }
                }
                invert_op_to_line(vm, target_c, original_buffer,
                                  vm->frames[callee_fi_c]->end_addr - 1,
                                  vm->frames[callee_fi_c]->addr + 1, 1);
                if (popped_eb) {
                    vm->frames[base_fi_eb]->trace_window_start = saved_bws_eb;
                    vm->frames[base_fi_eb]->trace_window_cursor = saved_bwc_eb;
                }
                for (int k = 0; k < pc_c; k++) vm->frames[callee_fi_c]->vars[pi_c[k]] = sv_c[k];
                vm->frames[callee_fi_c]->LocalVariables = slv_c;
            }
            else if (!strcmp(fw, "PUSHEQ")) op_pusheq_inv(vm, frame_name);
            else if (!strcmp(fw, "MINEQ"))  op_mineq_inv (vm, frame_name);
            else if (!strcmp(fw, "XOREQ"))  op_xoreq_inv (vm, frame_name);
            else if (!strcmp(fw, "SWAP"))   op_swap_inv  (vm, frame_name);
            else if (!strcmp(fw, "MNHALVE")) op_mnhalve_inv(vm, frame_name);
            else if (!strcmp(fw, "MNSPLIT32")) op_mnsplit32_inv(vm, frame_name);
            else if (!strcmp(fw, "PUSH"))   op_pop       (vm, frame_name);
            else if (!strcmp(fw, "POP"))    op_push      (vm, frame_name);
            else if (!strcmp(fw, "SSEND"))  op_srecv     (vm, frame_name);
            else if (!strcmp(fw, "SRECV"))  op_ssend     (vm, frame_name);
            else if (!strcmp(fw, "LOCAL"))  op_delocal   (vm, frame_name);
            else if (!strcmp(fw, "DELOCAL"))op_local     (vm, frame_name);
            else if (!strcmp(fw, "SHOW"))   { /* no-op */ }
            idx--;
        }
        for (int j = 0; j < nl; j++) free(lp[j]);
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
            char ob[16384]; strncpy(ob, lines[i], sizeof(ob) - 1);
            ob[sizeof(ob) - 1] = '\0';
            char *clean = skip_lineno(ob);
            char *fw = strtok(clean, " \t");
            if (!fw) continue;

            if (!strcmp(fw, "CALL")) {
                /* Inverti la procedura chiamata (anche ricorsiva): mirror del
                   path in invert_op_to_line per CALL. Per ricorsiva calcola
                   new_depth dal frame_name (@1, @_1, …) e usa clone_frame_for_depth. */
                vm_if_mark_call();
                char *pn = strtok(NULL, " \t");
                char base_cur_c[VAR_NAME_LENGTH];
                strncpy(base_cur_c, frame_name, VAR_NAME_LENGTH - 1);
                base_cur_c[VAR_NAME_LENGTH - 1] = '\0';
                char *at_cur_c = strchr(base_cur_c, '@');
                if (at_cur_c) *at_cur_c = '\0';
                int is_rec_c = (strcmp(pn, base_cur_c) == 0);
                int new_depth_c = 0;
                if (is_rec_c) {
                    const char *atf = strchr(frame_name, '@');
                    if (atf) {
                        const char *us = strrchr(frame_name, '_');
                        if (us && us > atf) new_depth_c = atoi(us + 1);
                        else new_depth_c = atoi(atf + 1);
                    }
                    new_depth_c++;
                }
                uint callee_fi_c = is_rec_c ? clone_frame_for_depth(vm, pn, new_depth_c)
                                            : (current_thread_args ? clone_frame_for_thread(vm, pn)
                                                                   : char_id_map_get(&FrameIndexer, pn));
                int  pc_c = vm->frames[callee_fi_c]->param_count;
                int *pi_c = vm->frames[callee_fi_c]->param_indices;
                Var *sv_c[64];
                for (int k = 0; k < pc_c; k++) sv_c[k] = vm->frames[callee_fi_c]->vars[pi_c[k]];
                Stack slv_c = vm->frames[callee_fi_c]->LocalVariables;
                stack_init(&vm->frames[callee_fi_c]->LocalVariables);
                char *p3c = NULL; int jjc = 0;
                while ((p3c = strtok(NULL, " \t")) && jjc < pc_c) {
                    if (!char_id_map_exists(&vm->frames[cfi]->VarIndexer, p3c)) { jjc++; continue; }
                    int si = char_id_map_get(&vm->frames[cfi]->VarIndexer, p3c);
                    vm->frames[callee_fi_c]->vars[pi_c[jjc++]] = vm->frames[cfi]->vars[si];
                }
                char target_c[VAR_NAME_LENGTH];
                if (is_rec_c && current_thread_args) {
                    make_frame_key_par_rec(pn, new_depth_c, target_c, sizeof(target_c));
                } else if (is_rec_c) {
                    make_frame_key(pn, new_depth_c, target_c, sizeof(target_c));
                } else if (current_thread_args) {
                    make_thread_frame_key(pn, target_c, sizeof(target_c));
                } else {
                    strncpy(target_c, pn, sizeof(target_c) - 1);
                    target_c[sizeof(target_c) - 1] = '\0';
                }
                /* Fix P3 trace: pop callee clone trace_window stack, set
                 * window_start corrente per invert_op_to_line. Save/restore
                 * base. */
                int saved_bws_eb2 = 0, saved_bwc_eb2 = 0, popped_eb2 = 0;
                uint base_fi_eb2 = 0;
                if (vm->branch_trace_active > 0 &&
                    vm->frames[callee_fi_c]->trace_window_top > 0) {
                    char pb_eb2[VAR_NAME_LENGTH];
                    strncpy(pb_eb2, pn, VAR_NAME_LENGTH - 1);
                    pb_eb2[VAR_NAME_LENGTH - 1] = '\0';
                    char *peb2_at = strchr(pb_eb2, '@');
                    if (peb2_at) *peb2_at = '\0';
                    if (!strcmp(pb_eb2, vm->branch_trace_proc)) {
                        int win = vm->frames[callee_fi_c]->trace_window_stack
                                  [--vm->frames[callee_fi_c]->trace_window_top];
                        vm->frames[callee_fi_c]->trace_window_start = win;
                        vm->frames[callee_fi_c]->trace_window_cursor = 0;
                        base_fi_eb2 = char_id_map_get(&FrameIndexer, pn);
                        saved_bws_eb2 = vm->frames[base_fi_eb2]->trace_window_start;
                        saved_bwc_eb2 = vm->frames[base_fi_eb2]->trace_window_cursor;
                        vm->frames[base_fi_eb2]->trace_window_start = win;
                        vm->frames[base_fi_eb2]->trace_window_cursor = 0;
                        popped_eb2 = 1;
                    }
                }
                invert_op_to_line(vm, target_c, original_buffer,
                                  vm->frames[callee_fi_c]->end_addr - 1,
                                  vm->frames[callee_fi_c]->addr + 1, 1);
                if (popped_eb2) {
                    vm->frames[base_fi_eb2]->trace_window_start = saved_bws_eb2;
                    vm->frames[base_fi_eb2]->trace_window_cursor = saved_bwc_eb2;
                }
                for (int k = 0; k < pc_c; k++) vm->frames[callee_fi_c]->vars[pi_c[k]] = sv_c[k];
                vm->frames[callee_fi_c]->LocalVariables = slv_c;
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
                int  pc = vm->frames[callee_fi]->param_count, *pi = vm->frames[callee_fi]->param_indices;
                Var *sv[MAX_PROC_PARAMS]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[callee_fi]->vars[pi[k]];
                Stack slv = vm->frames[callee_fi]->LocalVariables;
                stack_init(&vm->frames[callee_fi]->LocalVariables);
                char *p3 = NULL; int jj = 0;
                while ((p3 = strtok(NULL, " \t")) && jj < pc) {
                    int si = char_id_map_get(&vm->frames[cfi]->VarIndexer, p3);
                    vm->frames[callee_fi]->vars[pi[jj++]] = vm->frames[cfi]->vars[si];
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
                for (int k = 0; k < pc; k++) vm->frames[callee_fi]->vars[pi[k]] = sv[k];
                vm->frames[callee_fi]->LocalVariables = slv;
                continue;
            }

            if      (!strcmp(fw, "PUSHEQ")) op_pusheq_inv(vm, frame_name);
            else if (!strcmp(fw, "MINEQ"))  op_mineq_inv (vm, frame_name);
            else if (!strcmp(fw, "XOREQ"))  op_xoreq_inv (vm, frame_name);
            else if (!strcmp(fw, "SWAP"))   op_swap_inv  (vm, frame_name);
            else if (!strcmp(fw, "MNHALVE")) op_mnhalve_inv(vm, frame_name);
            else if (!strcmp(fw, "MNSPLIT32")) op_mnsplit32_inv(vm, frame_name);
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

    for (int v = 0; v < vm->frames[cfi]->var_count; v++)
        if (tmp_alloc[v] && vm->frames[cfi]->vars[v] == tmp_alloc[v]) {
            free(tmp_alloc[v]->value); free(tmp_alloc[v]); vm->frames[cfi]->vars[v] = NULL;
        }

    memcpy(vm->frames[cfi]->vars, saved, sizeof(Var *) * MAX_VARS);
    vm->frames[cfi]->LocalVariables = saved_lv;
}

#endif /* VM_INVERT_H */