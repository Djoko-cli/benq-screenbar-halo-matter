#pragma once
// ===========================================================================
//  Protocole JSON du pont, v1 (docs/PROTOCOLE-JSON.md) : briques pures
//
//  - Writer : une ligne machine, RS + objet JSON compact en ASCII + LF, 1024
//    octets au plus (section 2.2). Chaines echappees ('"' et '\'), tout octet
//    hors 0x20..0x7E remplace par '?', jamais de \uXXXX. Au-dela de 1024
//    octets, la ligne est marquee trop longue : jamais emise.
//  - Messages construits depuis des donnees simples : Etat, codes de champs,
//    evenements du pilote (halo1_events.h), reponse, livraison, battement.
//  - Mecaniques de la session : prefixe id= des lignes de l'hote, assemblage
//    des lignes recues, plafonds de debit par type, cadence, file des lignes
//    periodiques.
//
//  Pur et sans Arduino : teste sur l'hote (tools/host_tests). La session,
//  l'ecriture sur le port serie et les instantanes sont dans json_mode.cpp.
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#include "halo1_events.h"
#include "halo1_proto.h"

namespace jsonp {

constexpr uint8_t kVersion = 1;         // v : version majeure
constexpr uint8_t kRev = 0;             // hello.rev : revision mineure (ajouts)
constexpr size_t kLineMax = 1024;       // RS et LF compris
constexpr size_t kBudget = 896;         // pire cas vise par message (marge de 128 pour les ajouts)
constexpr size_t kCmdMax = 127;         // ligne de l'hote, prefixe id= compris
constexpr size_t kCmdTextMax = 40;      // reponse.cmd
constexpr size_t kMsgMax = 120;         // reponse.msg
constexpr size_t kLogTextMax = 191;     // log.txt
constexpr size_t kStrMax = 255;         // toute autre chaine
constexpr uint8_t kRS = 0x1E;
constexpr uint8_t kCtrlU = 0x15;        // vide la ligne en cours de saisie
constexpr uint32_t kIdMax = 999999999;  // id=<1..999999999>
constexpr uint8_t kIdsMax = 8;          // livraison.ids

// ---------------------------------------------------------------------------
//  Ecrivain d'une ligne machine
// ---------------------------------------------------------------------------

class Writer {
 public:
  // Ouvre la ligne : RS {"v":1,"t":type,"n":n,"ms":ms. Le champ suivant d'un
  // message en blocs est "bloc" (str("bloc", ...)).
  void begin(const char *type, uint32_t n, uint32_t ms);
  // Champs. k nul : element du tableau ouvert. Les cles sont des litteraux
  // ASCII du firmware, jamais echappees.
  void str(const char *k, const char *v, size_t max = kStrMax);  // v nul : null ; max caracteres de v
  void u32(const char *k, uint32_t v);
  void i32(const char *k, int32_t v);
  void boolean(const char *k, bool v);
  void null(const char *k);
  void hex(const char *k, const uint8_t *p, size_t n);  // "C5A5" (majuscules, sans 0x) ; n == 0 : ""
  void hexU32(const char *k, uint32_t v, uint8_t digits, bool prefix0x = false);  // "3FA2C901", "0x1A2B"
  void obj(const char *k);
  void arr(const char *k);
  void end();  // ferme l'objet ou le tableau ouvert
  // Ferme la ligne (} LF). false : plus de kLineMax octets, ou objets mal
  // fermes (bogue) ; la ligne ne doit pas etre emise.
  bool finish();
  const uint8_t *data() const { return buf_; }
  size_t size() const { return len_; }
  bool overflow() const { return over_; }

 private:
  static constexpr uint8_t kDepth = 8;
  void put(char c);
  void puts(const char *s);
  void sep(const char *k);
  void num(uint32_t v, bool neg);
  void open(const char *k, char o, char c);
  uint8_t buf_[kLineMax];
  size_t len_ = 0;
  bool over_ = false, bad_ = false;
  uint8_t depth_ = 0;
  bool first_[kDepth] = {};
  char close_[kDepth] = {};
};

// ---------------------------------------------------------------------------
//  Textes du protocole (enumerations : ASCII minuscule, '_' comme separateur)
// ---------------------------------------------------------------------------

const char *lampsCode(uint8_t lamps);        // avant, arriere, deux ; aucune (jamais dans un Etat)
const char *kindCode(halo1::Kind k);         // lum, temp, a, accuse_lampe, service, favori, invalide, crc_faux
const char *slotCode(uint8_t slot);          // lum, temp, a, brut
const char *verdictCode(uint8_t verdict);    // ack, ack_trame, max_rt, delai, fifo
const char *relaunchCode(halo1::Relaunch c); // verif, delais, bruit, sourde ; l3 pour None
const char *symptomCode(halo1::Relaunch c);  // verif, delais, bruit, sourde ; nul pour None

// Objet Etat (section 4) : marche, lampes, lum, niveau, temp, mired.
void state(Writer &w, const char *k, const halo1::State &s);
// Codes de champs de consigne : ["marche","lum","temp"].
void fields(Writer &w, const char *k, uint8_t mask);

// ---------------------------------------------------------------------------
//  Messages d'evenement et de reponse
// ---------------------------------------------------------------------------

void rx(Writer &w, uint32_t n, uint32_t ms, const halo1::RxEvent &e, uint32_t skipped);
void tx(Writer &w, uint32_t n, uint32_t ms, const halo1::TxEvent &e, uint32_t skipped);
void relaunch(Writer &w, uint32_t n, uint32_t ms, const halo1::RelaunchEvent &e);
void module(Writer &w, uint32_t n, uint32_t ms, const halo1::ModuleEvent &e);
void heartbeat(Writer &w, uint32_t n, uint32_t ms, uint32_t boot, uint32_t upS, uint32_t lost);
void sessionEnd(Writer &w, uint32_t n, uint32_t ms, const char *cause);  // t "fin" : commande, bail
void led(Writer &w, uint32_t n, uint32_t ms, const char *motif, const char *before, bool test);
void logLine(Writer &w, uint32_t n, uint32_t ms, const char *src, const char *niv, const char *txt, uint32_t skipped);

// Message reponse (section 6.3).
struct Reply {
  enum Suite : uint8_t { SuiteNone, SuiteDelivery, SuiteNothing };  // absent, livraison, aucune
  uint32_t id = 0;
  bool fin = true;              // false : etape debut
  const char *cmd = "";         // la commande sans le prefixe, tronquee a kCmdTextMax
  bool ok = true;
  const char *code = "ok";
  const char *msg = nullptr;    // nul : absent ; tronque a kMsgMax
  uint32_t durMs = 0;           // fin seulement
  Suite suite = SuiteNone;
  bool hasTarget = false;       // consigne, a_livrer, version (lampe asynchrone)
  halo1::State target;
  uint8_t dirty = 0;
  uint32_t version = 0;
  bool hasLease = false;        // bail_s, up_s (json 1, json ping)
  uint32_t leaseS = 0, upS = 0;
};
void reply(Writer &w, uint32_t n, uint32_t ms, const Reply &r);

// Message livraison (section 7.3).
enum class Issue : uint8_t { Delivered, GaveUp, Cancelled };  // livree, abandon, annulee
struct Delivery {
  Issue issue = Issue::Delivered;
  halo1::GiveUpCause cause = halo1::GiveUpCause::None;  // abandon seulement
  int8_t last = -1;             // tranche qui a clos la consigne (livree), -1 : null ; absente sinon
  uint32_t version = 0;
  halo1::State target, believed;
  uint8_t dirty = 0;
  const uint32_t *ids = nullptr;
  uint8_t nIds = 0;
  uint32_t idsLost = 0;
  bool hasWait = false;         // attente_ms connue (periode occupee vue)
  uint32_t waitMs = 0;
  uint32_t delivered = 0, giveUps = 0;
};
void delivery(Writer &w, uint32_t n, uint32_t ms, const Delivery &d);

// ---------------------------------------------------------------------------
//  Lignes de l'hote
// ---------------------------------------------------------------------------

// Prefixe "id=<n> " (n decimal 1..999999999, sans zero de tete superflu
// exige) en tete de ligne, espaces de tete ignores. true : *id rempli et
// *rest pointe sur la commande (espaces sautes). false : pas de prefixe
// valide, *rest = line.
bool parseIdPrefix(char *line, uint32_t *id, char **rest);
// Copie la commande pour reponse.cmd : kCmdTextMax caracteres au plus.
void copyCmd(char out[kCmdTextMax + 1], const char *cmd);

// Assemblage des octets recus en lignes (cliPoll). Mode machine : octets hors
// 0x20..0x7E ignores, sauf LF, CR, Ctrl-U et retour arriere, pour qu'aucun RS
// ne revienne dans un message. Au-dela de kCmdMax caracteres, la ligne est
// marquee trop longue (refusee a son LF, rien n'est execute).
class LineAssembler {
 public:
  enum class Ev : uint8_t { None, Echo, Erase, Clear, Line };
  Ev feed(uint8_t c, bool machine);
  char *text();  // ligne terminee par 0 (apres Ev::Line)
  bool tooLong() const { return tooLong_; }
  uint8_t cleared() const { return cleared_; }  // caracteres effaces par le dernier Ctrl-U
  uint8_t length() const { return len_; }
  void reset() {
    len_ = 0;
    tooLong_ = false;
  }

 private:
  char buf_[kCmdMax + 1] = {};
  uint8_t len_ = 0, cleared_ = 0;
  bool tooLong_ = false;
};

// ---------------------------------------------------------------------------
//  Debit
// ---------------------------------------------------------------------------

// Plafond par fenetre d'une seconde. Au-dela, l'evenement n'est pas produit
// (aucun n consomme) et le suivant du meme type porte 'sautes'.
class RateCap {
 public:
  explicit RateCap(uint16_t perSecond) : limit_(perSecond) {}
  bool available(uint32_t now);  // place dans la fenetre en cours
  void take() { count_++; }
  void skip() { skipped_++; }
  uint32_t takeSkipped() {
    const uint32_t s = skipped_;
    skipped_ = 0;
    return s;
  }

 private:
  uint16_t limit_, count_ = 0;
  bool started_ = false;
  uint32_t winAt_ = 0, skipped_ = 0;
};

// Au plus kLines lignes acceptees par kWindowMs glissantes (section 6.5).
class Cadence {
 public:
  static constexpr uint8_t kLines = 20;
  static constexpr uint32_t kWindowMs = 1000;
  bool allow(uint32_t now);  // true : ligne acceptee et comptee

 private:
  uint32_t at_[kLines] = {};
  uint8_t idx_ = 0, n_ = 0;
};

// ---------------------------------------------------------------------------
//  File des lignes periodiques (section 2.3)
// ---------------------------------------------------------------------------

constexpr uint32_t kLateMs = 500;  // ligne periodique perdue apres ce retard

enum class Item : uint8_t {
  HelloBase, HelloId, Config, EtatLampe, EtatTranches, EtatSante, CptPilote, CptRadio, CptMatter,
  NetThread, NetSubs, Heartbeat, Reply
};
struct Queued {
  Item item;
  uint8_t arg;   // Reply : place de la reponse differee
  bool session;  // periodique ou instantane de 'json 1' : retire a la fin du mode machine
  uint32_t at;   // mise en file (ou derniere demande explicite fondue dedans)
};

class Queue {
 public:
  static constexpr uint8_t kN = 24;
  // Ajoute en queue. Un element deja en file (hors Reply) n'est pas double ;
  // une demande explicite (session faux) fondue dedans le rend explicite et
  // repart de maintenant (son retard ne compte que depuis la demande).
  // false : file pleine.
  bool push(Item item, uint32_t now, bool session, uint8_t arg = 0);
  const Queued *front() const { return n_ ? &q_[head_] : nullptr; }
  void pop();
  uint8_t size() const { return n_; }
  bool has(Item item) const;
  // Retire de la tete les lignes en retard de plus de kLateMs et rend leur
  // nombre (n consomme, json_perdus). Une reponse n'est jamais perdue pour
  // retard : elle arrete le balayage, les lignes derriere elle attendent.
  uint8_t dropLate(uint32_t now);
  // La tete peut-elle partir avec 'room' octets libres dans le tampon
  // d'emission ? Ligne periodique : 2 x kLineMax (elle, puis la place d'un
  // evenement). Reponse : kLineMax (elle tient, quelle qu'elle soit).
  bool frontReady(int room) const;
  // Retire les elements de session ; ceux qui restent gardent leur ordre.
  uint8_t dropSession();
  void clear() { head_ = n_ = 0; }

 private:
  Queued q_[kN] = {};
  uint8_t head_ = 0, n_ = 0;
};

// ---------------------------------------------------------------------------
//  Bail (section 3.5)
// ---------------------------------------------------------------------------

// Le bail court depuis le plus recent : dernier octet recu, ou fin de la
// derniere commande (une commande de banc de 60 s ne le fait pas expirer).
// leaseS nul : sans bail, jamais expire.
bool leaseExpired(uint32_t now, uint32_t lastRx, uint32_t lastCmd, uint16_t leaseS);

// ---------------------------------------------------------------------------
//  Observateur de livraison (section 7.3)
// ---------------------------------------------------------------------------

// Releve du pilote (Halo1Lamp) a un tour de loop().
struct LampSample {
  bool busy = false;          // busy()
  bool targetBusy = false;    // targetBusy() : une tranche de consigne (lum, temp, a)
  uint32_t delivered = 0;     // deliveredCount()
  uint32_t giveUps = 0;       // giveUpCount()
  uint32_t pendingSince = 0;  // pendingSince(), 0 : aucune demande en attente
};

// Front de busy() vrai -> faux, ou compteur de livraison ou d'abandon change
// (consigne commencee et finie dans une commande bloquante) : une livraison.
// abandon l'emporte sur livree ; ni l'un ni l'autre : annulee. Une periode
// occupee par la seule tranche brute du banc ne donne rien. Hors mode machine,
// seulement s'il reste des id en attente.
class DeliveryWatch {
 public:
  // Point de depart : compteurs du moment, rien en attente.
  void reset(uint32_t delivered, uint32_t giveUps);
  // id d'une commande lampe acceptee (busy() vrai a cet instant) : la
  // periode occupee compte comme vue, une livraison suivra toujours, meme si
  // la periode finit avant le tour suivant sans compteur change (annulee).
  // 8 id au plus, les plus anciens sortent (ids_perdus).
  void pendingId(uint32_t id, uint32_t pendingSince);
  // Un tour. true : livraison a emettre maintenant ; *d rempli pour issue,
  // ids (pointe sur la liste interne, valable jusqu'au prochain pendingId),
  // ids_perdus, attente_ms, livrees, abandons ; le reste (version, consigne,
  // cru, a_livrer, cause, derniere) vient du pilote.
  bool poll(const LampSample &s, uint32_t now, bool machine, Delivery *d);
  uint8_t pending() const { return nIds_; }

 private:
  bool wasBusy_ = false, sawTarget_ = false;
  uint32_t since_ = 0, seenDelivered_ = 0, seenGiveUps_ = 0;
  uint32_t ids_[kIdsMax] = {};
  uint8_t nIds_ = 0;
  uint32_t idsLost_ = 0;
};

}  // namespace jsonp
