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
        fprintf(stderr, "[VM] get_findex: frame '%s' non trovato!\n", name);
        exit(EXIT_FAILURE);
    }
    return char_id_map_get(&FrameIndexer, name);
}

static inline int resolve_value(VM *vm, uint fi, const char *tok)
{
    if (char_id_map_exists(&vm->frames[fi].VarIndexer, tok)) {
        uint idx = char_id_map_get(&vm->frames[fi].VarIndexer, tok);
        return *(vm->frames[fi].vars[idx]->value);
    }
    return (int)strtol(tok, NULL, 10);
}

static inline Var *get_var(VM *vm, uint fi, const char *name, const char *op)
{
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, name)) {
        fprintf(stderr, "[VM] %s: variabile '%s' non definita!\n", op, name);
        exit(EXIT_FAILURE);
    }
    uint idx = char_id_map_get(&vm->frames[fi].VarIndexer, name);
    if (!vm->frames[fi].vars[idx]) {
        fprintf(stderr, "[VM] %s: variabile '%s' è NULL\n", op, name);
        exit(EXIT_FAILURE);
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
    while (*line >= '0' && *line <= '9') line++;
    while (*line == ' ' || *line == '\t') line++;
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
        vm_fatal("[VM] tipo non supportato\n");
    }
}

#endif /* VM_HELPERS_H */