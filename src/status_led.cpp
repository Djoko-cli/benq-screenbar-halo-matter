#include "status_led.h"

// ===========================================================================
//  Logique pure (compilee aussi sur l'hote, sans ARDUINO)
// ===========================================================================

namespace statusled {

// Orange : le vert d'une WS2812 parait bien plus fort que son rouge.
static constexpr Rgb kBlue{0, 0, kMax}, kOrange{kMax, kMax / 4, 0}, kGreen{0, kMax, 0}, kRed{kMax, 0, 0};
// Violet : bleu plein, moitie de rouge (le magenta pur se confond avec le rouge
// a cette intensite). Blanc de l'eclat : bien plus vif que la lueur (8).
static constexpr Rgb kViolet{kMax / 2, 0, kMax}, kWhite{kMax, kMax, kMax};

// Ordre du tableau du README ; les motifs bornes (vert, rouge x3, eclat blanc)
// finissent sur du noir, qui separe les pas. Le rouge fixe suit la lueur (noir
// a sa fin) et precede le vert : colle au rouge x3 ou au rouge de depart de
// l'arc-en-ciel, il ne s'en distinguerait pas. Le motif du bouton tenu 8 s
// dure 2000 ms, un multiple de son tour (400 ms) : il finit sur un noir.
const TestStep kTest[kTestSteps] = {
    {Pattern::Unpaired, 3000},    {Pattern::Offline, 4000},      {Pattern::Online, 2000},
    {Pattern::RadioFault, 2000},  {Pattern::Delivered, 1000},    {Pattern::Unreachable, 2000},
    {Pattern::ButtonReboot, 1000}, {Pattern::ButtonUnpair, 2000}, {Pattern::Identify, 4000},
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
    case Pattern::ButtonUnpair:
      switch ((t / kUnpairStepMs) % 4) {
        case 0: return kRed;
        case 2: return kViolet;
        default: return Rgb{};
      }
    case Pattern::ButtonReboot: return t < kRebootFlashMs ? kWhite : Rgb{};
    case Pattern::Unreachable: return t < kUnreachableMs && (t / kRedHalfMs) % 2 == 0 ? kRed : Rgb{};
    case Pattern::RadioFault: return kRed;  // fixe : le seul motif qui ne clignote pas
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
  if (p == Pattern::ButtonUnpair) return (t / kUnpairMonoHalfMs) % 2 == 0;
  return render(p, t) != Rgb{};
}

const char *patternName(Pattern p) {
  switch (p) {
    case Pattern::Identify: return "identification (arc-en-ciel)";
    case Pattern::ButtonUnpair: return "bouton tenu 8 s : relacher pour desappairer (rouge/violet rapide)";
    case Pattern::ButtonReboot: return "bouton, appui court : redemarrage (eclat blanc)";
    case Pattern::Unreachable: return "lampe injoignable (rouge x3)";
    case Pattern::RadioFault: return "module radio en panne (rouge fixe)";
    case Pattern::Delivered: return "consigne livree (eclat vert)";
    case Pattern::Unpaired: return "pas mis en service (bleu clignotant)";
    case Pattern::Offline: return "reseau absent (orange lent)";
    case Pattern::Online: return "operationnel (lueur blanche toutes les 10 s)";
  }
  return "?";
}

const char *patternCode(Pattern p) {
  switch (p) {
    case Pattern::Identify: return "identification";
    case Pattern::ButtonUnpair: return "desappairage";
    case Pattern::ButtonReboot: return "redemarrage";
    case Pattern::Unreachable: return "injoignable";
    case Pattern::RadioFault: return "panne_radio";
    case Pattern::Delivered: return "livree";
    case Pattern::Unpaired: return "non_appaire";
    case Pattern::Offline: return "hors_reseau";
    case Pattern::Online: return "operationnel";
  }
  return "operationnel";
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

void Logic::setFault(bool on, uint32_t now) {
  if (on && !fault_) faultAt_ = now;
  fault_ = on;
}

Button buttonFor(bootbtn::Phase p) {
  switch (p) {
    case bootbtn::Phase::Armed:
    case bootbtn::Phase::Unpair: return Button::Unpair;
    case bootbtn::Phase::Reboot: return Button::Reboot;
    case bootbtn::Phase::Idle:
    case bootbtn::Phase::Held:
    case bootbtn::Phase::Locked: break;
  }
  return Button::None;
}

void Logic::setButton(Button b, uint32_t now) {
  if (b == button_) return;
  button_ = b;
  buttonAt_ = now;
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
  // Le bouton passe avant 'led test' : c'est une action de l'utilisateur.
  if (button_ != Button::None) {
    t = now - buttonAt_;
    return button_ == Button::Unpair ? Pattern::ButtonUnpair : Pattern::ButtonReboot;
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
  if (fault_) {
    t = now - faultAt_;
    return Pattern::RadioFault;
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

uint32_t effectEnd(uint32_t end, uint8_t effect, uint32_t now) {
  uint32_t ms = 2000;  // Blink, Okay, et tout effet inconnu
  switch (effect) {
    case kEffectStop: return 0;
    case kEffectFinish: {
      if (!effectPending(end, now)) return 0;
      const uint32_t soon = now + kEffectFinishMs;
      return effectPending(end, soon) ? (soon ? soon : 1) : end;
    }
    case kEffectBreathe: ms = 15000; break;
    case kEffectChannelChange: ms = 8000; break;
    default: break;
  }
  const uint32_t e = now + ms;
  return e ? e : 1;  // 0 veut dire "aucun effet"
}

}  // namespace statusled

// ===========================================================================
//  Cote carte
// ===========================================================================

#ifdef ARDUINO
#include <Arduino.h>
#include <string.h>

#include "boot_button.h"
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
static bool sFrameValid = false;                        // sFrame vient d'un tour de statusLedPoll()
static bool sShownValid = false;                        // quelque chose a deja ete ecrit
static StatusLedObserver sObserver = nullptr;
static uint32_t sNetAt = 0, sSeenDelivered = 0, sSeenGiveUps = 0;

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
static void monoWrite(bool on) { digitalWrite(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW ? !on : on); }
#endif

// N'ecrit que si ce qui se voit change : 16 ecritures par lueur de 600 ms
// toutes les 10 s, 25 par seconde pendant l'arc-en-ciel, sinon une par
// changement de motif ou demi-periode de clignotement.
static void show(const Frame &f) {
  const Pattern before = sFrame.p;
  const bool changed = sFrameValid && f.p != before;
  sFrame = f;
  sFrameValid = true;
  if (changed && sObserver) sObserver(f.p, before, sLed.testing());
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
  // En diagnostic, la LED ne sert a rien : on laisse la broche en entree.
  pinMode(PIN_STATUS_LED, INPUT);
#ifdef PIN_RGB_STATUS_LED
  // Une seule ecriture, au noir : la WS2812 garde sa derniere couleur a travers
  // un reset ou un flash (son alimentation ne coupe pas), et le bleu d'un build
  // produit resterait allume sur le banc. IO8 ne sert a rien d'autre ici, et le
  // demarrage est fini.
  rgbLedWriteOrdered(PIN_RGB_STATUS_LED, STATUS_RGB_ORDER, 0, 0, 0);
#endif
#else
#ifdef PIN_RGB_STATUS_LED
  // Un seul voyant, la WS2812. La LED simple d'IO15 reste en entree : eteinte
  // quelle que soit sa polarite (non verifiee), et sans conflit avec GDO2 du
  // CC2500 si la carte de capture est branchee (bogue B9).
  pinMode(PIN_STATUS_LED, INPUT);
  // IO8 est une broche de strapping, deja echantillonnee a ce stade : la
  // piloter depuis setup() ne gene pas le demarrage.
  rgbWrite(Rgb{});
#else
  // Sans WS2812, c'est la LED simple qui porte les motifs.
  pinMode(PIN_STATUS_LED, OUTPUT);
  monoWrite(false);
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
  sLed.setFault(lamp.moduleFault(), now);
  sLed.setButton(buttonFor(bootButtonPhase()), now);
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

void statusLedSetObserver(StatusLedObserver fn) {
#ifndef DIAG_ONLY
  sObserver = fn;
#else
  (void)fn;
#endif
}

bool statusLedState(Pattern *p, bool *testing) {
#ifndef DIAG_ONLY
  if (p) *p = sFrame.p;
  if (testing) *testing = sLed.testing();
  return true;
#else
  (void)p;
  (void)testing;
  return false;
#endif
}

void statusLedCommand(const char *arg) {
#ifdef DIAG_ONLY
  (void)arg;
  Serial.println("LED d'etat : aucune en build diagnostic (IO15 en entree, WS2812 mise au noir au demarrage).");
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
  Serial.printf("LED d'etat : WS2812 sur IO%d (LED simple IO%d en entree, eteinte)\n", PIN_RGB_STATUS_LED,
                PIN_STATUS_LED);
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
  Serial.printf("  pilote    : %lu consigne(s) livree(s), %lu abandon(s) depuis le demarrage ; module radio %s\n",
                (unsigned long)sSeenDelivered, (unsigned long)sSeenGiveUps,
                lamp.moduleFault() ? "EN PANNE ('lampe')" : "ok");
#endif
}

#endif  // ARDUINO
