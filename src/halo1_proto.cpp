#include "halo1_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// L'auto-test couvre aussi les correspondances Matter : les deux fichiers sont
// toujours compiles ensemble.
#include "halo1_map.h"

namespace halo1 {

// ---------------------------------------------------------------------------
//  Adresse
// ---------------------------------------------------------------------------

void airOrder(const uint8_t addrReg[4], uint8_t air[4]) {
  for (uint8_t i = 0; i < 4; i++) air[i] = addrReg[3 - i];
}

bool addressAllowed(const uint8_t addrReg[4]) {
  static const uint8_t kZero[4] = {0x00, 0x00, 0x00, 0x00};
  static const uint8_t kOnes[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  if (!memcmp(addrReg, kZero, 4) || !memcmp(addrReg, kOnes, 4)) return false;
  // Les adresses d'appairage sont refusees dans les deux ordres : une saisie a
  // l'envers ne doit pas les laisser passer.
  static const uint8_t *const kPairing[2] = {kPairingH1Reg, kPairingH2Reg};
  for (const uint8_t *p : kPairing) {
    uint8_t rev[4];
    airOrder(p, rev);
    if (!memcmp(addrReg, p, 4) || !memcmp(addrReg, rev, 4)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
//  Charge
// ---------------------------------------------------------------------------

uint8_t clampBright(uint8_t v) { return v < kBrightMin ? kBrightMin : v > kBrightMax ? kBrightMax : v; }
uint8_t clampTemp(uint8_t v) { return v > kTempMax ? kTempMax : v; }

// Jamais de trame sans lampe : la lampe n'en a jamais recu, on ne sait pas ce
// qu'elle en ferait.
static uint8_t lampBits(uint8_t lamps) {
  lamps &= F_LAMPS;
  return lamps ? lamps : F_FRONT;
}

static uint8_t flagsFor(bool on, uint8_t lamps, uint8_t selector) {
  return (uint8_t)((on ? F_POWER : 0) | lampBits(lamps) | selector);
}

Payload makeTemp(bool on, uint8_t lamps, uint8_t temp) { return {flagsFor(on, lamps, F_TEMP), clampTemp(temp)}; }
Payload makeBright(bool on, uint8_t lamps, uint8_t bright) {
  return {flagsFor(on, lamps, F_BRIGHT), clampBright(bright)};
}
Payload makeAuto(bool on, uint8_t lamps, uint8_t counter) {
  return {flagsFor(on, lamps, F_AUTO), counter ? counter : (uint8_t)1};
}

Kind kindOf(Payload p) {
  const uint8_t f = p.flags;
  if ((f & 0xF8) == 0xF8) return Kind::Service;  // FF/FE/FD reveil, FA annonce
  if (f & F_RSV) return Kind::Reserved;           // favori (91, 89)
  const uint8_t sel = f & F_SELECT;
  if (!(f & F_LAMPS) || (sel != F_TEMP && sel != F_BRIGHT && sel != F_AUTO)) return Kind::Invalid;
  return sel == F_TEMP ? Kind::Temp : sel == F_BRIGHT ? Kind::Bright : Kind::Auto;
}

// ---------------------------------------------------------------------------
//  Trame sur l'air
//
//  64 bits lus apres l'adresse (ecoute passive, RXPW0 = 8), decodes en logiciel :
//    PCF 9 bits (longueur 6, PID 2, NO_ACK 1) | charge | CRC-16 | bourrage
//  CRC-16/CCITT 0x1021, init 0xFFFF, sur l'adresse SUR L'AIR puis PCF + charge.
// ---------------------------------------------------------------------------

static inline uint8_t bitAt(const uint8_t *b, unsigned i) { return (b[i >> 3] >> (7 - (i & 7))) & 1; }

static uint16_t readBits(const uint8_t *b, unsigned pos, unsigned n) {
  uint16_t v = 0;
  for (unsigned i = 0; i < n; i++) v = (uint16_t)((v << 1) | bitAt(b, pos + i));
  return v;
}

static void writeBits(uint8_t *b, unsigned pos, uint16_t v, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    const uint8_t mask = (uint8_t)(0x80 >> ((pos + i) & 7));
    if ((v >> (n - 1 - i)) & 1)
      b[(pos + i) >> 3] |= mask;
    else
      b[(pos + i) >> 3] &= (uint8_t)~mask;
  }
}

static uint16_t crcFeed(uint16_t crc, uint8_t bit) {
  crc ^= (uint16_t)(bit << 15);
  return (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
}

// CRC de l'adresse sur l'air puis des 'nbits' premiers bits de 'b'.
static uint16_t frameCrc(const uint8_t air[4], const uint8_t *b, unsigned nbits) {
  uint16_t crc = 0xFFFF;
  for (unsigned i = 0; i < 32; i++) crc = crcFeed(crc, bitAt(air, i));
  for (unsigned i = 0; i < nbits; i++) crc = crcFeed(crc, bitAt(b, i));
  return crc;
}

AirFrame decodeAir(const uint8_t raw[8], const uint8_t air[4]) {
  AirFrame f{};
  f.len = (uint8_t)readBits(raw, 0, 6);
  f.pid = (uint8_t)readBits(raw, 6, 2);
  f.noAck = bitAt(raw, 8);
  if (f.len > 4) return f;  // 64 bits lus : au plus 4 octets de charge + CRC
  const unsigned n = 9 + 8u * f.len;
  for (uint8_t q = 0; q < f.len; q++) f.pay[q] = (uint8_t)readBits(raw, 9 + 8u * q, 8);
  f.crc = readBits(raw, n, 16);
  f.crcOk = frameCrc(air, raw, n) == f.crc;
  return f;
}

void encodeAir(const uint8_t air[4], uint8_t pid, bool noAck, const uint8_t *pay, uint8_t len,
               uint8_t raw[8]) {
  if (!pay) len = 0;
  if (len > 4) len = 4;  // meme limite que decodeAir
  memset(raw, 0, 8);
  writeBits(raw, 0, len, 6);
  writeBits(raw, 6, pid & 3, 2);
  writeBits(raw, 8, noAck ? 1 : 0, 1);
  for (uint8_t q = 0; q < len; q++) writeBits(raw, 9 + 8u * q, pay[q], 8);
  const unsigned n = 9 + 8u * len;
  writeBits(raw, n, frameCrc(air, raw, n), 16);
}

Kind classify(const AirFrame &f) {
  if (!f.crcOk) return Kind::CrcBad;
  if (f.len == 0) return Kind::LampAck;  // accuse de la lampe, vide
  if (f.noAck) return Kind::Service;     // FA xx, seule trame sans demande d'accuse
  if (f.len != 2) return Kind::Invalid;
  return kindOf({f.pay[0], f.pay[1]});
}

// ---------------------------------------------------------------------------
//  Etat
// ---------------------------------------------------------------------------

uint8_t applyState(State &s, Payload p) {
  const Kind k = kindOf(p);
  if (k != Kind::Temp && k != Kind::Bright) return 0;
  uint8_t changed = 0;
  const bool power = (p.flags & F_POWER) != 0;
  const uint8_t lamps = p.flags & F_LAMPS;  // jamais 0 : kindOf l'a verifie
  if (power != s.power || lamps != s.lamps) {
    s.power = power;
    s.lamps = lamps;
    changed |= FLD_FLAGS;
  }
  if (k == Kind::Bright) {
    const uint8_t v = clampBright(p.value);
    if (v != s.bright) {
      s.bright = v;
      changed |= FLD_BRIGHT;
    }
  } else {
    const uint8_t v = clampTemp(p.value);
    if (v != s.temp) {
      s.temp = v;
      changed |= FLD_TEMP;
    }
  }
  return changed;
}

uint8_t coveredBy(Payload sent, const State &target) {
  const Kind k = kindOf(sent);
  if (k != Kind::Temp && k != Kind::Bright) return 0;  // A et le reste : aucun champ
  uint8_t f = 0;
  const bool on = (sent.flags & F_POWER) != 0;
  // Comparaison avec la consigne bornee comme par les constructeurs : sinon une
  // consigne hors invariants (lampes 0, luminosite < 4C...) ne serait jamais
  // couverte par la trame que plan() en tire, et partirait sans fin.
  if (on == target.power && (sent.flags & F_LAMPS) == lampBits(target.lamps)) f |= FLD_FLAGS;
  // Une trame d'extinction ne livre que FLAGS : la valeur differee reste due.
  if (on && k == Kind::Bright && clampBright(sent.value) == clampBright(target.bright)) f |= FLD_BRIGHT;
  if (on && k == Kind::Temp && clampTemp(sent.value) == clampTemp(target.temp)) f |= FLD_TEMP;
  return f;
}

uint8_t dueFields(const State &target, uint8_t fields) {
  // Decision A4 (a) : allumee, FLAGS rend aussi due la luminosite affichee. Sans
  // cela, une trame de temperature livree avant elle couvrirait FLAGS et la
  // luminosite ne partirait plus : la lampe s'allumerait a sa propre luminosite.
  // Repli A4 (b) : renvoyer fields tel quel.
  if ((fields & FLD_FLAGS) && target.power) fields |= FLD_BRIGHT;
  return (uint8_t)(fields & FLD_ALL);
}

// ---------------------------------------------------------------------------
//  Planification
// ---------------------------------------------------------------------------

Plan plan(const State &target, const State &believed, uint8_t dirty) {
  Plan p;
  if (!target.power) {
    // Extinction : temperature CRUE, pour qu'un reglage differe ne fuie pas
    // dans la trame. BRIGHT et TEMP restent a livrer jusqu'a l'allumage.
    if (dirty & FLD_FLAGS) {
      p.temp = true;
      p.pt = makeTemp(false, target.lamps, believed.temp);
    }
    return p;
  }
  // Decision A4 (a) : allumage et changement de lampes portes par la trame de
  // luminosite affichee. Repli A4 (b) : FLAGS seul -> makeTemp(true, lamps, believed.temp),
  // et dueFields n'ajoute plus BRIGHT.
  if (dirty & (FLD_BRIGHT | FLD_FLAGS)) {
    p.bright = true;
    p.pb = makeBright(true, target.lamps, target.bright);
  }
  if (dirty & FLD_TEMP) {
    p.temp = true;
    p.pt = makeTemp(true, target.lamps, target.temp);
  }
  return p;
}

uint8_t nextAuto(uint8_t last) { return (last == 0 || last == 255) ? 1 : (uint8_t)(last + 1); }

uint8_t crc8(const uint8_t *p, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *p++;
    for (uint8_t k = 0; k < 8; k++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

// ---------------------------------------------------------------------------
//  Auto-test embarque ('lampe autotest') : un sous-ensemble des tests hote
//  (tools/host_tests/test_halo1.cpp). Aucune radio. La table gamma en place est
//  seulement relue (construite a gamma 2,0 si mapInit n'a pas encore ete appele).
// ---------------------------------------------------------------------------

namespace {

struct Checker {
  char *msg;
  size_t n;
  int fails;
#if defined(__GNUC__)
  __attribute__((format(printf, 3, 4)))
#endif
  void expect(bool ok, const char *fmt, ...) {
    if (ok) return;
    if (fails++ || !msg || !n) return;  // seul le premier echec est decrit
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, n, fmt, ap);
    va_end(ap);
  }
};

// Vecteurs dores (adresse sur l'air 63 FD F0 4F), recalcules et, pour C4 BC,
// valides sur l'air.
struct Golden {
  uint8_t pid, noAck, len, pay[2];
  uint16_t crc;
  uint8_t raw[8];
};
const Golden kGolden[] = {
    {0, 0, 2, {0xC3, 0x35}, 0xF7A9, {0x08, 0x61, 0x9A, 0xFB, 0xD4, 0x80, 0x00, 0x00}},
    {1, 0, 2, {0xC3, 0x35}, 0x99C9, {0x09, 0x61, 0x9A, 0xCC, 0xE4, 0x80, 0x00, 0x00}},
    {1, 0, 2, {0xC4, 0xBC}, 0x00FF, {0x09, 0x62, 0x5E, 0x00, 0x7F, 0x80, 0x00, 0x00}},
    {0, 0, 2, {0xC4, 0xFE}, 0x0619, {0x08, 0x62, 0x7F, 0x03, 0x0C, 0x80, 0x00, 0x00}},
    {2, 0, 2, {0xC4, 0xFE}, 0xDAD9, {0x0A, 0x62, 0x7F, 0x6D, 0x6C, 0x80, 0x00, 0x00}},
    {0, 0, 2, {0xE1, 0x01}, 0xE1FA, {0x08, 0x70, 0x80, 0xF0, 0xFD, 0x00, 0x00, 0x00}},
    {2, 0, 2, {0xE1, 0x01}, 0x3D3A, {0x0A, 0x70, 0x80, 0x9E, 0x9D, 0x00, 0x00, 0x00}},
    {0, 0, 2, {0x42, 0x35}, 0xDF00, {0x08, 0x21, 0x1A, 0xEF, 0x80, 0x00, 0x00, 0x00}},
    {0, 0, 2, {0xC5, 0xA0}, 0x8E13, {0x08, 0x62, 0xD0, 0x47, 0x09, 0x80, 0x00, 0x00}},
    {1, 1, 0, {0, 0}, 0x5C90, {0x01, 0xAE, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00}},  // accuse reel
    {3, 1, 0, {0, 0}, 0x1C14, {0x03, 0x8E, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

// Table de D.3 : cru, consigne, champs a livrer -> trames attendues.
struct PlanCase {
  State b, t;
  uint8_t dirty;
  bool hb;
  Payload pb;
  bool ht;
  Payload pt;
};
const PlanCase kPlan[] = {
    {{true, F_LAMPS, 0xA5, 0x35}, {false, F_LAMPS, 0xA5, 0x35}, FLD_FLAGS, false, {0, 0}, true, {0x43, 0x35}},
    {{false, F_LAMPS, 0xA5, 0x64}, {true, F_LAMPS, 0xA5, 0x64}, FLD_FLAGS, true, {0xC5, 0xA5}, false, {0, 0}},
    {{true, F_LAMPS, 0xA5, 0x35}, {true, F_FRONT, 0xA5, 0x35}, FLD_FLAGS, true, {0xC4, 0xA5}, false, {0, 0}},
    {{true, F_LAMPS, 0xA5, 0x35}, {true, F_LAMPS, 0xC0, 0x35}, FLD_BRIGHT, true, {0xC5, 0xC0}, false, {0, 0}},
    {{true, F_LAMPS, 0xA5, 0x35},
     {true, F_LAMPS, 0x4C, 0x00},
     FLD_BRIGHT | FLD_TEMP,
     true,
     {0xC5, 0x4C},
     true,
     {0xC3, 0x00}},
    {{false, F_LAMPS, 0xA5, 0x35}, {false, F_LAMPS, 0x60, 0x35}, FLD_BRIGHT, false, {0, 0}, false, {0, 0}},
    {{false, F_LAMPS, 0xA5, 0x35},
     {true, F_LAMPS, 0x60, 0x35},
     FLD_BRIGHT | FLD_FLAGS,
     true,
     {0xC5, 0x60},
     false,
     {0, 0}},
    {{false, F_LAMPS, 0xA5, 0x64}, {false, F_LAMPS, 0xA5, 0x10}, FLD_TEMP, false, {0, 0}, false, {0, 0}},
    {{false, F_LAMPS, 0xA5, 0x64},
     {false, F_LAMPS, 0xA5, 0x10},
     FLD_TEMP | FLD_FLAGS,
     false,
     {0, 0},
     true,
     {0x43, 0x64}},
};

// Table de classify : charge, NO_ACK -> genre.
struct KindCase {
  uint8_t f, v, noAck;
  Kind k;
};
const KindCase kKinds[] = {
    {0xFF, 0x00, 0, Kind::Service},  {0xFE, 0x00, 0, Kind::Service},  {0xFD, 0x00, 0, Kind::Service},
    {0xFA, 0xA8, 1, Kind::Service},  {0x91, 0x00, 0, Kind::Reserved}, {0x89, 0x58, 0, Kind::Reserved},
    {0x00, 0x00, 0, Kind::Invalid},  {0xC6, 0x10, 0, Kind::Invalid},  {0xE1, 0x01, 0, Kind::Auto},
    {0xC3, 0x35, 0, Kind::Temp},     {0xC5, 0xA0, 0, Kind::Bright},
};

}  // namespace

int selfTest(char *msg, size_t n) {
  if (msg && n) msg[0] = 0;
  Checker c{msg, n, 0};
  uint8_t air[4];
  airOrder(kDefaultAddrReg, air);
  c.expect(air[0] == 0x63 && air[1] == 0xFD && air[2] == 0xF0 && air[3] == 0x4F, "ordre air de l'adresse");
  c.expect(addressAllowed(kDefaultAddrReg), "adresse par defaut refusee");
  c.expect(!addressAllowed(kPairingH1Reg) && !addressAllowed(kPairingH2Reg), "appairage accepte");

  // Vecteurs dores : codage, decodage, CRC.
  for (const Golden &g : kGolden) {
    uint8_t raw[8];
    encodeAir(air, g.pid, g.noAck, g.pay, g.len, raw);
    c.expect(!memcmp(raw, g.raw, 8), "codage %02X %02X PID %u", g.pay[0], g.pay[1], g.pid);
    const AirFrame f = decodeAir(g.raw, air);
    c.expect(f.crcOk && f.crc == g.crc && f.len == g.len && f.pid == g.pid && f.noAck == g.noAck &&
                 (g.len == 0 || (f.pay[0] == g.pay[0] && f.pay[1] == g.pay[1])),
             "decodage %02X %02X PID %u", g.pay[0], g.pay[1], g.pid);
  }
  // Un bit bascule dans la trame (PCF, charge ou CRC) : CRC faux.
  for (unsigned i = 0; i < 9 + 16 + 16; i++) {
    uint8_t raw[8];
    memcpy(raw, kGolden[3].raw, 8);
    raw[i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
    c.expect(!decodeAir(raw, air).crcOk, "bit %u bascule non detecte", i);
  }
  // Classification.
  for (const KindCase &k : kKinds) {
    const uint8_t pay[2] = {k.f, k.v};
    uint8_t raw[8];
    encodeAir(air, 0, k.noAck, pay, 2, raw);
    c.expect(classify(decodeAir(raw, air)) == k.k, "classify %02X %02X", k.f, k.v);
  }

  // Constructeurs et bornes.
  c.expect(makeBright(true, 0, 0x20) == Payload{0xC4, 0x4C}, "makeBright sans lampe");
  c.expect(makeTemp(false, 0xFF, 0xFF) == Payload{0x43, 0x64}, "makeTemp bornes");
  c.expect(makeAuto(true, F_FRONT, 0) == Payload{0xE0, 0x01}, "makeAuto numero 0");
  c.expect(makeAuto(true, F_FRONT, 3) == Payload{0xE0, 0x03}, "makeAuto E0 03");

  // Planification (D.3).
  for (size_t i = 0; i < sizeof(kPlan) / sizeof(kPlan[0]); i++) {
    const PlanCase &k = kPlan[i];
    const Plan p = plan(k.t, k.b, k.dirty);
    c.expect(p.bright == k.hb && (!k.hb || p.pb == k.pb) && p.temp == k.ht && (!k.ht || p.pt == k.pt),
             "plan, cas %u", (unsigned)i + 1);
  }

  // Etat : une trame A ne touche a rien ; extinction : FLAGS seul.
  {
    State s;
    s.power = true;
    s.lamps = F_FRONT;
    const State before = s;
    c.expect(applyState(s, {0x60, 0x01}) == 0 && s == before, "trame A appliquee a l'etat");
    c.expect(applyState(s, {0xC5, 0x20}) == (FLD_FLAGS | FLD_BRIGHT) && s.bright == kBrightMin,
             "applyState C5 20");
    State t;
    t.power = false;
    t.temp = 0x35;
    c.expect(coveredBy({0x43, 0x35}, t) == FLD_FLAGS, "coveredBy extinction");
    t.power = true;
    t.bright = 0xA5;
    c.expect(coveredBy({0xC5, 0xA5}, t) == (FLD_FLAGS | FLD_BRIGHT), "coveredBy C5 A5");
    c.expect(coveredBy({0xE1, 0x01}, t) == 0, "coveredBy trame A");
    // Consigne hors invariants : les trames que plan() en tire la couvrent quand meme.
    const State odd{true, 0, 0x20, 0xC8};
    const Plan q = plan(odd, odd, FLD_ALL);
    c.expect((FLD_ALL & ~(coveredBy(q.pb, odd) | coveredBy(q.pt, odd))) == 0, "coveredBy consigne hors bornes");
    // A4 (a) : allumee, FLAGS rend la luminosite due ; eteinte, non.
    c.expect(dueFields(t, FLD_FLAGS | FLD_TEMP) == FLD_ALL && dueFields(State(), FLD_FLAGS) == FLD_FLAGS,
             "dueFields");
  }
  c.expect(nextAuto(0) == 1 && nextAuto(255) == 1 && nextAuto(1) == 2 && nextAuto(254) == 255, "nextAuto");
  static const uint8_t kCheck[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  c.expect(crc8(kCheck, 9) == 0xF4, "crc8");

  // Correspondances Matter : table en place (quel que soit gamma) et mireds.
  c.expect(rawFromLevel(0) == kBrightMin && rawFromLevel(1) == kBrightMin && rawFromLevel(254) == kBrightMax,
           "table gamma : bornes");
  for (unsigned l = 2; l <= 254; l++)
    c.expect(rawFromLevel((uint8_t)l) >= rawFromLevel((uint8_t)(l - 1)), "table gamma non monotone en %u", l);
  for (unsigned r = kBrightMin; r <= kBrightMax; r++) {
    const uint8_t l = levelFromRaw((uint8_t)r);
    c.expect(rawFromLevel(l) >= r && (l == 1 || rawFromLevel((uint8_t)(l - 1)) < r), "levelFromRaw(%02X)", r);
    const uint8_t d = displayLevel(l, (uint8_t)r);
    c.expect(displayLevel(d, (uint8_t)r) == d, "affichage du niveau instable (%02X)", r);
  }
  for (unsigned t = 0; t <= kTempMax; t++)
    c.expect(tempFromMired(miredFromTemp((uint8_t)t)) == t, "aller-retour mired (temp %u)", t);
  c.expect(miredFromTemp(0) == kMiredCold && miredFromTemp(kTempMax) == kMiredWarm, "mireds : bornes");

  // Regles d'intention (E.4).
  {
    State on;
    on.power = true;
    MatterIntents in;
    in.has = IN_POWER | IN_FRONT | IN_BACK;  // tout eteindre, une fenetre
    Resolution r = resolveMatter(on, in, F_LAMPS);
    c.expect(!r.target.power && r.target.lamps == F_LAMPS && (r.fields & FLD_FLAGS), "R1 tout eteindre");
    in = MatterIntents();
    in.has = IN_POWER | IN_FRONT | IN_BACK;  // scene EP1 on, avant on, arriere off
    in.power = in.front = true;
    r = resolveMatter(State(), in, F_LAMPS);
    c.expect(r.target.power && r.target.lamps == F_FRONT, "scene avant seule");
    in = MatterIntents();
    in.has = IN_LEVEL;  // niveau lampe eteinte : differe
    in.level = 254;
    r = resolveMatter(State(), in, F_LAMPS);
    c.expect(!r.target.power && r.fields == FLD_BRIGHT && r.target.bright == kBrightMax &&
                 !plan(r.target, State(), r.fields).bright,
             "niveau lampe eteinte");
    in = MatterIntents();
    in.has = IN_POWER | IN_AUTO;  // A avec EP1 on : garde-fou de groupe
    in.power = true;
    c.expect(!resolveMatter(on, in, F_LAMPS).fireAuto, "A avec EP1 on");
    in.has = IN_AUTO;
    c.expect(resolveMatter(on, in, F_LAMPS).fireAuto && !resolveMatter(State(), in, F_LAMPS).fireAuto,
             "A seul");
  }
  return c.fails;
}

}  // namespace halo1
