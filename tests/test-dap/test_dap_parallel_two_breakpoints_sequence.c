#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_types.h"

VMDebugState *vm_debug_new(void);
void vm_debug_free(VMDebugState *dbg);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int vm_debug_continue(VMDebugState *dbg);
int vm_debug_step_back(VMDebugState *dbg);
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
    setbuf(stdout, NULL);
    char *bytecode = compile_kairos_to_bytecode("examples/prod_cons_parallelo_cript.kairos");
    if (!bytecode) return 2;

    VMDebugState *dbg = vm_debug_new();
    vm_debug_start(bytecode, dbg);
    vm_debug_set_breakpoint(dbg, 34); /* call producer */
    vm_debug_set_breakpoint(dbg, 36); /* call consumer */

    int l1 = vm_debug_continue(dbg);   /* expect 34 */
    int sb = vm_debug_step_back(dbg);  /* from 34 */
    int l2 = vm_debug_continue(dbg);   /* expect 34 again */
    int l3 = vm_debug_continue(dbg);   /* expect 36 (not ignored) */

    printf("sequence lines: l1=%d sb=%d l2=%d l3=%d current=%d frame=%s\n",
           l1, sb, l2, l3, dbg->current_line, dbg->current_frame);

    vm_debug_stop(dbg);
    vm_debug_free(dbg);
    free(bytecode);
    return 0;
}
