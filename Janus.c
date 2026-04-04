#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
pthread_t main_thread_id;
__u_short is_main_thread() {
    return pthread_equal(pthread_self(), main_thread_id);
}
#include "char_id_map.h"
CharIdMap FrameIndexer;
#include "stack.h"

#define uint unsigned int
#define perror(msg) do { printf(msg); exit(EXIT_FAILURE); } while(0)

typedef enum {
    TYPE_INT   = 0,
    TYPE_STACK = 1,
    TYPE_CHANNEL = 2,
    TYPE_PARAM = 3
} ValueType;

typedef struct ThreadArgs ThreadArgs; // forward declaration

typedef struct Waiter {
    pthread_cond_t  cond;
    int             ready;
    struct Waiter  *next;
    ThreadArgs     *thread_args;
} Waiter;

typedef struct ThreadArgs ThreadArgs;
typedef struct {
    pthread_mutex_t mtx;
    Waiter *send_q_head;
    Waiter *send_q_tail;
    Waiter *recv_q_head;
    Waiter *recv_q_tail;
    ThreadArgs *sender_args;
    ThreadArgs *receiver_args;
} Channel;

#define VAR_NAME_LENGTH  100
#define VAR_STACK_MAX_SIZE 128
#define VAR_CHANNEL_MAX_SIZE 128
typedef struct Var {
    ValueType T;
    int      *value;
    size_t    stack_len;
    int       is_local;
    char      name[VAR_NAME_LENGTH];
    Channel *channel;
} Var;

#define MAX_VARS  100
#define MAX_LABEL 100
#define MAX_NESTED 100
typedef struct {
    CharIdMap VarIndexer;
    Stack     LocalVariables;
    Var      *vars[MAX_VARS];
    int       var_count;
    CharIdMap LabelIndexer;
    uint      label[MAX_LABEL];
    char      name[VAR_NAME_LENGTH];
    uint      addr;
    uint      end_addr;
    int       param_indices[64];
    int       param_count;
    int       loop_restart_i[MAX_NESTED];
    int       loop_bottom_i[MAX_NESTED];
    int       loop_counter;
    int       recursion_depth;
} Frame;

#define MAX_FRAMES 100
typedef struct {
    Frame frames[MAX_FRAMES];
    int   frame_top;
} VM;
struct ThreadArgs {
    VM        *vm;
    char      *buffer;
    char       frame_name[VAR_NAME_LENGTH];
    char      *start_ptr;
    int        finished;
    int        blocked;
    int        turn_done;
    pthread_t  tid;
    pthread_mutex_t *done_mtx;
    pthread_cond_t  *done_cond;
    ThreadArgs *sender_to_notify;   /* ← NUOVO */
};

static __thread ThreadArgs *current_thread_args = NULL;
static __thread char *strtok_saveptr = NULL;
static __thread uint thread_val_IF = 0;
static pthread_mutex_t var_indexer_mtx = PTHREAD_MUTEX_INITIALIZER;

#define strtok(str, delim) strtok_r((str), (delim), &strtok_saveptr)

/* ======================================================================
 *  HELPER FUNCTIONS
 * ====================================================================== */

static void make_frame_key(const char *name, int depth, char *out, size_t out_sz)
{
    if (depth == 0)
        snprintf(out, out_sz, "%s", name);
    else
        snprintf(out, out_sz, "%s@%d", name, depth);
}

/* Crea una chiave per un frame clonato per-thread: "procname@t<tid>" */
static void make_thread_frame_key(const char *proc_name, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s@t%lu", proc_name, (unsigned long)pthread_self());
}

static uint get_findex(const char *frame_name)
{
    if (!char_id_map_exists(&FrameIndexer, frame_name)) {
        fprintf(stderr, "[VM] get_findex: frame '%s' non trovato!\n", frame_name);
        exit(EXIT_FAILURE);
    }
    return char_id_map_get(&FrameIndexer, frame_name);
}

static int resolve_value(VM *vm, uint Findex, const char *token)
{
    if (char_id_map_exists(&vm->frames[Findex].VarIndexer, token)) {
        uint idx = char_id_map_get(&vm->frames[Findex].VarIndexer, token);
        return *(vm->frames[Findex].vars[idx]->value);
    }
    return (int) strtol(token, NULL, 10);
}

static Var *get_var(VM *vm, uint Findex, const char *name, const char *op_name)
{
    if (!char_id_map_exists(&vm->frames[Findex].VarIndexer, name)) {
        fprintf(stderr, "[VM] %s: variabile '%s' non definita!\n", op_name, name);
        exit(EXIT_FAILURE);
    }
    uint idx = char_id_map_get(&vm->frames[Findex].VarIndexer, name);
    if (vm->frames[Findex].vars[idx] == NULL) {
        fprintf(stderr, "[VM] %s: variabile '%s' è NULL (già deallocata?)\n", op_name, name);
        exit(EXIT_FAILURE);
    }
    return vm->frames[Findex].vars[idx];
}

static char *go_to_line(char *buffer, uint line)
{
    if (!buffer)  return NULL;
    if (line == 0) return buffer;
    uint cur = 1;
    char *p = buffer;
    while (*p) {
        if (cur == line) return p;
        if (*p == '\n') cur++;
        p++;
    }
    return NULL;
}

/* ======================================================================
 *  HELPER: cancella variabile dal frame
 * ====================================================================== */

void delete_var(Var *vars[], int *size, int n)
{
    if (n < 0 || n >= *size) { printf("Indice fuori range!\n"); return; }
    free(vars[n]->value);
    free(vars[n]);
    vars[n] = NULL;
}

#include "ops_arith.h"

/* ======================================================================
 *  SWAP
 * ====================================================================== */

void op_swap(VM *vm, const char *frame_name)
{
    char *ID1    = strtok(NULL, " \t");
    char *ID2    = strtok(NULL, " \t");
    uint  Findex = get_findex(frame_name);
    Var  *v1     = get_var(vm, Findex, ID1, "SWAP");
    Var  *v2     = get_var(vm, Findex, ID2, "SWAP");
    int   tmp    = *(v1->value);
    *(v1->value) = *(v2->value);
    *(v2->value) = tmp;
}

/* ======================================================================
 *  STACK/CHANNEL OPS
 * ====================================================================== */
void op_push(VM *vm, const char *frame_name);
void op_pop(VM *vm, const char *frame_name);
static void notify_sender_turn_done(ThreadArgs *sender)
{
    if (!sender) return;
    pthread_mutex_lock(sender->done_mtx);
    sender->turn_done = 1;
    pthread_cond_signal(sender->done_cond);
    pthread_mutex_unlock(sender->done_mtx);
}
static int op_wait(Channel *ch, int is_send)
{ if (!is_send && current_thread_args && current_thread_args->sender_to_notify) {
        ThreadArgs *s = current_thread_args->sender_to_notify;
        current_thread_args->sender_to_notify = NULL;
        notify_sender_turn_done(s);
    }
    //fprintf(stderr, "[op_wait] is_send=%d send_q=%p recv_q=%p\n", is_send, ch->send_q_head, ch->recv_q_head);

    pthread_mutex_lock(&ch->mtx);

    Waiter *self = malloc(sizeof(Waiter));
    pthread_cond_init(&self->cond, NULL);
    self->ready = 0;
    self->next = NULL;
    self->thread_args = current_thread_args;

    if (is_send) {

        // 🔥 MATCH IMMEDIATO CON RECV
        if (ch->recv_q_head) {
            Waiter *w = ch->recv_q_head;
            ch->recv_q_head = w->next;
            if (!ch->recv_q_head) ch->recv_q_tail = NULL;

            // salva sender
            ch->sender_args = self->thread_args;

            // sveglia receiver
            w->ready = 1;
            pthread_cond_signal(&w->cond);

            pthread_mutex_unlock(&ch->mtx);

            // 🔥 FIX DETERMINISTICO: sender aspetta il receiver
            if (current_thread_args) {
                pthread_mutex_lock(current_thread_args->done_mtx);
                current_thread_args->blocked = 1;

                while (!current_thread_args->turn_done &&
                       !current_thread_args->finished)
                    pthread_cond_wait(current_thread_args->done_cond,
                                      current_thread_args->done_mtx);

                current_thread_args->turn_done = 0;
                current_thread_args->blocked = 0;
                pthread_mutex_unlock(current_thread_args->done_mtx);
            }

            pthread_cond_destroy(&self->cond);
            free(self);
            return 0;
        }

        // 🔵 NO MATCH → METTITI IN CODA
        if (ch->send_q_tail)
            ch->send_q_tail->next = self;
        else
            ch->send_q_head = self;
        ch->send_q_tail = self;

        //fprintf(stderr, "[WAIT send queue] self=%p current=%p\n", self, current_thread_args);

        if (current_thread_args) {
            pthread_mutex_lock(current_thread_args->done_mtx);
            current_thread_args->blocked = 1;
            pthread_cond_signal(current_thread_args->done_cond);
            pthread_mutex_unlock(current_thread_args->done_mtx);
        }

        while (!self->ready)
            pthread_cond_wait(&self->cond, &ch->mtx);

        if (current_thread_args) {
            pthread_mutex_lock(current_thread_args->done_mtx);
            current_thread_args->blocked = 0;
            pthread_mutex_unlock(current_thread_args->done_mtx);
        }

        pthread_mutex_unlock(&ch->mtx);

        pthread_cond_destroy(&self->cond);
        free(self);
        return 1;
    }

    else {

        //fprintf(stderr, "[WAIT recv] send_q_head=%p\n", ch->send_q_head);

        // 🔥 MATCH IMMEDIATO CON SEND
        if (ch->send_q_head) {
            Waiter *w = ch->send_q_head;
            ch->send_q_head = w->next;
            if (!ch->send_q_head) ch->send_q_tail = NULL;

            //fprintf(stderr, "[WAIT recv match] w=%p send_q_head dopo=%p\n", w, ch->send_q_head);

            // salva sender
            ch->sender_args = w->thread_args;

            // sveglia sender
            w->ready = 1;
            pthread_cond_signal(&w->cond);

            pthread_mutex_unlock(&ch->mtx);

            pthread_cond_destroy(&self->cond);
            free(self);
            return 0;
        }

        // 🔵 NO MATCH → METTITI IN CODA
        if (ch->recv_q_tail)
            ch->recv_q_tail->next = self;
        else
            ch->recv_q_head = self;
        ch->recv_q_tail = self;

        //fprintf(stderr, "[WAIT recv queue] current_thread_args=%p\n", current_thread_args);

        if (current_thread_args) {
            pthread_mutex_lock(current_thread_args->done_mtx);
            current_thread_args->blocked = 1;
            pthread_cond_signal(current_thread_args->done_cond);
            pthread_mutex_unlock(current_thread_args->done_mtx);
        }

        while (!self->ready)
            pthread_cond_wait(&self->cond, &ch->mtx);

        if (current_thread_args) {
            pthread_mutex_lock(current_thread_args->done_mtx);
            current_thread_args->blocked = 0;
            pthread_mutex_unlock(current_thread_args->done_mtx);
        }

        pthread_mutex_unlock(&ch->mtx);

        pthread_cond_destroy(&self->cond);
        free(self);
        return 0;
    }
}
void op_push(VM *vm, const char *frame_name)
{
    char *C_val   = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t") != NULL) perror("[VM] PUSH: troppi parametri!\n");

    uint Findex = get_findex(frame_name);
    int  val;

    if (char_id_map_exists(&vm->frames[Findex].VarIndexer, C_val)) {
        Var *src = get_var(vm, Findex, C_val, "PUSH");
        val = *(src->value);
        *(src->value) = 0;
    } else {
        val = (int) strtoul(C_val, NULL, 10);
    }

    if (!char_id_map_exists(&vm->frames[Findex].VarIndexer, C_stack))
        perror("[VM] PUSH: stack destinazione non trovato!\n");

    uint Sindex    = char_id_map_get(&vm->frames[Findex].VarIndexer, C_stack);
    Var *stack_var = vm->frames[Findex].vars[Sindex];
    if (stack_var->T != TYPE_STACK && stack_var->T != TYPE_CHANNEL)
        perror("[VM] PUSH: destinazione non è stack/channel!\n");

    stack_var->value = realloc(stack_var->value, (stack_var->stack_len + 1) * sizeof(int));
    if (!stack_var->value) perror("realloc failed\n");
    stack_var->value[stack_var->stack_len++] = val;

    if (stack_var->T == TYPE_CHANNEL) {
        int was_queued = op_wait(stack_var->channel, 1);
        if (was_queued && current_thread_args) {
            pthread_mutex_lock(current_thread_args->done_mtx);
            while (!current_thread_args->turn_done && !current_thread_args->finished)
                pthread_cond_wait(current_thread_args->done_cond, current_thread_args->done_mtx);
            current_thread_args->turn_done = 0;
            pthread_mutex_unlock(current_thread_args->done_mtx);
        }
    }
}

void op_pop(VM *vm, const char *frame_name)
{
    char *C_dest  = strtok(NULL, " \t");
    char *C_stack = strtok(NULL, " \t");
    if (strtok(NULL, " \t") != NULL) perror("[VM] POP: troppi parametri!\n");

    uint Findex = get_findex(frame_name);

    if (!char_id_map_exists(&vm->frames[Findex].VarIndexer, C_stack))
        perror("[VM] POP: stack non trovato!\n");

    uint Sindex    = char_id_map_get(&vm->frames[Findex].VarIndexer, C_stack);
    Var *stack_var = vm->frames[Findex].vars[Sindex];

    if (stack_var->T != TYPE_STACK && stack_var->T != TYPE_CHANNEL)
        perror("[VM] POP: sorgente non è stack/channel!\n");
    if (stack_var->T == TYPE_STACK && stack_var->stack_len == 0)
        perror("[VM] POP: stack vuoto!\n");

    ThreadArgs *sender_to_notify = NULL;
    int sender_was_waiting = 0;

    if (stack_var->T == TYPE_CHANNEL) {
        pthread_mutex_lock(&stack_var->channel->mtx);
        if (stack_var->channel->send_q_head) {
            sender_to_notify   = stack_var->channel->send_q_head->thread_args;
            sender_was_waiting = 1;
        }
        pthread_mutex_unlock(&stack_var->channel->mtx);
        op_wait(stack_var->channel, 0);
    }

    if (stack_var->T == TYPE_CHANNEL && stack_var->stack_len == 0) {
        fprintf(stderr, "[VM] POP: channel vuoto dopo op_wait!\n");
        exit(EXIT_FAILURE);
    }

    int popped = stack_var->value[--stack_var->stack_len];
    if (stack_var->stack_len > 0)
        stack_var->value = realloc(stack_var->value, stack_var->stack_len * sizeof(int));

    Var *dest = get_var(vm, Findex, C_dest, "POP");
    *(dest->value) += popped;

    if (stack_var->T == TYPE_CHANNEL) {
        ThreadArgs *sender_to_wake = NULL;
        if (sender_was_waiting && sender_to_notify) {
            sender_to_wake = sender_to_notify;
        } else {
            pthread_mutex_lock(&stack_var->channel->mtx);
            sender_to_wake = stack_var->channel->sender_args;
            stack_var->channel->sender_args = NULL;
            pthread_mutex_unlock(&stack_var->channel->mtx);
        }
        /* Non svegliare subito: il receiver finirà il suo turno
           e sveglierà il sender solo quando si ri-blocca */
        if (current_thread_args && sender_to_wake) {
            current_thread_args->sender_to_notify = sender_to_wake;
        }
    }}

/* ======================================================================
 *  SHOW
 * ====================================================================== */

void op_show(VM *vm, const char *frame_name)
{
    char *ID = strtok(NULL, " \t");
    if (strtok(NULL, " \t") != NULL) perror("[VM] SHOW: troppi parametri!\n");

    uint  Findex = get_findex(frame_name);
    Var  *v      = get_var(vm, Findex, ID, "SHOW");

    if (v->T == TYPE_INT) {
        printf("%s: %d\n", ID, *(v->value));
    } else if (v->T == TYPE_STACK) {
        printf("%s: [", ID);
        for (size_t k = 0; k < v->stack_len; k++) {
            printf("%d", v->value[k]);
            if (k + 1 < v->stack_len) printf(", ");
        }
        printf("]\n");
    } else if (v->T == TYPE_CHANNEL) {
        printf("%s: <", ID);
        for (size_t k = 0; k < v->stack_len; k++) {
            printf("%d", v->value[k]);
            if (k + 1 < v->stack_len) printf(", ");
        }
        printf(">\n");
    } else {
        perror("[VM] SHOW su variabile PARAM non linkata!\n");
    }
}

/* ======================================================================
 *  EVAL
 * ====================================================================== */

void op_eval(VM *vm, const char *frame_name)
{
    char *ID      = strtok(NULL, " \t");
    char *C_value = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    Var  *v       = get_var(vm, Findex, ID, "EVAL");
    int   rhs     = resolve_value(vm, Findex, C_value);
    thread_val_IF = (*(v->value) == rhs);
}

/* ======================================================================
 *  SALTI
 * ====================================================================== */

char *op_jmp(VM *vm, const char *frame_name, char *original_buffer)
{
    char *c_label = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    uint  Lindex  = char_id_map_get(&vm->frames[Findex].LabelIndexer, c_label);
    char *new_ptr = go_to_line(original_buffer, vm->frames[Findex].label[Lindex] + 1);
    if (!new_ptr) perror("[VM] JMP: label non trovata!\n");
    return new_ptr;
}

char *op_jmpf(VM *vm, const char *frame_name, char *original_buffer)
{
    char *c_label = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    if (thread_val_IF)
        return NULL;
    if (!char_id_map_exists(&vm->frames[Findex].LabelIndexer, c_label)) {
        exit(EXIT_FAILURE);
    }
    uint  Lindex  = char_id_map_get(&vm->frames[Findex].LabelIndexer, c_label);
    char *new_ptr = go_to_line(original_buffer, vm->frames[Findex].label[Lindex] + 1);
    if (!new_ptr) perror("[VM] JMP: label non trovata!\n");
    return new_ptr;
}

/* ======================================================================
 *  LOCAL / DELOCAL
 * ====================================================================== */

void op_local(VM *vm, const char *frame_name)
{
    char *Vtype    = strtok(NULL, " \t");
    char *Vname    = strtok(NULL, " \t");
    char *c_Vvalue = strtok(NULL, " \t");

    uint Findex = get_findex(frame_name);

    pthread_mutex_lock(&var_indexer_mtx);
    uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, Vname);
    pthread_mutex_unlock(&var_indexer_mtx);

    vm->frames[Findex].vars[Vindex] = malloc(sizeof(Var));
    memset(vm->frames[Findex].vars[Vindex], 0, sizeof(Var));

    if (strcmp(Vtype, "int") == 0) {
        vm->frames[Findex].vars[Vindex]->T     = TYPE_INT;
        vm->frames[Findex].vars[Vindex]->value = malloc(sizeof(int));
        *(vm->frames[Findex].vars[Vindex]->value) = 0;
    } else if (strcmp(Vtype, "stack") == 0) {
        vm->frames[Findex].vars[Vindex]->T         = TYPE_STACK;
        vm->frames[Findex].vars[Vindex]->stack_len = 0;
        vm->frames[Findex].vars[Vindex]->value     = malloc(VAR_STACK_MAX_SIZE * sizeof(int));
    } else if (strcmp(Vtype, "channel") == 0) {
        Var *v = vm->frames[vm->frame_top].vars[Vindex];
        v->T         = TYPE_CHANNEL;
        v->stack_len = 0;
        v->value     = malloc(VAR_CHANNEL_MAX_SIZE * sizeof(int));
        if (!v->channel) {
            v->channel = malloc(sizeof(Channel));
            v->channel->send_q_head   = NULL;
            v->channel->send_q_tail   = NULL;
            v->channel->recv_q_head   = NULL;
            v->channel->recv_q_tail   = NULL;
            v->channel->sender_args   = NULL;
            v->channel->receiver_args = NULL;
        }
        pthread_mutex_init(&v->channel->mtx, NULL);
    } else {
        perror("[VM] LOCAL: tipo non esistente\n");
    }

    strncpy(vm->frames[Findex].vars[Vindex]->name, Vname, VAR_NAME_LENGTH - 1);
    vm->frames[Findex].vars[Vindex]->name[VAR_NAME_LENGTH - 1] = '\0';
    vm->frames[Findex].vars[Vindex]->is_local = 1;

    if (Vindex >= (uint)vm->frames[Findex].var_count)
        vm->frames[Findex].var_count = Vindex + 1;

    Var *dst = vm->frames[Findex].vars[Vindex];

    if (char_id_map_exists(&vm->frames[Findex].VarIndexer, c_Vvalue)) {
        int  SrcIndex = char_id_map_get(&vm->frames[Findex].VarIndexer, c_Vvalue);
        Var *src      = vm->frames[Findex].vars[SrcIndex];
        if (src->T == TYPE_INT)
            *(dst->value) = *(src->value);
        else if (src->T == TYPE_STACK) {
            dst->stack_len = src->stack_len;
            memcpy(dst->value, src->value, src->stack_len * sizeof(int));
        } else {
            perror("[VM] LOCAL: copia da PARAM non linkato\n");
        }
    } else {
        if (dst->T == TYPE_INT)
            *(dst->value) = (int) strtol(c_Vvalue, NULL, 10);
        else if (dst->T == TYPE_STACK) {
            if (strcmp(c_Vvalue, "nil") == 0)
                dst->stack_len = 0;
            else
                perror("[VM] LOCAL: valore stack non compatibile\n");
        }
    }

    stack_push(&vm->frames[Findex].LocalVariables, dst);
}

void op_delocal(VM *vm, const char *frame_name)
{
    char *Vtype    = strtok(NULL, " \t");
    char *Vname    = strtok(NULL, " \t");
    char *c_Vvalue = strtok(NULL, " \t");

    uint Findex = get_findex(frame_name);

    int Vvalue;
    if (char_id_map_exists(&vm->frames[Findex].VarIndexer, c_Vvalue))
        Vvalue = *(vm->frames[Findex].vars[
                     char_id_map_get(&vm->frames[Findex].VarIndexer, c_Vvalue)]->value);
    else
        Vvalue = (int) strtoul(c_Vvalue, NULL, 10);

    Var *V = stack_pop(&vm->frames[Findex].LocalVariables);
    if (strcmp(Vtype, (V->T == 0 ? "int" : (V->T == 1 ? "stack" : "channel"))) != 0) {
        printf("[VM] DELOCAL: tipo errato! atteso %s, trovato %s\n",
               (V->T == 0 ? "int" : (V->T == 1 ? "stack" : "channel")), Vtype);
        exit(EXIT_FAILURE);
    }

    if (strcmp(Vtype, "int") == 0) {
        if (Vvalue == *(V->value)) {
            pthread_mutex_lock(&var_indexer_mtx);
            delete_var(vm->frames[Findex].vars, &vm->frames[Findex].var_count,
                       char_id_map_get(&vm->frames[Findex].VarIndexer, Vname));
            pthread_mutex_unlock(&var_indexer_mtx);
        } else {
            printf("[VM] DELOCAL: valore finale diverso dall'atteso! (%s, %d, %d)\n",
                   Vname, Vvalue, *(V->value));
            exit(1);
        }
    } else if (strcmp(Vtype, "stack") == 0) {
        if (V->stack_len == 0 && strcmp(c_Vvalue, "nil") == 0) {
            pthread_mutex_lock(&var_indexer_mtx);
            delete_var(vm->frames[Findex].vars, &vm->frames[Findex].var_count,
                       char_id_map_get(&vm->frames[Findex].VarIndexer, Vname));
            pthread_mutex_unlock(&var_indexer_mtx);
        } else if (V->stack_len != 0)
            perror("[VM] DELOCAL: stack deve essere nil!\n");
        else
            perror("[VM] DELOCAL: valore finale di stack diverso da nil!\n");
    } else if (strcmp(Vtype, "channel") == 0) {
        if (V->stack_len == 0 && strcmp(c_Vvalue, "empty") == 0) {
            pthread_mutex_lock(&var_indexer_mtx);
            delete_var(vm->frames[Findex].vars, &vm->frames[Findex].var_count,
                       char_id_map_get(&vm->frames[Findex].VarIndexer, Vname));
            pthread_mutex_unlock(&var_indexer_mtx);
        } else if (V->stack_len != 0)
            perror("[VM] DELOCAL: channel deve essere empty!\n");
        else
            perror("[VM] DELOCAL: valore finale di stack diverso da nil!\n");
    }
}

/* ======================================================================
 *  ASSERT
 * ====================================================================== */

void op_assert(VM *vm, const char *frame_name)
{
    char *ID1 = strtok(NULL, " \t");
    char *ID2 = strtok(NULL, " \t");
    if (!ID1 || !ID2) {
        fprintf(stderr, "[VM] ASSERT: argomenti mancanti\n");
        return;
    }
    uint Findex = get_findex(frame_name);
    unsigned long val1 = resolve_value(vm, Findex, ID1);
    unsigned long val2 = resolve_value(vm, Findex, ID2);
    if (val1 != val2) {
        /* assertion failed - currently non-fatal */
    }
}

/* ======================================================================
 *  Forward declaration
 * ====================================================================== */
void vm_run_BT(VM *vm, char *buffer, char *frame_name_init);

static char *skip_lineno(char *line)
{
    while (*line >= '0' && *line <= '9') line++;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

/* ======================================================================
 *  clone_frame_for_depth  (ricorsione)
 * ====================================================================== */
static uint clone_frame_for_depth(VM *vm, const char *proc_name, int depth)
{
    char key[VAR_NAME_LENGTH];
    make_frame_key(proc_name, depth, key, sizeof(key));

    if (char_id_map_exists(&FrameIndexer, key))
        return char_id_map_get(&FrameIndexer, key);

    uint base_fi  = char_id_map_get(&FrameIndexer, proc_name);
    uint clone_fi = char_id_map_get(&FrameIndexer, key);

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
    snprintf(clone->name, VAR_NAME_LENGTH, "%s", key);
    memcpy(clone->label, base->label, sizeof(base->label));

    stack_init(&clone->LocalVariables);

    for (int k = 0; k < clone->param_count; k++) {
        int pidx = clone->param_indices[k];
        clone->vars[pidx] = malloc(sizeof(Var));
        memset(clone->vars[pidx], 0, sizeof(Var));
        strncpy(clone->vars[pidx]->name,
                base->vars[pidx]->name, VAR_NAME_LENGTH - 1);
        clone->vars[pidx]->T        = TYPE_PARAM;
        clone->vars[pidx]->is_local = 0;
    }

    return clone_fi;
}

/* ======================================================================
 *  clone_frame_for_thread  (parallelismo)
 *  Crea il frame "procname@t<tid>" per isolare le chiamate per-thread.
 *  Deve essere chiamata con var_indexer_mtx già acquisito.
 * ====================================================================== */
static uint clone_frame_for_thread(VM *vm, const char *proc_name)
{
    char key[VAR_NAME_LENGTH];
    make_thread_frame_key(proc_name, key, sizeof(key));

    /* Se esiste già per questo thread, riusalo */
    if (char_id_map_exists(&FrameIndexer, key))
        return char_id_map_get(&FrameIndexer, key);

    uint base_fi  = char_id_map_get(&FrameIndexer, proc_name);
    uint clone_fi = char_id_map_get(&FrameIndexer, key); /* alloca la entry */

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
    snprintf(clone->name, VAR_NAME_LENGTH, "%s", key);
    memcpy(clone->label, base->label, sizeof(base->label));

    stack_init(&clone->LocalVariables);

    /* Alloca slot PARAM; i valori verranno linkati dal chiamante */
    for (int k = 0; k < clone->param_count; k++) {
        int pidx = clone->param_indices[k];
        clone->vars[pidx] = malloc(sizeof(Var));
        memset(clone->vars[pidx], 0, sizeof(Var));
        strncpy(clone->vars[pidx]->name,
                base->vars[pidx]->name, VAR_NAME_LENGTH - 1);
        clone->vars[pidx]->T        = TYPE_PARAM;
        clone->vars[pidx]->is_local = 0;
    }

    return clone_fi;
}

/* ======================================================================
 *  LoopDescriptor / collect_loops / line_loop_zone
 * ====================================================================== */

typedef struct {
    uint eval_entry_line;
    char eval_entry_id[64];
    char eval_entry_val[64];
    uint jmpf_err_line;
    uint from_start_line;
    uint from_end_line;
    uint from_err_line;
    uint eval_exit_line;
    char eval_exit_id[64];
    char eval_exit_val[64];
    uint jmpf_start_line;
} LoopDescriptor;

static int collect_loops(VM *vm, const char *frame_name,
                          char *original_buffer,
                          LoopDescriptor *loops_out, int max_loops)
{
    char base_name[VAR_NAME_LENGTH];
    strncpy(base_name, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base_name, '@');
    if (at) *at = '\0';

    uint findex     = char_id_map_get(&FrameIndexer, base_name);
    uint start_line = vm->frames[findex].addr + 1;
    char *ptr       = go_to_line(original_buffer, start_line);
    int   nloops    = 0;

    int    in_loop        = 0;
    uint   pending_eval_line = 0;
    char   pending_eval_id[64]  = {0};
    char   pending_eval_val[64] = {0};

    while (ptr && *ptr != '\0') {
        char *newline = strchr(ptr, '\n');
        if (!newline) break;
        *newline = '\0';

        char line_buf[512];
        strncpy(line_buf, ptr, sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';

        uint cur_line   = (uint)atoi(line_buf);
        char *clean     = skip_lineno(line_buf);
        char *firstWord = strtok(clean, " \t");

        if (!firstWord) { *newline = '\n'; ptr = newline + 1; continue; }

        if (strcmp(firstWord, "EVAL") == 0) {
            pending_eval_line = cur_line;
            char *id  = strtok(NULL, " \t");
            char *val = strtok(NULL, " \t");
            strncpy(pending_eval_id,  id  ? id  : "", 63);
            strncpy(pending_eval_val, val ? val : "", 63);

        } else if (strcmp(firstWord, "JMPF") == 0 && !in_loop) {
            char *lname = strtok(NULL, " \t");
            if (lname && strncmp(lname, "FROM_ERR", 8) == 0 && nloops < max_loops) {
                loops_out[nloops].eval_entry_line = pending_eval_line;
                strncpy(loops_out[nloops].eval_entry_id,  pending_eval_id,  63);
                strncpy(loops_out[nloops].eval_entry_val, pending_eval_val, 63);
                loops_out[nloops].jmpf_err_line = cur_line;
                in_loop = 1;
            }

        } else if (strcmp(firstWord, "LABEL") == 0 && in_loop) {
            char *lname = strtok(NULL, " \t");
            if (!lname) { *newline = '\n'; ptr = newline + 1; continue; }

            if (strncmp(lname, "FROM_START", 10) == 0)
                loops_out[nloops].from_start_line = cur_line;
            else if (strncmp(lname, "FROM_END", 8) == 0)
                loops_out[nloops].from_end_line = cur_line;
            else if (strncmp(lname, "FROM_ERR", 8) == 0) {
                loops_out[nloops].from_err_line = cur_line;
                in_loop = 0;
                nloops++;
            }

        } else if (strcmp(firstWord, "JMPF") == 0 && in_loop) {
            char *lname = strtok(NULL, " \t");
            if (lname && strncmp(lname, "FROM_START", 10) == 0) {
                loops_out[nloops].eval_exit_line  = pending_eval_line;
                strncpy(loops_out[nloops].eval_exit_id,  pending_eval_id,  63);
                strncpy(loops_out[nloops].eval_exit_val, pending_eval_val, 63);
                loops_out[nloops].jmpf_start_line = cur_line;
            }

        } else if (strcmp(firstWord, "END_PROC") == 0) {
            *newline = '\n';
            break;
        }

        *newline = '\n';
        ptr = newline + 1;
    }
    return nloops;
}

typedef enum {
    LOOP_ZONE_NONE,
    LOOP_ZONE_EVAL_ENTRY,
    LOOP_ZONE_JMPF_ERR,
    LOOP_ZONE_START_LABEL,
    LOOP_ZONE_EVAL_EXIT,
    LOOP_ZONE_JMPF_START,
    LOOP_ZONE_END_LABEL,
    LOOP_ZONE_ERR_LABEL,
} LoopZone;

static LoopZone line_loop_zone(uint line, LoopDescriptor *loops, int nloops, int *loop_idx)
{
    for (int i = 0; i < nloops; i++) {
        if (line == loops[i].eval_entry_line) { *loop_idx = i; return LOOP_ZONE_EVAL_ENTRY; }
        if (line == loops[i].jmpf_err_line)   { *loop_idx = i; return LOOP_ZONE_JMPF_ERR;   }
        if (line == loops[i].from_start_line) { *loop_idx = i; return LOOP_ZONE_START_LABEL; }
        if (line == loops[i].eval_exit_line)  { *loop_idx = i; return LOOP_ZONE_EVAL_EXIT;  }
        if (line == loops[i].jmpf_start_line) { *loop_idx = i; return LOOP_ZONE_JMPF_START; }
        if (line == loops[i].from_end_line)   { *loop_idx = i; return LOOP_ZONE_END_LABEL;  }
        if (line == loops[i].from_err_line)   { *loop_idx = i; return LOOP_ZONE_ERR_LABEL;  }
    }
    *loop_idx = -1;
    return LOOP_ZONE_NONE;
}

/* ======================================================================
 *  IfDescriptor / collect_ifs / line_if_zone
 * ====================================================================== */

typedef struct {
    uint eval_entry_line;
    char eval_entry_id[64];
    char eval_entry_val[64];
    uint jmpf_else_line;
    uint jmp_fi_line;
    uint else_label_line;
    uint fi_label_line;
    uint eval_exit_line;
    char eval_exit_id[64];
    char eval_exit_val[64];
    uint assert_line;
} IfDescriptor;

static int collect_ifs(VM *vm, const char *frame_name,
                       char *original_buffer,
                       IfDescriptor *ifs_out, int max_ifs)
{
    char base_name[VAR_NAME_LENGTH];
    strncpy(base_name, frame_name, VAR_NAME_LENGTH - 1);
    char *at = strchr(base_name, '@');
    if (at) *at = '\0';

    uint findex     = char_id_map_get(&FrameIndexer, base_name);
    uint start_line = vm->frames[findex].addr + 1;
    char *ptr       = go_to_line(original_buffer, start_line);
    int   nifs      = 0;

    int  in_if           = 0;
    uint pending_eval_line = 0;
    char pending_eval_id[64]  = {0};
    char pending_eval_val[64] = {0};

    while (ptr && *ptr != '\0') {
        char *newline = strchr(ptr, '\n');
        if (!newline) break;
        *newline = '\0';

        char line_buf[512];
        strncpy(line_buf, ptr, sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';

        uint cur_line   = (uint)atoi(line_buf);
        char *clean     = skip_lineno(line_buf);
        char *firstWord = strtok(clean, " \t");

        if (!firstWord) { *newline = '\n'; ptr = newline + 1; continue; }

        if (strcmp(firstWord, "EVAL") == 0) {
            pending_eval_line = cur_line;
            char *id  = strtok(NULL, " \t");
            char *val = strtok(NULL, " \t");
            strncpy(pending_eval_id,  id  ? id  : "", 63);
            strncpy(pending_eval_val, val ? val : "", 63);

        } else if (strcmp(firstWord, "JMPF") == 0 && !in_if) {
            char *lname = strtok(NULL, " \t");
            if (lname && strncmp(lname, "ELSE_", 5) == 0 && nifs < max_ifs) {
                ifs_out[nifs].eval_entry_line = pending_eval_line;
                strncpy(ifs_out[nifs].eval_entry_id,  pending_eval_id,  63);
                strncpy(ifs_out[nifs].eval_entry_val, pending_eval_val, 63);
                ifs_out[nifs].jmpf_else_line = cur_line;
                in_if = 1;
            }

        } else if (strcmp(firstWord, "JMP") == 0 && in_if) {
            char *lname = strtok(NULL, " \t");
            if (lname && strncmp(lname, "FI_", 3) == 0)
                ifs_out[nifs].jmp_fi_line = cur_line;

        } else if (strcmp(firstWord, "LABEL") == 0 && in_if) {
            char *lname = strtok(NULL, " \t");
            if (!lname) { *newline = '\n'; ptr = newline + 1; continue; }

            if (strncmp(lname, "ELSE_", 5) == 0)
                ifs_out[nifs].else_label_line = cur_line;
            else if (strncmp(lname, "FI_", 3) == 0)
                ifs_out[nifs].fi_label_line = cur_line;

        } else if (strcmp(firstWord, "ASSERT") == 0 && in_if) {
            ifs_out[nifs].eval_exit_line  = pending_eval_line;
            strncpy(ifs_out[nifs].eval_exit_id,  pending_eval_id,  63);
            strncpy(ifs_out[nifs].eval_exit_val, pending_eval_val, 63);
            ifs_out[nifs].assert_line = cur_line;
            in_if = 0;
            nifs++;

        } else if (strcmp(firstWord, "END_PROC") == 0) {
            *newline = '\n'; break;
        }

        *newline = '\n';
        ptr = newline + 1;
    }
    return nifs;
}

typedef enum {
    IF_ZONE_NONE,
    IF_ZONE_EVAL_ENTRY,
    IF_ZONE_JMPF_ELSE,
    IF_ZONE_JMP_FI,
    IF_ZONE_ELSE_LABEL,
    IF_ZONE_FI_LABEL,
    IF_ZONE_EVAL_EXIT,
    IF_ZONE_ASSERT,
} IfZone;

static IfZone line_if_zone(uint line, IfDescriptor *ifs, int nifs, int *if_idx)
{
    for (int i = 0; i < nifs; i++) {
        if (line == ifs[i].eval_entry_line) { *if_idx = i; return IF_ZONE_EVAL_ENTRY; }
        if (line == ifs[i].jmpf_else_line)  { *if_idx = i; return IF_ZONE_JMPF_ELSE;  }
        if (line == ifs[i].jmp_fi_line)     { *if_idx = i; return IF_ZONE_JMP_FI;     }
        if (line == ifs[i].else_label_line) { *if_idx = i; return IF_ZONE_ELSE_LABEL; }
        if (line == ifs[i].fi_label_line)   { *if_idx = i; return IF_ZONE_FI_LABEL;   }
        if (line == ifs[i].eval_exit_line)  { *if_idx = i; return IF_ZONE_EVAL_EXIT;  }
        if (line == ifs[i].assert_line)     { *if_idx = i; return IF_ZONE_ASSERT;     }
    }
    *if_idx = -1;
    return IF_ZONE_NONE;
}

static void do_eval(VM *vm, uint findex, const char *id, const char *val)
{
    int rhs = 0;
    if (char_id_map_exists(&vm->frames[findex].VarIndexer, val)) {
        uint vi = char_id_map_get(&vm->frames[findex].VarIndexer, val);
        rhs = *(vm->frames[findex].vars[vi]->value);
    } else {
        rhs = (int)strtol(val, NULL, 10);
    }
    uint vi2 = char_id_map_get(&vm->frames[findex].VarIndexer, id);
    thread_val_IF = (*(vm->frames[findex].vars[vi2]->value) == rhs);
}

void invert_op_to_line(VM *vm, const char *frame_name, char *buffer,
                       uint start, uint stop);

static void exec_branch_inverse(VM *vm, char *original_buffer,
                                const char *frame_name,
                                uint from_line, uint to_line,
                                uint caller_findex)
{
    char *lines[512];
    int   count = 0;

    char *ptr = go_to_line(original_buffer, from_line);
    if (!ptr) return;
    while (ptr && *ptr != '\0' && count < 512) {
        char *newline = strchr(ptr, '\n');
        if (!newline) break;
        *newline = '\0';
        uint cur_line = (uint)atoi(ptr);
        if (cur_line >= to_line) { *newline = '\n'; break; }
        lines[count++] = strdup(ptr);
        *newline = '\n';
        ptr = newline + 1;
    }

    uint callee_findex = char_id_map_get(&FrameIndexer, frame_name);

    Var *saved_vars[MAX_VARS];
    memcpy(saved_vars, vm->frames[callee_findex].vars, sizeof(Var*) * MAX_VARS);
    Stack saved_lv = vm->frames[callee_findex].LocalVariables;
    stack_init(&vm->frames[callee_findex].LocalVariables);

    for (int p = 0; p < vm->frames[callee_findex].param_count; p++) {
        int   pidx  = vm->frames[callee_findex].param_indices[p];
        char *pname = saved_vars[pidx]->name;
        if (char_id_map_exists(&vm->frames[caller_findex].VarIndexer, pname)) {
            int src = char_id_map_get(&vm->frames[caller_findex].VarIndexer, pname);
            vm->frames[callee_findex].vars[pidx] = vm->frames[caller_findex].vars[src];
        }
    }

    Var *temp_alloc[MAX_VARS];
    memset(temp_alloc, 0, sizeof(temp_alloc));
    for (int v = 0; v < vm->frames[callee_findex].var_count; v++) {
        if (vm->frames[callee_findex].vars[v] == NULL) {
            vm->frames[callee_findex].vars[v] = calloc(1, sizeof(Var));
            vm->frames[callee_findex].vars[v]->T     = TYPE_INT;
            vm->frames[callee_findex].vars[v]->value = calloc(1, sizeof(int));
            if (saved_vars[v])
                strncpy(vm->frames[callee_findex].vars[v]->name,
                        saved_vars[v]->name, VAR_NAME_LENGTH - 1);
            temp_alloc[v] = vm->frames[callee_findex].vars[v];
        }
    }

    for (int i = count - 1; i >= 0; i--) {
        char op_buf[512];
        strncpy(op_buf, lines[i], sizeof(op_buf) - 1);
        op_buf[sizeof(op_buf) - 1] = '\0';

        char *clean = skip_lineno(op_buf);
        char *fw    = strtok(clean, " \t");
        if (!fw) continue;

        if (strcmp(fw, "CALL") == 0 || strcmp(fw, "UNCALL") == 0) continue;

        if      (strcmp(fw, "PUSHEQ") == 0) op_pusheq_inv(vm, frame_name);
        else if (strcmp(fw, "MINEQ")  == 0) op_mineq_inv (vm, frame_name);

        else if (strcmp(fw, "SWAP")   == 0) op_swap_inv  (vm, frame_name);
        else if (strcmp(fw, "PUSH")   == 0) op_pop       (vm, frame_name);
        else if (strcmp(fw, "POP")    == 0) op_push      (vm, frame_name);
        else if (strcmp(fw, "LOCAL")  == 0) op_delocal   (vm, frame_name);
        else if (strcmp(fw, "DELOCAL")== 0) op_local     (vm, frame_name);
        else if (strcmp(fw, "SHOW")   == 0) op_show      (vm, frame_name);
    }

    for (int v = 0; v < vm->frames[callee_findex].var_count; v++) {
        if (temp_alloc[v] && vm->frames[callee_findex].vars[v] == temp_alloc[v]) {
            free(temp_alloc[v]->value);
            free(temp_alloc[v]);
            vm->frames[callee_findex].vars[v] = NULL;
        }
    }

    memcpy(vm->frames[callee_findex].vars, saved_vars, sizeof(Var*) * MAX_VARS);
    vm->frames[callee_findex].LocalVariables = saved_lv;

    for (int i = 0; i < count; i++) free(lines[i]);
}

static int line_is_inside_if(uint line, IfDescriptor *ifs, int nifs)
{
    for (int i = 0; i < nifs; i++) {
        if (line > ifs[i].jmpf_else_line && line < ifs[i].fi_label_line)
            return 1;
    }
    return 0;
}

void invert_op_to_line(VM *vm, const char *frame_name, char *buffer,
                       uint start, uint stop)
{
    (void)start; (void)stop;

    char *original_buffer = strdup(buffer);
    if (!original_buffer) { fprintf(stderr, "[UNCALL] strdup fallita\n"); exit(EXIT_FAILURE); }

    char base_name[VAR_NAME_LENGTH];
    strncpy(base_name, frame_name, VAR_NAME_LENGTH - 1);
    char *at_sign = strchr(base_name, '@');
    if (at_sign) *at_sign = '\0';

    uint findex_reset = char_id_map_get(&FrameIndexer, base_name);
    stack_init(&vm->frames[findex_reset].LocalVariables);

#define MAX_LOOPS    32
#define MAX_IFS      32
#define MAX_INV_CALL 64
#define MAX_LINES    1024

    LoopDescriptor loops[MAX_LOOPS];
    int nloops = collect_loops(vm, frame_name, original_buffer, loops, MAX_LOOPS);

    IfDescriptor ifs[MAX_IFS];
    int nifs = collect_ifs(vm, frame_name, original_buffer, ifs, MAX_IFS);

    char cur_frame[VAR_NAME_LENGTH];
    strncpy(cur_frame, frame_name, VAR_NAME_LENGTH - 1);
    cur_frame[VAR_NAME_LENGTH - 1] = '\0';

    uint findex     = char_id_map_get(&FrameIndexer, cur_frame);
    uint start_line = vm->frames[findex_reset].addr + 1;

    char *line_ptrs[MAX_LINES];
    uint  line_nos [MAX_LINES];
    int   nlines = 0;

    char *ptr = go_to_line(original_buffer, start_line);
    while (ptr && *ptr != '\0' && nlines < MAX_LINES) {
        char *newline = strchr(ptr, '\n');
        if (!newline) break;
        *newline = '\0';

        char tmp[512];
        strncpy(tmp, ptr, sizeof(tmp) - 1);
        tmp[sizeof(tmp)-1] = '\0';
        char *fw = strtok(skip_lineno(tmp), " \t");
        if (fw && strcmp(fw, "END_PROC") == 0) { *newline = '\n'; break; }

        line_ptrs[nlines] = strdup(ptr);
        line_nos [nlines] = (uint)atoi(ptr);
        nlines++;

        *newline = '\n';
        ptr = newline + 1;
    }

    int i = nlines - 1;
    while (i >= 0) {
        char op_buf[512];
        strncpy(op_buf, line_ptrs[i], sizeof(op_buf) - 1);
        op_buf[sizeof(op_buf) - 1] = '\0';

        uint  cur_line  = line_nos[i];
        char *clean     = skip_lineno(op_buf);
        char *firstWord = strtok(clean, " \t");

        if (!firstWord) { i--; continue; }

        int loop_idx = -1;
        LoopZone lzone = line_loop_zone(cur_line, loops, nloops, &loop_idx);

        if (lzone == LOOP_ZONE_EVAL_ENTRY || lzone == LOOP_ZONE_EVAL_EXIT ||
            lzone == LOOP_ZONE_START_LABEL || lzone == LOOP_ZONE_END_LABEL ||
            lzone == LOOP_ZONE_ERR_LABEL) {
            i--; continue;
        }
        if (lzone == LOOP_ZONE_JMPF_ERR) {
            do_eval(vm, findex,
                    loops[loop_idx].eval_entry_id,
                    loops[loop_idx].eval_entry_val);
            if (thread_val_IF) {
                i--;
            } else {
                int target = -1;
                for (int j = nlines - 1; j >= 0; j--)
                    if (line_nos[j] == loops[loop_idx].jmpf_start_line) { target = j; break; }
                if (target < 0) { fprintf(stderr, "[UNCALL] jmpf_start non trovato\n"); exit(1); }
                i = target - 1;
            }
            continue;
        }
        if (lzone == LOOP_ZONE_JMPF_START) {
            do_eval(vm, findex,
                    loops[loop_idx].eval_exit_id,
                    loops[loop_idx].eval_exit_val);
            if (thread_val_IF) {
                i--;
            } else {
                int target = -1;
                for (int j = 0; j < nlines; j++)
                    if (line_nos[j] == loops[loop_idx].jmpf_err_line) { target = j; break; }
                if (target < 0) { fprintf(stderr, "[UNCALL] jmpf_err non trovato\n"); exit(1); }
                i = target - 1;
            }
            continue;
        }

        int if_idx = -1;
        IfZone izone = line_if_zone(cur_line, ifs, nifs, &if_idx);

        if (izone == IF_ZONE_EVAL_ENTRY || izone == IF_ZONE_EVAL_EXIT ||
            izone == IF_ZONE_ELSE_LABEL || izone == IF_ZONE_FI_LABEL  ||
            izone == IF_ZONE_ASSERT     || izone == IF_ZONE_JMP_FI) {
            i--; continue;
        }

        if (izone == IF_ZONE_JMPF_ELSE) {
            int depth = vm->frames[findex_reset].recursion_depth;

            for (int d = 0; d < depth; d++) {
                Stack saved_lv = vm->frames[findex].LocalVariables;
                stack_init(&vm->frames[findex].LocalVariables);
                exec_branch_inverse(vm, original_buffer, cur_frame,
                                    ifs[if_idx].else_label_line + 1,
                                    ifs[if_idx].fi_label_line,
                                    findex);
                vm->frames[findex].LocalVariables = saved_lv;
            }

            {
                Stack saved_lv = vm->frames[findex].LocalVariables;
                stack_init(&vm->frames[findex].LocalVariables);
                exec_branch_inverse(vm, original_buffer, cur_frame,
                                    ifs[if_idx].jmpf_else_line + 1,
                                    ifs[if_idx].jmp_fi_line,
                                    findex);
                vm->frames[findex].LocalVariables = saved_lv;
            }

            int target = -1;
            for (int j = i - 1; j >= 0; j--) {
                if (line_nos[j] == ifs[if_idx].eval_entry_line) { target = j; break; }
            }
            i = (target >= 0) ? target - 1 : i - 1;
            continue;
        }

        if (line_is_inside_if(cur_line, ifs, nifs)) {
            i--; continue;
        }

        if (strcmp(firstWord, "CALL") == 0) {
            char *proc_name = strtok(NULL, " \t");
            uint  callee_fi = char_id_map_get(&FrameIndexer, proc_name);
            uint  caller_fi = char_id_map_get(&FrameIndexer, frame_name);
            int   param_count   = vm->frames[callee_fi].param_count;
            int  *param_indices = vm->frames[callee_fi].param_indices;

            Var *saved[64];
            for (int k = 0; k < param_count; k++)
                saved[k] = vm->frames[callee_fi].vars[param_indices[k]];

            char *param = NULL; int j = 0;
            while ((param = strtok(NULL, " \t")) != NULL && j < param_count) {
                int src_idx = char_id_map_get(&vm->frames[caller_fi].VarIndexer, param);
                vm->frames[callee_fi].vars[param_indices[j]] =
                    vm->frames[caller_fi].vars[src_idx];
                j++;
            }

            invert_op_to_line(vm, proc_name, original_buffer,
                              vm->frames[callee_fi].addr + 1,
                              vm->frames[callee_fi].end_addr - 1);

            for (int k = 0; k < param_count; k++)
                vm->frames[callee_fi].vars[param_indices[k]] = saved[k];

            i--; continue;
        }

        if (strcmp(firstWord, "UNCALL") == 0) {
            char *proc_name = strtok(NULL, " \t");
            uint  callee_fi = char_id_map_get(&FrameIndexer, proc_name);
            uint  caller_fi = findex;
            int   param_count   = vm->frames[callee_fi].param_count;
            int  *param_indices = vm->frames[callee_fi].param_indices;

            Var *saved[64];
            for (int k = 0; k < param_count; k++)
                saved[k] = vm->frames[callee_fi].vars[param_indices[k]];

            char *param = NULL; int j = 0;
            while ((param = strtok(NULL, " \t")) != NULL && j < param_count) {
                int src_idx = char_id_map_get(&vm->frames[caller_fi].VarIndexer, param);
                vm->frames[callee_fi].vars[param_indices[j]] =
                    vm->frames[caller_fi].vars[src_idx];
                j++;
            }
            char callee_name[VAR_NAME_LENGTH];
            strncpy(callee_name, proc_name, VAR_NAME_LENGTH - 1);
            vm_run_BT(vm, original_buffer, callee_name);
            for (int k = 0; k < param_count; k++)
                vm->frames[callee_fi].vars[param_indices[k]] = saved[k];
            i--; continue;
        }

        if      (strcmp(firstWord, "PUSHEQ") == 0) op_pusheq_inv(vm, cur_frame);
        else if (strcmp(firstWord, "MINEQ")  == 0) op_mineq_inv (vm, cur_frame);
       
        else if (strcmp(firstWord, "SWAP")   == 0) op_swap_inv  (vm, cur_frame);
        else if (strcmp(firstWord, "PUSH")   == 0) op_pop       (vm, cur_frame);
        else if (strcmp(firstWord, "POP")    == 0) op_push      (vm, cur_frame);
        else if (strcmp(firstWord, "LOCAL")  == 0) op_delocal   (vm, cur_frame);
        else if (strcmp(firstWord, "DELOCAL")== 0) op_local     (vm, cur_frame);
        else if (strcmp(firstWord, "SHOW")   == 0) op_show      (vm, cur_frame);
        else if (strcmp(firstWord, "PARAM")  == 0) { /* skip */ }
        else if (strcmp(firstWord, "LABEL")  == 0) { /* skip */ }
        else if (strcmp(firstWord, "EVAL")   == 0) { /* skip */ }
        else if (strcmp(firstWord, "JMPF")   == 0) { /* skip */ }
        else if (strcmp(firstWord, "JMP")    == 0) { /* skip */ }
        else if (strcmp(firstWord, "ASSERT") == 0) { /* skip */ }
        else if (strcmp(firstWord, "DECL")   == 0) { /* skip */ }
        else if (strcmp(firstWord, "HALT")   == 0) { /* skip */ }
        else {
            fprintf(stderr, "[UNCALL] op sconosciuta: '%s'\n", firstWord);
            exit(EXIT_FAILURE);
        }

        i--;
    }

    for (int j = 0; j < nlines; j++) free(line_ptrs[j]);
    free(original_buffer);

#undef MAX_INV_CALL
#undef MAX_LOOPS
#undef MAX_IFS
#undef MAX_LINES
}

/* ======================================================================
 *  vm_run_BT
 * ====================================================================== */

static void *thread_entry(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    VM         *vm   = args->vm;
    char        fname[VAR_NAME_LENGTH];
    strncpy(fname, args->frame_name, VAR_NAME_LENGTH - 1);
    fname[VAR_NAME_LENGTH - 1] = '\0';

    current_thread_args = args;

    char *ptr = args->start_ptr;

    while (ptr && *ptr != '\0') {
        char *newline = strchr(ptr, '\n');
        if (!newline) break;
        *newline = '\0';

        char line_buf[512];
        strncpy(line_buf, ptr, sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';

        char *clean     = skip_lineno(line_buf);
        char *firstWord = strtok(clean, " \t");

        if (!firstWord) { *newline = '\n'; ptr = newline + 1; continue; }

        if (strncmp(firstWord, "THREAD_", 7) == 0 ||
            strcmp(firstWord, "PAR_END")     == 0) {
            *newline = '\n';
            break;
        }

        if (strcmp(firstWord, "PAR_START") == 0) {
            *newline = '\n';
            char *par_ptr = newline + 1;

            char *thread_starts[16];
            int   n_threads = 0;
            int   depth = 1;
            char *scan = par_ptr;
            char *par_end_ptr = NULL;

            while (*scan && depth > 0) {
                char *nl = strchr(scan, '\n');
                if (!nl) break;
                *nl = '\0';
                char tmp[512];
                strncpy(tmp, scan, sizeof(tmp)-1);
                char *fw = strtok(skip_lineno(tmp), " \t");
                if (fw) {
                    if (strcmp(fw, "PAR_START") == 0) depth++;
                    else if (strcmp(fw, "PAR_END") == 0) {
                        depth--;
                        if (depth == 0) {
                            par_end_ptr = nl;
                            *nl = '\n';
                            break;
                        }
                    } else if (strncmp(fw, "THREAD_", 7) == 0 && depth == 1) {
                        thread_starts[n_threads++] = nl + 1;
                    }
                }
                *nl = '\n';
                scan = nl + 1;
            }

            pthread_mutex_t done_mtx  = PTHREAD_MUTEX_INITIALIZER;
            pthread_cond_t  done_cond = PTHREAD_COND_INITIALIZER;

            ThreadArgs *all_args[16];
            for (int t = 0; t < n_threads; t++) {
                all_args[t] = malloc(sizeof(ThreadArgs));
                all_args[t]->vm        = vm;
                all_args[t]->buffer    = args->buffer;
                all_args[t]->start_ptr = thread_starts[t];
                all_args[t]->finished  = 0;
                all_args[t]->sender_to_notify = NULL;
                all_args[t]->blocked   = 0;
                all_args[t]->turn_done = 0;
                all_args[t]->done_mtx  = &done_mtx;
                all_args[t]->done_cond = &done_cond;
                strncpy(all_args[t]->frame_name, fname, VAR_NAME_LENGTH-1);
            }

            for (int t = 0; t < n_threads; t++) {
                pthread_create(&all_args[t]->tid, NULL, thread_entry, all_args[t]);
                pthread_mutex_lock(&done_mtx);
                while (!all_args[t]->finished && !all_args[t]->blocked)
                    pthread_cond_wait(&done_cond, &done_mtx);
                pthread_mutex_unlock(&done_mtx);
            }

            pthread_mutex_lock(&done_mtx);
            int finished_count = 0;
            while (finished_count < n_threads) {
                finished_count = 0;
                for (int t = 0; t < n_threads; t++)
                    if (all_args[t]->finished) finished_count++;
                if (finished_count < n_threads)
                    pthread_cond_wait(&done_cond, &done_mtx);
            }
            pthread_mutex_unlock(&done_mtx);

            for (int t = 0; t < n_threads; t++) {
                pthread_join(all_args[t]->tid, NULL);
                free(all_args[t]);
            }

            ptr = par_end_ptr ? par_end_ptr + 1 : scan + 1;
            continue;
        }

        if      (strcmp(firstWord, "SHOW")    == 0) op_show   (vm, fname);
        else if (strcmp(firstWord, "PUSHEQ")  == 0) op_pusheq (vm, fname);
        else if (strcmp(firstWord, "MINEQ")   == 0) op_mineq  (vm, fname);
        
        else if (strcmp(firstWord, "SWAP")    == 0) op_swap   (vm, fname);
        else if (strcmp(firstWord, "PUSH")    == 0) op_push   (vm, fname);
        else if (strcmp(firstWord, "POP")     == 0) op_pop    (vm, fname);
        else if (strcmp(firstWord, "LOCAL")   == 0) op_local  (vm, fname);
        else if (strcmp(firstWord, "DELOCAL") == 0) op_delocal(vm, fname);
        else if (strcmp(firstWord, "SSEND")   == 0) op_push   (vm, fname);
        else if (strcmp(firstWord, "SRECV")   == 0) { fprintf(stderr, "[THREAD] SRECV\n"); op_pop(vm, fname); }
        else if (strcmp(firstWord, "EVAL")    == 0) {
            fprintf(stderr, "[THREAD] EVAL\n");
            op_eval(vm, fname);
            fprintf(stderr, "[THREAD] val_IF=%d\n", thread_val_IF);
        }
        else if (strcmp(firstWord, "ASSERT")  == 0) op_assert(vm, fname);
        else if (strcmp(firstWord, "JMPF") == 0) {
            fprintf(stderr, "[THREAD] JMPF val_IF=%d\n", thread_val_IF);
            *newline = '\n';
            char *new_ptr = op_jmpf(vm, fname, args->buffer);
            if (new_ptr) { ptr = new_ptr; continue; }
            ptr = newline + 1;
            continue;
        }
        else if (strcmp(firstWord, "JMP") == 0) {
            *newline = '\n';
            char *new_ptr = op_jmp(vm, fname, args->buffer);
            ptr = new_ptr;
            continue;
        }

        /* ----------------------------------------------------------------
         *  CALL dentro un thread → clone per-thread per evitare la race
         *  condition sui parametri del frame chiamato.
         * -------------------------------------------------------------- */
        else if (strcmp(firstWord, "CALL") == 0) {
            char *proc_name  = strtok(NULL, " \t");
            uint  cur_Findex = get_findex(fname);

            /* Crea (o recupera) il frame clone per questo thread */
            char thread_key[VAR_NAME_LENGTH];
            make_thread_frame_key(proc_name, thread_key, sizeof(thread_key));

            pthread_mutex_lock(&var_indexer_mtx);
            uint Findex = clone_frame_for_thread(vm, proc_name);
            pthread_mutex_unlock(&var_indexer_mtx);

            int   param_count   = vm->frames[Findex].param_count;
            int  *param_indices = vm->frames[Findex].param_indices;

            /* Salva i puntatori attuali e linka le variabili del chiamante */
            Var *saved[64];
            for (int k = 0; k < param_count; k++)
                saved[k] = vm->frames[Findex].vars[param_indices[k]];

            Stack saved_lv = vm->frames[Findex].LocalVariables;
            stack_init(&vm->frames[Findex].LocalVariables);

            char *param = NULL; int ii = 0;
            while ((param = strtok(NULL, " \t")) != NULL && ii < param_count) {
                int src_idx = char_id_map_get(&vm->frames[cur_Findex].VarIndexer, param);
                vm->frames[Findex].vars[param_indices[ii]] =
                    vm->frames[cur_Findex].vars[src_idx];
                ii++;
            }

            vm_run_BT(vm, args->buffer, thread_key);

            /* Ripristina */
            for (int k = 0; k < param_count; k++)
                vm->frames[Findex].vars[param_indices[k]] = saved[k];
            vm->frames[Findex].LocalVariables = saved_lv;
        }

        /* ----------------------------------------------------------------
         *  UNCALL dentro un thread → clone per-thread
         * -------------------------------------------------------------- */
        else if (strcmp(firstWord, "UNCALL") == 0) {
            char *proc_name  = strtok(NULL, " \t");
            uint  cur_Findex = get_findex(fname);

            char thread_key[VAR_NAME_LENGTH];
            make_thread_frame_key(proc_name, thread_key, sizeof(thread_key));

            pthread_mutex_lock(&var_indexer_mtx);
            uint Findex = clone_frame_for_thread(vm, proc_name);
            pthread_mutex_unlock(&var_indexer_mtx);

            int   param_count   = vm->frames[Findex].param_count;
            int  *param_indices = vm->frames[Findex].param_indices;

            Var *saved[64];
            for (int k = 0; k < param_count; k++)
                saved[k] = vm->frames[Findex].vars[param_indices[k]];

            Stack saved_lv = vm->frames[Findex].LocalVariables;
            stack_init(&vm->frames[Findex].LocalVariables);

            char *param = NULL; int ii = 0;
            while ((param = strtok(NULL, " \t")) != NULL && ii < param_count) {
                int src_idx = char_id_map_get(&vm->frames[cur_Findex].VarIndexer, param);
                vm->frames[Findex].vars[param_indices[ii]] =
                    vm->frames[cur_Findex].vars[src_idx];
                ii++;
            }

            invert_op_to_line(vm, thread_key, args->buffer,
                              vm->frames[Findex].end_addr - 1,
                              vm->frames[Findex].addr + 1);

            for (int k = 0; k < param_count; k++)
                vm->frames[Findex].vars[param_indices[k]] = saved[k];
            vm->frames[Findex].LocalVariables = saved_lv;
        }

        else if (strcmp(firstWord, "DECL")  == 0) { /* già allocato */ }
        else if (strcmp(firstWord, "PARAM") == 0) { /* skip */ }
        else if (strcmp(firstWord, "LABEL") == 0) { /* skip */ }
        else {
            fprintf(stderr, "[THREAD] op sconosciuta: '%s'\n", firstWord);
            exit(EXIT_FAILURE);
        }

        *newline = '\n';
        ptr = newline + 1;
    }
    /* Fine thread: se c'è un sender ancora in attesa, sveglialo */
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

void vm_run_BT(VM *vm, char *buffer, char *frame_name_init)
{
    char *original_buffer = strdup(buffer);

    char frame_name[VAR_NAME_LENGTH];
    strncpy(frame_name, frame_name_init, VAR_NAME_LENGTH - 1);
    frame_name[VAR_NAME_LENGTH - 1] = '\0';

    typedef struct {
        char *return_ptr;
        char  caller_frame[VAR_NAME_LENGTH];
        Var  *saved_params[64];
        int   saved_param_count;
        int   callee_findex;
        Stack saved_local_vars;
        int   is_recursive_clone;
    } CallRecord;

    CallRecord call_stack[MAX_FRAMES];
    int call_top = -1;

    uint  start_index = char_id_map_get(&FrameIndexer, frame_name);
    char *ptr = go_to_line(original_buffer, vm->frames[start_index].addr + 1);
    if (!ptr) {
        fprintf(stderr, "ERROR: indirizzo procedura '%s' non trovato\n", frame_name);
        free(original_buffer);
        return;
    }

    while (*ptr != '\0') {
        char *newline = strchr(ptr, '\n');
        if (!newline) break;

        *newline = '\0';

        char line_buf[512];
        strncpy(line_buf, ptr, sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';

        char *clean     = skip_lineno(line_buf);
        char *firstWord = strtok(clean, " \t");

        if (!firstWord) {
            *newline = '\n';
            ptr = newline + 1;
            continue;
        }

        if (strcmp(firstWord, "END_PROC") == 0) {
            uint Findex = get_findex(frame_name);
            if (stack_size(&vm->frames[Findex].LocalVariables) > -1)
                perror("[VM] END_PROC: variabili LOCAL non chiuse con DELOCAL!\n");

            *newline = '\n';

            if (call_top >= 0) {
                int cfi = call_stack[call_top].callee_findex;

                for (int k = 0; k < call_stack[call_top].saved_param_count; k++)
                    vm->frames[cfi].vars[
                        vm->frames[cfi].param_indices[k]
                    ] = call_stack[call_top].saved_params[k];

                vm->frames[cfi].LocalVariables = call_stack[call_top].saved_local_vars;

                if (call_stack[call_top].is_recursive_clone) {
                    for (int k = 0; k < vm->frames[cfi].param_count; k++) {
                        int pidx = vm->frames[cfi].param_indices[k];
                        free(vm->frames[cfi].vars[pidx]);
                        vm->frames[cfi].vars[pidx] = NULL;
                    }
                }

                ptr = call_stack[call_top].return_ptr;
                strncpy(frame_name, call_stack[call_top].caller_frame,
                        VAR_NAME_LENGTH - 1);
                call_top--;
                continue;
            } else {
                break;
            }

        } else if (strcmp(firstWord, "CALL") == 0) {
            char *proc_name  = strtok(NULL, " \t");
            uint  cur_Findex = get_findex(frame_name);

            char cur_base[VAR_NAME_LENGTH];
            strncpy(cur_base, frame_name, VAR_NAME_LENGTH - 1);
            char *at = strchr(cur_base, '@');
            if (at) *at = '\0';

            int is_recursive = (strcmp(proc_name, cur_base) == 0);

            int new_depth = 0;
            if (is_recursive) {
                char *at2 = strchr(frame_name, '@');
                int cur_depth = at2 ? atoi(at2 + 1) : 0;
                new_depth = cur_depth + 1;
            }

            uint Findex;
            int  is_clone = 0;
            if (is_recursive) {
                Findex   = clone_frame_for_depth(vm, proc_name, new_depth);
                is_clone = 1;
            } else {
                Findex = char_id_map_get(&FrameIndexer, proc_name);
            }

            if (call_top + 1 >= MAX_FRAMES)
                perror("[VM] CALL: call stack overflow!\n");

            call_top++;
            *newline = '\n';
            call_stack[call_top].return_ptr         = newline + 1;
            call_stack[call_top].is_recursive_clone = is_clone;
            strncpy(call_stack[call_top].caller_frame, frame_name, VAR_NAME_LENGTH - 1);

            int  param_count   = vm->frames[Findex].param_count;
            int *param_indices = vm->frames[Findex].param_indices;

            call_stack[call_top].callee_findex     = Findex;
            call_stack[call_top].saved_param_count = param_count;
            call_stack[call_top].saved_local_vars  = vm->frames[Findex].LocalVariables;
            stack_init(&vm->frames[Findex].LocalVariables);

            for (int k = 0; k < param_count; k++)
                call_stack[call_top].saved_params[k] =
                    vm->frames[Findex].vars[param_indices[k]];

            char *param = NULL;
            int   ii    = 0;
            while ((param = strtok(NULL, " \t")) != NULL) {
                if (ii >= param_count) {
                    fprintf(stderr, "ERROR: troppi parametri per '%s'\n", proc_name);
                    exit(EXIT_FAILURE);
                }
                int j = param_indices[ii];

                if (!char_id_map_exists(&vm->frames[cur_Findex].VarIndexer, param)) {
                    fprintf(stderr, "[VM] CALL: '%s' non definito nel frame chiamante!\n", param);
                    exit(EXIT_FAILURE);
                }
                int src_idx = char_id_map_get(&vm->frames[cur_Findex].VarIndexer, param);
                Var *src    = vm->frames[cur_Findex].vars[src_idx];
                if (!src) {
                    fprintf(stderr, "[VM] CALL: '%s' è NULL nel frame chiamante!\n", param);
                    exit(EXIT_FAILURE);
                }
                vm->frames[Findex].vars[j] = src;
                ii++;
            }
            if (ii != param_count) {
                fprintf(stderr, "ERROR: attesi %d params, ricevuti %d per '%s'\n",
                        param_count, ii, proc_name);
                exit(EXIT_FAILURE);
            }

            if (is_recursive) {
                uint base_fi = char_id_map_get(&FrameIndexer, proc_name);
                vm->frames[base_fi].recursion_depth = new_depth;
            }

            char new_frame_name[VAR_NAME_LENGTH];
            if (is_recursive)
                make_frame_key(proc_name, new_depth, new_frame_name, sizeof(new_frame_name));
            else
                strncpy(new_frame_name, proc_name, VAR_NAME_LENGTH - 1);

            strncpy(frame_name, new_frame_name, VAR_NAME_LENGTH - 1);
            ptr = go_to_line(original_buffer, vm->frames[Findex].addr + 1);
            if (!ptr) perror("[VM] CALL: indirizzo procedura non trovato!\n");
            continue;

        } else if (strcmp(firstWord, "UNCALL") == 0) {
            char *proc_name  = strtok(NULL, " \t");
            uint  Findex     = char_id_map_get(&FrameIndexer, proc_name);
            uint  cur_Findex = get_findex(frame_name);

            int  param_count   = vm->frames[Findex].param_count;
            int *param_indices = vm->frames[Findex].param_indices;

            Var *saved[64];
            for (int k = 0; k < param_count; k++)
                saved[k] = vm->frames[Findex].vars[param_indices[k]];

            char *param = NULL;
            int   ii    = 0;
            while ((param = strtok(NULL, " \t")) != NULL) {
                if (ii >= param_count) {
                    fprintf(stderr, "ERROR: troppi parametri per UNCALL '%s'\n", proc_name);
                    exit(EXIT_FAILURE);
                }
                int src_idx = char_id_map_get(&vm->frames[cur_Findex].VarIndexer, param);
                vm->frames[Findex].vars[param_indices[ii]] =
                    vm->frames[cur_Findex].vars[src_idx];
                ii++;
            }
            if (ii != param_count) {
                fprintf(stderr, "ERROR: parametri UNCALL mismatch '%s'\n", proc_name);
                exit(EXIT_FAILURE);
            }
            invert_op_to_line(vm, proc_name, original_buffer,
                              vm->frames[Findex].end_addr - 1,
                              vm->frames[Findex].addr + 1);

            for (int k = 0; k < param_count; k++)
                vm->frames[Findex].vars[param_indices[k]] = saved[k];

            *newline = '\n';
            ptr = newline + 1;
            continue;

        } else if (strcmp(firstWord, "LOCAL")   == 0) { op_local  (vm, frame_name);
        } else if (strcmp(firstWord, "DELOCAL") == 0) { op_delocal(vm, frame_name);
        } else if (strcmp(firstWord, "SHOW")    == 0) { op_show   (vm, frame_name);
        } else if (strcmp(firstWord, "PUSHEQ")  == 0) { op_pusheq (vm, frame_name);
        } else if (strcmp(firstWord, "MINEQ")   == 0) { op_mineq  (vm, frame_name);
        } else if (strcmp(firstWord, "SWAP")    == 0) { op_swap   (vm, frame_name);
        } else if (strcmp(firstWord, "PUSH")    == 0) { op_push   (vm, frame_name);
        } else if (strcmp(firstWord, "POP")     == 0) { op_pop    (vm, frame_name);
        } else if (strcmp(firstWord, "EVAL")    == 0) { op_eval   (vm, frame_name);
        } else if (strcmp(firstWord, "ASSERT")  == 0) { op_assert (vm, frame_name);
        } else if (strcmp(firstWord, "JMPF") == 0) {
            *newline = '\n';
            char *new_ptr = op_jmpf(vm, frame_name, original_buffer);
            if (new_ptr) { ptr = new_ptr; continue; }
        } else if (strcmp(firstWord, "JMP") == 0) {
            *newline = '\n';
            char *new_ptr = op_jmp(vm, frame_name, original_buffer);
            ptr = new_ptr;
            continue;
        } else if (strcmp(firstWord, "SSEND") == 0) {
            op_push(vm, frame_name);
        } else if (strcmp(firstWord, "SRECV") == 0) {
            op_pop(vm, frame_name);
        } else if (strcmp(firstWord, "PAR_START") == 0) {
            *newline = '\n';
            char *par_ptr = newline + 1;

            char *thread_starts[16];
            int   n_threads = 0;
            int   depth = 1;
            char *scan = par_ptr;

            while (*scan && depth > 0) {
                char *nl = strchr(scan, '\n');
                if (!nl) break;
                *nl = '\0';
                char tmp[512];
                strncpy(tmp, scan, sizeof(tmp)-1);
                char *fw = strtok(skip_lineno(tmp), " \t");
                if (fw) {
                    if (strcmp(fw, "PAR_START") == 0) depth++;
                    else if (strcmp(fw, "PAR_END") == 0) {
                        depth--;
                        if (depth == 0) { *nl = '\n'; break; }
                    } else if (strncmp(fw, "THREAD_", 7) == 0 && depth == 1) {
                        thread_starts[n_threads++] = nl + 1;
                    }
                }
                *nl = '\n';
                scan = nl + 1;
            }

            pthread_mutex_t done_mtx  = PTHREAD_MUTEX_INITIALIZER;
            pthread_cond_t  done_cond = PTHREAD_COND_INITIALIZER;

            ThreadArgs *all_args[16];
            for (int t = 0; t < n_threads; t++) {
                all_args[t] = malloc(sizeof(ThreadArgs));
                all_args[t]->vm        = vm;
                all_args[t]->buffer    = strdup(original_buffer);
                all_args[t]->start_ptr = thread_starts[t];
                all_args[t]->sender_to_notify = NULL;
                all_args[t]->finished  = 0;
                all_args[t]->blocked   = 0;
                all_args[t]->done_mtx  = &done_mtx;
                all_args[t]->done_cond = &done_cond;
                all_args[t]->turn_done = 0;
                strncpy(all_args[t]->frame_name, frame_name, VAR_NAME_LENGTH-1);
            }

            for (int t = 0; t < n_threads; t++) {
                pthread_create(&all_args[t]->tid, NULL, thread_entry, all_args[t]);
                pthread_mutex_lock(&done_mtx);
                while (!all_args[t]->finished && !all_args[t]->blocked)
                    pthread_cond_wait(&done_cond, &done_mtx);
                pthread_mutex_unlock(&done_mtx);
            }

            pthread_mutex_lock(&done_mtx);
            int finished_count = 0;
            while (finished_count < n_threads) {
                finished_count = 0;
                for (int t = 0; t < n_threads; t++)
                    if (all_args[t]->finished) finished_count++;
                if (finished_count < n_threads)
                    pthread_cond_wait(&done_cond, &done_mtx);
            }
            pthread_mutex_unlock(&done_mtx);

            for (int t = 0; t < n_threads; t++) {
                pthread_join(all_args[t]->tid, NULL);
                free(all_args[t]->buffer);
                free(all_args[t]);
            }

            ptr = scan + 1;
            continue;
        }

        *newline = '\n';
        ptr = newline + 1;
    }

    free(original_buffer);
}

/* ======================================================================
 *  Tutto il resto rimane INVARIATO
 * ====================================================================== */

void delete_frame(VM *vm, int n)
{
    if (n < 0 || n > vm->frame_top) { printf("Indice frame non valido\n"); return; }
    for (int i = n; i < vm->frame_top; i++) vm->frames[i] = vm->frames[i + 1];
    vm->frame_top--;
}

void vm_exec(VM *vm, char *buffer)
{
    char *original_buffer = strdup(buffer);
    char *ptr = buffer;
    int   current_line = 1;

    while (*ptr != '\0') {
        char *newline = strchr(ptr, '\n');
        if (newline != NULL) {
            *newline = '\0';
            if (strlen(ptr) > 6) {
                char *line      = ptr + 6;
                char *firstWord = strtok(line, " \t");

                if (strcmp(firstWord, "START") == 0) {
                    char_id_map_init(&FrameIndexer);
                    vm->frame_top = -1;

                } else if (strcmp(firstWord, "HALT") == 0) {
                    /* nop */

                } else if (strcmp(firstWord, "PROC") == 0) {
                    char *name  = strtok(NULL, " \t");
                    uint  index = char_id_map_get(&FrameIndexer, name);
                    vm->frame_top = index;
                    char_id_map_init(&vm->frames[vm->frame_top].VarIndexer);
                    stack_init(&vm->frames[vm->frame_top].LocalVariables);
                    strncpy(vm->frames[vm->frame_top].name, name, VAR_NAME_LENGTH - 1);
                    vm->frames[vm->frame_top].name[VAR_NAME_LENGTH - 1] = '\0';
                    vm->frames[vm->frame_top].addr = current_line;

                } else if (strcmp(firstWord, "END_PROC") == 0) {
                    char *name = strtok(NULL, " \t");
                    vm->frames[vm->frame_top].end_addr = current_line;
                    if (strcmp(name, "main") == 0) {
                        char *main_name = "main";
                        vm_run_BT(vm, original_buffer, main_name);
                    }

                } else if (strcmp(firstWord, "DECL") == 0) {
                    char *type  = strtok(NULL, " \t");
                    char *Vname = strtok(NULL, " \t");
                    int   Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, Vname);
                    if (stack_size(&vm->frames[vm->frame_top].LocalVariables) > -1)
                        perror("[VM] DECL non permessa: ci sono variabili LOCAL aperte!\n");
                    if (vm->frames[vm->frame_top].vars[Vindex] != NULL)
                        perror("[VM] Variabile già definita!\n");

                    vm->frames[vm->frame_top].vars[Vindex] = malloc(sizeof(Var));
                    memset(vm->frames[vm->frame_top].vars[Vindex], 0, sizeof(Var));

                    if (strcmp(type, "int") == 0) {
                        vm->frames[vm->frame_top].vars[Vindex]->T     = TYPE_INT;
                        vm->frames[vm->frame_top].vars[Vindex]->value = malloc(sizeof(int));
                        *(vm->frames[vm->frame_top].vars[Vindex]->value) = 0;
                    } else if (strcmp(type, "stack") == 0) {
                        vm->frames[vm->frame_top].vars[Vindex]->T         = TYPE_STACK;
                        vm->frames[vm->frame_top].vars[Vindex]->stack_len = 0;
                        vm->frames[vm->frame_top].vars[Vindex]->value     =
                            malloc(VAR_STACK_MAX_SIZE * sizeof(int));
                    } else if (strcmp(type, "channel") == 0) {
                        Var *v = vm->frames[vm->frame_top].vars[Vindex];
                        v->T         = TYPE_CHANNEL;
                        v->stack_len = 0;
                        v->value     = malloc(VAR_CHANNEL_MAX_SIZE * sizeof(int));
                        if (!v->channel) {
                            v->channel = malloc(sizeof(Channel));
                            v->channel->send_q_head   = NULL;
                            v->channel->send_q_tail   = NULL;
                            v->channel->recv_q_head   = NULL;
                            v->channel->recv_q_tail   = NULL;
                            v->channel->sender_args   = NULL;
                            v->channel->receiver_args = NULL;
                        }
                        pthread_mutex_init(&v->channel->mtx, NULL);
                    } else {
                        perror("[VM] DECL: tipo non esistente\n");
                    }

                    if (Vindex >= vm->frames[vm->frame_top].var_count)
                        vm->frames[vm->frame_top].var_count = Vindex + 1;
                    vm->frames[vm->frame_top].vars[Vindex]->is_local = 0;
                    strncpy(vm->frames[vm->frame_top].vars[Vindex]->name, Vname, VAR_NAME_LENGTH - 1);
                    vm->frames[vm->frame_top].vars[Vindex]->name[VAR_NAME_LENGTH - 1] = '\0';

                } else if (strcmp(firstWord, "PARAM") == 0) {
                    char *Vtype = strtok(NULL, " \t");
                    char *Vname = strtok(NULL, " \t");
                    int   Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, Vname);
                    if (vm->frames[vm->frame_top].vars[Vindex] != NULL)
                        perror("[VM] PARAM già definito!\n");

                    vm->frames[vm->frame_top].vars[Vindex] = malloc(sizeof(Var));
                    memset(vm->frames[vm->frame_top].vars[Vindex], 0, sizeof(Var));

                    if (Vindex >= vm->frames[vm->frame_top].var_count)
                        vm->frames[vm->frame_top].var_count = Vindex + 1;

                    if (strcmp(Vtype, "int") == 0)
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_INT;
                    else if (strcmp(Vtype, "stack") == 0)
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_STACK;
                    else if (strcmp(Vtype, "channel") == 0)
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_CHANNEL;
                    else
                        perror("[VM] PARAM: tipo non esistente\n");

                    vm->frames[vm->frame_top].vars[Vindex]->value    = NULL;
                    vm->frames[vm->frame_top].vars[Vindex]->T        = TYPE_PARAM;
                    vm->frames[vm->frame_top].vars[Vindex]->is_local = 0;
                    strncpy(vm->frames[vm->frame_top].vars[Vindex]->name, Vname, VAR_NAME_LENGTH - 1);
                    vm->frames[vm->frame_top].vars[Vindex]->name[VAR_NAME_LENGTH - 1] = '\0';
                    vm->frames[vm->frame_top].param_indices[vm->frames[vm->frame_top].param_count++] = Vindex;

                } else if (strcmp(firstWord, "LABEL") == 0) {
                    char *Lname  = strtok(NULL, " \t");
                    uint  Lindex = char_id_map_get(&vm->frames[vm->frame_top].LabelIndexer, Lname);
                    vm->frames[vm->frame_top].label[Lindex] = current_line;

                } else if (strcmp(firstWord, "LOCAL")   == 0 ||
                           strcmp(firstWord, "DELOCAL") == 0 ||
                           strcmp(firstWord, "CALL")    == 0 ||
                           strcmp(firstWord, "UNCALL")  == 0 ||
                           strcmp(firstWord, "SHOW")    == 0 ||
                           strcmp(firstWord, "PUSHEQ")  == 0 ||
                           strcmp(firstWord, "MINEQ")   == 0 ||
                           strcmp(firstWord, "SWAP")    == 0 ||
                           strcmp(firstWord, "PUSH")    == 0 ||
                           strcmp(firstWord, "POP")     == 0 ||
                           strcmp(firstWord, "EVAL")    == 0 ||
                           strcmp(firstWord, "JMPF")    == 0 ||
                           strcmp(firstWord, "JMP")     == 0 ||
                           strcmp(firstWord, "SSEND")   == 0 ||
                           strcmp(firstWord, "SRECV")   == 0 ||
                           strcmp(firstWord, "PAR_START") == 0 ||
                           strncmp(firstWord, "THREAD_", 7) == 0 ||
                           strcmp(firstWord, "PAR_END") == 0 ||
                           strcmp(firstWord, "ASSERT")  == 0) {
                    /* gestite a runtime */
                } else {
                    printf("[VM] Istruzione sconosciuta: %s\n", firstWord);
                    exit(EXIT_FAILURE);
                }
            } else {
                printf("[VM] Bytecode formattato male!\n");
            }

            *newline = '\n';
            ptr = newline + 1;
            current_line++;
        } else {
            if (strlen(ptr) > 6) printf("%s\n", ptr + 6);
            else printf("[VM] Bytecode formattato male!\n");
            break;
        }
    }
    free(original_buffer);
}

/* ======================================================================
 *  VM DUMP
 * ====================================================================== */
void vm_dump(VM *vm)
{
    printf("=== VM dump ===\n");
    for (int i = 0; i <= vm->frame_top; i++) {
        Frame *f = &vm->frames[i];
        if (strcmp(f->name, "main") != 0) continue;
        for (int j = 0; j < f->var_count; j++) {
            Var *v = f->vars[j];
            if (!v) continue;
            printf("%s: ", v->name);
            if (v->T == TYPE_INT) {
                printf("%d", *(v->value));
            } else {
                printf("[");
                for (size_t k = 0; k < v->stack_len; k++) {
                    printf("%d", v->value[k]);
                    if (k + 1 < v->stack_len) printf(", ");
                }
                printf("]");
            }
            printf("\n");
        }
    }
}

/* ======================================================================
 *  MAIN / vm_run_from_string
 * ====================================================================== */

#define START_BUFFER 256
#define AST_BUFFER  (1024 * 10)
#include "check_if_reversibility.h"
void vm_run_from_string(const char *bytecode)
{
    main_thread_id = pthread_self();
    char ast[AST_BUFFER];
    ast[0] = '\0';
    strncat(ast, bytecode, sizeof(ast) - 1);

    if (vm_check_if_reversibility(ast) > 0) {
        fprintf(stderr, "Warning: Bytecode may not be fully reversible. Check logs for details.\n");
    }

    VM vm;
    memset(&vm, 0, sizeof(VM));
    vm_exec(&vm, ast);
    vm_dump(&vm);
}