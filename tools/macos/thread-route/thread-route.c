// ===========================================================================
//  thread-route : garde la route du reseau Thread sur un Mac
//
//  Le Mac joint les noeuds Thread (le pont Halo) par le prefixe OMR que les
//  routeurs de bordure (HomePod, Apple TV) annoncent sur le LAN (option RIO).
//  Bug du noyau de macOS (docs/PROTOCOLE-JSON.md, 10.1) : il retire la route
//  de ce prefixe quand il change de routeur (l'un d'eux parait un instant
//  injoignable, ou son annonce expire : defrouter_delreq), mais sa liste des
//  routes annoncees peut garder une entree marquee posee (NDDRF_INSTALLED)
//  sans route dans la table : il ne la remet plus tant que ce routeur reste
//  joignable, et le Mac perd le reseau Thread.
//
//  Ce demon, lance par launchd en root, relit cette liste (sysctl
//  net.inet6.icmp6.nd6_rtilist) a chaque message du noyau sur les routes, et
//  toutes les 10 s : un voisin perdu (echec de la detection d'injoignabilite)
//  ne donne lieu a aucun message. Pour chaque prefixe ULA /64 annonce :
//    - aucune route : il en pose une, statique, marquee RTF_PROTO1, via un
//      routeur de bordure joignable ;
//    - notre route passe par un routeur parti, ou injoignable a deux passages
//      (2 s au moins) alors qu'un autre est joignable : il en change, sur
//      place (route change) ;
//    - plus aucune annonce : il retire notre route.
//  Une route statique n'est pas a l'abri : le noyau la retire aussi (par
//  prefixe et masque) quand il change de routeur ou qu'une annonce expire, et
//  pose alors en general la sienne ; sinon, le demon remet la notre.
//  Il ne touche a aucune autre route (celles du noyau, celles posees a la
//  main), ni a aucun prefixe hors de fc00::/7. Aucune entree hors du noyau :
//  ni port reseau, ni fichier de commande. Les changements passent par
//  /sbin/route (argv fixe, sans shell). A l'arret (launchctl bootout), il
//  retire les routes qu'il a posees.
//
//  Options : -n essai (dit ce qu'il ferait, sans rien changer ; pas besoin
//  d'etre root), -1 un seul passage, -v detaille chaque passage.
// ===========================================================================
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "logique.h"

#define HR_MAX_PREFIXES 32
#define HR_PERIODE_S 10            // passage de controle
#define HR_ATTENTE_EVENEMENT_MS 300  // un message du noyau : passage peu apres (rafales fondues)
#define HR_CHANGEMENTS_MAX 6       // par prefixe et par minute, au-dela : on attend
#define HR_LOT_MAX 256             // messages lus d'affilee, au plus (un flot sans fin ne bloque rien)
#define HR_ROUTE "/sbin/route"

static volatile sig_atomic_t sFin = 0;
static bool sEssai = false, sDetail = false;
static int sSockNd = -1, sSockGet = -1;
static unsigned sSeq = 0;  // rtm_seq : 1..INT_MAX, puis on reprend

// Prefixes pour lesquels nous avons pose une route (a retirer s'ils ne sont
// plus annonces, et a l'arret).
static struct {
  bool utilise;
  struct in6_addr prefixe;
  int64_t fenetre;          // debut de la minute en cours (ms, horloge monotone)
  unsigned changements;     // dans cette minute
  bool attenteNotee;        // "attente" deja au journal pour cette minute
  hr_suspicion suspicion;   // notre routeur semble injoignable : a confirmer
} sNotres[HR_MAX_PREFIXES];

// Etats deja au journal (une ligne au changement d'etat, pas a chaque passage).
static bool sIllisible = false;    // liste des routes annoncees illisible
static bool sTronquee = false;     // liste tronquee : nos routes ne sont plus retirees
static bool sPleineNotee = false;  // sNotres pleine : un changement refuse

// ---------------------------------------------------------------------------
//  Journal (stderr : launchd le range dans /Library/Logs/fr.djoko.thread.route.log)
// ---------------------------------------------------------------------------

__attribute__((format(printf, 1, 2))) static void journal(const char *fmt, ...) {
  char quand[32];
  const time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(quand, sizeof(quand), "%Y-%m-%d %H:%M:%S", &tm);
  fprintf(stderr, "%s thread-route : ", quand);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}

static int64_t maintenant_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *texte_adresse(const struct in6_addr *a, char *buf, size_t n) {
  return inet_ntop(AF_INET6, a, buf, (socklen_t)n) ? buf : "?";
}

// Portee incorporee des adresses de lien local (convention KAME des messages
// du noyau) : retiree pour comparer, remise pour interroger le noyau.
static void sans_portee(struct in6_addr *a) {
  if (IN6_IS_ADDR_LINKLOCAL(a)) a->s6_addr[2] = a->s6_addr[3] = 0;
}
static void avec_portee(struct in6_addr *a, unsigned itf) {
  if (IN6_IS_ADDR_LINKLOCAL(a)) {
    a->s6_addr[2] = (uint8_t)(itf >> 8);
    a->s6_addr[3] = (uint8_t)itf;
  }
}

// ---------------------------------------------------------------------------
//  Lectures du noyau (aucune ne demande d'etre root)
// ---------------------------------------------------------------------------

static int etat_voisin(const struct in6_addr *adresse, unsigned itf) {
  struct in6_nbrinfo nbi;
  memset(&nbi, 0, sizeof(nbi));
  if (!if_indextoname(itf, nbi.ifname)) return HR_ND_ABSENT;
  nbi.addr = *adresse;
  avec_portee(&nbi.addr, itf);
  if (ioctl(sSockNd, SIOCGNBRINFO_IN6, &nbi) < 0) return HR_ND_ABSENT;
  return nbi.state;
}

// Interface montee et en marche (IFF_UP et IFF_RUNNING) ; false si illisible.
static bool interface_active(unsigned itf) {
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  if (!if_indextoname(itf, ifr.ifr_name)) return false;
  if (ioctl(sSockNd, SIOCGIFFLAGS, &ifr) < 0) return false;
  return (ifr.ifr_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING);
}

// Liste des routes annoncees (RIO) du noyau ; seuls les prefixes geres sont
// gardes, au plus max. Rend leur nombre, -1 en cas d'echec. *tronquee : un
// prefixe gere a pu etre ecarte (plus de max, ou liste coupee).
static int lire_annonces(hr_annonce *out, int max, bool *tronquee) {
  *tronquee = false;
  int mib[4] = {CTL_NET, PF_INET6, IPPROTO_ICMPV6, ICMPV6CTL_ND6_RTILIST};
  size_t len = 0;
  if (sysctl(mib, 4, NULL, &len, NULL, 0) < 0) return -1;
  if (!len) return 0;
  len += 4096;  // la liste peut grandir entre les deux appels
  char *buf = malloc(len);
  if (!buf) return -1;
  if (sysctl(mib, 4, buf, &len, NULL, 0) < 0) {
    free(buf);
    return -1;
  }
  int n = 0;
  const char *p = buf, *fin = buf + len;
  while ((size_t)(fin - p) >= sizeof(struct in6_route_info)) {
    struct in6_route_info ri;
    memcpy(&ri, p, sizeof(ri));
    p += sizeof(ri);
    // Prefixe gere ou non avant toute question au noyau : les autres sont sautes.
    hr_annonce *a = NULL;
    if (hr_prefixe_gere(&ri.prefix, ri.prefixlen)) {
      if (n < max) {
        a = &out[n++];
        memset(a, 0, sizeof(*a));
        a->prefixe = ri.prefix;
        a->longueur = ri.prefixlen;
        a->complet = true;
      } else {
        *tronquee = true;
      }
    }
    for (unsigned i = 0; i < ri.defrtrs; i++) {
      if ((size_t)(fin - p) < sizeof(struct in6_defrouter)) {
        p = fin;  // liste coupee : on garde ce qui a ete lu
        *tronquee = true;
        if (a) a->complet = false;
        break;
      }
      struct in6_defrouter dr;
      memcpy(&dr, p, sizeof(dr));
      p += sizeof(dr);
      if (!a) continue;
      if (a->n >= HR_MAX_ROUTEURS) {
        a->complet = false;
        continue;
      }
      hr_routeur *r = &a->routeurs[a->n++];
      r->adresse = dr.rtaddr.sin6_addr;
      sans_portee(&r->adresse);
      r->interface = dr.if_index ? dr.if_index : dr.rtaddr.sin6_scope_id;
      r->installe = (dr.stateflags & NDDRF_INSTALLED) != 0;
      r->duree = dr.rtlifetime;
      r->etat_nd = r->interface ? etat_voisin(&r->adresse, r->interface) : HR_ND_ABSENT;
      r->active = r->interface && interface_active(r->interface);
    }
  }
  if (p != fin) *tronquee = true;  // reste illisible : des prefixes peuvent manquer
  free(buf);
  return n;
}

#define ARRONDI(a) ((a) > 0 ? (1 + (((a) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))

typedef struct {
  struct rt_msghdr h;
  char sa[512];
} message_route;

// Demande RTM_GET ; rend la reponse (rtm_errno compris), false si pas de
// reponse dans la seconde.
static bool demander(message_route *m, size_t len) {
  // Comme toute socket de routage, celle-ci recoit aussi tous les messages du
  // noyau : ceux arrives depuis la demande precedente sont ecartes (sinon ils
  // s'accumulent, et une file pleine perdrait notre reponse). Au plus
  // HR_LOT_MAX ; pas d'arret sur sFin : nettoyer() passe ici apres le signal.
  message_route jete;
  for (int k = 0; k < HR_LOT_MAX && recv(sSockGet, &jete, sizeof(jete), MSG_DONTWAIT) > 0; k++) {
  }
  m->h.rtm_msglen = (u_short)len;
  m->h.rtm_version = RTM_VERSION;
  m->h.rtm_type = RTM_GET;
  const int seq = (int)(sSeq++ % (unsigned)INT_MAX) + 1;
  m->h.rtm_seq = seq;
  // Echec de la recherche (ESRCH) : la reponse arrive quand meme, rtm_errno rempli.
  if (write(sSockGet, m, len) < 0 && errno != ESRCH) return false;
  const int64_t limite = maintenant_ms() + 1000;
  for (;;) {
    const int64_t reste = limite - maintenant_ms();
    if (reste <= 0) return false;
    fd_set lus;
    FD_ZERO(&lus);
    FD_SET(sSockGet, &lus);
    struct timeval tv = {(time_t)(reste / 1000), (suseconds_t)((reste % 1000) * 1000)};
    if (select(sSockGet + 1, &lus, NULL, NULL, &tv) < 0 && errno != EINTR) return false;
    const ssize_t r = recv(sSockGet, m, sizeof(*m), MSG_DONTWAIT);
    if (r < (ssize_t)sizeof(m->h)) continue;
    // Les messages d'autres processus (et du noyau) passent : on attend le notre.
    if (m->h.rtm_version == RTM_VERSION && m->h.rtm_type == RTM_GET && m->h.rtm_seq == seq &&
        m->h.rtm_pid == getpid())
      return true;
  }
}

static void ajouter_sa(message_route *m, size_t *len, const void *sa, size_t salen) {
  memcpy((char *)m + *len, sa, salen);
  *len += ARRONDI(salen);
}

// Route exacte prefixe/longueur dans la table. false : le noyau ne repond pas.
static bool lire_route(const struct in6_addr *prefixe, uint8_t longueur, hr_route *out) {
  memset(out, 0, sizeof(*out));
  message_route m;
  memset(&m, 0, sizeof(m));
  size_t len = sizeof(m.h);
  struct sockaddr_in6 dst, masque;
  memset(&dst, 0, sizeof(dst));
  dst.sin6_len = sizeof(dst);
  dst.sin6_family = AF_INET6;
  dst.sin6_addr = *prefixe;
  memset(&masque, 0, sizeof(masque));
  masque.sin6_len = sizeof(masque);
  masque.sin6_family = AF_INET6;
  for (uint8_t i = 0; i < longueur / 8 && i < 16; i++) masque.sin6_addr.s6_addr[i] = 0xFF;
  m.h.rtm_addrs = RTA_DST | RTA_NETMASK;
  m.h.rtm_flags = RTF_UP;
  ajouter_sa(&m, &len, &dst, sizeof(dst));
  ajouter_sa(&m, &len, &masque, sizeof(masque));
  if (!demander(&m, len)) return false;
  if (m.h.rtm_errno == ESRCH) return true;  // pas de route
  if (m.h.rtm_errno) return false;
  // Adresses de la reponse, dans l'ordre des bits.
  const char *cp = m.sa, *fin = (const char *)&m + (m.h.rtm_msglen < sizeof(m) ? m.h.rtm_msglen : sizeof(m));
  struct sockaddr_in6 rdst, rgw, rmasque;
  bool aDst = false, aGw = false, aMasque = false;
  memset(&rmasque, 0, sizeof(rmasque));
  for (int bit = 0; bit < RTAX_MAX && cp < fin; bit++) {
    if (!(m.h.rtm_addrs & (1 << bit))) continue;
    const struct sockaddr *sa = (const struct sockaddr *)cp;
    const size_t salen = sa->sa_len;
    if (cp + ARRONDI(salen) > fin) break;
    if (bit == RTAX_DST && sa->sa_family == AF_INET6 && salen >= sizeof(rdst)) {
      memcpy(&rdst, sa, sizeof(rdst));
      aDst = true;
    } else if (bit == RTAX_GATEWAY && sa->sa_family == AF_INET6 && salen >= sizeof(rgw)) {
      memcpy(&rgw, sa, sizeof(rgw));
      aGw = true;
    } else if (bit == RTAX_NETMASK) {
      // Masque parfois raccourci par le noyau (octets nuls de fin omis).
      memcpy(&rmasque, sa, salen < sizeof(rmasque) ? salen : sizeof(rmasque));
      aMasque = true;
    }
    cp += ARRONDI(salen);
  }
  // La reponse doit etre la route exacte du prefixe (pas une route plus large).
  if (!aDst || memcmp(&rdst.sin6_addr, prefixe, sizeof(*prefixe))) return true;
  if (aMasque) {
    for (unsigned i = 0; i < 16; i++) {
      const uint8_t attendu = i < longueur / 8 ? 0xFF : 0;
      if (rmasque.sin6_addr.s6_addr[i] != attendu) return true;
    }
  }
  out->existe = true;
  out->notre = (m.h.rtm_flags & RTF_STATIC) && (m.h.rtm_flags & RTF_PROTO1);
  out->interface = m.h.rtm_index;
  if (aGw) {
    out->passerelle = rgw.sin6_addr;
    sans_portee(&out->passerelle);
  }
  return true;
}

// Interface de la route IPv4 par defaut : l'interface principale du Mac (a
// rang egal, on prefere ses routeurs). 0 : inconnue.
static unsigned interface_principale(void) {
  message_route m;
  memset(&m, 0, sizeof(m));
  size_t len = sizeof(m.h);
  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_len = sizeof(dst);
  dst.sin_family = AF_INET;
  dst.sin_addr.s_addr = htonl(0x01010101);  // une adresse quelconque hors du LAN
  m.h.rtm_addrs = RTA_DST;
  m.h.rtm_flags = RTF_UP;
  ajouter_sa(&m, &len, &dst, sizeof(dst));
  return demander(&m, len) && !m.h.rtm_errno ? m.h.rtm_index : 0;
}

// ---------------------------------------------------------------------------
//  Changements (root) : /sbin/route, argv fixe, sans shell
// ---------------------------------------------------------------------------

static int lancer_route(char *const argv[]) {
  static char *const env[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", NULL};
  // Sortie normale ("add net ...") ecartee ; ses erreurs vont au journal.
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) return -1;
  posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t pid;
  const int e = posix_spawn(&pid, HR_ROUTE, &fa, NULL, argv, env);
  posix_spawn_file_actions_destroy(&fa);
  if (e != 0) return -1;
  int st = 0;
  while (waitpid(pid, &st, 0) < 0)
    if (errno != EINTR) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// /sbin/route sort avec 0 meme quand le noyau refuse (network_cmds : exit(0)
// apres newroute) : le resultat se lit dans la table.
static bool route_notre_via(const struct in6_addr *prefixe, const hr_routeur *via) {
  hr_route r;
  return lire_route(prefixe, 64, &r) && r.existe && r.notre && r.interface == via->interface &&
         !memcmp(&r.passerelle, &via->adresse, sizeof(r.passerelle));
}

// Arguments de /sbin/route : prefixe, et passerelle "adresse%interface".
typedef struct {
  char p[INET6_ADDRSTRLEN];
  char gw[INET6_ADDRSTRLEN + IFNAMSIZ + 2];
} arguments_route;

static bool preparer(const struct in6_addr *prefixe, const hr_routeur *via, arguments_route *t) {
  char a[INET6_ADDRSTRLEN], itf[IFNAMSIZ];
  if (!inet_ntop(AF_INET6, prefixe, t->p, sizeof(t->p)) || !inet_ntop(AF_INET6, &via->adresse, a, sizeof(a)) ||
      !if_indextoname(via->interface, itf))
    return false;
  snprintf(t->gw, sizeof(t->gw), "%s%%%s", a, itf);
  return true;
}

static bool route_ajouter(const struct in6_addr *prefixe, const hr_routeur *via) {
  arguments_route t;
  if (!preparer(prefixe, via, &t)) return false;
  char *const argv[] = {"route", "-n", "add", "-inet6", "-prefixlen", "64", "-proto1", t.p, t.gw, NULL};
  return lancer_route(argv) == 0 && route_notre_via(prefixe, via);
}

// Sur place (RTM_CHANGE : passerelle et interface, drapeaux gardes, sans trou).
// Meme course minime que route_retirer.
static bool route_changer(const struct in6_addr *prefixe, const hr_routeur *via) {
  arguments_route t;
  if (!preparer(prefixe, via, &t)) return false;
  char *const argv[] = {"route", "-n", "change", "-inet6", "-prefixlen", "64", t.p, t.gw, NULL};
  return lancer_route(argv) == 0 && route_notre_via(prefixe, via);
}

static bool route_retirer(const struct in6_addr *prefixe) {
  char p[INET6_ADDRSTRLEN];
  if (!inet_ntop(AF_INET6, prefixe, p, sizeof(p))) return false;
  // Course minime : le noyau peut poser la sienne entre notre RTM_GET et ce retrait (prefixe et masque seuls).
  char *const argv[] = {"route", "-n", "delete", "-inet6", "-prefixlen", "64", p, NULL};
  hr_route r;
  return lancer_route(argv) == 0 && lire_route(prefixe, 64, &r) && !(r.existe && r.notre);
}

// Sur place d'abord ; a defaut, retrait puis ajout, si la route est encore la notre.
static bool route_remplacer(const struct in6_addr *prefixe, const hr_routeur *via, const char *texte) {
  if (route_changer(prefixe, via)) return true;
  hr_route r;
  if (!lire_route(prefixe, 64, &r) || (r.existe && !r.notre)) return false;
  if (r.existe) {
    journal("route %s/64 : changement sur place refuse, retrait puis ajout", texte);
    if (!route_retirer(prefixe)) return false;
  }
  return route_ajouter(prefixe, via);
}

// ---------------------------------------------------------------------------
//  Passage
// ---------------------------------------------------------------------------

static int trouver_notre(const struct in6_addr *prefixe) {
  for (int i = 0; i < HR_MAX_PREFIXES; i++)
    if (sNotres[i].utilise && !memcmp(&sNotres[i].prefixe, prefixe, sizeof(*prefixe))) return i;
  return -1;
}

// Rend l'indice du prefixe dans sNotres, -1 : table pleine.
static int retenir(const struct in6_addr *prefixe) {
  const int i = trouver_notre(prefixe);
  if (i >= 0) return i;
  for (int j = 0; j < HR_MAX_PREFIXES; j++)
    if (!sNotres[j].utilise) {
      memset(&sNotres[j], 0, sizeof(sNotres[j]));
      sNotres[j].utilise = true;
      sNotres[j].prefixe = *prefixe;
      return j;
    }
  return -1;
}

static void liberer(int i) {
  sNotres[i].utilise = false;
  sPleineNotee = false;  // une place : un prochain refus sera note
}

static void oublier(const struct in6_addr *prefixe) {
  const int i = trouver_notre(prefixe);
  if (i >= 0) liberer(i);
}

// Au plus HR_CHANGEMENTS_MAX changements par prefixe et par minute (pas de
// va-et-vient sans fin si deux routeurs se relaient) ; apres un echec, plus
// d'essai avant la minute suivante. Une seule ligne de journal par attente.
// Prefixe impossible a suivre (table pleine) : aucun changement, jamais une
// route que l'on ne saurait ni garder ni retirer.
static bool changement_permis(const struct in6_addr *prefixe, const char *texte) {
  const int i = retenir(prefixe);
  if (i < 0) {
    if (!sPleineNotee) journal("%s/64 : deja %d prefixes suivis, route laissee au noyau", texte, HR_MAX_PREFIXES);
    sPleineNotee = true;
    return false;
  }
  const int64_t t = maintenant_ms();
  if (!sNotres[i].fenetre || t - sNotres[i].fenetre >= 60000) {
    sNotres[i].fenetre = t;
    sNotres[i].changements = 0;
    sNotres[i].attenteNotee = false;
  }
  if (sNotres[i].changements >= HR_CHANGEMENTS_MAX) {
    if (!sNotres[i].attenteNotee) journal("%s/64 : trop de changements ou echec, nouvel essai dans la minute", texte);
    sNotres[i].attenteNotee = true;
    return false;
  }
  sNotres[i].changements++;
  return true;
}

static void echec(const struct in6_addr *prefixe) {
  const int i = trouver_notre(prefixe);
  if (i >= 0) sNotres[i].changements = HR_CHANGEMENTS_MAX;
}

static void appliquer(const hr_annonce *a, const hr_decision *d, const hr_route *r) {
  char p[INET6_ADDRSTRLEN], via[INET6_ADDRSTRLEN], ancien[INET6_ADDRSTRLEN], itf[IFNAMSIZ] = "?", itf0[IFNAMSIZ] = "?";
  texte_adresse(&a->prefixe, p, sizeof(p));
  texte_adresse(&d->via.adresse, via, sizeof(via));
  texte_adresse(&r->passerelle, ancien, sizeof(ancien));
  if_indextoname(d->via.interface, itf);
  if (r->interface) if_indextoname(r->interface, itf0);
  switch (d->geste) {
    case HR_RIEN:
      return;
    case HR_AJOUTER:
      if (sEssai) {
        journal("essai : ajouterait %s/64 via %s%%%s", p, via, itf);
        return;
      }
      if (!changement_permis(&a->prefixe, p)) return;
      if (route_ajouter(&a->prefixe, &d->via)) {
        journal("route %s/64 remise via %s%%%s", p, via, itf);
      } else {
        journal("route %s/64 : ajout via %s%%%s refuse", p, via, itf);
        echec(&a->prefixe);
      }
      return;
    case HR_REMPLACER:
      if (d->suspect) {
        // Routeur injoignable : on ne change que si cela dure (deux passages, 2 s).
        const int i = trouver_notre(&a->prefixe);
        if (i < 0 || !hr_confirme(&sNotres[i].suspicion, true, (double)maintenant_ms() / 1000.0)) {
          if (sEssai) journal("essai : remplacerait %s/64 (via %s%%%s) par %s%%%s si cela dure", p, ancien, itf0, via, itf);
          return;
        }
      }
      if (sEssai) {
        journal("essai : remplacerait %s/64 (via %s%%%s) par %s%%%s", p, ancien, itf0, via, itf);
        return;
      }
      if (!changement_permis(&a->prefixe, p)) return;
      if (route_remplacer(&a->prefixe, &d->via, p)) {
        journal("route %s/64 : %s%%%s remplace par %s%%%s", p, ancien, itf0, via, itf);
      } else {
        journal("route %s/64 : remplacement par %s%%%s refuse", p, via, itf);
        echec(&a->prefixe);
      }
      return;
    case HR_RETIRER:
      if (sEssai) {
        journal("essai : retirerait %s/64 (plus annonce)", p);
        return;
      }
      if (!changement_permis(&a->prefixe, p)) return;
      if (route_retirer(&a->prefixe)) {
        journal("route %s/64 retiree (plus annoncee)", p);
        oublier(&a->prefixe);
      } else {
        journal("route %s/64 : retrait refuse", p);  // gardee en memoire : nouvel essai
        echec(&a->prefixe);
      }
      return;
  }
}

static void passage(void) {
  hr_annonce annonces[HR_MAX_PREFIXES];
  bool tronquee = false;
  const int n = lire_annonces(annonces, HR_MAX_PREFIXES, &tronquee);
  if (n < 0) {
    if (!sIllisible) journal("liste des routes annoncees illisible (errno %d)", errno);
    sIllisible = true;
    return;
  }
  if (sIllisible) journal("liste des routes annoncees de nouveau lisible");
  sIllisible = false;
  if (tronquee && !sTronquee)
    journal("plus de %d prefixes ULA /64 annonces, ou liste coupee : nos routes ne sont plus retirees faute d'annonce",
            HR_MAX_PREFIXES);
  else if (!tronquee && sTronquee)
    journal("liste des routes annoncees de nouveau complete");
  sTronquee = tronquee;
  const unsigned principale = interface_principale();
  for (int i = 0; i < n; i++) {
    hr_route r;
    if (!lire_route(&annonces[i].prefixe, annonces[i].longueur, &r)) continue;
    const hr_decision d = hr_decider(&annonces[i], &r, principale);
    if (sDetail) {
      char p[INET6_ADDRSTRLEN];
      texte_adresse(&annonces[i].prefixe, p, sizeof(p));
      static const char *const kGeste[] = {"rien", "ajouter", "remplacer", "retirer"};
      journal("%s/64 : %u routeur(s)%s, route %s%s -> %s%s", p, annonces[i].n,
              annonces[i].complet ? "" : " (liste tronquee)", r.existe ? "presente" : "absente",
              r.existe ? (r.notre ? " (la notre)" : " (noyau ou a la main)") : "", kGeste[d.geste],
              d.suspect ? " (a confirmer)" : "");
    }
    const int j = r.existe && r.notre ? retenir(&annonces[i].prefixe) : trouver_notre(&annonces[i].prefixe);
    // Soupcon suivi tant que la decision le porte, remis a zero sinon.
    if (j >= 0 && !(d.geste == HR_REMPLACER && d.suspect)) hr_confirme(&sNotres[j].suspicion, false, 0);
    appliquer(&annonces[i], &d, &r);
  }
  // Liste tronquee : un prefixe qui n'y figure pas est peut-etre encore annonce.
  if (tronquee) return;
  // Nos prefixes qui ne sont plus annonces du tout (liste du noyau videe).
  for (int j = 0; j < HR_MAX_PREFIXES; j++) {
    if (!sNotres[j].utilise) continue;
    bool annonce = false;
    for (int i = 0; i < n && !annonce; i++)
      annonce = !memcmp(&annonces[i].prefixe, &sNotres[j].prefixe, sizeof(sNotres[j].prefixe));
    if (annonce) continue;
    hr_annonce vide;
    memset(&vide, 0, sizeof(vide));
    vide.prefixe = sNotres[j].prefixe;
    vide.longueur = 64;
    vide.complet = true;
    hr_route r;
    if (!lire_route(&vide.prefixe, 64, &r)) continue;
    const hr_decision d = hr_decider(&vide, &r, principale);
    if (d.geste == HR_RETIRER) appliquer(&vide, &d, &r);
    else liberer(j);  // plus de route a nous : rien a retenir
  }
}

// A l'arret : nos routes partent avec nous.
static void nettoyer(void) {
  for (int j = 0; j < HR_MAX_PREFIXES; j++) {
    if (!sNotres[j].utilise) continue;
    hr_route r;
    if (lire_route(&sNotres[j].prefixe, 64, &r) && r.existe && r.notre && route_retirer(&sNotres[j].prefixe)) {
      char p[INET6_ADDRSTRLEN];
      journal("arret : route %s/64 retiree", texte_adresse(&sNotres[j].prefixe, p, sizeof(p)));
    }
  }
}

static void sur_signal(int s) {
  (void)s;
  sFin = 1;
}

int main(int argc, char **argv) {
  bool unSeul = false;
  int opt;
  while ((opt = getopt(argc, argv, "n1v")) != -1) {
    switch (opt) {
      case 'n': sEssai = true; break;
      case '1': unSeul = true; break;
      case 'v': sDetail = true; break;
      default:
        fprintf(stderr, "usage : thread-route [-n] [-1] [-v]\n");
        return 2;
    }
  }
  if (!sEssai && geteuid() != 0) {
    fprintf(stderr, "thread-route : root requis pour poser des routes (ou -n pour un essai)\n");
    return 1;
  }
  sSockNd = socket(AF_INET6, SOCK_DGRAM, 0);
  sSockGet = socket(PF_ROUTE, SOCK_RAW, AF_UNSPEC);
  const int ecoute = socket(PF_ROUTE, SOCK_RAW, AF_UNSPEC);
  if (sSockNd < 0 || sSockGet < 0 || ecoute < 0) {
    journal("sockets impossibles (errno %d)", errno);
    return 1;
  }
  // Les reponses a nos RTM_GET passent aussi sur la socket d'ecoute : leur type
  // est ignore plus bas.
  fcntl(ecoute, F_SETFL, O_NONBLOCK);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sur_signal;  // sans SA_RESTART : select() rend la main
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);

  if (unSeul) {
    passage();
    return 0;
  }
  journal("demarre%s (prefixes ULA /64 annonces par les routeurs de bordure)", sEssai ? " en essai" : "");
  int64_t prochain = 0;  // passage de controle
  int64_t evenement = -1;  // passage demande par un message du noyau
  while (!sFin) {
    const int64_t t = maintenant_ms();
    if (t >= prochain || (evenement >= 0 && t >= evenement)) {
      passage();
      prochain = t + HR_PERIODE_S * 1000;
      evenement = -1;
    }
    fd_set lus;
    FD_ZERO(&lus);
    FD_SET(ecoute, &lus);
    int64_t attente = (evenement >= 0 ? evenement : prochain) - maintenant_ms();
    if (attente < 0) attente = 0;
    struct timeval tv = {(time_t)(attente / 1000), (suseconds_t)((attente % 1000) * 1000)};
    if (select(ecoute + 1, &lus, NULL, NULL, &tv) > 0) {
      char buf[2048];
      // Message sur les routes, les adresses ou les interfaces : passage peu
      // apres (les rafales, et nos propres changements, sont fondues). Au plus
      // HR_LOT_MAX messages par reveil : le reste attend le tour suivant.
      ssize_t lu;
      for (int k = 0; k < HR_LOT_MAX && !sFin && (lu = read(ecoute, buf, sizeof(buf))) > 0; k++) {
        if (lu < 4) continue;  // rtm_msglen, rtm_version, rtm_type : communs a tous les messages
        bool signale = false;
        switch ((u_char)buf[offsetof(struct rt_msghdr, rtm_type)]) {
          case RTM_ADD:
          case RTM_DELETE:
          case RTM_CHANGE: {
            // En-tete complet exige. Une demande refusee, meme d'un utilisateur
            // sans droits, est diffusee a tous avec rtm_errno : ignoree.
            struct rt_msghdr h;
            if (lu < (ssize_t)sizeof(h)) break;
            memcpy(&h, buf, sizeof(h));
            signale = h.rtm_errno == 0;
            break;
          }
          case RTM_IFINFO:   // if_msghdr, ifa_msghdr : seuls les 4 premiers
          case RTM_NEWADDR:  // octets sont communs avec rt_msghdr
          case RTM_DELADDR:
            signale = true;
            break;
          default: break;  // RTM_MISS (recherches sans reponse, ~1/s ici), RTM_GET...
        }
        if (signale && evenement < 0) evenement = maintenant_ms() + HR_ATTENTE_EVENEMENT_MS;
      }
    }
  }
  if (!sEssai) nettoyer();
  journal("arrete");
  return 0;
}
