#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef void VMDebugState;

VMDebugState *vm_debug_new(void);
void vm_debug_free(VMDebugState *dbg);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int vm_debug_step(VMDebugState *dbg);
int vm_debug_step_back(VMDebugState *dbg);
int vm_debug_continue(VMDebugState *dbg);
void vm_debug_set_breakpoint(VMDebugState *dbg, int line);
int vm_debug_dump_json_ext(VMDebugState *dbg, char *out, int outsz);
int vm_debug_vars_json_ext(VMDebugState *dbg, char *out, int outsz);
int vm_debug_get_output_fd(VMDebugState *dbg);

static char *compile_kairos_to_bytecode(const char *kairos_file) {
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

static void print_state(VMDebugState *dbg, const char *label) {
    char st[65536] = {0};
    char vars[65536] = {0};
    vm_debug_dump_json_ext(dbg, st, (int)sizeof(st));
    vm_debug_vars_json_ext(dbg, vars, (int)sizeof(vars));
    printf("[%s] state=%s\n", label, st);
    printf("[%s] vars=%s\n", label, vars);
}

static void drain_dap_pipe_output(int fd, char *acc, size_t accsz) {
    if (fd < 0) return;
    char chunk[4096];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk) - 1);
        if (n > 0) {
            chunk[n] = '\0';
            if (strlen(acc) + (size_t)n + 1 < accsz) strcat(acc, chunk);
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        break;
    }
}

int main(void) {
    const char *src = "tests/test_loop.kairos";
    char *bytecode = compile_kairos_to_bytecode(src);
    if (!bytecode) {
        fprintf(stderr, "ERRORE: impossibile generare bytecode da %s\n", src);
        return 2;
    }

    VMDebugState *dbg = vm_debug_new();
    vm_debug_start(bytecode, dbg);
    int out_fd = vm_debug_get_output_fd(dbg);
    if (out_fd >= 0) {
        int flags = fcntl(out_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(out_fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Avanza con step fino a show(x) per evitare blocchi iniziali su continue. */
    int line = -1;
    int guard = 0;
    while (guard++ < 10000) {
        line = vm_debug_step(dbg);
        if (line < 0) break;
        if (line == 7) break;
    }
    printf("step-until-line7 -> line=%d (guard=%d)\n", line, guard);
    print_state(dbg, "after-continue-1");

    line = vm_debug_step_back(dbg);
    printf("step_back -> line=%d\n", line);
    print_state(dbg, "after-step-back");
    out_fd = vm_debug_get_output_fd(dbg);
    if (out_fd >= 0) {
        int flags = fcntl(out_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(out_fd, F_SETFL, flags | O_NONBLOCK);
    }

    char out[32768] = {0};
    /* Resume fino a fine, come nel caso utente */
    line = vm_debug_continue(dbg);
    drain_dap_pipe_output(out_fd, out, sizeof(out));

    printf("continue-2 -> line=%d\n", line);
    print_state(dbg, "after-continue-2");
    printf("dap-output(pipe):\n%s\n", out);

    char vars2[65536] = {0};
    vm_debug_vars_json_ext(dbg, vars2, (int)sizeof(vars2));
    if (line < 0 && strstr(out, "x: 4") != NULL) {
        printf("RESULT: PASS (output DAP presente fino a fine)\n");
    } else {
        printf("RESULT: FAIL (output DAP assente/incompleto)\n");
    }

    vm_debug_stop(dbg);
    vm_debug_free(dbg);
    free(bytecode);
    return 0;
}
