#pragma once

/* forward declaration: op_swap è definita in vm.c dopo questo include */
void op_swap(VM *vm, const char *frame_name);

/* ======================================================================
 *  ops_arith.h — Operazioni aritmetiche e di confronto della VM
 *
 *  Questo file va incluso in vm.c DOPO la definizione di:
 *    - tipi VM, Frame, Var, ValueType
 *    - macro perror, uint
 *    - helper: get_findex(), resolve_value(), get_var()
 * ====================================================================== */

void op_pusheq(VM *vm, const char *frame_name)
{
    char *ID      = strtok(NULL, " \t");
    char *C_value = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    Var  *v       = get_var(vm, Findex, ID, "PUSHEQ");
    if (v->T != TYPE_INT) perror("[VM] PUSHEQ non su INT!\n");
    *(v->value) += resolve_value(vm, Findex, C_value);
}

void op_mineq(VM *vm, const char *frame_name)
{
    char *ID      = strtok(NULL, " \t");
    char *C_value = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    Var  *v       = get_var(vm, Findex, ID, "MINEQ");
    if (v->T != TYPE_INT) perror("[VM] MINEQ non su INT!\n");
    *(v->value) -= resolve_value(vm, Findex, C_value);
}

void op_prodeq(VM *vm, const char *frame_name)
{
    char *ID      = strtok(NULL, " \t");
    char *C_value = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    Var  *v       = get_var(vm, Findex, ID, "PRODEQ");
    if (v->T != TYPE_INT) perror("[VM] PRODEQ non su INT!\n");
    *(v->value) *= resolve_value(vm, Findex, C_value);
}

void op_diveq(VM *vm, const char *frame_name)
{
    char *ID      = strtok(NULL, " \t");
    char *C_value = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    Var  *v       = get_var(vm, Findex, ID, "DIVEQ");
    if (v->T != TYPE_INT) perror("[VM] DIVEQ non su INT!\n");
    int rhs = resolve_value(vm, Findex, C_value);
    if (rhs == 0) perror("[VM] Divisione per zero!\n");
    *(v->value) /= rhs;
}

void op_modeq(VM *vm, const char *frame_name)
{
    char *ID      = strtok(NULL, " \t");
    char *C_value = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    Var  *v       = get_var(vm, Findex, ID, "MODEQ");
    if (v->T != TYPE_INT) perror("[VM] MODEQ non su INT!\n");
    int rhs = resolve_value(vm, Findex, C_value);
    if (rhs == 0) perror("[VM] Modulo per zero!\n");
    *(v->value) %= rhs;
}

void op_expeq(VM *vm, const char *frame_name)
{
    char *ID      = strtok(NULL, " \t");
    char *C_value = strtok(NULL, " \t");
    uint  Findex  = get_findex(frame_name);
    Var  *v       = get_var(vm, Findex, ID, "EXPEQ");
    if (v->T != TYPE_INT) perror("[VM] EXPEQ non su INT!\n");
    int exp    = resolve_value(vm, Findex, C_value);
    int base   = *(v->value);
    int result = 1;
    for (int i = 0; i < exp; i++) result *= base;
    *(v->value) = result;
}

/* ======================================================================
 *  Inverse per UNCALL
 *
 *  Operazioni invertibili:
 *    PUSHEQ  (+= n)  →  MINEQ  (-= n)
 *    MINEQ   (-= n)  →  PUSHEQ (+= n)
 *    PRODEQ  (*= n)  →  DIVEQ  (/= n)   [solo se n != 0 e divisione esatta]
 *    DIVEQ   (/= n)  →  PRODEQ (*= n)
 *    SWAP            →  SWAP             [autorivoltabile]
 *
 *  Operazioni NON invertibili in aritmetica intera:
 *    MODEQ   (%= n)  →  impossibile recuperare il quoziente perso
 *    EXPEQ   (^= n)  →  richiederebbe radice n-esima intera
 * ====================================================================== */

void op_pusheq_inv(VM *vm, const char *frame_name) { op_mineq (vm, frame_name); }
void op_mineq_inv (VM *vm, const char *frame_name) { op_pusheq(vm, frame_name); }
void op_prodeq_inv(VM *vm, const char *frame_name) { op_diveq (vm, frame_name); }
void op_diveq_inv (VM *vm, const char *frame_name) { op_prodeq(vm, frame_name); }
void op_swap_inv  (VM *vm, const char *frame_name) { op_swap  (vm, frame_name); }

void op_modeq_inv(VM *vm, const char *frame_name)
{
    (void)vm; (void)frame_name;
    perror("[VM] UNCALL: MODEQ non è invertibile (informazione persa)\n");
}

void op_expeq_inv(VM *vm, const char *frame_name)
{
    (void)vm; (void)frame_name;
    perror("[VM] UNCALL: EXPEQ non è invertibile in aritmetica intera\n");
}