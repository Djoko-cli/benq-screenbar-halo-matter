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
//  Ce calendrier dit quand relancer nous-memes : reseau pret (attache + hote
//  SRP enregistre) depuis kSettleMs, jamais avant startDelayMs apres le
//  demarrage de Matter (la tentative de la pile cherche encore l'adresse ; la
//  rejoindre, c'est echouer avec elle), aucune tentative en cours. Apres
//  chaque echec : 30 s, 60 s, puis toutes les 5 min. Tant qu'un abonnement
//  est actif : un coup d'oeil toutes les 5 min (un autre abonne sauve peut
//  rester sans abonnement ; le pont saute ceux qui en ont un).
//
//  Un episode (compteur d'essais) ne se clot que sur un abonnement tenu
//  kStableMs, ou une coupure du reseau d'au moins kOutageMs : un abonnement
//  repris puis perdu aussitot, ou un reseau qui bagote (changement de parent,
//  serveur SRP qui bascule), ne ramene pas l'attente a 30 s.
//
//  Pur et sans Arduino : teste sur l'hote (tools/host_tests). Toutes les
//  durees sont des ecarts non signes a partir d'un instant, et les attentes
//  franchies sont retenues : le retour a zero de millis() ne bloque rien.
// ===========================================================================

struct ResumePlanner {
  static constexpr uint32_t kSettleMs = 10000;     // reseau pret depuis 10 s
  static constexpr uint32_t kNotBeforeMs = 50000;  // pas avant 50 s apres Matter.begin()
  static constexpr uint32_t kIdleMs = 300000;      // rien a relancer, ou abonnement actif : 5 min
  static constexpr uint32_t kStableMs = 300000;    // abonnement tenu 5 min : episode clos
  static constexpr uint32_t kOutageMs = 300000;    // reseau perdu 5 min ou plus : nouvel episode

  // Attente apres la fin de la n-ieme tentative d'un episode.
  static uint32_t backoffMs(uint8_t n) { return n <= 1 ? 30000u : n == 2 ? 60000u : 300000u; }

  bool ready = false;      // Thread attache et hote SRP enregistre
  bool settled = false;    // ... depuis au moins kSettleMs
  uint32_t readySince = 0;
  uint32_t lostAt = 0;     // derniere perte du reseau
  uint32_t startDelayMs = kNotBeforeMs;  // kNotBeforeMs + plancher sauve (startDelay())
  bool started = false;    // startDelayMs ecoule depuis le demarrage de Matter
  bool subsKnown = false;  // au moins un comptage des abonnements
  uint32_t subs = 0;       // abonnements actifs au dernier comptage
  uint32_t activeSince = 0;  // passage de 0 a au moins un abonnement actif
  bool stable = false;     // ... tenu depuis au moins kStableMs
  uint8_t tries = 0;       // tentatives de l'episode en cours
  bool waiting = false;    // tentative lancee, fin pas encore vue
  bool hold = false;       // attente en cours : holdMs a partir de holdAt
  uint32_t holdAt = 0, holdMs = 0;

  // La pile tente sa reprise au plus grand plancher (intervalle min) des
  // abonnements sauves apres Server::Init, et cherche l'adresse jusqu'a 45 s :
  // premiere tentative du pont apres les deux. Au-dela de 600 s, la pile
  // passe apres le pont et le rejoint (ou saute l'abonnement repris).
  void startDelay(uint16_t maxSavedMinS) {
    startDelayMs = kNotBeforeMs + 1000u * (maxSavedMinS < 600 ? maxSavedMinS : 600u);
  }

  // Etat du reseau, a chaque releve. Retrouve apres une longue coupure :
  // nouvel episode. Apres une courte : l'attente et le compteur restent.
  void network(bool r, uint32_t now) {
    if (r && !ready) {
      readySince = now;
      if ((uint32_t)(now - lostAt) >= kOutageMs) {
        tries = 0;
        hold = false;
      }
    }
    if (!r && ready) lostAt = now;
    if (!r) settled = false;
    ready = r;
  }

  // Nombre d'abonnements actifs, a chaque comptage (toutes les 2 s).
  void subscriptions(uint32_t n, uint32_t now) {
    if (n > 0) {
      if (!subsKnown || subs == 0) {  // abonnement (re)venu : coup d'oeil dans 5 min
        activeSince = now;
        stable = false;
        wait(now, kIdleMs);
      } else if (!stable && (uint32_t)(now - activeSince) >= kStableMs) {
        stable = true;  // tenu : l'episode est clos
        tries = 0;
      }
    } else if (subsKnown && subs > 0) {  // perte
      if (stable) {
        tries = 0;
        wait(now, kSettleMs);
      } else {
        wait(now, backoffMs(tries));  // repris puis perdu aussitot : comme un echec
      }
    }
    subs = n;
    subsKnown = true;
  }

  // true : lancer une tentative maintenant (puis fired()).
  bool due(uint32_t now, uint32_t matterStartMs) {
    if (!started && (uint32_t)(now - matterStartMs) >= startDelayMs) started = true;
    if (ready && !settled && (uint32_t)(now - readySince) >= kSettleMs) settled = true;
    if (hold && (uint32_t)(now - holdAt) >= holdMs) hold = false;
    return started && ready && settled && subsKnown && !waiting && !hold;
  }

  // Un coup d'oeil (abonnement actif) n'est pas un essai de l'episode.
  void fired() {
    waiting = true;
    if (subs == 0 && tries < 255) tries++;
  }

  // Fin de la tentative : 'launched' abonnes contactes (0 : rien a relancer,
  // ou tous deja servis). Un abonnement actif ramene au coup d'oeil de 5 min.
  void finished(uint32_t launched, uint32_t now) {
    waiting = false;
    wait(now, launched && subs == 0 ? backoffMs(tries) : kIdleMs);
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
