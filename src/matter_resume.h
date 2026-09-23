#pragma once
#include <stdint.h>

// ===========================================================================
//  Calendrier de la relance des abonnements Matter (pont Thread)
//
//  Terrain du 23/09 : apres chaque redemarrage, Apple Home ne recoit plus rien
//  du noeud (ni miroir de la telecommande) tant que l'utilisateur n'agit pas
//  dans l'app. La pile tente bien de reprendre l'abonnement d'Apple, mais UNE
//  fois, pendant Server::Init, avant que Thread et SRP soient prets : la
//  recherche d'adresse expire au bout de 45 s (erreur 32 a 47 s), et la
//  tentative suivante n'arrive que 300 s plus tard.
//
//  Ce calendrier dit quand relancer nous-memes la reprise : reseau pret
//  (attache + hote SRP enregistre) depuis kSettleMs, jamais avant kNotBeforeMs
//  apres le demarrage de Matter (la tentative du SDK cherche encore l'adresse ;
//  la rejoindre, c'est echouer avec elle), aucun abonnement actif, aucune
//  tentative en cours. Apres chaque echec : 30 s, 60 s, puis toutes les 5 min.
//
//  Pur et sans Arduino : teste sur l'hote (tools/host_tests). Toutes les
//  durees sont des ecarts non signes a partir d'un instant, et les attentes
//  franchies sont retenues : le retour a zero de millis() ne bloque rien.
// ===========================================================================

struct ResumePlanner {
  static constexpr uint32_t kSettleMs = 10000;     // reseau pret depuis 10 s
  static constexpr uint32_t kNotBeforeMs = 50000;  // pas avant 50 s apres Matter.begin()
  static constexpr uint32_t kIdleMs = 300000;      // rien a relancer : nouveau coup d'oeil dans 5 min

  // Attente apres la fin de la n-ieme tentative d'un episode.
  static uint32_t backoffMs(uint8_t n) { return n <= 1 ? 30000u : n == 2 ? 60000u : 300000u; }

  bool ready = false;      // Thread attache et hote SRP enregistre
  bool settled = false;    // ... depuis au moins kSettleMs
  uint32_t readySince = 0;
  bool started = false;    // kNotBeforeMs ecoule depuis le demarrage de Matter
  bool subsKnown = false;  // au moins un comptage des abonnements
  uint32_t subs = 0;       // abonnements actifs au dernier comptage
  uint8_t tries = 0;       // tentatives de l'episode en cours
  bool waiting = false;    // tentative lancee, fin pas encore vue
  bool hold = false;       // attente en cours : holdMs a partir de holdAt
  uint32_t holdAt = 0, holdMs = 0;

  // Etat du reseau, a chaque releve. Un reseau retrouve ouvre un nouvel
  // episode : compteur a zero, stabilisation a refaire.
  void network(bool r, uint32_t now) {
    if (r && !ready) {
      readySince = now;
      tries = 0;
      hold = false;
    }
    if (!r) settled = false;
    ready = r;
  }

  // Nombre d'abonnements actifs, a chaque comptage. Un abonnement actif clot
  // l'episode ; sa perte en ouvre un nouveau, apres kSettleMs.
  void subscriptions(uint32_t n, uint32_t now) {
    if (n > 0) {
      tries = 0;
      hold = false;
    } else if (subsKnown && subs > 0) {
      tries = 0;
      wait(now, kSettleMs);
    }
    subs = n;
    subsKnown = true;
  }

  // true : lancer une tentative maintenant (puis fired()).
  bool due(uint32_t now, uint32_t matterStartMs) {
    if (!started && (uint32_t)(now - matterStartMs) >= kNotBeforeMs) started = true;
    if (ready && !settled && (uint32_t)(now - readySince) >= kSettleMs) settled = true;
    if (hold && (uint32_t)(now - holdAt) >= holdMs) hold = false;
    return started && ready && settled && subsKnown && subs == 0 && !waiting && !hold;
  }

  void fired() {
    waiting = true;
    if (tries < 255) tries++;
  }

  // Fin de la tentative : 'launched' abonnements relances (0 : rien a relancer,
  // ou rien lance parce qu'un abonnement etait deja actif).
  void finished(uint32_t launched, uint32_t now) {
    waiting = false;
    wait(now, launched ? backoffMs(tries) : kIdleMs);
  }

  // Temps restant avant la prochaine tentative possible, pour l'affichage
  // (0 : aucune attente de calendrier en cours).
  uint32_t holdLeftMs(uint32_t now) const {
    if (!hold) return 0;
    const uint32_t e = now - holdAt;
    return e >= holdMs ? 0 : holdMs - e;
  }

 private:
  void wait(uint32_t now, uint32_t ms) {
    hold = true;
    holdAt = now;
    holdMs = ms;
  }
};
