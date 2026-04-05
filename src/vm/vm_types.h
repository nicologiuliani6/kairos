#ifndef VM_TYPES_H
#define VM_TYPES_H

#include <pthread.h>
#include "char_id_map.h"
#include "stack.h"

#define uint     unsigned int
#define vm_fatal(msg) do { fprintf(stderr, msg); exit(EXIT_FAILURE); } while(0)

/* ======================================================================
 *  Tipi fondamentali
 * ====================================================================== */

typedef enum {
    TYPE_INT     = 0,
    TYPE_STACK   = 1,
    TYPE_CHANNEL = 2,
    TYPE_PARAM   = 3
} ValueType;

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

#define VAR_NAME_LENGTH      100
#define VAR_STACK_MAX_SIZE   128
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
typedef struct {
    Frame frames[MAX_FRAMES];
    int   frame_top;
} VM;

struct ThreadArgs {
    VM        *vm;
    char      *buffer;
    char       frame_name[VAR_NAME_LENGTH];
    char      *start_ptr;
    int        finished, blocked, turn_done;
    int        is_inverse; 
    pthread_t  tid;
    pthread_mutex_t *done_mtx;
    pthread_cond_t  *done_cond;
    ThreadArgs *sender_to_notify;
};

/* ── thread-local state (extern, definite in Janus.c) ── */
extern __thread ThreadArgs *current_thread_args;
extern __thread char       *strtok_saveptr;
extern __thread uint        thread_val_IF;

extern pthread_mutex_t var_indexer_mtx;
extern CharIdMap       FrameIndexer;

/* Ridefinizione thread-safe di strtok */
#undef  strtok
#define strtok(str, delim) strtok_r((str), (delim), &strtok_saveptr)

#endif /* VM_TYPES_H */