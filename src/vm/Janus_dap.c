#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "vm_types.h"
#include "vm_debug.h"
#include "vm_panic.h"
#include "Kairos_core.h"

void invert_op_to_line(VM *vm, const char *frame_name, char *buffer,
                       uint start, uint stop);
int vm_check_if_reversibility(const char *buffer);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int vm_debug_step(VMDebugState *dbg);

/* Stato globale del debugger (usato dall'API esterna). */
static VM       *g_debug_vm       = NULL;
VMDebugState    *g_debug_dbg      = NULL;
static char     *g_debug_buf      = NULL;
static char     *g_debug_src_raw  = NULL;
static char     *g_debug_buf_orig = NULL;
static pthread_t g_debug_tid;

static int vm_debug_rebuild_to_history_top(VMDebugState *dbg, int target_top)
{
    if (!dbg || !g_debug_src_raw) return -1;

    int bp_count = dbg->bp_count;
    int bps[DBG_MAX_BREAKPOINTS];
    if (bp_count > DBG_MAX_BREAKPOINTS) bp_count = DBG_MAX_BREAKPOINTS;
    for (int i = 0; i < bp_count; i++) bps[i] = dbg->breakpoints[i];

    char *src = strdup(g_debug_src_raw);
    if (!src) return -1;

    vm_debug_stop(dbg);

    dbg->history_top = -1;
    dbg->first_pause_reached = 0;
    dbg->mode = VM_MODE_STEP;
    dbg->out_len = 0;
    dbg->bp_count = bp_count;
    for (int i = 0; i < bp_count; i++) dbg->breakpoints[i] = bps[i];

    vm_debug_start(src, dbg);
    free(src);

    /* Rebuild interno per step-back/reverse-continue:
       non deve produrre output utente (es. SHOW) in Debug Console. */
    dbg->suppress_output = 1;

    int guard = 0;
    while (dbg->history_top < target_top && guard++ < DBG_MAX_HISTORY * 4) {
        int line = vm_debug_step(dbg);
        if (line < 0) break;
    }
    dbg->suppress_output = 0;
    return (dbg->mode == VM_MODE_DONE) ? -1 : dbg->current_line;
}

static void *debug_exec_thread(void *arg)
{
    FILE *f = fopen("/tmp/kairos-vm.log", "a");
    if (f) { fprintf(f, "debug_exec_thread AVVIATO\n"); fclose(f); }

    (void)arg;
    vm_exec(g_debug_vm, g_debug_buf);
    vm_dump(g_debug_vm);

    if (g_debug_dbg) {
        pthread_mutex_lock(&g_debug_dbg->pause_mtx);
        g_debug_dbg->mode = VM_MODE_DONE;
        pthread_cond_broadcast(&g_debug_dbg->pause_cond);
        pthread_mutex_unlock(&g_debug_dbg->pause_mtx);

        if (!g_debug_dbg->shutting_down) {
            pthread_mutex_lock(&g_debug_dbg->pause_mtx);
            while (g_debug_dbg->mode == VM_MODE_DONE)
                pthread_cond_wait(&g_debug_dbg->pause_cond, &g_debug_dbg->pause_mtx);
            pthread_mutex_unlock(&g_debug_dbg->pause_mtx);
        }

        if (g_debug_dbg->on_pause)
            g_debug_dbg->on_pause(-1, "done", g_debug_dbg->userdata);
    }
    return NULL;
}

VMDebugState *vm_debug_new(void)
{
    VMDebugState *dbg = calloc(1, sizeof(VMDebugState));
    dbg_init(dbg);
    return dbg;
}

void vm_debug_free(VMDebugState *dbg)
{
    if (!dbg) return;
    dbg_destroy(dbg);
    free(dbg);
}

void vm_debug_start(const char *bytecode, VMDebugState *dbg)
{
    if (g_debug_vm) {
        if (g_debug_dbg) dbg_resume(g_debug_dbg, VM_MODE_DONE);
        pthread_join(g_debug_tid, NULL);
        free(g_debug_buf);
        free(g_debug_src_raw);
        vm_free(g_debug_vm);
        free(g_debug_vm);
        g_debug_vm  = NULL;
        g_debug_buf = NULL;
        g_debug_src_raw = NULL;
    }

    g_debug_dbg = dbg;
    g_debug_vm  = calloc(1, sizeof(VM));
    g_debug_vm->dbg = dbg;

    int fds[2];
    if (pipe(fds) == 0) {
        dbg->output_pipe_rd = fds[0];
        dbg->output_pipe_fd = fds[1];
    } else {
        dbg->output_pipe_rd = -1;
        dbg->output_pipe_fd = -1;
    }

    char *normalized = normalize_bytecode_physical_lines(bytecode);
    if (!normalized) {
        vm_debug_panic("[DAP] normalizzazione bytecode fallita\n");
        return;
    }

    size_t blen = strlen(normalized) + 1;
    size_t rlen = strlen(bytecode) + 1;
    g_debug_buf = malloc(blen);
    memcpy(g_debug_buf, normalized, blen);
    g_debug_src_raw = malloc(rlen);
    memcpy(g_debug_src_raw, bytecode, rlen);
    g_debug_buf_orig = malloc(blen);
    memcpy(g_debug_buf_orig, normalized, blen);
    free(normalized);

    if (vm_check_if_reversibility(g_debug_buf) > 0)
        fprintf(stderr, "Warning: il bytecode potrebbe non essere completamente reversibile.\n");

    dbg->mode = VM_MODE_STEP;
    dbg->first_pause_reached = 0;
    dbg->shutting_down = 0;
    dbg->ignore_breakpoint_once_line = -1;

    pthread_create(&g_debug_tid, NULL, debug_exec_thread, NULL);
    pthread_mutex_lock(&dbg->pause_mtx);
    while (!dbg->first_pause_reached && dbg->mode != VM_MODE_DONE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);
    pthread_mutex_unlock(&dbg->pause_mtx);
}

int vm_debug_step(VMDebugState *dbg)
{
    if (!dbg || dbg->mode == VM_MODE_DONE) return -1;
    pthread_mutex_lock(&dbg->pause_mtx);
    dbg->mode = VM_MODE_STEP;
    pthread_cond_broadcast(&dbg->pause_cond);

    while (dbg->mode != VM_MODE_PAUSE && dbg->mode != VM_MODE_DONE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);
    pthread_mutex_unlock(&dbg->pause_mtx);

    return (dbg->mode == VM_MODE_DONE) ? -1 : dbg->current_line;
}

int vm_debug_step_back(VMDebugState *dbg)
{
    if (!dbg || dbg->history_top < 0) return -1;
    int target_top = dbg->history_top - 1;
    return vm_debug_rebuild_to_history_top(dbg, target_top);
}

int vm_debug_continue(VMDebugState *dbg)
{
    if (!dbg || dbg->mode == VM_MODE_DONE) return -1;
    pthread_mutex_lock(&dbg->pause_mtx);
    dbg->mode = VM_MODE_CONTINUE;
    pthread_cond_broadcast(&dbg->pause_cond);

    while (dbg->mode != VM_MODE_PAUSE && dbg->mode != VM_MODE_DONE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);

    int result = (dbg->mode == VM_MODE_DONE) ? -1 : dbg->current_line;
    if (dbg->mode == VM_MODE_DONE) {
        dbg->mode = VM_MODE_IDLE;
        pthread_cond_broadcast(&dbg->pause_cond);
    }

    pthread_mutex_unlock(&dbg->pause_mtx);
    return result;
}

int vm_debug_continue_inverse(VMDebugState *dbg)
{
    if (!dbg || dbg->history_top < 0) return 0;
    int target_top = -1;
    for (int i = dbg->history_top - 1; i >= 0; i--) {
        if (dbg_is_breakpoint(dbg, dbg->history[i].line)) {
            target_top = i;
            break;
        }
    }
    return vm_debug_rebuild_to_history_top(dbg, target_top);
}

int vm_debug_goto_line(VMDebugState *dbg, int target_line)
{
    if (!dbg) return -1;
    _vm_debug_set_breakpoint(dbg, target_line);
    int result = vm_debug_continue(dbg);
    _vm_debug_clear_breakpoint(dbg, target_line);
    return result;
}

int vm_debug_dump_json_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!g_debug_vm || !out) return 0;
    (void)dbg;
    return vm_debug_dump_json(g_debug_vm, out, outsz);
}

int vm_debug_vars_json_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!g_debug_vm || !out || !dbg) return 0;
    return vm_debug_vars_json(g_debug_vm, dbg->current_frame, out, outsz);
}

void vm_debug_stop(VMDebugState *dbg)
{
    if (!dbg) return;
    dbg->shutting_down = 1;
    pthread_mutex_lock(&dbg->pause_mtx);
    if (dbg->mode == VM_MODE_DONE) {
        dbg->mode = VM_MODE_IDLE;
    } else {
        dbg->mode = VM_MODE_DONE;
    }
    pthread_cond_broadcast(&dbg->pause_cond);
    pthread_mutex_unlock(&dbg->pause_mtx);
    pthread_join(g_debug_tid, NULL);

    if (dbg->output_pipe_fd > 0) { close(dbg->output_pipe_fd); dbg->output_pipe_fd = -1; }
    if (dbg->output_pipe_rd > 0) { close(dbg->output_pipe_rd); dbg->output_pipe_rd = -1; }

    free(g_debug_buf);
    g_debug_buf = NULL;
    free(g_debug_src_raw);
    g_debug_src_raw = NULL;
    free(g_debug_buf_orig);
    g_debug_buf_orig = NULL;
    vm_free(g_debug_vm);
    free(g_debug_vm);
    g_debug_vm  = NULL;
    g_debug_dbg = NULL;
}

int vm_debug_get_output_fd(VMDebugState *dbg)
{
    return dbg ? dbg->output_pipe_rd : -1;
}

void vm_debug_set_breakpoint(VMDebugState *dbg, int line)
{
    _vm_debug_set_breakpoint(dbg, line);
}

void vm_debug_clear_breakpoint(VMDebugState *dbg, int line)
{
    _vm_debug_clear_breakpoint(dbg, line);
}

void vm_debug_ignore_breakpoint_once(VMDebugState *dbg, int line)
{
    if (!dbg) return;
    dbg->ignore_breakpoint_once_line = line;
}

void vm_debug_clear_all_breakpoints(VMDebugState *dbg)
{
    if (dbg) dbg->bp_count = 0;
}

int vm_debug_output_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!dbg || dbg->out_len == 0) return 0;
    int n = dbg->out_len < outsz - 1 ? dbg->out_len : outsz - 1;
    memcpy(out, dbg->out_buf, n);
    out[n] = '\0';
    dbg->out_len = 0;
    return n;
}

int vm_debug_error_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!dbg || dbg->last_error[0] == '\0') return 0;
    int n = (int)strlen(dbg->last_error);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, dbg->last_error, n);
    out[n] = '\0';
    return n;
}
