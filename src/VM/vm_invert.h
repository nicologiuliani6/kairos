#ifndef VM_INVERT_H
#define VM_INVERT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"
#include "vm_helpers.h"
#include "vm_ops.h"

/* Forward — vm_run_BT è definita in Janus.c */
void vm_run_BT(VM *vm, char *buffer, char *frame_name_init);

/* ======================================================================
 *  Descrittori strutturali per loop e if (usati dall'inversore)
 * ====================================================================== */

typedef struct {
    uint eval_entry_line;
    char eval_entry_id[64], eval_entry_val[64];
    uint jmpf_err_line, from_start_line, from_end_line, from_err_line;
    uint eval_exit_line;
    char eval_exit_id[64], eval_exit_val[64];
    uint jmpf_start_line;
} LoopDescriptor;

typedef struct {
    uint eval_entry_line;
    char eval_entry_id[64], eval_entry_val[64];
    uint jmpf_else_line, jmp_fi_line, else_label_line, fi_label_line;
    uint eval_exit_line;
    char eval_exit_id[64], eval_exit_val[64];
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

static inline int collect_loops(VM *vm, const char *frame_name, char *buf,
                                 LoopDescriptor *out, int max)
{
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi].addr + 1);
    int n = 0, in_loop = 0;
    uint peval = 0; char pid[64] = {0}, pval[64] = {0};

    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[512]; strncpy(lb, ptr, sizeof(lb) - 1);
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { *nl = '\n'; ptr = nl + 1; continue; }

        if (!strcmp(fw, "EVAL")) {
            peval = cur;
            char *a = strtok(NULL, " \t"), *b = strtok(NULL, " \t");
            strncpy(pid, a ? a : "", 63); strncpy(pval, b ? b : "", 63);
        } else if (!strcmp(fw, "JMPF") && !in_loop) {
            char *ln = strtok(NULL, " \t");
            if (ln && !strncmp(ln, "FROM_ERR", 8)) {
                out[n].eval_entry_line = peval;
                strncpy(out[n].eval_entry_id,  pid,  63);
                strncpy(out[n].eval_entry_val, pval, 63);
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
    int n = 0, in_if = 0;
    uint peval = 0; char pid[64] = {0}, pval[64] = {0};

    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[512]; strncpy(lb, ptr, sizeof(lb) - 1);
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { *nl = '\n'; ptr = nl + 1; continue; }

        if (!strcmp(fw, "EVAL")) {
            peval = cur;
            char *a = strtok(NULL, " \t"), *b = strtok(NULL, " \t");
            strncpy(pid, a ? a : "", 63); strncpy(pval, b ? b : "", 63);
        } else if (!strcmp(fw, "JMPF") && !in_if) {
            char *ln = strtok(NULL, " \t");
            if (ln && !strncmp(ln, "ELSE_", 5)) {
                out[n].eval_entry_line = peval;
                strncpy(out[n].eval_entry_id,  pid,  63);
                strncpy(out[n].eval_entry_val, pval, 63);
                out[n].jmpf_else_line = cur; in_if = 1;
            }
        } else if (!strcmp(fw, "JMP") && in_if) {
            char *ln = strtok(NULL, " \t");
            if (ln && !strncmp(ln, "FI_", 3)) out[n].jmp_fi_line = cur;
        } else if (!strcmp(fw, "LABEL") && in_if) {
            char *ln = strtok(NULL, " \t"); if (!ln) { *nl = '\n'; ptr = nl + 1; continue; }
            if      (!strncmp(ln, "ELSE_", 5)) out[n].else_label_line = cur;
            else if (!strncmp(ln, "FI_",   3)) out[n].fi_label_line   = cur;
        } else if (!strcmp(fw, "ASSERT") && in_if) {
            out[n].eval_exit_line = peval;
            strncpy(out[n].eval_exit_id,  pid,  63);
            strncpy(out[n].eval_exit_val, pval, 63);
            out[n].assert_line = cur; in_if = 0; n++;
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

static inline void do_eval(VM *vm, uint fi, const char *id, const char *val)
{
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, id);
    int rhs = char_id_map_exists(&vm->frames[fi].VarIndexer, val)
        ? *(vm->frames[fi].vars[char_id_map_get(&vm->frames[fi].VarIndexer, val)]->value)
        : (int)strtol(val, NULL, 10);
    thread_val_IF = (*(vm->frames[fi].vars[vi]->value) == rhs);
}

static inline int line_is_inside_if(uint line, IfDescriptor *ifs, int nifs)
{
    for (int i = 0; i < nifs; i++)
        if (line > ifs[i].jmpf_else_line && line < ifs[i].fi_label_line) return 1;
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
                       uint start, uint stop)
{
    (void)start; (void)stop;
    char *orig = strdup(buffer);
    if (!orig) { fprintf(stderr, "[UNCALL] strdup fallita\n"); exit(EXIT_FAILURE); }

    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi_reset = char_id_map_get(&FrameIndexer, base);
    stack_init(&vm->frames[fi_reset].LocalVariables);

#define MAX_LOOPS 32
#define MAX_IFS   32
#define MAX_LINES 1024

    LoopDescriptor loops[MAX_LOOPS]; int nloops = collect_loops(vm, frame_name, orig, loops, MAX_LOOPS);
    IfDescriptor   ifs  [MAX_IFS];   int nifs   = collect_ifs  (vm, frame_name, orig, ifs,   MAX_IFS);

    char cur_frame[VAR_NAME_LENGTH]; strncpy(cur_frame, frame_name, VAR_NAME_LENGTH - 1);
    uint fi       = char_id_map_get(&FrameIndexer, cur_frame);
    uint start_ln = vm->frames[fi_reset].addr + 1;

    char *lp[MAX_LINES]; uint ln[MAX_LINES]; int nl = 0;
    char *ptr = go_to_line(orig, start_ln);
    while (ptr && *ptr && nl < MAX_LINES) {
        char *newline = strchr(ptr, '\n'); if (!newline) break;
        *newline = '\0';
        char tmp[512]; strncpy(tmp, ptr, sizeof(tmp) - 1);
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw && !strcmp(fw, "END_PROC")) { *newline = '\n'; break; }
        lp[nl] = strdup(ptr); ln[nl] = (uint)atoi(ptr); nl++;
        *newline = '\n'; ptr = newline + 1;
    }

    int i = nl - 1;
    while (i >= 0) {
        char ob[512]; strncpy(ob, lp[i], sizeof(ob) - 1);
        uint  cur   = ln[i];
        char *clean = skip_lineno(ob);
        char *fw    = strtok(clean, " \t");
        if (!fw) { i--; continue; }

        int li = -1; LoopZone lz = line_loop_zone(cur, loops, nloops, &li);
        if (lz == LOOP_ZONE_EVAL_ENTRY || lz == LOOP_ZONE_EVAL_EXIT  ||
            lz == LOOP_ZONE_START_LABEL|| lz == LOOP_ZONE_END_LABEL  ||
            lz == LOOP_ZONE_ERR_LABEL)  { i--; continue; }

        if (lz == LOOP_ZONE_JMPF_ERR) {
            do_eval(vm, fi, loops[li].eval_entry_id, loops[li].eval_entry_val);
            if (thread_val_IF) { i--; }
            else {
                int t = -1;
                for (int j = nl - 1; j >= 0; j--) if (ln[j] == loops[li].jmpf_start_line) { t = j; break; }
                if (t < 0) { fprintf(stderr, "[UNCALL] jmpf_start\n"); exit(1); }
                i = t - 1;
            }
            continue;
        }
        if (lz == LOOP_ZONE_JMPF_START) {
            do_eval(vm, fi, loops[li].eval_exit_id, loops[li].eval_exit_val);
            if (thread_val_IF) { i--; }
            else {
                int t = -1;
                for (int j = 0; j < nl; j++) if (ln[j] == loops[li].jmpf_err_line) { t = j; break; }
                if (t < 0) { fprintf(stderr, "[UNCALL] jmpf_err\n"); exit(1); }
                i = t - 1;
            }
            continue;
        }

        int ii = -1; IfZone iz = line_if_zone(cur, ifs, nifs, &ii);
        if (iz == IF_ZONE_EVAL_ENTRY || iz == IF_ZONE_EVAL_EXIT || iz == IF_ZONE_ELSE_LABEL ||
            iz == IF_ZONE_FI_LABEL   || iz == IF_ZONE_ASSERT    || iz == IF_ZONE_JMP_FI)
            { i--; continue; }

        if (iz == IF_ZONE_JMPF_ELSE) {
            int depth = vm->frames[fi_reset].recursion_depth;
            for (int d = 0; d < depth; d++) {
                Stack sv = vm->frames[fi].LocalVariables; stack_init(&vm->frames[fi].LocalVariables);
                exec_branch_inverse(vm, orig, cur_frame, ifs[ii].else_label_line + 1, ifs[ii].fi_label_line, fi);
                vm->frames[fi].LocalVariables = sv;
            }
            { Stack sv = vm->frames[fi].LocalVariables; stack_init(&vm->frames[fi].LocalVariables);
              exec_branch_inverse(vm, orig, cur_frame, ifs[ii].jmpf_else_line + 1, ifs[ii].jmp_fi_line, fi);
              vm->frames[fi].LocalVariables = sv; }
            int t = -1;
            for (int j = i - 1; j >= 0; j--) if (ln[j] == ifs[ii].eval_entry_line) { t = j; break; }
            i = (t >= 0) ? t - 1 : i - 1;
            continue;
        }

        if (line_is_inside_if(cur, ifs, nifs)) { i--; continue; }

        if (!strcmp(fw, "CALL")) {
            char *pn = strtok(NULL, " \t");
            uint cfi  = char_id_map_get(&FrameIndexer, pn);
            uint curi = char_id_map_get(&FrameIndexer, frame_name);
            int  pc = vm->frames[cfi].param_count, *pi = vm->frames[cfi].param_indices;
            Var *sv[64]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi].vars[pi[k]];
            char *p = NULL; int j = 0;
            while ((p = strtok(NULL, " \t")) && j < pc) {
                int si = char_id_map_get(&vm->frames[curi].VarIndexer, p);
                vm->frames[cfi].vars[pi[j++]] = vm->frames[curi].vars[si];
            }
            invert_op_to_line(vm, pn, orig, vm->frames[cfi].addr + 1, vm->frames[cfi].end_addr - 1);
            for (int k = 0; k < pc; k++) vm->frames[cfi].vars[pi[k]] = sv[k];
            i--; continue;
        }
        if (!strcmp(fw, "UNCALL")) {
            char *pn = strtok(NULL, " \t");
            uint cfi  = char_id_map_get(&FrameIndexer, pn);
            uint curi = fi;
            int  pc = vm->frames[cfi].param_count, *pi = vm->frames[cfi].param_indices;
            Var *sv[64]; for (int k = 0; k < pc; k++) sv[k] = vm->frames[cfi].vars[pi[k]];
            char *p = NULL; int j = 0;
            while ((p = strtok(NULL, " \t")) && j < pc) {
                int si = char_id_map_get(&vm->frames[curi].VarIndexer, p);
                vm->frames[cfi].vars[pi[j++]] = vm->frames[curi].vars[si];
            }
            char cn[VAR_NAME_LENGTH]; strncpy(cn, pn, VAR_NAME_LENGTH - 1);
            vm_run_BT(vm, orig, cn);
            for (int k = 0; k < pc; k++) vm->frames[cfi].vars[pi[k]] = sv[k];
            i--; continue;
        }

        if      (!strcmp(fw, "PUSHEQ")) op_pusheq_inv(vm, cur_frame);
        else if (!strcmp(fw, "MINEQ"))  op_mineq_inv (vm, cur_frame);
        else if (!strcmp(fw, "XOREQ"))  op_xoreq_inv (vm, cur_frame);  /* ← XOR inverso */
        else if (!strcmp(fw, "SWAP"))   op_swap_inv  (vm, cur_frame);
        else if (!strcmp(fw, "PUSH"))   op_pop       (vm, cur_frame);
        else if (!strcmp(fw, "POP"))    op_push      (vm, cur_frame);
        else if (!strcmp(fw, "LOCAL"))  op_delocal   (vm, cur_frame);
        else if (!strcmp(fw, "DELOCAL"))op_local     (vm, cur_frame);
        else if (!strcmp(fw, "SHOW"))   op_show      (vm, cur_frame);
        else if (!strcmp(fw, "PARAM") || !strcmp(fw, "LABEL") || !strcmp(fw, "EVAL")  ||
                 !strcmp(fw, "JMPF")  || !strcmp(fw, "JMP")   || !strcmp(fw, "ASSERT")||
                 !strcmp(fw, "DECL")  || !strcmp(fw, "HALT"))  { /* skip */ }
        else { fprintf(stderr, "[UNCALL] op sconosciuta: '%s'\n", fw); exit(EXIT_FAILURE); }
        i--;
    }

    for (int j = 0; j < nl; j++) free(lp[j]);
    free(orig);
#undef MAX_LOOPS
#undef MAX_IFS
#undef MAX_LINES
}

/* ======================================================================
 *  exec_branch_inverse
 * ====================================================================== */

static void exec_branch_inverse(VM *vm, char *original_buffer,
                                const char *frame_name,
                                uint from_line, uint to_line,
                                uint caller_fi)
{
    char *lines[512]; int count = 0;
    char *ptr = go_to_line(original_buffer, from_line);
    if (!ptr) return;
    while (ptr && *ptr && count < 512) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        if ((uint)atoi(ptr) >= to_line) { *nl = '\n'; break; }
        lines[count++] = strdup(ptr);
        *nl = '\n'; ptr = nl + 1;
    }

    uint cfi = char_id_map_get(&FrameIndexer, frame_name);
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

    for (int i = count - 1; i >= 0; i--) {
        char ob[512]; strncpy(ob, lines[i], sizeof(ob) - 1);
        char *fw = strtok(skip_lineno(ob), " \t");
        if (!fw || !strcmp(fw, "CALL") || !strcmp(fw, "UNCALL")) continue;
        if      (!strcmp(fw, "PUSHEQ")) op_pusheq_inv(vm, frame_name);
        else if (!strcmp(fw, "MINEQ"))  op_mineq_inv (vm, frame_name);
        else if (!strcmp(fw, "XOREQ"))  op_xoreq_inv (vm, frame_name);  /* ← XOR inverso */
        else if (!strcmp(fw, "SWAP"))   op_swap_inv  (vm, frame_name);
        else if (!strcmp(fw, "PUSH"))   op_pop       (vm, frame_name);
        else if (!strcmp(fw, "POP"))    op_push      (vm, frame_name);
        else if (!strcmp(fw, "LOCAL"))  op_delocal   (vm, frame_name);
        else if (!strcmp(fw, "DELOCAL"))op_local     (vm, frame_name);
        else if (!strcmp(fw, "SHOW"))   op_show      (vm, frame_name);
    }

    for (int v = 0; v < vm->frames[cfi].var_count; v++)
        if (tmp_alloc[v] && vm->frames[cfi].vars[v] == tmp_alloc[v]) {
            free(tmp_alloc[v]->value); free(tmp_alloc[v]); vm->frames[cfi].vars[v] = NULL;
        }

    memcpy(vm->frames[cfi].vars, saved, sizeof(Var *) * MAX_VARS);
    vm->frames[cfi].LocalVariables = saved_lv;
    for (int i = 0; i < count; i++) free(lines[i]);
}

#endif /* VM_INVERT_H */