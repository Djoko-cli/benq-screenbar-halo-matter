#include "matter_bridge.h"

#include <Matter.h>
#include <stdarg.h>

#include "config.h"
#include "halo1_lamp.h"

#if MATTER_NET_THREAD
#include <esp_ieee802154.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <openthread/link.h>
#include <openthread/platform/radio.h>
#include <openthread/thread.h>
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
//  EP4 "Halo auto"     prise momentanee : un appui sur le bouton A
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
static struct {
  uint32_t windows, bootIgnored, autoFired, autoRefused, reflects, writes, writeFails, lockBusy, logDropped;
} sStats = {};

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
template <class F>
static void syncAttr(MatterEndPoint &ep, uint32_t cluster, uint32_t attr, F want) {
  esp_matter_attr_val_t v = esp_matter_invalid(nullptr);
  uint16_t cur = 0;
  if (!ep.getAttributeVal(cluster, attr, &v) || !valueOf(v, cur)) {
    sStats.writeFails++;
    return;
  }
  const uint16_t w = want(cur);
  if (w == cur) return;
  setValue(v, w);
  if (ep.updateAttributeVal(cluster, attr, &v))
    sStats.writes++;
  else
    sStats.writeFails++;
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
  // la consigne, ne saute jamais vers une voisine.
  syncAttr(mainLight, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id,
           [&](uint16_t cur) { return (uint16_t)halo1::displayLevel((uint8_t)cur, t.bright); });
  syncAttr(mainLight, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id,
           [&](uint16_t cur) { return halo1::displayMired(cur, t.temp); });
  syncAttr(frontLamp, OnOff::Id, OnOff::Attributes::OnOff::Id, [&](uint16_t) { return (uint16_t)front; });
  syncAttr(backLamp, OnOff::Id, OnOff::Attributes::OnOff::Id, [&](uint16_t) { return (uint16_t)back; });
#if HALO1_EXPOSE_AUTO
  // Jamais remis a on : seul un controleur l'y met. Remis a off par un
  // controleur pendant l'impulsion, il y reste.
  syncAttr(autoButton, OnOff::Id, OnOff::Attributes::OnOff::Id,
           [&](uint16_t cur) { return (uint16_t)(sAutoPulse && cur); });
#endif
  chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  sSeenVersion = lamp.version();
  sForceReflect = false;
  sLastReflect = now;
  sStats.reflects++;
  return true;
}

// ===========================================================================
//  Cycle de vie
// ===========================================================================

void matterBridgeBegin() {
  // La table gamma est deja construite par lamp.begin().
  sLoopTask = xTaskGetCurrentTaskHandle();
  const halo1::State t = lamp.target();

#if MATTER_NET_THREAD
  // Avant le premier begin() d'accessoire : c'est lui qui cree le noeud, et le
  // core refuse ensuite de changer de reseau. BLE garde pour l'appairage.
  if (!Matter.selectNetwork(MATTER_NETWORK_THREAD))
    Serial.println("!! selectNetwork(THREAD) refuse : le noeud resterait en Wi-Fi");
#endif
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
#if MATTER_THREAD_MED
    // esp_matter met Thread en routeur a chaque demarrage : on l'ecrase.
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    chip::DeviceLayer::ConnectivityMgr().SetThreadDeviceType(
        chip::DeviceLayer::ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
#endif
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
  // Jamais de reflet tant que la boite contient des intentions : un curseur en
  // cours ne revient pas en arriere.
  if (pending) return;
  // Boite vide apres le delai et la fenetre la plus longue : toute fenetre
  // ouverte pendant le delai est refermee. Le garde-fou se desarme pour de bon,
  // et le retour a zero de millis() (49,7 jours) ne peut pas le rouvrir.
  if (sBootGuard && (uint32_t)(now - sBootMs) >= HALO1_BOOT_IGNORE_MS + HALO1_COALESCE_MAX_MS)
    sBootGuard = false;
  const bool pulseOver = sAutoPulse && (uint32_t)(now - sAutoPulseAt) >= HALO1_AUTO_PULSE_MS;
  if (pulseOver) sAutoPulse = false;
  const bool changed =
      lamp.version() != sSeenVersion && (uint32_t)(now - sLastReflect) >= HALO1_REFLECT_MIN_MS;
  if (!sForceReflect && !pulseOver && !changed) return;
  if (!reflect(now)) sForceReflect = true;  // rien de fait : nouvel essai au passage suivant
}

bool matterIsCommissioned() { return Matter.isDeviceCommissioned(); }
#if MATTER_NET_THREAD
// Matter.isDeviceConnected() passe par _IsThreadAttached, qui prend le verrou
// OpenThread SANS limite de temps (esp_openthread_lock_acquire(portMAX_DELAY),
// firmware.elf) : appele par la LED a chaque passage de loop(), il pouvait
// bloquer tick() hors de tout budget (C.9). Ici, un essai sans attente par
// seconde ; verrou occupe : dernier etat connu, nouvel essai au passage suivant.
bool matterIsConnected() {
  static constexpr uint32_t kPollMs = 1000;
  static uint32_t at = 0;
  static bool known = false, attached = false;
  const uint32_t now = millis();
  if (sMatterStarted && (!known || (uint32_t)(now - at) >= kPollMs) && otLockTry(0)) {
    const otDeviceRole role = otThreadGetDeviceRole(esp_openthread_get_instance());
    esp_openthread_lock_release();
    attached = role != OT_DEVICE_ROLE_DISABLED && role != OT_DEVICE_ROLE_DETACHED;
    known = true;
    at = now;
  }
  return attached;
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
  out.printf("  luminosite      : niveau 1-254 -> 4C-FE, gamma %.2f\n", (double)halo1::mapGamma());
  out.printf("  ordres          : %lu fenetres, %lu ignorees au demarrage ; A : %lu appuis, %lu refuses\n",
             (unsigned long)sStats.windows, (unsigned long)sStats.bootIgnored, (unsigned long)sStats.autoFired,
             (unsigned long)sStats.autoRefused);
  out.printf("  reflets         : %lu (%lu attributs ecrits, %lu echecs, %lu verrou occupe), %lu traces perdues\n",
             (unsigned long)sStats.reflects, (unsigned long)sStats.writes, (unsigned long)sStats.writeFails,
             (unsigned long)sStats.lockBusy, (unsigned long)sStats.logDropped);
}
