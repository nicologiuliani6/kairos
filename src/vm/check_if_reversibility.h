#ifndef CHECK_IF_REVERSIBILITY_H
#define CHECK_IF_REVERSIBILITY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIR_MAX_LINES   4096
#define CIR_MAX_ARGS       8
#define CIR_TOK_LEN      128
#define CIR_MAX_LABELS   256
#define CIR_LINE_PREFIX    6

typedef struct {
    int  lineno;
    char op  [CIR_TOK_LEN];
    char arg [CIR_MAX_ARGS][CIR_TOK_LEN];
    int  argc;
} CIR_Line;

typedef struct {
    char name  [CIR_TOK_LEN];
    int  lineno;
} CIR_Label;

static int writes_to(const CIR_Line *L, const char *var_name)
{
    const char *op = L->op;

    if (strcmp(op, "PUSHEQ") == 0 || strcmp(op, "MINEQ")  == 0 ||
        strcmp(op, "XOREQ")  == 0 ||
        strcmp(op, "PRODEQ") == 0 || strcmp(op, "DIVEQ")  == 0 ||
        strcmp(op, "MODEQ")  == 0 || strcmp(op, "EXPEQ")  == 0)
        return L->argc >= 1 && strcmp(L->arg[0], var_name) == 0;

    if (strcmp(op, "POP") == 0)
        return L->argc >= 1 && strcmp(L->arg[0], var_name) == 0;

    if (strcmp(op, "PUSH") == 0)
        return L->argc >= 1 && strcmp(L->arg[0], var_name) == 0;

    if (strcmp(op, "SWAP") == 0)
        return (L->argc >= 1 && strcmp(L->arg[0], var_name) == 0) ||
               (L->argc >= 2 && strcmp(L->arg[1], var_name) == 0);

    if (strcmp(op, "LOCAL") == 0 || strcmp(op, "DELOCAL") == 0)
        return L->argc >= 2 && strcmp(L->arg[1], var_name) == 0;

    return 0;
}

static int label_lineno(const CIR_Label *labels, int nlabels, const char *name)
{
    for (int i = 0; i < nlabels; i++)
        if (strcmp(labels[i].name, name) == 0)
            return labels[i].lineno;
    return -1;
}

static int idx_at_lineno(const CIR_Line *lines, int nlines, int target)
{
    for (int i = 0; i < nlines; i++)
        if (lines[i].lineno >= target)
            return i;
    return nlines;
}

static int parse_lines(const char *buffer, CIR_Line *lines, int max_lines)
{
    int nlines = 0;
    const char *p = buffer;
    int lineno = 1;

    while (*p && nlines < max_lines) {
        const char *nl = strchr(p, '\n');
        if (!nl) break;

        int row_len = (int)(nl - p);

        if (row_len > CIR_LINE_PREFIX) {
            const char *content = p + CIR_LINE_PREFIX;
            int clen = row_len - CIR_LINE_PREFIX;
            if (clen > 511) clen = 511;

            char tmp[512];
            strncpy(tmp, content, clen);
            tmp[clen] = '\0';

            char *tok = strtok(tmp, " \t\r");
            if (tok) {
                CIR_Line *L = &lines[nlines];
                L->lineno = lineno;
                L->argc   = 0;
                strncpy(L->op, tok, CIR_TOK_LEN - 1);
                L->op[CIR_TOK_LEN - 1] = '\0';

                while ((tok = strtok(NULL, " \t\r")) != NULL &&
                       L->argc < CIR_MAX_ARGS) {
                    strncpy(L->arg[L->argc], tok, CIR_TOK_LEN - 1);
                    L->arg[L->argc][CIR_TOK_LEN - 1] = '\0';
                    L->argc++;
                }
                nlines++;
            }
        }

        p = nl + 1;
        lineno++;
    }

    return nlines;
}

/* ------------------------------------------------------------------ */
/*  Check: LOCAL/DELOCAL reversibility                                */
/* ------------------------------------------------------------------ */

static int vm_check_local_src_modified(const char *buffer)
{
    static CIR_Line lines[CIR_MAX_LINES];
    int nlines = parse_lines(buffer, lines, CIR_MAX_LINES);
    int errors = 0;

    char proc_name[CIR_TOK_LEN] = "<globale>";
    int  in_proc = 0;

    for (int i = 0; i < nlines; i++) {
        CIR_Line *L = &lines[i];

        if (strcmp(L->op, "PROC") == 0) {
            in_proc = 1;
            if (L->argc > 0)
                strncpy(proc_name, L->arg[0], CIR_TOK_LEN - 1);
            continue;
        }
        if (strcmp(L->op, "END_PROC") == 0) { in_proc = 0; continue; }
        if (!in_proc) continue;
        if (strcmp(L->op, "LOCAL") != 0 || L->argc < 3) continue;

        const char *dst = L->arg[1];
        const char *src = L->arg[2];

        if (strcmp(src, "nil") == 0) continue;

        char *endptr;
        /*long  src_num      =*/ strtol(src, &endptr, 10);
        int   src_is_literal = (*endptr == '\0');

        int depth = 0;
        for (int j = i + 1; j < nlines; j++) {
            CIR_Line *M = &lines[j];

            if (strcmp(M->op, "END_PROC") == 0) break;

            if (strcmp(M->op, "LOCAL") == 0 && M->argc >= 2 &&
                strcmp(M->arg[1], dst) == 0) {
                depth++;
                continue;
            }

            if (strcmp(M->op, "DELOCAL") == 0 && M->argc >= 2 &&
                strcmp(M->arg[1], dst) == 0) {
                if (depth > 0) { depth--; continue; }

                const char *delocal_val = M->argc >= 3 ? M->arg[2] : "";
                char *ep2;
                long  delocal_num      = strtol(delocal_val, &ep2, 10);
                int   delocal_is_literal = (*ep2 == '\0');
                int   delocal_is_zero    = (delocal_is_literal && delocal_num == 0);

                if (!src_is_literal) {
                    for (int k = i + 1; k < j; k++) {
                        if (writes_to(&lines[k], src)) {
                            printf("[WARNING] Procedura \"%s\": la variabile '%s' "
                                   "usata come sorgente di LOCAL a riga %d "
                                   "viene modificata da '%s' a riga %d, "
                                   "prima del DELOCAL a riga %d. "
                                   "Il programma potrebbe non essere reversibile.\n",
                                   proc_name, src,
                                   L->lineno,
                                   lines[k].op, lines[k].lineno,
                                   M->lineno);
                            errors++;
                        }
                    }
                }

                if (!src_is_literal && delocal_is_zero) {
                    for (int k = i + 1; k < j; k++) {
                        CIR_Line *Op = &lines[k];
                        if (strcmp(Op->op, "MINEQ") == 0 &&
                            Op->argc >= 2 &&
                            strcmp(Op->arg[0], dst) == 0 &&
                            strcmp(Op->arg[1], dst) == 0) {
                            printf("[WARNING] Procedura \"%s\": "
                                   "LOCAL int %s %s (riga %d) inizializza da variabile, "
                                   "ma 'MINEQ %s %s' (riga %d) azzera '%s' "
                                   "e DELOCAL (riga %d) verifica 0: "
                                   "in UNCALL '%s' non puo' essere ricostruito. "
                                   "Il programma non e' reversibile.\n",
                                   proc_name, dst, src, L->lineno,
                                   dst, dst, Op->lineno, dst,
                                   M->lineno, dst);
                            errors++;
                        }

                        if ((strcmp(Op->op, "PRODEQ") == 0 ||
                             strcmp(Op->op, "MODEQ")  == 0) &&
                            Op->argc >= 2 &&
                            strcmp(Op->arg[0], dst) == 0 &&
                            strcmp(Op->arg[1], "0") == 0) {
                            printf("[WARNING] Procedura \"%s\": "
                                   "LOCAL int %s %s (riga %d) inizializza da variabile, "
                                   "ma '%s %s 0' (riga %d) azzera '%s' "
                                   "e DELOCAL (riga %d) verifica 0: "
                                   "in UNCALL '%s' non puo' essere ricostruito.\n",
                                   proc_name, dst, src, L->lineno,
                                   Op->op, dst, Op->lineno, dst,
                                   M->lineno, dst);
                            errors++;
                        }
                    }
                }
                /*
                if (src_is_literal && delocal_is_literal && src_num != delocal_num) {
                    printf("[WARNING] Procedura \"%s\": "
                           "LOCAL int %s %ld (riga %d) ma "
                           "DELOCAL int %s %ld (riga %d): "
                           "valori iniziale e finale diversi, "
                           "verificare reversibilita'.\n",
                           proc_name,
                           dst, src_num, L->lineno,
                           dst, delocal_num, M->lineno);
                    errors++;
                }
                */

                break;
            }
        }
    }
    return errors;
}

/* ------------------------------------------------------------------ */
/*  Funzione principale                                                 */
/* ------------------------------------------------------------------ */

int vm_check_if_reversibility(const char *buffer)
{
    static CIR_Line  lines [CIR_MAX_LINES];
    static CIR_Label labels[CIR_MAX_LABELS];

    int nlines  = parse_lines(buffer, lines, CIR_MAX_LINES);
    int errors  = 0;
    int nlabels = 0;

    char proc_name[CIR_TOK_LEN] = "<globale>";
    int  in_proc = 0;

    for (int i = 0; i < nlines; i++) {
        CIR_Line *L = &lines[i];

        if (strcmp(L->op, "PROC") == 0) {
            in_proc = 1;
            if (L->argc > 0)
                strncpy(proc_name, L->arg[0], CIR_TOK_LEN - 1);
            else
                strncpy(proc_name, "?", CIR_TOK_LEN - 1);

            nlabels = 0;
            for (int j = i + 1; j < nlines; j++) {
                if (strcmp(lines[j].op, "END_PROC") == 0) break;
                if (strcmp(lines[j].op, "LABEL") == 0 &&
                    lines[j].argc > 0 &&
                    nlabels < CIR_MAX_LABELS) {
                    strncpy(labels[nlabels].name,
                            lines[j].arg[0], CIR_TOK_LEN - 1);
                    labels[nlabels].lineno = lines[j].lineno;
                    nlabels++;
                }
            }
            continue;
        }

        if (strcmp(L->op, "END_PROC") == 0) { in_proc = 0; continue; }
        if (!in_proc) continue;

        /* ---- Controllo int/stack fuori da MAIN ---- */
        if (strcmp(proc_name, "main") != 0) {
            const char *op = L->op;
            if (strcmp(op, "DECL") == 0) 
            {
                printf("[ERROR] In una funzione (%s) puoi dichiarare solo variabili local!\n (riga %d)\n",
                       proc_name,L->lineno);
                errors++;
                exit(EXIT_FAILURE);
            }
        }

        /* ---- Check blocchi if-fi e chiamate ricorsive ---- */
        if (strcmp(L->op, "EVAL") != 0 || L->argc < 2) continue;
        if (i + 1 >= nlines)                            continue;

        CIR_Line *Ljmpf = &lines[i + 1];
        if (strcmp(Ljmpf->op, "JMPF") != 0 || Ljmpf->argc < 1) continue;

        int jmpf_target = label_lineno(labels, nlabels, Ljmpf->arg[0]);
        if (jmpf_target < 0)                  continue;
        if (jmpf_target <= Ljmpf->lineno)     continue;  /* backward → loop */

        int jmpf_tgt_idx = idx_at_lineno(lines, nlines, jmpf_target);

        int is_loop = 0;
        for (int k = i + 2; k < jmpf_tgt_idx; k++) {
            if (strcmp(lines[k].op, "JMPF") == 0 && lines[k].argc > 0) {
                int kt = label_lineno(labels, nlabels, lines[k].arg[0]);
                if (kt >= 0 && kt < lines[k].lineno) {
                    is_loop = 1;
                    break;
                }
            }
        }
        if (is_loop) continue;

        int fi_tgt_lineno = jmpf_target;
        int found_else    = 0;
        for (int k = i + 2; k < jmpf_tgt_idx; k++) {
            if (strcmp(lines[k].op, "JMP") == 0 && lines[k].argc > 0) {
                int jt = label_lineno(labels, nlabels, lines[k].arg[0]);
                if (jt > jmpf_target) {
                    fi_tgt_lineno = jt;
                    found_else    = 1;
                    break;
                }
            }
        }

        int fi_tgt_idx = idx_at_lineno(lines, nlines, fi_tgt_lineno);
        const char *cond_var = L->arg[0];

        int has_recursive_call = 0;
        for (int k = i + 2; k < fi_tgt_idx; k++) {
            if (strcmp(lines[k].op, "CALL") == 0 &&
                lines[k].argc > 0 &&
                strcmp(lines[k].arg[0], proc_name) == 0) {
                has_recursive_call = 1;
                break;
            }
        }
        if (has_recursive_call) continue;

        for (int k = i + 2; k < fi_tgt_idx; k++) {
            if (strcmp(lines[k].op, "LABEL") == 0 ||
                strcmp(lines[k].op, "JMP")   == 0 ||
                strcmp(lines[k].op, "JMPF")  == 0 ||
                strcmp(lines[k].op, "EVAL")  == 0)
                continue;

            if (writes_to(&lines[k], cond_var)) {
                printf("[WARNING] La procedura \"%s\" dentro un blocco %s "
                       "ha la variabile di controllo '%s' modificata "
                       "da istruzione: %s (riga %d)\n",
                       proc_name,
                       found_else ? "if-else-fi" : "if-fi",
                       cond_var,
                       lines[k].op,
                       lines[k].lineno);
                errors++;
            }
        }
    }

    /* ---- Check LOCAL/DELOCAL reversibility ---- */
    errors += vm_check_local_src_modified(buffer);

    return errors;
}

#endif /* CHECK_IF_REVERSIBILITY_H */