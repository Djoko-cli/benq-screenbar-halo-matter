#pragma once
// ===========================================================================
//  Enveloppe H1 du transport reseau (docs/PROTOCOLE-JSON.md, 10.4) : briques pures
//
//  Datagrammes UDP entre l'app et la carte, a travers les routeurs de bordure
//  Thread. Integrite et authenticite par une cle partagee de 32 octets (PSK)
//  et HMAC-SHA256 ; pas de confidentialite en v1.
//
//    app  -> carte : H1 SALUT <kid> <na> <mac_salut>
//    carte -> app  : H1 DEFI <sid> <nc> <mac_defi>
//    puis, dans les deux sens : H1 <sid> <ctr> <mac> <charge>
//
//  Textes canoniques (le MAC porte sur eux, octet pour octet) : hexadecimal
//  en MAJUSCULES seulement, ctr en decimal sans zero de tete (1..4294967295).
//  Toute autre forme est refusee a la lecture.
//
//  Pur et sans Arduino : teste sur l'hote (tools/host_tests/test_h1.cpp). Le
//  HMAC et le SHA-256 viennent de la plateforme : mbedTLS (SHA materiel du C6,
//  h1_crypto.cpp) sur la carte, CommonCrypto dans les tests.
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

namespace h1 {

constexpr size_t kKeyLen = 32;     // PSK et cle de session
constexpr size_t kNonceLen = 16;   // na, nc
constexpr size_t kMacLen = 16;     // MAC tronque
constexpr size_t kKidHex = 8;      // empreinte de la cle
constexpr size_t kSidHex = 8;
constexpr size_t kNonceHex = 32;
constexpr size_t kMacHex = 32;
constexpr uint32_t kWindow = 32;               // fenetre glissante des ctr, par sens
constexpr uint32_t kForgetMs = 600000;         // session oubliee apres 10 min sans message valide
constexpr uint32_t kProvisionalMs = 30000;     // poignee de main sans premier message : oubliee
constexpr uint8_t kDefiPerSecond = 2;          // DEFI emis au plus, EN TOUT
constexpr uint8_t kSlots = 2;                  // sessions etablies
// Une session etablie active depuis moins longtemps n'est jamais evincee par
// une nouvelle : trois clients pour deux places ne se chassent pas en boucle.
constexpr uint32_t kEvictIdleMs = 30000;
constexpr uint8_t kSeenNa = 16;                // na des derniers SALUT acceptes (rejeu d'un SALUT)
// "H1 " sid " " ctr " " mac " " : 3 + 8 + 1 + 10 + 1 + 32 + 1
constexpr size_t kHeaderMax = 56;
// "H1 DEFI " sid " " nc " " mac : 8 + 8 + 1 + 32 + 1 + 32
constexpr size_t kDefiLen = 82;
// "H1 SALUT " kid " " na " " mac : 9 + 8 + 1 + 32 + 1 + 32 (plus long que le DEFI : aucune amplification)
constexpr size_t kSalutLen = 83;

// --- Crypto de la plateforme -------------------------------------------------

struct Part {
  const void *p;
  size_t n;
};
// HMAC-SHA256(key, concatenation des parts). false : echec de la plateforme
// (memoire) ; out est alors indefini et tout calcul qui en depend echoue
// (jamais de MAC par defaut : un MAC nul serait devinable).
bool hmacSha256(const uint8_t *key, size_t keyLen, const Part *parts, size_t nParts, uint8_t out[32]);
bool sha256(const void *p, size_t n, uint8_t out[32]);

// --- Outils ------------------------------------------------------------------

// Comparaison en temps constant (aucune sortie anticipee).
bool equalCt(const uint8_t *a, const uint8_t *b, size_t n);
// Mise a zero que l'optimiseur ne retire pas (cles et secrets sur la pile).
void wipe(void *p, size_t n);
// 2n chiffres majuscules, puis un 0 final.
void toHex(const uint8_t *p, size_t n, char *out);
// Exactement 2n chiffres MAJUSCULES : false sinon (out est alors indefini).
bool fromHex(const char *s, size_t n, uint8_t *out);

// kid : 8 premiers chiffres (majuscules) de SHA-256(psk).
bool keyId(const uint8_t psk[kKeyLen], char kid[kKidHex + 1]);

// mac_salut = HMAC-SHA256(psk, "H1|SALUT|" kid "|" na), 16 premiers octets : seul
// qui a la cle peut faire depenser un DEFI ou remplacer la poignee de main en cours.
bool salutMac(const uint8_t psk[kKeyLen], const char *kid, const char *naHex, uint8_t out[kMacLen]);
// mac_defi = HMAC-SHA256(psk, "H1|DEFI|" kid "|" na "|" nc "|" sid), 16 premiers octets.
bool defiMac(const uint8_t psk[kKeyLen], const char *kid, const char *naHex, const char *ncHex, const char *sidHex,
             uint8_t out[kMacLen]);
// Ks = HMAC-SHA256(psk, "H1|SESSION|" na "|" nc "|" sid).
bool sessionKey(const uint8_t psk[kKeyLen], const char *naHex, const char *ncHex, const char *sidHex,
                uint8_t ks[kKeyLen]);
// mac = HMAC-SHA256(Ks, sens "|" sid "|" ctr "|" charge), 16 premiers octets ; sens 'A'
// (app -> carte) ou 'C' (carte -> app).
bool messageMac(const uint8_t ks[kKeyLen], char dir, const char *sidHex, uint32_t ctr, const uint8_t *payload,
                size_t n, uint8_t out[kMacLen]);

// --- Lecture d'un datagramme -------------------------------------------------

enum class Kind : uint8_t { Invalid, Salut, Defi, Data };

struct Parsed {
  Kind kind = Kind::Invalid;
  char kid[kKidHex + 1] = {};     // Salut
  char na[kNonceHex + 1] = {};    // Salut
  char nc[kNonceHex + 1] = {};    // Defi
  char sidHex[kSidHex + 1] = {};  // Defi, Data
  uint32_t sid = 0;
  uint32_t ctr = 0;               // Data
  uint8_t mac[kMacLen] = {};      // Salut, Defi, Data
  const uint8_t *payload = nullptr;  // Data : tout ce qui suit la quatrieme espace
  size_t payloadLen = 0;
};
Parsed parse(const uint8_t *d, size_t n);

// --- Fenetre contre le rejeu et le desordre ---------------------------------

// top : plus grand ctr accepte ; bits : bit i = top - i deja vu.
struct Window {
  uint32_t top = 0, bits = 0;
  bool fresh(uint32_t ctr) const;  // acceptable (jamais vu, dans la fenetre)
  void commit(uint32_t ctr);       // apres un MAC juste seulement
};

// --- Sessions (cote carte) ----------------------------------------------------

// Adresses du plus recent message au MAC juste (ctr le plus haut vu) : l'app
// (adresse et port, qui changent avec les adresses temporaires de l'iPhone)
// et l'adresse locale qu'elle a visee (source des reponses). Un message plus
// ancien, retarde ou rejoue d'ailleurs, ne detourne pas les reponses.
struct Peer {
  uint8_t ip[16] = {};
  uint16_t port = 0;
  uint8_t local[16] = {};
};

struct Session {
  bool used = false;
  uint32_t sid = 0;
  char sidHex[kSidHex + 1] = {};
  uint8_t ks[kKeyLen] = {};
  Window rx;             // ctr de l'app
  uint32_t tx = 0;       // dernier ctr emis par la carte
  uint32_t since = 0;    // creation (provisoire) ou promotion (etablie)
  uint32_t lastAt = 0;   // dernier message au MAC juste (ou SALUT pour la provisoire)
  Peer peer;
};

enum class Verdict : uint8_t {
  Ok,          // Data : message accepte ; Salut : DEFI a emettre
  Invalid,     // forme refusee
  NoKey,       // pas de cle : transport coupe
  WrongKid,    // SALUT pour une autre cle
  Limited,     // plus de kDefiPerSecond DEFI dans la seconde
  UnknownSid,  // aucune session ne porte ce sid
  BadMac,
  Replay,      // ctr deja vu ou hors fenetre ; SALUT deja vu (meme na)
  Full,        // premier message, mais les deux places sont actives (kEvictIdleMs)
};
const char *verdictText(Verdict v);

class Table {
 public:
  // Cle en vigueur (nullptr : aucune). Changer de cle fait tout tomber. false :
  // empreinte incalculable, le transport reste coupe (hasKey() faux).
  bool setKey(const uint8_t *psk);
  bool hasKey() const { return hasKey_; }
  const char *kid() const { return kid_; }

  // Aleatoire de la plateforme (nc, sid), tire seulement pour un SALUT admis :
  // kid, MAC, na jamais vu, limite des DEFI.
  typedef void (*Random)(void *p, size_t n);
  // SALUT : session provisoire (une seule place, la precedente est remplacee)
  // et texte du DEFI dans out (kDefiLen octets, sans 0 final).
  Verdict onSalut(const Parsed &p, Random rnd, const Peer &from, uint32_t now, char out[kDefiLen]);
  bool sidInUse(uint32_t sid) const;

  // Message de l'app. Ok : *slot = session etablie (0..kSlots-1) ; *fresh vrai si
  // elle vient d'y etre promue (premier message au MAC juste de la poignee de
  // main) : l'etat de session JSON de cet emplacement repart de zero. La session
  // provisoire promue prend un emplacement libre, sinon celui de la session la
  // moins recemment active, si elle est muette depuis kEvictIdleMs ; sinon Full
  // (rien n'est promu ; ce ctr est brule, l'app renvoie avec un ctr neuf).
  Verdict onData(const Parsed &p, const Peer &from, uint32_t now, uint8_t *slot, bool *fresh);

  // En-tete d'un message de la carte pour l'emplacement slot, MAC calcule sur
  // payload : "H1 <sid> <ctr> <mac> " dans hdr, longueur rendue (0 : pas de
  // session, ou compteur epuise).
  size_t seal(uint8_t slot, const uint8_t *payload, size_t n, char hdr[kHeaderMax + 1]);

  // Oubli des sessions inactives. Rend le masque des emplacements liberes
  // (bit i : emplacement i).
  uint8_t expire(uint32_t now);
  // Tout tombe (cle changee ou effacee). Rend le masque des emplacements liberes.
  uint8_t clear();

  const Session &slot(uint8_t i) const { return est_[i < kSlots ? i : 0]; }
  const Session &provisional() const { return prov_; }
  uint8_t established() const;

 private:
  bool hasKey_ = false;
  uint8_t psk_[kKeyLen] = {};
  char kid_[kKidHex + 1] = {};
  Session est_[kSlots];
  Session prov_;
  uint32_t defiAt_ = 0;
  uint8_t defiN_ = 0;
  bool defiStarted_ = false;
  uint8_t seen_[kSeenNa][kNonceLen] = {};
  uint8_t seenN_ = 0, seenNext_ = 0;
};

}  // namespace h1
