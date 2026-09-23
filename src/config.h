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
// adaptees : 1 MHz est large pour 10 octets de payload et reste tres fiable.
#ifndef RF_SPI_HZ
#define RF_SPI_HZ 1000000UL
#endif

// ===========================================================================
//  Matter
// ===========================================================================
#ifndef FW_VERSION
#define FW_VERSION "0.2.0"
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
#define HALO1_GAP_MS 100               // telecommande ~100 ; seul 500 est prouve depuis l'ESP32
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
#define HALO1_AUTO_PULSE_MS 1000       // l'endpoint du bouton A revient a off apres 1 s
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
#ifndef HALO1_EXPOSE_AUTO
#define HALO1_EXPOSE_AUTO 1            // bouton A expose dans Matter (A2)
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

// ===========================================================================
//  Limites de la lampe (a reverifier sur le Halo 1re gen via la CLI)
// ===========================================================================
#define HALO_CT_MIN_K 2700
#define HALO_CT_MAX_K 6500
#define HALO_BRIGHT_MIN 1
#define HALO_BRIGHT_MAX 100

// ===========================================================================
//  Ordonnancement (tout est non bloquant : homeSpan.poll() doit continuer
//  a tourner, aucune operation RF ne doit depasser ~15 ms)
// ===========================================================================
#define HALO_POLL_INTERVAL_MS 5000    // interrogation d'etat au repos
#define HALO_COALESCE_MS 250          // regroupe les rafales de curseur (app domotique)
#define HALO_VERIFY_INTERVAL_MS 400   // cadence de verification de convergence
#define HALO_VERIFY_MAX_TRIES 12      // ~4,8 s : la lampe fait un fondu progressif
#define HALO_SETTLE_MS 1500           // delai avant de re-refleter l'etat vers Matter
#define HALO_ADOPT_MS 2500            // delai avant d'adopter un changement externe

// ===========================================================================
//  Endpoints Matter optionnels
//  Le bit 5 du registre de controle est documente comme "capteur ultrason" sur
//  le Halo 2. Sa signification sur le Halo 1 reste a confirmer (cf. docs/PROTOCOL.md).
// ===========================================================================
#define HALO_EXPOSE_SENSOR_SWITCH 1
#define HALO_EXPOSE_AUTO_SWITCH 1
