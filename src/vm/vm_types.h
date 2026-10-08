#ifndef VM_TYPES_H
#define VM_TYPES_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "char_id_map.h"
#include "stack.h"

#define uint     unsigned int

/* Copia una riga di bytecode in un buffer senza il riempimento di strncpy:
   strncpy azzera tutto il resto della destinazione, e con buffer da 16 KB questo
   significava scrivere 16 KB per ogni istruzione eseguita, a ogni thread. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* Copia di una riga (o di un'espressione) in un buffer della sua lunghezza esatta:
   niente limite fisso sulla lunghezza, e niente da liberare. */
#define VM_LINE_COPY(dst, src) \
    size_t dst##_len = strlen(src); char dst[dst##_len + 1]; memcpy(dst, (src), dst##_len + 1)

/* ----------------------------------------------------------------------
 *  Blocchi che crescono senza essere mai liberati sotto i piedi di un lettore
 *
 *  Alcuni vettori (vm->frames, Frame.vars, i nomi condivisi della VM) sono letti
 *  da altri thread senza lock mentre chi li possiede puo' doverli ingrandire.
 *  realloc libererebbe il blocco vecchio che un lettore sta ancora usando.
 *  vm_grow_keep alloca un blocco nuovo, copia, e aggancia il vecchio in
 *  un'intestazione nascosta prima del dato: resta vivo finche' vm_grow_free
 *  non libera tutta la catena, alla distruzione della struttura. Crescendo per
 *  raddoppio la catena pesa meno del blocco corrente.
 * ---------------------------------------------------------------------- */
static inline void *vm_grow_keep(void *old, size_t old_bytes, size_t new_bytes)
{
    void **blk = (void **)malloc(sizeof(void *) * 2 + new_bytes);
    if (!blk) { fprintf(stderr, "[VM] memoria esaurita (%zu byte)\n", new_bytes); exit(1); }
    blk[0] = old ? (void *)((void **)old - 2) : NULL;    /* blocco precedente */
    blk[1] = NULL;                                         /* allinea a 16 byte */
    void *p = blk + 2;
    if (old && old_bytes) memcpy(p, old, old_bytes);
    if (new_bytes > old_bytes) memset((char *)p + old_bytes, 0, new_bytes - old_bytes);
    return p;
}

static inline void vm_grow_free(void *p)
{
    void **b = p ? (void **)p - 2 : NULL;
    while (b) { void **prev = (void **)b[0]; free(b); b = prev; }
}

/* Stringa che si riscrive spesso (nome della procedura corrente di un
   meccanismo, frame mostrato dal debugger): il buffer cresce solo quando serve
   e i buffer vecchi restano vivi, perche' un altro thread puo' leggerla mentre
   viene riscritta. Come prima della modifica, una lettura concorrente puo'
   vedere un testo a meta', mai memoria liberata. */
static inline void vm_name_set(char **s, size_t *cap, const char *src)
{
    size_t l = strlen(src) + 1;
    if (l > *cap) {
        size_t nc = *cap ? *cap : 32;
        while (nc < l) nc *= 2;
        char *n = (char *)vm_grow_keep(*s, 0, nc);
        memcpy(n, src, l);
        __atomic_store_n(s, n, __ATOMIC_RELEASE);
        *cap = nc;
        return;
    }
    memcpy(*s, src, l);
}

static inline const char *vm_name_get(const char *s)
{
    return s ? s : "";
}

/* Confronta la parte di `name` prima della '@' (il nome base di un frame
   clonato, "proc@3" -> "proc") con `base`, senza copiarla. */
static inline int vm_base_eq(const char *name, const char *base)
{
    size_t n = strcspn(name, "@");
    return strncmp(name, base, n) == 0 && base[n] == '\0';
}

/* Copia del nome base di un frame ("proc@t123" -> "proc") in un buffer della
   sua lunghezza. */
#define VM_FRAME_BASE(dst, src) \
    size_t dst##_len = strcspn((src), "@"); char dst[dst##_len + 1]; \
    memcpy(dst, (src), dst##_len); dst[dst##_len] = '\0'

/* Buffer per una chiave di frame costruita da un nome di procedura: il nome
   e un suffisso numerico ("@t<tid>", "@w<tid>_<depth>", "@<depth>"). */
#define VM_FRAME_KEY_BUF(dst, proc) char dst[strlen(proc) + 48]

/* ----------------------------------------------------------------------
 *  strtok e i thread
 *
 *  strtok di glibc tiene il punto a cui e' arrivato in una variabile statica
 *  condivisa da tutto il processo: due thread che tokenizzano insieme si
 *  sovrascrivono a vicenda lo stato. Non e' il buffer del programma a essere
 *  in gara, e' il segnaposto dentro strtok, quindi dare a ogni thread una
 *  copia del bytecode non protegge da questo.
 *
 *  strtok_r prende il segnaposto dal chiamante. Dichiarandolo __thread e
 *  ridefinendo strtok, tutti i punti d'uso passano alla versione rientrante
 *  senza toccarne nessuno.
 * ---------------------------------------------------------------------- */
static __thread char *vm_tok_save;
#define strtok(_s, _d) strtok_r((_s), (_d), &vm_tok_save)

/* Mnemo --opt-uncall-user-calls: vedere MnemoHistFloorSnapEntry più sotto.
 * Capacità iniziale di vm->mn_hist_floor_snaps (heap, cresce raddoppiando
 * via vm_ensure_hist_floor_snap_cap). Nessun hard cap. */
#define MNEMO_HIST_SNAP_INIT_CAP 384

typedef enum {
    TYPE_INT     = 0,
    TYPE_STACK   = 1,
    TYPE_CHANNEL = 2,
    TYPE_PARAM   = 3,
    TYPE_ARRAY   = 4     /* int[n]: value = n celle, stack_len = n, lunghezza fissa */
} ValueType;

typedef struct ThreadArgs ThreadArgs;

typedef struct Waiter {
    pthread_cond_t  cond;
    int             ready;
    struct Waiter  *next;
    ThreadArgs     *thread_args;
} Waiter;

/* Titolarita' di un canale. La logica sta tutta in vm_session.h: qui c'e'
   solo la forma del dato, perche' vive dentro Channel. */
typedef struct {
    pthread_t t;
    unsigned  epoch;
} SessionOwner;

typedef struct {
    SessionOwner owner[2];      /* titolari correnti: due, sessione binaria */
    int          owner_n;
} SessionState;

typedef struct {
    pthread_mutex_t mtx;
    Waiter *send_q_head, *send_q_tail;
    Waiter *recv_q_head, *recv_q_tail;
    ThreadArgs *sender_args;
    int64_t *buf;
    size_t buf_len;
    int refcount;
    SessionState session;       /* vm_session.h — azzerato dalla calloc */
} Channel;

/* Nessun nome ha una lunghezza massima: nomi di variabili, di frame e chiavi
 * di clonazione sono stringhe sull'heap della loro lunghezza, o buffer locali
 * dimensionati sul nome che contengono. */

/* Capacità iniziale del buffer di uno stack: cresce raddoppiando. */
#define VAR_STACK_INIT_CAP 8

typedef struct {
    size_t hist_len_floor;
    char  *opt_call_callee;     /* heap; riscritto quando la voce si riusa */
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
    size_t    stack_cap;    /* TYPE_STACK: celle allocate in value (>= stack_len) */
    int       is_local;
    char     *name;         /* heap, della lunghezza del nome */
    Channel  *channel;
    /* Lock re-entrante per mutazioni int concorrenti (solo con current_thread_args). */
    int        ref_lock_depth;
    pthread_t  ref_lock_owner;
} Var;

/* Capacità iniziali degli array per-Frame (heap, crescono raddoppiando via
 * frame_ensure_*). Nessuna è un massimo. Il vettore delle variabili e lo stack
 * dei local sono inoltre dimensionati, alla prima passata, sul numero di nomi
 * che il testo della procedura può introdurre (names_hint/locals_hint): così
 * non crescono durante l'esecuzione, quando altri thread possono leggerli. */
#define FRAME_VARS_INIT_CAP     8
#define FRAME_LABEL_INIT_CAP    8
#define FRAME_PARAMS_INIT_CAP   8
#define FRAME_TRACE_INIT_CAP    16

typedef struct {
    CharIdMap VarIndexer;
    Stack     LocalVariables;
    /* vars: heap (blocco vm_grow_keep), cresce via frame_ensure_vars. La prima
     * passata lo dimensiona su names_hint, il numero di nomi che il testo della
     * procedura può introdurre: la crescita a run time resta solo una valvola,
     * e quando scatta il blocco vecchio non viene liberato (altri thread possono
     * star leggendo vars[] del frame in cui girano i rami di un par). */
    Var     **vars;
    int       vars_cap;
    int       var_count;
    CharIdMap LabelIndexer;
    /* label/param_indices/trace_window_stack: heap, crescono raddoppiando via
     * frame_ensure_*. label e param_indices si scrivono solo nella prima
     * passata (un thread solo), il clone li copia. */
    uint     *label;
    int       label_cap;
    char     *name;         /* heap */
    int       names_hint;   /* nomi che il testo della procedura può introdurre */
    int       locals_hint;  /* LOCAL nel testo della procedura */
    uint      addr, end_addr;
    int      *param_indices;
    int       param_indices_cap;
    int       param_count;
    int       recursion_depth;
    /* active: # di attivazioni correnti di questa proc-base sul call stack
     * forward (CALL incrementa, END_PROC decrementa). >0 a una nuova CALL =
     * re-entrancy: self-ricorsione (già gestita da is_rec) o ricorsione MUTUA
     * (is_even→is_odd→is_even). Per la mutua serve clonare il frame come per la
     * self-rec, altrimenti il delocal della call annidata libera i LOCAL int
     * condivisi del frame base → `push(__mn_eN)` su Var* NULL. */
    int       active;
    /* Fix P3 trace: per-clone-frame LIFO stack di trace_window_start.
     * Forward CALL push branch_trace_top corrente. Inverse INVOP_CALL/
     * UNCALL pop e setta come trace_window_start corrente (consumato
     * da JMPF_ELSE handler via trace_window_cursor). Stack necessario
     * perché clones reused (es. fib(1) e fib(0) entrambi a fib@2). */
    int      *trace_window_stack;
    int       trace_window_cap;
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

/* Debugger: breakpoint, storia dei passi e output crescono su richiesta. La
 * storia non e' piu' un anello: l'indice di un record (history_top) e' quello
 * che il rebuild per lo step-back usa come traguardo, e un anello che scarta i
 * record vecchi lo falsava oltre la sua capacita'. */
#define DBG_BP_INIT_CAP       16
#define DBG_HISTORY_INIT_CAP  1024
#define DBG_OUT_INIT_CAP      4096

typedef struct {
    int   line;
    char *frame;    /* heap */
    char *instr;    /* heap */
} ExecRecord;

typedef struct {
    VMExecMode  mode;
    int        *breakpoints;
    int         bp_cap;
    int         bp_count;
    int         current_line;
    char       *current_frame;      /* vm_name_set: puo' essere letto da un altro thread */
    size_t      current_frame_cap;
    ExecRecord *history;
    int         history_cap;
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
    /* Output buffer — usato in DAP_MODE al posto di printf. Cresce su
       richiesta; out_mtx lo protegge perche' piu' rami di un par possono
       stampare insieme mentre il controllore lo svuota. */
    char       *out_buf;
    int         out_cap;
    int         out_len;
    pthread_mutex_t out_mtx;
    int         suppress_output;
    int         rebuild_active;
    int         rebuild_target_top;
    char       *last_error;         /* heap, NULL = nessun errore */
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
    /* Vincolo pop: solo mentre si invierte la proc. UNCALL Mnemo (`inv_name`), non i figli invert_op_to_line.
     * I due nomi si confrontavano con strcmp; ora sono gli indici in FrameIndexer
     * dei due frame, piu' uno (0 = nessuno): stessi nomi, stesso indice, e
     * nessun buffer di lunghezza fissa. */
    int    mn_hist_floor_pop_guard_anchor_fi1;
    int    mn_hist_floor_pop_guard_cur_inv_fi1;
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
    char  *branch_trace_proc;      /* vm_name_set, NULL = "" */
    size_t branch_trace_proc_cap;
    /* Cache delle line-range dei from-loop di `branch_trace_proc`. La forward
     * op_jmpf NON deve pushare su branch_trace gli IF DENTRO un loop body: il
     * loro inverse usa recompute (line_inside_loop_body), non consuma il cursor
     * della window → se fossero pushati la window LIFO si disallinea e gli IF
     * top-level leggono entry sbagliate. Cache lazy ricomputata quando
     * branch_trace_proc cambia (gestisce window annidate). */
    /* Coppie (lo, hi) in un blocco vm_grow_keep: op_jmpf le legge, anche dai
     * rami di un par, mentre un nuovo snap puo' riscriverle. */
    uint  *bt_loop_lohi;
    int    bt_loop_cap;
    int    bt_loop_n;
    char  *bt_loops_cached_proc;
    size_t bt_loops_cached_proc_cap;
    /* Mnemo dynamic pointer pool: heap reversibile indicizzato a runtime.
     * Sostituisce le celle statiche __mn_mem* del pool puntatori — cresce
     * on-demand (zero-filled, doubling), così malloc-in-loop a bound runtime
     * senza free funziona senza --ptr-pool-size. Ops POOLPUSH/POOLPOP/POOLADD/
     * POOLSUB/POOLGET/POOLGETNEG (vm_ops.h), reversibili. Reset a ogni run. */
    int64_t *mn_pool;
    long     mn_pool_len;   /* celle valide (zero-filled fino a qui) */
    long     mn_pool_cap;   /* capacità allocata */
} VM;

struct ThreadArgs {
    VM        *vm;
    char      *buffer;
    char      *frame_name;          /* heap */
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