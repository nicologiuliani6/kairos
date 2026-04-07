#ifndef STACK_H
#define STACK_H

#include <stdio.h>
#include <stdlib.h>

void vm_debug_panic(const char *fmt, ...);


#define STACK_MAX 100

// ===== Forward declaration =====
typedef struct Var Var;

// ===== Stack di puntatori =====
typedef struct {
    Var* data[STACK_MAX];
    int top;
} Stack;

// ===== Implementazione =====

// init
static inline void stack_init(Stack* s) {
    s->top = -1;
}

// empty
static inline int stack_is_empty(Stack* s) {
    return s->top == -1;
}

// full
static inline int stack_is_full(Stack* s) {
    return s->top == STACK_MAX - 1;
}

// size
static inline int stack_size(Stack* s) {
    return s->top;
}

// push (passi un puntatore già esistente)
static inline void stack_push(Stack* s, Var* v) {
    if (stack_is_full(s)) {
        vm_debug_panic("Stack overflow\n");
    }
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

// opzionale: free di tutto lo stack
static inline void stack_free_all(Stack* s) {
    while (!stack_is_empty(s)) {
        free(stack_pop(s));
    }
}

#endif