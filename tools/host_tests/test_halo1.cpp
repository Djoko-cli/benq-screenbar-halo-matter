// Tests hote du protocole Halo 1 et des correspondances Matter (sans carte).
// Lancer : sh tools/test_halo1.sh
//
// Vecteurs et tables : docs/PLAN-PILOTE-HALO1.md (H/C2, D.3, E.2, E.4) et
// docs/PROTOCOL.md. Adresse sur l'air 63 FD F0 4F.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "halo1_map.h"
#include "halo1_proto.h"

using namespace halo1;

static int gChecks = 0, gFails = 0;

#define CHECK(cond, ...)                                      \
  do {                                                        \
    gChecks++;                                                \
    if (!(cond)) {                                            \
      if (++gFails <= 40) {                                   \
        printf("ECHEC %s:%d : ", __FILE__, __LINE__);         \
        printf(__VA_ARGS__);                                  \
        printf("\n");                                         \
      }                                                       \
    }                                                         \
  } while (0)

static uint8_t gAir[4];

static const char *kindName(Kind k) {
  switch (k) {
    case Kind::Temp: return "Temp";
    case Kind::Bright: return "Bright";
    case Kind::Auto: return "Auto";
    case Kind::LampAck: return "LampAck";
    case Kind::Service: return "Service";
    case Kind::Reserved: return "Reserved";
    case Kind::Invalid: return "Invalid";
    case Kind::CrcBad: return "CrcBad";
  }
  return "?";
}

static State mk(bool power, uint8_t lamps, uint8_t bright, uint8_t temp) {
  State s;
  s.power = power;
  s.lamps = lamps;
  s.bright = bright;
  s.temp = temp;
  return s;
}

static Payload P(uint8_t f, uint8_t v) { return Payload{f, v}; }  // sans virgule nue dans CHECK

static AirFrame frame(uint8_t f, uint8_t v, uint8_t noAck, uint8_t len = 2, uint8_t pid = 0) {
  const uint8_t pay[4] = {f, v, 0x5A, 0xA5};
  uint8_t raw[8];
  encodeAir(gAir, pid, noAck, pay, len, raw);
  return decodeAir(raw, gAir);
}

// ---------------------------------------------------------------------------
//  Trame sur l'air
// ---------------------------------------------------------------------------

struct Golden {
  uint8_t pid, noAck, len, pay[2];
  uint16_t crc;
  uint8_t raw[8];
  Kind kind;
};
static const Golden kGolden[] = {
    {0, 0, 2, {0xC3, 0x35}, 0xF7A9, {0x08, 0x61, 0x9A, 0xFB, 0xD4, 0x80, 0x00, 0x00}, Kind::Temp},
    {1, 0, 2, {0xC3, 0x35}, 0x99C9, {0x09, 0x61, 0x9A, 0xCC, 0xE4, 0x80, 0x00, 0x00}, Kind::Temp},
    {1, 0, 2, {0xC4, 0xBC}, 0x00FF, {0x09, 0x62, 0x5E, 0x00, 0x7F, 0x80, 0x00, 0x00}, Kind::Bright},
    {0, 0, 2, {0xC4, 0xFE}, 0x0619, {0x08, 0x62, 0x7F, 0x03, 0x0C, 0x80, 0x00, 0x00}, Kind::Bright},
    {2, 0, 2, {0xC4, 0xFE}, 0xDAD9, {0x0A, 0x62, 0x7F, 0x6D, 0x6C, 0x80, 0x00, 0x00}, Kind::Bright},
    {0, 0, 2, {0xE1, 0x01}, 0xE1FA, {0x08, 0x70, 0x80, 0xF0, 0xFD, 0x00, 0x00, 0x00}, Kind::Auto},
    {2, 0, 2, {0xE1, 0x01}, 0x3D3A, {0x0A, 0x70, 0x80, 0x9E, 0x9D, 0x00, 0x00, 0x00}, Kind::Auto},
    {0, 0, 2, {0x42, 0x35}, 0xDF00, {0x08, 0x21, 0x1A, 0xEF, 0x80, 0x00, 0x00, 0x00}, Kind::Temp},
    {0, 0, 2, {0xC5, 0xA0}, 0x8E13, {0x08, 0x62, 0xD0, 0x47, 0x09, 0x80, 0x00, 0x00}, Kind::Bright},
    {1, 1, 0, {0, 0}, 0x5C90, {0x01, 0xAE, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00}, Kind::LampAck},  // reel
    {3, 1, 0, {0, 0}, 0x1C14, {0x03, 0x8E, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}, Kind::LampAck},
};

static void testAddress() {
  airOrder(kDefaultAddrReg, gAir);
  CHECK(gAir[0] == 0x63 && gAir[1] == 0xFD && gAir[2] == 0xF0 && gAir[3] == 0x4F, "ordre air");
  CHECK(addressAllowed(kDefaultAddrReg), "adresse par defaut refusee");
  const uint8_t zero[4] = {0, 0, 0, 0}, ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t h1Air[4] = {0x59, 0x01, 0x00, 0xB0}, h2Air[4] = {0xE2, 0x08, 0x00, 0xB0};
  const uint8_t one[4] = {0x00, 0x00, 0x00, 0x01}, dflAir[4] = {0x63, 0xFD, 0xF0, 0x4F};
  CHECK(!addressAllowed(zero), "0 accepte");
  CHECK(!addressAllowed(ones), "FFFFFFFF accepte");
  CHECK(!addressAllowed(kPairingH1Reg) && !addressAllowed(h1Air), "appairage Halo 1 accepte");
  CHECK(!addressAllowed(kPairingH2Reg) && !addressAllowed(h2Air), "appairage Halo 2 accepte");
  CHECK(addressAllowed(one) && addressAllowed(dflAir), "adresse ordinaire refusee");
}

static void testGolden() {
  for (const Golden &g : kGolden) {
    uint8_t raw[8];
    encodeAir(gAir, g.pid, g.noAck, g.pay, g.len, raw);
    CHECK(!memcmp(raw, g.raw, 8), "codage %02X %02X PID %u : %02X %02X %02X %02X %02X %02X", g.pay[0],
          g.pay[1], g.pid, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    const AirFrame f = decodeAir(g.raw, gAir);
    CHECK(f.crcOk, "CRC %04X refuse", g.crc);
    CHECK(f.crc == g.crc, "CRC lu %04X au lieu de %04X", f.crc, g.crc);
    CHECK(f.len == g.len && f.pid == g.pid && f.noAck == g.noAck, "PCF %u/%u/%u", f.len, f.pid, f.noAck);
    CHECK(g.len == 0 || (f.pay[0] == g.pay[0] && f.pay[1] == g.pay[1]), "charge %02X %02X", f.pay[0],
          f.pay[1]);
    CHECK(classify(f) == g.kind, "classify %02X %02X : %s", g.pay[0], g.pay[1], kindName(classify(f)));

    // Un bit bascule n'importe ou dans PCF + charge + CRC : CRC faux.
    const unsigned nbits = 9 + 8u * g.len + 16;
    for (unsigned i = 0; i < nbits; i++) {
      uint8_t bad[8];
      memcpy(bad, g.raw, 8);
      bad[i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
      const AirFrame b = decodeAir(bad, gAir);
      CHECK(!b.crcOk && classify(b) == Kind::CrcBad, "bit %u de %02X %02X bascule non detecte", i, g.pay[0],
            g.pay[1]);
    }
    // Meme trame, autre adresse : CRC faux.
    uint8_t other[4];
    memcpy(other, gAir, 4);
    other[3] ^= 0x01;
    CHECK(!decodeAir(g.raw, other).crcOk, "CRC independant de l'adresse");
  }
}

static void testRoundTrip() {
  const uint8_t pats[3][4] = {{0x00, 0x00, 0x00, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF}, {0xC5, 0xA0, 0x3C, 0x81}};
  for (uint8_t pid = 0; pid < 4; pid++)
    for (uint8_t len = 0; len <= 4; len++)
      for (uint8_t na = 0; na < 2; na++)
        for (const auto &pat : pats) {
          uint8_t raw[8];
          encodeAir(gAir, pid, na, pat, len, raw);
          const AirFrame f = decodeAir(raw, gAir);
          CHECK(f.crcOk && f.len == len && f.pid == pid && f.noAck == na, "aller-retour PID %u len %u NO_ACK %u",
                pid, len, na);
          bool same = true;
          for (uint8_t q = 0; q < len; q++) same = same && f.pay[q] == pat[q];
          for (uint8_t q = len; q < 4; q++) same = same && f.pay[q] == 0;
          CHECK(same, "aller-retour charge, len %u", len);
          // Bourrage apres le CRC : a zero.
          const unsigned end = 9 + 8u * len + 16;
          bool pad = true;
          for (unsigned i = end; i < 64; i++) pad = pad && !((raw[i >> 3] >> (7 - (i & 7))) & 1);
          CHECK(pad, "bourrage non nul, len %u", len);
        }
  // PID hors plage : seuls les 2 bits de poids faible.
  uint8_t raw[8];
  const uint8_t pay[2] = {0xC3, 0x35};
  encodeAir(gAir, 5, false, pay, 2, raw);
  CHECK(decodeAir(raw, gAir).pid == 1, "PID 5 -> 1");
  // Longueur > 4 : jamais de CRC valide, classe CrcBad.
  for (uint8_t len = 5; len < 64; len++) {
    uint8_t r[8] = {(uint8_t)(len << 2), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const AirFrame f = decodeAir(r, gAir);
    CHECK(!f.crcOk && f.len == len && classify(f) == Kind::CrcBad, "longueur %u acceptee", len);
  }
  // 'lampe decode 08627F030C800000' : len 2, PID 0, C4 FE, CRC 0619 OK.
  const uint8_t dec[8] = {0x08, 0x62, 0x7F, 0x03, 0x0C, 0x80, 0x00, 0x00};
  const AirFrame f = decodeAir(dec, gAir);
  CHECK(f.crcOk && f.len == 2 && f.pid == 0 && f.pay[0] == 0xC4 && f.pay[1] == 0xFE && f.crc == 0x0619,
        "exemple de 'lampe decode'");
}

static void testClassify() {
  struct Case {
    uint8_t f, v, noAck, len;
    Kind k;
  };
  const Case cases[] = {
      {0xFF, 0x00, 0, 2, Kind::Service},  {0xFE, 0x00, 0, 2, Kind::Service},  {0xFD, 0x00, 0, 2, Kind::Service},
      {0xFA, 0xA8, 1, 2, Kind::Service},  {0xFA, 0xF8, 1, 2, Kind::Service},  {0x91, 0x00, 0, 2, Kind::Reserved},
      {0x89, 0x58, 0, 2, Kind::Reserved}, {0x89, 0xE0, 0, 2, Kind::Reserved}, {0x00, 0x00, 0, 2, Kind::Invalid},
      {0xC6, 0x10, 0, 2, Kind::Invalid},  {0xE1, 0x01, 0, 2, Kind::Auto},     {0xE0, 0x03, 0, 2, Kind::Auto},
      {0xA1, 0x02, 0, 2, Kind::Auto},     {0x60, 0x01, 0, 2, Kind::Auto},     {0xC3, 0x35, 0, 2, Kind::Temp},
      {0x83, 0x35, 0, 2, Kind::Temp},     {0x42, 0x64, 0, 2, Kind::Temp},     {0xC4, 0xBC, 0, 2, Kind::Bright},
      {0x05, 0xA7, 0, 2, Kind::Bright},   {0xC5, 0xA0, 0, 2, Kind::Bright},   {0x80, 0x00, 0, 2, Kind::Invalid},
      {0xC1, 0x00, 0, 2, Kind::Invalid},  {0xE7, 0x00, 0, 2, Kind::Invalid},  {0xC3, 0x35, 1, 2, Kind::Service},
      {0xC3, 0x35, 0, 1, Kind::Invalid},  {0xC3, 0x35, 0, 3, Kind::Invalid},  {0xC3, 0x35, 0, 4, Kind::Invalid},
      {0x00, 0x00, 0, 0, Kind::LampAck},  {0x00, 0x00, 1, 0, Kind::LampAck},
  };
  for (const Case &c : cases) {
    const Kind k = classify(frame(c.f, c.v, c.noAck, c.len));
    CHECK(k == c.k, "classify %02X %02X NO_ACK %u len %u : %s au lieu de %s", c.f, c.v, c.noAck, c.len,
          kindName(k), kindName(c.k));
  }
  // kindOf sur les 256 premiers octets : definition de B.2.
  for (unsigned f = 0; f < 256; f++) {
    const Kind k = kindOf({(uint8_t)f, 0});
    const unsigned sel = f & F_SELECT;
    Kind want;
    if ((f & 0xF8) == 0xF8)
      want = Kind::Service;
    else if (f & F_RSV)
      want = Kind::Reserved;
    else if (!(f & F_LAMPS) || (sel != F_TEMP && sel != F_BRIGHT && sel != F_AUTO))
      want = Kind::Invalid;
    else
      want = sel == F_TEMP ? Kind::Temp : sel == F_BRIGHT ? Kind::Bright : Kind::Auto;
    CHECK(k == want, "kindOf(%02X) = %s", f, kindName(k));
  }
}

// ---------------------------------------------------------------------------
//  Constructeurs, bornes, etat
// ---------------------------------------------------------------------------

static void testBounds() {
  for (unsigned v = 0; v < 256; v++) {
    const uint8_t b = clampBright((uint8_t)v), t = clampTemp((uint8_t)v);
    CHECK(b == (v < 0x4C ? 0x4C : v > 0xFE ? 0xFE : v), "clampBright(%02X) = %02X", v, b);
    CHECK(t == (v > 0x64 ? 0x64 : v), "clampTemp(%02X) = %02X", v, t);
  }
  for (unsigned lamps = 0; lamps < 256; lamps++)
    for (unsigned on = 0; on < 2; on++) {
      const Payload pt = makeTemp(on, (uint8_t)lamps, 0xFF);
      const Payload pb = makeBright(on, (uint8_t)lamps, 0x00);
      const Payload pa = makeAuto(on, (uint8_t)lamps, 0);
      const uint8_t wantLamps = (lamps & F_LAMPS) ? (lamps & F_LAMPS) : F_FRONT;
      CHECK(kindOf(pt) == Kind::Temp && pt.value == 0x64 && (pt.flags & F_LAMPS) == wantLamps &&
                !!(pt.flags & F_POWER) == !!on && !(pt.flags & F_RSV),
            "makeTemp lampes %02X", lamps);
      CHECK(kindOf(pb) == Kind::Bright && pb.value == 0x4C && (pb.flags & F_LAMPS) == wantLamps &&
                !!(pb.flags & F_POWER) == !!on && !(pb.flags & F_RSV),
            "makeBright lampes %02X", lamps);
      CHECK(kindOf(pa) == Kind::Auto && pa.value == 1 && (pa.flags & F_LAMPS) == wantLamps &&
                !!(pa.flags & F_POWER) == !!on && !(pa.flags & F_RSV),
            "makeAuto lampes %02X", lamps);
    }
  CHECK(makeBright(true, F_LAMPS, 0xFF) == P(0xC5, 0xFE), "makeBright FF");
  CHECK(makeTemp(true, F_BACK, 0x35) == P(0x83, 0x35), "makeTemp arriere");
  CHECK(makeAuto(true, F_BACK, 7) == P(0xA1, 0x07), "makeAuto arriere");
  CHECK(makeAuto(true, F_FRONT, 3) == P(0xE0, 0x03), "E0 03");
}

static void testApplyState() {
  State s;  // eteinte, deux, A5, 35
  CHECK(!s.power && s.lamps == F_LAMPS && s.bright == 0xA5 && s.temp == 0x35, "valeurs par defaut");
  CHECK(applyState(s, {0xC4, 0xBC}) == (FLD_FLAGS | FLD_BRIGHT) && s == mk(true, F_FRONT, 0xBC, 0x35), "C4 BC");
  CHECK(applyState(s, {0xC4, 0xBC}) == 0, "C4 BC repete");
  CHECK(applyState(s, {0x44, 0xBC}) == FLD_FLAGS && s == mk(false, F_FRONT, 0xBC, 0x35), "44 BC");
  CHECK(applyState(s, {0x83, 0x64}) == (FLD_FLAGS | FLD_TEMP) && s == mk(true, F_BACK, 0xBC, 0x64), "83 64");
  CHECK(applyState(s, {0xC5, 0x20}) == (FLD_FLAGS | FLD_BRIGHT) && s == mk(true, F_LAMPS, 0x4C, 0x64),
        "C5 20 borne a 4C");
  CHECK(applyState(s, {0xC3, 0xC8}) == 0 && s.temp == 0x64, "C3 C8 borne a 64");
  CHECK(applyState(s, {0xC5, 0xFF}) == FLD_BRIGHT && s.bright == 0xFE, "C5 FF borne a FE");
  // Rien d'autre ne touche a l'etat : A (meme '60 01', bit 7 a zero), favori, service, invalide.
  const State before = s;
  const Payload inert[] = {{0xE1, 0x01}, {0x60, 0x01}, {0xA1, 0x02}, {0x91, 0x00}, {0x89, 0x58},
                           {0xFF, 0x00}, {0xFA, 0xA8}, {0x00, 0x00}, {0xC6, 0x10}, {0x80, 0x10}};
  for (const Payload &p : inert) {
    CHECK(applyState(s, p) == 0 && s == before, "%02X %02X a modifie l'etat", p.flags, p.value);
  }
}

static void testCoveredBy() {
  const State onBoth = mk(true, F_LAMPS, 0xA5, 0x35);
  CHECK(coveredBy({0xC5, 0xA5}, onBoth) == (FLD_FLAGS | FLD_BRIGHT), "C5 A5");
  CHECK(coveredBy({0xC5, 0xA6}, onBoth) == FLD_FLAGS, "C5 A6");
  CHECK(coveredBy({0xC4, 0xA5}, onBoth) == FLD_BRIGHT, "C4 A5 : autres lampes");
  CHECK(coveredBy({0xC3, 0x35}, onBoth) == (FLD_FLAGS | FLD_TEMP), "C3 35");
  CHECK(coveredBy({0x43, 0x35}, onBoth) == 0, "43 35 : consigne allumee");
  const State offBoth = mk(false, F_LAMPS, 0x60, 0x35);
  // Une trame d'extinction n'efface que FLAGS, meme si la valeur coincide.
  CHECK(coveredBy({0x43, 0x35}, offBoth) == FLD_FLAGS, "43 35 : extinction");
  CHECK(coveredBy({0x45, 0x60}, offBoth) == FLD_FLAGS, "45 60 : extinction");
  CHECK(coveredBy({0x42, 0x35}, offBoth) == 0, "42 35 : autres lampes");
  CHECK(coveredBy({0xE1, 0x01}, onBoth) == 0 && coveredBy({0x91, 0x00}, onBoth) == 0 &&
            coveredBy({0xFF, 0x00}, onBoth) == 0,
        "A, favori, service : rien");
  CHECK(coveredBy({0xC5, 0x20}, mk(true, F_LAMPS, 0x4C, 0x35)) == (FLD_FLAGS | FLD_BRIGHT), "C5 20 = 4C");
}

// ---------------------------------------------------------------------------
//  Planification (D.3)
// ---------------------------------------------------------------------------

static void checkPlan(const char *what, const Plan &p, bool hb, Payload pb, bool ht, Payload pt) {
  CHECK(p.bright == hb && (!hb || p.pb == pb) && p.temp == ht && (!ht || p.pt == pt),
        "plan %s : lum %d %02X %02X, temp %d %02X %02X", what, p.bright, p.pb.flags, p.pb.value, p.temp,
        p.pt.flags, p.pt.value);
}

static void testPlan() {
  const Payload none{0, 0};
  // allumee, deux, A5, 35 -> eteinte (FLAGS) : 43 35
  checkPlan("extinction", plan(mk(false, F_LAMPS, 0xA5, 0x35), mk(true, F_LAMPS, 0xA5, 0x35), FLD_FLAGS),
            false, none, true, {0x43, 0x35});
  // eteinte, deux, temp 64 -> allumee (FLAGS) : C5 A5
  checkPlan("allumage", plan(mk(true, F_LAMPS, 0xA5, 0x64), mk(false, F_LAMPS, 0xA5, 0x64), FLD_FLAGS), true,
            {0xC5, 0xA5}, false, none);
  // allumee, deux -> avant seule (FLAGS) : C4 A5
  checkPlan("avant seule", plan(mk(true, F_FRONT, 0xA5, 0x35), mk(true, F_LAMPS, 0xA5, 0x35), FLD_FLAGS),
            true, {0xC4, 0xA5}, false, none);
  // allumee, deux -> lum C0 (BRIGHT) : C5 C0
  checkPlan("lum C0", plan(mk(true, F_LAMPS, 0xC0, 0x35), mk(true, F_LAMPS, 0xA5, 0x35), FLD_BRIGHT), true,
            {0xC5, 0xC0}, false, none);
  // allumee, deux -> temp 00 + lum 4C : C5 4C et C3 00
  checkPlan("temp 00 + lum 4C",
            plan(mk(true, F_LAMPS, 0x4C, 0x00), mk(true, F_LAMPS, 0xA5, 0x35), FLD_BRIGHT | FLD_TEMP), true,
            {0xC5, 0x4C}, true, {0xC3, 0x00});

  // eteinte -> lum 60 (BRIGHT), reste eteinte : rien ; puis allumee : C5 60.
  {
    const State b = mk(false, F_LAMPS, 0xA5, 0x35);
    State t = b;
    uint8_t dirty = 0;
    t.bright = 0x60;
    dirty |= FLD_BRIGHT;
    checkPlan("lum 60 eteinte", plan(t, b, dirty), false, none, false, none);
    t.power = true;
    dirty |= FLD_FLAGS;
    checkPlan("puis allumee", plan(t, b, dirty), true, {0xC5, 0x60}, false, none);
    CHECK((dirty & ~coveredBy({0xC5, 0x60}, t)) == 0, "C5 60 livre tout");
  }
  // eteinte, temp crue 64 -> temp 10 differee, puis eteinte (FLAGS) : 43 64, pas 43 10.
  {
    const State b = mk(false, F_LAMPS, 0xA5, 0x64);
    State t = b;
    uint8_t dirty = 0;
    t.temp = 0x10;
    dirty |= FLD_TEMP;
    checkPlan("temp 10 differee", plan(t, b, dirty), false, none, false, none);
    dirty |= FLD_FLAGS;
    const Plan p = plan(t, b, dirty);
    checkPlan("puis eteinte", p, false, none, true, {0x43, 0x64});
    dirty &= (uint8_t)~coveredBy(p.pt, t);
    CHECK(dirty == FLD_TEMP, "temp differee toujours due apres 43 64 (dirty %u)", dirty);
    checkPlan("apres 43 64", plan(t, b, dirty), false, none, false, none);
    // A l'allumage, la temperature differee part enfin.
    t.power = true;
    dirty |= FLD_FLAGS;
    checkPlan("allumage apres differe", plan(t, b, dirty), true, {0xC5, 0xA5}, true, {0xC3, 0x10});
  }
  // allumee, avant seule -> A (compteur 3) : E0 03, hors plan (tranche AUTO).
  {
    const State s = mk(true, F_FRONT, 0xA5, 0x35);
    checkPlan("A", plan(s, s, 0), false, none, false, none);
    CHECK(makeAuto(true, s.lamps, 3) == P(0xE0, 0x03), "A compteur 3");
  }
  // Rien a livrer : rien a emettre, allumee ou non.
  checkPlan("rien", plan(mk(true, F_LAMPS, 0xA5, 0x35), mk(false, F_FRONT, 0x60, 0x00), 0), false, none, false,
            none);
  // Resynchronisation (FLD_ALL) allumee : luminosite et temperature.
  checkPlan("sync", plan(mk(true, F_BACK, 0x60, 0x64), mk(true, F_BACK, 0x60, 0x64), FLD_ALL), true,
            {0x85, 0x60}, true, {0x83, 0x64});
}

// ---------------------------------------------------------------------------
//  Numero A, CRC-8
// ---------------------------------------------------------------------------

static void testAutoAndCrc8() {
  CHECK(nextAuto(0) == 1 && nextAuto(255) == 1, "nextAuto 0/255");
  for (unsigned l = 0; l < 256; l++) {
    const uint8_t n = nextAuto((uint8_t)l);
    CHECK(n != 0 && n != l, "nextAuto(%u) = %u", l, n);
    if (l >= 1 && l <= 254) CHECK(n == l + 1, "nextAuto(%u) = %u", l, n);
  }
  const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(crc8(check, 9) == 0xF4, "crc8 123456789 = %02X", crc8(check, 9));
  CHECK(crc8(check, 0) == 0, "crc8 vide");
}

// ---------------------------------------------------------------------------
//  Conversions Matter (E.2)
// ---------------------------------------------------------------------------

static void checkTableInvariants(const char *what) {
  CHECK(rawFromLevel(0) == rawFromLevel(1) && rawFromLevel(1) == 0x4C && rawFromLevel(254) == 0xFE,
        "%s : bornes", what);
  for (unsigned l = 2; l <= 254; l++)
    CHECK(rawFromLevel((uint8_t)l) >= rawFromLevel((uint8_t)(l - 1)), "%s : non monotone en %u", what, l);
  CHECK(rawFromLevel(255) == 0xFE, "%s : niveau 255", what);
  for (unsigned r = 0; r < 256; r++) {
    const uint8_t l = levelFromRaw((uint8_t)r);
    CHECK(l >= 1 && l <= 254, "%s : levelFromRaw(%02X) = %u", what, r, l);
    if (r <= 0xFE)
      CHECK(rawFromLevel(l) >= r && (l == 1 || rawFromLevel((uint8_t)(l - 1)) < r),
            "%s : levelFromRaw(%02X) = %u n'est pas le plus petit", what, r, l);
  }
  CHECK(levelFromRaw(0xFF) == 254, "%s : levelFromRaw(FF)", what);
}

static void checkDisplay(const char *what) {
  // La valeur ecrite par un controleur reste affichee.
  for (unsigned l = 1; l <= 254; l++)
    CHECK(displayLevel((uint8_t)l, rawFromLevel((uint8_t)l)) == l, "%s : niveau %u saute", what, l);
  // Idempotent, dans 1..254, et toujours la valeur canonique hors correspondance.
  for (unsigned a = 0; a < 256; a++)
    for (unsigned r = 0x4C; r <= 0xFE; r++) {
      const uint8_t d = displayLevel((uint8_t)a, (uint8_t)r);
      CHECK(d >= 1 && d <= 254 && displayLevel(d, (uint8_t)r) == d, "%s : affichage niveau %u/%02X", what, a, r);
      CHECK(rawFromLevel(d) == r || d == levelFromRaw((uint8_t)r), "%s : affichage niveau %u/%02X faux", what, a,
            r);
    }
  for (unsigned m = kMiredCold; m <= kMiredWarm; m++)
    CHECK(displayMired((uint16_t)m, tempFromMired((uint16_t)m)) == m, "mired %u saute", m);
  for (unsigned a = 0; a <= 600; a++)
    for (unsigned t = 0; t <= 100; t++) {
      const uint16_t d = displayMired((uint16_t)a, (uint8_t)t);
      CHECK(d >= kMiredCold && d <= kMiredWarm && displayMired(d, (uint8_t)t) == d && tempFromMired(d) == t,
            "affichage mired %u/%u", a, t);
    }
}

static void testLevelMap() {
  // Avant tout mapInit : gamma 2 (decision A3).
  CHECK(mapGamma() == 2.0f, "gamma par defaut %f", (double)mapGamma());
  CHECK(rawFromLevel(138) == 0x80, "table par defaut");

  // gamma 1 : formule entiere exacte, aller-retour brut -> niveau -> brut identite.
  mapInit(1.0f);
  CHECK(mapGamma() == 1.0f, "gamma 1");
  for (unsigned l = 0; l <= 254; l++) {
    const unsigned L = l ? l : 1;
    CHECK(rawFromLevel((uint8_t)l) == 0x4C + ((L - 1) * 178 + 126) / 253, "gamma 1 : raw(%u)", l);
  }
  for (unsigned r = 0x4C; r <= 0xFE; r++) {
    const uint8_t l = levelFromRaw((uint8_t)r);
    CHECK(rawFromLevel(l) == r, "gamma 1 : aller-retour %02X -> %u -> %02X", r, l, rawFromLevel(l));
    // Inverse ferme de E.2 : exact lui aussi, mais il vise le niveau le plus
    // proche et non le plus petit (38 valeurs sur 179 different de levelFromRaw,
    // qui suit B.3 pour tout gamma). Meme valeur brute dans tous les cas.
    const unsigned lf = 1 + ((r - 0x4C) * 253 + 89) / 178;
    CHECK(rawFromLevel((uint8_t)lf) == r && lf >= l, "gamma 1 : inverse ferme L(%02X) = %u", r, lf);
  }
  checkTableInvariants("gamma 1");
  checkDisplay("gamma 1");

  // gamma 2 : points verifies, monotone, 15 valeurs du haut inaccessibles.
  mapInit(2.0f);
  CHECK(mapGamma() == 2.0f, "gamma 2");
  const struct {
    uint8_t l, raw;
  } pts[] = {{1, 0x4C}, {64, 0x57}, {127, 0x78}, {138, 0x80}, {171, 0x9C}, {191, 0xB0}, {254, 0xFE}};
  for (const auto &p : pts)
    CHECK(rawFromLevel(p.l) == p.raw, "gamma 2 : L%u = %02X au lieu de %02X", p.l, rawFromLevel(p.l), p.raw);
  checkTableInvariants("gamma 2");
  checkDisplay("gamma 2");
  bool hit[256] = {};
  for (unsigned l = 1; l <= 254; l++) hit[rawFromLevel((uint8_t)l)] = true;
  unsigned missing = 0, lowestMissing = 0xFF;
  for (unsigned r = 0x4C; r <= 0xFE; r++)
    if (!hit[r]) {
      missing++;
      if (r < lowestMissing) lowestMissing = r;
    }
  CHECK(missing == 15 && lowestMissing >= 0xB0, "gamma 2 : %u valeurs inaccessibles, la plus basse %02X", missing,
        lowestMissing);

  // Autres gammas (reglage au banc) : les invariants tiennent.
  const float others[] = {0.5f, 1.5f, 2.2f, 3.7f};
  for (float g : others) {
    mapInit(g);
    char what[16];
    snprintf(what, sizeof(what), "gamma %.1f", (double)g);
    checkTableInvariants(what);
  }
  // Gamma absurde : lineaire.
  mapInit(0.0f);
  CHECK(mapGamma() == 1.0f, "gamma 0");
  mapInit(-2.0f);
  CHECK(mapGamma() == 1.0f, "gamma negatif");
  mapInit(NAN);
  CHECK(mapGamma() == 1.0f, "gamma NaN");
  mapInit(2.0f);
}

static void testMireds() {
  for (unsigned t = 0; t <= 100; t++) {
    const uint16_t m = miredFromTemp((uint8_t)t);
    CHECK(m >= kMiredCold && m <= kMiredWarm && tempFromMired(m) == t, "aller-retour temp %u -> %u -> %u", t, m,
          tempFromMired(m));
  }
  CHECK(miredFromTemp(0) == 153 && miredFromTemp(100) == 370, "mireds : bornes");
  CHECK(miredFromTemp(200) == 370 && miredFromTemp(255) == 370, "mireds : temp > 100");
  CHECK(tempFromMired(153) == 0 && tempFromMired(370) == 100, "temp : bornes");
  CHECK(tempFromMired(0) == 0 && tempFromMired(152) == 0 && tempFromMired(371) == 100 &&
            tempFromMired(65535) == 100,
        "temp : mireds hors plage");
  for (unsigned m = kMiredCold + 1; m <= kMiredWarm; m++)
    CHECK(tempFromMired((uint16_t)m) >= tempFromMired((uint16_t)(m - 1)), "temp non monotone en %u", m);
}

// ---------------------------------------------------------------------------
//  Regles d'intention (E.4) et memoire de selection
// ---------------------------------------------------------------------------

// Ecritures Matter, comme les callbacks de E.3 : derniere valeur gagne.
static void post(MatterIntents &in, char ep, bool on) {
  switch (ep) {
    case 'P': in.has |= IN_POWER; in.power = on; break;
    case 'F': in.has |= IN_FRONT; in.front = on; break;
    case 'B': in.has |= IN_BACK; in.back = on; break;
    case 'A': if (on) in.has |= IN_AUTO; break;
  }
}

static MatterIntents intents(const char *seq) {  // "P0F1B0" : EP1 off, avant on, arriere off
  MatterIntents in;
  for (const char *p = seq; p[0] && p[1]; p += 2) post(in, p[0], p[1] == '1');
  return in;
}

static void testRules() {
  const State off = mk(false, F_LAMPS, 0xA5, 0x35);
  const State onBoth = mk(true, F_LAMPS, 0xA5, 0x35);
  const State onFront = mk(true, F_FRONT, 0xA5, 0x35);
  Resolution r;

  // R1 : EP1 eteint, l'extinction gagne toujours, lampes = memoire.
  r = resolveMatter(onFront, intents("P0F1B1"), F_BACK);
  CHECK(!r.target.power && r.target.lamps == F_BACK && r.fields == FLD_FLAGS, "R1");
  // R2 : lampes demandees.
  r = resolveMatter(off, intents("F1"), F_LAMPS);
  CHECK(r.target.power && r.target.lamps == F_FRONT && r.fields == FLD_FLAGS, "R2 avant depuis eteinte");
  r = resolveMatter(onFront, intents("B1"), F_FRONT);
  CHECK(r.target.power && r.target.lamps == F_LAMPS, "R2 arriere en plus");
  r = resolveMatter(onBoth, intents("F0"), F_LAMPS);
  CHECK(r.target.power && r.target.lamps == F_BACK, "R2 avant off");
  // R2' : deja allumee, EP1 on ne change rien mais part quand meme.
  r = resolveMatter(onFront, intents("P1"), F_LAMPS);
  CHECK(r.target == onFront && r.fields == FLD_FLAGS, "R2'");
  // R3a : EP1 allume depuis eteinte : derniere selection.
  r = resolveMatter(off, intents("P1"), F_BACK);
  CHECK(r.target.power && r.target.lamps == F_BACK && r.fields == FLD_FLAGS, "R3a");
  // R3b : plus aucune lampe : eteinte, lampes = memoire, jamais 0.
  r = resolveMatter(onFront, intents("F0"), F_FRONT);
  CHECK(!r.target.power && r.target.lamps == F_FRONT, "R3b");
  r = resolveMatter(onBoth, intents("F0B0"), 0);
  CHECK(!r.target.power && r.target.lamps == F_LAMPS, "R3b memoire 0");
  r = resolveMatter(off, intents("B0"), F_LAMPS);
  CHECK(!r.target.power && r.target.lamps == F_LAMPS && r.fields == FLD_FLAGS, "R3b depuis eteinte");
  // Rien de marche ni de lampe : consigne intacte, aucun champ.
  r = resolveMatter(onBoth, MatterIntents(), F_LAMPS);
  CHECK(r.target == onBoth && r.fields == 0 && !r.fireAuto, "boite vide");

  // Niveau et mireds, y compris lampe eteinte (differe).
  MatterIntents in;
  in.has = IN_LEVEL;
  in.level = 138;
  r = resolveMatter(off, in, F_LAMPS);
  CHECK(!r.target.power && r.target.bright == rawFromLevel(138) && r.fields == FLD_BRIGHT, "niveau eteinte");
  CHECK(!plan(r.target, off, r.fields).bright && !plan(r.target, off, r.fields).temp, "niveau eteinte : differe");
  in.level = 0;
  r = resolveMatter(onBoth, in, F_LAMPS);
  CHECK(r.target.bright == 0x4C && r.target.power, "niveau 0 traite comme 1");
  in = MatterIntents();
  in.has = IN_MIREDS;
  in.mireds = 370;
  r = resolveMatter(off, in, F_LAMPS);
  CHECK(!r.target.power && r.target.temp == 100 && r.fields == FLD_TEMP, "mireds eteinte");
  in.has |= IN_LEVEL | IN_POWER;
  in.level = 254;
  in.power = true;
  r = resolveMatter(off, in, F_FRONT);
  CHECK(r.target == mk(true, F_FRONT, 0xFE, 100) && r.fields == FLD_ALL, "allumage + niveau + mireds");
  CHECK(plan(r.target, off, r.fields).pb == P(0xC4, 0xFE) && plan(r.target, off, r.fields).pt == P(0xC2, 0x64),
        "allumage + niveau + mireds : trames");

  // A : seulement lampe allumee et sans intention marche/lampe dans la fenetre.
  CHECK(resolveMatter(onFront, intents("A1"), F_FRONT).fireAuto, "A seul allumee");
  CHECK(!resolveMatter(off, intents("A1"), F_LAMPS).fireAuto, "A seul eteinte");
  CHECK(!resolveMatter(off, intents("P1A1"), F_LAMPS).fireAuto, "A avec EP1 on (eteinte)");
  CHECK(!resolveMatter(onBoth, intents("A1P1"), F_LAMPS).fireAuto, "A avec EP1 on (allumee)");
  CHECK(!resolveMatter(onBoth, intents("F1A1"), F_LAMPS).fireAuto, "A avec une lampe");
  CHECK(!resolveMatter(onBoth, intents("A0"), F_LAMPS).fireAuto, "EP4 off");
  in = MatterIntents();
  in.has = IN_AUTO | IN_LEVEL;
  in.level = 100;
  r = resolveMatter(onBoth, in, F_LAMPS);
  CHECK(r.fireAuto && r.fields == FLD_BRIGHT, "A avec un niveau");

  // Scene {EP1 on, avant on, arriere off} dans n'importe quel ordre : avant seule.
  const char *perms[] = {"P1F1B0", "P1B0F1", "F1P1B0", "F1B0P1", "B0P1F1", "B0F1P1"};
  const State bases[] = {off, onBoth, mk(true, F_BACK, 0x60, 0x10), mk(false, F_BACK, 0x60, 0x10)};
  for (const char *p : perms)
    for (const State &b : bases) {
      r = resolveMatter(b, intents(p), F_LAMPS);
      CHECK(r.target.power && r.target.lamps == F_FRONT && r.fields == FLD_FLAGS, "scene %s", p);
    }
}

// Petit banc : consigne + memoire de selection, fenetres Matter espacees.
struct Bench {
  State t;
  SelectionMemory mem;
  uint32_t now = 0;
  explicit Bench(const State &s) : t(s) { mem.reset(s.lamps); }
  void run(uint32_t ms) {  // tick() toutes les ms
    for (uint32_t i = 0; i < ms; i++) mem.update(t, ++now, 2000);
  }
  void window(const char *seq) {
    const Resolution r = resolveMatter(t, intents(seq), mem.memory(t));
    t = r.target;
    run(150);  // coalescence (120 ms de calme) avant la fenetre suivante
  }
};

static void testSelectionMemory() {
  SelectionMemory m;
  m.reset(0);
  CHECK(m.memory(mk(true, F_FRONT, 0, 0)) == F_LAMPS, "reset(0)");
  m.reset(F_BACK | 0x80);
  CHECK(m.memory(mk(true, F_FRONT, 0, 0)) == F_BACK, "reset masque");
  CHECK(m.memory(mk(false, F_FRONT, 0, 0)) == F_FRONT, "eteinte : lampes de la consigne");

  // Selection gardee seulement apres 2 s allumee.
  m.reset(F_LAMPS);
  m.update(mk(true, F_LAMPS, 0, 0), 5000, 2000);
  m.update(mk(true, F_FRONT, 0, 0), 5100, 2000);
  CHECK(m.memory(mk(true, F_FRONT, 0, 0)) == F_LAMPS, "selection de 0 s");
  m.update(mk(true, F_FRONT, 0, 0), 7099, 2000);
  CHECK(m.memory(mk(true, F_FRONT, 0, 0)) == F_LAMPS, "selection de 1,999 s");
  m.update(mk(true, F_FRONT, 0, 0), 7100, 2000);
  CHECK(m.memory(mk(true, F_FRONT, 0, 0)) == F_FRONT, "selection de 2 s");
  m.update(mk(false, F_BACK, 0, 0), 7200, 2000);
  CHECK(m.memory(mk(true, F_FRONT, 0, 0)) == F_BACK, "eteinte : copie des lampes");
  // Passage de millis() par zero.
  m.reset(F_LAMPS);
  m.update(mk(true, F_BACK, 0, 0), 0xFFFFFF00u, 2000);
  m.update(mk(true, F_BACK, 0, 0), 0x00000100u, 2000);
  CHECK(m.memory(mk(true, F_BACK, 0, 0)) == F_LAMPS, "debordement : 512 ms");
  m.update(mk(true, F_BACK, 0, 0), 0x00000800u, 2000);
  CHECK(m.memory(mk(true, F_BACK, 0, 0)) == F_BACK, "debordement : 2,3 s");

  // 'Tout eteindre' : la memoire garde 'deux', dans tous les ordres, en une ou
  // plusieurs fenetres.
  const char *const scen[][3] = {
      {"P0F0B0", nullptr, nullptr}, {"B0F0P0", nullptr, nullptr}, {"F0B0", "P0", nullptr},
      {"P0", "F0B0", nullptr},      {"F0", "B0P0", nullptr},      {"B0", "F0P0", nullptr},
      {"F0", "B0", "P0"},           {"B0", "F0", "P0"},           {"P0", "F0", "B0"},
      {"P0", "B0", "F0"},           {"F0", "P0", "B0"},           {"B0", "P0", "F0"},
  };
  for (const auto &s : scen) {
    Bench b(mk(true, F_LAMPS, 0xA5, 0x35));
    b.run(5000);
    for (const char *w : s)
      if (w) b.window(w);
    CHECK(!b.t.power && b.t.lamps == F_LAMPS, "tout eteindre %s|%s|%s : %d %02X", s[0], s[1] ? s[1] : "",
          s[2] ? s[2] : "", b.t.power, b.t.lamps);
    b.window("P1");  // 'Allume la Halo' : les deux
    CHECK(b.t.power && b.t.lamps == F_LAMPS, "rallumage apres %s", s[0]);
  }
  // Une selection tenue 2 s est retrouvee par EP1 on.
  {
    Bench b(mk(true, F_LAMPS, 0xA5, 0x35));
    b.run(5000);
    b.window("F0");  // arriere seule
    b.run(2500);
    b.window("P0");
    CHECK(!b.t.power && b.t.lamps == F_BACK, "extinction apres arriere seule");
    b.window("P1");
    CHECK(b.t.power && b.t.lamps == F_BACK, "EP1 on retrouve arriere seule");
    b.window("P0F0B0");  // tout eteindre : arriere gardee
    CHECK(!b.t.power && b.t.lamps == F_BACK, "tout eteindre garde arriere seule");
  }
}

// ---------------------------------------------------------------------------

int main() {
  testLevelMap();  // en premier : gamma par defaut avant tout mapInit
  testAddress();
  testGolden();
  testRoundTrip();
  testClassify();
  testBounds();
  testApplyState();
  testCoveredBy();
  testPlan();
  testAutoAndCrc8();
  testMireds();
  testRules();
  testSelectionMemory();

  // L'auto-test embarque passe, quel que soit le gamma en place.
  const float gammas[] = {2.0f, 1.0f, 0.5f, 3.7f};
  for (float g : gammas) {
    mapInit(g);
    char msg[96];
    const int n = selfTest(msg, sizeof(msg));
    CHECK(n == 0, "selfTest gamma %.1f : %d echec(s), %s", (double)g, n, msg);
  }
  CHECK(selfTest(nullptr, 0) == 0, "selfTest sans message");
  mapInit(2.0f);

  printf("%d verification(s), %d echec(s)\n", gChecks, gFails);
  return gFails ? 1 : 0;
}
