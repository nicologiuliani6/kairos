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
int vm_debug_continue_inverse(VMDebugState *dbg);
void vm_debug_set_breakpoint(VMDebugState *dbg, int line);

static char *compile_kairos_to_bytecode(const char *kairos_file)
{
    char cmd[1024];
    snprintf(
        cmd,
        sizeof(cmd),
        "./venv/bin/python -m src.kairos \"%s\" --dump-bytecode >/dev/null 2>/dev/null; cat bytecode.txt",
        kairos_file
    );

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        pclose(fp);
        return NULL;
    }

    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) {
                free(buf);
                pclose(fp);
                return NULL;
            }
            buf = tmp;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    pclose(fp);

    if (len == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void print_history_tail(VMDebugState *dbg, int n)
{
    int from = dbg->history_top - n + 1;
    if (from < 0) from = 0;
    printf("history_top=%d current_line=%d\n", dbg->history_top, dbg->current_line);
    for (int i = from; i <= dbg->history_top; i++) {
        printf("  [%d] line=%d instr=%s\n", i, dbg->history[i].line, dbg->history[i].instr);
    }
}

int main(void)
{
    const char *src = "examples/prod_cons_sequenziale_cript.kairos";
    char *bytecode = compile_kairos_to_bytecode(src);
    if (!bytecode) {
        fprintf(stderr, "ERRORE: impossibile generare bytecode da %s\n", src);
        return 2;
    }

    {
        VMDebugState *dbg = vm_debug_new();
        vm_debug_start(bytecode, dbg);
        vm_debug_set_breakpoint(dbg, 40);

        int line_bp = vm_debug_continue(dbg);
        printf("[A] breakpoint hit line=%d\n", line_bp);
        print_history_tail(dbg, 8);

        int line_sb1 = vm_debug_step_back(dbg);
        printf("[A] step_back #1 -> line=%d\n", line_sb1);

        int line_sb2 = vm_debug_step_back(dbg);
        printf("[A] step_back #2 -> line=%d\n", line_sb2);

        int line_sb3 = vm_debug_step_back(dbg);
        printf("[A] step_back #3 -> line=%d\n", line_sb3);

        int line_rev = vm_debug_continue_inverse(dbg);
        printf("[A] revert (continue_inverse) -> line=%d\n", line_rev);

        vm_debug_stop(dbg);
        vm_debug_free(dbg);
    }

    {
        VMDebugState *dbg = vm_debug_new();
        vm_debug_start(bytecode, dbg);
        vm_debug_set_breakpoint(dbg, 40);
        vm_debug_set_breakpoint(dbg, 47);

        int line_40 = vm_debug_continue(dbg);
        printf("[B] first breakpoint -> line=%d\n", line_40);
        int line_47 = vm_debug_continue(dbg);
        printf("[B] second breakpoint -> line=%d\n", line_47);
        int line_rev = vm_debug_continue_inverse(dbg);
        printf("[B] revert to previous breakpoint -> line=%d\n", line_rev);
        print_history_tail(dbg, 12);

        int line_sb1 = vm_debug_step_back(dbg);
        printf("[B] step_back #1 -> line=%d\n", line_sb1);
        int line_sb2 = vm_debug_step_back(dbg);
        printf("[B] step_back #2 -> line=%d\n", line_sb2);
        int line_sb3 = vm_debug_step_back(dbg);
        printf("[B] step_back #3 -> line=%d\n", line_sb3);

        vm_debug_stop(dbg);
        vm_debug_free(dbg);
    }

    {
        VMDebugState *dbg = vm_debug_new();
        vm_debug_start(bytecode, dbg);
        vm_debug_set_breakpoint(dbg, 42);

        int line_42 = vm_debug_continue(dbg);
        printf("[C] breakpoint at show -> line=%d\n", line_42);
        print_history_tail(dbg, 16);

        int line_sb1 = vm_debug_step_back(dbg);
        printf("[C] step_back #1 -> line=%d\n", line_sb1);
        int line_sb2 = vm_debug_step_back(dbg);
        printf("[C] step_back #2 -> line=%d\n", line_sb2);
        int line_sb3 = vm_debug_step_back(dbg);
        printf("[C] step_back #3 -> line=%d\n", line_sb3);

        vm_debug_stop(dbg);
        vm_debug_free(dbg);
    }

    free(bytecode);
    return 0;
}
