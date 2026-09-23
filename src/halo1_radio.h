#pragma once
// Radio du pilote Halo 1 : les sequences BC5602 prouvees au banc ('txack',
// 'ecoute'), rejouees sans bloquer sur les 40 ms du reset. Aucune trace ici :
// le pilote decide de ce qu'il affiche.
#include "bc5602.h"
#include "config.h"
#include "halo1_proto.h"

// Corps de l'ancien configStdAutoAck de halo.cpp, decoupe autour de ses
// 2 x 20 ms d'attente. Comportement sur l'air IDENTIQUE.
void halo1StdReset(BC5602 &r);  // CMD_SOFTWARE_RESET seul, sans attente
// Format standard avec accuse automatique (PCF 9 bits, charge dynamique, CRC-16,
// ENAA pipe 0, ARD 2 ms / ARC 3). Laisse CE a 0, drapeaux acquittes, FIFO videes.
void halo1StdConfigure(BC5602 &r, const uint8_t addrReg[4], uint8_t ch, uint8_t rate, bool receiver);
// Ecoute passive par-dessus halo1StdConfigure : jamais d'accuse, 64 bits lus
// apres l'adresse, PCF et CRC decodes en logiciel. N'entre pas en reception.
void halo1PassiveOverrides(BC5602 &r);  // ENAA 0, DPL2 0, DPL1 0, PKT1 0, RXPW0 8
// Version bloquante pour txAck / sniffStd / prxAck : reset + delay(40) + configure.
void configStdAutoAck(BC5602 &r, const uint8_t addrReg[4], uint8_t ch, uint8_t rate, bool receiver);
void applyXoTrim(BC5602 &r);  // defini dans halo.cpp : trim du quartz memorise par 'xo'

// Garde d'antenne : une autre radio de la carte se tait pendant chaque paquet.
// Seul le build Thread en fournit une (matter_bridge.cpp) : le terrain du 23/09
// y montre des MAX_RT en rafale juste apres chaque commande Apple Home (19
// paquets sur 64 en MED), 0 au banc sans Thread. Le pilote ne connait ni
// OpenThread ni Matter : il appelle ces deux crochets, rien d'autre.
struct Halo1AirGuard {
  // Juste avant le paquet. true : garde tenue, leave() suivra le verdict.
  // false : garde refusee (verrou pas obtenu a temps), paquet emis sans elle.
  // *waitedUs : attente active de la fin d'une emission deja partie, 0 si
  // l'autre radio n'emettait pas ; maxWaitUs exactement si elle emettait
  // encore au plafond, moins sinon.
  bool (*enter)(uint32_t maxWaitUs, uint32_t *waitedUs);
  void (*leave)();
};

class Halo1Radio {
 public:
  enum class Mode : uint8_t { Unknown, Resetting, Tx, Rx, Sleep };
  enum class Verdict : uint8_t { Ack, AckForeign, MaxRt, Timeout, FifoRefused };
  // Cause d'une reconfiguration complete (Snapshot::why).
  enum : uint8_t { WHY_MODE, WHY_SILENCE, WHY_TX, WHY_VERIFY };
  struct TxReport {
    Verdict v; uint8_t irq1, rt2, status; uint16_t us;
    // Trame recue a la place de l'accuse (AckForeign). fLen = PKT4 brut, jusqu'a
    // 32 : fPay n'est rempli que si 1 <= fLen <= 4.
    uint8_t fLen; uint8_t fPay[4];
  };
  struct Tuning {
    uint16_t resetWaitMs = HALO1_RESET_WAIT_MS;  // 40 : chemin prouve
    uint16_t rearmMs = HALO1_RX_REARM_MS;        // 100 (= sniffStd)
    uint16_t silenceMs = HALO1_RX_SILENCE_MS;    // 500 (= sniffStd, 06a3185)
    bool strongRearm = false;  // LIGHT_SLEEP puis RX : NON PROUVE (banc T10)
    bool lightSwitch = false;  // bascule TX<->RX sans reset : NON PROUVE (banc T10)
    bool airGuard = true;      // garde d'antenne, si le build en fournit une ('lampe garde')
  };
  struct Stats {
    // rearmsOffRx : ceux des rearmements faits sur OMST != RX (les autres sont
    // periodiques). Incident du 24/09, puce sourde : ~450 par seconde ; la
    // surveillance en fait un symptome (halo1_watch.h, surdite).
    uint32_t fullConfigs, silenceReconf, txReconf, verifyFail, rearms, rearmsOffRx, rxRaw, lightSwitches;
    // Garde d'antenne : paquets gardes, gardes refusees, attentes d'une emission
    // en cours (dont plafonnees a HALO1_AIR_GUARD_WAIT_US), plus longue attente.
    uint32_t guarded, guardRefused, guardWaits, guardCapped, guardMaxUs;
  };
  // Pris avant chaque reconfiguration de silence, d'echec TX ou de verification.
  // Un silence qui suit un silence remplace le precedent : au calme, il en vient
  // un toutes les ~540 ms, qui chasserait sinon les instantanes d'echec.
  struct Snapshot {
    uint32_t atMs; uint8_t why, sta1, irq1, status, mask, ce, cfg1, rfch, dm1, pkt1, enaa,
        dpl1, dpl2, rxpw0, rt1;
  };

  void begin(BC5602 &chip, const uint8_t addrReg[4]);  // aucun acces SPI
  void setAddress(const uint8_t addrReg[4]);           // + invalidate()
  const uint8_t *air() const { return air_; }
  bool present() const { return chip_ && chip_->present(); }
  const BC5602 *chip() const { return chip_; }  // quartz et calibration apres une relance
  void invalidate() { mode_ = Mode::Unknown; configured_ = false; }  // un outil CLI a touche la puce
  // Tx, Rx ou Sleep. Sleep : CE=0 et LIGHT_SLEEP, sans reset. Tx ou Rx depuis un
  // autre mode : reconfiguration complete (reset, puis configuration apres
  // resetWaitMs dans service()), ou bascule legere si tuning.lightSwitch.
  void request(Mode m, uint32_t nowMs);
  bool ready(Mode m) const { return mode_ == m; }
  Mode mode() const { return mode_; }
  void service(uint32_t nowMs);                        // acheve un reset apres resetWaitMs
  bool restartWanted() const { return restartWanted_; }
  void restartDone() {  // apres halo.begin(), qui a reecrit IO1
    restartWanted_ = false; verifyFails_ = 0; spi3Wire_ = false; mode_ = Mode::Unknown; configured_ = false;
  }
  // Lecture SPI possible : faux entre un reset et la configuration qui le suit
  // (SPI 3 fils, bc5602.cpp), y compris si invalidate() a coupe ce reset.
  bool readable() const { return present() && !spi3Wire_; }
  // La puce porte la configuration du pilote : halo1StdConfigure a tourne depuis
  // le dernier reset, outil de banc ou relance. Sinon, RFCH/DM1/RT1 sont ceux de
  // halo.begin() ou du dernier outil (une mise en veille ne configure rien).
  bool configured() const { return configured_; }
  // Exige ready(Tx), sinon FifoRefused sans toucher a la puce. Bloquant : <= 30 ms
  // d'attente active (1,6-1,7 ms mesures), plus l'entree de la garde d'antenne
  // (build Thread : <= 26 ms), rendue au plus 13 ms apres CE=1. Apres tout
  // verdict autre que Ack/AckForeign : lance une reconfiguration vers Tx.
  TxReport sendOne(const uint8_t *pay, uint8_t len, uint32_t nowMs);
  // Garde tenue autour de chaque paquet de sendOne, jamais entre deux paquets.
  // nullptr la retire. Sans garde, 'lampe garde' ne regle rien.
  void setAirGuard(const Halo1AirGuard *g) { guard_ = g; }
  bool hasAirGuard() const { return guard_ != nullptr; }
  // Exige ready(Rx). Une iteration de la boucle de sniffStd ; true = raw[8] rempli.
  bool pollRx(uint32_t nowMs, uint8_t raw[8]);
  uint8_t snapshots(Snapshot *out, uint8_t max) const;  // du plus recent au plus ancien
  bool readConfig(uint8_t out[3]);  // RFCH, DM1, RT1 relus ; false si !readable()

  Tuning tuning;
  Stats stats{};

 private:
  static constexpr uint8_t kSnaps = 4;
  void beginReset(Mode target, uint32_t nowMs, uint8_t why);
  void finishReset(uint32_t nowMs);
  bool verify();  // RFCH == 5, DM1 == 0x82, RT1 == 0x73
  void takeSnapshot(uint8_t why, uint32_t nowMs);
  void lightToTx();
  void lightToRx();
  const Halo1AirGuard *guardEnter();  // garde tenue, ou nullptr : rien a rendre
  BC5602 *chip_ = nullptr;
  const Halo1AirGuard *guard_ = nullptr;  // survit a begin() et aux relances
  Mode mode_ = Mode::Unknown, target_ = Mode::Unknown;
  uint8_t addrReg_[4] = {0x4F, 0xF0, 0xFD, 0x63}, air_[4] = {0x63, 0xFD, 0xF0, 0x4F};
  uint32_t resetAt_ = 0, lastArm_ = 0, lastFrame_ = 0, lastFull_ = 0;
  uint8_t verifyFails_ = 0;
  bool restartWanted_ = false;
  bool spi3Wire_ = false;  // reset envoye, IO1 pas encore reecrit : aucune lecture
  bool configured_ = false;
  Snapshot snaps_[kSnaps] = {};
  uint8_t snapIdx_ = 0, snapN_ = 0;
};
