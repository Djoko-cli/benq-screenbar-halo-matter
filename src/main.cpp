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
//    2. 'ecoute 4FF0FD63 5' verifie le lien de la lampe (sur l'air 63 FD F0 4F)
//    3. appairage Matter avec le code affiche au demarrage ('matter' le rappelle)
//    4. 'help' pour la liste des commandes
// ===========================================================================

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_log.h>
#include <stdarg.h>
#include <string.h>
#ifndef DIAG_ONLY
#include <lib/support/logging/CHIPLogging.h>
#endif
#include <sdkconfig.h>

#include "boot_button.h"
#include "cli.h"
#include "config.h"
#include "halo.h"
#include "halo1_lamp.h"
#include "json_mode.h"
#include "status_led.h"
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
  // d'attendre plutot que de perdre des donnees -- les bilans des outils de
  // banc sont des rafales, et c'est la sortie qu'on peut le moins se permettre
  // de perdre.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && defined(ARDUINO_USB_MODE) && \
    ARDUINO_USB_MODE == 1
  Serial.setTxBufferSize(4096);
  Serial.setTxTimeoutMs(1000);
#endif
  Serial.begin(115200);
  delay(400);
  // Identifiant de ce demarrage (protocole JSON, hello.boot) : avant toute
  // radio de l'ESP32, donc avant matterBridgeBegin().
  jsonBegin();

  statusLedBegin();  // WS2812 au noir, IO15 en entree (bogue B9)
  // Bouton BOOT (redemarrage, desappairage) : releve des le premier tour de
  // loop() ; tenu a ce moment, il est ignore jusqu'a son relachement. Pose
  // aussi la garde de tous les esp_restart() (IO9 relue haute avant le
  // reset) : avant netBegin() et matterBridgeBegin(), pour passer apres
  // leurs gestionnaires d'arret.
  bootButtonBegin();

  // Tant que le noeud n'est pas mis en service, la pile Matter repete une
  // erreur reseau toutes les 5 s. C'est normal (pas encore d'identifiants
  // Wi-Fi), mais ca noie la sortie des captures RF. Reactivable avec 'chiplog'.
  setChipLogging(false);

  Serial.println();
  Serial.println("=== BenQ ScreenBar Halo -> Matter ===");

  Serial.print("  cause du dernier demarrage : ");
  Serial.println(resetReasonText());
  // Meme chaine que le "Programme interne" d'Apple Home : celle du descripteur
  // d'application (src/app_desc.c). Deux controles : le symbole lie, que lit la
  // pile Matter, et le descripteur en tete de l'image flashee, que lisent le
  // chargeur d'amorcage et esptool. Le premier differe si app_desc.c n'est pas
  // lie, le second aussi s'il n'est plus place en tete de .flash.appdesc.
  Serial.printf("firmware %s\n", FW_VERSION_FULL);
  if (strcmp(esp_app_get_description()->version, FW_VERSION_FULL))
    Serial.printf("!! descripteur d'application : %s (src/app_desc.c pas lie ?)\n",
                  esp_app_get_description()->version);
  {
    esp_app_desc_t flashed;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_err_t err = running ? esp_ota_get_partition_description(running, &flashed) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK)
      Serial.printf("!! descripteur de l'image flashee illisible (%s)\n", esp_err_to_name(err));
    else if (strncmp(flashed.version, FW_VERSION_FULL, sizeof(flashed.version)))
      Serial.printf("!! descripteur de l'image flashee : %.*s (src/app_desc.c plus en tete de l'image ?)\n",
                    (int)sizeof(flashed.version), flashed.version);
  }

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
  // Le pilote reprend l'etat sauve et se met en ecoute passive (produit) ou en
  // veille (diagnostic) ; halo.begin() lui sert de relance du module.
  lamp.begin(halo.radio, HALO1_LISTEN_DEFAULT, []() { return halo.begin(); });
  jsonAttach();  // evenements du pilote et de la LED vers le mode machine
  {
    char st[48];
    Halo1Lamp::describe(lamp.target(), st, sizeof(st));
    Serial.printf("Lampe Halo 1 : %s, ecoute %s ('lampe' pour le detail)\n", st,
                  lamp.listening() ? "active" : "coupee");
  }

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
  lamp.tick();
#ifndef DIAG_ONLY
  matterBridgePoll();
#endif
  cliPoll();
  // Apres toute consigne (tick, Matter, CLI) et avant la LED : la livraison
  // precede l'eclat vert ou rouge (docs/PROTOCOLE-JSON.md, 12.2).
  jsonPoll();
  // Avant la LED, qui montre sa phase ; peut redemarrer la carte (seulement
  // bouton relache, broche relue haute : IO9 est une broche de strapping).
  bootButtonPoll();
  statusLedPoll();
  // Cede la main a IDLE et aux taches moins prioritaires : tick() tourne ainsi
  // a ~1 kHz, assez pour l'ecoute passive.
  delay(1);
}
