#include "boot_button.h"

// ===========================================================================
//  Logique pure (compilee aussi sur l'hote, sans ARDUINO)
// ===========================================================================

namespace bootbtn {

const char *phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "relache";
    case Phase::Held: return "tenu";
    case Phase::Armed: return "tenu 8 s (relacher = desappairer)";
    case Phase::Reboot: return "redemarrage en attente";
    case Phase::Unpair: return "desappairage en attente";
    case Phase::Locked: return "tenu au demarrage (ignore)";
  }
  return "?";
}

const char *eventName(Event e) {
  switch (e) {
    case Event::None: return "aucun";
    case Event::Armed: return "arme";
    case Event::Cancelled: return "annule";
    case Event::Unsure: return "incertain";
    case Event::Dropped: return "abandonne";
    case Event::BootReleased: return "relache apres le demarrage";
    case Event::Reboot: return "redemarrage";
    case Event::Unpair: return "desappairage";
  }
  return "?";
}

void Machine::begin(bool low, uint32_t now) {
  started_ = true;
  raw_ = stable_ = low;
  highValid_ = !low;
  lastNow_ = rawAt_ = highSince_ = pressAt_ = now;
  edgeGap_ = runGap_ = startGap_ = holdGap_ = 0;
  enter(low ? Phase::Locked : Phase::Idle, now);
}

Event Machine::update(bool low, uint32_t now) {
  if (!started_) {
    begin(low, now);
    return Event::None;
  }
  const uint32_t gap = now - lastNow_;
  lastNow_ = now;

  // Trous de releves (loop() bloquee). Un front est date a edgeGap_ pres ;
  // pendant un appui, tout trou a pu cacher un relachement et un nouvel appui.
  if (low != raw_) {
    raw_ = low;
    rawAt_ = now;
    edgeGap_ = gap;
    runGap_ = 0;
  } else if (gap > runGap_) {
    runGap_ = gap;
  }
  if (stable_ && gap > holdGap_) holdGap_ = gap;
  // Broche haute sans interruption, et sans trou de releves, depuis highSince_.
  if (low)
    highValid_ = false;
  else if (!highValid_ || gap > kMaxGapMs) {
    highValid_ = true;
    highSince_ = now;
  }

  // Anti-rebond : un nouveau niveau compte s'il a tenu kDebounceMs.
  if (raw_ != stable_ && now - rawAt_ >= kDebounceMs) {
    stable_ = raw_;
    return stable_ ? pressed(now) : released(now);
  }

  const bool settled = highValid_ && now - highSince_ >= kSettleMs;
  switch (phase_) {
    case Phase::Held:
      // Encore bas au releve de pressAt_ + kLongMs - 1, sans trou depuis le
      // premier releve bas : l'appui a dure au moins kLongMs (un releve couvre
      // sa milliseconde, comme dans lastPressMs_), c'est certain ; un trou
      // avant le premier releve bas ne ferait que l'allonger. Relache juste
      // apres, il mesure donc kLongMs ou plus.
      if (raw_ && holdGap_ <= kMaxGapMs && now - pressAt_ >= kLongMs - 1) {
        enter(Phase::Armed, now);
        return Event::Armed;
      }
      break;
    case Phase::Reboot:
      if (settled && now - phaseAt_ >= kRebootDelayMs) {
        enter(Phase::Idle, now);
        return Event::Reboot;
      }
      break;
    case Phase::Unpair:
      if (settled) {
        enter(Phase::Idle, now);
        return Event::Unpair;
      }
      break;
    default: break;
  }
  return Event::None;
}

Event Machine::pressed(uint32_t now) {
  pressAt_ = rawAt_;
  startGap_ = edgeGap_;
  holdGap_ = runGap_;
  const bool pending = phase_ == Phase::Reboot || phase_ == Phase::Unpair;
  enter(Phase::Held, now);
  return pending ? Event::Dropped : Event::None;
}

Event Machine::released(uint32_t now) {
  lastPressMs_ = rawAt_ - pressAt_;
  // Le trou du front de relachement et de son anti-rebond est dans holdGap_.
  lastGapMs_ = startGap_ > holdGap_ ? startGap_ : holdGap_;
  switch (phase_) {
    case Phase::Locked:
      enter(Phase::Idle, now);
      return Event::BootReleased;
    case Phase::Armed:  // tenu kLongMs, prouve a l'armement
      enter(Phase::Unpair, now);
      return Event::None;
    default: break;
  }
  if (lastGapMs_ > kMaxGapMs) {
    enter(Phase::Idle, now);
    return Event::Unsure;
  }
  if (lastPressMs_ < kShortMaxMs) {
    enter(Phase::Reboot, now);
    return Event::None;
  }
  // De 2000 a 7999 ms ; ou 8000 ms et plus sans releve bas assez tardif pour
  // armer (releves espaces, sans trou au-dela de kMaxGapMs) : annule.
  enter(Phase::Idle, now);
  return Event::Cancelled;
}

}  // namespace bootbtn

// ===========================================================================
//  Cote carte
// ===========================================================================

#ifdef ARDUINO
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <stdarg.h>
#include <stdio.h>

#include "config.h"
#include "halo1_lamp.h"
#include "json_mode.h"
#include "status_led.h"
#ifndef DIAG_ONLY
#include "matter_bridge.h"
#endif

using namespace bootbtn;

// L'eclat blanc doit etre fini, et la LED ecrite au noir (un tour de loop()
// au moins), avant le reset : la WS2812 garde sa couleur a travers un reset.
static_assert(kRebootDelayMs >= statusled::kRebootFlashMs + 50, "reset avant la fin de l'eclat blanc");

static constexpr uint32_t kGuardMaxMs = 1000;        // derniere garde : au-dela, action abandonnee
static constexpr uint32_t kShutdownHighMs = 50;      // garde de esp_restart() : broche haute de suite
static constexpr uint32_t kUnpairRestartMs = 10000;  // desappairage sans redemarrage : on redemarre

static Machine sBtn;
// Desappairage demande (tache loop seulement) : bouton inerte, LED
// rouge/violet, jusqu'au redemarrage par la tache CHIP.
static bool sUnpairing = false;
static uint32_t sUnpairAt = 0;
// Un esp_restart() est entre dans waitBootHigh() (n'importe quelle tache).
static bool sRestarting = false;

static bool pinLow() { return digitalRead(PIN_DECOMMISSION_BTN) == LOW; }

// Annonce d'une action du bouton : message log (src bouton) en mode
// 'json log 1', sinon une ligne de texte, perdue plutot que d'attendre un
// hote qui ne lit pas (comme le journal du pont).
static void announce(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void announce(const char *fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
  if (jsonLog("bouton", "notice", line)) return;
  if (Serial.availableForWrite() < n + 2) return;
  Serial.println(line);
}

// Derniere garde juste avant un reset : la broche relue haute sans
// interruption pendant kSettleMs (le temps aussi de laisser partir
// l'annonce). false si le bouton est rappuye et tenu : mieux vaut abandonner
// que redemarrer broche basse (mode telechargement) ou bloquer loop().
static bool pinSettled() {
  const uint32_t t0 = millis();
  uint32_t highSince = t0;
  for (;;) {
    const uint32_t now = millis();
    if (pinLow())
      highSince = now;
    else if (now - highSince >= kSettleMs)
      return true;
    if (now - t0 >= kGuardMaxMs) return false;
    delay(1);
  }
}

// Garde de TOUS les resets logiciels (esp_restart() : bouton, 'reboot',
// 'decommission', et surtout la fin du desappairage : esp_matter::
// factory_reset() efface l'espace NVS du noeud puis confie la suite a la
// tache CHIP, qui retire les fabriques, efface le reseau et appelle
// esp_restart() bien apres matterDecommissionNow(), bouton libre entre-temps).
// Pas de reset tant qu'IO9 n'a pas ete relue haute kShutdownHighMs de suite.
// esp_restart() appelle ses gestionnaires du dernier enregistre au premier :
// enregistre avant le Wi-Fi et Matter, celui-ci passe apres l'arret du Wi-Fi,
// juste avant le reset (seule la synchro de l'horloge RTC, enregistree avant
// setup(), le suit). Sans borne : un bouton coince bloque la carte dans le
// firmware (le relacher la redemarre) au lieu de la laisser en mode
// telechargement. vTaskDelay laisse tourner les autres taches, et la tache
// qui attend nourrit le chien de garde si elle y est inscrite (5 s, panique :
// un reset sans ces gestionnaires).
static void waitBootHigh() {
  __atomic_store_n(&sRestarting, true, __ATOMIC_RELAXED);
  // Hors d'une tache (interruption, ordonnanceur arrete) : attente impossible.
  if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return;
  const bool wdt = esp_task_wdt_status(nullptr) == ESP_OK;
  bool told = false;
  int64_t highSince = esp_timer_get_time();
  for (;;) {
    const int64_t now = esp_timer_get_time();
    if (gpio_get_level((gpio_num_t)PIN_DECOMMISSION_BTN) == 0) {
      highSince = now;
      // Texte seul (le mode machine le tolere) : jsonLog() n'appartient qu'a
      // la tache loop, et c'est peut-etre la tache CHIP qui attend ici.
      if (!told && Serial.availableForWrite() >= 96) {
        told = true;
        Serial.println("[bouton] tenu pendant un redemarrage : reset au relachement (IO9, broche de strapping)");
      }
    } else if (now - highSince >= (int64_t)kShutdownHighMs * 1000) {
      return;
    }
    if (wdt) esp_task_wdt_reset();
    vTaskDelay(1);
  }
}

void bootButtonBegin() {
  pinMode(PIN_DECOMMISSION_BTN, INPUT_PULLUP);
  // Avant netBegin() et matterBridgeBegin() (voir waitBootHigh). Cinq places
  // seulement : un echec se dit.
  const esp_err_t err = esp_register_shutdown_handler(waitBootHigh);
  if (err != ESP_OK)
    Serial.printf("!! bouton BOOT : garde du reset non enregistree (%s) ; ne pas tenir BOOT pendant un redemarrage\n",
                  esp_err_to_name(err));
}

// Pendant le desappairage, la LED garde le motif rouge/violet jusqu'au reset.
bootbtn::Phase bootButtonPhase() { return sUnpairing ? Phase::Unpair : sBtn.phase(); }

void bootButtonPoll() {
  if (sUnpairing) {
    // La tache CHIP efface puis redemarre : le bouton est inerte (un reset
    // en plein effacement laisserait Matter a moitie retire). Filet si rien
    // ne redemarre : Matter.decommission() ne rend rien, un echec est muet.
    if (__atomic_load_n(&sRestarting, __ATOMIC_RELAXED) || millis() - sUnpairAt < kUnpairRestartMs) return;
    announce("[bouton] toujours en marche %lu s apres le desappairage : redemarrage",
             (unsigned long)(kUnpairRestartMs / 1000));
    if (pinSettled()) ESP.restart();
    sUnpairAt = millis();  // bouton tenu : nouvel essai plus tard
    return;
  }
  const Event e = sBtn.update(pinLow(), millis());
  const unsigned long held = sBtn.lastPressMs();
  switch (e) {
    case Event::None: return;
    case Event::Armed:
#ifdef DIAG_ONLY
      announce("[bouton] tenu 8 s : build diagnostic sans Matter, relacher ne desappairera rien");
#else
      announce("[bouton] tenu 8 s : relacher pour desappairer (retrait de Matter)");
#endif
      return;
    case Event::Cancelled: announce("[bouton] appui de %lu ms (2 a 8 s) : annule, rien fait", held); return;
    case Event::Unsure:
      announce("[bouton] appui de %lu ms ignore : releves interrompus %lu ms (loop() bloquee), duree incertaine",
               held, (unsigned long)sBtn.lastGapMs());
      return;
    case Event::Dropped: announce("[bouton] nouvel appui : action en attente abandonnee"); return;
    case Event::BootReleased: announce("[bouton] relache : il etait tenu au demarrage, ignore"); return;
    case Event::Reboot:
      announce("[bouton] appui court (%lu ms) : redemarrage", held);
      lamp.persistNow();  // comme 'reboot' : etat cru de la lampe, sans attendre l'echeance
      if (!pinSettled()) break;
      ESP.restart();
      return;
    case Event::Unpair:
#ifdef DIAG_ONLY
      announce("[bouton] appui long (%lu ms) : build diagnostic sans Matter, rien a desappairer (rien fait)", held);
      return;
#else
      announce("[bouton] appui long (%lu ms) : retrait de toutes les fabriques Matter, puis redemarrage", held);
      lamp.persistNow();
      if (!pinSettled()) break;
      // esp_matter::factory_reset() efface l'espace NVS du noeud, puis la
      // tache CHIP retire les fabriques, efface le reseau et redemarre, un
      // moment plus tard (waitBootHigh garde ce reset-la aussi).
      sUnpairing = true;
      sUnpairAt = millis();
      matterDecommissionNow();
      return;
#endif
  }
  // Rappuye pendant la derniere garde : pas de reset broche basse. S'il est
  // encore tenu, il est ignore jusqu'a son relachement.
  announce("[bouton] rappuye juste avant le reset : action annulee");
  sBtn.begin(pinLow(), millis());
}

#endif  // ARDUINO
