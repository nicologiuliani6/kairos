// sat_solver.c — mini SAT solver con il backtracking integrato (try/rollback).
//
// Formula CNF su 3 variabili a, b, c:
//     (a ∨ b) ∧ (¬a ∨ c) ∧ (¬b ∨ ¬c) ∧ (a ∨ c)
//
// Cerchiamo un'assegnazione che soddisfi TUTTE le clausole. Ogni candidato è
// codificato nei 3 bit di `assign` (0..7). Per ciascuno il body:
//   1. decodifica i bit in a, b, c;
//   2. conta quante clausole sono soddisfatte in `sat`.
// La condizione di commit è `sat == 4` (tutte e 4 le clausole vere).
//
// Perché il backtracking integrato è comodo qui:
//   valutare un candidato MUTA molte celle (a, b, c, sat e i temporanei delle
//   clausole). Su un candidato che NON soddisfa la formula bisogna riportare
//   tutto allo stato iniziale per provare il prossimo. Con try/rollback si
//   scrive solo la valutazione IN AVANTI: la rollback annulla AUTOMATICAMENTE
//   l'intera valutazione (a, b, c, sat tornano a 0) — niente reset manuale,
//   niente rischio di dimenticare una cella. È l'inverso esatto del body.

#include <stdio.h>

int main(void) {
    int assign = 0;
    int a = 0, b = 0, c = 0;
    int sat = 0;

    while (sat != 4 && assign < 8) {
        try (sat == 4) {
            // candidato: bit 0..2 di assign
            a = assign & 1;
            b = (assign >> 1) & 1;
            c = (assign >> 2) & 1;
            // conta le clausole soddisfatte
            if (a || b)   sat += 1;     // (a ∨ b)
            if (!a || c)  sat += 1;     // (¬a ∨ c)
            if (!b || !c) sat += 1;     // (¬b ∨ ¬c)
            if (a || c)   sat += 1;     // (a ∨ c)
        } rollback {
            // candidato insoddisfacente: a, b, c, sat già ripristinati
            // dall'inversione automatica → passa al prossimo
            assign += 1;
        }
    }

    if (sat == 4)
        printf("SAT: a=%d b=%d c=%d (assign=%d)\n", a, b, c, assign);
    else
        printf("UNSAT\n");

    return assign;     // soluzione attesa: a=1 b=0 c=1, assign=5
}
