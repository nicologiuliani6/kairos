#ifndef VM_TYPES_H
#define VM_TYPES_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "char_id_map.h"
#include "stack.h"

#define DBG_OUTPUT_BUF_SIZE (1024 * 1024)  // 1 MB

#define uint     unsigned int

/* Mnemo --opt-uncall-user-calls: vedere MnemoHistFloorSnapEntry sotto VAR_NAME_LENGTH.
 * Capacità iniziale di vm->mn_hist_floor_snaps (heap, cresce raddoppiando
 * via vm_ensure_hist_floor_snap_cap). Nessun hard cap. */
#define MNEMO_HIST_SNAP_INIT_CAP 384

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
    int64_t *buf;
    size_t buf_len;
    int refcount;
} Channel;

#define VAR_NAME_LENGTH      100
#define VAR_STACK_MAX_SIZE   512
#define VAR_CHANNEL_MAX_SIZE 128

typedef struct {
    size_t hist_len_floor;
    char   opt_call_callee[VAR_NAME_LENGTH];
    /* Floor su FrameIndexer.count al momento dello snap. Dopo UNCALL match
     * il `cleanup` ripristina FrameIndexer a questa lunghezza, liberando
     * frame_indices generati durante il pattern (forward+inverse). Necessario
     * per opt-uncall su user fn invertibili contenenti __mn_putd_uint
     * (auto-ricorsivo): la depth cresce per digit e tra cicli consecutivi
     * non veniva mai resettata → MAX_FRAMES overflow. */
    int    frame_indexer_count_at_snap;
} MnemoHistFloorSnapEntry;

typedef struct Var {
    ValueType T;
    int64_t  *value;
    size_t    stack_len;
    int       is_local;
    char      name[VAR_NAME_LENGTH];
    Channel  *channel;
    /* Lock re-entrante per mutazioni int concorrenti (solo con current_thread_args). */
    int        ref_lock_depth;
    pthread_t  ref_lock_owner;
} Var;

/* Cap statici dei Frame. Dopo Frame ** refactor (vm->frames = array di
 * Frame*, ogni Frame heap-alloc separato) il realloc del pointer array
 * NON sposta i Frame individuali → safe bumpare senza rompere ex33
 * parallel2_fib (multithread cross-frame pointer holds).
 * Valori storici lasciati intatti per ora; ulteriore dyn alloc per-Frame
 * field rimane TODO (richiede realloc per-Frame inline). */
#define MAX_VARS         4096
#define MAX_LABEL        16384
#define MAX_NESTED       4096
#define MAX_PROC_PARAMS  1024

typedef struct {
    CharIdMap VarIndexer;
    Stack     LocalVariables;
    Var      *vars[MAX_VARS];
    int       var_count;
    CharIdMap LabelIndexer;
    uint      label[MAX_LABEL];
    char      name[VAR_NAME_LENGTH];
    uint      addr, end_addr;
    int       param_indices[MAX_PROC_PARAMS];
    int       param_count;
    int       loop_restart_i[MAX_NESTED];
    int       loop_bottom_i[MAX_NESTED];
    int       loop_counter;
    int       recursion_depth;
    /* Fix P3 trace: per-clone-frame LIFO stack di trace_window_start.
     * Forward CALL push branch_trace_top corrente. Inverse INVOP_CALL/
     * UNCALL pop e setta come trace_window_start corrente (consumato
     * da JMPF_ELSE handler via trace_window_cursor). Stack necessario
     * perché clones reused (es. fib(1) e fib(0) entrambi a fib@2). */
#define VM_TRACE_WIN_STACK_MAX 4096
    int       trace_window_stack[VM_TRACE_WIN_STACK_MAX];
    int       trace_window_top;
    int       trace_window_start;
    int       trace_window_cursor;
} Frame;

/* Capacità iniziale di vm->frames; cresce dinamicamente (raddoppia)
 * via vm_ensure_frame_cap quando clone_frame_for_* o vm_exec creano
 * un nuovo frame indice oltre vm->frames_cap. Nessun hard cap. */
#define VM_FRAMES_INIT_CAP 256

/* ======================================================================
 *  Debug
 * ====================================================================== */

typedef enum {
    VM_MODE_IDLE,
    VM_MODE_RUN,
    VM_MODE_PAUSE,
    VM_MODE_STEP,
    VM_MODE_STEP_BACK,
    VM_MODE_CONTINUE,
    VM_MODE_CONTINUE_INV,
    VM_MODE_DONE
} VMExecMode;

#define DBG_MAX_BREAKPOINTS 256
#define DBG_MAX_HISTORY     4096
#define DBG_INSTR_LEN       512

typedef struct {
    int  line;
    char frame[VAR_NAME_LENGTH];
    char instr[DBG_INSTR_LEN];
} ExecRecord;

#define DBG_OUTPUT_BUF_SIZE (1024 * 1024)  // 1 MB

typedef struct {
    VMExecMode  mode;
    int         breakpoints[DBG_MAX_BREAKPOINTS];
    int         bp_count;
    int         current_line;
    char        current_frame[VAR_NAME_LENGTH];
    ExecRecord  history[DBG_MAX_HISTORY];
    int         history_top;
    pthread_mutex_t pause_mtx;
    pthread_cond_t  pause_cond;
    void (*on_pause)(int line, const char *frame_name, void *userdata);
    void       *userdata;
    int         initialized;
    int         first_pause_reached;
    int         needs_pc_resync;
    int         shutting_down;
    int         ignore_breakpoint_once_line;
    /* Output buffer — usato in DAP_MODE al posto di printf */
    char        out_buf[DBG_OUTPUT_BUF_SIZE];
    int         out_len;
    int         suppress_output;
    int         rebuild_active;
    int         rebuild_target_top;
    char last_error[1024]; 
    int output_pipe_fd;   /* scrittura: la VM ci scrive sopra */
    int output_pipe_rd;   /* lettura:   Node.js legge da qui  */
} VMDebugState;

typedef struct {
    Frame **frames;      /* heap array di Frame*, ogni Frame heap-allocato singolo */
    uint   frames_cap;   /* capacità allocata corrente */
    int   frame_top;
    VMDebugState *dbg;   /* NULL = normale, non-NULL = debug */
    int   inversion_depth;
    int   suppress_show; /* 1 durante vm_run_BT di replay (inverso di UNCALL): no op_show */
    int   mn_dumped;     /* 1 = opcode DUMP (--check-invertibility) ha già stampato il dump mid-run: salta il dump finale post-uncall (vuoto) */
    int   show_char_pending; /* ultimo SHOW è stato show(x,char): il prossimo show classico prefissa \n */
    Var  *invert_hist_guard_var;   /* NULL = nessun vincolo pop su hist */
    size_t invert_hist_floor_min;
    /* Vincolo pop: solo mentre si invierte la proc. UNCALL Mnemo (`inv_name`), non i figli invert_op_to_line. */
    char   mn_hist_floor_pop_guard_anchor[VAR_NAME_LENGTH];
    char   mn_hist_floor_pop_guard_cur_inv_proc[VAR_NAME_LENGTH];
    MnemoHistFloorSnapEntry *mn_hist_floor_snaps;
    uint   mn_hist_floor_snaps_cap;
    int    mn_hist_floor_snap_sp;
    /* Fix P3 execution trace: attivato SOLO dentro opt-uncall pattern
     * Mnemo (delimitato da CALL __mn_hist_floor_snap … UNCALL match).
     * op_jmpf forward push branch-take su trace LIFO se active>0.
     * vm_invert JMPF_ELSE handler pop una entry e replay quel branch
     * specifico. Cosi non interferisce con path inverse legacy
     * (divmod ecc. che usano replay basato su recursion_depth). */
/* branch_trace heap-allocato, cresce on-demand via vm_ensure_branch_trace_cap. */
#define VM_BRANCH_TRACE_INIT_CAP 1024
    int   *branch_trace;
    uint   branch_trace_cap;
    int    branch_trace_top;
    int    branch_trace_active;
    /* Proc name (base) di cui le chiamate ricorsive partecipano alla
     * trace. Settato da __mn_hist_floor_snap. op_jmpf push solo se
     * current proc base name matches. Procs diverse non interferiscono. */
    char   branch_trace_proc[VAR_NAME_LENGTH];
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

extern __thread ThreadArgs *current_thread_args;
extern __thread char       *strtok_saveptr;
extern __thread uint        thread_val_IF;

extern pthread_mutex_t var_indexer_mtx;
extern CharIdMap       FrameIndexer;

#undef  strtok
#define strtok(str, delim) strtok_r((str), (delim), &strtok_saveptr)

#endif /* VM_TYPES_H */