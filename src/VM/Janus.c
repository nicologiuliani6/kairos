#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "char_id_map.h"
#include "stack.h"

#define uint     unsigned int
#define vm_fatal(msg) do { fprintf(stderr, msg); exit(EXIT_FAILURE); } while(0)

/* ======================================================================
 *  Tipi
 * ====================================================================== */

typedef enum { TYPE_INT = 0, TYPE_STACK = 1, TYPE_CHANNEL = 2, TYPE_PARAM = 3 } ValueType;

typedef struct ThreadArgs ThreadArgs;

typedef struct Waiter {
    pthread_cond_t  cond;
    int             ready;
    struct Waiter  *next;
    ThreadArgs     *thread_args;
} Waiter;

typedef struct {
    pthread_mutex_t mtx;
    Waiter *send_q_head, *send_q_tail;
    Waiter *recv_q_head, *recv_q_tail;
    ThreadArgs *sender_args;
} Channel;

#define VAR_NAME_LENGTH  100
#define VAR_STACK_MAX_SIZE  128
#define VAR_CHANNEL_MAX_SIZE 128

typedef struct Var {
    ValueType T;
    int      *value;
    size_t    stack_len;
    int       is_local;
    char      name[VAR_NAME_LENGTH];
    Channel  *channel;
} Var;

#define MAX_VARS   100
#define MAX_LABEL  100
#define MAX_NESTED 100

typedef struct {
    CharIdMap VarIndexer;
    Stack     LocalVariables;
    Var      *vars[MAX_VARS];
    int       var_count;
    CharIdMap LabelIndexer;
    uint      label[MAX_LABEL];
    char      name[VAR_NAME_LENGTH];
    uint      addr, end_addr;
    int       param_indices[64];
    int       param_count;
    int       loop_restart_i[MAX_NESTED];
    int       loop_bottom_i[MAX_NESTED];
    int       loop_counter;
    int       recursion_depth;
} Frame;

#define MAX_FRAMES 100
typedef struct { Frame frames[MAX_FRAMES]; int frame_top; } VM;

struct ThreadArgs {
    VM        *vm;
    char      *buffer;
    char       frame_name[VAR_NAME_LENGTH];
    char      *start_ptr;
    int        finished, blocked, turn_done;
    pthread_t  tid;
    pthread_mutex_t *done_mtx;
    pthread_cond_t  *done_cond;
    ThreadArgs *sender_to_notify;
};

/* ── thread-local state ── */
static __thread ThreadArgs *current_thread_args = NULL;
static __thread char       *strtok_saveptr      = NULL;
static __thread uint        thread_val_IF        = 0;

static pthread_mutex_t var_indexer_mtx = PTHREAD_MUTEX_INITIALIZER;

CharIdMap FrameIndexer;

#define strtok(str, delim) strtok_r((str), (delim), &strtok_saveptr)

/* ======================================================================
 *  Helper generici
 * ====================================================================== */

static void make_frame_key(const char *name, int depth, char *out, size_t sz)
{
    if (depth == 0) snprintf(out, sz, "%s", name);
    else            snprintf(out, sz, "%s@%d", name, depth);
}

static void make_thread_frame_key(const char *proc, char *out, size_t sz)
{
    snprintf(out, sz, "%s@t%lu", proc, (unsigned long)pthread_self());
}

static uint get_findex(const char *name)
{
    if (!char_id_map_exists(&FrameIndexer, name)) {
        fprintf(stderr, "[VM] get_findex: frame '%s' non trovato!\n", name);
        exit(EXIT_FAILURE);
    }
    return char_id_map_get(&FrameIndexer, name);
}

static int resolve_value(VM *vm, uint fi, const char *tok)
{
    if (char_id_map_exists(&vm->frames[fi].VarIndexer, tok)) {
        uint idx = char_id_map_get(&vm->frames[fi].VarIndexer, tok);
        return *(vm->frames[fi].vars[idx]->value);
    }
    return (int)strtol(tok, NULL, 10);
}

static Var *get_var(VM *vm, uint fi, const char *name, const char *op)
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

static char *go_to_line(char *buf, uint line)
{
    if (!buf || line == 0) return buf;
    uint cur = 1;
    for (char *p = buf; *p; p++) {
        if (cur == line) return p;
        if (*p == '\n') cur++;
    }
    return NULL;
}

static char *skip_lineno(char *line)
{
    while (*line >= '0' && *line <= '9') line++;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

static void delete_var(Var *vars[], int *size, int n)
{
    if (n < 0 || n >= *size) { printf("Indice fuori range!\n"); return; }
    free(vars[n]->value);
    free(vars[n]);
    vars[n] = NULL;
}

/* ======================================================================
 *  Notify / op_wait
 * ====================================================================== */

static void notify_sender_turn_done(ThreadArgs *sender)
{
    if (!sender) return;
    pthread_mutex_lock(sender->done_mtx);
    sender->turn_done = 1;
    pthread_cond_signal(sender->done_cond);
    pthread_mutex_unlock(sender->done_mtx);
}

/* Blocca il thread corrente finché turn_done non è settato. */
static void wait_for_turn_done(ThreadArgs *ta)
{
    if (!ta) return;
    pthread_mutex_lock(ta->done_mtx);
    ta->blocked = 1;
    while (!ta->turn_done && !ta->finished)
        pthread_cond_wait(ta->done_cond, ta->done_mtx);
    ta->turn_done = 0;
    ta->blocked   = 0;
    pthread_mutex_unlock(ta->done_mtx);
}

static void signal_blocked(ThreadArgs *ta)
{
    if (!ta) return;
    pthread_mutex_lock(ta->done_mtx);
    ta->blocked = 1;
    pthread_cond_signal(ta->done_cond);
    pthread_mutex_unlock(ta->done_mtx);
}

static void clear_blocked(ThreadArgs *ta)
{
    if (!ta) return;
    pthread_mutex_lock(ta->done_mtx);
    ta->blocked = 0;
    pthread_mutex_unlock(ta->done_mtx);
}

/* Handshake su canale. Ritorna 1 se il sender era in coda. */
static int op_wait(Channel *ch, int is_send)
{
    /* Prima di bloccarci di nuovo, svegliamo il sender pendente */
    if (!is_send && current_thread_args && current_thread_args->sender_to_notify) {
        ThreadArgs *s = current_thread_args->sender_to_notify;
        current_thread_args->sender_to_notify = NULL;
        notify_sender_turn_done(s);
    }

    pthread_mutex_lock(&ch->mtx);

    Waiter *self = malloc(sizeof(Waiter));
    pthread_cond_init(&self->cond, NULL);
    self->ready = 0;
    self->next  = NULL;
    self->thread_args = current_thread_args;

    if (is_send) {
        if (ch->recv_q_head) {                          /* match immediato con recv */
            Waiter *w = ch->recv_q_head;
            ch->recv_q_head = w->next;
            if (!ch->recv_q_head) ch->recv_q_tail = NULL;
            ch->sender_args = self->thread_args;
            w->ready = 1;
            pthread_cond_signal(&w->cond);
            pthread_mutex_unlock(&ch->mtx);
            /* Aspetta che il receiver abbia finito il suo turno */
            wait_for_turn_done(current_thread_args);
            pthread_cond_destroy(&self->cond);
            free(self);
            return 0;
        }
        /* Nessun match → vai in coda e segnala blocked */
        if (ch->send_q_tail) ch->send_q_tail->next = self; else ch->send_q_head = self;
        ch->send_q_tail = self;
        signal_blocked(current_thread_args);
        while (!self->ready) pthread_cond_wait(&self->cond, &ch->mtx);
        clear_blocked(current_thread_args);
        pthread_mutex_unlock(&ch->mtx);
        pthread_cond_destroy(&self->cond);
        free(self);
        return 1;      /* era in coda: chiamante deve aspettare turn_done */
    }
    else {
        if (ch->send_q_head) {                          /* match immediato con send */
            Waiter *w = ch->send_q_head;
            ch->send_q_head = w->next;
            if (!ch->send_q_head) ch->send_q_tail = NULL;
            ch->sender_args = w->thread_args;
            w->ready = 1;
            pthread_cond_signal(&w->cond);
            pthread_mutex_unlock(&ch->mtx);
            pthread_cond_destroy(&self->cond);
            free(self);
            return 0;
        }
        if (ch->recv_q_tail) ch->recv_q_tail->next = self; else ch->recv_q_head = self;
        ch->recv_q_tail = self;
        signal_blocked(current_thread_args);
        while (!self->ready) pthread_cond_wait(&self->cond, &ch->mtx);
        clear_blocked(current_thread_args);
        pthread_mutex_unlock(&ch->mtx);
        pthread_cond_destroy(&self->cond);
        free(self);
        return 0;
    }
}

/* ======================================================================
 *  Operazioni aritmetiche (include dopo i tipi)
 * ====================================================================== */
#include "ops_arith.h"

/* ======================================================================
 *  SWAP
 * ====================================================================== */
void op_swap(VM *vm, const char *frame_name)
{
    char *ID1 = strtok(NULL, " \t"), *ID2 = strtok(NULL, " \t");
    uint  fi  = get_findex(frame_name);
    Var  *v1  = get_var(vm, fi, ID1, "SWAP");
    Var  *v2  = get_var(vm, fi, ID2, "SWAP");
    int   tmp = *(v1->value);
    *(v1->value) = *(v2->value);
    *(v2->value) = tmp;
}

/* ======================================================================
 *  PUSH / POP (con supporto channel)
 * ====================================================================== */

/* Forward */
void op_push(VM *vm, const char *frame_name);
void op_pop (VM *vm, const char *frame_name);

void op_push(VM *vm, const char *frame_name)
{
    char *C_val   = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_fatal("[VM] PUSH: troppi parametri!\n");

    uint fi  = get_findex(frame_name);
    int  val;

    if (char_id_map_exists(&vm->frames[fi].VarIndexer, C_val)) {
        Var *src = get_var(vm, fi, C_val, "PUSH");
        val = *(src->value);
        *(src->value) = 0;
    } else {
        val = (int)strtoul(C_val, NULL, 10);
    }

    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, C_stack))
        vm_fatal("[VM] PUSH: stack destinazione non trovato!\n");

    uint  si  = char_id_map_get(&vm->frames[fi].VarIndexer, C_stack);
    Var  *sv  = vm->frames[fi].vars[si];
    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL)
        vm_fatal("[VM] PUSH: destinazione non è stack/channel!\n");

    sv->value = realloc(sv->value, (sv->stack_len + 1) * sizeof(int));
    if (!sv->value) vm_fatal("realloc failed\n");
    sv->value[sv->stack_len++] = val;

    if (sv->T == TYPE_CHANNEL) {
        int was_queued = op_wait(sv->channel, 1);
        if (was_queued)
            wait_for_turn_done(current_thread_args);
    }
}

void op_pop(VM *vm, const char *frame_name)
{
    char *C_dest  = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_fatal("[VM] POP: troppi parametri!\n");

    uint fi = get_findex(frame_name);
    if (!char_id_map_exists(&vm->frames[fi].VarIndexer, C_stack))
        vm_fatal("[VM] POP: stack non trovato!\n");

    uint  si = char_id_map_get(&vm->frames[fi].VarIndexer, C_stack);
    Var  *sv = vm->frames[fi].vars[si];

    if (sv->T != TYPE_STACK && sv->T != TYPE_CHANNEL) vm_fatal("[VM] POP: sorgente non è stack/channel!\n");
    if (sv->T == TYPE_STACK && sv->stack_len == 0)    vm_fatal("[VM] POP: stack vuoto!\n");

    ThreadArgs *sender_to_wake = NULL;

    if (sv->T == TYPE_CHANNEL) {
        /* Leggiamo sender pendente prima di rilasciare il mutex in op_wait */
        pthread_mutex_lock(&sv->channel->mtx);
        if (sv->channel->send_q_head)
            sender_to_wake = sv->channel->send_q_head->thread_args;
        pthread_mutex_unlock(&sv->channel->mtx);

        op_wait(sv->channel, 0);

        if (!sender_to_wake) {
            pthread_mutex_lock(&sv->channel->mtx);
            sender_to_wake = sv->channel->sender_args;
            sv->channel->sender_args = NULL;
            pthread_mutex_unlock(&sv->channel->mtx);
        }
    }

    if (sv->T == TYPE_CHANNEL && sv->stack_len == 0) {
        fprintf(stderr, "[VM] POP: channel vuoto dopo op_wait!\n");
        exit(EXIT_FAILURE);
    }

    int popped = sv->value[--sv->stack_len];
    if (sv->stack_len > 0)
        sv->value = realloc(sv->value, sv->stack_len * sizeof(int));

    Var *dest = get_var(vm, fi, C_dest, "POP");
    *(dest->value) += popped;

    /* Rinvia il sender al prossimo op_wait del receiver */
    if (sv->T == TYPE_CHANNEL && sender_to_wake && current_thread_args)
        current_thread_args->sender_to_notify = sender_to_wake;
}

/* ======================================================================
 *  SHOW / EVAL / ASSERT
 * ====================================================================== */

void op_show(VM *vm, const char *frame_name)
{
    char *ID = strtok(NULL, " \t");
    if (strtok(NULL, " \t")) vm_fatal("[VM] SHOW: troppi parametri!\n");
    uint fi = get_findex(frame_name);
    Var *v  = get_var(vm, fi, ID, "SHOW");

    if (v->T == TYPE_INT) {
        printf("%s: %d\n", ID, *(v->value));
    } else if (v->T == TYPE_STACK || v->T == TYPE_CHANNEL) {
        char open = (v->T == TYPE_STACK) ? '[' : '<';
        char clos = (v->T == TYPE_STACK) ? ']' : '>';
        printf("%s: %c", ID, open);
        for (size_t k = 0; k < v->stack_len; k++) {
            printf("%d", v->value[k]);
            if (k + 1 < v->stack_len) printf(", ");
        }
        printf("%c\n", clos);
    } else {
        vm_fatal("[VM] SHOW su variabile PARAM non linkata!\n");
    }
}

void op_eval(VM *vm, const char *frame_name)
{
    char *ID = strtok(NULL, " \t"), *C_val = strtok(NULL, " \t");
    uint  fi = get_findex(frame_name);
    Var  *v  = get_var(vm, fi, ID, "EVAL");
    thread_val_IF = (*(v->value) == resolve_value(vm, fi, C_val));
}

void op_assert(VM *vm, const char *frame_name)
{
    char *ID1 = strtok(NULL, " \t"), *ID2 = strtok(NULL, " \t");
    if (!ID1 || !ID2) { fprintf(stderr, "[VM] ASSERT: argomenti mancanti\n"); return; }
    uint fi = get_findex(frame_name);
    /* assertion failed is currently non-fatal */
    (void)(resolve_value(vm, fi, ID1) != resolve_value(vm, fi, ID2));
}

/* ======================================================================
 *  Salti
 * ====================================================================== */

char *op_jmp(VM *vm, const char *fname, char *buf)
{
    char *lbl    = strtok(NULL, " \t");
    uint  fi     = get_findex(fname);
    uint  li     = char_id_map_get(&vm->frames[fi].LabelIndexer, lbl);
    char *newptr = go_to_line(buf, vm->frames[fi].label[li] + 1);
    if (!newptr) vm_fatal("[VM] JMP: label non trovata!\n");
    return newptr;
}

char *op_jmpf(VM *vm, const char *fname, char *buf)
{
    char *lbl = strtok(NULL, " \t");
    if (thread_val_IF) return NULL;
    uint fi = get_findex(fname);
    if (!char_id_map_exists(&vm->frames[fi].LabelIndexer, lbl)) exit(EXIT_FAILURE);
    uint  li     = char_id_map_get(&vm->frames[fi].LabelIndexer, lbl);
    char *newptr = go_to_line(buf, vm->frames[fi].label[li] + 1);
    if (!newptr) vm_fatal("[VM] JMPF: label non trovata!\n");
    return newptr;
}

/* ======================================================================
 *  LOCAL / DELOCAL
 * ====================================================================== */

static void alloc_var(Var *v, const char *type, const char *name)
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

void op_local(VM *vm, const char *frame_name)
{
    char *Vtype = strtok(NULL, " \t"), *Vname = strtok(NULL, " \t"), *c_val = strtok(NULL, " \t");
    uint  fi    = get_findex(frame_name);

    pthread_mutex_lock(&var_indexer_mtx);
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, Vname);
    pthread_mutex_unlock(&var_indexer_mtx);

    vm->frames[fi].vars[vi] = malloc(sizeof(Var));
    alloc_var(vm->frames[fi].vars[vi], Vtype, Vname);

    if (vi >= (uint)vm->frames[fi].var_count)
        vm->frames[fi].var_count = vi + 1;

    Var *dst = vm->frames[fi].vars[vi];

    if (char_id_map_exists(&vm->frames[fi].VarIndexer, c_val)) {
        int  si  = char_id_map_get(&vm->frames[fi].VarIndexer, c_val);
        Var *src = vm->frames[fi].vars[si];
        if (src->T == TYPE_INT)        *(dst->value) = *(src->value);
        else if (src->T == TYPE_STACK) { dst->stack_len = src->stack_len; memcpy(dst->value, src->value, src->stack_len * sizeof(int)); }
        else vm_fatal("[VM] LOCAL: copia da PARAM non linkato\n");
    } else {
        if (dst->T == TYPE_INT)        *(dst->value) = (int)strtol(c_val, NULL, 10);
        else if (dst->T == TYPE_STACK) { if (strcmp(c_val, "nil") != 0) vm_fatal("[VM] LOCAL: valore stack non compatibile\n"); }
    }

    stack_push(&vm->frames[fi].LocalVariables, dst);
}

void op_delocal(VM *vm, const char *frame_name)
{
    char *Vtype = strtok(NULL, " \t"), *Vname = strtok(NULL, " \t"), *c_val = strtok(NULL, " \t");
    uint  fi    = get_findex(frame_name);

    int Vvalue = 0;
    if (c_val) {
        if (char_id_map_exists(&vm->frames[fi].VarIndexer, c_val))
            Vvalue = *(vm->frames[fi].vars[char_id_map_get(&vm->frames[fi].VarIndexer, c_val)]->value);
        else
            Vvalue = (int)strtoul(c_val, NULL, 10);
    }

    Var *V = stack_pop(&vm->frames[fi].LocalVariables);
    const char *actual_type = (V->T == TYPE_INT) ? "int" : (V->T == TYPE_STACK ? "stack" : "channel");

    if (strcmp(Vtype, actual_type) != 0) {
        fprintf(stderr, "[VM] DELOCAL: tipo errato! atteso %s, trovato %s\n", actual_type, Vtype);
        exit(EXIT_FAILURE);
    }

    int ok = 0;
    if (V->T == TYPE_INT)     ok = (Vvalue == *(V->value));
    else if (V->T == TYPE_STACK)   ok = (V->stack_len == 0 && strcmp(c_val, "nil")   == 0);
    else if (V->T == TYPE_CHANNEL) ok = (V->stack_len == 0 && strcmp(c_val, "empty") == 0);

    if (!ok) {
        if (V->T == TYPE_INT)
            fprintf(stderr, "[VM] DELOCAL: valore finale errato! (%s, %d, %d)\n", Vname, Vvalue, *(V->value));
        else
            fprintf(stderr, "[VM] DELOCAL: %s non è nil/empty!\n", Vname);
        exit(EXIT_FAILURE);
    }

    pthread_mutex_lock(&var_indexer_mtx);
    delete_var(vm->frames[fi].vars, &vm->frames[fi].var_count,
               char_id_map_get(&vm->frames[fi].VarIndexer, Vname));
    pthread_mutex_unlock(&var_indexer_mtx);
}

/* ======================================================================
 *  Clone frame — corpo comune estratto
 * ====================================================================== */

static void init_clone_frame(VM *vm, uint clone_fi, uint base_fi, const char *key)
{
    Frame *base  = &vm->frames[base_fi];
    Frame *clone = &vm->frames[clone_fi];

    memset(clone, 0, sizeof(Frame));
    clone->VarIndexer   = base->VarIndexer;
    clone->LabelIndexer = base->LabelIndexer;
    clone->addr         = base->addr;
    clone->end_addr     = base->end_addr;
    clone->var_count    = base->var_count;
    clone->param_count  = base->param_count;
    memcpy(clone->param_indices, base->param_indices, sizeof(base->param_indices));
    memcpy(clone->label,         base->label,         sizeof(base->label));
    snprintf(clone->name, VAR_NAME_LENGTH, "%s", key);
    stack_init(&clone->LocalVariables);

    for (int k = 0; k < clone->param_count; k++) {
        int pidx = clone->param_indices[k];
        clone->vars[pidx] = calloc(1, sizeof(Var));
        strncpy(clone->vars[pidx]->name, base->vars[pidx]->name, VAR_NAME_LENGTH - 1);
        clone->vars[pidx]->T = TYPE_PARAM;
    }
}

static uint clone_frame_for_depth(VM *vm, const char *proc, int depth)
{
    char key[VAR_NAME_LENGTH];
    make_frame_key(proc, depth, key, sizeof(key));
    if (char_id_map_exists(&FrameIndexer, key))
        return char_id_map_get(&FrameIndexer, key);
    uint base_fi  = char_id_map_get(&FrameIndexer, proc);
    uint clone_fi = char_id_map_get(&FrameIndexer, key);
    init_clone_frame(vm, clone_fi, base_fi, key);
    return clone_fi;
}

static uint clone_frame_for_thread(VM *vm, const char *proc)
{
    char key[VAR_NAME_LENGTH];
    make_thread_frame_key(proc, key, sizeof(key));
    if (char_id_map_exists(&FrameIndexer, key))
        return char_id_map_get(&FrameIndexer, key);
    uint base_fi  = char_id_map_get(&FrameIndexer, proc);
    uint clone_fi = char_id_map_get(&FrameIndexer, key);
    init_clone_frame(vm, clone_fi, base_fi, key);
    return clone_fi;
}

/* ======================================================================
 *  PAR — scan + esecuzione thread estratti
 * ====================================================================== */

typedef struct {
    char *starts[16];
    int   count;
    char *after_end;   /* puntatore dopo PAR_END + '\n' */
} ParBlock;

static ParBlock scan_par_block(char *par_ptr)
{
    ParBlock pb = { .count = 0, .after_end = NULL };
    int   depth = 1;
    char *scan  = par_ptr;

    while (*scan && depth > 0) {
        char *nl = strchr(scan, '\n');
        if (!nl) break;
        *nl = '\0';
        char tmp[512];
        strncpy(tmp, scan, sizeof(tmp) - 1);
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw) {
            if      (strcmp(fw, "PAR_START") == 0) depth++;
            else if (strcmp(fw, "PAR_END")   == 0) {
                depth--;
                if (depth == 0) { *nl = '\n'; pb.after_end = nl + 1; break; }
            } else if (strncmp(fw, "THREAD_", 7) == 0 && depth == 1 && pb.count < 16) {
                pb.starts[pb.count++] = nl + 1;
            }
        }
        *nl = '\n';
        scan = nl + 1;
    }
    return pb;
}

/* Forward */
static void *thread_entry(void *arg);

static void exec_par_threads(VM *vm, char *buffer, const char *frame_name,
                              ParBlock *pb, int dup_buffer)
{
    pthread_mutex_t done_mtx  = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  done_cond = PTHREAD_COND_INITIALIZER;

    ThreadArgs *args[16];
    for (int t = 0; t < pb->count; t++) {
        args[t] = calloc(1, sizeof(ThreadArgs));
        args[t]->vm        = vm;
        args[t]->buffer    = dup_buffer ? strdup(buffer) : buffer;
        args[t]->start_ptr = pb->starts[t];
        args[t]->done_mtx  = &done_mtx;
        args[t]->done_cond = &done_cond;
        strncpy(args[t]->frame_name, frame_name, VAR_NAME_LENGTH - 1);
    }

    /* Avvia i thread uno alla volta, attendendo che si blocchino o terminino */
    for (int t = 0; t < pb->count; t++) {
        pthread_create(&args[t]->tid, NULL, thread_entry, args[t]);
        pthread_mutex_lock(&done_mtx);
        while (!args[t]->finished && !args[t]->blocked)
            pthread_cond_wait(&done_cond, &done_mtx);
        pthread_mutex_unlock(&done_mtx);
    }

    /* Attendi tutti */
    pthread_mutex_lock(&done_mtx);
    for (;;) {
        int done = 0;
        for (int t = 0; t < pb->count; t++) done += args[t]->finished;
        if (done == pb->count) break;
        pthread_cond_wait(&done_cond, &done_mtx);
    }
    pthread_mutex_unlock(&done_mtx);

    for (int t = 0; t < pb->count; t++) {
        pthread_join(args[t]->tid, NULL);
        if (dup_buffer) free(args[t]->buffer);
        free(args[t]);
    }
}

/* ======================================================================
 *  Inversione (UNCALL)
 * ====================================================================== */

void vm_run_BT(VM *vm, char *buffer, char *frame_name_init);

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

typedef enum { LOOP_ZONE_NONE, LOOP_ZONE_EVAL_ENTRY, LOOP_ZONE_JMPF_ERR,
               LOOP_ZONE_START_LABEL, LOOP_ZONE_EVAL_EXIT, LOOP_ZONE_JMPF_START,
               LOOP_ZONE_END_LABEL, LOOP_ZONE_ERR_LABEL } LoopZone;

typedef enum { IF_ZONE_NONE, IF_ZONE_EVAL_ENTRY, IF_ZONE_JMPF_ELSE, IF_ZONE_JMP_FI,
               IF_ZONE_ELSE_LABEL, IF_ZONE_FI_LABEL, IF_ZONE_EVAL_EXIT, IF_ZONE_ASSERT } IfZone;

static int collect_loops(VM *vm, const char *frame_name, char *buf,
                         LoopDescriptor *out, int max)
{
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH-1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi].addr + 1);
    int n = 0, in_loop = 0;
    uint peval = 0; char pid[64]={0}, pval[64]={0};

    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[512]; strncpy(lb, ptr, sizeof(lb)-1);
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { *nl='\n'; ptr=nl+1; continue; }

        if (!strcmp(fw,"EVAL")) {
            peval=cur; char *a=strtok(NULL," \t"), *b=strtok(NULL," \t");
            strncpy(pid, a?a:"", 63); strncpy(pval, b?b:"", 63);
        } else if (!strcmp(fw,"JMPF") && !in_loop) {
            char *ln=strtok(NULL," \t");
            if (ln && !strncmp(ln,"FROM_ERR",8)) {
                out[n].eval_entry_line=peval; strncpy(out[n].eval_entry_id,pid,63); strncpy(out[n].eval_entry_val,pval,63);
                out[n].jmpf_err_line=cur; in_loop=1;
            }
        } else if (!strcmp(fw,"LABEL") && in_loop) {
            char *ln=strtok(NULL," \t"); if (!ln){ *nl='\n'; ptr=nl+1; continue; }
            if      (!strncmp(ln,"FROM_START",10)) out[n].from_start_line=cur;
            else if (!strncmp(ln,"FROM_END",8))    out[n].from_end_line=cur;
            else if (!strncmp(ln,"FROM_ERR",8))    { out[n].from_err_line=cur; in_loop=0; n++; }
        } else if (!strcmp(fw,"JMPF") && in_loop) {
            char *ln=strtok(NULL," \t");
            if (ln && !strncmp(ln,"FROM_START",10)) {
                out[n].eval_exit_line=peval; strncpy(out[n].eval_exit_id,pid,63); strncpy(out[n].eval_exit_val,pval,63);
                out[n].jmpf_start_line=cur;
            }
        } else if (!strcmp(fw,"END_PROC")) { *nl='\n'; break; }
        *nl='\n'; ptr=nl+1;
    }
    return n;
}

static int collect_ifs(VM *vm, const char *frame_name, char *buf,
                       IfDescriptor *out, int max)
{
    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH-1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi = char_id_map_get(&FrameIndexer, base);
    char *ptr = go_to_line(buf, vm->frames[fi].addr + 1);
    int n = 0, in_if = 0;
    uint peval = 0; char pid[64]={0}, pval[64]={0};

    while (ptr && *ptr && n < max) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl = '\0';
        char lb[512]; strncpy(lb, ptr, sizeof(lb)-1);
        uint cur = (uint)atoi(lb);
        char *fw = strtok(skip_lineno(lb), " \t");
        if (!fw) { *nl='\n'; ptr=nl+1; continue; }

        if (!strcmp(fw,"EVAL")) {
            peval=cur; char *a=strtok(NULL," \t"), *b=strtok(NULL," \t");
            strncpy(pid, a?a:"", 63); strncpy(pval, b?b:"", 63);
        } else if (!strcmp(fw,"JMPF") && !in_if) {
            char *ln=strtok(NULL," \t");
            if (ln && !strncmp(ln,"ELSE_",5)) {
                out[n].eval_entry_line=peval; strncpy(out[n].eval_entry_id,pid,63); strncpy(out[n].eval_entry_val,pval,63);
                out[n].jmpf_else_line=cur; in_if=1;
            }
        } else if (!strcmp(fw,"JMP") && in_if) {
            char *ln=strtok(NULL," \t"); if (ln && !strncmp(ln,"FI_",3)) out[n].jmp_fi_line=cur;
        } else if (!strcmp(fw,"LABEL") && in_if) {
            char *ln=strtok(NULL," \t"); if (!ln){ *nl='\n'; ptr=nl+1; continue; }
            if      (!strncmp(ln,"ELSE_",5)) out[n].else_label_line=cur;
            else if (!strncmp(ln,"FI_",3))   out[n].fi_label_line=cur;
        } else if (!strcmp(fw,"ASSERT") && in_if) {
            out[n].eval_exit_line=peval; strncpy(out[n].eval_exit_id,pid,63); strncpy(out[n].eval_exit_val,pval,63);
            out[n].assert_line=cur; in_if=0; n++;
        } else if (!strcmp(fw,"END_PROC")) { *nl='\n'; break; }
        *nl='\n'; ptr=nl+1;
    }
    return n;
}

static LoopZone line_loop_zone(uint line, LoopDescriptor *L, int n, int *idx)
{
    for (int i=0;i<n;i++) {
        if (line==L[i].eval_entry_line)  { *idx=i; return LOOP_ZONE_EVAL_ENTRY; }
        if (line==L[i].jmpf_err_line)    { *idx=i; return LOOP_ZONE_JMPF_ERR;   }
        if (line==L[i].from_start_line)  { *idx=i; return LOOP_ZONE_START_LABEL; }
        if (line==L[i].eval_exit_line)   { *idx=i; return LOOP_ZONE_EVAL_EXIT;  }
        if (line==L[i].jmpf_start_line)  { *idx=i; return LOOP_ZONE_JMPF_START; }
        if (line==L[i].from_end_line)    { *idx=i; return LOOP_ZONE_END_LABEL;  }
        if (line==L[i].from_err_line)    { *idx=i; return LOOP_ZONE_ERR_LABEL;  }
    }
    *idx=-1; return LOOP_ZONE_NONE;
}

static IfZone line_if_zone(uint line, IfDescriptor *I, int n, int *idx)
{
    for (int i=0;i<n;i++) {
        if (line==I[i].eval_entry_line) { *idx=i; return IF_ZONE_EVAL_ENTRY; }
        if (line==I[i].jmpf_else_line)  { *idx=i; return IF_ZONE_JMPF_ELSE;  }
        if (line==I[i].jmp_fi_line)     { *idx=i; return IF_ZONE_JMP_FI;     }
        if (line==I[i].else_label_line) { *idx=i; return IF_ZONE_ELSE_LABEL; }
        if (line==I[i].fi_label_line)   { *idx=i; return IF_ZONE_FI_LABEL;   }
        if (line==I[i].eval_exit_line)  { *idx=i; return IF_ZONE_EVAL_EXIT;  }
        if (line==I[i].assert_line)     { *idx=i; return IF_ZONE_ASSERT;     }
    }
    *idx=-1; return IF_ZONE_NONE;
}

static void do_eval(VM *vm, uint fi, const char *id, const char *val)
{
    uint vi = char_id_map_get(&vm->frames[fi].VarIndexer, id);
    int rhs = char_id_map_exists(&vm->frames[fi].VarIndexer, val)
        ? *(vm->frames[fi].vars[char_id_map_get(&vm->frames[fi].VarIndexer, val)]->value)
        : (int)strtol(val, NULL, 10);
    thread_val_IF = (*(vm->frames[fi].vars[vi]->value) == rhs);
}

static int line_is_inside_if(uint line, IfDescriptor *ifs, int nifs)
{
    for (int i=0;i<nifs;i++)
        if (line > ifs[i].jmpf_else_line && line < ifs[i].fi_label_line) return 1;
    return 0;
}

static void exec_branch_inverse(VM *vm, char *original_buffer,
                                const char *frame_name,
                                uint from_line, uint to_line,
                                uint caller_fi);

void invert_op_to_line(VM *vm, const char *frame_name, char *buffer,
                       uint start, uint stop)
{
    (void)start; (void)stop;
    char *orig = strdup(buffer);
    if (!orig) { fprintf(stderr, "[UNCALL] strdup fallita\n"); exit(EXIT_FAILURE); }

    char base[VAR_NAME_LENGTH]; strncpy(base, frame_name, VAR_NAME_LENGTH-1);
    char *at = strchr(base, '@'); if (at) *at = '\0';
    uint fi_reset = char_id_map_get(&FrameIndexer, base);
    stack_init(&vm->frames[fi_reset].LocalVariables);

#define MAX_LOOPS 32
#define MAX_IFS   32
#define MAX_LINES 1024

    LoopDescriptor loops[MAX_LOOPS]; int nloops = collect_loops(vm, frame_name, orig, loops, MAX_LOOPS);
    IfDescriptor   ifs  [MAX_IFS];   int nifs   = collect_ifs  (vm, frame_name, orig, ifs,   MAX_IFS);

    char cur_frame[VAR_NAME_LENGTH]; strncpy(cur_frame, frame_name, VAR_NAME_LENGTH-1);
    uint fi       = char_id_map_get(&FrameIndexer, cur_frame);
    uint start_ln = vm->frames[fi_reset].addr + 1;

    char *lp[MAX_LINES]; uint ln[MAX_LINES]; int nl = 0;
    char *ptr = go_to_line(orig, start_ln);
    while (ptr && *ptr && nl < MAX_LINES) {
        char *newline = strchr(ptr, '\n'); if (!newline) break;
        *newline = '\0';
        char tmp[512]; strncpy(tmp, ptr, sizeof(tmp)-1);
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw && !strcmp(fw,"END_PROC")) { *newline='\n'; break; }
        lp[nl] = strdup(ptr); ln[nl] = (uint)atoi(ptr); nl++;
        *newline='\n'; ptr=newline+1;
    }

    int i = nl - 1;
    while (i >= 0) {
        char ob[512]; strncpy(ob, lp[i], sizeof(ob)-1);
        uint  cur  = ln[i];
        char *clean = skip_lineno(ob);
        char *fw    = strtok(clean, " \t");
        if (!fw) { i--; continue; }

        int li=-1; LoopZone lz = line_loop_zone(cur, loops, nloops, &li);
        if (lz==LOOP_ZONE_EVAL_ENTRY || lz==LOOP_ZONE_EVAL_EXIT ||
            lz==LOOP_ZONE_START_LABEL|| lz==LOOP_ZONE_END_LABEL ||
            lz==LOOP_ZONE_ERR_LABEL)  { i--; continue; }

        if (lz==LOOP_ZONE_JMPF_ERR) {
            do_eval(vm, fi, loops[li].eval_entry_id, loops[li].eval_entry_val);
            if (thread_val_IF) { i--; }
            else {
                int t=-1; for(int j=nl-1;j>=0;j--) if(ln[j]==loops[li].jmpf_start_line){t=j;break;}
                if (t<0) { fprintf(stderr,"[UNCALL] jmpf_start\n"); exit(1); } i=t-1;
            }
            continue;
        }
        if (lz==LOOP_ZONE_JMPF_START) {
            do_eval(vm, fi, loops[li].eval_exit_id, loops[li].eval_exit_val);
            if (thread_val_IF) { i--; }
            else {
                int t=-1; for(int j=0;j<nl;j++) if(ln[j]==loops[li].jmpf_err_line){t=j;break;}
                if (t<0) { fprintf(stderr,"[UNCALL] jmpf_err\n"); exit(1); } i=t-1;
            }
            continue;
        }

        int ii=-1; IfZone iz = line_if_zone(cur, ifs, nifs, &ii);
        if (iz==IF_ZONE_EVAL_ENTRY || iz==IF_ZONE_EVAL_EXIT || iz==IF_ZONE_ELSE_LABEL ||
            iz==IF_ZONE_FI_LABEL   || iz==IF_ZONE_ASSERT    || iz==IF_ZONE_JMP_FI)
            { i--; continue; }

        if (iz==IF_ZONE_JMPF_ELSE) {
            int depth = vm->frames[fi_reset].recursion_depth;
            for (int d=0; d<depth; d++) {
                Stack sv = vm->frames[fi].LocalVariables; stack_init(&vm->frames[fi].LocalVariables);
                exec_branch_inverse(vm, orig, cur_frame, ifs[ii].else_label_line+1, ifs[ii].fi_label_line, fi);
                vm->frames[fi].LocalVariables = sv;
            }
            { Stack sv = vm->frames[fi].LocalVariables; stack_init(&vm->frames[fi].LocalVariables);
              exec_branch_inverse(vm, orig, cur_frame, ifs[ii].jmpf_else_line+1, ifs[ii].jmp_fi_line, fi);
              vm->frames[fi].LocalVariables = sv; }
            int t=-1; for(int j=i-1;j>=0;j--) if(ln[j]==ifs[ii].eval_entry_line){t=j;break;}
            i = (t>=0) ? t-1 : i-1;
            continue;
        }

        if (line_is_inside_if(cur, ifs, nifs)) { i--; continue; }

        if (!strcmp(fw,"CALL")) {
            char *pn=strtok(NULL," \t"); uint cfi=char_id_map_get(&FrameIndexer,pn), curi=char_id_map_get(&FrameIndexer,frame_name);
            int pc=vm->frames[cfi].param_count, *pi=vm->frames[cfi].param_indices;
            Var *sv[64]; for(int k=0;k<pc;k++) sv[k]=vm->frames[cfi].vars[pi[k]];
            char *p=NULL; int j=0; while((p=strtok(NULL," \t"))&&j<pc){ int si=char_id_map_get(&vm->frames[curi].VarIndexer,p); vm->frames[cfi].vars[pi[j++]]=vm->frames[curi].vars[si]; }
            invert_op_to_line(vm,pn,orig,vm->frames[cfi].addr+1,vm->frames[cfi].end_addr-1);
            for(int k=0;k<pc;k++) vm->frames[cfi].vars[pi[k]]=sv[k];
            i--; continue;
        }
        if (!strcmp(fw,"UNCALL")) {
            char *pn=strtok(NULL," \t"); uint cfi=char_id_map_get(&FrameIndexer,pn), curi=fi;
            int pc=vm->frames[cfi].param_count, *pi=vm->frames[cfi].param_indices;
            Var *sv[64]; for(int k=0;k<pc;k++) sv[k]=vm->frames[cfi].vars[pi[k]];
            char *p=NULL; int j=0; while((p=strtok(NULL," \t"))&&j<pc){ int si=char_id_map_get(&vm->frames[curi].VarIndexer,p); vm->frames[cfi].vars[pi[j++]]=vm->frames[curi].vars[si]; }
            char cn[VAR_NAME_LENGTH]; strncpy(cn,pn,VAR_NAME_LENGTH-1);
            vm_run_BT(vm,orig,cn);
            for(int k=0;k<pc;k++) vm->frames[cfi].vars[pi[k]]=sv[k];
            i--; continue;
        }

        if      (!strcmp(fw,"PUSHEQ")) op_pusheq_inv(vm,cur_frame);
        else if (!strcmp(fw,"MINEQ"))  op_mineq_inv (vm,cur_frame);
        else if (!strcmp(fw,"SWAP"))   op_swap_inv  (vm,cur_frame);
        else if (!strcmp(fw,"PUSH"))   op_pop       (vm,cur_frame);
        else if (!strcmp(fw,"POP"))    op_push      (vm,cur_frame);
        else if (!strcmp(fw,"LOCAL"))  op_delocal   (vm,cur_frame);
        else if (!strcmp(fw,"DELOCAL"))op_local     (vm,cur_frame);
        else if (!strcmp(fw,"SHOW"))   op_show      (vm,cur_frame);
        else if (!strcmp(fw,"PARAM") || !strcmp(fw,"LABEL") || !strcmp(fw,"EVAL")  ||
                 !strcmp(fw,"JMPF")  || !strcmp(fw,"JMP")   || !strcmp(fw,"ASSERT")||
                 !strcmp(fw,"DECL")  || !strcmp(fw,"HALT"))  { /* skip */ }
        else { fprintf(stderr,"[UNCALL] op sconosciuta: '%s'\n",fw); exit(EXIT_FAILURE); }
        i--;
    }

    for (int j=0;j<nl;j++) free(lp[j]);
    free(orig);
#undef MAX_LOOPS
#undef MAX_IFS
#undef MAX_LINES
}

static void exec_branch_inverse(VM *vm, char *original_buffer,
                                const char *frame_name,
                                uint from_line, uint to_line,
                                uint caller_fi)
{
    char *lines[512]; int count = 0;
    char *ptr = go_to_line(original_buffer, from_line);
    if (!ptr) return;
    while (ptr && *ptr && count < 512) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl='\0';
        if ((uint)atoi(ptr) >= to_line) { *nl='\n'; break; }
        lines[count++] = strdup(ptr);
        *nl='\n'; ptr=nl+1;
    }

    uint cfi = char_id_map_get(&FrameIndexer, frame_name);
    Var *saved[MAX_VARS]; memcpy(saved, vm->frames[cfi].vars, sizeof(Var*)*MAX_VARS);
    Stack saved_lv = vm->frames[cfi].LocalVariables;
    stack_init(&vm->frames[cfi].LocalVariables);

    for (int p=0; p<vm->frames[cfi].param_count; p++) {
        int pidx = vm->frames[cfi].param_indices[p];
        char *pname = saved[pidx]->name;
        if (char_id_map_exists(&vm->frames[caller_fi].VarIndexer, pname)) {
            int src = char_id_map_get(&vm->frames[caller_fi].VarIndexer, pname);
            vm->frames[cfi].vars[pidx] = vm->frames[caller_fi].vars[src];
        }
    }

    Var *tmp_alloc[MAX_VARS]; memset(tmp_alloc, 0, sizeof(tmp_alloc));
    for (int v=0; v<vm->frames[cfi].var_count; v++) {
        if (!vm->frames[cfi].vars[v]) {
            vm->frames[cfi].vars[v] = calloc(1, sizeof(Var));
            vm->frames[cfi].vars[v]->T     = TYPE_INT;
            vm->frames[cfi].vars[v]->value = calloc(1, sizeof(int));
            if (saved[v]) strncpy(vm->frames[cfi].vars[v]->name, saved[v]->name, VAR_NAME_LENGTH-1);
            tmp_alloc[v] = vm->frames[cfi].vars[v];
        }
    }

    for (int i=count-1; i>=0; i--) {
        char ob[512]; strncpy(ob, lines[i], sizeof(ob)-1);
        char *fw = strtok(skip_lineno(ob), " \t");
        if (!fw || !strcmp(fw,"CALL") || !strcmp(fw,"UNCALL")) continue;
        if      (!strcmp(fw,"PUSHEQ")) op_pusheq_inv(vm,frame_name);
        else if (!strcmp(fw,"MINEQ"))  op_mineq_inv (vm,frame_name);
        else if (!strcmp(fw,"SWAP"))   op_swap_inv  (vm,frame_name);
        else if (!strcmp(fw,"PUSH"))   op_pop       (vm,frame_name);
        else if (!strcmp(fw,"POP"))    op_push      (vm,frame_name);
        else if (!strcmp(fw,"LOCAL"))  op_delocal   (vm,frame_name);
        else if (!strcmp(fw,"DELOCAL"))op_local     (vm,frame_name);
        else if (!strcmp(fw,"SHOW"))   op_show      (vm,frame_name);
    }

    for (int v=0; v<vm->frames[cfi].var_count; v++)
        if (tmp_alloc[v] && vm->frames[cfi].vars[v]==tmp_alloc[v]) {
            free(tmp_alloc[v]->value); free(tmp_alloc[v]); vm->frames[cfi].vars[v]=NULL;
        }

    memcpy(vm->frames[cfi].vars, saved, sizeof(Var*)*MAX_VARS);
    vm->frames[cfi].LocalVariables = saved_lv;
    for (int i=0;i<count;i++) free(lines[i]);
}

/* ======================================================================
 *  thread_entry
 * ====================================================================== */

static void *thread_entry(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    VM         *vm   = args->vm;
    char        fname[VAR_NAME_LENGTH];
    strncpy(fname, args->frame_name, VAR_NAME_LENGTH-1);
    fname[VAR_NAME_LENGTH-1] = '\0';
    current_thread_args = args;

    char *ptr = args->start_ptr;

    while (ptr && *ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl='\0';
        char lb[512]; strncpy(lb, ptr, sizeof(lb)-1);
        char *fw = strtok(skip_lineno(lb), " \t");

        if (!fw || strncmp(fw,"THREAD_",7)==0 || !strcmp(fw,"PAR_END"))
            { *nl='\n'; break; }

        if (!strcmp(fw,"PAR_START")) {
            *nl='\n';
            ParBlock pb = scan_par_block(nl + 1);
            exec_par_threads(vm, args->buffer, fname, &pb, 0);
            ptr = pb.after_end ? pb.after_end : nl + 1;
            continue;
        }

        if      (!strcmp(fw,"SHOW"))    op_show   (vm,fname);
        else if (!strcmp(fw,"PUSHEQ"))  op_pusheq (vm,fname);
        else if (!strcmp(fw,"MINEQ"))   op_mineq  (vm,fname);
        else if (!strcmp(fw,"SWAP"))    op_swap   (vm,fname);
        else if (!strcmp(fw,"PUSH") || !strcmp(fw,"SSEND")) op_push(vm,fname);
        else if (!strcmp(fw,"POP")  || !strcmp(fw,"SRECV")) op_pop (vm,fname);
        else if (!strcmp(fw,"LOCAL"))   op_local  (vm,fname);
        else if (!strcmp(fw,"DELOCAL")) op_delocal(vm,fname);
        else if (!strcmp(fw,"EVAL"))    op_eval   (vm,fname);
        else if (!strcmp(fw,"ASSERT"))  op_assert (vm,fname);
        else if (!strcmp(fw,"JMPF")) {
            *nl='\n';
            char *np = op_jmpf(vm,fname,args->buffer);
            ptr = np ? np : nl+1;
            continue;
        }
        else if (!strcmp(fw,"JMP")) {
            *nl='\n';
            ptr = op_jmp(vm,fname,args->buffer);
            continue;
        }
        else if (!strcmp(fw,"CALL")) {
            char *pn = strtok(NULL," \t");
            uint cfi_cur = get_findex(fname);
            pthread_mutex_lock(&var_indexer_mtx);
            uint cfi = clone_frame_for_thread(vm, pn);
            pthread_mutex_unlock(&var_indexer_mtx);
            int pc=vm->frames[cfi].param_count, *pi=vm->frames[cfi].param_indices;
            Var *sv[64]; for(int k=0;k<pc;k++) sv[k]=vm->frames[cfi].vars[pi[k]];
            Stack slv=vm->frames[cfi].LocalVariables; stack_init(&vm->frames[cfi].LocalVariables);
            char thread_key[VAR_NAME_LENGTH]; make_thread_frame_key(pn,thread_key,sizeof(thread_key));
            char *p=NULL; int ii=0; while((p=strtok(NULL," \t"))&&ii<pc){ int si=char_id_map_get(&vm->frames[cfi_cur].VarIndexer,p); vm->frames[cfi].vars[pi[ii++]]=vm->frames[cfi_cur].vars[si]; }
            vm_run_BT(vm,args->buffer,thread_key);
            for(int k=0;k<pc;k++) vm->frames[cfi].vars[pi[k]]=sv[k];
            vm->frames[cfi].LocalVariables=slv;
        }
        else if (!strcmp(fw,"UNCALL")) {
            char *pn = strtok(NULL," \t");
            uint cfi_cur = get_findex(fname);
            pthread_mutex_lock(&var_indexer_mtx);
            uint cfi = clone_frame_for_thread(vm, pn);
            pthread_mutex_unlock(&var_indexer_mtx);
            int pc=vm->frames[cfi].param_count, *pi=vm->frames[cfi].param_indices;
            Var *sv[64]; for(int k=0;k<pc;k++) sv[k]=vm->frames[cfi].vars[pi[k]];
            Stack slv=vm->frames[cfi].LocalVariables; stack_init(&vm->frames[cfi].LocalVariables);
            char thread_key[VAR_NAME_LENGTH]; make_thread_frame_key(pn,thread_key,sizeof(thread_key));
            char *p=NULL; int ii=0; while((p=strtok(NULL," \t"))&&ii<pc){ int si=char_id_map_get(&vm->frames[cfi_cur].VarIndexer,p); vm->frames[cfi].vars[pi[ii++]]=vm->frames[cfi_cur].vars[si]; }
            invert_op_to_line(vm,thread_key,args->buffer,vm->frames[cfi].end_addr-1,vm->frames[cfi].addr+1);
            for(int k=0;k<pc;k++) vm->frames[cfi].vars[pi[k]]=sv[k];
            vm->frames[cfi].LocalVariables=slv;
        }
        else if (!strcmp(fw,"DECL") || !strcmp(fw,"PARAM") || !strcmp(fw,"LABEL")) { /* skip */ }
        else { fprintf(stderr,"[THREAD] op sconosciuta: '%s'\n",fw); exit(EXIT_FAILURE); }

        *nl='\n'; ptr=nl+1;
    }

    /* Sveglia sender pendente prima di terminare */
    if (args->sender_to_notify) {
        ThreadArgs *s = args->sender_to_notify;
        args->sender_to_notify = NULL;
        notify_sender_turn_done(s);
    }
    pthread_mutex_lock(args->done_mtx);
    args->finished = 1;
    pthread_cond_signal(args->done_cond);
    pthread_mutex_unlock(args->done_mtx);
    return NULL;
}

/* ======================================================================
 *  vm_run_BT
 * ====================================================================== */

void vm_run_BT(VM *vm, char *buffer, char *frame_name_init)
{
    char *orig = strdup(buffer);
    char  fname[VAR_NAME_LENGTH];
    strncpy(fname, frame_name_init, VAR_NAME_LENGTH-1);
    fname[VAR_NAME_LENGTH-1] = '\0';

    typedef struct {
        char *return_ptr;
        char  caller_frame[VAR_NAME_LENGTH];
        Var  *saved_params[64];
        int   saved_param_count, callee_findex;
        Stack saved_local_vars;
        int   is_recursive_clone;
    } CallRecord;

    CallRecord cs[MAX_FRAMES]; int cs_top = -1;
    uint  si  = char_id_map_get(&FrameIndexer, fname);
    char *ptr = go_to_line(orig, vm->frames[si].addr + 1);
    if (!ptr) { fprintf(stderr,"ERROR: '%s' non trovato\n",fname); free(orig); return; }

    while (*ptr) {
        char *nl = strchr(ptr, '\n'); if (!nl) break; *nl='\0';
        char lb[512]; strncpy(lb, ptr, sizeof(lb)-1);
        char *fw = strtok(skip_lineno(lb), " \t");

        if (!fw) { *nl='\n'; ptr=nl+1; continue; }

        if (!strcmp(fw,"END_PROC")) {
            uint fi = get_findex(fname);
            if (stack_size(&vm->frames[fi].LocalVariables) > -1)
                vm_fatal("[VM] END_PROC: variabili LOCAL non chiuse!\n");
            *nl='\n';
            if (cs_top >= 0) {
                int cfi = cs[cs_top].callee_findex;
                for (int k=0;k<cs[cs_top].saved_param_count;k++)
                    vm->frames[cfi].vars[vm->frames[cfi].param_indices[k]] = cs[cs_top].saved_params[k];
                vm->frames[cfi].LocalVariables = cs[cs_top].saved_local_vars;
                if (cs[cs_top].is_recursive_clone)
                    for (int k=0;k<vm->frames[cfi].param_count;k++) {
                        int pidx = vm->frames[cfi].param_indices[k];
                        free(vm->frames[cfi].vars[pidx]); vm->frames[cfi].vars[pidx]=NULL;
                    }
                ptr = cs[cs_top].return_ptr;
                strncpy(fname, cs[cs_top].caller_frame, VAR_NAME_LENGTH-1);
                cs_top--;
            } else break;
            continue;
        }

        if (!strcmp(fw,"CALL")) {
            char *pn = strtok(NULL," \t");
            uint cfi_cur = get_findex(fname);
            char base[VAR_NAME_LENGTH]; strncpy(base,fname,VAR_NAME_LENGTH-1);
            char *at=strchr(base,'@'); if(at)*at='\0';
            int is_rec = !strcmp(pn,base);
            int new_depth = 0;
            if (is_rec) { char *at2=strchr(fname,'@'); int cd=at2?atoi(at2+1):0; new_depth=cd+1; }
            uint cfi = is_rec ? clone_frame_for_depth(vm,pn,new_depth) : char_id_map_get(&FrameIndexer,pn);
            if (cs_top+1 >= MAX_FRAMES) vm_fatal("[VM] CALL: stack overflow!\n");
            cs_top++;
            *nl='\n';
            cs[cs_top].return_ptr         = nl+1;
            cs[cs_top].is_recursive_clone = is_rec;
            cs[cs_top].callee_findex      = cfi;
            strncpy(cs[cs_top].caller_frame, fname, VAR_NAME_LENGTH-1);
            int pc=vm->frames[cfi].param_count, *pi=vm->frames[cfi].param_indices;
            cs[cs_top].saved_param_count  = pc;
            cs[cs_top].saved_local_vars   = vm->frames[cfi].LocalVariables;
            stack_init(&vm->frames[cfi].LocalVariables);
            for(int k=0;k<pc;k++) cs[cs_top].saved_params[k]=vm->frames[cfi].vars[pi[k]];
            char *p=NULL; int ii=0;
            while((p=strtok(NULL," \t"))&&ii<pc) {
                if (!char_id_map_exists(&vm->frames[cfi_cur].VarIndexer,p)) { fprintf(stderr,"[VM] CALL: '%s' non def\n",p); exit(EXIT_FAILURE); }
                int src=char_id_map_get(&vm->frames[cfi_cur].VarIndexer,p);
                if (!vm->frames[cfi_cur].vars[src]) { fprintf(stderr,"[VM] CALL: '%s' NULL\n",p); exit(EXIT_FAILURE); }
                vm->frames[cfi].vars[pi[ii++]] = vm->frames[cfi_cur].vars[src];
            }
            if (ii!=pc) { fprintf(stderr,"ERROR: params mismatch '%s'\n",pn); exit(EXIT_FAILURE); }
            if (is_rec) { uint bfi=char_id_map_get(&FrameIndexer,pn); vm->frames[bfi].recursion_depth=new_depth; }
            char nfname[VAR_NAME_LENGTH];
            if (is_rec) make_frame_key(pn,new_depth,nfname,sizeof(nfname));
            else        strncpy(nfname,pn,VAR_NAME_LENGTH-1);
            strncpy(fname,nfname,VAR_NAME_LENGTH-1);
            ptr = go_to_line(orig, vm->frames[cfi].addr+1);
            if (!ptr) vm_fatal("[VM] CALL: indirizzo non trovato!\n");
            continue;
        }

        if (!strcmp(fw,"UNCALL")) {
            char *pn=strtok(NULL," \t"); uint cfi=char_id_map_get(&FrameIndexer,pn), curi=get_findex(fname);
            int pc=vm->frames[cfi].param_count, *pi=vm->frames[cfi].param_indices;
            Var *sv[64]; for(int k=0;k<pc;k++) sv[k]=vm->frames[cfi].vars[pi[k]];
            char *p=NULL; int ii=0; while((p=strtok(NULL," \t"))&&ii<pc){ int src=char_id_map_get(&vm->frames[curi].VarIndexer,p); vm->frames[cfi].vars[pi[ii++]]=vm->frames[curi].vars[src]; }
            if (ii!=pc) { fprintf(stderr,"ERROR: params mismatch UNCALL '%s'\n",pn); exit(EXIT_FAILURE); }
            invert_op_to_line(vm,pn,orig,vm->frames[cfi].end_addr-1,vm->frames[cfi].addr+1);
            for(int k=0;k<pc;k++) vm->frames[cfi].vars[pi[k]]=sv[k];
            *nl='\n'; ptr=nl+1; continue;
        }

        if (!strcmp(fw,"PAR_START")) {
            *nl='\n';
            ParBlock pb = scan_par_block(nl+1);
            exec_par_threads(vm, orig, fname, &pb, 1 /* dup buffer per thread */);
            ptr = pb.after_end ? pb.after_end : nl+1;
            continue;
        }

        if      (!strcmp(fw,"LOCAL"))   op_local  (vm,fname);
        else if (!strcmp(fw,"DELOCAL")) op_delocal(vm,fname);
        else if (!strcmp(fw,"SHOW"))    op_show   (vm,fname);
        else if (!strcmp(fw,"PUSHEQ"))  op_pusheq (vm,fname);
        else if (!strcmp(fw,"MINEQ"))   op_mineq  (vm,fname);
        else if (!strcmp(fw,"SWAP"))    op_swap   (vm,fname);
        else if (!strcmp(fw,"PUSH") || !strcmp(fw,"SSEND")) op_push(vm,fname);
        else if (!strcmp(fw,"POP")  || !strcmp(fw,"SRECV")) op_pop (vm,fname);
        else if (!strcmp(fw,"EVAL"))    op_eval   (vm,fname);
        else if (!strcmp(fw,"ASSERT"))  op_assert (vm,fname);
        else if (!strcmp(fw,"JMPF")) {
            *nl='\n'; char *np=op_jmpf(vm,fname,orig);
            ptr = np ? np : nl+1; continue;
        }
        else if (!strcmp(fw,"JMP")) {
            *nl='\n'; ptr=op_jmp(vm,fname,orig); continue;
        }
        else if (!strcmp(fw,"PROC") || !strcmp(fw,"PARAM") || !strcmp(fw,"LABEL") ||
                 !strcmp(fw,"DECL") || !strcmp(fw,"HALT"))  { /* skip */ }
        else { fprintf(stderr,"[VM] op sconosciuta: '%s'\n",fw); exit(EXIT_FAILURE); }

        *nl='\n'; ptr=nl+1;
    }
    free(orig);
}

/* ======================================================================
 *  vm_exec — prima passata (raccolta frame/dichiarazioni)
 * ====================================================================== */

void vm_exec(VM *vm, char *buffer)
{
    char *orig = strdup(buffer);
    char *ptr  = buffer;
    int   line = 1;

    while (*ptr) {
        char *nl = strchr(ptr, '\n');
        if (!nl) break;
        *nl = '\0';

        if (strlen(ptr) > 6) {
            char *fw = strtok(ptr + 6, " \t");

            if (!strcmp(fw,"START")) {
                char_id_map_init(&FrameIndexer);
                vm->frame_top = -1;

            } else if (!strcmp(fw,"PROC")) {
                char *name = strtok(NULL," \t");
                uint  idx  = char_id_map_get(&FrameIndexer, name);
                vm->frame_top = idx;
                char_id_map_init(&vm->frames[idx].VarIndexer);
                stack_init(&vm->frames[idx].LocalVariables);
                strncpy(vm->frames[idx].name, name, VAR_NAME_LENGTH-1);
                vm->frames[idx].addr = line;

            } else if (!strcmp(fw,"END_PROC")) {
                char *name = strtok(NULL," \t");
                vm->frames[vm->frame_top].end_addr = line;
                if (!strcmp(name,"main"))
                    vm_run_BT(vm, orig, "main");

            } else if (!strcmp(fw,"DECL")) {
                char *type = strtok(NULL," \t"), *vn = strtok(NULL," \t");
                int   vi   = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, vn);
                if (vm->frames[vm->frame_top].vars[vi]) vm_fatal("[VM] Variabile già definita!\n");
                vm->frames[vm->frame_top].vars[vi] = malloc(sizeof(Var));
                alloc_var(vm->frames[vm->frame_top].vars[vi], type, vn);
                vm->frames[vm->frame_top].vars[vi]->is_local = 0;
                if (vi >= vm->frames[vm->frame_top].var_count)
                    vm->frames[vm->frame_top].var_count = vi+1;

            } else if (!strcmp(fw,"PARAM")) {
                char *vtype = strtok(NULL," \t"), *vn = strtok(NULL," \t");
                int   vi    = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, vn);
                if (vm->frames[vm->frame_top].vars[vi]) vm_fatal("[VM] PARAM già definito!\n");
                vm->frames[vm->frame_top].vars[vi] = calloc(1, sizeof(Var));
                vm->frames[vm->frame_top].vars[vi]->T        = TYPE_PARAM;
                vm->frames[vm->frame_top].vars[vi]->is_local = 0;
                strncpy(vm->frames[vm->frame_top].vars[vi]->name, vn, VAR_NAME_LENGTH-1);
                (void)vtype; /* il tipo è già TYPE_PARAM */
                if (vi >= vm->frames[vm->frame_top].var_count)
                    vm->frames[vm->frame_top].var_count = vi+1;
                vm->frames[vm->frame_top].param_indices[vm->frames[vm->frame_top].param_count++] = vi;

            } else if (!strcmp(fw,"LABEL")) {
                char *ln  = strtok(NULL," \t");
                uint  li  = char_id_map_get(&vm->frames[vm->frame_top].LabelIndexer, ln);
                vm->frames[vm->frame_top].label[li] = line;

            } else if (!strcmp(fw,"HALT")) { /* nop */
            } else {
                /* tutte le istruzioni runtime sono ignorate in questa fase */
            }
        }
        *nl='\n'; ptr=nl+1; line++;
    }
    free(orig);
}

/* ======================================================================
 *  vm_dump
 * ====================================================================== */

void vm_dump(VM *vm)
{
    printf("=== VM dump ===\n");
    for (int i=0; i<=vm->frame_top; i++) {
        Frame *f = &vm->frames[i];
        if (strcmp(f->name,"main") != 0) continue;
        for (int j=0; j<f->var_count; j++) {
            Var *v = f->vars[j]; if (!v) continue;
            printf("%s: ", v->name);
            if (v->T == TYPE_INT) printf("%d", *(v->value));
            else { printf("["); for(size_t k=0;k<v->stack_len;k++){printf("%d",v->value[k]); if(k+1<v->stack_len)printf(", ");} printf("]"); }
            printf("\n");
        }
    }
}

/* ======================================================================
 *  Entry point
 * ====================================================================== */
#define AST_BUFFER (1024*10)
#include "check_if_reversibility.h"

void vm_run_from_string(const char *bytecode)
{
    char ast[AST_BUFFER];
    ast[0] = '\0';
    strncat(ast, bytecode, sizeof(ast)-1);

    if (vm_check_if_reversibility(ast) > 0)
        fprintf(stderr, "Warning: il bytecode potrebbe non essere completamente reversibile.\n");

    VM vm; memset(&vm, 0, sizeof(VM));
    vm_exec(&vm, ast);
    vm_dump(&vm);
}