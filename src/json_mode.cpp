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
#include "h1_proto.h"
#include "halo1_lamp.h"
#include "status_led.h"
#ifndef DIAG_ONLY
#include "matter_bridge.h"
#endif
#if MATTER_NET_THREAD
#include "net_udp.h"
#endif

using namespace jsonp;
using namespace halo1;

// ===========================================================================
//  Etat
// ===========================================================================

static constexpr uint32_t kHbMs = 2000;          // battement quand les etat sont coupes ou lents
static constexpr uint32_t kPeriodDefault = 1000, kCountersDefault = 1000, kNetDefault = 5000;
static constexpr uint16_t kLeaseDefault = 30;
static constexpr uint8_t kReplies = 4;           // reponses differees (apres un instantane)
static constexpr uint32_t kHeapBlocMs = 10000;   // plus grand bloc du tas relu au plus toutes les 10 s
// Transport reseau (sections 10.5 et 10.6) : profil de 'json 1', trames
// coupees seules apres 60 s, retard admis dans la file (le debit plafonne du
// reseau la vide plus lentement que l'USB : un instantane fait ~6 Ko, deux
// instantanes simultanes ~12 Ko a 3 Ko/s).
static constexpr uint32_t kRemotePeriod = 2000, kRemoteCounters = 0, kRemoteNet = 30000;
static constexpr uint32_t kRemoteFramesMs = 60000;
static constexpr uint32_t kRemoteLateMs = 6000;
// Plafonds d'evenements par seconde d'une session reseau (USB : 50, 10, 50, 20).
static constexpr uint16_t kRemoteRxCap = 10, kRemoteCrcCap = 5, kRemoteTxCap = 10, kRemoteLogCap = 10;
// Sessions : l'USB, plus une par session reseau etablie dans le build Thread.
#if MATTER_NET_THREAD
static constexpr uint8_t kSinks = kOrigins;
#else
static constexpr uint8_t kSinks = 1;
#endif

static Writer sW;              // le seul tampon de formatage (1024 octets)
static bool sBusy = false;     // une ligne en cours de formatage dans sW
static uint32_t sBoot = 0;

// Reponse differee : part par la file (apres les lignes deja en file, ou
// quand une ligne entiere tient dans le tampon d'emission).
struct PendingReply {
  bool used;
  Reply r;         // r.cmd pointe sur cmd ; r.code litteral ; r.msg nul
  uint32_t t0;     // durAtSend : duree_ms mesuree a l'envoi (instantane)
  bool durAtSend;
  bool cache;      // gardee pour un id repete (reseau) ; pas un refus deja_traite
  char cmd[kCmdTextMax + 1];
};

// Une session par transport (origine). L'USB (kUsb) existe toujours ; une
// origine reseau n'a de sens que tant que sa session H1 est etablie
// (net_udp.cpp : jsonRemoteReset a chaque changement).
struct Sink {
  bool machine = false;
  uint32_t periodMs = kPeriodDefault, countersMs = kCountersDefault, netMs = kNetDefault;
  uint16_t leaseS = kLeaseDefault;
  bool frames = true, log = false;
  uint32_t framesUntil = 0;  // reseau : trames coupees a cet instant (0 : sans limite)
  uint32_t nextEtat = 0, nextCpt = 0, nextNet = 0, nextHb = 0;
  uint32_t lastRx = 0, lastCmd = 0;  // bail : dernier octet ou message recu, fin de la derniere commande
  uint32_t n = 0;                    // n de la prochaine ligne produite sur ce transport
  uint32_t lost = 0, tooLong = 0, rejected = 0;
  uint32_t loopMaxMs = 0;            // plus long tour de loop() depuis le bloc sante emis
  uint32_t topId = 0;                // reseau : plus haut id admis dans cette session (jsonRemoteAdmit)
  Queue q;
  PendingReply replies[kReplies] = {};
  RateCap rxCap{50}, crcCap{10}, txCap{50}, logCap{20};
  Cadence cadence;
};
static Sink sSinks[kSinks];
#if MATTER_NET_THREAD
static ReplyCache sCache[kSinks - 1];  // origines reseau seulement
#endif
static uint8_t sOrigin = kUsb;           // origine de la commande en cours

static uint32_t sConfigSig = 0;
static bool sRefreshSaved = false;             // 'json etat' : abonnements sauves relus
static uint32_t sLoopAt = 0;
// heap_caps_get_largest_free_block() parcourt tout le tas en section critique
// (interruptions masquees sur ce C6 mono-coeur) : relu au plus toutes les
// kHeapBlocMs, et a chaque 'json 1' ou 'json etat'.
static uint32_t sHeapBloc = 0, sHeapBlocAt = 0;
static bool sHeapBlocStale = true;

static DeliveryWatch sDel;  // observateur de livraison (section 7.3)

static uint32_t upS() { return (uint32_t)(esp_timer_get_time() / 1000000); }
static bool remote(uint8_t o) { return o != kUsb; }

static bool anyMachine() {
  for (const Sink &s : sSinks)
    if (s.machine) return true;
  return false;
}

// ===========================================================================
//  Emission
// ===========================================================================

// Place d'emission libre sur ce transport : octets du tampon de HWCDC (USB),
// ou places de la file des datagrammes x une ligne (reseau).
static int room(uint8_t o) {
  if (o == kUsb) return Serial.availableForWrite();
#if MATTER_NET_THREAD
  return (int)netUdpFreeSlots() * (int)kLineMax;
#else
  return 0;
#endif
}

// Ecrit la ligne fermee de sW sur ce transport, entiere ou pas du tout.
static bool emit(uint8_t o) {
  const size_t len = sW.size();
  if (o == kUsb) {
    if (Serial.availableForWrite() < (int)len) return false;
    Serial.write(sW.data(), len);
    return true;
  }
#if MATTER_NET_THREAD
  // Un datagramme : l'objet JSON seul, sans RS ni LF (section 10.2).
  return len >= 2 && netUdpSend((uint8_t)(o - 1), sW.data() + 1, len - 2);
#else
  return false;
#endif
}

// Reserve le tampon unique. false : une ligne est deja en cours (bogue :
// aucun formatage ne doit en appeler un autre), rien n'est produit.
static bool claim() {
  if (sBusy) return false;
  sBusy = true;
  return true;
}

// Ferme la ligne formatee pour la session o et l'ecrit, ou la perd sans
// attendre. n compte toute ligne produite, ecrite ou perdue.
static bool send(uint8_t o) {
  sBusy = false;
  Sink &s = sSinks[o];
  s.n++;
  if (!sW.finish()) {
    s.tooLong++;
    return false;
  }
  if (!emit(o)) {
    s.lost++;
    return false;
  }
  return true;
}

// Evenement frequent (rx, tx, log) vers une session reseau : seulement s'il
// reste ensuite la place d'une ligne periodique ou d'une reponse (debit
// plafonne : 20 evenements par seconde depasseraient les 3 Ko/s et
// affameraient etat et reponses). Sinon perdu, n consomme et compte.
static bool eventRoom(uint8_t o) {
  if (!remote(o) || room(o) >= 2 * (int)kLineMax) return true;
  sSinks[o].n++;
  sSinks[o].lost++;
  return false;
}

// Texte emis par le protocole lui-meme sur l'USB (fin de bail, invite) : meme
// chemin non bloquant, perdu et compte si le tampon est plein.
static void textNb(const char *s) {
  const size_t len = strlen(s);
  if (Serial.availableForWrite() < (int)len) {
    sSinks[kUsb].lost++;
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

static void helloBase(uint8_t o, uint32_t now) {
  const Sink &s = sSinks[o];
  const esp_app_desc_t *d = esp_app_get_description();
  sW.begin("hello", s.n, now);
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
  sW.str("transport", remote(o) ? "udp" : "usb");
  sW.u32("periode_ms", s.periodMs);
  sW.u32("compteurs_ms", s.countersMs);
  sW.u32("reseau_ms", s.netMs);
  sW.u32("bail_s", s.leaseS);
  sW.boolean("trames", s.frames);
  sW.boolean("log", s.log);
  sW.end();
  sW.obj("limites");
  sW.u32("ligne_max", kLineMax);
  sW.u32("cmd_max", kCmdMax);
  sW.end();
}

static void helloIdentity(uint8_t o, uint32_t now) {
  sW.begin("hello", sSinks[o].n, now);
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
#if MATTER_NET_THREAD
  sW.str(nullptr, "udp");
  sW.str(nullptr, "cle");
#endif
  sW.end();
}

static uint32_t gammaC() { return (uint32_t)(mapGamma() * 100.0f + 0.5f); }

static void config(uint8_t o, uint32_t now) {
  sW.begin("config", sSinks[o].n, now);
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

static void etatLampe(uint8_t o, uint32_t now) {
  sW.begin("etat", sSinks[o].n, now);
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

static void etatTranches(uint8_t o, uint32_t now) {
  sW.begin("etat", sSinks[o].n, now);
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

static void etatSante(uint8_t o, uint32_t now) {
  const Sink &k = sSinks[o];
  sW.begin("etat", k.n, now);
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
  if (sHeapBlocStale || now - sHeapBlocAt >= kHeapBlocMs) {
    sHeapBloc = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    sHeapBlocAt = now;
    sHeapBlocStale = false;
  }
  sW.u32("heap_bloc", sHeapBloc);
  sW.u32("pile_boucle", (uint32_t)uxTaskGetStackHighWaterMark(nullptr));  // octets sous ESP-IDF
  sW.u32("boucle_max_ms", k.loopMaxMs);  // remis a 0 quand la ligne part (produce)
  sW.u32("json_perdus", k.lost);
  sW.u32("json_trop_longs", k.tooLong);
  sW.u32("rejets", k.rejected);
  sW.end();
}

static void cptPilote(uint8_t o, uint32_t now) {
  const Halo1Lamp::Stats &s = lamp.stats;
  sW.begin("compteurs", sSinks[o].n, now);
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

static void cptRadio(uint8_t o, uint32_t now) {
  const Halo1Radio::Stats &r = lamp.radio.stats;
  sW.begin("compteurs", sSinks[o].n, now);
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

// Une reponse vient de partir vers une origine reseau : gardee pour un id repete.
static void cacheReply(uint8_t o, const Reply &r) {
#if MATTER_NET_THREAD
  if (remote(o) && r.fin) sCache[o - 1].put(r);
#else
  (void)o;
  (void)r;
#endif
}

// Une ligne de la file de la session o : formatee maintenant, avec les
// valeurs du moment.
static void produce(uint8_t o, const Queued &q, uint32_t now) {
  if (!claim()) return;
  Sink &k = sSinks[o];
  Reply sent;
  bool isReply = false;
  switch (q.item) {
    case Item::HelloBase: helloBase(o, now); break;
    case Item::HelloId: helloIdentity(o, now); break;
    case Item::Config:
      config(o, now);
      sConfigSig = configSig();
      break;
    case Item::EtatLampe: etatLampe(o, now); break;
    case Item::EtatTranches: etatTranches(o, now); break;
    case Item::EtatSante: etatSante(o, now); break;
    case Item::CptPilote: cptPilote(o, now); break;
    case Item::CptRadio: cptRadio(o, now); break;
#ifndef DIAG_ONLY
    case Item::CptMatter:
      sW.begin("compteurs", k.n, now);
      sW.str("bloc", "matter");
      matterJsonCounters(sW);
      break;
    case Item::NetThread:
      sW.begin("reseau", k.n, now);
      sW.str("bloc", "thread");
      matterJsonNetThread(sW, now, remote(o));
      break;
#if MATTER_NET_THREAD
    case Item::NetSubs:
      sW.begin("reseau", k.n, now);
      sW.str("bloc", "abonnements");
      matterJsonNetSubs(sW, now, sRefreshSaved);
      sRefreshSaved = false;
      break;
    case Item::NetIp:
      sW.begin("reseau", k.n, now);
      sW.str("bloc", "ip");
      netUdpJson(sW, now);
      break;
#endif
#endif
    case Item::Heartbeat: heartbeat(sW, k.n, now, sBoot, upS(), k.lost); break;
    case Item::Reply: {
      PendingReply &p = k.replies[q.arg < kReplies ? q.arg : 0];
      Reply r = p.r;
      r.cmd = p.cmd;
      if (p.durAtSend) r.durMs = now - p.t0;
      if (r.hasLease) {
        r.leaseS = k.leaseS;
        r.upS = upS();
      }
      reply(sW, k.n, now, r);
      p.used = false;
      sent = r;
      isReply = p.cache;
      break;
    }
    default:  // bloc absent de ce build : rien de produit
      sBusy = false;
      return;
  }
  // Le maximum n'est remis a 0 que s'il est parti : perdue, la ligne suivante le porte.
  const bool ok = send(o);
  if (ok && q.item == Item::EtatSante) k.loopMaxMs = 0;
  // Perdue ou non, une reponse est donnee pour cet id : un id repete la renvoie
  // (sauf un refus deja_traite, jamais garde).
  if (isReply) cacheReply(o, sent);
}

static void drain(uint8_t o, uint32_t now) {
  Sink &k = sSinks[o];
  // Ligne periodique perdue par retard : n consomme (trou visible), comptee.
  // Jamais une reponse (Queue::dropLate).
  const uint8_t late = k.q.dropLate(now, remote(o) ? kRemoteLateMs : kLateMs);
  k.n += late;
  k.lost += late;
  const Queued *q = k.q.front();
  if (!q || sBusy) return;
  // Periodique : avec 2048 octets libres, il en reste 1024 apres elle pour un
  // evenement. Reponse : des que 1024 sont libres. Sinon, au tour suivant.
  if (!k.q.frontReady(room(o))) return;
  const Queued item = *q;
  k.q.pop();
  produce(o, item, now);
}

static void push(uint8_t o, Item item, uint32_t now, bool session) {
  Sink &k = sSinks[o];
  if (!k.q.push(item, now, session)) {
    // File pleine : la ligne est perdue, comme une ligne en retard.
    k.n++;
    k.lost++;
  }
}

static void pushState(uint8_t o, uint32_t now, bool session) {
  push(o, Item::EtatLampe, now, session);
  push(o, Item::EtatTranches, now, session);
  push(o, Item::EtatSante, now, session);
}

static void pushCounters(uint8_t o, uint32_t now, bool session) {
  push(o, Item::CptPilote, now, session);
  push(o, Item::CptRadio, now, session);
#ifndef DIAG_ONLY
  push(o, Item::CptMatter, now, session);
#endif
}

static void pushNet(uint8_t o, uint32_t now, bool session) {
#ifndef DIAG_ONLY
  push(o, Item::NetThread, now, session);
#if MATTER_NET_THREAD
  push(o, Item::NetSubs, now, session);
  push(o, Item::NetIp, now, session);
#endif
#else
  (void)o;
  (void)now;
  (void)session;
#endif
}

static void pushHello(uint8_t o, uint32_t now, bool session) {
  push(o, Item::HelloBase, now, session);
  push(o, Item::HelloId, now, session);
  push(o, Item::Config, now, session);
}

// ===========================================================================
//  Reponses (vers l'origine de la commande en cours)
// ===========================================================================

// Ecrit la reponse tout de suite (perdue et comptee si elle ne tient pas).
// Perdue ou non, elle est donnee pour cet id : gardee (reseau) pour un renvoi.
static void replyEmit(uint8_t o, const Reply &r, bool cache) {
  if (claim()) {
    reply(sW, sSinks[o].n, millis(), r);
    send(o);
  }
  if (cache) cacheReply(o, r);
}

// Reponse par la file de la session o (apres les lignes deja en file) ; sans
// place (4 reponses en attente, file pleine) : tout de suite. Differee, elle
// perd son msg (il pointerait sur un tampon disparu).
static void replyQueue(uint8_t o, const Reply &r, uint32_t t0, bool durAtSend, bool cache = true) {
  Sink &k = sSinks[o];
  for (uint8_t i = 0; i < kReplies; i++) {
    PendingReply &p = k.replies[i];
    if (p.used) continue;
    p.used = true;
    p.r = r;
    p.r.msg = nullptr;
    p.r.key = nullptr;
    p.r.kid = nullptr;
    p.r.hasKid = false;
    p.t0 = t0;
    p.durAtSend = durAtSend;
    p.cache = cache;
    copyCmd(p.cmd, r.cmd);
    if (k.q.push(Item::Reply, millis(), false, i)) return;
    p.used = false;
    break;
  }
  Reply now = r;
  if (durAtSend) now.durMs = millis() - t0;
  if (now.hasLease) {
    now.leaseS = k.leaseS;
    now.upS = upS();
  }
  replyEmit(o, now, cache);
}

// Reponse immediate. Reseau : la cle n'y part jamais ; sans place dans la file
// des datagrammes, ou derriere une reponse deja en file (l'ordre des reponses
// est garde), elle attend dans la file de la session au lieu d'etre perdue.
static void replyTo(uint8_t o, const Reply &r, bool cache = true) {
  Reply out = r;
  if (remote(o)) out.key = nullptr;
  if (remote(o) && (room(o) < (int)kLineMax || sSinks[o].q.has(Item::Reply))) {
    replyQueue(o, out, 0, false, cache);
    return;
  }
  replyEmit(o, out, cache);
}

void jsonReply(const Reply &r) { replyTo(sOrigin, r); }

// Reponse apres les lignes d'un instantane deja en file.
static void replyAfterQueue(const JsonCmd &c, bool lease) {
  if (!c.hasId) return;
  Reply r;
  r.id = c.id;
  r.cmd = c.cmd;
  r.hasLease = lease;
  replyQueue(sOrigin, r, c.t0, true);
}

void jsonReplyEnd(const Reply &r) {
  // Rien a doubler et la place d'une ligne entiere : tout de suite, juste
  // apres le texte. Sinon (le texte de la commande a rempli le tampon
  // d'emission : 'help' en ecrit 7 Ko), par la file, des que la place revient.
  if (!sSinks[sOrigin].q.has(Item::Reply) && room(sOrigin) >= (int)kLineMax) {
    jsonReply(r);
    return;
  }
  replyQueue(sOrigin, r, 0, false);
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
  r.leaseS = sSinks[sOrigin].leaseS;
  r.upS = upS();
  jsonReply(r);
}

void jsonRefuse(const JsonCmd &c, const char *code, const char *msg) {
  sSinks[sOrigin].rejected++;
  if (c.hasId) replyNow(c, false, code, msg);
  else if (sOrigin == kUsb && !sSinks[kUsb].machine) Serial.printf("Ligne refusee (%s) : %s\n", code, msg);
}

// ===========================================================================
//  Session
// ===========================================================================

static void resetTimers(Sink &k, uint32_t now) {
  k.nextEtat = now + k.periodMs;
  k.nextCpt = now + k.countersMs;
  k.nextNet = now + k.netMs;
  k.nextHb = now + kHbMs;
}

static void enterMachine(uint8_t o, uint16_t leaseS, uint32_t now) {
  Sink &k = sSinks[o];
  k.machine = true;
  k.loopMaxMs = 0;  // pas les tours du mode humain (commandes de banc) avant la session
  if (remote(o)) {
    // Profil distant (10.6) : chaque etat (~1,2 Ko, trois datagrammes) fait une
    // douzaine de trames 802.15.4 a quelques cm du BM5602.
    k.periodMs = kRemotePeriod;
    k.countersMs = kRemoteCounters;
    k.netMs = kRemoteNet;
    k.frames = false;
    k.rxCap.setLimit(kRemoteRxCap);
    k.crcCap.setLimit(kRemoteCrcCap);
    k.txCap.setLimit(kRemoteTxCap);
    k.logCap.setLimit(kRemoteLogCap);
  } else {
    k.periodMs = kPeriodDefault;
    k.countersMs = kCountersDefault;
    k.netMs = kNetDefault;
    k.frames = true;
  }
  k.framesUntil = 0;
  k.leaseS = leaseS;
  k.log = false;
  k.lastRx = k.lastCmd = now;
  resetTimers(k, now);
}

// Fin du mode machine : lignes de session retirees, message fin, puis (USB)
// texte et invite par le chemin non bloquant.
static void leaveMachine(uint8_t o, bool lease, uint32_t now) {
  Sink &k = sSinks[o];
  k.q.dropSession();
  if (claim()) {
    sessionEnd(sW, k.n, now, lease ? "bail" : "commande");
    send(o);
  }
  k.machine = false;
  if (o != kUsb) return;
  if (lease) {
    char t[80];
    snprintf(t, sizeof(t), "json : mode machine coupe (hote muet depuis %u s)\r\n> ", (unsigned)k.leaseS);
    textNb(t);
  } else {
    textNb("> ");
  }
}

bool jsonMachine() { return sSinks[kUsb].machine; }
void jsonNoteRx() { sSinks[kUsb].lastRx = millis(); }
uint32_t jsonBootId() { return sBoot; }

// Une origine invalide ne change rien (jamais de repli silencieux sur l'USB,
// qui echappe a la liste blanche).
void jsonSetOrigin(uint8_t origin) {
  if (origin < kSinks) sOrigin = origin;
}
uint8_t jsonOrigin() { return sOrigin; }

void jsonRemoteReset(uint8_t origin) {
  if (origin == kUsb || origin >= kSinks) return;
  // Sur place (un Sink temporaire couterait pres d'un Ko de pile). n continue :
  // numero de ligne du transport depuis le demarrage ; le reste repart des
  // valeurs par defaut.
  Sink &k = sSinks[origin];
  k.machine = false;
  k.periodMs = kPeriodDefault;
  k.countersMs = kCountersDefault;
  k.netMs = kNetDefault;
  k.leaseS = kLeaseDefault;
  k.frames = true;
  k.log = false;
  k.framesUntil = 0;
  k.nextEtat = k.nextCpt = k.nextNet = k.nextHb = 0;
  k.lastRx = k.lastCmd = 0;
  k.lost = k.tooLong = k.rejected = 0;
  k.loopMaxMs = 0;
  k.topId = 0;
  k.q.clear();
  for (PendingReply &p : k.replies) p.used = false;
  k.rxCap = RateCap(50);
  k.crcCap = RateCap(10);
  k.txCap = RateCap(50);
  k.logCap = RateCap(20);
  k.cadence = Cadence();
#if MATTER_NET_THREAD
  sCache[origin - 1].clear();
#endif
  sDel.dropOrigin(origin);
}

void jsonNoteRemoteRx(uint8_t origin) {
  if (origin != kUsb && origin < kSinks) sSinks[origin].lastRx = millis();
}

bool jsonRemoteAdmit(uint32_t id, const char *shown) {
#if !MATTER_NET_THREAD
  (void)id;
  (void)shown;
  return false;
#else
  const uint8_t o = sOrigin;
  if (o == kUsb || o >= kSinks) return false;
  Sink &k = sSinks[o];
  // Reponse differee de cet id encore en file (instantane) : elle partira.
  for (const PendingReply &p : k.replies)
    if (p.used && p.r.id == id) return true;
  const Reply *cached = sCache[o - 1].find(id);
  if (cached && !strcmp(cached->cmd, shown)) {
    // Meme id, meme commande : la reponse perdue repart, rien n'est reexecute.
    Reply out = *cached;
    char cmd[kCmdTextMax + 1];
    copyCmd(cmd, cached->cmd);  // put() va reecrire l'entree : plus de pointeur dedans
    out.cmd = cmd;
    replyTo(o, out);
    return true;
  }
  if (cached || id <= k.topId) {
    // id deja traite, reponse plus en cache (ou autre commande sous le meme id) :
    // jamais de nouvelle execution. Refus non garde (le cache reste celui de l'id).
    Reply r;
    r.id = id;
    r.cmd = shown;
    r.ok = false;
    r.code = "deja_traite";
    r.msg = "id deja traite (reponse plus disponible) : rien n'est reexecute";
    replyTo(o, r, false);
    return true;
  }
  k.topId = id;
  return false;
#endif
}

void jsonCountRejected() { sSinks[sOrigin].rejected++; }

bool jsonCadenceOk(uint32_t now) { return sSinks[sOrigin].cadence.allow(now); }

void jsonAfterCommand() {
  const uint32_t now = millis();
  sSinks[sOrigin].lastCmd = now;
  if (!anyMachine()) return;
  const uint32_t sig = configSig();
  if (sig != sConfigSig) {
    sConfigSig = sig;  // une seule fois par changement, meme si la ligne se perd
    for (uint8_t o = 0; o < kSinks; o++)
      if (sSinks[o].machine) push(o, Item::Config, now, true);
  }
}

static void printSession(Print &out) {
  const Sink &k = sSinks[kUsb];
  out.printf("Mode machine : %s", k.machine ? "ACTIF" : "coupe ('json 1' pour l'activer)");
  if (k.machine) {
    if (k.leaseS) out.printf(", bail de %u s", k.leaseS);
    else out.print(", sans bail (jusqu'a 'json 0')");
  }
  out.println();
  out.printf("  periodes : etat %lu ms, compteurs %lu ms, reseau %lu ms ; trames %s ; log %s\n",
             (unsigned long)k.periodMs, (unsigned long)k.countersMs, (unsigned long)k.netMs,
             k.frames ? "oui" : "non", k.log ? "oui" : "non");
  out.printf("  lignes   : n = %lu, %lu perdue(s), %lu trop longue(s), %lu ligne(s) de l'hote refusee(s)\n",
             (unsigned long)k.n, (unsigned long)k.lost, (unsigned long)k.tooLong, (unsigned long)k.rejected);
  for (uint8_t o = 1; o < kSinks; o++) {
    const Sink &r = sSinks[o];
    if (!r.n && !r.machine) continue;
    out.printf("  reseau %u : mode machine %s ; n = %lu, %lu perdue(s), %lu refusee(s)\n", (unsigned)o,
               r.machine ? "actif" : "coupe", (unsigned long)r.n, (unsigned long)r.lost, (unsigned long)r.rejected);
  }
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

// Periode : 0 (coupe, si permis) ou lo..60000 ms.
static bool setPeriod(char *p, uint32_t lo, bool zeroOk, uint32_t *out, uint32_t *next, uint32_t now) {
  uint32_t v;
  if (!parseU32(nextWord(p), &v) || *nextWord(p) || (!v && !zeroOk) || (v && (v < lo || v > 60000))) return false;
  *out = v;
  *next = now + v;
  return true;
}

// 'json cle ...' (section 10.4) : USB seulement. La liste blanche la refuse
// deja au reseau ; refusee ici aussi (defense en profondeur : la reponse de
// 'nouvelle' porte la cle).
static void keyCommand(char *p, const JsonCmd &c) {
  if (sOrigin != kUsb) {
    replyNow(c, false, "interdite", "json cle : USB seulement");
    return;
  }
#if MATTER_NET_THREAD
  const char *w = nextWord(p);
  char kid[9] = {};
  if (!*w) {
    const bool has = netUdpKid(kid);
    Reply r;
    r.id = c.id;
    r.cmd = c.cmd;
    r.durMs = millis() - c.t0;
    r.hasKid = true;
    r.kid = has ? kid : nullptr;
    if (c.hasId) jsonReply(r);
    else if (has) Serial.printf("json cle : empreinte %s (transport reseau actif, port UDP %u)\n", kid, kHaloUdpPort);
    else Serial.println("json cle : aucune cle, transport reseau coupe");
    return;
  }
  if (!strcmp(w, "nouvelle")) {
    const char *hex = nextWord(p);
    uint8_t appRandom[32];
    bool hexOk = strlen(hex) == 64 && !*nextWord(p);
    for (uint8_t i = 0; hexOk && i < 32; i++) {
      auto nib = [](char ch) -> int {
        return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
      };
      const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
      if (hi < 0 || lo < 0) hexOk = false;
      else appRandom[i] = (uint8_t)(hi << 4 | lo);
    }
    if (!c.hasId) {
      // La cle ne s'affiche jamais en texte : 'pio device monitor' enregistre la
      // session (log2file) dans un fichier a la racine du depot.
      h1::wipe(appRandom, sizeof(appRandom));
      Serial.println("json cle nouvelle : reservee a l'app (ligne avec id=, la cle part dans la reponse)");
      return;
    }
    if (!hexOk) {
      h1::wipe(appRandom, sizeof(appRandom));
      replyNow(c, false, "usage", "json cle nouvelle <64 hexa majuscules> (alea de l'app)");
      return;
    }
    // La reponse est la seule copie de la cle : pas de cle neuve si elle ne peut
    // pas partir tout de suite (tampon d'emission USB plein).
    if (Serial.availableForWrite() < (int)kLineMax) {
      h1::wipe(appRandom, sizeof(appRandom));
      replyNow(c, false, "refuse", "tampon USB plein : rien n'est change, reessayer");
      return;
    }
    char keyHex[65];
    const NetKeyResult res = netUdpKeyNew(appRandom, keyHex, kid);
    h1::wipe(appRandom, sizeof(appRandom));
    if (res == NetKeyResult::Crypto || res == NetKeyResult::Nvs) {
      replyNow(c, false, "refuse",
               res == NetKeyResult::Nvs ? "cle non ecrite (NVS) : ancienne cle gardee"
                                        : "cle non creee (crypto) : ancienne cle gardee");
      return;
    }
    Reply r;
    r.id = c.id;
    r.cmd = c.cmd;
    r.durMs = millis() - c.t0;
    r.msg = res == NetKeyResult::Ok ? "nouvelle cle : les sessions reseau tombent"
                                    : "cle ecrite mais pas chargee : transport reseau coupe jusqu'au redemarrage";
    r.key = keyHex;
    r.hasKid = true;
    r.kid = kid;
    jsonReply(r);
    h1::wipe(keyHex, sizeof(keyHex));
    return;
  }
  if (!strcmp(w, "efface") && !*nextWord(p)) {
    const bool ok = netUdpKeyErase();
    if (c.hasId) {
      Reply r;
      r.id = c.id;
      r.cmd = c.cmd;
      r.ok = ok;
      r.code = ok ? "ok" : "refuse";
      r.msg = ok ? "cle effacee : transport reseau coupe" : "effacement NVS en echec (cle retiree de la memoire)";
      r.durMs = millis() - c.t0;
      r.hasKid = true;
      r.kid = nullptr;
      jsonReply(r);
    } else {
      Serial.println(ok ? "json cle : cle effacee, transport reseau coupe"
                        : "json cle : effacement NVS en echec (cle retiree de la memoire)");
    }
    return;
  }
  replyNow(c, false, "usage", "json cle [nouvelle <64 hexa> | efface]");
  if (!c.hasId) Serial.println("Usage : json cle [efface]   ('json cle nouvelle' : app seulement)");
#else
  (void)p;
  // Transport reseau (section 10) : pas dans ce firmware.
  replyNow(c, false, "refuse", "json cle : transport reseau absent de ce firmware");
  if (!c.hasId) Serial.println("json cle : transport reseau absent de ce firmware (USB seulement)");
#endif
}

void jsonCommand(char *arg, const JsonCmd &c) {
  char *p = arg;
  const char *sub = nextWord(p);
  const uint32_t now = millis();
  const uint8_t o = sOrigin;
  Sink &k = sSinks[o];
  static const char *const kUsage =
      "json [1 [bail 0|10..600] | 0 | etat | hello | ping | periode ms | compteurs ms | reseau ms | "
#if MATTER_NET_THREAD
      "trames 0|1 | log 0|1 | cle]";
#else
      "trames 0|1 | log 0|1]";
#endif
  // Reseau (10.5) : bornes propres, verifiees ici aussi (la liste blanche les
  // verifie deja ; defense en profondeur).
  const bool rem = remote(o);
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
    if (bad || (rem && (lease < 10 || lease > 120))) {
      usage = rem ? "json 1 [bail 10..120] (reseau)" : "json 1 [bail 0|10..600]";
    } else {
      // Idempotent : renvoyer 'json 1' resynchronise (instantane complet).
      enterMachine(o, (uint16_t)lease, now);
      sRefreshSaved = true;
      sHeapBlocStale = true;
      pushHello(o, now, true);
      pushState(o, now, true);
      pushCounters(o, now, true);
      pushNet(o, now, true);
      replyAfterQueue(c, true);
      return;
    }
  } else if (!strcmp(sub, "0")) {
    if (*nextWord(p)) {
      usage = "json 0";
    } else {
      replyNow(c, true, "ok", k.machine ? nullptr : "deja en mode humain");
      if (k.machine) leaveMachine(o, false, now);
      else if (!c.hasId) Serial.println("json : mode machine deja coupe");
      return;
    }
  } else if (!strcmp(sub, "etat")) {
    sRefreshSaved = true;
    sHeapBlocStale = true;
    pushState(o, now, false);
    pushCounters(o, now, false);
    pushNet(o, now, false);
    replyAfterQueue(c, false);
    return;
  } else if (!strcmp(sub, "hello")) {
    pushHello(o, now, false);
    replyAfterQueue(c, false);
    return;
  } else if (!strcmp(sub, "ping")) {
    // Le bail court deja depuis cette ligne (octets recus, fin de commande).
    replyNow(c, true, "ok", nullptr, true);
    if (!c.hasId) {
      if (k.machine) Serial.printf("json : bail renouvele (%u s)\n", k.leaseS);
      else Serial.println("json : pas de session machine ('json 1')");
    }
    return;
  } else if (!strcmp(sub, "periode")) {
    if (!setPeriod(p, rem ? 2000 : 200, !rem, &k.periodMs, &k.nextEtat, now))
      usage = rem ? "json periode 2000..60000 (reseau)" : "json periode 0|200..60000";
    k.nextHb = now + kHbMs;
    sessionChanged = !usage;
  } else if (!strcmp(sub, "compteurs")) {
    if (!setPeriod(p, rem ? 5000 : 200, true, &k.countersMs, &k.nextCpt, now))
      usage = rem ? "json compteurs 0|5000..60000 (reseau)" : "json compteurs 0|200..60000";
    sessionChanged = !usage;
  } else if (!strcmp(sub, "reseau")) {
    if (!setPeriod(p, rem ? 10000 : 1000, true, &k.netMs, &k.nextNet, now))
      usage = rem ? "json reseau 0|10000..60000 (reseau)" : "json reseau 0|1000..60000";
    sessionChanged = !usage;
  } else if (!strcmp(sub, "trames") || !strcmp(sub, "log")) {
    const char *w = nextWord(p);
    const bool on = !strcmp(w, "1");
    if ((!on && strcmp(w, "0")) || *nextWord(p)) {
      usage = !strcmp(sub, "trames") ? "json trames 0|1" : "json log 0|1";
    } else {
      const bool frames = !strcmp(sub, "trames");
      (frames ? k.frames : k.log) = on;
      // A distance, les trames se coupent seules apres 60 s (10.5).
      const uint32_t until = now + kRemoteFramesMs;
      if (frames) k.framesUntil = on && remote(o) ? (until ? until : 1) : 0;
      sessionChanged = true;
    }
  } else if (!strcmp(sub, "cle")) {
    keyCommand(p, c);
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
  if (sessionChanged && k.machine) push(o, Item::HelloBase, now, true);
  replyNow(c, true, "ok", nullptr);
  if (!c.hasId)
    Serial.printf("json : etat %lu ms, compteurs %lu ms, reseau %lu ms, trames %s, log %s\n",
                  (unsigned long)k.periodMs, (unsigned long)k.countersMs, (unsigned long)k.netMs,
                  k.frames ? "oui" : "non", k.log ? "oui" : "non");
}

// ===========================================================================
//  Evenements (vers chaque session en mode machine)
// ===========================================================================

// Evenement du pont : formate une fois (n provisoire 0), puis renumerote pour
// chaque session en mode machine (Writer::setN) ; aucun champ propre a une
// session dans intent, abonnement, thread. Le tampon reste reserve jusqu'a la
// fin de la diffusion.
Writer *jsonEventOpen(const char *type) {
  if (!anyMachine() || !claim()) return nullptr;
  sW.begin(type, 0, millis());
  return &sW;
}

void jsonEventSend() {
  if (!sBusy) return;
  const bool ok = sW.finish();
  for (uint8_t o = 0; o < kSinks; o++) {
    Sink &k = sSinks[o];
    if (!k.machine) continue;
    const bool numbered = ok && sW.setN(k.n);
    k.n++;
    if (!numbered) {
      k.tooLong++;
      continue;
    }
    if (!emit(o)) k.lost++;
  }
  sBusy = false;
}

bool jsonLog(const char *src, const char *niv, const char *txt) {
  const uint32_t now = millis();
  // Les annonces du bouton BOOT passent hors plafond : quelques lignes par
  // appui (anti-rebond de 30 ms), et celle d'une action precede souvent un
  // reset, apres lequel aucun log ne porterait ses 'sautes'. Elles portent
  // celles des autres.
  const bool capped = strcmp(src, "bouton") != 0;
  bool usbTaken = false;  // le texte n'est plus ecrit sur l'USB
  for (uint8_t o = 0; o < kSinks; o++) {
    Sink &k = sSinks[o];
    if (!k.machine || !k.log) continue;
    if (capped && !k.logCap.available(now)) {
      k.logCap.skip();
      if (o == kUsb) usbTaken = true;
      continue;
    }
    if (!eventRoom(o)) continue;
    if (!claim()) continue;  // ligne en cours (jamais attendu) : en texte sur l'USB
    if (capped) k.logCap.take();
    logLine(sW, k.n, now, src, niv, txt, k.logCap.takeSkipped());
    send(o);
    if (o == kUsb) usbTaken = true;
  }
  return usbTaken;
}

static void onLampRx(const RxEvent &e) {
  const uint32_t now = millis();
  const bool crc = e.kind == Kind::CrcBad;
  for (uint8_t o = 0; o < kSinks; o++) {
    Sink &k = sSinks[o];
    if (!k.machine || !k.frames) continue;
    if (!k.rxCap.available(now) || (crc && !k.crcCap.available(now))) {
      k.rxCap.skip();
      continue;
    }
    if (!eventRoom(o) || !claim()) continue;
    k.rxCap.take();
    if (crc) k.crcCap.take();
    rx(sW, k.n, now, e, k.rxCap.takeSkipped());
    send(o);
  }
}

static void onLampTx(const TxEvent &e) {
  const uint32_t now = millis();
  for (uint8_t o = 0; o < kSinks; o++) {
    Sink &k = sSinks[o];
    if (!k.machine || !k.frames) continue;
    if (!k.txCap.available(now)) {
      k.txCap.skip();
      continue;
    }
    if (!eventRoom(o) || !claim()) continue;
    k.txCap.take();
    tx(sW, k.n, now, e, k.txCap.takeSkipped());
    send(o);
  }
}

static void onLampRelaunch(const RelaunchEvent &e) {
  for (uint8_t o = 0; o < kSinks; o++) {
    if (!sSinks[o].machine || !claim()) continue;
    relaunch(sW, sSinks[o].n, millis(), e);
    send(o);
  }
}

static void onLampModule(const ModuleEvent &e) {
  for (uint8_t o = 0; o < kSinks; o++) {
    if (!sSinks[o].machine || !claim()) continue;
    module(sW, sSinks[o].n, millis(), e);
    send(o);
  }
}

static bool onLampLog(bool trace, const char *line) { return jsonLog("lampe", trace ? "trace" : "notice", line); }

static const LampHooks kLampHooks = {onLampRx, onLampTx, onLampRelaunch, onLampModule, onLampLog};

static void onLed(statusled::Pattern now, statusled::Pattern before, bool testing) {
  for (uint8_t o = 0; o < kSinks; o++) {
    if (!sSinks[o].machine || !claim()) continue;
    led(sW, sSinks[o].n, millis(), statusled::patternCode(now), statusled::patternCode(before), testing);
    send(o);
  }
}

// ===========================================================================
//  Observateur de livraison (section 7.3)
// ===========================================================================

void jsonPendingId(uint32_t id) { sDel.pendingId(id, lamp.pendingSince(), sOrigin); }

// Pur et teste sur l'hote (DeliveryWatch, json_out) ; ici, le releve du pilote
// et l'emission : chaque session recoit la livraison avec ses propres id
// (hors mode machine, seulement si elle en attendait).
static void deliveryPoll(uint32_t now) {
  LampSample s;
  s.busy = lamp.busy();
  s.targetBusy = lamp.targetBusy();
  s.delivered = lamp.deliveredCount();
  s.giveUps = lamp.giveUpCount();
  s.pendingSince = lamp.pendingSince();
  Delivery d;
  if (!sDel.poll(s, now, anyMachine(), &d)) return;
  d.cause = lamp.lastGiveUp();
  d.last = lamp.lastDelivered();
  d.version = lamp.version();
  d.target = lamp.target();
  d.believed = lamp.believed();
  d.dirty = lamp.dirty();
  for (uint8_t o = 0; o < kSinks; o++) {
    uint32_t ids[kIdsMax];
    uint8_t n = 0;
    for (uint8_t i = 0; i < d.nIds; i++)
      if (d.origins && d.origins[i] == o) ids[n++] = d.ids[i];
    const uint32_t lost = d.idsLostBy ? d.idsLostBy[o] : 0;
    if (!sSinks[o].machine && !n && !lost) continue;
    Delivery mine = d;
    mine.ids = ids;
    mine.nIds = n;
    mine.idsLost = lost;
    if (!claim()) continue;
    delivery(sW, sSinks[o].n, now, mine);
    send(o);
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
}

void jsonAttach() {
  lamp.setHooks(&kLampHooks);
  statusLedSetObserver(onLed);
  sDel.reset(lamp.deliveredCount(), lamp.giveUpCount());
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
  // Premier tour : pas de mesure (le reste de setup(), Matter.begin() compris,
  // n'est pas un tour de loop()).
  if (sLoopAt) {
    const uint32_t turn = now - sLoopAt;
    for (Sink &k : sSinks)
      if (turn > k.loopMaxMs) k.loopMaxMs = turn;
  }
  sLoopAt = now ? now : 1;

  deliveryPoll(now);

  // L'USB d'abord ; les sessions reseau, qui partagent la file des datagrammes
  // et son debit, a tour de role (deux instantanes simultanes avancent ensemble).
  static uint8_t sTurn = 0;
  sTurn++;
  for (uint8_t i = 0; i < kSinks; i++) {
    const uint8_t o = i == 0 || kSinks < 3 ? i : (uint8_t)(1 + ((i - 1 + sTurn) % (kSinks - 1)));
    Sink &k = sSinks[o];
    if (k.machine && leaseExpired(now, k.lastRx, k.lastCmd, k.leaseS)) leaveMachine(o, true, now);
    if (k.machine) {
      if (k.framesUntil && (int32_t)(now - k.framesUntil) >= 0) {
        k.frames = false;
        k.framesUntil = 0;
        push(o, Item::HelloBase, now, true);  // reglage de session change
      }
      if (due(k.nextEtat, k.periodMs, now)) pushState(o, now, true);
      if (due(k.nextCpt, k.countersMs, now)) pushCounters(o, now, true);
      if (due(k.nextNet, k.netMs, now)) pushNet(o, now, true);
      if ((!k.periodMs || k.periodMs > kHbMs) && due(k.nextHb, kHbMs, now)) push(o, Item::Heartbeat, now, true);
    }
    drain(o, now);
  }
}
