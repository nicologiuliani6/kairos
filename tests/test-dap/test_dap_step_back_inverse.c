/* Il passo indietro del debugger esegue l'inversa dell'istruzione precedente.
 *
 * Si avanza a passi su tests/fixtures/dap_step_back.kairos annotando lo stato
 * (tutti i frame, in JSON) dopo ogni passo; poi si torna indietro un passo alla
 * volta. Ogni stato raggiunto all'indietro dev'essere uno degli stati gia'
 * visti in avanti, e si torna esattamente allo stato iniziale. Poi si riparte
 * in avanti: il programma deve arrivare in fondo senza errori. Lo stesso su
 * tests/fixtures/dap_step_back_par.kairos, dove si torna indietro anche
 * attraverso un par completato e uno show. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_types.h"

VMDebugState *vm_debug_new(void);
void vm_debug_free(VMDebugState *dbg);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int  vm_debug_step(VMDebugState *dbg);
int  vm_debug_step_back(VMDebugState *dbg);
int  vm_debug_continue(VMDebugState *dbg);
int  vm_debug_dump_json_ext(VMDebugState *dbg, char *out, int outsz);
int  vm_debug_error_ext(VMDebugState *dbg, char *out, int outsz);

static char *compile(const char *f)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "./venv/bin/python -m src.kairos \"%s\" --dump-bytecode >/dev/null 2>&1; cat bytecode.txt; rm -f bytecode.txt", f);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *b = malloc(cap);
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) b = realloc(b, cap *= 2);
        b[len++] = (char)c;
    }
    b[len] = '\0';
    pclose(fp);
    return len ? b : NULL;
}

static char *state(VMDebugState *d)
{
    int n = vm_debug_dump_json_ext(d, NULL, 0) + 1;
    char *s = calloc((size_t)n + 1, 1);
    vm_debug_dump_json_ext(d, s, n);
    /* la riga corrente non conta: confrontiamo lo store */
    char *v = strstr(s, "\"frames\"");
    char *out = strdup(v ? v : s);
    free(s);
    return out;
}

static int run(const char *fixture)
{
    printf("== %s\n", fixture);
    char *bc = compile(fixture);
    if (!bc) { fprintf(stderr, "compilation failed\n"); return 0; }
    VMDebugState *d = vm_debug_new();
    vm_debug_start(bc, d);

    enum { MAXS = 400 };
    char *seen[MAXS]; int ns = 0;
    seen[ns++] = state(d);
    char *initial = seen[0];
    int steps = 0;
    for (; steps < 60; steps++) {
        int l = vm_debug_step(d);
        if (l < 0) break;
        seen[ns++] = state(d);
    }
    printf("forward: %d steps\n", steps);

    int back = 0, bad = 0;
    char err[256];
    for (;;) {
        int lb = vm_debug_step_back(d);
        err[0] = '\0';
        vm_debug_error_ext(d, err, sizeof err);
        if (err[0]) { printf("stop: %s\n", err); break; }
        back++;
        char *st = state(d);
        int ok = 0;
        for (int i = 0; i < ns; i++) if (!strcmp(st, seen[i])) { ok = 1; break; }
        if (!ok) { bad++; printf("state never seen forwards after %d steps back:\n%s\n", back, st); }
        free(st);
        if (back > 200) break;
    }
    char *fin = state(d);
    int back_to_start = !strcmp(fin, initial);
    printf("backward: %d steps, unknown states: %d, initial state: %s\n",
           back, bad, back_to_start ? "yes" : "no");

    int line = vm_debug_continue(d);   /* di nuovo in avanti fino in fondo */
    err[0] = '\0';
    vm_debug_error_ext(d, err, sizeof err);
    printf("forward to the end: line=%d err='%s'\n", line, err);

    int pass = (bad == 0 && back_to_start && back > 0 && line < 0 && !err[0]);
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    vm_debug_stop(d);
    vm_debug_free(d);
    return pass;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int ok = run("tests/fixtures/dap_step_back.kairos");
    ok &= run("tests/fixtures/dap_step_back_par.kairos");
    return ok ? 0 : 1;
}
