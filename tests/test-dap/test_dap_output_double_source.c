#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
/*
 * Test mirato: dimostra che in DAP_MODE l'output dei SHOW viene prodotto su due sorgenti:
 * - pipe realtime (vm_debug_get_output_fd)
 * - buffer interno (vm_debug_output_ext)
 *
 * Se un client legge entrambe e le stampa, vedrà output duplicato.
 */

typedef struct VMDebugState VMDebugState;
VMDebugState *vm_debug_new(void);
void vm_debug_free(VMDebugState *dbg);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int  vm_debug_continue(VMDebugState *dbg);
int  vm_debug_get_output_fd(VMDebugState *dbg);
int  vm_debug_output_ext(VMDebugState *dbg, char *out, int outsz);

static char *compile_kairos_to_bytecode(const char *kairos_file)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "./venv/bin/python -m src.kairos \"%s\" --dump-bytecode >/dev/null 2>/dev/null; cat bytecode.txt",
             kairos_file);
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 1 << 20;
    size_t len = 0;
    char *buf = (char *)calloc(1, cap);
    if (!buf) { pclose(p); return NULL; }
    for (;;) {
        if (len + 4096 + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); pclose(p); return NULL; }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, 4096, p);
        len += n;
        if (n == 0) break;
    }
    int st = pclose(p);
    if (st != 0 || len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

static void set_nonblock(int fd)
{
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static size_t drain_fd_into(int fd, char *acc, size_t accsz)
{
    if (fd < 0 || !acc || accsz == 0) return 0;
    size_t pos = strlen(acc);
    for (;;) {
        if (pos + 65536 + 1 >= accsz) break;
        char tmp[65536];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            memcpy(acc + pos, tmp, (size_t)n);
            pos += (size_t)n;
            acc[pos] = '\0';
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        break;
    }
    return pos;
}

static size_t drain_outbuf_into(VMDebugState *dbg, char *acc, size_t accsz)
{
    if (!dbg || !acc || accsz == 0) return 0;
    size_t pos = strlen(acc);
    for (;;) {
        if (pos + 65536 + 1 >= accsz) break;
        char tmp[65536];
        int n = vm_debug_output_ext(dbg, tmp, (int)sizeof(tmp));
        if (n <= 0) break;
        if (pos + (size_t)n + 1 >= accsz) break;
        memcpy(acc + pos, tmp, (size_t)n);
        pos += (size_t)n;
        acc[pos] = '\0';
    }
    return pos;
}

int main(void)
{
    setbuf(stdout, NULL);

    const char *src = "tests/test_loop_condition.kairos";
    char *bytecode = compile_kairos_to_bytecode(src);
    if (!bytecode) {
        fprintf(stderr, "ERRORE: bytecode non generato da %s\n", src);
        return 2;
    }

    VMDebugState *dbg = vm_debug_new();
    vm_debug_start(bytecode, dbg);

    int fd = vm_debug_get_output_fd(dbg);
    set_nonblock(fd);

    /* Run fino a fine programma. */
    int line = 0;
    while (line >= 0) {
        line = vm_debug_continue(dbg);
    }

    /* Leggi entrambe le sorgenti come farebbe un client "buggato". */
    size_t cap = 4u * 1024u * 1024u;
    char *pipe_out = (char *)calloc(1, cap);
    char *buf_out  = (char *)calloc(1, cap);
    if (!pipe_out || !buf_out) {
        fprintf(stderr, "OOM\n");
        vm_debug_stop(dbg);
        vm_debug_free(dbg);
        free(bytecode);
        free(pipe_out);
        free(buf_out);
        return 3;
    }

    /* Best-effort: drena più volte per lasciare arrivare tutti i chunk. */
    for (int i = 0; i < 10; i++) {
        drain_fd_into(fd, pipe_out, cap);
        usleep(20 * 1000);
    }
    drain_outbuf_into(dbg, buf_out, cap);

    printf("=== PIPE OUTPUT (vm_debug_get_output_fd) ===\n%s\n", pipe_out);
    printf("=== OUT_BUF OUTPUT (vm_debug_output_ext) ===\n%s\n", buf_out);

    int has_xy_pipe = (strstr(pipe_out, "x: 1") != NULL) && (strstr(pipe_out, "y: 0") != NULL);
    int has_xy_buf  = (strstr(buf_out,  "x: 1") != NULL) && (strstr(buf_out,  "y: 0") != NULL);
    printf("SUMMARY: pipe_has_xy=%d outbuf_has_xy=%d\n", has_xy_pipe, has_xy_buf);
    printf("NOTE: se un client stampa entrambe le sezioni, vedrai x/y duplicati.\n");

    vm_debug_stop(dbg);
    vm_debug_free(dbg);
    free(bytecode);
    free(pipe_out);
    free(buf_out);
    return 0;
}

