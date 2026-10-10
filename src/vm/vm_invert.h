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

/* Le guardie (operandi di EVAL) sono stringhe sull'heap della loro lunghezza:
 * nessun limite sulla lunghezza di una guardia. NULL vale "" (vm_name_get).
 * Le libera loop_descs_free / if_descs_free. L'operatore resta in un campo di 8
 * byte: e' uno dei sei confronti che il compilatore emette (==, !=, <, ...). */
typedef struct {
    uint eval_entry_line;
    char *eval_entry_id, *eval_entry_val;
    char eval_entry_op[8]; /* bytecode EVAL memorizza !=, <, … — obbligatorio per invert */
    uint jmpf_err_line, from_start_line, from_end_line, from_err_line;
    uint eval_exit_line;
    char *eval_exit_id, *eval_exit_val;
    char eval_exit_op[8];
    uint jmpf_start_line;
    /* Ciclo a DUE corpi (`from b1 do c1 loop c2 until b2`): valorizzati solo in
     * quel layout, restano 0 per il ciclo a un corpo. from_back_line != 0 e' il
     * discriminante fra i due layout a runtime. */
    uint from_back_line;   /* LABEL FROM_BACK_<uid>  (testa di c2) */
    uint jmp_start_line;   /* JMP  FROM_START_<uid>  (salta c2 al primo giro) */
} LoopDescriptor;

typedef struct {
    uint eval_entry_line;
    char *eval_entry_id, *eval_entry_val;
    char eval_entry_op[8];
    uint jmpf_else_line, jmp_fi_line, else_label_line, fi_label_line;
    uint eval_exit_line;
    char *eval_exit_id, *eval_exit_val;
    char eval_exit_op[8];
    uint assert_line;
} IfDescriptor;

static inline void loop_descs_free(LoopDescriptor *L, int n)
{
    for (int i = 0; i < n; i++) {
        free(L[i].eval_entry_id); free(L[i].eval_entry_val);
        free(L[i].eval_exit_id);  free(L[i].eval_exit_val);
    }
}

static inline void if_descs_free(IfDescriptor *I, int n)
{
    for (int i = 0; i < n; i++) {
        free(I[i].eval_entry_id); free(I[i].eval_entry_val);
        free(I[i].eval_exit_id);  free(I[i].eval_exit_val);
    }
}

typedef enum {
    LOOP_ZONE_NONE, LOOP_ZONE_EVAL_ENTRY, LOOP_ZONE_JMPF_ERR,
    LOOP_ZONE_START_LABEL, LOOP_ZONE_EVAL_EXIT, LOOP_ZONE_JMPF_START,
    LOOP_ZONE_END_LABEL, LOOP_ZONE_ERR_LABEL,
    LOOP_ZONE_BACK_LABEL, LOOP_ZONE_JMP_START   /* solo layout a due corpi */
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
    INVOP_PUSHEQ, INVOP_MINEQ, INVOP_XOREQ, INVOP_SWAP,
    INVOP_PUSH, INVOP_POP, INVOP_SSEND, INVOP_SRECV,
    INVOP_LOCAL, INVOP_DELOCAL, INVOP_SHOW,
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

/* Copia di una guardia nel campo di un descrittore (o nel temporaneo di
 * collect_*), della sua lunghezza. */
static inline void guard_copy(char **dst, const char *src)
{
    char *n = char_id_strdup(src ? src : "");
    free(*dst);
    *dst = n;
}

/* Raccoglie i from-loop della procedura in un vettore che cresce: nessun
 * numero massimo di cicli per procedura né di livelli di annidamento (prima
 * 32 e 32, oltre i quali i cicli in più venivano ignorati in silenzio e
 * l'inversione sbagliava). *outp va liberato con loop_descs_free + free. */
static inline int collect_loops(VM *vm, const char *frame_name, char *buf,
                                 LoopDescriptor **outp)
{
    VM_FRAME_BASE(base, frame_name);
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi]->addr + 1);
    int n = 0, out_cap = 0;
    LoopDescriptor *out = NULL;
    /* Stack di loop aperti per UID. Due layout Kairos.
     *
     * (a) UN corpo — `from b1 do c1 loop until b2`, c2 vuoto:
     *   LOCAL → EVAL → JMPF FROM_ERR_<UID> → LABEL FROM_START_<UID> → c1
     *   → EVAL → JMPF FROM_START_<UID> → LABEL FROM_END_<UID> → LABEL FROM_ERR_<UID>
     *
     * (b) DUE corpi — `from b1 do c1 loop c2 until b2`, traccia c1 [c2 c1]*:
     *   LOCAL → EVAL → JMPF FROM_ERR_<UID> → JMP FROM_START_<UID>
     *   → LABEL FROM_BACK_<UID> → c2 → LABEL FROM_START_<UID> → c1
     *   → EVAL → JMPF FROM_BACK_<UID> → LABEL FROM_END_<UID> → LABEL FROM_ERR_<UID>
     *   Il JMP iniziale salta c2 al primo giro; il back-edge (JMPF FROM_BACK) ci rientra.
     *   from_back_line/jmp_start_line sono valorizzati SOLO qui: 0 nel layout (a).
     *
     * UID coerente tra JMPF FROM_ERR / JMP FROM_START / LABEL FROM_BACK / LABEL FROM_START
     * / JMPF FROM_START|FROM_BACK / LABEL FROM_END / LABEL FROM_ERR. Slot aperto da prima
     * reference, chiuso da LABEL FROM_ERR_<UID>.
     */
    int    stack_cap  = 0;
    int   *stack_slot = NULL;
    char **stack_uid  = NULL;
    int top = -1;

    uint peval = 0; char *pid = NULL, *pval = NULL; char pop[8] = {'=', '=', '\0'};

    /* find_or_open: cerca slot per uid; se non aperto, alloca. */
    #define LOOP_FIND_OR_OPEN(uid, slot_out) do { \
        slot_out = -1; \
        for (int _s = top; _s >= 0; _s--) { \
            if (!strcmp(stack_uid[_s], uid)) { slot_out = stack_slot[_s]; break; } \
        } \
        if (slot_out < 0) { \
            if (n == out_cap) { \
                out_cap = out_cap ? out_cap * 2 : 8; \
                out = (LoopDescriptor *)realloc(out, sizeof(LoopDescriptor) * (size_t)out_cap); \
                if (!out) vm_debug_panic("[VM] collect_loops: memoria esaurita\n"); \
            } \
            if (top + 1 == stack_cap) { \
                stack_cap = stack_cap ? stack_cap * 2 : 8; \
                stack_slot = (int *)realloc(stack_slot, sizeof(int) * (size_t)stack_cap); \
                stack_uid  = (char **)realloc(stack_uid, sizeof(char *) * (size_t)stack_cap); \
                if (!stack_slot || !stack_uid) vm_debug_panic("[VM] collect_loops: memoria esaurita\n"); \
            } \
            slot_out = n++; \
            memset(&out[slot_out], 0, sizeof(LoopDescriptor)); \
            top++; \
            stack_slot[top] = slot_out; \
            stack_uid[top]  = char_id_strdup(uid); \
        } \
    } while (0)

    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break;
        size_t llen = (size_t)(nl - ptr);
        char lb[llen + 1]; memcpy(lb, ptr, llen); lb[llen] = '\0';
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { ptr = nl + 1; continue; }

        if (!strcmp(fw, "EVAL")) {
            peval = cur;
            char *a = strtok(NULL, " \t");  /* lhs */
            char *op = strtok(NULL, " \t");
            VM_REST_EXPR(rhs);
            guard_copy(&pid, a   ? a   : "");
            guard_copy(&pval, rhs);
            _copy_compare_op(pop, op);
        } else if (!strcmp(fw, "LABEL")) {
            char *ln = strtok(NULL, " \t");
            if (!ln) { ptr = nl + 1; continue; }
            const char *uid;
            if ((uid = _loop_label_uid(ln, "FROM_START", 10)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) out[slot].from_start_line = cur;
            } else if ((uid = _loop_label_uid(ln, "FROM_BACK", 9)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) out[slot].from_back_line = cur;
            } else if ((uid = _loop_label_uid(ln, "FROM_END", 8)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) out[slot].from_end_line = cur;
            } else if ((uid = _loop_label_uid(ln, "FROM_ERR", 8)) != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) out[slot].from_err_line = cur;
                /* Chiude loop, pop stack. */
                for (int s = top; s >= 0; s--) {
                    if (!strcmp(stack_uid[s], uid)) {
                        free(stack_uid[s]);
                        for (int k = s; k < top; k++) {
                            stack_slot[k] = stack_slot[k + 1];
                            stack_uid[k]  = stack_uid[k + 1];
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
                    guard_copy(&out[slot].eval_entry_id, pid);
                    guard_copy(&out[slot].eval_entry_val, pval);
                    _copy_compare_op(out[slot].eval_entry_op, pop);
                    out[slot].jmpf_err_line = cur;
                }
            } else if ((uid = _loop_label_uid(ln, "FROM_START", 10)) != NULL ||
                       (uid = _loop_label_uid(ln, "FROM_BACK",   9)) != NULL) {
                /* Test d'uscita: JMPF FROM_START (un corpo) o JMPF FROM_BACK (due corpi). */
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) {
                    out[slot].eval_exit_line = peval;
                    guard_copy(&out[slot].eval_exit_id, pid);
                    guard_copy(&out[slot].eval_exit_val, pval);
                    _copy_compare_op(out[slot].eval_exit_op, pop);
                    out[slot].jmpf_start_line = cur;
                }
            }
        } else if (!strcmp(fw, "JMP")) {
            /* Solo il layout a due corpi ha un JMP FROM_START_<uid> (salta c2 al primo giro). */
            char *ln = strtok(NULL, " \t");
            if (!ln) { ptr = nl + 1; continue; }
            const char *uid = _loop_label_uid(ln, "FROM_START", 10);
            if (uid != NULL) {
                int slot; LOOP_FIND_OR_OPEN(uid, slot);
                if (slot >= 0) out[slot].jmp_start_line = cur;
            }
        } else if (!strcmp(fw, "END_PROC")) { break; }
        ptr = nl + 1;
    }
    #undef LOOP_FIND_OR_OPEN
    for (int k = 0; k <= top; k++) free(stack_uid[k]);
    free(stack_slot); free(stack_uid);
    free(pid); free(pval);
    *outp = out;
    return n;
}

static inline int collect_ifs(VM *vm, const char *frame_name, char *buf,
                               IfDescriptor *out, int max)
{
    VM_FRAME_BASE(base, frame_name);
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi]->addr + 1);
    int n = 0;

    /* Stack di IF aperti — match label per uid (ELSE_<uid>, FI_<uid>) per gestire
     * nested IF (es. loop con IF di guard dentro body). Top dello stack =
     * IF correntemente in costruzione; chiuso da ASSERT (con sentinel handling
     * per EVAL-FI seguita o no da ASSERT).
     *
     * Heap-alloc a capacità `max`: la profondità di annidamento di una else-if
     * chain (`if i==0 else if i==1 …`) può essere lunga quanto il programma:
     * con stack fissi [64] una catena più lunga scriveva OOB → corruzione → NULL
     * deref in resolve_atom. depth ≤ #IF aperti ≤ n < max, quindi `max` slot bastano. */
    int    stack_cap          = max > 0 ? max : 1;
    int   *stack_idx          = malloc(sizeof(*stack_idx) * (size_t)stack_cap);
    char **stack_uid          = calloc((size_t)stack_cap, sizeof(char *));
    int   *stack_eval_exit_set= malloc(sizeof(*stack_eval_exit_set) * (size_t)stack_cap);
    int   top = -1;

    uint peval = 0; char *pid = NULL, *pval = NULL;
    char pfi_op[8] = {'=', '=', '\0'};

    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n');
        if (!nl) break;
        size_t llen = (size_t)(nl - ptr);
        char lb[llen + 1]; memcpy(lb, ptr, llen); lb[llen] = '\0';
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { ptr = nl + 1; continue; }

        if (!strcmp(fw, "EVAL")) {
            peval = cur;
            char *a = strtok(NULL, " \t");  /* lhs */
            char *iop = strtok(NULL, " \t");
            VM_REST_EXPR(rhs);
            guard_copy(&pid, a   ? a   : "");
            guard_copy(&pval, rhs);
            _copy_compare_op(pfi_op, iop);

            /* EVAL FI: se top dello stack è in_then-completato (jmp_fi visto) e
             * aspettiamo l'EVAL/ASSERT di chiusura, registra eval_exit qui. */
            if (top >= 0 && out[stack_idx[top]].fi_label_line && !stack_eval_exit_set[top]) {
                int ti = stack_idx[top];
                out[ti].eval_exit_line = cur;
                guard_copy(&out[ti].eval_exit_id, pid);
                guard_copy(&out[ti].eval_exit_val, pval);
                _copy_compare_op(out[ti].eval_exit_op, pfi_op);
                stack_eval_exit_set[top] = 1;

                /* Peek rigo dopo: se ASSERT, sentinel; altrimenti EVAL=ASSERT collassati. */
                char *n2 = strchr(nl + 1, '\n');
                int  nx_asrt = 0;
                if (n2) {
                    size_t peek_len = (size_t)(n2 - (nl + 1));
                    char ptmp[peek_len + 1];
                    memcpy(ptmp, nl + 1, peek_len);
                    ptmp[peek_len] = '\0';
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
                if (top + 1 >= stack_cap || n >= max) { ptr = nl + 1; continue; }
                int idx = n++;
                top++;
                stack_idx[top] = idx;
                guard_copy(&stack_uid[top], ln + 5);
                stack_eval_exit_set[top] = 0;
                memset(&out[idx], 0, sizeof(IfDescriptor));
                out[idx].eval_entry_line = peval;
                guard_copy(&out[idx].eval_entry_id, pid);
                guard_copy(&out[idx].eval_entry_val, pval);
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
                    guard_copy(&out[ti].eval_exit_id, pid);
                    guard_copy(&out[ti].eval_exit_val, pval);
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
    free(stack_idx);
    for (int k = 0; k < stack_cap; k++) free(stack_uid[k]);
    free(stack_uid);
    free(stack_eval_exit_set);
    free(pid); free(pval);
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
                if ((!strncmp(arg1, "FROM_START", 10) || !strncmp(arg1, "FROM_BACK", 9)) &&
                    line == L[i].jmpf_start_line) {
                    *idx = i; return LOOP_ZONE_JMPF_START;
                }
            }
            /* Layout a due corpi: JMP FROM_START_<uid> è il salto d'ingresso che
             * scavalca c2 al primo giro. All'indietro è solo un no-op. */
            if (!strcmp(fw, "JMP") && !strncmp(arg1, "FROM_START", 10) &&
                L[i].jmp_start_line && line == L[i].jmp_start_line) {
                *idx = i; return LOOP_ZONE_JMP_START;
            }
            if (!strcmp(fw, "LABEL")) {
                if (!strncmp(arg1, "FROM_BACK", 9) &&
                    L[i].from_back_line && line == L[i].from_back_line) {
                    *idx = i; return LOOP_ZONE_BACK_LABEL;
                }
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
        VM_LINE_COPY(buf, lp[j]);
        VM_LINE_COPY(scan, skip_lineno(buf));
        char *ff = strtok(scan, " \t");
        char *a1 = strtok(NULL, " \t");
        /* FROM_BACK: stesso ruolo (test d'uscita) nel layout a due corpi. */
        if (ff && !strcmp(ff, "JMPF") && a1 &&
            (!strncmp(a1, "FROM_START", 10) || !strncmp(a1, "FROM_BACK", 9))) return j;
    }
    return -1;
}

/* Riga del `JMPF FROM_ERR_<uid>`: chiusura del loop in inversa (layout a due corpi). */
static inline int lp_row_first_jmpf_from_err(uint line, char **lp, uint *ln, int nl)
{
    for (int j = 0; j < nl; j++) {
        if (ln[j] != line) continue;
        VM_LINE_COPY(buf, lp[j]);
        VM_LINE_COPY(scan, skip_lineno(buf));
        char *ff = strtok(scan, " \t");
        char *a1 = strtok(NULL, " \t");
        if (ff && !strcmp(ff, "JMPF") && a1 && !strncmp(a1, "FROM_ERR", 8)) return j;
    }
    return -1;
}

static inline int lp_row_first_eval_at_line(uint line, char **lp, uint *ln, int nl)
{
    for (int j = 0; j < nl; j++) {
        if (ln[j] != line) continue;
        VM_LINE_COPY(buf, lp[j]);
        VM_LINE_COPY(scan, skip_lineno(buf));
        char *ff = strtok(scan, " \t");
        if (ff && !strcmp(ff, "EVAL")) return j;
    }
    return -1;
}

static inline void do_eval(VM *vm, uint fi, const char *id, const char *op,
                           const char *val)
{
    int64_t lval;
    if (strchr(id, '[') || id[0] == '(') {
        lval = resolve_expr(vm, fi, id);         /* cella di array o espressione */
    } else {
        uint vi = char_id_map_get(&vm->frames[fi]->VarIndexer, id);
        lval = *(vm->frames[fi]->vars[vi]->value);
    }
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
    } else if (id && (strchr(id, '[') || id[0] == '(')) {
        lval = resolve_expr(vm, fi, id);         /* cella di array o espressione */
    } else {
        /* Var fuori scope (slot delocal'd ma ancora nell'indexer) → lval=0
         * invece di NULL-deref. Succede invertendo la guardia di un IF il cui
         * id è un local già delocal'd a questo punto della reverse-walk (es.
         * `if lc != 0` wrapper di un loop, con disj-chain profonda che sfasa
         * l'ordine). Meglio un eval prudente (→ eventuale DELOCAL mismatch
         * pulito) che un SIGSEGV. */
        int vil = char_id_map_lookup(&vm->frames[fi]->VarIndexer, id);
        Var *gv = (vil >= 0) ? vm->frames[fi]->vars[vil] : NULL;
        if (gv && gv->value)
            lval = invert_if_entry_lval(vm, fi, id, *(gv->value));
        else
            lval = invert_if_entry_lval(vm, fi, id, 0);
    }
    int64_t rval = resolve_expr(vm, fi, val);
    thread_val_IF = eval_cond(lval, op, rval);
}

/* Quale ramo aveva preso un IF, deciso all'indietro.
 *
 * Andando all'indietro lo store e' nello stato DOPO l'if, quindi la guardia
 * d'ingresso non vale piu': rivalutarla da' la risposta giusta solo quando il
 * corpo non tocca cio' che essa testa. E' invece l'asserzione d'uscita a dire,
 * per costruzione, quale ramo e' stato preso: e' la ragione per cui il
 * condizionale reversibile ne ha due. Si usa quella, e si ricade sull'ingresso
 * solo se l'asserzione non e' stata raccolta.
 *
 * Dopo la chiamata thread_val_IF vale 1 se il ramo preso era il THEN. */
static inline void do_eval_if_branch(VM *vm, uint fi, const IfDescriptor *d)
{
    if (vm_name_get(d->eval_exit_id)[0] != '\0')
        do_eval_if_entry(vm, fi, vm_name_get(d->eval_exit_id), d->eval_exit_op, vm_name_get(d->eval_exit_val));
    else
        do_eval_if_entry(vm, fi, vm_name_get(d->eval_entry_id), d->eval_entry_op, vm_name_get(d->eval_entry_val));
}

/* Il condizionale reversibile ha due guardie e la semantica ne chiede due
 * controlli, uno per verso. In avanti si controlla l'asserzione d'uscita
 * (op_assert, "IF/FI non reversibile"). All'indietro l'asserzione d'uscita
 * sceglie il ramo, e resta da verificare che la guardia d'ingresso sia coerente
 * con la scelta.
 *
 * Senza questa verifica l'inverso e' definito su piu' stati di quanti il
 * diretto ne produca: dato uno stato fuori dall'immagine risponde in silenzio
 * invece di rifiutarlo. Ne segue che l'idioma con cui si scrive un'asserzione
 * (`local ok = 0 / if C then ok += 1 fi ok == 1 / delocal ok = 1`) vale solo in
 * avanti, e all'indietro non controlla nulla: e' la ragione per cui il
 * controllo c'e' e non e' opzionale.
 *
 * Va chiamata dopo aver invertito il ramo, quando lo store e' tornato allo
 * stato che precedeva l'if e la guardia d'ingresso torna quindi valutabile. */
static inline void check_if_entry_inverse(VM *vm, uint fi,
                                          const IfDescriptor *d, int took_then)
{
    /* Senza asserzione d'uscita il ramo e' stato dedotto dalla guardia stessa:
     * riverificarla sarebbe una tautologia. */
    if (vm_name_get(d->eval_exit_id)[0] == '\0' || vm_name_get(d->eval_entry_id)[0] == '\0') return;
    /* Nei rami di par altri thread possono mutare gli int condivisi fra la
     * valutazione e il controllo, come gia' fa op_assert in avanti. */
    if (current_thread_args != NULL) return;

    int64_t lval = resolve_value(vm, fi, vm_name_get(d->eval_entry_id));
    int64_t rval = resolve_value(vm, fi, vm_name_get(d->eval_entry_val));
    int guardia = eval_cond(lval, d->eval_entry_op, rval) ? 1 : 0;
    if (guardia != took_then) {
        vm_debug_panic(
            "[VM] IF inverso: l'asserzione d'uscita indica il ramo %s, ma la "
            "guardia d'ingresso (%s %s %s) vale %d: stato fuori dall'immagine "
            "del diretto\n",
            took_then ? "then" : "else",
            vm_name_get(d->eval_entry_id), d->eval_entry_op, vm_name_get(d->eval_entry_val), guardia);
    }
}

/* `from id == 0`: la guardia d'ingresso vale solo alla prima iterata forward.
   In inversa: ripetere il corpo finché id>0; uscire a JMPF_ERR e JMPF_START quando id<=0. */
static inline int loop_entry_eq_zero_guard(const LoopDescriptor *L, int li)
{
    return !strcmp(L[li].eval_entry_op, "==") && !strcmp(vm_name_get(L[li].eval_entry_val), "0");
}

static inline int64_t loop_entry_counter_val(VM *vm, uint fi, const LoopDescriptor *L, int li)
{
    const char *eid = vm_name_get(L[li].eval_entry_id);
    if (strchr(eid, '[') || eid[0] == '(') return resolve_value(vm, fi, eid);
    if (!char_id_map_exists(&vm->frames[fi]->VarIndexer, eid)) return 0;
    uint vi = char_id_map_get(&vm->frames[fi]->VarIndexer, eid);
    return *(vm->frames[fi]->vars[vi]->value);
}

static inline int loop_peel_more_at_until(VM *vm, uint fi, LoopDescriptor *L, int li,
                                          int exit_cond_true)
{
    if (loop_entry_eq_zero_guard(L, li))
        return loop_entry_counter_val(vm, fi, L, li) > 0;
    return exit_cond_true;
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

/* Vettore che cresce: nessun numero massimo di par per procedura (prima 32,
 * oltre i quali le istruzioni dei par in più venivano eseguite due volte
 * all'indietro). *outp va liberato con free. */
static inline int collect_par_ranges(char *buf, uint proc_start, uint proc_end,
                                     ParRange **outp)
{
    int   n   = 0, cap = 0;
    ParRange *out = NULL;
    char *ptr = go_to_line(buf, proc_start);
    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        uint cur = (uint)atoi(ptr);
        if (cur >= proc_end) { *nl = '\n'; break; }
        VM_LINE_COPY(tmp, ptr);
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw && !strcmp(fw, "PAR_START")) {
            if (n == cap) {
                cap = cap ? cap * 2 : 4;
                out = (ParRange *)realloc(out, sizeof(ParRange) * (size_t)cap);
                if (!out) vm_debug_panic("[VM] collect_par_ranges: memoria esaurita\n");
            }
            out[n].start_line = cur;
            int   depth = 1;
            uint  max_inner = cur;
            char *scan  = nl + 1;
            *nl = '\n';
            while (scan && *scan && depth > 0) {
                char *nl2 = strchr(scan, '\n'); if (!nl2) break; *nl2 = '\0';
                uint  cur2 = (uint)atoi(scan);
                if (cur2 > max_inner) max_inner = cur2;
                VM_LINE_COPY(tmp2, scan);
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
    *outp = out;
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

/* Analisi strutturale per procedura (loop, if, par), calcolata una volta per
 * thread e procedura base. */
typedef struct {
    char *base;
    int nloops, nifs, npars;
    LoopDescriptor *loops;
    IfDescriptor   *ifs;
    ParRange       *pars;
} FrameAnalysisCache;
/* Thread-local: due thread par (es. fib_left/fib_right) chiamano
   invert_op_to_line in parallelo; senza __thread la cache (n++ + array
   writes) è una race → descriptor partial-write → DELOCAL con
   value errato a fine inverse. Una voce per procedura base, in un vettore
   che cresce: le voci non si spostano mai fuori da una chiamata in corso,
   perché loops/ifs/pars sono blocchi propri, non dentro il vettore. */
static __thread FrameAnalysisCache *_fa_cache = NULL;
static __thread int _fa_cache_n = 0, _fa_cache_cap = 0;

/* Alla fine di un thread di par: la cache è __thread, la libera chi l'ha creata. */
static void vm_invert_free_cache(void)
{
    for (int c = 0; c < _fa_cache_n; c++) {
        free(_fa_cache[c].base);
        loop_descs_free(_fa_cache[c].loops, _fa_cache[c].nloops);
        if_descs_free(_fa_cache[c].ifs, _fa_cache[c].nifs);
        free(_fa_cache[c].loops);
        free(_fa_cache[c].ifs);
        free(_fa_cache[c].pars);
    }
    free(_fa_cache);
    _fa_cache = NULL;
    _fa_cache_n = _fa_cache_cap = 0;
}



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
    VM_FRAME_BASE(base, frame_name);
    uint fi_reset = char_id_map_get(&FrameIndexer, base);
    /* Svuota tenendo il buffer: lo stack non è stato salvato da nessuno qui. */
    stack_clear(&vm->frames[fi_reset]->LocalVariables);

    /* Per-frame analysis cache. collect_loops/ifs/par_ranges scan ~50KB
       bytecode per invocation. Molte UNCALL sulle stesse procedure →
       cache per base name evita N rescan. */
    /* `ifs` heap-allocato a capacità = righe del frame: una else-if chain
     * (store a indice runtime su array di N elementi) ha ~N IF annidati.
     * nifs ≤ righe del frame, quindi la capacità per-frame è un bound esatto e
     * compatto. Loop e par in vettori che crescono (collect_loops /
     * collect_par_ranges): nessun numero massimo per procedura. */
    LoopDescriptor *loops;
    IfDescriptor   *ifs;
    ParRange       *pars;
    int nloops, nifs, npars;
    int _fa_hit = -1;
    for (int _c = 0; _c < _fa_cache_n; _c++) {
        if (!strcmp(_fa_cache[_c].base, base)) { _fa_hit = _c; break; }
    }
    if (_fa_hit < 0) {
        /* Capacità IF = righe del frame (bound esatto su nifs). */
        int _frame_ifs_cap = (int)(vm->frames[fi_reset]->end_addr -
                                   vm->frames[fi_reset]->addr) + 2;
        if (_frame_ifs_cap < 16) _frame_ifs_cap = 16;
        if (_fa_cache_n == _fa_cache_cap) {
            int nc = _fa_cache_cap ? _fa_cache_cap * 2 : 16;
            FrameAnalysisCache *n = (FrameAnalysisCache *)realloc(
                _fa_cache, sizeof(FrameAnalysisCache) * (size_t)nc);
            if (!n) vm_debug_panic("[UNCALL] cache analisi: memoria esaurita\n");
            _fa_cache = n;
            _fa_cache_cap = nc;
        }
        FrameAnalysisCache e;
        e.base   = char_id_strdup(base);
        e.ifs    = malloc(sizeof(IfDescriptor) * (size_t)_frame_ifs_cap);
        if (!e.ifs) vm_debug_panic("[UNCALL] cache analisi: memoria esaurita\n");
        e.nloops = collect_loops(vm, frame_name, orig, &e.loops);
        e.nifs   = collect_ifs  (vm, frame_name, orig, e.ifs, _frame_ifs_cap);
        e.npars  = collect_par_ranges(orig, vm->frames[fi_reset]->addr + 1,
                                      vm->frames[fi_reset]->end_addr, &e.pars);
        _fa_hit = _fa_cache_n;
        _fa_cache[_fa_cache_n++] = e;
    }
    loops  = _fa_cache[_fa_hit].loops;  nloops = _fa_cache[_fa_hit].nloops;
    ifs    = _fa_cache[_fa_hit].ifs;    nifs   = _fa_cache[_fa_hit].nifs;
    pars   = _fa_cache[_fa_hit].pars;   npars  = _fa_cache[_fa_hit].npars;

    VM_LINE_COPY(cur_frame, frame_name);
    uint fi       = get_findex(cur_frame);
    uint start_ln = vm->frames[fi_reset]->addr + 1;
    (void)start_ln;
    /* Heap, dimensionato allo span del proc: con un [1024] fisso una
     * procedura > 1024 righe (es. fill con store a indice runtime su array di
     * ~95+ elementi: disj-chain profonda) veniva troncata in coda → push/delocal
     * del loop-guard esterno (lc1) fuori dalla collection → lc1 non ricreato in
     * inverse → "MINEQ: variabile lc1 è NULL". span = start - stop. */
    size_t lp_cap = (start > stop) ? (size_t)(start - stop) + 2 : 2;
    char   **lp    = malloc(sizeof(char *)  * lp_cap);
    uint    *ln    = malloc(sizeof(uint)    * lp_cap);
    uint8_t *lp_op = malloc(sizeof(uint8_t) * lp_cap);
    int nl = 0;
    /* Arena: one malloc per invert call instead of N strdups.
       Upper bound = size of remaining buffer from stop+1 to END_PROC. */
    size_t _arena_cap = strlen(orig) + 1;
    char  *_arena = (char *)malloc(_arena_cap);
    if (!_arena) vm_debug_panic("[UNCALL] arena malloc fallita\n");
    char  *_arena_p = _arena;
    char *ptr = go_to_line(orig, stop + 1);   // ← CAMBIA: parti da dopo PROC
    while (ptr && *ptr && (size_t)nl < lp_cap) {
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

    int i = nl - 1;
    while (i >= 0) {
        uint  cur   = ln[i];
        /* skip_lineno è puro arithmetic, può operare su lp[i] direttamente (immutato). */
        char *clean = skip_lineno(lp[i]);
        size_t _clean_len = strlen(clean);
        /* exe_line: copia indipendente per strtok finale (dopo dispatch early-skip).
           zbuf: copia indipendente per strtok early (fw_cls/arg1_cls). */
        char exe_line[_clean_len + 1];
        memcpy(exe_line, clean, _clean_len); exe_line[_clean_len] = '\0';
        char zbuf[_clean_len + 1];
        memcpy(zbuf, clean, _clean_len); zbuf[_clean_len] = '\0';
        char *fw_cls = strtok(zbuf, " \t");
        char *arg1_cls = strtok(NULL, " \t");
        if (!fw_cls) { i--; continue; }
        uint8_t op_tag = lp_op[i];
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
        /* ---- Layout a DUE corpi (from_back_line != 0) --------------------------
         * All'indietro il cammino decrescente attraversa c1, poi LABEL FROM_START,
         * poi c2, poi LABEL FROM_BACK. Serve una decisione dopo ogni I(c1) — la
         * guardia d'ingresso b1 è vera solo nello stato pre-loop — così da
         * produrre I(c1) [I(c2) I(c1)]*, speculare a c1 [c2 c1]* forward. */
        if (lz == LOOP_ZONE_START_LABEL && li >= 0 && loops[li].from_back_line) {
            do_eval(vm, fi, vm_name_get(loops[li].eval_entry_id), loops[li].eval_entry_op,
                    vm_name_get(loops[li].eval_entry_val));
            if (thread_val_IF) {
                /* Stato pre-loop raggiunto: niente altro c2 da invertire. Chiude
                 * sul JMPF FROM_ERR, esattamente come il layout a un corpo. */
                int tt = lp_row_first_jmpf_from_err(loops[li].jmpf_err_line, lp, ln, nl);
                i = (tt >= 0) ? tt : i - 1;
            } else {
                i--;   /* scende dentro c2 */
            }
            continue;
        }
        if (lz == LOOP_ZONE_BACK_LABEL) {
            /* c2 invertito: risali all'EVAL d'uscita per un altro I(c1). */
            int t = lp_row_first_eval_at_line(loops[li].eval_exit_line, lp, ln, nl);
            if (t < 0) { vm_debug_panic("[UNCALL] loop_eval_exit (two-body)\n"); }
            i = t - 1;
            continue;
        }
        if (lz == LOOP_ZONE_JMP_START) { i--; continue; }  /* salto forward: no-op inverso */

        if (lz == LOOP_ZONE_EVAL_ENTRY || lz == LOOP_ZONE_EVAL_EXIT  ||
            lz == LOOP_ZONE_START_LABEL|| lz == LOOP_ZONE_END_LABEL  ||
            lz == LOOP_ZONE_ERR_LABEL)  { i--; continue; }

        if (lz == LOOP_ZONE_JMPF_ERR) {
            do_eval(vm, fi, vm_name_get(loops[li].eval_entry_id), loops[li].eval_entry_op,
                    vm_name_get(loops[li].eval_entry_val));
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
            do_eval(vm, fi, vm_name_get(loops[li].eval_exit_id), loops[li].eval_exit_op,
                    vm_name_get(loops[li].eval_exit_val));
            /* Forward: exit loop se exit-cond vera (senza jmp). Inverse: mentre il corpo da
               questa iterazione non è stato completamente inverted, ripeti da prima
               dell'EVAL until; quando la guardia coincide con uscita inversa, solo i--.
               I ramif erano scambiati: con e0==0 al primo incontr prendevamo i-- e mai il corpo. */
            int peel_more = loop_peel_more_at_until(vm, fi, loops, li, thread_val_IF);
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
                    stack_restore(&vm->frames[fi]->LocalVariables, sv);
                }
                uint then_from = ifs[ii].jmpf_else_line + 1;
                uint then_to = ifs[ii].jmp_fi_line;
                if (then_from < then_to) {
                    Stack sv = vm->frames[fi]->LocalVariables;
                    stack_init(&vm->frames[fi]->LocalVariables);
                    exec_branch_inverse(vm, orig, cur_frame, then_from, then_to, fi);
                    stack_restore(&vm->frames[fi]->LocalVariables, sv);
                }
            } else {
                do_eval_if_branch(vm, fi, &ifs[ii]);
                /* exec_branch_inverse puo' rivalutare thread_val_IF (if annidati). */
                int took_then_inv = thread_val_IF ? 1 : 0;
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
                    check_if_entry_inverse(vm, fi, &ifs[ii], took_then_inv);
                    int t = -1;
                    for (int j = i - 1; j >= 0; j--) if (ln[j] == ifs[ii].eval_entry_line) { t = j; break; }
                    i = (t >= 0) ? t - 1 : i - 1;
                    continue;
                }
                Stack sv = vm->frames[fi]->LocalVariables;
                stack_init(&vm->frames[fi]->LocalVariables);
                exec_branch_inverse(vm, orig, cur_frame, branch_from, branch_to, fi);
                stack_restore(&vm->frames[fi]->LocalVariables, sv);
                check_if_entry_inverse(vm, fi, &ifs[ii], took_then_inv);
            }
            int t = -1;
            for (int j = i - 1; j >= 0; j--) if (ln[j] == ifs[ii].eval_entry_line) { t = j; break; }
            i = (t >= 0) ? t - 1 : i - 1;
            continue;
        }

        char *fw = strtok(exe_line, " \t");
        if (!fw) { i--; continue; }

        if (op_tag == INVOP_PAR_END) { i--; continue; }

        if (op_tag == INVOP_CALL) {
            if (vm->dbg && vm->dbg->initialized)
                dbg_hook(vm->dbg, invert_extract_srcline(lp[i]), cur_frame, lp[i]);
            char *pn = strtok(NULL, " \t");
            /* Ricorsiva se pn è il nome base del frame corrente. */
            int is_rec = vm_base_eq(frame_name, pn);
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
            VM_PARAM_SAVE(sv, pc); for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi]->vars[pi[k]];
            char *p = NULL; int j = 0;
            while ((p = strtok(NULL, " \t")) && j < pc) {
                int si = char_id_map_get(&vm->frames[curi]->VarIndexer, p);
                vm->frames[cfi]->vars[pi[j++]] = vm->frames[curi]->vars[si];
            }
            VM_FRAME_KEY_BUF(target, pn);
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
            invert_op_to_line(vm, target, orig, vm->frames[cfi]->end_addr - 1,
                              vm->frames[cfi]->addr + 1, 1);
            for (int k = 0; k < pc; k++) vm->frames[cfi]->vars[pi[k]] = sv[k];
            VM_PARAM_SAVE_FREE(sv);
            i--; continue;
        }
        if (op_tag == INVOP_UNCALL) {
            if (vm->dbg && vm->dbg->initialized)
                dbg_hook(vm->dbg, invert_extract_srcline(lp[i]), cur_frame, lp[i]);
            char *pn = strtok(NULL, " \t");
            /* Ricorsiva se pn è il nome base del frame corrente. */
            int is_rec = vm_base_eq(frame_name, pn);
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
            VM_PARAM_SAVE(sv, pc); for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi]->vars[pi[k]];
            char *p = NULL; int j = 0;
            while ((p = strtok(NULL, " \t")) && j < pc) {
                int si = char_id_map_get(&vm->frames[curi]->VarIndexer, p);
                vm->frames[cfi]->vars[pi[j++]] = vm->frames[curi]->vars[si];
            }
            VM_FRAME_KEY_BUF(cn, pn);
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
            vm->inversion_depth          = 0;
            vm->suppress_show = 1;
            vm_run_BT(vm, orig, cn);
            vm->inversion_depth       = saved_inv;
            vm->suppress_show = ss;
            for (int k = 0; k < pc; k++) vm->frames[cfi]->vars[pi[k]] = sv[k];
            VM_PARAM_SAVE_FREE(sv);
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
                par_block_free(&pb);
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

        switch (op_tag) {
            case INVOP_PUSHEQ:  op_pusheq_inv(vm, cur_frame); break;
            case INVOP_MINEQ:   op_mineq_inv (vm, cur_frame); break;
            case INVOP_XOREQ:   op_xoreq_inv (vm, cur_frame); break;
            case INVOP_SWAP:    op_swap_inv  (vm, cur_frame); break;
            case INVOP_PUSH:    op_pop       (vm, cur_frame); break;
            case INVOP_POP:     op_push      (vm, cur_frame); break;
            case INVOP_SSEND:   op_srecv     (vm, cur_frame); break;
            case INVOP_SRECV:   op_ssend     (vm, cur_frame); break;
            case INVOP_LOCAL:   op_delocal   (vm, cur_frame); break;
            case INVOP_DELOCAL: op_local     (vm, cur_frame); break;
            case INVOP_SHOW:    /* no-op in inverse */ break;
            case INVOP_START: case INVOP_PARAM: case INVOP_LABEL:
            case INVOP_EVAL:  case INVOP_JMPF:  case INVOP_JMP:
            case INVOP_ASSERT: case INVOP_DECL: case INVOP_HALT:
            case INVOP_THREAD: /* skip */ break;
            default: vm_debug_panic("[UNCALL] op sconosciuta: '%s'\n", fw);
        }
        i--;
    }

    free(_arena);
    free(lp); free(ln); free(lp_op);
    /* orig non strduped → niente free(orig) */
    VMLOG("[INVERT] completata, righe processate=%d\n", nl);
    vm->inversion_depth--;
    /* orig = buffer (no strdup), niente free */
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
        VM_LINE_COPY(lb, ptr);
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
        VM_LINE_COPY(lb, ptr);
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
        VM_LINE_COPY(lb, ptr);
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
    /* snapshot vars dimensionato sulla capacità dinamica del frame. */
    int _vcap = vm->frames[cfi]->vars_cap > 0 ? vm->frames[cfi]->vars_cap : 1;
    Var **saved = (Var **)malloc(sizeof(Var *) * (size_t)_vcap);
    memcpy(saved, vm->frames[cfi]->vars, sizeof(Var *) * (size_t)_vcap);
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

    Var **tmp_alloc = (Var **)calloc((size_t)_vcap, sizeof(Var *));
    for (int v = 0; v < vm->frames[cfi]->var_count; v++) {
        if (!vm->frames[cfi]->vars[v]) {
            vm->frames[cfi]->vars[v]        = calloc(1, sizeof(Var));
            vm->frames[cfi]->vars[v]->T     = TYPE_INT;
            vm->frames[cfi]->vars[v]->value = calloc(1, sizeof(int64_t));
            vm->frames[cfi]->vars[v]->name  = char_id_strdup(saved[v] ? saved[v]->name : "");
            tmp_alloc[v] = vm->frames[cfi]->vars[v];
        }
    }

    if (branch_span_has_from_loop(original_buffer, from_line, to_line)) {
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
    } else if (branch_span_has_nested_if(original_buffer, from_line, to_line)) {
        /* Branch contiene nested IF: linear-reverse standard processerebbe entrambi
         * THEN+ELSE come atomic ops (non riconosce JMPF/LABEL/EVAL) corrompendo
         * hist. Collect_ifs sull'intero frame, itera righe del branch in reverse,
         * skippa quelle interne a nested IF e dispatch ai loro JMPF ELSE_<uid>
         * con recurse exec_branch_inverse sul ramo che era vero in forward. */
        /* Heap, capacità = righe del frame (bound su nifs): un cap fisso (256)
         * troncava le else-if chain di store a indice runtime su array grandi
         * (> ~256 elementi, fino a ARR_MAX=1024) → collect_ifs parziale →
         * branch-pairing errato → POP sbilanciato. */
        uint _ebi_fi = get_findex(frame_name);
        int local_max_ifs = (int)(vm->frames[_ebi_fi]->end_addr -
                                  vm->frames[_ebi_fi]->addr) + 2;
        if (local_max_ifs < 256) local_max_ifs = 256;
        IfDescriptor  *ifs = calloc((size_t)local_max_ifs, sizeof(IfDescriptor));
        int nifs2 = collect_ifs(vm, frame_name, original_buffer, ifs, local_max_ifs);

        /* Heap, dimensionato allo span del branch: con [512] fisso un branch
         * THEN/ELSE > 512 righe (es. `if lc1 != 0` che racchiude un from-loop con
         * disj-chain runtime-index profonda, array > ~90 elementi) veniva troncato
         * → la coda del loop (EVAL until, peel) cadeva fuori → peel zone mai
         * rilevata → loop counter non invertito (DELOCAL lc0 atteso=0 trovato<0). */
        int lp_cap = (to_line > from_line) ? (int)(to_line - from_line) + 2 : 2;
        char **lp = malloc(sizeof(char *) * (size_t)lp_cap);
        uint  *ln = malloc(sizeof(uint)   * (size_t)lp_cap);
        int nl = 0;
        char *p3 = go_to_line(original_buffer, from_line);
        while (p3 && *p3 && nl < lp_cap) {
            char *nl3 = strchr(p3, '\n'); if (!nl3) break; *nl3 = '\0';
            uint cur_ln = (uint)atoi(p3);
            if (cur_ln >= to_line) { *nl3 = '\n'; break; }
            lp[nl] = strdup(p3); ln[nl] = cur_ln; nl++;
            *nl3 = '\n'; p3 = nl3 + 1;
        }
        int idx = nl - 1;
        while (idx >= 0) {
            uint cur = ln[idx];
            VM_LINE_COPY(ob, lp[idx]);
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
            /* Dispatch SOLO IF immediatamente nel branch. Una else-if chain
             * (`if C0 else if C1 else if C2 ...`) annida ogni IF nell'ELSE del
             * precedente: C2,C3,... matchano il filtro (jmpf_else in span) ma
             * sono enclosed dentro C1. La recursion del parent li gestisce già;
             * dispatcharli anche qui = doppia inversione → hist sbilanciato
             * (pop extra). Skip se enclosed da un altro nested IF dello span. */
            if (matched >= 0) {
                uint el = ifs[matched].jmpf_else_line;
                for (int k = 0; k < nifs2; k++) {
                    if (k == matched) continue;
                    if (ifs[k].jmpf_else_line <= from_line) continue;
                    if (ifs[k].assert_line >= to_line) continue;
                    if ((el > ifs[k].jmpf_else_line && el < ifs[k].jmp_fi_line) ||
                        (el > ifs[k].else_label_line && el < ifs[k].fi_label_line)) {
                        matched = -1; break;
                    }
                }
                if (matched < 0) { idx--; continue; }
            }
            if (matched >= 0) {
                do_eval_if_branch(vm, cfi, &ifs[matched]);
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
                    stack_restore(&vm->frames[cfi]->LocalVariables, sv2);
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
                /* Ricorsiva se pn è il nome base del frame corrente. */
                int is_rec_c = vm_base_eq(frame_name, pn);
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
                VM_PARAM_SAVE(sv_c, pc_c);
                for (int k = 0; k < pc_c; k++) sv_c[k] = vm->frames[callee_fi_c]->vars[pi_c[k]];
                Stack slv_c = vm->frames[callee_fi_c]->LocalVariables;
                stack_init(&vm->frames[callee_fi_c]->LocalVariables);
                char *p3c = NULL; int jjc = 0;
                while ((p3c = strtok(NULL, " \t")) && jjc < pc_c) {
                    if (!char_id_map_exists(&vm->frames[cfi]->VarIndexer, p3c)) { jjc++; continue; }
                    int si = char_id_map_get(&vm->frames[cfi]->VarIndexer, p3c);
                    vm->frames[callee_fi_c]->vars[pi_c[jjc++]] = vm->frames[cfi]->vars[si];
                }
                VM_FRAME_KEY_BUF(target_c, pn);
                if (is_rec_c && current_thread_args) make_frame_key_par_rec(pn, new_depth_c, target_c, sizeof(target_c));
                else if (is_rec_c) make_frame_key(pn, new_depth_c, target_c, sizeof(target_c));
                else if (current_thread_args) make_thread_frame_key(pn, target_c, sizeof(target_c));
                else { strncpy(target_c, pn, sizeof(target_c) - 1); target_c[sizeof(target_c) - 1] = '\0'; }
                invert_op_to_line(vm, target_c, original_buffer,
                                  vm->frames[callee_fi_c]->end_addr - 1,
                                  vm->frames[callee_fi_c]->addr + 1, 1);
                for (int k = 0; k < pc_c; k++) vm->frames[callee_fi_c]->vars[pi_c[k]] = sv_c[k];
                stack_restore(&vm->frames[callee_fi_c]->LocalVariables, slv_c);
                VM_PARAM_SAVE_FREE(sv_c);
            }
            else if (!strcmp(fw, "PUSHEQ")) op_pusheq_inv(vm, frame_name);
            else if (!strcmp(fw, "MINEQ"))  op_mineq_inv (vm, frame_name);
            else if (!strcmp(fw, "XOREQ"))  op_xoreq_inv (vm, frame_name);
            else if (!strcmp(fw, "SWAP"))   op_swap_inv  (vm, frame_name);
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
        free(lp); free(ln);
        if_descs_free(ifs, nifs2);
        free(ifs);
    } else {
        int lines_cap = (to_line > from_line) ? (int)(to_line - from_line) + 2 : 2;
        char **lines = malloc(sizeof(char *) * (size_t)lines_cap); int count = 0;
        char *p2 = go_to_line(original_buffer, from_line);
        if (p2 && *p2) {
            while (p2 && *p2 && count < lines_cap) {
                char *nl2 = strchr(p2, '\n'); if (!nl2) break; *nl2 = '\0';
                if ((uint)atoi(p2) >= to_line) { *nl2 = '\n'; break; }
                lines[count++] = strdup(p2);
                *nl2 = '\n'; p2 = nl2 + 1;
            }
        }
        for (int i = count - 1; i >= 0; i--) {
            VM_LINE_COPY(ob, lines[i]);
            char *clean = skip_lineno(ob);
            char *fw = strtok(clean, " \t");
            if (!fw) continue;

            if (!strcmp(fw, "CALL")) {
                /* Inverti la procedura chiamata (anche ricorsiva): mirror del
                   path in invert_op_to_line per CALL. Per ricorsiva calcola
                   new_depth dal frame_name (@1, @_1, …) e usa clone_frame_for_depth. */
                vm_if_mark_call();
                char *pn = strtok(NULL, " \t");
                /* Ricorsiva se pn è il nome base del frame corrente. */
                int is_rec_c = vm_base_eq(frame_name, pn);
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
                VM_PARAM_SAVE(sv_c, pc_c);
                for (int k = 0; k < pc_c; k++) sv_c[k] = vm->frames[callee_fi_c]->vars[pi_c[k]];
                Stack slv_c = vm->frames[callee_fi_c]->LocalVariables;
                stack_init(&vm->frames[callee_fi_c]->LocalVariables);
                char *p3c = NULL; int jjc = 0;
                while ((p3c = strtok(NULL, " \t")) && jjc < pc_c) {
                    if (!char_id_map_exists(&vm->frames[cfi]->VarIndexer, p3c)) { jjc++; continue; }
                    int si = char_id_map_get(&vm->frames[cfi]->VarIndexer, p3c);
                    vm->frames[callee_fi_c]->vars[pi_c[jjc++]] = vm->frames[cfi]->vars[si];
                }
                VM_FRAME_KEY_BUF(target_c, pn);
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
                invert_op_to_line(vm, target_c, original_buffer,
                                  vm->frames[callee_fi_c]->end_addr - 1,
                                  vm->frames[callee_fi_c]->addr + 1, 1);
                for (int k = 0; k < pc_c; k++) vm->frames[callee_fi_c]->vars[pi_c[k]] = sv_c[k];
                stack_restore(&vm->frames[callee_fi_c]->LocalVariables, slv_c);
                VM_PARAM_SAVE_FREE(sv_c);
                continue;
            }

            if (!strcmp(fw, "UNCALL")) {
                vm_if_mark_call();
                char *pn = strtok(NULL, " \t");
                /* Ricorsiva se pn è il nome base del frame corrente. */
                int is_rec = vm_base_eq(frame_name, pn);
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
                VM_PARAM_SAVE(sv, pc); for (int k = 0; k < pc; k++) sv[k] = vm->frames[callee_fi]->vars[pi[k]];
                Stack slv = vm->frames[callee_fi]->LocalVariables;
                stack_init(&vm->frames[callee_fi]->LocalVariables);
                char *p3 = NULL; int jj = 0;
                while ((p3 = strtok(NULL, " \t")) && jj < pc) {
                    int si = char_id_map_get(&vm->frames[cfi]->VarIndexer, p3);
                    vm->frames[callee_fi]->vars[pi[jj++]] = vm->frames[cfi]->vars[si];
                }
                VM_FRAME_KEY_BUF(cn, pn);
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
                vm->inversion_depth       = 0;
                vm->suppress_show = 1;
                vm_run_BT(vm, original_buffer, cn);
                vm->inversion_depth       = saved_inv;
                vm->suppress_show = ss;
                for (int k = 0; k < pc; k++) vm->frames[callee_fi]->vars[pi[k]] = sv[k];
                stack_restore(&vm->frames[callee_fi]->LocalVariables, slv);
                VM_PARAM_SAVE_FREE(sv);
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
        free(lines);
    }

    for (int v = 0; v < vm->frames[cfi]->var_count && v < _vcap; v++)
        if (tmp_alloc[v] && vm->frames[cfi]->vars[v] == tmp_alloc[v]) {
            free(tmp_alloc[v]->value); free(tmp_alloc[v]->name); free(tmp_alloc[v]);
            vm->frames[cfi]->vars[v] = NULL;
        }

    /* Restore SOLO gli slot param relinkati al caller (vedi 1525-1531).
     * Il vecchio blanket `memcpy(vars, saved)` reinstaurava OGNI slot,
     * inclusi quelli che il branch ha legittimamente liberato via op_delocal
     * (op_local/op_delocal fanno free+realloc del Var): sotto la recursion
     * annidata `exec_branch_inverse → invert_op_to_line → exec_branch_inverse`
     * lo snapshot raw `saved[v]` puntava al Var dell'outer poi liberato dal
     * branch interno → restore reinstaurava un puntatore freed → use-after-free
     * al successivo op_local (SIGSEGV su loop annidati). Gli slot non-param
     * vanno lasciati com'è il branch li ha lasciati (op_delocal li ha già
     * messi a NULL; i tmp sono stati liberati sopra). */
    for (int p = 0; p < vm->frames[cfi]->param_count; p++) {
        int pidx = vm->frames[cfi]->param_indices[p];
        if (pidx < _vcap)
            vm->frames[cfi]->vars[pidx] = saved[pidx];
    }
    stack_restore(&vm->frames[cfi]->LocalVariables, saved_lv);
    free(saved);
    free(tmp_alloc);
}

#endif /* VM_INVERT_H */