// Tests hote de l'enveloppe H1 du transport reseau (src/h1_proto.*) :
// vecteurs calcules independamment (Python : hmac, hashlib ; les memes que
// docs/PROTOCOLE-JSON.md, 10.4), lecture stricte des datagrammes, fenetre
// contre le rejeu, poignee de main, promotion et oubli des sessions.
// Lancer : sh tools/test_halo1.sh
//
// La crypto de la plateforme vient ici de CommonCrypto (macOS) ; sur la carte,
// de mbedTLS (src/h1_crypto.cpp).
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "h1_proto.h"

using namespace h1;

namespace h1 {
bool hmacSha256(const uint8_t *key, size_t keyLen, const Part *parts, size_t nParts, uint8_t out[32]) {
  CCHmacContext c;
  CCHmacInit(&c, kCCHmacAlgSHA256, key, keyLen);
  for (size_t i = 0; i < nParts; i++) CCHmacUpdate(&c, parts[i].p, parts[i].n);
  CCHmacFinal(&c, out);
  return true;
}
bool sha256(const void *p, size_t n, uint8_t out[32]) {
  CC_SHA256(p, (CC_LONG)n, out);
  return true;
}
}  // namespace h1

static int gChecks = 0, gFails = 0;

#define CHECK(cond, ...)                              \
  do {                                                \
    gChecks++;                                        \
    if (!(cond)) {                                    \
      if (++gFails <= 40) {                           \
        printf("ECHEC %s:%d : ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                          \
        printf("\n");                                 \
      }                                               \
    }                                                 \
  } while (0)

// --- Vecteurs (Python, 24/09/2026) -------------------------------------------

static const char *kPskHex = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
static const char *kKid = "630DCD29";
static const char *kNa = "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF";
static const char *kNcHex = "505152535455565758595A5B5C5D5E5F";
static const uint32_t kSid = 0x1234ABCD;
static const char *kSalut = "H1 SALUT 630DCD29 A0A1A2A3A4A5A6A7A8A9AAABACADAEAF 52D853E3FFE9E9CCEFFA98BB5304B32D";
static const char *kDefi = "H1 DEFI 1234ABCD 505152535455565758595A5B5C5D5E5F BFF13F71B42243E6017D2807F8E6171F";
static const char *kKsHex = "20D6D83D97ED44F2BBF8CE56389BD475CBE2B625CE6CE24768B6B4C1C625012F";
static const char *kMsgA = "H1 1234ABCD 1 FD97A0C9E604524B49C763452D0310CE id=1 json 1";
static const char *kJsonC = "{\"v\":1,\"t\":\"hb\",\"n\":7,\"ms\":1234}";
static const char *kHdrC = "H1 1234ABCD 1 347A2E6A129BC822ECFF39BEC910451C ";
static const char *kMacAMax = "62CA08CFED5A5FE89EB9AAE8CC3D9CB7";  // A, ctr 4294967295, charge "x"

static uint8_t gPsk[32], gNc[16];

static Parsed P(const char *s) { return parse((const uint8_t *)s, strlen(s)); }

static Peer peer(uint8_t tag) {
  Peer p;
  p.ip[0] = 0xFD;
  p.ip[15] = tag;
  p.port = (uint16_t)(50000 + tag);
  p.local[0] = 0xFD;
  p.local[15] = 0x77;
  return p;
}


// Aleatoire de la plateforme, scripte : nc puis sid (octets dans l'ordre de la memoire).
static uint8_t gRnd[64];
static size_t gRndLen = 0, gRndPos = 0;
static uint32_t gRndCalls = 0;
static void setRnd(const uint8_t *nc, uint32_t sid) {
  memcpy(gRnd, nc, 16);
  memcpy(gRnd + 16, &sid, 4);
  gRndLen = 20;
  gRndPos = 0;
}
static void rnd(void *p, size_t n) {
  gRndCalls++;
  for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = gRndPos < gRndLen ? gRnd[gRndPos++] : 0x5A;
}

// SALUT signe avec la cle de test, na tire d'un compteur (un na neuf par appel).
static std::string salut(uint32_t tag, const uint8_t *key = nullptr) {
  uint8_t na[16] = {};
  memcpy(na, &tag, 4);
  na[15] = 0xA5;
  char naHex[33], kid[9];
  toHex(na, 16, naHex);
  keyId(key ? key : gPsk, kid);
  uint8_t mac[16];
  salutMac(key ? key : gPsk, kid, naHex, mac);
  char mh[33];
  toHex(mac, 16, mh);
  return std::string("H1 SALUT ") + kid + " " + naHex + " " + mh;
}

static Verdict salutV(Table &t, const std::string &m, uint32_t sid, uint32_t now, uint8_t tag = 1) {
  setRnd(gNc, sid);
  char defi[kDefiLen + 1] = {};
  return t.onSalut(parse((const uint8_t *)m.data(), m.size()), rnd, peer(tag), now, defi);
}

static std::string hex(const uint8_t *p, size_t n) {
  char b[129];
  toHex(p, n, b);
  return b;
}

// Message A signe pour la session s (tests sans vecteur fige).
static std::string msgA(const Session &s, uint32_t ctr, const char *payload) {
  uint8_t mac[kMacLen];
  messageMac(s.ks, 'A', s.sidHex, ctr, (const uint8_t *)payload, strlen(payload), mac);
  char b[256];
  snprintf(b, sizeof(b), "H1 %s %u %s %s", s.sidHex, ctr, hex(mac, kMacLen).c_str(), payload);
  return b;
}

static Verdict data(Table &t, const std::string &m, uint32_t now, uint8_t *slot, bool *fresh, uint8_t tag = 1) {
  const Parsed p = parse((const uint8_t *)m.data(), m.size());
  return t.onData(p, peer(tag), now, slot, fresh);
}

// Poignee de main complete : SALUT, DEFI, premier message. Rend l'emplacement.
static int establish(Table &t, uint32_t sid, uint32_t now, uint8_t tag) {
  if (salutV(t, salut(sid), sid, now, tag) != Verdict::Ok) return -1;
  const Session s = t.provisional();
  uint8_t slot = 0xFF;
  bool fresh = false;
  if (data(t, msgA(s, 1, "id=1 json 1"), now, &slot, &fresh, tag) != Verdict::Ok || !fresh) return -1;
  return slot;
}

// --- Tests -----------------------------------------------------------------------

static void testVectors() {
  CHECK(fromHex(kPskHex, 32, gPsk), "cle de test");
  CHECK(fromHex(kNcHex, 16, gNc), "nc de test");
  char kid[9];
  CHECK(keyId(gPsk, kid) && !strcmp(kid, kKid), "kid : %s", kid);

  uint8_t ks[32];
  CHECK(sessionKey(gPsk, kNa, kNcHex, "1234ABCD", ks) && hex(ks, 32) == kKsHex, "Ks : %s", hex(ks, 32).c_str());
  uint8_t mac[16];
  CHECK(messageMac(ks, 'A', "1234ABCD", 4294967295u, (const uint8_t *)"x", 1, mac) && hex(mac, 16) == kMacAMax,
        "MAC ctr max : %s", hex(mac, 16).c_str());

  Table t;
  CHECK(t.setKey(gPsk) && t.hasKey() && !strcmp(t.kid(), kKid), "setKey");
  char defi[kDefiLen + 1] = {};
  const Parsed ps = P(kSalut);
  uint8_t ms[16];
  CHECK(ps.kind == Kind::Salut && salutMac(gPsk, kKid, kNa, ms) && equalCt(ms, ps.mac, 16), "MAC du SALUT");
  setRnd(gNc, kSid);
  const Verdict v = t.onSalut(ps, rnd, peer(1), 1000, defi);
  CHECK(v == Verdict::Ok && std::string(defi, kDefiLen) == kDefi, "DEFI : %s (%s)", defi, verdictText(v));
  CHECK(t.onSalut(ps, rnd, peer(1), 1100, defi) == Verdict::Replay, "SALUT rejoue (meme na) : ignore");
  CHECK(!memcmp(t.provisional().ks, ks, 32), "Ks de la session provisoire");
  CHECK(t.established() == 0, "rien d'etabli avant le premier message");

  // Le DEFI se relit (cote app) et son MAC se verifie.
  const Parsed d = P(kDefi);
  uint8_t md[16];
  CHECK(d.kind == Kind::Defi && d.sid == kSid && !strcmp(d.nc, kNcHex), "lecture du DEFI");
  CHECK(defiMac(gPsk, kKid, kNa, d.nc, d.sidHex, md) && equalCt(md, d.mac, 16), "MAC du DEFI");

  uint8_t slot = 0xFF;
  bool fresh = false;
  const Verdict va = data(t, kMsgA, 1100, &slot, &fresh, 2);
  CHECK(va == Verdict::Ok && slot == 0 && fresh, "premier message : %s slot %u", verdictText(va), slot);
  CHECK(t.established() == 1 && !t.provisional().used, "session promue");
  CHECK(t.slot(0).peer.port == peer(2).port, "adresse du dernier message au MAC juste");
  const Parsed a = P(kMsgA);
  CHECK(a.payloadLen == 11 && !memcmp(a.payload, "id=1 json 1", 11), "charge");

  char hdr[kHeaderMax + 1];
  const size_t n = t.seal(0, (const uint8_t *)kJsonC, strlen(kJsonC), hdr);
  CHECK(n == strlen(kHdrC) && !strcmp(hdr, kHdrC), "en-tete C : %s", hdr);
  const size_t n2 = t.seal(0, (const uint8_t *)kJsonC, strlen(kJsonC), hdr);
  CHECK(n2 && !strncmp(hdr, "H1 1234ABCD 2 ", 14), "ctr C suivant : %s", hdr);
  CHECK(t.seal(1, (const uint8_t *)"x", 1, hdr) == 0, "emplacement vide : rien");
}

static void testParse() {
  CHECK(P(kSalut).kind == Kind::Salut, "SALUT");
  CHECK(!strcmp(P(kSalut).na, kNa) && !strcmp(P(kSalut).kid, kKid), "SALUT : champs");
  CHECK(strlen(kSalut) == kSalutLen, "longueur du SALUT");
  CHECK(P("H1 SALUT 630dcd29 A0A1A2A3A4A5A6A7A8A9AAABACADAEAF 52D853E3FFE9E9CCEFFA98BB5304B32D").kind ==
            Kind::Invalid,
        "minuscules refusees");
  CHECK(P("H1 SALUT 630DCD29 A0A1A2A3A4A5A6A7A8A9AAABACADAEAF 52D853E3FFE9E9CCEFFA98BB5304B32D ").kind ==
            Kind::Invalid,
        "espace finale");
  CHECK(P("H1 SALUT 630DCD29 A0A1A2A3A4A5A6A7A8A9AAABACADAEAF").kind == Kind::Invalid, "SALUT sans MAC (v0)");
  CHECK(P("H1 SALUT 630DCD29 A0A1A2A3A4A5A6A7A8A9AAABACADAEA 52D853E3FFE9E9CCEFFA98BB5304B32D").kind == Kind::Invalid,
        "na court");
  CHECK(P("H1 SALUT 630DCD29  A0A1A2A3A4A5A6A7A8A9AAABACADAEAF 52D853E3FFE9E9CCEFFA98BB5304B32D").kind ==
            Kind::Invalid,
        "double espace");
  CHECK(P("h1 SALUT 630DCD29 A0A1A2A3A4A5A6A7A8A9AAABACADAEAF 52D853E3FFE9E9CCEFFA98BB5304B32D").kind ==
            Kind::Invalid,
        "prefixe");
  CHECK(P("").kind == Kind::Invalid && P("H1").kind == Kind::Invalid && P("H1 ").kind == Kind::Invalid, "vide");

  const Parsed m = P(kMsgA);
  CHECK(m.kind == Kind::Data && m.sid == kSid && m.ctr == 1, "message");
  CHECK(P("H1 1234ABCD 01 FD97A0C9E604524B49C763452D0310CE x").kind == Kind::Invalid, "ctr : zero de tete");
  CHECK(P("H1 1234ABCD 0 FD97A0C9E604524B49C763452D0310CE x").kind == Kind::Invalid, "ctr 0");
  CHECK(P("H1 1234ABCD 4294967296 FD97A0C9E604524B49C763452D0310CE x").kind == Kind::Invalid, "ctr > 2^32-1");
  CHECK(P("H1 1234ABCD 99999999999 FD97A0C9E604524B49C763452D0310CE x").kind == Kind::Invalid, "ctr 11 chiffres");
  const Parsed mx = P("H1 1234ABCD 4294967295 62CA08CFED5A5FE89EB9AAE8CC3D9CB7 x");
  CHECK(mx.kind == Kind::Data && mx.ctr == 4294967295u, "ctr max");
  CHECK(P("H1 1234ABCD 1 FD97A0C9E604524B49C763452D0310CE").kind == Kind::Invalid, "sans espace avant la charge");
  const Parsed e = P("H1 1234ABCD 1 FD97A0C9E604524B49C763452D0310CE ");
  CHECK(e.kind == Kind::Data && e.payloadLen == 0, "charge vide lisible (refusee plus haut)");
  CHECK(P("H1 1234abcd 1 FD97A0C9E604524B49C763452D0310CE x").kind == Kind::Invalid, "sid minuscule");
  CHECK(P("H1 1234ABCD 1 fd97A0C9E604524B49C763452D0310CE x").kind == Kind::Invalid, "mac minuscule");
  CHECK(P("H1 1234ABCD 1a FD97A0C9E604524B49C763452D0310CE x").kind == Kind::Invalid, "ctr non decimal");
  const Parsed sp = P("H1 1234ABCD 3 FD97A0C9E604524B49C763452D0310CE id=2 lampe  on ");
  CHECK(sp.kind == Kind::Data && sp.payloadLen == 15, "charge : octet pour octet, espaces compris");
}

static void testWindow() {
  Window w;
  CHECK(!w.fresh(0), "ctr 0");
  CHECK(w.fresh(1), "1");
  w.commit(1);
  CHECK(!w.fresh(1), "1 rejoue");
  w.commit(40);
  CHECK(!w.fresh(8) && w.fresh(9), "fenetre de 32 : 8 hors, 9 dedans");
  w.commit(9);
  CHECK(!w.fresh(9) && w.fresh(10), "9 vu, 10 pas");
  CHECK(!w.fresh(40) && w.fresh(41), "haut de fenetre");
  w.commit(100);
  CHECK(!w.fresh(40) && !w.fresh(68) && w.fresh(69) && w.fresh(99), "saut de 60");
  w.commit(4294967295u);
  CHECK(!w.fresh(4294967295u) && w.fresh(4294967294u), "ctr max");
}

static void testSessions() {
  Table t;
  CHECK(salutV(t, salut(1), 11, 0) == Verdict::NoKey, "sans cle");
  t.setKey(gPsk);
  uint8_t other[32];
  for (uint8_t i = 0; i < 32; i++) other[i] = (uint8_t)(0xFF - i);
  CHECK(salutV(t, salut(2, other), 11, 0) == Verdict::WrongKid, "autre cle");
  // MAC faux : refuse, et aucun aleatoire tire (le cout d'un SALUT sans la cle reste nul).
  std::string bad = salut(3);
  bad[bad.size() - 1] = bad[bad.size() - 1] == '0' ? '1' : '0';
  const uint32_t calls = gRndCalls;
  CHECK(salutV(t, bad, 11, 0) == Verdict::BadMac && gRndCalls == calls, "SALUT au MAC faux");
  // sid tire nul : retire.
  CHECK(salutV(t, salut(4), 0, 100) == Verdict::Ok && t.provisional().sid == 0x5A5A5A5Au, "sid nul retire");

  // Limite globale : 2 DEFI par seconde ; un SALUT limite ne remplace rien.
  t.clear();
  CHECK(salutV(t, salut(10), 11, 5000) == Verdict::Ok, "DEFI 1");
  CHECK(salutV(t, salut(11), 12, 5300) == Verdict::Ok, "DEFI 2");
  CHECK(salutV(t, salut(12), 13, 5900) == Verdict::Limited, "DEFI 3 dans la seconde");
  CHECK(t.provisional().sid == 12, "la limite ne remplace pas la provisoire");
  CHECK(salutV(t, salut(13), 14, 6000) == Verdict::Ok, "seconde suivante");
  CHECK(salutV(t, salut(13), 15, 7000) == Verdict::Replay, "SALUT rejoue plus tard : ignore");

  // Une nouvelle poignee de main remplace la provisoire : l'ancienne n'aboutit plus.
  t.clear();
  salutV(t, salut(20), 21, 10000);
  const Session old = t.provisional();
  salutV(t, salut(21), 22, 10100, 2);
  uint8_t slot;
  bool fresh;
  CHECK(data(t, msgA(old, 1, "id=1 json 1"), 10200, &slot, &fresh) == Verdict::UnknownSid, "provisoire remplacee");

  // MAC faux : refuse, et la fenetre n'avance pas.
  const Session cur = t.provisional();
  std::string badMsg = msgA(cur, 5, "id=1 json 1");
  badMsg[badMsg.size() - 1] = '2';
  CHECK(data(t, badMsg, 10300, &slot, &fresh) == Verdict::BadMac, "charge modifiee");
  CHECK(t.established() == 0, "rien de promu sur un MAC faux");
  CHECK(data(t, msgA(cur, 5, "id=1 json 1"), 10400, &slot, &fresh, 2) == Verdict::Ok && fresh && slot == 0, "promue");
  CHECK(data(t, msgA(cur, 5, "id=1 json 1"), 10500, &slot, &fresh) == Verdict::Replay, "rejeu");
  CHECK(data(t, msgA(cur, 4, "id=2 json ping"), 10600, &slot, &fresh) == Verdict::Ok && !fresh, "desordre accepte");
  const Session s0 = t.slot(0);
  CHECK(data(t, msgA(s0, 6, "x").replace(3, 8, "DEADBEEF"), 10700, &slot, &fresh) == Verdict::UnknownSid,
        "sid inconnu");

  // Adresse des reponses : celle du plus recent message seulement.
  CHECK(data(t, msgA(s0, 10, "id=3 json ping"), 10800, &slot, &fresh, 3) == Verdict::Ok &&
            t.slot(0).peer.port == peer(3).port,
        "message le plus recent : adresse suivie");
  CHECK(data(t, msgA(s0, 9, "id=4 json ping"), 10900, &slot, &fresh, 4) == Verdict::Ok &&
            t.slot(0).peer.port == peer(3).port,
        "message plus ancien (retarde, rejoue d'ailleurs) : adresse inchangee");

  // Deux sessions etablies ; une troisieme ne chasse qu'une session muette depuis 30 s.
  CHECK(establish(t, 31, 20000, 5) == 1, "seconde session, emplacement libre");
  CHECK(t.established() == 2, "deux etablies");
  const Session s1 = t.slot(1);
  CHECK(data(t, msgA(s0, 11, "id=5 json ping"), 30000, &slot, &fresh) == Verdict::Ok && slot == 0, "0 active");
  CHECK(salutV(t, salut(40), 41, 40000, 6) == Verdict::Ok, "troisieme client : poignee de main");
  const Session s2 = t.provisional();
  const std::string first = msgA(s2, 1, "id=1 json 1");
  CHECK(data(t, first, 40000, &slot, &fresh, 6) == Verdict::Full, "deux places actives : complet");
  CHECK(t.provisional().used && t.established() == 2, "rien de change");
  CHECK(data(t, msgA(s1, 2, "id=2 json ping"), 45000, &slot, &fresh) == Verdict::Ok && slot == 1, "1 toujours la");
  // Ce message ne peut plus servir (ctr brule : pas de promotion vers l'adresse d'un rejoueur).
  CHECK(data(t, first, 60000, &slot, &fresh, 9) == Verdict::Replay && t.provisional().used, "message refuse rejoue");
  // 30 s plus tard, la 0 (dernier message a 30 s) est muette depuis 30 s : le renvoi (ctr neuf) passe.
  CHECK(data(t, msgA(s2, 2, "id=1 json 1"), 60000, &slot, &fresh, 6) == Verdict::Ok && fresh && slot == 0 &&
            t.slot(0).peer.port == peer(6).port,
        "0 remplacee apres 30 s");
  CHECK(data(t, msgA(s0, 12, "id=6 json ping"), 60100, &slot, &fresh) == Verdict::UnknownSid, "0 evincee");
  CHECK(data(t, msgA(s1, 3, "id=3 json ping"), 60200, &slot, &fresh) == Verdict::Ok && slot == 1, "1 intacte");

  // Oubli : 10 min sans message valide ; provisoire apres 30 s.
  salutV(t, salut(50), 51, 60300, 7);
  CHECK(t.expire(60300 + kProvisionalMs - 1) == 0 && t.provisional().used, "provisoire encore la");
  CHECK(t.expire(60300 + kProvisionalMs) == 0 && !t.provisional().used, "provisoire oubliee");
  CHECK(t.expire(60000 + kForgetMs - 1) == 0, "rien a 10 min moins 1 ms");
  CHECK(t.expire(60200 + kForgetMs) == 0x03, "les deux oubliees (derniers messages a 60,0 et 60,2 s)");
  CHECK(t.established() == 0, "table vide");

  // Nouvelle cle : tout tombe ; sans cle, plus rien ne passe.
  CHECK(establish(t, 61, 700000, 8) == 0, "session");
  CHECK(t.setKey(nullptr) && !t.hasKey() && t.established() == 0, "cle effacee");
  CHECK(salutV(t, salut(62), 62, 700100) == Verdict::NoKey, "sans cle apres effacement");
  t.setKey(gPsk);
  CHECK(establish(t, 63, 700200, 8) == 0, "cle remise");
  CHECK(t.clear() == 0x01 && t.established() == 0, "clear rend le masque");

  // wipe
  uint8_t secret[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  wipe(secret, sizeof(secret));
  uint8_t zero[8] = {};
  CHECK(!memcmp(secret, zero, 8), "wipe");
}

int main() {
  testVectors();
  testParse();
  testWindow();
  testSessions();
  printf("test_h1 : %d verification(s), %d echec(s)\n", gChecks, gFails);
  return gFails ? 1 : 0;
}
