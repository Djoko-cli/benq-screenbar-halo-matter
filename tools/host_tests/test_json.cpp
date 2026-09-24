// Tests hote des briques du protocole JSON (src/json_out.*) : ecrivain,
// echappement, ordre des champs, tailles, messages d'evenement compares aux
// exemples de docs/PROTOCOLE-JSON.md (section 12), lignes de l'hote,
// plafonds de debit, cadence et file des periodiques.
// Lancer : sh tools/test_halo1.sh
//
// Avec un chemin en argument, les messages realistes produits (evenements,
// reponses, livraisons ; ni les essais de l'ecrivain, ni les pires cas
// artificiels) y sont ecrits (RS + JSON + LF) : tools/json_check.py les
// valide ensuite contre la specification.
#include <stdio.h>
#include <string.h>

#include <string>

#include "halo1_map.h"
#include "halo1_proto.h"
#include "json_out.h"

using namespace halo1;
using namespace jsonp;

static int gChecks = 0, gFails = 0;
static FILE *gCapture = nullptr;
static bool gCaptureOn = false;  // seulement les messages realistes

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

static Writer gW;
static uint8_t gAir[4];

// Ferme la ligne et la rend telle qu'elle partirait (RS ... LF).
static std::string finish(Writer &w, bool *ok = nullptr) {
  const bool good = w.finish();
  if (ok) *ok = good;
  std::string s((const char *)w.data(), w.size());
  if (good && gCapture && gCaptureOn) fwrite(s.data(), 1, s.size(), gCapture);
  return s;
}

static std::string framed(const char *json) { return std::string("\x1e") + json + "\n"; }

static void expectLine(Writer &w, const char *json, const char *what) {
  bool ok = false;
  const std::string got = finish(w, &ok);
  const std::string want = framed(json);
  CHECK(ok && got == want, "%s :\n  obtenu  %s  attendu %s", what, got.c_str() + (got.empty() ? 0 : 1),
        want.c_str() + 1);
}

static State mk(bool power, uint8_t lamps, uint8_t bright, uint8_t temp) {
  State s;
  s.power = power;
  s.lamps = lamps;
  s.bright = bright;
  s.temp = temp;
  return s;
}

// ---------------------------------------------------------------------------
//  Ecrivain
// ---------------------------------------------------------------------------

static void testWriter() {
  gW.begin("x", 5, 7);
  expectLine(gW, "{\"v\":1,\"t\":\"x\",\"n\":5,\"ms\":7}", "enveloppe seule");

  // Ordre v, t, n, ms, puis bloc ; entiers extremes ; imbrication.
  gW.begin("etat", 4294967295u, 4294967295u);
  gW.str("bloc", "lampe");
  gW.i32("neg", -2147483647 - 1);
  gW.i32("pos", 2147483647);
  gW.u32("zero", 0);
  gW.obj("o");
  gW.arr("a");
  gW.u32(nullptr, 1);
  gW.str(nullptr, "b");
  gW.obj(nullptr);
  gW.boolean("t", true);
  gW.null("z");
  gW.end();
  gW.end();
  gW.boolean("f", false);
  gW.end();
  gW.arr("vide");
  gW.end();
  expectLine(gW,
             "{\"v\":1,\"t\":\"etat\",\"n\":4294967295,\"ms\":4294967295,\"bloc\":\"lampe\",\"neg\":-2147483648,"
             "\"pos\":2147483647,\"zero\":0,\"o\":{\"a\":[1,\"b\",{\"t\":true,\"z\":null}],\"f\":false},\"vide\":[]}",
             "imbrication et entiers extremes");

  // Echappement : '"' et '\', octets hors 0x20..0x7E -> '?', jamais de \u.
  gW.begin("log", 1, 2);
  gW.str("s", "a\"b\\c\x01\x1e\x7f\xc3\xa9 ~");
  gW.str("tronq", "abcdefgh", 3);
  gW.str("tronq_esc", "\"\"\"\"", 2);  // max compte les caracteres de la valeur, pas l'echappement
  gW.str("nul", nullptr);
  expectLine(gW, "{\"v\":1,\"t\":\"log\",\"n\":1,\"ms\":2,\"s\":\"a\\\"b\\\\c????? ~\",\"tronq\":\"abc\",\"tronq_esc\":\"\\\"\\\"\",\"nul\":null}",
             "echappement");

  // Hexa.
  const uint8_t b[3] = {0x0A, 0xFF, 0x2E};
  gW.begin("h", 0, 0);
  gW.hex("b", b, 3);
  gW.hex("vide", b, 0);
  gW.hexU32("boot", 0x3FA2C901, 8);
  gW.hexU32("pan", 0x1A2B, 4, true);
  gW.hexU32("petit", 0x5, 4, true);
  expectLine(gW, "{\"v\":1,\"t\":\"h\",\"n\":0,\"ms\":0,\"b\":\"0AFF2E\",\"vide\":\"\",\"boot\":\"3FA2C901\",\"pan\":\"0x1A2B\","
                 "\"petit\":\"0x0005\"}",
             "hexa");

  // Mal ferme : jamais emis.
  bool ok = true;
  gW.begin("x", 0, 0);
  gW.obj("o");
  finish(gW, &ok);
  CHECK(!ok, "objet non ferme accepte");
  gW.begin("x", 0, 0);
  gW.end();
  finish(gW, &ok);
  CHECK(!ok, "fermeture en trop acceptee");

  // Taille : exactement 1024 octets (RS et LF compris) passe, 1025 non.
  for (int extra = 0; extra < 2; extra++) {
    gW.begin("x", 0, 0);
    // enveloppe : RS {"v":1,"t":"x","n":0,"ms":0 = 1 + 27 ; ,"p":"..." = 7 + len ; } LF = 2
    const size_t head = 1 + strlen("{\"v\":1,\"t\":\"x\",\"n\":0,\"ms\":0");
    const size_t pad = kLineMax - head - 7 - 2 + (size_t)extra;
    std::string s(pad, 'a');
    gW.str("p", s.c_str(), pad);
    const bool good = gW.finish();
    CHECK(good == (extra == 0) && (!good || gW.size() == kLineMax), "limite de 1024 octets (extra %d, taille %zu)",
          extra, gW.size());
    CHECK(gW.size() <= kLineMax, "jamais plus de 1024 octets en tampon (%zu)", gW.size());
  }
  // Un depassement enorme reste borne et refuse.
  gW.begin("x", 0, 0);
  for (int i = 0; i < 300; i++) gW.u32("k", 4294967295u);
  CHECK(!gW.finish() && gW.overflow() && gW.size() <= kLineMax, "depassement borne");
}

static void testState() {
  mapInit(2.0f);
  // Valeurs de l'exemple 12.1 : lum A5 = niveau 180, temp 53 = 268 mireds.
  gW.begin("x", 0, 0);
  state(gW, "consigne", mk(true, F_LAMPS, 0xA5, 53));
  fields(gW, "tous", FLD_ALL);
  fields(gW, "aucun", 0);
  fields(gW, "lum", FLD_BRIGHT);
  state(gW, "avant", mk(false, F_FRONT, 0xBA, 0));
  state(gW, "arriere", mk(false, F_BACK, 0x4C, 100));
  expectLine(gW,
             "{\"v\":1,\"t\":\"x\",\"n\":0,\"ms\":0,\"consigne\":{\"marche\":true,\"lampes\":\"deux\",\"lum\":165,"
             "\"niveau\":180,\"temp\":53,\"mired\":268},\"tous\":[\"marche\",\"lum\",\"temp\"],\"aucun\":[],"
             "\"lum\":[\"lum\"],\"avant\":{\"marche\":false,\"lampes\":\"avant\",\"lum\":186,\"niveau\":200,\"temp\":0,"
             "\"mired\":153},\"arriere\":{\"marche\":false,\"lampes\":\"arriere\",\"lum\":76,\"niveau\":4,\"temp\":100,"
             "\"mired\":370}}",
             "objet Etat et codes de champs");
  CHECK(!strcmp(lampsCode(0), "aucune"), "lampes 0");
  CHECK(!strcmp(relaunchCode(Relaunch::None), "l3") && symptomCode(Relaunch::None) == nullptr, "l3 / symptome nul");
  CHECK(!strcmp(slotCode(EV_SLOT_AUTO), "a") && !strcmp(slotCode(EV_SLOT_RAW), "brut"), "codes de tranche");
  CHECK(!strcmp(verdictCode(EV_TX_ACK_FOREIGN), "ack_trame") && !strcmp(verdictCode(EV_TX_TIMEOUT), "delai"),
        "codes de verdict");
}

// ---------------------------------------------------------------------------
//  Messages compares aux exemples de la section 12
// ---------------------------------------------------------------------------

static RxEvent listenEvent(uint8_t pid, uint8_t f, uint8_t v, uint8_t len = 2, bool flipCrc = false) {
  RxEvent e{};
  e.listen = true;
  const uint8_t pay[2] = {f, v};
  encodeAir(gAir, pid, false, pay, len, e.raw);
  if (flipCrc) e.raw[3] ^= 0x10;  // un bit du CRC (exemple 12.3)
  e.f = decodeAir(e.raw, gAir);
  e.kind = classify(e.f);
  return e;
}

static void testRx() {
  airOrder(kDefaultAddrReg, gAir);
  RxEvent e = listenEvent(0, 0xFF, 0x00);
  rx(gW, 105, 101310, e, 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"rx\",\"n\":105,\"ms\":101310,\"source\":\"ecoute\",\"brut\":\"087F8068D3800000\",\"len\":2,"
             "\"pid\":0,\"no_ack\":0,\"charge\":\"FF00\",\"crc\":\"D1A7\",\"crc_ok\":true,\"type\":\"service\",\"sens\":null}",
             "rx service (12.3)");
  e = listenEvent(1, 0xC5, 0xA5);
  rx(gW, 106, 101402, e, 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"rx\",\"n\":106,\"ms\":101402,\"source\":\"ecoute\",\"brut\":\"0962D2D86B000000\",\"len\":2,"
             "\"pid\":1,\"no_ack\":0,\"charge\":\"C5A5\",\"crc\":\"B0D6\",\"crc_ok\":true,\"type\":\"lum\",\"sens\":{"
             "\"marche\":true,\"lampes\":\"deux\",\"lum\":165}}",
             "rx luminosite (12.3)");
  e = listenEvent(1, 0, 0, 0);
  rx(gW, 107, 101404, e, 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"rx\",\"n\":107,\"ms\":101404,\"source\":\"ecoute\",\"brut\":\"0126588000000000\",\"len\":0,"
             "\"pid\":1,\"no_ack\":0,\"charge\":\"\",\"crc\":\"4CB1\",\"crc_ok\":true,\"type\":\"accuse_lampe\",\"sens\":null}",
             "rx accuse de la lampe (12.3)");
  e = listenEvent(3, 0xC3, 0x35);
  rx(gW, 109, 101611, e, 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"rx\",\"n\":109,\"ms\":101611,\"source\":\"ecoute\",\"brut\":\"0B619AA284800000\",\"len\":2,"
             "\"pid\":3,\"no_ack\":0,\"charge\":\"C335\",\"crc\":\"4509\",\"crc_ok\":true,\"type\":\"temp\",\"sens\":{"
             "\"marche\":true,\"lampes\":\"deux\",\"temp\":53}}",
             "rx temperature (12.3)");
  e = listenEvent(2, 0xE0, 0x01);
  e.copy = false;
  rx(gW, 118, 102930, e, 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"rx\",\"n\":118,\"ms\":102930,\"source\":\"ecoute\",\"brut\":\"0A70008705800000\",\"len\":2,"
             "\"pid\":2,\"no_ack\":0,\"charge\":\"E001\",\"crc\":\"0E0B\",\"crc_ok\":true,\"type\":\"a\",\"sens\":{"
             "\"numero\":1,\"copie\":false}}",
             "rx bouton A (12.3)");
  // Un bit retourne dans le CRC ; sautes : present seulement s'il y en a.
  e = listenEvent(1, 0xC5, 0xA5, 2, true);
  CHECK(e.kind == Kind::CrcBad, "CRC retourne : %s", kindCode(e.kind));
  rx(gW, 121, 103031, e, 3);
  expectLine(gW,
             "{\"v\":1,\"t\":\"rx\",\"n\":121,\"ms\":103031,\"source\":\"ecoute\",\"brut\":\"0962D2C86B000000\",\"len\":2,"
             "\"pid\":1,\"no_ack\":0,\"charge\":\"C5A5\",\"crc\":\"90D6\",\"crc_ok\":false,\"type\":\"crc_faux\",\"sens\":null,"
             "\"sautes\":3}",
             "rx CRC faux (12.3)");
  // Trame a la place d'un accuse : ni brut, ni PID, ni NO_ACK, ni CRC.
  RxEvent a{};
  a.listen = false;
  a.f.crcOk = true;
  a.f.len = 2;
  a.f.pay[0] = 0xC5;
  a.f.pay[1] = 0x80;
  a.kind = classify(a.f);
  rx(gW, 3, 4, a, 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"rx\",\"n\":3,\"ms\":4,\"source\":\"accuse\",\"brut\":null,\"len\":2,\"pid\":null,"
             "\"no_ack\":null,\"charge\":\"C580\",\"crc\":null,\"crc_ok\":true,\"type\":\"lum\",\"sens\":{\"marche\":true,"
             "\"lampes\":\"deux\",\"lum\":128}}",
             "rx accuse etranger");
  // Longueur illisible sur 64 bits : charge vide.
  RxEvent big{};
  big.listen = true;
  memset(big.raw, 0xFF, 8);
  big.f = decodeAir(big.raw, gAir);
  big.kind = classify(big.f);
  rx(gW, 9, 9, big, 0);
  bool ok = false;
  const std::string s = finish(gW, &ok);
  CHECK(ok && s.find("\"len\":63") != std::string::npos && s.find("\"charge\":\"\"") != std::string::npos &&
            s.find("\"type\":\"crc_faux\"") != std::string::npos,
        "rx de longueur 63 : %s", s.c_str() + 1);
}

static void testEvents() {
  TxEvent t{};
  t.num = 21;
  t.slot = EV_SLOT_BRIGHT;
  t.pay = Payload{0xC5, 0xBA};
  t.attempt = 1;
  t.repeats = 3;
  t.acks = 1;
  t.verdict = EV_TX_ACK;
  t.us = 1719;
  t.rt2 = 0x00;
  t.irq1 = 0x2E;
  t.status = 0x11;
  tx(gW, 72, 95004, t, 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"tx\",\"n\":72,\"ms\":95004,\"num\":21,\"tranche\":\"lum\",\"charge\":\"C5BA\",\"essai\":1,"
             "\"paquets\":3,\"accuses\":1,\"verdict\":\"ack\",\"us\":1719,\"rt2\":\"00\",\"irq1\":\"2E\",\"status\":\"11\"}",
             "tx (12.2)");
  t = TxEvent{};
  t.num = 40;
  t.slot = EV_SLOT_BRIGHT;
  t.pay = Payload{0xC5, 0x80};
  t.attempt = 1;
  t.repeats = 3;
  t.verdict = EV_TX_MAX_RT;
  t.us = 11476;
  t.rt2 = 0x10;
  t.irq1 = 0x1E;
  t.status = 0x01;
  tx(gW, 208, 120450, t, 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"tx\",\"n\":208,\"ms\":120450,\"num\":40,\"tranche\":\"lum\",\"charge\":\"C580\",\"essai\":1,"
             "\"paquets\":3,\"accuses\":0,\"verdict\":\"max_rt\",\"us\":11476,\"rt2\":\"10\",\"irq1\":\"1E\",\"status\":\"01\"}",
             "tx MAX_RT (12.4)");

  RelaunchEvent r{};
  r.cause = Relaunch::RxDeaf;
  r.rank = 1;
  r.deaf = ChipWatch::Deaf{1204, 2870};
  r.ok = true;
  r.crystal = r.calib = 1;
  r.durMs = 312;
  r.total = 1;
  relaunch(gW, 19040, 3605120, r);
  expectLine(gW,
             "{\"v\":1,\"t\":\"relance\",\"n\":19040,\"ms\":3605120,\"cause\":\"sourde\",\"rang\":1,\"detail\":{"
             "\"hors_rx\":1204,\"ms\":2870},\"ok\":true,\"quartz\":true,\"calib\":true,\"duree_ms\":312,\"total\":1,"
             "\"panne\":false}",
             "relance (12.4)");
  r = RelaunchEvent{};
  r.cause = Relaunch::None;
  r.ok = false;
  r.crystal = r.calib = -1;
  r.durMs = 480;
  r.total = 3;
  r.failed = true;
  relaunch(gW, 1, 2, r);
  expectLine(gW,
             "{\"v\":1,\"t\":\"relance\",\"n\":1,\"ms\":2,\"cause\":\"l3\",\"rang\":null,\"detail\":{},\"ok\":false,"
             "\"quartz\":null,\"calib\":null,\"duree_ms\":480,\"total\":3,\"panne\":true}",
             "relance L3 ratee");
  r = RelaunchEvent{};
  r.cause = Relaunch::RxNoise;
  r.rank = 2;
  r.flood = ChipWatch::Flood{120, 118, 9000};
  r.ok = r.crystal = r.calib = 1;
  relaunch(gW, 1, 2, r);
  bool ok = false;
  std::string s = finish(gW, &ok);
  CHECK(ok && s.find("\"detail\":{\"trames\":120,\"crc_faux\":118,\"ms\":9000}") != std::string::npos, "relance bruit : %s",
        s.c_str() + 1);
  r.cause = Relaunch::TxTimeout;
  r.timeoutRun = 3;
  relaunch(gW, 1, 2, r);
  s = finish(gW, &ok);
  CHECK(ok && s.find("\"detail\":{\"suite\":3}") != std::string::npos, "relance delais : %s", s.c_str() + 1);
  r.cause = Relaunch::Verify;
  r.verifyFails = 7;
  relaunch(gW, 1, 2, r);
  s = finish(gW, &ok);
  CHECK(ok && s.find("\"cause\":\"verif\",\"rang\":2,\"detail\":{\"verif_ratees\":7}") != std::string::npos,
        "relance verif : %s", s.c_str() + 1);

  ModuleEvent m{};
  m.state = ModuleState::Fault;
  m.unrecovered = 3;
  m.symptom = Relaunch::TxTimeout;
  m.retryS = 600;
  module(gW, 28770, 5410022, m);
  expectLine(gW,
             "{\"v\":1,\"t\":\"module\",\"n\":28770,\"ms\":5410022,\"etat\":\"panne\",\"sans_guerison\":3,"
             "\"symptome\":\"delais\",\"essai_s\":600}",
             "module panne (12.4)");
  m = ModuleEvent{};
  m.state = ModuleState::ConfigRejected;
  m.regsKnown = true;
  m.rfch = 0x05;
  m.dm1 = 0x82;
  m.rt1 = 0x70;
  module(gW, 1, 2, m);
  expectLine(gW, "{\"v\":1,\"t\":\"module\",\"n\":1,\"ms\":2,\"etat\":\"config_rejetee\",\"rfch\":\"05\",\"dm1\":\"82\",\"rt1\":\"70\"}",
             "module config rejetee");
  m.regsKnown = false;
  module(gW, 1, 2, m);
  expectLine(gW, "{\"v\":1,\"t\":\"module\",\"n\":1,\"ms\":2,\"etat\":\"config_rejetee\",\"rfch\":null,\"dm1\":null,\"rt1\":null}",
             "module config rejetee illisible");
  const ModuleState simple[] = {ModuleState::Recovered, ModuleState::Lost, ModuleState::Found, ModuleState::ConfigVerified};
  const char *const names[] = {"retabli", "perdu", "retrouve", "config_verifiee"};
  for (int i = 0; i < 4; i++) {
    m = ModuleEvent{};
    m.state = simple[i];
    module(gW, 1, 2, m);
    char want[128];
    snprintf(want, sizeof(want), "{\"v\":1,\"t\":\"module\",\"n\":1,\"ms\":2,\"etat\":\"%s\"}", names[i]);
    expectLine(gW, want, names[i]);
  }

  led(gW, 77, 95207, "livree", "operationnel", false);
  expectLine(gW, "{\"v\":1,\"t\":\"led\",\"n\":77,\"ms\":95207,\"motif\":\"livree\",\"avant\":\"operationnel\",\"test\":false}",
             "led (12.2)");
  logLine(gW, 640, 200100, "lampe", "notice", "[lampe] injoignable : consigne abandonnee", 0);
  expectLine(gW,
             "{\"v\":1,\"t\":\"log\",\"n\":640,\"ms\":200100,\"src\":\"lampe\",\"niv\":\"notice\",\"txt\":\"[lampe] "
             "injoignable : consigne abandonnee\"}",
             "log (12.7)");
  heartbeat(gW, 641, 202000, 0x3FA2C901, 202, 0);
  expectLine(gW, "{\"v\":1,\"t\":\"hb\",\"n\":641,\"ms\":202000,\"boot\":\"3FA2C901\",\"up_s\":202,\"json_perdus\":0}",
             "hb (12.7)");
  sessionEnd(gW, 662, 231400, "bail");
  expectLine(gW, "{\"v\":1,\"t\":\"fin\",\"n\":662,\"ms\":231400,\"cause\":\"bail\"}", "fin (12.7)");
}

static void testReply() {
  mapInit(2.0f);
  Reply r;
  r.id = 1;
  r.cmd = "json 1";
  r.durMs = 12;
  r.hasLease = true;
  r.leaseS = 30;
  r.upS = 83;
  reply(gW, 11, 83523, r);
  expectLine(gW,
             "{\"v\":1,\"t\":\"reponse\",\"n\":11,\"ms\":83523,\"id\":1,\"etape\":\"fin\",\"cmd\":\"json 1\",\"ok\":true,"
             "\"code\":\"ok\",\"duree_ms\":12,\"bail_s\":30,\"up_s\":83}",
             "reponse json 1 (12.1)");
  r = Reply();
  r.id = 2;
  r.cmd = "lampe niveau 200";
  r.code = "accepte";
  r.durMs = 1;
  r.suite = Reply::SuiteDelivery;
  r.hasTarget = true;
  r.target = mk(true, F_LAMPS, rawFromLevel(200), 53);
  r.dirty = FLD_BRIGHT;
  r.version = 13;
  reply(gW, 71, 95002, r);
  expectLine(gW,
             "{\"v\":1,\"t\":\"reponse\",\"n\":71,\"ms\":95002,\"id\":2,\"etape\":\"fin\",\"cmd\":\"lampe niveau 200\","
             "\"ok\":true,\"code\":\"accepte\",\"duree_ms\":1,\"suite\":\"livraison\",\"consigne\":{\"marche\":true,"
             "\"lampes\":\"deux\",\"lum\":186,\"niveau\":200,\"temp\":53,\"mired\":268},\"a_livrer\":[\"lum\"],"
             "\"version\":13}",
             "reponse accepte (12.2)");
  r = Reply();
  r.id = 8;
  r.fin = false;
  r.cmd = "lampe stats";
  r.code = "en_cours";
  reply(gW, 530, 180002, r);
  expectLine(gW,
             "{\"v\":1,\"t\":\"reponse\",\"n\":530,\"ms\":180002,\"id\":8,\"etape\":\"debut\",\"cmd\":\"lampe stats\","
             "\"ok\":true,\"code\":\"en_cours\"}",
             "reponse debut (12.6)");
  r.fin = true;
  r.code = "execute";
  r.durMs = 7;
  reply(gW, 531, 180009, r);
  expectLine(gW,
             "{\"v\":1,\"t\":\"reponse\",\"n\":531,\"ms\":180009,\"id\":8,\"etape\":\"fin\",\"cmd\":\"lampe stats\","
             "\"ok\":true,\"code\":\"execute\",\"duree_ms\":7}",
             "reponse fin (12.6)");
  r = Reply();
  r.id = 9;
  r.cmd = "lampe auto";
  r.ok = false;
  r.code = "refuse";
  r.msg = "lampe eteinte : A n'est pas emis";
  reply(gW, 585, 190001, r);
  expectLine(gW,
             "{\"v\":1,\"t\":\"reponse\",\"n\":585,\"ms\":190001,\"id\":9,\"etape\":\"fin\",\"cmd\":\"lampe auto\","
             "\"ok\":false,\"code\":\"refuse\",\"msg\":\"lampe eteinte : A n'est pas emis\",\"duree_ms\":0}",
             "reponse refus (12.6)");
  // cmd tronquee a 40 caracteres.
  char cmd[kCmdTextMax + 1];
  copyCmd(cmd, "lampe rampe 4C FE 01 20 et encore des mots pour depasser");
  CHECK(strlen(cmd) == kCmdTextMax && !strncmp(cmd, "lampe rampe 4C FE 01 20 et encore des mo", kCmdTextMax),
        "copyCmd : '%s'", cmd);
}

static void testDelivery() {
  mapInit(2.0f);
  const State s = mk(true, F_LAMPS, 186, 53);
  const uint32_t ids[1] = {2};
  Delivery d;
  d.issue = Issue::Delivered;
  d.last = EV_SLOT_BRIGHT;
  d.version = 13;
  d.target = d.believed = s;
  d.ids = ids;
  d.nIds = 1;
  d.hasWait = true;
  d.waitMs = 204;
  d.delivered = 5;
  delivery(gW, 76, 95206, d);
  expectLine(gW,
             "{\"v\":1,\"t\":\"livraison\",\"n\":76,\"ms\":95206,\"issue\":\"livree\",\"derniere\":\"lum\",\"version\":13,"
             "\"consigne\":{\"marche\":true,\"lampes\":\"deux\",\"lum\":186,\"niveau\":200,\"temp\":53,\"mired\":268},"
             "\"cru\":{\"marche\":true,\"lampes\":\"deux\",\"lum\":186,\"niveau\":200,\"temp\":53,\"mired\":268},"
             "\"a_livrer\":[],\"ids\":[2],\"ids_perdus\":0,\"attente_ms\":204,\"livrees\":5,\"abandons\":0}",
             "livraison livree (12.2)");
  const uint32_t ids7[1] = {7};
  d = Delivery();
  d.issue = Issue::GaveUp;
  d.cause = GiveUpCause::Unreachable;
  d.version = 17;
  d.target = d.believed = s;
  d.ids = ids7;
  d.nIds = 1;
  d.hasWait = true;
  d.waitMs = 4520;
  d.delivered = 5;
  d.giveUps = 1;
  delivery(gW, 251, 124970, d);
  expectLine(gW,
             "{\"v\":1,\"t\":\"livraison\",\"n\":251,\"ms\":124970,\"issue\":\"abandon\",\"cause\":\"injoignable\","
             "\"version\":17,\"consigne\":{\"marche\":true,\"lampes\":\"deux\",\"lum\":186,\"niveau\":200,\"temp\":53,"
             "\"mired\":268},\"cru\":{\"marche\":true,\"lampes\":\"deux\",\"lum\":186,\"niveau\":200,\"temp\":53,"
             "\"mired\":268},\"a_livrer\":[],\"ids\":[7],\"ids_perdus\":0,\"attente_ms\":4520,\"livrees\":5,"
             "\"abandons\":1}",
             "livraison abandon (12.4)");
  d = Delivery();
  d.issue = Issue::Cancelled;
  d.target = d.believed = s;
  d.dirty = FLD_BRIGHT;
  delivery(gW, 1, 2, d);
  bool ok = false;
  const std::string got = finish(gW, &ok);
  CHECK(ok && got.find("\"issue\":\"annulee\",\"version\"") != std::string::npos &&
            got.find("\"ids\":[],\"ids_perdus\":0,\"attente_ms\":null") != std::string::npos &&
            got.find("\"a_livrer\":[\"lum\"]") != std::string::npos,
        "livraison annulee : %s", got.c_str() + 1);
}

// ---------------------------------------------------------------------------
//  Pires cas : chaque message tient dans le budget de 896 octets
// ---------------------------------------------------------------------------

static void checkBudget(const char *what, size_t *worst) {
  bool ok = false;
  const std::string s = finish(gW, &ok);
  CHECK(ok && s.size() <= kBudget, "%s : %zu octets (budget %zu)", what, s.size(), kBudget);
  if (s.size() > *worst) *worst = s.size();
}

static void testWorstCases() {
  const uint32_t M = 4294967295u;
  size_t worst = 0;
  // rx : toutes les formes, compteurs au maximum.
  for (int kind = 0; kind < 8; kind++)
    for (int listen = 0; listen < 2; listen++) {
      RxEvent e{};
      e.listen = listen;
      memset(e.raw, 0xFF, 8);
      e.f.len = 4;
      e.f.pid = 3;
      e.f.noAck = 1;
      e.f.crc = 0xFFFF;
      memset(e.f.pay, 0xFF, 4);
      e.f.pay[0] = 0x45;  // marche, arriere
      e.kind = (Kind)kind;
      rx(gW, M, M, e, M);
      checkBudget("rx", &worst);
    }
  TxEvent t{};
  t.num = M;
  t.slot = EV_SLOT_RAW;
  t.attempt = t.repeats = t.acks = 255;
  t.verdict = EV_TX_ACK_FOREIGN;
  t.us = 65535;
  tx(gW, M, M, t, M);
  checkBudget("tx", &worst);
  RelaunchEvent r{};
  r.rank = 255;
  r.flood = ChipWatch::Flood{65535, 65535, M};
  r.deaf = ChipWatch::Deaf{M, M};
  r.timeoutRun = 255;
  r.verifyFails = M;
  r.durMs = r.total = M;
  for (int c = 0; c < 5; c++) {
    r.cause = (Relaunch)c;
    relaunch(gW, M, M, r);
    checkBudget("relance", &worst);
  }
  ModuleEvent m{};
  m.state = ModuleState::Fault;
  m.unrecovered = 255;
  m.symptom = Relaunch::RxNoise;
  m.retryS = M;
  module(gW, M, M, m);
  checkBudget("module", &worst);
  // reponse : cmd et msg a leur longueur maximale, entierement echappes.
  const std::string q(200, '"'), bs(200, '\\');
  Reply rp;
  rp.id = kIdMax;
  rp.cmd = q.c_str();
  rp.ok = false;
  rp.code = "radio_absente";
  rp.msg = bs.c_str();
  rp.durMs = M;
  rp.suite = Reply::SuiteDelivery;
  rp.hasTarget = true;
  rp.target = mk(true, F_BACK, 0xFE, 0x64);
  rp.dirty = FLD_ALL;
  rp.version = M;
  rp.hasLease = true;
  rp.leaseS = rp.upS = M;
  reply(gW, M, M, rp);
  checkBudget("reponse", &worst);
  // livraison : 8 id au maximum.
  uint32_t ids[kIdsMax];
  for (uint32_t &i : ids) i = kIdMax;
  Delivery d;
  d.issue = Issue::GaveUp;
  d.cause = GiveUpCause::Unreachable;
  d.version = M;
  d.target = d.believed = mk(true, F_BACK, 0xFE, 0x64);
  d.dirty = FLD_ALL;
  d.ids = ids;
  d.nIds = kIdsMax;
  d.idsLost = M;
  d.hasWait = true;
  d.waitMs = M;
  d.delivered = d.giveUps = M;
  delivery(gW, M, M, d);
  checkBudget("livraison", &worst);
  d.issue = Issue::Delivered;
  d.last = EV_SLOT_TEMP;
  delivery(gW, M, M, d);
  checkBudget("livraison livree", &worst);
  // log : 191 caracteres, tous echappes.
  const std::string lq(400, '\\');
  logLine(gW, M, M, "matter", "notice", lq.c_str(), M);
  checkBudget("log", &worst);
  heartbeat(gW, M, M, M, M, M);
  checkBudget("hb", &worst);
  led(gW, M, M, "identification", "identification", true);
  checkBudget("led", &worst);
  printf("  pire cas des evenements : %zu octets (budget %zu)\n", worst, kBudget);
}

// ---------------------------------------------------------------------------
//  Lignes de l'hote
// ---------------------------------------------------------------------------

static bool idOf(const char *in, uint32_t *id, std::string *rest) {
  char buf[160];
  snprintf(buf, sizeof(buf), "%s", in);
  char *r = nullptr;
  const bool ok = parseIdPrefix(buf, id, &r);
  *rest = r;
  return ok;
}

static void testIdPrefix() {
  uint32_t id = 0;
  std::string rest;
  CHECK(idOf("id=17 lampe niveau 200", &id, &rest) && id == 17 && rest == "lampe niveau 200", "id=17");
  CHECK(idOf("  id=1   json 1", &id, &rest) && id == 1 && rest == "json 1", "espaces");
  CHECK(idOf("id=999999999 json ping", &id, &rest) && id == 999999999 && rest == "json ping", "id maximal");
  CHECK(idOf("id=5", &id, &rest) && id == 5 && rest.empty(), "id seul");
  CHECK(idOf("id=007 x", &id, &rest) && id == 7, "zeros de tete");
  CHECK(!idOf("id=0 json 1", &id, &rest) && rest == "id=0 json 1", "id=0 refuse");
  CHECK(!idOf("id=1000000000 json 1", &id, &rest), "id trop grand");
  CHECK(!idOf("id=0000000001 x", &id, &rest), "plus de 9 chiffres");
  CHECK(!idOf("id= 5 x", &id, &rest), "sans chiffre");
  CHECK(!idOf("id=17lampe", &id, &rest), "sans espace apres le numero");
  CHECK(!idOf("id=-3 x", &id, &rest), "negatif");
  CHECK(!idOf("lampe id=3", &id, &rest), "pas en tete");
  CHECK(!idOf("ID=3 x", &id, &rest), "majuscules");
}

static std::string feedAll(LineAssembler &a, const char *bytes, size_t n, bool machine, int *lines,
                           std::string *echo) {
  std::string last;
  for (size_t i = 0; i < n; i++) {
    const LineAssembler::Ev e = a.feed((uint8_t)bytes[i], machine);
    if (e == LineAssembler::Ev::Echo && echo) *echo += bytes[i];
    if (e == LineAssembler::Ev::Line) {
      last = a.text();
      last += a.tooLong() ? "|trop long" : "";
      (*lines)++;
      a.reset();
    }
  }
  return last;
}

static void testAssembler() {
  LineAssembler a;
  int lines = 0;
  std::string echo;
  const char in1[] = "id=1 json 1\r\n";
  CHECK(feedAll(a, in1, sizeof(in1) - 1, true, &lines, &echo) == "id=1 json 1" && lines == 1, "ligne simple, CR ignore");
  // Mode machine : RS, echappements et octets hauts ignores.
  lines = 0;
  const char in2[] = "le\x1e" "d\x1b\x01\xff\x80 test\n";
  CHECK(feedAll(a, in2, sizeof(in2) - 1, true, &lines, nullptr) == "led test" && lines == 1, "octets filtres");
  // Mode humain : inchange, tout octet est garde (sauf CR, LF, Ctrl-U, retour arriere).
  lines = 0;
  const char in3[] = "a\x01" "b\n";
  CHECK(feedAll(a, in3, sizeof(in3) - 1, false, &lines, nullptr) == std::string("a\x01" "b") && lines == 1,
        "mode humain : octets gardes");
  // Ctrl-U vide la ligne ; retour arriere.
  lines = 0;
  const char in4[] = "reste d'une session\x15" "\nid=2 json pinh\x08g\n";
  a.reset();
  std::string last = feedAll(a, in4, sizeof(in4) - 1, true, &lines, nullptr);
  CHECK(lines == 2 && last == "id=2 json ping", "Ctrl-U puis retour arriere : %d ligne(s), '%s'", lines, last.c_str());
  a.reset();
  a.feed('a', false);
  a.feed('b', false);
  CHECK(a.feed(kCtrlU, false) == LineAssembler::Ev::Clear && a.cleared() == 2 && a.length() == 0, "Ctrl-U compte");
  CHECK(a.feed(8, false) == LineAssembler::Ev::None, "retour arriere sur ligne vide");
  // 127 caracteres passent, 128 marquent la ligne trop longue ; le suivant repart.
  for (int extra = 0; extra < 2; extra++) {
    a.reset();
    lines = 0;
    std::string s(kCmdMax + (size_t)extra, 'x');
    s += "\n";
    last = feedAll(a, s.c_str(), s.size(), true, &lines, nullptr);
    CHECK(lines == 1 && (extra ? last.size() == kCmdMax + strlen("|trop long") : last.size() == kCmdMax),
          "limite de 127 (extra %d) : %zu", extra, last.size());
  }
  lines = 0;
  last = feedAll(a, "ok\n", 3, true, &lines, nullptr);
  CHECK(last == "ok", "apres une ligne trop longue");
  // Ctrl-U efface aussi le depassement.
  a.reset();
  std::string s(200, 'y');
  s += "\x15" "json\n";
  lines = 0;
  last = feedAll(a, s.c_str(), s.size(), true, &lines, nullptr);
  CHECK(last == "json", "Ctrl-U apres depassement : '%s'", last.c_str());
}

// ---------------------------------------------------------------------------
//  Debit, cadence, file
// ---------------------------------------------------------------------------

static void testRate() {
  RateCap cap(50);
  uint32_t t = 0u - 300;  // a travers le retour a zero de millis()
  unsigned passed = 0;
  for (int i = 0; i < 80; i++) {
    if (cap.available(t)) {
      cap.take();
      passed++;
    } else {
      cap.skip();
    }
    t += 5;  // 80 evenements en 400 ms
  }
  CHECK(passed == 50 && cap.takeSkipped() == 30 && cap.takeSkipped() == 0, "50 par seconde : %u", passed);
  t += 1000;
  CHECK(cap.available(t), "nouvelle fenetre");
  // Cadence : 20 lignes par seconde glissante.
  Cadence c;
  uint32_t now = 1000;
  int ok = 0;
  for (int i = 0; i < 25; i++) ok += c.allow(now + (uint32_t)i * 10);  // 25 lignes en 250 ms
  CHECK(ok == 20, "cadence : %d lignes acceptees sur 25", ok);
  CHECK(!c.allow(now + 999), "21e ligne dans la seconde");
  CHECK(c.allow(now + 1000), "la plus ancienne sort de la fenetre");
  CHECK(!c.allow(now + 1001), "une seule place liberee");
  CHECK(c.allow(now + 1010), "la suivante sort a son tour");
}

static void testQueue() {
  Queue q;
  CHECK(q.push(Item::EtatLampe, 0, true) && q.push(Item::EtatSante, 1, true) && q.size() == 2, "deux elements");
  CHECK(q.push(Item::EtatLampe, 2, true) && q.size() == 2, "doublon ignore");
  CHECK(q.push(Item::Reply, 3, false, 1) && q.push(Item::Reply, 3, false, 2) && q.size() == 4, "reponses jamais fondues");
  CHECK(q.push(Item::EtatSante, 4, false) && q.size() == 4, "doublon explicite");
  CHECK(q.dropSession() == 1 && q.size() == 3, "fin de session : %u restent", q.size());
  const Queued *f = q.front();
  CHECK(f && f->item == Item::EtatSante && !f->session && f->at == 4, "sante promue, gardee a sa place, retard depuis la demande");
  q.pop();
  f = q.front();
  CHECK(f && f->item == Item::Reply && f->arg == 1, "ordre garde");
  q.clear();
  int pushed = 0;
  for (int i = 0; i < 40; i++) pushed += q.push(Item::Reply, (uint32_t)i, false, (uint8_t)i);
  CHECK(pushed == Queue::kN && q.size() == Queue::kN, "file pleine a %u", q.size());
  CHECK(!q.push(Item::Config, 50, false), "rien de plus");
  for (int i = 0; i < 5; i++) q.pop();
  CHECK(q.push(Item::Config, 51, false) && q.has(Item::Config), "place rendue");
  uint8_t prev = 4;
  bool order = true;
  while (const Queued *e = q.front()) {
    if (e->item == Item::Reply) {
      order &= e->arg == prev + 1;
      prev = e->arg;
    }
    q.pop();
  }
  CHECK(order, "ordre FIFO a travers le tour de l'anneau");
}

// Retard (500 ms) et place libre : les deux gardes de drain() (json_mode.cpp).
static void testQueueDrain() {
  Queue q;
  CHECK(!q.frontReady(4096) && q.dropLate(1000) == 0, "file vide");
  q.push(Item::EtatLampe, 0, true);
  q.push(Item::EtatSante, 100, true);
  q.push(Item::Reply, 200, false, 0);
  q.push(Item::CptPilote, 250, true);
  CHECK(q.dropLate(500) == 0 && q.size() == 4, "500 ms tout juste : rien de perdu");
  CHECK(q.dropLate(700) == 2 && q.front()->item == Item::Reply, "deux periodiques perdues, la reponse arrete le balayage");
  CHECK(q.dropLate(1000000) == 0 && q.size() == 2, "une reponse n'est jamais perdue pour retard");
  CHECK(!q.frontReady(kLineMax - 1) && q.frontReady(kLineMax), "reponse : des qu'une ligne entiere tient");
  CHECK(!q.frontReady(-1), "place inconnue (verrou pris) : on attend");
  q.pop();
  CHECK(q.front()->item == Item::CptPilote && q.dropLate(1000000) == 1 && !q.size(),
        "derriere la reponse, la periodique en retard est perdue a son tour");
  q.push(Item::CptRadio, 2000, true);
  CHECK(!q.frontReady(2 * kLineMax - 1) && q.frontReady(2 * kLineMax), "periodique : une ligne, puis la place d'un evenement");
  q.clear();
  // Fusion : une demande explicite repart de maintenant, pas une periodique.
  q.push(Item::EtatSante, 0, true);
  q.push(Item::EtatSante, 450, false);
  CHECK(q.size() == 1 && !q.front()->session && q.front()->at == 450, "json etat fondu dans une periodique");
  CHECK(q.dropLate(950) == 0 && q.dropLate(951) == 1, "retard compte depuis la demande explicite");
  q.push(Item::EtatLampe, 0, true);
  q.push(Item::EtatLampe, 400, true);
  CHECK(q.front()->at == 0 && q.dropLate(501) == 1, "periodique fondue : garde son retard");
  q.clear();
  q.push(Item::HelloBase, 0, false);
  q.push(Item::HelloBase, 300, true);
  CHECK(!q.front()->session && q.front()->at == 0, "periodique fondue dans une demande explicite : ni session, ni retard remis");
}

static void testLease() {
  CHECK(!leaseExpired(1000000, 0, 0, 0), "sans bail : jamais");
  CHECK(!leaseExpired(30999, 1000, 500, 30) && leaseExpired(31000, 1000, 500, 30), "30 s apres le dernier octet");
  // Commande de banc de 60 s : le bail part de sa fin.
  CHECK(!leaseExpired(90000, 1000, 70000, 30) && leaseExpired(100000, 1000, 70000, 30), "30 s apres la fin de la commande");
  const uint32_t rx = 0xFFFFF000u;
  CHECK(!leaseExpired(rx + 29999u, rx, rx - 0x1000u, 30) && leaseExpired(rx + 30000u, rx, rx - 0x1000u, 30),
        "a travers le retour a zero de millis()");
  CHECK(!leaseExpired(9999, 0, 0, 10) && leaseExpired(600000, 0, 0, 600), "bornes 10 et 600 s");
}

// Observateur de livraison : scenarios de 7.3, 12.2, 12.4 et U10.
struct WatchRun {
  DeliveryWatch w;
  LampSample s;
  Delivery d;
  bool poll(uint32_t now, bool machine = true) {
    d = Delivery();
    return w.poll(s, now, machine, &d);
  }
};

static void testDeliveryWatch() {
  mapInit(2.0f);
  const State st = mk(true, F_LAMPS, 186, 53);
  // 12.2 : id=2 lampe niveau 200 accepte, trois paquets, livree 204 ms plus tard.
  {
    WatchRun r;
    r.w.reset(4, 0);
    r.s.delivered = 4;
    CHECK(!r.poll(95000), "repos");
    r.s.busy = r.s.targetBusy = true;
    r.s.pendingSince = 95002;
    r.w.pendingId(2, 95002);
    CHECK(!r.poll(95003) && !r.poll(95104), "rafale en cours");
    r.s = LampSample();
    r.s.delivered = 5;  // complete() : delivered_++, pendingSince_ remis a 0
    CHECK(r.poll(95206), "front de busy : livraison");
    CHECK(r.d.issue == Issue::Delivered && r.d.nIds == 1 && r.d.ids[0] == 2 && r.d.hasWait && r.d.waitMs == 204 &&
              r.d.delivered == 5 && r.d.giveUps == 0 && r.d.idsLost == 0,
          "12.2 : issue %d, %u id, attente %u", (int)r.d.issue, r.d.nIds, r.d.waitMs);
    r.d.last = EV_SLOT_BRIGHT;
    r.d.version = 13;
    r.d.target = r.d.believed = st;
    delivery(gW, 76, 95206, r.d);
    expectLine(gW,
               "{\"v\":1,\"t\":\"livraison\",\"n\":76,\"ms\":95206,\"issue\":\"livree\",\"derniere\":\"lum\",\"version\":13,"
               "\"consigne\":{\"marche\":true,\"lampes\":\"deux\",\"lum\":186,\"niveau\":200,\"temp\":53,\"mired\":268},"
               "\"cru\":{\"marche\":true,\"lampes\":\"deux\",\"lum\":186,\"niveau\":200,\"temp\":53,\"mired\":268},"
               "\"a_livrer\":[],\"ids\":[2],\"ids_perdus\":0,\"attente_ms\":204,\"livrees\":5,\"abandons\":0}",
               "livraison de l'observateur (12.2)");
    CHECK(!r.poll(95300) && r.w.pending() == 0, "une seule livraison, liste videe");
  }
  // 12.4 : lampe debranchee, trois tours, abandon ; reprise sans tranche active.
  {
    WatchRun r;
    r.w.reset(5, 0);
    r.s.busy = r.s.targetBusy = true;
    r.s.delivered = 5;
    r.s.pendingSince = 120450;
    r.w.pendingId(7, 120450);
    CHECK(!r.poll(120451), "premier tour");
    r.s.targetBusy = false;  // attente de reprise : busy() seul
    r.s.pendingSince = 0;    // endBurst()
    CHECK(!r.poll(121000) && !r.poll(123500), "reprises");
    r.s = LampSample();
    r.s.delivered = 5;
    r.s.giveUps = 1;
    CHECK(r.poll(124970), "abandon");
    CHECK(r.d.issue == Issue::GaveUp && r.d.nIds == 1 && r.d.ids[0] == 7 && r.d.hasWait && r.d.waitMs == 4520 &&
              r.d.giveUps == 1,
          "12.4 : issue %d, attente %u", (int)r.d.issue, r.d.waitMs);
  }
  // U10 : trame de la telecommande pendant la reprise, tranche retiree : annulee.
  {
    WatchRun r;
    r.w.reset(0, 0);
    r.s.busy = r.s.targetBusy = true;
    r.s.pendingSince = 1000;
    r.w.pendingId(3, 1000);
    CHECK(!r.poll(1001), "rafale");
    r.s.targetBusy = false;
    CHECK(!r.poll(1500), "reprise");
    r.s = LampSample();
    CHECK(r.poll(2100) && r.d.issue == Issue::Cancelled && r.d.nIds == 1 && r.d.ids[0] == 3 && r.d.hasWait &&
              r.d.waitMs == 1100,
          "U10 : issue %d, %u id", (int)r.d.issue, r.d.nIds);
  }
  // Acceptee puis periode finie avant le tour suivant, sans compteur change
  // ('lampe oublie' dans la meme lecture USB) : annulee, jamais reportee.
  {
    WatchRun r;
    r.w.reset(9, 2);
    r.s.delivered = 9;
    r.s.giveUps = 2;
    r.w.pendingId(3, 5000);
    CHECK(r.poll(5002) && r.d.issue == Issue::Cancelled && r.d.nIds == 1 && r.d.ids[0] == 3 && r.d.hasWait &&
              r.d.waitMs == 2,
          "periode jamais vue : annulee avec l'id");
    r.s.busy = r.s.targetBusy = true;
    CHECK(!r.poll(6000), "consigne suivante (Matter)");
    r.s.busy = r.s.targetBusy = false;
    r.s.delivered = 10;
    CHECK(r.poll(6300) && r.d.issue == Issue::Delivered && r.d.nIds == 0, "la suivante ne porte pas l'id 3");
  }
  // Commande bloquante (humaine, waitIdle) : pas de front vu, compteur change.
  {
    WatchRun r;
    r.w.reset(1, 0);
    r.s.delivered = 2;
    CHECK(r.poll(100) && r.d.issue == Issue::Delivered && !r.d.hasWait && r.d.nIds == 0,
          "bloquante : livree, attente_ms null");
    CHECK(!r.poll(101), "compteurs repris : rien de plus");
    // Deux consignes enchainees (lampe rampe) : abandon l'emporte.
    r.s.delivered = 5;
    r.s.giveUps = 1;
    CHECK(r.poll(200) && r.d.issue == Issue::GaveUp, "abandon l'emporte sur livree");
    // Mode humain sans id : l'etat avance, rien n'est emis.
    r.s.delivered = 6;
    CHECK(!r.poll(300, false) && !r.poll(301, true), "hors mode machine sans id : rien, puis rien de double");
  }
  // lampe brut : periode occupee par la seule tranche brute : rien.
  {
    WatchRun r;
    r.w.reset(0, 0);
    r.s.busy = true;
    CHECK(!r.poll(10) && !r.poll(20), "brut en cours");
    r.s.busy = false;
    CHECK(!r.poll(30), "brut fini : aucune livraison");
    // Une consigne pendant le brut : livraison a la fin.
    r.s.busy = true;
    CHECK(!r.poll(40), "brut");
    r.s.targetBusy = true;
    CHECK(!r.poll(50), "consigne");
    r.s.busy = r.s.targetBusy = false;
    CHECK(r.poll(60) && r.d.issue == Issue::Cancelled, "consigne vue puis retiree : annulee");
  }
  // Id en mode humain : livraison quand meme ; 10 id : les 8 derniers, 2 perdus.
  {
    WatchRun r;
    r.w.reset(0, 0);
    r.s.busy = r.s.targetBusy = true;
    for (uint32_t i = 1; i <= 10; i++) r.w.pendingId(i, 700);
    CHECK(r.w.pending() == kIdsMax, "8 id au plus");
    CHECK(!r.poll(701, false), "occupe");
    r.s.busy = r.s.targetBusy = false;
    r.s.delivered = 1;
    CHECK(r.poll(900, false) && r.d.nIds == kIdsMax && r.d.ids[0] == 3 && r.d.ids[7] == 10 && r.d.idsLost == 2,
          "hors mode machine avec id : %u id, premier %u, %u perdus", r.d.nIds, r.d.nIds ? r.d.ids[0] : 0,
          r.d.idsLost);
    CHECK(!r.poll(901) && r.w.pending() == 0, "liste et pertes remises a zero");
  }
  // Livraison pendant que la suivante demarre (meme tour) : deux livraisons.
  {
    WatchRun r;
    r.w.reset(0, 0);
    r.s.busy = r.s.targetBusy = true;
    r.s.pendingSince = 100;
    r.w.pendingId(1, 100);
    CHECK(!r.poll(101), "premiere consigne");
    r.s.delivered = 1;
    r.s.pendingSince = 400;  // nouvelle consigne posee dans le meme tour
    CHECK(r.poll(400) && r.d.issue == Issue::Delivered && r.d.nIds == 1 && r.d.waitMs == 300, "premiere livree");
    r.w.pendingId(2, 400);
    r.s.busy = r.s.targetBusy = false;
    r.s.delivered = 2;
    r.s.pendingSince = 0;
    CHECK(r.poll(650) && r.d.issue == Issue::Delivered && r.d.nIds == 1 && r.d.ids[0] == 2 && r.d.waitMs == 250,
          "seconde livree, attente depuis sa propre demande : %u", r.d.waitMs);
  }
}

// Transport reseau (section 10) : liste blanche, cache des reponses, cle.
static void testRemote() {
  struct Case {
    const char *cmd;
    bool ok;
  };
  static const Case kCases[] = {
      {"json 1", true},
      {"json 1 bail 30", true},
      {"json 1 bail 10", true},
      {"json 1 bail 120", true},
      {"json 1 bail 0", false},
      {"json 1 bail 9", false},
      {"json 1 bail 121", false},
      {"json 1 bail x", true},  // usage, dit par l'aiguillage
      // Zeros de tete : lus comme strtoul les lit (revue du 24/09).
      {"json 1 bail 0000000000", false},
      {"json 1 bail 0000000600", false},
      {"json 1 bail 00000000030", true},
      {"json 1 bail 99999999999", false},
      {"json periode 0000000200", false},
      {"json compteurs 0000000200", false},
      {"json reseau 00000001000", false},
      {"json periode 00000002000", true},
      {"json 0", true},
      {"json etat", true},
      {"json hello", true},
      {"json ping", true},
      {"json periode 2000", true},
      {"json periode 1999", false},
      {"json periode 0", false},
      {"json compteurs 0", true},
      {"json compteurs 4999", false},
      {"json compteurs 5000", true},
      {"json reseau 0", true},
      {"json reseau 9999", false},
      {"json reseau 10000", true},
      {"json trames 1", true},
      {"json log 0", true},
      {"json", false},
      {"json cle", false},
      {"json cle efface", false},
      {"json cle nouvelle 00", false},
      {"lampe on", true},
      {"lampe  niveau 200", true},
      {"lampe mode deux", true},
      {"lampe auto", true},
      {"lampe sync", true},
      {"lampe", false},
      {"lampe brut C5 A5", false},
      {"lampe stats raz", false},
      {"lampe oublie", false},
      {"led test", true},
      {"led stop", true},
      {"led", false},
      {"led test 2", false},
      {"reboot", false},
      {"decommission", false},
      {"matter med 0", false},
      {"txack 63FDF04F 5 C5A5", false},
      {"chiplog", false},
      {"", false},
      {"   ", false},
  };
  for (const Case &c : kCases) {
    const char *why = remoteRefusal(c.cmd);
    CHECK((why == nullptr) == c.ok, "remoteRefusal('%s') : %s", c.cmd, why ? why : "permise");
    CHECK(!why || strlen(why) <= kMsgMax, "msg trop long : %s", why);
  }

  // Un evenement du pont formate une fois, renumerote pour chaque session.
  gW.begin("thread", 0, 4242);
  gW.str("role", "child");
  bool fin = gW.finish();
  CHECK(fin && gW.setN(123456), "setN");
  CHECK(std::string((const char *)gW.data(), gW.size()) ==
            framed("{\"v\":1,\"t\":\"thread\",\"n\":123456,\"ms\":4242,\"role\":\"child\"}"),
        "setN : n remplace, le reste intact");
  CHECK(gW.setN(7) && std::string((const char *)gW.data(), gW.size()) ==
                          framed("{\"v\":1,\"t\":\"thread\",\"n\":7,\"ms\":4242,\"role\":\"child\"}"),
        "setN : plus court");
  gW.begin("thread", 0, 1);
  CHECK(!gW.setN(3), "setN : ligne pas fermee");

  // reponse.cmd ne renvoie jamais l'alea de 'json cle nouvelle'.
  char shown[kCmdTextMax + 1];
  copyCmd(shown, "json cle nouvelle 000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F");
  maskCmd(shown);
  CHECK(!strcmp(shown, "json cle nouvelle"), "maskCmd : %s", shown);
  copyCmd(shown, "json cle efface");
  maskCmd(shown);
  CHECK(!strcmp(shown, "json cle efface"), "maskCmd : rien a masquer");

  // Cache des reponses : un id repete recoit la meme reponse ; ni msg ni cle.
  ReplyCache cache;
  CHECK(!cache.find(1), "cache vide");
  Reply r;
  r.id = 5;
  r.cmd = "lampe niveau 200";
  r.code = "accepte";
  r.msg = "niveau 200 -> lum BA (gamma 2.00)";
  r.suite = Reply::SuiteDelivery;
  r.hasTarget = true;
  r.target = mk(true, F_LAMPS, rawFromLevel(200), 53);
  r.version = 13;
  cache.put(r);
  const Reply *got = cache.find(5);
  CHECK(got && got->suite == Reply::SuiteDelivery && got->version == 13 && !got->msg && !strcmp(got->cmd, r.cmd) &&
            got->cmd != r.cmd,
        "reponse gardee, msg retire, cmd copiee");
  Reply debut = r;
  debut.id = 6;
  debut.fin = false;
  cache.put(debut);
  CHECK(!cache.find(6), "etape debut jamais gardee");
  for (uint32_t id = 10; id < 10 + ReplyCache::kN; id++) {
    r.id = id;
    cache.put(r);
  }
  CHECK(!cache.find(5) && cache.find(10) && cache.find(10 + ReplyCache::kN - 1), "8 dernieres seulement");
  r.id = 12;
  r.code = "ok";
  cache.put(r);
  CHECK(cache.find(12) && !strcmp(cache.find(12)->code, "ok"), "meme id : la plus recente");
  cache.clear();
  CHECK(!cache.find(12), "cache vide apres clear");

  // Reponses de 'json cle' : cle (une fois) et empreinte.
  Reply k;
  k.id = 3;
  k.cmd = "json cle nouvelle";  // l'alea de l'app n'est jamais renvoye (json_mode.cpp)
  k.durMs = 4;
  k.key = "20D6D83D97ED44F2BBF8CE56389BD475CBE2B625CE6CE24768B6B4C1C625012F";
  k.hasKid = true;
  k.kid = "630DCD29";
  gCaptureOn = true;
  reply(gW, 40, 5000, k);
  expectLine(gW,
             "{\"v\":1,\"t\":\"reponse\",\"n\":40,\"ms\":5000,\"id\":3,\"etape\":\"fin\",\"cmd\":"
             "\"json cle nouvelle\",\"ok\":true,\"code\":\"ok\",\"duree_ms\":4,"
             "\"cle\":\"20D6D83D97ED44F2BBF8CE56389BD475CBE2B625CE6CE24768B6B4C1C625012F\",\"empreinte\":\"630DCD29\"}",
             "reponse json cle nouvelle");
  k = Reply();
  k.id = 4;
  k.cmd = "json cle";
  k.hasKid = true;  // sans cle : empreinte null
  reply(gW, 41, 5001, k);
  expectLine(gW,
             "{\"v\":1,\"t\":\"reponse\",\"n\":41,\"ms\":5001,\"id\":4,\"etape\":\"fin\",\"cmd\":\"json cle\","
             "\"ok\":true,\"code\":\"ok\",\"duree_ms\":0,\"empreinte\":null}",
             "reponse json cle sans cle");
  gCaptureOn = false;

  // Livraison : chaque origine ne voit que ses id ; ids perdus par origine.
  {
    WatchRun wr;
    wr.w.reset(0, 0);
    wr.s.busy = wr.s.targetBusy = true;
    wr.s.pendingSince = 1000;
    wr.w.pendingId(1, 1000, kUsb);
    wr.w.pendingId(2, 1000, 1);
    wr.w.pendingId(3, 1000, 2);
    for (uint32_t id = 10; id < 16; id++) wr.w.pendingId(id, 1000, 1);  // 9 id : le plus ancien (1, USB) sort
    CHECK(!wr.poll(1001), "rafale");
    wr.s = LampSample();
    wr.s.delivered = 1;
    CHECK(wr.poll(1200), "livraison");
    uint8_t usb = 0, one = 0, two = 0;
    for (uint8_t i = 0; i < wr.d.nIds; i++) {
      if (wr.d.origins[i] == kUsb) usb++;
      if (wr.d.origins[i] == 1) one++;
      if (wr.d.origins[i] == 2) two++;
    }
    CHECK(wr.d.nIds == kIdsMax && usb == 0 && one == 7 && two == 1, "origines : usb %u, 1 : %u, 2 : %u", usb, one, two);
    CHECK(wr.d.idsLostBy[kUsb] == 1 && wr.d.idsLostBy[1] == 0 && wr.d.idsLostBy[2] == 0 && wr.d.idsLost == 1,
          "ids perdus par origine");
    // Session partie : ses id sortent sans compter comme perdus.
    wr.s.busy = wr.s.targetBusy = true;
    wr.w.pendingId(20, 2000, 1);
    wr.w.pendingId(21, 2000, 2);
    wr.w.dropOrigin(1);
    CHECK(wr.w.pending() == 1, "dropOrigin");
    wr.s = LampSample();
    wr.s.delivered = 2;
    CHECK(wr.poll(2200) && wr.d.nIds == 1 && wr.d.ids[0] == 21 && wr.d.origins[0] == 2 && wr.d.idsLostBy[1] == 0,
          "livraison suivante : l'id de la session restante");
  }
}

int main(int argc, char **argv) {
  if (argc > 1) gCapture = fopen(argv[1], "wb");
  mapInit(2.0f);
  airOrder(kDefaultAddrReg, gAir);
  testWriter();
  testState();
  gCaptureOn = true;
  testRx();
  testEvents();
  testReply();
  testDelivery();
  gCaptureOn = false;
  testWorstCases();
  testIdPrefix();
  testAssembler();
  testRate();
  testQueue();
  testQueueDrain();
  testLease();
  testDeliveryWatch();
  testRemote();
  if (gCapture) fclose(gCapture);
  printf("%d verification(s) JSON, %d echec(s)\n", gChecks, gFails);
  return gFails ? 1 : 0;
}
