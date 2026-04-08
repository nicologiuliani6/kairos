#ifndef VM_HELPERS_H
#define VM_HELPERS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"

/* ======================================================================
 *  Helper generici
 * ====================================================================== */

static inline void make_frame_key(const char *name, int depth, char *out, size_t sz)
{
    if (depth == 0) snprintf(out, sz, "%s", name);
    else            snprintf(out, sz, "%s@%d", name, depth);
}

static inline void make_thread_frame_key(const char *proc, char *out, size_t sz)
{
    snprintf(out, sz, "%s@t%lu", proc, (unsigned long)pthread_self());
}

static inline uint get_findex(const char *name)
{
    if (!char_id_map_exists(&FrameIndexer, name)) {
        vm_debug_panic("[VM] get_findex: frame '%s' non trovato!\n", name);
    }
    return char_id_map_get(&FrameIndexer, name);
}

/* ======================================================================
 *  resolve_expr — valuta ricorsivamente espressioni della forma:
 *    atom      ::= ID | NUMBER
 *    expr      ::= atom | '(' expr op expr ')'
 *    op        ::= '+' | '-'
 *  Esempi: "x", "42", "(y + 1)", "((a + b) - c)"
 * ====================================================================== */
static inline int resolve_expr(VM *vm, uint fi, const char *tok);

static inline int resolve_atom(VM *vm, uint fi, const char *s)
{
    if (char_id_map_exists(&vm->frames[fi].VarIndexer, s)) {
        uint idx = char_id_map_get(&vm->frames[fi].VarIndexer, s);
        return *(vm->frames[fi].vars[idx]->value);
    }
    return (int)strtol(s, NULL, 10);
}

/*
 * Trova il token (atom o sotto-espressione parentesizzata) che inizia
 * a `p` e ne restituisce la lunghezza. `end` punta al carattere dopo.
 */
static inline int token_len(const char *p)
{
    if (*p == '(') {
        int depth = 0, i = 0;
        do {
            if (p[i] == '(') depth++;
            else if (p[i] == ')') depth--;
            i++;
        } while (depth > 0 && p[i]);
        return i;
    }
    int i = 0;
    while (p[i] && p[i] != ' ' && p[i] != ')' && p[i] != '+' && p[i] != '-')
        i++;
    return i > 0 ? i : 1;
}

static inline int resolve_expr(VM *vm, uint fi, const char *tok)
{
    /* Salta spazi iniziali */
    while (*tok == ' ') tok++;

    /* Espressione parentesizzata: (left op right) */
    if (*tok == '(') {
        /* Salta '(' iniziale */
        const char *inner = tok + 1;
        while (*inner == ' ') inner++;

        /* Leggi left operand */
        int llen = token_len(inner);
        char left[256]; strncpy(left, inner, llen < 255 ? llen : 255); left[llen] = '\0';
        int lval = resolve_expr(vm, fi, left);

        /* Salta l'operand e spazi */
        const char *after_left = inner + llen;
        while (*after_left == ' ') after_left++;

        /* Leggi operatore */
        char op = *after_left;
        const char *after_op = after_left + 1;
        while (*after_op == ' ') after_op++;

        /* Leggi right operand (fino alla ')' di chiusura) */
        int rlen = token_len(after_op);
        char right[256]; strncpy(right, after_op, rlen < 255 ? rlen : 255); right[rlen] = '\0';
        int rval = resolve_expr(vm, fi, right);

        if (op == '+') return lval + rval;
        if (op == '-') return lval - rval;
        vm_debug_panic("[VM] resolve_expr: operatore sconosciuto '%c'\n", op);
    }

    /* Atom semplice */
    return resolve_atom(vm, fi, tok);
}

static inline int resolve_value(VM *vm, uint fi, const char *tok)
{
    return resolve_expr(vm, fi, tok);
}

/*
 * read_rest_of_expr — legge tutto ciò che rimane sulla riga corrente
 * come unica stringa (gestisce espressioni tipo "(y + z)" che strtok
 * spezzerebbe in più token).
 */
static inline void read_rest_of_expr(char *out, size_t outsz)
{
    const char *rest = strtok(NULL, "");
    if (!rest) rest = "";
    while (*rest == ' ' || *rest == '\t') rest++;
    strncpy(out, rest, outsz - 1);
    out[outsz - 1] = '\0';
}

static inline Var *get_var(VM *vm, uint fi, const char *name, const char *op)
{
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, name)) {
        vm_debug_panic("[VM] %s: variabile '%s' non definita!\n", op, name);
    }
    uint idx = char_id_map_get(&vm->frames[fi].VarIndexer, name);
    if (!vm->frames[fi].vars[idx]) {
        vm_debug_panic("[VM] %s: variabile '%s' è NULL\n", op, name);
    }
    return vm->frames[fi].vars[idx];
}

static inline char *go_to_line(char *buf, uint line)
{
    if (!buf || line == 0) return buf;
    uint cur = 1;
    for (char *p = buf; *p; p++) {
        if (cur == line) return p;
        if (*p == '\n') cur++;
    }
    return NULL;
}

static inline char *skip_lineno(char *line)
{
    /* Salta i 4 digit fisici */
    while (*line >= '0' && *line <= '9') line++;
    /* Salta spazi */
    while (*line == ' ' || *line == '\t') line++;
    /* Salta il tag sorgente @N se presente */
    if (*line == '@') {
        line++;
        while (*line >= '0' && *line <= '9') line++;
        while (*line == ' ' || *line == '\t') line++;
    }
    return line;
}

static inline void delete_var(Var *vars[], int *size, int n)
{
    if (n < 0 || n >= *size) { printf("Indice fuori range!\n"); return; }
    free(vars[n]->value);
    free(vars[n]);
    vars[n] = NULL;
}

/* ======================================================================
 *  alloc_var — usata da op_local e vm_exec
 * ====================================================================== */

static inline void alloc_var(Var *v, const char *type, const char *name)
{
    memset(v, 0, sizeof(Var));
    strncpy(v->name, name, VAR_NAME_LENGTH - 1);
    v->is_local = 1;

    if (strcmp(type, "int") == 0) {
        v->T     = TYPE_INT;
        v->value = calloc(1, sizeof(int));
    } else if (strcmp(type, "stack") == 0) {
        v->T         = TYPE_STACK;
        v->stack_len = 0;
        v->value     = malloc(VAR_STACK_MAX_SIZE * sizeof(int));
    } else if (strcmp(type, "channel") == 0) {
        v->T         = TYPE_CHANNEL;
        v->stack_len = 0;
        v->value     = malloc(VAR_CHANNEL_MAX_SIZE * sizeof(int));
        v->channel   = calloc(1, sizeof(Channel));
        pthread_mutex_init(&v->channel->mtx, NULL);
    } else {
        vm_debug_panic("[VM] tipo non supportato\n");
    }
}

#endif /* VM_HELPERS_H */