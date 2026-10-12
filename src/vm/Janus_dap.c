#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "vm_types.h"
#include "vm_debug.h"
#include "vm_panic.h"
#include "Kairos_core.h"

void invert_op_to_line(VM *vm, const char *frame_name, char *buffer,
                       uint start, uint stop, int honor_if_line_skip);
void vm_debug_start(const char *bytecode, VMDebugState *dbg);
void vm_debug_stop(VMDebugState *dbg);
int vm_debug_step(VMDebugState *dbg);
int vm_debug_continue(VMDebugState *dbg);

/* Stato globale del debugger (usato dall'API esterna). */
static VM       *g_debug_vm       = NULL;
VMDebugState    *g_debug_dbg      = NULL;
static char     *g_debug_buf      = NULL;
static pthread_t g_debug_tid;

static void *debug_exec_thread(void *arg)
{
    FILE *f = fopen("/tmp/kairos-vm.log", "a");
    if (f) { fprintf(f, "debug_exec_thread started\n"); fclose(f); }

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
            /* Si esce anche allo spegnimento: vm_debug_stop rimette DONE e
               questo thread, se non si e' ancora risvegliato, lo rivedrebbe. */
            while (g_debug_dbg->mode == VM_MODE_DONE && !g_debug_dbg->shutting_down)
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
        vm_free(g_debug_vm);
        free(g_debug_vm);
        g_debug_vm  = NULL;
        g_debug_buf = NULL;
    }

    g_debug_dbg = dbg;
    g_debug_vm  = calloc(1, sizeof(VM));
    g_debug_vm->dbg = dbg;

    if (!(dbg->output_pipe_rd > 0 && dbg->output_pipe_fd > 0)) {
        int fds[2];
        if (pipe(fds) == 0) {
            dbg->output_pipe_rd = fds[0];
            dbg->output_pipe_fd = fds[1];
            (void)fcntl(fds[1], F_SETFL, O_NONBLOCK);
            /* Pipe kernel default ~64 KiB: con migliaia di SHOW si riempie, write()
             * non bloccanti falliscono e l'output resta solo in out_buf fino alla pausa.
             * Su Linux alziamo il buffer (es. loop 10k × ~8 B ≈ 80 KiB). */
#if defined(__linux__) && defined(F_SETPIPE_SZ)
            (void)fcntl(fds[1], F_SETPIPE_SZ, 1024 * 1024);
#endif
        } else {
            dbg->output_pipe_rd = -1;
            dbg->output_pipe_fd = -1;
        }
    }

    char *normalized = normalize_bytecode_physical_lines(bytecode);
    if (!normalized) {
        vm_debug_panic("[DAP] normalizzazione bytecode failed\n");
        return;
    }

    size_t blen = strlen(normalized) + 1;
    g_debug_buf = malloc(blen);
    memcpy(g_debug_buf, normalized, blen);
    free(normalized);

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
    dbg_set_error(dbg, NULL);
    pthread_mutex_lock(&dbg->pause_mtx);
    dbg->mode = VM_MODE_STEP;
    pthread_cond_broadcast(&dbg->pause_cond);

    while (dbg->mode != VM_MODE_PAUSE && dbg->mode != VM_MODE_DONE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);
    pthread_mutex_unlock(&dbg->pause_mtx);

    return (dbg->mode == VM_MODE_DONE) ? -1 : dbg->current_line;
}

/* Passo indietro: lo esegue l'interprete, che e' fermo su un'istruzione.
 * Gli si chiede STEP_BACK; esegue l'inversa dell'istruzione precedente del
 * frame, riporta li' il puntatore e si ferma di nuovo. Se il passo non e'
 * possibile resta dov'e' e il motivo si legge con vm_debug_error_ext. */
int vm_debug_step_back(VMDebugState *dbg)
{
    if (!dbg || dbg->mode == VM_MODE_DONE) return -1;
    pthread_mutex_lock(&dbg->pause_mtx);
    if (dbg->mode != VM_MODE_PAUSE) {
        pthread_mutex_unlock(&dbg->pause_mtx);
        return dbg->current_line;
    }
    dbg->ignore_breakpoint_once_line = -1;
    dbg_set_error(dbg, NULL);
    dbg->mode = VM_MODE_STEP_BACK;
    pthread_cond_broadcast(&dbg->pause_cond);
    while (dbg->mode != VM_MODE_PAUSE && dbg->mode != VM_MODE_DONE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);
    int line = (dbg->mode == VM_MODE_DONE) ? -1 : dbg->current_line;
    pthread_mutex_unlock(&dbg->pause_mtx);
    return line;
}

int vm_debug_continue(VMDebugState *dbg)
{
    if (!dbg || dbg->mode == VM_MODE_DONE) return -1;
    dbg_set_error(dbg, NULL);
    pthread_mutex_lock(&dbg->pause_mtx);

    /* Se siamo fermi su una linea che e` anche breakpoint (es. dopo revert),
       ignora una sola volta quel breakpoint per eseguire davvero l'istruzione
       corrente invece di rientrare subito in pausa sulla stessa riga. */
    if (dbg->mode == VM_MODE_PAUSE && dbg_is_breakpoint(dbg, dbg->current_line))
        dbg->ignore_breakpoint_once_line = dbg->current_line;

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

/* Indietro fino al breakpoint precedente: passi indietro ripetuti, finche'
 * si arriva su una riga con breakpoint o un passo non e' piu' possibile. */
int vm_debug_continue_inverse(VMDebugState *dbg)
{
    if (!dbg) return -1;
    int line = dbg->current_line;
    for (;;) {
        int before = dbg->history_top;
        line = vm_debug_step_back(dbg);
        if (line < 0) return line;
        if (dbg->last_error && dbg->last_error[0]) return line;
        if (dbg->history_top >= before) return line;
        if (dbg_is_breakpoint(dbg, line)) return line;
    }
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
    /* out == NULL (con outsz 0) chiede solo la lunghezza necessaria. */
    if (!g_debug_vm) return 0;
    if (!out) outsz = 0;
    (void)dbg;
    return vm_debug_dump_json(g_debug_vm, out, outsz);
}

int vm_debug_vars_json_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!g_debug_vm || !dbg) return 0;
    if (!out) outsz = 0;
    return vm_debug_vars_json(g_debug_vm, vm_name_get(dbg->current_frame), out, outsz);
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
    if (!dbg || !out || outsz <= 0) return 0;
    pthread_mutex_lock(&dbg->out_mtx);
    if (dbg->out_len == 0) { pthread_mutex_unlock(&dbg->out_mtx); return 0; }
    int n = dbg->out_len < outsz - 1 ? dbg->out_len : outsz - 1;
    memcpy(out, dbg->out_buf, n);
    out[n] = '\0';
    /* Quel che non entra nel buffer del chiamante resta per la lettura dopo,
       invece di andare perso. */
    dbg->out_len -= n;
    if (dbg->out_len > 0) memmove(dbg->out_buf, dbg->out_buf + n, (size_t)dbg->out_len);
    dbg->out_buf[dbg->out_len] = '\0';
    pthread_mutex_unlock(&dbg->out_mtx);
    return n;
}

int vm_debug_error_ext(VMDebugState *dbg, char *out, int outsz)
{
    if (!dbg || !dbg->last_error || dbg->last_error[0] == '\0') return 0;
    int n = (int)strlen(dbg->last_error);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, dbg->last_error, n);
    out[n] = '\0';
    return n;
}
