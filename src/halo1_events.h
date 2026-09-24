#pragma once
// ===========================================================================
//  Evenements du pilote Halo 1, en donnees simples
//
//  Le pilote (Halo1Lamp) les produit au moment ou il les constate, dans la
//  tache loop, et les passe a ses crochets (Halo1Lamp::setHooks) ; le
//  protocole JSON (json_out.h, docs/PROTOCOLE-JSON.md section 7) les met en
//  forme. Le pilote ne connait pas le JSON, le JSON ne connait pas le pilote.
//
//  Pur et sans Arduino : teste sur l'hote (tools/host_tests).
// ===========================================================================
#include <stdint.h>

#include "halo1_proto.h"
#include "halo1_watch.h"

namespace halo1 {

// Tranches du pilote, dans l'ordre de Halo1Lamp::SLOT_* (verifie a la
// compilation dans halo1_lamp.cpp).
enum : uint8_t { EV_SLOT_BRIGHT, EV_SLOT_TEMP, EV_SLOT_AUTO, EV_SLOT_RAW, EV_SLOT_N };
// Verdicts d'un paquet, dans l'ordre de Halo1Radio::Verdict (idem).
enum : uint8_t { EV_TX_ACK, EV_TX_ACK_FOREIGN, EV_TX_MAX_RT, EV_TX_TIMEOUT, EV_TX_FIFO, EV_TX_N };

// Trame entendue : ecoute passive (listen, raw lu apres l'adresse), ou trame
// recue a la place d'un accuse (AckForeign : ni raw, ni PID, ni NO_ACK, CRC
// verifie par la puce).
struct RxEvent {
  bool listen;
  uint8_t raw[8];
  AirFrame f;
  Kind kind;  // classify(f)
  bool copy;  // Kind::Auto : copie d'un meme appui (AutoPressFilter::feed faux)
};

// Paquet emis, apres son verdict (Halo1Lamp::onVerdict), avant la fin de sa
// tranche.
struct TxEvent {
  uint32_t num;  // numero du paquet depuis le demarrage, a partir de 0
  uint8_t slot;  // EV_SLOT_*
  Payload pay;
  uint8_t attempt, repeats, acks;  // rang dans la tranche, paquets prevus, accuses (celui-ci compris)
  uint8_t verdict;                 // EV_TX_*
  uint16_t us;
  uint8_t rt2, irq1, status;
};

// Relance du module BM5602 : L2 (verification ou symptome) ou essai L3
// (cause None). Emise une fois le resultat connu.
struct RelaunchEvent {
  Relaunch cause;
  uint8_t rank;                // relances de suite sans guerison, celle-ci comprise ; 0 pour L3
  uint8_t timeoutRun;          // TxTimeout : delais de suite, lus avant relaunched()
  ChipWatch::Flood flood;      // RxNoise
  ChipWatch::Deaf deaf;        // RxDeaf
  uint32_t verifyFails;        // Verify : verifications ratees (Halo1Radio::Stats)
  bool ok;                     // halo.begin() a reussi
  int8_t crystal, calib;       // -1 : inconnu (relance ratee)
  uint32_t durMs;              // duree de la relance (boucle bloquee)
  uint32_t total;              // relances comptees apres celle-ci
  bool failed;                 // EN PANNE apres celle-ci
};

// Transitions du module annoncees par le pilote.
enum class ModuleState : uint8_t { Fault, Recovered, Lost, Found, ConfigRejected, ConfigVerified };
struct ModuleEvent {
  ModuleState state;
  uint8_t unrecovered;  // Fault : relances de suite sans guerison
  Relaunch symptom;     // Fault
  uint32_t retryS;      // Fault : un essai toutes les retryS secondes
  bool regsKnown;       // ConfigRejected : RFCH, DM1 et RT1 relus
  uint8_t rfch, dm1, rt1;
};

// Pourquoi une consigne a ete abandonnee (Halo1Lamp::giveUp).
enum class GiveUpCause : uint8_t { None, Unreachable, Module };

// Crochets du pilote. Chaque pointeur peut etre nul. Appeles dans la tache
// loop, jamais sous la garde d'antenne ni dans une section critique.
struct LampHooks {
  void (*rx)(const RxEvent &e);
  void (*tx)(const TxEvent &e);
  void (*relaunch)(const RelaunchEvent &e);
  void (*module)(const ModuleEvent &e);
  // Annonce (trace faux) ou trace du pilote, une ligne sans fin de ligne :
  // true si le crochet l'a prise (mode 'json log 1'), false pour l'afficher.
  bool (*log)(bool trace, const char *line);
};

}  // namespace halo1
