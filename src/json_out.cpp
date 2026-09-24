#include "json_out.h"

#include <string.h>

#include "halo1_map.h"

namespace jsonp {

using namespace halo1;

// ===========================================================================
//  Writer
// ===========================================================================

void Writer::put(char c) {
  if (len_ < kLineMax) buf_[len_++] = (uint8_t)c;
  else over_ = true;
}

void Writer::puts(const char *s) {
  while (*s) put(*s++);
}

// Virgule avant tout champ sauf le premier de son niveau, puis la cle.
void Writer::sep(const char *k) {
  if (!depth_) {
    bad_ = true;
    return;
  }
  if (!first_[depth_ - 1]) put(',');
  first_[depth_ - 1] = false;
  if (k) {
    put('"');
    puts(k);
    put('"');
    put(':');
  }
}

void Writer::num(uint32_t v, bool neg) {
  char t[10];
  uint8_t i = 0;
  do {
    t[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  if (neg) put('-');
  while (i) put(t[--i]);
}

void Writer::begin(const char *type, uint32_t n, uint32_t ms) {
  len_ = 0;
  over_ = bad_ = false;
  depth_ = 1;
  first_[0] = true;
  close_[0] = '}';
  put((char)kRS);
  put('{');
  u32("v", kVersion);
  str("t", type);
  u32("n", n);
  u32("ms", ms);
}

void Writer::str(const char *k, const char *v, size_t max) {
  sep(k);
  if (!v) {
    puts("null");
    return;
  }
  put('"');
  for (size_t i = 0; v[i] && i < max; i++) {
    const uint8_t c = (uint8_t)v[i];
    if (c == '"' || c == '\\') {
      put('\\');
      put((char)c);
    } else if (c < 0x20 || c > 0x7E) {
      put('?');  // jamais de \uXXXX, jamais d'octet hors ASCII imprimable
    } else {
      put((char)c);
    }
  }
  put('"');
}

void Writer::u32(const char *k, uint32_t v) {
  sep(k);
  num(v, false);
}

void Writer::i32(const char *k, int32_t v) {
  sep(k);
  if (v < 0) num((uint32_t)(-(int64_t)v), true);
  else num((uint32_t)v, false);
}

void Writer::boolean(const char *k, bool v) {
  sep(k);
  puts(v ? "true" : "false");
}

void Writer::null(const char *k) {
  sep(k);
  puts("null");
}

static const char kHexDigits[] = "0123456789ABCDEF";

void Writer::hex(const char *k, const uint8_t *p, size_t n) {
  sep(k);
  put('"');
  for (size_t i = 0; i < n; i++) {
    put(kHexDigits[p[i] >> 4]);
    put(kHexDigits[p[i] & 0x0F]);
  }
  put('"');
}

void Writer::hexU32(const char *k, uint32_t v, uint8_t digits, bool prefix0x) {
  sep(k);
  put('"');
  if (prefix0x) puts("0x");
  if (digits > 8) digits = 8;
  for (int8_t i = (int8_t)digits - 1; i >= 0; i--) put(kHexDigits[(v >> (4 * i)) & 0x0F]);
  put('"');
}

void Writer::open(const char *k, char o, char c) {
  sep(k);
  put(o);
  if (depth_ >= kDepth) {
    bad_ = true;
    return;
  }
  first_[depth_] = true;
  close_[depth_] = c;
  depth_++;
}

void Writer::obj(const char *k) { open(k, '{', '}'); }
void Writer::arr(const char *k) { open(k, '[', ']'); }

void Writer::end() {
  if (depth_ <= 1) {
    bad_ = true;
    return;
  }
  depth_--;
  put(close_[depth_]);
}

bool Writer::finish() {
  if (depth_ != 1) bad_ = true;
  put('}');
  put('\n');
  depth_ = 0;
  return !over_ && !bad_;
}

bool Writer::setN(uint32_t n) {
  if (over_ || bad_ || depth_) return false;
  // RS {"v":1,"t":"<type>","n":<chiffres>,... : le type est un litteral du
  // firmware, sans '"' ; le premier ,"n": est donc le bon.
  static const char kKey[] = ",\"n\":";
  const size_t kl = sizeof(kKey) - 1;
  size_t p = 0;
  while (p + kl <= len_ && memcmp(buf_ + p, kKey, kl)) p++;
  if (p + kl > len_) return false;
  p += kl;
  size_t q = p;
  while (q < len_ && buf_[q] >= '0' && buf_[q] <= '9') q++;
  char d[10];
  size_t dn = 0;
  do {
    d[dn++] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  const size_t newLen = len_ - (q - p) + dn;
  if (q == p || newLen > kLineMax) return false;
  memmove(buf_ + p + dn, buf_ + q, len_ - q);
  for (size_t i = 0; i < dn; i++) buf_[p + i] = (uint8_t)d[dn - 1 - i];
  len_ = newLen;
  return true;
}

// ===========================================================================
//  Textes
// ===========================================================================

const char *lampsCode(uint8_t lamps) {
  switch (lamps & F_LAMPS) {
    case F_FRONT: return "avant";
    case F_BACK: return "arriere";
    case F_LAMPS: return "deux";
    default: return "aucune";
  }
}

const char *kindCode(Kind k) {
  switch (k) {
    case Kind::Bright: return "lum";
    case Kind::Temp: return "temp";
    case Kind::Auto: return "a";
    case Kind::LampAck: return "accuse_lampe";
    case Kind::Service: return "service";
    case Kind::Reserved: return "favori";
    case Kind::Invalid: return "invalide";
    case Kind::CrcBad: return "crc_faux";
  }
  return "invalide";
}

const char *slotCode(uint8_t slot) {
  static const char *const kText[EV_SLOT_N] = {"lum", "temp", "a", "brut"};
  return slot < EV_SLOT_N ? kText[slot] : "brut";
}

const char *verdictCode(uint8_t verdict) {
  static const char *const kText[EV_TX_N] = {"ack", "ack_trame", "max_rt", "delai", "fifo"};
  return verdict < EV_TX_N ? kText[verdict] : "fifo";
}

const char *symptomCode(Relaunch c) {
  switch (c) {
    case Relaunch::Verify: return "verif";
    case Relaunch::TxTimeout: return "delais";
    case Relaunch::RxNoise: return "bruit";
    case Relaunch::RxDeaf: return "sourde";
    case Relaunch::None: break;
  }
  return nullptr;
}

const char *relaunchCode(Relaunch c) {
  const char *s = symptomCode(c);
  return s ? s : "l3";
}

void state(Writer &w, const char *k, const State &s) {
  w.obj(k);
  w.boolean("marche", s.power);
  w.str("lampes", lampsCode(s.lamps));
  w.u32("lum", s.bright);
  w.u32("niveau", levelFromRaw(s.bright));
  w.u32("temp", s.temp);
  w.u32("mired", miredFromTemp(s.temp));
  w.end();
}

void fields(Writer &w, const char *k, uint8_t mask) {
  w.arr(k);
  if (mask & FLD_FLAGS) w.str(nullptr, "marche");
  if (mask & FLD_BRIGHT) w.str(nullptr, "lum");
  if (mask & FLD_TEMP) w.str(nullptr, "temp");
  w.end();
}

// ===========================================================================
//  Messages
// ===========================================================================

void rx(Writer &w, uint32_t n, uint32_t ms, const RxEvent &e, uint32_t skipped) {
  const AirFrame &f = e.f;
  w.begin("rx", n, ms);
  w.str("source", e.listen ? "ecoute" : "accuse");
  if (e.listen) w.hex("brut", e.raw, 8);
  else w.null("brut");
  w.u32("len", f.len);
  if (e.listen) {
    w.u32("pid", f.pid);
    w.u32("no_ack", f.noAck);
  } else {
    w.null("pid");
    w.null("no_ack");
  }
  // Au-dela de 4 octets, decodeAir ne lit ni charge ni CRC : charge vide.
  w.hex("charge", f.pay, f.len <= 4 ? f.len : 0);
  if (e.listen) {
    const uint8_t crc[2] = {(uint8_t)(f.crc >> 8), (uint8_t)f.crc};
    w.hex("crc", crc, 2);
    w.boolean("crc_ok", f.crcOk);
  } else {
    w.null("crc");
    w.boolean("crc_ok", true);  // verifie par la puce
  }
  w.str("type", kindCode(e.kind));
  const Payload p{f.pay[0], f.pay[1]};
  if (e.kind == Kind::Bright || e.kind == Kind::Temp) {
    w.obj("sens");
    w.boolean("marche", (p.flags & F_POWER) != 0);
    w.str("lampes", lampsCode(p.flags));
    w.u32(e.kind == Kind::Bright ? "lum" : "temp", p.value);  // valeur de l'air, non bornee
    w.end();
  } else if (e.kind == Kind::Auto) {
    w.obj("sens");
    w.u32("numero", p.value);
    w.boolean("copie", e.copy);
    w.end();
  } else {
    w.null("sens");
  }
  if (skipped) w.u32("sautes", skipped);
}

void tx(Writer &w, uint32_t n, uint32_t ms, const TxEvent &e, uint32_t skipped) {
  w.begin("tx", n, ms);
  w.u32("num", e.num);
  w.str("tranche", slotCode(e.slot));
  const uint8_t pay[2] = {e.pay.flags, e.pay.value};
  w.hex("charge", pay, 2);
  w.u32("essai", e.attempt);
  w.u32("paquets", e.repeats);
  w.u32("accuses", e.acks);
  w.str("verdict", verdictCode(e.verdict));
  w.u32("us", e.us);
  w.hex("rt2", &e.rt2, 1);
  w.hex("irq1", &e.irq1, 1);
  w.hex("status", &e.status, 1);
  if (skipped) w.u32("sautes", skipped);
}

void relaunch(Writer &w, uint32_t n, uint32_t ms, const RelaunchEvent &e) {
  w.begin("relance", n, ms);
  w.str("cause", relaunchCode(e.cause));
  if (e.cause == Relaunch::None) w.null("rang");
  else w.u32("rang", e.rank);
  w.obj("detail");
  switch (e.cause) {
    case Relaunch::TxTimeout: w.u32("suite", e.timeoutRun); break;
    case Relaunch::RxNoise:
      w.u32("trames", e.flood.frames);
      w.u32("crc_faux", e.flood.bad);
      w.u32("ms", e.flood.ms);
      break;
    case Relaunch::RxDeaf:
      w.u32("hors_rx", e.deaf.rearms);
      w.u32("ms", e.deaf.ms);
      break;
    case Relaunch::Verify: w.u32("verif_ratees", e.verifyFails); break;
    case Relaunch::None: break;
  }
  w.end();
  w.boolean("ok", e.ok);
  if (e.crystal < 0) w.null("quartz");
  else w.boolean("quartz", e.crystal != 0);
  if (e.calib < 0) w.null("calib");
  else w.boolean("calib", e.calib != 0);
  w.u32("duree_ms", e.durMs);
  w.u32("total", e.total);
  w.boolean("panne", e.failed);
}

void module(Writer &w, uint32_t n, uint32_t ms, const ModuleEvent &e) {
  static const char *const kState[] = {"panne", "retabli", "perdu", "retrouve", "config_rejetee", "config_verifiee"};
  w.begin("module", n, ms);
  w.str("etat", kState[(uint8_t)e.state < 6 ? (uint8_t)e.state : 0]);
  if (e.state == ModuleState::Fault) {
    w.u32("sans_guerison", e.unrecovered);
    w.str("symptome", symptomCode(e.symptom));
    w.u32("essai_s", e.retryS);
  } else if (e.state == ModuleState::ConfigRejected) {
    if (e.regsKnown) {
      w.hex("rfch", &e.rfch, 1);
      w.hex("dm1", &e.dm1, 1);
      w.hex("rt1", &e.rt1, 1);
    } else {
      w.null("rfch");
      w.null("dm1");
      w.null("rt1");
    }
  }
}

void heartbeat(Writer &w, uint32_t n, uint32_t ms, uint32_t boot, uint32_t upS, uint32_t lost) {
  w.begin("hb", n, ms);
  w.hexU32("boot", boot, 8);
  w.u32("up_s", upS);
  w.u32("json_perdus", lost);
}

void sessionEnd(Writer &w, uint32_t n, uint32_t ms, const char *cause) {
  w.begin("fin", n, ms);
  w.str("cause", cause);
}

void led(Writer &w, uint32_t n, uint32_t ms, const char *motif, const char *before, bool test) {
  w.begin("led", n, ms);
  w.str("motif", motif);
  w.str("avant", before);
  w.boolean("test", test);
}

void logLine(Writer &w, uint32_t n, uint32_t ms, const char *src, const char *niv, const char *txt,
             uint32_t skipped) {
  w.begin("log", n, ms);
  w.str("src", src);
  w.str("niv", niv);
  w.str("txt", txt, kLogTextMax);
  if (skipped) w.u32("sautes", skipped);
}

void reply(Writer &w, uint32_t n, uint32_t ms, const Reply &r) {
  w.begin("reponse", n, ms);
  w.u32("id", r.id);
  w.str("etape", r.fin ? "fin" : "debut");
  w.str("cmd", r.cmd ? r.cmd : "", kCmdTextMax);
  w.boolean("ok", r.ok);
  w.str("code", r.code);
  if (r.msg) w.str("msg", r.msg, kMsgMax);
  if (r.fin) w.u32("duree_ms", r.durMs);
  if (r.suite != Reply::SuiteNone) w.str("suite", r.suite == Reply::SuiteDelivery ? "livraison" : "aucune");
  if (r.hasTarget) {
    state(w, "consigne", r.target);
    fields(w, "a_livrer", r.dirty);
    w.u32("version", r.version);
  }
  if (r.hasLease) {
    w.u32("bail_s", r.leaseS);
    w.u32("up_s", r.upS);
  }
  if (r.key) w.str("cle", r.key, 64);
  if (r.hasKid) w.str("empreinte", r.kid, 8);
}

void ReplyCache::clear() {
  // Sur place : un objet temporaire coute pres d'un Ko de pile a la tache loop.
  for (Entry &e : e_) e.used = false;
  next_ = 0;
}

void ReplyCache::put(const Reply &r) {
  if (!r.fin || !r.id) return;
  // Une seule entree par id : la plus recente (un id repete ne s'execute pas,
  // mais une reponse differee peut suivre une reponse immediate du meme id).
  for (Entry &e : e_)
    if (e.used && e.r.id == r.id) e.used = false;
  Entry &e = e_[next_];
  next_ = (uint8_t)((next_ + 1) % kN);
  e.used = true;
  e.r = r;
  copyCmd(e.cmd, r.cmd ? r.cmd : "");
  e.r.cmd = e.cmd;
  e.r.msg = nullptr;
  e.r.key = nullptr;
  e.r.hasKid = false;
  e.r.kid = nullptr;
}

const Reply *ReplyCache::find(uint32_t id) const {
  if (!id) return nullptr;
  for (const Entry &e : e_)
    if (e.used && e.r.id == id) return &e.r;
  return nullptr;
}

void delivery(Writer &w, uint32_t n, uint32_t ms, const Delivery &d) {
  static const char *const kIssue[] = {"livree", "abandon", "annulee"};
  w.begin("livraison", n, ms);
  w.str("issue", kIssue[(uint8_t)d.issue < 3 ? (uint8_t)d.issue : 2]);
  if (d.issue == Issue::GaveUp && d.cause != GiveUpCause::None)
    w.str("cause", d.cause == GiveUpCause::Unreachable ? "injoignable" : "module");
  // Livree seulement ; absente pour abandon et annulee (exemples 12.2 et 12.4).
  if (d.issue == Issue::Delivered) {
    if (d.last >= 0 && d.last < EV_SLOT_N) w.str("derniere", slotCode((uint8_t)d.last));
    else w.null("derniere");
  }
  w.u32("version", d.version);
  state(w, "consigne", d.target);
  state(w, "cru", d.believed);
  fields(w, "a_livrer", d.dirty);
  w.arr("ids");
  for (uint8_t i = 0; i < d.nIds && i < kIdsMax; i++) w.u32(nullptr, d.ids[i]);
  w.end();
  w.u32("ids_perdus", d.idsLost);
  if (d.hasWait) w.u32("attente_ms", d.waitMs);
  else w.null("attente_ms");
  w.u32("livrees", d.delivered);
  w.u32("abandons", d.giveUps);
}

// ===========================================================================
//  Lignes de l'hote
// ===========================================================================

// Mot i (0, 1, ...) de s, separe par des espaces ; nullptr s'il manque.
static const char *word(const char *s, uint8_t i, size_t *len) {
  *len = 0;
  for (;;) {
    while (*s == ' ') s++;
    if (!*s) return nullptr;
    const char *w = s;
    while (*s && *s != ' ') s++;
    if (!i--) {
      *len = (size_t)(s - w);
      return w;
    }
  }
}

static bool wordIs(const char *w, size_t n, const char *k) { return w && strlen(k) == n && !strncmp(w, k, n); }

// Entier decimal, lu comme le lit l'aiguillage (strtoul : zeros de tete
// compris, au-dela de 4294967295 sature) ; false si le mot n'en est pas un
// (l'aiguillage dira usage). Sans cela, 'bail 0000000000' passerait la liste
// blanche puis vaudrait 0.
static bool wordNum(const char *w, size_t n, uint32_t *v) {
  if (!w || !n) return false;
  uint64_t x = 0;
  for (size_t i = 0; i < n; i++) {
    if (w[i] < '0' || w[i] > '9') return false;
    x = x * 10 + (uint64_t)(w[i] - '0');
    if (x > 0xFFFFFFFFull) x = 0xFFFFFFFFull + 1;  // sature, sans debordement
  }
  *v = x > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)x;
  return true;
}

void maskCmd(char *shown) {
  size_t n0, n1, n2;
  const char *w0 = word(shown, 0, &n0), *w1 = word(shown, 1, &n1), *w2 = word(shown, 2, &n2);
  if (wordIs(w0, n0, "json") && wordIs(w1, n1, "cle") && wordIs(w2, n2, "nouvelle")) strcpy(shown, "json cle nouvelle");
}

const char *remoteRefusal(const char *cmd) {
  static const char kDenied[] = "interdite a distance (10.5) : USB seulement";
  size_t n0, n1, n2, n3, n4;
  const char *w0 = word(cmd, 0, &n0), *w1 = word(cmd, 1, &n1), *w2 = word(cmd, 2, &n2);
  const char *w3 = word(cmd, 3, &n3), *w4 = word(cmd, 4, &n4);
  uint32_t v = 0;
  if (wordIs(w0, n0, "json")) {
    if (wordIs(w1, n1, "1")) {
      // Jamais 'bail 0' a distance : la carte emettrait sur Thread pour un
      // iPhone parti jusqu'a l'oubli de la session (10 min).
      if (wordIs(w2, n2, "bail") && wordNum(w3, n3, &v) && !w4 && (v < 10 || v > 120))
        return "json 1 : bail de 10 a 120 s a distance";
      return nullptr;
    }
    if (wordIs(w1, n1, "0") || wordIs(w1, n1, "etat") || wordIs(w1, n1, "hello") || wordIs(w1, n1, "ping") ||
        wordIs(w1, n1, "trames") || wordIs(w1, n1, "log"))
      return nullptr;
    if (wordIs(w1, n1, "periode"))
      return wordNum(w2, n2, &v) && v < 2000 ? "json periode : 2000..60000 ms a distance" : nullptr;
    if (wordIs(w1, n1, "compteurs"))
      return wordNum(w2, n2, &v) && v && v < 5000 ? "json compteurs : 0 ou 5000..60000 ms a distance" : nullptr;
    if (wordIs(w1, n1, "reseau"))
      return wordNum(w2, n2, &v) && v && v < 10000 ? "json reseau : 0 ou 10000..60000 ms a distance" : nullptr;
    return kDenied;  // 'json' seul (texte humain), 'json cle ...'
  }
  if (wordIs(w0, n0, "lampe")) {
    // Les commandes lampe asynchrones (cli_lampe.cpp : lampeIsAsync).
    static const char *const kAsync[] = {"on", "off", "avant", "arriere", "mode", "lum",
                                         "niveau", "temp", "mired", "auto", "sync"};
    for (const char *k : kAsync)
      if (wordIs(w1, n1, k)) return nullptr;
    return kDenied;
  }
  if (wordIs(w0, n0, "led") && (wordIs(w1, n1, "test") || wordIs(w1, n1, "stop")) && !w2) return nullptr;
  return kDenied;
}

bool parseIdPrefix(char *line, uint32_t *id, char **rest) {
  *rest = line;
  char *p = line;
  while (*p == ' ') p++;
  if (strncmp(p, "id=", 3)) return false;
  p += 3;
  uint32_t v = 0;
  uint8_t digits = 0;
  while (*p >= '0' && *p <= '9') {
    if (++digits > 9) return false;
    v = v * 10 + (uint32_t)(*p++ - '0');
  }
  if (!digits || v < 1 || v > kIdMax || (*p && *p != ' ')) return false;
  while (*p == ' ') p++;
  *id = v;
  *rest = p;
  return true;
}

void copyCmd(char out[kCmdTextMax + 1], const char *cmd) {
  size_t i = 0;
  for (; cmd && cmd[i] && i < kCmdTextMax; i++) out[i] = cmd[i];
  out[i] = 0;
}

LineAssembler::Ev LineAssembler::feed(uint8_t c, bool machine) {
  if (c == '\r') return Ev::None;  // CRLF de l'hote : le CR est ignore
  if (c == '\n') {
    buf_[len_] = 0;
    return Ev::Line;
  }
  if (c == kCtrlU) {
    cleared_ = len_;
    len_ = 0;
    tooLong_ = false;
    return Ev::Clear;
  }
  if (c == 8 || c == 127) {  // retour arriere
    if (!len_) return Ev::None;
    len_--;
    return Ev::Erase;
  }
  if (machine && (c < 0x20 || c > 0x7E)) return Ev::None;
  if (len_ >= kCmdMax) {
    tooLong_ = true;
    return Ev::None;
  }
  buf_[len_++] = (char)c;
  return Ev::Echo;
}

char *LineAssembler::text() {
  buf_[len_] = 0;
  return buf_;
}

// ===========================================================================
//  Debit
// ===========================================================================

bool RateCap::available(uint32_t now) {
  if (!started_ || now - winAt_ >= 1000) {
    started_ = true;
    winAt_ = now;
    count_ = 0;
  }
  return count_ < limit_;
}

bool Cadence::allow(uint32_t now) {
  // at_[idx_] : la plus ancienne des kLines dernieres lignes acceptees.
  if (n_ >= kLines && now - at_[idx_] < kWindowMs) return false;
  at_[idx_] = now;
  idx_ = (uint8_t)((idx_ + 1) % kLines);
  if (n_ < kLines) n_++;
  return true;
}

// ===========================================================================
//  File
// ===========================================================================

bool Queue::has(Item item) const {
  for (uint8_t i = 0; i < n_; i++)
    if (q_[(head_ + i) % kN].item == item) return true;
  return false;
}

bool Queue::push(Item item, uint32_t now, bool session, uint8_t arg) {
  if (item != Item::Reply) {
    for (uint8_t i = 0; i < n_; i++) {
      Queued &q = q_[(head_ + i) % kN];
      if (q.item != item) continue;
      // Demande explicite : survit a la fin du mode machine, et son retard
      // part d'elle (la ligne est formatee a l'envoi : rien n'est perime).
      if (!session) {
        q.session = false;
        q.at = now;
      }
      return true;
    }
  }
  if (n_ >= kN) return false;
  q_[(head_ + n_) % kN] = Queued{item, arg, session, now};
  n_++;
  return true;
}

void Queue::pop() {
  if (!n_) return;
  head_ = (uint8_t)((head_ + 1) % kN);
  n_--;
}

uint8_t Queue::dropLate(uint32_t now, uint32_t lateMs) {
  uint8_t dropped = 0;
  while (n_) {
    const Queued &q = q_[head_];
    if (q.item == Item::Reply || now - q.at <= lateMs) break;
    pop();
    dropped++;
  }
  return dropped;
}

bool Queue::frontReady(int room) const {
  if (!n_) return false;
  const size_t need = q_[head_].item == Item::Reply ? kLineMax : 2 * kLineMax;
  return room >= 0 && (size_t)room >= need;
}

uint8_t Queue::dropSession() {
  Queued keep[kN];
  uint8_t k = 0, dropped = 0;
  for (uint8_t i = 0; i < n_; i++) {
    const Queued &q = q_[(head_ + i) % kN];
    if (q.session) dropped++;
    else keep[k++] = q;
  }
  for (uint8_t i = 0; i < k; i++) q_[i] = keep[i];
  head_ = 0;
  n_ = k;
  return dropped;
}

// ===========================================================================
//  Bail
// ===========================================================================

bool leaseExpired(uint32_t now, uint32_t lastRx, uint32_t lastCmd, uint16_t leaseS) {
  if (!leaseS) return false;
  const uint32_t last = (int32_t)(lastRx - lastCmd) > 0 ? lastRx : lastCmd;
  return now - last >= (uint32_t)leaseS * 1000u;
}

// ===========================================================================
//  Observateur de livraison
// ===========================================================================

void DeliveryWatch::reset(uint32_t delivered, uint32_t giveUps) {
  *this = DeliveryWatch();
  seenDelivered_ = delivered;
  seenGiveUps_ = giveUps;
}

void DeliveryWatch::pendingId(uint32_t id, uint32_t pendingSince, uint8_t origin) {
  if (origin >= kOrigins) origin = kUsb;
  if (nIds_ == kIdsMax) {
    lostBy_[org_[0]]++;
    memmove(ids_, ids_ + 1, sizeof(ids_[0]) * (kIdsMax - 1));
    memmove(org_, org_ + 1, sizeof(org_[0]) * (kIdsMax - 1));
    nIds_--;
    idsLost_++;
  }
  ids_[nIds_] = id;
  org_[nIds_] = origin;
  nIds_++;
  wasBusy_ = sawTarget_ = true;
  if (pendingSince) since_ = pendingSince;
}

void DeliveryWatch::dropOrigin(uint8_t origin) {
  uint8_t k = 0;
  for (uint8_t i = 0; i < nIds_; i++) {
    if (org_[i] == origin) continue;
    ids_[k] = ids_[i];
    org_[k] = org_[i];
    k++;
  }
  nIds_ = k;
  if (origin < kOrigins) lostBy_[origin] = 0;
}

bool DeliveryWatch::poll(const LampSample &s, uint32_t now, bool machine, Delivery *d) {
  bool emit = false;
  const bool gaveUp = s.giveUps != seenGiveUps_, delivered = s.delivered != seenDelivered_;
  if (gaveUp || delivered || (!s.busy && wasBusy_)) {
    if ((gaveUp || delivered || sawTarget_) && (machine || nIds_)) {
      d->issue = gaveUp ? Issue::GaveUp : delivered ? Issue::Delivered : Issue::Cancelled;
      d->ids = ids_;
      d->nIds = nIds_;
      d->idsLost = idsLost_;
      d->origins = org_;
      memcpy(lostOut_, lostBy_, sizeof(lostOut_));
      memset(lostBy_, 0, sizeof(lostBy_));
      d->idsLostBy = lostOut_;
      d->hasWait = wasBusy_ && since_;  // periode pas vue : commande bloquante
      d->waitMs = d->hasWait ? now - since_ : 0;
      d->delivered = s.delivered;
      d->giveUps = s.giveUps;
      nIds_ = 0;  // ids_ et org_ restent lisibles jusqu'au prochain pendingId, lostOut_ jusqu'au prochain poll
      idsLost_ = 0;
      emit = true;
    }
    seenDelivered_ = s.delivered;
    seenGiveUps_ = s.giveUps;
    wasBusy_ = sawTarget_ = false;
    since_ = 0;
  }
  if (s.busy) {
    wasBusy_ = true;
    if (s.targetBusy) sawTarget_ = true;
    // endBurst() et giveUp() le remettent a 0 avant le front : releve ici.
    if (s.pendingSince) since_ = s.pendingSince;
  }
  return emit;
}

}  // namespace jsonp
