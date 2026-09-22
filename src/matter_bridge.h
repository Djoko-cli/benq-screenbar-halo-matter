#pragma once
#include <Arduino.h>

// Crée les endpoints Matter puis démarre la pile. À appeler après halo.begin()
// pour que l'état initial des endpoints reflète la vraie lampe.
void matterBridgeBegin();

// Reflète l'état de la lampe vers Matter. À appeler depuis loop().
void matterBridgePoll();

void matterPrintStatus(Print &out);
void matterDecommissionNow();
bool matterIsCommissioned();
bool matterIsConnected();
