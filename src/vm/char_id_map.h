#ifndef CHAR_ID_MAP_H
#define CHAR_ID_MAP_H

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CHAR_ID_MAP_SIZE 256
#define CHAR_ID_MAP_NAME_LEN 128

typedef struct {
    char names[CHAR_ID_MAP_SIZE][CHAR_ID_MAP_NAME_LEN];
    int  count;
} CharIdMap;

/**
 * Inizializza la struttura
 */
static inline void char_id_map_init(CharIdMap *m) {
    m->count = 0;
    for (int i = 0; i < CHAR_ID_MAP_SIZE; i++)
        m->names[i][0] = '\0';
}

/**
 * Restituisce l'id associato alla stringa.
 * Se la stringa non è mai stata vista, assegna un nuovo id.
 * NOTA: il parametro è ora const char* invece di char
 */
static inline int char_id_map_get(CharIdMap *m, const char *name) {
    /* Hot path: prefisso check rapido sul primo char prima di strcmp completo. */
    char c0 = name[0];
    for (int i = 0; i < m->count; i++) {
        if (m->names[i][0] == c0 && strcmp(m->names[i], name) == 0)
            return i;
    }
    if (m->count >= CHAR_ID_MAP_SIZE) {
        fprintf(stderr, "[VM] CHAR_ID_MAP overflow (%d) on '%s'\n", CHAR_ID_MAP_SIZE, name);
        exit(1);
    }
    int id = m->count++;
    strncpy(m->names[id], name, CHAR_ID_MAP_NAME_LEN - 1);
    m->names[id][CHAR_ID_MAP_NAME_LEN - 1] = '\0';
    return id;
}

/**
 * Controlla se una stringa è già stata vista.
 * Restituisce -1 se mancante (così il chiamante può evitare doppia scansione).
 */
static inline int char_id_map_lookup(CharIdMap *m, const char *name) {
    char c0 = name[0];
    for (int i = 0; i < m->count; i++) {
        if (m->names[i][0] == c0 && strcmp(m->names[i], name) == 0)
            return i;
    }
    return -1;
}

static inline int char_id_map_exists(CharIdMap *m, const char *name) {
    return char_id_map_lookup(m, name) >= 0;
}

/**
 * Reset della mappa
 */
static inline void char_id_map_reset(CharIdMap *m) {
    char_id_map_init(m);
}

#endif // CHAR_ID_MAP_H