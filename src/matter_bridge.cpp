#include "matter_bridge.h"

#include <Matter.h>
#include <Preferences.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <platform/ConfigurationManager.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <stdarg.h>

#include "config.h"
#include "halo1_lamp.h"
#include "status_led.h"

#if MATTER_NET_THREAD
#include <app/CASESessionManager.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadHandler.h>
#include <app/SubscriptionResumptionSessionEstablisher.h>
#include <esp_event.h>
#include <esp_ieee802154.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <lib/support/CHIPMem.h>
#include <openthread/dns_client.h>
#include <openthread/link.h>
#include <openthread/platform/radio.h>
#include <openthread/srp_client.h>
#include <openthread/thread.h>

#include "matter_resume.h"

// Les bibliotheques precompilees ont ces deux options (sdkconfig, puis
// AppBuildConfig.h) : sans elles ici, SubscriptionInfo n'aurait pas la meme
// forme que dans la pile (mResumptionRetries), et la reprise non plus.
#if !CHIP_CONFIG_PERSIST_SUBSCRIPTIONS || !CHIP_CONFIG_SUBSCRIPTION_TIMEOUT_RESUMPTION
#error "reprise des abonnements : CHIP_CONFIG_PERSIST_SUBSCRIPTIONS et _SUBSCRIPTION_TIMEOUT_RESUMPTION attendus"
#endif
#ifndef MATTER_THREAD_MED
#define MATTER_THREAD_MED 0
#endif
// Prendre le verrou OpenThread avant l'init de la pile planterait.
static bool sMatterStarted = false;

// esp_openthread_lock_acquire(ticks) prend deux mutex recursifs a la suite,
// chacun avec le meme delai : 2 x ticks au pire. Le second est pris seul, et
// brievement, par la tache lwIP a chaque paquet IPv6 sortant. Si le second
// expire, IDF 5.5.5 rend lui-meme le premier (firmware.elf desassemble) : un
// refus ne laisse rien a rendre. Ici, totalMs est le delai total.
static bool otLockTry(uint32_t totalMs) { return esp_openthread_lock_acquire(pdMS_TO_TICKS(totalMs / 2)); }

// ===========================================================================
//  Garde d'antenne Thread autour de chaque paquet lampe
//
//  Terrain du 23/09 : Thread a +20 dBm sur le canal 25 (2475 MHz), a quelques
//  cm du BM5602 qui parle a la lampe sur 2405 MHz. Juste apres chaque commande
//  Apple Home, des paquets lampe finissent en MAX_RT par rafales (17/250 en
//  routeur, 19/64 en MED), 0 au banc sans Thread ; certains accuses n'arrivent
//  qu'apres des reprises du BC5602 (RT2 03). Hypothese : nos propres trames
//  Thread (reponse a l'ecriture, rapports d'attributs) chevauchent l'echange
//  de ~1,6 ms et aveuglent le BM5602 (ou la lampe).
//
//  Le verrou OpenThread tenu pendant l'echange empeche toute nouvelle trame :
//  CSMA et reprises MAC sont logicielles sur le C6 (otPlatRadioGetCaps n'a ni
//  CSMA_BACKOFF ni TRANSMIT_RETRIES), donc dans la tache OT. Une trame deja
//  partie se termine en materiel : on attend la fin de l'etat TRANSMIT (CCA,
//  emission, attente de son accuse). Reste hors d'atteinte l'accuse MAC que la
//  puce renvoie seule a une trame recue (~0,5 ms) : l'API publique le classe
//  en RECEIVE, comme l'ecoute permanente d'un routeur ou d'un MED.
//
//  REGLE : sous ce verrou, AUCUN appel Matter/CHIP (verrou de la pile
//  compris). La tache CHIP prend le verrou OT en tenant le sien : prendre le
//  sien ici bloquerait les deux taches pour toujours. Seul le SPI du BC5602
//  tourne entre enter et leave (Halo1Radio::sendOne).
// ===========================================================================

static constexpr uint32_t kGuardLockMs = 20;  // au-dela, paquet emis sans garde
static bool sGuardHeld = false;

static bool airGuardEnter(uint32_t maxWaitUs, uint32_t *waitedUs) {
  *waitedUs = 0;
  if (!sMatterStarted || !otLockTry(kGuardLockMs)) return false;
  sGuardHeld = true;
  if (esp_ieee802154_get_state() == ESP_IEEE802154_RADIO_TRANSMIT) {
    const uint32_t t0 = micros();
    uint32_t w;
    bool busy;
    do {
      delayMicroseconds(50);
      w = micros() - t0;
      busy = esp_ieee802154_get_state() == ESP_IEEE802154_RADIO_TRANSMIT;
    } while (busy && w < maxWaitUs);
    // maxWaitUs exactement : trame encore en l'air au plafond. Finie pendant
    // le dernier pas, elle ne compte pas comme plafonnee.
    *waitedUs = busy ? maxWaitUs : (w >= maxWaitUs ? maxWaitUs - 1 : (w ? w : 1));
  }
  return true;
}

static void airGuardLeave() {
  if (!sGuardHeld) return;
  sGuardHeld = false;
  esp_openthread_lock_release();
}

static const Halo1AirGuard kAirGuard = {airGuardEnter, airGuardLeave};
#endif

using namespace chip::app::Clusters;

// ===========================================================================
//  Pont Matter de la lampe Halo 1 (plan du pilote Halo 1, section E)
//
//  EP1 "Halo"          lumiere a temperature de couleur : marche, luminosite
//                      unique (une trame ne porte qu'une valeur), temperature
//  EP2 "Halo avant"    cette lampe est allumee (marche ET lampe avant)
//  EP3 "Halo arriere"  idem pour la lampe arriere
//  EP4 "Halo auto"     prise momentanee : un appui sur le bouton A ; un A de
//                      la telecommande entendu y fait la meme impulsion, sans
//                      rien emettre. DESACTIVE par defaut depuis le 23/09
//                      (HALO1_EXPOSE_AUTO 0) : tout son code est compile hors
//                      du firmware, rien d'autre ne change.
//
//  Les numeros viennent de l'ordre de creation ; les noms se donnent dans
//  l'app. Matter est multi-admin : le meme noeud se jumelle a Apple Home,
//  Google Home, Alexa et Home Assistant. L'identite du noeud (fabricant,
//  produit, numero de serie, versions) est posee avant Matter.begin().
//
//  Les callbacks tournent dans la tache CHIP : ils deposent l'ordre dans une
//  boite d'intentions et c'est tout (ni SPI, ni appel au pilote). La tache
//  loop vide la boite apres un moment de calme, en tire UNE consigne
//  (resolveMatter), puis reflete la consigne vers Matter sous le verrou de la
//  pile. Matter affiche toujours la consigne du pilote.
// ===========================================================================

static MatterColorTemperatureLight mainLight;
#if HALO1_SELECTORS_AS_LIGHTS
// A1 (a) : "eteins les lumieres" donne a la piece touche aussi les deux lampes.
using LampEndpoint = MatterOnOffLight;
#else
// A1 (b) : hors des commandes de piece, mais les lampes s'affichent en prises.
using LampEndpoint = MatterOnOffPlugin;
#endif
static LampEndpoint frontLamp, backLamp;
#if HALO1_EXPOSE_AUTO
static MatterOnOffPlugin autoButton;
#endif

// ===========================================================================
//  Boite d'intentions : remplie par la tache CHIP, videe par la tache loop
// ===========================================================================

static TaskHandle_t sLoopTask = nullptr;  // pris dans matterBridgeBegin() (setup = tache loop)
static portMUX_TYPE sInboxMux = portMUX_INITIALIZER_UNLOCKED;
static halo1::MatterIntents sInbox;  // la derniere valeur gagne, champ par champ
static uint32_t sInFirst = 0, sInLast = 0;

// Nos reflets passent par attribute::update() et redeclenchent les callbacks,
// mais toujours dans la tache loop ; les ordres des controleurs arrivent dans
// la tache CHIP. Un callback dans la tache loop est donc notre propre echo.
static inline bool ownEcho() { return xTaskGetCurrentTaskHandle() == sLoopTask; }

// Toujours true : la bibliotheque met son cache a jour, et aucun ordre n'est
// refuse ici (les regles s'appliquent a la resolution, dans la tache loop).
template <class F>
static bool post(uint8_t bit, F set) {
  if (ownEcho()) return true;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sInboxMux);
  if (!sInbox.has) sInFirst = now;
  sInLast = now;
  sInbox.has |= bit;
  set(sInbox);
  portEXIT_CRITICAL(&sInboxMux);
  return true;
}

// Un callback par attribut, jamais onChange() : celui-ci transmet aussi les
// valeurs en cache des autres attributs, qui passeraient pour des ordres.
static bool onMainOnOff(bool on) {
  return post(halo1::IN_POWER, [=](halo1::MatterIntents &i) { i.power = on; });
}
static bool onMainLevel(uint8_t l) {
  if (l > 254) return true;  // CurrentLevel nul : aucun niveau a appliquer
  return post(halo1::IN_LEVEL, [=](halo1::MatterIntents &i) { i.level = l; });
}
static bool onMainMired(uint16_t m) {
  return post(halo1::IN_MIREDS, [=](halo1::MatterIntents &i) { i.mireds = m; });
}
static bool onFront(bool on) {
  return post(halo1::IN_FRONT, [=](halo1::MatterIntents &i) { i.front = on; });
}
static bool onBack(bool on) {
  return post(halo1::IN_BACK, [=](halo1::MatterIntents &i) { i.back = on; });
}
#if HALO1_EXPOSE_AUTO
// Seul le passage a on est un appui ; le retour a off ne demande rien.
static bool onAuto(bool on) { return on ? post(halo1::IN_AUTO, [](halo1::MatterIntents &) {}) : true; }
#endif

// ===========================================================================
//  Identify ("Identifier" dans Apple Home) : arc-en-ciel sur la LED d'etat
//
//  Tache CHIP : les rappels ne font que poser des valeurs atomiques, lues par
//  la tache loop (matterIdentifying). Une session Identify (IdentifyTime) finit
//  par un STOP de la pile, par endpoint. Un TriggerEffect n'en recoit jamais
//  (commentaire d'app_identification_cb, bibliotheque Matter) : sa fin est
//  donc datee ici, par endpoint, d'apres l'effet demande (statusled::effectEnd).
// ===========================================================================

static_assert(statusled::kEffectBlink == MatterIdentifyRequest::BLINK &&
                  statusled::kEffectBreathe == MatterIdentifyRequest::BREATHE &&
                  statusled::kEffectOkay == MatterIdentifyRequest::OKAY &&
                  statusled::kEffectChannelChange == MatterIdentifyRequest::CHANNEL_CHANGE &&
                  statusled::kEffectFinish == MatterIdentifyRequest::FINISH &&
                  statusled::kEffectStop == MatterIdentifyRequest::STOP,
              "identifiants d'effet Identify");

static constexpr uint8_t kIdentifyEps = HALO1_EXPOSE_AUTO ? 4 : 3;  // EP1 a EP3, et EP4 s'il existe
static uint32_t sIdentifyEps = 0;                       // un bit par endpoint en session Identify
static uint32_t sIdentifyEffectEnd[kIdentifyEps] = {};  // fin d'un TriggerEffect (millis), 0 = aucun
static uint32_t sIdentifyCount = 0;                     // demandes recues (session ou effet)

static bool onIdentify(const MatterEndPoint &ep, uint8_t i, bool active) {
  // Remplie par la bibliotheque juste avant ce rappel, dans cette meme tache.
  const MatterIdentifyRequest r = ep.getIdentifyRequest();
  if (active) __atomic_fetch_add(&sIdentifyCount, 1, __ATOMIC_RELAXED);
  if (r.fromTriggerEffect) {
    // Seule cette tache pose une fin ; la tache loop ne fait qu'effacer une fin
    // echue, par echange compare (matterIdentifying) : rien ne se perd.
    const uint32_t end = __atomic_load_n(&sIdentifyEffectEnd[i], __ATOMIC_RELAXED);
    __atomic_store_n(&sIdentifyEffectEnd[i], statusled::effectEnd(end, r.effectId, millis()), __ATOMIC_RELAXED);
  } else if (active) {
    __atomic_fetch_or(&sIdentifyEps, 1u << i, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_and(&sIdentifyEps, ~(1u << i), __ATOMIC_RELAXED);
  }
  return true;
}

// ===========================================================================
//  Etat du pont (tache loop uniquement)
// ===========================================================================

static uint32_t sBootMs = 0, sSeenVersion = 0, sLastReflect = 0;
static bool sBootGuard = true;     // garde-fou de demarrage encore arme
static bool sForceReflect = true;  // realigner Matter sur la consigne au prochain passage
#if HALO1_EXPOSE_AUTO
static uint32_t sAutoPulseAt = 0;
static bool sAutoPulse = false;       // EP4 a on : impulsion du bouton A en cours
static uint32_t sSeenRemoteAuto = 0;  // lamp.remoteAutoCount() deja reflete
// A de la telecommande entendu, EP4 pas encore mis a on : l'impulsion n'a pas
// commence (sAutoPulseAt = instant de l'appui entendu). Elle part de la montee
// reellement ecrite (reflect), que la boite d'intentions, le verrou de la pile
// ou un echec d'ecriture peuvent retarder. Au-dela de kAutoRaiseMaxWaitMs,
// l'appui n'est plus reflete : une fenetre de coalescence (400 ms) et un
// echange CASE (quelques centaines de ms) y tiennent largement, et un reflet
// plus tardif tromperait plus qu'il n'informerait.
static bool sAutoRaise = false;
static constexpr uint32_t kAutoRaiseMaxWaitMs = 3000;
#endif
static struct {
  uint32_t windows, bootIgnored, autoFired, autoRefused, autoHeard, autoLost, reflects, writes, writeFails,
      lockBusy, logDropped;
} sStats = {};

// ===========================================================================
//  Duree de l'impulsion d'EP4, reglable sur le terrain
//
//  Terrain du 23/09 : apres un appui dans Apple Home, l'interrupteur met ~10 s
//  a revenir a off dans l'app, alors que le firmware le remet a off au bout de
//  1 s. Hypothese : Home ecarte un rapport contraire trop proche de sa propre
//  ecriture, et ne relit l'attribut que plus tard. La duree se regle donc sans
//  reflasher ('matter impulsion <ms>'), et reste en NVS : espace de noms du
//  pilote, une cle a part. Lue et ecrite dans la tache loop seulement. Sans
//  EP4, ni lue ni ecrite : une valeur deja sauvee attend son retour.
// ===========================================================================

static const char *const kNvsNs = "halo1";
#if HALO1_EXPOSE_AUTO
static const char *const kNvsPulseKey = "impulsion";
static_assert(HALO1_AUTO_PULSE_MS >= kMatterPulseMinMs && HALO1_AUTO_PULSE_MS <= kMatterPulseMaxMs,
              "HALO1_AUTO_PULSE_MS hors de kMatterPulseMinMs..kMatterPulseMaxMs");
static uint16_t sAutoPulseMs = HALO1_AUTO_PULSE_MS;

static void loadAutoPulse() {
  Preferences p;
  // En ecriture meme pour lire, comme le pilote : en lecture seule, un espace
  // de noms absent fait loguer une erreur NVS au premier demarrage.
  if (!p.begin(kNvsNs, false)) return;
  // isKey() d'abord : interroger une cle absente logue une erreur NVS.
  if (p.isKey(kNvsPulseKey)) {
    const uint16_t v = p.getUShort(kNvsPulseKey, HALO1_AUTO_PULSE_MS);
    if (v >= kMatterPulseMinMs && v <= kMatterPulseMaxMs) sAutoPulseMs = v;  // sinon : defaut
  }
  p.end();
}

uint16_t matterAutoPulseMs() { return sAutoPulseMs; }

bool matterSetAutoPulseMs(uint32_t ms, bool *saved) {
  if (saved) *saved = false;
  if (ms < kMatterPulseMinMs || ms > kMatterPulseMaxMs) return false;
  sAutoPulseMs = (uint16_t)ms;  // une impulsion en cours finit a la nouvelle duree
  Preferences p;
  if (!p.begin(kNvsNs, false)) return true;
  const bool ok = p.putUShort(kNvsPulseKey, sAutoPulseMs) == sizeof(uint16_t);
  p.end();
  if (saved) *saved = ok;
  return true;
}
#endif  // HALO1_EXPOSE_AUTO

// Journal du pont : comme celui du pilote, perdu plutot que d'attendre le port.
static void bridgeLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bridgeLog(const char *fmt, ...) {
  char line[192];  // la trace la plus longue d'une fenetre fait 171 caracteres
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
  if (n < 0 || Serial.availableForWrite() < n + 2) {
    sStats.logDropped++;
    return;
  }
  Serial.println(line);
}

static void append(char *buf, size_t n, size_t &w, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void append(char *buf, size_t n, size_t &w, const char *fmt, ...) {
  if (w >= n) return;
  va_list ap;
  va_start(ap, fmt);
  const int k = vsnprintf(buf + w, n - w, fmt, ap);
  va_end(ap);
  if (k > 0) w += (size_t)k;
}

// " EP1 on niveau 127 mireds 250 avant off A"
static void describeIntents(const halo1::MatterIntents &in, char *buf, size_t n) {
  size_t w = 0;
  buf[0] = 0;
  if (in.has & halo1::IN_POWER) append(buf, n, w, " EP1 %s", in.power ? "on" : "off");
  if (in.has & halo1::IN_LEVEL) append(buf, n, w, " niveau %u", in.level);
  if (in.has & halo1::IN_MIREDS) append(buf, n, w, " mireds %u", in.mireds);
  if (in.has & halo1::IN_FRONT) append(buf, n, w, " avant %s", in.front ? "on" : "off");
  if (in.has & halo1::IN_BACK) append(buf, n, w, " arriere %s", in.back ? "on" : "off");
  if (in.has & halo1::IN_AUTO) append(buf, n, w, " A");
}

// ===========================================================================
//  Matter -> lampe : une fenetre de coalescence refermee
// ===========================================================================

static void applyIntents(const halo1::MatterIntents &in, uint32_t first, uint32_t now) {
  char what[72];
  describeIntents(in, what, sizeof(what));
  // Dans tous les cas, Matter est realigne sur la consigne resolue : EP4
  // repasse a off tout de suite si A est refuse.
  sForceReflect = true;
  // Garde-fou : rien n'est emis au demarrage. Un controleur ne peut pas ecrire
  // aussi tot ; un ordre la vient de la pile elle-meme. La fenetre est jugee a
  // son premier ordre, pour qu'elle ne passe pas en se refermant apres le delai.
  // Difference signee : un ordre depose pendant Matter.begin() precede sBootMs.
  if (sBootGuard && (int32_t)(first - sBootMs) < (int32_t)HALO1_BOOT_IGNORE_MS) {
    sStats.bootIgnored++;
    bridgeLog("[matter] ordres ignores au demarrage :%s", what);
    return;
  }
  sStats.windows++;
  const halo1::Resolution r = halo1::resolveMatter(lamp.target(), in, lamp.memoryLamps());
  if (r.fields) lamp.request(r.target, r.fields);
  const char *autoText = "";
#if HALO1_EXPOSE_AUTO
  // Sans EP4, rien ne depose IN_AUTO (onAuto n'est pas branche).
  if (in.has & halo1::IN_AUTO) {
    // resolveMatter applique le garde-fou de groupe (A2) : A arrive avec un
    // ordre marche ou lampe -> commande de piece ou tuile regroupee, ignore.
    if (r.fireAuto && lamp.pressAuto()) {
      sAutoPulse = true;
      sAutoPulseAt = now;
      sStats.autoFired++;
      autoText = ", appui A";
    } else {
      sStats.autoRefused++;
      autoText = r.target.power ? ", A ignore (avec un ordre marche/lampe)" : ", A refuse (lampe eteinte)";
    }
  }
#endif
  if (lamp.tracing()) {
    char st[48], fl[32];
    Halo1Lamp::describe(lamp.target(), st, sizeof(st));
    Halo1Lamp::describeFields(r.fields, fl, sizeof(fl));
    bridgeLog("[matter]%s -> %s (a livrer : %s)%s", what, st, fl, autoText);
  }
}

// ===========================================================================
//  Lampe -> Matter : reflet de la consigne
// ===========================================================================

// Valeur d'un attribut booleen, uint8 ou uint16, nullable ou non.
static bool valueOf(const esp_matter_attr_val_t &v, uint16_t &out) {
  switch ((int)v.type & ~ESP_MATTER_VAL_NULLABLE_BASE) {
    case ESP_MATTER_VAL_TYPE_BOOLEAN: out = v.val.b ? 1 : 0; return true;
    case ESP_MATTER_VAL_TYPE_UINT8: out = v.val.u8; return true;
    case ESP_MATTER_VAL_TYPE_UINT16: out = v.val.u16; return true;
    default: return false;
  }
}

static void setValue(esp_matter_attr_val_t &v, uint16_t x) {
  switch ((int)v.type & ~ESP_MATTER_VAL_NULLABLE_BASE) {
    case ESP_MATTER_VAL_TYPE_BOOLEAN: v.val.b = x != 0; break;
    case ESP_MATTER_VAL_TYPE_UINT8: v.val.u8 = (uint8_t)x; break;
    default: v.val.u16 = x; break;
  }
}

// Ecrit la valeur voulue (calculee d'apres la valeur actuelle) si elle differe,
// en gardant le type lu : CurrentLevel est nullable. updateAttributeVal passe
// par PRE_UPDATE, donc le cache de la bibliotheque suit ; il corrige aussi une
// valeur restauree depuis la NVS, que les setters sauteraient (cache egal).
// false si l'attribut n'a pas pu etre lu ou ecrit.
template <class F>
static bool syncAttr(MatterEndPoint &ep, uint32_t cluster, uint32_t attr, F want) {
  esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
  uint16_t cur = 0;
  if (!ep.getAttributeVal(cluster, attr, &v) || !valueOf(v, cur)) {
    sStats.writeFails++;
    return false;
  }
  const uint16_t w = want(cur);
  if (w == cur) return true;
  setValue(v, w);
  if (ep.updateAttributeVal(cluster, attr, &v)) {
    sStats.writes++;
    return true;
  }
  sStats.writeFails++;
  return false;
}

// Sous le verrou de la pile : aucun ordre d'un controleur ne s'intercale. false
// si rien n'a ete fait (verrou occupe, ou ordre arrive entre-temps).
static bool reflect(uint32_t now) {
  // Sans attendre : la tache CHIP garde le verrou pendant tout un evenement
  // (crypto PASE/CASE : des centaines de ms), et tick() ne doit pas s'arreter
  // (C.9). Occupe : nouvel essai au passage suivant, ~1 ms plus tard.
  // chip_stack_lock() avec un delai fini ferait un seul essai, dormirait, puis
  // afficherait une erreur a chaque refus.
  if (!chip::DeviceLayer::PlatformMgr().TryLockChipStack()) {
    sStats.lockBusy++;
    return false;
  }
  // La tache CHIP ecrit les attributs sous ce verrou : un ordre arrive juste
  // avant lui est deja dans la boite, et passe d'abord (curseur en cours).
  portENTER_CRITICAL(&sInboxMux);
  const bool pending = sInbox.has != 0;
  portEXIT_CRITICAL(&sInboxMux);
  if (pending) {
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    return false;
  }
  const halo1::State t = lamp.target();
  const bool front = t.power && (t.lamps & halo1::F_FRONT);
  const bool back = t.power && (t.lamps & halo1::F_BACK);
  syncAttr(mainLight, OnOff::Id, OnOff::Attributes::OnOff::Id, [&](uint16_t) { return (uint16_t)t.power; });
  // Affichage stable (E.2) : une valeur ecrite par un controleur, qui donne deja
  // la consigne, ne saute jamais vers une voisine. Jamais sous le plancher
  // (kMatterLevelFloor) : Apple Home montrerait 0 %, donc une lampe pleine.
  syncAttr(mainLight, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id,
           [&](uint16_t cur) { return (uint16_t)halo1::displayLevel((uint8_t)cur, t.bright); });
  syncAttr(mainLight, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id,
           [&](uint16_t cur) { return halo1::displayMired(cur, t.temp); });
  syncAttr(frontLamp, OnOff::Id, OnOff::Attributes::OnOff::Id, [&](uint16_t) { return (uint16_t)front; });
  syncAttr(backLamp, OnOff::Id, OnOff::Attributes::OnOff::Id, [&](uint16_t) { return (uint16_t)back; });
#if HALO1_EXPOSE_AUTO
  // Mis a on par un controleur, ou ici une fois pour un A de la telecommande
  // (sAutoRaise). Notre ecriture repasse par onAuto, mais dans la tache loop :
  // ownEcho() l'ecarte, rien n'est emis. Remis a off par un controleur pendant
  // l'impulsion, il y reste. L'impulsion d'un A entendu part d'ici, de la
  // montee reelle (ou d'EP4 deja a on) ; echec : nouvel essai au passage suivant.
  const bool raising = sAutoRaise;
  if (syncAttr(autoButton, OnOff::Id, OnOff::Attributes::OnOff::Id,
               [&](uint16_t cur) { return (uint16_t)(sAutoPulse && (cur || sAutoRaise)); })) {
    if (raising) sAutoPulseAt = now;
    sAutoRaise = false;
  }
#endif
  chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  sSeenVersion = lamp.version();
#if HALO1_EXPOSE_AUTO
  sForceReflect = sAutoRaise;  // montee d'EP4 ratee : a refaire
#else
  sForceReflect = false;
#endif
  sLastReflect = now;
  sStats.reflects++;
  return true;
}

#if MATTER_NET_THREAD
// ===========================================================================
//  Abonnements d'Apple Home et reseau Thread (build Thread)
//
//  Terrain du 23/09 : apres chaque redemarrage, Apple Home ne voit plus rien
//  du noeud (ni miroir de la telecommande : 66 ecritures d'attributs perdues)
//  tant que l'utilisateur n'agit pas dans l'app, et s'y raccroche ensuite en
//  2-3 min. La pile reprend l'abonnement d'Apple (persistant) une seule fois
//  au demarrage, des Server::Init : son init DNS-SD passe par mDNS, prete tout
//  de suite, alors que Thread et SRP ne le sont pas. La recherche d'adresse
//  de l'abonne expire (erreur 32 = CHIP_ERROR_TIMEOUT, vue a 47 s), et la
//  tentative suivante vient 300 s plus tard (puis 600, 600, 900 s...). Chaque
//  echec d'un etablisseur de la pile incremente un compteur sauve avec
//  l'abonnement (reprise tant qu'il vaut 10 au plus) : le 12e echec, meme a
//  travers les redemarrages, le fait oublier (firmware.elf desassemble).
//
//  Ce que ce bloc ajoute :
//   - mesures dans 'matter' : abonnements actifs et sauves, roles Thread
//     horodates, compteurs MLE, etat SRP et DNS, tentatives de reprise ;
//   - (a) relance de la reprise quand le reseau est pret (ResumePlanner),
//     session d'abord : un echec ne touche pas aux compteurs de la pile ;
//   - (b) type Thread choisi a l'execution ('matter med', enveloppe plus bas) ;
//   - (c) plafond de l'intervalle max des abonnements neufs (20 s par defaut).
//
//  Verrous : OpenThread seulement sous otLockTry (attente bornee) ; pile CHIP
//  seulement sous TryLockChipStack ou dans la tache CHIP (ScheduleWork) ;
//  jamais le verrou CHIP sous le verrou OT (la tache CHIP prend OT en tenant
//  le sien). Les rappels (tache CHIP, tache des evenements IDF) n'ecrivent que
//  des compteurs, sous sSubMux ; seule la tache loop affiche.
// ===========================================================================

using chip::app::SubscriptionResumptionStorage;
using SubInfo = SubscriptionResumptionStorage::SubscriptionInfo;
using ThreadDeviceType = chip::DeviceLayer::ConnectivityManager::ThreadDeviceType;

static portMUX_TYPE sSubMux = portMUX_INITIALIZER_UNLOCKED;
// Incremente sous sSubMux par tout evenement a tracer : la tache loop ne
// copie les compteurs que s'il a bouge (lecture atomique, sans le verrou).
static uint32_t sEventSeq = 0;

// "12,3" : des millisecondes en secondes, une decimale.
static const char *secs(char *b, size_t n, uint32_t ms) {
  snprintf(b, n, "%lu,%lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100));
  return b;
}

// --- Reglages (NVS halo1, tache loop) --------------------------------------

static const char *const kNvsMedKey = "med";
static const char *const kNvsResumeKey = "reprise";
static const char *const kNvsMaxIntKey = "maxint";
static constexpr uint8_t kMedRouter = 0, kMedEarly = 1, kMedLate = 2;  // 2 : MED apres Matter.begin() (ancien)
static uint8_t sMedMode = MATTER_THREAD_MED ? kMedEarly : kMedRouter;  // prochain demarrage
static uint8_t sMedBoot = MATTER_THREAD_MED ? kMedEarly : kMedRouter;  // ce demarrage
static bool sResumeAuto = true;
// Ecrit par la tache loop, lu par la tache CHIP (OnSubscriptionRequested).
// Defaut si la NVS n'a rien : 'matter maxint 0' l'ote pour de bon.
static volatile uint16_t sMaxIntCap = kMatterMaxIntDefaultS;

static void loadThreadSettings() {
  Preferences p;
  if (!p.begin(kNvsNs, false)) return;
  if (p.isKey(kNvsMedKey)) {
    const uint8_t v = p.getUChar(kNvsMedKey, sMedMode);
    if (v < kMatterMedModes) sMedMode = v;
  }
  if (p.isKey(kNvsResumeKey)) sResumeAuto = p.getUChar(kNvsResumeKey, 1) != 0;
  if (p.isKey(kNvsMaxIntKey)) {
    const uint16_t v = p.getUShort(kNvsMaxIntKey, kMatterMaxIntDefaultS);
    if (v == 0 || (v >= kMatterMaxIntMinS && v <= kMatterMaxIntMaxS)) sMaxIntCap = v;
  }
  p.end();
  sMedBoot = sMedMode;
}

static bool saveU8(const char *key, uint8_t v) {
  Preferences p;
  if (!p.begin(kNvsNs, false)) return false;
  const bool ok = p.putUChar(key, v) == sizeof(uint8_t);
  p.end();
  return ok;
}

static bool saveU16(const char *key, uint16_t v) {
  Preferences p;
  if (!p.begin(kNvsNs, false)) return false;
  const bool ok = p.putUShort(key, v) == sizeof(uint16_t);
  p.end();
  return ok;
}

bool matterResumeAuto() { return sResumeAuto; }
void matterSetResumeAuto(bool on, bool *saved) {
  sResumeAuto = on;
  const bool ok = saveU8(kNvsResumeKey, on ? 1 : 0);
  if (saved) *saved = ok;
}

uint8_t matterMedMode() { return sMedMode; }
bool matterSetMedMode(uint32_t mode, bool *saved) {
  if (saved) *saved = false;
  if (mode >= kMatterMedModes) return false;
  sMedMode = (uint8_t)mode;  // sMedBoot ne bouge pas : prochain demarrage
  const bool ok = saveU8(kNvsMedKey, sMedMode);
  if (saved) *saved = ok;
  return true;
}

uint16_t matterMaxIntervalCap() { return sMaxIntCap; }
bool matterSetMaxIntervalCap(uint32_t s, bool *saved) {
  if (saved) *saved = false;
  if (s != 0 && (s < kMatterMaxIntMinS || s > kMatterMaxIntMaxS)) return false;
  sMaxIntCap = (uint16_t)s;
  const bool ok = saveU16(kNvsMaxIntKey, (uint16_t)s);
  if (saved) *saved = ok;
  return true;
}

// --- (b) Type Thread des l'init --------------------------------------------
//
//  esp_matter::start (firmware.elf) : _InitThreadStack (Thread demarre avec le
//  mode restaure de sa NVS et cherche son parent), puis _SetThreadDeviceType
//  (Router), puis _StartThreadTask, puis Server::Init. Passer ensuite en MED,
//  comme le faisait ce pont, change deux fois le mode (routeur puis MED) :
//  deux ecritures en flash, et OpenThread relance l'attache a chaque passage
//  FTD <-> MTD (Mle::SetDeviceMode ; detache s'il etait deja attache).
//  L'enveloppe remplace la demande de routeur par MED (mode 1) : le mode
//  restaure, MED depuis le premier demarrage, ne change plus, rien n'est
//  ecrit, et l'attache n'est pas relancee (meme mode : sortie immediate).
//
//  Editeur de liens : -Wl,--wrap=<symbole> dans l'env esp32c6thread, avec
//  HALO_WRAP_THREAD_DEVTYPE (l'un sans l'autre ne lie pas), comme le core le
//  fait pour ESP32Utils::InitWiFiStack. Dans les bibliotheques, seul
//  esp_matter_core.cpp.obj y fait reference (nm) ; nos appels passent aussi
//  par ici. Methode : 'this' en premier argument, un pointeur suffit.
#ifndef HALO_WRAP_THREAD_DEVTYPE
#define HALO_WRAP_THREAD_DEVTYPE 0
#endif
static uint8_t sDevTypeCalls = 0, sDevTypeSwaps = 0;  // tache loop (Matter.begin() et apres)
// Copie a la sortie de Matter.begin() : les reglages d'esp_matter seuls, sans
// celui du pont qui suit (il passe aussi par l'enveloppe).
static uint8_t sDevTypeCallsEsp = 0, sDevTypeSwapsEsp = 0;

#if HALO_WRAP_THREAD_DEVTYPE
extern "C" CHIP_ERROR
__real__ZN4chip11DeviceLayer8Internal40GenericThreadStackManagerImpl_OpenThreadINS0_22ThreadStackManagerImplEE20_SetThreadDeviceTypeENS0_19ConnectivityManager16ThreadDeviceTypeE(
    void *self, ThreadDeviceType type);

extern "C" CHIP_ERROR
__wrap__ZN4chip11DeviceLayer8Internal40GenericThreadStackManagerImpl_OpenThreadINS0_22ThreadStackManagerImplEE20_SetThreadDeviceTypeENS0_19ConnectivityManager16ThreadDeviceTypeE(
    void *self, ThreadDeviceType type) {
  if (sDevTypeCalls < 255) sDevTypeCalls++;
  if (sMedBoot == kMedEarly && type == chip::DeviceLayer::ConnectivityManager::kThreadDeviceType_Router) {
    type = chip::DeviceLayer::ConnectivityManager::kThreadDeviceType_MinimalEndDevice;
    if (sDevTypeSwaps < 255) sDevTypeSwaps++;
  }
  return __real__ZN4chip11DeviceLayer8Internal40GenericThreadStackManagerImpl_OpenThreadINS0_22ThreadStackManagerImplEE20_SetThreadDeviceTypeENS0_19ConnectivityManager16ThreadDeviceTypeE(
      self, type);
}
#endif

// Mode lu juste apres Matter.begin(), avant notre propre bascule eventuelle.
static bool sModeKnown = false;
static otLinkModeConfig sModeAtBegin = {};

static void linkModeText(const otLinkModeConfig &m, char *b) {
  b[0] = m.mRxOnWhenIdle ? 'r' : '-';
  b[1] = m.mDeviceType ? 'd' : '-';
  b[2] = m.mNetworkData ? 'n' : '-';
  b[3] = 0;
}

// --- Historique des roles Thread (tache des evenements IDF) ------------------
//
//  esp_openthread poste OPENTHREAD_EVENT_ROLE_CHANGED (role precedent et
//  nouveau) sur la boucle d'evenements par defaut, depuis son rappel d'etat
//  OpenThread. Ecoute branchee AVANT Matter.begin() : les roles du demarrage
//  y sont. Aucun verrou OpenThread ni CHIP ici.
struct RoleChange {
  uint32_t ms;
  uint8_t from, to;
};
static constexpr uint8_t kRoleHistory = 6;
static RoleChange sRoles[kRoleHistory];
static uint32_t sRoleChanges = 0;  // depuis le demarrage, sous sSubMux
static bool sRoleHooked = false;

static void onOtRoleChanged(void *, esp_event_base_t, int32_t, void *data) {
  if (!data) return;
  const auto *e = static_cast<const esp_openthread_role_changed_event_t *>(data);
  const uint32_t now = millis();
  portENTER_CRITICAL(&sSubMux);
  sRoles[sRoleChanges % kRoleHistory] = {now, (uint8_t)e->previous_role, (uint8_t)e->current_role};
  sRoleChanges++;
  sEventSeq++;
  portEXIT_CRITICAL(&sSubMux);
}

static void hookRoleChanges() {
  // esp_matter::start cree la meme boucle et accepte qu'elle existe deja
  // (ESP_ERR_INVALID_STATE, firmware.elf).
  const esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;
  sRoleHooked =
      esp_event_handler_register(OPENTHREAD_EVENT, OPENTHREAD_EVENT_ROLE_CHANGED, onOtRoleChanged, nullptr) == ESP_OK;
}

static const char *roleName(uint8_t r) { return otThreadDeviceRoleToString((otDeviceRole)r); }

// --- Etat du reseau (tache loop, verrou OT sans attente) ---------------------

static bool srpRegistered(uint8_t st) {
  return st == OT_SRP_CLIENT_ITEM_STATE_REGISTERED || st == OT_SRP_CLIENT_ITEM_STATE_TO_REFRESH ||
         st == OT_SRP_CLIENT_ITEM_STATE_REFRESHING;
}

static bool roleAttached(otDeviceRole r) {
  return r == OT_DEVICE_ROLE_CHILD || r == OT_DEVICE_ROLE_ROUTER || r == OT_DEVICE_ROLE_LEADER;
}

static struct {
  bool known;
  uint32_t at;
  otDeviceRole role;
  uint8_t srpHost;  // otSrpClientItemState, 0xFF : inconnu
  uint32_t readyAt;  // premier instant ou le reseau a ete pret (0 : jamais)
} sNet = {false, 0, OT_DEVICE_ROLE_DISABLED, 0xFF, 0};

// Matter.isDeviceConnected() passe par _IsThreadAttached, qui prend le verrou
// OpenThread SANS limite de temps (esp_openthread_lock_acquire(portMAX_DELAY),
// firmware.elf) : appele par la LED a chaque passage de loop(), il pouvait
// bloquer tick() hors de tout budget (C.9). Ici, un essai sans attente par
// seconde ; verrou occupe : dernier etat connu, nouvel essai au passage suivant.
static void netPoll(uint32_t now) {
  static constexpr uint32_t kPollMs = 1000;
  if (!sMatterStarted || (sNet.known && (uint32_t)(now - sNet.at) < kPollMs) || !otLockTry(0)) return;
  otInstance *ot = esp_openthread_get_instance();
  const otDeviceRole role = otThreadGetDeviceRole(ot);
  const otSrpClientHostInfo *h = otSrpClientGetHostInfo(ot);
  const uint8_t host = h ? (uint8_t)h->mState : 0xFF;
  esp_openthread_lock_release();
  sNet.role = role;
  sNet.srpHost = host;
  sNet.known = true;
  sNet.at = now;
  if (!sNet.readyAt && roleAttached(role) && srpRegistered(host)) sNet.readyAt = now ? now : 1;
}

static bool netReady() { return sNet.known && roleAttached(sNet.role) && srpRegistered(sNet.srpHost); }

// --- Abonnements vus par la pile (tache CHIP) --------------------------------
//
//  Seul crochet public sur la vie des abonnements : ReadHandler::
//  ApplicationCallback (un seul par pile ; ni esp_matter ni la bibliotheque
//  Arduino n'en posent, nm). OnSubscriptionEstablished vient aussi pour un
//  abonnement repris (ReadHandler::OnSubscriptionResumed, qui le marque actif
//  juste avant), sans OnSubscriptionRequested : c'est ce qui les distingue.
//  Une reprise lancee par le pont s'etablit pendant son appel (session deja
//  ouverte, voir plus bas) : sResumingNow la distingue de celles de la pile.
enum : uint8_t { kEstFresh, kEstBridge, kEstStack };
static struct {
  uint32_t requested, capped, fresh, byBridge, byStack, terminated;
  uint32_t firstAt;  // premier abonnement etabli depuis le demarrage (0 : aucun)
  uint32_t lastAt;
  uint8_t lastKind;
  uint16_t lastMin, lastMax;
  uint64_t reqPeer;
  uint16_t reqMin, reqMax, reqApplied;
  uint32_t reqAt;
} sSubs = {};

// Demandes d'abonnement pas encore etablies : plusieurs peuvent se chevaucher
// (hub et iPhone, deux abonnements du hub). Une demande refusee apres coup
// n'a ni etablissement ni fin (OnSubscriptionTerminated ne vient que pour un
// abonnement actif) : elle expire. Sous le verrou de la pile seulement.
struct PendingRequest {
  const void *rh;
  uint64_t node;
  uint8_t fabric;
  uint32_t at;
};
static constexpr uint8_t kPendingMax = 4;
static constexpr uint32_t kPendingMs = 60000;
static PendingRequest sPending[kPendingMax] = {};
static bool sResumingNow = false;     // ResumeSubscription du pont en cours
static uint32_t sBridgeEstablished = 0;  // abonnements etablis pendant ces appels

static bool pendingLive(const PendingRequest &p, uint32_t now) {
  return p.rh && (uint32_t)(now - p.at) < kPendingMs;
}

// Retire la demande de ce ReadHandler ; true si elle etait encore valable.
static bool takePending(const void *rh, uint32_t now) {
  bool live = false;
  for (PendingRequest &p : sPending)
    if (p.rh == rh) {
      live |= pendingLive(p, now);
      p.rh = nullptr;
    }
  return live;
}

// L'abonne a-t-il deja un abonnement actif, ou une demande en cours ? Sous le
// verrou de la pile. SubjectHasActiveSubscription lit la session de chaque
// abonnement sans la verifier (firmware.elf), or un ReadHandler dont la
// session a ete evincee garde un SessionHolder vide jusqu'a son rapport
// suivant. GetAccessingFabricIndex, elle, verifie : un abonnement sans
// session (ou en PASE) compte pour la fabrique 0. Aucun : l'appel est sur.
// Sinon, repli prudent : tout abonnement de la meme fabrique sert l'abonne.
static bool subjectServed(chip::app::InteractionModelEngine *im, uint8_t fabric, uint64_t node, uint32_t now) {
  for (const PendingRequest &p : sPending)
    if (pendingLive(p, now) && p.node == node && p.fabric == fabric) return true;
  constexpr auto kSub = chip::app::ReadHandler::InteractionType::Subscribe;
  if (im->GetNumActiveReadHandlers(kSub, chip::kUndefinedFabricIndex))
    return im->GetNumActiveReadHandlers(kSub, fabric) != 0;
  return im->SubjectHasActiveSubscription(fabric, node);
}

class SubscriptionWatch : public chip::app::ReadHandler::ApplicationCallback {
 public:
  // (c) Plafond de l'intervalle max : Apple demande sans doute 600 s pour ce
  // noeud (code Darwin public). Apres un redemarrage qu'aucune reprise ne
  // rattrape, Apple ne s'apercoit de la perte qu'au bout de cet intervalle
  // (plus une marge), puis se reabonne seul ; un plafond plus court (180 s par
  // defaut) borne la duree sans miroir, au prix d'un rapport vide par
  // intervalle. Abonnements neufs seulement : un abonnement repris garde
  // l'intervalle sauve ; il ne sert donc qu'a partir du reabonnement suivant
  // d'Apple. Regle du SDK : plancher <= max <= max(3600, plafond demande).
  CHIP_ERROR OnSubscriptionRequested(chip::app::ReadHandler &rh, chip::Transport::SecureSession &session) override {
    uint16_t floorS = 0, maxS = 0;
    rh.GetReportingIntervals(floorS, maxS);
    uint16_t applied = maxS;
    const uint16_t cap = sMaxIntCap;
    if (cap) {
      const uint16_t want = cap < floorS ? floorS : cap;
      if (want < maxS && rh.SetMaxReportingInterval(want) == CHIP_NO_ERROR) applied = want;
    }
    const uint32_t now = millis();
    PendingRequest *slot = nullptr;
    for (PendingRequest &p : sPending)
      if (!slot && !pendingLive(p, now)) slot = &p;
    if (!slot) {  // quatre demandes vivantes : la plus ancienne cede sa place
      slot = &sPending[0];
      for (PendingRequest &p : sPending)
        if ((uint32_t)(now - p.at) > (uint32_t)(now - slot->at)) slot = &p;
    }
    *slot = {&rh, session.GetPeerNodeId(), session.GetFabricIndex(), now};
    portENTER_CRITICAL(&sSubMux);
    sSubs.requested++;
    if (applied != maxS) sSubs.capped++;
    sSubs.reqPeer = session.GetPeerNodeId();
    sSubs.reqMin = floorS;
    sSubs.reqMax = maxS;
    sSubs.reqApplied = applied;
    sSubs.reqAt = now;
    sEventSeq++;
    portEXIT_CRITICAL(&sSubMux);
    return CHIP_NO_ERROR;
  }

  void OnSubscriptionEstablished(chip::app::ReadHandler &rh) override {
    uint16_t minS = 0, maxS = 0;
    rh.GetReportingIntervals(minS, maxS);
    const uint32_t now = millis();
    const bool requested = takePending(&rh, now);
    const uint8_t kind = sResumingNow ? kEstBridge : requested ? kEstFresh : kEstStack;
    if (kind == kEstBridge) sBridgeEstablished++;
    portENTER_CRITICAL(&sSubMux);
    if (kind == kEstFresh) sSubs.fresh++;
    else if (kind == kEstBridge) sSubs.byBridge++;
    else sSubs.byStack++;
    if (!sSubs.firstAt) sSubs.firstAt = now ? now : 1;
    sSubs.lastAt = now;
    sSubs.lastKind = kind;
    sSubs.lastMin = minS;
    sSubs.lastMax = maxS;
    sEventSeq++;
    portEXIT_CRITICAL(&sSubMux);
  }

  void OnSubscriptionTerminated(chip::app::ReadHandler &rh) override {
    takePending(&rh, millis());
    portENTER_CRITICAL(&sSubMux);
    sSubs.terminated++;
    sEventSeq++;
    portEXIT_CRITICAL(&sSubMux);
  }
};
static SubscriptionWatch sSubWatch;

// --- (a) Relance de la reprise (tache CHIP) ---------------------------------
//
//  En deux temps, pour qu'un echec ne coute rien :
//   1. resumeWork : pour chaque abonne sauve sans abonnement (ni actif, ni
//      demande en cours), un observateur ouvre une session CASE vers lui
//      (FindOrEstablishSession avec nos propres rappels : fin, duree, erreur).
//      Echec (adresse introuvable : erreur 32) : rien d'autre. Ni le compteur
//      d'essais sauve avec l'abonnement, ni le calendrier de la pile ne
//      bougent.
//   2. Session ouverte : resumePeerWork, poste (ScheduleWork) pour passer
//      apres un etablisseur de la pile accroche a la meme mise en place : il
//      est prevenu dans la meme passe et cree son ReadHandler, actif tout de
//      suite (OnSubscriptionResumed). Si l'abonne n'a toujours rien : pour
//      chacun de ses abonnements sauves, un SubscriptionResumptionSession
//      Establisher (Platform::New, comme InteractionModelEngine::
//      ResumeSubscriptionsTimerCallback), dont ResumeSubscription() trouve la
//      session ouverte : OperationalSessionSetup::Connect s'y attache, et
//      HandleDeviceConnected cree le ReadHandler pendant l'appel, puis remet a
//      0 le compteur sauve et celui de la pile (firmware.elf desassemble).
//
//  La pile garde seule ses propres essais : chaque echec d'un de ses
//  etablisseurs incremente le compteur sauve (jusqu'a 11) ; le 12e echec, qui
//  le trouve a 11, fait oublier l'abonnement. Un abonne deja servi est saute
//  (subjectServed : abonnement actif ou demande en cours). Reste hors
//  d'atteinte l'identifiant d'un abonnement vivant (prive) : un abonne qui en
//  a un vivant et un autre sauve mort garde le mort a la pile.

enum : intptr_t { kResumeAuto = 0, kResumeManual = 1 };
enum : uint8_t { kRunLaunched, kRunNothing, kRunNoStorage, kRunNoIterator };
enum : uint8_t { kPeerResumed, kPeerServed, kPeerNothing, kPeerNoStorage, kPeerNoIterator, kPeerQueueFull };
static constexpr uint32_t kWatchLostMs = 180000;  // recherche 45 s + CASE : bien en deca

struct SavedSub {
  uint64_t node;
  uint32_t id, retries;
  uint16_t minS, maxS;
  uint8_t fabric;
};
static constexpr uint8_t kSavedMax = 6;

enum : uint8_t { kWatchIdle, kWatchOpening, kWatchResuming };
struct ResumeWatch {
  ResumeWatch() : conn(onConnected, this), fail(onFailed, this) {}
  chip::Callback::Callback<chip::OnDeviceConnected> conn;
  chip::Callback::Callback<chip::OnDeviceConnectionFailure> fail;
  uint64_t node = 0;
  uint8_t fabric = 0;
  uint8_t state = kWatchIdle;  // sous sSubMux ; node et fabric ne changent qu'a l'arret
  uint32_t startMs = 0;
  static void onConnected(void *ctx, chip::Messaging::ExchangeManager &, const chip::SessionHandle &);
  static void onFailed(void *ctx, const chip::ScopedNodeId &, CHIP_ERROR err);
};
static constexpr uint8_t kWatchMax = 2;
static ResumeWatch sWatch[kWatchMax];

static struct {
  // passages (resumeWork)
  uint32_t runs, autoRuns, opened;
  uint32_t runSeq, runAt, runSaved;
  uint8_t runKind, runVerdict, runPeers, runLaunched, runServed, runBusy;
  // fins de session (observateurs)
  uint32_t ok, failed, lost, late;
  uint32_t doneSeq, doneAt, doneMs, doneErr;
  uint64_t doneNode;
  // reprises, session ouverte (resumePeerWork)
  uint32_t resumed;
  uint32_t peerSeq, peerAt;
  uint64_t peerNode;
  uint8_t peerVerdict, peerResumed, peerUnsettled, peerFailed;
} sResume = {};
static bool sResumePosted = false;  // travail poste, pas encore execute ; sous sSubMux

// Tache CHIP : fin de la reprise d'un abonne, observateur rendu.
static void peerDone(ResumeWatch *w, uint8_t verdict, uint8_t resumed, uint8_t unsettled, uint8_t failed) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&sSubMux);
  sResume.resumed += resumed;
  sResume.peerSeq++;
  sResume.peerAt = now;
  sResume.peerNode = w->node;
  sResume.peerVerdict = verdict;
  sResume.peerResumed = resumed;
  sResume.peerUnsettled = unsettled;
  sResume.peerFailed = failed;
  w->state = kWatchIdle;
  sEventSeq++;
  portEXIT_CRITICAL(&sSubMux);
}

static void resumePeerWork(intptr_t arg);

// Tache CHIP (parfois pendant FindOrEstablishSession, dans resumeWork).
static void watchDone(ResumeWatch *w, CHIP_ERROR err) {
  const uint32_t now = millis();
  bool post = false;
  portENTER_CRITICAL(&sSubMux);
  if (w->state == kWatchOpening) {
    if (err == CHIP_NO_ERROR) sResume.ok++;
    else sResume.failed++;
    sResume.doneErr = err.AsInteger();
    sResume.doneMs = now - w->startMs;
    sResume.doneAt = now;
    sResume.doneNode = w->node;
    sResume.doneSeq++;
    sEventSeq++;
    post = err == CHIP_NO_ERROR;
    w->state = post ? kWatchResuming : kWatchIdle;
  } else {
    sResume.late++;  // fin arrivee apres l'abandon (kWatchLostMs)
  }
  portEXIT_CRITICAL(&sSubMux);
  if (post && chip::DeviceLayer::PlatformMgr().ScheduleWork(resumePeerWork, reinterpret_cast<intptr_t>(w)) !=
                  CHIP_NO_ERROR)
    peerDone(w, kPeerQueueFull, 0, 0, 0);
}

void ResumeWatch::onConnected(void *ctx, chip::Messaging::ExchangeManager &, const chip::SessionHandle &) {
  watchDone(static_cast<ResumeWatch *>(ctx), CHIP_NO_ERROR);
}

void ResumeWatch::onFailed(void *ctx, const chip::ScopedNodeId &, CHIP_ERROR err) {
  watchDone(static_cast<ResumeWatch *>(ctx), err);
}

// Tache loop : une tentative de ce pont est-elle en cours ? Un observateur
// muet depuis kWatchLostMs est abandonne. Ses rappels peuvent rester
// accroches a une session : un nouvel Enqueue les en decroche d'abord
// (GroupedCallbackList::Enqueue appelle Cancel()). Une reprise postee finit
// toujours (file de la pile). *launched : abonnes contactes par le dernier
// passage, lu dans le meme instant.
static bool resumeInFlight(uint32_t now, uint32_t *launched = nullptr) {
  bool busy;
  portENTER_CRITICAL(&sSubMux);
  busy = sResumePosted;
  if (launched) *launched = sResume.runLaunched;
  for (ResumeWatch &w : sWatch) {
    if (w.state == kWatchIdle) continue;
    // Age signe : un observateur arme par la tache CHIP apres notre millis()
    // (elle passe devant la tache loop) est tout jeune, pas perdu.
    if (w.state == kWatchOpening && (int32_t)(now - w.startMs) >= (int32_t)kWatchLostMs) {
      w.state = kWatchIdle;
      sResume.lost++;
    } else {
      busy = true;
    }
  }
  portEXIT_CRITICAL(&sSubMux);
  return busy;
}

// Sous le verrou de la pile. L'iterateur est rendu avant de sortir : il n'y en
// a que deux, partages avec la pile. -1 : aucun de libre.
static int collectSaved(SubscriptionResumptionStorage *st, SavedSub *out, uint8_t max, uint32_t &total) {
  total = 0;
  auto *it = st->IterateSubscriptions();
  if (!it) return -1;
  SubInfo info;
  uint8_t n = 0;
  while (it->Next(info)) {
    total++;
    if (n < max)
      out[n++] = {info.mNodeId, info.mSubscriptionId, info.mResumptionRetries, info.mMinInterval, info.mMaxInterval,
                  info.mFabricIndex};
  }
  it->Release();
  return n;
}

// Recharge un abonnement sauve complet (chemins compris), iterateur rendu :
// les rappels synchrones d'une reprise ecrivent dans ce stockage.
static bool loadSaved(SubscriptionResumptionStorage *st, const SavedSub &s, SubInfo &info) {
  auto *it = st->IterateSubscriptions();
  if (!it) return false;
  bool found = false;
  while (!found && it->Next(info))
    found = info.mNodeId == s.node && info.mFabricIndex == s.fabric && info.mSubscriptionId == s.id;
  it->Release();
  return found;
}

// 1. Ouvrir une session vers chaque abonne sauve sans abonnement.
static void resumeWork(intptr_t kind) {
  // Tache CHIP, verrou de la pile tenu.
  auto *im = chip::app::InteractionModelEngine::GetInstance();
  SubscriptionResumptionStorage *st = im->GetSubscriptionResumptionStorage();
  chip::CASESessionManager *mgr = im->GetCASESessionManager();
  const uint32_t t0 = millis();
  SavedSub saved[kSavedMax];
  uint32_t total = 0;
  uint8_t peers = 0, launched = 0, served = 0, busy = 0, verdict;
  int n = 0;
  if (!st || !mgr) {
    verdict = kRunNoStorage;
  } else if ((n = collectSaved(st, saved, kSavedMax, total)) < 0) {
    verdict = kRunNoIterator;
  } else {
    for (int i = 0; i < n; i++) {
      const SavedSub &s = saved[i];
      bool seen = false;  // un seul passage par abonne
      for (int k = 0; k < i; k++) seen |= saved[k].node == s.node && saved[k].fabric == s.fabric;
      if (seen) continue;
      peers++;
      if (subjectServed(im, s.fabric, s.node, t0)) {
        served++;
        continue;
      }
      ResumeWatch *w = nullptr;
      bool same = false;  // tentative d'avant pas finie pour cet abonne
      const uint32_t now = millis();
      portENTER_CRITICAL(&sSubMux);
      for (ResumeWatch &c : sWatch) {
        if (c.state != kWatchIdle) same |= c.node == s.node && c.fabric == s.fabric;
        else if (!w) w = &c;
      }
      if (!same && w) {
        w->node = s.node;
        w->fabric = s.fabric;
        w->startMs = now;
        w->state = kWatchOpening;
      }
      portEXIT_CRITICAL(&sSubMux);
      if (same || !w) {
        busy++;
        continue;
      }
      launched++;
      // Peut finir pendant l'appel (session deja ouverte, ou pas de place).
      mgr->FindOrEstablishSession(chip::ScopedNodeId(s.node, s.fabric), &w->conn, &w->fail);
    }
    verdict = launched ? kRunLaunched : kRunNothing;
  }
  const uint32_t now = millis();
  portENTER_CRITICAL(&sSubMux);
  sResume.runs++;
  if (kind == kResumeAuto) sResume.autoRuns++;
  sResume.opened += launched;
  sResume.runSeq++;
  sResume.runAt = now;
  sResume.runSaved = total;
  sResume.runKind = (uint8_t)kind;
  sResume.runVerdict = verdict;
  sResume.runPeers = peers;
  sResume.runLaunched = launched;
  sResume.runServed = served;
  sResume.runBusy = busy;
  sEventSeq++;
  sResumePosted = false;  // en dernier : les observateurs sont deja marques
  portEXIT_CRITICAL(&sSubMux);
}

// 2. Session ouverte vers un abonne : reprendre ses abonnements sauves.
static void resumePeerWork(intptr_t arg) {
  // Tache CHIP, verrou de la pile tenu. L'observateur est en kWatchResuming :
  // personne d'autre n'y touche jusqu'a peerDone().
  ResumeWatch *w = reinterpret_cast<ResumeWatch *>(arg);
  auto *im = chip::app::InteractionModelEngine::GetInstance();
  SubscriptionResumptionStorage *st = im->GetSubscriptionResumptionStorage();
  chip::CASESessionManager *mgr = im->GetCASESessionManager();
  SavedSub saved[kSavedMax];
  uint32_t total = 0;
  uint8_t resumed = 0, unsettled = 0, failed = 0, verdict;
  int n = 0;
  if (subjectServed(im, w->fabric, w->node, millis())) {
    verdict = kPeerServed;  // la pile (ou un abonnement neuf) est passee avant
  } else if (!st || !mgr) {
    verdict = kPeerNoStorage;
  } else if ((n = collectSaved(st, saved, kSavedMax, total)) < 0) {
    verdict = kPeerNoIterator;
  } else {
    for (int i = 0; i < n; i++) {
      const SavedSub &s = saved[i];
      if (s.node != w->node || s.fabric != w->fabric) continue;
      SubInfo info;
      if (!loadSaved(st, s, info)) {
        failed++;
        continue;
      }
      auto *est = chip::Platform::New<chip::app::SubscriptionResumptionSessionEstablisher>();
      if (!est) {
        failed++;
        break;
      }
      const uint32_t before = sBridgeEstablished;
      sResumingNow = true;
      const CHIP_ERROR err = est->ResumeSubscription(*mgr, info);
      sResumingNow = false;
      // Erreur possible seulement avant la session (copie des chemins) : rien
      // n'est accroche, l'etablisseur est a nous. Sinon il s'est deja libere.
      if (err != CHIP_NO_ERROR) {
        chip::Platform::Delete(est);
        failed++;
      } else if (sBridgeEstablished != before) {
        resumed++;
      } else {
        // Session fermee entre-temps (l'etablisseur en rouvre une, comme ceux
        // de la pile), ou pas de ReadHandler libre.
        unsettled++;
      }
    }
    verdict = resumed ? kPeerResumed : kPeerNothing;
  }
  peerDone(w, verdict, resumed, unsettled, failed);
}

// Tache loop. ScheduleWork se passe du verrou de la pile.
static bool postResume(intptr_t kind) {
  portENTER_CRITICAL(&sSubMux);
  const bool already = sResumePosted;
  sResumePosted = true;
  portEXIT_CRITICAL(&sSubMux);
  if (already) return false;
  if (chip::DeviceLayer::PlatformMgr().ScheduleWork(resumeWork, kind) == CHIP_NO_ERROR) return true;
  portENTER_CRITICAL(&sSubMux);
  sResumePosted = false;
  portEXIT_CRITICAL(&sSubMux);
  return false;
}

// --- Tache loop : comptage, calendrier, traces -------------------------------

static ResumePlanner sPlan;
static struct {
  bool known;
  uint32_t at, subs, reads;
} sCount = {};
static uint32_t sSeenRoles = 0, sSeenRunSeq = 0, sSeenDoneSeq = 0, sSeenPeerSeq = 0;
static uint32_t sSeenRequested = 0, sSeenEstablished = 0, sSeenTerminated = 0;

// Abonnements actifs, toutes les 2 s, sans attendre le verrou de la pile.
static bool countPoll(uint32_t now) {
  static constexpr uint32_t kCountMs = 2000;
  if (sCount.known && (uint32_t)(now - sCount.at) < kCountMs) return false;
  if (!chip::DeviceLayer::PlatformMgr().TryLockChipStack()) return false;
  auto *im = chip::app::InteractionModelEngine::GetInstance();
  const uint32_t subs = im->GetNumActiveReadHandlers(chip::app::ReadHandler::InteractionType::Subscribe);
  const uint32_t all = im->GetNumActiveReadHandlers();
  chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  sCount.known = true;
  sCount.at = now;
  sCount.subs = subs;
  sCount.reads = all > subs ? all - subs : 0;
  return true;
}

static void nodeText(char *b, size_t n, uint64_t id) {
  snprintf(b, n, "0x%08lX%08lX", (unsigned long)(id >> 32), (unsigned long)(id & 0xFFFFFFFFu));
}

static const char *const kEstText[] = {"neuf", "repris par le pont", "repris par la pile"};

// Evenements rares (quelques-uns par demarrage) : toujours traces. Seule
// exception : le coup d'oeil de 5 min qui ne trouve rien a faire, trace une
// fois tant que son resultat ne change pas.
static void tracePoll() {
  static uint32_t seen = 0;
  static uint32_t quiet = 0;  // signature du dernier passage auto sans effet trace (0 : aucun)
  if (__atomic_load_n(&sEventSeq, __ATOMIC_RELAXED) == seen) return;
  RoleChange roles[kRoleHistory];
  uint32_t nRoles;
  decltype(sSubs) subs;
  decltype(sResume) res;
  portENTER_CRITICAL(&sSubMux);
  seen = sEventSeq;
  nRoles = sRoleChanges;
  memcpy(roles, sRoles, sizeof(roles));
  subs = sSubs;
  res = sResume;
  portEXIT_CRITICAL(&sSubMux);
  char a[16], b[16], node[24];

  if (nRoles != sSeenRoles) {
    const uint32_t from = nRoles - sSeenRoles > kRoleHistory ? nRoles - kRoleHistory : sSeenRoles;
    for (uint32_t i = from; i < nRoles; i++) {
      const RoleChange &c = roles[i % kRoleHistory];
      bridgeLog("[matter] Thread : %s -> %s a +%s s", roleName(c.from), roleName(c.to), secs(a, sizeof(a), c.ms));
    }
    sSeenRoles = nRoles;
  }
  if (subs.requested != sSeenRequested) {
    sSeenRequested = subs.requested;
    nodeText(node, sizeof(node), subs.reqPeer);
    bridgeLog("[matter] abonnement demande par %s a +%s s : plancher %u s, max %u s -> %u s", node,
              secs(a, sizeof(a), subs.reqAt), subs.reqMin, subs.reqMax, subs.reqApplied);
  }
  if (subs.fresh + subs.byBridge + subs.byStack != sSeenEstablished) {
    sSeenEstablished = subs.fresh + subs.byBridge + subs.byStack;
    bridgeLog("[matter] abonnement etabli (%s) a +%s s : min %u s, max %u s", kEstText[subs.lastKind],
              secs(a, sizeof(a), subs.lastAt), subs.lastMin, subs.lastMax);
  }
  if (subs.terminated != sSeenTerminated) {
    sSeenTerminated = subs.terminated;
    bridgeLog("[matter] abonnement termine (%lu en tout)", (unsigned long)subs.terminated);
  }
  if (res.runSeq != sSeenRunSeq) {
    sSeenRunSeq = res.runSeq;
    const char *kind = res.runKind == kResumeManual ? "manuelle" : "auto";
    const uint32_t sig = 1u + (res.runSaved & 0xFF) + ((uint32_t)res.runPeers << 8) +
                         ((uint32_t)res.runServed << 16) + ((uint32_t)res.runBusy << 24);
    switch (res.runVerdict) {
      case kRunLaunched:
        bridgeLog("[matter] reprise %s a +%s s : session demandee vers %u abonne(s) sur %u (%lu abonnement(s) "
                  "sauve(s)), %u deja servi(s), %u deja en cours",
                  kind, secs(a, sizeof(a), res.runAt), res.runLaunched, res.runPeers, (unsigned long)res.runSaved,
                  res.runServed, res.runBusy);
        quiet = 0;
        break;
      case kRunNothing:
        if (res.runKind == kResumeAuto && sig == quiet) break;
        bridgeLog("[matter] reprise %s : rien a relancer (%lu sauve(s), %u abonne(s) : %u deja servi(s), %u deja en "
                  "cours)",
                  kind, (unsigned long)res.runSaved, res.runPeers, res.runServed, res.runBusy);
        quiet = res.runKind == kResumeAuto ? sig : 0;
        break;
      case kRunNoStorage:
        bridgeLog("[matter] reprise %s : pas de stockage d'abonnements", kind);
        quiet = 0;
        break;
      default:
        bridgeLog("[matter] reprise %s : iterateur du stockage occupe, a refaire", kind);
        quiet = 0;
        break;
    }
  }
  if (res.doneSeq != sSeenDoneSeq) {
    sSeenDoneSeq = res.doneSeq;
    nodeText(node, sizeof(node), res.doneNode);
    if (res.doneErr == CHIP_NO_ERROR.AsInteger())
      bridgeLog("[matter] reprise : session CASE avec %s ouverte en %s s", node, secs(b, sizeof(b), res.doneMs));
    else
      bridgeLog("[matter] reprise : echec 0x%lX avec %s apres %s s%s (sans frais : compteurs de la pile intacts)",
                (unsigned long)res.doneErr, node, secs(b, sizeof(b), res.doneMs),
                res.doneErr == CHIP_ERROR_TIMEOUT.AsInteger() ? " (delai : adresse introuvable ou CASE muet)" : "");
  }
  if (res.peerSeq != sSeenPeerSeq) {
    sSeenPeerSeq = res.peerSeq;
    nodeText(node, sizeof(node), res.peerNode);
    switch (res.peerVerdict) {
      case kPeerResumed:
      case kPeerNothing:
        bridgeLog("[matter] reprise : %u abonnement(s) de %s repris, %u sans ReadHandler immediat, %u rate(s)",
                  res.peerResumed, node, res.peerUnsettled, res.peerFailed);
        break;
      case kPeerServed:
        bridgeLog("[matter] reprise : %s deja servi a l'ouverture de la session (pile ou abonnement neuf)", node);
        break;
      case kPeerNoStorage: bridgeLog("[matter] reprise : pas de stockage d'abonnements pour %s", node); break;
      case kPeerNoIterator: bridgeLog("[matter] reprise : iterateur du stockage occupe pour %s, a refaire", node); break;
      default: bridgeLog("[matter] reprise : file de la pile pleine, %s pas repris", node); break;
    }
  }
}

static void threadPoll(uint32_t now) {
  static uint32_t last = 0;
  if (!sMatterStarted || (uint32_t)(now - last) < 50) return;  // rien de presse : 20 fois par seconde
  last = now;
  netPoll(now);
  const bool counted = countPoll(now);
  tracePoll();
  sPlan.network(netReady(), now);
  if (counted) sPlan.subscriptions(sCount.subs, now);
  uint32_t launched = 0;
  const bool inFlight = resumeInFlight(now, &launched);
  if (sPlan.waiting && !inFlight) sPlan.finished(launched, now);
  if (sResumeAuto && !inFlight && sPlan.due(now, sBootMs) && Matter.isDeviceCommissioned() &&
      postResume(kResumeAuto))
    sPlan.fired();
}

bool matterResumeNow(Print &out) {
  if (!sMatterStarted) {
    out.println("Pile Thread absente : rien a reprendre.");
    return false;
  }
  if (!Matter.isDeviceCommissioned()) {
    out.println("Noeud pas mis en service : aucun abonnement.");
    return false;
  }
  const uint32_t now = millis();
  if (resumeInFlight(now)) {
    out.println("Une tentative de reprise est deja en cours : attendre sa fin ('matter').");
    return false;
  }
  if (!postResume(kResumeManual)) {
    out.println("!! Travail refuse par la pile (file pleine ?) : reessayer.");
    return false;
  }
  out.println("Reprise demandee : resultat dans les traces '[matter] reprise'.");
  if (!netReady())
    out.printf("  (reseau pas pret : role %s, hote SRP %s : echec probable)\n", roleName(sNet.role),
               sNet.srpHost == 0xFF ? "?" : otSrpClientItemStateToString((otSrpClientItemState)sNet.srpHost));
  if ((uint32_t)(now - sBootMs) < sPlan.startDelayMs)
    out.printf("  (moins de %lu s apres le demarrage : la tentative de la pile peut encore chercher l'adresse, "
               "et celle-ci la rejoindre)\n",
               (unsigned long)(sPlan.startDelayMs / 1000));
  return true;
}

// Etat pour 'matter' : releve sous chaque verrou tour a tour, puis affiche.
static void threadStatus(Print &out) {
  char a[16], b[16], c[16];

  // Verrou OpenThread (borne), rien d'autre dessous.
  struct {
    bool ok;
    otLinkModeConfig mode;
    otMleCounters mle;
    bool srpRunning;
    uint8_t host, svcTotal, svcReg;
    char srpServer[OT_IP6_ADDRESS_STRING_SIZE];
    uint16_t srpPort;
    char dns[OT_IP6_ADDRESS_STRING_SIZE];
    uint16_t dnsPort;
  } ot = {};
  otInstance *inst = esp_openthread_get_instance();
  if (inst && otLockTry(50)) {
    ot.ok = true;
    ot.mode = otThreadGetLinkMode(inst);
    const otMleCounters *m = otThreadGetMleCounters(inst);
    if (m) ot.mle = *m;
    ot.srpRunning = otSrpClientIsRunning(inst);
    const otSrpClientHostInfo *h = otSrpClientGetHostInfo(inst);
    ot.host = h ? (uint8_t)h->mState : 0xFF;
    for (const otSrpClientService *s = otSrpClientGetServices(inst); s && ot.svcTotal < 255; s = s->mNext) {
      ot.svcTotal++;
      if (srpRegistered((uint8_t)s->mState)) ot.svcReg++;
    }
    const otSockAddr *srv = otSrpClientGetServerAddress(inst);
    if (srv) {
      otIp6AddressToString(&srv->mAddress, ot.srpServer, sizeof(ot.srpServer));
      ot.srpPort = srv->mPort;
    }
    const otDnsQueryConfig *dc = otDnsClientGetDefaultConfig(inst);
    if (dc) {
      otIp6AddressToString(&dc->mServerSockAddr.mAddress, ot.dns, sizeof(ot.dns));
      ot.dnsPort = dc->mServerSockAddr.mPort;
    }
    esp_openthread_lock_release();
  }

  // Verrou de la pile (borne), rien d'autre dessous.
  bool chipOk = false;
  uint32_t subs = 0, reads = 0, total = 0;
  int nSaved = 0;
  SavedSub saved[kSavedMax];
  bool servedSaved[kSavedMax] = {};
  for (uint32_t t0 = millis(); !chipOk && (uint32_t)(millis() - t0) < 50;) {
    chipOk = chip::DeviceLayer::PlatformMgr().TryLockChipStack();
    if (!chipOk) delay(1);
  }
  if (chipOk) {
    auto *im = chip::app::InteractionModelEngine::GetInstance();
    subs = im->GetNumActiveReadHandlers(chip::app::ReadHandler::InteractionType::Subscribe);
    const uint32_t all = im->GetNumActiveReadHandlers();
    reads = all > subs ? all - subs : 0;
    SubscriptionResumptionStorage *st = im->GetSubscriptionResumptionStorage();
    nSaved = st ? collectSaved(st, saved, kSavedMax, total) : -2;
    for (int i = 0; i < nSaved; i++) servedSaved[i] = subjectServed(im, saved[i].fabric, saved[i].node, millis());
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  }

  // Lu apres les verrous : un observateur arme entre-temps n'a pas d'age negatif.
  const uint32_t now = millis();
  RoleChange roles[kRoleHistory];
  uint32_t nRoles;
  decltype(sSubs) sb;
  decltype(sResume) rs;
  uint32_t watchAge = 0;
  uint64_t watchNode = 0;
  uint8_t opening = 0, resuming = 0;
  portENTER_CRITICAL(&sSubMux);
  nRoles = sRoleChanges;
  memcpy(roles, sRoles, sizeof(roles));
  sb = sSubs;
  rs = sResume;
  for (const ResumeWatch &w : sWatch) {
    if (w.state == kWatchResuming) resuming++;
    if (w.state != kWatchOpening) continue;
    opening++;
    const int32_t age = (int32_t)(now - w.startMs);
    watchAge = age > 0 ? (uint32_t)age : 0;
    watchNode = w.node;
  }
  portEXIT_CRITICAL(&sSubMux);

  out.printf("  demarrage       : il y a %s s ; Matter pret a +%s s ; reseau pret (attache + SRP) ",
             secs(a, sizeof(a), now), secs(b, sizeof(b), sBootMs));
  if (sNet.readyAt) out.printf("a +%s s\n", secs(c, sizeof(c), sNet.readyAt));
  else out.println("jamais");

  static const char *const kMedText[kMatterMedModes] = {"routeur", "MED des l'init", "MED apres Matter.begin()"};
  char mode[4] = "?";
  if (sModeKnown) linkModeText(sModeAtBegin, mode);
  out.printf("  type Thread     : %s ce demarrage (mode %s apres Matter.begin()) ; esp_matter : %u reglage(s), %u "
             "routeur -> MED ; prochain : %s ('matter med 0|1|2')%s\n",
             kMedText[sMedBoot], mode, sDevTypeCallsEsp, sDevTypeSwapsEsp, kMedText[sMedMode],
             HALO_WRAP_THREAD_DEVTYPE ? "" : " ; enveloppe ABSENTE : 1 = 2");

  if (!sRoleHooked) {
    out.println("  roles Thread    : ecoute des evenements indisponible");
  } else {
    out.printf("  roles Thread    : %lu changement(s)", (unsigned long)nRoles);
    const uint32_t from = nRoles > kRoleHistory ? nRoles - kRoleHistory : 0;
    for (uint32_t i = from; i < nRoles; i++) {
      const RoleChange &r = roles[i % kRoleHistory];
      out.printf("%s+%s %s>%s", i == from ? " : " : ", ", secs(a, sizeof(a), r.ms), roleName(r.from), roleName(r.to));
    }
    out.println();
  }

  if (ot.ok) {
    linkModeText(ot.mode, mode);
    out.printf("  MLE             : mode %s ; %u attache(s) tentee(s), detache %ux, enfant %ux, routeur %ux, chef "
               "%ux, parent change %ux\n",
               mode, ot.mle.mAttachAttempts, ot.mle.mDetachedRole, ot.mle.mChildRole, ot.mle.mRouterRole,
               ot.mle.mLeaderRole, ot.mle.mParentChanges);
    out.printf("  SRP             : client %s, serveur [%s]:%u, hote %s, services %u/%u enregistre(s) ; DNS [%s]:%u\n",
               ot.srpRunning ? "actif" : "ARRETE", ot.srpServer[0] ? ot.srpServer : "-", ot.srpPort,
               ot.host == 0xFF ? "?" : otSrpClientItemStateToString((otSrpClientItemState)ot.host), ot.svcReg,
               ot.svcTotal, ot.dns[0] ? ot.dns : "-", ot.dnsPort);
  } else {
    out.println("  MLE, SRP        : verrou OpenThread occupe, reessayer");
  }

  if (chipOk) {
    out.printf("  abonnements     : %lu actif(s), %lu lecture(s) en cours ; ", (unsigned long)subs,
               (unsigned long)reads);
    if (nSaved == -2) out.println("pas de stockage");
    else if (nSaved < 0) out.println("sauves : iterateur occupe");
    else out.printf("%lu sauve(s)%s\n", (unsigned long)total, total ? " :" : "");
    for (int i = 0; i < nSaved; i++) {
      char node[24];
      nodeText(node, sizeof(node), saved[i].node);
      out.printf("                    abonne %s (fabrique %u, %s), id 0x%08lX, %lu echec(s) de reprise de la pile "
                 "(oubli au 12e), min %u s, max %u s\n",
                 node, saved[i].fabric, servedSaved[i] ? "servi" : "SANS abonnement actif", (unsigned long)saved[i].id,
                 (unsigned long)saved[i].retries, saved[i].minS, saved[i].maxS);
    }
  } else {
    out.println("  abonnements     : pile occupee, reessayer");
  }

  out.printf("  abonnes (IM)    : %lu demande(s) ; etablis : %lu neuf(s), %lu repris par le pont, %lu repris par la "
             "pile ; %lu termine(s) ; premier etabli ",
             (unsigned long)sb.requested, (unsigned long)sb.fresh, (unsigned long)sb.byBridge,
             (unsigned long)sb.byStack, (unsigned long)sb.terminated);
  if (sb.firstAt) out.printf("a +%s s (%s)\n", secs(a, sizeof(a), sb.firstAt), kEstText[sb.lastKind]);
  else out.println(": aucun depuis le demarrage");
  if (sb.requested) {
    char node[24];
    nodeText(node, sizeof(node), sb.reqPeer);
    out.printf("                    derniere demande a +%s s par %s : plancher %u s, max %u s -> %u s\n",
               secs(a, sizeof(a), sb.reqAt), node, sb.reqMin, sb.reqMax, sb.reqApplied);
  }

  out.printf("  reprise         : auto %s ('matter reprise [auto 0|1]') ; %lu passage(s) dont %lu auto ; %lu "
             "session(s) demandee(s) : %lu ouverte(s), %lu echec(s), %lu sans nouvelles ; %lu abonnement(s) repris\n",
             sResumeAuto ? "oui" : "non", (unsigned long)rs.runs, (unsigned long)rs.autoRuns,
             (unsigned long)rs.opened, (unsigned long)rs.ok, (unsigned long)rs.failed, (unsigned long)rs.lost,
             (unsigned long)rs.resumed);
  if (rs.doneSeq) {
    out.printf("                    derniere session a +%s s apres %s s : ", secs(a, sizeof(a), rs.doneAt),
               secs(b, sizeof(b), rs.doneMs));
    if (rs.doneErr == CHIP_NO_ERROR.AsInteger()) out.println("ouverte");
    else out.printf("erreur 0x%lX\n", (unsigned long)rs.doneErr);
  }
  if (rs.peerSeq) {
    static const char *const kPeerText[] = {"repris",           "deja servi",         "rien repris",
                                            "pas de stockage", "iterateur occupe", "file pleine"};
    out.printf("                    derniere reprise a +%s s : %s (%u repris, %u sans ReadHandler, %u rate(s))\n",
               secs(a, sizeof(a), rs.peerAt), kPeerText[rs.peerVerdict], rs.peerResumed, rs.peerUnsettled,
               rs.peerFailed);
  }
  if (opening) {
    char node[24];
    nodeText(node, sizeof(node), watchNode);
    out.printf("                    session en cours vers %s depuis %s s\n", node, secs(a, sizeof(a), watchAge));
  } else if (resuming) {
    out.println("                    session ouverte, reprise postee");
  } else if (sResumeAuto && sPlan.waiting) {
    out.println("                    tentative auto postee");
  } else if (sResumeAuto) {
    const uint32_t left = sPlan.holdLeftMs(now);
    if (left && sPlan.subs)
      out.printf("                    prochain coup d'oeil auto dans %s s (abonnement actif)\n",
                 secs(a, sizeof(a), left));
    else if (left)
      out.printf("                    prochaine relance auto dans %s s (essai %u de l'episode)\n",
                 secs(a, sizeof(a), left), sPlan.tries + 1);
  }

  if (sMaxIntCap)
    out.printf("  intervalle max  : plafonne a %u s pour les abonnements neufs ('matter maxint', 0 = celui "
               "d'Apple), %lu applique(s) depuis le demarrage\n",
               sMaxIntCap, (unsigned long)sb.capped);
  else
    out.println("  intervalle max  : celui du controleur ('matter maxint <10..3600>' pour plafonner)");
}
#endif  // MATTER_NET_THREAD

// ===========================================================================
//  Identite du noeud (Basic Information, EP0)
//
//  Valeurs de config.h (decisions de Majid du 23/09). La bibliotheque les
//  garde et ne les publie qu'a Matter.begin() (fournisseur DeviceInstanceInfo
//  enveloppe, NodeLabel ecrit, attribut SerialNumber cree) : a reposer a CHAQUE
//  demarrage, avant begin() ; l'ordre par rapport aux begin() d'endpoints est
//  indifferent. Jamais setSetupDiscriminator ni setSetupPasscode, et ni VID ni
//  PID : le noeud est appaire dans Apple Home avec le certificat de test.
//  La version logicielle ("Programme interne") vient du descripteur
//  d'application (src/app_desc.c), pas d'ici.
// ===========================================================================

// Bornes de la bibliotheque (MatterIdentity.h) : au-dela, le setter refuse.
static constexpr size_t kIdMax = 32, kIdHwStringMax = 64;
static_assert(sizeof(MATTER_VENDOR_NAME) - 1 <= kIdMax, "MATTER_VENDOR_NAME : 32 caracteres au plus");
static_assert(sizeof(MATTER_PRODUCT_NAME) - 1 <= kIdMax, "MATTER_PRODUCT_NAME : 32 caracteres au plus");
static_assert(sizeof(MATTER_NODE_LABEL) - 1 <= kIdMax, "MATTER_NODE_LABEL : 32 caracteres au plus");
static_assert(sizeof(MATTER_HW_VERSION_STRING) - 1 <= kIdHwStringMax, "MATTER_HW_VERSION_STRING : 64 au plus");
static_assert(MATTER_HW_VERSION >= 0 && MATTER_HW_VERSION <= 0xFFFF, "MATTER_HW_VERSION : entier de 16 bits");

// Prefixe + adresse MAC d'usine (eFuse) en 12 chiffres hexa : unique par carte.
static char sSerial[sizeof(MATTER_SERIAL_PREFIX) + 12] = {};
static_assert(sizeof(sSerial) - 1 <= kIdMax, "MATTER_SERIAL_PREFIX trop long : numero de serie de 32 au plus");

enum : uint8_t { kIdVendor, kIdProduct, kIdHw, kIdHwString, kIdSerial, kIdNodeLabel, kIdCount };
static const char *const kIdText[kIdCount] = {"fabricant",       "produit",         "version materielle",
                                              "materiel (texte)", "numero de serie", "nom du noeud"};
static uint8_t sIdRefused = 0;  // un bit par valeur refusee (kIdVendor...)

static void applyIdentity() {
  uint8_t mac[6] = {};
  const bool macOk = esp_efuse_mac_get_default(mac) == ESP_OK;
  if (macOk)
    snprintf(sSerial, sizeof(sSerial), "%s%02X%02X%02X%02X%02X%02X", MATTER_SERIAL_PREFIX, mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
  // Evaluees dans l'ordre (liste d'initialisation), toutes meme apres un refus.
  const bool ok[kIdCount] = {
      Matter.setVendorName(MATTER_VENDOR_NAME),
      Matter.setProductName(MATTER_PRODUCT_NAME),
      Matter.setHardwareVersion(MATTER_HW_VERSION),
      Matter.setHardwareVersionString(MATTER_HW_VERSION_STRING),
      macOk && Matter.setSerialNumber(sSerial),  // MAC illisible : numero d'usine de la pile
      Matter.setDeviceName(MATTER_NODE_LABEL),
  };
  for (uint8_t i = 0; i < kIdCount; i++)
    if (!ok[i]) {
      sIdRefused |= (uint8_t)(1u << i);
      Serial.printf("!! identite Matter : %s refuse, valeur d'usine de la pile gardee\n", kIdText[i]);
    }
}

// Ce que la pile rapporte aux controleurs, relu apres Matter.begin() : preuve
// que l'enveloppe est en place. Verrou de la pile borne a 50 ms, comme
// threadStatus ; sans lui, les valeurs demandees sont affichees a la place.
// Le nom du noeud (NodeLabel) est toujours la valeur demandee.
static void printIdentity(Print &out) {
  char vendor[kIdMax + 1], product[kIdMax + 1], serial[kIdMax + 1], hwString[kIdHwStringMax + 1],
      sw[chip::DeviceLayer::ConfigurationManager::kMaxSoftwareVersionStringLength + 1];
  uint16_t hw = MATTER_HW_VERSION;
  auto get = [](CHIP_ERROR e, char *b) {
    if (e != CHIP_NO_ERROR) strcpy(b, "?");  // "?" tient dans tous les tampons
  };
  bool locked = false, fromStack = false;
  for (uint32_t t0 = millis(); !locked && (uint32_t)(millis() - t0) < 50;) {
    locked = chip::DeviceLayer::PlatformMgr().TryLockChipStack();
    if (!locked) delay(1);
  }
  if (locked) {
    chip::DeviceLayer::DeviceInstanceInfoProvider *p = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
    if (p) {
      get(p->GetVendorName(vendor, sizeof(vendor)), vendor);
      get(p->GetProductName(product, sizeof(product)), product);
      get(p->GetSerialNumber(serial, sizeof(serial)), serial);
      get(p->GetHardwareVersionString(hwString, sizeof(hwString)), hwString);
      if (p->GetHardwareVersion(hw) != CHIP_NO_ERROR) hw = 0;
      fromStack = true;
    }
    get(chip::DeviceLayer::ConfigurationMgr().GetSoftwareVersionString(sw, sizeof(sw)), sw);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  } else {
    snprintf(sw, sizeof(sw), "%s", esp_app_get_description()->version);
  }
  if (!fromStack) {
    snprintf(vendor, sizeof(vendor), "%s", MATTER_VENDOR_NAME);
    snprintf(product, sizeof(product), "%s", MATTER_PRODUCT_NAME);
    snprintf(serial, sizeof(serial), "%s", sSerial[0] ? sSerial : "?");
    snprintf(hwString, sizeof(hwString), "%s", MATTER_HW_VERSION_STRING);
  }
  out.printf("  identite        : %s, %s, n/s %s, nom \"%s\"%s\n", vendor, product, serial, MATTER_NODE_LABEL,
             fromStack ? "" : locked ? " (demandees : fournisseur absent)" : " (demandees : pile occupee)");
  out.printf("  versions        : programme %s, materiel %u (%s)\n", sw, hw, hwString);
  if (strcmp(esp_app_get_description()->version, FW_VERSION_FULL))
    out.printf("  !! descripteur  : %s au lieu de %s (src/app_desc.c pas lie ?)\n",
               esp_app_get_description()->version, FW_VERSION_FULL);
  if (sIdRefused) {
    out.print("  !! refuse       :");
    for (uint8_t i = 0; i < kIdCount; i++)
      if (sIdRefused & (1u << i)) out.printf(" %s", kIdText[i]);
    out.println(" (valeurs d'usine de la pile a la place)");
  }
}

// ===========================================================================
//  Cycle de vie
// ===========================================================================

void matterBridgeBegin() {
  // La table gamma est deja construite par lamp.begin().
  sLoopTask = xTaskGetCurrentTaskHandle();
  const halo1::State t = lamp.target();
#if HALO1_EXPOSE_AUTO
  loadAutoPulse();
  sSeenRemoteAuto = lamp.remoteAutoCount();
#endif

#if MATTER_NET_THREAD
  // Avant Matter.begin() : le type Thread est applique pendant esp_matter::start
  // (enveloppe), et les roles du demarrage doivent etre dans l'historique.
  loadThreadSettings();
  hookRoleChanges();
  // Avant le premier begin() d'accessoire : c'est lui qui cree le noeud, et le
  // core refuse ensuite de changer de reseau. BLE garde pour l'appairage.
  if (!Matter.selectNetwork(MATTER_NETWORK_THREAD))
    Serial.println("!! selectNetwork(THREAD) refuse : le noeud resterait en Wi-Fi");
#endif
  // levelFromRaw : niveau rapporte, jamais sous le plancher (Apple Home).
  mainLight.begin(t.power, halo1::levelFromRaw(t.bright), halo1::miredFromTemp(t.temp));
  // La bibliotheque ne renseigne pas la plage physique de temperature : sans
  // elle, les applications affichent un curseur bien plus large que la lampe.
  esp_matter_attr_val_t val = esp_matter_uint16(halo1::kMiredCold);
  mainLight.setAttributeVal(ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, &val);
  val = esp_matter_uint16(halo1::kMiredWarm);
  mainLight.setAttributeVal(ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, &val);
  frontLamp.begin(t.power && (t.lamps & halo1::F_FRONT));
  backLamp.begin(t.power && (t.lamps & halo1::F_BACK));
#if HALO1_EXPOSE_AUTO
  autoButton.begin(false);
#endif

  mainLight.onChangeOnOff(onMainOnOff);
  mainLight.onChangeBrightness(onMainLevel);
  mainLight.onChangeColorTemperature(onMainMired);
  frontLamp.onChangeOnOff(onFront);
  backLamp.onChangeOnOff(onBack);
#if HALO1_EXPOSE_AUTO
  autoButton.onChangeOnOff(onAuto);
#endif
  // Tous les endpoints : on ne sait pas lequel le controleur vise.
  mainLight.onIdentify([](bool on) { return onIdentify(mainLight, 0, on); });
  frontLamp.onIdentify([](bool on) { return onIdentify(frontLamp, 1, on); });
  backLamp.onIdentify([](bool on) { return onIdentify(backLamp, 2, on); });
#if HALO1_EXPOSE_AUTO
  autoButton.onIdentify([](bool on) { return onIdentify(autoButton, 3, on); });
#endif

  applyIdentity();  // avant Matter.begin(), a chaque demarrage
  Matter.begin();
#if MATTER_NET_THREAD
  // Matter.begin() ne rend rien : un echec d'esp_matter::start ne fait qu'un
  // log. L'instance OpenThread n'existe qu'apres esp_openthread_init, qui cree
  // le verrou (esp_matter::start l'initialise avant de rendre la main).
  sMatterStarted = chip::DeviceLayer::ThreadStackMgrImpl().OTInstance() != nullptr;
  if (!sMatterStarted) {
    Serial.println("!! pile Thread absente : ni garde d'antenne, ni etat Thread");
  } else {
    // Mode en place a la sortie de Matter.begin() : MED ('rn') si l'enveloppe a
    // agi, FTD ('rdn') sinon. Lu sous le verrou OT, relache avant celui de la pile.
    if (otLockTry(100)) {
      sModeAtBegin = otThreadGetLinkMode(esp_openthread_get_instance());
      sModeKnown = true;
      esp_openthread_lock_release();
    }
    sDevTypeCallsEsp = sDevTypeCalls;
    sDevTypeSwapsEsp = sDevTypeSwaps;
    // Mode 2 (ancien) : esp_matter a mis Thread en routeur, on l'ecrase, et
    // OpenThread relance l'attache. Mode 1 : filet si l'enveloppe manque au
    // lien, ou si le mode lu n'est pas MED ('rn') ; sinon rien a refaire (et
    // pas deux verrous pris sans limite pour rien).
    const bool medInPlace =
        HALO_WRAP_THREAD_DEVTYPE && sModeKnown && !sModeAtBegin.mDeviceType && sModeAtBegin.mRxOnWhenIdle;
    if (sMedBoot == kMedLate || (sMedBoot == kMedEarly && !medInPlace)) {
      chip::DeviceLayer::PlatformMgr().LockChipStack();
      chip::DeviceLayer::ConnectivityMgr().SetThreadDeviceType(
          chip::DeviceLayer::ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
      chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    }
    // Suivi des abonnements : un seul rappel applicatif par pile, libre (nm).
    // Plus grand plancher sauve : la pile y tente sa reprise (ResumePlanner).
    SavedSub saved[kSavedMax];
    uint32_t total = 0;
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto *im = chip::app::InteractionModelEngine::GetInstance();
    im->RegisterReadHandlerAppCallback(&sSubWatch);
    SubscriptionResumptionStorage *st = im->GetSubscriptionResumptionStorage();
    const int nSaved = st ? collectSaved(st, saved, kSavedMax, total) : 0;
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    uint16_t maxMin = 0;
    for (int i = 0; i < nSaved; i++)
      if (saved[i].minS > maxMin) maxMin = saved[i].minS;
    sPlan.startDelay(maxMin);
    // Pile OpenThread demarree : son verrou existe. Active par defaut ('lampe garde').
    lamp.radio.setAirGuard(&kAirGuard);
  }
#endif
  // Matter.begin() attend la fin de l'init de la pile, dont les ecritures de
  // demarrage passent par les callbacks : le delai du garde-fou part d'ici.
  sBootMs = millis();
  // Premier reflet force : la pile a pu restaurer d'autres valeurs depuis la NVS.
  sForceReflect = true;
}

void matterBridgePoll() {
  const uint32_t now = millis();
#if MATTER_NET_THREAD
  threadPoll(now);  // mesures, traces et relance des abonnements
#endif
  halo1::MatterIntents in;
  uint32_t first = 0;
  bool ready = false;
  portENTER_CRITICAL(&sInboxMux);
  // Differences signees : un ordre arrive entre millis() et le verrou a un
  // horodatage posterieur a 'now'.
  if (sInbox.has && ((int32_t)(now - sInLast) >= (int32_t)HALO1_COALESCE_QUIET_MS ||
                     (int32_t)(now - sInFirst) >= (int32_t)HALO1_COALESCE_MAX_MS)) {
    in = sInbox;
    first = sInFirst;
    sInbox = halo1::MatterIntents();
    ready = true;
  }
  const bool pending = sInbox.has != 0;
  portEXIT_CRITICAL(&sInboxMux);

  if (ready) applyIntents(in, first, now);
#if HALO1_EXPOSE_AUTO
  // A de la telecommande entendu par le pilote : meme impulsion d'EP4 qu'apres
  // un appui dans l'app, pour que l'app (et ses automatisations) le voie. Rien
  // n'est emis vers la lampe, et ce n'est pas un ordre Matter : aucune fenetre
  // ni pressAuto(), et notre ecriture d'EP4 est un echo (ownEcho).
  const uint32_t heard = lamp.remoteAutoCount();
  if (heard != sSeenRemoteAuto) {
    sSeenRemoteAuto = heard;
    sAutoPulse = sAutoRaise = true;
    sAutoPulseAt = now;  // debut de l'attente de la montee ; l'impulsion part de celle-ci
    sForceReflect = true;
    sStats.autoHeard++;
    if (lamp.tracing()) bridgeLog("[matter] A de la telecommande -> impulsion EP4 (%u ms)", sAutoPulseMs);
  }
#endif
  // Jamais de reflet tant que la boite contient des intentions : un curseur en
  // cours ne revient pas en arriere.
  if (pending) return;
  // Boite vide apres le delai et la fenetre la plus longue : toute fenetre
  // ouverte pendant le delai est refermee. Le garde-fou se desarme pour de bon,
  // et le retour a zero de millis() (49,7 jours) ne peut pas le rouvrir.
  if (sBootGuard && (uint32_t)(now - sBootMs) >= HALO1_BOOT_IGNORE_MS + HALO1_COALESCE_MAX_MS)
    sBootGuard = false;
  bool pulseOver = false;  // sans EP4, jamais d'impulsion
#if HALO1_EXPOSE_AUTO
  // Montee pas encore ecrite : l'impulsion n'a pas commence, seule l'attente
  // est bornee (un EP4 casse ne garde pas l'impulsion pour toujours).
  const uint32_t pulseAge = now - sAutoPulseAt;
  pulseOver = sAutoPulse && pulseAge >= (sAutoRaise ? kAutoRaiseMaxWaitMs : (uint32_t)sAutoPulseMs);
  if (pulseOver) {
    sAutoPulse = false;
    if (sAutoRaise) {  // EP4 jamais mis a on apres la fin de l'impulsion
      sAutoRaise = false;
      sStats.autoLost++;
      if (lamp.tracing()) bridgeLog("[matter] A de la telecommande non reflete : EP4 pas ecrit en %lu ms",
                                    (unsigned long)kAutoRaiseMaxWaitMs);
    }
  }
#endif
  const bool changed =
      lamp.version() != sSeenVersion && (uint32_t)(now - sLastReflect) >= HALO1_REFLECT_MIN_MS;
  if (!sForceReflect && !pulseOver && !changed) return;
  if (!reflect(now)) sForceReflect = true;  // rien de fait : nouvel essai au passage suivant
}

bool matterIsCommissioned() { return Matter.isDeviceCommissioned(); }
#if MATTER_NET_THREAD
// Releve partage avec la relance des abonnements (netPoll : verrou OT sans
// attente, au plus une fois par seconde).
bool matterIsConnected() {
  netPoll(millis());
  return sNet.known && roleAttached(sNet.role);
}
#else
bool matterIsConnected() { return Matter.isDeviceConnected(); }
#endif
void matterDecommissionNow() { Matter.decommission(); }

bool matterIdentifying() {
  const uint32_t now = millis();
  bool on = __atomic_load_n(&sIdentifyEps, __ATOMIC_RELAXED) != 0;
  // Toutes les fins, a chaque appel : une fin echue doit etre oubliee bien
  // avant que le retour a zero de millis() la fasse paraitre future.
  for (uint32_t &slot : sIdentifyEffectEnd) {
    uint32_t end = __atomic_load_n(&slot, __ATOMIC_RELAXED);
    if (statusled::effectPending(end, now))
      on = true;
    else if (end)  // echu : oublie, sauf si un nouvel effet vient d'etre pose par la tache CHIP
      __atomic_compare_exchange_n(&slot, &end, 0u, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  }
  return on;
}

void matterPrintStatus(Print &out) {
  out.println();
  out.println("=== Matter ===");
  out.printf("  mise en service : %s\n", Matter.isDeviceCommissioned() ? "faite" : "EN ATTENTE");
  out.printf("  reseau          : %s\n", matterIsConnected() ? "connecte" : "non connecte");
#if MATTER_NET_THREAD
  out.printf("  reseau Matter   : %s, mise en service Thread sur EP%u, Wi-Fi %s\n",
             Matter.getSelectedNetwork() == MATTER_NETWORK_THREAD ? "THREAD" : "PAS THREAD",
             Matter.getNetworkEndPointId(MATTER_NETWORK_THREAD),
             Matter.isWiFiConnected() ? "CONNECTE (anormal)" : "coupe");
  otInstance *ot = sMatterStarted ? esp_openthread_get_instance() : nullptr;
  if (ot && otLockTry(50)) {
    // Lu sous le verrou, affiche apres : Serial peut attendre 1 s par ecriture
    // (setTxTimeoutMs), et tout Thread attendrait avec lui.
    const uint8_t ch = otLinkGetChannel(ot);
    const otDeviceRole role = otThreadGetDeviceRole(ot);
    const uint16_t pan = otLinkGetPanId(ot);
    int8_t rssi = 0, pw = 0;
    const bool parent = otThreadGetParentAverageRssi(ot, &rssi) == OT_ERROR_NONE;
    otPlatRadioGetTransmitPower(ot, &pw);
    esp_openthread_lock_release();
    // Canal 11 = 2405 MHz, la frequence de la lampe : a eviter.
    out.printf("  Thread          : role %s, canal %u (%u MHz), PAN 0x%04X, puissance %d dBm",
               otThreadDeviceRoleToString(role), ch, 2405u + 5u * (ch - 11u), pan, pw);
    if (parent) out.printf(", parent %d dBm", rssi);
    out.println();
  }
  if (sMatterStarted) threadStatus(out);
#elif CONFIG_ENABLE_CHIPOBLE
  out.println("  commissioning   : BLE (le Wi-Fi est fourni par le controleur)");
#else
  out.println("  commissioning   : IP (Wi-Fi a configurer avec 'wifi <ssid> <mdp>')");
#endif
  if (!Matter.isDeviceCommissioned()) {
    out.printf("  code manuel     : %s\n", Matter.getManualPairingCode().c_str());
    out.printf("  QR code         : %s\n", Matter.getOnboardingQRCodeUrl().c_str());
  }
  printIdentity(out);
  out.printf("  endpoints       : EP%u Halo, EP%u Halo avant, EP%u Halo arriere (%s)", mainLight.getEndPointId(),
             frontLamp.getEndPointId(), backLamp.getEndPointId(), HALO1_SELECTORS_AS_LIGHTS ? "lumieres" : "prises");
#if HALO1_EXPOSE_AUTO
  out.printf(", EP%u Halo auto (bouton A)", autoButton.getEndPointId());
#else
  out.print(", EP4 Halo auto desactive");
#endif
  out.println();
  out.printf("  temperature     : %u-%u mireds = temp 00 (froid) a 64 (chaud), ~%u-%u K nominaux\n",
             halo1::kMiredCold, halo1::kMiredWarm, (unsigned)((1000000UL + halo1::kMiredCold / 2) / halo1::kMiredCold),
             (unsigned)((1000000UL + halo1::kMiredWarm / 2) / halo1::kMiredWarm));
  out.printf("  luminosite      : niveau 1-254 -> 4C-FE, gamma %.2f ; rapporte jamais sous %u (1-%u = 4C)\n",
             (double)halo1::mapGamma(), halo1::kMatterLevelFloor, halo1::kMatterLevelFloor);
#if HALO1_EXPOSE_AUTO
  out.printf("  ordres          : %lu fenetres, %lu ignorees au demarrage ; A : %lu appuis, %lu refuses\n",
             (unsigned long)sStats.windows, (unsigned long)sStats.bootIgnored, (unsigned long)sStats.autoFired,
             (unsigned long)sStats.autoRefused);
  out.printf("  bouton A (EP4)  : impulsion %u ms ('matter impulsion <%u..%u>', NVS), %lu A de la telecommande "
             "entendu(s), %lu non reflete(s)\n",
             sAutoPulseMs, kMatterPulseMinMs, kMatterPulseMaxMs, (unsigned long)sStats.autoHeard,
             (unsigned long)sStats.autoLost);
#else
  out.printf("  ordres          : %lu fenetres, %lu ignorees au demarrage\n", (unsigned long)sStats.windows,
             (unsigned long)sStats.bootIgnored);
  out.println("  bouton A (EP4)  : desactive (HALO1_EXPOSE_AUTO 0) : ni endpoint, ni miroir des A de la");
  out.println("                    telecommande, ni 'matter impulsion' ; 'lampe auto' reste (README)");
#endif
  out.printf("  identify        : %lu demande(s) recue(s), %s ('led' pour la LED d'etat)\n",
             (unsigned long)__atomic_load_n(&sIdentifyCount, __ATOMIC_RELAXED),
             matterIdentifying() ? "EN COURS" : "aucune en cours");
  out.printf("  reflets         : %lu (%lu attributs ecrits, %lu echecs, %lu verrou occupe), %lu traces perdues\n",
             (unsigned long)sStats.reflects, (unsigned long)sStats.writes, (unsigned long)sStats.writeFails,
             (unsigned long)sStats.lockBusy, (unsigned long)sStats.logDropped);
}
