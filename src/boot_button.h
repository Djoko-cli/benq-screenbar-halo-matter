#pragma once
#include <stdint.h>

// ===========================================================================
//  Bouton BOOT (IO9 sur le C6 SuperMini) : redemarrage et desappairage
//
//  Dans le boitier imprime, seul BOOT est accessible (RST ne l'est pas).
//  Decision de Majid (24/09) :
//    - appui court (< 2 s)          : eclat blanc, puis redemarrage, au RELACHEMENT ;
//    - appui de 2 s a 8 s, relache  : ANNULE, rien ne se passe (garde-fou) ;
//    - appui de 8 s ou plus         : rouge/violet rapide tant qu'on tient
//      ("relache pour desappairer") ; au RELACHEMENT : retrait de Matter
//      (toutes les fabriques, l'accessoire sort d'Apple Home), puis redemarrage.
//  Build diagnostic (sans Matter) : l'appui court redemarre, l'appui long ne
//  fait que l'expliquer sur la console.
//
//  IO9 est une broche de strapping : tenue basse au moment d'un reset, elle
//  fait demarrer le C6 en mode telechargement (ROM), ou il reste "mort"
//  jusqu'a une coupure d'alimentation. Aucune action ne part donc avant que
//  le relachement soit vu ET que la broche soit relue haute sans interruption
//  pendant kSettleMs ; la carte relit encore la broche juste avant le reset.
//
//  La logique (bootbtn::Machine) est pure et sans Arduino : testee sur l'hote
//  (tools/host_tests). Toutes les durees sont des ecarts non signes : le
//  retour a zero de millis() ne change rien.
//
//  Mesure : la duree d'un appui est l'ecart entre le premier releve bas et le
//  premier releve haut qui le termine (anti-rebond de kDebounceMs dans les
//  deux sens) : < 2000 ms = court, 2000 a 7999 ms = annule. L'appui long est
//  arme quand la broche est encore relevee basse kLongMs - 1 ms apres le
//  premier releve bas (un releve couvre sa milliseconde : relache au releve
//  suivant, l'appui mesure 8000 ms). Si loop() a manque des releves (commande
//  de banc, relance du module) de plus de kMaxGapMs pendant un appui ou a
//  l'un de ses fronts, sa duree est incertaine : il n'est pas arme, et son
//  relachement est ignore (Event::Unsure).
// ===========================================================================

namespace bootbtn {

constexpr uint32_t kDebounceMs = 30;       // un niveau doit tenir 30 ms pour compter
constexpr uint32_t kShortMaxMs = 2000;     // appui court : duree < 2000 ms
constexpr uint32_t kLongMs = 8000;         // appui long : encore bas 8000 ms apres le debut
constexpr uint32_t kSettleMs = 100;        // broche haute sans interruption avant tout reset
constexpr uint32_t kRebootDelayMs = 250;   // apres le relachement confirme : eclat blanc, noir, reset
constexpr uint32_t kMaxGapMs = 100;        // au-dela, un front n'est pas date assez finement

// Phase courante (la LED d'etat en depend).
enum class Phase : uint8_t {
  Idle,    // relache, rien en attente
  Held,    // appui en cours, moins de kLongMs
  Armed,   // appui en cours depuis kLongMs ou plus : relacher desappaire
  Reboot,  // appui court relache : redemarrage des que la broche est stable
  Unpair,  // appui long relache : desappairage des que la broche est stable
  Locked,  // tenu au demarrage : ignore jusqu'au premier relachement
};

// Ce qu'un releve vient de decider (au plus un par appel).
enum class Event : uint8_t {
  None,
  Armed,         // kLongMs atteints, bouton toujours tenu
  Cancelled,     // relache entre kShortMaxMs et le seuil long : rien
  Unsure,        // front mal date (trou de releves) : rien
  Dropped,       // nouvel appui pendant l'attente d'une action : action abandonnee
  BootReleased,  // relachement d'un appui tenu au demarrage : rien
  Reboot,        // REDEMARRER maintenant (broche haute depuis kSettleMs)
  Unpair,        // DESAPPAIRER maintenant (broche haute depuis kSettleMs)
};

const char *phaseName(Phase p);
const char *eventName(Event e);

class Machine {
 public:
  // Premier releve : bouton deja bas = tenu au demarrage (Locked).
  void begin(bool low, uint32_t now);
  // Un releve de la broche (low = bouton enfonce), a chaque tour de loop().
  Event update(bool low, uint32_t now);
  Phase phase() const { return phase_; }
  uint32_t phaseAt() const { return phaseAt_; }  // entree dans la phase courante
  // Duree du dernier appui relache (premier releve haut - premier releve bas).
  uint32_t lastPressMs() const { return lastPressMs_; }
  // Plus grand trou de releves autour des fronts du dernier appui (Unsure).
  uint32_t lastGapMs() const { return lastGapMs_; }
  bool started() const { return started_; }

 private:
  Event pressed(uint32_t now);
  Event released(uint32_t now);
  void enter(Phase p, uint32_t now) {
    phase_ = p;
    phaseAt_ = now;
  }
  bool started_ = false;
  bool raw_ = false, stable_ = false;  // dernier releve, niveau apres anti-rebond (true = bas)
  bool highValid_ = false;             // broche relevee haute sans interruption depuis highSince_
  uint32_t lastNow_ = 0, rawAt_ = 0, highSince_ = 0;
  // Trous de releves : avant le premier releve du niveau courant (edgeGap_),
  // pendant ce niveau (runGap_) ; avant le premier releve bas de l'appui
  // (startGap_), et depuis (holdGap_, jusqu'au relachement confirme).
  uint32_t edgeGap_ = 0, runGap_ = 0, startGap_ = 0, holdGap_ = 0;
  uint32_t pressAt_ = 0, lastPressMs_ = 0, lastGapMs_ = 0;
  Phase phase_ = Phase::Idle;
  uint32_t phaseAt_ = 0;
};

}  // namespace bootbtn

// --- Cote carte (boot_button.cpp, tache loop uniquement) --------------------

// Dans setup() : broche en entree tiree au 3V3. Le premier releve se fait au
// premier bootButtonPoll() : un bouton encore tenu a ce moment est ignore
// jusqu'a son relachement.
void bootButtonBegin();
// A chaque tour de loop(), avant statusLedPoll() : releve, et action (ne
// revient pas si elle redemarre la carte).
void bootButtonPoll();
// Phase courante, pour la LED d'etat.
bootbtn::Phase bootButtonPhase();
