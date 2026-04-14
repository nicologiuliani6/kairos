#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_types.h"

VMDebugState *vm_debug_new(void);
void vm_debug_free(VMDebugState *dbg);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int vm_debug_step_back(VMDebugState *dbg);
int vm_debug_continue(VMDebugState *dbg);
void vm_debug_set_breakpoint(VMDebugState *dbg, int line);

static char *compile_kairos_to_bytecode(const char *kairos_file)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "./venv/bin/python -m src.kairos \"%s\" --dump-bytecode >/dev/null 2>/dev/null; cat bytecode.txt",
             kairos_file);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { pclose(fp); return NULL; }
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); pclose(fp); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

int main(void)
{
    char *bytecode = compile_kairos_to_bytecode("examples/prod_cons_sequenziale_cript.kairos");
    if (!bytecode) return 2;

    VMDebugState *dbg = vm_debug_new();
    vm_debug_start(bytecode, dbg);
    vm_debug_set_breakpoint(dbg, 42);

    int line = vm_debug_continue(dbg);
    printf("breakpoint line=%d current=%d history_top=%d\n", line, dbg->current_line, dbg->history_top);
    for (int i = dbg->history_top - 8; i <= dbg->history_top; i++) {
        if (i < 0) continue;
        printf("H[%d] line=%d instr=%s\n", i, dbg->history[i].line, dbg->history[i].instr);
    }

    int s1 = vm_debug_step_back(dbg);
    int s2 = vm_debug_step_back(dbg);
    int s3 = vm_debug_step_back(dbg);
    printf("stepbacks: %d %d %d\n", s1, s2, s3);

    vm_debug_stop(dbg);
    vm_debug_free(dbg);
    free(bytecode);
    return 0;
}
