#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "char_id_map.h" //per la ricerca del frame in base al nome della procedura
CharIdMap FrameIndexer;
#include "stack.h"

#define perror(msg) {printf(msg); exit(EXIT_FAILURE);}

typedef enum {
    TYPE_INT = 0,
    TYPE_STACK = 1
} ValueType;

#define VAR_NAME_LENGTH 100
#define VAR_STACK_MAX_SIZE 128 //byte
typedef struct Var{
    ValueType T; //da qui capiamo se e' INT o STACK
    int*  value;
    size_t stack_len;  //indica la lunghezza dello stack
    int  is_local;   // 1 = local/delocal, 0 = decl normale
} Var;

//cancellare elemento n delle variabili del frame
// NON shiftiamo: char_id_map assegna indici stabili e permanenti,
// shiftare romperebbe la corrispondenza nome->indice
void delete_var(Var vars[], int *size, int n) {
    if (n < 0 || n >= *size) {
        printf("Indice fuori range!\n");
        return;
    }
    free(vars[n].value);              // libera memoria
    memset(&vars[n], 0, sizeof(Var)); // azzera lo slot (value=NULL, slot libero)
    // non shiftiamo e non decrementiamo size:
    // lo slot e' libero (value==NULL) e puo' essere riusato
}

#define MAX_VARS 100
typedef struct {
    CharIdMap VarIndexer;
    Stack LocalVariables;
    Var  vars[MAX_VARS];
    int  var_count;  // high-water mark: indice massimo usato + 1
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


void vm_exec(VM *vm, char* buffer) {
    char *ptr = buffer;

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
                } else if (strcmp(firstWord, "END_PROC") == 0){
                    char* name = strtok(NULL, " \t"); //prendiamo il nome della funzione
                    //printf("%d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
                    if (stack_size(&vm->frames[vm->frame_top].LocalVariables) > -1){
                        perror("[VM] END_PROC con variabili LOCAL ancora aperte!\n");
                    }
                    //printf("%d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
                    if (strcmp(name, "main") == 0){
                        //printf("%s\n", name);
                        // controlliamo che main non abbia LOCAL ancora aperte
                        if (stack_size(&vm->frames[vm->frame_top].LocalVariables) > 0){
                            perror("[VM] END_PROC con variabili LOCAL ancora aperte!\n");
                        }
                        // cancelliamo tutti i frame TRANNE quello di main
                        // main rimane in vita cosi' il dump puo' mostrarne le variabili
                        uint main_index = char_id_map_get(&FrameIndexer, 'm'); //indice del frame di main
                        for (int f = 0; f <= vm->frame_top; f++){
                            if (f == (int)main_index) continue; // saltiamo il frame di main
                            // liberiamo tutte le variabili ancora allocate nel frame f (le DECL)
                            for (int i = 0; i < vm->frames[f].var_count; i++){
                                if (vm->frames[f].vars[i].value != NULL){
                                    free(vm->frames[f].vars[i].value);
                                }
                            }
                            // azzeriamo lo slot del frame (NON shiftiamo: FrameIndexer usa indici stabili)
                            memset(&vm->frames[f], 0, sizeof(Frame));
                        }
                        // frame_top torna all'indice di main, che e' l'unico rimasto
                        vm->frame_top = (int)main_index;
                    }
                } else if (strcmp(firstWord, "DECL") == 0) {
                    //segniamo la crezione di una nuova variabile
                    char* type = strtok(NULL, " \t");
                    //printf("%s \n", type);
                    char* Vname = strtok(NULL, " \t");
                    //printf("%s\n", Vname);

                    // prendiamo Vindex PRIMA di assegnare T, cosi' usiamo sempre l'indice stabile
                    int Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *Vname); //int perche puo essere -1
                    printf("%d\n",stack_size(&vm->frames[Vindex].LocalVariables));
                    if (stack_size(&vm->frames[Vindex].LocalVariables) > -1){
                        perror("[VM] DECL non permessa: ci sono ancora variabili LOCAL aperte!\n");
                    }
                    if (vm->frames[vm->frame_top].vars[Vindex].value != NULL){
                        //variabile gia definita con DECL
                        perror("[VM] Varriabile gia definita precedente!\n");
                    }

                    // assegniamo T direttamente su Vindex (non su var_count)
                    if (strcmp(type, "int") == 0){
                        vm->frames[vm->frame_top].vars[Vindex].T = TYPE_INT;
                    } else if (strcmp(type, "stack") == 0){
                        vm->frames[vm->frame_top].vars[Vindex].T = TYPE_STACK;
                    } else perror("[VM] type variabile non esistente\n");

                    // aggiorniamo var_count come high-water mark
                    if (Vindex >= vm->frames[vm->frame_top].var_count)
                        vm->frames[vm->frame_top].var_count = Vindex + 1;

                    //printf("Frame %d, Var: %s, IndexFrame: %d\n", vm->frame_top, Vname, Vindex);
                    if (vm->frames[vm->frame_top].vars[Vindex].T == TYPE_STACK){
                        vm->frames[vm->frame_top].vars[Vindex].stack_len = 0; //segniamo che e' uno stack mettendo 0 di lunghezza
                        vm->frames[vm->frame_top].vars[Vindex].value = malloc(VAR_STACK_MAX_SIZE * sizeof(int)); //dimensione dello stack
                        //printf("%s\n", c_Vvalue);
                    } 
                    else {
                        //se e' INT
                        vm->frames[vm->frame_top].vars[Vindex].value = malloc(sizeof(int));
                        int Vvalue = 0; 
                        *(vm->frames[vm->frame_top].vars[Vindex].value) = Vvalue; //la variabile DECL viene inizializzata a 0
                        //printf("%d\n",*(vm->frames[vm->frame_top].vars[Vindex].value));
                    }
                    vm->frames[vm->frame_top].vars[Vindex].is_local = 0; //la variabile DECL non è definita locale
                } else if (strcmp(firstWord, "LOCAL") == 0){
                    //segniamo la crezione di una nuova variabile
                    char* type = strtok(NULL, " \t");
                    //printf("%s \n", type);
                    char* Vname = strtok(NULL, " \t");
                    //printf("%s\n", Vname);

                    // prendiamo Vindex PRIMA di assegnare T, cosi' usiamo sempre l'indice stabile
                    int Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *Vname); //int perche puo essere -1
                    //controlliamo se era gia stato definita la variabile con DECL
                    if (vm->frames[vm->frame_top].vars[Vindex].value != NULL){
                        //variabile gia definita con DECL
                        perror("[VM] Varriabile local gia definita precedente!\n");
                    }

                    // assegniamo T direttamente su Vindex (non su var_count)
                    if (strcmp(type, "int") == 0){
                        vm->frames[vm->frame_top].vars[Vindex].T = TYPE_INT;
                    } else if (strcmp(type, "stack") == 0){
                        vm->frames[vm->frame_top].vars[Vindex].T = TYPE_STACK;
                    } else perror("[VM] type variabile non esistente\n");

                    // aggiorniamo var_count come high-water mark
                    if (Vindex >= vm->frames[vm->frame_top].var_count)
                        vm->frames[vm->frame_top].var_count = Vindex + 1;

                    //printf("Frame %d, Var: %s, IndexFrame: %d\n", vm->frame_top, Vname, Vindex);
                    //controllo se abbiamo gia definito la variabile
                    //printf("%d <= %d\n",Vindex, stack_size(&vm->frames[vm->frame_top].LocalVariables));
                    char * c_Vvalue = strtok(NULL, " \t");
                    //printf("%s\n", c_Vvalue);
                    //printf("%d\n", vm->frames[vm->frame_top].vars[Vindex].T);
                    if (vm->frames[vm->frame_top].vars[Vindex].T == TYPE_STACK){
                        if(strcmp(c_Vvalue, "nil") == 0){
                            //array inizializzato correttamente a vuoto
                            vm->frames[vm->frame_top].vars[Vindex].stack_len = 0; //segniamo che e' uno stack mettendo 0 di lunghezza
                            vm->frames[vm->frame_top].vars[Vindex].value = malloc(VAR_STACK_MAX_SIZE * sizeof(int)); //dimensione dello stack
                        } else perror("[VM] Valore stack per init non compatibile!\n");
                        //printf("%s\n", c_Vvalue);
                    } 
                    else {
                        //se e' INT
                        vm->frames[vm->frame_top].vars[Vindex].value = malloc(sizeof(int));
                        int Vvalue = (int) strtoul(c_Vvalue, NULL, 10); 
                        *(vm->frames[vm->frame_top].vars[Vindex].value) = Vvalue; //la variabile LOCAL prende il valore passato dal utente
                        //printf("%d\n",*(vm->frames[vm->frame_top].vars[Vindex].value));
                    }
                    vm->frames[vm->frame_top].vars[Vindex].is_local = 1; //la variabile LOCAL e' locale

                    Var* V = &vm->frames[vm->frame_top].vars[Vindex];
                    //printf("%d\n", *V->value);
                    stack_push(&vm->frames[vm->frame_top].LocalVariables, V); //mettiamo nello stack LIFO la variabile LOCAL in head
                } else if (strcmp(firstWord, "DELOCAL") == 0){
                    char* Vtype = strtok(NULL, " \t");
                    char* Vname = strtok(NULL, " \t");
                    char * c_Vvalue = strtok(NULL, " \t");
                    int Vvalue = (unsigned int) strtoul(c_Vvalue, NULL, 10);;
                    Var *V = stack_pop(&vm->frames[vm->frame_top].LocalVariables);
                    //printf("%s %s %d =? %d\n", Vtype, Vname, Vvalue, V->value);
                    if(strcmp(Vtype, (V->T == 0 ? "int" : "stack") ) == 0){
                        printf("%d\n",stack_size(&vm->frames[vm->frame_top].LocalVariables));
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
                    //printf("%d\n", V->value);
                } else {
                    printf("Istruzione sconosciuta: %s\n", firstWord);
                }
            } else {
                printf("[VM] Bytecode formattato male!\n");
            }

            *newline = '\n';  // ripristina il carattere
            ptr = newline + 1; // passa alla prossima riga
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
        printf("frame[%d]: \n", i);
        for (int j = 0; j < f->var_count; j++) {
            Var *v = &f->vars[j];
            if (v->value == NULL) continue; // slot libero (variabile gia' deallocata), saltiamo
            printf("\tVar[%d] Type: %s, is_local: %d, value: ", 
                   j, (v->T == 0 ? "INT" : "STACK"), v->is_local);
            
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