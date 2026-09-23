#pragma once
#include <stdint.h>

// ===========================================================================
//  Surveillance du BM5602 : quand relancer le module sur symptome (niveau L2)
//
//  Incident du 24/09 (carte produit, Thread) : une pointe de pied a coulisse
//  metallique a touche le quartz du BM5602, bloque ~70 min. Compteurs depuis
//  le demarrage (13 min au moins avant) : 1566 paquets sur 1812 en DELAI
//  (ni TX_DS ni MAX_RT en 30 ms), 96 403 trames brutes en ecoute dont 99,8 %
//  de CRC faux (d'ordinaire : quelques unes par heure), sans date : on ne
//  sait pas a quel rythme elles sont venues. Phase finale observee (trace
//  des 2 dernieres minutes) : 186 DELAI de suite, 0 MAX_RT ; en ecoute, AUCUNE
//  trame (reconfiguration de silence toutes les ~540 ms, le plus vite
//  possible), 350 a 450 rearmements par seconde, presque tous sur OMST != RX
//  (les periodiques en font 10 au plus ; ~7 par seconde en tout d'ordinaire),
//  STA1, IRQ1 et STATUS lus a 00 ; mais RFCH, DM1 et RT1 relus conformes : la
//  verification ne voyait rien (2 echecs, 0 relance) et la reconfiguration L1
//  ne guerissait pas. 'rfinit' (halo.begin() : attente du quartz,
//  calibration) a gueri tout de suite.
//
//  Trois symptomes de PUCE, jamais d'environnement :
//  - kTimeoutRun delais de suite. Un accuse ou un MAX_RT remet la serie a
//    zero : MAX_RT dit que la puce emet et attend l'accuse sans le recevoir
//    (lampe debranchee), il ne fait JAMAIS relancer. FIFO refusee : neutre.
//    Incident : la serie de 3 tombe des la premiere commande (~0,4 s).
//  - deluge de bruit en ecoute : dans une fenetre de kNoiseWindowMs, au moins
//    kNoiseMinFrames trames brutes, dont au moins kNoiseBadPct % de CRC faux.
//    Il couvre une phase ou la puce malade recoit du bruit (les ~96 000 CRC
//    faux de l'incident). Usage normal : la molette de la telecommande donne
//    ~9 trames par seconde (et autant d'accuses de la lampe au plus), au CRC
//    juste ; le CRC faux se compte en unites par heure. Il faudrait 90 CRC
//    faux en 10 s, et une proportion que l'on ne voit qu'avec une puce malade.
//  - ecoute sourde : au moins kDeafMinRearms rearmements sur OMST != RX dans
//    une fenetre glissante de kDeafWindowMs (kDeafSlices tranches de
//    kDeafSliceMs ; la somme couvre la tranche en cours et les precedentes,
//    donc toujours moins de 10 s). C'est la phase finale de l'incident :
//    aucune trame, 350 a 450 rearmements hors RX par seconde, vue en ~2-3 s
//    au lieu d'attendre la commande suivante. D'ordinaire : ~7 rearmements
//    par seconde EN TOUT, presque tous periodiques (un par 100 ms au plus,
//    puce en RX : ils ne comptent pas ici) ; les hors RX, bien moins d'un par
//    seconde. Le seuil, 100 par seconde en moyenne, est plus de 10 fois
//    au-dessus de tous les rearmements normaux reunis et 3,5 fois sous
//    l'incident. Lampe debranchee (MAX_RT) : l'ecoute reste normale, rien.
//  Rien sur une lampe muette : la debrancher ne doit pas relancer en boucle.
//
//  Limites : une relance sur symptome au plus kGapMs apres la precedente,
//  quelle qu'en soit la cause, essai L3 compris (held). Celle des
//  verifications ratees n'attend pas (la radio reste inerte sans elle, et L3
//  arrete deja sa boucle), mais elle compte ici comme les autres. Apres
//  kFruitless relances de suite sans signe de guerison, si le symptome
//  revient : module EN PANNE, un essai toutes les kBackoffMs. Signe de
//  guerison : un accuse, ou une fenetre d'ecoute close sans deluge et
//  majoritairement au CRC juste. Une trame juste isolee ne suffit pas : un
//  CRC-16 laisse passer du bruit, et une puce malade peut rester sous le
//  seuil du deluge (60 fausses et une juste ne guerissent pas), ce qui
//  remettrait sinon le compte a zero a chaque essai. Le silence ne prouve
//  rien, sauf apres une relance pour surdite : une fenetre close sans CRC
//  faux, avec au moins kCalmMinInRx rearmements periodiques (la puce disait
//  RX) et au plus kCalmMaxOffRx hors RX, montre que ce symptome-la est parti.
//  Sans elle, une piece calme (lampe debranchee ou pas commandee, telecommande
//  posee) laisserait le module EN PANNE apres une relance qui a gueri. Apres
//  une autre cause, elle ne compte pas : une puce qui reste en RX peut encore
//  mal emettre, et les delais garderaient sinon leur limite de 10 min. L'etat
//  EN PANNE dure jusqu'a un signe de guerison.
//
//  Chaque relance efface les preuves (serie, fenetres) : le symptome doit etre
//  vu de nouveau apres elle. Un outil de banc (forget) les efface aussi : il a
//  pu laisser la puce dans n'importe quel etat.
//
//  Pur et sans Arduino : teste sur l'hote (tools/host_tests). Toutes les
//  durees sont des ecarts non signes a partir d'un instant, et les attentes
//  echues sont oubliees a chaque appel de due() : le retour a zero de millis()
//  ne ranime rien.
// ===========================================================================

namespace halo1 {

// Verdict d'un paquet tel que la surveillance le lit : Halo1Radio::Verdict
// Ack et AckForeign -> Ack (TX_DS), MaxRt, Timeout, FifoRefused -> Refused.
enum class TxSeen : uint8_t { Ack, MaxRt, Timeout, Refused };
// Cause d'une relance automatique du module.
enum class Relaunch : uint8_t { None, Verify, TxTimeout, RxNoise, RxDeaf };
constexpr uint8_t kRelaunchCauses = 5;
const char *relaunchText(Relaunch c);  // "-", "verif.", "delais", "bruit", "sourde"

class ChipWatch {
 public:
  static constexpr uint8_t kTimeoutRun = 3;           // delais de suite
  static constexpr uint32_t kNoiseWindowMs = 10000;   // fenetre du deluge
  static constexpr uint16_t kNoiseMinFrames = 100;    // trames brutes au moins...
  static constexpr uint8_t kNoiseBadPct = 90;         // ... dont 90 % de CRC faux
  static constexpr uint32_t kDeafWindowMs = 10000;    // fenetre glissante de la surdite...
  static constexpr uint32_t kDeafSliceMs = 1000;      // ... en tranches d'une seconde
  static constexpr uint8_t kDeafSlices = (uint8_t)(kDeafWindowMs / kDeafSliceMs);
  static constexpr uint16_t kDeafMinRearms = 1000;    // rearmements hors RX au moins
  static constexpr uint16_t kCalmMaxOffRx = 10;       // guerison apres surdite : au plus 10 hors RX...
  static constexpr uint16_t kCalmMinInRx = 20;        // ... et 20 periodiques au moins, sur 10 s
  static constexpr uint32_t kGapMs = 60000;           // une relance au plus par minute
  static constexpr uint8_t kFruitless = 3;            // relances de suite sans guerison...
  static constexpr uint32_t kBackoffMs = 600000;      // ... puis un essai toutes les 10 min
  static constexpr uint8_t kHistN = 4;                // dernieres relances gardees

  struct Flood { uint16_t frames, bad; uint32_t ms; };  // fenetre qui a declenche
  // Surdite qui a fait relancer : rearmements hors RX, sur au plus ms (depuis
  // le debut de la plus ancienne tranche non vide).
  struct Deaf { uint32_t rearms, ms; };
  struct Entry { Relaunch cause; uint32_t atMs; };

  void txVerdict(TxSeen v);
  void rxFrame(bool crcOk, uint32_t nowMs);
  // Rearmements de l'ecoute depuis l'appel precedent (ecarts des compteurs de
  // Halo1Radio::Stats) : offRx faits sur OMST != RX, inRx periodiques (la puce
  // disait RX). A chaque tour d'ecoute.
  void rxRearms(uint32_t offRx, uint32_t inRx, uint32_t nowMs);
  // Symptome present et relance permise : sa cause (delais, bruit, surdite) ;
  // sinon None. A appeler a chaque tour : ferme la fenetre echue, oublie une
  // attente echue et leve EN PANNE si le symptome revient apres kFruitless
  // relances sans guerison.
  Relaunch due(uint32_t nowMs);
  // Relance faite ou tentee (symptome ou verification) : comptee, datee,
  // attente de kGapMs (kBackoffMs apres kFruitless), preuves effacees.
  void relaunched(Relaunch cause, uint32_t nowMs);
  // Outil de banc, nouvel essai L3 : preuves effacees (serie de delais, fenetre
  // d'ecoute, surdite), limites et compteurs gardes.
  void forget(uint32_t nowMs);
  // Essai L3 (halo.begin() hors symptome) : attente de gapMs() comme apres une
  // relance, sans compte ni historique.
  void held(uint32_t nowMs) {
    holding_ = true;
    lastAt_ = nowMs;
  }
  void clearCounts();  // 'lampe stats raz' : compteurs et historique seulement

  bool failed() const { return failed_; }  // module EN PANNE
  Relaunch symptom() const;                // symptome present, sans les limites
  uint8_t timeoutRun() const { return timeouts_; }
  bool noisy() const { return noisy_; }
  uint16_t windowFrames() const { return winFrames_; }
  uint16_t windowBad() const { return winBad_; }
  const Flood &lastFlood() const { return flood_; }
  uint32_t deafRearms() const;  // hors RX dans la fenetre glissante (au dernier appel)
  const Deaf &lastDeaf() const { return deaf_; }
  uint8_t unrecovered() const { return unrecovered_; }  // relances de suite sans guerison
  uint32_t gapMs() const { return unrecovered_ >= kFruitless ? kBackoffMs : kGapMs; }
  uint32_t waitMs(uint32_t nowMs) const;  // avant la prochaine relance permise (0 : permise)
  uint32_t count(Relaunch c) const { return (uint8_t)c < kRelaunchCauses ? counts_[(uint8_t)c] : 0; }
  uint32_t total() const;
  uint8_t history(Entry *out, uint8_t max) const;  // du plus recent au plus ancien

 private:
  void roll(uint32_t nowMs);
  void openWindow(uint32_t nowMs);
  void rollDeaf(uint32_t nowMs);
  void clearDeaf(uint32_t nowMs);
  uint32_t deafSpan(uint32_t nowMs) const;
  void recovered();

  uint8_t timeouts_ = 0;  // sature a 255
  bool winOpen_ = false, winMet_ = false, noisy_ = false;
  uint32_t winAt_ = 0;
  uint16_t winFrames_ = 0, winBad_ = 0;
  uint32_t winOffRx_ = 0, winInRx_ = 0;  // rearmements de la fenetre (guerison apres surdite)
  Flood flood_{};
  // Surdite : tranches de kDeafSliceMs, deafIdx_ celle en cours, ouverte a deafAt_.
  uint16_t deafSlice_[kDeafSlices] = {};  // saturees a 65535
  uint8_t deafIdx_ = 0;
  uint32_t deafAt_ = 0;
  Deaf deaf_{};
  bool holding_ = false, failed_ = false;
  uint32_t lastAt_ = 0;
  uint8_t unrecovered_ = 0;  // sature a 255
  Relaunch lastCause_ = Relaunch::None;  // derniere relance comptee (guerison apres surdite)
  uint32_t counts_[kRelaunchCauses] = {};
  Entry hist_[kHistN] = {};
  uint8_t histIdx_ = 0, histN_ = 0;
};

}  // namespace halo1
