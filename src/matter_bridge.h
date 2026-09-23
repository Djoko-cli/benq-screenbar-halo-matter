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
void matterDecommissionNow();
bool matterIsCommissioned();
bool matterIsConnected();
