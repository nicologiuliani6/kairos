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
}

static inline uint clone_frame_for_depth(VM *vm, const char *proc, int depth)
{
    char key[VAR_NAME_LENGTH];
    make_frame_key(proc, depth, key, sizeof(key));
    if (char_id_map_exists(&FrameIndexer, key))
        return char_id_map_get(&FrameIndexer, key);
    uint base_fi  = char_id_map_get(&FrameIndexer, proc);
    uint clone_fi = char_id_map_get(&FrameIndexer, key);
    init_clone_frame(vm, clone_fi, base_fi, key);
    return clone_fi;
}

static inline uint clone_frame_for_thread(VM *vm, const char *proc)
{
    char key[VAR_NAME_LENGTH];
    make_thread_frame_key(proc, key, sizeof(key));
    if (char_id_map_exists(&FrameIndexer, key))
        return char_id_map_get(&FrameIndexer, key);
    uint base_fi  = char_id_map_get(&FrameIndexer, proc);
    uint clone_fi = char_id_map_get(&FrameIndexer, key);
    init_clone_frame(vm, clone_fi, base_fi, key);
    return clone_fi;
}

#endif /* VM_FRAMES_H */