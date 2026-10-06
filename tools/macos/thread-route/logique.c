#include "logique.h"

#include <string.h>

// Valeurs de netinet6/nd6.h (ND6_LLINFO_*), recopiees pour que la logique se
// compile sans les entetes du noyau.
enum {
  ND_PURGE = -3,
  ND_SANS_ETAT = -2,
  ND_INCOMPLET = 0,
  ND_JOIGNABLE = 1,
  ND_PERIME = 2,
  ND_DELAI = 3,
  ND_SONDE = 4
};

bool hr_prefixe_gere(const struct in6_addr *prefixe, uint8_t longueur) {
  return longueur == 64 && (prefixe->s6_addr[0] & 0xFE) == 0xFC;
}

bool hr_joignable(int etat_nd) {
  switch (etat_nd) {
    case ND_JOIGNABLE:
    case ND_PERIME:
    case ND_DELAI:
    case ND_SONDE: return true;
    default: return false;  // incomplet, sans etat, purge, absent, inconnu
  }
}

int hr_rang_nd(int etat_nd) {
  switch (etat_nd) {
    case ND_JOIGNABLE: return 5;
    case ND_DELAI:
    case ND_SONDE: return 4;
    case ND_PERIME: return 3;
    case HR_ND_ABSENT: return 2;  // hors du cache : pas encore resolu, ou libere apres un echec
    case ND_INCOMPLET: return 0;  // resolution en cours, sans reponse
    case ND_SANS_ETAT:
    case ND_PURGE:
    default: return 1;
  }
}

static bool meilleur_que(const hr_routeur *a, const hr_routeur *b, unsigned principale) {
  const bool ja = hr_joignable(a->etat_nd), jb = hr_joignable(b->etat_nd);
  if (ja != jb) return ja;
  if (a->installe != b->installe) return a->installe;
  const int ra = hr_rang_nd(a->etat_nd), rb = hr_rang_nd(b->etat_nd);
  if (ra != rb) return ra > rb;
  const bool pa = a->interface == principale, pb = b->interface == principale;
  if (pa != pb) return pa;
  const int c = memcmp(&a->adresse, &b->adresse, sizeof(a->adresse));
  if (c) return c < 0;
  return a->interface < b->interface;
}

const hr_routeur *hr_meilleur(const hr_annonce *a, unsigned interface_principale) {
  const hr_routeur *best = NULL;
  for (unsigned i = 0; i < a->n && i < HR_MAX_ROUTEURS; i++) {
    const hr_routeur *r = &a->routeurs[i];
    if (!r->duree || !r->interface || !r->active) continue;
    if (!best || meilleur_que(r, best, interface_principale)) best = r;
  }
  return best;
}

// Un routeur annonce encore le prefixe (duree > 0), utilisable ou non.
static bool encore_annonce(const hr_annonce *a) {
  for (unsigned i = 0; i < a->n && i < HR_MAX_ROUTEURS; i++)
    if (a->routeurs[i].duree) return true;
  return false;
}

hr_decision hr_decider(const hr_annonce *a, const hr_route *r, unsigned interface_principale) {
  hr_decision d = {HR_RIEN, {0}, false};
  const hr_routeur *best = hr_meilleur(a, interface_principale);
  if (!best) {
    // Plus aucun routeur n'annonce le prefixe : notre route ne mene plus a rien.
    // Annonces sur des interfaces tombees, ou liste tronquee : on la garde.
    if (r->existe && r->notre && a->complet && !encore_annonce(a)) d.geste = HR_RETIRER;
    return d;
  }
  if (!r->existe) {
    d.geste = HR_AJOUTER;
    d.via = *best;
    return d;
  }
  if (!r->notre) return d;  // celle du noyau, ou une route posee a la main
  // Notre route : son routeur annonce-t-il encore le prefixe ?
  const hr_routeur *actuel = NULL;
  for (unsigned i = 0; i < a->n && i < HR_MAX_ROUTEURS; i++) {
    const hr_routeur *x = &a->routeurs[i];
    if (x->duree && x->interface == r->interface && !memcmp(&x->adresse, &r->passerelle, sizeof(x->adresse)))
      actuel = x;
  }
  if (!actuel) {
    // Liste tronquee : il peut etre parmi les routeurs ecartes, rien a conclure.
    if (a->complet) {
      d.geste = HR_REMPLACER;
      d.via = *best;
    }
    return d;
  }
  // Injoignable (ou interface tombee) alors qu'un autre est joignable : on
  // change de routeur, une fois le soupcon confirme.
  if ((!hr_joignable(actuel->etat_nd) || !actuel->active) && hr_joignable(best->etat_nd) && best != actuel) {
    d.geste = HR_REMPLACER;
    d.via = *best;
    d.suspect = true;
  }
  return d;
}

bool hr_confirme(hr_suspicion *s, bool suspect, double maintenant) {
  if (!suspect) {
    s->depuis = 0;
    s->vues = 0;
    return false;
  }
  if (!s->vues) s->depuis = maintenant;
  if (s->vues < HR_CONFIRMATION_VUES) s->vues++;
  return s->vues >= HR_CONFIRMATION_VUES && maintenant - s->depuis >= HR_CONFIRMATION_S;
}
