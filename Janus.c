#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "char_id_map.h"
CharIdMap FrameIndexer;
#include "stack.h"

#define uint unsigned int
#define perror(msg) do { printf(msg); exit(EXIT_FAILURE); } while(0)

typedef enum {
    TYPE_INT   = 0,
    TYPE_STACK = 1,
    TYPE_PARAM = 2
} ValueType;

#define VAR_NAME_LENGTH  100
#define VAR_STACK_MAX_SIZE 128

typedef struct Var {
    ValueType T;
    int      *value;
    size_t    stack_len;
    int       is_local;
    char      name[VAR_NAME_LENGTH];
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
    uint      val_IF;
    int       param_indices[64];
    int       param_count;
    int loop_restart_i[MAX_NESTED];
    int loop_bottom_i[MAX_NESTED];
    int loop_counter;
    int       recursion_depth;   /* ← aggiunto */
} Frame;

#define MAX_FRAMES 100

typedef struct {
    Frame frames[MAX_FRAMES];
    int   frame_top;
} VM;

/* ======================================================================
 *  HELPER FUNCTIONS
 * ====================================================================== */

static uint get_findex(const char *frame_name)
{
    return char_id_map_get(&FrameIndexer, frame_name);
}

/* Risolve un token: se è il nome di una variabile INT ne ritorna il valore,
   altrimenti lo converte come letterale intero. */
static int resolve_value(VM *vm, uint Findex, const char *token)
{
    if (char_id_map_exists(&vm->frames[Findex].VarIndexer, token)) {
        uint idx = char_id_map_get(&vm->frames[Findex].VarIndexer, token);
        return *(vm->frames[Findex].vars[idx]->value);
    }
    return (int) strtol(token, NULL, 10);
}

/* Ritorna il puntatore alla Var, con controlli di esistenza e non-NULL. */
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

/* Spostamento del ptr alla riga voluta (helper già presente, ora in cima). */
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
 *  HELPER: cancella variabile dal frame  (deve stare prima di op_delocal)
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
 *  STACK OPS
 * ====================================================================== */

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
    if (stack_var->T != TYPE_STACK) perror("[VM] PUSH: destinazione non è stack!\n");

    stack_var->value = realloc(stack_var->value, (stack_var->stack_len + 1) * sizeof(int));
    if (!stack_var->value) perror("realloc failed\n");
    stack_var->value[stack_var->stack_len++] = val;
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
    if (stack_var->T != TYPE_STACK) perror("[VM] POP: sorgente non è stack!\n");
    if (stack_var->stack_len == 0)  perror("[VM] POP: stack vuoto!\n");

    int popped = stack_var->value[--stack_var->stack_len];
    if (stack_var->stack_len > 0)
        stack_var->value = realloc(stack_var->value, stack_var->stack_len * sizeof(int));

    Var *dest = get_var(vm, Findex, C_dest, "POP");
    *(dest->value) += popped;
}

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
    } else {
        perror("[VM] SHOW su variabile PARAM non linkata!\n");
    }
}

/* ======================================================================
 *  EVAL  (imposta val_IF nel frame corrente)
 * ====================================================================== */

void op_eval(VM *vm, const char *frame_name)
{
    char *ID      = strtok(NULL, " \t");
    char *C_value = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    Var  *v       = get_var(vm, Findex, ID, "EVAL");
    int   rhs     = resolve_value(vm, Findex, C_value);
    vm->frames[Findex].val_IF = (*(v->value) == rhs);
}

/* ======================================================================
 *  SALTI  (ritornano il nuovo ptr, o NULL se non si salta)
 * ====================================================================== */

/* JMP incondizionato: ritorna sempre il nuovo ptr. */
char *op_jmp(VM *vm, const char *frame_name, char *original_buffer)
{   
    char *c_label = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    uint  Lindex  = char_id_map_get(&vm->frames[Findex].LabelIndexer, c_label);
    char *new_ptr = go_to_line(original_buffer, vm->frames[Findex].label[Lindex] + 1);
    if (!new_ptr) perror("[VM] JMP: label non trovata!\n");
    return new_ptr;
}

/* JMPF: ritorna il nuovo ptr se si salta, NULL se la condizione era vera. */
char *op_jmpf(VM *vm, const char *frame_name, char *original_buffer)
{
    uint Findex = get_findex(frame_name);
    if (vm->frames[Findex].val_IF)
        return NULL; /* condizione vera: non saltiamo */

    char *c_label = strtok(NULL, " \t");
    uint  Lindex  = char_id_map_get(&vm->frames[Findex].LabelIndexer, c_label);
    return go_to_line(original_buffer, vm->frames[Findex].label[Lindex] + 1);
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
    uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, Vname);

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
    } else {
        perror("[VM] LOCAL: tipo non esistente\n");
    }

    strncpy(vm->frames[Findex].vars[Vindex]->name, Vname, VAR_NAME_LENGTH - 1);
    vm->frames[Findex].vars[Vindex]->name[VAR_NAME_LENGTH - 1] = '\0';
    vm->frames[Findex].vars[Vindex]->is_local = 1;

    if (Vindex >= (uint)vm->frames[Findex].var_count)
        vm->frames[Findex].var_count = Vindex + 1;

    Var *dst = vm->frames[Findex].vars[Vindex];

    /* Assegnazione valore iniziale */
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

    if (strcmp(Vtype, (V->T == 0 ? "int" : "stack")) != 0)
        perror("[VM] DELOCAL: tipo o variabile errati\n");

    if (strcmp(Vtype, "stack") == 0) {
        if (V->stack_len == 0 && strcmp(c_Vvalue, "nil") == 0)
            delete_var(vm->frames[Findex].vars, &vm->frames[Findex].var_count,
                       char_id_map_get(&vm->frames[Findex].VarIndexer, Vname));
        else if (V->stack_len != 0)
            perror("[VM] DELOCAL: stack deve essere nil!\n");
        else
            perror("[VM] DELOCAL: valore finale di stack diverso da nil!\n");
    } else {
        if (Vvalue == *(V->value))
            delete_var(vm->frames[Findex].vars, &vm->frames[Findex].var_count,
                       char_id_map_get(&vm->frames[Findex].VarIndexer, Vname));
        else {
            printf("[VM] DELOCAL: valore finale diverso dall'atteso! (%s, %d, %d)\n",
                   Vname, Vvalue, *(V->value));
            exit(1);
        }
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
        //printf("[VM] ASSERT fallito! (v1=%lu, v2=%lu)\n", val1, val2);
        //exit(EXIT_FAILURE);
    }
}



/* ======================================================================
 *  PATCH: sostituire integralmente invert_op_to_line
 *
 *  MODIFICA 1 — aggiungere questa forward declaration SUBITO PRIMA
 *               della funzione invert_op_to_line nel file vm.c:
 * ====================================================================== */
void vm_run_BT(VM *vm, char *buffer, char *frame_name_init); /* forward decl */
 
    /* skip line number */
static char *skip_lineno(char *line)
{
    while (*line >= '0' && *line <= '9') line++;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}


/* ======================================================================
 *  Struttura che descrive un loop trovato nel corpo di una procedura
 * ====================================================================== */
typedef struct {
    uint eval_entry_line;   /* riga EVAL prima di JMPF FROM_ERR  */
    char eval_entry_id[64]; /* variabile dell'EVAL entry          */
    char eval_entry_val[64];
    uint jmpf_err_line;     /* riga JMPF FROM_ERR                 */
    uint from_start_line;   /* riga LABEL FROM_START              */
    uint from_end_line;     /* riga LABEL FROM_END                */
    uint from_err_line;     /* riga LABEL FROM_ERR                */
    uint eval_exit_line;    /* riga EVAL prima di JMPF FROM_START */
    char eval_exit_id[64];
    char eval_exit_val[64];
    uint jmpf_start_line;   /* riga JMPF FROM_START               */
} LoopDescriptor;

/* ======================================================================
 *  Scansiona il corpo della procedura e raccoglie i descrittori
 *  di tutti i loop presenti (non annidati per ora).
 * ====================================================================== */
static int collect_loops(VM *vm, const char *frame_name,
                          char *original_buffer,
                          LoopDescriptor *loops_out, int max_loops)
{
    uint findex     = char_id_map_get(&FrameIndexer, frame_name);
    uint start_line = vm->frames[findex].addr + 1;
    char *ptr       = go_to_line(original_buffer, start_line);
    int   nloops    = 0;

    /* stato della scansione */
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
                /* inizio loop */
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

/* ======================================================================
 *  Controlla se la riga `line` appartiene a un loop e quale loop è,
 *  e in quale zona (entry-guard, body, exit-check, labels).
 * ====================================================================== */
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
 *  invert_op_to_line — riscrittura completa
 *
 *  Strategia:
 *  - Scorre il corpo IN AVANTI
 *  - Op normali → esegue op inversa
 *  - CALL → raccoglie loop del callee, chiama ricorsivamente invert
 *  - UNCALL → esegue forward (vm_run_BT sul solo callee)
 *  - Loop (from/until) → inverte entry/exit condition e corpo
 * ====================================================================== */
/* ======================================================================
 *  Struttura che descrive un if/fi trovato nel corpo
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

/* ======================================================================
 *  Scansione statica: raccoglie tutti gli if/fi del corpo
 * ====================================================================== */
static int collect_ifs(VM *vm, const char *frame_name,
                       char *original_buffer,
                       IfDescriptor *ifs_out, int max_ifs)
{
    uint findex     = char_id_map_get(&FrameIndexer, frame_name);
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

/* ======================================================================
 *  Zone per if/fi
 * ====================================================================== */
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

/* ======================================================================
 *  Helper: esegui EVAL dato un IfDescriptor/LoopDescriptor genericamente
 * ====================================================================== */
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
    vm->frames[findex].val_IF = (*(vm->frames[findex].vars[vi2]->value) == rhs);
}
/* ======================================================================
 *  Esegue un singolo ramo if (then o else) al contrario con op inverse.
 *  from_line = prima riga del ramo, to_line = riga della label che chiude
 * ====================================================================== */
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

    //fprintf(stderr, "[DBG exec_branch_inverse] frame=%s from=%u to=%u count=%d\n", frame_name, from_line, to_line, count);

    uint callee_findex = char_id_map_get(&FrameIndexer, frame_name);

    /* Salva le vars del callee */
    Var *saved_vars[MAX_VARS];
    memcpy(saved_vars, vm->frames[callee_findex].vars,
           sizeof(Var*) * MAX_VARS);

    /* Linka i PARAM del callee alle var del caller per nome */
    for (int p = 0; p < vm->frames[callee_findex].param_count; p++) {
        int   pidx  = vm->frames[callee_findex].param_indices[p];
        char *pname = vm->frames[callee_findex].vars[pidx]->name;
        if (char_id_map_exists(&vm->frames[caller_findex].VarIndexer, pname)) {
            int src = char_id_map_get(&vm->frames[caller_findex].VarIndexer, pname);
            vm->frames[callee_findex].vars[pidx] = vm->frames[caller_findex].vars[src];
        }
    }

    /* DECL vars (es. 'zero') che sono NULL: alloca temporaneamente */
    Var *temp_alloc[MAX_VARS];
    memset(temp_alloc, 0, sizeof(temp_alloc));
    for (int v = 0; v < vm->frames[callee_findex].var_count; v++) {
        if (vm->frames[callee_findex].vars[v] == NULL) {
            vm->frames[callee_findex].vars[v] = calloc(1, sizeof(Var));
            vm->frames[callee_findex].vars[v]->T     = TYPE_INT;
            vm->frames[callee_findex].vars[v]->value = calloc(1, sizeof(int));
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

        //fprintf(stderr, "[DBG exec_branch] i=%d fw=%s\n", i, fw);

        if      (strcmp(fw, "PUSHEQ") == 0) op_pusheq_inv(vm, frame_name);
        else if (strcmp(fw, "MINEQ")  == 0) op_mineq_inv (vm, frame_name);
        else if (strcmp(fw, "PRODEQ") == 0) op_prodeq_inv(vm, frame_name);
        else if (strcmp(fw, "DIVEQ")  == 0) op_diveq_inv (vm, frame_name);
        else if (strcmp(fw, "MODEQ")  == 0) op_modeq_inv (vm, frame_name);
        else if (strcmp(fw, "EXPEQ")  == 0) op_expeq_inv (vm, frame_name);
        else if (strcmp(fw, "SWAP")   == 0) op_swap_inv  (vm, frame_name);
        else if (strcmp(fw, "PUSH")   == 0) op_pop       (vm, frame_name);
        else if (strcmp(fw, "POP")    == 0) op_push      (vm, frame_name);
        else if (strcmp(fw, "LOCAL")  == 0) op_delocal   (vm, frame_name);
        else if (strcmp(fw, "DELOCAL")== 0) op_local     (vm, frame_name);
        else if (strcmp(fw, "SHOW")   == 0) op_show      (vm, frame_name);
        /* LABEL, EVAL, ASSERT, JMP, JMPF → skip */
    }

    /* Libera le DECL temporanee */
    for (int v = 0; v < vm->frames[callee_findex].var_count; v++) {
        if (temp_alloc[v]) {
            free(temp_alloc[v]->value);
            free(temp_alloc[v]);
            vm->frames[callee_findex].vars[v] = NULL;
        }
    }

    /* Ripristina le vars del callee */
    memcpy(vm->frames[callee_findex].vars, saved_vars,
           sizeof(Var*) * MAX_VARS);

    for (int i = 0; i < count; i++) free(lines[i]);
}


/* ======================================================================
 *  invert_op_to_line — versione finale con if/fi + loop + call/uncall
 * ====================================================================== */

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

    uint findex_reset = char_id_map_get(&FrameIndexer, frame_name);
    stack_init(&vm->frames[findex_reset].LocalVariables);

#define MAX_LOOPS    32
#define MAX_IFS      32
#define MAX_INV_CALL 64
#define MAX_LINES    1024

    LoopDescriptor loops[MAX_LOOPS];
    int nloops = collect_loops(vm, frame_name, original_buffer, loops, MAX_LOOPS);

    IfDescriptor ifs[MAX_IFS];
    int nifs = collect_ifs(vm, frame_name, original_buffer, ifs, MAX_IFS);

    /* debug: stampa descrittori if */
    for (int ii = 0; ii < nifs; ii++) {
        //fprintf(stderr, "[DBG IF %d] eval_entry=%u jmpf_else=%u else_label=%u "
        //        "jmp_fi=%u fi_label=%u eval_exit=%u assert=%u\n",
        //        ii,
        //        ifs[ii].eval_entry_line, ifs[ii].jmpf_else_line,
        //        ifs[ii].else_label_line, ifs[ii].jmp_fi_line,
        //        ifs[ii].fi_label_line,   ifs[ii].eval_exit_line,
        //        ifs[ii].assert_line);
    }

    char cur_frame[VAR_NAME_LENGTH];
    strncpy(cur_frame, frame_name, VAR_NAME_LENGTH - 1);
    cur_frame[VAR_NAME_LENGTH - 1] = '\0';

    uint findex     = char_id_map_get(&FrameIndexer, cur_frame);
    uint start_line = vm->frames[findex].addr + 1;

    /* raccoglie righe */
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

    /* processa al contrario */
    int i = nlines - 1;
    while (i >= 0) {
        char op_buf[512];
        strncpy(op_buf, line_ptrs[i], sizeof(op_buf) - 1);
        op_buf[sizeof(op_buf) - 1] = '\0';

        uint  cur_line  = line_nos[i];
        char *clean     = skip_lineno(op_buf);
        char *firstWord = strtok(clean, " \t");

        if (!firstWord) { i--; continue; }

        /* ---- zone loop ---- */
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

            if (vm->frames[findex].val_IF) {
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

            if (vm->frames[findex].val_IF) {
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

        /* ---- zone if ---- */
        int if_idx = -1;
        IfZone izone = line_if_zone(cur_line, ifs, nifs, &if_idx);

        if (izone == IF_ZONE_EVAL_ENTRY || izone == IF_ZONE_EVAL_EXIT ||
            izone == IF_ZONE_ELSE_LABEL || izone == IF_ZONE_FI_LABEL  ||
            izone == IF_ZONE_ASSERT     || izone == IF_ZONE_JMP_FI) {
            i--; continue;
        }

        if (izone == IF_ZONE_JMPF_ELSE) {
            int depth = vm->frames[findex].recursion_depth;
            //fprintf(stderr, "[DBG IF_JMPF_ELSE] depth=%d k_start=%d\n",
            //        depth,
            //        *(vm->frames[findex].vars[
            //            char_id_map_get(&vm->frames[findex].VarIndexer,
            //                            ifs[if_idx].eval_entry_id)
            //        ]->value));

            /* Disfa il ramo else depth volte (una per ogni chiamata ricorsiva) */
            for (int d = 0; d < depth; d++) {
                exec_branch_inverse(vm, original_buffer, cur_frame,
                                    ifs[if_idx].else_label_line + 1,
                                    ifs[if_idx].fi_label_line,
                                    findex);
            }

            /* Disfa il ramo then una volta sola (il caso base) */
            exec_branch_inverse(vm, original_buffer, cur_frame,
                                ifs[if_idx].jmpf_else_line + 1,
                                ifs[if_idx].jmp_fi_line,
                                findex);

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

        /* ---- CALL → UNCALL ricorsivo ---- */
        if (strcmp(firstWord, "CALL") == 0) {
            char *proc_name = strtok(NULL, " \t");
            uint  callee_fi = char_id_map_get(&FrameIndexer, proc_name);
            uint  caller_fi = char_id_map_get(&FrameIndexer, frame_name);
            int   param_count   = vm->frames[callee_fi].param_count;
            int  *param_indices = vm->frames[callee_fi].param_indices;

            //fprintf(stderr, "[DBG CALL→UNCALL] proc=%s param_count=%d\n", proc_name, param_count);

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

        /* ---- UNCALL → CALL forward ---- */
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

        /* ---- op inverse ---- */
        if      (strcmp(firstWord, "PUSHEQ") == 0) op_pusheq_inv(vm, cur_frame);
        else if (strcmp(firstWord, "MINEQ")  == 0) op_mineq_inv (vm, cur_frame);
        else if (strcmp(firstWord, "PRODEQ") == 0) op_prodeq_inv(vm, cur_frame);
        else if (strcmp(firstWord, "DIVEQ")  == 0) op_diveq_inv (vm, cur_frame);
        else if (strcmp(firstWord, "MODEQ")  == 0) op_modeq_inv (vm, cur_frame);
        else if (strcmp(firstWord, "EXPEQ")  == 0) op_expeq_inv (vm, cur_frame);
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



        /* ---------- fine procedura ---------- */
        if (strcmp(firstWord, "END_PROC") == 0) {
    uint Findex = get_findex(frame_name);
    if (stack_size(&vm->frames[Findex].LocalVariables) > -1)
        perror("[VM] END_PROC: variabili LOCAL non chiuse con DELOCAL!\n");

    *newline = '\n';

    if (call_top >= 0) {
        int cfi = call_stack[call_top].callee_findex;

        // ← RIMUOVI il decremento recursion_depth qui

        for (int k = 0; k < call_stack[call_top].saved_param_count; k++)
            vm->frames[cfi].vars[
                vm->frames[cfi].param_indices[k]
            ] = call_stack[call_top].saved_params[k];

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
            uint  Findex     = char_id_map_get(&FrameIndexer, proc_name);
            uint  cur_Findex = get_findex(frame_name);

            

            if (call_top + 1 >= MAX_FRAMES)
                perror("[VM] CALL: call stack overflow!\n");

            call_top++;
            *newline = '\n';
            call_stack[call_top].return_ptr = newline + 1;
            strncpy(call_stack[call_top].caller_frame, frame_name, VAR_NAME_LENGTH - 1);

            int  param_count   = vm->frames[Findex].param_count;
            int *param_indices = vm->frames[Findex].param_indices;

            call_stack[call_top].callee_findex     = Findex;
            call_stack[call_top].saved_param_count = param_count;

            for (int k = 0; k < param_count; k++) {
                call_stack[call_top].saved_params[k] =
                    vm->frames[Findex].vars[param_indices[k]];
                
            }

            char *param = NULL;
            int   i     = 0;
            while ((param = strtok(NULL, " \t")) != NULL) {
                if (i >= param_count) {
                    fprintf(stderr, "ERROR: troppi parametri per '%s'\n", proc_name);
                    exit(EXIT_FAILURE);
                }
                int j = param_indices[i];

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
                i++;
            }
            if (i != param_count) {
                fprintf(stderr, "ERROR: attesi %d params, ricevuti %d per '%s'\n",
                        param_count, i, proc_name);
                exit(EXIT_FAILURE);
            }


            // Incrementa solo se è una chiamata ricorsiva (stesso frame)
            if (strcmp(proc_name, frame_name) == 0) {
                vm->frames[Findex].recursion_depth++;
                //fprintf(stderr, "[DBG CALL ricorsivo] %s depth=%d\n",  proc_name, vm->frames[Findex].recursion_depth);
            }
            strncpy(frame_name, proc_name, VAR_NAME_LENGTH - 1);
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
    int   i     = 0;
    while ((param = strtok(NULL, " \t")) != NULL) {
        if (i >= param_count) {
            fprintf(stderr, "ERROR: troppi parametri per UNCALL '%s'\n", proc_name);
            exit(EXIT_FAILURE);
        }
        int src_idx = char_id_map_get(&vm->frames[cur_Findex].VarIndexer, param);
        vm->frames[Findex].vars[param_indices[i]] =
            vm->frames[cur_Findex].vars[src_idx];
        i++;
    }
    if (i != param_count) {
        fprintf(stderr, "ERROR: parametri UNCALL mismatch '%s'\n", proc_name);
        exit(EXIT_FAILURE);
    }

    /* RIMOSSO: non ripristinare k — deve restare 0 com'è dopo il forward */

    invert_op_to_line(vm, proc_name, original_buffer,
                      vm->frames[Findex].end_addr - 1,
                      vm->frames[Findex].addr + 1);

    for (int k = 0; k < param_count; k++)
        vm->frames[Findex].vars[param_indices[k]] = saved[k];

    *newline = '\n';
    ptr = newline + 1;
    continue;
}else if (strcmp(firstWord, "LOCAL")   == 0) { op_local  (vm, frame_name);
        } else if (strcmp(firstWord, "DELOCAL") == 0) { op_delocal(vm, frame_name);
        } else if (strcmp(firstWord, "SHOW")    == 0) { op_show   (vm, frame_name);
        } else if (strcmp(firstWord, "PUSHEQ")  == 0) { op_pusheq (vm, frame_name);
        } else if (strcmp(firstWord, "MINEQ")   == 0) { op_mineq  (vm, frame_name);
        } else if (strcmp(firstWord, "PRODEQ")  == 0) { op_prodeq (vm, frame_name);
        } else if (strcmp(firstWord, "DIVEQ")   == 0) { op_diveq  (vm, frame_name);
        } else if (strcmp(firstWord, "MODEQ")   == 0) { op_modeq  (vm, frame_name);
        } else if (strcmp(firstWord, "EXPEQ")   == 0) { op_expeq  (vm, frame_name);
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
        }

        *newline = '\n';
        ptr = newline + 1;
    }

    free(original_buffer);
}
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
                           strcmp(firstWord, "PRODEQ")  == 0 ||
                           strcmp(firstWord, "DIVEQ")   == 0 ||
                           strcmp(firstWord, "MODEQ")   == 0 ||
                           strcmp(firstWord, "EXPEQ")   == 0 ||
                           strcmp(firstWord, "SWAP")    == 0 ||
                           strcmp(firstWord, "PUSH")    == 0 ||
                           strcmp(firstWord, "POP")     == 0 ||
                           strcmp(firstWord, "EVAL")    == 0 ||
                           strcmp(firstWord, "JMPF")    == 0 ||
                           strcmp(firstWord, "JMP")     == 0 ||
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
 *  MAIN
 * ====================================================================== */

#define START_BUFFER 256
#define AST_BUFFER  (1024 * 10)


#include "check_if_reversibility.h"
// Funzione esportabile — non usa main(), riceve il bytecode come stringa
void vm_run_from_string(const char *bytecode)
{
    char ast[AST_BUFFER];
    ast[0] = '\0';
    strncat(ast, bytecode, sizeof(ast) - 1);

    if (vm_check_if_reversibility(ast) > 0) {
        // Handle reversibility issues
        fprintf(stderr, "Warning: Bytecode may not be fully reversible. Check logs for details.\n");
        //exit(EXIT_FAILURE);
    }

    VM vm;
    memset(&vm, 0, sizeof(VM));
    vm_exec(&vm, ast);
    vm_dump(&vm);
}