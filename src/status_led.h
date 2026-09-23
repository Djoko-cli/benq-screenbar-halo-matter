#pragma once
#include <stdint.h>

// ===========================================================================
//  LED d'etat du produit (signature validee le 23/09)
//
//  WS2812 de la carte (IO8 sur le C6 SuperMini), builds produit seulement :
//    - pas mis en service         : bleu clignotant (250 ms / 250 ms)
//    - mis en service, sans reseau : orange lent (1 s / 1 s)
//    - tout va bien               : eteinte, breve lueur blanche toutes les 10 s
//    - consigne livree a la lampe : eclat vert (150 ms)
//    - lampe injoignable (abandon): rouge, 3 clignements
//    - Identify (Apple Home)      : arc-en-ciel pendant toute l'identification
//  Priorite : Identify > rouge > vert > etat du reseau. Intensite basse (elle
//  est sous un bureau) : 24/255 au plus par canal, 8/255 pour la lueur.
//
//  Sans WS2812 (autres cibles), la LED simple de PIN_STATUS_LED suit les memes
//  motifs en tout ou rien, sans la lueur. Build diagnostic : aucune LED pilotee.
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

// Motifs, du plus prioritaire au moins prioritaire.
enum class Pattern : uint8_t { Identify, Unreachable, Delivered, Unpaired, Offline, Online };

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

// Couleur d'un motif, t ms apres son depart. Les motifs bornes (vert, rouge)
// rendent du noir une fois finis.
Rgb render(Pattern p, uint32_t t);
// Meme chose pour une LED simple : pas de lueur, clignotement rapide pour Identify.
bool renderMono(Pattern p, uint32_t t);
// Roue des couleurs a l'intensite kMax : hue 0..767 (rouge, vert, bleu).
Rgb wheel(uint16_t hue);
const char *patternName(Pattern p);

// Sequence de banc ('led test') : chaque motif a tour de role.
struct TestStep {
  Pattern p;
  uint32_t ms;
};
constexpr uint8_t kTestSteps = 6;
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
  void startTest(uint32_t now);                // 'led test' : kTest, puis retour a la normale
  void stopTest() { testing_ = false; }
  bool testing() const { return testing_; }
  Net net() const { return net_; }
  // Ce qu'il faut afficher maintenant. Oublie les evenements echus.
  Frame frame(uint32_t now);

 private:
  Pattern pick(uint32_t now, uint32_t &t);
  Net net_ = Net::Unpaired;
  bool identify_ = false, red_ = false, green_ = false, testing_ = false;
  uint32_t netAt_ = 0, identAt_ = 0, redAt_ = 0, greenAt_ = 0, testAt_ = 0;
};

}  // namespace statusled

// --- Cote carte (status_led.cpp, tache loop uniquement) ---------------------

// Au debut de setup() : met la LED d'etat au noir (IO8 apres le demarrage,
// donc apres l'echantillonnage des broches de strapping) et IO15 eteinte.
// Build diagnostic : IO15 en entree, WS2812 jamais pilotee.
void statusLedBegin();
// A chaque tour de loop() : lit l'etat (reseau toutes les 200 ms, compteurs du
// pilote, Identify) et n'ecrit la LED que si sa couleur change.
void statusLedPoll();
// Commande 'led [test|stop]'.
void statusLedCommand(const char *arg);
