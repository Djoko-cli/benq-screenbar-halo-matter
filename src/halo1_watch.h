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
//  Deux symptomes de PUCE, jamais d'environnement :
//  - kTimeoutRun delais de suite. Un accuse ou un MAX_RT remet la serie a
//    zero : MAX_RT dit que la puce emet et attend l'accuse sans le recevoir
//    (lampe debranchee), il ne fait JAMAIS relancer. FIFO refusee : neutre.
//    Incident : la serie de 3 tombe des la premiere commande (~0,4 s).
//  - deluge de bruit en ecoute : dans une fenetre de kNoiseWindowMs, au moins
//    kNoiseMinFrames trames brutes, dont au moins kNoiseBadPct % de CRC faux.
//    Il couvre une phase ou la puce malade recoit du bruit (les ~96 000 CRC
//    faux de l'incident), pas la phase sourde : celle-ci n'est vue qu'a la
//    commande suivante, par les delais. Usage normal : la molette de la
//    telecommande donne ~9 trames par seconde (et autant d'accuses de la
//    lampe au plus), au CRC juste ; le CRC faux se compte en unites par
//    heure. Il faudrait 90 CRC faux en 10 s, et une proportion que l'on ne
//    voit qu'avec une puce malade.
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
//  remettrait sinon le compte a zero a chaque essai. L'etat EN PANNE dure
//  jusqu'a ce signe.
//
//  Chaque relance efface les preuves (serie, fenetre) : le symptome doit etre
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
enum class Relaunch : uint8_t { None, Verify, TxTimeout, RxNoise };
constexpr uint8_t kRelaunchCauses = 4;
const char *relaunchText(Relaunch c);  // "-", "verif.", "delais", "bruit"

class ChipWatch {
 public:
  static constexpr uint8_t kTimeoutRun = 3;           // delais de suite
  static constexpr uint32_t kNoiseWindowMs = 10000;   // fenetre du deluge
  static constexpr uint16_t kNoiseMinFrames = 100;    // trames brutes au moins...
  static constexpr uint8_t kNoiseBadPct = 90;         // ... dont 90 % de CRC faux
  static constexpr uint32_t kGapMs = 60000;           // une relance au plus par minute
  static constexpr uint8_t kFruitless = 3;            // relances de suite sans guerison...
  static constexpr uint32_t kBackoffMs = 600000;      // ... puis un essai toutes les 10 min
  static constexpr uint8_t kHistN = 4;                // dernieres relances gardees

  struct Flood { uint16_t frames, bad; uint32_t ms; };  // fenetre qui a declenche
  struct Entry { Relaunch cause; uint32_t atMs; };

  void txVerdict(TxSeen v);
  void rxFrame(bool crcOk, uint32_t nowMs);
  // Symptome present et relance permise : sa cause (delais avant bruit) ;
  // sinon None. A appeler a chaque tour : ferme la fenetre echue, oublie une
  // attente echue et leve EN PANNE si le symptome revient apres kFruitless
  // relances sans guerison.
  Relaunch due(uint32_t nowMs);
  // Relance faite ou tentee (symptome ou verification) : comptee, datee,
  // attente de kGapMs (kBackoffMs apres kFruitless), preuves effacees.
  void relaunched(Relaunch cause, uint32_t nowMs);
  // Outil de banc, nouvel essai L3 : preuves effacees, limites et compteurs gardes.
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
  uint8_t unrecovered() const { return unrecovered_; }  // relances de suite sans guerison
  uint32_t gapMs() const { return unrecovered_ >= kFruitless ? kBackoffMs : kGapMs; }
  uint32_t waitMs(uint32_t nowMs) const;  // avant la prochaine relance permise (0 : permise)
  uint32_t count(Relaunch c) const { return (uint8_t)c < kRelaunchCauses ? counts_[(uint8_t)c] : 0; }
  uint32_t total() const;
  uint8_t history(Entry *out, uint8_t max) const;  // du plus recent au plus ancien

 private:
  void roll(uint32_t nowMs);
  void openWindow(uint32_t nowMs);
  void recovered();

  uint8_t timeouts_ = 0;  // sature a 255
  bool winOpen_ = false, winMet_ = false, noisy_ = false;
  uint32_t winAt_ = 0;
  uint16_t winFrames_ = 0, winBad_ = 0;
  Flood flood_{};
  bool holding_ = false, failed_ = false;
  uint32_t lastAt_ = 0;
  uint8_t unrecovered_ = 0;  // sature a 255
  uint32_t counts_[kRelaunchCauses] = {};
  Entry hist_[kHistN] = {};
  uint8_t histIdx_ = 0, histN_ = 0;
};

}  // namespace halo1
