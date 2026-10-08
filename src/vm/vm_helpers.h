#ifndef VM_HELPERS_H
#define VM_HELPERS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"

/* ======================================================================
 *  Helper generici
 * ====================================================================== */

/* Cresce f->vars (heap) per indicizzare almeno `idx`. Init cap = MAX_VARS:
 * per i programmi noti (var_count < MAX_VARS) il buffer è allocato una volta a
 * MAX_VARS e mai riallocato → identico all'array statico precedente (stesso
 * fast path). Oltre, raddoppia. Zero-fill della regione nuova (Var* = NULL). */
static inline void frame_ensure_vars(Frame *f, int idx)
{
    if (idx < f->vars_cap) return;
    int new_cap = f->vars_cap ? f->vars_cap : MAX_VARS;
    while (idx >= new_cap) new_cap *= 2;
    Var **nv = (Var **)realloc(f->vars, sizeof(Var *) * (size_t)new_cap);
    if (!nv) {
        fprintf(stderr, "[VM] frame_ensure_vars: realloc(%d) fallita\n", new_cap);
        exit(1);
    }
    memset(nv + f->vars_cap, 0, sizeof(Var *) * (size_t)(new_cap - f->vars_cap));
    f->vars = nv;
    f->vars_cap = new_cap;
}

/* Stessa logica (init cap = vecchio MAX, doubling oltre) per gli altri array
 * per-Frame: identici al bump statico per i programmi noti, crescono solo come
 * valvola di sicurezza. */
static inline void frame_ensure_labels(Frame *f, int idx)
{
    if (idx < f->label_cap) return;
    int nc = f->label_cap ? f->label_cap : MAX_LABEL;
    while (idx >= nc) nc *= 2;
    uint *n = (uint *)realloc(f->label, sizeof(uint) * (size_t)nc);
    if (!n) { fprintf(stderr, "[VM] frame_ensure_labels: realloc(%d) fallita\n", nc); exit(1); }
    memset(n + f->label_cap, 0, sizeof(uint) * (size_t)(nc - f->label_cap));
    f->label = n; f->label_cap = nc;
}

static inline void frame_ensure_params(Frame *f, int idx)
{
    if (idx < f->param_indices_cap) return;
    int nc = f->param_indices_cap ? f->param_indices_cap : MAX_PROC_PARAMS;
    while (idx >= nc) nc *= 2;
    int *n = (int *)realloc(f->param_indices, sizeof(int) * (size_t)nc);
    if (!n) { fprintf(stderr, "[VM] frame_ensure_params: realloc(%d) fallita\n", nc); exit(1); }
    memset(n + f->param_indices_cap, 0, sizeof(int) * (size_t)(nc - f->param_indices_cap));
    f->param_indices = n; f->param_indices_cap = nc;
}

static inline void frame_ensure_trace(Frame *f, int idx)
{
    if (idx < f->trace_window_cap) return;
    int nc = f->trace_window_cap ? f->trace_window_cap : VM_TRACE_WIN_STACK_MAX;
    while (idx >= nc) nc *= 2;
    int *n = (int *)realloc(f->trace_window_stack, sizeof(int) * (size_t)nc);
    if (!n) { fprintf(stderr, "[VM] frame_ensure_trace: realloc(%d) fallita\n", nc); exit(1); }
    memset(n + f->trace_window_cap, 0, sizeof(int) * (size_t)(nc - f->trace_window_cap));
    f->trace_window_stack = n; f->trace_window_cap = nc;
}

static inline void make_frame_key(const char *name, int depth, char *out, size_t sz)
{
    if (depth == 0) snprintf(out, sz, "%s", name);
    else            snprintf(out, sz, "%s@%d", name, depth);
}

/*
 * Chiave per cloni ricorsivi dentro THREAD_* (par): evita che due worker
 * condividano lo stesso FrameIndexer di "proc@k" del thread principale.
 * pthread_self in 8 hex + profondità restano sotto CHAR_ID_MAP_NAME_LEN per
 * nomi procedura tipici del progetto.
 */
static inline void make_frame_key_par_rec(const char *name, int depth, char *out, size_t sz)
{
    unsigned tid = (unsigned)(unsigned long)pthread_self();
    snprintf(out, sz, "%s@w%x_%d", name, tid, depth);
}

static inline void make_thread_frame_key(const char *proc, char *out, size_t sz)
{
    snprintf(out, sz, "%s@t%lu", proc, (unsigned long)pthread_self());
}

static inline uint get_findex(const char *name)
{
    /* Cammino veloce senza lock: l'indice e' append-only e pubblica ogni voce
       solo quando e' completa (vedi char_id_map_get). Il lock serve solo al
       caso raro, voce mancante. Prenderlo a ogni chiamata serializzava tutti i
       rami di un par. */
    int gia = char_id_map_lookup(&FrameIndexer, name);
    if (gia >= 0) return (uint)gia;

    pthread_mutex_lock(&var_indexer_mtx);
    int exists = char_id_map_exists(&FrameIndexer, name);
    if (!exists) {
        pthread_mutex_unlock(&var_indexer_mtx);
        vm_debug_panic("[VM] get_findex: frame '%s' non trovato!\n", name);
    }
    uint idx = char_id_map_get(&FrameIndexer, name);
    pthread_mutex_unlock(&var_indexer_mtx);
    return idx;
}

/* ======================================================================
 *  resolve_expr — valuta ricorsivamente espressioni della forma:
 *    atom      ::= ID | NUMBER
 *    expr      ::= atom | '(' expr op expr ')'
 *    op        ::= '+' | '-'
 *  Esempi: "x", "42", "(y + 1)", "((a + b) - c)"
 * ====================================================================== */
static inline int64_t resolve_expr(VM *vm, uint fi, const char *tok);

static inline Var *get_var(VM *vm, uint fi, const char *name, const char *op);
static inline int64_t *array_cell(VM *vm, uint fi, const char *tok, Var **vout, const char *op);

static inline int64_t resolve_atom(VM *vm, uint fi, const char *s)
{
    if (strchr(s, '[')) {                       /* cella di array: a[idx] */
        Var *av;
        return *array_cell(vm, fi, s, &av, "lettura");
    }
    int idx = char_id_map_lookup(&vm->frames[fi]->VarIndexer, s);
    if (idx >= 0) {
        /* Slot delocal'd ma ancora nell'indexer: evita NULL-deref (ritorna 0). */
        Var *v = vm->frames[fi]->vars[idx];
        if (v && v->value)
            return *(v->value);
        return 0;
    }
    /* strtoull preserva bit-pattern per costanti unsigned > 2^63 (es. 0x8...
     * stored come 0x8000000000000000 = INT64_MIN). strtoll saturerebbe a
     * INT64_MAX perdendo l'high bit. Per "-N" strtoull accetta segno e
     * ritorna two's complement. */
    return (int64_t)strtoull(s, NULL, 10);
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
    while (p[i] && !strchr(" )+-*/%=#<>{}&|", p[i])) {
        if (p[i] == '[') {                      /* a[...]: l'indice e' parte del token */
            int depth = 0;
            do {
                if (p[i] == '[') depth++;
                else if (p[i] == ']') depth--;
                i++;
            } while (depth > 0 && p[i]);
            continue;
        }
        i++;
    }
    return i > 0 ? i : 1;
}

static inline int64_t resolve_expr(VM *vm, uint fi, const char *tok)
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
        char left[llen + 1]; memcpy(left, inner, (size_t)llen); left[llen] = '\0';
        int64_t lval = resolve_expr(vm, fi, left);

        /* Salta l'operand e spazi */
        const char *after_left = inner + llen;
        while (*after_left == ' ') after_left++;

        /* Leggi operatore */
        char op = *after_left;
        const char *after_op = after_left + 1;
        while (*after_op == ' ') after_op++;

        /* Leggi right operand (fino alla ')' di chiusura) */
        int rlen = token_len(after_op);
        char right[rlen + 1]; memcpy(right, after_op, (size_t)rlen); right[rlen] = '\0';
        int64_t rval = resolve_expr(vm, fi, right);

        if (op == '+') return lval + rval;
        if (op == '-') return lval - rval;
        if (op == '*') return lval * rval;
        if (op == '/' || op == '%') {
            if (rval == 0)
                vm_debug_panic("[VM] Div-Err: divisione per zero (%lld %c 0)\n",
                               (long long)lval, op);
            return op == '/' ? lval / rval : lval % rval;
        }
        /* Confronti e connettivi, come in Janus: 1 vero, 0 falso. Il frontend
           codifica gli operatori di due caratteri in uno: = # { } & | */
        if (op == '=') return lval == rval;
        if (op == '#') return lval != rval;
        if (op == '<') return lval <  rval;
        if (op == '>') return lval >  rval;
        if (op == '{') return lval <= rval;
        if (op == '}') return lval >= rval;
        if (op == '&') return (lval != 0) && (rval != 0);
        if (op == '|') return (lval != 0) || (rval != 0);
        vm_debug_panic("[VM] resolve_expr: operatore sconosciuto '%c'\n", op);
    }

    /* Atom semplice */
    return resolve_atom(vm, fi, tok);
}

static inline int64_t resolve_value(VM *vm, uint fi, const char *tok)
{
    return resolve_expr(vm, fi, tok);
}

/*
 * read_rest_of_expr — legge tutto ciò che rimane sulla riga corrente
 * come unica stringa (gestisce espressioni tipo "(y + z)" che strtok
 * spezzerebbe in più token).
 */
/* Resto della riga come un'unica espressione, in un buffer della sua lunghezza. */
#define VM_REST_EXPR(name) \
    const char *name##_src = strtok(NULL, ""); \
    if (!name##_src) name##_src = ""; \
    while (*name##_src == ' ' || *name##_src == '\t') name##_src++; \
    VM_LINE_COPY(name, name##_src)

static inline void read_rest_of_expr(char *out, size_t outsz)
{
    const char *rest = strtok(NULL, "");
    if (!rest) rest = "";
    while (*rest == ' ' || *rest == '\t') rest++;
    strncpy(out, rest, outsz - 1);
    out[outsz - 1] = '\0';
}

/* Cella `a[idx]` di un array: ritorna il puntatore alla cella. L'indice e' un'espressione
   valutata nello stesso store sia in avanti sia all'indietro. Fuori dai limiti e'
   un errore (Idx-Err), non un valore convenzionale. */
static inline int64_t *array_cell(VM *vm, uint fi, const char *tok, Var **vout, const char *op)
{
    const char *lb = strchr(tok, '[');
    const char *rb = strrchr(tok, ']');
    if (!lb || !rb || rb < lb)
        vm_debug_panic("[VM] %s: cella di array malformata '%s'\n", op, tok);
    char name[VAR_NAME_LENGTH];
    size_t nl = (size_t)(lb - tok);
    if (nl >= sizeof(name)) nl = sizeof(name) - 1;
    memcpy(name, tok, nl); name[nl] = '\0';
    size_t il = (size_t)(rb - lb - 1);
    char inner[il + 1];
    memcpy(inner, lb + 1, il); inner[il] = '\0';
    Var *v = get_var(vm, fi, name, op);
    if (v->T != TYPE_ARRAY)
        vm_debug_panic("[VM] %s: '%s' non e' un array\n", op, name);
    int64_t i = resolve_expr(vm, fi, inner);
    if (i < 0 || (uint64_t)i >= (uint64_t)v->stack_len)
        vm_debug_panic("[VM] Idx-Err: indice %lld fuori dai limiti di '%s' (lunghezza %zu)\n",
                       (long long)i, name, v->stack_len);
    *vout = v;
    return &v->value[i];
}

/* Luogo di un assegnamento: una variabile int o una cella di array. */
static inline int64_t *lvalue_ptr(VM *vm, uint fi, const char *id, Var **vout, const char *op)
{
    if (strchr(id, '['))
        return array_cell(vm, fi, id, vout, op);
    Var *v = get_var(vm, fi, id, op);
    if (v->T != TYPE_INT) vm_debug_panic("[VM] %s non su INT!\n", op);
    *vout = v;
    return v->value;
}

static inline Var *get_var(VM *vm, uint fi, const char *name, const char *op)
{
    int idx = char_id_map_lookup(&vm->frames[fi]->VarIndexer, name);
    if (idx < 0) {
        vm_debug_panic("[VM] %s: variabile '%s' non definita!\n", op, name);
    }
    if (!vm->frames[fi]->vars[idx]) {
        vm_debug_panic("[VM] %s: variabile '%s' è NULL\n", op, name);
    }
    return vm->frames[fi]->vars[idx];
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

/*
 * Aggiunge sempre il prefisso fisico "NNNN" usando il numero della riga
 * nel file/stringa in ingresso.
 */
static inline char *normalize_bytecode_physical_lines(const char *input)
{
    if (!input) return NULL;
    size_t in_len = strlen(input);
    size_t out_cap = in_len * 8 + 64; /* margine ampio: prefisso su ogni riga */
    char *out = malloc(out_cap);
    if (!out) return NULL;

    size_t out_len = 0;
    unsigned line_no = 1;
    const char *cur = input;

    while (*cur) {
        const char *nl = strchr(cur, '\n');
        size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        int empty = (len == 0);

        if (!empty) {
            int w = snprintf(out + out_len, out_cap - out_len, "%04u  ", line_no);
            if (w < 0 || (size_t)w >= out_cap - out_len) { free(out); return NULL; }
            out_len += (size_t)w;
        }

        if (!empty) {
            if (out_len + len + 2 >= out_cap) { free(out); return NULL; }
            memcpy(out + out_len, cur, len);
            out_len += len;
        }

        if (nl) {
            out[out_len++] = '\n';
            cur = nl + 1;
        } else {
            break;
        }
        line_no++;
    }

    out[out_len] = '\0';
    return out;
}

static inline void delete_var(Var *vars[], int *size, int n)
{
    if (n < 0 || n >= *size) { printf("Indice fuori range!\n"); return; }
    Var *v = vars[n];
    if (v->T == TYPE_CHANNEL && v->channel) {
        pthread_mutex_lock(&v->channel->mtx);
        v->channel->refcount--;
        int do_free = (v->channel->refcount <= 0);
        pthread_mutex_unlock(&v->channel->mtx);
        if (do_free) {
            pthread_mutex_destroy(&v->channel->mtx);
            free(v->channel->buf);
            free(v->channel);
        }
    } else {
        free(v->value);
    }
    free(v);
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
        v->value = calloc(1, sizeof(int64_t));
    } else if (strcmp(type, "stack") == 0) {
        v->T         = TYPE_STACK;
        v->stack_len = 0;
        v->value     = malloc(VAR_STACK_MAX_SIZE * sizeof(int64_t));
    } else if (strcmp(type, "array") == 0) {
        /* Le celle le alloca op_local, che conosce la lunghezza. */
        v->T         = TYPE_ARRAY;
        v->stack_len = 0;
        v->value     = NULL;
    } else if (strcmp(type, "channel") == 0) {
        v->T         = TYPE_CHANNEL;
        v->stack_len = 0;
        v->value     = NULL;
        v->channel   = calloc(1, sizeof(Channel));
        pthread_mutex_init(&v->channel->mtx, NULL);
        v->channel->buf = calloc((size_t)VAR_CHANNEL_MAX_SIZE, sizeof(int64_t));
        v->channel->buf_len = 0;
        v->channel->refcount = 1;
    } else {
        vm_debug_panic("[VM] tipo non supportato\n");
    }
}

#endif /* VM_HELPERS_H */