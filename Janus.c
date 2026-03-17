#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "char_id_map.h" //per la ricerca del frame in base al nome della procedura
CharIdMap FrameIndexer;


typedef enum {
    TYPE_INT,
    TYPE_STACK
} ValueType;

#define VAR_NAME_LENGTH 100
typedef struct {
    ValueType T;
    int  value;
    int  is_local;   // 1 = local/delocal, 0 = decl normale
} Var;

#define MAX_VARS 100
typedef struct {
    CharIdMap VarIndexer;
    Var  vars[MAX_VARS];
    int  var_count;
} Frame;

#define MAX_FRAMES 10
typedef struct {
    Frame frames[MAX_FRAMES];
    int   frame_top;   // indice del frame corrente (-1 = vuoto)
} VM;

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
                    char_id_map_init(&vm->frames[vm->frame_top].VarIndexer);
                } else if (strcmp(firstWord, "DECL") == 0) {
                    //segniamo la crezione di una nuova variabile
                    char* type = strtok(NULL, " \t");
                    //printf("%s ", type);
                    if (strcmp(type, "int") == 0){
                        vm->frames[vm->frame_top].vars[++vm->frames[vm->frame_top].var_count].T = TYPE_INT;
                    } else if (strcmp(type, "stack") == 0){
                        vm->frames[vm->frame_top].vars[++vm->frames[vm->frame_top].var_count].T = TYPE_STACK;
                    } else perror("[VM] type variabile non esistente\n");
                    char* Vname = strtok(NULL, " \t");
                    //printf("%s\n", name_var);
                    uint Vindex = char_id_map_get(&vm->frames[vm->frame_top].VarIndexer, *Vname);
                    printf("Frame %d, Var: %s, IndexFrame: %d\n", vm->frame_top, Vname, Vindex);
                    uint Vvalue = 0;
                    vm->frames[vm->frame_top].vars[Vindex].value = Vvalue; //la variabile inizializzata di DECL è 0 per default
                    vm->frames[vm->frame_top].vars[Vindex].is_local = 0; //la variabile DECL non è definita locale
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
            printf("\t Var.[%d] = %d\n", j, f->vars[j].value);
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
    size_t length = sizeof(ast);
    vm_exec(&vm, ast);
    vm_dump(&vm);
    return 0;
}
