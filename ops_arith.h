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
    if(resolve_value(vm, Findex, C_value) == 0) {
        //printf("[WARING] Prodotto per 0!\n");
        //exit(EXIT_FAILURE);
    }
    *(v->value) -= resolve_value(vm, Findex, C_value);
}

/* ======================================================================
 *  Inverse per UNCALL
 *
 *  Operazioni invertibili:
 *    PUSHEQ  (+= n)  →  MINEQ  (-= n)
 *    MINEQ   (-= n)  →  PUSHEQ (+= n)
 *    SWAP            →  SWAP             [autorivoltabile]
 *
 * ====================================================================== */

void op_pusheq_inv(VM *vm, const char *frame_name) { op_mineq (vm, frame_name); }
void op_mineq_inv (VM *vm, const char *frame_name) { op_pusheq(vm, frame_name); }
void op_swap_inv  (VM *vm, const char *frame_name) { op_swap  (vm, frame_name); }
