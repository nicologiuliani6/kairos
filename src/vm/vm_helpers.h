#ifndef VM_HELPERS_H
#define VM_HELPERS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"

/* ======================================================================
 *  Helper generici
 * ====================================================================== */

/* Cresce f->vars per indicizzare almeno `idx`. La prima allocazione è
 * dimensionata su names_hint (vedi Frame): in esecuzione normale il vettore
 * non cresce più. Quando cresce, il blocco vecchio non si libera (vm_grow_keep):
 * i rami di un par girano nello stesso frame e leggono vars[] senza lock.
 * Zero-fill della regione nuova (Var* = NULL). */
static inline void frame_ensure_vars(Frame *f, int idx)
{
    if (idx < f->vars_cap) return;
    int new_cap = f->vars_cap ? f->vars_cap * 2 : FRAME_VARS_INIT_CAP;
    if (new_cap < f->names_hint) new_cap = f->names_hint;
    while (idx >= new_cap) new_cap *= 2;
    Var **nv = (Var **)vm_grow_keep(f->vars, sizeof(Var *) * (size_t)f->vars_cap,
                                    sizeof(Var *) * (size_t)new_cap);
    __atomic_store_n(&f->vars, nv, __ATOMIC_RELEASE);
    f->vars_cap = new_cap;
}

/* Stessa logica (raddoppio da una capacità iniziale piccola) per gli altri
 * array per-Frame, che si scrivono da un thread solo. */
static inline void frame_ensure_labels(Frame *f, int idx)
{
    if (idx < f->label_cap) return;
    int nc = f->label_cap ? f->label_cap : FRAME_LABEL_INIT_CAP;
    while (idx >= nc) nc *= 2;
    uint *n = (uint *)realloc(f->label, sizeof(uint) * (size_t)nc);
    if (!n) { fprintf(stderr, "[VM] frame_ensure_labels: realloc(%d) failed\n", nc); exit(1); }
    memset(n + f->label_cap, 0, sizeof(uint) * (size_t)(nc - f->label_cap));
    f->label = n; f->label_cap = nc;
}

static inline void frame_ensure_params(Frame *f, int idx)
{
    if (idx < f->param_indices_cap) return;
    int nc = f->param_indices_cap ? f->param_indices_cap : FRAME_PARAMS_INIT_CAP;
    while (idx >= nc) nc *= 2;
    int *n = (int *)realloc(f->param_indices, sizeof(int) * (size_t)nc);
    if (!n) { fprintf(stderr, "[VM] frame_ensure_params: realloc(%d) failed\n", nc); exit(1); }
    memset(n + f->param_indices_cap, 0, sizeof(int) * (size_t)(nc - f->param_indices_cap));
    f->param_indices = n; f->param_indices_cap = nc;
}

/* Rimpiazza una stringa posseduta (nome di frame, di variabile). */
static inline void vm_str_replace(char **dst, const char *src)
{
    char *n = char_id_strdup(src);
    free(*dst);
    *dst = n;
}

/* Vettore di Var* per salvare i parametri attorno a una chiamata: sullo stack
 * del C finché sono pochi, sull'heap oltre. Nessun numero massimo di
 * parametri. Uso: VM_PARAM_SAVE(sv, pc); ... VM_PARAM_SAVE_FREE(sv); */
#define VM_PARAM_SAVE_SMALL 16
#define VM_PARAM_SAVE(name, n) \
    Var *name##_small[VM_PARAM_SAVE_SMALL]; \
    Var **name = ((n) <= VM_PARAM_SAVE_SMALL) ? name##_small \
                 : (Var **)malloc(sizeof(Var *) * (size_t)(n)); \
    if (!name) vm_debug_panic("[VM] parameters: out of memory\n")
#define VM_PARAM_SAVE_FREE(name) \
    do { if (name != name##_small) free(name); } while (0)

static inline void make_frame_key(const char *name, int depth, char *out, size_t sz)
{
    if (depth == 0) snprintf(out, sz, "%s", name);
    else            snprintf(out, sz, "%s@%d", name, depth);
}

/*
 * Chiave per cloni ricorsivi dentro THREAD_* (par): evita che due worker
 * condividano lo stesso FrameIndexer di "proc@k" del thread principale.
 * Il buffer `out` lo dimensiona il chiamante con VM_FRAME_KEY_BUF (nome della
 * procedura più il suffisso numerico): nessuna lunghezza massima.
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

/* Il frame in cui il thread sta eseguendo: vm_run_BT lo pubblica prima di
 * ogni istruzione (nome = il suo buffer, vivo finche' lo pubblica) e lo
 * ritira all'uscita, cosi' le istruzioni non ricercano il nome nella tabella
 * dei frame. Un altro nome, o un altro buffer, va per la via normale. */
static __thread const char *tl_fx_nome = NULL;
static __thread uint        tl_fx_fi   = 0;

static inline uint get_findex(const char *name)
{
    if (name == tl_fx_nome) return tl_fx_fi;
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
        vm_debug_panic("[VM] get_findex: frame '%s' not found\n", name);
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
static inline int64_t *lvalue_ptr(VM *vm, uint fi, const char *id, Var **vout, const char *op);
static inline int64_t *array_cell(VM *vm, uint fi, const char *tok, Var **vout, const char *op);

static inline int64_t resolve_atom(VM *vm, uint fi, const char *s)
{
    if (strchr(s, '[')) {                       /* cella di array: a[idx] */
        Var *av;
        return *array_cell(vm, fi, s, &av, "lettura");
    }
    int idx = char_id_map_lookup(&vm->frames[fi]->VarIndexer, s);
    if (idx >= 0) {
        /* Slot già delocal'd ma ancora nell'indexer. */
        Var *v = vm->frames[fi]->vars[idx];
        if (v && v->value)
            return *(v->value);
        vm_debug_panic("[VM] read: variable '%s' outside its local/delocal block\n", s);
    }
    const char *c = (*s == '-') ? s + 1 : s;
    if (!(*c >= '0' && *c <= '9'))
        vm_debug_panic("[VM] read: variable '%s' is not defined\n", s);
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

static inline int64_t apply_binop(char op, int64_t lval, int64_t rval)
{
    /* Interi a 64 bit in complemento a due: + - * modulo 2^64 (in C
       l'overflow con segno non e' definito, quindi si calcola senza segno). */
    if (op == '+') return (int64_t)((uint64_t)lval + (uint64_t)rval);
    if (op == '-') return (int64_t)((uint64_t)lval - (uint64_t)rval);
    if (op == '*') return (int64_t)((uint64_t)lval * (uint64_t)rval);
    if (op == '/' || op == '%') {
        if (rval == 0)
            vm_debug_panic("[VM] Div-Err: division by zero (%lld %c 0)\n",
                           (long long)lval, op);
        if (rval == -1)   /* INT64_MIN / -1 trabocca: anche qui modulo 2^64 */
            return op == '/' ? (int64_t)(0 - (uint64_t)lval) : 0;
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
    vm_debug_panic("[VM] resolve_expr: unknown operator '%c'\n", op);
    return 0;
}

/* Valuta l'espressione che comincia in *pp e avanza *pp oltre la sua fine.
 * Lavora sul testo senza copiarlo: lo stack del C cresce solo con
 * l'annidamento delle parentesi, non con la lunghezza dell'espressione. */
static int64_t eval_expr_at(VM *vm, uint fi, const char **pp)
{
    const char *p = *pp;
    while (*p == ' ') p++;
    if (*p == '(') {
        p++;
        int64_t lval = eval_expr_at(vm, fi, &p);
        while (*p == ' ') p++;
        char op = *p ? *p++ : '\0';
        int64_t rval = eval_expr_at(vm, fi, &p);
        while (*p == ' ') p++;
        if (*p == ')') p++;
        *pp = p;
        return apply_binop(op, lval, rval);
    }
    /* atomo: numero (anche negativo), variabile o cella a[...] */
    int len = (*p == '-') ? 1 + token_len(p + 1) : token_len(p);
    char small[64];
    char *atom = (len < (int)sizeof small) ? small : (char *)malloc((size_t)len + 1);
    if (!atom) vm_debug_panic("[VM] out of memory\n");
    memcpy(atom, p, (size_t)len); atom[len] = '\0';
    int64_t v = resolve_atom(vm, fi, atom);
    if (atom != small) free(atom);
    *pp = p + len;
    return v;
}

/* Valutazione sul testo, com'era: resta come riferimento del valutatore
 * compilato (vm_cexpr, in fondo al file), che ne ripete parsing e messaggi. */
static inline int64_t resolve_expr_testo(VM *vm, uint fi, const char *tok)
{
    return eval_expr_at(vm, fi, &tok);
}

static inline int64_t resolve_value(VM *vm, uint fi, const char *tok);

/*
 * Resto della riga corrente come un'unica espressione (gestisce espressioni
 * tipo "(y + z)" che strtok spezzerebbe in più token), in un buffer della sua
 * lunghezza.
 */
#define VM_REST_EXPR(name) \
    const char *name##_src = strtok(NULL, ""); \
    if (!name##_src) name##_src = ""; \
    while (*name##_src == ' ' || *name##_src == '\t') name##_src++; \
    const char *name = name##_src   /* la riga e' gia' una copia privata: niente seconda copia */

/* Cella `a[idx]` di un array: ritorna il puntatore alla cella. L'indice e' un'espressione
   valutata nello stesso store sia in avanti sia all'indietro. Fuori dai limiti e'
   un errore (Idx-Err), non un valore convenzionale. */
static inline int64_t *array_cell(VM *vm, uint fi, const char *tok, Var **vout, const char *op)
{
    const char *lb = strchr(tok, '[');
    const char *rb = strrchr(tok, ']');
    if (!lb || !rb || rb < lb)
        vm_debug_panic("[VM] %s: malformed array cell '%s'\n", op, tok);
    size_t nl = (size_t)(lb - tok);
    char name[nl + 1];
    memcpy(name, tok, nl); name[nl] = '\0';
    size_t il = (size_t)(rb - lb - 1);
    char inner[il + 1];
    memcpy(inner, lb + 1, il); inner[il] = '\0';
    Var *v = get_var(vm, fi, name, op);
    if (v->T != TYPE_ARRAY)
        vm_debug_panic("[VM] %s: '%s' is not an array\n", op, name);
    int64_t i = resolve_expr(vm, fi, inner);
    if (i < 0 || (uint64_t)i >= (uint64_t)v->stack_len)
        vm_debug_panic("[VM] Idx-Err: index %lld out of the bounds of '%s' (length %zu)\n",
                       (long long)i, name, v->stack_len);
    *vout = v;
    return &v->value[i];
}

/* Luogo di un assegnamento: una variabile int o una cella di array. */
static inline int64_t *lvalue_ptr_testo(VM *vm, uint fi, const char *id, Var **vout, const char *op)
{
    if (strchr(id, '['))
        return array_cell(vm, fi, id, vout, op);
    Var *v = get_var(vm, fi, id, op);
    if (v->T != TYPE_INT) vm_debug_panic("[VM] %s on a non-int\n", op);
    *vout = v;
    return v->value;
}

static inline Var *get_var(VM *vm, uint fi, const char *name, const char *op)
{
    int idx = char_id_map_lookup(&vm->frames[fi]->VarIndexer, name);
    if (idx < 0) {
        vm_debug_panic("[VM] %s: variable '%s' is not defined\n", op, name);
    }
    if (!vm->frames[fi]->vars[idx]) {
        vm_debug_panic("[VM] %s: variable '%s' is NULL\n", op, name);
    }
    return vm->frames[fi]->vars[idx];
}

/* ----------------------------------------------------------------------
 *  Indice delle righe del programma
 *
 *  Salti, chiamate e inversioni ritrovano una riga per numero. Contarla
 *  scorrendo il testo dall'inizio costava quanto la sua posizione (circa
 *  0.01 us per riga, a ogni salto). Il testo del programma non cambia mai
 *  durante l'esecuzione: prima di eseguirlo se ne registrano gli inizi di
 *  riga, e go_to_line diventa un accesso a un vettore. Per un puntatore che
 *  non sta nel programma indicizzato resta la scansione di prima.
 * ---------------------------------------------------------------------- */
/* Codice dell'istruzione di una riga: il ciclo principale lo legge invece di
 * confrontare la parola con ogni nome di istruzione. */
enum {
    OPC_UNKNOWN = 0, OPC_END_PROC, OPC_CALL, OPC_UNCALL, OPC_PAR_START, OPC_LOCAL, OPC_DELOCAL,
    OPC_SHOW, OPC_PUSHEQ, OPC_MINEQ, OPC_XOREQ, OPC_SWAP, OPC_PUSH, OPC_POP, OPC_SSEND, OPC_SRECV,
    OPC_EVAL, OPC_ASSERT, OPC_JMPF, OPC_JMP, OPC_LABEL, OPC_START, OPC_PROC, OPC_PARAM, OPC_DECL,
    OPC_HALT, OPC_THREAD, OPC_PAR_END, OPC_VUOTA = 255
};

static inline int vm_opc_n(const char *w, size_t n)
{
#define VM_OPC_IS(s) (n == sizeof(s) - 1 && !memcmp(w, s, n))
    if (VM_OPC_IS("PUSHEQ"))    return OPC_PUSHEQ;
    if (VM_OPC_IS("MINEQ"))     return OPC_MINEQ;
    if (VM_OPC_IS("EVAL"))      return OPC_EVAL;
    if (VM_OPC_IS("JMPF"))      return OPC_JMPF;
    if (VM_OPC_IS("JMP"))       return OPC_JMP;
    if (VM_OPC_IS("LABEL"))     return OPC_LABEL;
    if (VM_OPC_IS("LOCAL"))     return OPC_LOCAL;
    if (VM_OPC_IS("DELOCAL"))   return OPC_DELOCAL;
    if (VM_OPC_IS("ASSERT"))    return OPC_ASSERT;
    if (VM_OPC_IS("CALL"))      return OPC_CALL;
    if (VM_OPC_IS("UNCALL"))    return OPC_UNCALL;
    if (VM_OPC_IS("END_PROC"))  return OPC_END_PROC;
    if (VM_OPC_IS("SWAP"))      return OPC_SWAP;
    if (VM_OPC_IS("PUSH"))      return OPC_PUSH;
    if (VM_OPC_IS("POP"))       return OPC_POP;
    if (VM_OPC_IS("XOREQ"))     return OPC_XOREQ;
    if (VM_OPC_IS("SHOW"))      return OPC_SHOW;
    if (VM_OPC_IS("SSEND"))     return OPC_SSEND;
    if (VM_OPC_IS("SRECV"))     return OPC_SRECV;
    if (VM_OPC_IS("PAR_START")) return OPC_PAR_START;
    if (VM_OPC_IS("PAR_END"))   return OPC_PAR_END;
    if (VM_OPC_IS("START"))     return OPC_START;
    if (VM_OPC_IS("PROC"))      return OPC_PROC;
    if (VM_OPC_IS("PARAM"))     return OPC_PARAM;
    if (VM_OPC_IS("DECL"))      return OPC_DECL;
    if (VM_OPC_IS("HALT"))      return OPC_HALT;
    if (n >= 7 && !memcmp(w, "THREAD_", 7)) return OPC_THREAD;
#undef VM_OPC_IS
    return OPC_UNKNOWN;
}

static inline int vm_opc(const char *w) { return vm_opc_n(w, strlen(w)); }

typedef struct {
    const char *base;   /* testo indicizzato, NULL = nessun indice */
    size_t      len;    /* strlen(base) */
    size_t     *off;    /* off[k] = inizio della riga k + 1, k < n */
    size_t     *fine;   /* fine[k] = posizione del '\n' della riga, SIZE_MAX se manca */
    size_t     *arg;    /* arg[k] = posizione subito dopo la prima parola */
    uint8_t    *op;     /* op[k] = OPC_* della prima parola, OPC_VUOTA se non ce n'e' */
    uint        n;      /* righe che iniziano prima della fine del testo */
} VmLineIndex;

static VmLineIndex g_line_index;

static inline void vm_line_index_build(const char *buf)
{
    size_t len = strlen(buf), n = 1;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n' && i + 1 < len) n++;
    size_t *off = (size_t *)malloc(sizeof(size_t) * n);
    size_t *fine = (size_t *)malloc(sizeof(size_t) * n);
    size_t *arg = (size_t *)malloc(sizeof(size_t) * n);
    uint8_t *op = (uint8_t *)malloc(n);
    if (!off || !fine || !arg || !op) {          /* senza indice si scorre */
        free(off); free(fine); free(arg); free(op); return;
    }
    size_t k = 0;
    off[k++] = 0;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n' && i + 1 < len) off[k++] = i + 1;
    for (k = 0; k < n; k++) {
        const char *p = buf + off[k];
        const char *nl = strchr(p, '\n');
        fine[k] = nl ? (size_t)(nl - buf) : SIZE_MAX;
        /* come skip_lineno e strtok(" \t") sulla copia della riga */
        while (*p >= '0' && *p <= '9') p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '@') {
            p++;
            while (*p >= '0' && *p <= '9') p++;
            while (*p == ' ' || *p == '\t') p++;
        }
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        op[k] = (e == p) ? (uint8_t)OPC_VUOTA : (uint8_t)vm_opc_n(p, (size_t)(e - p));
        arg[k] = (size_t)(e - buf);
    }
    g_line_index.off  = off;
    g_line_index.fine = fine;
    g_line_index.arg  = arg;
    g_line_index.op   = op;
    g_line_index.n    = (uint)n;
    g_line_index.len  = len;
    g_line_index.base = buf;
}

static inline void vm_line_index_free(void)
{
    free(g_line_index.off);
    free(g_line_index.fine);
    free(g_line_index.arg);
    free(g_line_index.op);
    memset(&g_line_index, 0, sizeof g_line_index);
}

/* Riga (da 0) che contiene la posizione q del testo indicizzato. */
static inline uint vm_line_of(size_t q)
{
    uint lo = 0, hi = g_line_index.n - 1;
    while (lo < hi) {
        uint mid = lo + (hi - lo + 1) / 2;
        if (g_line_index.off[mid] <= q) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* La riga `line` contando da buf (buf e' la riga 1), o NULL se il testo
 * finisce prima: stessa semantica della scansione, che resta per i buffer
 * non indicizzati. */
static inline char *go_to_line(char *buf, uint line)
{
    if (!buf || line == 0) return buf;
    const char *b = g_line_index.base;
    if (b && buf >= b && buf < b + g_line_index.len) {
        if (line == 1) return buf;
        size_t q = (size_t)(buf - b);
        uint first = (buf == b) ? 0 : vm_line_of(q);
        uint want = first + (line - 1);
        if (want >= g_line_index.n) return NULL;
        return (char *)b + g_line_index.off[want];
    }
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
    if (n < 0 || n >= *size) { printf("Index out of range\n"); return; }
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
    free(v->name);
    free(v);
    vars[n] = NULL;
}

/* ======================================================================
 *  Stack di valori di una Var: capacità separata dalla lunghezza, cresce
 *  raddoppiando. Nessun numero massimo di elementi. Il pop non rialloca:
 *  la capacità resta per i push successivi.
 * ====================================================================== */

static inline void var_stack_reserve(Var *v, size_t n)
{
    if (n <= v->stack_cap && v->value) return;
    size_t nc = v->stack_cap ? v->stack_cap : VAR_STACK_INIT_CAP;
    while (nc < n) nc *= 2;
    int64_t *nv = (int64_t *)realloc(v->value, nc * sizeof(int64_t));
    if (!nv) vm_debug_panic("[VM] stack: out of memory (%zu cells)\n", nc);
    v->value     = nv;
    v->stack_cap = nc;
}

static inline void var_stack_push(Var *v, int64_t val)
{
    if (v->stack_len >= v->stack_cap || !v->value) var_stack_reserve(v, v->stack_len + 1);
    v->value[v->stack_len++] = val;
}

/* ======================================================================
 *  alloc_var — usata da op_local e vm_exec
 * ====================================================================== */

static inline void alloc_var(Var *v, const char *type, const char *name)
{
    memset(v, 0, sizeof(Var));
    v->name     = char_id_strdup(name);
    v->is_local = 1;

    if (strcmp(type, "int") == 0) {
        v->T     = TYPE_INT;
        v->value = calloc(1, sizeof(int64_t));
    } else if (strcmp(type, "stack") == 0) {
        v->T         = TYPE_STACK;
        v->stack_len = 0;
        v->stack_cap = 0;
        var_stack_reserve(v, VAR_STACK_INIT_CAP);   /* cresce con i push */
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
        v->channel->buf = NULL;     /* lo allocano ssend/push, della misura del messaggio */
        v->channel->buf_len = 0;
        v->channel->refcount = 1;
    } else {
        vm_debug_panic("[VM] unsupported type\n");
    }
}


/* ======================================================================
 *  vm_cexpr: espressioni e luoghi compilati una volta per testo
 *
 *  Valutare un'espressione sul testo voleva dire, a ogni esecuzione,
 *  scorrerla carattere per carattere, copiare ogni atomo, cercare ogni nome
 *  nella tabella del frame e convertire ogni numero. Qui il testo si analizza
 *  una volta, con lo stesso algoritmo di eval_expr_at, in un piccolo albero;
 *  gli alberi stanno in una tabella per thread indicizzata dal testo. Ogni
 *  nodo con un nome ricorda l'indice trovato nell'ultimo frame (frame, gen
 *  del frame, esecuzione): la tabella dei nomi di un frame cresce soltanto,
 *  quindi un indice trovato resta valido finche' il frame non viene
 *  reinizializzato, e allora cambia gen.
 *
 *  La semantica e' quella del valutatore sul testo, errori compresi: un
 *  nome indefinito, una cella malformata, un indice fuori dai limiti fermano
 *  la VM quando l'espressione si valuta, con gli stessi messaggi.
 * ====================================================================== */

enum { CN_NUM, CN_VAR, CN_ARR, CN_BIN, CN_MALE };

typedef struct CNode {
    int           k;
    char          op;           /* CN_BIN */
    int64_t       num;          /* CN_NUM */
    char         *name;         /* nome (CN_VAR, CN_ARR), token (CN_NUM, CN_MALE) */
    struct CNode *a, *b;        /* CN_BIN: operandi; CN_ARR: indice in a */
    unsigned      c_run, c_gen, c_fi;
    int           c_idx;
} CNode;

static inline CNode *cn_nuovo(int k)
{
    CNode *n = (CNode *)calloc(1, sizeof(CNode));
    if (!n) vm_debug_panic("[VM] out of memory\n");
    n->k = k;
    return n;
}

static inline char *cn_copia(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (!d) vm_debug_panic("[VM] out of memory\n");
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static CNode *cn_compila(const char *testo);

/* Un atomo, come resolve_atom: cella di array se contiene '[', numero se
 * (dopo un '-') comincia con una cifra, altrimenti un nome. */
static CNode *cn_atomo(char *atom)
{
    if (strchr(atom, '[')) {
        const char *lb = strchr(atom, '[');
        const char *rb = strrchr(atom, ']');
        if (!lb || !rb || rb < lb) {
            CNode *n = cn_nuovo(CN_MALE);
            n->name = atom;
            return n;
        }
        CNode *n = cn_nuovo(CN_ARR);
        n->name = cn_copia(atom, (size_t)(lb - atom));
        char *inner = cn_copia(lb + 1, (size_t)(rb - lb - 1));
        n->a = cn_compila(inner);
        free(inner);
        free(atom);
        return n;
    }
    const char *c = (*atom == '-') ? atom + 1 : atom;
    if (*c >= '0' && *c <= '9') {
        CNode *n = cn_nuovo(CN_NUM);
        n->num = (int64_t)strtoull(atom, NULL, 10);
        n->name = atom;
        return n;
    }
    CNode *n = cn_nuovo(CN_VAR);
    n->name = atom;
    return n;
}

/* Lo stesso cammino di eval_expr_at. */
static CNode *cn_compila_at(const char **pp)
{
    const char *p = *pp;
    while (*p == ' ') p++;
    if (*p == '(') {
        p++;
        CNode *l = cn_compila_at(&p);
        while (*p == ' ') p++;
        char op = *p ? *p++ : '\0';
        CNode *r = cn_compila_at(&p);
        while (*p == ' ') p++;
        if (*p == ')') p++;
        *pp = p;
        CNode *n = cn_nuovo(CN_BIN);
        n->op = op; n->a = l; n->b = r;
        return n;
    }
    int len = (*p == '-') ? 1 + token_len(p + 1) : token_len(p);
    size_t resto = strlen(p);
    size_t n = (size_t)len < resto ? (size_t)len : resto;   /* mai oltre la fine del testo */
    *pp = p + n;
    return cn_atomo(cn_copia(p, n));
}

static CNode *cn_compila(const char *testo)
{
    const char *p = testo;
    return cn_compila_at(&p);
}

/* Un luogo, come lvalue_ptr: cella di array se contiene '[', altrimenti un
 * nome (anche se sembra un numero: get_var lo rifiuta). */
static CNode *cn_compila_luogo(const char *id)
{
    if (strchr(id, '['))
        return cn_atomo(cn_copia(id, strlen(id)));
    CNode *n = cn_nuovo(CN_VAR);
    n->name = cn_copia(id, strlen(id));
    return n;
}

static void cn_libera(CNode *n)
{
    if (!n) return;
    cn_libera(n->a);
    cn_libera(n->b);
    free(n->name);
    free(n);
}

/* --- tabelle per thread: testo -> albero --- */

typedef struct { uint32_t h, len; char *key; CNode *n; } CSlot;
typedef struct { CSlot *s; uint32_t mask, count; } CTab;

static __thread CTab tl_cn_espr, tl_cn_luogo;

/* La riga che il thread sta eseguendo, copiata in [tl_riga_copia,
 * tl_riga_fine) da tl_riga_orig nel testo del programma (che non cambia):
 * un testo che sta nella copia si riconosce dal suo indirizzo nel programma,
 * senza calcolarne l'hash. Un testo altrove passa dall'hash del contenuto. */
static __thread const char *tl_riga_copia = NULL, *tl_riga_fine = NULL, *tl_riga_orig = NULL;

typedef struct { const char *key; size_t len; CNode *n; } CPSlot;
typedef struct { CPSlot *s; uint32_t mask, count; unsigned run; } CPTab;
static __thread CPTab tl_cp_espr, tl_cp_luogo;

static inline uint32_t cp_hash(const char *p)
{
    uint64_t x = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(x >> 32);
}

static void cp_tab_cresci(CPTab *t)
{
    uint32_t nc = t->s ? (t->mask + 1) * 2 : 1024;
    CPSlot *ns = (CPSlot *)calloc(nc, sizeof(CPSlot));
    if (!ns) vm_debug_panic("[VM] out of memory\n");
    if (t->s) {
        for (uint32_t i = 0; i <= t->mask; i++) {
            if (!t->s[i].key) continue;
            uint32_t j = cp_hash(t->s[i].key) & (nc - 1);
            while (ns[j].key) j = (j + 1) & (nc - 1);
            ns[j] = t->s[i];
        }
        free(t->s);
    }
    t->s = ns;
    t->mask = nc - 1;
}

static void cp_tab_libera(CPTab *t)
{
    free(t->s);                      /* i nodi appartengono alle tabelle per contenuto */
    memset(t, 0, sizeof *t);
}

static inline uint32_t cn_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static void cn_tab_cresci(CTab *t)
{
    uint32_t nc = t->s ? (t->mask + 1) * 2 : 256;
    CSlot *ns = (CSlot *)calloc(nc, sizeof(CSlot));
    if (!ns) vm_debug_panic("[VM] out of memory\n");
    if (t->s) {
        for (uint32_t i = 0; i <= t->mask; i++) {
            if (!t->s[i].key) continue;
            uint32_t j = t->s[i].h & (nc - 1);
            while (ns[j].key) j = (j + 1) & (nc - 1);
            ns[j] = t->s[i];
        }
        free(t->s);
    }
    t->s = ns;
    t->mask = nc - 1;
}

static inline CNode *cn_cerca_testo(CTab *t, const char *testo, int luogo)
{
    size_t len = strlen(testo);
    uint32_t h = cn_hash(testo, len);
    if (t->s) {
        uint32_t i = h & t->mask;
        while (t->s[i].key) {
            if (t->s[i].h == h && t->s[i].len == len && !memcmp(t->s[i].key, testo, len))
                return t->s[i].n;
            i = (i + 1) & t->mask;
        }
    }
    if (!t->s || 2 * (t->count + 1) > t->mask + 1) cn_tab_cresci(t);
    CNode *n = luogo ? cn_compila_luogo(testo) : cn_compila(testo);
    uint32_t i = h & t->mask;
    while (t->s[i].key) i = (i + 1) & t->mask;
    t->s[i].h = h;
    t->s[i].len = (uint32_t)len;
    t->s[i].key = cn_copia(testo, len);
    t->s[i].n = n;
    t->count++;
    return n;
}

static inline CNode *cn_cerca(CTab *t, const char *testo, int luogo)
{
    if (testo >= tl_riga_copia && testo < tl_riga_fine && tl_riga_copia) {
        CPTab *pt = luogo ? &tl_cp_luogo : &tl_cp_espr;
        if (pt->run != g_vm_run_id) {        /* indirizzi di un altro programma */
            cp_tab_libera(pt);
            pt->run = g_vm_run_id;
        }
        const char *key = tl_riga_orig + (testo - tl_riga_copia);
        size_t len = strlen(testo);
        if (pt->s) {
            uint32_t i = cp_hash(key) & pt->mask;
            while (pt->s[i].key) {
                if (pt->s[i].key == key && pt->s[i].len == len) return pt->s[i].n;
                i = (i + 1) & pt->mask;
            }
        }
        CNode *n = cn_cerca_testo(t, testo, luogo);
        if (!pt->s || 2 * (pt->count + 1) > pt->mask + 1) cp_tab_cresci(pt);
        uint32_t i = cp_hash(key) & pt->mask;
        while (pt->s[i].key) i = (i + 1) & pt->mask;
        pt->s[i].key = key;
        pt->s[i].len = len;
        pt->s[i].n = n;
        pt->count++;
        return n;
    }
    return cn_cerca_testo(t, testo, luogo);
}

static void cn_tab_libera(CTab *t)
{
    if (t->s) {
        for (uint32_t i = 0; i <= t->mask; i++) {
            if (!t->s[i].key) continue;
            free(t->s[i].key);
            cn_libera(t->s[i].n);
        }
        free(t->s);
    }
    memset(t, 0, sizeof *t);
}

/* Alla fine di un'esecuzione o di un thread di par: le tabelle sono __thread. */
static inline void vm_cexpr_free_cache(void)
{
    cp_tab_libera(&tl_cp_espr);
    cp_tab_libera(&tl_cp_luogo);
    cn_tab_libera(&tl_cn_espr);
    cn_tab_libera(&tl_cn_luogo);
}

/* --- valutazione --- */

static inline int cn_indice(VM *vm, uint fi, CNode *n)
{
    Frame *f = vm->frames[fi];
    unsigned run = g_vm_run_id;
    if (n->c_run == run && n->c_fi == fi && n->c_gen == f->gen) return n->c_idx;
    int idx = char_id_map_lookup(&f->VarIndexer, n->name);
    if (idx >= 0) {
        n->c_run = run; n->c_fi = fi; n->c_gen = f->gen; n->c_idx = idx;
    }
    return idx;
}

static int64_t cn_valuta(VM *vm, uint fi, CNode *n);

/* Come array_cell, dopo il parsing. */
static inline int64_t *cn_cella(VM *vm, uint fi, CNode *n, Var **vout, const char *op)
{
    if (n->k == CN_MALE)
        vm_debug_panic("[VM] %s: malformed array cell '%s'\n", op, n->name);
    int idx = cn_indice(vm, fi, n);
    if (idx < 0)
        vm_debug_panic("[VM] %s: variable '%s' is not defined\n", op, n->name);
    Var *v = vm->frames[fi]->vars[idx];
    if (!v)
        vm_debug_panic("[VM] %s: variable '%s' is NULL\n", op, n->name);
    if (v->T != TYPE_ARRAY)
        vm_debug_panic("[VM] %s: '%s' is not an array\n", op, n->name);
    int64_t i = cn_valuta(vm, fi, n->a);
    if (i < 0 || (uint64_t)i >= (uint64_t)v->stack_len)
        vm_debug_panic("[VM] Idx-Err: index %lld out of the bounds of '%s' (length %zu)\n",
                       (long long)i, n->name, v->stack_len);
    *vout = v;
    return &v->value[i];
}

static int64_t cn_valuta(VM *vm, uint fi, CNode *n)
{
    switch (n->k) {
    case CN_NUM:
        return n->num;
    case CN_VAR: {
        int idx = cn_indice(vm, fi, n);
        if (idx >= 0) {
            Var *v = vm->frames[fi]->vars[idx];
            if (v && v->value)
                return *(v->value);
            vm_debug_panic("[VM] read: variable '%s' outside its local/delocal block\n", n->name);
        }
        vm_debug_panic("[VM] read: variable '%s' is not defined\n", n->name);
        return 0;
    }
    case CN_ARR:
    case CN_MALE: {
        Var *av;
        return *cn_cella(vm, fi, n, &av, "lettura");
    }
    default: {
        int64_t l = cn_valuta(vm, fi, n->a);
        int64_t r = cn_valuta(vm, fi, n->b);
        return apply_binop(n->op, l, r);
    }
    }
}

static inline int64_t resolve_expr(VM *vm, uint fi, const char *tok)
{
    return cn_valuta(vm, fi, cn_cerca(&tl_cn_espr, tok, 0));
}

static inline int64_t resolve_value(VM *vm, uint fi, const char *tok)
{
    return resolve_expr(vm, fi, tok);
}

/* Luogo di un assegnamento: una variabile int o una cella di array. */
static inline int64_t *lvalue_ptr(VM *vm, uint fi, const char *id, Var **vout, const char *op)
{
    CNode *n = cn_cerca(&tl_cn_luogo, id, 1);
    if (n->k != CN_VAR)
        return cn_cella(vm, fi, n, vout, op);
    int idx = cn_indice(vm, fi, n);
    if (idx < 0)
        vm_debug_panic("[VM] %s: variable '%s' is not defined\n", op, n->name);
    Var *v = vm->frames[fi]->vars[idx];
    if (!v)
        vm_debug_panic("[VM] %s: variable '%s' is NULL\n", op, n->name);
    if (v->T != TYPE_INT) vm_debug_panic("[VM] %s on a non-int\n", op);
    *vout = v;
    return v->value;
}

#endif /* VM_HELPERS_H */