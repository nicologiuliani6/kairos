/*
 * test_debug.c — driver minimale per testare le API di debug di libvm.so
 *
 * Compilazione (dalla root del progetto):
 *   gcc -o test_debug test_debug.c -L./build -lvm -Wl,-rpath,./build -pthread
 *
 * Uso:
 *   ./test_debug <file.janus>          esegui step-by-step fino alla fine
 *   ./test_debug <file.janus> <riga>   metti breakpoint sulla riga e continua
 *
 * Il programma .janus viene compilato da janus.py e il bytecode viene
 * passato direttamente alla VM — nessun file intermedio.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Dichiarazioni delle API esportate da libvm.so ── */
typedef void VMDebugState;   /* opaco dal lato C del test */

VMDebugState *vm_debug_new(void);
void          vm_debug_free(VMDebugState *dbg);
void          vm_debug_start(const char *bytecode, VMDebugState *dbg);
void          vm_debug_stop(VMDebugState *dbg);
int           vm_debug_step(VMDebugState *dbg);
int           vm_debug_step_back(VMDebugState *dbg);
int           vm_debug_continue(VMDebugState *dbg);
int           vm_debug_goto_line(VMDebugState *dbg, int line);
void          vm_debug_set_breakpoint(VMDebugState *dbg, int line);
void          vm_debug_clear_breakpoint(VMDebugState *dbg, int line);
int           vm_debug_dump_json_ext(VMDebugState *dbg, char *out, int outsz);
int           vm_debug_vars_json_ext(VMDebugState *dbg, char *out, int outsz);

/* ── Callback chiamata ogni volta che la VM si ferma ── */
static void on_pause(int line, const char *frame, void *userdata)
{
    (void)userdata;
    if (line < 0)
        printf("\n[DEBUG] Esecuzione terminata.\n");
    else
        printf("\n[DEBUG] Fermato alla riga %d (frame: %s)\n", line, frame);
}

/* ── Legge il bytecode da janus.py su stdout ── */
static char *compile_janus(const char *janus_file)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
         "LD_PRELOAD=/usr/lib/gcc/x86_64-linux-gnu/14/libasan.so "
         "./venv/bin/python -m src.janus \"%s\" --dump-bytecode 2>/dev/null"
         " ; cat bytecode.txt",
         janus_file);

    FILE *fp = popen(cmd, "r");
    if (!fp) { perror("popen"); return NULL; }

    size_t cap = 64 * 1024, len = 0;
    char  *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    pclose(fp);

    if (len == 0) { fprintf(stderr, "Errore: bytecode vuoto.\n"); free(buf); return NULL; }
    return buf;
}

/* ── Stampa le variabili del frame corrente ── */
static void print_vars(VMDebugState *dbg)
{
    char buf[65536];
    int n = vm_debug_vars_json_ext(dbg, buf, sizeof(buf));
    if (n <= 2) { printf("  (nessuna variabile)\n"); return; }

    /* Stampa JSON grezzo — in un debugger reale verrebbe parsato */
    printf("  variabili: %s\n", buf);
}

/* ================================================================
 *  Modalità 1: step-by-step fino alla fine
 * ================================================================ */
static void run_step_mode(VMDebugState *dbg, const char *bytecode)
{
    printf("[TEST] Modalità: step-by-step\n");
    printf("       Premi INVIO per ogni step, 'q' per uscire, 'b' per step-back.\n\n");

    vm_debug_start(bytecode, dbg);

    int line;
    char input[16];
    while (1) {
        print_vars(dbg);
        printf("  [s]tep / [b]ack / [c]ontinue / [q]uit > ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;
        char cmd = input[0];

        if (cmd == 'q') { vm_debug_stop(dbg); break; }
        else if (cmd == 'b') {
            line = vm_debug_step_back(dbg);
            if (line < 0) printf("  (inizio della history)\n");
            else          printf("  <- step-back alla riga %d\n", line);
        }
        else if (cmd == 'c') {
            line = vm_debug_continue(dbg);
            if (line < 0) { printf("  Esecuzione terminata.\n"); break; }
        }
        else {
            /* default: step */
            line = vm_debug_step(dbg);
            if (line < 0) { printf("  Esecuzione terminata.\n"); break; }
        }
    }
}

/* ================================================================
 *  Modalità 2: breakpoint su riga specificata
 * ================================================================ */
static void run_breakpoint_mode(VMDebugState *dbg, const char *bytecode, int bp_line)
{
    printf("[TEST] Modalità: breakpoint alla riga %d\n\n", bp_line);

    vm_debug_set_breakpoint(dbg, bp_line);
    vm_debug_start(bytecode, dbg);

    /* Aspetta il primo breakpoint */
    int line = vm_debug_continue(dbg);
    if (line < 0) { printf("Breakpoint non raggiunto (programma terminato).\n"); return; }

    printf("Breakpoint raggiunto alla riga %d\n", line);
    print_vars(dbg);

    /* Dump completo dello stato */
    char json[65536];
    int  n = vm_debug_dump_json_ext(dbg, json, sizeof(json));
    if (n > 0) {
        printf("\nDump completo VM (%d byte):\n%s\n", n, json);
    }

    /* Step avanti di 5 istruzioni */
    printf("\nAvanzamento di 5 step:\n");
    for (int i = 0; i < 5; i++) {
        line = vm_debug_step(dbg);
        if (line < 0) { printf("  Programma terminato dopo %d step.\n", i + 1); return; }
        printf("  step %d → riga %d\n", i + 1, line);
        print_vars(dbg);
    }

    /* Continua fino alla fine */
    printf("\nContinuo fino alla fine...\n");
    vm_debug_continue(dbg);
    printf("Fatto.\n");
}

/* ================================================================
 *  main
 * ================================================================ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file.janus> [riga_breakpoint]\n", argv[0]);
        return 1;
    }

    const char *janus_file = argv[1];
    int         bp_line    = (argc >= 3) ? atoi(argv[2]) : 0;

    printf("[TEST] Compilazione: %s\n", janus_file);
    char *bytecode = compile_janus(janus_file);
    if (!bytecode) return 1;
    printf("[TEST] Bytecode: %zu byte\n\n", strlen(bytecode));

    VMDebugState *dbg = vm_debug_new();

    /* Installa callback on_pause */
    /* (cast necessario perché VMDebugState è opaco qui;
       nel progetto reale si usa la struct completa) */
    typedef struct {
        int mode, bp[256], bp_count, cur_line;
        char cur_frame[100];
        /* ... altri campi ... */
        void (*on_pause)(int, const char *, void *);
        void *userdata;
    } DBGPartial;
    ((DBGPartial *)dbg)->on_pause = on_pause;
    ((DBGPartial *)dbg)->userdata = NULL;

    if (bp_line > 0)
        run_breakpoint_mode(dbg, bytecode, bp_line);
    else
        run_step_mode(dbg, bytecode);

    vm_debug_free(dbg);
    free(bytecode);
    return 0;
}