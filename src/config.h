#pragma once
#include <Arduino.h>

// ===========================================================================
//  Cablage SPI vers le module BM5602-60-1
//  (les valeurs par defaut sont surchargees par build_flags dans platformio.ini)
// ===========================================================================
#ifndef PIN_RF_SCK
#define PIN_RF_SCK 18
#endif
#ifndef PIN_RF_MOSI          // -> broche SDIO du BM5602
#define PIN_RF_MOSI 23
#endif
#ifndef PIN_RF_MISO          // <- broche GIO2 du BM5602 (mode 4 fils)
#define PIN_RF_MISO 19
#endif
#ifndef PIN_RF_CSN
#define PIN_RF_CSN 5
#endif
// Broches d'ecoute du bus SPI d'un appareil tiers (commande 'sniffspi').
// MISO n'existe pas ici : on n'ecoute que le sens microcontroleur -> puce,
// et surtout on ne pilote JAMAIS une ligne du bus observe.
// Dialogue SWD avec un microcontroleur tiers. SWCLK est connu (trou 3 de J5,
// seul a se tenir pres de 0 V par son tirage interne) ; SWDIO est cherche
// parmi plusieurs broches, pour ne pas avoir a recabler entre chaque essai.
// GIO3 du module (pastille 8), non cablee d'origine. Contrairement a GIO2,
// elle ne sert pas au SPI : on peut donc balayer son selecteur sans perdre
// la liaison avec la puce, et compter les trames recues pendant la mesure.
#ifndef PIN_GIO3_TAP
#define PIN_GIO3_TAP 3

#endif

// --- Module CC2500 (24TRGC5-V4), sur sa propre carte -----------------------
// Valeurs de depart seulement : la commande 'ccpins' les change a chaud, sans
// reflasher, parce qu'on ne sait pas d'avance quelles broches la carte expose.
#ifndef PIN_CC_SCK
#define PIN_CC_SCK 18
#endif
#ifndef PIN_CC_MISO
#define PIN_CC_MISO 19
#endif
#ifndef PIN_CC_MOSI
#define PIN_CC_MOSI 20
#endif
#ifndef PIN_CC_CSN
#define PIN_CC_CSN 14
#endif
#ifndef PIN_CC_GDO0
#define PIN_CC_GDO0 3
#endif
#ifndef PIN_CC_GDO2
#define PIN_CC_GDO2 15
#endif
#ifndef PIN_CC_PA_EN
#define PIN_CC_PA_EN 7
#endif
#ifndef PIN_CC_RX_EN
#define PIN_CC_RX_EN 6
#endif

#ifndef PIN_SWD_CLK
#define PIN_SWD_CLK 18
#endif

#ifndef PIN_TAP_SCK
#define PIN_TAP_SCK 6
#endif
#ifndef PIN_TAP_MOSI
#define PIN_TAP_MOSI 7
#endif
#ifndef PIN_TAP_CS
#define PIN_TAP_CS 11
#endif

#ifndef PIN_STATUS_LED
#define PIN_STATUS_LED 2
#endif

// Le datasheet BC5602 autorise plus, mais le module a des pistes courtes non
// adaptees : 1 MHz est largement suffisant et reste tres fiable.
#ifndef RF_SPI_HZ
#define RF_SPI_HZ 1000000UL
#endif

// ===========================================================================
//  Matter
// ===========================================================================
// FW_VERSION, FW_GIT_REV et FW_VERSION_FULL : fw_version.h (platformio.ini).
#include "fw_version.h"

// Identite du noeud (Basic Information, EP0), decisions de Majid du 23/09.
// Posee a chaque demarrage avant Matter.begin() (matter_bridge.cpp) ; le VID et
// le PID restent ceux du certificat de test (0xFFF1 / 0x8000) : les changer
// casserait l'appairage. Chaines de 32 caracteres au plus (64 pour la version
// materielle), verifie a la compilation. Numero de serie : prefixe + adresse
// MAC d'usine en 12 chiffres hexa, unique par carte. La version logicielle
// ("Programme interne") est FW_VERSION_FULL, via src/app_desc.c.
#ifndef MATTER_VENDOR_NAME
#define MATTER_VENDOR_NAME "Djoko-CLI"
#endif
#ifndef MATTER_PRODUCT_NAME
#define MATTER_PRODUCT_NAME "Pont ScreenBar Halo"
#endif
#ifndef MATTER_NODE_LABEL
#define MATTER_NODE_LABEL "Halo"              // NodeLabel, reecrit a chaque demarrage
#endif
#ifndef MATTER_HW_VERSION
#define MATTER_HW_VERSION 1
#endif
#ifndef MATTER_HW_VERSION_STRING
#define MATTER_HW_VERSION_STRING "ESP32-C6 SuperMini + BM5602"  // montage de reference
#endif
#ifndef MATTER_SERIAL_PREFIX
#define MATTER_SERIAL_PREFIX "HALO1-"
#endif

// Appui long sur ce bouton = retrait de toutes les fabriques Matter
// (decommissioning), pour re-appairer l'accessoire de zero.
#ifndef PIN_DECOMMISSION_BTN
#ifdef BOOT_PIN
#define PIN_DECOMMISSION_BTN BOOT_PIN
#else
#define PIN_DECOMMISSION_BTN 0
#endif
#endif
#define DECOMMISSION_HOLD_MS 5000

// Certaines cartes cablent la LED d'etat a l'envers (broche -> LED -> 3V3).
#ifndef STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW 0
#endif

// LED d'etat RGB adressable (WS2812) : PIN_RGB_STATUS_LED, donnee par
// platformio.ini (IO8 sur le C6 SuperMini). Definie, c'est elle le voyant, et
// la LED simple de PIN_STATUS_LED reste en entree ; sinon la LED simple suit les
// memes motifs en tout ou rien (status_led.h). Ordre des couleurs : GRB, celui
// de la WS2812B ; si 'led test' montre du rouge au lieu du vert, passer a
// LED_COLOR_ORDER_RGB.
#ifndef STATUS_RGB_ORDER
#define STATUS_RGB_ORDER LED_COLOR_ORDER_GRB
#endif

// ===========================================================================
//  Radio : canaux observes dans le dossier FCC (JVPCR20CCTR / JVPCR20C)
//  Valeur du registre RFCH = frequence(MHz) - 2400
// ===========================================================================
#define RF_CHANNEL_1 5    // 2405 MHz - canal par defaut du BenQ
#define RF_CHANNEL_2 46   // 2446 MHz
#define RF_CHANNEL_3 75   // 2475 MHz

// ===========================================================================
//  Halo 1 : pilote produit (prouve, ou reglable au banc via 'lampe')
//  Chaque valeur se surcharge par -D dans platformio.ini.
// ===========================================================================
#ifndef HALO1_REPEATS
#define HALO1_REPEATS 3                // comme la telecommande ; une trame seule a ete ignoree
#endif
#ifndef HALO1_MIN_ACKS
#define HALO1_MIN_ACKS 2               // accuses exiges pour qu'une trame soit livree
#endif
#ifndef HALO1_MAX_ATTEMPTS
#define HALO1_MAX_ATTEMPTS 5           // paquets au plus par trame, a chaque tour
#endif
#ifndef HALO1_GAP_MS
#define HALO1_GAP_MS 100               // telecommande ~100 ; 100 ms prouve au banc (T1, 23/09)
#endif
#ifndef HALO1_RETRY_MS
#define HALO1_RETRY_MS 1000            // reprise apres un echec : 1 s x rang de l'echec
#endif
#ifndef HALO1_PLAN_RETRIES
#define HALO1_PLAN_RETRIES 2           // reprises avant d'abandonner la consigne
#endif
#ifndef HALO1_RESET_WAIT_MS
#define HALO1_RESET_WAIT_MS 40         // 2 x 20 ms du chemin prouve (configStdAutoAck)
#endif
#ifndef HALO1_RX_REARM_MS
#define HALO1_RX_REARM_MS 100          // rearmement de l'ecoute, comme 'ecoute'
#endif
#ifndef HALO1_RX_SILENCE_MS
#define HALO1_RX_SILENCE_MS 500        // reconfiguration apres ce silence, comme 'ecoute'
#endif
#ifndef HALO1_REMOTE_HOLDOFF_MS
#define HALO1_REMOTE_HOLDOFF_MS 250    // pas de rafale juste apres une trame de la telecommande...
#endif
#ifndef HALO1_REMOTE_HOLDOFF_MAX_MS
#define HALO1_REMOTE_HOLDOFF_MAX_MS 2000  // ... sauf pour une demande qui attend depuis 2 s
#endif
#ifndef HALO1_SELECTION_STABLE_MS
#define HALO1_SELECTION_STABLE_MS 2000 // selection de lampes retenue apres 2 s allumee
#endif
#ifndef HALO1_PERSIST_DELAY_MS
#define HALO1_PERSIST_DELAY_MS 10000   // etat cru sauve 10 s apres son dernier changement...
#endif
#ifndef HALO1_PERSIST_MAX_MS
#define HALO1_PERSIST_MAX_MS 60000     // ... et jamais plus de 60 s apres le premier
#endif
#ifndef HALO1_COALESCE_QUIET_MS
#define HALO1_COALESCE_QUIET_MS 120    // pont Matter : ordres regroupes jusqu'a 120 ms de calme...
#endif
#ifndef HALO1_COALESCE_MAX_MS
#define HALO1_COALESCE_MAX_MS 400      // ... ou 400 ms au plus
#endif
#ifndef HALO1_REFLECT_MIN_MS
#define HALO1_REFLECT_MIN_MS 250       // ecart minimal entre deux reflets vers Matter
#endif
#ifndef HALO1_AUTO_PULSE_MS
#define HALO1_AUTO_PULSE_MS 1000       // EP4 (si expose) revient a off apres 1 s ; 'matter impulsion' en NVS
#endif
#ifndef HALO1_BOOT_IGNORE_MS
#define HALO1_BOOT_IGNORE_MS 2000      // ordres Matter ignores juste apres le demarrage
#endif
#ifndef HALO1_LEVEL_GAMMA
#define HALO1_LEVEL_GAMMA 2.0f         // niveau Matter -> luminosite brute (decision A3)
#endif
#ifndef HALO1_SELECTORS_AS_LIGHTS
#define HALO1_SELECTORS_AS_LIGHTS 1    // lampes avant/arriere : lumieres (1) ou prises (0), A1
#endif
// Bouton A dans Matter (A2) : EP4 "Halo auto". Desactive le 23/09 (decision de
// Majid) : ni endpoint, ni miroir des A de la telecommande, ni 'matter
// impulsion'. Code garde : -DHALO1_EXPOSE_AUTO=1 le remet (README). EP1 a EP3
// gardent leurs numeros, EP4 etant cree en dernier.
#ifndef HALO1_EXPOSE_AUTO
#define HALO1_EXPOSE_AUTO 0
#endif
#ifndef HALO1_AIR_GUARD_WAIT_US
#define HALO1_AIR_GUARD_WAIT_US 6000   // garde Thread : fin d'une trame 802.15.4 deja partie (4,3 ms + accuse)
#endif
#ifndef HALO1_LISTEN_DEFAULT
#ifdef DIAG_ONLY
#define HALO1_LISTEN_DEFAULT false     // diag : aucune activite radio de fond
#else
#define HALO1_LISTEN_DEFAULT true      // produit : suivre la telecommande
#endif
#endif
