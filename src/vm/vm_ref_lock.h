#ifndef VM_REF_LOCK_H
#define VM_REF_LOCK_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vm_types.h"

void vm_debug_panic(const char *fmt, ...);

/*
 * Rilevatore di mutazioni concorrenti su una cella int dentro un blocco PAR.
 *
 * NON serve alla correttezza: la semantica vuole che rami paralleli lavorino su
 * porzioni disgiunte dello store, quindi due thread sulla stessa cella sono gia'
 * un programma sbagliato. Questo serve a dirlo con un messaggio invece che con un
 * risultato storto.
 *
 * Proprio perche' e' un rilevatore, non puo' permettersi di costare: si trova
 * sul cammino di OGNI somma, sottrazione e xor eseguiti in un thread. Un mutex
 * unico per tutto il processo, che e' quello che c'era, serializza rami che per
 * ipotesi non condividono nulla, e vanifica il parallelismo. Lo stato sta quindi
 * nella cella stessa, e si tocca con operazioni atomiche: in un programma
 * corretto ogni thread trova sempre e solo le proprie celle, e non c'e' contesa.
 */
static inline void var_par_mut_acquire(Var *v)
{
    if (!current_thread_args || !v || v->T != TYPE_INT) return;
    pthread_t self = pthread_self();

    int atteso = 0;
    if (__atomic_compare_exchange_n(&v->ref_lock_depth, &atteso, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        v->ref_lock_owner = self;      /* cella libera: siamo i soli qui */
        return;
    }

    /* Gia' impegnata: o siamo noi, ed e' rientranza legittima, oppure e' un
       altro thread, e allora il programma viola la disgiunzione. */
    if (!pthread_equal(v->ref_lock_owner, self)) {
        char nm[VAR_NAME_LENGTH];
        strncpy(nm, v->name, VAR_NAME_LENGTH - 1);
        nm[VAR_NAME_LENGTH - 1] = '\0';
        vm_debug_panic(
            "[VM] mutazione concorrente sulla variabile int '%s' da un altro thread\n",
            nm);
    }
    __atomic_fetch_add(&v->ref_lock_depth, 1, __ATOMIC_ACQ_REL);
}

static inline void var_par_mut_release(Var *v)
{
    if (!current_thread_args || !v || v->T != TYPE_INT) return;
    int prima = __atomic_fetch_sub(&v->ref_lock_depth, 1, __ATOMIC_ACQ_REL);
    if (prima <= 0)
        vm_debug_panic("[VM] unlock par-mut inconsistente per '%s'\n", v->name);
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
