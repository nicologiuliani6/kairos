#ifndef CHAR_ID_MAP_H
#define CHAR_ID_MAP_H

#include <stddef.h>

#define CHAR_ID_MAP_SIZE 256

typedef struct {
    int map[CHAR_ID_MAP_SIZE];
    int next_id;
} CharIdMap;

/**
 * Inizializza la struttura
 */
static inline void char_id_map_init(CharIdMap *m) {
    for (int i = 0; i < CHAR_ID_MAP_SIZE; i++) {
        m->map[i] = -1;
    }
    m->next_id = 0;
}

/**
 * Restituisce l'id associato al char.
 * Se il char non è mai stato visto, assegna un nuovo id.
 */
static inline int char_id_map_get(CharIdMap *m, char c) {
    unsigned char uc = (unsigned char)c;

    if (m->map[uc] == -1) {
        m->map[uc] = m->next_id++;
    }

    return m->map[uc];
}

/**
 * Controlla se un char è già stato visto
 */
static inline int char_id_map_exists(CharIdMap *m, char c) {
    return m->map[(unsigned char)c] != -1;
}

/**
 * Reset della mappa
 */
static inline void char_id_map_reset(CharIdMap *m) {
    char_id_map_init(m);
}

#endif // CHAR_ID_MAP_H
