#ifndef VM_DEC_H
#define VM_DEC_H

#include <stdint.h>
#include <string.h>
#include <limits.h>
#include "vm_types.h"
#include "vm_helpers.h"

/* ======================================================================
 *  Righe decodificate
 *
 *  Le istruzioni piu' frequenti (assegnamenti, condizioni, salti, local,
 *  push e pop, swap) si leggevano dal testo a ogni esecuzione: copia della
 *  riga, strtok, ricerca delle espressioni compilate e dei nomi nelle tabelle
 *  del frame. Qui una riga si decodifica una volta per thread: i suoi token,
 *  le espressioni compilate (vm_cexpr), i nomi con l'indice trovato
 *  nell'ultimo frame, la destinazione dei salti.
 *
 *  Una riga si decodifica alla seconda esecuzione: la prima va per il testo,
 *  perche' molte righe (quelle dei dati) si eseguono una volta sola. Una riga
 *  che il decodificatore non riconosce come ben formata resta sempre sulla
 *  via del testo, che si comporta come prima (stessi errori, stessi crash).
 *
 *  Le vm_x_* fanno quello che fanno le op_* sul testo, negli stessi passi e
 *  con gli stessi messaggi. Ricevono anche l'indice del frame, che il
 *  chiamante conosce gia' (le op_* lo ricalcolano con get_findex, che non ha
 *  effetti). La decodifica vale per la famiglia: PUSHEQ, MINEQ e XOREQ hanno
 *  gli stessi argomenti, come LOCAL e DELOCAL, PUSH e POP; l'inversione usa la
 *  riga decodificata per eseguire l'istruzione inversa.
 * ====================================================================== */

enum { VMC_NO = 0, VMC_EQ, VMC_NE, VMC_GE, VMC_LE, VMC_GT, VMC_LT };

static inline int vm_cmp_cod(const char *op)
{
    if (!strcmp(op, "==")) return VMC_EQ;
    if (!strcmp(op, "!=")) return VMC_NE;
    if (!strcmp(op, ">=")) return VMC_GE;
    if (!strcmp(op, "<=")) return VMC_LE;
    if (!strcmp(op, ">"))  return VMC_GT;
    if (!strcmp(op, "<"))  return VMC_LT;
    return VMC_NO;
}

/* Come eval_cond. Un operatore sconosciuto lascia la riga sul testo. */
static inline int vm_cmp(int c, int64_t l, int64_t r)
{
    switch (c) {
    case VMC_EQ: return l == r;
    case VMC_NE: return l != r;
    case VMC_GE: return l >= r;
    case VMC_LE: return l <= r;
    case VMC_GT: return l >  r;
    default:     return l <  r;
    }
}

/* Un nome da cercare in una tabella del frame (variabili o etichette), con
 * l'esito dell'ultima ricerca. Le tabelle crescono soltanto: un indice
 * trovato vale finche' il frame resta lo stesso, un'assenza finche' la
 * tabella non ha voci nuove. Il gen del frame (unico nel processo, mai 0 per
 * un frame inizializzato) identifica il frame e la sua inizializzazione. */
typedef struct {
    const char *s;
    uint32_t    h;
    unsigned    run, gen;
    uint        fi;
    int         count, idx;
} VmNome;

static inline void vm_nome_init(VmNome *n, const char *s)
{
    memset(n, 0, sizeof *n);
    n->s = s;
    n->h = s ? char_id_hash(s) : 0;
    n->idx = -1;
}

/* Come char_id_map_lookup sulla tabella delle variabili (etichetta = 0) o
 * delle etichette (1) del frame fi. */
static inline int vm_nome_cerca(VM *vm, uint fi, VmNome *n, int etichetta)
{
    Frame *f = vm->frames[fi];
    CharIdMap *m = etichetta ? &f->LabelIndexer : &f->VarIndexer;
    if (n->gen == f->gen && n->gen) {
        if (n->idx >= 0) return n->idx;
        if (n->count == __atomic_load_n(&m->count, __ATOMIC_ACQUIRE)) return -1;
    }
    int c = __atomic_load_n(&m->count, __ATOMIC_ACQUIRE);
    int idx = char_id_map_lookup_h(m, n->s, n->h);
    n->run = g_vm_run_id; n->fi = fi; n->gen = f->gen; n->count = c; n->idx = idx;
    return idx;
}

/* Una riga del programma trovata per un frame: la destinazione di un salto,
 * la riga della guardia di un ciclo. */
typedef struct {
    unsigned    run, gen;
    uint        fi;
    int         count;          /* voci delle etichette del frame (guardia) */
    const char *buf;
    char       *dest;
} VmSalto;

enum { VME_ALTRA = 0, VME_ELSE, VME_FROM_ERR, VME_FROM_START };

typedef struct VmDec {
    uint8_t     opc;            /* istruzione della riga */
    uint8_t     cmp;            /* EVAL, ASSERT: confronto */
    uint8_t     stesso;         /* PUSHEQ, MINEQ, XOREQ: `x op= x` */
    uint8_t     etk;            /* JMPF, JMP, LABEL: tipo dell'etichetta */
    uint8_t     nil, empty;     /* LOCAL, DELOCAL: il valore e' "nil", "empty" */
    int8_t      tipo;           /* LOCAL, DELOCAL: TYPE_* */
    char       *txt;            /* argomenti, divisi come li divide strtok */
    const char *t1, *t2, *t3, *t4;
    CNode      *a, *b;          /* luoghi, espressioni */
    VmNome      n1, n2;
    int64_t     num;            /* strtoull del valore */
    long        len;            /* strtol della lunghezza */
    VmSalto     j;              /* JMPF, JMP: destinazione */
    VmSalto     g;              /* FROM_START_<uid>: riga della guardia d'ingresso */
    uint        gk;             /* indice di quella riga, UINT_MAX se fuori indice */
    struct VmDec *ge;           /* quella riga decodificata, quando lo e' */
    VmNome      back;           /* FROM_START_<uid>: FROM_BACK_<uid> */
    char       *back_s;
} VmDec;

static inline int vm_dec_ammessa(int opc)
{
    switch (opc) {
    case OPC_PUSHEQ: case OPC_MINEQ: case OPC_XOREQ: case OPC_SWAP:
    case OPC_PUSH: case OPC_POP: case OPC_EVAL: case OPC_ASSERT:
    case OPC_LOCAL: case OPC_DELOCAL: case OPC_LABEL: case OPC_JMPF: case OPC_JMP:
        return 1;
    }
    return 0;
}

static void vm_dec_libera(VmDec *d)
{
    cn_libera(d->a);
    cn_libera(d->b);
    free(d->back_s);
    free(d);
}

/* Decodifica della riga k dell'indice: NULL se resta sul testo. I token si
 * prendono come li prendono le op_* (strtok sulla copia degli argomenti). */
static VmDec *vm_dec_nuova(uint k)
{
    int opc = g_line_index.op[k];
    if (!vm_dec_ammessa(opc) || g_line_index.fine[k] == SIZE_MAX) return NULL;
    size_t a = g_line_index.arg[k], e = g_line_index.fine[k];
    if (a > e) return NULL;
    size_t n = e - a;
    VmDec *d = (VmDec *)calloc(1, sizeof(VmDec) + n + 1);
    if (!d) return NULL;
    d->txt = (char *)(d + 1);
    memcpy(d->txt, g_line_index.base + a, n);
    d->txt[n] = '\0';
    d->opc = (uint8_t)opc;
    d->gk = UINT_MAX;
    d->tipo = -1;
    char *sp = d->txt;
#define VMD_TOK() vm_strtok_r(NULL, " \t", &sp)
    switch (opc) {
    case OPC_PUSHEQ: case OPC_MINEQ: case OPC_XOREQ: {
        char *id = VMD_TOK();
        const char *ex = vm_strtok_r(NULL, "", &sp);
        if (!ex) ex = "";
        while (*ex == ' ' || *ex == '\t') ex++;
        if (!id) goto testo;
        d->t1 = id; d->t2 = ex;
        d->a = cn_compila_luogo(id);
        d->b = cn_compila(ex);
        d->stesso = (uint8_t)(expr_is_bare_ident(ex) && !strcmp(id, ex));
        break;
    }
    case OPC_SWAP: {
        char *x = VMD_TOK(), *y = VMD_TOK();
        if (!x || !y) goto testo;
        d->t1 = x; d->t2 = y;
        d->a = cn_compila_luogo(x);
        d->b = cn_compila_luogo(y);
        break;
    }
    case OPC_PUSH: case OPC_POP: {
        char *x = VMD_TOK(), *y = VMD_TOK();
        if (VMD_TOK() || !x || !y) goto testo;
        d->t1 = x; d->t2 = y;
        vm_nome_init(&d->n1, x);
        vm_nome_init(&d->n2, y);
        d->num = (int64_t)strtoull(x, NULL, 10);
        break;
    }
    case OPC_EVAL: case OPC_ASSERT: {
        char *l = VMD_TOK(), *o = VMD_TOK();
        const char *r = vm_strtok_r(NULL, "", &sp);
        if (!r) r = "";
        while (*r == ' ' || *r == '\t') r++;
        if (!l || !o || r[0] == '\0') goto testo;
        int c = vm_cmp_cod(o);
        if (c == VMC_NO) goto testo;
        d->t1 = l; d->t2 = o; d->t3 = r;
        d->cmp = (uint8_t)c;
        d->a = cn_compila(l);
        d->b = cn_compila(r);
        break;
    }
    case OPC_LOCAL: case OPC_DELOCAL: {
        char *ty = VMD_TOK(), *nm = VMD_TOK(), *cv = VMD_TOK();
        char *cl = (ty && !strcmp(ty, "array")) ? VMD_TOK() : NULL;
        if (!ty || !nm) goto testo;
        int T = vm_tipo_cod(ty);
        if (T < 0) goto testo;
        d->tipo = (int8_t)T;
        d->t1 = ty; d->t2 = nm; d->t3 = cv; d->t4 = cl;
        vm_nome_init(&d->n1, nm);
        if (cv) {
            vm_nome_init(&d->n2, cv);
            d->a = cn_compila(cv);
            d->num = (int64_t)strtoull(cv, NULL, 10);
            d->nil = (uint8_t)!strcmp(cv, "nil");
            d->empty = (uint8_t)!strcmp(cv, "empty");
        }
        if (cl) d->len = strtol(cl, NULL, 10);
        break;
    }
    case OPC_JMPF: case OPC_JMP: case OPC_LABEL: {
        char *l = VMD_TOK();
        if (!l && opc != OPC_LABEL) goto testo;
        d->t1 = l;
        if (!l) break;
        if (!strncmp(l, "ELSE_", 5))             d->etk = VME_ELSE;
        else if (!strncmp(l, "FROM_ERR_", 9))    d->etk = VME_FROM_ERR;
        else if (!strncmp(l, "FROM_START_", 11)) d->etk = VME_FROM_START;
        vm_nome_init(&d->n1, l);
        if (d->etk == VME_FROM_START) {
            size_t ul = strlen(l + 11);
            d->back_s = (char *)malloc(sizeof("FROM_BACK_") + ul);
            if (!d->back_s) goto testo;
            memcpy(d->back_s, "FROM_BACK_", sizeof("FROM_BACK_") - 1);
            memcpy(d->back_s + sizeof("FROM_BACK_") - 1, l + 11, ul + 1);
            vm_nome_init(&d->back, d->back_s);
        }
        break;
    }
    }
#undef VMD_TOK
    return d;
testo:
    vm_dec_libera(d);
    return NULL;
}

/* --- tabella per thread: riga -> decodifica --- */

#define VMD_VISTA ((VmDec *)(uintptr_t)1)     /* eseguita una volta, sul testo */
#define VMD_TESTO ((VmDec *)(uintptr_t)2)     /* resta sul testo */

static __thread VmDec  **tl_dec     = NULL;
static __thread uint     tl_dec_n   = 0;
static __thread unsigned tl_dec_run = 0;

/* Alla fine di un'esecuzione o di un thread di par: la tabella e' __thread. */
static inline void vm_dec_free_cache(void)
{
    for (uint k = 0; tl_dec && k < tl_dec_n; k++)
        if ((uintptr_t)tl_dec[k] > 2) vm_dec_libera(tl_dec[k]);
    free(tl_dec);
    tl_dec = NULL;
    tl_dec_n = 0;
    tl_dec_run = 0;
}

/* La tabella del thread per l'esecuzione corrente, creata se manca (NULL
 * senza indice o senza memoria): resta la stessa fino alla fine
 * dell'esecuzione, e vm_run_BT la legge una volta sola. */
static inline VmDec **vm_dec_tab(void)
{
    if (tl_dec_run != g_vm_run_id || !tl_dec) {
        vm_dec_free_cache();
        if (!g_line_index.base || !g_line_index.n) return NULL;
        tl_dec = (VmDec **)calloc(g_line_index.n, sizeof(VmDec *));
        if (!tl_dec) return NULL;
        tl_dec_n = g_line_index.n;
        tl_dec_run = g_vm_run_id;
    }
    return tl_dec;
}

/* La riga k decodificata, o NULL per la via del testo. */
static inline VmDec *vm_dec_riga(uint k)
{
    if (!vm_dec_tab() || k >= tl_dec_n) return NULL;
    VmDec *d = tl_dec[k];
    if ((uintptr_t)d > 2) return d;
    if (d == VMD_TESTO) return NULL;
#ifndef VM_DEC_SUBITO          /* per le prove: -DVM_DEC_SUBITO decodifica subito */
    if (!d) { tl_dec[k] = VMD_VISTA; return NULL; }
#endif
    d = vm_dec_nuova(k);
    tl_dec[k] = d ? d : VMD_TESTO;
    return d;
}

/* Riga del testo indicizzato che comincia in p, UINT_MAX se non c'e'. */
static inline uint vm_dec_riga_di(const char *p)
{
    const char *b = g_line_index.base;
    if (!b || p < b || p >= b + g_line_index.len) return UINT_MAX;
    size_t q = (size_t)(p - b);
    uint k = vm_line_of(q);
    return g_line_index.off[k] == q ? k : UINT_MAX;
}

/* ======================================================================
 *  Esecuzione
 * ====================================================================== */

static inline void vm_x_pusheq(VM *vm, const char *fname, uint fi, VmDec *d)
{
    (void)fname;
    Var *v;
    int64_t *cell = lvalue_ptr_f(vm->frames[fi], d->a, &v, "PUSHEQ");
    var_par_mut_acquire(v);
    *cell = (int64_t)((uint64_t)*cell + (uint64_t)cn_val(vm->frames[fi], d->b));
    var_par_mut_release(v);
}

static inline void vm_x_mineq(VM *vm, const char *fname, uint fi, VmDec *d)
{
    (void)fname;
    Var *v;
    int64_t *cell = lvalue_ptr_f(vm->frames[fi], d->a, &v, "MINEQ");
    if (d->stesso)
        vm_debug_panic("[VM] MINEQ: `%s -= %s` is not reversible (it loses information)\n", d->t1, d->t2);
    var_par_mut_acquire(v);
    *cell = (int64_t)((uint64_t)*cell - (uint64_t)cn_val(vm->frames[fi], d->b));
    var_par_mut_release(v);
}

static inline void vm_x_xoreq(VM *vm, const char *fname, uint fi, VmDec *d)
{
    (void)fname;
    Var *v;
    int64_t *cell = lvalue_ptr_f(vm->frames[fi], d->a, &v, "XOREQ");
    if (d->stesso)
        vm_debug_panic("[VM] XOREQ: `%s ^= %s` is not reversible (it loses information)\n", d->t1, d->t2);
    var_par_mut_acquire(v);
    *cell ^= cn_val(vm->frames[fi], d->b);
    var_par_mut_release(v);
}

static inline void vm_x_swap(VM *vm, const char *fname, uint fi, VmDec *d)
{
    (void)fname;
    Var *v1, *v2;
    int64_t *c1 = lvalue_ptr_f(vm->frames[fi], d->a, &v1, "SWAP");
    int64_t *c2 = lvalue_ptr_f(vm->frames[fi], d->b, &v2, "SWAP");
    var_par_mut_acquire2(v1, v2);
    int64_t tmp = *c1;
    *c1 = *c2;
    *c2 = tmp;
    var_par_mut_release2(v1, v2);
}

static inline void vm_x_push(VM *vm, const char *frame_name, uint fi, VmDec *d)
{
    int64_t val;
    int i1 = vm_nome_cerca(vm, fi, &d->n1, 0);
    if (i1 >= 0) {
        Var *src = vm->frames[fi]->vars[i1];
        if (!src) vm_debug_panic("[VM] %s: variable '%s' is NULL\n", "PUSH", d->t1);
        var_par_mut_acquire(src);
        val = *(src->value);
        *(src->value) = 0;
        var_par_mut_release(src);
    } else {
        val = d->num;
    }
    int si = vm_nome_cerca(vm, fi, &d->n2, 0);
    if (si < 0)
        vm_debug_panic("[VM] PUSH: destination stack not found\n");
    Var *sv = vm->frames[fi]->vars[si];
    if (sv->T != TYPE_STACK)
        vm_debug_panic("[VM] PUSH: '%s' is not a stack (channels use ssend/srecv) frame=%s\n",
            d->t2, frame_name);
    var_stack_push(sv, val);
}

static inline void vm_x_pop(VM *vm, const char *frame_name, uint fi, VmDec *d)
{
    const char *C_dest = d->t1, *C_stack = d->t2;
    int si = vm_nome_cerca(vm, fi, &d->n2, 0);
    if (si < 0)
        vm_debug_panic("[VM] POP: stack not found\n");
    Var *sv = vm->frames[fi]->vars[si];
    if (sv->T != TYPE_STACK)
        vm_debug_panic("[VM] POP: '%s' is not a stack (channels use ssend/srecv) frame=%s\n",
                       C_stack, frame_name);
    if (sv->stack_len == 0)
        vm_debug_panic("[VM] POP: empty stack (Pop-Err, frame=%s dest=%s stack=%s inv=%d)\n",
                       frame_name, C_dest, C_stack, g_inversion_depth);
    int64_t popped = sv->value[--sv->stack_len];

    int di = vm_nome_cerca(vm, fi, &d->n1, 0);
    if (di < 0)
        vm_debug_panic("[VM] %s: variable '%s' is not defined\n", "POP", C_dest);
    Var *dest = vm->frames[fi]->vars[di];
    if (!dest)
        vm_debug_panic("[VM] %s: variable '%s' is NULL\n", "POP", C_dest);
    var_par_mut_acquire(dest);
    if (dest->T == TYPE_INT && *(dest->value) != 0) {
        var_par_mut_release(dest);
        vm_debug_panic(
            "[VM] POP: destination '%s' is not zero before the pop (Pop-Err2, current value=%lld) frame=%s\n",
            C_dest, (long long)*(dest->value), frame_name);
    }
    *(dest->value) += popped;
    var_par_mut_release(dest);
}

static inline void vm_x_eval(VM *vm, const char *fname, uint fi, VmDec *d)
{
    (void)fname;
    int64_t lval = cn_val(vm->frames[fi], d->a);
    int64_t rval = cn_val(vm->frames[fi], d->b);
    thread_val_IF = vm_cmp(d->cmp, lval, rval);
}

static inline void vm_x_assert(VM *vm, const char *frame_name, uint fi, VmDec *d)
{
    int fi_result = (int)thread_val_IF;
    if (if_branch_top >= 0) {
        int took_then = if_branch_stack[if_branch_top--];
        if (!took_then && fi_result) {
            vm_debug_panic(
                "[VM] IF/FI not reversible: else branch taken but the fi assertion is true (IfError, frame=%s lhs=%s op=%s rhs=%s)\n",
                frame_name, d->t1, d->t2, d->t3);
        }
        if (took_then && !fi_result) {
            vm_debug_panic(
                "[VM] IF/FI not reversible: then branch taken but the fi assertion is false (IfError, frame=%s lhs=%s op=%s rhs=%s)\n",
                frame_name, d->t1, d->t2, d->t3);
        }
        return;
    }
    int64_t lval = cn_val(vm->frames[fi], d->a);
    int64_t rval = cn_val(vm->frames[fi], d->b);
    if (!vm_cmp(d->cmp, lval, rval))
        vm_debug_panic("[VM] ASSERT failed: %s %s %s\n", d->t1, d->t2, d->t3);
}

/* La riga della guardia d'ingresso del ciclo FROM_START_<uid> (come in
 * loop_entry_guard), per frame; NULL se non c'e', e allora la via del testo
 * da' l'errore. */
static inline const char *vm_x_guardia_riga(VM *vm, uint fi, char *buf, VmDec *d)
{
    Frame *f = vm->frames[fi];
    int cnt = __atomic_load_n(&f->LabelIndexer.count, __ATOMIC_ACQUIRE);
    VmSalto *s = &d->g;
    if (s->dest && s->gen == f->gen && s->gen && s->buf == buf && s->count == cnt)
        return s->dest;
    uint back = 3;
    int li = vm_nome_cerca(vm, fi, &d->back, 1);
    if (li < 0) {
        back = 2;
        li = vm_nome_cerca(vm, fi, &d->n1, 1);
        if (li < 0) return NULL;
    }
    char *ln = go_to_line(buf, f->label[li] - back);
    char *nl = ln ? strchr(ln, '\n') : NULL;
    if (!nl) return NULL;
    s->run = g_vm_run_id; s->fi = fi; s->gen = f->gen; s->buf = buf; s->count = cnt;
    s->dest = ln;
    d->gk = vm_dec_riga_di(ln);
    d->ge = NULL;
    return ln;
}

/* loop_entry_guard sulla riga decodificata della guardia, quando c'e'. */
static inline int vm_x_guardia(VM *vm, const char *fname, uint fi, char *buf, VmDec *d)
{
    const char *ln = vm_x_guardia_riga(vm, fi, buf, d);
    if (ln && d->gk != UINT_MAX && g_line_index.op[d->gk] == OPC_EVAL) {
        VmDec *e = d->ge ? d->ge : (d->ge = vm_dec_riga(d->gk));
        if (e) {
            uint saved = thread_val_IF;
            vm_x_eval(vm, fname, fi, e);
            int b1 = thread_val_IF ? 1 : 0;
            thread_val_IF = saved;
            return b1;
        }
    }
    return loop_entry_guard(vm, fname, buf, d->t1 + 11);
}

static inline void vm_x_rientro(VM *vm, const char *fname, uint fi, char *buf, VmDec *d)
{
    if (vm_x_guardia(vm, fname, fi, buf, d))
        vm_debug_panic("[VM] FROM: entry guard true on re-entering the loop "
                       "(LoopError2, frame=%s)\n", fname);
}

static inline void vm_x_label(VM *vm, const char *fname, uint fi, char *buf, VmDec *d)
{
    if (d->etk != VME_FROM_START) return;
    if (vm_nome_cerca(vm, fi, &d->back, 1) >= 0)
        vm_x_rientro(vm, fname, fi, buf, d);
}

static inline char *vm_x_salto_noto(VM *vm, uint fi, const char *buf, VmDec *d)
{
    VmSalto *s = &d->j;
    unsigned gen = vm->frames[fi]->gen;
    if (s->dest && s->gen == gen && gen && s->buf == buf)
        return s->dest;
    return NULL;
}

static inline void vm_x_salto_metti(VM *vm, uint fi, const char *buf, VmDec *d, char *dest)
{
    VmSalto *s = &d->j;
    s->run = g_vm_run_id; s->fi = fi; s->gen = vm->frames[fi]->gen; s->buf = buf;
    s->dest = dest;
}

static inline char *vm_x_jmpf(VM *vm, const char *fname, uint fi, char *buf, VmDec *d)
{
    if (d->etk == VME_ELSE)
        vm_if_push_branch(thread_val_IF ? 1 : 0);
    if (thread_val_IF) return NULL;
    if (d->etk == VME_FROM_ERR)
        vm_debug_panic("[VM] FROM: entry guard false on entering the loop "
                       "(LoopError1, frame=%s)\n", fname);
    if (d->etk == VME_FROM_START)
        vm_x_rientro(vm, fname, fi, buf, d);
    char *dest = vm_x_salto_noto(vm, fi, buf, d);
    if (dest) return dest;
    int li = vm_nome_cerca(vm, fi, &d->n1, 1);
    if (li < 0) vm_debug_panic("EXIT_FAILURE");
    char *newptr = go_to_line(buf, vm->frames[fi]->label[li] + 1);
    if (!newptr) vm_debug_panic("[VM] JMPF: label not found\n");
    vm_x_salto_metti(vm, fi, buf, d, newptr);
    return newptr;
}

static inline char *vm_x_jmp(VM *vm, const char *fname, uint fi, char *buf, VmDec *d)
{
    char *dest = vm_x_salto_noto(vm, fi, buf, d);
    if (dest) return dest;
    int gia = vm_nome_cerca(vm, fi, &d->n1, 1);
    uint li = gia >= 0 ? (uint)gia
                       : (uint)char_id_map_get(&vm->frames[fi]->LabelIndexer, d->t1);
    char *newptr = go_to_line(buf, vm->frames[fi]->label[li] + 1);
    if (!newptr) vm_debug_panic("[VM] JMP: label '%s' not found (frame='%s' fi=%u li=%u line=%u)\n",
                                d->t1, fname, fi, li, vm->frames[fi]->label[li]);
    if (gia >= 0) vm_x_salto_metti(vm, fi, buf, d, newptr);
    return newptr;
}

static inline void vm_x_local(VM *vm, const char *frame_name, uint fi, VmDec *d)
{
    const char *Vname = d->t2, *c_val = d->t3, *c_len = d->t4;

    int vi_gia = vm_nome_cerca(vm, fi, &d->n1, 0);
    uint vi;
    if (vi_gia >= 0) {
        vi = (uint)vi_gia;
    } else {
        pthread_mutex_lock(&var_indexer_mtx);
        vi = char_id_map_get(&vm->frames[fi]->VarIndexer, Vname);
        pthread_mutex_unlock(&var_indexer_mtx);
    }

    frame_ensure_vars(vm->frames[fi], (int)vi);
    if (vm->frames[fi]->vars[vi]) {
        if (vm->frames[fi]->vars[vi]->is_local)
            vm_debug_panic(
                "[VM] LOCAL: variable '%s' already allocated (Local-Err: missing DELOCAL before this LOCAL?) frame=%s\n",
                Vname, frame_name);
        delete_var(vm->frames[fi]->vars, &vm->frames[fi]->var_count, (int)vi);
    }

    vm->frames[fi]->vars[vi] = vm_var_nuova(d->tipo, Vname);
    if (!vm->frames[fi]->vars[vi]) vm_debug_panic("[VM] LOCAL: allocation failed\n");

    int vc = __atomic_load_n(&vm->frames[fi]->var_count, __ATOMIC_RELAXED);
    while ((int)vi >= vc &&
           !__atomic_compare_exchange_n(&vm->frames[fi]->var_count, &vc, (int)vi + 1, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;

    Var *dst = vm->frames[fi]->vars[vi];

    if (dst->T == TYPE_CHANNEL && g_inversion_depth > 0) {
        Channel *restored = channel_restore_pop(frame_name, Vname);
        if (restored) {
            pthread_mutex_destroy(&dst->channel->mtx);
            free(dst->channel->buf);
            free(dst->channel);
            dst->channel = restored;
        }
    }

    if (dst->T == TYPE_ARRAY) {
        long n = c_len ? d->len : 0;
        if (n <= 0) vm_debug_panic("[VM] LOCAL: array '%s' without a valid length\n", Vname);
        int64_t fill = 0;
        if (c_val) {
            if (vm_nome_cerca(vm, fi, &d->n2, 0) >= 0)
                fill = cn_val(vm->frames[fi], d->a);
            else
                fill = d->num;
        }
        if ((unsigned long)n > SIZE_MAX / sizeof(int64_t))
            vm_debug_panic("[VM] LOCAL: array '%s' too large (%ld cells)\n", Vname, n);
        dst->value = malloc((size_t)n * sizeof(int64_t));
        if (!dst->value) vm_debug_panic("[VM] LOCAL: array allocation failed\n");
        for (long k = 0; k < n; k++) dst->value[k] = fill;
        dst->stack_len = (size_t)n;
    } else if (c_val && vm_nome_cerca(vm, fi, &d->n2, 0) >= 0) {
        uint  si  = (uint)d->n2.idx;
        Var  *src = vm->frames[fi]->vars[si];
        if (!src) vm_debug_panic("[VM] LOCAL: NULL source\n");
        if (src->T == TYPE_INT)
            *(dst->value) = *(src->value);
        else if (src->T == TYPE_STACK) {
            var_stack_reserve(dst, src->stack_len);
            dst->stack_len = src->stack_len;
            memcpy(dst->value, src->value, src->stack_len * sizeof(int64_t));
        } else {
            vm_debug_panic("[VM] LOCAL: copy from an unlinked PARAM\n");
        }
    } else {
        if (dst->T == TYPE_INT)
            *(dst->value) = c_val ? d->num : 0;
        else if (dst->T == TYPE_STACK) {
            if (c_val && !d->nil)
                vm_debug_panic("[VM] LOCAL: incompatible stack value\n");
        }
    }

    stack_push(frame_locals(vm, fi), dst);
}

static inline void vm_x_delocal(VM *vm, const char *frame_name, uint fi, VmDec *d)
{
    const char *Vtype = d->t1, *Vname = d->t2, *c_val = d->t3, *c_len = d->t4;

    int64_t Vvalue = 0;
    if (c_val) {
        if (vm_nome_cerca(vm, fi, &d->n2, 0) >= 0)
            Vvalue = cn_val(vm->frames[fi], d->a);
        else
            Vvalue = d->num;
    }

    Var *V = stack_pop(frame_locals(vm, fi));

    if (strcmp(V->name, Vname) != 0)
        vm_debug_panic("[VM] DELOCAL: wrong order, expected '%s', found '%s'\n", Vname, V->name);

    int actual = (V->T == TYPE_INT)  ? TYPE_INT
               : (V->T == TYPE_STACK) ? TYPE_STACK
               : (V->T == TYPE_ARRAY) ? TYPE_ARRAY
                                      : TYPE_CHANNEL;
    if (actual != d->tipo) {
        const char *actual_type = (actual == TYPE_INT)  ? "int"
                                : (actual == TYPE_STACK) ? "stack"
                                : (actual == TYPE_ARRAY) ? "array"
                                                         : "channel";
        vm_debug_panic("[VM] DELOCAL: wrong type, expected %s, found %s\n",
                actual_type, Vtype);
    }

    int ok = 0;
    if (V->T == TYPE_ARRAY) {
        long n = c_len ? d->len : -1;
        ok = (n == (long)V->stack_len);
        for (size_t k = 0; ok && k < V->stack_len; k++)
            ok = (V->value[k] == Vvalue);
    }
    else if (V->T == TYPE_INT)     ok = (*(V->value) == Vvalue);
    else if (V->T == TYPE_STACK)   ok = (V->stack_len == 0 && c_val && d->nil);
    else if (V->T == TYPE_CHANNEL) ok = (V->channel->buf_len == 0 && c_val && d->empty);

    if (!ok) {
        if (V->T == TYPE_ARRAY)
            vm_debug_panic(
                "[VM] DELOCAL: array '%s' is not all %lld when closed (or has another length)\n",
                Vname, (long long)Vvalue);
        else if (V->T == TYPE_INT)
            vm_debug_panic(
                "[VM] DELOCAL: wrong final value (Deloc-Err, frame=%s var=%s, expected=%lld, found=%lld, c_val=%s)\n",
                frame_name, Vname, (long long)Vvalue, (long long)*(V->value), c_val ? c_val : "NULL");
        else
            vm_debug_panic("[VM] DELOCAL: %s is not nil/empty\n", Vname);
    }

    int vi_gia = vm_nome_cerca(vm, fi, &d->n1, 0);
    uint vi;
    if (vi_gia >= 0) {
        vi = (uint)vi_gia;
    } else {
        pthread_mutex_lock(&var_indexer_mtx);
        vi = char_id_map_get(&vm->frames[fi]->VarIndexer, Vname);
        pthread_mutex_unlock(&var_indexer_mtx);
    }

    if (V->T == TYPE_CHANNEL && g_inversion_depth == 0) {
        int should_track = 0;
        pthread_mutex_lock(&V->channel->mtx);
        if (V->channel->refcount > 1) {
            V->channel->refcount++;
            should_track = 1;
        }
        pthread_mutex_unlock(&V->channel->mtx);
        if (should_track)
            channel_restore_push(frame_name, Vname, V->channel);
    }

    delete_var(vm->frames[fi]->vars, &vm->frames[fi]->var_count, (int)vi);
}

#endif /* VM_DEC_H */
