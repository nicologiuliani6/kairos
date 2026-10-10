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
    /* label/param_indices: heap, crescono raddoppiando via
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
     * condivisi del frame base → `push(x)` su Var* NULL. */
    int       active;
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
    int   show_char_pending; /* ultimo SHOW è stato show(x,char): il prossimo show classico prefissa \n */
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