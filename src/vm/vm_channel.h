#ifndef VM_CHANNEL_H
#define VM_CHANNEL_H

#include <stdlib.h>
#include "vm_types.h"

/* ======================================================================
 *  Notify / wait — handshake tra thread su canale
 * ====================================================================== */

static inline void notify_sender_turn_done(ThreadArgs *sender)
{
    if (!sender) return;
    pthread_mutex_lock(sender->done_mtx);
    sender->turn_done = 1;
    pthread_cond_signal(sender->done_cond);
    pthread_mutex_unlock(sender->done_mtx);
}

/* Blocca il thread corrente finché turn_done non è settato. */
static inline void wait_for_turn_done(ThreadArgs *ta)
{
    if (!ta) return;
    pthread_mutex_lock(ta->done_mtx);
    ta->blocked = 1;
    while (!ta->turn_done && !ta->finished)
        pthread_cond_wait(ta->done_cond, ta->done_mtx);
    ta->turn_done = 0;
    ta->blocked   = 0;
    pthread_mutex_unlock(ta->done_mtx);
}

static inline void signal_blocked(ThreadArgs *ta)
{
    if (!ta) return;
    pthread_mutex_lock(ta->done_mtx);
    ta->blocked = 1;
    pthread_cond_signal(ta->done_cond);
    pthread_mutex_unlock(ta->done_mtx);
}

static inline void clear_blocked(ThreadArgs *ta)
{
    if (!ta) return;
    pthread_mutex_lock(ta->done_mtx);
    ta->blocked = 0;
    pthread_mutex_unlock(ta->done_mtx);
}

/* ======================================================================
 *  op_wait — rendez-vous su canale
 *  Ritorna 1 se il sender era già in coda (chiamante deve aspettare turn_done).
 * ====================================================================== */

static inline int op_wait(Channel *ch, int is_send)
{
    /* Prima di bloccarci, svegliamo l'eventuale sender pendente */
    if (!is_send && current_thread_args && current_thread_args->sender_to_notify) {
        ThreadArgs *s = current_thread_args->sender_to_notify;
        current_thread_args->sender_to_notify = NULL;
        notify_sender_turn_done(s);
    }

    pthread_mutex_lock(&ch->mtx);

    Waiter *self = malloc(sizeof(Waiter));
    pthread_cond_init(&self->cond, NULL);
    self->ready = 0;
    self->next  = NULL;
    self->thread_args = current_thread_args;

    if (is_send) {
        if (ch->recv_q_head) {                          /* match immediato con recv */
            Waiter *w = ch->recv_q_head;
            ch->recv_q_head = w->next;
            if (!ch->recv_q_head) ch->recv_q_tail = NULL;
            ch->sender_args = self->thread_args;
            w->ready = 1;
            pthread_cond_signal(&w->cond);
            pthread_mutex_unlock(&ch->mtx);
            wait_for_turn_done(current_thread_args);
            pthread_cond_destroy(&self->cond);
            free(self);
            return 0;
        }
        /* Nessun match → vai in coda e segnala blocked */
        if (ch->send_q_tail) ch->send_q_tail->next = self; else ch->send_q_head = self;
        ch->send_q_tail = self;
        signal_blocked(current_thread_args);
        while (!self->ready) pthread_cond_wait(&self->cond, &ch->mtx);
        clear_blocked(current_thread_args);
        pthread_mutex_unlock(&ch->mtx);
        pthread_cond_destroy(&self->cond);
        free(self);
        return 1;      /* era in coda: chiamante deve aspettare turn_done */
    } else {
        if (ch->send_q_head) {                          /* match immediato con send */
            Waiter *w = ch->send_q_head;
            ch->send_q_head = w->next;
            if (!ch->send_q_head) ch->send_q_tail = NULL;
            ch->sender_args = w->thread_args;
            w->ready = 1;
            pthread_cond_signal(&w->cond);
            pthread_mutex_unlock(&ch->mtx);
            pthread_cond_destroy(&self->cond);
            free(self);
            return 0;
        }
        if (ch->recv_q_tail) ch->recv_q_tail->next = self; else ch->recv_q_head = self;
        ch->recv_q_tail = self;
        signal_blocked(current_thread_args);
        while (!self->ready) pthread_cond_wait(&self->cond, &ch->mtx);
        clear_blocked(current_thread_args);
        pthread_mutex_unlock(&ch->mtx);
        pthread_cond_destroy(&self->cond);
        free(self);
        return 0;
    }
}

#endif /* VM_CHANNEL_H */