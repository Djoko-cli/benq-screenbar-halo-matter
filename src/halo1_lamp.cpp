#include "halo1_lamp.h"

#include <Preferences.h>
#include <stdarg.h>
#include <string.h>

using namespace halo1;
using Mode = Halo1Radio::Mode;
using Verdict = Halo1Radio::Verdict;

Halo1Lamp lamp;

// Espace de noms NVS du pilote. 'benqhalo' est ignore : ses 'chan' et 'rate'
// peuvent contenir des valeurs d'essai, et les outils de banc s'en servent.
static const char *const kNvsNs = "halo1";
static constexpr uint8_t kBlobVersion = 1;
// Jamais d'ecriture NVS moins de 500 ms apres une emission (D.10).
static constexpr uint32_t kPersistAfterTxMs = 500;
// Module perdu (L3) : nouvel essai de relance toutes les 60 s.
static constexpr uint32_t kLostRetryMs = 60000;

// ---------------------------------------------------------------------------
//  Textes
// ---------------------------------------------------------------------------

static const char *lampsText(uint8_t lamps) {
  switch (lamps & F_LAMPS) {
    case F_FRONT: return "avant";
    case F_BACK: return "arriere";
    case F_LAMPS: return "deux";
    default: return "aucune";
  }
}

static const char *verdictText(Verdict v) {
  switch (v) {
    case Verdict::Ack: return "ACK";
    case Verdict::AckForeign: return "ACK+TRAME";
    case Verdict::MaxRt: return "MAX_RT";
    case Verdict::Timeout: return "DELAI";
    case Verdict::FifoRefused: return "FIFO";
  }
  return "?";
}

static const char *const kSlotText[Halo1Lamp::SLOT_N] = {"lum", "temp", "A", "brut"};

void Halo1Lamp::describe(const State &s, char *buf, size_t n) {
  snprintf(buf, n, "%s %s lum %02X temp %02X", s.power ? "allumee" : "eteinte", lampsText(s.lamps), s.bright,
           s.temp);
}

void Halo1Lamp::describeFields(uint8_t fields, char *buf, size_t n) {
  if (!n) return;
  buf[0] = 0;
  if (!(fields & FLD_ALL)) {
    snprintf(buf, n, "-");
    return;
  }
  size_t w = 0;
  if (fields & FLD_FLAGS) w += (size_t)snprintf(buf + w, n - w, "marche/lampes");
  if ((fields & FLD_BRIGHT) && w < n) w += (size_t)snprintf(buf + w, n - w, "%slum", w ? "+" : "");
  if ((fields & FLD_TEMP) && w < n) snprintf(buf + w, n - w, "%stemp", w ? "+" : "");
}

void Halo1Lamp::trace(const char *fmt, ...) {
  if (!trace_) return;
  char line[112];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  // Le pilote ne doit jamais attendre le port serie : une trace qui ne tient
  // pas dans le tampon d'emission est perdue et comptee.
  if (n < 0 || Serial.availableForWrite() < n + 2) {
    stats.traceDropped++;
    return;
  }
  Serial.println(line);
}

void Halo1Lamp::notice(const char *msg) {
  if (Serial.availableForWrite() < (int)strlen(msg) + 2) {
    stats.traceDropped++;
    return;
  }
  Serial.println(msg);
}

// ---------------------------------------------------------------------------
//  Cycle de vie
// ---------------------------------------------------------------------------

void Halo1Lamp::begin(BC5602 &chip, bool listen, RestartFn restart) {
  // Table gamma construite ici, dans les deux builds : le build diag n'a pas de
  // pont Matter, et le repli de halo1_map ignore HALO1_LEVEL_GAMMA.
  mapInit(HALO1_LEVEL_GAMMA);
  restart_ = restart;
  listening_ = listen;
  loadNvs();  // cru = consigne = blob NVS, ou valeurs par defaut
  dirty_ = confirmed_ = 0;
  selMem_.reset(target_.lamps);
  const uint32_t now = millis();
  // Comme si la derniere trame de la telecommande et la derniere emission
  // etaient anciennes : ni attente, ni sauvegarde retardee au demarrage.
  remoteAt_ = lastTxEndAt_ = now - 60000;
  radio.begin(chip, addrReg_);  // aucun acces SPI
  // Rien n'est emis : ecoute passive (jamais d'accuse) ou veille.
  radio.request(listen ? Mode::Rx : Mode::Sleep, now);
}

void Halo1Lamp::tick() {
  const uint32_t now = millis();
  if (!radio.present() || (stuck_ && !relaunched_)) {
    // L3 : relance ratee (module muet) ou sans effet (configuration toujours
    // rejetee). Pilote inactif, nouvel essai de relance toutes les 60 s.
    if (lost()) {
      if (busy()) giveUp();  // une consigne arrivee pendant la perte ne partirait jamais
      if (restart_ && (uint32_t)(now - restartAt_) >= kLostRetryMs) restartModule(now, Relaunch::None);
    }
    return;
  }
  radio.service(now);
  if (relaunched_ && radio.stats.fullConfigs != relaunchConfigs_) {
    relaunched_ = false;  // configuration verifiee : la relance a gueri la puce
    if (stuck_) notice("[lampe] BM5602 retrouve : configuration verifiee");
    stuck_ = false;
  }
  if (radio.restartWanted()) {  // L2 : 3 verifications ratees de suite
    restartModule(now, Relaunch::Verify);
    if (!radio.present() || (stuck_ && !relaunched_)) return;
  } else if (restart_) {
    // L2 sur symptome de puce (incident du 24/09, halo1_watch.h) : delais TX en
    // serie, deluge de CRC faux en ecoute ; jamais une lampe muette. Au milieu
    // d'une rafale aussi : ses paquets restants partent de la puce relancee.
    // Seul tick() relance : un outil de banc tourne dans la CLI, sans tick(),
    // puis invalide la radio et efface les preuves (invalidateRadio).
    const Relaunch why = watch_.due(now);
    noteFault();
    if (why != Relaunch::None) {
      restartModule(now, why);
      if (!radio.present() || (stuck_ && !relaunched_)) return;
    }
  }
  traceRadio();
  if (phase_ == Phase::Backoff && (int32_t)(now - retryAt_) >= 0) phase_ = Phase::Idle;
  if (phase_ == Phase::Idle) {
    if (!anyActive()) {
      pendingSince_ = 0;  // un reglage differe seul n'attend rien
    } else {
      // Tranche armee sans demande (reglage differe libere par une trame de la
      // telecommande) : l'attente compte a partir d'ici.
      markPending(now);
      // On ne se bat pas avec la telecommande : pas de rafale juste apres l'une
      // de ses trames, sauf pour une demande qui attend depuis trop longtemps.
      if ((uint32_t)(now - remoteAt_) < HALO1_REMOTE_HOLDOFF_MS &&
          (uint32_t)(now - pendingSince_) < HALO1_REMOTE_HOLDOFF_MAX_MS) {
        if (!holding_) {
          holding_ = true;
          stats.holdoffs++;
          trace("[lampe] attente : la telecommande vient de parler");
        }
      } else {
        phase_ = Phase::Burst;
        nextTxAt_ = now;
        rr_ = 0;  // luminosite d'abord quand les deux tranches sont actives
      }
    }
  }
  if (phase_ != Phase::Idle || !anyActive()) holding_ = false;  // une attente par demande
  if (phase_ == Phase::Burst) {
    if (anyActive()) {
      // Redemande a chaque passage : un outil de banc a pu invalider la radio
      // au milieu d'une rafale. Sans effet si la puce est deja en Tx.
      radio.request(Mode::Tx, now);
      if (radio.ready(Mode::Tx) && (int32_t)(now - nextTxAt_) >= 0) sendPacket(now);
    }
    if (phase_ == Phase::Burst && !anyActive()) endBurst();
  }
  if (phase_ != Phase::Burst) {
    if (listening_) {
      radio.request(Mode::Rx, now);
      uint8_t raw[8];
      if (radio.ready(Mode::Rx) && radio.pollRx(now, raw)) {
        const AirFrame f = decodeAir(raw, radio.air());
        watch_.rxFrame(f.crcOk, now);  // deluge de CRC faux : symptome de puce
        onAir(f, now);
      }
    } else {
      radio.request(Mode::Sleep, now);
    }
  }
  selMem_.update(target_, now, HALO1_SELECTION_STABLE_MS);
  // Horloge fraiche : un paquet emis dans ce tour a note sa fin (et l'echeance
  // de sauvegarde) apres 'now'.
  maybePersist(millis());
}

void Halo1Lamp::settleRadio() {
  const uint32_t t0 = millis();
  while (radio.present() && radio.mode() == Mode::Resetting && (uint32_t)(millis() - t0) < 200) {
    radio.service(millis());
    delay(1);
  }
}

bool Halo1Lamp::waitIdle(uint32_t maxMs) {
  const uint32_t t0 = millis();
  while (busy()) {
    if (!radio.present() || (uint32_t)(millis() - t0) >= maxMs) return false;
    tick();
    delay(1);
  }
  return true;
}

// L2 : relance complete du module (halo.begin(), ~300 ms bloquant, rare), puis
// reconfiguration. Relance ratee, ou sans effet sur la verification (aucune
// configuration verifiee depuis la precedente) : L3, pilote inactif, nouvel
// essai toutes les 60 s. Les relances L2 (verification ou symptome) sont
// annoncees et comptees par la surveillance ; les essais L3 ne font qu'effacer
// ses preuves.
void Halo1Lamp::restartModule(uint32_t now, Relaunch cause) {
  restartAt_ = now;
  if (cause == Relaunch::Verify && relaunched_ && radio.stats.fullConfigs == relaunchConfigs_) {
    // halo.begin() n'y peut rien : relancer en boucle bloquerait loop() ~300 ms
    // toutes les ~130 ms, sans jamais rendre la radio prete. Pas de
    // restartDone() : la radio reste inerte (demande de relance levee) et garde
    // la configuration rejetee, que 'lampe regs' montre.
    relaunched_ = false;
    if (!stuck_) {  // une annonce par panne, pas une par essai
      char msg[144];
      uint8_t c[3];
      if (radio.readConfig(c))
        snprintf(msg, sizeof(msg),
                 "[lampe] BM5602 : configuration rejetee apres relance (RFCH %02X DM1 %02X RT1 %02X, attendu 05 82 "
                 "73) : nouvel essai toutes les 60 s",
                 c[0], c[1], c[2]);
      else
        snprintf(msg, sizeof(msg), "[lampe] BM5602 : configuration rejetee apres relance : nouvel essai toutes les 60 s");
      notice(msg);
    }
    stuck_ = true;
    if (busy()) giveUp();
    return;
  }
  if (cause == Relaunch::None) {
    watch_.forget(now);
  } else {
    announceRelaunch(cause);  // avant les ~300 ms de halo.begin()
    watch_.relaunched(cause, now);
  }
  const bool ok = restart_ && restart_();
  radio.restartDone();
  if (ok) {
    stats.restarts++;
    if (lost_) notice("[lampe] BM5602 retrouve");
    lost_ = false;
    // stuck_ reste leve jusqu'a une configuration verifiee (tick).
    relaunched_ = true;
    relaunchConfigs_ = radio.stats.fullConfigs;
    trace("[lampe] RADIO module relance");
    return;
  }
  if (!lost_) notice("[lampe] BM5602 perdu : nouvel essai de relance toutes les 60 s");
  lost_ = true;
  stuck_ = relaunched_ = false;  // module muet : sa configuration ne se juge plus
  if (busy()) giveUp();
}

void Halo1Lamp::announceRelaunch(Relaunch cause) {
  char msg[176];
  const unsigned n = (unsigned)watch_.unrecovered() + 1u;  // celle-ci comprise
  switch (cause) {
    case Relaunch::TxTimeout:
      snprintf(msg, sizeof(msg),
               "[lampe] BM5602 : %u paquets de suite sans TX_DS ni MAX_RT en 30 ms : relance automatique du module "
               "(%u depuis la derniere guerison)",
               (unsigned)watch_.timeoutRun(), n);
      break;
    case Relaunch::RxNoise: {
      const ChipWatch::Flood &f = watch_.lastFlood();
      snprintf(msg, sizeof(msg),
               "[lampe] BM5602 : deluge en ecoute (%u trames en %lu ms, %u au CRC faux) : relance automatique du "
               "module (%u depuis la derniere guerison)",
               (unsigned)f.frames, (unsigned long)f.ms, (unsigned)f.bad, n);
      break;
    }
    default:
      snprintf(msg, sizeof(msg),
               "[lampe] BM5602 : configuration rejetee 3 fois de suite : relance automatique du module (%u depuis la "
               "derniere guerison)",
               n);
      break;
  }
  notice(msg);
}

void Halo1Lamp::noteFault() {
  const bool failed = watch_.failed();
  if (failed == faultSeen_) return;
  faultSeen_ = failed;
  char msg[176];
  if (failed)
    snprintf(msg, sizeof(msg),
             "[lampe] BM5602 EN PANNE : %u relances automatiques de suite sans guerison, le symptome revient (%s) : "
             "un essai toutes les %lu min",
             (unsigned)watch_.unrecovered(), relaunchText(watch_.symptom()),
             (unsigned long)(ChipWatch::kBackoffMs / 60000));
  else
    snprintf(msg, sizeof(msg), "[lampe] BM5602 retabli : accuse, ou trame au CRC juste hors deluge, depuis la relance");
  notice(msg);
}

void Halo1Lamp::traceRadio() {
  const Halo1Radio::Stats &r = radio.stats;
  if (r.silenceReconf != seenSilence_) {
    seenSilence_ = r.silenceReconf;
    trace("[lampe] RADIO reconf silence #%lu", (unsigned long)seenSilence_);
  }
  if (r.txReconf != seenTxReconf_) {
    seenTxReconf_ = r.txReconf;
    trace("[lampe] RADIO reconf apres echec TX #%lu", (unsigned long)seenTxReconf_);
  }
  if (r.verifyFail != seenVerify_) {
    seenVerify_ = r.verifyFail;
    trace("[lampe] RADIO verification ratee #%lu", (unsigned long)seenVerify_);
  }
}

// ---------------------------------------------------------------------------
//  Consignes
// ---------------------------------------------------------------------------

void Halo1Lamp::markPending(uint32_t now) {
  if (!pendingSince_) pendingSince_ = now ? now : 1;  // 0 = aucune demande en attente
}

void Halo1Lamp::request(const State &t, uint8_t fields) {
  const uint32_t now = millis();
  fields &= FLD_ALL;
  if (!fields) return;
  stats.requests++;
  State n = target_;
  if (fields & FLD_FLAGS) {
    n.power = t.power;
    // Jamais 0 : meme garde-fou que les constructeurs de trames.
    n.lamps = (t.lamps & F_LAMPS) ? (uint8_t)(t.lamps & F_LAMPS) : F_FRONT;
  }
  if (fields & FLD_BRIGHT) n.bright = clampBright(t.bright);
  if (fields & FLD_TEMP) n.temp = clampTemp(t.temp);
  if (n != target_) version_++;
  target_ = n;
  // Toujours emis, meme si la lampe est crue deja dans cet etat : c'est la
  // resynchronisation. Allumee, FLAGS rend aussi due la luminosite (A4 (a)).
  dirty_ |= dueFields(target_, fields);
  markPending(now);
  failures_ = 0;
  if (phase_ == Phase::Backoff) phase_ = Phase::Idle;  // nouvelle intention : tout de suite
  replan(now);
}

bool Halo1Lamp::pressAuto() {
  // L'effet de A lampe eteinte n'est pas connu : on ne l'envoie pas.
  if (!target_.power) {
    stats.autoIgnoredOff++;
    return false;
  }
  Slot &a = slots_[SLOT_AUTO];
  if (a.active) return true;  // les appuis rapproches fusionnent
  const uint32_t now = millis();
  // Numero reserve des l'appui et reutilise tel quel a chaque reprise : la
  // lampe ignore un numero deja traite, donc jamais de double declenchement.
  lastAuto_ = nextAuto(lastAuto_);
  schedulePersist(now);
  a = Slot{true, makeAuto(true, target_.lamps, lastAuto_), 0, 0, tuning.repeats};
  markPending(now);
  if (phase_ == Phase::Backoff) phase_ = Phase::Idle;
  return true;
}

const char *Halo1Lamp::sendRaw(Payload p, bool force, uint8_t packets, uint16_t gapMs) {
  if (p.flags == 0x0A || p.flags == 0xFA)
    return "refuse : 0A (appairage Halo 2) et FA (annonce de la telecommande) ne sont jamais emis";
  const Kind k = kindOf(p);
  if (!force && (k == Kind::Service || k == Kind::Reserved || k == Kind::Invalid))
    return "refuse : trame de service, de favori ou invalide ('force' pour l'emettre quand meme)";
  if (!packets) packets = 1;
  const uint32_t now = millis();
  slots_[SLOT_RAW] = Slot{true, p, 0, 0, packets};
  rawGapMs_ = gapMs;
  markPending(now);
  if (phase_ == Phase::Backoff) phase_ = Phase::Idle;
  return nullptr;
}

void Halo1Lamp::believe(Payload p) {
  const uint32_t now = millis();
  if (kindOf(p) == Kind::Auto) noteAuto(p.value, now);
  // Sans emettre (D.2) : une tranche deja active suit la nouvelle consigne, mais
  // un reglage differe reste a livrer jusqu'a la prochaine consigne.
  else onRemotePayload(p, now, false);
}

// ---------------------------------------------------------------------------
//  Tranches (D.4)
// ---------------------------------------------------------------------------

bool Halo1Lamp::anyActive() const {
  for (const Slot &s : slots_)
    if (s.active) return true;
  return false;
}

bool Halo1Lamp::busy() const { return anyActive() || phase_ == Phase::Backoff; }

void Halo1Lamp::replan(uint32_t now, bool credit, bool arm) {
  // Deux passes au plus : une tranche abandonnee apres un accuse met a jour
  // l'etat cru, dont la trame d'extinction reprend la temperature.
  for (uint8_t pass = 0; pass < 2; pass++) {
    const State before = believed_;
    const Plan p = plan(target_, believed_, dirty_);
    setSlot(SLOT_BRIGHT, p.bright, p.pb, now, credit, arm);
    setSlot(SLOT_TEMP, p.temp, p.pt, now, credit, arm);
    if (believed_ == before) break;
  }
}

void Halo1Lamp::setSlot(uint8_t id, bool want, Payload p, uint32_t now, bool credit, bool arm) {
  Slot &s = slots_[id];
  if (!want) {
    if (!s.active) return;
    if (credit && s.acks) applyBelieved(s.pay, now, false);  // la lampe l'a sans doute appliquee
    s.active = false;
    stats.cancelled++;
    return;
  }
  if (!s.active && !arm) return;  // 'lampe croire' : rien de neuf ne part
  if (s.active && s.pay == p) return;  // meme charge : on garde les compteurs
  if (s.active) {
    // Un curseur qui bouge : la valeur finale obtient toujours sa rafale complete.
    if (credit && s.acks) applyBelieved(s.pay, now, false);
    stats.preempted++;
  }
  s = Slot{true, p, 0, 0, tuning.repeats};
}

int8_t Halo1Lamp::pickSlot() {
  if (slots_[SLOT_RAW].active) return SLOT_RAW;
  const bool b = slots_[SLOT_BRIGHT].active, t = slots_[SLOT_TEMP].active;
  if (b && t) {
    const uint8_t id = rr_ ? SLOT_TEMP : SLOT_BRIGHT;
    rr_ ^= 1;
    return (int8_t)id;
  }
  if (b) return SLOT_BRIGHT;
  if (t) return SLOT_TEMP;
  Slot &a = slots_[SLOT_AUTO];
  if (!a.active) return -1;
  // Eteinte depuis l'appui, meme au milieu de sa rafale : A n'est plus envoye
  // (effet inconnu lampe eteinte).
  if (!target_.power) {
    a.active = false;
    stats.autoIgnoredOff++;
    trace("[lampe] A %u abandonne : lampe eteinte", a.pay.value);
    return -1;
  }
  // Premier paquet : drapeaux refaits avec les lampes de la consigne, meme numero (Q8).
  if (!a.attempts) a.pay = makeAuto(true, target_.lamps, a.pay.value);
  return SLOT_AUTO;
}

void Halo1Lamp::sendPacket(uint32_t now) {
  const int8_t id = pickSlot();
  if (id < 0) return;
  Slot &s = slots_[id];
  const uint8_t pay[2] = {s.pay.flags, s.pay.value};
  const Halo1Radio::TxReport r = radio.sendOne(pay, 2, now);
  const uint32_t end = millis();
  lastTxEndAt_ = end;
  nextTxAt_ = now + (id == SLOT_RAW ? rawGapMs_ : tuning.gapMs);  // debut + ecart
  log_[txCount_ % kLogN] = TxLog{s.pay, (uint8_t)id, r.v, r.irq1, r.rt2, r.status, r.us};
  txCount_++;
  onVerdict((uint8_t)id, r, end);
}

// La fin reelle de la derniere emission est deja notee par sendPacket().
void Halo1Lamp::endBurst() {
  phase_ = Phase::Idle;
  pendingSince_ = 0;
}

bool Halo1Lamp::txLog(uint32_t n, TxLog &out) const {
  if (n >= txCount_ || txCount_ - n > kLogN) return false;
  out = log_[n % kLogN];
  return true;
}

void Halo1Lamp::onVerdict(uint8_t id, const Halo1Radio::TxReport &r, uint32_t now) {
  Slot &s = slots_[id];
  s.attempts++;
  stats.packets++;
  bool foreign = false;
  switch (r.v) {
    case Verdict::Ack:
      s.acks++;
      stats.acks++;
      link_ = Halo1Link::Ok;
      lastAckAt_ = now;
      acked_ = true;
      break;
    case Verdict::AckForeign:  // jamais observe : non compte, transmis au suivi
      stats.ackForeign++;
      foreign = r.fLen == 2;
      break;
    case Verdict::MaxRt: stats.maxRt++; break;
    case Verdict::Timeout: stats.timeouts++; break;
    case Verdict::FifoRefused: stats.fifoRefused++; break;
  }
  // TX_DS (avec ou sans trame etrangere) : puce saine et lampe jointe. MAX_RT :
  // puce saine, pas d'accuse (lampe debranchee) ; seuls les delais s'enchainent.
  watch_.txVerdict(r.v == Verdict::Ack || r.v == Verdict::AckForeign ? TxSeen::Ack
                   : r.v == Verdict::MaxRt                           ? TxSeen::MaxRt
                   : r.v == Verdict::Timeout                         ? TxSeen::Timeout
                                                                     : TxSeen::Refused);
  trace("[lampe] TX %02X %02X #%u/%u %s %u us RT2 %02X", s.pay.flags, s.pay.value, s.attempts, s.repeats,
        verdictText(r.v), r.us, r.rt2);
  const uint8_t need = s.repeats < tuning.minAcks ? s.repeats : tuning.minAcks;
  const uint8_t most = tuning.maxAttempts > s.repeats ? tuning.maxAttempts : s.repeats;
  bool done;
  if (id == SLOT_RAW) done = s.attempts >= s.repeats;  // exactement n paquets
  else done = (s.attempts >= s.repeats && s.acks >= need) || s.attempts >= most;
  if (done) complete(id, now);
  // Apres la tranche : la trame etrangere peut changer la consigne et replanifier.
  if (foreign) {
    AirFrame f{};
    f.crcOk = true;
    f.len = 2;
    f.pay[0] = r.fPay[0];
    f.pay[1] = r.fPay[1];
    onAir(f, now);
  }
}

void Halo1Lamp::complete(uint8_t id, uint32_t now) {
  Slot &s = slots_[id];
  const Payload p = s.pay;
  if (id == SLOT_RAW) {
    // Banc : exactement n paquets, jamais repris. Un accuse suffit a croire la
    // trame, appliquee comme une trame de la telecommande.
    s.active = false;
    trace("[lampe] brut %02X %02X : %u/%u accuses", p.flags, p.value, s.acks, s.attempts);
    if (s.acks) {  // replanification comprise (D.4), contrairement a 'lampe croire'
      if (kindOf(p) == Kind::Auto) noteAuto(p.value, now);
      else onRemotePayload(p, now);
    }
    return;
  }
  const uint8_t need = s.repeats < tuning.minAcks ? s.repeats : tuning.minAcks;
  if (s.acks < need) {
    fail(id, now);
    return;
  }
  s.active = false;
  failures_ = 0;
  trace("[lampe] %s %02X %02X livree : %u/%u accuses", kSlotText[id], p.flags, p.value, s.acks, s.attempts);
  if (id == SLOT_AUTO) {
    stats.autoSent++;
    if (lastAuto_ != p.value) {
      lastAuto_ = p.value;
      schedulePersist(now);
    }
    confirmed_ &= (uint8_t)~FLD_BRIGHT;  // le mode auto fait deriver la luminosite
  } else {
    applyBelieved(p, now, true);
    dirty_ &= (uint8_t)~coveredBy(p, target_);
  }
  replan(now);
  if (!anyActive()) delivered_++;  // derniere trame de la consigne : LED d'etat
}

// ---------------------------------------------------------------------------
//  Echecs (D.5)
// ---------------------------------------------------------------------------

void Halo1Lamp::fail(uint8_t id, uint32_t now) {
  Slot &s = slots_[id];
  if (s.acks) {
    // Meilleure estimation : un accuse ne prouve pas que la trame a ete
    // appliquee, mais la lampe l'a recue au moins une fois.
    stats.weakFails++;
    if (id == SLOT_AUTO) {
      if (lastAuto_ != s.pay.value) {
        lastAuto_ = s.pay.value;
        schedulePersist(now);
      }
    } else {
      applyBelieved(s.pay, now, false);
    }
  }
  failures_++;
  if (failures_ > tuning.planRetries) {
    giveUp();
    return;
  }
  // Tranche rearmee (meme charge, meme numero A), reprise apres 1 s puis 2 s ;
  // on ecoute pendant l'attente.
  s.attempts = s.acks = 0;
  phase_ = Phase::Backoff;
  retryAt_ = now + (uint32_t)tuning.retryMs * failures_;
  stats.retries++;
  trace("[lampe] %s %02X %02X : echec %u, reprise dans %lu ms", kSlotText[id], s.pay.flags, s.pay.value,
        failures_, (unsigned long)((uint32_t)tuning.retryMs * failures_));
}

void Halo1Lamp::giveUp() {
  // Matter revient a l'etat cru : mieux vaut un etat juste qu'une consigne
  // que la lampe n'a jamais recue.
  for (Slot &s : slots_) s.active = false;
  if (target_ != believed_) version_++;
  target_ = believed_;
  dirty_ = 0;
  failures_ = 0;
  link_ = Halo1Link::Lost;
  stats.giveUps++;
  giveUps_++;
  phase_ = Phase::Idle;
  pendingSince_ = 0;
  notice("[lampe] injoignable : consigne abandonnee");
}

// ---------------------------------------------------------------------------
//  Suivi de la telecommande (D.6)
// ---------------------------------------------------------------------------

void Halo1Lamp::onAir(const AirFrame &f, uint32_t now) {
  stats.rxFrames++;
  const Payload p{f.pay[0], f.pay[1]};
  char st[48];
  switch (classify(f)) {
    case Kind::CrcBad:
      stats.rxCrcBad++;
      break;
    case Kind::LampAck:
      // La lampe repond (a la telecommande) : une reprise en attente part tout de suite.
      stats.rxLampAcks++;
      if (phase_ == Phase::Backoff) retryAt_ = now;
      if (link_ == Halo1Link::Lost) link_ = Halo1Link::Unknown;
      trace("[lampe] RX accuse lampe PID %u", f.pid);
      break;
    case Kind::Service:  // FF/FE/FD 00 (reveil), FA xx (annonce)
      stats.rxService++;
      remoteAt_ = now;
      trace("[lampe] RX tele PID %u %02X %02X : service", f.pid, p.flags, p.value);
      break;
    case Kind::Reserved:  // 91 xx, 89 xx : favori
      stats.rxReserved++;
      remoteAt_ = now;
      remoteAuto_.reset();  // autre commande : le numero A repart a 01
      trace("[lampe] RX tele PID %u %02X %02X : favori", f.pid, p.flags, p.value);
      break;
    case Kind::Invalid:
      stats.rxInvalid++;
      trace("[lampe] RX PID %u len %u %02X %02X : invalide", f.pid, f.len, p.flags, p.value);
      break;
    case Kind::Auto: {
      // Aucun changement de marche ni de lampes : le sens des bits d'une trame
      // A est inconnu ('60 01' observe).
      stats.rxAuto++;
      remoteAt_ = now;
      // 3 copies par appui : comptees une fois (AutoPressFilter).
      const bool press = remoteAuto_.feed(p.value, now);
      if (press) remoteAutoPresses_++;  // reflete dans Matter par le pont (EP4, s'il est expose)
      noteAuto(p.value, now);
      trace("[lampe] RX tele PID %u %02X %02X -> A numero %u%s", f.pid, p.flags, p.value, p.value,
            press ? "" : " (copie)");
      break;
    }
    default:  // Temp, Bright
      stats.rxState++;
      remoteAt_ = now;
      remoteAuto_.reset();  // autre commande : le numero A repart a 01
      onRemotePayload(p, now);
      describe(believed_, st, sizeof(st));
      trace("[lampe] RX tele PID %u %02X %02X -> %s", f.pid, p.flags, p.value, st);
      break;
  }
}

void Halo1Lamp::onRemotePayload(Payload p, uint32_t now, bool arm) {
  const Kind k = kindOf(p);
  if (k != Kind::Temp && k != Kind::Bright) return;
  // Nos paquets deja accuses (rafale en cours, trame recue a la place d'un
  // accuse) sont anterieurs a cette trame : ils passent d'abord dans l'etat
  // cru, et replan() ne les y remet pas apres elle.
  for (uint8_t id : {(uint8_t)SLOT_BRIGHT, (uint8_t)SLOT_TEMP})
    if (slots_[id].active && slots_[id].acks) applyBelieved(slots_[id].pay, now, false);
  if (applyState(believed_, p)) schedulePersist(now);
  if (applyState(target_, p)) version_++;  // bornes appliquees par applyState
  // La telecommande gagne champ par champ : un reglage en attente sur un autre
  // champ part avec les nouveaux drapeaux.
  const uint8_t f = (uint8_t)(FLD_FLAGS | (k == Kind::Bright ? FLD_BRIGHT : FLD_TEMP));
  dirty_ &= (uint8_t)~f;
  confirmed_ |= f;
  replan(now, false, arm);
}

void Halo1Lamp::noteAuto(uint8_t value, uint32_t now) {
  uint8_t last = value;
  Slot &a = slots_[SLOT_AUTO];
  // La telecommande vient d'utiliser notre numero : la lampe ecarterait notre
  // appui comme un doublon, on en prend un autre.
  if (a.active && !a.acks && a.pay.value == value) {
    last = nextAuto(value);
    a.pay.value = last;
  }
  if (last != lastAuto_) {
    lastAuto_ = last;
    schedulePersist(now);
  }
  confirmed_ &= (uint8_t)~FLD_BRIGHT;  // le mode auto fait deriver la luminosite
}

void Halo1Lamp::applyBelieved(Payload p, uint32_t now, bool confirm) {
  const Kind k = kindOf(p);
  if (k != Kind::Temp && k != Kind::Bright) return;
  if (applyState(believed_, p)) schedulePersist(now);
  if (confirm) confirmed_ |= (uint8_t)(FLD_FLAGS | (k == Kind::Bright ? FLD_BRIGHT : FLD_TEMP));
}

// ---------------------------------------------------------------------------
//  NVS (D.10) : "halo1/addr" (4 octets, ordre registre) et "halo1/etat" :
//  {ver=1, power<<7 | lamps, bright, temp, lastAuto, 0, 0, crc8(7 premiers)}
// ---------------------------------------------------------------------------

static bool blobValid(const uint8_t b[8]) {
  if (b[0] != kBlobVersion || crc8(b, 7) != b[7] || b[5] || b[6]) return false;
  const uint8_t lamps = b[1] & (uint8_t)~F_POWER;
  if (!lamps || (lamps & (uint8_t)~F_LAMPS)) return false;
  return b[2] >= kBrightMin && b[2] <= kBrightMax && b[3] <= kTempMax;
}

void Halo1Lamp::buildBlob(uint8_t b[8]) const {
  const uint8_t lamps = (believed_.lamps & F_LAMPS) ? (uint8_t)(believed_.lamps & F_LAMPS) : F_LAMPS;
  b[0] = kBlobVersion;
  b[1] = (uint8_t)((believed_.power ? F_POWER : 0) | lamps);
  b[2] = clampBright(believed_.bright);
  b[3] = clampTemp(believed_.temp);
  b[4] = lastAuto_;
  b[5] = b[6] = 0;
  b[7] = crc8(b, 7);
}

void Halo1Lamp::loadNvs() {
  memcpy(addrReg_, kDefaultAddrReg, 4);
  target_ = believed_ = State();
  lastAuto_ = 0;
  memset(saved_, 0, sizeof(saved_));
  Preferences p;
  // Ouverture en ecriture meme pour lire : en lecture seule, un espace de noms
  // absent fait loguer une erreur NVS au premier demarrage.
  if (!p.begin(kNvsNs, false)) return;
  uint8_t a[4];
  // isKey() d'abord : interroger une cle absente logue une erreur NVS.
  if (p.isKey("addr") && p.getBytes("addr", a, 4) == 4 && addressAllowed(a)) memcpy(addrReg_, a, 4);
  uint8_t b[8];
  if (p.isKey("etat") && p.getBytes("etat", b, 8) == 8 && blobValid(b)) {
    State s;
    s.power = (b[1] & F_POWER) != 0;
    s.lamps = b[1] & F_LAMPS;
    s.bright = b[2];
    s.temp = b[3];
    target_ = believed_ = s;
    lastAuto_ = b[4];
    memcpy(saved_, b, sizeof(saved_));
  }
  p.end();
}

void Halo1Lamp::schedulePersist(uint32_t now) {
  if (!persistDirty_) {
    persistDirty_ = true;
    persistFirst_ = now;
  }
  persistDue_ = now + HALO1_PERSIST_DELAY_MS;
}

// Au repos seulement, jamais juste apres une emission : l'ecriture flash peut
// prendre quelques dizaines de ms.
void Halo1Lamp::maybePersist(uint32_t now) {
  if (!persistDirty_ || busy() || phase_ == Phase::Burst) return;
  // Differences signees : une date posterieure a 'now' compte comme recente.
  if ((int32_t)(now - lastTxEndAt_) < (int32_t)kPersistAfterTxMs) return;
  if ((int32_t)(now - persistDue_) < 0 && (int32_t)(now - persistFirst_) < (int32_t)HALO1_PERSIST_MAX_MS) return;
  saveState();
}

void Halo1Lamp::saveState() {
  uint8_t b[8];
  buildBlob(b);
  persistDirty_ = false;
  if (!memcmp(b, saved_, sizeof(saved_))) return;  // rien de neuf : pas d'usure
  Preferences p;
  if (!p.begin(kNvsNs, false)) return;
  const bool ok = p.putBytes("etat", b, sizeof(b)) == sizeof(b);
  p.end();
  if (!ok) return;
  memcpy(saved_, b, sizeof(saved_));
  stats.persisted++;
}

void Halo1Lamp::persistNow() { saveState(); }

void Halo1Lamp::forget() {
  Preferences p;
  if (p.begin(kNvsNs, false)) {
    if (p.isKey("etat")) p.remove("etat");
    p.end();
  }
  for (Slot &s : slots_) s.active = false;
  if (phase_ == Phase::Backoff) phase_ = Phase::Idle;
  target_ = believed_ = State();
  dirty_ = confirmed_ = 0;
  lastAuto_ = 0;
  failures_ = 0;
  pendingSince_ = 0;
  persistDirty_ = false;
  memset(saved_, 0, sizeof(saved_));
  selMem_.reset(target_.lamps);
  version_++;
}

bool Halo1Lamp::setAddress(const uint8_t addrReg[4]) {
  if (!addressAllowed(addrReg)) return false;
  Preferences p;
  if (!p.begin(kNvsNs, false)) return false;
  const bool ok = p.putBytes("addr", addrReg, 4) == 4;
  p.end();
  if (!ok) return false;
  memcpy(addrReg_, addrReg, 4);
  settleRadio();  // pas de reset coupe : la puce reste lisible
  radio.setAddress(addrReg_);  // + reconfiguration complete au prochain usage
  return true;
}

// ---------------------------------------------------------------------------
//  Affichage
// ---------------------------------------------------------------------------

void Halo1Lamp::printStatus(Print &out) const {
  char a[48], b[48], fa[32], fb[32];
  out.println();
  out.println("=== Lampe Halo 1 (pilote) ===");
  if (lost())
    out.println(stuck_ ? "  BM5602 PERDU : configuration rejetee meme apres relance ('lampe regs'), "
                         "nouvel essai toutes les 60 s"
                       : "  BM5602 PERDU : nouvel essai de relance toutes les 60 s");
  else if (!radio.present())
    out.println("  BM5602 absent : pilote inactif ('rfinit', puis 'lampe')");
  else if (watch_.failed())
    out.printf("  BM5602 EN PANNE : %u relances automatiques de suite sans guerison ; prochain essai dans %lu s si le "
               "symptome dure, puis toutes les %lu min ('lampe stats')\n",
               (unsigned)watch_.unrecovered(), (unsigned long)(watch_.waitMs(millis()) / 1000),
               (unsigned long)(ChipWatch::kBackoffMs / 60000));
  const uint8_t *r = addrReg_, *air = radio.air();
  out.printf("  adresse     : %02X %02X %02X %02X (sur l'air %02X %02X %02X %02X), canal %u, 125 kbps\n", r[0],
             r[1], r[2], r[3], air[0], air[1], air[2], air[3], (unsigned)kChannel);
  out.printf("  ecoute      : %s, trace %s\n", listening_ ? "oui" : "non", trace_ ? "oui" : "non");
  describe(target_, a, sizeof(a));
  describeFields(dirty_, fa, sizeof(fa));
  out.printf("  consigne    : %s (a livrer : %s)\n", a, fa);
  describe(believed_, b, sizeof(b));
  describeFields(confirmed_, fb, sizeof(fb));
  out.printf("  etat cru    : %s (confirme : %s)\n", b, fb);
  out.print("  tranches    :");
  bool any = false;
  for (uint8_t i = 0; i < SLOT_N; i++) {
    const Slot &s = slots_[i];
    if (!s.active) continue;
    out.printf(" %s %02X %02X %u/%u accuses (%u paquets) ;", kSlotText[i], s.pay.flags, s.pay.value, s.acks,
               s.attempts, s.repeats);
    any = true;
  }
  out.println(any ? "" : " aucune");
  const uint32_t now = millis();
  if (phase_ == Phase::Backoff)
    out.printf("  phase       : reprise dans %ld ms (echec %u sur %u)\n", (long)(int32_t)(retryAt_ - now),
               failures_, (unsigned)tuning.planRetries + 1);
  else
    out.printf("  phase       : %s\n", phase_ == Phase::Burst ? "rafale" : "repos");
  const char *link = link_ == Halo1Link::Ok ? "ok" : link_ == Halo1Link::Lost ? "PERDU (consigne abandonnee)" : "inconnu";
  if (acked_)
    out.printf("  lien        : %s, dernier accuse il y a %lu ms\n", link, (unsigned long)(now - lastAckAt_));
  else
    out.printf("  lien        : %s, aucun accuse depuis le demarrage\n", link);
  static const char *const kModes[] = {"inconnu", "reset", "emission", "ecoute", "veille"};
  const Halo1Radio::Stats &rs = radio.stats;
  out.printf("  radio       : %s, %lu config. (%lu silence, %lu apres echec TX, %lu verif. ratees), %lu rearm.\n",
             kModes[(uint8_t)radio.mode()], (unsigned long)rs.fullConfigs, (unsigned long)rs.silenceReconf,
             (unsigned long)rs.txReconf, (unsigned long)rs.verifyFail, (unsigned long)rs.rearms);
  out.printf("  surveil.    : %u delai(s) de suite (relance a %u), fenetre d'ecoute %u trames dont %u CRC faux "
             "(deluge : %u dont %u %%), %lu relance(s) auto\n",
             (unsigned)watch_.timeoutRun(), (unsigned)ChipWatch::kTimeoutRun, (unsigned)watch_.windowFrames(),
             (unsigned)watch_.windowBad(), (unsigned)ChipWatch::kNoiseMinFrames, (unsigned)ChipWatch::kNoiseBadPct,
             (unsigned long)watch_.total());
  out.printf("  dernier A   : %u (%lu appuis entendus de la telecommande), memoire des lampes : %s\n", lastAuto_,
             (unsigned long)remoteAutoPresses_, lampsText(selMem_.memory(target_)));
  out.printf("  sauvegarde  : %s\n", persistDirty_ ? "en attente" : "a jour");
  out.printf("  reglages    : %u paquets (%u accuses, %u au plus), ecart %u ms, reprise %u ms x %u, gamma %.2f\n",
             tuning.repeats, tuning.minAcks, tuning.maxAttempts, tuning.gapMs, tuning.retryMs, tuning.planRetries,
             (double)mapGamma());
}

void Halo1Lamp::printStats(Print &out) const {
  const Stats &s = stats;
  const Halo1Radio::Stats &r = radio.stats;
  out.printf("  emission : %lu consignes, %lu paquets, %lu accuses, %lu ACK+TRAME, %lu MAX_RT, %lu delais, "
             "%lu FIFO refusees\n",
             (unsigned long)s.requests, (unsigned long)s.packets, (unsigned long)s.acks, (unsigned long)s.ackForeign,
             (unsigned long)s.maxRt, (unsigned long)s.timeouts, (unsigned long)s.fifoRefused);
  out.printf("  tranches : %lu faibles, %lu preemptees, %lu annulees, %lu reprises, %lu abandons, "
             "%lu attentes (telecommande)\n",
             (unsigned long)s.weakFails, (unsigned long)s.preempted, (unsigned long)s.cancelled,
             (unsigned long)s.retries, (unsigned long)s.giveUps, (unsigned long)s.holdoffs);
  out.printf("  bouton A : %lu livres, %lu refuses (lampe eteinte)\n", (unsigned long)s.autoSent,
             (unsigned long)s.autoIgnoredOff);
  out.printf("  ecoute   : %lu trames : %lu d'etat, %lu A, %lu accuses lampe, %lu service, %lu favori, "
             "%lu invalides, %lu CRC faux\n",
             (unsigned long)s.rxFrames, (unsigned long)s.rxState, (unsigned long)s.rxAuto,
             (unsigned long)s.rxLampAcks, (unsigned long)s.rxService, (unsigned long)s.rxReserved,
             (unsigned long)s.rxInvalid, (unsigned long)s.rxCrcBad);
  out.printf("  radio    : %lu config., %lu reconf. silence, %lu reconf. TX, %lu verif. ratees, %lu rearm., "
             "%lu brutes, %lu bascules legeres\n",
             (unsigned long)r.fullConfigs, (unsigned long)r.silenceReconf, (unsigned long)r.txReconf,
             (unsigned long)r.verifyFail, (unsigned long)r.rearms, (unsigned long)r.rxRaw,
             (unsigned long)r.lightSwitches);
  if (radio.hasAirGuard())
    out.printf("  garde    : %s, %lu paquets gardes, %lu refus du verrou, %lu attentes d'emission Thread "
               "(%lu plafonnees, max %lu us)\n",
               radio.tuning.airGuard ? "oui" : "COUPEE", (unsigned long)r.guarded, (unsigned long)r.guardRefused,
               (unsigned long)r.guardWaits, (unsigned long)r.guardCapped, (unsigned long)r.guardMaxUs);
  out.printf("  divers   : %lu sauvegardes, %lu traces perdues, %lu relances du module\n", (unsigned long)s.persisted,
             (unsigned long)s.traceDropped, (unsigned long)s.restarts);
  // Relances automatiques (L2) par cause, et les dernieres, datees.
  const ChipWatch &w = watch_;
  out.printf("  relances : %lu auto (%lu verif., %lu delais, %lu bruit), %u de suite sans guerison%s",
             (unsigned long)w.total(), (unsigned long)w.count(Relaunch::Verify),
             (unsigned long)w.count(Relaunch::TxTimeout), (unsigned long)w.count(Relaunch::RxNoise),
             (unsigned)w.unrecovered(), w.failed() ? " : EN PANNE" : "");
  ChipWatch::Entry h[ChipWatch::kHistN];
  const uint8_t n = w.history(h, ChipWatch::kHistN);
  const uint32_t now = millis();
  for (uint8_t i = 0; i < n; i++)
    out.printf("%s%s il y a %lu s", i ? ", " : " ; dernieres : ", relaunchText(h[i].cause),
               (unsigned long)((now - h[i].atMs) / 1000));
  out.println();
  const uint32_t wait = w.waitMs(now);
  if (wait) out.printf("  relance  : prochaine permise dans %lu s\n", (unsigned long)(wait / 1000));
}

void Halo1Lamp::clearStats() {
  stats = Stats{};
  radio.stats = Halo1Radio::Stats{};
  watch_.clearCounts();  // compteurs et historique ; ni l'attente ni l'etat EN PANNE
  seenSilence_ = seenTxReconf_ = seenVerify_ = 0;
  relaunchConfigs_ = 0;  // relance en cours : toujours aucune configuration verifiee depuis
}
