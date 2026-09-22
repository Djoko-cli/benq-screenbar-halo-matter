#pragma once

#include <Arduino.h>

// Petite console série de mise au point : mise en service radio, recherche
// d'adresse, sniffer, envoi de trames brutes, état Matter.
// `cliPoll()` est non bloquante et doit être appelée depuis loop().
void cliBegin();

// Identification du module CC2500 et de son etage d'entree.
void ccIdentify(Print &out);
void ccDiagnose(Print &out);
void ccRawProbe(Print &out);
void ccListen(Print &out, uint32_t dwellMs);
void ccCapture(Print &out, uint32_t pattern, uint32_t nbits);
void ccFreqOffset(Print &out, int trigDbm, uint32_t dwellMs);
void ccFreqSweep(Print &out, int32_t spanKhz, int32_t stepKhz, uint32_t dwellMs);
void ccStream(Print &out, uint32_t nbits, uint8_t passes);
void ccDumpBursts(Print &out, int trigDbm, uint8_t count);
void ccCommonRuns(Print &out, int trigDbm);
void ccPulseWidths(Print &out, int trigDbm, uint16_t tries);
void ccCrcHunt(Print &out, uint32_t nbits, uint8_t repeats, uint16_t minLen, uint16_t maxLen);
void ccPresence(Print &out, uint32_t phaseMs);
void ccFrontEnd(Print &out, uint32_t dwellMs);
void ccFindAddress(Print &out, uint32_t nbits, uint8_t minRun, uint8_t repeats);
extern uint8_t ccPins[8];
void cliPoll();
