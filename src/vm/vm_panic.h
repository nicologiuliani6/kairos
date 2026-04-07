#ifndef VM_PANIC_H
#define VM_PANIC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include "vm_debug.h"

extern VMDebugState *g_debug_dbg;

#ifdef DEFINE_VM_DEBUG_PANIC
void vm_debug_panic(const char *fmt, ...)
{
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (!g_debug_dbg) {
        fprintf(stderr, "%s", msg);
        exit(EXIT_FAILURE);
    }

    strncpy(g_debug_dbg->last_error, msg, sizeof(g_debug_dbg->last_error) - 1);

    int olen = (int)strlen(msg);
    int room = (int)sizeof(g_debug_dbg->out_buf) - g_debug_dbg->out_len - 1;
    if (room > 0) {
        int n = olen < room ? olen : room;
        memcpy(g_debug_dbg->out_buf + g_debug_dbg->out_len, msg, n);
        g_debug_dbg->out_len += n;
    }

    pthread_mutex_lock(&g_debug_dbg->pause_mtx);
    g_debug_dbg->mode = VM_MODE_DONE;
    pthread_cond_broadcast(&g_debug_dbg->pause_cond);
    pthread_mutex_unlock(&g_debug_dbg->pause_mtx);

    pthread_exit(NULL);
}
#endif /* DEFINE_VM_DEBUG_PANIC */

#endif /* VM_PANIC_H */