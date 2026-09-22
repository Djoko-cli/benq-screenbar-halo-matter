#include "cli.h"

#include <stdlib.h>
#include <string.h>

#include "halo.h"
#include "swd.h"

// Definie dans main.cpp.
extern const char *resetReasonText();
#ifndef DIAG_ONLY
#include "matter_bridge.h"
#include "net.h"
#endif

// Definis dans main.cpp
extern bool chipLogging;
void setChipLogging(bool on);

// ---------------------------------------------------------------------------
//  Analyse d'arguments
// ---------------------------------------------------------------------------

// Renvoie le nombre d'octets decodes, ou -1 si la chaine n'est pas de
// l'hexadecimal valide. Espaces, ':', '-' et ',' sont ignores.
static int parseHexBytes(const char *s, uint8_t *out, int maxBytes) {
  int n = 0, nibbles = 0;
  uint8_t cur = 0;
  for (; *s; s++) {
    char c = *s;
    if (c == ' ' || c == ':' || c == '-' || c == ',') continue;
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    cur = (uint8_t)((cur << 4) | v);
    if (++nibbles == 2) {
      if (n >= maxBytes) return -1;
      out[n++] = cur;
      cur = 0;
      nibbles = 0;
    }
  }
  return nibbles ? -1 : n;
}

// Detache le premier mot de `s` et renvoie le reste (jamais nul).
static char *splitWord(char *s) {
  char *sp = strchr(s, ' ');
  if (!sp) return s + strlen(s);
  *sp++ = 0;
  while (*sp == ' ') sp++;
  return sp;
}

// ---------------------------------------------------------------------------
//  Commandes
// ---------------------------------------------------------------------------

static void cmdHelp() {
  Serial.println();
  Serial.println("=== Commandes ===");
  Serial.println("  help                  cette aide");
  Serial.println("  info                  materiel, configuration et etat de la lampe");
  Serial.println("  matter                etat Matter, code d'appairage");
  Serial.println("  poll                  interroge la lampe maintenant");
  Serial.println("  debug                 bascule les traces RF");
  Serial.println("  regs                  dump des registres du BC5602");
  Serial.println("  rfinit                re-teste le module apres correction du cablage");
  Serial.println("  calib                 relance la calibration du VCO");
  Serial.println("  regcfg                recharge les registres analogiques recommandes");
  Serial.println("  spectre               balaye 2400-2483 MHz et mesure le RSSI reel");
  Serial.println("  rxdiag                trace la machine d'etats pendant l'entree en RX");
  Serial.println("  chasse                trouve le canal de la telecommande par son energie");
  Serial.println("  guet [canal]          se gare sur un canal et guette les salves (RSSI rapide)");
  Serial.println("  rxseq                 teste toutes les sequences d'entree en RX");
  Serial.println("  gio                   cherche une sortie de bits demodules sur GIO2");
  Serial.println("  direct [ms]           mode direct DIR_EN : bits bruts sans adresse");
  Serial.println("  rxdirect [ms]         reception en mode direct : entree RX + selecteurs GIO2");
  Serial.println("  bande                 ou emet la telecommande : 84 canaux, repos puis molette");
  Serial.println("  cause                 pourquoi la carte a redemarre la derniere fois");
  Serial.println("  appaire [s] [canal]   capture pendant l'appairage ; canal 0 = les 3 du FCC");
  Serial.println("  preambule [ms]        cale le correlateur sur le preambule, balaie X sur 256");
  Serial.println("  presence              entend-on la telecommande, et sur quel canal");
  Serial.println("  rafale [s] [seuil]    duree des rafales -> en deduit le debit");
  Serial.println("  boucle [n]            etalonnage a une carte : deux modules sur le meme bus");
  Serial.println("  etalon tx [s] [1|2]   ETALONNAGE : cette carte EMET (preambule 1 ou 2 o.)");
  Serial.println("  etalon rx [s]         ETALONNAGE a deux cartes : cette carte ECOUTE");
  Serial.println("  autotest              verifie la carte maillon par maillon, sans partenaire");
  Serial.println("  sniffspi [s]          ecoute passive du bus SPI d'un appareil tiers");
  Serial.println("  swd                   cherche SWDIO et interroge le microcontroleur");
  Serial.println("  syncpayload [ms]      le correlateur sait-il se caler au milieu d'une trame ?");
  Serial.println("  ancre [s] [1|2] [K] [lum]  chasse ancree ; K et lum arriere imposables");
  Serial.println("  gio3 [ms]             balaie les 16 valeurs du selecteur GIO3 en reception");
  Serial.println("  debitgio [ms]         trouve le debit de la source, sans connaitre l'adresse");
  Serial.println("  canalgio [ms]         trouve le canal demodulable, sans connaitre l'adresse");
  Serial.println("  canalpico [ms]        meme balayage, avec la sequence du pilote tiers");
  Serial.println("  largeur [ms]          balaie les adresses de 3, 4 et 5 octets x 84 canaux");
  Serial.println("  modem [banque reg]    balaie CFO1, ou un registre donne, en guettant GIO3");
  Serial.println("  survie                les valeurs recommandees survivent-elles au reset ?");
  Serial.println("  holtek [0|1]          reappliquer les reglages analogiques apres chaque reset");
  Serial.println("  fil                   le fil GIO3 fait-il contact ? (test electrique)");
  Serial.println("  discrimine [ms]       canal 5 : la telecommande, ou le Wi-Fi 1 ?");
  Serial.println("  forme [ms]            polarite et longueur du preambule : 12 formes");
  Serial.println("  amont [ms] [adr]      ecoute a la maniere du projet amont, SANS reset");
  Serial.println("  gio3check [ms]        ces sorties sont-elles avant ou apres le correlateur ?");
  Serial.println("  gio3bits [sel]        lit l'adresse dans le flux demodule (defaut : 14)");
  Serial.println("  taptest [s]           suivi en direct du contact des 3 fils d'ecoute");
  Serial.println("  debit [125|250|500]   debit radio, persiste ; sans argument, affiche");
  Serial.println("  amble [1|2]           longueur de preambule attendue");
  Serial.println("  aw [3|4|5]            longueur d'adresse attendue");
  Serial.println("  chiplog               bascule les logs de la pile Matter");
  Serial.println();
  Serial.println("  addr                  affiche l'adresse de communication");
  Serial.println("  addr 11223344         definit l'adresse (ordre d'ecriture, cf. find)");
  Serial.println("  tail 0102             octets de queue du payload");
  Serial.println("  tail ffff             desactive le controle des octets de queue");
  Serial.println("  chan 5                canal radio : 5=2405 MHz, 46=2446, 75=2475");
  Serial.println("  erase                 efface la configuration radio");
  Serial.println();
  Serial.println("  find                  cherche l'adresse (regle la telecommande sur");
  Serial.println("                        10 % lampe arriere + 3925 K, puis bouge un reglage)");
  Serial.println("  find 25 4000          idem avec d'autres valeurs (luminosite %, Kelvin)");
  Serial.println("  find x550f0a          idem avec un mot de synchro brut de 3 octets");
  Serial.println("  find 100 2700 sweep   idem en balayant 3 debits x 3 canaux (135 s)");
  Serial.println("  pair                  ecoute sur l'adresse d'appairage E2 08 00 B0");
  Serial.println("  sniff                 mode sniffer sur l'adresse configuree");
  Serial.println("  normal                retour au mode normal");
  Serial.println("  send 0300320FA0...    envoie un payload brut de 10 octets (20 hexa)");
  Serial.println();
  Serial.println("  wifi <ssid> <mdp>     identifiants Wi-Fi (cibles sans commissioning BLE)");
  Serial.println("  decommission          retire toutes les fabriques Matter");
  Serial.println("  reboot                redemarre");
}

static void cmdAddress(char *arg) {
  if (!*arg) {
    const uint8_t *a = halo.address();
    Serial.printf("Adresse (ordre d'ecriture) : %02X %02X %02X %02X\n", a[0], a[1], a[2], a[3]);
    Serial.printf("Adresse (ordre sur l'air)  : %02X %02X %02X %02X\n", a[3], a[2], a[1], a[0]);
    return;
  }
  uint8_t v[4];
  if (parseHexBytes(arg, v, 4) != 4) {
    Serial.println("Format attendu : addr 11223344 (8 caracteres hexa)");
    return;
  }
  halo.setAddress(v);
  Serial.printf("Adresse enregistree : %02X %02X %02X %02X\n", v[0], v[1], v[2], v[3]);
}

static void cmdTail(char *arg) {
  if (!*arg) {
    Serial.printf("Octets de queue : %02X %02X\n", halo.tail()[0], halo.tail()[1]);
    return;
  }
  uint8_t v[2];
  if (parseHexBytes(arg, v, 2) != 2) {
    Serial.println("Format attendu : tail 0102 (4 caracteres hexa)");
    return;
  }
  halo.setTail(v[0], v[1]);
  Serial.printf("Octets de queue enregistres : %02X %02X%s\n", v[0], v[1],
                (v[0] == 0xFF && v[1] == 0xFF) ? " (controle desactive)" : "");
}

static void cmdChannel(char *arg) {
  if (!*arg) {
    Serial.printf("Canal : %u (%u MHz)\n", halo.channel(), 2400 + halo.channel());
    return;
  }
  int ch = atoi(arg);
  if (ch < 0 || ch > 83) {
    Serial.println("Canal hors plage (0-83). Le BenQ utilise 5, 46 ou 75.");
    return;
  }
  halo.setChannel((uint8_t)ch);
  Serial.printf("Canal : %d (%d MHz)\n", ch, 2400 + ch);
}

static void cmdFind(char *arg) {
  uint8_t sync[3];

  // Mot-cle optionnel "sweep" : balaie les trois canaux du dossier FCC.
  bool sweep = false;
  char *kw = strstr(arg, "sweep");
  if (kw) {
    sweep = true;
    *kw = 0;  // retire le mot-cle avant l'analyse numerique
  }

  if (*arg == 'x' || *arg == 'X') {
    if (parseHexBytes(arg + 1, sync, 3) != 3) {
      Serial.println("Format attendu : find x550f0a (6 caracteres hexa)");
      return;
    }
  } else {
    long brightness = 10, kelvin = 3925;
    if (*arg) {
      char *end = nullptr;
      brightness = strtol(arg, &end, 10);
      kelvin = strtol(end, nullptr, 10);
      if (brightness < HALO_BRIGHT_MIN || brightness > HALO_BRIGHT_MAX || kelvin < HALO_CT_MIN_K ||
          kelvin > HALO_CT_MAX_K) {
        Serial.printf("Valeurs attendues : luminosite %d-%d %%, temperature %d-%d K\n", HALO_BRIGHT_MIN,
                      HALO_BRIGHT_MAX, HALO_CT_MIN_K, HALO_CT_MAX_K);
        return;
      }
    }
    // Sur l'air le payload contient [luminosite arriere, CT poids fort, CT poids
    // faible] ; l'adresse s'ecrit dans l'ordre inverse.
    sync[0] = (uint8_t)(kelvin & 0xFF);
    sync[1] = (uint8_t)(kelvin >> 8);
    sync[2] = (uint8_t)brightness;
    Serial.printf("Mot de synchro deduit de : lampe arriere %ld %%, %ld K\n", brightness, kelvin);
  }

  Serial.printf("Synchro (ordre d'ecriture) : %02X %02X %02X\n", sync[0], sync[1], sync[2]);
  Serial.printf("  capture %d s%s. Regle CES valeurs exactes a la telecommande,\n",
                sweep ? 135 : 60, sweep ? ", en balayant 3 debits x 3 canaux" : "");
  Serial.println("puis actionne un bouton toutes les 2-3 s pour la faire emettre.");
  halo.findAddressBegin(sync, sweep ? 135000 : 60000, sweep);  // 5 passes de 9 combos
}

static void cmdSendRaw(char *arg) {
  uint8_t pkt[10];
  if (parseHexBytes(arg, pkt, 10) != 10) {
    Serial.println("Format attendu : send + 20 caracteres hexa (10 octets)");
    return;
  }
  if (!halo.addressConfigured()) {
    Serial.println("Adresse non configuree.");
    return;
  }
  halo.setMode(HaloMode::Normal);
  halo.prepareToTransfer();
  bool sent = halo.sendWithAck(pkt);

  uint8_t ack[10];
  halo.readAck(ack);
  Serial.printf("Emission : %s\n", sent ? "acquittee" : "PAS d'acquittement");
  Serial.print("ACK      : ");
  for (int i = 0; i < 10; i++) Serial.printf("%02X ", ack[i]);
  Serial.printf(" %s\n", halo.validate(ack) ? "(valide)" : "(non conforme au format attendu)");
  halo.prepareToSniff();
}

#ifndef DIAG_ONLY
static void cmdWifi(char *arg) {
  char *pass = splitWord(arg);
  if (!*arg) {
    Serial.println("Format attendu : wifi <ssid> <mot-de-passe>");
    return;
  }
  netSetCredentials(arg, pass);
}
#endif

// ---------------------------------------------------------------------------
//  Dispatch
// ---------------------------------------------------------------------------

static void handleLine(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;
  char *arg = splitWord(line);

  if (!strcmp(line, "help") || !strcmp(line, "?")) cmdHelp();
  else if (!strcmp(line, "info")) {
    halo.printInfo(Serial);
#ifndef DIAG_ONLY
    netPrintStatus(Serial);
#endif
  }
#ifndef DIAG_ONLY
  else if (!strcmp(line, "matter")) matterPrintStatus(Serial);
#endif
  else if (!strcmp(line, "poll")) {
    if (!halo.addressConfigured()) {
      Serial.println("Adresse non configuree : lance d'abord 'find'.");
    } else {
      Serial.printf("Interrogation : %s\n",
                    halo.pollNow() ? "reponse valide" : "pas de reponse exploitable");
      halo.printState(Serial);
    }
  } else if (!strcmp(line, "debug")) {
    halo.debug = !halo.debug;
    Serial.printf("Debug %s\n", halo.debug ? "active" : "desactive");
  } else if (!strcmp(line, "chiplog")) {
    setChipLogging(!chipLogging);
    Serial.printf("Logs de la pile Matter %s\n", chipLogging ? "actives" : "coupes");
  } else if (!strcmp(line, "amble")) {
    if (*arg) halo.setPreambleTwoBytes(atoi(arg) == 2);
    uint8_t cfo1 = halo.radio.readRegister(bc5602::B0_CFO1 | bc5602::CMD_READ_REGISTER);
    Serial.printf("  preambule : %d octet(s) demande, CFO1=0x%02X -> %d octet(s) effectif\n",
                  halo.preambleTwoBytes() ? 2 : 1, cfo1, (cfo1 & 0x40) ? 2 : 1);
    Serial.println("  (reglage persistant : reapplique apres chaque reset)");
  } else if (!strcmp(line, "aw")) {
    // DM1 bits 7-6 : longueur d'adresse. 01=3, 10=4, 11=5 octets. On n'a
    // jamais essaye 5 octets.
    uint8_t dm1 = halo.radio.readRegister(bc5602::REG_DM1 | bc5602::CMD_READ_REGISTER);
    if (*arg) {
      int n = atoi(arg);
      uint8_t bits = (n == 3) ? 0x40 : (n == 5) ? 0xC0 : 0x80;
      dm1 = (uint8_t)((dm1 & 0x3F) | bits);
      halo.radio.writeRegister(bc5602::REG_DM1 | bc5602::CMD_WRITE_REGISTER, dm1);
    }
    dm1 = halo.radio.readRegister(bc5602::REG_DM1 | bc5602::CMD_READ_REGISTER);
    uint8_t aw = (dm1 >> 6) & 0x03;
    Serial.printf("  adresse : %d octets  (DM1=0x%02X)\n", aw == 1 ? 3 : aw == 3 ? 5 : 4, dm1);
    Serial.println("  (non persistant, et sans adresse de 5 octets connue ce reglage");
    Serial.println("   ne sert qu'a verifier que le registre accepte la valeur)");
  } else if (!strcmp(line, "gio")) {
    halo.setMode(HaloMode::Normal);
    halo.probeGioFunctions(Serial);
  } else if (!strcmp(line, "debit")) {
    const long v = strtol(arg, nullptr, 10);
    if (v == 125) halo.setDataRate(bc5602::DATARATE_125K);
    else if (v == 250) halo.setDataRate(bc5602::DATARATE_250K);
    else if (v == 500) halo.setDataRate(bc5602::DATARATE_500K);
    else if (*arg) {
      Serial.println("Valeurs acceptees : 125, 250 ou 500.");
    }
    Serial.print("Debit radio : ");
    Serial.println(BenqHalo::dataRateName(halo.dataRate()));
  } else if (!strcmp(line, "taptest")) {
    uint32_t secs = 20;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 3 && v <= 120) secs = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.tapTest(Serial, secs);
  } else if (!strcmp(line, "gio3bits")) {
    long sel = strtol(arg, nullptr, 10);
    if (sel < 0 || sel > 15) sel = 14;
    halo.setMode(HaloMode::Normal);
    halo.captureGio3Bits(Serial, (uint8_t)sel);
  } else if (!strcmp(line, "gio3check")) {
    uint32_t dwell = 2000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 500 && v <= 10000) dwell = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.checkGio3Correlator(Serial, dwell);
  } else if (!strcmp(line, "holtek")) {
    if (*arg) halo.setHoltekTuning(strtol(arg, nullptr, 10) != 0);
    Serial.print("Reglages analogiques Holtek reappliques apres reset : ");
    Serial.println(halo.holtekTuning() ? "oui" : "non");
  } else if (!strcmp(line, "survie")) {
    halo.setMode(HaloMode::Normal);
    halo.compareAfterReset(Serial);
  } else if (!strcmp(line, "modem")) {
    char *end = nullptr;
    long bank = -1, reg = 0;
    if (*arg) {
      bank = strtol(arg, &end, 10);
      if (end && *end) reg = strtol(end, &end, 16);
      if (bank < 0 || bank > 2) bank = -1;
    }
    uint32_t dwell = 800;
    if (end && *end) {
      const long v = strtol(end, nullptr, 10);
      if (v >= 100 && v <= 10000) dwell = (uint32_t)v;
    }
    halo.setMode(HaloMode::Normal);
    halo.sweepModemRegister(Serial, (int)bank, (uint8_t)reg, dwell);
  } else if (!strcmp(line, "largeur")) {
    uint32_t dwell = 600;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 200 && v <= 5000) dwell = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.sweepAddressWidths(Serial, dwell);
  } else if (!strcmp(line, "canalpico")) {
    uint32_t dwell = 1500;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 300 && v <= 10000) dwell = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.sweepChannelsPico(Serial, 0, 83, dwell);
  } else if (!strcmp(line, "canalgio")) {
    // canalgio [ms] [premier] [dernier] : restreindre la plage permet un temps
    // de pose bien plus long, donc d'attraper un emetteur qui parle par a-coups.
    char *end = nullptr;
    uint32_t dwell = 700;
    const long v = strtol(arg, &end, 10);
    // Camper sur un seul canal demande des dizaines de secondes : une source a
    // faible rapport cyclique se rate en une seconde.
    if (v >= 200 && v <= 120000) dwell = (uint32_t)v;
    long from = 0, to = 83;
    if (end && *end) {
      from = strtol(end, &end, 10);
      if (end && *end) to = strtol(end, nullptr, 10);
    }
    if (from < 0 || from > 83) from = 0;
    if (to < from || to > 83) to = 83;
    halo.setMode(HaloMode::Normal);
    halo.probeChannelByGio3(Serial, (uint8_t)from, (uint8_t)to, dwell);
  } else if (!strcmp(line, "debitgio")) {
    uint32_t dwell = 3000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 500 && v <= 120000) dwell = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.probeRateByGio3(Serial, dwell);
  } else if (!strcmp(line, "amont")) {
    // amont [ms] [adresse hex 8 chiffres] : sequence de reception du projet
    // amont, sans reset logiciel. Sans adresse, celle du Halo 2.
    char *end = nullptr;
    uint32_t dwell = 20000;
    const long v = strtol(arg, &end, 10);
    if (v >= 1000 && v <= 120000) dwell = (uint32_t)v;
    uint8_t a[4] = {0x9C, 0xEA, 0xBB, 0x86};
    while (end && *end == ' ') end++;
    if (end && strlen(end) >= 8) {
      for (uint8_t i = 0; i < 4; i++) {
        char pair[3] = {end[i * 2], end[i * 2 + 1], 0};
        a[i] = (uint8_t)strtol(pair, nullptr, 16);
      }
    }
    uint8_t plen = 13;
    if (end && strlen(end) > 8) {
      const long pl = strtol(end + 8, nullptr, 10);
      if (pl >= 1 && pl <= 32) plen = (uint8_t)pl;
    }
    halo.setMode(HaloMode::Normal);
    halo.listenLikeUpstream(Serial, dwell, a, plen);
  } else if (!strcmp(line, "forme")) {
    uint32_t dwell = 20000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 1000 && v <= 120000) dwell = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.probePreambleShape(Serial, dwell);
  } else if (!strcmp(line, "discrimine")) {
    uint32_t ph = 20000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 3000 && v <= 120000) ph = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.discriminateWifi(Serial, ph);
  } else if (!strcmp(line, "fil")) {
    halo.setMode(HaloMode::Normal);
    halo.checkGio3Wire(Serial);
  } else if (!strcmp(line, "gio3")) {
    uint32_t dwell = 1500;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 300 && v <= 10000) dwell = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.sweepGio3(Serial, dwell);
  } else if (!strcmp(line, "ancre")) {
    char *end = nullptr;
    long secs = strtol(arg, &end, 10);
    if (secs < 20 || secs > 1800) secs = 180;
    long grp = 1, kelvin = 0;
    if (end && *end) grp = strtol(end, &end, 10);
    if (end && *end) kelvin = strtol(end, &end, 10);
    if (kelvin < 2000 || kelvin > 7000) kelvin = 0;
    long back = -1;
    if (end && *end) back = strtol(end, nullptr, 10);
    if (back < 0 || back > 100) back = -1;
    halo.setMode(HaloMode::Normal);
    halo.huntAnchored(Serial, (uint32_t)secs, (grp == 2) ? 1 : 0, 60, (uint16_t)kelvin,
                      (int16_t)back);
  } else if (!strcmp(line, "syncpayload")) {
    uint32_t dwell = 3000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 500 && v <= 20000) dwell = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.validatePayloadSync(Serial, dwell);
  } else if (!strcmp(line, "swd")) {
    halo.setMode(HaloMode::Normal);
    Serial.println();
    Serial.println("=== Interrogation du microcontroleur par SWD ===");
    Serial.println("  Cablage sur le connecteur J5, avec les memes fils qu'avant :");
    Serial.printf("    trou 1 (masse)  -> GND\n");
    Serial.printf("    trou 3 (SWCLK)  -> IO%u   (l'ancien fil SCK)\n", (unsigned)PIN_SWD_CLK);
    Serial.println("    un trou inconnu -> IO20  (l'ancien fil SDIO)");
    Serial.println("    un autre        -> IO14  (l'ancien fil CSN)");
    Serial.println("  SWDIO est l'un des trous 2, 4, 5 ou 6. Deux sont testes a");
    Serial.println("  chaque passage : essaie 2 et 4, puis 5 et 6 si besoin.");
    Serial.println("  La telecommande doit etre ALIMENTEE, piles branchees.");
    Serial.println();
    halo.releaseSpiBus();
    static const uint8_t candidates[] = {20, 14};
    swd::probeCandidates(Serial, PIN_SWD_CLK, candidates, sizeof(candidates));
    halo.restoreSpiBus();
    Serial.println();
  } else if (!strcmp(line, "sniffspi")) {
    uint32_t secs = 60;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 5 && v <= 600) secs = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.sniffSpiBus(Serial, secs);
  } else if (!strcmp(line, "autotest")) {
    halo.setMode(HaloMode::Normal);
    halo.selfTest(Serial);
  } else if (!strcmp(line, "etalon")) {
    while (*arg == ' ') arg++;
    const bool tx = (arg[0] == 't');
    const bool rx = (arg[0] == 'r');
    if (!tx && !rx) {
      Serial.println("Usage : 'etalon tx' sur la carte qui emet, 'etalon rx' sur l'autre.");
    } else {
      const char *num = arg;
      while (*num && *num != ' ') num++;
      char *end = nullptr;
      const long v = strtol(num, &end, 10);
      // Argument facultatif : longueur du preambule emis, 1 ou 2 octets.
      const long amb = (end && *end) ? strtol(end, nullptr, 10) : 2;
      halo.setMode(HaloMode::Normal);
      if (tx)
        halo.calibrationBeacon(Serial, (v >= 10 && v <= 600) ? (uint32_t)v : 120,
                               (amb == 1) ? 1 : 2);
      else halo.calibrationListen(Serial, (v >= 5 && v <= 120) ? (uint32_t)v : 15);
    }
  } else if (!strcmp(line, "boucle")) {
    uint16_t n = 50;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 5 && v <= 500) n = (uint16_t)v;
    halo.setMode(HaloMode::Normal);
    halo.loopbackTest(Serial, n);
  } else if (!strcmp(line, "rafale")) {
    uint32_t secs = 20;
    long thr = 60;
    char *end = nullptr;
    const long v = strtol(arg, &end, 10);
    if (v >= 5 && v <= 120) secs = (uint32_t)v;
    if (end && *end) {
      const long t = strtol(end, nullptr, 10);
      if (t >= 20 && t <= 120) thr = t;
    }
    halo.setMode(HaloMode::Normal);
    halo.measureBursts(Serial, secs, (uint8_t)thr);
  } else if (!strcmp(line, "presence")) {
    halo.setMode(HaloMode::Normal);
    halo.probePresence(Serial);
  } else if (!strcmp(line, "preambule")) {
    uint32_t dwell = 500;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 50 && v <= 5000) dwell = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.huntByPreamble(Serial, dwell);
  } else if (!strcmp(line, "appaire")) {
    uint32_t secs = 180;
    char *end = nullptr;
    const long v = strtol(arg, &end, 10);
    if (v >= 20 && v <= 900) secs = (uint32_t)v;
    // Argument facultatif : se limiter a un seul canal.
    const long ch = (end && *end) ? strtol(end, nullptr, 10) : 0;
    halo.setMode(HaloMode::Normal);
    halo.capturePairing(Serial, secs, (ch > 0 && ch < 84) ? (uint8_t)ch : 0);
  } else if (!strcmp(line, "cause")) {
    Serial.print("  cause du dernier demarrage : ");
    Serial.println(resetReasonText());
  } else if (!strcmp(line, "bande")) {
    halo.setMode(HaloMode::Normal);
    halo.sweepBand(Serial);
  } else if (!strcmp(line, "rxdirect")) {
    uint32_t win = 2000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 200 && v <= 30000) win = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.probeDirectRx(Serial, win);
  } else if (!strcmp(line, "direct")) {
    uint32_t win = 3000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 200 && v <= 30000) win = (uint32_t)v;
    halo.setMode(HaloMode::Normal);
    halo.probeDirectMode(Serial, win);
  } else if (!strcmp(line, "rxseq")) {
    halo.setMode(HaloMode::Normal);
    halo.probeRxSequences(Serial);
  } else if (!strcmp(line, "guet")) {
    halo.setMode(HaloMode::Normal);
    Serial.println();
    Serial.println("=== Guet sur canal fixe ===");
    if (*arg) {
      halo.watchChannel(Serial, (uint8_t)atoi(arg), 15000);
    } else {
      Serial.println("  ACTIONNE LA TELECOMMANDE SANS ARRET, 8 s par canal.");
      Serial.flush();
      for (uint8_t ch : {RF_CHANNEL_1, RF_CHANNEL_2, RF_CHANNEL_3}) halo.watchChannel(Serial, ch);
    }
    Serial.println("  (des pics +12dB = un emetteur proche ; aucun = rien sur ce canal)");
  } else if (!strcmp(line, "chasse")) {
    halo.setMode(HaloMode::Normal);
    halo.huntRemote(Serial);
  } else if (!strcmp(line, "rxdiag")) {
    halo.setMode(HaloMode::Normal);
    halo.diagnoseRx(Serial);
  } else if (!strcmp(line, "spectre")) {
    halo.setMode(HaloMode::Normal);
    halo.scanSpectrum(Serial);
  } else if (!strcmp(line, "regcfg")) {
    if (!halo.radio.present()) {
      Serial.println("BM5602 absent.");
    } else {
      Serial.println("Chargement des valeurs recommandees Holtek...");
      uint8_t bad = halo.radio.registerConfigure(&Serial);
      Serial.printf("  ecarts de relecture : %u registre(s)\n", bad);
      halo.setMode(HaloMode::Normal);
    }
  } else if (!strcmp(line, "calib")) {
    if (!halo.radio.present()) {
      Serial.println("BM5602 absent.");
    } else {
      halo.radio.command(bc5602::CMD_LIGHT_SLEEP);
      bool xtal = halo.radio.waitCrystalReady(100);
      Serial.printf("  quartz pret : %s\n", xtal ? "oui" : "NON (XCLK_RDY jamais pose)");
      bool ok = xtal && halo.radio.calibrate();
      Serial.printf("  calibration VCO : %s\n", ok ? "terminee" : "ECHEC (ACAL_EN pas retombe)");
      halo.setMode(HaloMode::Normal);  // reconfigure la radio derriere
      halo.printInfo(Serial);
    }
  } else if (!strcmp(line, "rfinit")) {
    Serial.printf("  re-initialisation BM5602 : %s\n", halo.begin() ? "OK" : "echec");
    halo.printInfo(Serial);
  } else if (!strcmp(line, "regs")) {
    if (halo.radio.present()) halo.radio.dumpRegisters(Serial);
    else Serial.println("BM5602 absent.");
  } else if (!strcmp(line, "addr")) cmdAddress(arg);
  else if (!strcmp(line, "tail")) cmdTail(arg);
  else if (!strcmp(line, "chan")) cmdChannel(arg);
  else if (!strcmp(line, "erase")) {
    uint8_t zero[4] = {0, 0, 0, 0};
    halo.setAddress(zero);
    halo.setTail(0x01, 0x02);
    halo.setChannel(RF_CHANNEL_1);
    Serial.println("Configuration radio effacee.");
  } else if (!strcmp(line, "find")) cmdFind(arg);
  else if (!strcmp(line, "pair")) {
    halo.startSniffer(HALO_PAIRING_ADDRESS);
    Serial.println("Ecoute sur l'adresse d'appairage E2 08 00 B0 (sur l'air).");
    Serial.println("Lance maintenant l'appairage entre la telecommande et la lampe.");
    Serial.println("'normal' pour revenir au mode normal.");
  } else if (!strcmp(line, "sniff")) {
    if (!halo.addressConfigured()) {
      Serial.println("Adresse non configuree : 'find' d'abord, ou 'pair' pour l'appairage.");
    } else {
      halo.startSniffer(nullptr);
      Serial.println("Mode sniffer. 'normal' pour revenir au mode normal.");
    }
  } else if (!strcmp(line, "normal")) {
    halo.setMode(HaloMode::Normal);
    Serial.println("Mode normal.");
  } else if (!strcmp(line, "send")) cmdSendRaw(arg);
#ifndef DIAG_ONLY
  else if (!strcmp(line, "wifi")) cmdWifi(arg);
  else if (!strcmp(line, "decommission")) {
    Serial.println("Retrait de toutes les fabriques Matter, redemarrage...");
    matterDecommissionNow();
  }
#endif
  else if (!strcmp(line, "reboot")) {
    Serial.println("Redemarrage...");
    delay(100);
    ESP.restart();
  } else {
    Serial.printf("Commande inconnue : \"%s\". Tape 'help'.\n", line);
  }
}

// ---------------------------------------------------------------------------

static char buf[128];
static uint8_t len = 0;

void cliBegin() {
  Serial.println("Tape 'help' pour la liste des commandes.");
  Serial.print("> ");
}

void cliPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      Serial.println();
      buf[len] = 0;
      handleLine(buf);
      Serial.flush();  // l'USB CDC du C6 perd des octets si on enchaine trop vite
      len = 0;
      Serial.print("> ");
      continue;
    }
    if (c == 8 || c == 127) {  // retour arriere
      if (len) {
        len--;
        Serial.print("\b \b");
      }
      continue;
    }
    if (len < sizeof(buf) - 1) {
      buf[len++] = c;
      Serial.print(c);
    }
  }
}
