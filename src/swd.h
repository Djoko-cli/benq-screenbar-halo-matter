#pragma once

#include <Arduino.h>

// Dialogue SWD (Serial Wire Debug) genere en bit-banging.
//
// Deux fils seulement : SWCLK, pilote par nous, et SWDIO, bidirectionnel avec
// des cycles de retournement. Le debit importe peu ici -- on vise la fiabilite,
// pas la vitesse, et quelques centaines de kilohertz suffisent largement a lire
// une memoire flash.
namespace swd {

// Registres du Debug Port, adresses par A[3:2].
constexpr uint8_t DP_IDCODE = 0x0;  // en lecture ; ABORT en ecriture
constexpr uint8_t DP_CTRLSTAT = 0x4;
constexpr uint8_t DP_SELECT = 0x8;  // en ecriture ; RESEND en lecture
constexpr uint8_t DP_RDBUFF = 0xC;  // en lecture

// Acquittements renvoyes par la cible, sur trois bits.
constexpr uint8_t ACK_OK = 0b001;
constexpr uint8_t ACK_WAIT = 0b010;
constexpr uint8_t ACK_FAULT = 0b100;

// Choisit les broches utilisees. A appeler avant toute autre fonction.
void begin(uint8_t clkPin, uint8_t ioPin, uint16_t halfPeriodUs = 2);

// Sequence d'accroche : reset de ligne, bascule JTAG vers SWD, reset de ligne.
// A l'issue, la cible doit repondre a une lecture d'IDCODE.
void connect();

// Un echange elementaire. Renvoie l'acquittement ; la donnee lue va dans *data.
uint8_t transfer(bool accessPort, bool read, uint8_t addr, uint32_t *data);

// Lit l'IDCODE du Debug Port. Renvoie false si la cible ne repond pas.
bool readIdcode(uint32_t *idcode, uint8_t *ack);

// Demande la mise sous tension des domaines de debug et verifie l'acquittement
// materiel. C'est la preuve qu'on ne fait pas que lire un IDCODE par hasard :
// la cible nous rend la main sur son coeur de debug.
bool powerUpDebug(uint32_t *ctrlStat);

// Balaie plusieurs broches candidates pour SWDIO et rapporte celle qui repond.
// Evite d'avoir a recabler entre chaque essai : on relie les trous inconnus a
// plusieurs broches de l'ESP32 d'un coup, et le firmware trouve la bonne.
void probeCandidates(Print &out, uint8_t clkPin, const uint8_t *candidates, uint8_t count);

}  // namespace swd
