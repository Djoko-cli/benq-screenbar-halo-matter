#include "json_mode.h"

#include <bootloader_random.h>
#include <esp_app_desc.h>
#include <esp_arduino_version.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "halo1_lamp.h"
#include "status_led.h"
#ifndef DIAG_ONLY
#include "matter_bridge.h"
#endif

using namespace jsonp;
using namespace halo1;

// ===========================================================================
//  Etat
// ===========================================================================

static constexpr uint32_t kLateMs = 500;         // ligne periodique perdue apres ce retard
static constexpr uint32_t kHbMs = 2000;          // battement quand les etat sont coupes ou lents
static constexpr uint32_t kPeriodDefault = 1000, kCountersDefault = 1000, kNetDefault = 5000;
static constexpr uint16_t kLeaseDefault = 30;
static constexpr uint8_t kReplies = 4;           // reponses differees (apres un instantane)

static Writer sW;              // le seul tampon de formatage (1024 octets)
static bool sBusy = false;     // une ligne en cours de formatage dans sW
static uint32_t sN = 0;        // n de la prochaine ligne produite (USB)
static uint32_t sLost = 0, sTooLong = 0, sRejected = 0;
static uint32_t sBoot = 0;

static struct {
  bool machine = false;
  uint32_t periodMs = kPeriodDefault, countersMs = kCountersDefault, netMs = kNetDefault;
  uint16_t leaseS = kLeaseDefault;
  bool frames = true, log = false;
  uint32_t nextEtat = 0, nextCpt = 0, nextNet = 0, nextHb = 0;
} sS;
static uint32_t sLastRx = 0, sLastCmd = 0;  // bail : dernier octet recu, fin de la derniere commande

static Queue sQ;
static struct PendingReply {
  bool used;
  uint32_t id, t0;
  bool lease;
  char cmd[kCmdTextMax + 1];
} sReplies[kReplies];

static RateCap sRxCap(50), sCrcCap(10), sTxCap(50), sLogCap(20);
static Cadence sCadence;
static uint32_t sConfigSig = 0;
static bool sRefreshSaved = false;             // 'json etat' : abonnements sauves relus
static uint32_t sLoopAt = 0, sLoopMaxMs = 0;   // plus long tour de loop() depuis le bloc sante precedent

// Observateur de livraison (section 7.3).
static struct {
  bool wasBusy, sawTarget;
  uint32_t since;
  uint32_t seenDelivered, seenGiveUps;
  uint32_t ids[kIdsMax];
  uint8_t nIds;
  uint32_t idsLost;
} sDel = {};

static uint32_t upS() { return (uint32_t)(esp_timer_get_time() / 1000000); }

// ===========================================================================
//  Emission
// ===========================================================================

// Reserve le tampon unique. false : une ligne est deja en cours (bogue :
// aucun formatage ne doit en appeler un autre), rien n'est produit.
static bool claim() {
  if (sBusy) return false;
  sBusy = true;
  return true;
}

// Ferme la ligne et l'ecrit d'un seul Serial.write, ou la perd sans attendre.
// n compte toute ligne produite, ecrite ou perdue.
static bool send() {
  sBusy = false;
  sN++;
  if (!sW.finish()) {
    sTooLong++;
    return false;
  }
  const size_t len = sW.size();
  if (Serial.availableForWrite() < (int)len) {
    sLost++;
    return false;
  }
  Serial.write(sW.data(), len);
  return true;
}

// Texte emis par le protocole lui-meme (fin de bail, invite) : meme chemin
// non bloquant, perdu et compte si le tampon est plein.
static void textNb(const char *s) {
  const size_t len = strlen(s);
  if (Serial.availableForWrite() < (int)len) {
    sLost++;
    return;
  }
  Serial.write((const uint8_t *)s, len);
}

// ===========================================================================
//  Textes
// ===========================================================================

static const char *resetCode(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "mise_sous_tension";
    case ESP_RST_EXT: return "broche";
    case ESP_RST_SW: return "logiciel";
    case ESP_RST_PANIC: return "panique";
    case ESP_RST_INT_WDT: return "chien_int";
    case ESP_RST_TASK_WDT: return "chien_tache";
    case ESP_RST_WDT: return "chien";
    case ESP_RST_BROWNOUT: return "baisse_tension";
    case ESP_RST_USB: return "usb";
    default: return "inconnue";
  }
}

static const char *phaseCode(Halo1Lamp::Phase p) {
  switch (p) {
    case Halo1Lamp::Phase::Burst: return "rafale";
    case Halo1Lamp::Phase::Backoff: return "reprise";
    default: return "repos";
  }
}

static const char *linkCode(Halo1Link l) {
  switch (l) {
    case Halo1Link::Ok: return "ok";
    case Halo1Link::Lost: return "perdu";
    default: return "inconnu";
  }
}

static const char *radioModeCode(Halo1Radio::Mode m) {
  switch (m) {
    case Halo1Radio::Mode::Resetting: return "reset";
    case Halo1Radio::Mode::Tx: return "emission";
    case Halo1Radio::Mode::Rx: return "ecoute";
    case Halo1Radio::Mode::Sleep: return "veille";
    default: return "inconnu";
  }
}

// MAC-48 d'usine (tampon de 8 : voir applyIdentity, matter_bridge.cpp).
static bool factoryMac(uint8_t mac[8]) {
  memset(mac, 0, 8);
  return esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK;
}

// ===========================================================================
//  Messages de session et instantanes
// ===========================================================================

static void bootUp() {
  sW.hexU32("boot", sBoot, 8);
  sW.u32("up_s", upS());
}

static void helloBase(uint32_t now) {
  const esp_app_desc_t *d = esp_app_get_description();
  sW.begin("hello", sN, now);
  sW.str("bloc", "base");
  sW.u32("rev", kRev);
  sW.str("fw", FW_VERSION_FULL);
  sW.str("fw_desc", d->version, sizeof(d->version));
  sW.str("date", d->date, sizeof(d->date));
  sW.str("heure", d->time, sizeof(d->time));
  sW.str("env", FW_ENV);
#ifdef DIAG_ONLY
  sW.str("build", "diag");
  sW.str("reseau_build", "aucun");
#else
  sW.str("build", "produit");
#if MATTER_NET_THREAD
  sW.str("reseau_build", "thread");
#else
  sW.str("reseau_build", "wifi");
#endif
#endif
  sW.str("puce", CONFIG_IDF_TARGET);
  sW.str("idf", esp_get_idf_version());
  sW.str("arduino", ESP_ARDUINO_VERSION_STR);
  sW.hexU32("boot", sBoot, 8);
  const esp_reset_reason_t r = esp_reset_reason();
  sW.str("reset", resetCode(r));
  sW.u32("reset_n", (uint32_t)r);
  sW.u32("up_s", upS());
  sW.obj("session");
  sW.str("transport", "usb");
  sW.u32("periode_ms", sS.periodMs);
  sW.u32("compteurs_ms", sS.countersMs);
  sW.u32("reseau_ms", sS.netMs);
  sW.u32("bail_s", sS.leaseS);
  sW.boolean("trames", sS.frames);
  sW.boolean("log", sS.log);
  sW.end();
  sW.obj("limites");
  sW.u32("ligne_max", kLineMax);
  sW.u32("cmd_max", kCmdMax);
  sW.end();
}

static void helloIdentity(uint32_t now) {
  sW.begin("hello", sN, now);
  sW.str("bloc", "identite");
  sW.hexU32("boot", sBoot, 8);
  uint8_t mac[8];
  const bool macOk = factoryMac(mac);
  if (macOk) sW.hex("mac", mac, 6);
  else sW.null("mac");
  sW.obj("id");
  sW.str("fabricant", MATTER_VENDOR_NAME, 32);
  sW.str("produit", MATTER_PRODUCT_NAME, 32);
  if (macOk) {
    // Meme calcul que le pont (applyIdentity), build diag compris.
    char serial[sizeof(MATTER_SERIAL_PREFIX) + 12];
    snprintf(serial, sizeof(serial), "%s%02X%02X%02X%02X%02X%02X", MATTER_SERIAL_PREFIX, mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    sW.str("serie", serial, 32);
  } else {
    sW.null("serie");
  }
  sW.str("nom", MATTER_NODE_LABEL, 32);
  sW.u32("hw", MATTER_HW_VERSION);
  sW.str("hw_txt", MATTER_HW_VERSION_STRING, 64);
  sW.end();
  sW.arr("caps");
#ifndef DIAG_ONLY
  sW.str(nullptr, "matter");
#if MATTER_NET_THREAD
  sW.str(nullptr, "thread");
#endif
#endif
  if (lamp.radio.hasAirGuard()) sW.str(nullptr, "garde");
#if HALO1_EXPOSE_AUTO && !defined(DIAG_ONLY)
  sW.str(nullptr, "ep4");
#endif
#ifndef DIAG_ONLY
  sW.str(nullptr, "led");
#endif
  sW.str(nullptr, "lampe_async");
  sW.str(nullptr, "trames");
  sW.str(nullptr, "log");
  sW.end();
}

static uint32_t gammaC() { return (uint32_t)(mapGamma() * 100.0f + 0.5f); }

static void config(uint32_t now) {
  sW.begin("config", sN, now);
  sW.obj("lampe");
  sW.hex("adresse", lamp.address(), 4);
  sW.hex("air", lamp.radio.air(), 4);
  sW.u32("canal", kChannel);
  sW.u32("debit_kbps", 125);  // bc5602::DATARATE_125K, le seul que la lampe connaisse
  sW.end();
  const Halo1Lamp::Tuning &t = lamp.tuning;
  const Halo1Radio::Tuning &rt = lamp.radio.tuning;
  sW.obj("reglages");
  sW.u32("paquets", t.repeats);
  sW.u32("accuses_min", t.minAcks);
  sW.u32("paquets_max", t.maxAttempts);
  sW.u32("ecart_ms", t.gapMs);
  sW.u32("reprise_ms", t.retryMs);
  sW.u32("reprises", t.planRetries);
  sW.u32("rearm_ms", rt.rearmMs);
  sW.u32("silence_ms", rt.silenceMs);
  sW.boolean("rearm_fort", rt.strongRearm);
  sW.boolean("leger", rt.lightSwitch);
  if (lamp.radio.hasAirGuard()) sW.boolean("garde", rt.airGuard);
  else sW.null("garde");
  sW.u32("gamma_c", gammaC());
  sW.end();
  sW.obj("seuils");
  sW.u32("delais_suite", ChipWatch::kTimeoutRun);
  sW.u32("deluge_trames", ChipWatch::kNoiseMinFrames);
  sW.u32("deluge_pct", ChipWatch::kNoiseBadPct);
  sW.u32("fenetre_ms", ChipWatch::kNoiseWindowMs);
  sW.u32("sourd_hors_rx", ChipWatch::kDeafMinRearms);
  sW.u32("sans_guerison", ChipWatch::kFruitless);
  sW.u32("ecart_ms", ChipWatch::kGapMs);
  sW.u32("repli_ms", ChipWatch::kBackoffMs);
  sW.end();
#ifndef DIAG_ONLY
  matterJsonConfig(sW);
#else
  sW.null("matter");
#endif
}

// Empreinte des reglages lents : config renvoyee en mode machine quand elle
// change ('lampe rafale|ecart|rx|leger|garde|gamma|adresse', 'matter ...').
static uint32_t configSig() {
  uint32_t h = 2166136261u;  // FNV-1a
  auto mix = [&h](uint32_t v) {
    for (uint8_t i = 0; i < 4; i++) {
      h ^= (v >> (8 * i)) & 0xFF;
      h *= 16777619u;
    }
  };
  const uint8_t *a = lamp.address();
  mix((uint32_t)a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 | (uint32_t)a[3] << 24);
  const Halo1Lamp::Tuning &t = lamp.tuning;
  mix((uint32_t)t.repeats | (uint32_t)t.minAcks << 8 | (uint32_t)t.maxAttempts << 16 | (uint32_t)t.planRetries << 24);
  mix((uint32_t)t.gapMs | (uint32_t)t.retryMs << 16);
  const Halo1Radio::Tuning &rt = lamp.radio.tuning;
  mix((uint32_t)rt.rearmMs | (uint32_t)rt.silenceMs << 16);
  mix((uint32_t)rt.strongRearm | (uint32_t)rt.lightSwitch << 1 | (uint32_t)rt.airGuard << 2 |
      (uint32_t)lamp.radio.hasAirGuard() << 3);
  mix(gammaC());
#ifndef DIAG_ONLY
  mix(matterConfigSig());
#endif
  return h;
}

static void etatLampe(uint32_t now) {
  sW.begin("etat", sN, now);
  sW.str("bloc", "lampe");
  bootUp();
  state(sW, "consigne", lamp.target());
  state(sW, "cru", lamp.believed());
  fields(sW, "a_livrer", lamp.dirty());
  fields(sW, "confirme", lamp.confirmed());
  sW.u32("version", lamp.version());
  const Halo1Lamp::Phase ph = lamp.phase();
  sW.str("phase", phaseCode(ph));
  if (ph == Halo1Lamp::Phase::Backoff) sW.u32("reprise_ms", lamp.retryInMs(now));
  else sW.null("reprise_ms");
  sW.u32("echecs", lamp.failures());
  sW.str("lien", linkCode(lamp.link()));
  if (lamp.acked()) sW.u32("accuse_ms", now - lamp.lastAckAt());
  else sW.null("accuse_ms");
  sW.u32("dernier_a", lamp.lastAuto());
  sW.u32("a_entendus", lamp.remoteAutoCount());
  sW.str("memoire", lampsCode(lamp.memoryLamps()));
  sW.u32("livrees", lamp.deliveredCount());
  sW.u32("abandons", lamp.giveUpCount());
  sW.boolean("sauve_attente", lamp.persistPending());
  sW.boolean("ecoute", lamp.listening());
  sW.boolean("trace", lamp.tracing());
}

static void etatTranches(uint32_t now) {
  sW.begin("etat", sN, now);
  sW.str("bloc", "tranches");
  bootUp();
  sW.arr("tranches");
  for (uint8_t i = 0; i < Halo1Lamp::SLOT_N; i++) {
    const Halo1Lamp::Slot &s = lamp.slot(i);
    if (!s.active) continue;
    sW.obj(nullptr);
    sW.str("tranche", slotCode(i));
    const uint8_t pay[2] = {s.pay.flags, s.pay.value};
    sW.hex("charge", pay, 2);
    sW.u32("accuses", s.acks);
    sW.u32("essais", s.attempts);
    sW.u32("paquets", s.repeats);
    sW.end();
  }
  sW.end();
}

static void etatSante(uint32_t now) {
  sW.begin("etat", sN, now);
  sW.str("bloc", "sante");
  bootUp();
  const Halo1Radio &r = lamp.radio;
  sW.obj("radio");
  sW.boolean("presente", r.present());
  sW.boolean("perdue", lamp.lost());
  sW.str("mode", radioModeCode(r.mode()));
  sW.boolean("configuree", r.configured());
  const BC5602 *chip = r.chip();
  if (chip) {
    sW.boolean("quartz", chip->crystalReady());
    sW.boolean("calib", chip->calibrated());
  } else {
    sW.null("quartz");
    sW.null("calib");
  }
  sW.end();
  const ChipWatch &w = lamp.watch();
  sW.obj("surveil");
  sW.boolean("panne", w.failed());
  sW.boolean("defaut", lamp.moduleFault());
  sW.str("symptome", symptomCode(w.symptom()));
  sW.u32("delais_suite", w.timeoutRun());
  sW.u32("fen_trames", w.windowFrames());
  sW.u32("fen_crc_faux", w.windowBad());
  sW.u32("hors_rx_10s", w.deafRearms());
  sW.u32("sans_guerison", w.unrecovered());
  sW.u32("attente_ms", w.waitMs(now));
  sW.u32("relances", w.total());
  ChipWatch::Entry last;
  if (w.history(&last, 1)) {
    sW.obj("derniere");
    sW.str("cause", relaunchCode(last.cause));
    sW.u32("il_y_a_s", (now - last.atMs) / 1000);
    sW.end();
  } else {
    sW.null("derniere");
  }
  sW.end();
  sW.obj("led");
  statusled::Pattern p;
  bool testing = false;
  if (statusLedState(&p, &testing)) {
    sW.str("motif", statusled::patternCode(p));
    sW.boolean("test", testing);
  } else {
    sW.null("motif");
    sW.null("test");
  }
  sW.end();
#ifndef DIAG_ONLY
  sW.obj("matter");
  sW.boolean("en_service", matterIsCommissioned());
  sW.boolean("connecte", matterIsConnected());
  sW.boolean("identify", matterIdentifying());
  sW.end();
#else
  sW.null("matter");
#endif
  sW.obj("sys");
  sW.u32("heap", esp_get_free_heap_size());
  sW.u32("heap_min", esp_get_minimum_free_heap_size());
  sW.u32("heap_bloc", (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  sW.u32("pile_boucle", (uint32_t)uxTaskGetStackHighWaterMark(nullptr));  // octets sous ESP-IDF
  sW.u32("boucle_max_ms", sLoopMaxMs);
  sLoopMaxMs = 0;
  sW.u32("json_perdus", sLost);
  sW.u32("json_trop_longs", sTooLong);
  sW.u32("rejets", sRejected);
  sW.end();
}

static void cptPilote(uint32_t now) {
  const Halo1Lamp::Stats &s = lamp.stats;
  sW.begin("compteurs", sN, now);
  sW.str("bloc", "pilote");
  sW.u32("raz", lamp.statsResets());
  sW.obj("tx");
  sW.u32("consignes", s.requests);
  sW.u32("paquets", s.packets);
  sW.u32("accuses", s.acks);
  sW.u32("ack_trame", s.ackForeign);
  sW.u32("max_rt", s.maxRt);
  sW.u32("delais", s.timeouts);
  sW.u32("fifo", s.fifoRefused);
  sW.u32("total", lamp.txCount());
  sW.end();
  sW.obj("tranches");
  sW.u32("faibles", s.weakFails);
  sW.u32("preemptees", s.preempted);
  sW.u32("annulees", s.cancelled);
  sW.u32("reprises", s.retries);
  sW.u32("abandons", s.giveUps);
  sW.u32("attentes", s.holdoffs);
  sW.end();
  sW.obj("a");
  sW.u32("livres", s.autoSent);
  sW.u32("refuses", s.autoIgnoredOff);
  sW.end();
  sW.obj("rx");
  sW.u32("trames", s.rxFrames);
  sW.u32("etat", s.rxState);
  sW.u32("a", s.rxAuto);
  sW.u32("accuses_lampe", s.rxLampAcks);
  sW.u32("service", s.rxService);
  sW.u32("favori", s.rxReserved);
  sW.u32("invalides", s.rxInvalid);
  sW.u32("crc_faux", s.rxCrcBad);
  sW.end();
  sW.obj("divers");
  sW.u32("sauvegardes", s.persisted);
  sW.u32("traces_perdues", s.traceDropped);
  sW.u32("relances_module", s.restarts);
  sW.end();
}

static void cptRadio(uint32_t now) {
  const Halo1Radio::Stats &r = lamp.radio.stats;
  sW.begin("compteurs", sN, now);
  sW.str("bloc", "radio");
  sW.u32("raz", lamp.statsResets());
  sW.obj("radio");
  sW.u32("configs", r.fullConfigs);
  sW.u32("reconf_silence", r.silenceReconf);
  sW.u32("reconf_tx", r.txReconf);
  sW.u32("verif_ratees", r.verifyFail);
  sW.u32("rearm", r.rearms);
  sW.u32("rearm_hors_rx", r.rearmsOffRx);
  sW.u32("brutes", r.rxRaw);
  sW.u32("bascules", r.lightSwitches);
  sW.end();
  if (lamp.radio.hasAirGuard()) {
    sW.obj("garde");
    sW.boolean("active", lamp.radio.tuning.airGuard);
    sW.u32("gardes", r.guarded);
    sW.u32("refus", r.guardRefused);
    sW.u32("attentes", r.guardWaits);
    sW.u32("plafonnees", r.guardCapped);
    sW.u32("max_us", r.guardMaxUs);
    sW.end();
  } else {
    sW.null("garde");
  }
  const ChipWatch &w = lamp.watch();
  sW.obj("relances");
  sW.u32("total", w.total());
  sW.u32("verif", w.count(Relaunch::Verify));
  sW.u32("delais", w.count(Relaunch::TxTimeout));
  sW.u32("bruit", w.count(Relaunch::RxNoise));
  sW.u32("sourde", w.count(Relaunch::RxDeaf));
  sW.end();
}

// Une ligne de la file : formatee maintenant, avec les valeurs du moment.
static void produce(const Queued &q, uint32_t now) {
  if (!claim()) return;
  switch (q.item) {
    case Item::HelloBase: helloBase(now); break;
    case Item::HelloId: helloIdentity(now); break;
    case Item::Config:
      config(now);
      sConfigSig = configSig();
      break;
    case Item::EtatLampe: etatLampe(now); break;
    case Item::EtatTranches: etatTranches(now); break;
    case Item::EtatSante: etatSante(now); break;
    case Item::CptPilote: cptPilote(now); break;
    case Item::CptRadio: cptRadio(now); break;
#ifndef DIAG_ONLY
    case Item::CptMatter:
      sW.begin("compteurs", sN, now);
      sW.str("bloc", "matter");
      matterJsonCounters(sW);
      break;
    case Item::NetThread:
      sW.begin("reseau", sN, now);
      sW.str("bloc", "thread");
      matterJsonNetThread(sW, now);
      break;
#if MATTER_NET_THREAD
    case Item::NetSubs:
      sW.begin("reseau", sN, now);
      sW.str("bloc", "abonnements");
      matterJsonNetSubs(sW, now, sRefreshSaved);
      sRefreshSaved = false;
      break;
#endif
#endif
    case Item::Heartbeat: heartbeat(sW, sN, now, sBoot, upS(), sLost); break;
    case Item::Reply: {
      PendingReply &p = sReplies[q.arg < kReplies ? q.arg : 0];
      Reply r;
      r.id = p.id;
      r.cmd = p.cmd;
      r.durMs = now - p.t0;
      r.hasLease = p.lease;
      r.leaseS = sS.leaseS;
      r.upS = upS();
      reply(sW, sN, now, r);
      p.used = false;
      break;
    }
    default:  // bloc absent de ce build : rien de produit
      sBusy = false;
      return;
  }
  send();
}

// Ligne periodique perdue par retard : n consomme (trou visible), comptee.
static void dropLate(const Queued &q) {
  sN++;
  sLost++;
  if (q.item == Item::Reply && q.arg < kReplies) sReplies[q.arg].used = false;
}

static void drain(uint32_t now) {
  while (const Queued *q = sQ.front()) {
    if (now - q->at <= kLateMs) break;
    dropLate(*q);
    sQ.pop();
  }
  const Queued *q = sQ.front();
  if (!q || sBusy) return;
  // Une ligne fait 1024 octets au plus : avec 2048 libres, il en reste 1024
  // apres elle pour un evenement. Sinon, au tour suivant.
  if (Serial.availableForWrite() < (int)(2 * kLineMax)) return;
  const Queued item = *q;
  sQ.pop();
  produce(item, now);
}

static void push(Item item, uint32_t now, bool session) {
  if (!sQ.push(item, now, session)) {
    // File pleine : la ligne est perdue, comme une ligne en retard.
    sN++;
    sLost++;
  }
}

static void pushState(uint32_t now, bool session) {
  push(Item::EtatLampe, now, session);
  push(Item::EtatTranches, now, session);
  push(Item::EtatSante, now, session);
}

static void pushCounters(uint32_t now, bool session) {
  push(Item::CptPilote, now, session);
  push(Item::CptRadio, now, session);
#ifndef DIAG_ONLY
  push(Item::CptMatter, now, session);
#endif
}

static void pushNet(uint32_t now, bool session) {
#ifndef DIAG_ONLY
  push(Item::NetThread, now, session);
#if MATTER_NET_THREAD
  push(Item::NetSubs, now, session);
#endif
#else
  (void)now;
  (void)session;
#endif
}

static void pushHello(uint32_t now, bool session) {
  push(Item::HelloBase, now, session);
  push(Item::HelloId, now, session);
  push(Item::Config, now, session);
}

// ===========================================================================
//  Reponses
// ===========================================================================

void jsonReply(const Reply &r) {
  if (!claim()) return;
  reply(sW, sN, millis(), r);
  send();
}

// Reponse apres les lignes deja en file (instantane) ; sans place : tout de suite.
static void replyAfterQueue(const JsonCmd &c, bool lease) {
  if (!c.hasId) return;
  for (uint8_t i = 0; i < kReplies; i++) {
    PendingReply &p = sReplies[i];
    if (p.used) continue;
    p.used = true;
    p.id = c.id;
    p.t0 = c.t0;
    p.lease = lease;
    copyCmd(p.cmd, c.cmd);
    if (sQ.push(Item::Reply, millis(), false, i)) return;
    p.used = false;
    break;
  }
  Reply r;
  r.id = c.id;
  r.cmd = c.cmd;
  r.durMs = millis() - c.t0;
  r.hasLease = lease;
  r.leaseS = sS.leaseS;
  r.upS = upS();
  jsonReply(r);
}

static void replyNow(const JsonCmd &c, bool ok, const char *code, const char *msg, bool lease = false) {
  if (!c.hasId) return;
  Reply r;
  r.id = c.id;
  r.cmd = c.cmd;
  r.ok = ok;
  r.code = code;
  r.msg = msg;
  r.durMs = millis() - c.t0;
  r.hasLease = lease;
  r.leaseS = sS.leaseS;
  r.upS = upS();
  jsonReply(r);
}

void jsonRefuse(const JsonCmd &c, const char *code, const char *msg) {
  sRejected++;
  if (c.hasId) replyNow(c, false, code, msg);
  else if (!sS.machine) Serial.printf("Ligne refusee (%s) : %s\n", code, msg);
}

// ===========================================================================
//  Session
// ===========================================================================

static void resetTimers(uint32_t now) {
  sS.nextEtat = now + sS.periodMs;
  sS.nextCpt = now + sS.countersMs;
  sS.nextNet = now + sS.netMs;
  sS.nextHb = now + kHbMs;
}

static void enterMachine(uint16_t leaseS, uint32_t now) {
  sS.machine = true;
  sS.periodMs = kPeriodDefault;
  sS.countersMs = kCountersDefault;
  sS.netMs = kNetDefault;
  sS.leaseS = leaseS;
  sS.frames = true;
  sS.log = false;
  sLastRx = sLastCmd = now;
  resetTimers(now);
}

// Fin du mode machine : lignes de session retirees, message fin, puis texte
// et invite par le chemin non bloquant.
static void leaveMachine(bool lease, uint32_t now) {
  sQ.dropSession();
  if (claim()) {
    sessionEnd(sW, sN, now, lease ? "bail" : "commande");
    send();
  }
  sS.machine = false;
  if (lease) {
    char t[80];
    snprintf(t, sizeof(t), "json : mode machine coupe (hote muet depuis %u s)\r\n> ", (unsigned)sS.leaseS);
    textNb(t);
  } else {
    textNb("> ");
  }
}

bool jsonMachine() { return sS.machine; }
void jsonNoteRx() { sLastRx = millis(); }
uint32_t jsonBootId() { return sBoot; }

bool jsonCadenceOk(uint32_t now) { return sCadence.allow(now); }

void jsonAfterCommand() {
  sLastCmd = millis();
  if (!sS.machine) return;
  const uint32_t sig = configSig();
  if (sig != sConfigSig) {
    sConfigSig = sig;  // une seule fois par changement, meme si la ligne se perd
    push(Item::Config, millis(), true);
  }
}

static void printSession(Print &out) {
  out.printf("Mode machine : %s", sS.machine ? "ACTIF" : "coupe ('json 1' pour l'activer)");
  if (sS.machine) {
    if (sS.leaseS) out.printf(", bail de %u s", sS.leaseS);
    else out.print(", sans bail (jusqu'a 'json 0')");
  }
  out.println();
  out.printf("  periodes : etat %lu ms, compteurs %lu ms, reseau %lu ms ; trames %s ; log %s\n",
             (unsigned long)sS.periodMs, (unsigned long)sS.countersMs, (unsigned long)sS.netMs,
             sS.frames ? "oui" : "non", sS.log ? "oui" : "non");
  out.printf("  lignes   : n = %lu, %lu perdue(s), %lu trop longue(s), %lu ligne(s) de l'hote refusee(s)\n",
             (unsigned long)sN, (unsigned long)sLost, (unsigned long)sTooLong, (unsigned long)sRejected);
  out.printf("  demarrage : boot %08lX\n", (unsigned long)sBoot);
  out.println("  protocole : docs/PROTOCOLE-JSON.md (v1)");
}

static char *nextWord(char *&p) {
  while (*p == ' ') p++;
  char *w = p;
  while (*p && *p != ' ') p++;
  if (*p) *p++ = 0;
  return w;
}

static bool parseU32(const char *s, uint32_t *v) {
  if (!*s) return false;
  char *end = nullptr;
  const unsigned long x = strtoul(s, &end, 10);
  if (!end || *end || *s == '-' || *s == '+') return false;
  *v = (uint32_t)x;
  return true;
}

// Periode : 0 (coupe) ou lo..60000 ms.
static bool setPeriod(char *p, uint32_t lo, uint32_t *out, uint32_t *next, uint32_t now) {
  uint32_t v;
  if (!parseU32(nextWord(p), &v) || *nextWord(p) || (v && (v < lo || v > 60000))) return false;
  *out = v;
  *next = now + v;
  return true;
}

void jsonCommand(char *arg, const JsonCmd &c) {
  char *p = arg;
  const char *sub = nextWord(p);
  const uint32_t now = millis();
  static const char *const kUsage =
      "json [1 [bail 0|10..600] | 0 | etat | hello | ping | periode ms | compteurs ms | reseau ms | "
      "trames 0|1 | log 0|1]";
  const char *usage = nullptr;  // non nul : arguments refuses
  bool sessionChanged = false;

  if (!*sub) {
    printSession(Serial);
    replyNow(c, true, "ok", nullptr);
    return;
  }
  if (!strcmp(sub, "1")) {
    uint32_t lease = kLeaseDefault;
    const char *w = nextWord(p);
    const bool bad = *w && (strcmp(w, "bail") || !parseU32(nextWord(p), &lease) || *nextWord(p) ||
                            (lease && (lease < 10 || lease > 600)));
    if (bad) {
      usage = "json 1 [bail 0|10..600]";
    } else {
      // Idempotent : renvoyer 'json 1' resynchronise (instantane complet).
      enterMachine((uint16_t)lease, now);
      sRefreshSaved = true;
      pushHello(now, true);
      pushState(now, true);
      pushCounters(now, true);
      pushNet(now, true);
      replyAfterQueue(c, true);
      return;
    }
  } else if (!strcmp(sub, "0")) {
    if (*nextWord(p)) {
      usage = "json 0";
    } else {
      replyNow(c, true, "ok", sS.machine ? nullptr : "deja en mode humain");
      if (sS.machine) leaveMachine(false, now);
      else if (!c.hasId) Serial.println("json : mode machine deja coupe");
      return;
    }
  } else if (!strcmp(sub, "etat")) {
    sRefreshSaved = true;
    pushState(now, false);
    pushCounters(now, false);
    pushNet(now, false);
    replyAfterQueue(c, false);
    return;
  } else if (!strcmp(sub, "hello")) {
    pushHello(now, false);
    replyAfterQueue(c, false);
    return;
  } else if (!strcmp(sub, "ping")) {
    // Le bail court deja depuis cette ligne (octets recus, fin de commande).
    replyNow(c, true, "ok", nullptr, true);
    if (!c.hasId) {
      if (sS.machine) Serial.printf("json : bail renouvele (%u s)\n", sS.leaseS);
      else Serial.println("json : pas de session machine ('json 1')");
    }
    return;
  } else if (!strcmp(sub, "periode")) {
    if (!setPeriod(p, 200, &sS.periodMs, &sS.nextEtat, now)) usage = "json periode 0|200..60000";
    sS.nextHb = now + kHbMs;
    sessionChanged = !usage;
  } else if (!strcmp(sub, "compteurs")) {
    if (!setPeriod(p, 200, &sS.countersMs, &sS.nextCpt, now)) usage = "json compteurs 0|200..60000";
    sessionChanged = !usage;
  } else if (!strcmp(sub, "reseau")) {
    if (!setPeriod(p, 1000, &sS.netMs, &sS.nextNet, now)) usage = "json reseau 0|1000..60000";
    sessionChanged = !usage;
  } else if (!strcmp(sub, "trames") || !strcmp(sub, "log")) {
    const char *w = nextWord(p);
    const bool on = !strcmp(w, "1");
    if ((!on && strcmp(w, "0")) || *nextWord(p)) {
      usage = !strcmp(sub, "trames") ? "json trames 0|1" : "json log 0|1";
    } else {
      (!strcmp(sub, "trames") ? sS.frames : sS.log) = on;
      sessionChanged = true;
    }
  } else if (!strcmp(sub, "cle")) {
    // Transport reseau (section 10) : pas dans ce firmware.
    replyNow(c, false, "refuse", "json cle : transport reseau absent de ce firmware");
    if (!c.hasId) Serial.println("json cle : transport reseau absent de ce firmware (USB seulement)");
    return;
  } else {
    usage = kUsage;
  }

  if (usage) {
    replyNow(c, false, "usage", usage);
    if (!c.hasId) Serial.printf("Usage : %s\n", usage);
    return;
  }
  // Reglage de session change : hello.base le porte.
  if (sessionChanged && sS.machine) push(Item::HelloBase, now, true);
  replyNow(c, true, "ok", nullptr);
  if (!c.hasId)
    Serial.printf("json : etat %lu ms, compteurs %lu ms, reseau %lu ms, trames %s, log %s\n",
                  (unsigned long)sS.periodMs, (unsigned long)sS.countersMs, (unsigned long)sS.netMs,
                  sS.frames ? "oui" : "non", sS.log ? "oui" : "non");
}

// ===========================================================================
//  Evenements
// ===========================================================================

Writer *jsonEventOpen(const char *type) {
  if (!sS.machine || !claim()) return nullptr;
  sW.begin(type, sN, millis());
  return &sW;
}

void jsonEventSend() {
  if (sBusy) send();
}

bool jsonLog(const char *src, const char *niv, const char *txt) {
  if (!sS.machine || !sS.log) return false;
  const uint32_t now = millis();
  if (!sLogCap.available(now)) {
    sLogCap.skip();
    return true;
  }
  if (!claim()) return false;  // ligne en cours (jamais attendu) : en texte
  sLogCap.take();
  logLine(sW, sN, now, src, niv, txt, sLogCap.takeSkipped());
  send();
  return true;
}

static void onLampRx(const RxEvent &e) {
  if (!sS.machine || !sS.frames) return;
  const uint32_t now = millis();
  const bool crc = e.kind == Kind::CrcBad;
  if (!sRxCap.available(now) || (crc && !sCrcCap.available(now))) {
    sRxCap.skip();
    return;
  }
  if (!claim()) return;
  sRxCap.take();
  if (crc) sCrcCap.take();
  rx(sW, sN, now, e, sRxCap.takeSkipped());
  send();
}

static void onLampTx(const TxEvent &e) {
  if (!sS.machine || !sS.frames) return;
  const uint32_t now = millis();
  if (!sTxCap.available(now)) {
    sTxCap.skip();
    return;
  }
  if (!claim()) return;
  sTxCap.take();
  tx(sW, sN, now, e, sTxCap.takeSkipped());
  send();
}

static void onLampRelaunch(const RelaunchEvent &e) {
  if (!sS.machine || !claim()) return;
  relaunch(sW, sN, millis(), e);
  send();
}

static void onLampModule(const ModuleEvent &e) {
  if (!sS.machine || !claim()) return;
  module(sW, sN, millis(), e);
  send();
}

static bool onLampLog(bool trace, const char *line) { return jsonLog("lampe", trace ? "trace" : "notice", line); }

static const LampHooks kLampHooks = {onLampRx, onLampTx, onLampRelaunch, onLampModule, onLampLog};

static void onLed(statusled::Pattern now, statusled::Pattern before, bool testing) {
  if (!sS.machine || !claim()) return;
  led(sW, sN, millis(), statusled::patternCode(now), statusled::patternCode(before), testing);
  send();
}

// ===========================================================================
//  Observateur de livraison (section 7.3)
// ===========================================================================

void jsonPendingId(uint32_t id) {
  if (sDel.nIds == kIdsMax) {
    memmove(sDel.ids, sDel.ids + 1, sizeof(sDel.ids[0]) * (kIdsMax - 1));
    sDel.nIds--;
    sDel.idsLost++;
  }
  sDel.ids[sDel.nIds++] = id;
}

// Front de busy() vrai -> faux, ou compteur de livraison ou d'abandon change
// (consigne commencee et finie dans une commande bloquante). abandon l'emporte
// sur livree ; ni l'un ni l'autre : annulee. Une periode occupee par la seule
// tranche brute du banc ne donne rien.
static void deliveryPoll(uint32_t now) {
  const bool busy = lamp.busy();
  const uint32_t delivered = lamp.deliveredCount(), giveUps = lamp.giveUpCount();
  const bool changed = delivered != sDel.seenDelivered || giveUps != sDel.seenGiveUps;
  if (changed || (!busy && sDel.wasBusy)) {
    if ((changed || sDel.sawTarget) && (sS.machine || sDel.nIds)) {
      Delivery d;
      d.issue = giveUps != sDel.seenGiveUps ? Issue::GaveUp : delivered != sDel.seenDelivered ? Issue::Delivered
                                                                                              : Issue::Cancelled;
      d.cause = lamp.lastGiveUp();
      d.last = lamp.lastDelivered();
      d.version = lamp.version();
      d.target = lamp.target();
      d.believed = lamp.believed();
      d.dirty = lamp.dirty();
      d.ids = sDel.ids;
      d.nIds = sDel.nIds;
      d.idsLost = sDel.idsLost;
      d.hasWait = sDel.wasBusy && sDel.since;  // pas vue : commande bloquante
      d.waitMs = now - sDel.since;
      d.delivered = delivered;
      d.giveUps = giveUps;
      if (claim()) {
        delivery(sW, sN, now, d);
        send();
      }
      sDel.nIds = 0;
      sDel.idsLost = 0;
    }
    sDel.seenDelivered = delivered;
    sDel.seenGiveUps = giveUps;
    sDel.wasBusy = sDel.sawTarget = false;
    sDel.since = 0;
  }
  if (busy) {
    sDel.wasBusy = true;
    if (lamp.targetBusy()) sDel.sawTarget = true;
    // endBurst() et giveUp() le remettent a 0 avant le front : releve ici.
    if (lamp.pendingSince()) sDel.since = lamp.pendingSince();
  }
}

void jsonDeliveryFlush() { deliveryPoll(millis()); }

// ===========================================================================
//  Cycle de vie
// ===========================================================================

void jsonBegin() {
  // Aucune radio de l'ESP32 n'est active a ce stade : la source d'entropie
  // de l'ADC donne l'aleatoire (sans elle, IDF ne garantit qu'un pseudo-alea).
  bootloader_random_enable();
  sBoot = esp_random();
  bootloader_random_disable();
  sLoopAt = millis();
}

void jsonAttach() {
  lamp.setHooks(&kLampHooks);
  statusLedSetObserver(onLed);
  sDel.seenDelivered = lamp.deliveredCount();
  sDel.seenGiveUps = lamp.giveUpCount();
  sConfigSig = configSig();
}

// Periode echue : la suivante part de la precedente ; tres en retard (commande
// bloquante), de maintenant.
static bool due(uint32_t &next, uint32_t period, uint32_t now) {
  if (!period || (int32_t)(now - next) < 0) return false;
  next += period;
  if ((int32_t)(now - next) >= 0) next = now + period;
  return true;
}

void jsonPoll() {
  const uint32_t now = millis();
  const uint32_t turn = now - sLoopAt;
  if (turn > sLoopMaxMs) sLoopMaxMs = turn;
  sLoopAt = now;

  deliveryPoll(now);

  if (sS.machine && sS.leaseS) {
    // Le bail court depuis le plus recent : dernier octet recu, ou fin de la
    // derniere commande (une commande de banc de 60 s ne le fait pas expirer).
    const uint32_t last = (int32_t)(sLastRx - sLastCmd) > 0 ? sLastRx : sLastCmd;
    if (now - last >= (uint32_t)sS.leaseS * 1000u) leaveMachine(true, now);
  }

  if (sS.machine) {
    if (due(sS.nextEtat, sS.periodMs, now)) pushState(now, true);
    if (due(sS.nextCpt, sS.countersMs, now)) pushCounters(now, true);
    if (due(sS.nextNet, sS.netMs, now)) pushNet(now, true);
    if ((!sS.periodMs || sS.periodMs > kHbMs) && due(sS.nextHb, kHbMs, now)) push(Item::Heartbeat, now, true);
  }
  drain(now);
}
