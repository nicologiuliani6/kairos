#ifndef CHAR_ID_MAP_H
#define CHAR_ID_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* ----------------------------------------------------------------------
 *  CharIdMap — nome -> id, senza dimensioni fissate in anticipo
 *
 *  Ogni nome riceve un id progressivo (0, 1, 2, ...) che non cambia piu'.
 *  Ne' il numero di nomi ne' la loro lunghezza hanno un massimo: i nomi sono
 *  copiati sull'heap della loro lunghezza, il vettore delle voci e la tabella
 *  hash crescono raddoppiando.
 *
 *  Lettori senza lock. La mappa e' letta da piu' thread senza esclusione
 *  (char_id_map_lookup, get_findex, clone_frame_for_thread): una voce, una volta
 *  pubblicata, non cambia piu'. Per questo, quando il vettore o la tabella
 *  crescono, non si fa realloc: si costruisce una copia piu' grande, la si
 *  pubblica con un'unica scrittura atomica e il vecchio blocco resta vivo
 *  (agganciato a `prev`) finche' la mappa non viene distrutta. Un lettore che
 *  stava ancora scorrendo il blocco vecchio trova memoria valida e voci
 *  complete. Lo spreco e' al piu' quello di una serie geometrica: meno del
 *  doppio dell'ultimo blocco.
 *
 *  Ordine di pubblicazione di una voce nuova: prima il nome nel vettore, poi lo
 *  slot nella tabella, per ultimo il conteggio, tutti con release. Il lettore
 *  li legge nell'ordine opposto con acquire, quindi vede o una voce completa o
 *  nessuna voce, mai una a meta'.
 *
 *  Scrittori. Gli inserimenti passano per un mutex unico (char_id_map_ins_mtx),
 *  preso solo nel caso raro della voce mancante: chi trova il nome non lo tocca.
 *  E' un mutex interno, annidato dentro var_indexer_mtx quando il chiamante
 *  tiene gia' quello, mai il contrario.
 * ---------------------------------------------------------------------- */

typedef struct CharIdVec {
    struct CharIdVec *prev;   /* blocco sostituito: resta vivo fino a destroy */
    int        cap;
    uint32_t  *hash;
    char     **name;
} CharIdVec;

typedef struct CharIdTab {
    struct CharIdTab *prev;   /* idem */
    uint32_t  mask;           /* dimensione - 1, dimensione potenza di due */
    int32_t  *slot;           /* 0 = libero, altrimenti id + 1 */
} CharIdTab;

typedef struct {
    CharIdVec *vec;
    CharIdTab *tab;
    int        count;   /* voci visibili */
} CharIdMap;

extern pthread_mutex_t char_id_map_ins_mtx;

static inline uint32_t char_id_hash(const char *s)
{
    uint32_t h = 2166136261u;               /* FNV-1a */
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static inline uint32_t char_id_slot0(uint32_t h, uint32_t mask)
{
    return (h ^ (h >> 15)) & mask;
}

static inline char *char_id_strdup(const char *s)
{
    size_t l = strlen(s) + 1;
    char *d = (char *)malloc(l);
    if (!d) { fprintf(stderr, "[VM] CharIdMap: memoria esaurita\n"); exit(1); }
    memcpy(d, s, l);
    return d;
}

static inline CharIdVec *char_id_vec_new(int cap)
{
    /* Un solo blocco: intestazione, puntatori ai nomi, hash. */
    CharIdVec *v = (CharIdVec *)malloc(sizeof(CharIdVec)
                                       + sizeof(char *)   * (size_t)cap
                                       + sizeof(uint32_t) * (size_t)cap);
    if (!v) { fprintf(stderr, "[VM] CharIdMap: memoria esaurita\n"); exit(1); }
    v->prev = NULL;
    v->cap  = cap;
    v->name = (char **)(v + 1);
    v->hash = (uint32_t *)(v->name + cap);
    return v;
}

static inline CharIdTab *char_id_tab_new(uint32_t size)
{
    CharIdTab *t = (CharIdTab *)malloc(sizeof(CharIdTab) + sizeof(int32_t) * (size_t)size);
    if (!t) { fprintf(stderr, "[VM] CharIdMap: memoria esaurita\n"); exit(1); }
    t->prev = NULL;
    t->mask = size - 1;
    t->slot = (int32_t *)(t + 1);
    memset(t->slot, 0, sizeof(int32_t) * (size_t)size);
    return t;
}

/* Tabella nuova con le voci [0, n) del vettore. Non ancora pubblicata: scritture
   semplici. */
static inline CharIdTab *char_id_tab_build(const CharIdVec *v, int n, uint32_t size)
{
    while ((uint32_t)n * 2 > size) size *= 2;
    CharIdTab *t = char_id_tab_new(size);
    for (int id = 0; id < n; id++) {
        uint32_t i = char_id_slot0(v->hash[id], t->mask);
        while (t->slot[i]) i = (i + 1) & t->mask;
        t->slot[i] = id + 1;
    }
    return t;
}

/**
 * Inizializza la struttura. La mappa deve essere azzerata o gia' valida: se
 * contiene voci le libera (un frame riusato, l'indice dei frame di un run
 * precedente).
 */
static inline void char_id_map_destroy(CharIdMap *m)
{
    if (m->vec) {
        for (int i = 0; i < m->count; i++) free(m->vec->name[i]);
    }
    for (CharIdVec *v = m->vec; v; ) { CharIdVec *p = v->prev; free(v); v = p; }
    for (CharIdTab *t = m->tab; t; ) { CharIdTab *p = t->prev; free(t); t = p; }
    memset(m, 0, sizeof(*m));
}

static inline void char_id_map_init(CharIdMap *m)
{
    char_id_map_destroy(m);
}

/**
 * Controlla se una stringa è già stata vista.
 * Restituisce -1 se mancante (così il chiamante può evitare doppia scansione).
 * Nessun lock: vedi il commento in testa al file.
 */
static inline int char_id_map_lookup_h(CharIdMap *m, const char *name, uint32_t h)
{
    CharIdTab *t = __atomic_load_n(&m->tab, __ATOMIC_ACQUIRE);
    if (!t) return -1;
    CharIdVec *v = NULL;
    for (uint32_t i = char_id_slot0(h, t->mask);; i = (i + 1) & t->mask) {
        int32_t s = __atomic_load_n(&t->slot[i], __ATOMIC_ACQUIRE);
        if (s == 0) return -1;
        int id = s - 1;
        /* Il vettore si rilegge dopo lo slot: una voce inserita dopo una
           crescita sta solo nel vettore nuovo. */
        if (!v || id >= v->cap) v = __atomic_load_n(&m->vec, __ATOMIC_ACQUIRE);
        if (v->hash[id] == h && strcmp(v->name[id], name) == 0 &&
            id < __atomic_load_n(&m->count, __ATOMIC_ACQUIRE))
            return id;
    }
}

static inline int char_id_map_lookup(CharIdMap *m, const char *name)
{
    return char_id_map_lookup_h(m, name, char_id_hash(name));
}

/* Inserimento: chiamare con char_id_map_ins_mtx preso e il nome assente. */
static inline int char_id_map_insert_locked(CharIdMap *m, const char *name, uint32_t h)
{
    int id = m->count;
    CharIdVec *v = m->vec;
    if (!v || id >= v->cap) {
        CharIdVec *nv = char_id_vec_new(v ? v->cap * 2 : 16);
        if (v) {
            memcpy(nv->name, v->name, sizeof(char *)   * (size_t)id);
            memcpy(nv->hash, v->hash, sizeof(uint32_t) * (size_t)id);
        }
        nv->prev = v;
        __atomic_store_n(&m->vec, nv, __ATOMIC_RELEASE);
        v = nv;
    }
    v->name[id] = char_id_strdup(name);
    __atomic_store_n(&v->hash[id], h, __ATOMIC_RELEASE);

    CharIdTab *t = m->tab;
    if (!t || (uint32_t)(id + 1) * 2 > t->mask + 1) {
        CharIdTab *nt = char_id_tab_build(v, id + 1, t ? (t->mask + 1) * 2 : 32);
        nt->prev = t;
        __atomic_store_n(&m->tab, nt, __ATOMIC_RELEASE);
    } else {
        uint32_t i = char_id_slot0(h, t->mask);
        while (t->slot[i]) i = (i + 1) & t->mask;
        __atomic_store_n(&t->slot[i], id + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&m->count, id + 1, __ATOMIC_RELEASE);
    return id;
}

/**
 * Restituisce l'id associato alla stringa.
 * Se la stringa non è mai stata vista, assegna un nuovo id.
 */
static inline int char_id_map_get(CharIdMap *m, const char *name)
{
    uint32_t h = char_id_hash(name);
    int id = char_id_map_lookup_h(m, name, h);
    if (id >= 0) return id;
    pthread_mutex_lock(&char_id_map_ins_mtx);
    id = char_id_map_lookup_h(m, name, h);   /* ricontrollo sotto lock */
    if (id < 0) id = char_id_map_insert_locked(m, name, h);
    pthread_mutex_unlock(&char_id_map_ins_mtx);
    return id;
}

static inline int char_id_map_exists(CharIdMap *m, const char *name)
{
    return char_id_map_lookup(m, name) >= 0;
}

/* Copia profonda: la copia ha vettore, tabella e nomi suoi, e da qui in poi
   cresce per conto proprio (un frame clonato inserisce i suoi LOCAL senza
   toccare il frame base). La sorgente si legge col protocollo dei lettori,
   quindi non serve lock. */
static inline void char_id_map_copy(CharIdMap *dst, CharIdMap *src)
{
    memset(dst, 0, sizeof(*dst));
    int n = __atomic_load_n(&src->count, __ATOMIC_ACQUIRE);
    if (n <= 0) return;
    CharIdVec *sv = __atomic_load_n(&src->vec, __ATOMIC_ACQUIRE);
    int cap = 16;
    while (cap < n) cap *= 2;
    CharIdVec *v = char_id_vec_new(cap);
    for (int i = 0; i < n; i++) {
        v->name[i] = char_id_strdup(sv->name[i]);
        v->hash[i] = sv->hash[i];
    }
    dst->vec   = v;
    dst->tab   = char_id_tab_build(v, n, 32);
    dst->count = n;
}

/* Nome della voce `id` (valida se id < count). */
static inline const char *char_id_map_name(CharIdMap *m, int id)
{
    CharIdVec *v = __atomic_load_n(&m->vec, __ATOMIC_ACQUIRE);
    return v->name[id];
}

/**
 * Reset della mappa
 */
static inline void char_id_map_reset(CharIdMap *m)
{
    char_id_map_init(m);
}

#endif // CHAR_ID_MAP_H
