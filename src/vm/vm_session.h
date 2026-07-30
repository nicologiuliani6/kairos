#ifndef VM_SESSION_H
#define VM_SESSION_H

/* ======================================================================
 *  Disciplina di sessione sui canali — CONTROLLO DINAMICO
 *
 *  Tutta la disciplina di sessione sta qui dentro e in nessun altro posto.
 *  Fuori da questo file esistono soltanto:
 *    - il campo `SessionState session` dentro Channel (vm_types.h);
 *    - una chiamata per punto d'uso in vm_ops.h (ssend, srecv, channel-ref);
 *    - session_par_enter / session_par_exit in vm_par.h.
 *  Cambiare la disciplina, o toglierla, non deve richiedere di leggere altro.
 *
 *  COSA GARANTISCE
 *
 *  Un canale ha, in ogni istante, al piu' DUE titolari: e' una sessione
 *  binaria. La titolarita' e' stato a RUNTIME, non una proprieta' del testo
 *  del programma, e puo' cambiare durante l'esecuzione: e' la forma
 *  *dinamica* del vincolo, contro quella *statica* in cui un canale
 *  appartiene agli stessi due rami per tutta la vita.
 *
 *  Un thread diventa titolare quando usa il canale (ssend/srecv) oppure
 *  quando lo riceve come payload di un altro canale. Smette di esserlo
 *  quando lo spedisce a qualcun altro. Da qui discendono, senza casi
 *  speciali, i due modi di passare un canale:
 *
 *    - NAME PASSING (stile pi-calcolo): il mittente crea un canale privato,
 *      lo manda e continua a usarlo per ricevere la risposta. Cede la
 *      titolarita' spedendolo e se la riprende al primo uso successivo: i
 *      titolari finiscono per essere il mittente e il destinatario, che e'
 *      esattamente quel che serve. E' il caso di examples/pi_calculus.kairos.
 *
 *    - DELEGA vera: il canale e' fra A e B, A lo passa a C e smette di
 *      usarlo. Titolari: B e C. Se A lo riusasse sarebbe un terzo titolare,
 *      e viene fermato. E' la condizione "smetti di usarlo prima che l'altro
 *      cominci", che qui non e' un obbligo dichiarato ma una conseguenza
 *      del contare al piu' due titolari alla volta.
 *
 *  Perche' dinamico e non statico: leggendo il testo si puo' contare quanti
 *  rami nominano un canale, ma non si puo' seguire un canale che cambia
 *  titolare, perche' il nuovo titolare lo riceve sotto un altro nome, magari
 *  in un'altra procedura. Il runtime lo segue senza sforzo, perche' vede
 *  l'identita' del Channel e non il nome della variabile.
 *
 *  EPOCHE
 *
 *  I titolari sono thread, e i thread di un `par` muoiono quando il blocco
 *  finisce. Uno stesso `par` rieseguito (tipicamente call e poi uncall) crea
 *  thread nuovi, e i vecchi titolari sono defunti. Invece di inseguirli, si
 *  marca ogni titolarita' con l'epoca in cui e' nata: all'uscita dal `par`
 *  piu' esterno l'epoca avanza e le titolarita' precedenti smettono di
 *  valere. Fuori da un `par` non c'e' concorrenza, quindi non c'e' nulla da
 *  sorvegliare. I `par` annidati NON fanno avanzare l'epoca: la titolarita'
 *  vale per tutto il blocco piu' esterno.
 * ====================================================================== */

#include <pthread.h>
#include "vm_types.h"
#include "vm_panic.h"

#define SESSION_MAX_OWNERS 2   /* sessione binaria: due, per definizione */

/* Profondita' di annidamento dei `par` ed epoca corrente. Le tocca solo il
   thread che entra o esce da un par, mai due thread insieme. */
static volatile unsigned g_session_par_depth = 0;
static volatile unsigned g_session_epoch     = 1;

/* ponytail: un solo lock globale per tutto lo stato di sessione, invece del
   mutex di ciascun canale. Lo stato e' minuscolo e lo si tocca una volta per
   comunicazione, quindi la contesa e' irrilevante; in cambio si evita di
   annidare il mutex di un canale dentro quello di un altro, che e' esattamente
   cio' che succederebbe ricevendo un canale come payload (si tiene gia' il
   mutex del canale di trasporto). */
static pthread_mutex_t g_session_mtx = PTHREAD_MUTEX_INITIALIZER;

static inline void session_par_enter(void)
{
    g_session_par_depth++;
}

static inline void session_par_exit(void)
{
    if (g_session_par_depth > 0) g_session_par_depth--;
    if (g_session_par_depth == 0) g_session_epoch++;   /* le titolarita' scadono */
}

/* ---------------------------------------------------------------------- */

/* Scarta i titolari di epoche passate: thread ormai terminati. */
static inline void session_purge(SessionState *s)
{
    int w = 0;
    for (int i = 0; i < s->owner_n; i++)
        if (s->owner[i].epoch == g_session_epoch)
            s->owner[w++] = s->owner[i];
    s->owner_n = w;
}

static inline int session_is_owner(const SessionState *s, pthread_t t)
{
    for (int i = 0; i < s->owner_n; i++)
        if (pthread_equal(s->owner[i].t, t)) return 1;
    return 0;
}

/* Registra il thread corrente come titolare, oppure ferma l'esecuzione se
   il canale ne ha gia' due. `come` descrive il modo in cui il thread e'
   arrivato al canale, e serve solo a rendere leggibile la diagnosi. */
static inline void session_claim(Channel *ch, const char *name, const char *come)
{
    if (!ch) return;
    pthread_t me = pthread_self();
    SessionState *s = &ch->session;

    pthread_mutex_lock(&g_session_mtx);
    session_purge(s);

    if (session_is_owner(s, me)) { pthread_mutex_unlock(&g_session_mtx); return; }

    if (s->owner_n >= SESSION_MAX_OWNERS) {
        pthread_mutex_unlock(&g_session_mtx);
        vm_debug_panic(
            "[VM] SESSIONE: il channel '%s' ha gia' due titolari, un terzo thread non "
            "puo' %s (la sessione e' binaria); se il canale e' stato passato a un altro "
            "thread, chi lo cede deve smettere di usarlo prima che il nuovo cominci\n",
            name, come);
    }

    s->owner[s->owner_n].t     = me;
    s->owner[s->owner_n].epoch = g_session_epoch;
    s->owner_n++;
    pthread_mutex_unlock(&g_session_mtx);
}

/* Prima di ogni ssend/srecv sul canale. */
static inline void session_acquire(Channel *ch, const char *name)
{
    session_claim(ch, name, "usarlo");
}

/* Il thread ha ricevuto il canale come payload di un altro canale. */
static inline void session_delegate_acquire(Channel *ch, const char *name)
{
    session_claim(ch, name, "riceverlo");
}

/* Il thread spedisce il canale a qualcun altro: cede la titolarita'. Se poi
   lo riusa se la riprende, ammesso che nel frattempo non l'abbiano presa in
   due: e' quel che distingue il name passing dalla delega vera. */
static inline void session_delegate_release(Channel *ch, const char *name)
{
    (void)name;
    if (!ch) return;
    pthread_t me = pthread_self();
    SessionState *s = &ch->session;

    pthread_mutex_lock(&g_session_mtx);
    session_purge(s);
    int w = 0;
    for (int i = 0; i < s->owner_n; i++)
        if (!pthread_equal(s->owner[i].t, me)) s->owner[w++] = s->owner[i];
    s->owner_n = w;
    pthread_mutex_unlock(&g_session_mtx);
}

#endif /* VM_SESSION_H */
