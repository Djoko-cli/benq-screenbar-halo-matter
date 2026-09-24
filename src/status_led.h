#pragma once
#include <stdint.h>

#include "boot_button.h"

// ===========================================================================
//  LED d'etat du produit (signature validee le 23/09)
//
//  WS2812 de la carte (IO8 sur le C6 SuperMini), builds produit seulement :
//    - pas mis en service         : bleu clignotant (250 ms / 250 ms)
//    - mis en service, sans reseau : orange lent (1 s / 1 s)
//    - tout va bien               : eteinte, breve lueur blanche toutes les 10 s
//    - consigne livree a la lampe : eclat vert (150 ms)
//    - lampe injoignable (abandon): rouge, 3 clignements
//    - module radio en panne      : rouge fixe, tant que dure la panne (relances
//      automatiques sans effet, ou module perdu : Halo1Lamp::moduleFault())
//    - Identify (Apple Home)      : arc-en-ciel pendant toute l'identification
//    - bouton BOOT tenu 8 s       : rouge, noir, violet, noir (100 ms chacun)
//      tant qu'on tient ("relache pour desappairer"), puis pendant le
//      desappairage, jusqu'au redemarrage (src/boot_button.h)
//    - bouton BOOT, appui court   : eclat blanc (150 ms), noir, redemarrage
//  Priorite : Identify > bouton > rouge x3 > rouge fixe > vert > etat du
//  reseau ('led test' passe sous le bouton). Les trois clignements restent
//  visibles sur le rouge fixe (noir entre eux).
//  Intensite basse (elle est sous un bureau) : 24/255 au plus par canal, 8/255
//  pour la lueur.
//
//  Sans WS2812 declaree (PIN_RGB_STATUS_LED), la LED simple de PIN_STATUS_LED
//  suit les memes motifs en tout ou rien, sans la lueur (esp32dev : IO2). Les
//  DevKit C3 et S3 n'ont pas de LED simple (IO8 et IO48 y portent leur WS2812,
//  non declaree) : pas de voyant visible. Build diagnostic : la WS2812 est
//  seulement mise au noir au demarrage, IO15 reste en entree.
//
//  La logique (statusled::) est pure et sans Arduino : testee sur l'hote
//  (tools/host_tests). Toutes les durees sont des ecarts non signes a partir
//  d'un instant, et les evenements echus sont oublies a chaque appel : le
//  retour a zero de millis() ne ranime rien.
// ===========================================================================

namespace statusled {

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
};
inline bool operator==(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
inline bool operator!=(Rgb a, Rgb b) { return !(a == b); }

enum class Net : uint8_t { Unpaired, Offline, Online };

// Motifs, du plus prioritaire au moins prioritaire. ButtonUnpair et
// ButtonReboot (bouton BOOT) ne coexistent jamais.
enum class Pattern : uint8_t {
  Identify,
  ButtonUnpair,
  ButtonReboot,
  Unreachable,
  RadioFault,
  Delivered,
  Unpaired,
  Offline,
  Online
};

// Ce que le bouton BOOT demande a la LED.
enum class Button : uint8_t { None, Unpair, Reboot };
// D'apres sa phase : Armed et Unpair -> Unpair, Reboot -> Reboot, les autres
// (Idle, Held, Locked) -> rien.
Button buttonFor(bootbtn::Phase p);

constexpr uint8_t kMax = 24;                 // plafond par canal
constexpr uint8_t kGlowMax = 8;              // sommet de la lueur blanche
constexpr uint32_t kUnpairedHalfMs = 250;    // bleu : demi-periode
constexpr uint32_t kOfflineHalfMs = 1000;    // orange : demi-periode
constexpr uint32_t kGlowPeriodMs = 10000;    // une lueur toutes les 10 s...
constexpr uint32_t kGlowMs = 600;            // ... de 600 ms (montee puis descente)
constexpr uint32_t kDeliveredMs = 150;       // eclat vert
constexpr uint32_t kRedHalfMs = 200;         // rouge : 200 ms allume, 200 eteint...
constexpr uint32_t kRedBlinks = 3;           // ... trois fois
constexpr uint32_t kUnreachableMs = kRedBlinks * 2 * kRedHalfMs;
constexpr uint32_t kRainbowMs = 2000;        // un tour de roue
constexpr uint32_t kStepMs = 40;             // pas de l'arc-en-ciel (25 images/s)
constexpr uint32_t kIdentifyMonoHalfMs = 100;  // LED simple : clignotement rapide
constexpr uint32_t kUnpairStepMs = 100;        // bouton tenu 8 s : rouge, noir, violet, noir...
constexpr uint32_t kUnpairMonoHalfMs = 50;     // ... LED simple : clignotement tres rapide
constexpr uint32_t kRebootFlashMs = 150;       // appui court : eclat blanc avant le redemarrage

// Couleur d'un motif, t ms apres son depart. Les motifs bornes (vert, rouge)
// rendent du noir une fois finis.
Rgb render(Pattern p, uint32_t t);
// Meme chose pour une LED simple : pas de lueur, clignotement rapide pour
// Identify, tres rapide pour le bouton tenu 8 s.
bool renderMono(Pattern p, uint32_t t);
// Roue des couleurs a l'intensite kMax : hue 0..767 (rouge, vert, bleu).
Rgb wheel(uint16_t hue);
const char *patternName(Pattern p);
// Code du motif pour le protocole JSON (docs/PROTOCOLE-JSON.md, 7.9) :
// identification, desappairage, redemarrage, injoignable, panne_radio,
// livree, non_appaire, hors_reseau, operationnel.
const char *patternCode(Pattern p);

// Sequence de banc ('led test') : chaque motif a tour de role.
struct TestStep {
  Pattern p;
  uint32_t ms;
};
constexpr uint8_t kTestSteps = 9;
extern const TestStep kTest[kTestSteps];
uint32_t testTotalMs();

struct Frame {
  Pattern p;
  Rgb c;
  bool mono;
};

class Logic {
 public:
  void setNet(Net n, uint32_t now);           // la phase repart de zero a chaque changement
  void setIdentify(bool on, uint32_t now);    // l'arc-en-ciel part du debut de l'identification
  void delivered(uint32_t now);                // eclat vert, relance s'il est en cours
  void unreachable(uint32_t now);              // trois clignements rouges, relances
  void setFault(bool on, uint32_t now);        // rouge fixe tant que le module radio est en panne
  void setButton(Button b, uint32_t now);      // bouton BOOT ; le motif part du debut a chaque changement
  void startTest(uint32_t now);                // 'led test' : kTest, puis retour a la normale
  void stopTest() { testing_ = false; }
  bool testing() const { return testing_; }
  Net net() const { return net_; }
  // Ce qu'il faut afficher maintenant. Oublie les evenements echus.
  Frame frame(uint32_t now);

 private:
  Pattern pick(uint32_t now, uint32_t &t);
  Net net_ = Net::Unpaired;
  Button button_ = Button::None;
  bool identify_ = false, red_ = false, green_ = false, testing_ = false, fault_ = false;
  uint32_t netAt_ = 0, identAt_ = 0, redAt_ = 0, greenAt_ = 0, testAt_ = 0, faultAt_ = 0, buttonAt_ = 0;
};

// --- Identify par TriggerEffect (matter_bridge.cpp) ------------------------
// La pile n'envoie jamais de STOP apres un effet : sa fin est datee (millis)
// par endpoint, 0 = aucun effet. Identifiants de la spec Matter
// (EffectIdentifierEnum, MatterIdentifyRequest::EffectId dans la bibliotheque).
constexpr uint8_t kEffectBlink = 0x00, kEffectBreathe = 0x01, kEffectOkay = 0x02, kEffectChannelChange = 0x0B,
                  kEffectFinish = 0xFE, kEffectStop = 0xFF;
constexpr uint32_t kEffectFinishMs = 1000;  // Finish : le cycle en cours s'acheve (1 s au plus)

// Effet en cours a now ? Ecart signe : juste a travers le retour a zero de
// millis(), tant qu'une fin echue est oubliee (remise a 0) avant 24,8 jours.
inline bool effectPending(uint32_t end, uint32_t now) { return end && (int32_t)(end - now) > 0; }
// Fin d'effet d'un endpoint qui recoit l'effet 'effect' a now, son effet
// precedent finissant a 'end'. Stop : fin immediate. Finish : l'effet en cours
// finit son cycle (kEffectFinishMs au plus) et rien ne s'allume sans effet en
// cours. Les autres durent ce que dit la spec (Breathe 15 s, ChannelChange
// 8 s), 2 s au moins (Blink et Okay y durent a peine une seconde). Jamais 0
// pour un effet en cours.
uint32_t effectEnd(uint32_t end, uint8_t effect, uint32_t now);

}  // namespace statusled

// --- Cote carte (status_led.cpp, tache loop uniquement) ---------------------

// Au debut de setup() : met la WS2812 au noir (IO8 apres le demarrage, donc
// apres l'echantillonnage des broches de strapping) et laisse la LED simple
// d'IO15 en entree : eteinte quelle que soit sa polarite, sans conflit avec
// GDO2 du CC2500 sur la carte de capture (bogue B9). Sans WS2812, la LED simple
// passe en sortie, eteinte. Build diagnostic : IO15 en entree, et la WS2812 mise
// au noir une seule fois (elle garde sa derniere couleur a travers un reset).
void statusLedBegin();
// A chaque tour de loop(), apres bootButtonPoll() : lit l'etat (reseau toutes
// les 200 ms, compteurs et panne du module radio, Identify, bouton BOOT) et
// n'ecrit la LED que si sa couleur change.
void statusLedPoll();
// Commande 'led [test|stop]'.
void statusLedCommand(const char *arg);
// Motif affiche au dernier statusLedPoll() et test en cours ; false en build
// diagnostic (aucun voyant).
bool statusLedState(statusled::Pattern *p, bool *testing);
// Appele par statusLedPoll() quand le motif choisi change (evenement 'led' du
// protocole JSON). nullptr : aucun. Jamais appele en build diagnostic.
using StatusLedObserver = void (*)(statusled::Pattern now, statusled::Pattern before, bool testing);
void statusLedSetObserver(StatusLedObserver fn);
