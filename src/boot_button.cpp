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

static constexpr uint32_t kGuardMaxMs = 1000;  // derniere garde : au-dela, action abandonnee

static Machine sBtn;

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

void bootButtonBegin() { pinMode(PIN_DECOMMISSION_BTN, INPUT_PULLUP); }

bootbtn::Phase bootButtonPhase() { return sBtn.phase(); }

void bootButtonPoll() {
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
      // esp_matter::factory_reset() : efface, puis redemarre depuis la tache
      // CHIP, quelques millisecondes plus tard.
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
