#ifndef VM_FRAMES_H
#define VM_FRAMES_H

#include <string.h>
#include <stdlib.h>
#include "vm_types.h"
#include "vm_helpers.h"

void vm_debug_panic(const char *fmt, ...);

/* Profondità di ricorsione: nessun massimo. Ogni livello è un frame clonato
 * ("proc@<depth>") che costa poche centinaia di byte più le variabili della
 * procedura. Il vecchio tetto (512) serviva a trasformare in errore la
 * ricorsione inversa non terminante del replay per recursion_depth sulle
 * primitive di stampa Mnemo (__mn_putd_uint sotto --check-invertibility):
 * quel caso oggi non arriva più qui, perché l'inverso delle CALL __mn_put*
 * è saltato in invert_op_to_line (identità sullo stato), e la ricorsione
 * delle procedure utente gira in avanti sul corpo inverso compilato
 * (<proc>__inv, src/frontend/inverse.py). Una ricorsione davvero infinita si
 * comporta come in qualunque linguaggio: consuma memoria finché c'è. */

/* ======================================================================
 *  Frames dynamic capacity
 * ======================================================================
 *  vm->frames cresce on-demand (raddoppia). Zero-fill della nuova
 *  regione necessario perché init_clone_frame fa memset(clone, 0, sizeof)
 *  ma altri call site leggono campi prima di init.
 */
static inline void vm_ensure_frame_cap(VM *vm, uint needed)
{
    /* Grow del pointer array vm->frames se needed >= cap. Ogni slot vuoto.
     * Gli altri thread leggono vm->frames[fi] senza lock (ogni istruzione di
     * ogni ramo di un par): il vettore vecchio non si libera, lo si sostituisce
     * con uno più grande pubblicato in un colpo solo (vm_grow_keep). Con la
     * ricorsione senza tetto il vettore cresce davvero anche dentro un par. */
    if (needed >= vm->frames_cap || !vm->frames) {
        uint new_cap = vm->frames_cap ? vm->frames_cap : VM_FRAMES_INIT_CAP;
        while (new_cap <= needed) new_cap *= 2;
        Frame **nf = (Frame **)vm_grow_keep(vm->frames, sizeof(Frame *) * vm->frames_cap,
                                            sizeof(Frame *) * new_cap);
        __atomic_store_n(&vm->frames, nf, __ATOMIC_RELEASE);
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

    /* Slot riusato (indici dei frame riciclati dal pattern opt-uncall di
     * Mnemo): le mappe e il nome del clone precedente sono sull'heap. */
    char_id_map_destroy(&clone->VarIndexer);
    char_id_map_destroy(&clone->LabelIndexer);
    free(clone->name);
    stack_release(&clone->LocalVariables);

    memset(clone, 0, sizeof(Frame));
    /* Copia profonda: il clone inserisce i suoi LOCAL nella sua mappa. */
    char_id_map_copy(&clone->VarIndexer,   &base->VarIndexer);
    char_id_map_copy(&clone->LabelIndexer, &base->LabelIndexer);
    clone->names_hint   = base->names_hint;
    clone->locals_hint  = base->locals_hint;
    clone->addr         = base->addr;
    clone->end_addr     = base->end_addr;
    clone->var_count    = base->var_count;
    clone->param_count  = base->param_count;
    /* param_indices/label ora heap dinamici: il memset ha azzerato puntatori e
     * cap del clone. Alloca e copia per COUNT (non sizeof). param_indices fino a
     * param_count; label fino al numero di label del base (LabelIndexer.count). */
    if (base->param_count > 0) {
        frame_ensure_params(clone, base->param_count);
        memcpy(clone->param_indices, base->param_indices,
               sizeof(int) * (size_t)base->param_count);
    }
    if (base->label_cap > 0) {
        frame_ensure_labels(clone, base->label_cap - 1);
        memcpy(clone->label, base->label, sizeof(uint) * (size_t)base->label_cap);
    }
    clone->name = char_id_strdup(key);
    stack_init(&clone->LocalVariables);

    /* memset ha azzerato clone->vars (NULL) e vars_cap (0): alloca il buffer
     * heap del clone prima di scriverci (gli slot Var* restano NULL), della
     * stessa capacità del base. */
    frame_ensure_vars(clone, base->vars_cap > base->var_count ? base->vars_cap - 1
                                                              : base->var_count);

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
        clone->vars[pidx]->name = char_id_strdup(base->vars[pidx]->name);
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
    pthread_mutex_lock(&var_indexer_mtx);
    VM_FRAME_KEY_BUF(key, proc);
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
    VM_FRAME_KEY_BUF(key, proc);
    make_thread_frame_key(proc, key, sizeof(key));

    /* Cammino veloce senza esclusione. L'indice dei frame e' append-only e
       pubblica ogni voce solo quando e' completa, quindi trovarne una gia'
       presente e' sicuro anche senza lock. E' il caso normale: il clone si crea
       una volta per procedura e per thread, poi ogni chiamata successiva lo
       ritrova. Prendere qui un mutex unico per tutto il processo, come si faceva,
       serializzava OGNI chiamata di procedura di OGNI ramo. */
    int gia = char_id_map_lookup(&FrameIndexer, key);
    if (gia >= 0) return (uint)gia;

    pthread_mutex_lock(&var_indexer_mtx);
    gia = char_id_map_lookup(&FrameIndexer, key);   /* ricontrollo sotto lock */
    if (gia >= 0) {
        pthread_mutex_unlock(&var_indexer_mtx);
        return (uint)gia;
    }
    uint base_fi  = (uint)char_id_map_get(&FrameIndexer, proc);
    uint clone_fi = (uint)char_id_map_get(&FrameIndexer, key);
    init_clone_frame(vm, clone_fi, base_fi, key);
    pthread_mutex_unlock(&var_indexer_mtx);
    return clone_fi;
}

#endif /* VM_FRAMES_H */