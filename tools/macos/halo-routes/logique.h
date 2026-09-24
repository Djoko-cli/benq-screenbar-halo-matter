#pragma once
// ===========================================================================
//  halo-routes : decision pure (sans appel systeme), testee sur le Mac
//  (test_logique.c). Voir halo-routes.c pour le contexte.
//
//  Pour un prefixe annonce par les routeurs de bordure Thread (option RIO),
//  a partir de ce que le noyau en sait (liste des routes annoncees, etat des
//  voisins, interfaces) et de la route presente dans la table, choisir un
//  geste :
//    - rien : une route existe et elle n'est pas a nous (celle du noyau, ou une
//      route posee a la main), ou la notre passe par un routeur valable ;
//    - ajouter : aucune route (le noyau l'a retiree sans la remettre) ;
//    - remplacer : notre route passe par un routeur qui n'annonce plus le
//      prefixe (liste complete seulement), ou qui semble injoignable (ou dont
//      l'interface est tombee) alors qu'un autre est joignable : soupcon a
//      confirmer (hr_confirme) avant d'agir ;
//    - retirer : plus aucun routeur n'annonce le prefixe.
// ===========================================================================
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#define HR_MAX_ROUTEURS 32

// Etat du voisin (ND6_LLINFO_* du noyau), ou HR_ND_ABSENT : pas dans le cache.
#define HR_ND_ABSENT (-100)

// Soupcon confirme : vu a HR_CONFIRMATION_VUES passages, et tenu depuis
// HR_CONFIRMATION_S secondes.
#define HR_CONFIRMATION_VUES 2
#define HR_CONFIRMATION_S 2.0

typedef struct {
  struct in6_addr adresse;  // lien local, sans portee incorporee
  unsigned interface;       // index de l'interface
  int etat_nd;              // ND6_LLINFO_* ou HR_ND_ABSENT
  bool installe;            // NDDRF_INSTALLED : le noyau croit la route posee par lui
  bool active;              // interface IFF_UP et IFF_RUNNING
  uint32_t duree;           // duree de l'annonce (s) ; 0 : retiree
} hr_routeur;

typedef struct {
  struct in6_addr prefixe;
  uint8_t longueur;
  hr_routeur routeurs[HR_MAX_ROUTEURS];
  unsigned n;
  bool complet;  // false : routeurs ecartes (au-dela de HR_MAX_ROUTEURS, liste tronquee)
} hr_annonce;

typedef struct {
  bool existe;               // route exacte prefixe/longueur dans la table
  bool notre;                 // RTF_STATIC et RTF_PROTO1 : posee par nous
  struct in6_addr passerelle;  // sans portee incorporee
  unsigned interface;
} hr_route;

typedef enum { HR_RIEN, HR_AJOUTER, HR_REMPLACER, HR_RETIRER } hr_geste;

typedef struct {
  hr_geste geste;
  hr_routeur via;  // ajouter, remplacer
  bool suspect;    // remplacer sur un soupcon d'injoignabilite : a confirmer
} hr_decision;

// Anti-rebond d'un soupcon, par prefixe.
typedef struct {
  double depuis;  // premier constat (s)
  unsigned vues;  // constats de suite (plafonne a HR_CONFIRMATION_VUES)
} hr_suspicion;

// Prefixe gere : ULA (fc00::/7) de longueur 64, comme les prefixes OMR Thread.
bool hr_prefixe_gere(const struct in6_addr *prefixe, uint8_t longueur);

// Voisin probablement joignable, comme le juge le noyau
// (ND6_IS_LLINFO_PROBREACH : etat au-dela d'incomplet) : joignable, perime,
// delai, sonde. Non : incomplet (resolution en cours), sans etat (entree
// recreee), purge, absent du cache (entree liberee apres un echec de la
// detection d'injoignabilite), tout autre etat.
bool hr_joignable(int etat_nd);

// Rang d'un voisin, pour departager a joignabilite egale : plus haut, mieux.
// Joignable > delai, sonde > perime > absent du cache > autre (sans etat,
// purge) > incomplet.
int hr_rang_nd(int etat_nd);

// Meilleur routeur en service (duree > 0, interface connue et active) :
// joignable d'abord ; puis celui que le noyau croit installe (s'il meurt, le
// noyau retire lui-meme notre route et pose la sienne, au lieu de retirer
// plus tard une bonne route a nous) ; puis le rang du voisin ; puis
// l'interface principale ; puis la plus petite adresse, puis le plus petit
// index d'interface (choix stable). NULL : aucun.
const hr_routeur *hr_meilleur(const hr_annonce *a, unsigned interface_principale);

hr_decision hr_decider(const hr_annonce *a, const hr_route *r, unsigned interface_principale);

// Sans soupcon : remise a zero, faux. Avec : vrai une fois le soupcon vu a
// HR_CONFIRMATION_VUES passages et tenu HR_CONFIRMATION_S secondes
// (maintenant : horloge monotone, en s).
bool hr_confirme(hr_suspicion *s, bool suspect, double maintenant);
