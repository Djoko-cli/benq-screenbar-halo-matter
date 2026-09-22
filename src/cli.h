#pragma once

// Petite console série de mise au point : mise en service radio, recherche
// d'adresse, sniffer, envoi de trames brutes, état Matter.
// `cliPoll()` est non bloquante et doit être appelée depuis loop().
void cliBegin();
void cliPoll();
