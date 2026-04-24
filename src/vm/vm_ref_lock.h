#ifndef VM_REF_LOCK_H
#define VM_REF_LOCK_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"

void vm_debug_panic(const char *fmt, ...);

static pthread_mutex_t g_var_par_mut_mtx = PTHREAD_MUTEX_INITIALIZER;

/*
 * Lock esclusivo (re-entrante sullo stesso pthread) solo durante mutazioni su int
 * dentro thread PAR. Parametri int solo letti (es. stesso n in producer/consumer)
 * non vengono bloccati alla CALL; le scritture concorrenti sulla stessa cella sì.
 */
static inline void var_par_mut_acquire(Var *v)
{
    if (!current_thread_args || !v || v->T != TYPE_INT) return;
    pthread_mutex_lock(&g_var_par_mut_mtx);
    pthread_t self = pthread_self();
    if (v->ref_lock_depth > 0 && !pthread_equal(v->ref_lock_owner, self)) {
        char nm[VAR_NAME_LENGTH];
        strncpy(nm, v->name, VAR_NAME_LENGTH - 1);
        nm[VAR_NAME_LENGTH - 1] = '\0';
        pthread_mutex_unlock(&g_var_par_mut_mtx);
        vm_debug_panic(
            "[VM] mutazione concorrente sulla variabile int '%s' da un altro thread\n",
            nm);
    }
    if (v->ref_lock_depth == 0)
        v->ref_lock_owner = self;
    v->ref_lock_depth++;
    pthread_mutex_unlock(&g_var_par_mut_mtx);
}

static inline void var_par_mut_release(Var *v)
{
    if (!current_thread_args || !v || v->T != TYPE_INT) return;
    pthread_mutex_lock(&g_var_par_mut_mtx);
    if (v->ref_lock_depth <= 0) {
        pthread_mutex_unlock(&g_var_par_mut_mtx);
        vm_debug_panic("[VM] unlock par-mut inconsistente per '%s'\n", v->name);
    }
    v->ref_lock_depth--;
    if (v->ref_lock_depth == 0)
        memset(&v->ref_lock_owner, 0, sizeof(v->ref_lock_owner));
    pthread_mutex_unlock(&g_var_par_mut_mtx);
}

static inline void var_par_mut_acquire2(Var *a, Var *b)
{
    if (!current_thread_args) return;
    if (a == b) {
        var_par_mut_acquire(a);
        return;
    }
    if ((uintptr_t)a > (uintptr_t)b) {
        Var *t = a;
        a      = b;
        b      = t;
    }
    var_par_mut_acquire(a);
    var_par_mut_acquire(b);
}

static inline void var_par_mut_release2(Var *a, Var *b)
{
    if (!current_thread_args) return;
    if (a == b) {
        var_par_mut_release(a);
        return;
    }
    if ((uintptr_t)a > (uintptr_t)b) {
        Var *t = a;
        a      = b;
        b      = t;
    }
    var_par_mut_release(b);
    var_par_mut_release(a);
}

#endif /* VM_REF_LOCK_H */
