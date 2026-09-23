#include "matter_bridge.h"

#include <Matter.h>
#include <Preferences.h>
#include <stdarg.h>

#include "config.h"
#include "halo1_lamp.h"

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
//                      rien emettre
//
//  Les numeros viennent de l'ordre de creation ; les noms se donnent dans
//  l'app. Matter est multi-admin : le meme noeud se jumelle a Apple Home,
//  Google Home, Alexa et Home Assistant.
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
//  Etat du pont (tache loop uniquement)
// ===========================================================================

static uint32_t sBootMs = 0, sAutoPulseAt = 0, sSeenVersion = 0, sLastReflect = 0;
static bool sBootGuard = true;     // garde-fou de demarrage encore arme
static bool sAutoPulse = false;    // EP4 a on : impulsion du bouton A en cours
static bool sForceReflect = true;  // realigner Matter sur la consigne au prochain passage
#if HALO1_EXPOSE_AUTO
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
//  pilote, une cle a part. Lue et ecrite dans la tache loop seulement.
// ===========================================================================

static const char *const kNvsNs = "halo1";
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
//  echec incremente un compteur sauve avec l'abonnement : au 11e, meme a
//  travers les redemarrages, la pile l'oublie (firmware.elf desassemble).
//
//  Ce que ce bloc ajoute :
//   - mesures dans 'matter' : abonnements actifs et sauves, roles Thread
//     horodates, compteurs MLE, etat SRP et DNS, tentatives de reprise ;
//   - (a) relance de la reprise quand le reseau est pret (ResumePlanner) ;
//   - (b) type Thread choisi a l'execution ('matter med', enveloppe plus bas) ;
//   - (c) plafond optionnel de l'intervalle max des abonnements neufs.
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
static constexpr uint8_t kMedRouter = 0, kMedEarly = 1;  // 2 : MED apres Matter.begin() (ancien)
static uint8_t sMedMode = MATTER_THREAD_MED ? kMedEarly : kMedRouter;  // prochain demarrage
static uint8_t sMedBoot = MATTER_THREAD_MED ? kMedEarly : kMedRouter;  // ce demarrage
static bool sResumeAuto = true;
// Ecrit par la tache loop, lu par la tache CHIP (OnSubscriptionRequested).
static volatile uint16_t sMaxIntCap = 0;

static void loadThreadSettings() {
  Preferences p;
  if (!p.begin(kNvsNs, false)) return;
  if (p.isKey(kNvsMedKey)) {
    const uint8_t v = p.getUChar(kNvsMedKey, sMedMode);
    if (v < kMatterMedModes) sMedMode = v;
  }
  if (p.isKey(kNvsResumeKey)) sResumeAuto = p.getUChar(kNvsResumeKey, 1) != 0;
  if (p.isKey(kNvsMaxIntKey)) {
    const uint16_t v = p.getUShort(kNvsMaxIntKey, 0);
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
//  abonnement repris (ReadHandler::OnSubscriptionResumed), sans
//  OnSubscriptionRequested avant : c'est ce qui les distingue.
static struct {
  uint32_t requested, capped, fresh, resumed, terminated;
  uint32_t firstAt;  // premier abonnement etabli depuis le demarrage (0 : aucun)
  uint32_t lastAt;
  bool lastResumed;
  uint16_t lastMin, lastMax;
  uint64_t reqPeer;
  uint16_t reqMin, reqMax, reqApplied;
  uint32_t reqAt;
} sSubs = {};
static const void *sRequestedHandler = nullptr;  // tache CHIP seulement

class SubscriptionWatch : public chip::app::ReadHandler::ApplicationCallback {
 public:
  // (c) Plafond de l'intervalle max : Apple le fixe sans doute a 600 s pour ce
  // noeud (code Darwin public). Apres un redemarrage qu'aucune reprise ne
  // rattrape, Apple ne s'apercoit de la perte qu'au bout de cet intervalle
  // (plus une marge) ; un plafond plus court borne la duree sans miroir, au
  // prix d'un rapport vide par intervalle. Abonnements neufs seulement : un
  // abonnement repris garde l'intervalle sauve. Regle du SDK :
  // plancher <= max <= max(3600, plafond demande).
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
    sRequestedHandler = &rh;
    return CHIP_NO_ERROR;
  }

  void OnSubscriptionEstablished(chip::app::ReadHandler &rh) override {
    uint16_t minS = 0, maxS = 0;
    rh.GetReportingIntervals(minS, maxS);
    const uint32_t now = millis();
    portENTER_CRITICAL(&sSubMux);
    // Une demande restee sans suite (refusee apres OnSubscriptionRequested) ne
    // fait pas passer pour neuf, plus tard, un abonnement repris a la meme
    // adresse : 30 s au plus entre la demande et l'etablissement.
    const bool fresh = sRequestedHandler == &rh && (uint32_t)(now - sSubs.reqAt) < 30000;
    if (fresh) sSubs.fresh++;
    else sSubs.resumed++;
    if (!sSubs.firstAt) sSubs.firstAt = now ? now : 1;
    sSubs.lastAt = now;
    sSubs.lastResumed = !fresh;
    sSubs.lastMin = minS;
    sSubs.lastMax = maxS;
    sEventSeq++;
    portEXIT_CRITICAL(&sSubMux);
    if (sRequestedHandler == &rh) sRequestedHandler = nullptr;
  }

  void OnSubscriptionTerminated(chip::app::ReadHandler &rh) override {
    portENTER_CRITICAL(&sSubMux);
    sSubs.terminated++;
    sEventSeq++;
    portEXIT_CRITICAL(&sSubMux);
    if (sRequestedHandler == &rh) sRequestedHandler = nullptr;
  }
};
static SubscriptionWatch sSubWatch;

// --- (a) Relance de la reprise (tache CHIP) ---------------------------------
//
//  API publique, celle qu'emploie la pile a chaque tentative
//  (InteractionModelEngine::ResumeSubscriptionsTimerCallback) : pour chaque
//  abonnement sauve, un SubscriptionResumptionSessionEstablisher alloue par
//  Platform::New, dont ResumeSubscription() ouvre (ou rejoint) une session
//  CASE vers l'abonne ; ses rappels le liberent (Platform::Delete), creent le
//  ReadHandler et remettent le compteur d'essais a 0, ou l'incrementent et
//  programment la tentative suivante de la pile. ResumeSubscriptions()
//  n'aurait rien fait : il sort tant qu'une tentative est programmee.
//
//  Un observateur par abonne (FindOrEstablishSession avec nos propres
//  rappels, sur la meme mise en place de session) donne la fin, sa duree et
//  l'erreur. Il est lance avant l'etablisseur, qui le rejoint.
//
//  Garde-fous : aucun abonnement actif (sinon, faute d'API publique pour lire
//  l'identifiant d'un abonnement vivant, on risquerait un doublon) ; aucune
//  tentative de ce pont en cours ; jamais un abonnement deja a
//  kResumeMaxRetries essais (le 11e echec le fait oublier : la pile garde
//  seule la main sur ses derniers essais). Reste possible : rejoindre une
//  tentative de la pile en cours (d'ou kNotBeforeMs au demarrage) ; en cas
//  de succes, deux ReadHandler pour le meme abonnement, rapports doubles
//  jusqu'au prochain reabonnement d'Apple.

enum : intptr_t { kResumeAuto = 0, kResumeManual = 1 };
enum : uint8_t { kRunLaunched, kRunSubsActive, kRunNothing, kRunNoStorage, kRunNoIterator };
static constexpr uint32_t kResumeMaxRetries = 8;
static constexpr uint32_t kWatchLostMs = 180000;  // recherche 45 s + CASE : bien en deca

struct SavedSub {
  uint64_t node;
  uint32_t id, retries;
  uint16_t minS, maxS;
  uint8_t fabric;
};
static constexpr uint8_t kSavedMax = 6;

struct ResumeWatch {
  ResumeWatch() : conn(onConnected, this), fail(onFailed, this) {}
  chip::Callback::Callback<chip::OnDeviceConnected> conn;
  chip::Callback::Callback<chip::OnDeviceConnectionFailure> fail;
  uint64_t node = 0;
  uint8_t fabric = 0;
  uint32_t startMs = 0;
  bool pending = false;  // sous sSubMux
  static void onConnected(void *ctx, chip::Messaging::ExchangeManager &, const chip::SessionHandle &);
  static void onFailed(void *ctx, const chip::ScopedNodeId &, CHIP_ERROR err);
};
static constexpr uint8_t kWatchMax = 2;
static ResumeWatch sWatch[kWatchMax];

static struct {
  // lancements (resumeWork)
  uint32_t runs, autoRuns, launched;
  uint32_t runSeq, runAt, runSubs, runSaved;
  uint8_t runKind, runVerdict, runLaunched, runSkipped, runBusy, runFailed;
  // fins de session (observateurs)
  uint32_t ok, failed, lost, late;
  uint32_t doneSeq, doneAt, doneMs, doneErr;
  uint64_t doneNode;
} sResume = {};
static bool sResumePosted = false;  // travail poste, pas encore execute ; sous sSubMux

static void watchDone(ResumeWatch *w, CHIP_ERROR err) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&sSubMux);
  if (w->pending) {
    w->pending = false;
    if (err == CHIP_NO_ERROR) sResume.ok++;
    else sResume.failed++;
    sResume.doneErr = err.AsInteger();
    sResume.doneMs = now - w->startMs;
    sResume.doneAt = now;
    sResume.doneNode = w->node;
    sResume.doneSeq++;
    sEventSeq++;
  } else {
    sResume.late++;  // fin arrivee apres l'abandon (kWatchLostMs)
  }
  portEXIT_CRITICAL(&sSubMux);
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
// (GroupedCallbackList::Enqueue appelle Cancel()). *launched : abonnements
// relances par le dernier passage, lu dans le meme instant.
static bool resumeInFlight(uint32_t now, uint32_t *launched = nullptr) {
  bool busy;
  portENTER_CRITICAL(&sSubMux);
  busy = sResumePosted;
  if (launched) *launched = sResume.runLaunched;
  for (ResumeWatch &w : sWatch) {
    if (!w.pending) continue;
    if ((uint32_t)(now - w.startMs) >= kWatchLostMs) {
      w.pending = false;
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

static void resumeWork(intptr_t kind) {
  // Tache CHIP, verrou de la pile tenu.
  auto *im = chip::app::InteractionModelEngine::GetInstance();
  const uint32_t subs = im->GetNumActiveReadHandlers(chip::app::ReadHandler::InteractionType::Subscribe);
  SubscriptionResumptionStorage *st = im->GetSubscriptionResumptionStorage();
  chip::CASESessionManager *mgr = im->GetCASESessionManager();
  SavedSub saved[kSavedMax];
  uint32_t total = 0;
  uint8_t launched = 0, skipped = 0, busy = 0, failed = 0, verdict;
  int n = 0;
  if (subs > 0) {
    verdict = kRunSubsActive;
  } else if (!st || !mgr) {
    verdict = kRunNoStorage;
  } else if ((n = collectSaved(st, saved, kSavedMax, total)) < 0) {
    verdict = kRunNoIterator;
  } else {
    uint8_t watched = 0;  // observateurs lances par ce passage (un par abonne)
    ResumeWatch *mine[kWatchMax] = {};
    for (int i = 0; i < n; i++) {
      const SavedSub &s = saved[i];
      if (s.retries >= kResumeMaxRetries) {
        skipped++;
        continue;
      }
      bool known = false;
      for (uint8_t k = 0; k < watched; k++) known |= mine[k]->node == s.node && mine[k]->fabric == s.fabric;
      ResumeWatch *w = nullptr;
      if (!known) {
        portENTER_CRITICAL(&sSubMux);
        for (ResumeWatch &c : sWatch) {
          if (c.pending && c.node == s.node && c.fabric == s.fabric) {  // tentative d'avant pas finie
            w = nullptr;
            break;
          }
          if (!c.pending && !w) w = &c;
        }
        portEXIT_CRITICAL(&sSubMux);
        if (!w) {
          busy++;
          continue;
        }
      }
      SubInfo info;
      if (!loadSaved(st, s, info)) {
        failed++;
        continue;
      }
      if (w) {
        const uint32_t now = millis();
        portENTER_CRITICAL(&sSubMux);
        w->node = s.node;
        w->fabric = s.fabric;
        w->startMs = now;
        w->pending = true;
        portEXIT_CRITICAL(&sSubMux);
        mine[watched++] = w;
        // Peut finir tout de suite (session deja ouverte, ou pas de place).
        mgr->FindOrEstablishSession(chip::ScopedNodeId(s.node, s.fabric), &w->conn, &w->fail);
      }
      auto *est = chip::Platform::New<chip::app::SubscriptionResumptionSessionEstablisher>();
      if (!est) {
        failed++;
        break;
      }
      // Erreur possible seulement avant l'ouverture de session (copie des
      // chemins) : rien n'est accroche, l'etablisseur est a nous.
      if (est->ResumeSubscription(*mgr, info) != CHIP_NO_ERROR) {
        chip::Platform::Delete(est);
        failed++;
        continue;
      }
      launched++;
    }
    verdict = launched ? kRunLaunched : kRunNothing;
  }
  const uint32_t now = millis();
  portENTER_CRITICAL(&sSubMux);
  sResume.runs++;
  if (kind == kResumeAuto) sResume.autoRuns++;
  sResume.launched += launched;
  sResume.runSeq++;
  sResume.runAt = now;
  sResume.runSubs = subs;
  sResume.runSaved = total;
  sResume.runKind = (uint8_t)kind;
  sResume.runVerdict = verdict;
  sResume.runLaunched = launched;
  sResume.runSkipped = skipped;
  sResume.runBusy = busy;
  sResume.runFailed = failed;
  sEventSeq++;
  sResumePosted = false;  // en dernier : les observateurs sont deja marques
  portEXIT_CRITICAL(&sSubMux);
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
static uint32_t sSeenRoles = 0, sSeenRunSeq = 0, sSeenDoneSeq = 0;
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

// Evenements rares (quelques-uns par demarrage) : toujours traces.
static void tracePoll() {
  static uint32_t seen = 0;
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
  if (subs.fresh + subs.resumed != sSeenEstablished) {
    sSeenEstablished = subs.fresh + subs.resumed;
    bridgeLog("[matter] abonnement etabli (%s) a +%s s : min %u s, max %u s", subs.lastResumed ? "repris" : "neuf",
              secs(a, sizeof(a), subs.lastAt), subs.lastMin, subs.lastMax);
  }
  if (subs.terminated != sSeenTerminated) {
    sSeenTerminated = subs.terminated;
    bridgeLog("[matter] abonnement termine (%lu en tout)", (unsigned long)subs.terminated);
  }
  if (res.runSeq != sSeenRunSeq) {
    sSeenRunSeq = res.runSeq;
    const char *kind = res.runKind == kResumeManual ? "manuelle" : "auto";
    switch (res.runVerdict) {
      case kRunLaunched:
        bridgeLog("[matter] reprise %s a +%s s : %u abonnement(s) relance(s) sur %lu sauve(s), %u laisse(s) a la "
                  "pile (>= %lu essais), %u deja en cours, %u rate(s)",
                  kind, secs(a, sizeof(a), res.runAt), res.runLaunched, (unsigned long)res.runSaved, res.runSkipped,
                  (unsigned long)kResumeMaxRetries, res.runBusy, res.runFailed);
        break;
      case kRunSubsActive:
        bridgeLog("[matter] reprise %s : %lu abonnement(s) deja actif(s), rien de lance", kind,
                  (unsigned long)res.runSubs);
        break;
      case kRunNothing:
        bridgeLog("[matter] reprise %s : rien a relancer (%lu sauve(s), %u a >= %lu essais, %u en cours, %u rate(s))",
                  kind, (unsigned long)res.runSaved, res.runSkipped, (unsigned long)kResumeMaxRetries, res.runBusy,
                  res.runFailed);
        break;
      case kRunNoStorage: bridgeLog("[matter] reprise %s : pas de stockage d'abonnements", kind); break;
      default: bridgeLog("[matter] reprise %s : iterateur du stockage occupe, a refaire", kind); break;
    }
  }
  if (res.doneSeq != sSeenDoneSeq) {
    sSeenDoneSeq = res.doneSeq;
    nodeText(node, sizeof(node), res.doneNode);
    if (res.doneErr == CHIP_NO_ERROR.AsInteger())
      bridgeLog("[matter] reprise : session CASE avec %s ouverte en %s s", node, secs(b, sizeof(b), res.doneMs));
    else
      bridgeLog("[matter] reprise : echec 0x%lX avec %s apres %s s%s", (unsigned long)res.doneErr, node,
                secs(b, sizeof(b), res.doneMs),
                res.doneErr == CHIP_ERROR_TIMEOUT.AsInteger() ? " (delai : adresse introuvable ou CASE muet)" : "");
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
  if ((uint32_t)(now - sBootMs) < ResumePlanner::kNotBeforeMs)
    out.println("  (moins de 50 s apres le demarrage : la tentative de la pile peut encore chercher l'adresse, "
                "et celle-ci la rejoindre)");
  return true;
}

// Etat pour 'matter' : releve sous chaque verrou tour a tour, puis affiche.
static void threadStatus(Print &out) {
  const uint32_t now = millis();
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
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  }

  RoleChange roles[kRoleHistory];
  uint32_t nRoles;
  decltype(sSubs) sb;
  decltype(sResume) rs;
  uint32_t watchAge = 0;
  bool watching = false;
  portENTER_CRITICAL(&sSubMux);
  nRoles = sRoleChanges;
  memcpy(roles, sRoles, sizeof(roles));
  sb = sSubs;
  rs = sResume;
  for (const ResumeWatch &w : sWatch)
    if (w.pending) {
      watching = true;
      watchAge = now - w.startMs;
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
             kMedText[sMedBoot], mode, sDevTypeCalls, sDevTypeSwaps, kMedText[sMedMode],
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
      out.printf("                    abonne %s (fabrique %u), id 0x%08lX, %lu echec(s) de reprise, min %u s, "
                 "max %u s\n",
                 node, saved[i].fabric, (unsigned long)saved[i].id, (unsigned long)saved[i].retries, saved[i].minS,
                 saved[i].maxS);
    }
  } else {
    out.println("  abonnements     : pile occupee, reessayer");
  }

  out.printf("  abonnes (IM)    : %lu demande(s), %lu neuf(s), %lu repris, %lu termine(s) ; premier etabli ",
             (unsigned long)sb.requested, (unsigned long)sb.fresh, (unsigned long)sb.resumed,
             (unsigned long)sb.terminated);
  if (sb.firstAt) out.printf("a +%s s\n", secs(a, sizeof(a), sb.firstAt));
  else out.println(": aucun depuis le demarrage");
  if (sb.requested) {
    char node[24];
    nodeText(node, sizeof(node), sb.reqPeer);
    out.printf("                    derniere demande a +%s s par %s : plancher %u s, max %u s -> %u s\n",
               secs(a, sizeof(a), sb.reqAt), node, sb.reqMin, sb.reqMax, sb.reqApplied);
  }

  out.printf("  reprise         : auto %s ('matter reprise [auto 0|1]') ; %lu lancement(s) dont %lu auto, %lu "
             "abonnement(s) relance(s) ; CASE %lu ouverte(s), %lu echec(s), %lu sans nouvelles\n",
             sResumeAuto ? "oui" : "non", (unsigned long)rs.runs, (unsigned long)rs.autoRuns,
             (unsigned long)rs.launched, (unsigned long)rs.ok, (unsigned long)rs.failed, (unsigned long)rs.lost);
  if (rs.doneSeq) {
    out.printf("                    derniere fin a +%s s apres %s s : ", secs(a, sizeof(a), rs.doneAt),
               secs(b, sizeof(b), rs.doneMs));
    if (rs.doneErr == CHIP_NO_ERROR.AsInteger()) out.println("session ouverte");
    else out.printf("erreur 0x%lX\n", (unsigned long)rs.doneErr);
  }
  if (watching) out.printf("                    tentative en cours depuis %s s\n", secs(a, sizeof(a), watchAge));
  else if (sResumeAuto && sPlan.waiting) out.println("                    tentative auto postee");
  else if (sResumeAuto) {
    const uint32_t left = sPlan.holdLeftMs(now);
    if (left) out.printf("                    prochaine auto possible dans %s s (essai %u de l'episode)\n",
                         secs(a, sizeof(a), left), sPlan.tries + 1);
  }

  if (sMaxIntCap)
    out.printf("  intervalle max  : plafonne a %u s pour les abonnements neufs ('matter maxint'), %lu applique(s)\n",
               sMaxIntCap, (unsigned long)sb.capped);
  else
    out.println("  intervalle max  : celui du controleur ('matter maxint <60..3600>' pour plafonner)");
}
#endif  // MATTER_NET_THREAD

// ===========================================================================
//  Cycle de vie
// ===========================================================================

void matterBridgeBegin() {
  // La table gamma est deja construite par lamp.begin().
  sLoopTask = xTaskGetCurrentTaskHandle();
  const halo1::State t = lamp.target();
  loadAutoPulse();
#if HALO1_EXPOSE_AUTO
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
    if (sMedBoot != kMedRouter) {
      // Mode 2 (ancien) : esp_matter a mis Thread en routeur, on l'ecrase, et
      // OpenThread relance l'attache. Mode 1 : deja MED, OpenThread sort sans
      // rien faire (meme mode) ; filet si l'enveloppe manque au lien.
      chip::DeviceLayer::PlatformMgr().LockChipStack();
      chip::DeviceLayer::ConnectivityMgr().SetThreadDeviceType(
          chip::DeviceLayer::ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
      chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    }
    // Suivi des abonnements : un seul rappel applicatif par pile, libre (nm).
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    chip::app::InteractionModelEngine::GetInstance()->RegisterReadHandlerAppCallback(&sSubWatch);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
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
  const uint32_t pulseAge = now - sAutoPulseAt;
#if HALO1_EXPOSE_AUTO
  // Montee pas encore ecrite : l'impulsion n'a pas commence, seule l'attente
  // est bornee (un EP4 casse ne garde pas l'impulsion pour toujours).
  const bool pulseOver = sAutoPulse && pulseAge >= (sAutoRaise ? kAutoRaiseMaxWaitMs : (uint32_t)sAutoPulseMs);
#else
  const bool pulseOver = sAutoPulse && pulseAge >= sAutoPulseMs;
#endif
  if (pulseOver) {
    sAutoPulse = false;
#if HALO1_EXPOSE_AUTO
    if (sAutoRaise) {  // EP4 jamais mis a on apres la fin de l'impulsion
      sAutoRaise = false;
      sStats.autoLost++;
      if (lamp.tracing()) bridgeLog("[matter] A de la telecommande non reflete : EP4 pas ecrit en %lu ms",
                                    (unsigned long)kAutoRaiseMaxWaitMs);
    }
#endif
  }
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
  out.printf("  endpoints       : EP%u Halo, EP%u Halo avant, EP%u Halo arriere (%s)", mainLight.getEndPointId(),
             frontLamp.getEndPointId(), backLamp.getEndPointId(), HALO1_SELECTORS_AS_LIGHTS ? "lumieres" : "prises");
#if HALO1_EXPOSE_AUTO
  out.printf(", EP%u Halo auto (bouton A)", autoButton.getEndPointId());
#endif
  out.println();
  out.printf("  temperature     : %u-%u mireds = temp 00 (froid) a 64 (chaud), ~%u-%u K nominaux\n",
             halo1::kMiredCold, halo1::kMiredWarm, (unsigned)((1000000UL + halo1::kMiredCold / 2) / halo1::kMiredCold),
             (unsigned)((1000000UL + halo1::kMiredWarm / 2) / halo1::kMiredWarm));
  out.printf("  luminosite      : niveau 1-254 -> 4C-FE, gamma %.2f ; rapporte jamais sous %u (1-%u = 4C)\n",
             (double)halo1::mapGamma(), halo1::kMatterLevelFloor, halo1::kMatterLevelFloor);
  out.printf("  ordres          : %lu fenetres, %lu ignorees au demarrage ; A : %lu appuis, %lu refuses\n",
             (unsigned long)sStats.windows, (unsigned long)sStats.bootIgnored, (unsigned long)sStats.autoFired,
             (unsigned long)sStats.autoRefused);
#if HALO1_EXPOSE_AUTO
  out.printf("  bouton A (EP4)  : impulsion %u ms ('matter impulsion <%u..%u>', NVS), %lu A de la telecommande "
             "entendu(s), %lu non reflete(s)\n",
             sAutoPulseMs, kMatterPulseMinMs, kMatterPulseMaxMs, (unsigned long)sStats.autoHeard,
             (unsigned long)sStats.autoLost);
#else
  out.printf("  bouton A (EP4)  : absent (HALO1_EXPOSE_AUTO 0), impulsion %u ms\n", sAutoPulseMs);
#endif
  out.printf("  reflets         : %lu (%lu attributs ecrits, %lu echecs, %lu verrou occupe), %lu traces perdues\n",
             (unsigned long)sStats.reflects, (unsigned long)sStats.writes, (unsigned long)sStats.writeFails,
             (unsigned long)sStats.lockBusy, (unsigned long)sStats.logDropped);
}
