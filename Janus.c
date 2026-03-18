#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "char_id_map.h" //per la ricerca del frame in base al nome della procedura
CharIdMap FrameIndexer;
#include "stack.h"

#define uint unsigned int
#define perror(msg) {printf(msg); exit(EXIT_FAILURE);}

typedef enum {
    TYPE_INT = 0,
    TYPE_STACK = 1,
    TYPE_PARAM = 2 
} ValueType;

#define VAR_NAME_LENGTH 100
#define VAR_STACK_MAX_SIZE 128 //byte
typedef struct Var{
    ValueType T; //da qui capiamo se e' INT o STACK
    int*  value;
    size_t stack_len;  //indica la lunghezza dello stack
    int  is_local;   // 1 = local/delocal, 0 = decl normale
    char name[VAR_NAME_LENGTH]; // nome della variabile
} Var;

//cancellare elemento n delle variabili del frame
// NON shiftiamo: char_id_map assegna indici stabili e permanenti,
// shiftare romperebbe la corrispondenza nome->indice
void delete_var(Var *vars[], int *size, int n) {
    if (n < 0 || n >= *size) {
        printf("Indice fuori range!\n");
        return;
    }
    free(vars[n]->value);  // libera memoria del valore
    free(vars[n]);         // libera la struttura Var
    vars[n] = NULL;        // azzera il puntatore (slot libero)
    // non shiftiamo e non decrementiamo size:
    // lo slot e' libero (vars[n]==NULL) e puo' essere riusato
}

#define MAX_VARS 100
#define MAX_LABEL 100
typedef struct {
    CharIdMap VarIndexer;
    Stack LocalVariables;
    Var  *vars[MAX_VARS];
    int  var_count;  // high-water mark: indice massimo usato + 1
    CharIdMap LabelIndexer;
    uint label[MAX_LABEL];
    char name[VAR_NAME_LENGTH]; // nome del frame (nome della procedura)
    uint addr; //indirizzo della procedure
    uint val_IF; //bool
} Frame;

#define MAX_FRAMES 100
typedef struct {
    Frame frames[MAX_FRAMES];
    int   frame_top;   // indice del frame corrente (-1 = vuoto)
} VM;
void delete_frame(VM *vm, int n) {
    if (n < 0 || n > vm->frame_top) {
        printf("Indice frame non valido\n");
        return;
    }

    // liberare eventuali risorse del frame da cancellare
    // free(vm->frames[n].vars); ecc.

    // shift dei frame successivi
    for (int i = n; i < vm->frame_top; i++) {
        vm->frames[i] = vm->frames[i + 1];
    }

    vm->frame_top--;  // decrementa il numero di frame
}

char* go_to_line(char* buffer, uint line) {
    if (buffer == NULL) return NULL;
    if (line == 0) return buffer;

    uint current_line = 1;
    char* ptr = buffer;

    while (*ptr != '\0') {
        if (current_line == line) {
            return ptr;
        }
        if (*ptr == '\n') {
            current_line++;
        }
        ptr++;
    }

    return NULL;
}

void vm_run_BT(VM *vm, char* buffer, char* frame_name) {
    //printf("ESEGUO: %s\n", frame_name);
    char* original_buffer = strdup(buffer);
    uint main_index = char_id_map_get(&FrameIndexer, frame_name); 

    //printf("main_index: %u\n", main_index);
    //printf("addr: %u\n", vm->frames[main_index].addr);

    if (buffer == NULL) {
        fprintf(stderr, "ERROR: buffer is NULL\n");
        return;
    }

    buffer = go_to_line(buffer, vm->frames[main_index].addr+1);

    if (buffer == NULL) {
        fprintf(stderr, "ERROR: line %u not found in buffer\n", vm->frames[main_index].addr);
        return;
    }

    //printf("%s\n", buffer);
    char *ptr = buffer;
    while (*ptr != '\0') {
        char *newline = strchr(ptr, '\n');
         if (newline != NULL) {
            *newline = '\0';  // temporaneamente terminate la riga
            char line_buf[512];
            strncpy(line_buf, ptr + 6, sizeof(line_buf) - 1);
            line_buf[sizeof(line_buf) - 1] = '\0';
            char *firstWord = strtok(line_buf, " \t");
            if (strcmp(firstWord, "END_PROC") == 0){
                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                if (stack_size(&vm->frames[Findex].LocalVariables) > -1){
                    perror("[VM] END_PROC: ci sono ancora variabili LOCAL non chiuse con DELOCAL!\n");
                }
                return;
            } else if (strcmp(firstWord, "LOCAL") == 0){
                char* Vtype = strtok(NULL, " \t");
                char* Vname = strtok(NULL, " \t");
                //printf("LOCAL %s\n", Vname);
                char* c_Vvalue = strtok(NULL, " \t");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, Vname);
                Var* dst = vm->frames[Findex].vars[Vindex];
                if (char_id_map_exists(&vm->frames[Findex].VarIndexer, c_Vvalue)) {
                    // e' un ID: copiamo il valore aggiornato al momento dell'esecuzione
                    int SrcIndex = char_id_map_get(&vm->frames[Findex].VarIndexer, c_Vvalue);
                    Var* src = vm->frames[Findex].vars[SrcIndex];

                    if (src->T == TYPE_INT) {
                        *(dst->value) = *(src->value);
                    } else if (src->T == TYPE_STACK) {
                        dst->stack_len = src->stack_len;
                        memcpy(dst->value, src->value, src->stack_len * sizeof(int));
                    } else perror("[VM] tentata copia su variabile PARAM\n");
                } else {
                    // e' un numero

                    if (dst->T == TYPE_INT) {
                        int Vvalue = (int) strtoul(c_Vvalue, NULL, 10);
                        *(dst->value) = Vvalue;
                    } else if (dst->T == TYPE_STACK) {
                        if (strcmp(c_Vvalue, "nil") == 0) {
                            dst->stack_len = 0; // gia' 0, ma lo resettiamo esplicitamente
                        } else perror("[VM] Valore stack per LOCAL non compatibile!\n");
                    } else perror("[VM] tentata copia su variabile PARAM\n");
                }
                
                // push nello stack LIFO per il controllo DELOCAL
                //printf("%d index\n",Findex);
                //printf("PRE LOCAL SIZE= %d\n",stack_size(&vm->frames[Findex].LocalVariables));
                stack_push(&vm->frames[Findex].LocalVariables, dst);
                //printf("POST LOCAL SIZE= %d\n",stack_size(&vm->frames[Findex].LocalVariables));
                //printf("%d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
            } else if (strcmp(firstWord, "DELOCAL") == 0){
                char* Vtype = strtok(NULL, " \t");
                char* Vname = strtok(NULL, " \t");
                char* c_Vvalue = strtok(NULL, " \t");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name); // <-- usa frame_name

                int Vvalue = 0;
                if (char_id_map_exists(&vm->frames[Findex].VarIndexer, c_Vvalue)) {
                    int SrcIndex = char_id_map_get(&vm->frames[Findex].VarIndexer, c_Vvalue);
                    Vvalue = *(vm->frames[Findex].vars[SrcIndex]->value);
                } else {
                    Vvalue = (int) strtoul(c_Vvalue, NULL, 10);
                }

                // TUTTO usa Findex, non vm->frame_top
                Var *V = stack_pop(&vm->frames[Findex].LocalVariables);
                
                if(strcmp(Vtype, (V->T == 0 ? "int" : "stack")) == 0){
                    if (strcmp(Vtype, "stack") == 0){
                        if(V->stack_len == 0){
                            if (strcmp(c_Vvalue, "nil") == 0)
                                delete_var(vm->frames[Findex].vars, &vm->frames[Findex].var_count,
                                        char_id_map_get(&vm->frames[Findex].VarIndexer, Vname));
                            else perror("[VM] DEALLOC stack deve essere nil!\n");
                        } else perror("[VM] DEALLOC valore finale di stack diverso da quello aspettato!\n");
                    } else if (Vvalue == *(V->value)){
                        delete_var(vm->frames[Findex].vars, &vm->frames[Findex].var_count,
                                char_id_map_get(&vm->frames[Findex].VarIndexer, Vname));
                    } else perror("[VM] DEALLOC variabile o valore finale diverso da quello aspettato!\n");
                } else perror("[VM] DEALLOC errato (tipo o variabile)\n");
            } else if (strcmp(firstWord, "CALL") == 0){
                //printf("CALL\n");
                //i link fra variabili sono gia stati fatti
                //dobbiamo solo far JMP quella procedure
                char* proc_name = strtok(NULL, " \t");
                vm_run_BT(vm, original_buffer, proc_name);
            } else if (strcmp(firstWord, "UNCALL") == 0){
                //todo implementare reversione
            } else if (strcmp(firstWord, "SHOW") == 0) {
                char* ID = strtok(NULL, " \t");   // primo token dopo SHOW
                //printf("ID: %s\n", ID);
                char* extra = strtok(NULL, " \t"); // prova a prendere un secondo token
                if (extra != NULL) {
                    perror("[VM] SHOW supporta 1 sola parametro!\n");
                }                 
                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                if (!char_id_map_exists(&vm->frames[Findex].VarIndexer, ID)) {
                    fprintf(stderr, "[VM] SHOW: variabile '%s' non definita!\n", ID);
                    exit(EXIT_FAILURE);
                }
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, ID);
                if (vm->frames[Findex].vars[Vindex]->T == TYPE_INT)//INT
                    printf("%s: %d\n", ID, *(vm->frames[Findex].vars[Vindex]->value));                    
                else if (vm->frames[Findex].vars[Vindex]->T == TYPE_STACK){
                    Var* sv = vm->frames[Findex].vars[Vindex];
                    printf("%s: [", ID);
                    for (size_t k = 0; k < sv->stack_len; k++) {
                        printf("%d", sv->value[k]);
                        if (k + 1 < sv->stack_len) printf(", ");
                    }
                    printf("]\n");
                } else perror("[VM] ERRORE show su variabile parametro non linkata!\n");
            } else if (strcmp(firstWord, "PUSHEQ") == 0){
                char* ID = strtok(NULL, " \t");
                char* C_Vvalue = strtok(NULL, " \t");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, ID);

                if (vm->frames[Findex].vars[Vindex]->T != TYPE_INT){
                    perror("[VM] Operatore non supportato su stack!\n");
                }

                int Vvalue;
                if (char_id_map_exists(&vm->frames[Findex].VarIndexer, C_Vvalue)) {
                    uint V2index = char_id_map_get(&vm->frames[Findex].VarIndexer, C_Vvalue);
                    Vvalue = *(vm->frames[Findex].vars[V2index]->value);
                } else {
                    Vvalue = (int) strtoul(C_Vvalue, NULL, 10);
                }

                *(vm->frames[Findex].vars[Vindex]->value) += Vvalue;
            } else if (strcmp(firstWord, "MINEQ") == 0){
                char* ID = strtok(NULL, " \t");
                char* C_Vvalue = strtok(NULL, " \t");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, ID);

                if (vm->frames[Findex].vars[Vindex]->T != TYPE_INT){
                    perror("[VM] Operatore non supportato su stack!\n");
                }

                int Vvalue;
                if (char_id_map_exists(&vm->frames[Findex].VarIndexer, C_Vvalue)) {
                    uint V2index = char_id_map_get(&vm->frames[Findex].VarIndexer, C_Vvalue);
                    Vvalue = *(vm->frames[Findex].vars[V2index]->value);
                } else {
                    Vvalue = (int) strtoul(C_Vvalue, NULL, 10);
                }

                *(vm->frames[Findex].vars[Vindex]->value) -= Vvalue;
            } else if (strcmp(firstWord, "PRODEQ") == 0){
                char* ID = strtok(NULL, " \t");
                char* C_Vvalue = strtok(NULL, " \t");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, ID);

                if (vm->frames[Findex].vars[Vindex]->T != TYPE_INT){
                    perror("[VM] Operatore non supportato su stack!\n");
                }

                int Vvalue;
                if (char_id_map_exists(&vm->frames[Findex].VarIndexer, C_Vvalue)) {
                    uint V2index = char_id_map_get(&vm->frames[Findex].VarIndexer, C_Vvalue);
                    Vvalue = *(vm->frames[Findex].vars[V2index]->value);
                } else {
                    Vvalue = (int) strtoul(C_Vvalue, NULL, 10);
                }

                *(vm->frames[Findex].vars[Vindex]->value) *= Vvalue;
            } else if (strcmp(firstWord, "DIVEQ") == 0){
                char* ID = strtok(NULL, " \t");
                char* C_Vvalue = strtok(NULL, " \t");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, ID);

                if (vm->frames[Findex].vars[Vindex]->T != TYPE_INT){
                    perror("[VM] Operatore non supportato su stack!\n");
                }

                int Vvalue;
                if (char_id_map_exists(&vm->frames[Findex].VarIndexer, C_Vvalue)) {
                    uint V2index = char_id_map_get(&vm->frames[Findex].VarIndexer, C_Vvalue);
                    Vvalue = *(vm->frames[Findex].vars[V2index]->value);
                } else {
                    Vvalue = (int) strtoul(C_Vvalue, NULL, 10);
                }

                if (Vvalue == 0) perror("[VM] Divisione per zero!\n");
                *(vm->frames[Findex].vars[Vindex]->value) /= Vvalue;
            } else if (strcmp(firstWord, "MODEQ") == 0){
                char* ID = strtok(NULL, " \t");
                char* C_Vvalue = strtok(NULL, " \t");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, ID);

                if (vm->frames[Findex].vars[Vindex]->T != TYPE_INT){
                    perror("[VM] Operatore non supportato su stack!\n");
                }

                int Vvalue;
                if (char_id_map_exists(&vm->frames[Findex].VarIndexer, C_Vvalue)) {
                    uint V2index = char_id_map_get(&vm->frames[Findex].VarIndexer, C_Vvalue);
                    Vvalue = *(vm->frames[Findex].vars[V2index]->value);
                } else {
                    Vvalue = (int) strtoul(C_Vvalue, NULL, 10);
                }

                if (Vvalue == 0) perror("[VM] Modulo per zero!\n");
                *(vm->frames[Findex].vars[Vindex]->value) %= Vvalue;
            } else if (strcmp(firstWord, "EXPEQ") == 0) {
                char* ID = strtok(NULL, " \t");
                char* C_Vvalue = strtok(NULL, " \t");
                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, ID);

                if (vm->frames[Findex].vars[Vindex]->T == TYPE_INT) {
                    int Vvalue;

                    // controlliamo se è una variabile o un numero
                    if (char_id_map_exists(&vm->frames[Findex].VarIndexer, C_Vvalue)) {
                        uint SrcIndex = char_id_map_get(&vm->frames[Findex].VarIndexer, C_Vvalue);
                        Vvalue = *(vm->frames[Findex].vars[SrcIndex]->value);
                    } else {
                        Vvalue = (int) strtoul(C_Vvalue, NULL, 10);
                    }

                    int old_value = *(vm->frames[Findex].vars[Vindex]->value);
                    int result = 1;
                    for (int i = 0; i < Vvalue; i++) {
                        result *= old_value;
                    }
                    *(vm->frames[Findex].vars[Vindex]->value) = result;

                } else {
                    perror("[VM] Operatore EXPEQ non supportato su stack!\n");
                }
            } else if (strcmp(firstWord, "PUSH") == 0) {
                char* C_Vvalue = strtok(NULL, " \t");   // variabile da pushare
                char* C_stack  = strtok(NULL, " \t");   // stack destinazione
                char* C_VvalueTEST = strtok(NULL, " \t");
                if (C_VvalueTEST != NULL)
                    perror("[VM] troppi parametri per PUSH!\n");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name);

                // 1) Risolvi il valore da pushare (ID o letterale)
                int value_to_push;
                if (char_id_map_exists(&vm->frames[Findex].VarIndexer, C_Vvalue)) {
                    uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, C_Vvalue);
                    Var* src_var = vm->frames[Findex].vars[Vindex];
                    value_to_push = *(src_var->value);

                    // Azzeriamo x per il trasferimento reversibile
                    *(src_var->value) = 0;
                } else {
                    value_to_push = (int) strtoul(C_Vvalue, NULL, 10);
                }

                // 2) Pusha sullo stack destinazione
                if (!char_id_map_exists(&vm->frames[Findex].VarIndexer, C_stack))
                    perror("[VM] PUSH: stack destinazione non trovato!\n");

                uint Sindex = char_id_map_get(&vm->frames[Findex].VarIndexer, C_stack);
                Var* stack_var = vm->frames[Findex].vars[Sindex];

                if (stack_var->T != TYPE_STACK)
                    perror("[VM] PUSH: la destinazione non e' uno stack!\n");

                stack_var->value = realloc(stack_var->value, (stack_var->stack_len + 1) * sizeof(int));
                if (!stack_var->value) perror("realloc failed");
                stack_var->value[stack_var->stack_len] = value_to_push;
                stack_var->stack_len++;
            } else if (strcmp(firstWord, "POP") == 0) {
                // pop(x, s): x += valore estratto dalla cima dello stack s
                char* C_Vdest  = strtok(NULL, " \t");   // variabile destinazione (x)
                char* C_stack  = strtok(NULL, " \t");   // stack sorgente (s)
                char* C_extraTEST = strtok(NULL, " \t");
                if (C_extraTEST != NULL)
                    perror("[VM] troppi parametri per POP!\n");

                uint Findex = char_id_map_get(&FrameIndexer, frame_name);

                // 1) Trova lo stack sorgente
                if (!char_id_map_exists(&vm->frames[Findex].VarIndexer, C_stack))
                    perror("[VM] POP: stack sorgente non trovato!\n");
                uint Sindex = char_id_map_get(&vm->frames[Findex].VarIndexer, C_stack);
                Var* stack_var = vm->frames[Findex].vars[Sindex];
                if (stack_var->T != TYPE_STACK)
                    perror("[VM] POP: la sorgente non e' uno stack!\n");
                if (stack_var->stack_len == 0)
                    perror("[VM] POP: stack vuoto!\n");

                // 2) Estrai il valore dalla cima (top = stack_len - 1)
                stack_var->stack_len--;
                int popped_value = stack_var->value[stack_var->stack_len];
                stack_var->value = realloc(stack_var->value, stack_var->stack_len * sizeof(int));
                // nota: se stack_len == 0, realloc con size 0 e' implementation-defined; potresti usare free+NULL

                // 3) Somma il valore estratto alla variabile destinazione (x += popped)
                if (!char_id_map_exists(&vm->frames[Findex].VarIndexer, C_Vdest))
                    perror("[VM] POP: variabile destinazione non trovata!\n");
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, C_Vdest);
                Var* dest_var = vm->frames[Findex].vars[Vindex];
                *(dest_var->value) += popped_value;   // x += pop(s)
            } else if (strcmp(firstWord, "EVAL") == 0) {
                //printf("EVAL\n");
                //controlliamo se il valore dato dal utente sia uguale a quello di runtime attuale
                char* ID = strtok(NULL, " \t"); 
                char* c_CValue = strtok(NULL, " \t"); 
                //printf("ID: %s=%s\n", ID, c_CValue);
                int Vvalue = (int) strtoul(c_CValue, NULL, 10);
                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                if(!char_id_map_exists(&vm->frames[Findex].VarIndexer, ID))
                    perror("[VM] EVAL di variabile non esistente a runtime!\n");
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, ID);
                //printf("EVAL %d =? %d\n", *(vm->frames[Findex].vars[Vindex]->value), Vvalue);
                vm->frames[Findex].val_IF = (*(vm->frames[Findex].vars[Vindex]->value) == Vvalue);
                //printf("EVAL: %d\n", vm->frames[Findex].val_IF);
            } else if (strcmp(firstWord, "JMPF") == 0){
                //printf("JMPF\n");
                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                if (!vm->frames[Findex].val_IF){
                    char* c_LABEL = strtok(NULL, " \t"); 
                    uint Lindex = char_id_map_get(&vm->frames[Findex].LabelIndexer, c_LABEL);
                    *newline = '\n';
                    //printf("JMPF to %s!\n", c_LABEL);
                    ptr = go_to_line(original_buffer, vm->frames[Findex].label[Lindex]+1);
                    continue;  // stesso fix
                }
            } else if (strcmp(firstWord, "JMP") == 0){
                char* c_LABEL = strtok(NULL, " \t"); 
                //printf("JMP %s\n", c_LABEL);
                uint Findex = char_id_map_get(&FrameIndexer, frame_name);
                uint Lindex = char_id_map_get(&vm->frames[Findex].LabelIndexer, c_LABEL);
                *newline = '\n';  // ripristina prima di riposizionare
                //printf("\t BUFFER \n%s\n", original_buffer);
                ptr = go_to_line(original_buffer, vm->frames[Findex].label[Lindex]+1);
                if (!ptr) perror("ERRORE buffer\n");
                continue;  // riprende il while dal nuovo ptr, niente ricorsione
            } else if (strcmp(firstWord, "ASSERT") == 0) {
                char *ID1 = strtok(NULL, " \t");
                char *ID2 = strtok(NULL, " \t");

                if (!ID1 || !ID2) {
                    fprintf(stderr, "[VM] ASSERT: argomenti mancanti\n");
                    continue;
                }
                if(!char_id_map_exists(&FrameIndexer, frame_name))
                    perror("[VM] Frame non trovato per ASSERT!\n");
                uint Findex = char_id_map_get(&FrameIndexer, frame_name);

                /* Risolve il valore di un token: numero letterale oppure variabile */
                char *endptr1, *endptr2;
                unsigned long val1 = strtoul(ID1, &endptr1, 10);
                unsigned long val2 = strtoul(ID2, &endptr2, 10);

                if (*endptr1 != '\0') {
                    /* ID1 è una variabile: leggi il suo valore */
                    if(!char_id_map_exists(&vm->frames[Findex].VarIndexer, ID1))
                        perror("[VM] ASSET fi prima variabile non inizializzata");
                    int V1index = char_id_map_get(&vm->frames[Findex].VarIndexer, ID1);
                    val1 = *(vm->frames[Findex].vars[V1index]->value);
                }

                if (*endptr2 != '\0') {
                    /* ID2 è una variabile: leggi il suo valore */
                    if(!char_id_map_exists(&vm->frames[Findex].VarIndexer, ID2))
                        perror("[VM] ASSET fi seconda variabile non inizializzata");
                    int V2index = char_id_map_get(&vm->frames[Findex].VarIndexer, ID2);
                    val2 = *(vm->frames[Findex].vars[V2index]->value);
                }
                //printf("EVAL %lu = %lu\n", val1, val2); 
                if (val1 != val2) {
                    perror("[VM] ASSERT valori fi diversi da quelli a runtime!\n");
                }
            } 
        }
        *newline = '\n';  // ripristina il carattere
        ptr = newline + 1;
    }
}
void vm_exec(VM *vm, char* buffer) {
    char* original_buffer = strdup(buffer); //vera copia non linkata con ptr e le sue modifiche
    char *ptr = buffer;
    int current_line = 1;
    while (*ptr != '\0') {
        // trova la fine della riga
        char *newline = strchr(ptr, '\n');
        if (newline != NULL) {
            *newline = '\0';  // temporaneamente terminate la riga
            if (strlen(ptr) > 6) {
                char *line = ptr + 6;  // salto i primi 6 caratteri
                //printf("%s\n", line);  // salta i primi 6 caratteri
                char *firstWord = strtok(line, " \t");  // divide per spazi o tab
                //printf("%s\n", firstWord);
                if (strcmp(firstWord, "START") == 0) {
                    char_id_map_init(&FrameIndexer);
                    vm->frame_top = -1; //settiamo il frame corrente come vuoto
                } else if (strcmp(firstWord, "HALT") == 0) {
                    //vm->frame_top = -1; //frame corrente vuoto
                } else if (strcmp(firstWord, "PROC") == 0) {
                    //char_id_map_init(&vm->frames[vm->frame_top].LabelIndexer); //init della map per il frame
                    char* name = strtok(NULL, " \t"); //prendiamo il nome della funzione
                    uint index = char_id_map_get(&FrameIndexer, name); //segnamoci il numero del frame corrispondente
                    vm->frame_top = index; //ora l'ultimo frame è quello creato
                    //printf("Creazione stack per new proc %s : %d\n", name, vm->frame_top);

                    //ora nel frame appena creato inizializziamo il frame
                    char_id_map_init(&vm->frames[vm->frame_top].VarIndexer); //inizializziamo la mappa degli indici delle variabili del frame
                    stack_init(&vm->frames[vm->frame_top].LocalVariables); //init. dello stack LIFO delle variabili locali del frame
                    strncpy(vm->frames[vm->frame_top].name, name, VAR_NAME_LENGTH - 1); // memorizziamo il nome del frame
                    vm->frames[vm->frame_top].name[VAR_NAME_LENGTH - 1] = '\0';
                    //copiamo indirizzo del frame
                    vm->frames[vm->frame_top].addr = current_line;
                } else if (strcmp(firstWord, "END_PROC") == 0){
                    char* name = strtok(NULL, " \t"); //prendiamo il nome della funzione
                    //printf("%d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
                    
                    //printf("%d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
                    if (strcmp(name, "main") == 0){
                        //printf("%s\n", name);
                        //INIZIAMO A ESEGUIRE
                        //printf("%s\n", original_buffer);
                        char* main_name = "main";
                        vm_run_BT(vm, original_buffer, main_name);
                    }
                } else if (strcmp(firstWord, "DECL") == 0) {
                    //segniamo la crezione di una nuova variabile
                    char* type = strtok(NULL, " \t");
                    //printf("%s ", type);
                    char* Vname = strtok(NULL, " \t");
                    //printf("%s \t", Vname);

                    // prendiamo Vindex PRIMA di assegnare T, cosi' usiamo sempre l'indice stabile
                    int Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, Vname); //int perche puo essere -1
                    //printf("INDEX %d\n", Vindex);
                    //printf("STACK SIZE DECL: %d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
                    if (stack_size(&vm->frames[vm->frame_top].LocalVariables) > -1){
                        perror("[VM] DECL non permessa: ci sono ancora variabili LOCAL aperte!\n");
                    }
                    if (vm->frames[vm->frame_top].vars[Vindex] != NULL){
                        //variabile gia definita con DECL
                        perror("[VM] Varriabile gia definita precedente!\n");
                    }

                    // allochiamo la struttura Var per questo slot
                    vm->frames[vm->frame_top].vars[Vindex] = malloc(sizeof(Var));
                    memset(vm->frames[vm->frame_top].vars[Vindex], 0, sizeof(Var));

                    // assegniamo T direttamente su Vindex (non su var_count)
                    if (strcmp(type, "int") == 0){
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_INT;
                    } else if (strcmp(type, "stack") == 0){
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_STACK;
                    } else perror("[VM] type variabile non esistente\n");

                    // aggiorniamo var_count come high-water mark
                    if (Vindex >= vm->frames[vm->frame_top].var_count)
                        vm->frames[vm->frame_top].var_count = Vindex + 1;

                    //ASSEGNIAZIONE VALORI DEFAULT
                    //printf("Frame %d, Var: %s, IndexFrame: %d\n", vm->frame_top, Vname, Vindex);
                    if (vm->frames[vm->frame_top].vars[Vindex]->T == TYPE_STACK){
                        vm->frames[vm->frame_top].vars[Vindex]->stack_len = 0; //segniamo che e' uno stack mettendo 0 di lunghezza
                        vm->frames[vm->frame_top].vars[Vindex]->value = malloc(VAR_STACK_MAX_SIZE * sizeof(int)); //dimensione dello stack
                        //printf("%s\n", c_Vvalue);
                    } 
                    else {
                        //se e' INT
                        vm->frames[vm->frame_top].vars[Vindex]->value = malloc(sizeof(int));
                        int Vvalue = 0; 
                        *(vm->frames[vm->frame_top].vars[Vindex]->value) = Vvalue; //la variabile DECL viene inizializzata a 0
                        //printf("%d\n",*(vm->frames[vm->frame_top].vars[Vindex]->value));
                    }

                    vm->frames[vm->frame_top].vars[Vindex]->is_local = 0; //la variabile DECL non è definita locale
                    strncpy(vm->frames[vm->frame_top].vars[Vindex]->name, Vname, VAR_NAME_LENGTH - 1); // memorizziamo il nome della variabile
                    vm->frames[vm->frame_top].vars[Vindex]->name[VAR_NAME_LENGTH - 1] = '\0';
                    //printf("DECL finito\n");
                    //printf("FINE STACK SIZE DECL: %d\n",stack_size(&vm->frames[Vindex].LocalVariables));
                } else if (strcmp(firstWord, "LOCAL") == 0){
                    //segniamo la crezione di una nuova variabile
                    char* type = strtok(NULL, " \t");
                    //printf("%s \n", type);
                    char* Vname = strtok(NULL, " \t");
                    //printf("%s\n", Vname);

                    // prendiamo Vindex PRIMA di assegnare T, cosi' usiamo sempre l'indice stabile
                    int Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, Vname); //int perche puo essere -1
                    //controlliamo se era gia stato definita la variabile con DECL
                    if (vm->frames[vm->frame_top].vars[Vindex] != NULL){
                        //variabile gia definita con DECL
                        perror("[VM] Varriabile local gia definita precedente!\n");
                    }

                    // allochiamo la struttura Var per questo slot
                    vm->frames[vm->frame_top].vars[Vindex] = malloc(sizeof(Var));
                    memset(vm->frames[vm->frame_top].vars[Vindex], 0, sizeof(Var));

                    // assegniamo T direttamente su Vindex (non su var_count)
                    if (strcmp(type, "int") == 0){
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_INT;
                    } else if (strcmp(type, "stack") == 0){
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_STACK;
                    } else perror("[VM] type variabile non esistente\n");

                    // aggiorniamo var_count come high-water mark
                    if (Vindex >= vm->frames[vm->frame_top].var_count)
                        vm->frames[vm->frame_top].var_count = Vindex + 1;

                    //printf("Frame %d, Var: %s, IndexFrame: %d\n", vm->frame_top, Vname, Vindex);
                    //controllo se abbiamo gia definito la variabile
                    //printf("%d <= %d\n",Vindex, stack_size(&vm->frames[vm->frame_top].LocalVariables));
                    char * c_Vvalue = strtok(NULL, " \t");
                    //controlliasmo se e' un ID o un valore
                    if (char_id_map_exists(&vm->frames[vm->frame_top].VarIndexer, c_Vvalue)) {
                        int C_Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, c_Vvalue);

                        Var* dst = vm->frames[vm->frame_top].vars[Vindex];
                        Var* src = vm->frames[vm->frame_top].vars[C_Vindex];

                        // libera memoria vecchia
                        if (dst->value != NULL) {
                            free(dst->value);
                        }

                        if (src->T == TYPE_STACK) {
                            if (dst->T == TYPE_STACK)
                                dst->stack_len = src->stack_len;
                            else 
                                perror("[VM] Assegnazione valore fra tipo diversi!\n");
                            dst->value = malloc(src->stack_len * sizeof(int));
                            memcpy(dst->value, src->value, src->stack_len * sizeof(int));

                        } else { // TYPE_INT
                            if (!(dst->T == TYPE_INT))
                                 perror("[VM] Assegnazione valore fra tipo diversi!\n");
                            dst->value = malloc(sizeof(int));
                            *(dst->value) = *(src->value);
                        }
                    }
                    else{
                        //printf("%s\n", c_Vvalue);
                        //printf("%d\n", vm->frames[vm->frame_top].vars[Vindex]->T);
                        if (vm->frames[vm->frame_top].vars[Vindex]->T == TYPE_STACK){
                            if(strcmp(c_Vvalue, "nil") == 0){
                                //array inizializzato correttamente a vuoto
                                vm->frames[vm->frame_top].vars[Vindex]->stack_len = 0; //segniamo che e' uno stack mettendo 0 di lunghezza
                                vm->frames[vm->frame_top].vars[Vindex]->value = malloc(VAR_STACK_MAX_SIZE * sizeof(int)); //dimensione dello stack
                            } else perror("[VM] Valore stack per init non compatibile!\n");
                            //printf("%s\n", c_Vvalue);
                        } 
                        else {
                            //se e' INT
                            vm->frames[vm->frame_top].vars[Vindex]->value = malloc(sizeof(int));
                            int Vvalue = (int) strtoul(c_Vvalue, NULL, 10); 
                            *(vm->frames[vm->frame_top].vars[Vindex]->value) = Vvalue; //la variabile LOCAL prende il valore passato dal utente
                            //printf("%d\n",*(vm->frames[vm->frame_top].vars[Vindex]->value));
                        }
                    }
                    strncpy(vm->frames[vm->frame_top].vars[Vindex]->name, Vname, VAR_NAME_LENGTH - 1); // memorizziamo il nome della variabile
                    vm->frames[vm->frame_top].vars[Vindex]->name[VAR_NAME_LENGTH - 1] = '\0';
                    vm->frames[vm->frame_top].vars[Vindex]->is_local = 1; //la variabile LOCAL e' locale
                    //la variabile la pushamo nello stack locale quando stiamo eseguendo
                    //Var* V = vm->frames[vm->frame_top].vars[Vindex];
                    //printf("%d\n", *V->value);
                    //stack_push(&vm->frames[vm->frame_top].LocalVariables, V); //mettiamo nello stack LIFO la variabile LOCAL in head
                } else if (strcmp(firstWord, "DELOCAL") == 0){
                    //da controllare in esecuzione
                } else if (strcmp(firstWord, "CALL") == 0) {
                    char* proc_name = strtok(NULL, " \t");
                    uint Pindex = char_id_map_get(&FrameIndexer, proc_name);

                    // 1) Pre-raccogliamo tutti gli indici TYPE_PARAM PRIMA di modificare l'array
                    int param_indices[64];
                    int param_count = 0;
                    for (int j = 0; j < vm->frames[Pindex].var_count; j++) {
                        if (vm->frames[Pindex].vars[j]->T == TYPE_PARAM) {
                            param_indices[param_count++] = j;
                        }
                    }

                    // 2) Ora linchiamo in ordine
                    char* param = NULL;
                    int i = 0;
                    while ((param = strtok(NULL, " \t")) != NULL) {
                        if (i >= param_count) {
                            fprintf(stderr, "ERROR: too many parameters for procedure frame[%d]\n", Pindex);
                            exit(EXIT_FAILURE);
                        }

                        int j = param_indices[i];
                        if (!char_id_map_exists(&vm->frames[vm->frame_top].VarIndexer, param)) {
                            fprintf(stderr, "[VM] CALL: parametro '%s' non definito nel frame chiamante!\n", param);
                            exit(EXIT_FAILURE);
                        }

                        int VtoLink_index = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, param);

                        if (vm->frames[vm->frame_top].vars[VtoLink_index] == NULL) {
                            fprintf(stderr, "[VM] CALL: variabile '%s' è NULL nel frame chiamante!\n", param);
                            exit(EXIT_FAILURE);
                        }

                        Var* Vto_link = vm->frames[vm->frame_top].vars[VtoLink_index];

                        //printf("LINKING param[%d] = %s -> linked with %s frame[%d]\n", i, param, Vto_link->name, vm->frame_top);

                        vm->frames[Pindex].vars[j] = Vto_link; // scrittura sicura, indici già noti
                        i++;
                    }

                    if (i != param_count) {
                        fprintf(stderr, "ERROR: expected %d params, got %d for frame[%d]\n",
                                param_count, i, Pindex);
                        exit(EXIT_FAILURE);
                    }
                } else if (strcmp(firstWord, "UNCALL") == 0){
                    //implementato nel run
                } else if (strcmp(firstWord, "PARAM") == 0){
                    //inizializziamo le variabili a NULL
                    //prendiamo il frame attuale
                    char* Vtype = strtok(NULL, " \t");
                    char* Vname = strtok(NULL, " \t");
                    //printf("%s %s\n", Vtype, Vname);
                    int Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, Vname);
                    //printf("%d\n", Vindex);
                    if (vm->frames[vm->frame_top].vars[Vindex] != NULL){
                        //variabile gia definita con DECL
                        perror("[VM] Non puoi definire piu' volte la stessa variabile nei parametri!\n");
                    }

                    // allochiamo la struttura Var per questo slot
                    vm->frames[vm->frame_top].vars[Vindex] = malloc(sizeof(Var));
                    memset(vm->frames[vm->frame_top].vars[Vindex], 0, sizeof(Var));

                     if (Vindex >= vm->frames[vm->frame_top].var_count)
                        vm->frames[vm->frame_top].var_count = Vindex + 1;

                    // assegniamo T direttamente su Vindex (non su var_count)
                    if (strcmp(Vtype, "int") == 0){
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_INT;
                    } else if (strcmp(Vtype, "stack") == 0){
                        vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_STACK;
                    } else perror("[VM] type variabile non esistente\n");

                    //vm->frames[vm->frame_top].var_count[(int*)(Vindex)] = NULL;

                    //NON DOBBIAMO DEFINIRE VALORI DI DEFAULT PERCHE POI LA LINKIAMO CON QUELLA DI RIFERIMENTO
                    //per non avere seg fault mettiamo valori nul
                    vm->frames[vm->frame_top].vars[Vindex]->value = NULL;
                    vm->frames[vm->frame_top].vars[Vindex]->T = TYPE_PARAM;

                    vm->frames[vm->frame_top].vars[Vindex]->is_local = 0; //la variabile PARAM non è definita locale
                    strncpy(vm->frames[vm->frame_top].vars[Vindex]->name, Vname, VAR_NAME_LENGTH - 1); // memorizziamo il nome della variabile
                    vm->frames[vm->frame_top].vars[Vindex]->name[VAR_NAME_LENGTH - 1] = '\0';
                    //printf("PARAM Frame %d, Var: %s, IndexFrame: %d\n", vm->frame_top, Vname, Vindex);

                } else if (strcmp(firstWord, "SHOW") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "PUSHEQ") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "MINEQ") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "PRODEQ") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "DIVEQ") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "MODEQ") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "EXPEQ") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "PUSH") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "POP") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "EVAL") == 0){
                    //tempo di esecuzione
                   
                } else if (strcmp(firstWord, "JMPF") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "JMP") == 0){
                    //tempo di esecuzione
                } else if (strcmp(firstWord, "LABEL") == 0){
                    //linkiamo la label per il frame attuale
                    char* Lname = strtok(NULL, " \t");
                    //printf("%s\n", Lname);
                    uint Lindex = char_id_map_get(&vm->frames[vm->frame_top].LabelIndexer, Lname); 
                    vm->frames[vm->frame_top].label[Lindex] = current_line;//salviamo la riga della label
                    //printf("LABEL riga %d\n", vm->frames[vm->frame_top].label[Lindex]);
                } else if (strcmp(firstWord, "ASSERT") == 0){
                    //tempo di esecuzione
                }   
                else {
                    printf("[VM] Istruzione sconosciuta: %s\n", firstWord);
                    exit(EXIT_FAILURE);
                }
            } else {
                printf("[VM] Bytecode formattato male!\n");
            }

            *newline = '\n';  // ripristina il carattere
            ptr = newline + 1; // passa alla prossima riga
            current_line++;
        } else {
            // ultima riga senza '\n'
            if (strlen(ptr) > 6) {
                printf("%s\n", ptr + 6);
            } else {
                printf("[VM] Bytecode formattato male!\n");
            }
            break;
        }
    }
}


void vm_dump(VM *vm) {
    printf("=== VM dump ===\n");
    for (int i = 0; i <= vm->frame_top; i++) {
        Frame *f = &vm->frames[i];
        if(strcmp(f->name, "main")==0){
            //stampiamo solo main
            //printf("frame[%d] (%s): \n", i, f->name);
            for (int j = 0; j < f->var_count; j++) {
                Var *v = f->vars[j];
                if (v == NULL) continue; // slot libero (variabile gia' deallocata), saltiamo
                //printf("\tVar[%d] Name: %s, Type: %s, is_local: %d, value: ", j, v->name, (v->T == 0 ? "INT" : (v->T == 1 ? "STACK" : "PARAM")), v->is_local);
                printf("%s: ", v->name);
                if (v->T == 0) {  // INT
                    printf("%d", *(v->value));
                } else { // STACK
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
}

#define START_BUFFER 256
#define AST_BUFFER 1024*10
int main(){
    char buffer[START_BUFFER];
    char ast[AST_BUFFER];  // buffer più grande per contenere tutto il file
    ast[0] = '\0';      // inizializza stringa vuota

    FILE *fp = fopen("bytecode.txt", "r");
    if (fp == NULL) {
        perror("Errore nell'apertura del file");
        return 1;
    }

    while (fgets(buffer, sizeof(buffer), fp)) {
        // concatena ogni riga al buffer completo
        strncat(ast, buffer, sizeof(ast) - strlen(ast) - 1);
    }
    fclose(fp);

    VM vm;
    memset(&vm, 0, sizeof(VM)); 
    //size_t length = sizeof(ast);
    vm_exec(&vm, ast);
    vm_dump(&vm);
    return 0;
}