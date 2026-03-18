#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "char_id_map.h" //per la ricerca del frame in base al nome della procedura
CharIdMap FrameIndexer;
#include "stack.h"

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
typedef struct {
    CharIdMap VarIndexer;
    Stack LocalVariables;
    Var  *vars[MAX_VARS];
    int  var_count;  // high-water mark: indice massimo usato + 1
    char name[VAR_NAME_LENGTH]; // nome del frame (nome della procedura)
    uint addr; //indirizzo della procedure
} Frame;

#define MAX_FRAMES 10
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
    uint main_index = char_id_map_get(&FrameIndexer, *frame_name); 

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
            char *line = ptr + 6;  // salto i primi 6 caratteri
            char *firstWord = strtok(line, " \t");  // divide per spazi o tab
            if (strcmp(firstWord, "END_PROC") == 0){
                return; //fina la procedura torniamo ricorsivamente
            } else if (strcmp(firstWord, "DELOCAL") == 0){
                //controlliamo se i valori corrispondono con quelli ottenuti
                char* Vtype = strtok(NULL, " \t");
                char* Vname = strtok(NULL, " \t");
                char * c_Vvalue = strtok(NULL, " \t");
                int Vvalue = (unsigned int) strtoul(c_Vvalue, NULL, 10);
                //printf("%d-1=",stack_size(&vm->frames[vm->frame_top].LocalVariables));
                Var *V = stack_pop(&vm->frames[vm->frame_top].LocalVariables);
                //printf("%d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
                //printf("%s %s %d =? %ls\n", Vtype, Vname, Vvalue, V->value);
                if(strcmp(Vtype, (V->T == 0 ? "int" : "stack") ) == 0){
                    //printf("%d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
                    if (strcmp(Vtype, "stack") == 0){
                        //se e' uno stack da deallocare sara' per essere giusto nil
                        //controliamo se la sua lunghezza e' 0
                        if(V->stack_len == 0){
                            //controlliamo che utente abbiamo messo nil come risultato finale dello stack 
                            if (strcmp(c_Vvalue, "nil") == 0)
                                delete_var(vm->frames[vm->frame_top].vars, &vm->frames[vm->frame_top].var_count, char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *Vname));
                            else perror("[VM] DEALLOC stack deve essere nil!\n");
                        } else perror("[VM] DEALLOC valore finale di stack diverso da quello aspettato!\n");
                    }
                    else if (Vvalue == *(V->value)){
                        //ora che abbiamo controllato che la variabile locale abbia il valore giusto la togliamo dallo stack
                        //l'abbiamo gia' tolta prima quando abbiamo definito V*
                        //ora la togliamo dal frame la variabile
                        delete_var(vm->frames[vm->frame_top].vars, &vm->frames[vm->frame_top].var_count, char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *Vname));
                    } else perror("[VM] DEALLOC variabile o valore finale diverso da quello aspettato!\n");
                } else perror("[VM] DEALLOC deve avere lo stesso tipo di ALLOCAL!\n");
                //printf("%ls\n", V->value);
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
                char* extra = strtok(NULL, " \t"); // prova a prendere un secondo token
                if (extra != NULL) {
                    perror("[VM] SHOW supporta 1 sola parametro!\n");
                } 
                uint Findex = char_id_map_get(&FrameIndexer, *frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, *ID);
                if (vm->frames[Findex].vars[Vindex]->T == TYPE_INT)//INT
                    printf("%s: %d\n", ID, *(vm->frames[Findex].vars[Vindex]->value));                    
                else if (vm->frames[Findex].vars[Vindex]->T == TYPE_STACK){
                    //todo printare per stack
                    printf("%s: {}", ID);
                } else perror("[VM] ERRORE show su variabile parametro non linkata!\n");
            } else if (strcmp(firstWord, "PUSHEQ") == 0){
                char* ID = strtok(NULL, " \t");
                char* C_Vvalue = strtok(NULL, " \t");

                uint Findex = char_id_map_get(&FrameIndexer, *frame_name);
                uint Vindex = char_id_map_get(&vm->frames[Findex].VarIndexer, *ID);

                if (vm->frames[Findex].vars[Vindex]->T == TYPE_INT){
                    // ← RIMOSSA la riga malloc
                    int Vvalue = (int) strtoul(C_Vvalue, NULL, 10); 
                    int old_value = *(vm->frames[Findex].vars[Vindex]->value);
                    //printf("OLD VALUE %d\n", old_value);
                    *(vm->frames[Findex].vars[Vindex]->value) += Vvalue;
                    //printf("NEW VALUE %d\n", *(vm->frames[Findex].vars[Vindex]->value));  
                }
            } else if (strcmp(firstWord, "MINEQ") == 0){
                //tempo di esecuzione
            } else if (strcmp(firstWord, "PRODEQ") == 0){
                //tempo di esecuzione
            } else if (strcmp(firstWord, "DIVEQ") == 0){
                //tempo di esecuzione
            } else if (strcmp(firstWord, "MODEQ") == 0){
                //tempo di esecuzione
                
         } 
            *newline = '\n';  // ripristina il carattere
            ptr = newline + 1; 
        }
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
                    char* name = strtok(NULL, " \t"); //prendiamo il nome della funzione
                    uint index = char_id_map_get(&FrameIndexer, *name); //segnamoci il numero del frame corrispondente
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
                    int Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *Vname); //int perche puo essere -1
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
                    int Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *Vname); //int perche puo essere -1
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
                    vm->frames[vm->frame_top].vars[Vindex]->is_local = 1; //la variabile LOCAL e' locale
                    strncpy(vm->frames[vm->frame_top].vars[Vindex]->name, Vname, VAR_NAME_LENGTH - 1); // memorizziamo il nome della variabile
                    vm->frames[vm->frame_top].vars[Vindex]->name[VAR_NAME_LENGTH - 1] = '\0';

                    Var* V = vm->frames[vm->frame_top].vars[Vindex];
                    //printf("%d\n", *V->value);
                    stack_push(&vm->frames[vm->frame_top].LocalVariables, V); //mettiamo nello stack LIFO la variabile LOCAL in head
                } else if (strcmp(firstWord, "DELOCAL") == 0){
                    //da controllare in esecuzione
                } else if (strcmp(firstWord, "CALL") == 0) {
                    char* proc_name = strtok(NULL, " \t");
                    uint Pindex = char_id_map_get(&FrameIndexer, *proc_name);

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
                        Var* Vto_link = vm->frames[vm->frame_top].vars[
                            char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *param)
                        ];

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
                    //printf("%s %s\n", type, ID);
                    int Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *Vname);

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
                } 
                else if (strcmp(firstWord, "PUSHEQ") == 0){
                    //tempo di esecuzione
                } 
                else if (strcmp(firstWord, "MINEQ") == 0){
                    //tempo di esecuzione
                } 
                else if (strcmp(firstWord, "PRODEQ") == 0){
                    //tempo di esecuzione
                } 
                else if (strcmp(firstWord, "DIVEQ") == 0){
                    //tempo di esecuzione
                } 
                else if (strcmp(firstWord, "MODEQ") == 0){
                    //tempo di esecuzione
                } else {
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
            printf("frame[%d] (%s): \n", i, f->name);
            for (int j = 0; j < f->var_count; j++) {
                Var *v = f->vars[j];
                if (v == NULL) continue; // slot libero (variabile gia' deallocata), saltiamo
                printf("\tVar[%d] Name: %s, Type: %s, is_local: %d, value: ", 
                    j, v->name, (v->T == 0 ? "INT" : (v->T == 1 ? "STACK" : "PARAM")), v->is_local);
                
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

int main(){
    char buffer[256];
    char ast[1024*10];  // buffer più grande per contenere tutto il file
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
    size_t length = sizeof(ast);
    vm_exec(&vm, ast);
    vm_dump(&vm);
    return 0;
}