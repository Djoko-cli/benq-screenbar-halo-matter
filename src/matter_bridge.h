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
