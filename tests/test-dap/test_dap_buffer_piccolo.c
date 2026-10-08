/*
 * Il JSON delle variabili non deve uscire troncato quando il buffer del client
 * e' piccolo: vm_debug_vars_json_ext si comporta come snprintf e restituisce la
 * lunghezza completa, cosi' il client riprova con un buffer abbastanza grande.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void VMDebugState;
VMDebugState *vm_debug_new(void);
void vm_debug_free(VMDebugState *dbg);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int  vm_debug_step(VMDebugState *dbg);
int  vm_debug_vars_json_ext(VMDebugState *dbg, char *out, int outsz);

static char *compila(const char *file)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "./venv/bin/python -m src.kairos \"%s\" --dump-bytecode >/dev/null 2>/dev/null; cat bytecode.txt", file);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 1 << 16, n = 0; char *b = malloc(cap);
    int c; while ((c = fgetc(fp)) != EOF) { if (n + 1 >= cap) b = realloc(b, cap *= 2); b[n++] = (char)c; }
    b[n] = '\0'; pclose(fp);
    return n ? b : NULL;
}

int main(void)
{
    /* uno stack da 2000 elementi: il JSON delle variabili supera di molto 64 byte */
    FILE *f = fopen("/tmp/dap_buffer_piccolo.kairos", "w");
    fprintf(f, "procedure main()\n    stack s\n    local int i = 0\n    local int v = 0\n"
               "    from i == 0 do\n        v += 7\n        push(v, s)\n        i += 1\n    loop until i == 2000\n"
               "    delocal int v = 0\n    show(i)\n    delocal int i = 2000\n");
    fclose(f);
    char *bc = compila("/tmp/dap_buffer_piccolo.kairos");
    if (!bc) { printf("RESULT: FAIL (bytecode)\n"); return 2; }

    VMDebugState *dbg = vm_debug_new();
    vm_debug_start(bc, dbg);
    int line = 0;
    for (int g = 0; g < 200000 && line >= 0 && line != 11; g++) line = vm_debug_step(dbg);

    char piccolo[64];
    int serve = vm_debug_vars_json_ext(dbg, piccolo, (int)sizeof piccolo);
    int solo_misura = vm_debug_vars_json_ext(dbg, NULL, 0);
    char *grande = malloc((size_t)serve + 1);
    int scritti = vm_debug_vars_json_ext(dbg, grande, serve + 1);
    int len = (int)strlen(grande);

    int ok = serve > (int)sizeof piccolo && solo_misura == serve && scritti == serve &&
             len == serve && grande[len - 1] == ']' && strstr(grande, "\"s\"") != NULL;
    printf("serve=%d misura=%d scritti=%d strlen=%d fine='%c'\n", serve, solo_misura, scritti, len, grande[len - 1]);
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    vm_debug_stop(dbg); vm_debug_free(dbg); free(grande); free(bc);
    return ok ? 0 : 1;
}
