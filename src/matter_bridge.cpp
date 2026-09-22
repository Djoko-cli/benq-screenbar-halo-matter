#include "matter_bridge.h"

#include <Matter.h>

#include "config.h"
#include "halo.h"

using namespace chip::app::Clusters;

// ===========================================================================
//  Endpoints Matter
//
//  Chaque endpoint est un accessoire distinct dans l'app de l'utilisateur.
//  Matter est multi-admin : le meme noeud se jumelle simultanement a Apple
//  Home, Google Home, Alexa et Home Assistant.
// ===========================================================================

static MatterOnOffPlugin masterPower;
static MatterColorTemperatureLight frontLight;
static MatterDimmableLight backLight;
#if HALO_EXPOSE_SENSOR_SWITCH
static MatterOnOffPlugin sensorSwitch;
#endif
#if HALO_EXPOSE_AUTO_SWITCH
static MatterOnOffPlugin autoTrigger;
static uint32_t autoFiredAt = 0;
#endif

// Garde anti-boucle. Les setters de la bibliotheque Matter passent par
// updateAttributeVal(), qui redeclenche la chaine de callbacks. Sans ce
// drapeau, refleter l'etat de la lampe vers Matter renverrait immediatement
// une commande radio vers la lampe.
static bool syncing = false;

// ===========================================================================
//  Conversions
//
//  Matter exprime la temperature de couleur en mireds (1 000 000 / Kelvin),
//  echelle inversee : mired eleve = lumiere chaude.
//  Le niveau de LevelControl va de 1 a 254, la lampe de 1 a 100 %.
// ===========================================================================

#define MIRED_MIN (uint16_t)(1000000UL / HALO_CT_MAX_K)  // 6500 K -> 153
#define MIRED_MAX (uint16_t)(1000000UL / HALO_CT_MIN_K)  // 2700 K -> 370
#define MATTER_LEVEL_MAX 254

static uint16_t kelvinToMired(uint16_t k) {
  if (k < HALO_CT_MIN_K) k = HALO_CT_MIN_K;
  if (k > HALO_CT_MAX_K) k = HALO_CT_MAX_K;
  return (uint16_t)(1000000UL / k);
}

static uint16_t miredToKelvin(uint16_t mired) {
  if (mired < MIRED_MIN) mired = MIRED_MIN;
  if (mired > MIRED_MAX) mired = MIRED_MAX;
  uint32_t k = 1000000UL / mired;
  if (k < HALO_CT_MIN_K) k = HALO_CT_MIN_K;
  if (k > HALO_CT_MAX_K) k = HALO_CT_MAX_K;
  return (uint16_t)k;
}

static uint8_t percentToLevel(uint8_t pct) {
  if (pct < HALO_BRIGHT_MIN) pct = HALO_BRIGHT_MIN;
  if (pct > HALO_BRIGHT_MAX) pct = HALO_BRIGHT_MAX;
  return (uint8_t)((pct * MATTER_LEVEL_MAX + 50) / 100);
}

static uint8_t levelToPercent(uint8_t level) {
  if (level > MATTER_LEVEL_MAX) level = MATTER_LEVEL_MAX;
  int pct = (level * 100 + MATTER_LEVEL_MAX / 2) / MATTER_LEVEL_MAX;
  if (pct < HALO_BRIGHT_MIN) pct = HALO_BRIGHT_MIN;
  if (pct > HALO_BRIGHT_MAX) pct = HALO_BRIGHT_MAX;
  return (uint8_t)pct;
}

// Reproduit la logique de l'appareil : eteindre la derniere lampe encore
// allumee revient a couper l'alimentation generale ; rallumer une lampe alors
// que le general est coupe demande deux trames (0x02 puis 0x03).
static void applyLampSelection() {
  if (!halo.desired.front && !halo.desired.back) {
    halo.desired.power = false;
    halo.requestPush(HALO_CMD_ONOFF);
  } else if (!halo.desired.power) {
    halo.desired.power = true;
    halo.requestPushThen(HALO_CMD_ONOFF, HALO_CMD_SET);
  } else {
    halo.requestPush(HALO_CMD_SET);
  }
}

// ===========================================================================
//  Callbacks Matter -> lampe
// ===========================================================================

static bool onMasterPower(bool state) {
  if (syncing) return true;
  halo.desired.power = state;
  // Rallumer sans qu'aucune des deux lampes ne soit selectionnee ne produirait rien.
  if (state && !halo.desired.front && !halo.desired.back) halo.desired.front = true;
  halo.requestPush(HALO_CMD_ONOFF);
  return true;
}

static bool onFrontLight(bool state, uint8_t level, uint16_t mired) {
  if (syncing) return true;
  halo.desired.front = state;
  halo.desired.frontBrightness = levelToPercent(level);
  halo.desired.colorTempK = miredToKelvin(mired);
  applyLampSelection();
  return true;
}

static bool onBackLight(bool state, uint8_t level) {
  if (syncing) return true;
  halo.desired.back = state;
  halo.desired.backBrightness = levelToPercent(level);
  applyLampSelection();
  return true;
}

#if HALO_EXPOSE_SENSOR_SWITCH
static bool onSensor(bool state) {
  if (syncing) return true;
  halo.desired.sensor = state;
  halo.requestPush(HALO_CMD_SET);
  return true;
}
#endif

#if HALO_EXPOSE_AUTO_SWITCH
static bool onAuto(bool state) {
  if (syncing) return true;
  if (state) {
    halo.requestPush(HALO_CMD_SET, true);
    autoFiredAt = millis();
  }
  return true;
}
#endif

// ===========================================================================
//  Cycle de vie
// ===========================================================================

void matterBridgeBegin() {
  bool frontOn = halo.reported.power && halo.reported.front;
  bool backOn = halo.reported.power && halo.reported.back;

  masterPower.begin(halo.reported.power);
  frontLight.begin(frontOn, percentToLevel(halo.reported.frontBrightness),
                   kelvinToMired(halo.reported.colorTempK));
  backLight.begin(backOn, percentToLevel(halo.reported.backBrightness));
#if HALO_EXPOSE_SENSOR_SWITCH
  sensorSwitch.begin(halo.reported.sensor);
#endif
#if HALO_EXPOSE_AUTO_SWITCH
  autoTrigger.begin(false);
#endif

  // La bibliotheque ne renseigne pas la plage physique de temperature de
  // couleur : sans ca les applications affichent un curseur bien plus large
  // que ce que la lampe sait faire.
  esp_matter_attr_val_t val = esp_matter_uint16(MIRED_MIN);
  frontLight.setAttributeVal(ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMinMireds::Id,
                             &val);
  val = esp_matter_uint16(MIRED_MAX);
  frontLight.setAttributeVal(ColorControl::Id, ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id,
                             &val);

  masterPower.onChange(onMasterPower);
  frontLight.onChange(onFrontLight);
  backLight.onChange(onBackLight);
#if HALO_EXPOSE_SENSOR_SWITCH
  sensorSwitch.onChange(onSensor);
#endif
#if HALO_EXPOSE_AUTO_SWITCH
  autoTrigger.onChange(onAuto);
#endif

  Matter.begin();
}

void matterBridgePoll() {
#if HALO_EXPOSE_AUTO_SWITCH
  // Le declencheur "Auto" se rearme tout seul.
  if (autoFiredAt && (millis() - autoFiredAt) > 1500) {
    autoFiredAt = 0;
    syncing = true;
    autoTrigger.setOnOff(false);
    syncing = false;
  }
#endif

  // Pendant un fondu de la lampe, l'etat rapporte est transitoire : on attend
  // la stabilisation pour ne pas faire osciller l'affichage des applications.
  if (!halo.settled()) return;

  syncing = true;
  masterPower.setOnOff(halo.reported.power);
  frontLight.setOnOff(halo.reported.power && halo.reported.front);
  frontLight.setBrightness(percentToLevel(halo.reported.frontBrightness));
  frontLight.setColorTemperature(kelvinToMired(halo.reported.colorTempK));
  backLight.setOnOff(halo.reported.power && halo.reported.back);
  backLight.setBrightness(percentToLevel(halo.reported.backBrightness));
#if HALO_EXPOSE_SENSOR_SWITCH
  sensorSwitch.setOnOff(halo.reported.sensor);
#endif
  syncing = false;
}

bool matterIsCommissioned() { return Matter.isDeviceCommissioned(); }
bool matterIsConnected() { return Matter.isDeviceConnected(); }
void matterDecommissionNow() { Matter.decommission(); }

void matterPrintStatus(Print &out) {
  out.println();
  out.println("=== Matter ===");
  out.printf("  mise en service : %s\n", Matter.isDeviceCommissioned() ? "faite" : "EN ATTENTE");
  out.printf("  reseau          : %s\n", Matter.isDeviceConnected() ? "connecte" : "non connecte");
#if CONFIG_ENABLE_CHIPOBLE
  out.println("  commissioning   : BLE (le Wi-Fi est fourni par le controleur)");
#else
  out.println("  commissioning   : IP (Wi-Fi a configurer avec 'wifi <ssid> <mdp>')");
#endif
  if (!Matter.isDeviceCommissioned()) {
    out.printf("  code manuel     : %s\n", Matter.getManualPairingCode().c_str());
    out.printf("  QR code         : %s\n", Matter.getOnboardingQRCodeUrl().c_str());
  }
  out.printf("  plage couleur   : %u-%u mireds (%d-%d K)\n", MIRED_MIN, MIRED_MAX, HALO_CT_MAX_K,
             HALO_CT_MIN_K);
}
