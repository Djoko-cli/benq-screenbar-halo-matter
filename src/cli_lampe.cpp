// Commandes 'lampe ...' : le chemin produit du pilote Halo 1 (plan du pilote
// Halo 1, G.2 et G.3). Les commandes d'etat passent par la consigne, comme
// Matter, attendent le repos du pilote et affichent une ligne de bilan.
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "halo.h"
#include "halo1_lamp.h"

using namespace halo1;
using Verdict = Halo1Radio::Verdict;

// Attente maximale d'une commande d'etat : 3 tours de 5 paquets, reprises a
// +1 s puis +2 s, soit ~5 s pour une lampe debranchee.
static constexpr uint32_t kWaitMs = 6000;

// ---------------------------------------------------------------------------
//  Analyse d'arguments
// ---------------------------------------------------------------------------

// Detache le mot suivant de 'p' (jamais nul, "" en fin de ligne).
static char *nextWord(char *&p) {
  while (*p == ' ') p++;
  char *w = p;
  while (*p && *p != ' ') p++;
  if (*p) *p++ = 0;
  return w;
}

static bool parseLong(const char *s, long &v, int base) {
  if (!*s) return false;
  char *end = nullptr;
  v = strtol(s, &end, base);
  return end && !*end;
}

// Exactement n octets en hexadecimal, d'un seul tenant.
static bool parseHexExact(const char *s, uint8_t *out, size_t n) {
  if (strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    // Chiffres seulement : strtoul accepterait un signe ('+5A5' lu 05 A5).
    if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1])) return false;
    char pair[3] = {s[2 * i], s[2 * i + 1], 0};
    char *end = nullptr;
    out[i] = (uint8_t)strtoul(pair, &end, 16);
    if (end != pair + 2) return false;
  }
  return true;
}

static bool parseOnOff(const char *s, bool &on) {
  if (!strcmp(s, "on") || !strcmp(s, "1")) on = true;
  else if (!strcmp(s, "off") || !strcmp(s, "0")) on = false;
  else return false;
  return true;
}

// Ajoute a une ligne sans jamais deborder.
static void add(char *buf, size_t n, size_t &w, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void add(char *buf, size_t n, size_t &w, const char *fmt, ...) {
  if (w + 1 >= n) return;
  va_list ap;
  va_start(ap, fmt);
  const int k = vsnprintf(buf + w, n - w, fmt, ap);
  va_end(ap);
  if (k > 0) w = (w + (size_t)k < n) ? w + (size_t)k : n - 1;
}

static const char *kindText(Kind k) {
  switch (k) {
    case Kind::Temp: return "temperature";
    case Kind::Bright: return "luminosite";
    case Kind::Auto: return "bouton A";
    case Kind::LampAck: return "accuse de la lampe";
    case Kind::Service: return "service";
    case Kind::Reserved: return "favori (bits 3/4)";
    case Kind::Invalid: return "invalide";
    case Kind::CrcBad: return "CRC faux";
  }
  return "?";
}

static const char *verdictText(Verdict v) {
  switch (v) {
    case Verdict::Ack: return "ACK";
    case Verdict::AckForeign: return "ACK+TRAME";
    case Verdict::MaxRt: return "MAX_RT";
    case Verdict::Timeout: return "DELAI";
    case Verdict::FifoRefused: return "FIFO";
  }
  return "?";
}

// ---------------------------------------------------------------------------
//  Bilan
// ---------------------------------------------------------------------------

struct Mark {
  uint32_t tx, giveUps, preempted, packets, acks;
};

static Mark mark() {
  return Mark{lamp.txCount(), lamp.stats.giveUps, lamp.stats.preempted, lamp.stats.packets, lamp.stats.acks};
}

// Une ligne : les paquets emis depuis la marque, regroupes par charge.
//   ok C5 A0 3/3 accuses (1650 1602 1611 us) -> cru : allumee deux lum A0 temp 35
static void printBilan(const Mark &m, bool idle) {
  struct Group {
    Payload pay;
    uint8_t n, acks, nus;
    uint16_t us[5];
    uint8_t bad[4];  // ACK+TRAME, MAX_RT, DELAI, FIFO
  };
  static constexpr uint8_t kGroups = 12;
  Group g[kGroups];
  uint8_t ng = 0;
  uint32_t acks = 0, gone = 0;
  for (uint32_t i = m.tx; i < lamp.txCount(); i++) {
    Halo1Lamp::TxLog e;
    if (!lamp.txLog(i, e)) {
      gone++;
      continue;
    }
    uint8_t k = 0;
    while (k < ng && g[k].pay != e.pay) k++;
    if (k == ng) {
      if (ng == kGroups) {
        gone++;
        continue;
      }
      g[ng++] = Group{e.pay, 0, 0, 0, {}, {}};
    }
    Group &x = g[k];
    if (x.n < 255) x.n++;
    if (e.v == Verdict::Ack) {
      x.acks++;
      acks++;
      if (x.nus < 5) x.us[x.nus++] = e.us;
    } else if (x.bad[(uint8_t)e.v - 1] < 255) {
      x.bad[(uint8_t)e.v - 1]++;
    }
  }
  const bool gaveUp = lamp.stats.giveUps != m.giveUps;
  const char *head = !idle ? "en cours" : gaveUp ? "ECHEC" : !ng ? (lamp.dirty() ? "differe" : "rien emis")
                     : acks ? "ok" : "ECHEC";
  char line[400];
  size_t w = 0;
  add(line, sizeof(line), w, "%s", head);
  const bool detail = ng <= 2;  // une rampe : charges et accuses seulement
  for (uint8_t k = 0; k < ng; k++) {
    const Group &x = g[k];
    add(line, sizeof(line), w, "%s %02X %02X %u/%u accuses", k ? "," : "", x.pay.flags, x.pay.value, x.acks, x.n);
    if (!detail) continue;
    add(line, sizeof(line), w, " (");
    for (uint8_t u = 0; u < x.nus; u++) add(line, sizeof(line), w, "%s%u", u ? " " : "", x.us[u]);
    if (x.nus) add(line, sizeof(line), w, " us");
    static const char *const kBad[4] = {"ACK+TRAME", "MAX_RT", "DELAI", "FIFO"};
    bool first = !x.nus;
    for (uint8_t b = 0; b < 4; b++) {
      if (!x.bad[b]) continue;
      add(line, sizeof(line), w, "%s%u %s", first ? "" : ", ", x.bad[b], kBad[b]);
      first = false;
    }
    add(line, sizeof(line), w, ")");
  }
  if (gone) add(line, sizeof(line), w, " (+%lu paquets hors journal)", (unsigned long)gone);
  char f[32], st[48];
  if (!ng && lamp.dirty()) {
    Halo1Lamp::describeFields(lamp.dirty() & (FLD_BRIGHT | FLD_TEMP), f, sizeof(f));
    add(line, sizeof(line), w, " : lampe eteinte, %s partira a l'allumage", f);
  }
  if (gaveUp) add(line, sizeof(line), w, " : injoignable, consigne abandonnee");
  if (!idle) add(line, sizeof(line), w, " : le pilote continue ('lampe' pour suivre)");
  Halo1Lamp::describe(lamp.believed(), st, sizeof(st));
  add(line, sizeof(line), w, " -> cru : %s", st);
  Serial.println(line);
}

static bool radioReady() {
  if (lamp.radio.present() && !lamp.lost()) return true;
  Serial.println(lamp.lost() ? "BM5602 perdu : rien n'est emis (nouvel essai de relance toutes les 60 s)."
                             : "BM5602 absent : rien n'est emis ('rfinit', puis 'lampe').");
  return false;
}

// Consigne, attente du repos, bilan.
static void runState(const State &s, uint8_t fields) {
  if (!radioReady()) return;
  const Mark m = mark();
  lamp.request(s, fields);
  printBilan(m, lamp.waitIdle(kWaitMs));
}

// Memes regles que Matter (resolveMatter), avec la memoire de selection.
static void runIntent(uint8_t bit, bool on) {
  MatterIntents in;
  in.has = bit;
  in.power = in.front = in.back = on;
  const Resolution r = resolveMatter(lamp.target(), in, lamp.memoryLamps());
  runState(r.target, r.fields);
}

// ---------------------------------------------------------------------------
//  Sous-commandes
// ---------------------------------------------------------------------------

static void cmdHelp() {
  Serial.println();
  Serial.println("=== Commandes 'lampe' (pilote Halo 1) ===");
  Serial.println("  lampe [etat]            consigne, etat cru, tranches, lien, radio");
  Serial.println("  lampe on | off          allumer / eteindre (memes regles que Matter)");
  Serial.println("  lampe avant on|off      lampe avant (idem 'lampe arriere on|off')");
  Serial.println("  lampe mode avant|arriere|deux  lampes allumees (et allumage)");
  Serial.println("  lampe lum <4C..FE>      luminosite brute, en hexa");
  Serial.println("  lampe niveau <1..254>   luminosite par la table gamma (niveau Matter)");
  Serial.println("  lampe temp <0..100>     temperature en decimal : 0 froid, 100 chaud");
  Serial.println("  lampe mired <153..370>  temperature en mireds");
  Serial.println("  lampe auto              bouton A (refuse lampe eteinte)");
  Serial.println("  lampe sync              renvoie tout ce qui est connu");
  Serial.println("  lampe rampe <de> <a> <pas> <ms>  simule un curseur (de, a, pas en hexa)");
  Serial.println("  lampe brut <XXYY> [n 1..10] [ecart 5..2000] [force]  charge brute, n paquets");
  Serial.println("  lampe croire <XXYY>     etat cru et consigne, sans rien emettre");
  Serial.println("  lampe rafale <n> [min] [max]  paquets par trame, accuses exiges, paquets au plus");
  Serial.println("  lampe ecart <5..2000>   ms entre deux paquets");
  Serial.println("  lampe ecoute 0|1        ecoute de fond de la telecommande (jamais d'accuse)");
  Serial.println("  lampe attends <ms>      laisse tourner le pilote seul (observer l'ecoute)");
  Serial.println("  lampe rx <rearm> <silence> | rx fort 0|1  reglages de l'ecoute (ms)");
  Serial.println("  lampe leger 0|1         bascule TX/RX sans reset (NON PROUVEE)");
  Serial.println("  lampe garde 0|1         Thread muet pendant chaque paquet (build Thread)");
  Serial.println("  lampe gamma <x.x>       courbe niveau -> luminosite (RAM)");
  Serial.println("  lampe trace 0|1         journal par evenement, jamais bloquant");
  Serial.println("  lampe stats [raz]       compteurs");
  Serial.println("  lampe regs              RFCH/DM1/RT1 relus, version de puce, instantanes");
  Serial.println("  lampe decode <16 hexa>  decode 8 octets lus en ecoute passive");
  Serial.println("  lampe autotest          auto-test du protocole, sans radio");
  Serial.println("  lampe adresse [8 hexa]  adresse de la lampe (ordre d'ecriture), en NVS");
  Serial.println("  lampe oublie | sauve    efface / ecrit l'etat sauve");
  Serial.println("  Valeurs affichees en hexa, comme sur l'air (temp 100 -> 64).");
}

static void cmdEtat() {
  lamp.printStatus(Serial);
  // Les outils de banc gardent leur canal et leur debit (NVS benqhalo) ; le
  // pilote les ignore : la lampe ne connait que le canal 5 a 125 kbps.
  const uint8_t ch = halo.channel(), rate = halo.dataRate();
  if (ch != kChannel || rate != bc5602::DATARATE_125K)
    Serial.printf("  outils      : canal %u, %s (NVS benqhalo) : ECART, ignore par le pilote\n", ch,
                  BenqHalo::dataRateName(rate));
  else
    Serial.println("  outils      : canal 5, 125 kbps (NVS benqhalo), comme le pilote");
}

static void cmdRampe(char *p) {
  long de, to, step, ms;
  const bool ok = parseLong(nextWord(p), de, 16) && parseLong(nextWord(p), to, 16) &&
                  parseLong(nextWord(p), step, 16) && parseLong(nextWord(p), ms, 10);
  if (!ok || de < kBrightMin || de > kBrightMax || to < kBrightMin || to > kBrightMax || step < 1 ||
      step > kBrightMax - kBrightMin || ms < 5 || ms > 2000) {
    Serial.println("Usage : lampe rampe <de 4C..FE> <a 4C..FE> <pas hexa> <ms 5..2000>");
    return;
  }
  if (!radioReady()) return;
  const Mark m = mark();
  uint32_t n = 0;
  for (long v = de;;) {
    State s = lamp.target();
    s.bright = (uint8_t)v;
    lamp.request(s, FLD_BRIGHT);
    n++;
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < (uint32_t)ms) {
      lamp.tick();
      delay(1);
    }
    if (v == to) break;
    v = de < to ? (v + step < to ? v + step : to) : (v - step > to ? v - step : to);
  }
  const bool idle = lamp.waitIdle(kWaitMs);
  Serial.printf("rampe : %lu consignes, %lu paquets, %lu accuses, %lu preemptions\n", (unsigned long)n,
                (unsigned long)(lamp.stats.packets - m.packets), (unsigned long)(lamp.stats.acks - m.acks),
                (unsigned long)(lamp.stats.preempted - m.preempted));
  printBilan(m, idle);
}

static void cmdBrut(char *p) {
  uint8_t pay[2];
  const bool okPay = parseHexExact(nextWord(p), pay, 2);
  long n = lamp.tuning.repeats, gap = lamp.tuning.gapMs;
  bool force = false, ok = okPay;
  uint8_t nums = 0;
  // Dans l'ordre : nombre de paquets, puis ecart ; 'force' n'importe ou.
  for (char *w = nextWord(p); ok && *w; w = nextWord(p)) {
    long v;
    if (!strcmp(w, "force")) {
      force = true;
    } else if (nums < 2 && parseLong(w, v, 10)) {
      if (nums++ == 0) n = v;
      else gap = v;
    } else {
      ok = false;
    }
  }
  if (!ok || n < 1 || n > 10 || gap < 5 || gap > 2000) {
    Serial.println("Usage : lampe brut <XXYY> [n 1..10] [ecart 5..2000 ms] [force]");
    return;
  }
  if (!radioReady()) return;
  const Mark m = mark();
  const char *err = lamp.sendRaw(Payload{pay[0], pay[1]}, force, (uint8_t)n, (uint16_t)gap);
  if (err) {
    Serial.println(err);
    return;
  }
  const bool idle = lamp.waitIdle((uint32_t)n * ((uint32_t)gap + 50) + kWaitMs);
  for (uint32_t i = m.tx; i < lamp.txCount(); i++) {
    Halo1Lamp::TxLog e;
    if (!lamp.txLog(i, e)) continue;
    Serial.printf("  paquet %2lu : %02X %02X  %-9s IRQ1 %02X  RT2 %02X  STATUS %02X  %5u us\n",
                  (unsigned long)(i - m.tx + 1), e.pay.flags, e.pay.value, verdictText(e.v), e.irq1, e.rt2,
                  e.status, e.us);
  }
  printBilan(m, idle);
}

static void cmdCroire(char *p) {
  uint8_t pay[2];
  if (!parseHexExact(nextWord(p), pay, 2)) {
    Serial.println("Usage : lampe croire <XXYY>");
    return;
  }
  const Payload pl{pay[0], pay[1]};
  const Kind k = kindOf(pl);
  if (k != Kind::Temp && k != Kind::Bright && k != Kind::Auto) {
    Serial.printf("refuse : %02X %02X est une trame %s ; seules luminosite, temperature et A se croient\n",
                  pay[0], pay[1], kindText(k));
    return;
  }
  // Jamais d'emission (G.2) : un reglage differe reste a livrer et partira
  // avec la prochaine consigne.
  lamp.believe(pl);
  char a[48], b[48], f[32];
  Halo1Lamp::describe(lamp.believed(), a, sizeof(a));
  Halo1Lamp::describe(lamp.target(), b, sizeof(b));
  Halo1Lamp::describeFields(lamp.dirty(), f, sizeof(f));
  Serial.printf("cru : %s ; consigne : %s ; a livrer : %s (rien emis%s)\n", a, b, f,
                lamp.busy() ? " ; la consigne deja en cours continue" : "");
}

static void cmdRafale(char *p) {
  Halo1Lamp::Tuning &t = lamp.tuning;
  char *w = nextWord(p);
  if (*w) {
    long n, mn = 0, mx = 0;
    const char *wm = nextWord(p);
    const char *wx = nextWord(p);
    bool ok = parseLong(w, n, 10) && n >= 1 && n <= 10;
    if (ok) {  // omis : valeurs de config.h ('rafale 3' revient aux reglages d'origine)
      mn = HALO1_MIN_ACKS < n ? HALO1_MIN_ACKS : n;
      mx = HALO1_MAX_ATTEMPTS > n ? HALO1_MAX_ATTEMPTS : n;
      if (*wm) ok = parseLong(wm, mn, 10) && mn >= 1 && mn <= n;
      if (ok && *wx) ok = parseLong(wx, mx, 10) && mx >= n && mx <= 20;
    }
    if (!ok) {
      Serial.println("Usage : lampe rafale <n 1..10> [min_accuses 1..n] [max n..20]");
      return;
    }
    t.repeats = (uint8_t)n;
    t.minAcks = (uint8_t)mn;
    t.maxAttempts = (uint8_t)mx;
  }
  Serial.printf("rafale : %u paquets par trame, %u accuses exiges, %u paquets au plus (RAM)\n", t.repeats,
                t.minAcks, t.maxAttempts);
}

static void cmdRx(char *p) {
  Halo1Radio::Tuning &t = lamp.radio.tuning;
  char *w = nextWord(p);
  if (!strcmp(w, "fort")) {
    bool on;
    if (!parseOnOff(nextWord(p), on)) {
      Serial.println("Usage : lampe rx fort 0|1");
      return;
    }
    t.strongRearm = on;
  } else if (*w) {
    long rearm, silence;
    if (!parseLong(w, rearm, 10) || !parseLong(nextWord(p), silence, 10) || rearm < 10 || silence < 100 ||
        silence > 60000 || !ChipWatch::calmVisible((uint32_t)rearm, (uint32_t)silence)) {
      // Hors de calmVisible, une piece calme ne donne plus assez de
      // rearmements periodiques pour la guerison apres surdite (halo1_watch.h).
      Serial.printf("Usage : lampe rx <rearmement 10..%u ms> <silence 100..60000 ms, > 2 x rearmement>\n",
                    (unsigned)ChipWatch::kCalmMaxRearmMs);
      return;
    }
    t.rearmMs = (uint16_t)rearm;
    t.silenceMs = (uint16_t)silence;
  }
  Serial.printf("ecoute : rearmement %u ms, reconfiguration apres %u ms de silence, rearmement fort %s (RAM)\n",
                t.rearmMs, t.silenceMs, t.strongRearm ? "oui (NON PROUVE)" : "non");
}

static void cmdAttends(char *p) {
  long ms;
  if (!parseLong(nextWord(p), ms, 10) || ms < 1 || ms > 600000) {
    Serial.println("Usage : lampe attends <ms 1..600000>");
    return;
  }
  const Halo1Lamp::Stats s0 = lamp.stats;
  const uint32_t sil0 = lamp.radio.stats.silenceReconf;
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < (uint32_t)ms) {
    lamp.tick();
    delay(1);
  }
  const Halo1Lamp::Stats &s = lamp.stats;
  char st[48];
  Halo1Lamp::describe(lamp.believed(), st, sizeof(st));
  Serial.printf("attente de %ld ms : %lu trames (%lu d'etat, %lu A, %lu accuses lampe, %lu CRC faux), "
                "%lu reconf. de silence%s -> cru : %s\n",
                ms, (unsigned long)(s.rxFrames - s0.rxFrames), (unsigned long)(s.rxState - s0.rxState),
                (unsigned long)(s.rxAuto - s0.rxAuto), (unsigned long)(s.rxLampAcks - s0.rxLampAcks),
                (unsigned long)(s.rxCrcBad - s0.rxCrcBad),
                (unsigned long)(lamp.radio.stats.silenceReconf - sil0),
                lamp.listening() ? "" : " (ecoute coupee : 'lampe ecoute 1')", st);
}

static void cmdGamma(char *p) {
  char *w = nextWord(p);
  if (*w) {
    char *end = nullptr;
    const float g = strtof(w, &end);
    if (end == w || *end || !(g >= 0.2f && g <= 5.0f)) {
      Serial.println("Usage : lampe gamma <0.2..5.0>");
      return;
    }
    mapInit(g);
  }
  Serial.printf("gamma %.2f (RAM) : niveau 1 -> %02X, 64 -> %02X, 127 -> %02X, 191 -> %02X, 254 -> %02X\n",
                (double)mapGamma(), rawFromLevel(1), rawFromLevel(64), rawFromLevel(127), rawFromLevel(191),
                rawFromLevel(254));
}

static void cmdRegs() {
  if (!lamp.radio.present()) {
    Serial.println("BM5602 absent.");
    return;
  }
  lamp.settleRadio();  // pas de lecture pendant le reset : SPI 3 fils, GIO2 muet
  uint8_t c[3];
  if (!lamp.radio.readConfig(c)) {
    Serial.println("  relecture impossible : puce en SPI 3 fils (reset coupe), reessayer apres 'lampe attends 100'");
  } else if (!lamp.radio.configured()) {
    // Ecoute coupee (diag), ou juste apres un outil de banc : une mise en veille
    // ne configure rien, un ECART ne voudrait rien dire.
    Serial.printf("  RFCH %02X  DM1 %02X  RT1 %02X  : puce pas encore configuree par le pilote (valeurs de halo.begin "
                  "ou du dernier outil), version puce 0x%06lX\n",
                  c[0], c[1], c[2], (unsigned long)halo.radio.chipVersion());
  } else {
    const bool ok = c[0] == kChannel && c[1] == (bc5602::ADDR_LEN_4 | bc5602::DATARATE_125K) && c[2] == 0x73;
    Serial.printf("  RFCH %02X  DM1 %02X  RT1 %02X  (attendu 05 82 73 : %s), version puce 0x%06lX\n", c[0], c[1],
                  c[2], ok ? "conforme" : "ECART", (unsigned long)halo.radio.chipVersion());
  }
  Halo1Radio::Snapshot s[4];
  const uint8_t n = lamp.radio.snapshots(s, 4);
  if (!n) Serial.println("  aucun instantane (pris avant une reconfiguration de silence, d'echec TX ou de verif.)");
  static const char *const kWhy[] = {"mode", "silence", "echec TX", "verif."};
  const uint32_t now = millis();
  for (uint8_t i = 0; i < n; i++) {
    const Halo1Radio::Snapshot &x = s[i];
    Serial.printf("  #%u il y a %lu ms, %s : STA1 %02X IRQ1 %02X STATUS %02X MASK %02X CE %02X CFG1 %02X "
                  "RFCH %02X DM1 %02X PKT1 %02X ENAA %02X DPL1 %02X DPL2 %02X RXPW0 %02X RT1 %02X\n",
                  i + 1, (unsigned long)(now - x.atMs), x.why < 4 ? kWhy[x.why] : "?", x.sta1, x.irq1, x.status,
                  x.mask, x.ce, x.cfg1, x.rfch, x.dm1, x.pkt1, x.enaa, x.dpl1, x.dpl2, x.rxpw0, x.rt1);
  }
}

static void cmdDecode(char *p) {
  uint8_t raw[8];
  if (!parseHexExact(nextWord(p), raw, 8)) {
    Serial.println("Usage : lampe decode <16 chiffres hexa>, ex. 08627F030C800000");
    return;
  }
  const uint8_t *air = lamp.radio.air();
  const AirFrame f = decodeAir(raw, air);
  if (f.len > 4) {
    Serial.printf("  len %u, PID %u, NO_ACK %u : plus de 4 octets de charge, illisible sur 64 bits\n", f.len,
                  f.pid, f.noAck);
    return;
  }
  char pay[16];
  size_t w = 0;
  pay[0] = 0;
  for (uint8_t i = 0; i < f.len; i++) add(pay, sizeof(pay), w, "%s%02X", i ? " " : "", f.pay[i]);
  if (!f.len) add(pay, sizeof(pay), w, "vide");
  if (f.crcOk) {
    Serial.printf("  len %u, PID %u, NO_ACK %u, charge %s, CRC %04X OK -> %s\n", f.len, f.pid, f.noAck, pay,
                  f.crc, kindText(classify(f)));
  } else {
    uint8_t re[8];
    encodeAir(air, f.pid, f.noAck, f.pay, f.len, re);
    Serial.printf("  len %u, PID %u, NO_ACK %u, charge %s, CRC lu %04X, attendu %04X : FAUX\n", f.len, f.pid,
                  f.noAck, pay, f.crc, decodeAir(re, air).crc);
  }
  Serial.printf("  (adresse sur l'air %02X %02X %02X %02X)\n", air[0], air[1], air[2], air[3]);
}

static void cmdAdresse(char *p) {
  char *w = nextWord(p);
  if (*w) {
    uint8_t a[4];
    if (!parseHexExact(w, a, 4)) {
      Serial.println("Usage : lampe adresse [8 chiffres hexa, ordre d'ecriture], ex. 4FF0FD63");
      return;
    }
    if (!addressAllowed(a)) {
      Serial.println("refuse : adresse interdite (00000000, FFFFFFFF ou adresse d'appairage)");
      return;
    }
    if (!lamp.setAddress(a)) {
      Serial.println("ECHEC : ecriture NVS impossible, adresse inchangee");
      return;
    }
  }
  const uint8_t *r = lamp.address(), *air = lamp.radio.air();
  Serial.printf("adresse de la lampe : %02X %02X %02X %02X (ordre d'ecriture), sur l'air %02X %02X %02X %02X\n",
                r[0], r[1], r[2], r[3], air[0], air[1], air[2], air[3]);
}

// ---------------------------------------------------------------------------
//  Commandes d'etat asynchrones (lignes de l'app avec id)
// ---------------------------------------------------------------------------

bool lampeIsAsync(const char *arg) {
  static const char *const kAsync[] = {"on",     "off",  "avant", "arriere", "mode", "lum",
                                       "niveau", "temp", "mired", "auto",    "sync"};
  while (*arg == ' ') arg++;
  size_t n = 0;
  while (arg[n] && arg[n] != ' ') n++;
  for (const char *k : kAsync)
    if (strlen(k) == n && !strncmp(arg, k, n)) return true;
  return false;
}

// Regles de runIntent, sans attente : consigne resolue comme un ordre Matter.
static uint8_t intentTarget(uint8_t bit, bool on, State &s) {
  MatterIntents in;
  in.has = bit;
  in.power = in.front = in.back = on;
  const Resolution r = resolveMatter(lamp.target(), in, lamp.memoryLamps());
  s = r.target;
  return r.fields;
}

void cmdLampeAsync(char *arg, LampeAsync &r) {
  char *p = arg;
  const char *sub = nextWord(p);
  long v;
  bool on;
  State s = lamp.target();
  uint8_t fields = 0;
  enum { kRequest, kAuto, kSync } what = kRequest;
  r.ok = false;
  r.code = "usage";
  r.msg = nullptr;
  if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
    fields = intentTarget(IN_POWER, !strcmp(sub, "on"), s);
  } else if (!strcmp(sub, "avant") || !strcmp(sub, "arriere")) {
    if (!parseOnOff(nextWord(p), on)) {
      r.msg = !strcmp(sub, "avant") ? "avant : on|off" : "arriere : on|off";
      return;
    }
    fields = intentTarget(!strcmp(sub, "avant") ? IN_FRONT : IN_BACK, on, s);
  } else if (!strcmp(sub, "mode")) {
    const char *m = nextWord(p);
    const uint8_t lamps = !strcmp(m, "avant") ? F_FRONT : !strcmp(m, "arriere") ? F_BACK
                          : !strcmp(m, "deux")  ? F_LAMPS : 0;
    if (!lamps) {
      r.msg = "mode : avant|arriere|deux";
      return;
    }
    s.power = true;
    s.lamps = lamps;
    fields = FLD_FLAGS;
  } else if (!strcmp(sub, "lum")) {
    if (!parseLong(nextWord(p), v, 16) || v < kBrightMin || v > kBrightMax) {
      r.msg = "lum : 4C..FE, en hexa";
      return;
    }
    s.bright = (uint8_t)v;
    fields = FLD_BRIGHT;
  } else if (!strcmp(sub, "niveau")) {
    if (!parseLong(nextWord(p), v, 10) || v < 1 || v > 254) {
      r.msg = "niveau : 1..254";
      return;
    }
    s.bright = rawFromLevel((uint8_t)v);
    fields = FLD_BRIGHT;
    snprintf(r.buf, sizeof(r.buf), "niveau %ld -> lum %02X (gamma %.2f)", v, s.bright, (double)mapGamma());
    r.msg = r.buf;
  } else if (!strcmp(sub, "temp")) {
    if (!parseLong(nextWord(p), v, 10) || v < 0 || v > kTempMax) {
      r.msg = "temp : 0..100, en decimal";
      return;
    }
    s.temp = (uint8_t)v;
    fields = FLD_TEMP;
  } else if (!strcmp(sub, "mired")) {
    if (!parseLong(nextWord(p), v, 10) || v < kMiredCold || v > kMiredWarm) {
      r.msg = "mired : 153..370";
      return;
    }
    s.temp = tempFromMired((uint16_t)v);
    fields = FLD_TEMP;
    snprintf(r.buf, sizeof(r.buf), "mired %ld -> temp %u/100", v, s.temp);
    r.msg = r.buf;
  } else if (!strcmp(sub, "auto")) {
    what = kAuto;
  } else if (!strcmp(sub, "sync")) {
    what = kSync;
  } else {
    r.msg = "lampe on|off|avant|arriere|mode|lum|niveau|temp|mired|auto|sync";
    return;
  }
  // Comme radioReady(), sans texte : rien n'est demande.
  if (lamp.lost() || !lamp.radio.present()) {
    const bool lost = lamp.lost();
    r.code = lost ? "radio_perdue" : "radio_absente";
    r.msg = lost ? "BM5602 perdu : rien n'est emis (nouvel essai de relance toutes les 60 s)"
                 : "BM5602 absent : rien n'est emis ('rfinit', puis 'lampe')";
    return;
  }
  if (what == kAuto) {
    if (!lamp.pressAuto()) {
      r.code = "refuse";
      r.msg = "lampe eteinte : A n'est pas emis";
      return;
    }
  } else if (what == kSync) {
    lamp.reassert();
  } else {
    lamp.request(s, fields);  // sans champ (rien a changer) : aucune demande
  }
  r.ok = true;
  // Occupe : une livraison suivra. Sinon, un reglage peut rester differe (lampe
  // eteinte : la luminosite partira a l'allumage), sans livraison pour lui.
  r.code = lamp.busy() ? "accepte" : lamp.dirty() ? "differe" : "ok";
}

// ---------------------------------------------------------------------------
//  Aiguillage
// ---------------------------------------------------------------------------

void cmdLampe(char *arg) {
  char *p = arg;
  char *sub = nextWord(p);
  long v;
  bool on;
  if (!*sub || !strcmp(sub, "etat")) {
    cmdEtat();
  } else if (!strcmp(sub, "help") || !strcmp(sub, "?")) {
    cmdHelp();
  } else if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
    runIntent(IN_POWER, !strcmp(sub, "on"));
  } else if (!strcmp(sub, "avant") || !strcmp(sub, "arriere")) {
    if (!parseOnOff(nextWord(p), on)) Serial.printf("Usage : lampe %s on|off\n", sub);
    else runIntent(!strcmp(sub, "avant") ? IN_FRONT : IN_BACK, on);
  } else if (!strcmp(sub, "mode")) {
    const char *m = nextWord(p);
    const uint8_t lamps = !strcmp(m, "avant") ? F_FRONT : !strcmp(m, "arriere") ? F_BACK
                          : !strcmp(m, "deux")  ? F_LAMPS : 0;
    if (!lamps) {
      Serial.println("Usage : lampe mode avant|arriere|deux");
    } else {
      State s = lamp.target();
      s.power = true;
      s.lamps = lamps;
      runState(s, FLD_FLAGS);
    }
  } else if (!strcmp(sub, "lum")) {
    if (!parseLong(nextWord(p), v, 16) || v < kBrightMin || v > kBrightMax) {
      Serial.println("Usage : lampe lum <4C..FE, hexa>");
    } else {
      State s = lamp.target();
      s.bright = (uint8_t)v;
      runState(s, FLD_BRIGHT);
    }
  } else if (!strcmp(sub, "niveau")) {
    if (!parseLong(nextWord(p), v, 10) || v < 1 || v > 254) {
      Serial.println("Usage : lampe niveau <1..254>");
    } else {
      State s = lamp.target();
      s.bright = rawFromLevel((uint8_t)v);
      Serial.printf("niveau %ld -> lum %02X (gamma %.2f)\n", v, s.bright, (double)mapGamma());
      runState(s, FLD_BRIGHT);
    }
  } else if (!strcmp(sub, "temp")) {
    if (!parseLong(nextWord(p), v, 10) || v < 0 || v > kTempMax) {
      Serial.println("Usage : lampe temp <0..100, decimal> (0 = le plus froid)");
    } else {
      State s = lamp.target();
      s.temp = (uint8_t)v;
      runState(s, FLD_TEMP);
    }
  } else if (!strcmp(sub, "mired")) {
    if (!parseLong(nextWord(p), v, 10) || v < kMiredCold || v > kMiredWarm) {
      Serial.println("Usage : lampe mired <153..370>");
    } else {
      State s = lamp.target();
      s.temp = tempFromMired((uint16_t)v);
      Serial.printf("mired %ld -> temp %02X (%u/100)\n", v, s.temp, s.temp);
      runState(s, FLD_TEMP);
    }
  } else if (!strcmp(sub, "auto")) {
    if (radioReady()) {
      const Mark m = mark();
      if (!lamp.pressAuto()) Serial.println("refuse : lampe eteinte");
      else printBilan(m, lamp.waitIdle(kWaitMs));
    }
  } else if (!strcmp(sub, "sync")) {
    if (radioReady()) {
      const Mark m = mark();
      lamp.reassert();
      printBilan(m, lamp.waitIdle(kWaitMs));
    }
  } else if (!strcmp(sub, "rampe")) {
    cmdRampe(p);
  } else if (!strcmp(sub, "brut")) {
    cmdBrut(p);
  } else if (!strcmp(sub, "croire")) {
    cmdCroire(p);
  } else if (!strcmp(sub, "rafale")) {
    cmdRafale(p);
  } else if (!strcmp(sub, "ecart")) {
    char *w = nextWord(p);
    if (*w && (!parseLong(w, v, 10) || v < 5 || v > 2000)) Serial.println("Usage : lampe ecart <5..2000 ms>");
    else {
      if (*w) lamp.tuning.gapMs = (uint16_t)v;
      Serial.printf("ecart entre deux paquets : %u ms (RAM)\n", lamp.tuning.gapMs);
    }
  } else if (!strcmp(sub, "ecoute")) {
    char *w = nextWord(p);
    if (*w && !parseOnOff(w, on)) Serial.println("Usage : lampe ecoute 0|1");
    else {
      if (*w) lamp.setListening(on);
      Serial.printf("ecoute de fond : %s\n", lamp.listening() ? "oui (jamais d'accuse)" : "non (puce en veille)");
    }
  } else if (!strcmp(sub, "attends")) {
    cmdAttends(p);
  } else if (!strcmp(sub, "rx")) {
    cmdRx(p);
  } else if (!strcmp(sub, "leger")) {
    char *w = nextWord(p);
    if (*w && !parseOnOff(w, on)) Serial.println("Usage : lampe leger 0|1");
    else {
      if (*w) lamp.radio.tuning.lightSwitch = on;
      Serial.printf("bascule legere TX/RX sans reset : %s (RAM)\n",
                    lamp.radio.tuning.lightSwitch ? "oui (NON PROUVEE)" : "non");
    }
  } else if (!strcmp(sub, "garde")) {
    char *w = nextWord(p);
    if (!lamp.radio.hasAirGuard()) {
      Serial.println("garde Thread : absente de ce build (Matter sur Thread seulement), rien a regler");
    } else if (*w && !parseOnOff(w, on)) {
      Serial.println("Usage : lampe garde 0|1");
    } else {
      if (*w) lamp.radio.tuning.airGuard = on;
      Serial.printf("garde Thread autour de chaque paquet : %s (RAM ; compteurs : 'lampe stats')\n",
                    lamp.radio.tuning.airGuard ? "oui" : "non");
    }
  } else if (!strcmp(sub, "gamma")) {
    cmdGamma(p);
  } else if (!strcmp(sub, "trace")) {
    char *w = nextWord(p);
    if (*w && !parseOnOff(w, on)) Serial.println("Usage : lampe trace 0|1");
    else {
      if (*w) lamp.setTrace(on);
      Serial.printf("trace : %s\n", lamp.tracing() ? "oui" : "non");
    }
  } else if (!strcmp(sub, "stats")) {
    if (!strcmp(nextWord(p), "raz")) {
      lamp.clearStats();
      Serial.println("compteurs remis a zero");
    } else {
      lamp.printStats(Serial);
    }
  } else if (!strcmp(sub, "regs")) {
    cmdRegs();
  } else if (!strcmp(sub, "decode")) {
    cmdDecode(p);
  } else if (!strcmp(sub, "autotest")) {
    char msg[96];
    const int n = selfTest(msg, sizeof(msg));
    if (!n) Serial.printf("autotest : ok (gamma %.2f)\n", (double)mapGamma());
    else Serial.printf("autotest : %d echec(s), premier : %s\n", n, msg);
  } else if (!strcmp(sub, "adresse")) {
    cmdAdresse(p);
  } else if (!strcmp(sub, "oublie")) {
    lamp.forget();
    char st[48];
    Halo1Lamp::describe(lamp.believed(), st, sizeof(st));
    Serial.printf("etat sauve efface : cru = consigne = %s (rien emis)\n", st);
  } else if (!strcmp(sub, "sauve")) {
    const uint32_t before = lamp.stats.persisted;
    lamp.persistNow();
    Serial.println(lamp.stats.persisted != before ? "etat cru sauve" : "etat sauve deja a jour");
  } else {
    Serial.printf("Sous-commande inconnue : \"%s\". Tape 'lampe help'.\n", sub);
  }
}
