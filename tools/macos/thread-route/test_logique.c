// Tests de la decision de thread-route (logique.c), sans rien toucher au systeme.
// Lancer : sh tools/macos/thread-route/tests.sh
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "logique.h"

static int gChecks = 0, gFails = 0;
#define CHECK(c, ...)                                     \
  do {                                                    \
    gChecks++;                                            \
    if (!(c)) {                                           \
      gFails++;                                           \
      printf("ECHEC %s:%d : ", __FILE__, __LINE__);      \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

enum { PURGE = -3, SANS_ETAT = -2, INCOMPLET = 0, JOIGNABLE = 1, PERIME = 2, DELAI = 3, SONDE = 4 };
enum { EN0 = 15, EN18 = 13 };

static struct in6_addr A(const char *s) {
  struct in6_addr a;
  inet_pton(AF_INET6, s, &a);
  return a;
}

static hr_routeur R(const char *adr, unsigned itf, int nd, bool installe, uint32_t duree) {
  hr_routeur r;
  memset(&r, 0, sizeof(r));
  r.adresse = A(adr);
  r.interface = itf;
  r.etat_nd = nd;
  r.installe = installe;
  r.active = true;
  r.duree = duree;
  return r;
}

// Routeur sur une interface tombee (ni IFF_UP ni IFF_RUNNING).
static hr_routeur R_inactif(const char *adr, unsigned itf, int nd, bool installe, uint32_t duree) {
  hr_routeur r = R(adr, itf, nd, installe, duree);
  r.active = false;
  return r;
}

static hr_annonce annonce(const char *prefixe) {
  hr_annonce a;
  memset(&a, 0, sizeof(a));
  a.prefixe = A(prefixe);
  a.longueur = 64;
  a.complet = true;
  return a;
}

static void ajoute(hr_annonce *a, hr_routeur r) { a->routeurs[a->n++] = r; }

static hr_route route(bool existe, bool notre, const char *gw, unsigned itf) {
  hr_route r;
  memset(&r, 0, sizeof(r));
  r.existe = existe;
  r.notre = notre;
  if (gw) r.passerelle = A(gw);
  r.interface = itf;
  return r;
}

static bool via(const hr_decision *d, const char *adr, unsigned itf) {
  const struct in6_addr a = A(adr);
  return !memcmp(&d->via.adresse, &a, sizeof(a)) && d->via.interface == itf;
}

static bool est(const hr_routeur *r, const char *adr, unsigned itf) {
  const struct in6_addr a = A(adr);
  return r && !memcmp(&r->adresse, &a, sizeof(a)) && r->interface == itf;
}

int main(void) {
  // Prefixes geres : ULA /64 seulement.
  struct in6_addr p = A("fd4f:9c:ed42::");
  CHECK(hr_prefixe_gere(&p, 64), "fd4f::/64");
  CHECK(!hr_prefixe_gere(&p, 48), "longueur 48");
  p = A("fc00:1::");
  CHECK(hr_prefixe_gere(&p, 64), "fc00::/64");
  p = A("2001:db8::");
  CHECK(!hr_prefixe_gere(&p, 64), "prefixe global : jamais");
  p = A("fe80::");
  CHECK(!hr_prefixe_gere(&p, 64), "lien local : jamais");

  // Joignable comme le juge le noyau (ND6_IS_LLINFO_PROBREACH).
  CHECK(hr_joignable(JOIGNABLE) && hr_joignable(PERIME) && hr_joignable(DELAI) && hr_joignable(SONDE),
        "joignables : REACHABLE, STALE, DELAY, PROBE");
  CHECK(!hr_joignable(INCOMPLET) && !hr_joignable(SANS_ETAT) && !hr_joignable(PURGE) &&
            !hr_joignable(HR_ND_ABSENT) && !hr_joignable(99),
        "non joignables : INCOMPLETE, NOSTATE, PURGE, absent, inconnu");

  // Le cas du 25/09 : route supprimee par le noyau, 5 bornes sur deux interfaces ;
  // le noyau croit toujours installee celle via 42a.
  hr_annonce a = annonce("fd4f:9c:ed42::");
  ajoute(&a, R("fe80::64e:879d:d1aa:fe01", EN18, JOIGNABLE, true, 1800));
  ajoute(&a, R("fe80::7c7:aa90:ed7d:91c", EN18, PERIME, false, 1800));
  ajoute(&a, R("fe80::e14:1eca:d086:9a92", EN0, JOIGNABLE, false, 1800));
  ajoute(&a, R("fe80::70a:fe76:9c5c:8ff2", EN0, INCOMPLET, false, 1800));
  hr_route sans = route(false, false, NULL, 0);
  hr_decision d = hr_decider(&a, &sans, EN0);
  CHECK(d.geste == HR_AJOUTER && via(&d, "fe80::64e:879d:d1aa:fe01", EN18) && !d.suspect,
        "pas de route : ajouter via le routeur joignable que le noyau croit installe");
  d = hr_decider(&a, &sans, EN18);
  CHECK(d.geste == HR_AJOUTER && via(&d, "fe80::64e:879d:d1aa:fe01", EN18), "principale en18");
  d = hr_decider(&a, &sans, 99);
  CHECK(d.geste == HR_AJOUTER && via(&d, "fe80::64e:879d:d1aa:fe01", EN18),
        "hors interface principale : celui que le noyau croit installe");

  // Meme cas sans entree installee : l'interface principale departage.
  hr_annonce b = a;
  b.routeurs[0].installe = false;
  d = hr_decider(&b, &sans, EN0);
  CHECK(d.geste == HR_AJOUTER && via(&d, "fe80::e14:1eca:d086:9a92", EN0),
        "rien d'installe : un routeur joignable de l'interface principale");
  d = hr_decider(&b, &sans, EN18);
  CHECK(d.geste == HR_AJOUTER && via(&d, "fe80::64e:879d:d1aa:fe01", EN18), "rien d'installe, principale en18");
  d = hr_decider(&b, &sans, 99);
  CHECK(d.geste == HR_AJOUTER && via(&d, "fe80::64e:879d:d1aa:fe01", EN18),
        "a rang egal hors interface principale : la plus petite adresse");

  // Une route du noyau, ou posee a la main : on n'y touche pas.
  hr_route noyau = route(true, false, "fe80::64e:879d:d1aa:fe01", EN18);
  CHECK(hr_decider(&a, &noyau, EN0).geste == HR_RIEN, "route du noyau");
  hr_route main_ = route(true, false, "fe80::1", EN18);
  CHECK(hr_decider(&a, &main_, EN0).geste == HR_RIEN, "route posee a la main, meme via un inconnu");

  // Notre route via un routeur valable : rien.
  hr_route nous = route(true, true, "fe80::64e:879d:d1aa:fe01", EN18);
  CHECK(hr_decider(&a, &nous, EN0).geste == HR_RIEN, "notre route, routeur joignable");
  hr_route nous_perime = route(true, true, "fe80::7c7:aa90:ed7d:91c", EN18);
  CHECK(hr_decider(&a, &nous_perime, EN0).geste == HR_RIEN, "perime : encore utilisable, pas de va-et-vient");

  // Notre route via un routeur injoignable : on change, si un autre est joignable,
  // une fois le soupcon confirme.
  hr_route nous_mort = route(true, true, "fe80::70a:fe76:9c5c:8ff2", EN0);
  d = hr_decider(&a, &nous_mort, EN0);
  CHECK(d.geste == HR_REMPLACER && via(&d, "fe80::64e:879d:d1aa:fe01", EN18) && d.suspect,
        "routeur incomplet : remplacer, a confirmer");
  hr_annonce tous_morts = annonce("fd4f:9c:ed42::");
  ajoute(&tous_morts, R("fe80::70a:fe76:9c5c:8ff2", EN0, INCOMPLET, false, 1800));
  ajoute(&tous_morts, R("fe80::64e:879d:d1aa:fe01", EN0, INCOMPLET, false, 1800));
  CHECK(hr_decider(&tous_morts, &nous_mort, EN0).geste == HR_RIEN, "tous injoignables : on garde (rien de mieux)");

  // Entree recreee (sans etat) ou liberee apres un echec (absente) : injoignable.
  hr_annonce c = annonce("fd4f:9c:ed42::");
  ajoute(&c, R("fe80::1", EN0, SANS_ETAT, false, 1800));
  ajoute(&c, R("fe80::2", EN0, JOIGNABLE, false, 1800));
  hr_route nous_1 = route(true, true, "fe80::1", EN0);
  d = hr_decider(&c, &nous_1, EN0);
  CHECK(d.geste == HR_REMPLACER && via(&d, "fe80::2", EN0) && d.suspect, "sans etat + autre joignable : remplacer, a confirmer");
  c.routeurs[0].etat_nd = HR_ND_ABSENT;
  c.routeurs[1].etat_nd = PERIME;
  d = hr_decider(&c, &nous_1, EN0);
  CHECK(d.geste == HR_REMPLACER && via(&d, "fe80::2", EN0) && d.suspect, "absent + autre perime : remplacer, a confirmer");
  c.routeurs[1].etat_nd = INCOMPLET;
  CHECK(hr_decider(&c, &nous_1, EN0).geste == HR_RIEN, "absent, aucun autre joignable : rien");
  c.routeurs[1].etat_nd = SANS_ETAT;
  CHECK(hr_decider(&c, &nous_1, EN0).geste == HR_RIEN, "absent, l'autre sans etat : rien");

  // Notre route via un routeur qui n'annonce plus le prefixe : remplacer sans attendre.
  hr_route nous_parti = route(true, true, "fe80::dead:beef", EN18);
  d = hr_decider(&a, &nous_parti, EN0);
  CHECK(d.geste == HR_REMPLACER && via(&d, "fe80::64e:879d:d1aa:fe01", EN18) && !d.suspect,
        "routeur parti : remplacer, sans attendre");
  // Meme adresse, autre interface : ce n'est pas le meme chemin.
  hr_route nous_autre_itf = route(true, true, "fe80::e14:1eca:d086:9a92", EN18);
  CHECK(hr_decider(&a, &nous_autre_itf, EN0).geste == HR_REMPLACER, "meme adresse, interface disparue");
  // Liste tronquee (routeurs ecartes) : il est peut-etre parmi eux, rien a conclure.
  hr_annonce tronquee = a;
  tronquee.complet = false;
  CHECK(hr_decider(&tronquee, &nous_parti, EN0).geste == HR_RIEN, "routeur absent d'une liste tronquee : rien");

  // Joignable prefere, puis celui que le noyau croit installe.
  hr_annonce e = annonce("fd4f:9c:ed42::");
  ajoute(&e, R("fe80::b", EN0, JOIGNABLE, false, 1800));
  ajoute(&e, R("fe80::a", EN18, PERIME, true, 1800));
  CHECK(est(hr_meilleur(&e, EN0), "fe80::a", EN18), "joignables : l'installe (perime) avant un REACHABLE non installe");
  e.routeurs[1].etat_nd = INCOMPLET;
  CHECK(est(hr_meilleur(&e, EN0), "fe80::b", EN0), "installe mais injoignable : le joignable d'abord");
  hr_annonce g = annonce("fd4f:9c:ed42::");
  ajoute(&g, R("fe80::1", EN0, PERIME, false, 1800));
  ajoute(&g, R("fe80::2", EN0, JOIGNABLE, false, 1800));
  CHECK(est(hr_meilleur(&g, EN0), "fe80::2", EN0), "a installation egale : REACHABLE avant STALE, meme adresse plus grande");

  // Interface tombee : jamais choisie.
  hr_annonce f = annonce("fd4f:9c:ed42::");
  ajoute(&f, R_inactif("fe80::a", EN0, JOIGNABLE, true, 1800));
  CHECK(hr_meilleur(&f, EN0) == NULL, "seul routeur sur une interface tombee : aucun");
  CHECK(hr_decider(&f, &sans, EN0).geste == HR_RIEN, "interfaces tombees, pas de route : rien");
  hr_route nous_a = route(true, true, "fe80::a", EN0);
  CHECK(hr_decider(&f, &nous_a, EN0).geste == HR_RIEN, "interfaces tombees : notre route gardee (encore annoncee)");
  ajoute(&f, R("fe80::c", EN18, PERIME, false, 1800));
  CHECK(est(hr_meilleur(&f, EN0), "fe80::c", EN18), "interface tombee ecartee, meme installee et joignable");
  d = hr_decider(&f, &nous_a, EN0);
  CHECK(d.geste == HR_REMPLACER && via(&d, "fe80::c", EN18) && d.suspect, "notre interface tombee : remplacer, a confirmer");

  // Annonces retirees (duree 0) : comme absentes.
  hr_annonce retiree = annonce("fd4f:9c:ed42::");
  ajoute(&retiree, R("fe80::64e:879d:d1aa:fe01", EN18, JOIGNABLE, true, 0));
  CHECK(hr_decider(&retiree, &nous, EN0).geste == HR_RETIRER, "plus d'annonce : retirer notre route");
  CHECK(hr_decider(&retiree, &noyau, EN0).geste == HR_RIEN, "plus d'annonce : jamais la route d'un autre");
  CHECK(hr_decider(&retiree, &sans, EN0).geste == HR_RIEN, "plus d'annonce, pas de route : rien");
  hr_annonce vide = annonce("fd4f:9c:ed42::");
  CHECK(hr_decider(&vide, &nous, EN0).geste == HR_RETIRER, "liste vide : retirer");
  vide.complet = false;
  CHECK(hr_decider(&vide, &nous, EN0).geste == HR_RIEN, "liste tronquee : jamais retirer");

  // Routeur sans interface (ne devrait pas arriver) : ignore.
  hr_annonce sans_itf = annonce("fd4f:9c:ed42::");
  ajoute(&sans_itf, R("fe80::1", 0, JOIGNABLE, false, 1800));
  CHECK(hr_meilleur(&sans_itf, EN0) == NULL, "interface 0 ignoree");

  // Rangs.
  CHECK(hr_rang_nd(JOIGNABLE) > hr_rang_nd(SONDE) && hr_rang_nd(SONDE) > hr_rang_nd(PERIME) &&
            hr_rang_nd(PERIME) > hr_rang_nd(HR_ND_ABSENT) && hr_rang_nd(HR_ND_ABSENT) > hr_rang_nd(SANS_ETAT) &&
            hr_rang_nd(SANS_ETAT) > hr_rang_nd(INCOMPLET),
        "ordre des rangs");

  // Anti-rebond : deux passages, 2 s au moins.
  hr_suspicion s = {0, 0};
  CHECK(!hr_confirme(&s, true, 100.0), "premier constat : pas encore");
  CHECK(!hr_confirme(&s, true, 101.0), "deuxieme constat a +1 s : trop tot");
  CHECK(hr_confirme(&s, true, 102.0), "constat a +2 s : confirme");
  CHECK(!hr_confirme(&s, false, 103.0) && s.vues == 0, "sans soupcon : remise a zero");
  CHECK(!hr_confirme(&s, true, 104.0), "apres remise a zero : premier constat");
  CHECK(hr_confirme(&s, true, 106.5), "deux constats a 2,5 s d'ecart : confirme");

  printf("thread-route : %d verification(s), %d echec(s)\n", gChecks, gFails);
  return gFails ? 1 : 0;
}
