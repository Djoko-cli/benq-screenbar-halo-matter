#pragma once

#include <Arduino.h>

// Petite console série de mise au point : mise en service radio, recherche
// d'adresse, sniffer, envoi de trames brutes, état Matter.
// `cliPoll()` est non bloquante et doit être appelée depuis loop().
void cliBegin();

// Identification du module CC2500 et de son etage d'entree.
void ccIdentify(Print &out);
extern uint8_t ccPins[8];
void cliPoll();
