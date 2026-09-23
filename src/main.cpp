// ===========================================================================
//  BenQ ScreenBar Halo (1re generation) -> Matter
//  ESP32 + module RF Holtek BM5602-60-1 (transceiver BC5602)
//
//  L'ESP32 se fait passer pour la telecommande 2.4 GHz de la lampe et l'expose
//  comme un noeud Matter. Matter etant multi-admin, le meme appareil se
//  jumelle simultanement a Apple Home, Google Home, Alexa et Home Assistant.
//
//  Premier demarrage :
//    1. moniteur serie a 115200 bauds
//    2. 'find' pour trouver l'adresse de communication de la lampe
//    3. appairage Matter avec le code affiche au demarrage ('matter' le rappelle)
//    4. 'help' pour la liste des commandes
// ===========================================================================

#include <Arduino.h>
#include <esp_system.h>
#include <esp_log.h>
#include <stdarg.h>
#include <string.h>
#ifndef DIAG_ONLY
#include <lib/support/logging/CHIPLogging.h>
#endif
#include <sdkconfig.h>

#include "cli.h"
#include "config.h"
#include "halo.h"
#ifndef DIAG_ONLY
#include "matter_bridge.h"
#include "net.h"
#endif

bool chipLogging = false;
static vprintf_like_t previousVprintf = nullptr;

// esp-matter n'emet pas ses traces par les macros ESP_LOGx : ni
// esp_log_level_set() par tag, ni le filtre interne de CHIP ne les arretent
// (on n'obtenait qu'une ligne de prefixe vide, repetee toutes les 5 s).
// Le hook de sortie du log est le seul point de passage obligatoire.
static int quietVprintf(const char *fmt, va_list args) {
  if (!chipLogging) {
    // Le tag est un ARGUMENT du format, pas une partie du format : il faut
    // formater la ligne pour pouvoir l'inspecter.
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0 && strstr(line, "chip[") != nullptr) return 0;  // ligne avalee
  }
  return previousVprintf ? previousVprintf(fmt, args) : vprintf(fmt, args);
}

void setChipLogging(bool on) {
  chipLogging = on;
  if (!previousVprintf) previousVprintf = esp_log_set_vprintf(quietVprintf);
#ifndef DIAG_ONLY
  chip::Logging::SetLogFilter(on ? chip::Logging::kLogCategory_Progress : chip::Logging::kLogCategory_None);
#endif
  esp_log_level_t level = on ? ESP_LOG_INFO : ESP_LOG_NONE;
  for (const char *tag : {"chip[DL]", "chip[SVR]", "chip[DIS]", "wifi"}) esp_log_level_set(tag, level);
}

static inline void ledWrite(bool on) {
  digitalWrite(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW ? !on : on);
}

// LED d'etat : clignotement rapide = a appairer, lent = reseau absent,
// eteinte = operationnel.
static void statusLed() {
  static uint32_t last = 0;
  static bool on = false;

  uint32_t period;
#ifdef DIAG_ONLY
  period = 0;
#else
  if (!matterIsCommissioned()) period = 200;
  else if (!matterIsConnected()) period = 1000;
  else period = 0;
#endif

  if (period == 0) {
    if (on) {
      on = false;
      ledWrite(false);
    }
    return;
  }
  if (millis() - last >= period) {
    last = millis();
    on = !on;
    ledWrite(on);
  }
}

// Appui long sur le bouton BOOT : retire toutes les fabriques Matter pour
// pouvoir re-appairer l'accessoire de zero.
static void decommissionButton() {
  static uint32_t pressedAt = 0;
  bool pressed = digitalRead(PIN_DECOMMISSION_BTN) == LOW;

  if (!pressed) {
    pressedAt = 0;
    return;
  }
  if (!pressedAt) {
    pressedAt = millis();
    return;
  }
  if (millis() - pressedAt >= DECOMMISSION_HOLD_MS) {
    pressedAt = 0;
    Serial.println();
    Serial.println("Bouton maintenu : retrait des fabriques Matter...");
#ifndef DIAG_ONLY
    matterDecommissionNow();
#endif
  }
}

// Sur l'USB natif du C6, un redemarrage fait re-enumerer le port et le moniteur
// ne se rattache pas : une panique passe donc inapercue, sans meme un message.
// Cette cause reste lisible pendant toute la session, d'ou la commande 'cause'.
const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "mise sous tension";
    case ESP_RST_EXT:      return "broche de reset";
    case ESP_RST_SW:       return "redemarrage logiciel";
    case ESP_RST_PANIC:    return "PANIQUE (exception)";
    case ESP_RST_INT_WDT:  return "CHIEN DE GARDE des interruptions";
    case ESP_RST_TASK_WDT: return "CHIEN DE GARDE de tache";
    case ESP_RST_WDT:      return "CHIEN DE GARDE (autre)";
    case ESP_RST_BROWNOUT: return "BAISSE DE TENSION";
    case ESP_RST_USB:      return "reinitialisation par l'USB";
    default:               return "inconnue";
  }
}

void setup() {
  // L'USB Serial/JTAG jette les octets des que son tampon d'emission sature
  // (tx_timeout_ms = 100 par defaut dans le core) : le banner de demarrage
  // arrivait deja tronque. On agrandit le tampon et on demande a write()
  // d'attendre plutot que de perdre des donnees -- le resume de 'find' est une
  // rafale, et c'est la sortie qu'on peut le moins se permettre de perdre.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && defined(ARDUINO_USB_MODE) && \
    ARDUINO_USB_MODE == 1
  Serial.setTxBufferSize(4096);
  Serial.setTxTimeoutMs(1000);
#endif
  Serial.begin(115200);
  delay(400);

#ifdef DIAG_ONLY
  // IO15 porte aussi GDO2 du CC2500 sur la carte de capture : le piloter en
  // sortie mettrait deux sorties en conflit sur le meme fil (audit, bogue B9).
  // En diagnostic, la LED ne sert a rien : on laisse la broche en entree.
  pinMode(PIN_STATUS_LED, INPUT);
#else
  pinMode(PIN_STATUS_LED, OUTPUT);
  ledWrite(false);
#endif
  pinMode(PIN_DECOMMISSION_BTN, INPUT_PULLUP);

  // Tant que le noeud n'est pas mis en service, la pile Matter repete une
  // erreur reseau toutes les 5 s. C'est normal (pas encore d'identifiants
  // Wi-Fi), mais ca noie la sortie des captures RF. Reactivable avec 'chiplog'.
  setChipLogging(false);

  Serial.println();
  Serial.println("=== BenQ ScreenBar Halo -> Matter ===");

  Serial.print("  cause du dernier demarrage : ");
  Serial.println(resetReasonText());
  Serial.printf("firmware %s\n", FW_VERSION);

  if (!halo.begin()) {
    Serial.println();
    Serial.println("!! Module BM5602 introuvable.");
    Serial.printf("!! Verifie le cablage SPI : SCK=%d MISO=%d MOSI=%d CSN=%d\n", PIN_RF_SCK, PIN_RF_MISO,
                  PIN_RF_MOSI, PIN_RF_CSN);
    Serial.println("!! (MISO va sur GIO2, MOSI sur SDIO -- voir docs/WIRING.md)");
    Serial.println("!! Matter demarre quand meme, mais la lampe ne repondra pas.");
  }
  // Rien n'est emis au demarrage : l'ancien HELLO (protocole Halo 2, pollNow)
  // est retire, la lampe est un Halo 1 (plan du pilote Halo 1, etape C1).

#ifndef DIAG_ONLY
  netBegin();
  matterBridgeBegin();
  setChipLogging(false);  // l'init de la pile peut avoir repose son propre filtre
#else
  Serial.println();
  Serial.println("*** BUILD DIAGNOSTIC : Matter, Wi-Fi et BLE desactives.");
  Serial.println("*** La radio de l'ESP32 est muette : l'environnement RF est propre.");
#endif

#ifndef DIAG_ONLY
  if (!matterIsCommissioned()) {
    Serial.println();
    Serial.println("Noeud Matter pas encore mis en service : ajoute l'accessoire");
    Serial.println("depuis ton application domotique avec les identifiants ci-dessous.");
  }
  matterPrintStatus(Serial);
#endif

  cliBegin();
}

void loop() {
  halo.tick();
#ifndef DIAG_ONLY
  matterBridgePoll();
#endif
  cliPoll();
  statusLed();
  decommissionButton();
}
