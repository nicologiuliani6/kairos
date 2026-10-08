#ifndef STACK_H
#define STACK_H

#include <stdio.h>
#include <stdlib.h>

void vm_debug_panic(const char *fmt, ...);


/* Stack Var* per LOCAL annidati / sequenza di local all’ingresso procedura.
 * Nessuna profondità massima: il vettore cresce raddoppiando.
 *
 * Proprietà del buffer. Il codice salva e ripristina lo stack di un frame
 * attorno a una chiamata (`Stack s = f->LocalVariables; stack_init(...); ...;
 * stack_restore(&f->LocalVariables, s)`): la copia salvata si porta via il
 * buffer, stack_init lascia il frame con uno stack vuoto e senza buffer, e
 * stack_restore libera quello usato nel frattempo prima di rimettere il
 * salvato. Così ogni buffer ha un solo proprietario e nessuno è condiviso.
 * stack_clear invece svuota lo stack tenendo il buffer: è il vecchio
 * `top = -1` per chi lo azzera senza averlo salvato. */
#define STACK_INIT_CAP 8

// ===== Forward declaration =====
typedef struct Var Var;

// ===== Stack di puntatori =====
typedef struct {
    Var **data;
    int   top;
    int   cap;
} Stack;

// ===== Implementazione =====

// init: stack vuoto, nessun buffer (lo alloca il primo push)
static inline void stack_init(Stack* s) {
    s->data = NULL;
    s->top  = -1;
    s->cap  = 0;
}

// svuota tenendo il buffer
static inline void stack_clear(Stack* s) {
    s->top = -1;
}

// empty
static inline int stack_is_empty(Stack* s) {
    return s->top == -1;
}

// size
static inline int stack_size(Stack* s) {
    return s->top;
}

/* Capacità per almeno `n` elementi. Chiamata anche prima di un par, dal thread
 * che lo lancia, così che i rami che fanno local nello stesso frame non
 * debbano riallocare il buffer mentre un altro ramo lo usa. */
static inline void stack_reserve(Stack* s, int n) {
    if (n <= s->cap) return;
    int nc = s->cap ? s->cap : STACK_INIT_CAP;
    while (nc < n) nc *= 2;
    Var **nd = (Var **)realloc(s->data, sizeof(Var *) * (size_t)nc);
    if (!nd) vm_debug_panic("[VM] Stack: memoria esaurita\n");
    s->data = nd;
    s->cap  = nc;
}

// push (passi un puntatore già esistente)
static inline void stack_push(Stack* s, Var* v) {
    if (s->top + 1 >= s->cap) stack_reserve(s, s->top + 2);
    s->data[++s->top] = v;
}

// pop (ritorna il puntatore, NON libera)
static inline Var* stack_pop(Stack* s) {
    if (stack_is_empty(s)) {
        vm_debug_panic("DELOCAL su variabile non local!\n");
    }
    return s->data[s->top--];
}

// peek
static inline Var* stack_peek(Stack* s) {
    if (stack_is_empty(s)) {
        vm_debug_panic("Stack empty\n");
    }
    return s->data[s->top];
}

// rimette uno stack salvato, liberando il buffer usato nel frattempo
static inline void stack_restore(Stack* dst, Stack saved) {
    if (dst->data && dst->data != saved.data) free(dst->data);
    *dst = saved;
}

// libera il buffer (non le Var)
static inline void stack_release(Stack* s) {
    free(s->data);
    stack_init(s);
}

#endif
