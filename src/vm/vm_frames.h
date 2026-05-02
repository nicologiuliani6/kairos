#ifndef VM_FRAMES_H
#define VM_FRAMES_H

#include <string.h>
#include "vm_types.h"
#include "vm_helpers.h"

/* ======================================================================
 *  Clone frame — corpo comune estratto
 * ====================================================================== */

static inline void init_clone_frame(VM *vm, uint clone_fi, uint base_fi, const char *key)
{
    Frame *base  = &vm->frames[base_fi];
    Frame *clone = &vm->frames[clone_fi];

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

    for (int k = 0; k < clone->param_count; k++) {
        int pidx = clone->param_indices[k];
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
        }
    }
}

static inline uint clone_frame_for_depth(VM *vm, const char *proc, int depth)
{
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