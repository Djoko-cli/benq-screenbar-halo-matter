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
