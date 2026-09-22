#include "cli.h"
#include "cc2500.h"

// Salves capturees : 24 x 256 bits, soit 2 ms de flux chacune a 125 kbit/s.
#define CC_BURSTS 24
#define CC_BURST_BITS 256

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
  Serial.println("  ccpins s mi mo cs g0 g2 pa rx   broches du module CC2500");
  Serial.println("  cc                    le CC2500 repond-il ? numero de piece et version");
  Serial.println("  ccdiag                diagnostic electrique du module CC2500");
  Serial.println("  ccraw                 le bus SPI du CC2500 transporte-t-il quelque chose ?");
  Serial.println("  ccrx [ms]             CC2500 en ecoute brute sur 2405 MHz");
  Serial.println("  cccap [motif] [bits]  capture le flux brut et y cherche un motif de 32 bits");
  Serial.println("  ccfind [bits] [run] [n]  trouve une adresse SANS la connaitre, n captures cumulees");
  Serial.println("  ccfront [ms]          table de verite mesuree de PA_EN et RX_EN");
  Serial.println("  ccpres [ms]           le CC2500 entend-il la source ? distribution du RSSI");
  Serial.println("  cccrc [bits] [n] [lo] [hi]  trouve les trames par leur CRC, sans hypothese");
  Serial.println("  ccbit [seuil] [n]     duree reelle d'un bit, mesuree en mode asynchrone");
  Serial.println("  cccommun [seuil]      ce que deux salves ont en commun : aucune hypothese");
  Serial.println("  ccdump [seuil] [n]    vidage brut des salves, pour analyse sur l'ordinateur");
  Serial.println("  ccflux [bits] [n]     capture CONTINUE videe telle quelle (pas de declenchement)");
  Serial.println("  ccfreq [span] [pas] [ms]  sur quelle frequence la source est-elle centree ?");
  Serial.println("  ccoff [seuil] [ms]    ecart de porteuse mesure par la puce (FREQEST)");
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
  } else if (!strcmp(line, "ccpins")) {
    // ccpins sck miso mosi csn gdo0 gdo2 paen rxen
    long v[8];
    char *p = arg;
    uint8_t n = 0;
    while (n < 8 && *p) {
      char *e = nullptr;
      const long x = strtol(p, &e, 10);
      if (e == p) break;
      v[n++] = x;
      p = e;
      while (*p == ' ') p++;
    }
    if (n == 8) {
      for (uint8_t i = 0; i < 8; i++) ccPins[i] = (uint8_t)v[i];
      Serial.println("Broches CC2500 enregistrees. Lance 'cc' pour tester.");
    }
    Serial.printf("CC2500 : SCK=%u MISO=%u MOSI=%u CSN=%u GDO0=%u GDO2=%u PA_EN=%u RX_EN=%u\n",
                  ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[4], ccPins[5], ccPins[6],
                  ccPins[7]);
  } else if (!strcmp(line, "cc")) {
    ccIdentify(Serial);
  } else if (!strcmp(line, "ccdiag")) {
    ccDiagnose(Serial);
  } else if (!strcmp(line, "ccraw")) {
    ccRawProbe(Serial);
  } else if (!strcmp(line, "ccoff")) {
    char *end = nullptr;
    long thr = -55;
    uint32_t d = 20000;
    if (*arg) thr = strtol(arg, &end, 10);
    if (thr < -90 || thr > -20) thr = -55;
    if (end && *end) {
      const long v = strtol(end, nullptr, 10);
      if (v >= 1000 && v <= 120000) d = (uint32_t)v;
    }
    ccFreqOffset(Serial, (int)thr, d);
  } else if (!strcmp(line, "ccfreq")) {
    char *end = nullptr;
    long span = 600, step = 50;
    uint32_t d = 1500;
    if (*arg) span = strtol(arg, &end, 10);
    if (span < 50 || span > 2000) span = 600;
    if (end && *end) step = strtol(end, &end, 10);
    if (step < 10 || step > 400) step = 50;
    if (end && *end) {
      const long v = strtol(end, nullptr, 10);
      if (v >= 200 && v <= 20000) d = (uint32_t)v;
    }
    ccFreqSweep(Serial, span, step, d);
  } else if (!strcmp(line, "ccflux")) {
    char *end = nullptr;
    uint32_t nb = 32768;
    long p = 4;
    const long v = strtol(arg, &end, 10);
    if (v >= 1024 && v <= 32768) nb = (uint32_t)v;
    if (end && *end) p = strtol(end, nullptr, 10);
    if (p < 1 || p > 40) p = 4;
    ccStream(Serial, nb, (uint8_t)p);
  } else if (!strcmp(line, "ccdump")) {
    char *end = nullptr;
    long thr = -50;
    long n = 24;
    if (*arg) thr = strtol(arg, &end, 10);
    if (thr < -90 || thr > -10) thr = -50;
    if (end && *end) n = strtol(end, nullptr, 10);
    if (n < 1 || n > CC_BURSTS) n = CC_BURSTS;
    ccDumpBursts(Serial, (int)thr, (uint8_t)n);
  } else if (!strcmp(line, "cccommun")) {
    long thr = -55;
    if (*arg) thr = strtol(arg, nullptr, 10);
    if (thr < -90 || thr > -20) thr = -55;
    ccCommonRuns(Serial, (int)thr);
  } else if (!strcmp(line, "ccbit")) {
    char *end = nullptr;
    long thr = -60;
    if (*arg) thr = strtol(arg, &end, 10);
    if (thr < -90 || thr > -20) thr = -60;
    long n = 200;
    if (end && *end) n = strtol(end, nullptr, 10);
    if (n < 1 || n > 2000) n = 200;
    ccPulseWidths(Serial, (int)thr, (uint16_t)n);
  } else if (!strcmp(line, "cccrc")) {
    char *end = nullptr;
    uint32_t nb = 32768;
    const long v = strtol(arg, &end, 10);
    if (v >= 1024 && v <= 32768) nb = (uint32_t)v;
    long rep = 10;
    if (end && *end) rep = strtol(end, &end, 10);
    if (rep < 1 || rep > 120) rep = 10;
    long lo = 60, hi = 200;
    if (end && *end) lo = strtol(end, &end, 10);
    if (end && *end) hi = strtol(end, nullptr, 10);
    if (lo < 24 || lo > 400) lo = 60;
    if (hi < lo || hi > 400) hi = 200;
    ccCrcHunt(Serial, nb, (uint8_t)rep, (uint16_t)lo, (uint16_t)hi);
  } else if (!strcmp(line, "ccpres")) {
    uint32_t d = 20000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 2000 && v <= 120000) d = (uint32_t)v;
    ccPresence(Serial, d);
  } else if (!strcmp(line, "ccfront")) {
    uint32_t d = 3000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 500 && v <= 20000) d = (uint32_t)v;
    ccFrontEnd(Serial, d);
  } else if (!strcmp(line, "ccfind")) {
    char *end = nullptr;
    uint32_t nb = 32768;
    const long v = strtol(arg, &end, 10);
    if (v >= 1024 && v <= 32768) nb = (uint32_t)v;
    long run = 12;
    if (end && *end) run = strtol(end, &end, 10);
    if (run < 6 || run > 40) run = 12;
    long rep = 1;
    if (end && *end) rep = strtol(end, nullptr, 10);
    if (rep < 1 || rep > 120) rep = 1;
    ccFindAddress(Serial, nb, (uint8_t)run, (uint8_t)rep);
  } else if (!strcmp(line, "cccap")) {
    // cccap [motif hex 8 chiffres] [nbits] -- par defaut l'adresse sur l'air
    // de la balise, E1 22 33 44.
    uint32_t pat = 0xE1223344UL;
    char *end = arg;
    if (*arg && strlen(arg) >= 8) pat = (uint32_t)strtoul(arg, &end, 16);
    uint32_t nb = 32768;
    if (end && *end) {
      const long v = strtol(end, nullptr, 10);
      if (v >= 1024 && v <= 32768) nb = (uint32_t)v;
    }
    ccCapture(Serial, pat, nb);
  } else if (!strcmp(line, "ccrx")) {
    uint32_t dwell = 2000;
    const long v = strtol(arg, nullptr, 10);
    if (v >= 200 && v <= 60000) dwell = (uint32_t)v;
    ccListen(Serial, dwell);
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

// Broches du CC2500, modifiables a chaud : on ignore ce que la deuxieme carte
// expose, et un reflash pour changer un numero de broche coute une manipulation
// de plus a chaque essai.
uint8_t ccPins[8] = {PIN_CC_SCK,  PIN_CC_MISO, PIN_CC_MOSI,  PIN_CC_CSN,
                     PIN_CC_GDO0, PIN_CC_GDO2, PIN_CC_PA_EN, PIN_CC_RX_EN};

// Premier jalon : la puce repond-elle ? PARTNUM vaut 0x80 sur un CC2500 et
// 0x00 sur un CC1101 -- ce qui tranchera du meme coup ce que cache le blob
// noir du module. Puis on balaye les quatre combinaisons de l'etage d'entree
// en lisant le RSSI : la table de verite du RFX2402E se mesure au lieu de se
// supposer, et une erreur de polarite rendrait le module sourd sans rien dire.
void ccIdentify(Print &out) {
  out.println();
  out.println("=== Le CC2500 repond-il ? ===");
  out.printf("  SCK=%u MISO=%u MOSI=%u CSN=%u GDO0=%u GDO2=%u PA_EN=%u RX_EN=%u\n", ccPins[0],
             ccPins[1], ccPins[2], ccPins[3], ccPins[4], ccPins[5], ccPins[6], ccPins[7]);
  Serial.flush();

  const bool ok = radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7]);
  const uint8_t part = radio2.partNumber();
  const uint8_t ver = radio2.version();
  out.printf("  PARTNUM 0x%02X, VERSION 0x%02X  -> %s\n", part, ver,
             part == 0x80   ? "CC2500 confirme"
             : part == 0x00 ? "CC1101 (sub-GHz !) ou bus muet"
                            : "puce inconnue");
  if (!ok) {
    out.println("  La puce ne repond pas. Verifie l'alimentation et le cablage,");
    out.println("  puis corrige les broches avec 'ccpins'.");
    return;
  }
  out.printf("  MARCSTATE 0x%02X (%s)\n", radio2.marcState(),
             cc2500::marcStateName(radio2.marcState()));
}

// Diagnostic electrique du CC2500, avant d'accuser le cablage au juge.
// Le datasheet dit que SO passe en haute impedance quand CSN est haut, et
// qu'il est PILOTE (bas quand la puce est prete) quand CSN est bas. On teste
// donc la broche contre les resistances internes de l'ESP32 dans les deux
// situations : c'est la seule mesure qui distingue un fil debranche d'une
// puce qui refuse de parler.
static void ccPinLevels(Print &out, const char *what, uint8_t pin) {
  pinMode(pin, INPUT_PULLUP);
  delay(3);
  uint8_t up = 0;
  for (uint8_t i = 0; i < 20; i++) { up += digitalRead(pin) ? 1 : 0; delayMicroseconds(200); }
  pinMode(pin, INPUT_PULLDOWN);
  delay(3);
  uint8_t dn = 0;
  for (uint8_t i = 0; i < 20; i++) { dn += digitalRead(pin) ? 1 : 0; delayMicroseconds(200); }
  pinMode(pin, INPUT);
  const char *verdict = (dn >= 18) ? "PILOTEE a 1" : (up <= 2 ? "PILOTEE a 0" : "libre (rien ne la pilote)");
  out.printf("    %-28s tirage haut %2u/20  tirage bas %2u/20  -> %s\n", what, up, dn, verdict);
}

void ccDiagnose(Print &out) {
  const uint8_t sck = ccPins[0], miso = ccPins[1], mosi = ccPins[2], csn = ccPins[3];
  out.println();
  out.println("=== Diagnostic electrique du module CC2500 ===");
  out.println("  SO est en haute impedance quand CSN est haut, et pilote quand");
  out.println("  CSN est bas. Si SO reste libre dans les DEUX cas, le fil ou");
  out.println("  l'alimentation sont en cause, pas le logiciel.");
  out.println();

  pinMode(csn, OUTPUT);
  digitalWrite(csn, HIGH);
  delay(5);
  out.println("  CSN haut (la puce doit lacher le bus) :");
  ccPinLevels(out, "SO / MISO", miso);

  digitalWrite(csn, LOW);
  delay(5);
  out.println("  CSN bas (la puce doit piloter SO) :");
  ccPinLevels(out, "SO / MISO", miso);
  digitalWrite(csn, HIGH);

  out.println();
  out.println("  Les broches que NOUS pilotons, pour verifier qu'elles sortent :");
  pinMode(sck, OUTPUT);
  pinMode(mosi, OUTPUT);
  digitalWrite(sck, HIGH);
  digitalWrite(mosi, HIGH);
  delay(2);
  out.printf("    SCK relu %d, MOSI relu %d  (doivent valoir 1)\n", digitalRead(sck),
             digitalRead(mosi));
  digitalWrite(sck, LOW);
  digitalWrite(mosi, LOW);
  delay(2);
  out.printf("    SCK relu %d, MOSI relu %d  (doivent valoir 0)\n", digitalRead(sck),
             digitalRead(mosi));

  out.println();
  out.println("  Vidage brut des registres 0x00 a 0x0F :");
  radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7]);
  char lineBuf[120];
  size_t w = (size_t)snprintf(lineBuf, sizeof(lineBuf), "   ");
  uint8_t nonZero = 0;
  for (uint8_t r = 0; r <= 0x0F; r++) {
    const uint8_t v = radio2.readRegister(r);
    if (v) nonZero++;
    w += (size_t)snprintf(lineBuf + w, sizeof(lineBuf) - w, " %02X", v);
  }
  out.println(lineBuf);
  if (!nonZero) {
    out.println("  Tous a zero : le bus ne rapporte rien. Apres un reset, un");
    out.println("  CC2500 doit montrer des valeurs par defaut non nulles");
    out.println("  (par exemple IOCFG2=0x29, PKTLEN=0xFF, MDMCFG4=0x8C).");
  }
}

// SPI bit-bange a la main. Il ne depend ni de la matrice de broches, ni de
// l'objet SPIClass partage, ni d'un reglage de mode : si celui-ci parle alors
// que le peripherique se tait, le fautif est la configuration et non le fil.
// Mode 0 : la puce echantillonne MOSI sur le front montant de SCK et presente
// son bit sur le front descendant.
static uint8_t ccBitBang(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t v) {
  uint8_t in = 0;
  for (int8_t b = 7; b >= 0; b--) {
    digitalWrite(mosi, (v >> b) & 1);
    delayMicroseconds(2);
    digitalWrite(sck, HIGH);
    delayMicroseconds(2);
    in = (uint8_t)((in << 1) | (digitalRead(miso) ? 1 : 0));
    digitalWrite(sck, LOW);
    delayMicroseconds(2);
  }
  return in;
}

void ccRawProbe(Print &out) {
  const uint8_t sck = ccPins[0], miso = ccPins[1], mosi = ccPins[2], csn = ccPins[3];
  out.println();
  out.println("=== Le bus SPI transporte-t-il quelque chose ? ===");
  out.println("  Le CC2500 renvoie un OCTET D'ETAT pendant le premier octet de");
  out.println("  chaque transaction : bit 7 = pas pret, bits 6-4 = etat, bits");
  out.println("  3-0 = octets libres dans la FIFO. En veille il vaut 0x0F.");
  out.println("  Un 0x00 franc signifie que rien ne revient.");
  out.println();

  // 1. Par le peripherique SPI.
  radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7]);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(csn, LOW);
  delayMicroseconds(50);
  const uint8_t st1 = SPI.transfer(0x3D);  // SNOP : ne fait rien, rend l'etat
  digitalWrite(csn, HIGH);
  SPI.endTransaction();
  out.printf("  peripherique SPI, strobe SNOP -> etat 0x%02X\n", st1);

  // 2. En bit-bang pur.
  SPI.end();
  pinMode(sck, OUTPUT);
  pinMode(mosi, OUTPUT);
  pinMode(miso, INPUT);
  digitalWrite(sck, LOW);
  digitalWrite(mosi, LOW);
  pinMode(csn, OUTPUT);
  digitalWrite(csn, HIGH);
  delay(2);

  digitalWrite(csn, LOW);
  delayMicroseconds(100);
  const uint8_t st2 = ccBitBang(sck, miso, mosi, 0x3D);
  digitalWrite(csn, HIGH);
  out.printf("  bit-bang, strobe SNOP         -> etat 0x%02X\n", st2);

  // 3. Lecture de PARTNUM et VERSION en bit-bang (bit de rafale obligatoire).
  digitalWrite(csn, LOW);
  delayMicroseconds(100);
  const uint8_t st3 = ccBitBang(sck, miso, mosi, 0xF0);  // lecture rafale 0x30
  const uint8_t part = ccBitBang(sck, miso, mosi, 0x00);
  digitalWrite(csn, HIGH);
  delayMicroseconds(50);
  digitalWrite(csn, LOW);
  delayMicroseconds(100);
  ccBitBang(sck, miso, mosi, 0xF1);
  const uint8_t ver = ccBitBang(sck, miso, mosi, 0x00);
  digitalWrite(csn, HIGH);
  out.printf("  bit-bang, PARTNUM 0x%02X (etat 0x%02X), VERSION 0x%02X\n", part, st3, ver);

  out.println();
  if (st1 == 0x00 && st2 == 0x00) {
    out.println("  Rien ne revient par aucune des deux voies : SCK ou SI n'atteint");
    out.println("  pas le module. Verifie ces deux fils en priorite.");
  } else if (st2 && !st1) {
    out.println("  Le bit-bang parle et pas le peripherique : le fautif est la");
    out.println("  configuration SPI, pas le cablage.");
  } else {
    out.println("  Le bus repond.");
  }

  SPI.begin(sck, miso, mosi, -1);
}

// ---------------------------------------------------------------------------
//  Configuration du CC2500 pour ECOUTER la telecommande BenQ en brut.
//
//  Porteuse 2405,000 MHz, GFSK, 125 kbps, excursion 165 kHz, filtre de canal
//  812,5 kHz. Quartz de 26 MHz (marquage T260 sur le module).
//
//  Les deux registres qui font tout le travail :
//    PKTCTRL0.PKT_FORMAT = 01 -> mode serie SYNCHRONE : le moteur de paquets
//      est debranche, GDO0 sort les bits demodules et GDO2 l'horloge de bit
//      recuperee par la puce elle-meme.
//    MDMCFG2.SYNC_MODE = 000 -> ni preambule ni mot de synchro exiges. Le
//      datasheet prevoit explicitement ce cas : "The MCU must then handle
//      preamble and sync word insertion and detection in software."
//  C'est exactement ce que le BC5602 refuse, et donc le seul chemin vers
//  l'adresse de la telecommande sans la connaitre d'avance.
//
//  Calculs verifies a la main depuis les formules du datasheet :
//    FREQ  = 2405e6 x 2^16 / 26e6 = 6 062 080 = 0x5C8000
//    debit = (256+59) x 2^12 x 26e6 / 2^28 = 124 969,5 Bd, soit -0,02 %
//    f_dev = 26e6/2^17 x (8+5) x 2^6 = 165 037 Hz
//    BW    = 26e6 / (8 x 4 x 1) = 812 500 Hz
// ---------------------------------------------------------------------------
static const uint8_t kCcRxConfig[][2] = {
    {cc2500::REG_IOCFG2, 0x0B},    // GDO2 = horloge serie
    {cc2500::REG_IOCFG0, 0x0C},    // GDO0 = donnee serie synchrone
    {cc2500::REG_FIFOTHR, 0x07},
    {cc2500::REG_PKTLEN, 0xFF},
    {cc2500::REG_PKTCTRL1, 0x00},  // aucun filtrage d'adresse, aucun seuil PQT
    {cc2500::REG_PKTCTRL0, 0x12},  // mode serie synchrone, CRC coupe
    {cc2500::REG_ADDR, 0x00},
    {cc2500::REG_CHANNR, 0x00},    // canal 0 : la porteuse est dans FREQ
    {cc2500::REG_FSCTRL1, 0x10},   // f_IF = 406,25 kHz, accordee au filtre
    {cc2500::REG_FSCTRL0, 0x00},
    {cc2500::REG_FREQ2, 0x5C},
    {cc2500::REG_FREQ1, 0x80},
    {cc2500::REG_FREQ0, 0x00},
    {cc2500::REG_MDMCFG4, 0x0C},
    {cc2500::REG_MDMCFG3, 0x3B},
    {cc2500::REG_MDMCFG2, 0x10},   // GFSK, NI preambule NI mot de synchro
    {cc2500::REG_MDMCFG1, 0x22},
    {cc2500::REG_MDMCFG0, 0xF8},
    {cc2500::REG_DEVIATN, 0x65},
    {cc2500::REG_MCSM2, 0x07},
    {cc2500::REG_MCSM1, 0x3C},     // rester en RX apres reception
    {cc2500::REG_MCSM0, 0x18},     // autocalibration a l'entree en RX
    {cc2500::REG_FOCCFG, 0x1E},
    {cc2500::REG_BSCFG, 0x1F},     // synchronisation de bit (0x1A, pas 0x1C)
    {cc2500::REG_AGCCTRL2, 0xC7},
    {cc2500::REG_AGCCTRL1, 0x00},
    {cc2500::REG_AGCCTRL0, 0xB2},
    {cc2500::REG_FREND1, 0xB6},
    {cc2500::REG_FREND0, 0x10},
    {cc2500::REG_FSCAL3, 0xEA},
    {cc2500::REG_FSCAL2, 0x0A},
    {cc2500::REG_FSCAL1, 0x00},
    {cc2500::REG_FSCAL0, 0x11},
    {cc2500::REG_TEST2, 0x88},
    {cc2500::REG_TEST1, 0x31},
    {cc2500::REG_TEST0, 0x0B},
};

void ccListen(Print &out, uint32_t dwellMs) {
  const uint8_t gdo0 = ccPins[4], gdo2 = ccPins[5];
  out.println();
  out.println("=== CC2500 en ecoute brute sur 2405 MHz ===");
  out.println("  Mode serie synchrone, ni preambule ni mot de synchro exiges.");
  out.println("  GDO2 doit alors battre a 125 kHz en continu : c'est l'horloge");
  out.println("  de bit recuperee par la puce, et son premier temoin de vie.");
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);

  // Relecture : une ecriture muette se voit ici, pas trois heures plus tard.
  uint8_t bad = 0;
  for (const auto &r : kCcRxConfig)
    if (radio2.readRegister(r[0]) != r[1]) bad++;
  out.printf("  Registres relus : %u ecart(s) sur %u.\n", bad,
             (unsigned)(sizeof(kCcRxConfig) / sizeof(kCcRxConfig[0])));

  // Etage d'entree RFX2402E : amplificateur faible bruit seul, jamais le PA.
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(5);
  const uint8_t marc = radio2.marcState();
  out.printf("  MARCSTATE 0x%02X (%s)\n", marc, cc2500::marcStateName(marc));
  if (marc != cc2500::MARC_RX) out.println("  ATTENTION : la puce n'est PAS en reception.");

  pinMode(gdo0, INPUT);
  pinMode(gdo2, INPUT);
  const uint32_t m0 = 1UL << gdo0, m2 = 1UL << gdo2;
  uint32_t e0 = 0, e2 = 0, samples = 0;
  uint32_t last = REG_READ(GPIO_IN_REG);
  const uint32_t until = millis() + dwellMs;
  while ((int32_t)(millis() - until) < 0) {
    for (uint16_t b = 0; b < 1024; b++) {
      const uint32_t now = REG_READ(GPIO_IN_REG);
      if ((now ^ last) & m2) e2++;
      if ((now ^ last) & m0) e0++;
      last = now;
      samples++;
    }
  }

  const int8_t rssiRaw = (int8_t)radio2.readStatus(cc2500::STA_RSSI);
  const int rssiDbm = (rssiRaw >= 0) ? (rssiRaw / 2 - 72) : (rssiRaw / 2 - 72);
  out.printf("  GDO2 (horloge) : %lu fronts.  GDO0 (donnees) : %lu fronts.\n",
             (unsigned long)e2, (unsigned long)e0);
  out.printf("  %lu echantillons en %lu ms, RSSI %d dBm.\n", (unsigned long)samples,
             (unsigned long)dwellMs, rssiDbm);
  out.println();
  if (e2 < 1000) {
    out.println("  L'horloge de bit ne bat pas. Soit GDO2 n'est pas sur la");
    out.println("  broche declaree, soit la puce n'est pas reellement en RX.");
  } else {
    out.println("  L'horloge bat : la chaine de demodulation tourne. Les fronts");
    out.println("  sur GDO0 sont des bits demodules -- du bruit tant qu'aucune");
    out.println("  source n'emet, une trame des qu'il y en a une.");
  }
}

// ---------------------------------------------------------------------------
//  Capture du flux binaire brut sorti par le CC2500, et recherche d'un motif.
//
//  Le CC2500 pose la donnee sur le front DESCENDANT de son horloge serie : on
//  echantillonne donc GDO0 sur le front MONTANT de GDO2. Aucune adresse n'est
//  connue de la puce a ce stade -- c'est tout l'interet.
//
//  La validation se fait contre la balise, dont on connait l'adresse sur l'air
//  (E1 22 33 44) et la charge utile (DE AD 55 0F A0 3C 01 02 03 04). Trouver ce
//  motif dans le flux prouve la chaine entiere : radio, demodulation, horloge,
//  echantillonnage. Tant qu'on ne l'a pas trouve, chercher l'adresse de la
//  telecommande serait chercher a l'aveugle avec un instrument non etalonne --
//  l'erreur que ce projet a deja payee plusieurs fois.
// ---------------------------------------------------------------------------
static uint8_t ccBits[4096];  // 32768 bits, soit ~0,26 s a 125 kbit/s

static inline bool ccBitAt(const uint8_t *buf, uint32_t i) {
  return (buf[i >> 3] >> (7 - (i & 7))) & 1;
}

// Compte les bits identiques entre le motif et le flux a partir d'un offset.
static uint8_t ccMatchBits(const uint8_t *buf, uint32_t at, uint32_t pattern, bool invert) {
  uint8_t same = 0;
  for (int8_t k = 31; k >= 0; k--) {
    const bool want = ((pattern >> k) & 1) ^ (invert ? 1 : 0);
    if (ccBitAt(buf, at + (uint32_t)(31 - k)) == want) same++;
  }
  return same;
}

void ccCapture(Print &out, uint32_t pattern, uint32_t nbits) {
  const uint8_t gdo0 = ccPins[4], gdo2 = ccPins[5];
  if (nbits > sizeof(ccBits) * 8) nbits = sizeof(ccBits) * 8;

  out.println();
  out.println("=== Capture du flux brut et recherche de motif ===");
  out.printf("  %lu bits echantillonnes sur le front montant de GDO2.\n", (unsigned long)nbits);
  out.printf("  Motif cherche : %08lX, dans les deux polarites.\n", (unsigned long)pattern);
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(5);
  if (radio2.marcState() != cc2500::MARC_RX) {
    out.println("  La puce n'est pas en reception : capture annulee.");
    return;
  }

  pinMode(gdo0, INPUT);
  pinMode(gdo2, INPUT);
  memset(ccBits, 0, sizeof(ccBits));

  const uint32_t m0 = 1UL << gdo0, m2 = 1UL << gdo2;
  uint32_t got = 0;
  uint32_t prev = REG_READ(GPIO_IN_REG) & m2;
  // Garde-fou : si l'horloge s'arretait, la boucle tournerait sans fin.
  const uint32_t deadline = millis() + 2000;

  noInterrupts();
  while (got < nbits) {
    const uint32_t now = REG_READ(GPIO_IN_REG);
    const uint32_t clk = now & m2;
    if (clk && !prev) {
      if (now & m0) ccBits[got >> 3] |= (uint8_t)(0x80 >> (got & 7));
      got++;
    }
    prev = clk;
    if ((got & 0x3FF) == 0 && (int32_t)(millis() - deadline) >= 0) break;
  }
  interrupts();

  out.printf("  %lu bits captures.\n", (unsigned long)got);
  if (got < nbits) out.println("  (interrompu : l'horloge s'est arretee)");

  // Recherche du motif, tolerante : on rapporte les meilleures correspondances
  // plutot qu'une egalite stricte, parce qu'un seul bit faux masquerait tout.
  uint8_t best = 0;
  uint32_t bestAt = 0;
  bool bestInv = false;
  uint16_t exact = 0;
  for (uint32_t i = 0; i + 32 <= got; i++) {
    for (uint8_t inv = 0; inv < 2; inv++) {
      const uint8_t m = ccMatchBits(ccBits, i, pattern, inv != 0);
      if (m == 32) exact++;
      if (m > best) {
        best = m;
        bestAt = i;
        bestInv = (inv != 0);
      }
    }
  }
  out.printf("  Meilleure correspondance : %u bits sur 32 a l'offset %lu%s.\n", best,
             (unsigned long)bestAt, bestInv ? " (polarite inversee)" : "");
  out.printf("  Correspondances exactes : %u.\n", exact);

  // Vidage autour du meilleur point : c'est la que doit se lire l'adresse
  // suivie du PCF et de la charge utile.
  const uint32_t from = (bestAt >= 32) ? (bestAt - 32) : 0;
  char lineBuf[160];
  out.println("  Flux autour de ce point (24 octets) :");
  size_t w = (size_t)snprintf(lineBuf, sizeof(lineBuf), "   ");
  for (uint8_t b = 0; b < 24 && from + (uint32_t)b * 8 + 8 <= got; b++) {
    uint8_t v = 0;
    for (uint8_t k = 0; k < 8; k++)
      v = (uint8_t)((v << 1) | (ccBitAt(ccBits, from + (uint32_t)b * 8 + k) ? 1 : 0));
    w += (size_t)snprintf(lineBuf + w, sizeof(lineBuf) - w, " %02X", v);
  }
  out.println(lineBuf);

  // Densite de transitions : un flux fige trahit un demodulateur sans signal.
  uint32_t flips = 0;
  for (uint32_t i = 1; i < got; i++)
    if (ccBitAt(ccBits, i) != ccBitAt(ccBits, i - 1)) flips++;
  out.printf("  Transitions dans le flux : %lu sur %lu bits (%lu %%).\n", (unsigned long)flips,
             (unsigned long)got, (unsigned long)(got ? flips * 100 / got : 0));
}

// ---------------------------------------------------------------------------
//  Trouver une adresse SANS la connaitre.
//
//  Le preambule est le seul element universel d'une trame : une alternance de
//  bits, identique sur tous les exemplaires. On le cherche donc dans le flux
//  brut, et on lit les 32 bits qui le suivent -- c'est l'adresse.
//
//  Le bruit produit lui aussi des alternances courtes. Le juge est la
//  REPETITION : la vraie adresse revient a chaque trame, une coincidence non.
//  On compte donc les occurrences de chaque candidat et on classe.
//
//  Valide sur la balise : la capture a rendu
//    FF C0 2A AA | E1 22 33 44 | DE AD 55 0F A0 3C 01 02 03 04 | C2 BA
//  soit preambule, adresse, charge utile et CRC, sans qu'aucune adresse n'ait
//  ete donnee a la puce.
// ---------------------------------------------------------------------------
struct CcCand {
  uint32_t addr;
  uint16_t seen;
};

// Une capture ne dure que 0,26 s, soit environ cinq trames d'une telecommande
// qui en emet une vingtaine par seconde. On en enchaine donc plusieurs et on
// CUMULE les candidats : la repetition, seul juge fiable, devient d'autant plus
// severe que les captures sont nombreuses.
void ccFindAddress(Print &out, uint32_t nbits, uint8_t minRun, uint8_t repeats) {
  if (nbits > sizeof(ccBits) * 8) nbits = sizeof(ccBits) * 8;
  if (repeats < 1) repeats = 1;
  const uint8_t gdo0 = ccPins[4], gdo2 = ccPins[5];

  out.println();
  out.println("=== Recherche d'adresse dans le flux brut ===");
  out.printf("  %u capture(s) de %lu bits, alternance d'au moins %u bits.\n", repeats,
             (unsigned long)nbits, minRun);
  out.println("  Le juge est la REPETITION : la vraie adresse revient a chaque");
  out.println("  trame, une coincidence de bruit non. Attention, un 0x55 dans la");
  out.println("  charge utile est lui-meme une alternance, donc un faux ancrage.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(5);
  if (radio2.marcState() != cc2500::MARC_RX) {
    out.println("  La puce n'est pas en reception : mesure annulee.");
    return;
  }

  pinMode(gdo0, INPUT);
  pinMode(gdo2, INPUT);
  const uint32_t m0 = 1UL << gdo0, m2 = 1UL << gdo2;

  static CcCand cands[192];
  uint8_t nCand = 0;
  uint32_t anchors = 0, totalBits = 0, flips = 0;
  int peakDbm = -128;

  for (uint8_t pass = 0; pass < repeats; pass++) {
    memset(ccBits, 0, sizeof(ccBits));
    uint32_t got = 0;
    uint32_t prev = REG_READ(GPIO_IN_REG) & m2;
    const uint32_t deadline = millis() + 2000;
    noInterrupts();
    while (got < nbits) {
      const uint32_t now = REG_READ(GPIO_IN_REG);
      const uint32_t clk = now & m2;
      if (clk && !prev) {
        if (now & m0) ccBits[got >> 3] |= (uint8_t)(0x80 >> (got & 7));
        got++;
      }
      prev = clk;
      if ((got & 0x3FF) == 0 && (int32_t)(millis() - deadline) >= 0) break;
    }
    interrupts();
    totalBits += got;

    // Temoin de trafic, sans lequel un resultat nul ne veut rien dire : un
    // flux demodule fige trahit une source absente, pas une adresse introuvable.
    for (uint32_t k = 1; k < got; k++)
      if (ccBitAt(ccBits, k) != ccBitAt(ccBits, k - 1)) flips++;
    const int dbm = (int)((int8_t)radio2.readStatus(cc2500::STA_RSSI)) / 2 - 72;
    if (dbm > peakDbm) peakDbm = dbm;

    uint32_t i = 1, runStart = 0;
    while (i < got) {
      if (ccBitAt(ccBits, i) != ccBitAt(ccBits, i - 1)) {
        i++;
        continue;
      }
      const uint32_t runLen = i - runStart;
      if (runLen >= minRun && i + 32 <= got) {
        anchors++;
        // L'alternance deborde d'un bit dans l'adresse quand le premier bit de
        // celle-ci prolonge le motif : on teste donc les alignements voisins.
        for (int8_t d = -2; d <= 0; d++) {
          const int32_t s = (int32_t)i + d;
          if (s < 0 || (uint32_t)s + 32 > got) continue;
          uint32_t a = 0;
          for (uint8_t k = 0; k < 32; k++)
            a = (a << 1) | (ccBitAt(ccBits, (uint32_t)s + k) ? 1 : 0);
          bool found = false;
          for (uint8_t j = 0; j < nCand; j++)
            if (cands[j].addr == a) {
              cands[j].seen++;
              found = true;
              break;
            }
          if (!found && nCand < 192) cands[nCand++] = {a, 1};
        }
      }
      runStart = i;
      i++;
    }
    delay(2);
  }

  out.printf("  %lu bits au total, %lu ancrage(s), %u candidat(s) distinct(s).\n",
             (unsigned long)totalBits, (unsigned long)anchors, nCand);
  out.printf("  TEMOIN : %lu transitions dans le flux (%lu pour mille), RSSI de pic %d dBm.\n",
             (unsigned long)flips, (unsigned long)(totalBits ? flips * 1000 / totalBits : 0),
             peakDbm);
  if (!nCand) {
    out.println("  Aucun preambule dans le flux : soit la source n'a pas emis,");
    out.println("  soit elle n'est pas sur ce canal.");
    return;
  }

  out.println("  Candidats les plus repetes (ordre SUR L'AIR) :");
  char lineBuf[132];
  for (uint8_t rank = 0; rank < 8; rank++) {
    uint8_t bi = 0xFF;
    uint16_t bs = 0;
    for (uint8_t j = 0; j < nCand; j++)
      if (cands[j].seen > bs) {
        bs = cands[j].seen;
        bi = j;
      }
    if (bi == 0xFF || bs == 0) break;
    const uint32_t a = cands[bi].addr;
    snprintf(lineBuf, sizeof(lineBuf), "    %02X %02X %02X %02X  vu %3u fois%s",
             (unsigned)(a >> 24), (unsigned)((a >> 16) & 0xFF), (unsigned)((a >> 8) & 0xFF),
             (unsigned)(a & 0xFF), bs, bs >= 5 ? "   <<< candidat serieux" : "");
    out.println(lineBuf);
    cands[bi].seen = 0;
  }
  out.println();
  out.println("  Pour l'ecrire dans le BM5602, inverser l'ordre : le dernier");
  out.println("  octet affiche part en premier sur l'air.");
}

// ---------------------------------------------------------------------------
//  Table de verite de l'etage d'entree RFX2402E, MESUREE et non supposee.
//  Les deux broches PA_EN et RX_EN commandent un amplificateur de puissance et
//  un amplificateur faible bruit. Une polarite fausse rend le module sourd, ou
//  le fait ecouter a travers un chemin attenue -- sans rien dire. On balaie
//  donc les quatre combinaisons en lisant le RSSI, avec une source connue.
// ---------------------------------------------------------------------------
void ccFrontEnd(Print &out, uint32_t dwellMs) {
  out.println();
  out.println("=== Etage d'entree : quelle combinaison ouvre la reception ? ===");
  out.println("  RSSI en dBm, mesure sur une source qui doit emettre pendant");
  out.println("  toute la mesure. Plus le chiffre est GRAND (moins negatif),");
  out.println("  plus le chemin de reception est ouvert.");
  out.println("  >>> BALISE ALLUMEE, ou molette tournee sans arret.");
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);

  char lineBuf[132];
  for (uint8_t combo = 0; combo < 4; combo++) {
    const bool pa = (combo & 2) != 0, rx = (combo & 1) != 0;
    radio2.strobe(cc2500::STROBE_SIDLE);
    radio2.setFrontEnd(pa, rx);
    delay(2);
    radio2.strobe(cc2500::STROBE_SRX);
    delay(10);

    int best = -128;
    long sum = 0;
    uint16_t n = 0;
    const uint32_t until = millis() + dwellMs;
    while ((int32_t)(millis() - until) < 0) {
      const int8_t raw = (int8_t)radio2.readStatus(cc2500::STA_RSSI);
      const int dbm = raw / 2 - 72;
      if (dbm > best) best = dbm;
      sum += dbm;
      n++;
      delay(1);
    }
    snprintf(lineBuf, sizeof(lineBuf), "    PA_EN=%d RX_EN=%d  -> RSSI moyen %ld dBm, pic %d dBm  (%u mesures)",
             pa ? 1 : 0, rx ? 1 : 0, n ? sum / n : 0, best, n);
    out.println(lineBuf);
    Serial.flush();
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  radio2.setFrontEnd(false, false);
  out.println();
  out.println("  Retenir la combinaison au pic le plus haut : c'est celle qui");
  out.println("  passe par l'amplificateur faible bruit.");
}

// ---------------------------------------------------------------------------
//  Le CC2500 entend-il la source, oui ou non ? Distribution du RSSI.
//
//  Un pic isole ne prouve rien : une salve d'une milliseconde se rate si on
//  echantillonne une fois par seconde, et une rafale Wi-Fi produit un pic
//  identique. On echantillonne donc en continu, sur deux phases -- repos puis
//  source active -- et on compare les DISTRIBUTIONS. C'est la mesure qui avait
//  etabli que la telecommande emet sur 2405 MHz, refaite ici avec un recepteur
//  qui rend des dBm veritables.
// ---------------------------------------------------------------------------
void ccPresence(Print &out, uint32_t phaseMs) {
  out.println();
  out.println("=== Le CC2500 entend-il la source sur 2405 MHz ? ===");
  out.println("  Distribution du RSSI, phase de repos puis phase active.");
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);  // table de verite mesuree : LNA seul
  radio2.strobe(cc2500::STROBE_SRX);
  delay(10);
  if (radio2.marcState() != cc2500::MARC_RX) {
    out.println("  La puce n'est pas en reception : mesure annulee.");
    return;
  }

  // Quatre bandes : plancher, un peu, fort, tres fort.
  uint32_t hist[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
  uint32_t total[2] = {0, 0};
  int peak[2] = {-128, -128};
  char lineBuf[132];

  for (uint8_t phase = 0; phase < 2; phase++) {
    if (phase == 0) out.println("  Phase 1 sur 2 -- NE TOUCHE A RIEN :");
    else out.println("  Phase 2 sur 2 -- TOURNE LA MOLETTE SANS T'ARRETER :");
    for (uint8_t k = 3; k >= 1; k--) {
      snprintf(lineBuf, sizeof(lineBuf), "    %u...", (unsigned)k);
      out.println(lineBuf);
      Serial.flush();
      delay(1000);
    }

    const uint32_t until = millis() + phaseMs;
    while ((int32_t)(millis() - until) < 0) {
      const int dbm = (int)((int8_t)radio2.readStatus(cc2500::STA_RSSI)) / 2 - 72;
      if (dbm > peak[phase]) peak[phase] = dbm;
      if (dbm >= -50) hist[phase][3]++;
      else if (dbm >= -60) hist[phase][2]++;
      else if (dbm >= -70) hist[phase][1]++;
      else hist[phase][0]++;
      total[phase]++;
      // La puce peut quitter le RX sur un evenement : on l'y remet.
      if ((total[phase] & 0x3FF) == 0 && radio2.marcState() != cc2500::MARC_RX)
        radio2.strobe(cc2500::STROBE_SRX);
    }
    out.println();
  }

  static const char *bands[4] = {"sous -70 dBm", "-70 a -60   ", "-60 a -50   ", "au-dessus de -50"};
  out.println("  bande             repos        actif      rapport");
  for (int8_t b = 3; b >= 0; b--) {
    const float r0 = total[0] ? (1000.0f * hist[0][b] / total[0]) : 0.0f;
    const float r1 = total[1] ? (1000.0f * hist[1][b] / total[1]) : 0.0f;
    const float ratio = (r0 > 0.02f) ? (r1 / r0) : (r1 > 0.02f ? 999.0f : 1.0f);
    snprintf(lineBuf, sizeof(lineBuf), "  %-17s %8.2f0/00 %8.2f0/00   x%.2f", bands[b],
             (double)r0, (double)r1, (double)ratio);
    out.println(lineBuf);
  }
  snprintf(lineBuf, sizeof(lineBuf), "  pics : repos %d dBm, actif %d dBm   (%lu et %lu mesures)",
           peak[0], peak[1], (unsigned long)total[0], (unsigned long)total[1]);
  out.println(lineBuf);
}

// ---------------------------------------------------------------------------
//  Trouver les trames par leur CRC, sans aucune hypothese de structure.
//
//  Modele identifie sur une trame reellement capturee de la balise
//  (E1 22 33 44 DE AD 55 0F A0 3C 01 02 03 04 puis C2 BA) : CRC-16/CCITT,
//  polynome 0x1021, etat initial 0xFFFF, couvrant l'ADRESSE ET la charge
//  utile, bit de poids fort d'abord. C'est aussi ce que documente le projet
//  xfranek pour la deuxieme generation.
//
//  On balaie donc chaque position de depart et chaque longueur couverte : la
//  ou les seize bits suivants valent le CRC de ce qui precede, il y a une
//  trame. Aucune hypothese sur le preambule, l'adresse ni la longueur -- et un
//  faux positif tous les 65 536 essais seulement, que la repetition elimine.
//  L'adresse est alors simplement les 32 premiers bits.
// ---------------------------------------------------------------------------
void ccCrcHunt(Print &out, uint32_t nbits, uint8_t repeats, uint16_t minLen, uint16_t maxLen) {
  if (nbits > sizeof(ccBits) * 8) nbits = sizeof(ccBits) * 8;
  if (repeats < 1) repeats = 1;
  const uint8_t gdo0 = ccPins[4], gdo2 = ccPins[5];

  out.println();
  out.println("=== Chasse aux trames par leur CRC ===");
  out.printf("  %u capture(s) de %lu bits, longueurs couvertes de %u a %u bits.\n", repeats,
             (unsigned long)nbits, minLen, maxLen);
  out.println("  CRC-16/CCITT 0x1021, init 0xFFFF, couvrant adresse + charge utile.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(5);
  if (radio2.marcState() != cc2500::MARC_RX) {
    out.println("  La puce n'est pas en reception : mesure annulee.");
    return;
  }
  pinMode(gdo0, INPUT);
  pinMode(gdo2, INPUT);

  struct Hit {
    uint32_t addr;
    uint16_t len;
    uint16_t seen;
  };
  static Hit hits[160];
  uint8_t nHit = 0;
  uint32_t total = 0, found = 0, flips = 0;
  int peakDbm = -128;

  const uint32_t m0 = 1UL << gdo0, m2 = 1UL << gdo2;
  for (uint8_t pass = 0; pass < repeats; pass++) {
    memset(ccBits, 0, sizeof(ccBits));
    uint32_t got = 0;
    uint32_t prev = REG_READ(GPIO_IN_REG) & m2;
    const uint32_t deadline = millis() + 2000;
    noInterrupts();
    while (got < nbits) {
      const uint32_t now = REG_READ(GPIO_IN_REG);
      const uint32_t clk = now & m2;
      if (clk && !prev) {
        if (now & m0) ccBits[got >> 3] |= (uint8_t)(0x80 >> (got & 7));
        got++;
      }
      prev = clk;
      if ((got & 0x3FF) == 0 && (int32_t)(millis() - deadline) >= 0) break;
    }
    interrupts();
    total += got;
    for (uint32_t k = 1; k < got; k++)
      if (ccBitAt(ccBits, k) != ccBitAt(ccBits, k - 1)) flips++;
    const int dbm = (int)((int8_t)radio2.readStatus(cc2500::STA_RSSI)) / 2 - 72;
    if (dbm > peakDbm) peakDbm = dbm;

    if (got < (uint32_t)maxLen + 32) continue;
    const uint32_t last = got - (uint32_t)maxLen - 16;
    for (uint32_t i = 0; i < last; i++) {
      uint16_t win = 0;
      for (uint8_t k = 0; k < 16; k++) win = (uint16_t)((win << 1) | (ccBitAt(ccBits, i + k) ? 1 : 0));
      uint16_t crc = 0xFFFF;
      for (uint16_t L = 1; L <= maxLen; L++) {
        crc ^= (uint16_t)((ccBitAt(ccBits, i + L - 1) ? 1 : 0) << 15);
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        win = (uint16_t)((win << 1) | (ccBitAt(ccBits, i + L + 15) ? 1 : 0));
        if (L < minLen) continue;
        if (win != crc) continue;

        // Ecarter les zones degenerees. Un flux sature a 1 (ou a 0) produit des
        // coincidences de CRC en quantite, et elles remplissaient la table au
        // point d'en chasser les vraies trames. Une trame reelle -- preambule,
        // adresse, charge utile -- est forcement riche en transitions.
        uint16_t tr = 0;
        for (uint16_t k = 1; k < L; k++)
          if (ccBitAt(ccBits, i + k) != ccBitAt(ccBits, i + k - 1)) tr++;
        if (tr * 8 < L) continue;
        found++;
        uint32_t a = 0;
        for (uint8_t k = 0; k < 32; k++) a = (a << 1) | (ccBitAt(ccBits, i + k) ? 1 : 0);
        bool dup = false;
        for (uint8_t j = 0; j < nHit; j++)
          if (hits[j].addr == a && hits[j].len == L) {
            hits[j].seen++;
            dup = true;
            break;
          }
        if (!dup && nHit < 160) hits[nHit++] = {a, L, 1};
      }
    }
    delay(1);
  }

  out.printf("  %lu bits, %lu trame(s) valide(s) par le CRC, %u distincte(s).\n",
             (unsigned long)total, (unsigned long)found, nHit);
  out.printf("  TEMOIN : %lu transitions (%lu pour mille), RSSI de pic %d dBm.\n",
             (unsigned long)flips, (unsigned long)(total ? flips * 1000 / total : 0), peakDbm);
  if (!nHit) {
    out.println("  Aucune trame ne passe le CRC. Si le temoin montre du trafic,");
    out.println("  c'est la demodulation ou le modele de trame qui est en cause,");
    out.println("  pas l'absence de source.");
    return;
  }

  out.println("  Trames trouvees (adresse SUR L'AIR, longueur couverte) :");
  char lineBuf[140];
  for (uint8_t rank = 0; rank < 10; rank++) {
    uint8_t bi = 0xFF;
    uint16_t bs = 0;
    for (uint8_t j = 0; j < nHit; j++)
      if (hits[j].seen > bs) {
        bs = hits[j].seen;
        bi = j;
      }
    if (bi == 0xFF || bs == 0) break;
    const uint32_t a = hits[bi].addr;
    snprintf(lineBuf, sizeof(lineBuf), "    %02X %02X %02X %02X   %3u bits couverts   vu %3u fois%s",
             (unsigned)(a >> 24), (unsigned)((a >> 16) & 0xFF), (unsigned)((a >> 8) & 0xFF),
             (unsigned)(a & 0xFF), hits[bi].len, bs, bs >= 3 ? "   <<< SERIEUX" : "");
    out.println(lineBuf);
    hits[bi].seen = 0;
  }
  out.println();
  out.println("  Pour l'ecrire dans le BM5602, inverser l'ordre des octets.");
}

// ---------------------------------------------------------------------------
//  Mesurer le DEBIT REEL de la source, a la regle.
//
//  Depuis le debut, le debit de 125 kbps vient du dossier FCC et d'un calcul de
//  Carson -- jamais d'une mesure directe. Le mode ASYNCHRONE du CC2500 le rend
//  possible : PKTCTRL0.PKT_FORMAT=11 sort la donnee demodulee BRUTE sur GDO0,
//  sans aucune decision de bit ni recuperation d'horloge. On echantillonne donc
//  cette broche aussi vite que possible et on mesure la duree des impulsions :
//  la plus courte qui revient souvent EST la periode d'un bit.
//
//  Declenchement sur le RSSI, sans quoi on ne capturerait que du bruit : la
//  telecommande n'emet qu'environ 1 % du temps.
// ---------------------------------------------------------------------------
void ccPulseWidths(Print &out, int trigDbm, uint16_t tries) {
  const uint8_t gdo0 = ccPins[4];
  out.println();
  out.println("=== Duree reelle d'un bit, mesuree en mode asynchrone ===");
  out.printf("  Declenchement au-dessus de %d dBm, %u tentatives.\n", trigDbm, tries);
  out.println("  La donnee sort BRUTE : aucune decision de bit n'est faite par");
  out.println("  la puce, donc les durees sont celles du signal lui-meme.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  // Bascule en asynchrone : donnee brute sur GDO0.
  radio2.writeRegister(cc2500::REG_PKTCTRL0, 0x32);  // PKT_FORMAT = 11
  radio2.writeRegister(cc2500::REG_IOCFG0, 0x0D);    // GDO0 = donnee asynchrone
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(10);
  if (radio2.marcState() != cc2500::MARC_RX) {
    out.println("  La puce n'est pas en reception : mesure annulee.");
    return;
  }
  pinMode(gdo0, INPUT);

  // Histogramme des durees, en pas d'echantillon, jusqu'a 64.
  static uint32_t hist[65];
  memset(hist, 0, sizeof(hist));
  const uint32_t m0 = 1UL << gdo0;
  uint16_t caught = 0;
  uint32_t loops = 0;
  uint32_t sampled = 0, elapsedTotal = 0;

  for (uint16_t attempt = 0; attempt < tries; attempt++) {
    // Attendre une salve.
    bool armed = false;
    const uint32_t giveUp = millis() + 200;
    while ((int32_t)(millis() - giveUp) < 0) {
      const int dbm = (int)((int8_t)radio2.readStatus(cc2500::STA_RSSI)) / 2 - 72;
      if (dbm >= trigDbm) {
        armed = true;
        break;
      }
      loops++;
    }
    if (!armed) continue;
    caught++;

    // Boucle a NOMBRE FIXE de tours : appeler micros() a chaque iteration la
    // ralentissait a 6,3 us par echantillon, soit moins d'un point par bit.
    // Le chronometre encadre la boucle au lieu de la traverser.
    uint32_t run = 1;
    uint32_t prev = REG_READ(GPIO_IN_REG) & m0;
    // Une salve dure environ 1 ms : une fenetre plus large ne ramasserait
    // que du bruit, qui noierait la distribution.
    const uint32_t nSamp = 4000;
    const uint32_t tA = micros();
    for (uint32_t s = 0; s < nSamp; s++) {
      const uint32_t now = REG_READ(GPIO_IN_REG) & m0;
      if (now == prev) {
        run++;
      } else {
        hist[run > 64 ? 64 : run]++;
        run = 1;
        prev = now;
      }
    }
    elapsedTotal += micros() - tA;
    sampled += nSamp;
  }

  const uint32_t elapsed = elapsedTotal;
  out.printf("  %u salve(s) attrapee(s), %lu echantillons.\n", caught, (unsigned long)sampled);
  if (!caught) {
    out.println("  Aucune salve au-dessus du seuil : baisse-le, ou rapproche");
    out.println("  la source.");
    return;
  }
  if (!sampled) return;

  // Cadence d'echantillonnage reelle, indispensable pour convertir en
  // microsecondes : la boucle ne tourne pas a une vitesse connue d'avance.
  const float sampleUs = (float)elapsed / (float)sampled;
  out.printf("  Cadence mesuree : %.3f us par echantillon.\n", (double)sampleUs);
  out.println("  Duree des paliers (les plus frequents en premier) :");

  char lineBuf[132];
  uint32_t peak = 0;
  for (uint8_t k = 1; k <= 64; k++)
    if (hist[k] > peak) peak = hist[k];
  for (uint8_t k = 1; k <= 48; k++) {
    if (!hist[k]) continue;
    const float us = k * sampleUs;
    const uint8_t bar = peak ? (uint8_t)((uint64_t)hist[k] * 40 / peak) : 0;
    char bars[41];
    for (uint8_t b = 0; b < bar && b < 40; b++) bars[b] = '#';
    bars[bar > 40 ? 40 : bar] = 0;
    snprintf(lineBuf, sizeof(lineBuf), "    %2u = %6.2f us %7.1f kbit/s %7lu %s", k, (double)us,
             (double)(us > 0 ? 1000.0f / us : 0.0f), (unsigned long)hist[k], bars);
    out.println(lineBuf);
    Serial.flush();
  }
  out.println();
  out.println("  Le palier le plus COURT qui revient souvent est la periode");
  out.println("  d'un bit. 8 us = 125 kbit/s, 4 us = 250, 2 us = 500.");
}

// ---------------------------------------------------------------------------
//  Ce que deux salves ont en commun.
//
//  Toutes les autres methodes supposaient quelque chose : un preambule d'une
//  certaine longueur, un CRC d'un certain modele, une trame d'une certaine
//  taille. Celle-ci ne suppose rien. Deux trames emises par la MEME
//  telecommande partagent forcement leur preambule et leur adresse, et ne
//  different que par la charge utile. Il suffit donc de capturer plusieurs
//  salves, de les glisser l'une contre l'autre, et de relever la plus longue
//  suite de bits identiques : c'est le preambule suivi de l'adresse.
//
//  Cette mesure dit aussi, gratuitement, si les trames arrivent lisibles : une
//  correspondance longue signifie une demodulation propre, une correspondance
//  courte signifie que les erreurs binaires hachent tout -- ce qui expliquerait
//  qu'aucun CRC ne tombe juste.
// ---------------------------------------------------------------------------
static uint8_t ccBurst[CC_BURSTS][CC_BURST_BITS / 8];

static inline bool ccBurstBit(uint8_t b, uint16_t i) {
  return (ccBurst[b][i >> 3] >> (7 - (i & 7))) & 1;
}

void ccCommonRuns(Print &out, int trigDbm) {
  const uint8_t gdo0 = ccPins[4], gdo2 = ccPins[5];
  out.println();
  out.println("=== Ce que deux salves ont en commun ===");
  out.printf("  %u salves de %u bits, declenchees au-dessus de %d dBm.\n", CC_BURSTS,
             CC_BURST_BITS, trigDbm);
  out.println("  Aucune hypothese : ni preambule, ni CRC, ni longueur de trame.");
  out.println("  Deux trames de la meme source partagent preambule et adresse.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(10);
  if (radio2.marcState() != cc2500::MARC_RX) {
    out.println("  La puce n'est pas en reception : mesure annulee.");
    return;
  }
  pinMode(gdo0, INPUT);
  pinMode(gdo2, INPUT);

  const uint32_t m0 = 1UL << gdo0, m2 = 1UL << gdo2;
  uint8_t caught = 0;
  memset(ccBurst, 0, sizeof(ccBurst));

  for (uint8_t b = 0; b < CC_BURSTS; b++) {
    bool armed = false;
    const uint32_t giveUp = millis() + 3000;
    while ((int32_t)(millis() - giveUp) < 0) {
      const int dbm = (int)((int8_t)radio2.readStatus(cc2500::STA_RSSI)) / 2 - 72;
      if (dbm >= trigDbm) {
        armed = true;
        break;
      }
    }
    if (!armed) break;

    uint16_t got = 0;
    uint32_t prev = REG_READ(GPIO_IN_REG) & m2;
    uint32_t guard = 0;
    while (got < CC_BURST_BITS && guard < 4000000UL) {
      const uint32_t now = REG_READ(GPIO_IN_REG);
      const uint32_t clk = now & m2;
      if (clk && !prev) {
        if (now & m0) ccBurst[b][got >> 3] |= (uint8_t)(0x80 >> (got & 7));
        got++;
      }
      prev = clk;
      guard++;
    }
    if (got == CC_BURST_BITS) caught++;
  }

  out.printf("  %u salve(s) capturee(s).\n", caught);
  if (caught < 2) {
    out.println("  Moins de deux salves : rien a comparer. Baisse le seuil.");
    return;
  }

  // Glisser chaque paire l'une contre l'autre et relever la plus longue suite
  // de bits identiques.
  uint16_t bestLen = 0;
  uint8_t bestA = 0, bestB = 0;
  int16_t bestShift = 0;
  uint16_t bestPos = 0;
  uint16_t lenHist[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

  for (uint8_t a = 0; a + 1 < caught; a++) {
    for (uint8_t b = (uint8_t)(a + 1); b < caught; b++) {
      uint16_t pairBest = 0;
      for (int16_t sh = -(CC_BURST_BITS - 48); sh <= (CC_BURST_BITS - 48); sh++) {
        uint16_t run = 0;
        for (uint16_t i = 0; i < CC_BURST_BITS; i++) {
          const int32_t j = (int32_t)i + sh;
          if (j < 0 || j >= CC_BURST_BITS) {
            run = 0;
            continue;
          }
          if (ccBurstBit(a, i) == ccBurstBit(b, (uint16_t)j)) {
            run++;
            if (run > pairBest) pairBest = run;
            // N'accepter comme meilleure suite qu'une plage RICHE en
            // transitions. Sans ce garde-fou, la plus longue correspondance est
            // toujours le repos entre deux trames -- une plage de uns, qui
            // coincide evidemment d'une salve a l'autre et masque la vraie.
            if (run > bestLen && run >= 24) {
              const uint16_t st = (uint16_t)(i + 1 - run);
              uint16_t tr = 0;
              for (uint16_t q = 1; q < run; q++)
                if (ccBurstBit(a, (uint16_t)(st + q)) != ccBurstBit(a, (uint16_t)(st + q - 1))) tr++;
              if (tr * 5 >= run) {
                bestLen = run;
                bestA = a;
                bestB = b;
                bestShift = sh;
                bestPos = st;
              }
            }
          } else {
            run = 0;
          }
        }
      }
      const uint8_t bucket = pairBest >= 64 ? 8 : (uint8_t)(pairBest / 8);
      lenHist[bucket]++;
    }
  }

  out.printf("  Plus longue suite commune RICHE : %u bits (salves %u et %u, decalage %d).\n",
             bestLen, bestA, bestB, bestShift);
  out.println("  Distribution sur toutes les paires :");
  char lineBuf[120];
  for (uint8_t k = 0; k <= 8; k++) {
    if (!lenHist[k]) continue;
    snprintf(lineBuf, sizeof(lineBuf), "    %2u-%2u bits communs : %u paire(s)%s", k * 8,
             k == 8 ? 255 : (k * 8 + 7), lenHist[k], k >= 5 ? "   <<< une adresse tient la-dedans" : "");
    out.println(lineBuf);
  }

  if (bestLen >= 24) {
    out.println("  Contenu de cette suite commune :");
    size_t w = (size_t)snprintf(lineBuf, sizeof(lineBuf), "   ");
    for (uint16_t k = 0; k + 8 <= bestLen && k < 96; k += 8) {
      uint8_t v = 0;
      for (uint8_t q = 0; q < 8; q++)
        v = (uint8_t)((v << 1) | (ccBurstBit(bestA, (uint16_t)(bestPos + k + q)) ? 1 : 0));
      w += (size_t)snprintf(lineBuf + w, sizeof(lineBuf) - w, " %02X", v);
    }
    out.println(lineBuf);
    out.println("  Le preambule alterne ouvre la suite ; l'adresse vient juste");
    out.println("  apres, sur quatre octets.");
    // Meme chose decalee d'un bit : le debut de la suite commune ne tombe pas
    // forcement sur une frontiere d'octet.
    for (uint8_t d = 1; d <= 7; d++) {
      size_t w2 = (size_t)snprintf(lineBuf, sizeof(lineBuf), "    +%u bit :", d);
      for (uint16_t k = 0; k + 8 + d <= bestLen && k < 88; k += 8) {
        uint8_t v = 0;
        for (uint8_t q = 0; q < 8; q++)
          v = (uint8_t)((v << 1) | (ccBurstBit(bestA, (uint16_t)(bestPos + k + q + d)) ? 1 : 0));
        w2 += (size_t)snprintf(lineBuf + w2, sizeof(lineBuf) - w2, " %02X", v);
      }
      out.println(lineBuf);
    }
  } else {
    out.println("  Aucune suite commune assez longue pour porter une adresse.");
    out.println("  Soit les salves ne sont pas des trames, soit les erreurs");
    out.println("  binaires hachent la demodulation.");
  }
}

// ---------------------------------------------------------------------------
//  A quelle frequence la source est-elle reellement centree ?
//
//  Mesure a l'appui : les salves de la telecommande arrivent fortes mais le
//  flux demodule ne contient que des uns. C'est la signature d'un
//  discriminateur SATURE -- un signal dont la porteuse est decalee par rapport
//  au centre du recepteur. Le filtre de canal fait 812 kHz, donc le RSSI voit
//  tres bien un signal decale de quelques centaines de kilohertz ; le
//  discriminateur, lui, part contre sa butee et n'en tire plus rien.
//
//  On balaie donc le registre FREQ de part et d'autre de 2405,000 MHz et on
//  retient l'offset qui EQUILIBRE le flux : une demodulation correcte donne
//  autant de uns que de zeros et beaucoup de transitions, une demodulation
//  saturee donne un flux fige.
//
//  Pas de reglage : 26 MHz / 2^16 = 396,7 Hz par unite de FREQ.
// ---------------------------------------------------------------------------
void ccFreqSweep(Print &out, int32_t spanKhz, int32_t stepKhz, uint32_t dwellMs) {
  const uint8_t gdo0 = ccPins[4], gdo2 = ccPins[5];
  out.println();
  out.println("=== Sur quelle frequence la source est-elle centree ? ===");
  out.printf("  Balayage de %+ld a %+ld kHz autour de 2405,000 MHz, pas de %ld kHz.\n",
             (long)-spanKhz, (long)spanKhz, (long)stepKhz);
  out.println("  On mesure l'EQUILIBRE du flux demodule : une demodulation");
  out.println("  correcte donne autant de uns que de zeros, une demodulation");
  out.println("  saturee donne un flux fige. Le taux de uns doit approcher 50 %.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  out.println();
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);
  pinMode(gdo0, INPUT);
  pinMode(gdo2, INPUT);

  const int32_t base = 0x5C8000L;  // 2405,000 MHz avec un quartz de 26 MHz
  const uint32_t m0 = 1UL << gdo0, m2 = 1UL << gdo2;
  char lineBuf[140];
  int32_t bestOff = 0;
  uint32_t bestScore = 0;

  for (int32_t kHz = -spanKhz; kHz <= spanKhz; kHz += stepKhz) {
    // 1 unite de FREQ = 26e6 / 2^16 = 396,7 Hz.
    const int32_t units = (int32_t)((float)kHz * 1000.0f / 396.7f);
    const uint32_t f = (uint32_t)(base + units);
    radio2.strobe(cc2500::STROBE_SIDLE);
    radio2.writeRegister(cc2500::REG_FREQ2, (uint8_t)((f >> 16) & 0xFF));
    radio2.writeRegister(cc2500::REG_FREQ1, (uint8_t)((f >> 8) & 0xFF));
    radio2.writeRegister(cc2500::REG_FREQ0, (uint8_t)(f & 0xFF));
    radio2.strobe(cc2500::STROBE_SCAL);
    delay(3);
    radio2.strobe(cc2500::STROBE_SRX);
    delay(3);

    uint32_t ones = 0, bits = 0, flips = 0;
    int peak = -128;
    bool prevBit = false;
    uint32_t prevClk = REG_READ(GPIO_IN_REG) & m2;
    const uint32_t until = millis() + dwellMs;
    while ((int32_t)(millis() - until) < 0) {
      for (uint16_t n = 0; n < 2048; n++) {
        const uint32_t now = REG_READ(GPIO_IN_REG);
        const uint32_t clk = now & m2;
        if (clk && !prevClk) {
          const bool b = (now & m0) != 0;
          if (b) ones++;
          if (bits && b != prevBit) flips++;
          prevBit = b;
          bits++;
        }
        prevClk = clk;
      }
      const int dbm = (int)((int8_t)radio2.readStatus(cc2500::STA_RSSI)) / 2 - 72;
      if (dbm > peak) peak = dbm;
    }

    const uint32_t pctOnes = bits ? (ones * 100 / bits) : 0;
    const uint32_t pctFlips = bits ? (flips * 1000 / bits) : 0;
    // Un flux equilibre ET riche en transitions : c'est la signature d'une
    // demodulation reussie. On note l'equilibre par sa distance a 50 %.
    const uint32_t balance = (pctOnes > 50) ? (100 - pctOnes) : pctOnes;
    const uint32_t score = balance * pctFlips;
    if (score > bestScore) {
      bestScore = score;
      bestOff = kHz;
    }
    snprintf(lineBuf, sizeof(lineBuf),
             "  %+5ld kHz : %3lu %% de uns, %4lu transitions pour mille, pic %d dBm", (long)kHz,
             (unsigned long)pctOnes, (unsigned long)pctFlips, peak);
    out.println(lineBuf);
    Serial.flush();
  }

  // Remettre la frequence nominale.
  radio2.strobe(cc2500::STROBE_SIDLE);
  radio2.writeRegister(cc2500::REG_FREQ2, 0x5C);
  radio2.writeRegister(cc2500::REG_FREQ1, 0x80);
  radio2.writeRegister(cc2500::REG_FREQ0, 0x00);

  out.println();
  snprintf(lineBuf, sizeof(lineBuf), "  Meilleur equilibre a %+ld kHz.", (long)bestOff);
  out.println(lineBuf);
  out.println("  Un flux proche de 100 %% de uns signale un discriminateur");
  out.println("  sature : la porteuse est ailleurs.");
}

// ---------------------------------------------------------------------------
//  De combien la porteuse de la source est-elle decalee ?
//
//  Le CC2500 estime lui-meme l'ecart de frequence du signal recu et le publie
//  dans FREQEST (0x32), en complement a deux, par pas de fXOSC/2^14 = 1,587
//  kHz. La boucle de compensation est active (FOCCFG=0x1E, limite a BW/4, soit
//  environ 203 kHz).
//
//  On ne lit ce registre que lorsque le RSSI atteste d'un signal : lu au
//  repos, il ne rapporterait que la derive du bruit. Le resultat se compare
//  directement entre la balise, dont on connait le quartz, et la telecommande.
//  Un ecart important expliquerait qu'un signal fort ne se demodule pas.
// ---------------------------------------------------------------------------
void ccFreqOffset(Print &out, int trigDbm, uint32_t dwellMs) {
  out.println();
  out.println("=== De combien la porteuse est-elle decalee ? ===");
  out.printf("  FREQEST lu seulement quand le RSSI depasse %d dBm.\n", trigDbm);
  out.println("  Un pas vaut 1,587 kHz ; la plage utile va de -203 a +203 kHz.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(10);

  // Histogramme par tranches de 16 kHz, de -208 a +208.
  static uint32_t hist[27];
  memset(hist, 0, sizeof(hist));
  uint32_t hits = 0, looks = 0;
  long sum = 0;
  int peak = -128;

  const uint32_t until = millis() + dwellMs;
  while ((int32_t)(millis() - until) < 0) {
    const int dbm = (int)((int8_t)radio2.readStatus(cc2500::STA_RSSI)) / 2 - 72;
    looks++;
    if (dbm > peak) peak = dbm;
    if (dbm < trigDbm) continue;
    const int8_t fe = (int8_t)radio2.readStatus(cc2500::STA_FREQEST);
    const int khz = (int)((float)fe * 1.5869f);
    sum += khz;
    hits++;
    int idx = (khz + 208) / 16;
    if (idx < 0) idx = 0;
    if (idx > 26) idx = 26;
    hist[idx]++;
    if (radio2.marcState() != cc2500::MARC_RX) radio2.strobe(cc2500::STROBE_SRX);
  }

  out.printf("  %lu lecture(s), %lu au-dessus du seuil, pic %d dBm.\n", (unsigned long)looks,
             (unsigned long)hits, peak);
  if (!hits) {
    out.println("  Rien au-dessus du seuil : baisse-le ou fais emettre la source.");
    return;
  }
  out.printf("  Ecart moyen : %+ld kHz.\n", sum / (long)hits);
  out.println("  Distribution :");

  uint32_t top = 0;
  for (uint8_t k = 0; k < 27; k++)
    if (hist[k] > top) top = hist[k];
  char lineBuf[132];
  for (uint8_t k = 0; k < 27; k++) {
    if (!hist[k]) continue;
    const int lo = (int)k * 16 - 208;
    const uint8_t bar = top ? (uint8_t)((uint64_t)hist[k] * 36 / top) : 0;
    char bars[37];
    for (uint8_t b = 0; b < bar && b < 36; b++) bars[b] = '#';
    bars[bar > 36 ? 36 : bar] = 0;
    snprintf(lineBuf, sizeof(lineBuf), "    %+4d a %+4d kHz : %6lu %s", lo, lo + 15,
             (unsigned long)hist[k], bars);
    out.println(lineBuf);
  }
  out.println();
  out.println("  Si la masse est loin de zero, il suffit de deplacer FREQ");
  out.println("  d'autant ; si elle est plaquee contre un bord, l'ecart");
  out.println("  depasse la plage de mesure et il faut balayer plus large.");
}

// ---------------------------------------------------------------------------
//  Vidage brut des salves, pour analyse sur l'ordinateur.
//
//  L'analyse embarquee accumulait les rustines : filtres de richesse, seuils
//  d'alternance, tolerances. Chacune ajoutait une hypothese, et la derniere en
//  date se perdait dans des coincidences. La carte fait ce qu'elle fait bien --
//  echantillonner au front d'horloge -- et l'ordinateur fait le reste, ou l'on
//  peut essayer dix alignements sans reflasher.
// ---------------------------------------------------------------------------
void ccDumpBursts(Print &out, int trigDbm, uint8_t count) {
  const uint8_t gdo0 = ccPins[4], gdo2 = ccPins[5];
  if (count > CC_BURSTS) count = CC_BURSTS;
  out.println();
  out.printf("=== Vidage brut de %u salves (seuil %d dBm) ===\n", count, trigDbm);
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(10);
  pinMode(gdo0, INPUT);
  pinMode(gdo2, INPUT);

  const uint32_t m0 = 1UL << gdo0, m2 = 1UL << gdo2;
  memset(ccBurst, 0, sizeof(ccBurst));
  uint8_t caught = 0;

  for (uint8_t b = 0; b < count; b++) {
    bool armed = false;
    int dbmAt = -128;
    const uint32_t giveUp = millis() + 3000;
    while ((int32_t)(millis() - giveUp) < 0) {
      const int dbm = (int)((int8_t)radio2.readStatus(cc2500::STA_RSSI)) / 2 - 72;
      if (dbm >= trigDbm) {
        armed = true;
        dbmAt = dbm;
        break;
      }
    }
    if (!armed) break;

    uint16_t got = 0;
    uint32_t prev = REG_READ(GPIO_IN_REG) & m2;
    uint32_t guard = 0;
    while (got < CC_BURST_BITS && guard < 4000000UL) {
      const uint32_t now = REG_READ(GPIO_IN_REG);
      const uint32_t clk = now & m2;
      if (clk && !prev) {
        if (now & m0) ccBurst[b][got >> 3] |= (uint8_t)(0x80 >> (got & 7));
        got++;
      }
      prev = clk;
      guard++;
    }
    if (got < CC_BURST_BITS) break;
    caught++;

    char lineBuf[110];
    size_t w = (size_t)snprintf(lineBuf, sizeof(lineBuf), "SALVE %2u %4d ", b, dbmAt);
    for (uint8_t k = 0; k < CC_BURST_BITS / 8; k++)
      w += (size_t)snprintf(lineBuf + w, sizeof(lineBuf) - w, "%02X", ccBurst[b][k]);
    out.println(lineBuf);
    Serial.flush();
  }
  out.printf("=== %u salve(s) ===\n", caught);
}

// ---------------------------------------------------------------------------
//  Capture CONTINUE, vidée telle quelle vers l'ordinateur.
//
//  Mesure a l'appui : toute capture DECLENCHEE sur le RSSI rate l'adresse.
//  Lire le RSSI par SPI bit-bange coute environ 190 us, quand preambule et
//  adresse ne durent ensemble que 384 us -- le temps de detecter, elles sont
//  passees. Les salves ainsi capturees contenaient bien la charge utile de la
//  balise, et jamais son adresse, pas une fois sur vingt-quatre.
//
//  On capture donc en continu, sans declenchement d'aucune sorte, et on laisse
//  l'ordinateur chercher dedans -- ou l'on peut essayer dix hypotheses sans
//  reflasher la carte.
// ---------------------------------------------------------------------------
void ccStream(Print &out, uint32_t nbits, uint8_t passes) {
  const uint8_t gdo0 = ccPins[4], gdo2 = ccPins[5];
  if (nbits > sizeof(ccBits) * 8) nbits = sizeof(ccBits) * 8;
  out.println();
  out.printf("=== Flux continu : %u passe(s) de %lu bits ===\n", passes, (unsigned long)nbits);
  Serial.flush();

  if (!radio2.begin(ccPins[0], ccPins[1], ccPins[2], ccPins[3], ccPins[6], ccPins[7])) {
    out.println("  La puce ne repond pas.");
    return;
  }
  radio2.strobe(cc2500::STROBE_SIDLE);
  for (const auto &r : kCcRxConfig) radio2.writeRegister(r[0], r[1]);
  radio2.setFrontEnd(false, true);
  radio2.strobe(cc2500::STROBE_SRX);
  delay(10);
  if (radio2.marcState() != cc2500::MARC_RX) {
    out.println("  La puce n'est pas en reception.");
    return;
  }
  pinMode(gdo0, INPUT);
  pinMode(gdo2, INPUT);

  const uint32_t m0 = 1UL << gdo0, m2 = 1UL << gdo2;
  for (uint8_t p = 0; p < passes; p++) {
    memset(ccBits, 0, sizeof(ccBits));
    uint32_t got = 0;
    uint32_t prev = REG_READ(GPIO_IN_REG) & m2;
    const uint32_t deadline = millis() + 2000;
    noInterrupts();
    while (got < nbits) {
      const uint32_t now = REG_READ(GPIO_IN_REG);
      const uint32_t clk = now & m2;
      if (clk && !prev) {
        if (now & m0) ccBits[got >> 3] |= (uint8_t)(0x80 >> (got & 7));
        got++;
      }
      prev = clk;
      if ((got & 0x3FF) == 0 && (int32_t)(millis() - deadline) >= 0) break;
    }
    interrupts();

    // Vidage par lignes de 32 octets, prefixees pour etre retrouvees au grep.
    const uint32_t bytesGot = got / 8;
    for (uint32_t k = 0; k < bytesGot; k += 32) {
      char lineBuf[80];
      size_t w = (size_t)snprintf(lineBuf, sizeof(lineBuf), "FLUX %u %5lu ", p, (unsigned long)k);
      for (uint32_t q = k; q < k + 32 && q < bytesGot; q++)
        w += (size_t)snprintf(lineBuf + w, sizeof(lineBuf) - w, "%02X", ccBits[q]);
      out.println(lineBuf);
      Serial.flush();
    }
    out.printf("FIN %u %lu\n", p, (unsigned long)got);
    Serial.flush();
  }
}

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
