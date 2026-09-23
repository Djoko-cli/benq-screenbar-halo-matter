#pragma once
#include <Arduino.h>

// Cree les endpoints Matter de la lampe Halo 1 puis demarre la pile. A appeler
// depuis setup() (la tache loop), apres lamp.begin() : les endpoints partent de
// la consigne du pilote.
void matterBridgeBegin();

// Applique les ordres Matter regroupes, puis reflete la consigne du pilote vers
// Matter. A appeler depuis loop().
void matterBridgePoll();

void matterPrintStatus(Print &out);

// Duree de l'impulsion d'EP4 "Halo auto" (bouton A), reglable sur le terrain
// ('matter impulsion <ms>') et gardee en NVS. Hors bornes : refusee (false),
// rien ne change. Sinon appliquee tout de suite ; *saved dit si l'ecriture NVS
// a reussi. A appeler depuis la tache loop (CLI).
constexpr uint16_t kMatterPulseMinMs = 300;
constexpr uint16_t kMatterPulseMaxMs = 15000;
uint16_t matterAutoPulseMs();
bool matterSetAutoPulseMs(uint32_t ms, bool *saved);
void matterDecommissionNow();
bool matterIsCommissioned();
bool matterIsConnected();

#if MATTER_NET_THREAD
// Abonnements d'Apple Home apres un redemarrage (build Thread). Toutes ces
// fonctions sont a appeler depuis la tache loop (CLI) ; les reglages sont
// gardes en NVS (espace halo1). Une ecriture NVS ratee laisse *saved a false.

// Relance tout de suite la reprise des abonnements sauves (banc). Refusee
// (false, raison affichee) si la pile est absente, le noeud pas mis en
// service, ou une tentative deja en cours. Le resultat arrive en traces
// '[matter] reprise ...'.
bool matterResumeNow(Print &out);

// Relance automatique : reseau pret (attache + SRP) depuis 10 s, pas avant
// 50 s apres le demarrage de Matter, aucun abonnement actif.
bool matterResumeAuto();
void matterSetResumeAuto(bool on, bool *saved);

// Type Thread au PROCHAIN demarrage : 0 routeur (reglage d'esp_matter), 1 MED
// des l'init de Thread (sans nouvelle attache), 2 MED apres Matter.begin()
// (ancien comportement : une attache de plus a chaque demarrage).
constexpr uint8_t kMatterMedModes = 3;
uint8_t matterMedMode();
bool matterSetMedMode(uint32_t mode, bool *saved);

// Plafond de l'intervalle max des abonnements NEUFS, en secondes : 0 = celui
// que demande le controleur, sinon kMatterMaxIntMinS..kMatterMaxIntMaxS.
constexpr uint16_t kMatterMaxIntMinS = 60;
constexpr uint16_t kMatterMaxIntMaxS = 3600;
uint16_t matterMaxIntervalCap();
bool matterSetMaxIntervalCap(uint32_t s, bool *saved);
#endif
