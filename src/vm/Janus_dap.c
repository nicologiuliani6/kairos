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

/* Stato globale del debugger (usato dall'API esterna). */
static VM       *g_debug_vm       = NULL;
VMDebugState    *g_debug_dbg      = NULL;
static char     *g_debug_buf      = NULL;
static char     *g_debug_buf_orig = NULL;
static pthread_t g_debug_tid;

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

        pthread_mutex_lock(&g_debug_dbg->pause_mtx);
        while (g_debug_dbg->mode == VM_MODE_DONE)
            pthread_cond_wait(&g_debug_dbg->pause_cond, &g_debug_dbg->pause_mtx);
        pthread_mutex_unlock(&g_debug_dbg->pause_mtx);

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
        vm_free(g_debug_vm);
        free(g_debug_vm);
        g_debug_vm  = NULL;
        g_debug_buf = NULL;
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

    size_t blen = strlen(bytecode) + 1;
    g_debug_buf = malloc(blen);
    memcpy(g_debug_buf, bytecode, blen);
    g_debug_buf_orig = malloc(blen);
    memcpy(g_debug_buf_orig, bytecode, blen);

    if (vm_check_if_reversibility(g_debug_buf) > 0)
        fprintf(stderr, "Warning: il bytecode potrebbe non essere completamente reversibile.\n");

    if (dbg->mode == VM_MODE_RUN)
        dbg->mode = VM_MODE_STEP;

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

    ExecRecord *rec = dbg_pop_history(dbg);
    if (!rec) return -1;
    if (g_debug_vm)
        invert_op_to_line(g_debug_vm, rec->frame, g_debug_buf_orig, rec->line, rec->line + 1);

    int prev_line = (dbg->history_top >= 0) ? dbg->history[dbg->history_top].line : 0;
    dbg->current_line = prev_line;
    if (dbg->history_top >= 0)
        strncpy(dbg->current_frame, dbg->history[dbg->history_top].frame, VAR_NAME_LENGTH - 1);

    if (dbg->on_pause)
        dbg->on_pause(prev_line, dbg->current_frame, dbg->userdata);
    return prev_line;
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
    if (!dbg || dbg->history_top < 0) return -1;
    while (dbg->history_top >= 0) {
        ExecRecord *rec = &dbg->history[dbg->history_top];
        if (dbg_is_breakpoint(dbg, rec->line)) {
            dbg->current_line = rec->line;
            strncpy(dbg->current_frame, rec->frame, VAR_NAME_LENGTH - 1);
            dbg->history_top--;
            if (dbg->on_pause)
                dbg->on_pause(rec->line, rec->frame, dbg->userdata);
            return rec->line;
        }
        dbg->history_top--;
    }
    if (dbg->on_pause)
        dbg->on_pause(0, "start", dbg->userdata);
    return 0;
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
    dbg_resume(dbg, VM_MODE_DONE);
    pthread_join(g_debug_tid, NULL);

    if (dbg->output_pipe_fd > 0) { close(dbg->output_pipe_fd); dbg->output_pipe_fd = -1; }
    if (dbg->output_pipe_rd > 0) { close(dbg->output_pipe_rd); dbg->output_pipe_rd = -1; }

    free(g_debug_buf);
    g_debug_buf = NULL;
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
