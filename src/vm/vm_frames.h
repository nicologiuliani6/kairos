#ifndef VM_FRAMES_H
#define VM_FRAMES_H

#include <string.h>
#include <stdlib.h>
#include "vm_types.h"
#include "vm_helpers.h"

void vm_debug_panic(const char *fmt, ...);

/* Cap di sicurezza per la profondità di ricorsione dei frame clonati.
 * Forward la profondità reale è piccola (cifre ≤ 19 per __mn_putd_uint,
 * divmod mnhalve ≤ ~64). L'inverse (UNCALL) col fallback recursion_depth-replay
 * (vm_invert.h ~riga 1130) NON termina per procedure con struttura
 * "1×THEN per livello poi base ELSE" (es. __mn_putd_uint): clona @1,@2,@3,…
 * all'infinito → OOM/hang. Questo cap non si attiva mai su inversione corretta;
 * trasforma il runaway in errore pulito (exit 1). Fix definitivo (branch_trace
 * whole-program o putd non-ricorsivo) in TODO. */
#define MN_CLONE_MAX_DEPTH 512

/* ======================================================================
 *  Frames dynamic capacity
 * ======================================================================
 *  vm->frames cresce on-demand (raddoppia). Zero-fill della nuova
 *  regione necessario perché init_clone_frame fa memset(clone, 0, sizeof)
 *  ma altri call site leggono campi prima di init.
 */
static inline void vm_ensure_frame_cap(VM *vm, uint needed)
{
    /* Grow del pointer array vm->frames se needed >= cap. Ogni slot vuoto. */
    if (needed >= vm->frames_cap) {
        uint new_cap = vm->frames_cap ? vm->frames_cap : VM_FRAMES_INIT_CAP;
        while (new_cap <= needed) new_cap *= 2;
        Frame **nf = (Frame **)realloc(vm->frames, sizeof(Frame *) * new_cap);
        if (!nf) {
            fprintf(stderr, "[VM] vm_ensure_frame_cap: realloc(%u) fallita\n", new_cap);
            exit(1);
        }
        memset(nf + vm->frames_cap, 0, sizeof(Frame *) * (new_cap - vm->frames_cap));
        vm->frames = nf;
        vm->frames_cap = new_cap;
    }
    /* Alloca Frame individuale per lo slot needed se ancora NULL. */
    if (!vm->frames[needed]) {
        vm->frames[needed] = (Frame *)calloc(1, sizeof(Frame));
        if (!vm->frames[needed]) {
            fprintf(stderr, "[VM] vm_ensure_frame_cap: calloc Frame fallita\n");
            exit(1);
        }
    }
}

/* ======================================================================
 *  Clone frame — corpo comune estratto
 * ====================================================================== */

static inline void init_clone_frame(VM *vm, uint clone_fi, uint base_fi, const char *key)
{
    vm_ensure_frame_cap(vm, clone_fi);
    vm_ensure_frame_cap(vm, base_fi);
    Frame *base  = vm->frames[base_fi];
    Frame *clone = vm->frames[clone_fi];

    memset(clone, 0, sizeof(Frame));
    clone->VarIndexer   = base->VarIndexer;
    clone->LabelIndexer = base->LabelIndexer;
    clone->addr         = base->addr;
    clone->end_addr     = base->end_addr;
    clone->var_count    = base->var_count;
    clone->param_count  = base->param_count;
    memcpy(clone->param_indices, base->param_indices, sizeof(base->param_indices));
    memcpy(clone->label,         base->label,         sizeof(base->label));
    snprintf(clone->name, VAR_NAME_LENGTH, "%s", key);
    stack_init(&clone->LocalVariables);

    /* memset ha azzerato clone->vars (NULL) e vars_cap (0): alloca il buffer
     * heap del clone prima di scriverci (gli slot Var* restano NULL). */
    frame_ensure_vars(clone, base->var_count);

    for (int k = 0; k < clone->param_count; k++) {
        int pidx = clone->param_indices[k];
        if (pidx < 0 || pidx >= clone->vars_cap) {
            fprintf(stderr, "[VM] init_clone_frame: pidx %d out of range (k=%d pc=%d frame=%s base_pc=%d)\n",
                pidx, k, clone->param_count, key, base->param_count);
            exit(1);
        }
        if (!base->vars[pidx]) {
            fprintf(stderr, "[VM] init_clone_frame: base vars[%d] NULL (k=%d pc=%d frame=%s var_count=%d)\n",
                pidx, k, clone->param_count, key, base->var_count);
            exit(1);
        }
        clone->vars[pidx] = calloc(1, sizeof(Var));
        strncpy(clone->vars[pidx]->name, base->vars[pidx]->name, VAR_NAME_LENGTH - 1);
        clone->vars[pidx]->T = TYPE_PARAM;
    }

    /*
     * vm_run_BT salta DECL: gli stack (e channel) dichiarati a livello di procedura
     * vengono allocati solo in vm_exec sul frame base. Un clone ricorsivo eredita
     * VarIndexer ma non le Var: la prima push su __mn_hist in una call annidata
     * andava in NULL → SIGSEGV in op_push. Duplichiamo slot DECL del base frame.
     * Gli int introdotti da LOCAL restano NULL finché non gira op_local sul clone.
     */
    for (uint vi = 0; vi < (uint)base->var_count; vi++) {
        int is_param = 0;
        for (int pk = 0; pk < base->param_count; pk++) {
            if (base->param_indices[pk] == (int)vi) {
                is_param = 1;
                break;
            }
        }
        if (is_param)
            continue;
        Var *bv = base->vars[vi];
        if (!bv)
            continue;
        if (bv->T == TYPE_STACK || bv->T == TYPE_CHANNEL) {
            const char *typ = (bv->T == TYPE_STACK) ? "stack" : "channel";
            clone->vars[vi] = malloc(sizeof(Var));
            if (!clone->vars[vi])
                vm_debug_panic("[VM] init_clone_frame: malloc Var fallita\n");
            alloc_var(clone->vars[vi], typ, bv->name);
            clone->vars[vi]->is_local = 0;
        } else if (bv->T == TYPE_INT) {
            /* Mnemo emit `int __mn_e<N> = 0` a livello procedura (DECL). Forward su
             * clone ricorsivo non re-esegue DECL → opt-uncall XOREQ su __mn_e<N>
             * andava in Var* NULL. Duplichiamo anche slot int DECL (LOCAL-allocati
             * sovrascriveranno via op_local). */
            clone->vars[vi] = malloc(sizeof(Var));
            if (!clone->vars[vi])
                vm_debug_panic("[VM] init_clone_frame: malloc Var int fallita\n");
            alloc_var(clone->vars[vi], "int", bv->name);
            clone->vars[vi]->is_local = 0;
        }
    }
}

static inline uint clone_frame_for_depth(VM *vm, const char *proc, int depth)
{
    if (depth > MN_CLONE_MAX_DEPTH)
        vm_debug_panic(
            "[VM] clone depth %d > %d su '%s' — ricorsione inversa non "
            "terminante (recursion_depth-replay non adatto a questa proc, "
            "es. __mn_putd_uint/printf sotto --check-invertibility). Vedi TODO.\n",
            depth, MN_CLONE_MAX_DEPTH, proc);
    pthread_mutex_lock(&var_indexer_mtx);
    char key[VAR_NAME_LENGTH];
    if (current_thread_args)
        make_frame_key_par_rec(proc, depth, key, sizeof(key));
    else
        make_frame_key(proc, depth, key, sizeof(key));
    if (char_id_map_exists(&FrameIndexer, key)) {
        uint r = (uint)char_id_map_get(&FrameIndexer, key);
        pthread_mutex_unlock(&var_indexer_mtx);
        return r;
    }
    uint base_fi  = (uint)char_id_map_get(&FrameIndexer, proc);
    uint clone_fi = (uint)char_id_map_get(&FrameIndexer, key);
    init_clone_frame(vm, clone_fi, base_fi, key);
    pthread_mutex_unlock(&var_indexer_mtx);
    return clone_fi;
}

static inline uint clone_frame_for_thread(VM *vm, const char *proc)
{
    pthread_mutex_lock(&var_indexer_mtx);
    char key[VAR_NAME_LENGTH];
    make_thread_frame_key(proc, key, sizeof(key));
    if (char_id_map_exists(&FrameIndexer, key)) {
        uint r = (uint)char_id_map_get(&FrameIndexer, key);
        pthread_mutex_unlock(&var_indexer_mtx);
        return r;
    }
    uint base_fi  = (uint)char_id_map_get(&FrameIndexer, proc);
    uint clone_fi = (uint)char_id_map_get(&FrameIndexer, key);
    init_clone_frame(vm, clone_fi, base_fi, key);
    pthread_mutex_unlock(&var_indexer_mtx);
    return clone_fi;
}

#endif /* VM_FRAMES_H */