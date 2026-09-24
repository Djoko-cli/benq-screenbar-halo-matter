// Tests hote du protocole Halo 1, des correspondances Matter, de la
// surveillance du BM5602, de la LED d'etat et du bouton BOOT (sans carte).
// Lancer : sh tools/test_halo1.sh
//
// Vecteurs et tables : docs/PLAN-PILOTE-HALO1.md (H/C2, D.3, E.2, E.4) et
// docs/PROTOCOL.md. Adresse sur l'air 63 FD F0 4F.
#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_button.h"
#include "halo1_map.h"
#include "halo1_proto.h"
#include "halo1_watch.h"
#include "matter_resume.h"
#include "status_led.h"

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
  // Consigne hors invariants : bornee comme par les constructeurs.
  CHECK(coveredBy({0xC4, 0x4C}, mk(true, 0, 0x20, 0x35)) == (FLD_FLAGS | FLD_BRIGHT), "lampes 0, lum 20");
  CHECK(coveredBy({0xC3, 0x64}, mk(true, 0xC3, 0xA5, 0xC8)) == (FLD_FLAGS | FLD_TEMP), "bits parasites, temp C8");
  CHECK(coveredBy({0x44, 0x60}, mk(false, 0, 0x60, 0x35)) == FLD_FLAGS, "extinction, lampes 0");
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
//  Livraison : plan() et coveredBy() jusqu'a epuisement
// ---------------------------------------------------------------------------

// Livre les trames de plan() une a une, comme les tranches de C4 (complete :
// etat cru mis a jour, champs couverts effaces, puis replan). tempFirst : la
// tranche de temperature finit la premiere (celle de luminosite a perdu un
// paquet). Renvoie le nombre de trames, ou -1 si plan() en redemande apres 'max'.
static int deliver(const State &t, State &b, uint8_t &dirty, bool tempFirst, Payload *out, int max) {
  for (int n = 0;; n++) {
    const Plan p = plan(t, b, dirty);
    if (!p.bright && !p.temp) return n;
    if (n == max) return -1;
    const Payload sent = (p.temp && (tempFirst || !p.bright)) ? p.pt : p.pb;
    if (out) out[n] = sent;
    applyState(b, sent);
    dirty &= (uint8_t)~coveredBy(sent, t);
  }
}

static void testDelivery() {
  // Toute consigne s'epuise, meme hors invariants (lampes 0 ou bits parasites,
  // luminosite < 4C, temperature > 64) et quel que soit l'ordre de fin des
  // tranches : sinon C4 rearmerait la meme tranche apres chaque rafale, sans fin.
  for (unsigned on = 0; on < 2; on++)
    for (unsigned lamps = 0; lamps < 256; lamps++)
      for (unsigned v = 0; v < 256; v++)
        for (unsigned fields = 1; fields <= FLD_ALL; fields++)
          for (unsigned tf = 0; tf < 2; tf++) {
            const State t = mk(on, (uint8_t)lamps, (uint8_t)v, (uint8_t)(255 - v));
            State b = mk(!on, F_LAMPS, 0xA5, 0x35);
            const uint8_t due = dueFields(t, (uint8_t)fields);
            uint8_t dirty = due;
            const int n = deliver(t, b, dirty, tf, nullptr, 4);
            const uint8_t wantLamps = (lamps & F_LAMPS) ? (lamps & F_LAMPS) : F_FRONT;
            // Allumee : tout est livre, en 2 trames au plus. Eteinte : une trame
            // au plus, et seules la luminosite et la temperature restent dues.
            bool ok = on ? (n >= 0 && n <= 2 && dirty == 0)
                         : (n >= 0 && n <= 1 && dirty == (due & (FLD_BRIGHT | FLD_TEMP)));
            if (fields & FLD_FLAGS) ok = ok && b.power == !!on && b.lamps == wantLamps;
            // A4 (a) : un allumage ou un changement de lampes part avec la luminosite affichee.
            if (on && (fields & (FLD_FLAGS | FLD_BRIGHT))) ok = ok && b.bright == clampBright((uint8_t)v);
            if (on && (fields & FLD_TEMP)) ok = ok && b.temp == clampTemp((uint8_t)(255 - v));
            CHECK(ok, "livraison %s lampes %02X lum %02X champs %u %s : %d trame(s), reste %u", on ? "on" : "off",
                  lamps, v, fields, tf ? "temp d'abord" : "lum d'abord", n, dirty);
          }

  // EP1 on + 370 mireds dans une fenetre, lampe eteinte : FLAGS | TEMP. La
  // tranche de temperature finit la premiere : C5 60 part quand meme.
  {
    const State off = mk(false, F_LAMPS, 0x60, 0x35);
    MatterIntents in;
    in.has = IN_POWER | IN_MIREDS;
    in.power = true;
    in.mireds = 370;
    const Resolution r = resolveMatter(off, in, F_LAMPS);
    CHECK(r.fields == (FLD_FLAGS | FLD_TEMP) && dueFields(r.target, r.fields) == FLD_ALL, "EP1 on + mireds : champs");
    State b = off;
    uint8_t dirty = dueFields(r.target, r.fields);
    const Plan p = plan(r.target, b, dirty);
    CHECK(p.pb == P(0xC5, 0x60) && p.pt == P(0xC3, 0x64), "EP1 on + mireds : trames");
    applyState(b, p.pt);
    dirty &= (uint8_t)~coveredBy(p.pt, r.target);
    CHECK(plan(r.target, b, dirty).bright && plan(r.target, b, dirty).pb == P(0xC5, 0x60),
          "C3 64 livree d'abord : C5 60 toujours due (reste %u)", dirty);
  }
  // Eteinte : FLAGS ne rend pas la luminosite due ; bits hors FLD_ALL ignores.
  CHECK(dueFields(mk(false, F_LAMPS, 0x60, 0x35), FLD_FLAGS) == FLD_FLAGS, "dueFields eteinte");
  CHECK(dueFields(mk(true, F_LAMPS, 0x60, 0x35), FLD_TEMP) == FLD_TEMP, "dueFields sans FLAGS");
  CHECK(dueFields(mk(true, F_LAMPS, 0x60, 0x35), 0xF8) == 0, "dueFields bits parasites");
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
//  Appuis A de la telecommande (AutoPressFilter, D.6)
// ---------------------------------------------------------------------------

// Trames A (numero, instant) ; numero 0 : autre commande (reset). Renvoie le
// nombre d'appuis comptes.
struct AutoRx { uint8_t value; uint32_t at; };
static unsigned countPresses(const AutoRx *rx, size_t n) {
  AutoPressFilter f;
  unsigned presses = 0;
  for (size_t i = 0; i < n; i++) {
    if (!rx[i].value) f.reset();
    else if (f.feed(rx[i].value, rx[i].at)) presses++;
  }
  return presses;
}

static void testAutoPresses() {
  // btn-A4.log : E0 01 x3 en 200 ms, un seul appui.
  const AutoRx copies[] = {{1, 5000}, {1, 5100}, {1, 5200}};
  CHECK(countPresses(copies, 3) == 1, "3 copies : %u appuis", countPresses(copies, 3));
  // Premiere trame depuis le demarrage, meme a l'instant 0.
  const AutoRx first[] = {{1, 0}};
  CHECK(countPresses(first, 1) == 1, "premiere trame a 0 ms");
  // Numero suivant : nouvel appui, meme 300 ms apres.
  const AutoRx next[] = {{1, 5000}, {1, 5100}, {2, 5300}, {2, 5400}};
  CHECK(countPresses(next, 4) == 2, "01 puis 02 : %u appuis", countPresses(next, 4));
  // Meme numero 1 s apres la trame precedente : nouvel appui ; 999 ms : copie.
  const AutoRx again[] = {{1, 5000}, {1, 6000}, {1, 6999}};
  CHECK(countPresses(again, 3) == 2, "01 puis 01 a +1000 : %u appuis", countPresses(again, 3));
  // Retour a zero de millis() entre deux copies : meme appui ; puis 1 s apres.
  const AutoRx wrap[] = {{1, 0xFFFFFFA0u}, {1, 0x00000040u}, {1, 0x00000440u}};
  CHECK(countPresses(wrap, 2) == 1 && countPresses(wrap, 3) == 2, "retour a zero : %u/%u appuis",
        countPresses(wrap, 2), countPresses(wrap, 3));
  // Une autre commande (C4 xx) entre deux A : le numero repart a 01, nouvel appui.
  const AutoRx reset[] = {{1, 5000}, {0, 5200}, {1, 5400}, {1, 5500}};
  CHECK(countPresses(reset, 4) == 2, "01, C4 xx, 01 a +400 : %u appuis", countPresses(reset, 4));
  // Instant anterieur dans le meme tour (trame d'accuse a la fin d'emission,
  // puis trame lue avec l'instant du debut du tour) : meme appui.
  const AutoRx back[] = {{1, 5030}, {1, 5000}, {1, 5100}};
  CHECK(countPresses(back, 3) == 1, "instant anterieur : %u appuis", countPresses(back, 3));
  // Tres anterieur (plus de 1 s) : nouvel appui, comme tres posterieur.
  const AutoRx far[] = {{1, 9000}, {1, 5000}};
  CHECK(countPresses(far, 2) == 2, "instant anterieur de 4 s : %u appuis", countPresses(far, 2));
  // Longue pause (plus de 24,8 jours) : nouvel appui malgre le changement de signe.
  const AutoRx pause[] = {{1, 5000}, {1, 5000u + 0x90000000u}};
  CHECK(countPresses(pause, 2) == 2, "pause de 28 jours : %u appuis", countPresses(pause, 2));
}

// ---------------------------------------------------------------------------
//  Conversions Matter (E.2)
// ---------------------------------------------------------------------------

// Pourcentage qu'affiche Apple Home pour un niveau : formule inconnue, donc
// les quatre candidates, sur 254 ou depuis MinLevel = 1 (sur 253), arrondi ou
// tronque. Le plancher doit donner au moins 1 % dans chacune.
static unsigned percentRounded(unsigned level) { return (level * 100 + 127) / 254; }
static unsigned percentTruncated(unsigned level) { return level * 100 / 254; }
static unsigned percentMinRounded(unsigned level) { return ((level - 1) * 100 + 126) / 253; }
static unsigned percentMinTruncated(unsigned level) { return (level - 1) * 100 / 253; }
static bool showsAtLeast1(unsigned level) {
  return level >= 1 && percentRounded(level) >= 1 && percentTruncated(level) >= 1 &&
         percentMinRounded(level) >= 1 && percentMinTruncated(level) >= 1;
}

static void checkTableInvariants(const char *what) {
  CHECK(rawFromLevel(0) == rawFromLevel(1) && rawFromLevel(1) == 0x4C && rawFromLevel(254) == 0xFE,
        "%s : bornes", what);
  // Niveaux 0..plancher : tous au minimum, quel que soit gamma.
  for (unsigned l = 0; l <= kMatterLevelFloor; l++)
    CHECK(rawFromLevel((uint8_t)l) == 0x4C, "%s : niveau %u sous le plancher -> %02X", what, l,
          rawFromLevel((uint8_t)l));
  for (unsigned l = 2; l <= 254; l++)
    CHECK(rawFromLevel((uint8_t)l) >= rawFromLevel((uint8_t)(l - 1)), "%s : non monotone en %u", what, l);
  CHECK(rawFromLevel(255) == 0xFE, "%s : niveau 255", what);
  for (unsigned r = 0; r < 256; r++) {
    const uint8_t l = levelFromRaw((uint8_t)r);
    CHECK(l >= kMatterLevelFloor && l <= 254, "%s : levelFromRaw(%02X) = %u", what, r, l);
    // Jamais 0 % dans Apple Home, quelle que soit sa formule.
    CHECK(showsAtLeast1(l), "%s : levelFromRaw(%02X) = %u, 0 %%", what, r, l);
    if (r <= 0xFE)
      CHECK(rawFromLevel(l) >= r && (l == kMatterLevelFloor || rawFromLevel((uint8_t)(l - 1)) < r),
            "%s : levelFromRaw(%02X) = %u n'est pas le plus petit", what, r, l);
  }
  CHECK(levelFromRaw(0x4C) == kMatterLevelFloor && levelFromRaw(0) == kMatterLevelFloor,
        "%s : minimum rapporte %u", what, levelFromRaw(0x4C));
  CHECK(levelFromRaw(0xFF) == 254, "%s : levelFromRaw(FF)", what);
}

static void checkDisplay(const char *what) {
  // La valeur ecrite par un controleur reste affichee, des le plancher ;
  // dessous, le plancher (meme valeur brute 4C).
  for (unsigned l = 0; l <= 254; l++) {
    const uint8_t d = displayLevel((uint8_t)l, rawFromLevel((uint8_t)l));
    if (l >= kMatterLevelFloor)
      CHECK(d == l, "%s : niveau %u saute a %u", what, l, d);
    else
      CHECK(d == kMatterLevelFloor, "%s : niveau %u affiche %u, pas le plancher", what, l, d);
  }
  // Idempotent, dans plancher..254, et toujours la valeur canonique hors correspondance.
  for (unsigned a = 0; a < 256; a++)
    for (unsigned r = 0x4C; r <= 0xFE; r++) {
      const uint8_t d = displayLevel((uint8_t)a, (uint8_t)r);
      CHECK(d >= kMatterLevelFloor && d <= 254 && displayLevel(d, (uint8_t)r) == d,
            "%s : affichage niveau %u/%02X", what, a, r);
      CHECK(rawFromLevel(d) == r || d == levelFromRaw((uint8_t)r), "%s : affichage niveau %u/%02X faux", what, a,
            r);
    }
  // Terrain du 23/09 : lampe au minimum (4C, molette ou 'C5 4C' renvoye par le
  // switch), CurrentLevel 1 en cache -> 0 % dans Apple Home, montre plein.
  // Quel que soit le cache, le niveau affiche est >= plancher et donne 4C.
  for (unsigned a = 0; a < 256; a++) {
    const uint8_t d = displayLevel((uint8_t)a, 0x4C);
    CHECK(d >= kMatterLevelFloor && rawFromLevel(d) == 0x4C && showsAtLeast1(d),
          "%s : 4C affiche %u (cache %u)", what, d, a);
  }
  CHECK(displayLevel(1, 0x4C) == kMatterLevelFloor && displayLevel(0, 0x4C) == kMatterLevelFloor,
        "%s : 4C depuis le niveau 1", what);
  // Un pas de molette au-dessus : affichage deja juste, inchange.
  CHECK(displayLevel(0, 0x4D) == levelFromRaw(0x4D) && levelFromRaw(0x4D) > kMatterLevelFloor,
        "%s : 4D affiche %u", what, displayLevel(0, 0x4D));
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
  // Plancher : le plus petit niveau montre a au moins 1 % par les quatre
  // formules (3 donne 0 % en (L-1)/253 tronque).
  CHECK(showsAtLeast1(kMatterLevelFloor) && !showsAtLeast1(kMatterLevelFloor - 1u),
        "plancher %u : pas le plus petit niveau a 1 %%", kMatterLevelFloor);
  // Avant tout mapInit : gamma 2 (decision A3).
  CHECK(mapGamma() == 2.0f, "gamma par defaut %f", (double)mapGamma());
  CHECK(rawFromLevel(138) == 0x80, "table par defaut");

  // gamma 1 : formule entiere exacte au-dessus du plancher, 4C jusqu'a lui.
  mapInit(1.0f);
  CHECK(mapGamma() == 1.0f, "gamma 1");
  for (unsigned l = 0; l <= 254; l++) {
    const unsigned L = l ? l : 1;
    const unsigned want = L <= kMatterLevelFloor ? 0x4C : 0x4C + ((L - 1) * 178 + 126) / 253;
    CHECK(rawFromLevel((uint8_t)l) == want, "gamma 1 : raw(%u)", l);
  }
  // Aller-retour brut -> niveau rapporte (>= plancher) -> brut : l'identite pour
  // toute valeur atteinte depuis le plancher. Seules 4D et 4E, que donnaient
  // les niveaux 3 et 4 (desormais 4C), ne le sont plus : elles se rapportent au
  // niveau de 4F, le premier au-dessus du plancher.
  CHECK(rawFromLevel(kMatterLevelFloor + 1) == 0x4F, "gamma 1 : premier niveau au-dessus du plancher -> %02X",
        rawFromLevel(kMatterLevelFloor + 1));
  for (unsigned r = 0x4C; r <= 0xFE; r++) {
    const uint8_t l = levelFromRaw((uint8_t)r);
    if (r == 0x4D || r == 0x4E) {
      CHECK(l == kMatterLevelFloor + 1 && rawFromLevel(l) == 0x4F, "gamma 1 : %02X -> %u -> %02X", r, l,
            rawFromLevel(l));
      continue;
    }
    CHECK(l >= kMatterLevelFloor && rawFromLevel(l) == r, "gamma 1 : aller-retour %02X -> %u -> %02X", r, l,
          rawFromLevel(l));
    // Inverse ferme de E.2, au-dessus du plancher : exact lui aussi, mais il
    // vise le niveau le plus proche et non le plus petit (levelFromRaw suit B.3
    // pour tout gamma). Meme valeur brute dans tous les cas.
    if (r > 0x4E) {
      const unsigned lf = 1 + ((r - 0x4C) * 253 + 89) / 178;
      CHECK(lf > kMatterLevelFloor && rawFromLevel((uint8_t)lf) == r && lf >= l,
            "gamma 1 : inverse ferme L(%02X) = %u", r, lf);
    }
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

  // Niveau ecrit par la pile sur un On/Off (LevelControl avec la fonction OnOff) :
  // minimum puis niveau garde, ou minimum si OnLevel est fixe. Ecarte : rien ne
  // reste a livrer lampe eteinte, qui partirait au prochain allumage a la
  // telecommande.
  const uint8_t shown = levelFromRaw(0xA5);
  const uint8_t stackLevels[] = {1, shown, 254, 0};
  const State offBases[] = {onBoth, off};
  for (uint8_t l : stackLevels)
    for (const State &b : offBases) {
      in = MatterIntents();
      in.has = IN_POWER | IN_LEVEL;
      in.level = l;
      r = resolveMatter(b, in, F_LAMPS);
      CHECK(!r.target.power && r.target.bright == 0xA5 && r.fields == FLD_FLAGS,
            "EP1 off + niveau %u (%s) : champs %u lum %02X", l, b.power ? "allumee" : "eteinte", r.fields,
            r.target.bright);
      uint8_t dirty = dueFields(r.target, r.fields);
      const Plan p = plan(r.target, b, dirty);
      CHECK(!p.bright && p.temp && p.pt == P(0x43, 0x35), "EP1 off + niveau %u : trame", l);
      dirty &= (uint8_t)~coveredBy(p.pt, r.target);
      CHECK(dirty == 0, "EP1 off + niveau %u : reste %u apres l'extinction", l, dirty);
    }
  // EP1 on + niveau affiche : rien de plus que l'allumage, qui part deja avec
  // la luminosite affichee (A4 (a)).
  in = MatterIntents();
  in.has = IN_POWER | IN_LEVEL;
  in.power = true;
  in.level = shown;
  r = resolveMatter(off, in, F_LAMPS);
  CHECK(r.target == onBoth && r.fields == FLD_FLAGS && dueFields(r.target, r.fields) == (FLD_FLAGS | FLD_BRIGHT),
        "EP1 on + niveau affiche");
  r = resolveMatter(onFront, in, F_LAMPS);
  CHECK(r.target == onFront && r.fields == FLD_FLAGS, "EP1 on + niveau affiche, deja allumee");
  // Valeur brute hors d'atteinte de Matter (reglee a la telecommande) : le niveau
  // affiche en donne une voisine, il est ecarte quand meme.
  unsigned unreachable = 0;
  for (unsigned v = kBrightMin; v <= kBrightMax && !unreachable; v++)
    if (rawFromLevel(levelFromRaw((uint8_t)v)) != v) unreachable = v;
  CHECK(unreachable != 0, "gamma 2 : aucune valeur brute hors d'atteinte");
  const State offFar = mk(false, F_LAMPS, (uint8_t)unreachable, 0x35);
  in.level = levelFromRaw((uint8_t)unreachable);
  r = resolveMatter(offFar, in, F_LAMPS);
  CHECK(r.target.power && r.target.bright == unreachable && r.fields == FLD_FLAGS,
        "EP1 on + niveau affiche, lum %02X hors d'atteinte", unreachable);
  // EP1 on + un autre niveau (OnLevel) : ordre de luminosite garde.
  in.level = 64;
  r = resolveMatter(off, in, F_LAMPS);
  CHECK(r.target.power && r.target.bright == rawFromLevel(64) && r.fields == (FLD_FLAGS | FLD_BRIGHT),
        "EP1 on + OnLevel");
  // Niveau seul (curseur), lampe eteinte : toujours differe, jamais ecarte.
  in.has = IN_LEVEL;
  in.level = shown;
  r = resolveMatter(off, in, F_LAMPS);
  CHECK(r.target == off && r.fields == FLD_BRIGHT, "niveau seul eteinte, egal a la consigne");

  // Plancher : un controleur ecrit 1..3 (1 % dans Apple Home) -> le minimum 4C.
  for (unsigned l = 0; l <= kMatterLevelFloor; l++) {
    in = MatterIntents();
    in.has = IN_LEVEL;
    in.level = (uint8_t)l;
    r = resolveMatter(onBoth, in, F_LAMPS);
    CHECK(r.target == mk(true, F_LAMPS, 0x4C, 0x35) && r.fields == FLD_BRIGHT, "niveau %u -> lum %02X", l,
          r.target.bright);
  }
  // EP1 on + un niveau sous le plancher, consigne deja a 4C : n'ajoute rien a
  // l'allumage (qui part avec 4C), comme le niveau affiche (le plancher).
  const State offMin = mk(false, F_LAMPS, 0x4C, 0x35);
  for (unsigned l = 0; l <= kMatterLevelFloor; l++) {
    in = MatterIntents();
    in.has = IN_POWER | IN_LEVEL;
    in.power = true;
    in.level = (uint8_t)l;
    r = resolveMatter(offMin, in, F_LAMPS);
    CHECK(r.target == mk(true, F_LAMPS, 0x4C, 0x35) && r.fields == FLD_FLAGS,
          "EP1 on + niveau %u, lum 4C : champs %u", l, r.fields);
    CHECK(plan(r.target, offMin, dueFields(r.target, r.fields)).pb == P(0xC5, 0x4C),
          "EP1 on + niveau %u, lum 4C : trame", l);
  }
  // ... mais depuis A5, le niveau 1 reste un ordre (4C).
  in.level = 1;
  r = resolveMatter(off, in, F_LAMPS);
  CHECK(r.target.power && r.target.bright == 0x4C && r.fields == (FLD_FLAGS | FLD_BRIGHT),
        "EP1 on + niveau 1 depuis A5");

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
//  Rejeu des essais de banc T1 et T2 (I.2) sur les fonctions pures
// ---------------------------------------------------------------------------

// Pilote simule (C4) : consigne, etat cru, champs a livrer, memoire de
// selection. Une trame dure ~343 ms (reset de 43 ms, 3 paquets a 100 ms), et
// serial_run.py envoie la commande suivante des le retour de l'invite.
struct Pilot {
  State t, b;  // 'lampe oublie' puis reboot : eteinte, deux, A5, 35
  uint8_t dirty = 0;
  SelectionMemory mem;
  uint32_t now = 0;
  char sent[32] = "";  // trames de la derniere commande, "85 60 83 64"
  Pilot() { mem.reset(t.lamps); }
  void wait(uint32_t ms) {  // tick() toutes les 10 ms
    for (uint32_t i = 0; i < ms; i += 10) mem.update(t, now += 10, 2000);
  }
  void request(const State &nt, uint8_t fields) {
    t = nt;
    dirty |= dueFields(t, fields);
    Payload out[4];
    const int n = deliver(t, b, dirty, false, out, 4);
    for (int i = 0; i < n; i++)
      snprintf(sent + strlen(sent), sizeof(sent) - strlen(sent), "%s%02X %02X", i ? " " : "", out[i].flags,
               out[i].value);
    wait(50 + 343u * (n > 0 ? n : 0));
  }
  void matter(const char *seq) {
    const Resolution r = resolveMatter(t, intents(seq), mem.memory(t));
    request(r.target, r.fields);
  }
  void cmd(const char *c) {  // commandes 'lampe ...' de G.2 ; "@3000" : attendre 3 s
    State s = t;
    unsigned v;
    sent[0] = 0;
    if (c[0] == '@') wait((uint32_t)strtoul(c + 1, nullptr, 10));
    else if (!strcmp(c, "on")) matter("P1");
    else if (!strcmp(c, "off")) matter("P0");
    else if (!strcmp(c, "avant on")) matter("F1");
    else if (!strcmp(c, "avant off")) matter("F0");
    else if (!strcmp(c, "arriere on")) matter("B1");
    else if (!strcmp(c, "arriere off")) matter("B0");
    else if (!strcmp(c, "mode avant")) {
      s.power = true;
      s.lamps = F_FRONT;
      request(s, FLD_FLAGS);
    } else if (sscanf(c, "lum %x", &v) == 1) {
      s.bright = (uint8_t)v;
      request(s, FLD_BRIGHT);
    } else if (sscanf(c, "temp %u", &v) == 1) {
      s.temp = (uint8_t)v;
      request(s, FLD_TEMP);
    } else if (!strcmp(c, "sync")) {
      request(s, FLD_ALL);
    } else {
      CHECK(false, "commande inconnue '%s'", c);
    }
  }
};

static void testBenchT2() {
  // Predictions de I.2 (T1 puis T2), chaque trame une fois ; "@..." : aucune trame.
  const char *const steps[][2] = {
      {"@3000", ""},       {"on", "C5 A5"},        {"temp 0", "C3 00"},   {"temp 100", "C3 64"},
      {"mode avant", "C4 A5"}, {"lum 4C", "C4 4C"}, {"lum FE", "C4 FE"},  {"@3000", ""},
      {"off", "42 64"},    {"lum 60", ""},         {"on", "C4 60"},       {"arriere on", "C5 60"},
      {"@3000", ""},       {"avant off", "85 60"}, {"@3000", ""},         {"arriere off", "03 64"},
      {"on", "85 60"},     {"sync", "85 60 83 64"},
  };
  Pilot p;
  for (const auto &s : steps) {
    p.cmd(s[0]);
    CHECK(!strcmp(p.sent, s[1]), "T2 '%s' : '%s' au lieu de '%s'", s[0], p.sent, s[1]);
  }
  // Sans l'attente avant 'off', 'mode avant' n'a pas tenu 2 s : la memoire
  // garde les deux lampes (43 64), et le rallumage aussi (C5 60).
  Pilot q;
  const char *const quick[] = {"@3000", "on", "temp 0", "temp 100", "mode avant", "lum 4C", "lum FE", "off"};
  for (const char *c : quick) q.cmd(c);
  CHECK(!strcmp(q.sent, "43 64"), "T2 sans attente : '%s'", q.sent);
  q.cmd("lum 60");
  q.cmd("on");
  CHECK(!strcmp(q.sent, "C5 60"), "T2 sans attente, rallumage : '%s'", q.sent);
}

// ---------------------------------------------------------------------------
//  Calendrier de relance des abonnements (src/matter_resume.h)
// ---------------------------------------------------------------------------

// Premier instant (pas de 100 ms) ou due() repond oui, entre from et to ; 0 sinon.
static uint32_t firstDue(ResumePlanner &p, uint32_t from, uint32_t to, uint32_t start) {
  for (uint32_t t = from; (uint32_t)(to - t) <= (uint32_t)(to - from); t += 100)
    if (p.due(t, start)) return t;
  return 0;
}

static void testResumePlanner() {
  const uint32_t base = 0xFFFF0000u;  // millis() repasse par zero pendant l'essai
  {
    ResumePlanner p;
    p.subscriptions(0, base + 1000);
    CHECK(!p.due(base + 4000, base), "reseau pas pret : aucune tentative");
    p.network(true, base + 5000);  // pret a +5 s : attendre +50 s (tentative de la pile)
    CHECK(firstDue(p, base + 5000, base + 120000, base) == base + 50000, "premiere tentative a +50 s : %lu",
          (unsigned long)(firstDue(p, base + 5000, base + 120000, base) - base));
    p.fired();
    CHECK(!p.due(base + 50100, base) && p.waiting, "tentative en cours : rien d'autre");
    // Echecs : 30 s, 60 s, puis 5 min apres chaque fin.
    const uint32_t gaps[] = {30000, 60000, 300000, 300000};
    uint32_t t = base + 95000;  // fin de la 1re (recherche d'adresse de 45 s)
    for (uint32_t g : gaps) {
      p.finished(1, t);
      CHECK(!p.due(t + g - 100, t), "pas avant %lu ms", (unsigned long)g);
      CHECK(p.due(t + g, base), "a %lu ms apres la fin", (unsigned long)g);
      CHECK(p.holdLeftMs(t + g) == 0, "attente finie");
      p.fired();
      t += g + 45000;
    }
    CHECK(p.tries == 5, "5 tentatives dans l'episode : %u", p.tries);
    // Abonnement repris : un coup d'oeil toutes les 5 min, episode pas encore clos.
    p.finished(1, t);
    p.subscriptions(1, t + 1000);
    CHECK(p.tries == 5 && !p.due(t + 1000 + 299900, base), "abonnement actif : rien avant 5 min");
    CHECK(p.due(t + 1000 + 300000, base), "abonnement actif : coup d'oeil a 5 min");
    p.fired();
    CHECK(p.tries == 5, "coup d'oeil : pas un essai de l'episode (%u)", p.tries);
    p.finished(0, t + 302000);  // abonne deja servi : rien de lance
    CHECK(!p.due(t + 302000 + 299900, base) && p.due(t + 302000 + 300000, base), "coup d'oeil suivant a 5 min");
    p.subscriptions(1, t + 1000 + ResumePlanner::kStableMs);
    CHECK(p.stable && p.tries == 0, "abonnement tenu 5 min : episode clos (%u)", p.tries);
    const uint32_t lost = t + 700000;
    p.subscriptions(0, lost);
    CHECK(!p.due(lost + 9900, base) && p.due(lost + 10000, base), "perte d'un abonnement tenu : relance a 10 s");
    p.fired();
    p.finished(0, lost + 11000);  // rien a relancer
    CHECK(!p.due(lost + 11000 + 299900, base) && p.due(lost + 11000 + 300000, base), "rien a relancer : 5 min");
  }
  {
    ResumePlanner p;  // repris puis perdu au bout de 20 s, quatre fois : 30 s, 60 s, 5 min, 5 min
    p.subscriptions(0, 0);
    p.network(true, 0);
    uint32_t t = firstDue(p, 0, 100000, 0);
    CHECK(t == 50000, "premiere tentative a 50 s : %lu", (unsigned long)t);
    const uint32_t gaps[] = {30000, 60000, 300000, 300000};
    for (uint32_t g : gaps) {
      p.fired();
      p.finished(1, t + 2000);  // session ouverte, abonnement repris, pas encore compte
      p.subscriptions(1, t + 3000);
      p.subscriptions(1, t + 21000);
      p.subscriptions(0, t + 23000);
      CHECK(!p.due(t + 23000 + g - 100, 0) && p.due(t + 23000 + g, 0), "perdu apres 20 s : %lu ms",
            (unsigned long)g);
      t += 23000 + g;
    }
    CHECK(p.tries == 4, "episode garde : %u", p.tries);
  }
  {
    ResumePlanner p;  // reseau pret tard : 10 s de stabilisation, pas plus
    p.subscriptions(0, 1000);
    p.network(true, 70000);
    CHECK(firstDue(p, 70000, 100000, 0) == 80000, "pret a 70 s : tentative a 80 s");
    p.fired();
    p.finished(1, 90000);  // echec : 30 s d'attente, jusqu'a 120 s
    // Courte coupure pendant l'attente : ni l'attente ni le compteur ne bougent.
    p.network(false, 100000);
    p.network(true, 105000);
    CHECK(p.tries == 1 && p.hold, "courte coupure : episode garde");
    CHECK(firstDue(p, 105000, 200000, 0) == 120000, "attente de 30 s tenue : %lu",
          (unsigned long)firstDue(p, 105000, 200000, 0));
    p.fired();
    p.finished(1, 160000);  // 2e echec : 60 s
    p.network(false, 170000);
    CHECK(!p.due(400000, 0), "reseau perdu : rien");
    p.network(true, 500000);  // coupure de 330 s : nouvel episode
    CHECK(p.tries == 0 && !p.hold, "longue coupure : nouvel episode");
    p.network(true, 505000);  // meme etat : readySince ne bouge pas
    CHECK(p.readySince == 500000, "releve suivant sans effet");
    CHECK(firstDue(p, 505000, 560000, 0) == 510000, "retrouve a 500 s : tentative a 510 s");
  }
  {
    ResumePlanner p;  // comptage pas encore fait : rien ; abonnements actifs : 5 min
    p.network(true, 0);
    CHECK(!p.due(100000, 0), "abonnements inconnus : rien");
    p.subscriptions(2, 100000);
    CHECK(!p.due(399900, 0) && p.due(400000, 0), "abonnements actifs : coup d'oeil a 5 min seulement");
  }
  {
    ResumePlanner p;  // plancher sauve de 20 s : la pile cherche jusqu'a ~65 s
    p.startDelay(20);
    p.subscriptions(0, 0);
    p.network(true, 0);
    CHECK(firstDue(p, 0, 200000, 0) == 70000, "plancher 20 s : premiere tentative a 70 s : %lu",
          (unsigned long)firstDue(p, 0, 200000, 0));
    p.startDelay(60000);
    CHECK(p.startDelayMs == 650000, "plancher borne a 600 s : %lu", (unsigned long)p.startDelayMs);
  }
  {
    ResumePlanner p;  // des semaines plus tard : les attentes franchies restent acquises
    p.subscriptions(0, 0);
    p.network(true, 0);
    CHECK(p.due(60000, 0), "a 60 s");
    CHECK(p.due(0x90000000u, 0) && p.due(0x10000000u, 0), "apres 24,8 jours puis le retour a zero");
  }
}

// ---------------------------------------------------------------------------
//  Surveillance du BM5602 (halo1_watch.h) : relance sur symptome, limites
// ---------------------------------------------------------------------------

// n trames, une toutes les stepMs a partir de t, dont badPerMille pour mille au
// CRC faux, reparties regulierement ; due() apres chaque trame, comme tick().
// Rend la premiere cause permise (et son instant dans *at), None sinon ; t
// avance d'autant.
static halo1::Relaunch feedRx(halo1::ChipWatch &w, uint32_t &t, uint32_t n, uint32_t stepMs, uint32_t badPerMille,
                              uint32_t *at = nullptr) {
  halo1::Relaunch first = halo1::Relaunch::None;
  uint32_t acc = 0;
  for (uint32_t i = 0; i < n; i++) {
    acc += badPerMille;
    const bool bad = acc >= 1000;
    if (bad) acc -= 1000;
    w.rxFrame(!bad, t);
    const halo1::Relaunch c = w.due(t);
    if (c != halo1::Relaunch::None && first == halo1::Relaunch::None) {
      first = c;
      if (at) *at = t;
    }
    t += stepMs;
  }
  return first;
}

static void timeouts(halo1::ChipWatch &w, unsigned n) {
  for (unsigned i = 0; i < n; i++) w.txVerdict(halo1::TxSeen::Timeout);
}

// Ecoute pendant ms, un tour toutes les stepMs : offPerS rearmements hors RX et
// inPerS periodiques par seconde, repartis regulierement, puis due() comme
// tick(). S'arrete a la premiere cause permise, t sur son instant (la ou le
// pilote relancerait) ; sinon None, t avance de ms.
static halo1::Relaunch listen(halo1::ChipWatch &w, uint32_t &t, uint32_t ms, uint32_t stepMs, uint32_t offPerS,
                              uint32_t inPerS) {
  uint32_t accOff = 0, accIn = 0;
  for (uint32_t e = 0; e < ms; e += stepMs, t += stepMs) {
    accOff += offPerS * stepMs;
    accIn += inPerS * stepMs;
    w.rxRearms(accOff / 1000, accIn / 1000, t);
    accOff %= 1000;
    accIn %= 1000;
    const halo1::Relaunch c = w.due(t);
    if (c != halo1::Relaunch::None) return c;
  }
  return halo1::Relaunch::None;
}

static void testChipWatch() {
  using W = ChipWatch;
  using R = Relaunch;
  using T = TxSeen;

  // Serie de delais : remise a zero par un accuse ou un MAX_RT, pas par une
  // FIFO refusee.
  {
    W w;
    uint32_t t = 1000;
    CHECK(w.due(t) == R::None && !w.failed() && w.symptom() == R::None, "neuve : rien");
    timeouts(w, 2);
    CHECK(w.due(t) == R::None && w.timeoutRun() == 2, "2 delais : rien");
    w.txVerdict(T::Ack);
    CHECK(w.timeoutRun() == 0 && w.due(t) == R::None, "accuse : serie remise a zero");
    timeouts(w, 2);
    w.txVerdict(T::MaxRt);
    timeouts(w, 2);
    CHECK(w.due(t) == R::None && w.timeoutRun() == 2, "MAX_RT coupe la serie");
    w.txVerdict(T::Refused);
    CHECK(w.due(t) == R::None && w.timeoutRun() == 2, "FIFO refusee : neutre");
    w.txVerdict(T::Timeout);
    CHECK(w.due(t) == R::TxTimeout && w.symptom() == R::TxTimeout, "3 delais de suite : relance");
    CHECK(!w.failed() && w.waitMs(t) == 0, "premiere relance : ni panne ni attente");
    timeouts(w, 300);
    CHECK(w.timeoutRun() == 255 && w.due(t) == R::TxTimeout, "serie saturee a 255");
  }

  // MAX_RT seuls, ou meles aux delais sans jamais 3 de suite (lampe
  // debranchee, emission difficile) : jamais de relance, jamais de panne.
  {
    W w;
    uint32_t t = 0, fired = 0;
    for (unsigned i = 0; i < 20000; i++, t += 100) {
      w.txVerdict(T::MaxRt);
      if (w.due(t) != R::None) fired++;
    }
    for (unsigned i = 0; i < 5000; i++, t += 100) {
      timeouts(w, 2);
      w.txVerdict(T::MaxRt);
      if (w.due(t) != R::None) fired++;
    }
    CHECK(!fired && !w.failed() && w.total() == 0 && w.unrecovered() == 0, "MAX_RT : %u relances", (unsigned)fired);
  }

  // Deluge : 100 trames au moins dans la fenetre de 10 s, 90 % de CRC faux au moins.
  {
    W w;
    uint32_t t = 50000;
    CHECK(feedRx(w, t, 99, 50, 1000) == R::None && w.windowFrames() == 99, "99 trames fausses : rien");
    CHECK(feedRx(w, t, 1, 50, 1000) == R::RxNoise && w.noisy(), "100e trame fausse : deluge");
    CHECK(w.lastFlood().frames == 100 && w.lastFlood().bad == 100 && w.lastFlood().ms == 99 * 50,
          "deluge retenu : %u trames, %u fausses, %lu ms", w.lastFlood().frames, w.lastFlood().bad,
          (unsigned long)w.lastFlood().ms);
  }
  {
    W w;
    uint32_t t = 0;
    feedRx(w, t, 10, 20, 0);
    CHECK(feedRx(w, t, 89, 20, 1000) == R::None, "10 justes + 89 fausses : pas encore 100");
    CHECK(feedRx(w, t, 1, 20, 1000) == R::RxNoise, "90 fausses sur 100 : deluge");
  }
  {
    W w;
    uint32_t t = 0;
    feedRx(w, t, 11, 20, 0);
    CHECK(feedRx(w, t, 89, 20, 1000) == R::None, "89 fausses sur 100 : rien");
    CHECK(feedRx(w, t, 20, 20, 0) == R::None && !w.noisy(), "puis des justes : toujours rien");
  }
  {
    // 150 trames fausses en 20 s : 75 par fenetre.
    W w;
    uint32_t t = 7;
    CHECK(feedRx(w, t, 150, 20000 / 150, 1000) == R::None, "150 fausses etalees sur 20 s : rien");
  }
  {
    // Molette de la telecommande et accuses de la lampe, 18 trames par seconde
    // pendant 10 min, un CRC faux toutes les 10 s ; puis la meme chose avec une
    // trame sur deux fausse (brouillage) : jamais de deluge.
    W w;
    uint32_t t = 0;
    CHECK(feedRx(w, t, 18 * 600, 1000 / 18, 5) == R::None, "molette : rien");
    CHECK(feedRx(w, t, 18 * 600, 1000 / 18, 500) == R::None, "molette brouillee a 50 %% : rien");
  }
  {
    // Deluge dense synthetique : ~23 trames par seconde, 99,8 % de CRC faux
    // (le rythme reel des ~96 000 CRC faux de l'incident n'est pas connu).
    W w;
    uint32_t t = 123456, at = 0;
    const uint32_t t0 = t;
    CHECK(feedRx(w, t, 23 * 60, 1000 / 23, 998, &at) == R::RxNoise, "deluge dense : relance");
    CHECK(at - t0 <= 5000, "deluge dense : vu en %lu ms", (unsigned long)(at - t0));
  }
  {
    // L'alerte tombe avec une fenetre calme, ou si plus rien n'arrive.
    W w;
    uint32_t t = 1000;
    w.relaunched(R::Verify, t);  // attente de 60 s : le deluge ne peut pas partir
    feedRx(w, t, 200, 20, 1000);
    CHECK(w.noisy() && w.due(t) == R::None && w.symptom() == R::RxNoise, "deluge retenu pendant l'attente");
    // Fenetre ouverte a 1000 par la relance, deluge de 1000 a 4980.
    feedRx(w, t, 110, 60, 0);  // justes de 5000 a 11540 : la fenetre du deluge se ferme a 11000
    CHECK(w.noisy(), "fenetre du deluge fermee : alerte gardee");
    feedRx(w, t, 170, 60, 0);  // justes jusqu'a 21740 : la fenetre calme se ferme a 21020
    CHECK(!w.noisy() && w.symptom() == R::None, "fenetre calme : alerte levee");
    feedRx(w, t, 600, 20, 1000);
    CHECK(w.noisy(), "nouveau deluge");
    t += 25000;
    CHECK(w.due(t) == R::None && !w.noisy(), "25 s sans trame : alerte levee");
  }

  // Limite d'une relance par minute, puis un essai toutes les 10 min apres 3
  // relances sans guerison ; panne levee par un accuse.
  {
    W w;
    uint32_t t = 5000;
    timeouts(w, 3);
    CHECK(w.due(t) == R::TxTimeout, "relance 1");
    w.relaunched(R::TxTimeout, t);
    const uint32_t t1 = t;
    CHECK(w.timeoutRun() == 0 && w.due(t) == R::None && w.unrecovered() == 1, "preuves effacees par la relance");
    timeouts(w, 3);
    CHECK(w.due(t1 + 1000) == R::None && w.waitMs(t1 + 1000) == 59000, "symptome revenu : attente de 60 s");
    CHECK(w.due(t1 + W::kGapMs - 1) == R::None, "59,999 s : toujours rien");
    CHECK(w.due(t1 + W::kGapMs) == R::TxTimeout && !w.failed(), "60 s : relance 2");
    w.relaunched(R::TxTimeout, t1 + W::kGapMs);
    const uint32_t t2 = t1 + W::kGapMs;
    timeouts(w, 3);
    CHECK(w.due(t2 + W::kGapMs) == R::TxTimeout, "relance 3");
    w.relaunched(R::TxTimeout, t2 + W::kGapMs);
    const uint32_t t3 = t2 + W::kGapMs;
    CHECK(w.unrecovered() == 3 && !w.failed(), "3 relances : pas de panne sans symptome");
    CHECK(w.due(t3 + 5 * W::kGapMs) == R::None && !w.failed(), "sans symptome, jamais de panne");
    timeouts(w, 3);
    CHECK(w.due(t3 + 5 * W::kGapMs) == R::None && w.failed(), "symptome apres 3 relances : EN PANNE");
    CHECK(w.waitMs(t3 + 5 * W::kGapMs) == W::kBackoffMs - 5 * W::kGapMs, "attente de 10 min : %lu",
          (unsigned long)w.waitMs(t3 + 5 * W::kGapMs));
    CHECK(w.due(t3 + W::kBackoffMs - 1) == R::None, "9 min 59,999 s : rien");
    CHECK(w.due(t3 + W::kBackoffMs) == R::TxTimeout && w.failed(), "10 min : relance 4");
    w.relaunched(R::TxTimeout, t3 + W::kBackoffMs);
    const uint32_t t4 = t3 + W::kBackoffMs;
    CHECK(w.failed() && w.unrecovered() == 4, "panne jusqu'a un signe de guerison");
    timeouts(w, 3);
    CHECK(w.due(t4 + W::kGapMs) == R::None, "toujours 10 min");
    w.txVerdict(T::Ack);
    CHECK(!w.failed() && w.unrecovered() == 0 && w.symptom() == R::None, "accuse : guerison");
    CHECK(w.gapMs() == W::kGapMs && w.waitMs(t4 + W::kGapMs) == 0, "guerison : retour a 60 s");
    CHECK(w.count(R::TxTimeout) == 4 && w.total() == 4 && w.count(R::RxNoise) == 0, "compteurs par cause");
    W::Entry h[W::kHistN + 2];
    const uint8_t n = w.history(h, W::kHistN + 2);
    CHECK(n == W::kHistN && h[0].atMs == t4 && h[1].atMs == t3 && h[2].atMs == t2 && h[3].atMs == t1 &&
              h[0].cause == R::TxTimeout,
          "historique : %u entrees", n);
    w.relaunched(R::RxNoise, t4 + W::kGapMs);
    CHECK(w.history(h, W::kHistN) == W::kHistN && h[0].cause == R::RxNoise && h[3].atMs == t2, "historique tournant");
    w.clearCounts();
    CHECK(w.total() == 0 && !w.history(h, W::kHistN) && w.unrecovered() == 1 && w.waitMs(t4 + W::kGapMs) > 0,
          "raz : compteurs seulement");
  }

  // Guerison par l'ecoute : une trame juste dans un deluge ne compte pas, une
  // trame juste dans une fenetre calme, si.
  {
    W w;
    uint32_t t = 0x40000000u;
    for (unsigned i = 0; i < 3; i++) {
      uint32_t at = 0;
      CHECK(feedRx(w, t, 23 * 70, 1000 / 23, 998, &at) == R::RxNoise, "deluge %u", i + 1);
      w.relaunched(R::RxNoise, at);
      t = at + 1;
    }
    feedRx(w, t, 23 * 30, 1000 / 23, 998);  // ~1 trame juste pour 500
    CHECK(w.failed() && w.unrecovered() == 3, "juste dans le deluge : pas de guerison");
    feedRx(w, t, 3, 4000, 0);  // 3 trames justes, puis plus rien
    t += 20000;
    w.due(t);
    CHECK(!w.failed() && w.unrecovered() == 0, "fenetre calme avec une trame juste : guerison");
    CHECK(w.count(R::RxNoise) == 3, "3 relances pour bruit");
  }
  {
    // Fenetre sous le seuil du deluge mais surtout fausse (puce malade qui
    // bruite moins de 10 trames par seconde) : une trame juste n'y guerit pas ;
    // une fenetre majoritairement juste, si.
    W w;
    uint32_t t = 0;
    for (unsigned i = 0; i < 3; i++) {
      w.relaunched(R::TxTimeout, t);
      t += W::kGapMs;
    }
    CHECK(feedRx(w, t, 60, 150, 1000) == R::None, "60 fausses : pas de deluge");
    feedRx(w, t, 1, 150, 0);
    t += 20000;
    w.due(t);
    CHECK(w.unrecovered() == 3 && !w.noisy(), "60 fausses + 1 juste : pas de guerison");
    timeouts(w, 3);
    CHECK(w.due(t) == R::None && w.failed(), "symptome : EN PANNE malgre la trame juste");
    feedRx(w, t, 2, 150, 1000);
    feedRx(w, t, 3, 150, 0);
    t += 20000;
    w.due(t);
    CHECK(!w.failed() && w.unrecovered() == 0, "3 justes sur 5 : guerison");
  }
  {
    // Moitie juste, moitie fausse : pas encore une guerison.
    W w;
    uint32_t t = 0;
    w.relaunched(R::TxTimeout, t);
    feedRx(w, t, 4, 100, 500);
    t += 20000;
    w.due(t);
    CHECK(w.unrecovered() == 1, "2 justes sur 4 : pas de guerison");
  }
  {
    // Fenetre calme SANS trame juste : pas de guerison (le silence ne prouve rien).
    W w;
    uint32_t t = 0;
    for (unsigned i = 0; i < 3; i++) {
      w.relaunched(R::TxTimeout, t);
      t += W::kGapMs;
    }
    timeouts(w, 3);
    w.due(t);
    CHECK(w.failed(), "panne");
    feedRx(w, t, 3, 4000, 1000);
    t += 30000;
    w.due(t);
    CHECK(w.failed(), "3 CRC faux et du silence : toujours en panne");
  }

  // Relance sur verification : comptee, et elle impose l'attente aux symptomes.
  {
    W w;
    uint32_t t = 900;
    w.relaunched(R::Verify, t);
    timeouts(w, 3);
    CHECK(w.due(t + 30000) == R::None && w.due(t + W::kGapMs) == R::TxTimeout, "apres une relance sur verif. : 60 s");
    CHECK(w.count(R::Verify) == 1 && w.unrecovered() == 1, "verif. comptee");
  }

  // Essai L3 (held) : l'attente d'une relance, sans compte ; apres 3
  // relances sans guerison, celle de 10 min.
  {
    W w;
    uint32_t t = 3000;
    w.forget(t);
    w.held(t);
    timeouts(w, 3);
    CHECK(w.due(t + 1000) == R::None && w.waitMs(t + 1000) == W::kGapMs - 1000, "essai L3 : attente de 60 s");
    CHECK(w.total() == 0 && w.unrecovered() == 0 && !w.history(nullptr, 0), "essai L3 : ni compte ni historique");
    CHECK(w.due(t + W::kGapMs) == R::TxTimeout, "60 s apres l'essai L3 : relance");
  }
  {
    W w;
    uint32_t t = 0;
    for (unsigned i = 0; i < 3; i++) {
      w.relaunched(R::TxTimeout, t);
      t += W::kBackoffMs;
    }
    w.held(t);
    timeouts(w, 3);
    CHECK(w.due(t + W::kGapMs) == R::None && w.failed(), "essai L3 en panne : pas de relance a 60 s");
    CHECK(w.due(t + W::kBackoffMs) == R::TxTimeout && w.unrecovered() == 3, "essai L3 en panne : 10 min");
  }

  // Outil de banc : preuves effacees, attente et compteurs gardes.
  {
    W w;
    uint32_t t = 100;
    w.relaunched(R::TxTimeout, t);
    timeouts(w, 3);
    feedRx(w, t, 150, 20, 1000);
    w.forget(t);
    CHECK(w.symptom() == R::None && w.timeoutRun() == 0 && !w.noisy() && w.windowFrames() == 0, "forget : preuves");
    CHECK(w.waitMs(t) > 0 && w.unrecovered() == 1 && w.total() == 1, "forget : limites et compteurs gardes");
  }

  // Retour a zero de millis().
  {
    W w;
    const uint32_t t0 = 0xFFFFF000u;
    w.relaunched(R::TxTimeout, t0);
    timeouts(w, 3);
    CHECK(w.due(0x00000100u) == R::None, "attente a cheval sur le retour a zero");
    CHECK(w.due(t0 + W::kGapMs - 1) == R::None && w.due(t0 + W::kGapMs) == R::TxTimeout,
          "60 s a travers le retour a zero");
  }
  {
    // Une attente echue ne revient pas 49,7 jours plus tard.
    W w;
    const uint32_t t0 = 1000;
    w.relaunched(R::TxTimeout, t0);
    CHECK(w.due(t0 + W::kGapMs) == R::None, "attente echue, sans symptome");
    timeouts(w, 3);
    CHECK(w.due(t0 + 30000) == R::TxTimeout, "49,7 jours plus tard : pas d'attente ranimee");
  }
  {
    // Deluge a cheval sur le retour a zero.
    W w;
    uint32_t t = 0xFFFFF000u, at = 0;
    CHECK(feedRx(w, t, 23 * 20, 1000 / 23, 998, &at) == R::RxNoise && at < 0x00002000u, "deluge a cheval : %08lX",
          (unsigned long)at);
  }

  // --- Ecoute sourde : rearmements sur OMST != RX (phase finale de l'incident) ---

  // Seuil : 1000 dans la fenetre glissante, pas 999.
  {
    W w;
    uint32_t t = 1000;
    w.due(t);
    w.rxRearms(999, 0, t);
    CHECK(w.due(t) == R::None && w.deafRearms() == 999 && w.symptom() == R::None, "999 hors RX : rien");
    w.rxRearms(1, 0, t + 5);
    CHECK(w.due(t + 5) == R::RxDeaf && w.symptom() == R::RxDeaf, "1000 hors RX : relance");
    CHECK(w.lastDeaf().rearms == 1000 && w.lastDeaf().ms == 5, "surdite retenue : %lu en %lu ms",
          (unsigned long)w.lastDeaf().rearms, (unsigned long)w.lastDeaf().ms);
  }
  {
    // Les periodiques ne comptent pas.
    W w;
    uint32_t t = 0;
    w.rxRearms(999, 5000, t);
    CHECK(w.due(t) == R::None && w.deafRearms() == 999, "999 hors RX + 5000 periodiques : rien");
  }
  {
    // 999 puis 1 juste apres la fenetre : les premiers sont sortis.
    W w;
    uint32_t t = 20000;
    w.due(t);
    w.rxRearms(999, 0, t);
    CHECK(w.due(t + W::kDeafWindowMs - 1) == R::None && w.deafRearms() == 999, "9,999 s : toujours 999");
    w.rxRearms(1, 0, t + W::kDeafWindowMs);
    CHECK(w.due(t + W::kDeafWindowMs) == R::None && w.deafRearms() == 1, "10 s plus tard : 1 seul");
  }
  {
    // Etales sur plus de 10 s : 1000 a 91 par seconde (un toutes les 11 ms),
    // puis 99 par seconde pendant 10 min : jamais 1000 en moins de 10 s.
    W w;
    uint32_t t = 3333;
    for (unsigned i = 0; i < 1000; i++, t += 11) {
      w.rxRearms(1, 0, t);
      CHECK(w.due(t) == R::None, "1000 sur 11 s : rien (a %u)", i);
    }
    CHECK(listen(w, t, 600000, 10, 99, 0) == R::None && w.deafRearms() < 1000, "99 par seconde : rien (%lu)",
          (unsigned long)w.deafRearms());
    // 100 par seconde : 1000 en moins de 10 s, relance.
    const uint32_t t0 = t;
    CHECK(listen(w, t, 60000, 10, 100, 0) == R::RxDeaf && t - t0 <= W::kDeafWindowMs,
          "100 par seconde : relance en %lu ms", (unsigned long)(t - t0));
  }
  {
    // Rythme de l'incident (350 et 450 par seconde, un tour toutes les 2 ms,
    // sans periodique), apres 9,5 s d'ecoute normale : la fenetre d'ecoute est
    // presque close et la tranche entamee, la fenetre glissante voit tout de
    // meme la surdite en moins de 3 s.
    const uint32_t rates[] = {350, 450};
    for (uint32_t rate : rates) {
      W w;
      uint32_t t = 0x30000000u;
      w.forget(t);
      CHECK(listen(w, t, 9500, 10, 0, 8) == R::None, "ecoute normale");
      const uint32_t t0 = t;
      CHECK(listen(w, t, 10000, 2, rate, 0) == R::RxDeaf, "%lu par seconde : relance", (unsigned long)rate);
      CHECK(t - t0 <= 3000 && t - t0 >= 2000, "%lu par seconde : vue en %lu ms", (unsigned long)rate,
            (unsigned long)(t - t0));
      CHECK(w.lastDeaf().rearms >= W::kDeafMinRearms && w.lastDeaf().ms >= t - t0 &&
                w.lastDeaf().ms <= t - t0 + W::kDeafSliceMs,
            "%lu par seconde : %lu en %lu ms au plus", (unsigned long)rate, (unsigned long)w.lastDeaf().rearms,
            (unsigned long)w.lastDeaf().ms);
    }
  }
  {
    // Ecoute normale pendant une heure : ~8 periodiques et un hors RX toutes les
    // 5 s, des trames justes, un CRC faux par minute ; jamais rien.
    W w;
    uint32_t t = 0, fired = 0;
    for (unsigned s = 0; s < 3600; s++) {
      if (listen(w, t, 1000, 10, s % 5 == 0 ? 1 : 0, 8) != R::None) fired++;
      w.rxFrame(s % 60 != 0, t);
      if (w.due(t) != R::None) fired++;
    }
    CHECK(!fired && !w.failed() && w.total() == 0 && w.deafRearms() <= 2, "ecoute normale : %u relances, %lu hors RX",
          (unsigned)fired, (unsigned long)w.deafRearms());
  }
  {
    // Lampe debranchee : une commande toutes les 2 s, 5 MAX_RT, puis le retour
    // en ecoute (2 hors RX a la reconfiguration, periodiques normaux) ; 10 min.
    W w;
    uint32_t t = 0x7FFFF000u, fired = 0;
    for (unsigned i = 0; i < 300; i++) {
      for (unsigned k = 0; k < 5; k++) w.txVerdict(T::MaxRt);
      if (w.due(t) != R::None) fired++;
      t += 500;  // la rafale, sans ecoute
      w.rxRearms(2, 0, t);
      if (listen(w, t, 1500, 10, 0, 8) != R::None) fired++;
    }
    CHECK(!fired && !w.failed() && w.total() == 0, "lampe debranchee : %u relances", (unsigned)fired);
  }

  // Avec les autres symptomes : delais avant bruit avant surdite ; une relance
  // efface toutes les preuves.
  {
    W w;
    uint32_t t = 5000;
    w.due(t);
    w.rxRearms(1500, 0, t);
    timeouts(w, 3);
    CHECK(w.due(t) == R::TxTimeout, "delais et surdite : delais d'abord");
    w.relaunched(R::TxTimeout, t);
    CHECK(w.symptom() == R::None && w.deafRearms() == 0 && w.timeoutRun() == 0, "relance : preuves effacees");
  }
  {
    W w;
    uint32_t t = 5000;
    feedRx(w, t, 100, 20, 1000);
    w.rxRearms(1500, 0, t);
    CHECK(w.due(t) == R::RxNoise, "bruit et surdite : bruit d'abord");
    w.relaunched(R::RxNoise, t);
    CHECK(w.symptom() == R::None && w.deafRearms() == 0 && !w.noisy(), "relance : preuves effacees");
  }
  {
    // Outil de banc : la surdite repart de zero.
    W w;
    uint32_t t = 100;
    w.due(t);
    w.rxRearms(900, 0, t);
    w.forget(t);
    w.rxRearms(200, 0, t + 10);
    CHECK(w.due(t + 10) == R::None && w.deafRearms() == 200, "forget : surdite effacee");
  }

  // Limite partagee : une relance pour delais impose ses 60 s a la surdite, et
  // inversement.
  {
    W w;
    uint32_t t = 7000;
    timeouts(w, 3);
    CHECK(w.due(t) == R::TxTimeout, "relance pour delais");
    w.relaunched(R::TxTimeout, t);
    const uint32_t t1 = t;
    CHECK(listen(w, t, 120000, 2, 450, 0) == R::RxDeaf && t == t1 + W::kGapMs,
          "surdite juste apres : relance a 60 s (%lu ms)", (unsigned long)(t - t1));
    w.relaunched(R::RxDeaf, t);
    const uint32_t t2 = t;
    timeouts(w, 3);
    CHECK(w.due(t2 + 1000) == R::None && w.due(t2 + W::kGapMs) == R::TxTimeout, "delais apres la surdite : 60 s");
    CHECK(w.count(R::RxDeaf) == 1 && w.count(R::TxTimeout) == 1 && w.total() == 2, "compteurs par cause");
    W::Entry h[1];
    CHECK(w.history(h, 1) == 1 && h[0].cause == R::RxDeaf && h[0].atMs == t2, "historique : sourde");
  }

  // EN PANNE sur surdite, puis guerison par une ecoute revenue en RX.
  {
    W w;
    uint32_t t = 0x20000000u;
    w.forget(t);
    for (unsigned i = 0; i < 3; i++) {
      CHECK(listen(w, t, W::kBackoffMs, 2, 450, 0) == R::RxDeaf, "surdite %u", i + 1);
      w.relaunched(R::RxDeaf, t);
    }
    CHECK(listen(w, t, 10000, 2, 450, 0) == R::None && w.failed() && w.unrecovered() == 3,
          "sourde apres 3 relances : EN PANNE");
    const uint32_t last = t - 10000;  // 3e relance
    CHECK(w.waitMs(t) == W::kBackoffMs - (t - last), "attente de 10 min");
    CHECK(listen(w, t, W::kBackoffMs, 2, 450, 0) == R::RxDeaf && t == last + W::kBackoffMs && w.failed(),
          "10 min : relance 4");
    w.relaunched(R::RxDeaf, t);
    // La relance guerit : piece calme, aucune trame, la puce dit RX.
    CHECK(listen(w, t, W::kNoiseWindowMs - 10, 10, 0, 8) == R::None && w.failed(), "9,99 s : toujours en panne");
    CHECK(listen(w, t, 20, 10, 0, 8) == R::None && !w.failed() && w.unrecovered() == 0,
          "fenetre de 10 s en RX : guerison");
    CHECK(w.gapMs() == W::kGapMs && w.count(R::RxDeaf) == 4, "retour a 60 s");
  }
  {
    // Bornes de la guerison apres surdite : 10 hors RX au plus, 20 periodiques
    // au moins, aucun CRC faux ; sans ecoute (diag), rien.
    struct Case { uint32_t off, in; bool bad, heals; const char *what; };
    const Case cases[] = {
        {10, 20, false, true, "10 hors RX, 20 periodiques"},
        {11, 80, false, false, "11 hors RX"},
        {0, 19, false, false, "19 periodiques"},
        {0, 80, true, false, "un CRC faux"},
        {0, 0, false, false, "sans ecoute"},
    };
    for (const Case &k : cases) {
      W w;
      uint32_t t = 1000;
      w.relaunched(R::RxDeaf, t);
      w.rxRearms(k.off, k.in, t + 500);
      if (k.bad) w.rxFrame(false, t + 600);
      w.due(t + W::kNoiseWindowMs);
      CHECK((w.unrecovered() == 0) == k.heals, "guerison apres surdite, %s : %u", k.what, (unsigned)w.unrecovered());
    }
  }
  {
    // Apres une autre cause, la meme ecoute calme ne guerit pas : les delais
    // gardent leur limite.
    const R others[] = {R::TxTimeout, R::RxNoise, R::Verify};
    for (R c : others) {
      W w;
      uint32_t t = 1000;
      w.relaunched(c, t);
      listen(w, t, 30000, 10, 0, 8);
      CHECK(w.unrecovered() == 1, "ecoute calme apres %s : pas de guerison", relaunchText(c));
    }
    // Surdite puis delais 60 s plus tard : la derniere cause decide.
    W w;
    uint32_t t = 1000;
    w.relaunched(R::RxDeaf, t);
    t += W::kGapMs;
    timeouts(w, 3);
    CHECK(w.due(t) == R::TxTimeout, "delais apres la surdite");
    w.relaunched(R::TxTimeout, t);
    listen(w, t, 30000, 10, 0, 8);
    CHECK(w.unrecovered() == 2, "ecoute calme apres surdite puis delais : pas de guerison (%u)",
          (unsigned)w.unrecovered());
  }
  {
    // Apres une relance pour surdite, l'emission doit aussi aller : un delai
    // dans la fenetre (meme suivi d'un MAX_RT) l'empeche, la fenetre suivante
    // sans delai guerit ; une serie de delais pas encore close par un accuse
    // ou un MAX_RT l'empeche aussi, fenetre apres fenetre.
    W w;
    uint32_t t = 1000;
    w.relaunched(R::RxDeaf, t);
    listen(w, t, 5000, 10, 0, 8);
    w.txVerdict(T::Timeout);
    w.txVerdict(T::MaxRt);
    listen(w, t, 5010, 10, 0, 8);  // la fenetre ferme a 10 s
    CHECK(w.unrecovered() == 1, "delai puis MAX_RT dans la fenetre : pas de guerison");
    listen(w, t, W::kNoiseWindowMs, 10, 0, 8);
    CHECK(w.unrecovered() == 0, "fenetre suivante sans delai : guerison");

    W v;
    t = 1000;
    v.relaunched(R::RxDeaf, t);
    v.txVerdict(T::Timeout);
    listen(v, t, 30000, 10, 0, 8);
    CHECK(v.unrecovered() == 1 && v.timeoutRun() == 1, "un delai en serie, 3 fenetres calmes : pas de guerison");
    v.txVerdict(T::MaxRt);
    listen(v, t, W::kNoiseWindowMs, 10, 0, 8);
    CHECK(v.unrecovered() == 0, "MAX_RT, la puce emet : guerison");
  }
  {
    // Relance 4 d'un module EN PANNE pour surdite : l'ecoute revient en RX mais
    // chaque envoi reste en delai (l'incident montrait les deux). EN PANNE et
    // les 10 min tiennent ; la relance suivante vient pour delais, a 10 min.
    W w;
    uint32_t t = 0x40000000u;
    w.forget(t);
    for (unsigned i = 0; i < 3; i++) {
      CHECK(listen(w, t, W::kBackoffMs, 2, 450, 0) == R::RxDeaf, "surdite %u", i + 1);
      w.relaunched(R::RxDeaf, t);
    }
    const uint32_t t3 = t;
    CHECK(listen(w, t, W::kBackoffMs + 10, 2, 450, 0) == R::RxDeaf && t == t3 + W::kBackoffMs && w.failed(),
          "EN PANNE, relance 4 a 10 min");
    w.relaunched(R::RxDeaf, t);
    const uint32_t t4 = t;
    // Une commande toutes les 2 s, 3 delais chacune, ecoute calme entre elles.
    for (unsigned i = 0; i < 15; i++) {
      timeouts(w, 3);
      CHECK(listen(w, t, 2000, 10, 0, 8) == R::None, "delais tenus par l'attente (%u)", i);
    }
    CHECK(w.failed() && w.unrecovered() == 4 && w.symptom() == R::TxTimeout && w.gapMs() == W::kBackoffMs,
          "ecoute en RX, envois en delai : toujours EN PANNE (%u)", (unsigned)w.unrecovered());
    CHECK(w.waitMs(t) == W::kBackoffMs - (t - t4), "attente de 10 min gardee");
    CHECK(w.due(t4 + W::kBackoffMs - 1) == R::None && w.due(t4 + W::kBackoffMs) == R::TxTimeout,
          "relance 5 pour delais a 10 min");
  }
  {
    // Reglage de l'ecoute qui laisse voir la guerison en piece calme (verifie
    // par 'lampe rx', et par static_assert dans halo1_lamp.cpp pour config.h).
    CHECK(W::calmVisible(100, 500), "reglage d'origine 100/500");
    CHECK(W::calmVisible(10, 100) && W::calmVisible(200, 401) && W::calmVisible(100, 5000) &&
              W::calmVisible(200, 60000),
          "reglages permis");
    CHECK(!W::calmVisible(200, 400) && !W::calmVisible(250, 500) && !W::calmVisible(201, 60000) &&
              !W::calmVisible(1000, 500) && !W::calmVisible(5000, 60000),
          "reglages refuses");
  }
  {
    // Surdite a cheval sur le retour a zero de millis().
    W w;
    uint32_t t = 0xFFFFF800u;
    w.forget(t);
    CHECK(listen(w, t, 10000, 2, 450, 0) == R::RxDeaf && t < 0x00001000u, "surdite a cheval : %08lX",
          (unsigned long)t);
  }

  CHECK(!strcmp(relaunchText(R::TxTimeout), "delais") && !strcmp(relaunchText(R::RxNoise), "bruit") &&
            !strcmp(relaunchText(R::Verify), "verif.") && !strcmp(relaunchText(R::RxDeaf), "sourde") &&
            !strcmp(relaunchText(R::None), "-"),
        "textes des causes");
}

// ---------------------------------------------------------------------------
//  LED d'etat (status_led.h) : motifs, priorites, intensite, rythme d'ecriture
// ---------------------------------------------------------------------------

static bool rgbIs(statusled::Rgb c, unsigned r, unsigned g, unsigned b) { return c.r == r && c.g == g && c.b == b; }
static bool dark(statusled::Rgb c) { return rgbIs(c, 0, 0, 0); }

// Changements de couleur vus par la LED sur [from, to[ en tours de 1 ms : ce
// que statusLedPoll() ecrirait (elle n'ecrit que les changements).
static unsigned writesOver(statusled::Logic &l, uint32_t from, uint32_t to) {
  unsigned n = 0;
  statusled::Rgb last = l.frame(from).c;
  for (uint32_t t = from + 1; t != to; t++) {
    const statusled::Rgb c = l.frame(t).c;
    if (c != last) n++;
    last = c;
  }
  return n;
}

static void testStatusLed() {
  using namespace statusled;
  using P = Pattern;

  // Motifs de l'etat du reseau.
  CHECK(rgbIs(render(P::Unpaired, 0), 0, 0, kMax), "bleu au depart");
  CHECK(dark(render(P::Unpaired, kUnpairedHalfMs)), "bleu eteint a la demi-periode");
  CHECK(rgbIs(render(P::Unpaired, 2 * kUnpairedHalfMs + 10), 0, 0, kMax), "bleu rallume");
  const Rgb orange = render(P::Offline, kOfflineHalfMs - 1);
  CHECK(orange.r == kMax && orange.g > 0 && orange.g < kMax / 2 && orange.b == 0, "orange %u %u %u", orange.r,
        orange.g, orange.b);
  CHECK(dark(render(P::Offline, kOfflineHalfMs)) && !dark(render(P::Offline, 2 * kOfflineHalfMs)), "orange lent");
  CHECK(dark(render(P::Online, 0)), "lueur : part du noir");
  CHECK(rgbIs(render(P::Online, kGlowMs / 2), kGlowMax, kGlowMax, kGlowMax), "lueur : sommet a 8");
  CHECK(dark(render(P::Online, kGlowMs)) && dark(render(P::Online, kGlowPeriodMs - 1)), "eteinte entre deux lueurs");
  CHECK(rgbIs(render(P::Online, kGlowPeriodMs + kGlowMs / 2), kGlowMax, kGlowMax, kGlowMax), "lueur suivante a 10 s");

  // Evenements bornes : vert 150 ms, rouge trois fois, puis noir.
  CHECK(rgbIs(render(P::Delivered, 0), 0, kMax, 0) && rgbIs(render(P::Delivered, kDeliveredMs - 1), 0, kMax, 0),
        "eclat vert");
  CHECK(dark(render(P::Delivered, kDeliveredMs)), "vert fini");
  unsigned blinks = 0;
  bool was = false;
  for (uint32_t t = 0; t < 3 * kUnreachableMs; t++) {
    const Rgb c = render(P::Unreachable, t);
    CHECK(dark(c) || rgbIs(c, kMax, 0, 0), "rouge seulement, t %u", (unsigned)t);
    if (!dark(c) && !was) blinks++;
    was = !dark(c);
  }
  CHECK(blinks == kRedBlinks, "%u clignements rouges au lieu de 3", blinks);

  // Arc-en-ciel : intensite constante, un tour en kRainbowMs, par pas de 40 ms.
  unsigned distinct = 0;
  Rgb prev = render(P::Identify, 0);
  bool sawR = false, sawG = false, sawB = false;
  for (uint32_t t = 0; t < kRainbowMs; t++) {
    const Rgb c = render(P::Identify, t);
    CHECK(c.r + c.g + c.b == kMax, "arc-en-ciel t %u : %u %u %u", (unsigned)t, c.r, c.g, c.b);
    if (t % kStepMs) CHECK(c == prev, "arc-en-ciel change hors d'un pas, t %u", (unsigned)t);
    if (c != prev) distinct++;
    sawR |= c.r == kMax;
    sawG |= c.g == kMax;
    sawB |= c.b == kMax;
    prev = c;
  }
  CHECK(distinct >= 40 && sawR && sawG && sawB, "arc-en-ciel : %u couleurs, R%u V%u B%u", distinct, sawR, sawG, sawB);
  CHECK(render(P::Identify, kRainbowMs) == render(P::Identify, 0), "un tour en 2 s");

  // Bouton BOOT tenu 8 s : rouge, noir, violet, noir, 100 ms chacun, sans fin.
  for (uint32_t t = 0; t < 5000; t++) {
    const Rgb c = render(P::ButtonUnpair, t);
    switch ((t / kUnpairStepMs) % 4) {
      case 0: CHECK(rgbIs(c, kMax, 0, 0), "bouton tenu : rouge, t %u", (unsigned)t); break;
      case 2: CHECK(c.b == kMax && c.r > 0 && c.r < kMax && c.g == 0, "bouton tenu : violet, t %u", (unsigned)t); break;
      default: CHECK(dark(c), "bouton tenu : noir, t %u", (unsigned)t); break;
    }
    CHECK(renderMono(P::ButtonUnpair, t) == ((t / kUnpairMonoHalfMs) % 2 == 0), "LED simple : bouton tenu, t %u",
          (unsigned)t);
  }
  // Violet : different de toutes les couleurs des autres motifs.
  {
    const Rgb violet = render(P::ButtonUnpair, 2 * kUnpairStepMs);
    const P others[] = {P::Unreachable, P::RadioFault, P::Delivered, P::Unpaired, P::Offline, P::Online};
    for (P p : others)
      for (uint32_t t = 0; t < 25000; t += 7) CHECK(render(p, t) != violet, "violet dans %s", patternName(p));
    for (uint32_t t = 0; t < kRainbowMs; t++)
      CHECK(render(P::Identify, t) != violet, "violet dans l'arc-en-ciel, t %u", (unsigned)t);
  }
  // Appui court : eclat blanc de 150 ms, puis noir jusqu'au redemarrage.
  CHECK(rgbIs(render(P::ButtonReboot, 0), kMax, kMax, kMax) &&
            rgbIs(render(P::ButtonReboot, kRebootFlashMs - 1), kMax, kMax, kMax),
        "eclat blanc");
  CHECK(dark(render(P::ButtonReboot, kRebootFlashMs)) && dark(render(P::ButtonReboot, 60000)), "eclat blanc fini");
  CHECK(renderMono(P::ButtonReboot, 0) && !renderMono(P::ButtonReboot, kRebootFlashMs), "LED simple : eclat");
  CHECK(bootbtn::kRebootDelayMs >= kRebootFlashMs + 50, "redemarrage avant la fin de l'eclat");

  // Phase du bouton BOOT -> LED (statusLedPoll) : rouge/violet des l'armement
  // et jusqu'au desappairage, eclat blanc pour le redemarrage, rien sinon.
  {
    using bootbtn::Phase;
    const struct {
      Phase ph;
      Button b;
    } map[] = {{Phase::Idle, Button::None},     {Phase::Held, Button::None},     {Phase::Armed, Button::Unpair},
               {Phase::Reboot, Button::Reboot}, {Phase::Unpair, Button::Unpair}, {Phase::Locked, Button::None}};
    for (const auto &m : map)
      CHECK(buttonFor(m.ph) == m.b, "LED pour la phase %s : %u", bootbtn::phaseName(m.ph), (unsigned)buttonFor(m.ph));
    // Et de bout en bout : la phase choisit le motif, qui passe sur 'led test'
    // et sur le rouge fixe, pas sur Identify.
    Logic l;
    l.setNet(Net::Online, 0);
    l.setFault(true, 0);
    l.startTest(0);
    l.setButton(buttonFor(Phase::Armed), 10);
    CHECK(l.frame(20).p == P::ButtonUnpair, "phase armee : %s", patternName(l.frame(20).p));
    // Relache apres l'armement (Armed -> Unpair) : le motif continue sans
    // repartir du debut (a +110 ms du depart : noir ; repris a 30 : rouge).
    l.setButton(buttonFor(Phase::Unpair), 30);
    CHECK(l.frame(40).p == P::ButtonUnpair && dark(l.frame(120).c), "desappairage : meme motif, meme depart");
    l.setButton(buttonFor(Phase::Held), 130);
    CHECK(l.frame(140).p != P::ButtonUnpair && l.frame(140).p != P::ButtonReboot, "tenu : pas de motif du bouton");
    l.setButton(buttonFor(Phase::Reboot), 150);
    CHECK(l.frame(150).p == P::ButtonReboot && rgbIs(l.frame(150).c, kMax, kMax, kMax), "redemarrage : eclat blanc");
    l.setIdentify(true, 160);
    CHECK(l.frame(170).p == P::Identify, "Identify passe devant le bouton");
    l.setIdentify(false, 180);
    l.setButton(buttonFor(Phase::Locked), 180);
    CHECK(l.frame(190).p != P::ButtonUnpair && l.frame(190).p != P::ButtonReboot, "tenu au demarrage : rien");
  }

  // Intensite : jamais plus de 24 par canal, 8 pour la lueur.
  const P all[] = {P::Identify,   P::ButtonUnpair, P::ButtonReboot, P::Unreachable, P::RadioFault,
                   P::Delivered,  P::Unpaired,     P::Offline,      P::Online};
  for (P p : all) {
    const unsigned cap = p == P::Online ? kGlowMax : kMax;
    for (uint32_t t = 0; t < 25000; t += 7) {
      const Rgb c = render(p, t);
      CHECK(c.r <= cap && c.g <= cap && c.b <= cap, "%s t %u : %u %u %u", patternName(p), (unsigned)t, c.r, c.g, c.b);
    }
    CHECK(patternName(p) && *patternName(p), "nom du motif %u", (unsigned)p);
  }
  // Codes du protocole JSON (7.9), dans l'ordre des motifs : tous distincts.
  static const char *const kCodes[] = {"identification", "desappairage", "redemarrage",
                                       "injoignable",    "panne_radio",  "livree",
                                       "non_appaire",    "hors_reseau",  "operationnel"};
  static_assert(sizeof(kCodes) / sizeof(kCodes[0]) == sizeof(all) / sizeof(all[0]), "un code par motif");
  for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    CHECK(!strcmp(patternCode(all[i]), kCodes[i]), "code du motif %u : %s", i, patternCode(all[i]));
    for (unsigned j = 0; j < i; j++) CHECK(strcmp(kCodes[i], kCodes[j]), "codes %u et %u egaux", i, j);
  }

  // LED simple : pas de lueur, Identify clignote vite, le reste suit la couleur.
  for (uint32_t t = 0; t < 25000; t += 13) {
    CHECK(!renderMono(P::Online, t), "LED simple allumee en ligne, t %u", (unsigned)t);
    CHECK(renderMono(P::Unpaired, t) == !dark(render(P::Unpaired, t)), "LED simple, bleu t %u", (unsigned)t);
  }
  CHECK(renderMono(P::Identify, 0) && !renderMono(P::Identify, kIdentifyMonoHalfMs), "LED simple : Identify");

  // Module radio en panne : rouge fixe, sans fin (LED simple : allumee).
  for (uint32_t t = 0; t < 700000; t += 997)
    CHECK(rgbIs(render(P::RadioFault, t), kMax, 0, 0) && renderMono(P::RadioFault, t), "rouge fixe, t %u", (unsigned)t);

  // Priorites : Identify > rouge > vert > reseau.
  {
    Logic l;
    const uint32_t t0 = 5000;
    l.setNet(Net::Online, t0);
    CHECK(l.frame(t0).p == P::Online, "en ligne");
    l.delivered(t0 + 100);
    CHECK(l.frame(t0 + 100).p == P::Delivered, "vert par-dessus le reseau");
    l.unreachable(t0 + 120);
    CHECK(l.frame(t0 + 120).p == P::Unreachable, "rouge par-dessus le vert");
    l.setIdentify(true, t0 + 130);
    CHECK(l.frame(t0 + 130).p == P::Identify, "Identify par-dessus tout");
    CHECK(l.frame(t0 + 130).c == render(P::Identify, 0), "arc-en-ciel depuis son debut");
    l.setIdentify(true, t0 + 500);  // deja en cours : la roue ne repart pas
    CHECK(l.frame(t0 + 500).c == render(P::Identify, 370), "Identify continu");
    l.setIdentify(false, t0 + 600);
    CHECK(l.frame(t0 + 600).p == P::Unreachable, "rouge restant apres Identify");
    CHECK(l.frame(t0 + 120 + kUnreachableMs).p == P::Online, "rouge fini, vert deja fini : reseau");
    l.delivered(t0 + 2000);
    CHECK(l.frame(t0 + 2000).p == P::Delivered && l.frame(t0 + 2000 + kDeliveredMs).p == P::Online, "vert seul");
    l.unreachable(t0 + 3000);
    l.unreachable(t0 + 3900);  // nouvel abandon : trois clignements de plus
    CHECK(l.frame(t0 + 3000 + kUnreachableMs).p == P::Unreachable, "rouge relance");
    CHECK(l.frame(t0 + 3900 + kUnreachableMs).p == P::Online, "rouge relance fini");
  }

  // Panne du module radio : au-dessus du vert et du reseau, sous le rouge x3
  // (dont les noirs restent visibles) et Identify ; jusqu'a sa levee.
  {
    Logic l;
    const uint32_t t0 = 40000;
    l.setNet(Net::Unpaired, t0);
    l.setFault(true, t0 + 10);
    CHECK(l.frame(t0 + 10).p == P::RadioFault && rgbIs(l.frame(t0 + 10).c, kMax, 0, 0), "rouge fixe sur le bleu");
    CHECK(l.frame(t0 + 3600000).p == P::RadioFault, "rouge fixe une heure plus tard");
    l.delivered(t0 + 100);
    CHECK(l.frame(t0 + 100).p == P::RadioFault, "rouge fixe par-dessus le vert");
    l.unreachable(t0 + 200);
    CHECK(l.frame(t0 + 200).p == P::Unreachable && dark(l.frame(t0 + 200 + kRedHalfMs).c), "rouge x3 : noirs visibles");
    CHECK(l.frame(t0 + 200 + kUnreachableMs).p == P::RadioFault, "rouge x3 fini : rouge fixe");
    l.setIdentify(true, t0 + 5000);
    CHECK(l.frame(t0 + 5000).p == P::Identify, "Identify par-dessus le rouge fixe");
    l.setIdentify(false, t0 + 6000);
    l.setFault(true, t0 + 7000);  // deja en panne : rien ne change
    CHECK(l.frame(t0 + 7000).p == P::RadioFault, "panne continue");
    l.setFault(false, t0 + 8000);
    CHECK(l.frame(t0 + 8000).p == P::Unpaired, "panne levee : retour au reseau");
  }

  // Bouton BOOT : juste sous Identify, au-dessus de tout le reste (test
  // compris) ; son motif repart du debut a chaque changement de phase.
  {
    Logic l;
    const uint32_t t0 = 70000;
    l.setNet(Net::Online, t0);
    l.setFault(true, t0);
    l.unreachable(t0 + 10);
    l.delivered(t0 + 10);
    l.startTest(t0 + 10);
    l.setButton(Button::Unpair, t0 + 20);
    CHECK(l.frame(t0 + 20).p == P::ButtonUnpair && rgbIs(l.frame(t0 + 20).c, kMax, 0, 0),
          "bouton tenu 8 s par-dessus rouge, vert, panne et test, depuis son debut");
    l.setButton(Button::Unpair, t0 + 250);  // meme phase : le motif continue
    CHECK(l.frame(t0 + 250).c == render(P::ButtonUnpair, 230), "bouton tenu : motif continu");
    l.setIdentify(true, t0 + 300);
    CHECK(l.frame(t0 + 300).p == P::Identify, "Identify par-dessus le bouton");
    l.setIdentify(false, t0 + 400);
    CHECK(l.frame(t0 + 400).p == P::ButtonUnpair, "bouton apres Identify");
    l.setButton(Button::None, t0 + 500);
    CHECK(l.testing() && l.frame(t0 + 500).p == kTest[0].p, "bouton relache : le test continue");
    l.stopTest();
    CHECK(l.frame(t0 + 500).p == P::Unreachable, "test arrete : rouge x3 restant");
    l.setButton(Button::Reboot, t0 + 600);
    CHECK(l.frame(t0 + 600).p == P::ButtonReboot && rgbIs(l.frame(t0 + 600).c, kMax, kMax, kMax),
          "appui court : blanc des le relachement");
    CHECK(l.frame(t0 + 600 + kRebootFlashMs).p == P::ButtonReboot && dark(l.frame(t0 + 600 + kRebootFlashMs).c),
          "appui court : noir avant le redemarrage");
    l.setButton(Button::None, t0 + 900);
    l.setButton(Button::Reboot, t0 + 1000);  // nouvel appui court : nouvel eclat
    CHECK(rgbIs(l.frame(t0 + 1000).c, kMax, kMax, kMax), "nouvel eclat");
    l.setButton(Button::None, t0 + 1100);
    CHECK(l.frame(t0 + 5000).p == P::RadioFault, "bouton fini : panne");
  }

  // Phase du reseau : repart a chaque changement, pas quand l'etat se repete.
  {
    Logic l;
    l.setNet(Net::Offline, 1000);
    l.setNet(Net::Unpaired, 7000);
    CHECK(l.frame(7000).p == P::Unpaired && rgbIs(l.frame(7000).c, 0, 0, kMax), "bleu des le changement");
    l.setNet(Net::Unpaired, 7100);
    CHECK(dark(l.frame(7000 + kUnpairedHalfMs).c), "meme etat : phase gardee");
    l.setNet(Net::Online, 9000);
    CHECK(rgbIs(l.frame(9000 + kGlowMs / 2).c, kGlowMax, kGlowMax, kGlowMax), "lueur au passage en ligne");
  }

  // 'led test' : chaque motif a tour de role, puis retour a la normale.
  {
    Logic l;
    l.setNet(Net::Online, 0);
    const uint32_t t0 = 20000;
    l.startTest(t0);
    uint32_t at = t0, total = 0;
    for (const TestStep &s : kTest) {
      CHECK(s.ms >= 1000, "pas de test trop court");
      CHECK(l.frame(at).p == s.p && l.frame(at + s.ms - 1).p == s.p, "test : %s", patternName(s.p));
      CHECK(l.frame(at).c == render(s.p, 0), "test : %s depuis son debut", patternName(s.p));
      // Chaque pas finit sur du noir, sauf les motifs sans fin qui precedent un
      // noir ou un bleu (la lueur finit noire, le rouge fixe precede le vert).
      if (s.p == P::ButtonReboot || s.p == P::ButtonUnpair || s.p == P::Delivered || s.p == P::Unreachable)
        CHECK(dark(l.frame(at + s.ms - 1).c), "test : %s finit sur du noir", patternName(s.p));
      at += s.ms;
      total += s.ms;
    }
    CHECK(total == testTotalMs(), "duree du test");
    CHECK(total == 21000, "duree du test : %u ms (README : 21 s)", (unsigned)total);
    CHECK(l.testing() && l.frame(at).p == P::Online && !l.testing(), "fin du test");
    l.startTest(at + 10);
    l.setIdentify(true, at + 20);
    CHECK(l.frame(at + 20).p == P::Identify, "Identify pendant le test");
    l.setIdentify(false, at + 30);
    l.stopTest();
    CHECK(l.frame(at + 30).p == P::Online && !l.testing(), "led stop");
  }

  // Retour a zero de millis() : un evenement court le franchit, et un echu ne
  // revient pas 49,7 jours plus tard.
  {
    Logic l;
    l.setNet(Net::Online, 0xFFFFF000u);
    l.delivered(0xFFFFFFF0u);
    CHECK(l.frame(0x00000010u).p == P::Delivered, "vert a cheval sur le retour a zero");
    CHECK(l.frame(0x00000200u).p == P::Online, "vert fini apres le retour a zero");
    CHECK(l.frame(0xFFFFFFF0u + 20).p == P::Online, "vert echu ne revient pas");
  }

  // Rythme d'ecriture : seulement les changements de couleur, jamais a chaque
  // tour de loop() (1 kHz).
  {
    Logic l;
    l.setNet(Net::Online, 0);
    const unsigned online = writesOver(l, 0, 20000);
    CHECK(online >= 4 && online <= 2 * 2 * kGlowMax + 2, "en ligne : %u ecritures en 20 s", online);
    l.setNet(Net::Unpaired, 20000);
    const unsigned blue = writesOver(l, 20000, 22000);
    CHECK(blue == 2000 / kUnpairedHalfMs - 1, "bleu : %u ecritures en 2 s", blue);
    l.setIdentify(true, 30000);
    const unsigned rainbow = writesOver(l, 30000, 30000 + kRainbowMs);
    CHECK(rainbow <= kRainbowMs / kStepMs, "arc-en-ciel : %u ecritures en 2 s", rainbow);
  }

  // Identify par TriggerEffect : fin datee par endpoint (matter_bridge.cpp).
  {
    CHECK(!effectPending(0, 0) && !effectPending(0, 0x80000000u) && !effectPending(0, 0xFFFFFFFFu), "0 : aucun effet");
    CHECK(effectPending(5, 0xFFFFFFF0u), "fin juste apres le retour a zero de millis()");
    CHECK(!effectPending(5, 5) && !effectPending(5, 6), "fin atteinte");
    CHECK(effectPending(5, 4), "1 ms avant la fin");

    const uint32_t now = 1000;
    CHECK(effectEnd(0, kEffectBlink, now) == now + 2000 && effectEnd(0, kEffectOkay, now) == now + 2000,
          "Blink, Okay : 2 s");
    CHECK(effectEnd(0, kEffectBreathe, now) == now + 15000, "Breathe : 15 s");
    CHECK(effectEnd(0, kEffectChannelChange, now) == now + 8000, "ChannelChange : 8 s");
    CHECK(effectEnd(0, 0x42, now) == now + 2000, "effet inconnu : 2 s");
    CHECK(effectEnd(now + 500, kEffectBreathe, now) == now + 15000, "nouvel effet : remplace le precedent");
    CHECK(effectEnd(now + 9000, kEffectStop, now) == 0 && effectEnd(0, kEffectStop, now) == 0, "Stop : fin immediate");
    CHECK(effectEnd(now + 9000, kEffectFinish, now) == now + kEffectFinishMs, "Finish : cycle en cours acheve");
    CHECK(effectEnd(now + 300, kEffectFinish, now) == now + 300, "Finish : un effet presque fini garde sa fin");
    CHECK(effectEnd(0, kEffectFinish, now) == 0, "Finish sans effet : rien ne s'allume");
    CHECK(effectEnd(now - 10, kEffectFinish, now) == 0, "Finish apres un effet echu : rien ne s'allume");

    // A cheval sur le retour a zero, et jamais 0 pour un effet en cours.
    const uint32_t late = 0xFFFFF000u, end = effectEnd(0, kEffectBreathe, late);
    CHECK(effectPending(end, late) && effectPending(end, late + 14999) && !effectPending(end, late + 15000),
          "Breathe a cheval sur le retour a zero");
    CHECK(effectEnd(end, kEffectFinish, late + 100) == late + 100 + kEffectFinishMs, "Finish a cheval");
    CHECK(effectEnd(0, kEffectBlink, 0u - 2000) == 1 && effectPending(1, 0u - 2000), "fin a 0 : decalee a 1");
    CHECK(effectEnd(0x100u, kEffectFinish, 0u - kEffectFinishMs) == 1, "Finish a 0 : decalee a 1");
  }
}

// ---------------------------------------------------------------------------
//  Bouton BOOT (boot_button.h) : seuils, anti-rebond, garde de la broche de
//  strapping, tenu au demarrage, trous de releves, retour a zero de millis()
// ---------------------------------------------------------------------------

namespace {

// Releves simules de la broche : un appel a update() toutes les 'step' ms.
struct Btn {
  bootbtn::Machine m;
  uint32_t now;
  bootbtn::Event ev[32];
  uint32_t at[32];
  unsigned n = 0;
  explicit Btn(uint32_t t0, bool lowAtBoot = false) : now(t0) {
    record(m.update(lowAtBoot, now));  // premier releve : begin()
    now++;
  }
  void record(bootbtn::Event e) {
    if (e == bootbtn::Event::None || n >= 32) return;
    ev[n] = e;
    at[n] = now;
    n++;
  }
  // 'ms' ms au niveau 'low' : releves a now, now + step... (step divise ms).
  void level(bool low, uint32_t ms, uint32_t step = 1) {
    for (uint32_t e = 0; e < ms; e += step) {
      record(m.update(low, now));
      now += step;
    }
  }
  void skip(uint32_t ms) { now += ms; }  // loop() bloquee : aucun releve
  bool only(bootbtn::Event e) const { return n == 1 && ev[0] == e; }
  void clear() { n = 0; }
};

}  // namespace

static void testBootButton() {
  using namespace bootbtn;

  // Appui court : relache a 1999 ms -> redemarrage, 250 ms apres le
  // relachement confirme (30 ms apres le premier releve haut). Aussi a cheval
  // sur le retour a zero de millis().
  for (uint32_t t0 : {1000u, 0xFFFFFFFFu - 1500u, 0xFFFFFFFFu - 1u}) {
    Btn b(t0);
    b.level(false, 500);
    b.level(true, 1999);
    CHECK(b.m.phase() == Phase::Held && b.n == 0, "court : tenu, t0 %08X", (unsigned)t0);
    const uint32_t up = b.now;  // premier releve haut
    b.level(false, kDebounceMs);
    CHECK(b.m.phase() == Phase::Held, "court : relachement pas encore confirme, t0 %08X", (unsigned)t0);
    b.level(false, 1);
    CHECK(b.m.phase() == Phase::Reboot && b.n == 0, "court : redemarrage en attente, t0 %08X", (unsigned)t0);
    CHECK(b.m.lastPressMs() == 1999, "court : %u ms", (unsigned)b.m.lastPressMs());
    b.level(false, 1000);
    CHECK(b.only(Event::Reboot), "court : %u evenement(s), t0 %08X", b.n, (unsigned)t0);
    CHECK(b.n && b.at[0] == up + kDebounceMs + kRebootDelayMs, "court : redemarrage a +%u ms",
          (unsigned)(b.at[0] - up));
    CHECK(b.m.phase() == Phase::Idle, "court : fini");
  }

  // 2000 ms tout juste, et jusqu'a 7999 ms : annule, jamais arme.
  for (uint32_t d : {2000u, 2001u, 5000u, 7998u, 7999u}) {
    Btn b(50000);
    b.level(false, 100);
    b.level(true, d);
    CHECK(b.n == 0 && b.m.phase() == Phase::Held, "%u ms : arme", (unsigned)d);
    b.level(false, 20000);
    CHECK(b.only(Event::Cancelled) && b.m.lastPressMs() == d, "%u ms : %u evenement(s), mesure %u ms", (unsigned)d,
          b.n, (unsigned)b.m.lastPressMs());
    CHECK(b.m.phase() == Phase::Idle, "%u ms : fini", (unsigned)d);
  }

  // Appui long : arme au releve de +7999 ms (8000 ms d'appui), une seule fois ;
  // desappairage 100 ms apres le premier releve haut, pas avant.
  for (uint32_t d : {8000u, 8001u, 60000u}) {
    for (uint32_t t0 : {5000u, 0xFFFFFFFFu - 4000u}) {
      Btn b(t0);
      b.level(false, 100);
      const uint32_t down = b.now;
      b.level(true, d);
      CHECK(b.only(Event::Armed) && b.at[0] == down + kLongMs - 1, "%u ms : arme a +%u ms (%u evenement(s))",
            (unsigned)d, (unsigned)(b.at[0] - down), b.n);
      CHECK(b.m.phase() == Phase::Armed, "%u ms : phase armee", (unsigned)d);
      const uint32_t up = b.now;
      b.level(false, kDebounceMs + 1);
      CHECK(b.m.phase() == Phase::Unpair && b.n == 1, "%u ms : desappairage en attente", (unsigned)d);
      b.level(false, 1000);
      CHECK(b.n == 2 && b.ev[1] == Event::Unpair && b.at[1] == up + kSettleMs,
            "%u ms : desappairage a +%u ms apres le relachement", (unsigned)d, (unsigned)(b.at[1] - up));
      CHECK(b.m.lastPressMs() == d && b.m.phase() == Phase::Idle, "%u ms : mesure %u", (unsigned)d,
            (unsigned)b.m.lastPressMs());
    }
  }

  // Parasites de 30 ms au plus : rien. 31 releves bas : un appui (court).
  {
    Btn b(1000);
    for (int i = 0; i < 50; i++) {
      b.level(true, kDebounceMs);
      b.level(false, 5);
    }
    b.level(false, 1000);
    CHECK(b.n == 0 && b.m.phase() == Phase::Idle, "parasites : %u evenement(s)", b.n);
    b.level(true, kDebounceMs + 1);
    b.level(false, 1000);
    CHECK(b.only(Event::Reboot) && b.m.lastPressMs() == kDebounceMs + 1, "31 ms : appui court");
  }

  // Rebonds a l'appui, pendant l'appui et au relachement : une seule mesure,
  // du premier releve bas du niveau qui tient au premier releve haut du
  // niveau qui tient.
  {
    Btn b(1000);
    b.level(false, 100);
    b.level(true, 3);
    b.level(false, 2);
    b.level(true, 4);
    b.level(false, 1);
    const uint32_t down = b.now;
    b.level(true, 1500);
    b.level(false, 10);
    b.level(true, 400);
    b.level(false, 5);
    b.level(true, 3);
    b.level(false, 2);
    b.level(true, 1);
    const uint32_t up = b.now;
    b.level(false, 1000);
    CHECK(b.only(Event::Reboot), "rebonds : %u evenement(s)", b.n);
    CHECK(b.m.lastPressMs() == up - down, "rebonds : %u ms au lieu de %u", (unsigned)b.m.lastPressMs(),
          (unsigned)(up - down));
    CHECK(b.n && b.at[0] == up + kDebounceMs + kRebootDelayMs, "rebonds : redemarrage a +%u",
          (unsigned)(b.at[0] - up));
  }
  {
    // Rebond pendant un appui long, et juste au releve de +7999 ms : arme au
    // premier releve bas suivant, jamais annule.
    Btn b(1000);
    b.level(false, 100);
    const uint32_t down = b.now;
    b.level(true, 5000);
    b.level(false, 20);
    b.level(true, 2979);
    b.level(false, 5);
    CHECK(b.n == 0, "rebond a +7999 : pas arme");
    b.level(true, 100);
    CHECK(b.only(Event::Armed) && b.at[0] == down + kLongMs + 4, "rebond a +7999 : arme a +%u",
          (unsigned)(b.at[0] - down));
    b.level(false, 1000);
    CHECK(b.n == 2 && b.ev[1] == Event::Unpair, "rebond a +7999 : desappairage");
  }

  // Garde de la broche de strapping : un rebond bas pendant l'attente
  // repousse l'action a 100 ms de releves hauts sans interruption.
  {
    Btn b(1000);
    b.level(false, 100);
    b.level(true, 500);
    b.level(false, 200);
    b.level(true, 10);  // moins de 30 ms : pas un nouvel appui
    const uint32_t last = b.now;
    b.level(false, 1000);
    CHECK(b.only(Event::Reboot) && b.at[0] == last + kSettleMs, "rebond pendant l'attente : redemarrage a +%u",
          (unsigned)(b.at[0] - last));
  }
  {
    Btn b(1000);
    b.level(false, 100);
    b.level(true, 9000);
    for (int i = 0; i < 20; i++) {
      b.level(false, kSettleMs - 1);
      b.level(true, 1);
    }
    CHECK(b.only(Event::Armed) && b.m.phase() == Phase::Unpair, "rebonds apres un appui long : toujours en attente");
    const uint32_t last = b.now;
    b.level(false, 1000);
    CHECK(b.n == 2 && b.ev[1] == Event::Unpair && b.at[1] == last + kSettleMs, "rebonds : desappairage a +%u",
          (unsigned)(b.at[1] - last));
  }

  // Nouvel appui pendant l'attente : l'action est abandonnee, le nouvel appui
  // compte seul.
  {
    Btn b(1000);
    b.level(false, 100);
    b.level(true, 500);
    b.level(false, 60);
    CHECK(b.m.phase() == Phase::Reboot, "attente du redemarrage");
    b.level(true, 3000);
    b.level(false, 1000);
    CHECK(b.n == 2 && b.ev[0] == Event::Dropped && b.ev[1] == Event::Cancelled, "nouvel appui : abandon puis annule");
    b.clear();
    b.level(true, 9000);
    b.level(false, 50);
    CHECK(b.m.phase() == Phase::Unpair, "attente du desappairage");
    b.level(true, 100);
    b.level(false, 1000);
    CHECK(b.n == 3 && b.ev[1] == Event::Dropped && b.ev[2] == Event::Reboot, "nouvel appui court apres un long");
  }

  // Tenu au demarrage : ignore jusqu'a son relachement, meme 20 s ; ensuite,
  // un appui court redemarre normalement.
  {
    Btn b(1000, true);
    CHECK(b.m.phase() == Phase::Locked && b.n == 0, "tenu au demarrage");
    b.level(true, 20000);
    CHECK(b.n == 0 && b.m.phase() == Phase::Locked, "tenu au demarrage : jamais arme");
    b.level(false, 5);
    b.level(true, 5);
    CHECK(b.m.phase() == Phase::Locked, "tenu au demarrage : un rebond ne le libere pas");
    b.level(false, 1000);
    CHECK(b.only(Event::BootReleased) && b.m.phase() == Phase::Idle, "tenu au demarrage : relache, rien fait");
    b.clear();
    b.level(true, 300);
    b.level(false, 1000);
    CHECK(b.only(Event::Reboot), "apres le demarrage : appui court normal");
  }
  {
    Btn b(1000, true);
    b.level(true, 500);
    b.level(false, 2000);
    CHECK(b.only(Event::BootReleased), "tenu brievement au demarrage : rien fait");
  }

  // Derniere garde ratee (bouton rappuye pendant pinSettled()) : la carte
  // relance la machine avec begin(broche basse). Action en attente oubliee,
  // appui en cours ignore jusqu'au relachement, jamais d'action.
  for (bool longPress : {false, true}) {
    Btn b(1000);
    b.level(false, 100);
    b.level(true, longPress ? 9000 : 500);
    b.level(false, kDebounceMs + 1);
    CHECK(b.m.phase() == (longPress ? Phase::Unpair : Phase::Reboot), "garde ratee : action en attente");
    b.clear();
    b.m.begin(true, b.now);
    CHECK(b.m.phase() == Phase::Locked, "garde ratee : verrouille");
    b.level(true, 12000);
    CHECK(b.n == 0 && b.m.phase() == Phase::Locked, "garde ratee : tenu, rien (%u evenement(s))", b.n);
    b.level(false, 5000);
    CHECK(b.only(Event::BootReleased) && b.m.phase() == Phase::Idle, "garde ratee (%s) : relache, rien fait",
          longPress ? "long" : "court");
  }
  {
    // Relance avec la broche deja haute : simplement au repos.
    Btn b(1000);
    b.level(false, 100);
    b.level(true, 500);
    b.level(false, kDebounceMs + 1);
    b.clear();
    b.m.begin(false, b.now);
    b.level(false, 5000);
    CHECK(b.n == 0 && b.m.phase() == Phase::Idle, "relance broche haute : rien en attente");
  }

  // Trous de releves (loop() bloquee) : un front date a plus de kMaxGapMs
  // pres rend la duree incertaine, l'appui est ignore.
  {
    Btn b(1000);
    b.level(false, 100);
    b.level(true, 1000);
    b.skip(kMaxGapMs + 1);  // bloquee au relachement
    b.level(false, 1000);
    CHECK(b.only(Event::Unsure) && b.m.lastGapMs() == kMaxGapMs + 2, "trou au relachement : %u evenement(s), %u ms",
          b.n, (unsigned)b.m.lastGapMs());
  }
  {
    // Trou avant le premier releve bas : un appui court est ignore...
    Btn b(1000);
    b.level(false, 100);
    b.skip(500);
    b.level(true, 1000);
    b.level(false, 1000);
    CHECK(b.only(Event::Unsure), "trou a l'appui : appui court ignore");
  }
  {
    // ... un appui long reste arme : le trou ne pouvait que l'allonger.
    Btn b(1000);
    b.level(false, 100);
    b.skip(500);
    const uint32_t down = b.now;
    b.level(true, 9000);
    b.level(false, 1000);
    CHECK(b.n == 2 && b.ev[0] == Event::Armed && b.at[0] == down + kLongMs - 1 && b.ev[1] == Event::Unpair,
          "trou a l'appui : appui long arme");
  }
  {
    // Trou pendant l'appui (un relachement et un nouvel appui ont pu s'y
    // cacher) : jamais arme, ignore.
    Btn b(1000);
    b.level(false, 100);
    b.level(true, 3000);
    b.skip(2000);
    b.level(true, 10000);
    b.level(false, 1000);
    CHECK(b.only(Event::Unsure), "trou pendant l'appui : %u evenement(s)", b.n);
  }
  {
    // Releves toutes les kMaxGapMs tout juste : acceptes.
    Btn b(1000);
    b.level(false, 1000, kMaxGapMs);
    b.level(true, 1500, kMaxGapMs);
    b.level(false, 2000, kMaxGapMs);
    CHECK(b.only(Event::Reboot) && b.m.lastPressMs() == 1500, "releves espaces de %u ms : appui court",
          (unsigned)kMaxGapMs);
    b.clear();
    b.level(true, 9000, kMaxGapMs);
    b.level(false, 2000, kMaxGapMs);
    CHECK(b.n == 2 && b.ev[0] == Event::Armed && b.ev[1] == Event::Unpair, "releves espaces : appui long");
  }
  {
    // Trou pendant l'attente : 100 ms de releves hauts apres lui.
    Btn b(1000);
    b.level(false, 100);
    b.level(true, 500);
    b.level(false, 50);
    b.skip(5000);
    const uint32_t back = b.now;
    b.level(false, 1000);
    CHECK(b.only(Event::Reboot) && b.at[0] == back + kSettleMs, "trou pendant l'attente : redemarrage a +%u",
          (unsigned)(b.at[0] - back));
  }

  // Proprietes sur des releves aleatoires (rebonds, appuis de toutes durees,
  // trous, tenu au demarrage, retour a zero de millis()) : une action ne part
  // que broche relevee haute, sans trou, depuis kSettleMs au moins ; un
  // redemarrage suit un appui mesure sous 2 s et jamais arme ; un
  // desappairage, un appui arme.
  {
    uint32_t seed = 0x1234567u;
    auto rnd = [&seed](uint32_t n) {
      seed = seed * 1103515245u + 12345u;
      return (seed >> 8) % n;
    };
    struct Obs {
      uint32_t t;
      bool low;
    };
    static Obs hist[1024];
    unsigned reboots = 0, unpairs = 0, unsure = 0, cancelled = 0, bad = 0;
    for (int round = 0; round < 24; round++) {
      Machine m;
      uint32_t now = round % 2 ? 0xFFFFFFFFu - rnd(300000) : rnd(100000);
      bool low = rnd(4) == 0, armed = false;
      Phase before = Phase::Idle;
      unsigned hn = 0;
      for (int run = 0; run < 300; run++) {
        uint32_t len;
        switch (rnd(6)) {
          case 0:
          case 1: len = 1 + rnd(40); break;
          case 2: len = 40 + rnd(400); break;
          case 3: len = 400 + rnd(2500); break;
          case 4: len = 1500 + rnd(1000); break;
          default: len = 6000 + rnd(4000); break;
        }
        const uint32_t end = now + len;
        while ((int32_t)(end - now) > 0) {
          const Event e = m.update(low, now);
          hist[hn % 1024] = Obs{now, low};
          hn++;
          if (m.phase() == Phase::Held && before != Phase::Held) armed = false;
          before = m.phase();
          if (e == Event::Armed) armed = true;
          if (e == Event::Unsure) unsure++;
          if (e == Event::Cancelled) cancelled++;
          if (e == Event::Reboot || e == Event::Unpair) {
            bool ok = false;
            uint32_t prevT = now;
            for (unsigned k = 0; k < hn && k < 1024; k++) {
              const Obs &o = hist[(hn - 1 - k) % 1024];
              if (o.low || prevT - o.t > kMaxGapMs) break;
              prevT = o.t;
              if (now - o.t >= kSettleMs) {
                ok = true;
                break;
              }
            }
            if (!ok && ++bad <= 5) CHECK(ok, "aleatoire : action sans broche haute stable, tour %d t %u", round, now);
            if (e == Event::Reboot) {
              reboots++;
              CHECK(m.lastPressMs() < kShortMaxMs && !armed, "aleatoire : redemarrage apres %u ms (arme %d)",
                    (unsigned)m.lastPressMs(), armed);
            } else {
              unpairs++;
              CHECK(armed && m.lastPressMs() >= kLongMs, "aleatoire : desappairage sans armement (%u ms)",
                    (unsigned)m.lastPressMs());
            }
          }
          // Surtout 1 ms, parfois quelques-unes, rarement un trou de 50 a 400 ms.
          const uint32_t r = rnd(100000);
          now += r < 97000 ? 1 : r < 99995 ? 2 + rnd(20) : 50 + rnd(350);
        }
        low = !low;
      }
    }
    CHECK(bad == 0, "aleatoire : %u action(s) sans broche haute stable", bad);
    CHECK(reboots >= 200 && unpairs >= 50 && unsure >= 50 && cancelled >= 200,
          "aleatoire : %u redemarrage(s), %u desappairage(s), %u incertain(s), %u annule(s)", reboots, unpairs, unsure,
          cancelled);
  }

  // Noms (messages de la console).
  for (int p = 0; p <= (int)Phase::Locked; p++) CHECK(*phaseName((Phase)p) != '?', "nom de phase %d", p);
  for (int e = 0; e <= (int)Event::Unpair; e++) CHECK(*eventName((Event)e) != '?', "nom d'evenement %d", e);
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
  testDelivery();
  testAutoAndCrc8();
  testAutoPresses();
  testMireds();
  testRules();
  testSelectionMemory();
  testBenchT2();
  testResumePlanner();
  testChipWatch();
  testStatusLed();
  testBootButton();

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
