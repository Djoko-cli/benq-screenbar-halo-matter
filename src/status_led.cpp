#include "status_led.h"

// ===========================================================================
//  Logique pure (compilee aussi sur l'hote, sans ARDUINO)
// ===========================================================================

namespace statusled {

// Orange : le vert d'une WS2812 parait bien plus fort que son rouge.
static constexpr Rgb kBlue{0, 0, kMax}, kOrange{kMax, kMax / 4, 0}, kGreen{0, kMax, 0}, kRed{kMax, 0, 0};

// Ordre du tableau du README ; les motifs bornes (vert, rouge) finissent sur du
// noir, qui separe les pas.
const TestStep kTest[kTestSteps] = {
    {Pattern::Unpaired, 3000},  {Pattern::Offline, 4000},     {Pattern::Online, 2000},
    {Pattern::Delivered, 1000}, {Pattern::Unreachable, 2000}, {Pattern::Identify, 4000},
};

uint32_t testTotalMs() {
  uint32_t total = 0;
  for (const TestStep &s : kTest) total += s.ms;
  return total;
}

Rgb wheel(uint16_t hue) {
  hue %= 768;
  const uint8_t up = (uint8_t)((hue % 256) * kMax / 255), down = (uint8_t)(kMax - up);
  switch (hue / 256) {
    case 0: return Rgb{down, up, 0};
    case 1: return Rgb{0, down, up};
    default: return Rgb{up, 0, down};
  }
}

Rgb render(Pattern p, uint32_t t) {
  switch (p) {
    case Pattern::Identify: {
      // Par pas de kStepMs : la couleur ne change que 25 fois par seconde.
      const uint32_t phase = t / kStepMs * kStepMs % kRainbowMs;
      return wheel((uint16_t)(phase * 768 / kRainbowMs));
    }
    case Pattern::Unreachable: return t < kUnreachableMs && (t / kRedHalfMs) % 2 == 0 ? kRed : Rgb{};
    case Pattern::Delivered: return t < kDeliveredMs ? kGreen : Rgb{};
    case Pattern::Unpaired: return (t / kUnpairedHalfMs) % 2 == 0 ? kBlue : Rgb{};
    case Pattern::Offline: return (t / kOfflineHalfMs) % 2 == 0 ? kOrange : Rgb{};
    case Pattern::Online: {
      // Triangle 0 -> kGlowMax -> 0 sur kGlowMs, en tete de chaque periode :
      // une lueur des le passage en ligne, puis toutes les 10 s.
      const uint32_t ph = t % kGlowPeriodMs, half = kGlowMs / 2;
      if (ph >= kGlowMs) return Rgb{};
      const uint32_t x = ph < half ? ph : kGlowMs - ph;
      const uint8_t v = (uint8_t)((kGlowMax * x + half / 2) / half);
      return Rgb{v, v, v};
    }
  }
  return Rgb{};
}

bool renderMono(Pattern p, uint32_t t) {
  if (p == Pattern::Online) return false;  // une LED simple ne sait pas luire doucement
  if (p == Pattern::Identify) return (t / kIdentifyMonoHalfMs) % 2 == 0;
  return render(p, t) != Rgb{};
}

const char *patternName(Pattern p) {
  switch (p) {
    case Pattern::Identify: return "identification (arc-en-ciel)";
    case Pattern::Unreachable: return "lampe injoignable (rouge x3)";
    case Pattern::Delivered: return "consigne livree (eclat vert)";
    case Pattern::Unpaired: return "pas mis en service (bleu clignotant)";
    case Pattern::Offline: return "reseau absent (orange lent)";
    case Pattern::Online: return "operationnel (lueur blanche toutes les 10 s)";
  }
  return "?";
}

void Logic::setNet(Net n, uint32_t now) {
  if (n == net_) return;
  net_ = n;
  netAt_ = now;
}

void Logic::setIdentify(bool on, uint32_t now) {
  if (on && !identify_) identAt_ = now;
  identify_ = on;
}

void Logic::delivered(uint32_t now) {
  green_ = true;
  greenAt_ = now;
}

void Logic::unreachable(uint32_t now) {
  red_ = true;
  redAt_ = now;
}

void Logic::startTest(uint32_t now) {
  testing_ = true;
  testAt_ = now;
}

Pattern Logic::pick(uint32_t now, uint32_t &t) {
  // Echeances d'abord, quel que soit le motif affiche : un evenement masque
  // par un plus prioritaire s'oublie quand meme a l'heure.
  if (red_ && now - redAt_ >= kUnreachableMs) red_ = false;
  if (green_ && now - greenAt_ >= kDeliveredMs) green_ = false;
  if (testing_ && now - testAt_ >= testTotalMs()) testing_ = false;

  if (identify_) {
    t = now - identAt_;
    return Pattern::Identify;
  }
  if (testing_) {
    uint32_t e = now - testAt_;
    for (const TestStep &s : kTest) {
      if (e < s.ms) {
        t = e;
        return s.p;
      }
      e -= s.ms;
    }
  }
  if (red_) {
    t = now - redAt_;
    return Pattern::Unreachable;
  }
  if (green_) {
    t = now - greenAt_;
    return Pattern::Delivered;
  }
  t = now - netAt_;
  switch (net_) {
    case Net::Unpaired: return Pattern::Unpaired;
    case Net::Offline: return Pattern::Offline;
    case Net::Online: break;
  }
  return Pattern::Online;
}

Frame Logic::frame(uint32_t now) {
  uint32_t t = 0;
  const Pattern p = pick(now, t);
  return Frame{p, render(p, t), renderMono(p, t)};
}

}  // namespace statusled

// ===========================================================================
//  Cote carte
// ===========================================================================

#ifdef ARDUINO
#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "halo1_lamp.h"
#ifndef DIAG_ONLY
#include "matter_bridge.h"
#endif

using namespace statusled;

#ifndef DIAG_ONLY
static constexpr uint32_t kNetSampleMs = 200;  // etat Matter releve 5 fois par seconde, pas a chaque tour
static Logic sLed;
static Frame sFrame{Pattern::Unpaired, Rgb{}, false};  // image du dernier tour ('led')
static bool sShownValid = false;                        // quelque chose a deja ete ecrit
static uint32_t sNetAt = 0, sSeenDelivered = 0, sSeenGiveUps = 0;

static void monoWrite(bool on) { digitalWrite(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW ? !on : on); }

#ifdef PIN_RGB_STATUS_LED
static Rgb sShown;  // couleur ecrite
// rgbLedWriteOrdered (core 3.x, esp32-hal-rgb-led) : 24 bits par le RMT,
// ~30 us d'emission. Le canal RMT est cree au premier appel, puis reutilise.
static void rgbWrite(Rgb c) {
  rgbLedWriteOrdered(PIN_RGB_STATUS_LED, STATUS_RGB_ORDER, c.r, c.g, c.b);
  sShown = c;
  sShownValid = true;
}
#else
static bool sShownOn = false;  // etat ecrit de la LED simple
#endif

// N'ecrit que si ce qui se voit change : quelques ecritures par seconde au
// plus hors Identify (25 par seconde pendant l'arc-en-ciel).
static void show(const Frame &f) {
  sFrame = f;
#ifdef PIN_RGB_STATUS_LED
  if (!sShownValid || f.c != sShown) rgbWrite(f.c);
#else
  if (sShownValid && f.mono == sShownOn) return;
  monoWrite(f.mono);
  sShownOn = f.mono;
  sShownValid = true;
#endif
}
#endif

void statusLedBegin() {
#ifdef DIAG_ONLY
  // IO15 porte aussi GDO2 du CC2500 sur la carte de capture : le piloter en
  // sortie mettrait deux sorties en conflit sur le meme fil (audit, bogue B9).
  // En diagnostic, la LED ne sert a rien : on laisse la broche en entree, et la
  // WS2812 n'est jamais pilotee.
  pinMode(PIN_STATUS_LED, INPUT);
#else
  // Avec une WS2812, la LED simple d'IO15 reste eteinte (sortie basse, comme
  // avant) : un seul voyant, la WS2812. Sans WS2812, c'est elle qui clignote.
  pinMode(PIN_STATUS_LED, OUTPUT);
  monoWrite(false);
#ifdef PIN_RGB_STATUS_LED
  // IO8 est une broche de strapping, deja echantillonnee a ce stade : la
  // piloter depuis setup() ne gene pas le demarrage.
  rgbWrite(Rgb{});
#endif
  sNetAt = millis() - kNetSampleMs;  // releve au premier tour de loop()
#endif
}

void statusLedPoll() {
#ifndef DIAG_ONLY
  const uint32_t now = millis();
  if (now - sNetAt >= kNetSampleMs) {
    sNetAt = now;
    sLed.setNet(!matterIsCommissioned() ? Net::Unpaired : !matterIsConnected() ? Net::Offline : Net::Online, now);
  }
  sLed.setIdentify(matterIdentifying(), now);
  const uint32_t delivered = lamp.deliveredCount(), giveUps = lamp.giveUpCount();
  if (delivered != sSeenDelivered) {
    sSeenDelivered = delivered;
    sLed.delivered(now);
  }
  if (giveUps != sSeenGiveUps) {
    sSeenGiveUps = giveUps;
    sLed.unreachable(now);
  }
  show(sLed.frame(now));
#endif
}

void statusLedCommand(const char *arg) {
#ifdef DIAG_ONLY
  (void)arg;
  Serial.println("LED d'etat : aucune en build diagnostic (IO15 en entree, WS2812 jamais pilotee).");
#else
  if (!strcmp(arg, "test")) {
    sLed.startTest(millis());
    Serial.printf("Test de la LED, %lu s ('led stop' pour l'arreter) :\n", (unsigned long)(testTotalMs() / 1000));
    uint32_t at = 0;
    for (const TestStep &s : kTest) {
      Serial.printf("  %2lu s  %s\n", (unsigned long)(at / 1000), patternName(s.p));
      at += s.ms;
    }
    return;
  }
  if (!strcmp(arg, "stop")) {
    sLed.stopTest();
    Serial.println("Test de la LED arrete.");
    return;
  }
  if (*arg) {
    Serial.println("Format attendu : led [test|stop]");
    return;
  }
#ifdef PIN_RGB_STATUS_LED
  Serial.printf("LED d'etat : WS2812 sur IO%d (LED simple IO%d eteinte)\n", PIN_RGB_STATUS_LED, PIN_STATUS_LED);
#else
  Serial.printf("LED d'etat : LED simple sur IO%d\n", PIN_STATUS_LED);
#endif
  Serial.printf("  motif     : %s%s\n", patternName(sFrame.p), sLed.testing() ? " -- test en cours" : "");
#ifdef PIN_RGB_STATUS_LED
  Serial.printf("  affiche   : R %u V %u B %u (plafond %u par canal)\n", (unsigned)sShown.r, (unsigned)sShown.g,
                (unsigned)sShown.b, (unsigned)kMax);
#else
  Serial.printf("  affiche   : %s\n", sShownOn ? "allumee" : "eteinte");
#endif
  Serial.printf("  pilote    : %lu consigne(s) livree(s), %lu abandon(s) depuis le demarrage\n",
                (unsigned long)sSeenDelivered, (unsigned long)sSeenGiveUps);
#endif
}

#endif  // ARDUINO
