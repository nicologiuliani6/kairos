// maze_backtrack.c — risolutore di labirinto con backtracking, animato via
// printf: ad ogni passo pulisce lo schermo (\033[2J) e ristampa la griglia col
// percorso corrente, così si vede il cammino avanzare e ritrarsi dai vicoli
// ciechi.
//
// Legenda:  '#' muro      (spazio) cella libera
//           '@' percorso corrente  'x' cella abbandonata (vicolo cieco)
//           'G' goal (in basso-dx); lo start è in alto-sx.
//
// DFS iterativa con stack esplicito del percorso (`sy`/`sx`). Il backtracking
// usa try/rollback per la mossa TENTATIVA di ogni vicino:
//
//   try (ok == 1) {                 // ok valutato DOPO il corpo
//       prev = grid[ny][nx];        // ricorda cosa c'era
//       grid[ny][nx] = '@';         // rivendica la cella
//       sy[top]=ny; sx[top]=nx; top++;   // spingi sullo stack
//       ok = (prev == ' ');         // commit solo se era LIBERA
//   } rollback { }                  // se non libera: marcatura e push
//                                   //   annullati dall'inversione automatica
//
// Quando una cella era libera (prev==' ') la condizione `ok==1` è vera → la
// mossa resta (commit). Quando il vicino è un muro/già visitato la condizione è
// falsa → la rollback ANNULLA da sola la marcatura '@' e la push sullo stack:
// nessun ripristino manuale. Quando da una cella non si avanza in nessuna
// direzione è un vicolo cieco: si fa pop dallo stack e la si marca 'x' (così non
// viene riprovata), e il frame successivo mostra il percorso che si accorcia.
//
// Esegui con --native-arith (mul/div/mod O(1)): l'animazione è più fluida.
// mnemo-main-argc: 0

#include <stdio.h>

#define H 7
#define W 9

char grid[H][W];

int dy[4] = {-1, 1, 0, 0};   // su, giù, sx, dx
int dx[4] = {0, 0, -1, 1};

int sy[H * W], sx[H * W];     // stack del percorso (coppie di celle)
int top = 0;

int gy = H - 1;              // goal in basso-dx
int gx = W - 1;
int frame = 0;

void draw(void) {
    printf("\033[2J\033[H");                         // pulisci schermo + home
    printf("maze backtracking  frame=%d  path-len=%d\n\n", frame, top);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            char ch = grid[y][x];
            if (y == gy && x == gx && ch == ' ')     // mostra il goal finché
                ch = 'G';                            //   non lo si raggiunge
            putchar(ch);
        }
        putchar('\n');
    }
}

int main(void) {
    const char *rows[H] = {
        "         ",
        " ### ### ",
        " #   #   ",
        " # ### # ",
        "   #   # ",
        " # # # # ",
        " #   #   ",
    };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            grid[y][x] = rows[y][x];

    sy[0] = 0; sx[0] = 0; top = 1;
    grid[0][0] = '@';                                // marca lo start
    frame = 0;
    draw();

    int done = 0;
    for (int step = 0; step < 400 && !done; step++) {
        int cy = sy[top - 1], cx = sx[top - 1];      // testa del percorso
        if (cy == gy && cx == gx) {
            done = 1;
        } else {
            int advanced = 0;
            for (int d = 0; d < 4 && !advanced; d++) {
                int ny = cy + dy[d], nx = cx + dx[d];
                int inb = (ny >= 0 && ny < H && nx >= 0 && nx < W);
                if (inb && grid[ny][nx] != '#') {
                    int ok = 0;
                    try (ok == 1) {
                        char prev = grid[ny][nx];
                        grid[ny][nx] = '@';
                        sy[top] = ny; sx[top] = nx; top += 1;
                        ok = (prev == ' ');          // commit solo se libera
                    } rollback {
                        // vicino già visitato ('@'/'x'): marcatura e push
                        // annullate dall'inversione → prova la prossima direzione
                    }
                    if (ok) advanced = 1;
                }
            }
            if (!advanced) {                         // vicolo cieco: backtrack
                top -= 1;
                grid[cy][cx] = 'x';
            }
            frame += 1;
            draw();
        }
    }

    if (done)
        printf("\nSOLVED  path-len=%d\n", top);
    else
        printf("\nNO PATH\n");
    return top;
}
