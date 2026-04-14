#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "vm_types.h"

VMDebugState *vm_debug_new(void);
void vm_debug_free(VMDebugState *dbg);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int vm_debug_continue(VMDebugState *dbg);
int vm_debug_step_back(VMDebugState *dbg);
void vm_debug_set_breakpoint(VMDebugState *dbg, int line);
int vm_debug_get_output_fd(VMDebugState *dbg);

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

static void drain_fd(int fd, char *acc, size_t accsz)
{
    if (fd < 0 || accsz < 2) return;
    size_t len = strnlen(acc, accsz - 1);
    char chunk[4096];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            size_t room = (len < accsz - 1) ? (accsz - 1 - len) : 0;
            if (room > 0) {
                size_t cpy = (size_t)n < room ? (size_t)n : room;
                memcpy(acc + len, chunk, cpy);
                len += cpy;
                acc[len] = '\0';
            }
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        break;
    }
}

int main(void)
{
    setbuf(stdout, NULL);
    char *bytecode = compile_kairos_to_bytecode("examples/prod_cons_sequenziale_cript.kairos");
    if (!bytecode) return 2;

    VMDebugState *dbg = vm_debug_new();
    vm_debug_start(bytecode, dbg);
    vm_debug_set_breakpoint(dbg, 42);

    int fd = vm_debug_get_output_fd(dbg);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    char out[1 << 15] = {0};
    int l1 = vm_debug_continue(dbg);   /* hit 42 */
    int l2 = vm_debug_step_back(dbg);  /* go back */
    int l3 = vm_debug_continue(dbg);   /* should hit 42 again */
    int l4 = vm_debug_continue(dbg);   /* should run shows then finish */
    drain_fd(fd, out, sizeof(out));

    printf("lines: %d %d %d %d\n", l1, l2, l3, l4);
    printf("has_result=%d has_buffer=%d\n",
           strstr(out, "result: [5, 4, 3, 2, 1]") != NULL,
           strstr(out, "buffer: []") != NULL);
    printf("out:\n%s\n", out);

    vm_debug_stop(dbg);
    vm_debug_free(dbg);
    free(bytecode);
    return 0;
}
