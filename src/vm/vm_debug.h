#ifndef VM_DEBUG_H
#define VM_DEBUG_H

/*
 * vm_debug.h — infrastruttura di debug della VM Kairos
 *
 * Include:
 *   - dbg_init / dbg_destroy
 *   - dbg_hook: chiamato prima di ogni istruzione in vm_run_BT
 *   - dbg_record: salva un record nella history (per step-back)
 *   - vm_debug_dump_json: serializza lo stato corrente in JSON
 *   - vm_debug_set/clear_breakpoint
 *
 * Nessuna dipendenza ciclica: include solo vm_types.h e vm_helpers.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"
#include "vm_helpers.h"
#define VMLOG(...) do { \
    FILE *_f = fopen("/tmp/kairos-vm.log", "a"); \
    if (_f) { fprintf(_f, __VA_ARGS__); fclose(_f); } \
} while(0)

/* ======================================================================
 *  Inizializzazione / distruzione
 * ====================================================================== */

static inline void dbg_init(VMDebugState *dbg)
{
    memset(dbg, 0, sizeof(VMDebugState));
    dbg->mode        = VM_MODE_RUN;
    dbg->history_top = -1;
    dbg->bp_count    = 0;
    pthread_mutex_init(&dbg->pause_mtx, NULL);
    pthread_cond_init (&dbg->pause_cond, NULL);
    dbg->initialized = 1;
}

static inline void dbg_destroy(VMDebugState *dbg)
{
    if (!dbg || !dbg->initialized) return;
    pthread_mutex_destroy(&dbg->pause_mtx);
    pthread_cond_destroy (&dbg->pause_cond);
    dbg->initialized = 0;
}

/* ======================================================================
 *  Breakpoint management
 * ====================================================================== */

static inline void _vm_debug_set_breakpoint(VMDebugState *dbg, int line)
{
    if (!dbg || dbg->bp_count >= DBG_MAX_BREAKPOINTS) return;
    for (int i = 0; i < dbg->bp_count; i++)
        if (dbg->breakpoints[i] == line) return;  /* già presente */
    dbg->breakpoints[dbg->bp_count++] = line;
}

static inline void _vm_debug_clear_breakpoint(VMDebugState *dbg, int line)
{
    if (!dbg) return;
    for (int i = 0; i < dbg->bp_count; i++) {
        if (dbg->breakpoints[i] == line) {
            dbg->breakpoints[i] = dbg->breakpoints[--dbg->bp_count];
            return;
        }
    }
}

void vm_debug_clear_all_breakpoints(VMDebugState *dbg);

static inline int dbg_is_breakpoint(VMDebugState *dbg, int line)
{
    if (!dbg) return 0;
    for (int i = 0; i < dbg->bp_count; i++)
        if (dbg->breakpoints[i] == line) return 1;
    return 0;
}

/* ======================================================================
 *  History (per step-back)
 * ====================================================================== */

static inline void dbg_record(VMDebugState *dbg,
                              int line, const char *frame, const char *instr)
{
    if (!dbg) return;
    if (!instr || !*instr) return;

    /* Registra solo istruzioni che hanno effetto semantico sullo stato.
       Evita che step-back si fermi su metadati di controllo (es. EVAL/JMP/LABEL). */
    char instr_copy[DBG_INSTR_LEN];
    strncpy(instr_copy, instr, sizeof(instr_copy) - 1);
    instr_copy[sizeof(instr_copy) - 1] = '\0';
    char *p = skip_lineno(instr_copy);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;

    char op[64];
    int oi = 0;
    while (*p && *p != ' ' && *p != '\t' && oi < (int)sizeof(op) - 1) {
        op[oi++] = *p++;
    }
    op[oi] = '\0';
    if (op[0] == '\0') return;
    if (!strcmp(op, "EVAL")   || !strcmp(op, "JMPF")  || !strcmp(op, "JMP")   ||
        !strcmp(op, "ASSERT") || !strcmp(op, "LABEL") || !strcmp(op, "DECL")  ||
        !strcmp(op, "PARAM")  || !strcmp(op, "HALT")  || !strcmp(op, "PAR_END") ||
        strncmp(op, "THREAD_", 7) == 0) {
        return;
    }

    int idx = dbg->history_top + 1;
    if (idx >= DBG_MAX_HISTORY) {
        /* Ring buffer: scorriamo di uno (perdiamo la storia più vecchia) */
        memmove(dbg->history, dbg->history + 1,
                (DBG_MAX_HISTORY - 1) * sizeof(ExecRecord));
        idx = DBG_MAX_HISTORY - 1;
    } else {
        dbg->history_top = idx;
    }
    dbg->history[idx].line = line;
    strncpy(dbg->history[idx].frame, frame,  VAR_NAME_LENGTH - 1);
    strncpy(dbg->history[idx].instr, instr,  DBG_INSTR_LEN   - 1);
}

static inline ExecRecord *dbg_pop_history(VMDebugState *dbg)
{
    if (!dbg || dbg->history_top < 0) return NULL;
    return &dbg->history[dbg->history_top--];
}

/* ======================================================================
 *  Hook principale — chiamato PRIMA di ogni istruzione in vm_run_BT
 *
 *  Comportamento:
 *    - aggiorna current_line / current_frame nel debug state
 *    - se il modo è STEP: transita in PAUSE dopo aver segnato il punto
 *    - se il modo è CONTINUE e la riga è un breakpoint: transita in PAUSE
 *    - in PAUSE: chiama on_pause (se presente) e si blocca su pause_cond
 *      finché il controllore esterno non cambia mode e segnala
 * ====================================================================== */

static inline void dbg_hook(VMDebugState *dbg,
                            int line, const char *frame, const char *instr_text)
{
    if (!dbg || !dbg->initialized) return;
    dbg->current_line = line;
    strncpy(dbg->current_frame, frame, VAR_NAME_LENGTH - 1);

    /* Durante l'inversione guidata da step-back non registrare nuova history,
       altrimenti si reinseriscono record mentre li stiamo consumando. */
    if (dbg->mode != VM_MODE_CONTINUE_INV)
        dbg_record(dbg, line, frame, instr_text);

    int should_pause = 0;
    pthread_mutex_lock(&dbg->pause_mtx);

    if (dbg->mode == VM_MODE_STEP || dbg->mode == VM_MODE_STEP_BACK)
        should_pause = 1;
    else if ((dbg->mode == VM_MODE_CONTINUE || dbg->mode == VM_MODE_RUN) &&
             dbg_is_breakpoint(dbg, line))
        should_pause = 1;

    {
        FILE *f = fopen("/tmp/kairos-vm.log", "a");
        if (f) { fprintf(f, "dbg_hook line=%d mode=%d should_pause=%d\n", line, dbg->mode, should_pause); fclose(f); }
    }

    if (should_pause) {
        dbg->mode = VM_MODE_PAUSE;
        if (dbg->on_pause)
            dbg->on_pause(line, frame, dbg->userdata);
        dbg->first_pause_reached = 1;
        pthread_cond_broadcast(&dbg->pause_cond);   /* sveglia vm_debug_start */
    }

    /* Se qualcuno ha messo in pausa il debugger, tutti i thread si fermano qui. */
    while (dbg->mode == VM_MODE_PAUSE)
        pthread_cond_wait(&dbg->pause_cond, &dbg->pause_mtx);
    pthread_mutex_unlock(&dbg->pause_mtx);
}
/* Sblocca la VM dopo una pausa (chiamato dal controllore esterno) */
static inline void dbg_resume(VMDebugState *dbg, VMExecMode new_mode)
{
    if (!dbg || !dbg->initialized) return;
    pthread_mutex_lock(&dbg->pause_mtx);
    dbg->mode = new_mode;
    pthread_cond_signal(&dbg->pause_cond);
    pthread_mutex_unlock(&dbg->pause_mtx);
}

/* ======================================================================
 *  vm_debug_dump_json — serializza lo stato della VM in JSON
 *
 *  Formato:
 *  {
 *    "line": 42,
 *    "frame": "main",
 *    "mode": "PAUSE",
 *    "frames": [
 *      {
 *        "name": "main",
 *        "vars": [
 *          { "name": "x", "type": "int",   "value": 5 },
 *          { "name": "s", "type": "stack", "value": [1,2,3] }
 *        ]
 *      }
 *    ]
 *  }
 *
 *  Il buffer out deve essere pre-allocato (suggerito: 64 KB).
 *  Ritorna il numero di byte scritti (senza '\0').
 * ====================================================================== */

static const char *mode_name(VMExecMode m) {
    switch (m) {
        case VM_MODE_RUN:          return "RUN";
        case VM_MODE_PAUSE:        return "PAUSE";
        case VM_MODE_STEP:         return "STEP";
        case VM_MODE_STEP_BACK:    return "STEP_BACK";
        case VM_MODE_CONTINUE:     return "CONTINUE";
        case VM_MODE_CONTINUE_INV: return "CONTINUE_INV";
        case VM_MODE_DONE:         return "DONE";
        default:                   return "UNKNOWN";
    }
}

static inline int vm_debug_dump_json(VM *vm, char *out, int outsz)
{
    VMDebugState *dbg = vm->dbg;
    int n = 0;

#define JWRITE(...) n += snprintf(out + n, outsz - n, __VA_ARGS__); \
                    if (n >= outsz) return n;

    JWRITE("{");
    if (dbg) {
        JWRITE("\"line\":%d,", dbg->current_line);
        JWRITE("\"frame\":\"%s\",", dbg->current_frame);
        JWRITE("\"mode\":\"%s\",", mode_name(dbg->mode));
    } else {
        JWRITE("\"line\":0,\"frame\":\"\",\"mode\":\"RUN\",");
    }

    JWRITE("\"frames\":[");
    int first_frame = 1;
    for (int fi = 0; fi <= vm->frame_top; fi++) {
        Frame *f = &vm->frames[fi];
        if (f->name[0] == '\0') continue;

        if (!first_frame) JWRITE(",");
        first_frame = 0;

        JWRITE("{\"name\":\"%s\",\"vars\":[", f->name);
        int first_var = 1;
        for (int vi = 0; vi < f->var_count; vi++) {
            Var *v = f->vars[vi];
            if (!v) continue;
            if (v->T == TYPE_PARAM && !v->value) continue;

            if (!first_var) JWRITE(",");
            first_var = 0;

            JWRITE("{\"name\":\"%s\",", v->name);

            if (v->T == TYPE_INT) {
                JWRITE("\"type\":\"int\",\"value\":%d}", *(v->value));
            } else if (v->T == TYPE_STACK) {
                JWRITE("\"type\":\"stack\",\"value\":[");
                for (size_t k = 0; k < v->stack_len; k++) {
                    if (k) JWRITE(",");
                    JWRITE("%d", v->value[k]);
                }
                JWRITE("]}");
            } else if (v->T == TYPE_CHANNEL) {
                JWRITE("\"type\":\"channel\",\"value\":[");
                for (size_t k = 0; k < v->stack_len; k++) {
                    if (k) JWRITE(",");
                    JWRITE("%d", v->value[k]);
                }
                JWRITE("]}");
            } else {
                JWRITE("\"type\":\"param\",\"value\":null}");
            }
        }
        JWRITE("]}");
    }
    JWRITE("]}");

#undef JWRITE
    return n;
}

/* ======================================================================
 *  vm_debug_vars_json — solo le variabili del frame corrente
 *  (versione compatta per il watch panel del DAP)
 * ====================================================================== */

static inline int vm_debug_vars_json(VM *vm, const char *frame_name,
                                     char *out, int outsz)
{
    int n = 0;
#define JWRITE(...) n += snprintf(out + n, outsz - n, __VA_ARGS__); \
                    if (n >= outsz) return n;

    if (!char_id_map_exists(&FrameIndexer, frame_name)) {
        JWRITE("[]");
        return n;
    }
    uint fi = char_id_map_get(&FrameIndexer, frame_name);
    Frame *f = &vm->frames[fi];

    JWRITE("[");
    int first = 1;
    for (int vi = 0; vi < f->var_count; vi++) {
        Var *v = f->vars[vi];
        if (!v) continue;
        if (v->T == TYPE_PARAM && !v->value) continue;

        if (!first) JWRITE(",");
        first = 0;

        JWRITE("{\"name\":\"%s\",", v->name);
        if (v->T == TYPE_INT) {
            JWRITE("\"type\":\"int\",\"value\":%d}", *(v->value));
        } else if (v->T == TYPE_STACK || v->T == TYPE_CHANNEL) {
            JWRITE("\"type\":\"%s\",\"value\":[",
                   v->T == TYPE_STACK ? "stack" : "channel");
            for (size_t k = 0; k < v->stack_len; k++) {
                if (k) JWRITE(",");
                JWRITE("%d", v->value[k]);
            }
            JWRITE("]}");
        } else {
            JWRITE("\"type\":\"param\",\"value\":null}");
        }
    }
    JWRITE("]");
#undef JWRITE
    return n;
}

#endif /* VM_DEBUG_H */