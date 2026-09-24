#include "h1_proto.h"

#include <string.h>

namespace h1 {

// ===========================================================================
//  Outils
// ===========================================================================

static const char kHex[] = "0123456789ABCDEF";

bool equalCt(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

void wipe(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  while (n--) *v++ = 0;
}

void toHex(const uint8_t *p, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = kHex[p[i] >> 4];
    out[2 * i + 1] = kHex[p[i] & 0x0F];
  }
  out[2 * n] = 0;
}

static int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;  // minuscules refusees : un seul texte canonique
}

bool fromHex(const char *s, size_t n, uint8_t *out) {
  for (size_t i = 0; i < n; i++) {
    const int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

static void hex32(uint32_t v, char out[kSidHex + 1]) {
  for (int i = 7; i >= 0; i--) {
    out[i] = kHex[v & 0x0F];
    v >>= 4;
  }
  out[8] = 0;
}

// Decimal sans zero de tete ; rend la longueur (1..10).
static size_t decimal(uint32_t v, char out[11]) {
  char t[10];
  size_t n = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  for (size_t i = 0; i < n; i++) out[i] = t[n - 1 - i];
  out[n] = 0;
  return n;
}

bool keyId(const uint8_t psk[kKeyLen], char kid[kKidHex + 1]) {
  uint8_t d[32];
  const bool ok = sha256(psk, kKeyLen, d);
  if (ok) toHex(d, kKidHex / 2, kid);
  wipe(d, sizeof(d));
  return ok;
}

bool salutMac(const uint8_t psk[kKeyLen], const char *kid, const char *naHex, uint8_t out[kMacLen]) {
  const Part parts[] = {{"H1|SALUT|", 9}, {kid, kKidHex}, {"|", 1}, {naHex, kNonceHex}};
  uint8_t full[32];
  const bool ok = hmacSha256(psk, kKeyLen, parts, sizeof(parts) / sizeof(parts[0]), full);
  if (ok) memcpy(out, full, kMacLen);
  wipe(full, sizeof(full));
  return ok;
}

bool defiMac(const uint8_t psk[kKeyLen], const char *kid, const char *naHex, const char *ncHex, const char *sidHex,
             uint8_t out[kMacLen]) {
  const Part parts[] = {{"H1|DEFI|", 8}, {kid, kKidHex},   {"|", 1}, {naHex, kNonceHex},
                        {"|", 1},        {ncHex, kNonceHex}, {"|", 1}, {sidHex, kSidHex}};
  uint8_t full[32];
  const bool ok = hmacSha256(psk, kKeyLen, parts, sizeof(parts) / sizeof(parts[0]), full);
  if (ok) memcpy(out, full, kMacLen);
  wipe(full, sizeof(full));
  return ok;
}

bool sessionKey(const uint8_t psk[kKeyLen], const char *naHex, const char *ncHex, const char *sidHex,
                uint8_t ks[kKeyLen]) {
  const Part parts[] = {{"H1|SESSION|", 11}, {naHex, kNonceHex}, {"|", 1},
                        {ncHex, kNonceHex},  {"|", 1},           {sidHex, kSidHex}};
  return hmacSha256(psk, kKeyLen, parts, sizeof(parts) / sizeof(parts[0]), ks);
}

bool messageMac(const uint8_t ks[kKeyLen], char dir, const char *sidHex, uint32_t ctr, const uint8_t *payload,
                size_t n, uint8_t out[kMacLen]) {
  char c[11];
  const size_t cn = decimal(ctr, c);
  const char d[2] = {dir, '|'};
  const Part parts[] = {{d, 2}, {sidHex, kSidHex}, {"|", 1}, {c, cn}, {"|", 1}, {payload, n}};
  uint8_t full[32];
  const bool ok = hmacSha256(ks, kKeyLen, parts, sizeof(parts) / sizeof(parts[0]), full);
  if (ok) memcpy(out, full, kMacLen);
  wipe(full, sizeof(full));
  return ok;
}

// ===========================================================================
//  Lecture
// ===========================================================================

// Jeton de len caracteres exactement a d[*i], suivi d'une espace (ou de la fin
// si last) ; *i passe apres l'espace.
static bool token(const uint8_t *d, size_t n, size_t *i, size_t len, bool last) {
  if (*i + len > n) return false;
  const size_t end = *i + len;
  if (last ? end != n : (end >= n || d[end] != ' ')) return false;
  *i = end + (last ? 0 : 1);
  return true;
}

static bool hexToken(const uint8_t *d, size_t at, size_t hexLen, char *text, uint8_t *bytes) {
  memcpy(text, d + at, hexLen);
  text[hexLen] = 0;
  uint8_t tmp[kMacLen];
  uint8_t *out = bytes ? bytes : tmp;
  return fromHex(text, hexLen / 2, out);
}

Parsed parse(const uint8_t *d, size_t n) {
  Parsed p;
  if (n < 3 || memcmp(d, "H1 ", 3)) return p;
  size_t i = 3;
  if (n - i >= 6 && !memcmp(d + i, "SALUT ", 6)) {
    i += 6;
    size_t at = i;
    if (!token(d, n, &i, kKidHex, false) || !hexToken(d, at, kKidHex, p.kid, nullptr)) return p;
    at = i;
    uint8_t na[kNonceLen];
    if (!token(d, n, &i, kNonceHex, false)) return p;
    memcpy(p.na, d + at, kNonceHex);
    p.na[kNonceHex] = 0;
    if (!fromHex(p.na, kNonceLen, na)) return p;
    at = i;
    char mac[kMacHex + 1];
    if (!token(d, n, &i, kMacHex, true) || !hexToken(d, at, kMacHex, mac, p.mac)) return p;
    p.kind = Kind::Salut;
    return p;
  }
  if (n - i >= 5 && !memcmp(d + i, "DEFI ", 5)) {
    i += 5;
    size_t at = i;
    uint8_t sid[4];
    if (!token(d, n, &i, kSidHex, false) || !hexToken(d, at, kSidHex, p.sidHex, sid)) return p;
    p.sid = (uint32_t)sid[0] << 24 | (uint32_t)sid[1] << 16 | (uint32_t)sid[2] << 8 | sid[3];
    at = i;
    uint8_t nc[kNonceLen];
    if (!token(d, n, &i, kNonceHex, false)) return p;
    memcpy(p.nc, d + at, kNonceHex);
    p.nc[kNonceHex] = 0;
    if (!fromHex(p.nc, kNonceLen, nc)) return p;
    at = i;
    char mac[kMacHex + 1];
    if (!token(d, n, &i, kMacHex, true) || !hexToken(d, at, kMacHex, mac, p.mac)) return p;
    p.kind = Kind::Defi;
    return p;
  }
  // Message : sid, ctr, mac, charge.
  size_t at = i;
  uint8_t sid[4];
  if (!token(d, n, &i, kSidHex, false) || !hexToken(d, at, kSidHex, p.sidHex, sid)) return p;
  p.sid = (uint32_t)sid[0] << 24 | (uint32_t)sid[1] << 16 | (uint32_t)sid[2] << 8 | sid[3];
  // ctr : 1 a 10 chiffres, sans zero de tete, au plus 4294967295.
  uint64_t ctr = 0;
  size_t digits = 0;
  while (i < n && d[i] >= '0' && d[i] <= '9' && digits < 11) {
    ctr = ctr * 10 + (uint64_t)(d[i] - '0');
    i++;
    digits++;
  }
  if (!digits || digits > 10 || d[i - digits] == '0' || ctr > 0xFFFFFFFFull || i >= n || d[i] != ' ') return p;
  p.ctr = (uint32_t)ctr;
  i++;
  at = i;
  char mac[kMacHex + 1];
  if (!token(d, n, &i, kMacHex, false) || !hexToken(d, at, kMacHex, mac, p.mac)) return p;
  p.payload = d + i;
  p.payloadLen = n - i;
  p.kind = Kind::Data;
  return p;
}

// ===========================================================================
//  Fenetre
// ===========================================================================

bool Window::fresh(uint32_t ctr) const {
  if (ctr == 0) return false;
  if (ctr > top) return true;
  const uint32_t back = top - ctr;
  if (back >= kWindow) return false;
  return !(bits & (1u << back));
}

void Window::commit(uint32_t ctr) {
  if (ctr > top) {
    const uint32_t shift = ctr - top;
    bits = shift >= kWindow ? 0 : bits << shift;
    bits |= 1u;
    top = ctr;
  } else if (top - ctr < kWindow) {
    bits |= 1u << (top - ctr);
  }
}

// ===========================================================================
//  Sessions
// ===========================================================================

const char *verdictText(Verdict v) {
  switch (v) {
    case Verdict::Ok: return "ok";
    case Verdict::Invalid: return "forme";
    case Verdict::NoKey: return "sans_cle";
    case Verdict::WrongKid: return "autre_cle";
    case Verdict::Limited: return "limite";
    case Verdict::UnknownSid: return "sid_inconnu";
    case Verdict::BadMac: return "mac_faux";
    case Verdict::Replay: return "rejeu";
    case Verdict::Full: return "complet";
  }
  return "?";
}

bool Table::setKey(const uint8_t *psk) {
  clear();
  hasKey_ = false;
  wipe(psk_, sizeof(psk_));
  kid_[0] = 0;
  wipe(seen_, sizeof(seen_));
  seenN_ = seenNext_ = 0;
  if (!psk) return true;
  if (!keyId(psk, kid_)) {
    kid_[0] = 0;
    return false;
  }
  memcpy(psk_, psk, kKeyLen);
  hasKey_ = true;
  return true;
}

bool Table::sidInUse(uint32_t sid) const {
  for (const Session &s : est_)
    if (s.used && s.sid == sid) return true;
  return prov_.used && prov_.sid == sid;
}

Verdict Table::onSalut(const Parsed &p, Random rnd, const Peer &from, uint32_t now, char out[kDefiLen]) {
  if (p.kind != Kind::Salut) return Verdict::Invalid;
  if (!hasKey_) return Verdict::NoKey;
  if (memcmp(p.kid, kid_, kKidHex)) return Verdict::WrongKid;
  // Limite globale : pas par source (adresses usurpables), pas d'amplification.
  // Jugee avant le HMAC (un SALUT de trop ne coute rien) ; seul un SALUT servi
  // la consomme.
  if (!defiStarted_ || now - defiAt_ >= 1000) {
    defiStarted_ = true;
    defiAt_ = now;
    defiN_ = 0;
  }
  if (defiN_ >= kDefiPerSecond) return Verdict::Limited;
  // Seul qui a la cle passe : ni DEFI depense, ni poignee de main remplacee.
  uint8_t mac[kMacLen];
  if (!salutMac(psk_, kid_, p.na, mac) || !equalCt(mac, p.mac, kMacLen)) return Verdict::BadMac;
  // Un SALUT rejoue (meme na) n'est plus servi. L'app tire un na neuf a chaque essai.
  uint8_t na[kNonceLen];
  fromHex(p.na, kNonceLen, na);  // deja verifie par parse()
  for (uint8_t i = 0; i < seenN_; i++)
    if (!memcmp(seen_[i], na, kNonceLen)) return Verdict::Replay;

  uint8_t nc[kNonceLen];
  rnd(nc, sizeof(nc));
  uint32_t sid = 0;
  for (uint8_t i = 0; i < 4 && (!sid || sidInUse(sid)); i++) rnd(&sid, sizeof(sid));
  if (!sid || sidInUse(sid)) return Verdict::Invalid;

  Session s;
  s.used = true;
  s.sid = sid;
  hex32(sid, s.sidHex);
  char ncHex[kNonceHex + 1];
  toHex(nc, kNonceLen, ncHex);
  const bool ok = sessionKey(psk_, p.na, ncHex, s.sidHex, s.ks) && defiMac(psk_, kid_, p.na, ncHex, s.sidHex, mac);
  if (!ok) {  // echec de la plateforme : aucune session, aucun DEFI
    wipe(&s, sizeof(s));
    return Verdict::Invalid;
  }
  defiN_++;
  memcpy(seen_[seenNext_], na, kNonceLen);
  seenNext_ = (uint8_t)((seenNext_ + 1) % kSeenNa);
  if (seenN_ < kSeenNa) seenN_++;
  s.since = s.lastAt = now;
  s.peer = from;
  prov_ = s;
  wipe(&s, sizeof(s));

  char macHex[kMacHex + 1];
  toHex(mac, kMacLen, macHex);
  memcpy(out, "H1 DEFI ", 8);
  memcpy(out + 8, prov_.sidHex, kSidHex);
  out[16] = ' ';
  memcpy(out + 17, ncHex, kNonceHex);
  out[49] = ' ';
  memcpy(out + 50, macHex, kMacHex);
  return Verdict::Ok;
}

Verdict Table::onData(const Parsed &p, const Peer &from, uint32_t now, uint8_t *slot, bool *fresh) {
  *fresh = false;
  if (p.kind != Kind::Data) return Verdict::Invalid;
  if (!hasKey_) return Verdict::NoKey;
  Session *s = nullptr;
  int idx = -1;
  for (uint8_t i = 0; i < kSlots; i++)
    if (est_[i].used && est_[i].sid == p.sid) {
      s = &est_[i];
      idx = i;
    }
  if (!s && prov_.used && prov_.sid == p.sid) s = &prov_;
  if (!s) return Verdict::UnknownSid;
  uint8_t mac[kMacLen];
  if (!messageMac(s->ks, 'A', s->sidHex, p.ctr, p.payload, p.payloadLen, mac) || !equalCt(mac, p.mac, kMacLen))
    return Verdict::BadMac;  // echec de la plateforme compris : jamais accepte
  // Le rejeu se juge apres le MAC : un ctr forge ne pousse jamais la fenetre.
  if (!s->rx.fresh(p.ctr)) return Verdict::Replay;
  // Seul le plus recent message fixe l'adresse des reponses.
  const bool newest = p.ctr > s->rx.top;
  if (idx >= 0) {
    s->rx.commit(p.ctr);
    s->lastAt = now;
    if (newest) s->peer = from;
    *slot = (uint8_t)idx;
    return Verdict::Ok;
  }
  // Premier message au MAC juste de la poignee de main : la session devient
  // etablie, dans un emplacement libre, sinon a la place de la moins
  // recemment active si elle est muette depuis kEvictIdleMs. Des SALUT du
  // LAN (sans la cle) ne touchent a rien ; trois clients pour deux places ne se
  // chassent pas en boucle.
  uint8_t k = 0;
  bool found = false;
  for (uint8_t i = 0; i < kSlots && !found; i++)
    if (!est_[i].used) {
      k = i;
      found = true;
    }
  if (!found) {
    uint32_t oldest = 0;
    for (uint8_t i = 0; i < kSlots; i++) {
      const uint32_t age = now - est_[i].lastAt;
      if (i == 0 || age > oldest) {
        oldest = age;
        k = i;
      }
    }
    if (oldest < kEvictIdleMs) {
      // Rien de promu, mais ce ctr est brule : un rejeu de ce message ne
      // promouvra jamais la session vers l'adresse d'un autre. L'app renvoie
      // avec un ctr neuf.
      prov_.rx.commit(p.ctr);
      return Verdict::Full;
    }
  }
  prov_.rx.commit(p.ctr);
  prov_.lastAt = now;
  prov_.peer = from;
  est_[k] = prov_;
  est_[k].since = now;
  prov_ = Session();
  *slot = k;
  *fresh = true;
  return Verdict::Ok;
}

size_t Table::seal(uint8_t slot, const uint8_t *payload, size_t n, char hdr[kHeaderMax + 1]) {
  if (slot >= kSlots) return 0;
  Session &s = est_[slot];
  if (!s.used || s.tx == 0xFFFFFFFFu) return 0;
  uint8_t mac[kMacLen];
  if (!messageMac(s.ks, 'C', s.sidHex, s.tx + 1, payload, n, mac)) return 0;
  s.tx++;
  size_t k = 0;
  memcpy(hdr, "H1 ", 3);
  k = 3;
  memcpy(hdr + k, s.sidHex, kSidHex);
  k += kSidHex;
  hdr[k++] = ' ';
  k += decimal(s.tx, hdr + k);
  hdr[k++] = ' ';
  toHex(mac, kMacLen, hdr + k);
  k += kMacHex;
  hdr[k++] = ' ';
  hdr[k] = 0;
  return k;
}

uint8_t Table::expire(uint32_t now) {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < kSlots; i++)
    if (est_[i].used && now - est_[i].lastAt >= kForgetMs) {
      est_[i] = Session();
      mask |= (uint8_t)(1u << i);
    }
  if (prov_.used && now - prov_.lastAt >= kProvisionalMs) prov_ = Session();
  return mask;
}

uint8_t Table::clear() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < kSlots; i++) {
    if (est_[i].used) mask |= (uint8_t)(1u << i);
    est_[i] = Session();
  }
  prov_ = Session();
  return mask;
}

uint8_t Table::established() const {
  uint8_t n = 0;
  for (const Session &s : est_) n += s.used ? 1 : 0;
  return n;
}

}  // namespace h1
