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
            printf("[VM] DELOCAL: valore finale diverso dall'atteso! (%d, %d)\n",
                   Vvalue, *(V->value));
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
        printf("[VM] ASSERT fallito! (v1=%lu, v2=%lu)\n", val1, val2);
        exit(EXIT_FAILURE);
    }
}


/* Esegue al contrario le istruzioni da riga stop fino a riga start (incluse).
 * start = riga precedente a END_PROC della procedura da invertire.
 * stop  = riga di PROC (prima istruzione del corpo).
 * Le righe vengono raccolte in un array e iterate in ordine inverso. */
void invert_op_to_line(VM *vm, const char *frame_name, char *buffer, uint start, uint stop)
{
    char *original_buffer = strdup(buffer);
    if (!original_buffer) perror("[UNCALL] strdup fallita\n");

    /* --- raccoglie puntatori a tutte le righe nell'intervallo [stop, start] --- */
    #define MAX_INVERT_LINES 1024
    char *lines[MAX_INVERT_LINES];
    int   line_count = 0;

    char *ptr = go_to_line(original_buffer, stop);
    if (!ptr) {
        fprintf(stderr, "[UNCALL] riga %d non trovata nel buffer\n", stop);
        free(original_buffer);
        return;
    }

    while (*ptr != '\0') {
        char *newline = strchr(ptr, '\n');
        if (!newline) break;

        if (line_count >= MAX_INVERT_LINES) {
            fprintf(stderr, "[UNCALL] troppe righe da invertire (max %d)\n", MAX_INVERT_LINES);
            free(original_buffer);
            return;
        }

        lines[line_count++] = ptr;

        /* calcoliamo la riga corrente contando le \n dall'inizio */
        uint cur_line = stop + line_count - 1;
        if (cur_line >= start) break;   /* incluso start, poi ci fermiamo */

        *newline = '\0';                /* termina temporaneamente per sicurezza */
        *newline = '\n';                /* ripristina subito */
        ptr = newline + 1;
    }

    /* --- itera in ordine inverso ed esegue l'op inversa --- */
    for (int i = line_count - 1; i >= 0; i--) {
        char *newline = strchr(lines[i], '\n');
        if (!newline) continue;
        *newline = '\0';

        char line_buf[512];
        strncpy(line_buf, lines[i] + 6, sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';

        char *firstWord = strtok(line_buf, " \t");
        if (!firstWord || strcmp(firstWord, "") == 0) {
            /* riga vuota, salta */
        } else if (strcmp(firstWord, "PUSHEQ")  == 0) { op_pusheq_inv(vm, frame_name);
        } else if (strcmp(firstWord, "MINEQ")   == 0) { op_mineq_inv (vm, frame_name);
        } else if (strcmp(firstWord, "PRODEQ")  == 0) { op_prodeq_inv(vm, frame_name);
        } else if (strcmp(firstWord, "DIVEQ")   == 0) { op_diveq_inv (vm, frame_name);
        } else if (strcmp(firstWord, "MODEQ")   == 0) { op_modeq_inv (vm, frame_name);
        } else if (strcmp(firstWord, "EXPEQ")   == 0) { op_expeq_inv (vm, frame_name);
        } else if (strcmp(firstWord, "SWAP")    == 0) { op_swap_inv  (vm, frame_name);
        } else if (strcmp(firstWord, "PUSH")    == 0) { op_pop       (vm, frame_name);
        } else if (strcmp(firstWord, "POP")     == 0) { op_push      (vm, frame_name);
        } else if (strcmp(firstWord, "LOCAL")   == 0) { op_delocal   (vm, frame_name);
        } else if (strcmp(firstWord, "DELOCAL") == 0) { op_local     (vm, frame_name);
        } else if (strcmp(firstWord, "SHOW")    == 0) { op_show      (vm, frame_name);
        } else if (strcmp(firstWord, "EVAL")    == 0) { op_eval      (vm, frame_name);
        } else if (strcmp(firstWord, "ASSERT")  == 0) { op_assert    (vm, frame_name);
        } else if (strcmp(firstWord, "JMP")     == 0 ||
                   strcmp(firstWord, "JMPF")    == 0 ||
                   strcmp(firstWord, "LABEL")   == 0 ||
                   strcmp(firstWord, "PROC")    == 0 ||
                   strcmp(firstWord, "END_PROC")== 0 ||
                   strcmp(firstWord, "CALL")    == 0 ||
                   strcmp(firstWord, "UNCALL")  == 0) {
            /* salti e struttura: gestione futura, per ora ignorati */
        } else {
            fprintf(stderr, "[UNCALL] istruzione sconosciuta in inversione: %s\n", firstWord);
        }

        *newline = '\n';
    }

    free(original_buffer);
    #undef MAX_INVERT_LINES
}


/* ======================================================================
 *  VM RUN  (dispatch loop snello)
 * ====================================================================== */

void vm_run_BT(VM *vm, char *buffer, char *frame_name_init)
{
    char *original_buffer = strdup(buffer);

    char frame_name[VAR_NAME_LENGTH];
    strncpy(frame_name, frame_name_init, VAR_NAME_LENGTH - 1);
    frame_name[VAR_NAME_LENGTH - 1] = '\0';

    typedef struct {
        char *return_ptr;
        char  caller_frame[VAR_NAME_LENGTH];
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
        strncpy(line_buf, ptr + 6, sizeof(line_buf) - 1);
        line_buf[sizeof(line_buf) - 1] = '\0';

        char *firstWord = strtok(line_buf, " \t");

        /* ---------- fine procedura ---------- */
        if (strcmp(firstWord, "END_PROC") == 0) {
            uint Findex = get_findex(frame_name);
            if (stack_size(&vm->frames[Findex].LocalVariables) > -1)
                perror("[VM] END_PROC: variabili LOCAL non chiuse con DELOCAL!\n");

            *newline = '\n';
            if (call_top >= 0) {
                ptr = call_stack[call_top].return_ptr;
                strncpy(frame_name, call_stack[call_top].caller_frame, VAR_NAME_LENGTH - 1);
                call_top--;
                continue;
            } else {
                break;
            }

        /* ---------- chiamata a procedura ---------- */
        } else if (strcmp(firstWord, "CALL") == 0) {
            char *proc_name  = strtok(NULL, " \t");
            uint  Findex     = char_id_map_get(&FrameIndexer, proc_name);
            uint  cur_Findex = get_findex(frame_name);

            if (call_top + 1 >= MAX_FRAMES) perror("[VM] CALL: call stack overflow!\n");
            call_top++;
            *newline = '\n';
            call_stack[call_top].return_ptr = newline + 1;
            strncpy(call_stack[call_top].caller_frame, frame_name, VAR_NAME_LENGTH - 1);
            strncpy(frame_name, proc_name, VAR_NAME_LENGTH - 1);

            int  param_count   = vm->frames[Findex].param_count;
            int *param_indices = vm->frames[Findex].param_indices;
            char *param = NULL;
            int   i = 0;
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
                int VtoLink_index = char_id_map_get(&vm->frames[cur_Findex].VarIndexer, param);
                if (vm->frames[cur_Findex].vars[VtoLink_index] == NULL) {
                    fprintf(stderr, "[VM] CALL: '%s' è NULL nel frame chiamante!\n", param);
                    exit(EXIT_FAILURE);
                }
                vm->frames[Findex].vars[j] = vm->frames[cur_Findex].vars[VtoLink_index];
                i++;
            }
            if (i != param_count) {
                fprintf(stderr, "ERROR: attesi %d params, ricevuti %d per '%s'\n",
                        param_count, i, proc_name);
                exit(EXIT_FAILURE);
            }

            ptr = go_to_line(original_buffer, vm->frames[Findex].addr + 1);
            if (!ptr) perror("[VM] CALL: indirizzo procedura non trovato!\n");
            continue;

        /* ---------- UNCALL (inversione) ---------- */
        } else if (strcmp(firstWord, "UNCALL") == 0) {
            char *proc_name  = strtok(NULL, " \t");
            uint  Findex     = char_id_map_get(&FrameIndexer, proc_name);
            uint  cur_Findex = get_findex(frame_name);

            /* --- link parametri (speculare a CALL) --- */
            int  param_count   = vm->frames[Findex].param_count;
            int *param_indices = vm->frames[Findex].param_indices;
            char *param = NULL;
            int   i = 0;
            while ((param = strtok(NULL, " \t")) != NULL) {
                if (i >= param_count) {
                    fprintf(stderr, "ERROR: troppi parametri per UNCALL '%s'\n", proc_name);
                    exit(EXIT_FAILURE);
                }
                int j = param_indices[i];
                if (!char_id_map_exists(&vm->frames[cur_Findex].VarIndexer, param)) {
                    fprintf(stderr, "[VM] UNCALL: '%s' non definito nel frame chiamante!\n", param);
                    exit(EXIT_FAILURE);
                }
                int VtoLink_index = char_id_map_get(&vm->frames[cur_Findex].VarIndexer, param);
                if (vm->frames[cur_Findex].vars[VtoLink_index] == NULL) {
                    fprintf(stderr, "[VM] UNCALL: '%s' è NULL nel frame chiamante!\n", param);
                    exit(EXIT_FAILURE);
                }
                vm->frames[Findex].vars[j] = vm->frames[cur_Findex].vars[VtoLink_index];
                i++;
            }
            if (i != param_count) {
                fprintf(stderr, "ERROR: attesi %d params, ricevuti %d per UNCALL '%s'\n",
                        param_count, i, proc_name);
                exit(EXIT_FAILURE);
            }

            /* --- esegui le istruzioni invertite nel frame del callee --- */
            uint start = vm->frames[Findex].end_addr - 1;
            uint stop  = vm->frames[Findex].addr + 1;
            invert_op_to_line(vm, proc_name, original_buffer, start, stop); /* <-- proc_name, non frame_name */

            *newline = '\n';   /* <-- ripristina prima del continue */
            ptr = newline + 1;
            continue; 
        } else if (strcmp(firstWord, "LOCAL")   == 0) { op_local  (vm, frame_name);
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
            char *new_ptr = op_jmpf(vm, frame_name, original_buffer);
            if (new_ptr) { *newline = '\n'; ptr = new_ptr; continue; }

        } else if (strcmp(firstWord, "JMP") == 0) {
            char *new_ptr = op_jmp(vm, frame_name, original_buffer);
            *newline = '\n'; ptr = new_ptr; continue;
        }

        *newline = '\n';
        ptr = newline + 1;
    }

    free(original_buffer);
}

/* ======================================================================
 *  VM EXEC  (prima passata: parsing statico del bytecode)
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

int main(void)
{
    char buffer[START_BUFFER];
    char ast[AST_BUFFER];
    ast[0] = '\0';

    FILE *fp = fopen("bytecode.txt", "r");
    if (!fp) { perror("Errore nell'apertura del file\n"); return 1; }

    while (fgets(buffer, sizeof(buffer), fp))
        strncat(ast, buffer, sizeof(ast) - strlen(ast) - 1);
    fclose(fp);

    VM vm;
    memset(&vm, 0, sizeof(VM));
    vm_exec(&vm, ast);
    vm_dump(&vm);
    return 0;
}