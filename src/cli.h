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
void ccCarrierDuty(Print &out, uint8_t csThr, uint32_t ms);
void ccAsyncCapture(Print &out, uint8_t count, uint32_t minRuns, uint32_t timeoutMs);
void ccAsyncScore(Print &out, uint32_t windowsWanted);
void ccRateSweep(Print &out, uint8_t mLo, uint8_t mHi, uint8_t passes);
void ccStream(Print &out, uint32_t nbits, uint8_t passes);
void ccDumpBursts(Print &out, int trigDbm, uint8_t count);
void ccCommonRuns(Print &out, int trigDbm);
void ccPulseWidths(Print &out, int trigDbm, uint16_t tries);
void ccCrcHunt(Print &out, uint32_t nbits, uint8_t repeats, uint16_t minLen, uint16_t maxLen);
void ccPresence(Print &out, uint32_t phaseMs);
void ccChannelScan(Print &out, uint32_t seconds, uint32_t dwellMs, int thrDbm);
void ccCsCapture(Print &out, uint32_t seconds, uint8_t maxWin, uint32_t minCs, uint32_t mhz, uint8_t agc);
void ccFrontEnd(Print &out, uint32_t dwellMs);
void ccFindAddress(Print &out, uint32_t nbits, uint8_t minRun, uint8_t repeats);
extern uint8_t ccPins[8];
void cliPoll();
