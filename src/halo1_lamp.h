#pragma once
// Pilote produit de la lampe Halo 1 (plan du pilote Halo 1, B.5 et D) : consigne,
// etat cru, tranches d'emission, suivi de la telecommande, sauvegarde NVS.
//
// Tout se passe dans la tache loop() : pont Matter (apres coalescence), CLI,
// tick(). Aucune section critique : les callbacks Matter n'appellent jamais cet
// objet. Rien n'est emis sans une consigne explicite : ni au demarrage, ni sur
// minuterie (hors reprises d'une consigne en cours). Une trame entendue ne fait
// partir qu'un reglage deja demande et differe (D.6), jamais rien de neuf.
#include <Arduino.h>

#include "config.h"
#include "halo1_map.h"
#include "halo1_radio.h"

enum class Halo1Link : uint8_t { Unknown, Ok, Lost };

class Halo1Lamp {
 public:
  using RestartFn = bool (*)();  // relance complete du module (halo.begin())
  void begin(BC5602 &chip, bool listen, RestartFn restart);  // NVS -> cru = consigne ; N'EMET RIEN
  void tick();                    // <= ~35 ms au pire (un paquet, + 26 ms de garde Thread), typiquement < 1 ms
  void invalidateRadio() { radio.invalidate(); }
  // Acheve un reset en cours (une reconfiguration de 40 ms, jusqu'a 3 si la
  // verification echoue ; 200 ms au plus) : un outil de banc trouve alors la
  // puce en SPI 4 fils, et non au milieu d'une reconfiguration.
  void settleRadio();

  // --- consignes ---
  void request(const halo1::State &t, uint8_t fields);  // fields = FLD_* a livrer : TOUJOURS emis
  bool pressAuto();                                     // false si consigne eteinte
  void reassert() { request(target_, halo1::FLD_ALL); }
  const char *sendRaw(halo1::Payload p, bool force, uint8_t packets, uint16_t gapMs);  // nullptr = accepte
  // Comme une trame de la telecommande (cru + consigne), SANS EMETTRE : un
  // reglage differe reste a livrer jusqu'a la prochaine consigne.
  void believe(halo1::Payload p);

  // --- lecture ---
  const halo1::State &target() const { return target_; }
  const halo1::State &believed() const { return believed_; }
  uint8_t dirty() const { return dirty_; }          // champs de la consigne pas encore livres
  uint8_t confirmed() const { return confirmed_; }  // champs confirmes depuis le demarrage
  uint8_t memoryLamps() const { return selMem_.memory(target_); }
  uint32_t version() const { return version_; }  // +1 a chaque changement de consigne
  bool busy() const;                             // tranche active ou attente de reprise
  Halo1Link link() const { return link_; }
  uint8_t lastAuto() const { return lastAuto_; }
  // Appuis sur A entendus de la telecommande depuis le demarrage, jamais remis a
  // zero ('lampe stats raz' compris) : le pont Matter reflete chaque changement
  // par une impulsion d'EP4 s'il est expose (HALO1_EXPOSE_AUTO), sans rien
  // emettre. Les copies d'un meme appui comptent une fois ; nos propres trames
  // A n'y passent jamais (onAir seul).
  uint32_t remoteAutoCount() const { return remoteAutoPresses_; }
  // Consignes livrees en entier (une trame livree, et plus aucune tranche
  // active) et abandons ("injoignable"), depuis le demarrage, jamais remis a
  // zero : la LED d'etat en tire un eclat vert ou trois clignements rouges.
  // Une trame brute du banc ('lampe brut') n'y compte pas.
  uint32_t deliveredCount() const { return delivered_; }
  uint32_t giveUpCount() const { return giveUps_; }
  // Niveau L3 : relance du module ratee (module muet), ou sans effet
  // (configuration toujours rejetee). Rien n'est emis ; nouvel essai de relance
  // toutes les 60 s.
  bool lost() const { return (lost_ && !radio.present()) || (stuck_ && !relaunched_); }

  // Journal des derniers paquets emis, pour le bilan des commandes 'lampe'.
  enum : uint8_t { SLOT_BRIGHT, SLOT_TEMP, SLOT_AUTO, SLOT_RAW, SLOT_N };
  struct TxLog {
    halo1::Payload pay;
    uint8_t slot;
    Halo1Radio::Verdict v;
    uint8_t irq1, rt2, status;
    uint16_t us;
  };
  uint32_t txCount() const { return txCount_; }   // paquets emis depuis le demarrage
  bool txLog(uint32_t n, TxLog &out) const;       // paquet numero n ; false si deja ecrase

  // --- banc ---
  bool waitIdle(uint32_t maxMs);  // fait tourner tick() (+ delay(1)) jusqu'au repos
  void setListening(bool on) { listening_ = on; }
  bool listening() const { return listening_; }
  void setTrace(bool on) { trace_ = on; }
  bool tracing() const { return trace_; }
  bool setAddress(const uint8_t addrReg[4]);  // NVS "halo1/addr" ; refuse les interdites
  const uint8_t *address() const { return addrReg_; }
  void forget();       // efface "halo1/etat", cru = consigne = valeurs par defaut
  void persistNow();   // ecrit l'etat cru s'il differe du dernier ecrit
  void printStatus(Print &out) const;
  void printStats(Print &out) const;
  void clearStats();
  // "allumee deux lum A5 temp 35" (valeurs en hexa, comme sur l'air).
  static void describe(const halo1::State &s, char *buf, size_t n);
  // "marche/lampes+lum+temp", ou "-" si aucun champ.
  static void describeFields(uint8_t fields, char *buf, size_t n);

  struct Tuning {
    uint8_t repeats = HALO1_REPEATS;           // 3 paquets par trame
    uint8_t minAcks = HALO1_MIN_ACKS;          // 2 accuses pour reussir
    uint8_t maxAttempts = HALO1_MAX_ATTEMPTS;  // 5 paquets au plus par trame
    uint16_t gapMs = HALO1_GAP_MS;             // 100 entre deux paquets
    uint16_t retryMs = HALO1_RETRY_MS;         // 1000 x rang de l'echec
    uint8_t planRetries = HALO1_PLAN_RETRIES;  // 2 reprises avant abandon
  } tuning;
  struct Stats {
    uint32_t requests, packets, acks, ackForeign, maxRt, timeouts, fifoRefused, weakFails,
        preempted, cancelled, retries, giveUps, autoSent, autoIgnoredOff, rxFrames, rxCrcBad,
        rxLampAcks, rxState, rxAuto, rxService, rxReserved, rxInvalid, holdoffs, persisted,
        traceDropped, restarts;
  } stats{};
  Halo1Radio radio;

 private:
  enum class Phase : uint8_t { Idle, Burst, Backoff };
  struct Slot { bool active; halo1::Payload pay; uint8_t attempts, acks, repeats; };
  static constexpr uint8_t kLogN = 32;
  bool anyActive() const;
  void markPending(uint32_t now);
  // credit : une tranche remplacee ou annulee apres un accuse passe dans l'etat
  // cru (faux si ses accuses y sont deja, voir onRemotePayload). arm : faux, une
  // tranche inactive n'est jamais armee (believe() n'emet rien).
  void replan(uint32_t now, bool credit = true, bool arm = true);
  void setSlot(uint8_t id, bool want, halo1::Payload p, uint32_t now, bool credit, bool arm);
  int8_t pickSlot();
  void sendPacket(uint32_t now);
  void endBurst();
  void onVerdict(uint8_t id, const Halo1Radio::TxReport &r, uint32_t now);
  void complete(uint8_t id, uint32_t now);
  void fail(uint8_t id, uint32_t now);
  void giveUp();
  void restartModule(uint32_t now);
  void onAir(const halo1::AirFrame &f, uint32_t now);
  void onRemotePayload(halo1::Payload p, uint32_t now, bool arm = true);
  void noteAuto(uint8_t value, uint32_t now);
  void applyBelieved(halo1::Payload p, uint32_t now, bool confirm);
  void schedulePersist(uint32_t now);
  void maybePersist(uint32_t now);
  void loadNvs();
  void buildBlob(uint8_t b[8]) const;
  void saveState();
  void traceRadio();
  // rien si !trace_ ; perdu (traceDropped) plutot que d'attendre le port serie
  void trace(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void notice(const char *msg);  // toujours affiche, mais jamais bloquant

  halo1::State target_, believed_;
  uint8_t dirty_ = 0, confirmed_ = 0, lastAuto_ = 0, rr_ = 0, failures_ = 0;
  Slot slots_[SLOT_N] = {};
  Phase phase_ = Phase::Idle;
  uint32_t version_ = 1, nextTxAt_ = 0, retryAt_ = 0, remoteAt_ = 0, pendingSince_ = 0,
           lastTxEndAt_ = 0, persistFirst_ = 0, persistDue_ = 0, lastAckAt_ = 0, restartAt_ = 0;
  // Appuis A de la telecommande comptes, copies ecartees par remoteAuto_.
  uint32_t remoteAutoPresses_ = 0;
  uint32_t delivered_ = 0, giveUps_ = 0;  // deliveredCount(), giveUpCount()
  halo1::AutoPressFilter remoteAuto_;
  uint16_t rawGapMs_ = 0;
  bool listening_ = false, trace_ = false, persistDirty_ = false, holding_ = false, lost_ = false,
       acked_ = false;
  // relaunched_ : relance reussie, aucune configuration verifiee depuis
  // (radio.stats.fullConfigs valait relaunchConfigs_) ; une nouvelle demande de
  // relance passe alors en L3 (stuck_) au lieu de relancer en boucle.
  bool relaunched_ = false, stuck_ = false;
  uint32_t relaunchConfigs_ = 0;
  Halo1Link link_ = Halo1Link::Unknown;
  uint8_t addrReg_[4] = {0x4F, 0xF0, 0xFD, 0x63};
  halo1::SelectionMemory selMem_;
  uint8_t saved_[8] = {};  // dernier blob "etat" lu ou ecrit
  RestartFn restart_ = nullptr;
  TxLog log_[kLogN] = {};
  uint32_t txCount_ = 0;
  uint32_t seenSilence_ = 0, seenTxReconf_ = 0, seenVerify_ = 0;  // traces de la radio
};
extern Halo1Lamp lamp;
