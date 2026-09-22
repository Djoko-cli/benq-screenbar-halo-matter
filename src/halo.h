#pragma once
#include <Arduino.h>
#include <Preferences.h>

#include "bc5602.h"
#include "config.h"

// ===========================================================================
//  Couche protocole BenQ ScreenBar Halo
//
//  Payload de 10 octets, echange en Enhanced ShockBurst avec auto-ACK :
//    [0] commande
//    [1] registre de controle (bits, voir plus bas)
//    [2] luminosite lampe avant   (0x01..0x64 = 1..100 %)
//    [3] temperature de couleur   octet de poids fort (Kelvin, 0x0A8C..0x1964)
//    [4] temperature de couleur   octet de poids faible
//    [5] luminosite lampe arriere (0x01..0x64)
//    [6] temperature de couleur arriere - poids fort (identique a l'avant)
//    [7] temperature de couleur arriere - poids faible
//    [8] octet de queue 0 (0x01 sur le Halo 2 observe)
//    [9] octet de queue 1 (0x02 sur le Halo 2 observe)
//
//  Registre de controle (octet 1) :
//    bit 0  marche/arret general
//    bit 1  mode Auto
//    bit 2  Favori
//    bit 3  \ 0 = avant seule, 1 = arriere seule, 2 = les deux
//    bit 4  /
//    bit 5  capteur (ultrason sur le Halo 2 - a confirmer sur le Halo 1)
//    bits 6-7 inutilises
//
//  La lampe n'emet JAMAIS spontanement : elle ne repond que dans le slot ACK
//  materiel. D'ou l'interrogation periodique + l'ecoute passive de la
//  telecommande entre deux interrogations.
// ===========================================================================

// Commandes (octet 0 du payload)
enum : uint8_t {
  HALO_CMD_HELLO = 0x00,    // la telecommande se reveille et contacte la lampe
  HALO_CMD_ONOFF = 0x02,    // allumage / extinction general
  HALO_CMD_SET = 0x03,      // luminosite + temperature de couleur
  HALO_CMD_SYNC = 0x04,     // demande d'etat
  HALO_CMD_SLEEP = 0x05,    // la telecommande s'endort
  HALO_CMD_PAIRING = 0x0A,  // appairage
  HALO_CMD_NONE = 0xFF,
};

// Adresse fixe utilisee pendant l'appairage (E2 08 00 B0 sur l'air),
// en ordre d'ecriture registre.
constexpr uint8_t HALO_PAIRING_ADDRESS[4] = {0xB0, 0x00, 0x08, 0xE2};

struct HaloState {
  bool power = true;
  bool front = true;
  bool back = false;
  bool sensor = true;
  uint8_t frontBrightness = 50;  // 1..100 %
  uint8_t backBrightness = 50;   // 1..100 %
  uint16_t colorTempK = 4000;    // 2700..6500 K

  bool operator==(const HaloState &o) const {
    return power == o.power && front == o.front && back == o.back && sensor == o.sensor &&
           frontBrightness == o.frontBrightness && backBrightness == o.backBrightness &&
           colorTempK == o.colorTempK;
  }
  bool operator!=(const HaloState &o) const { return !(*this == o); }
};

enum class HaloMode : uint8_t { Normal, Sniffer, Finder };
enum class HaloPhase : uint8_t { Idle, Push, Verify };

class BenqHalo {
 public:
  // --- cycle de vie ---
  bool begin();
  void tick();  // a appeler depuis loop(), aucune operation ne bloque > ~15 ms

  // --- configuration persistante (NVS) ---
  void loadConfig();
  void saveConfig();
  bool addressConfigured() const;
  void setAddress(const uint8_t addr[4]);
  const uint8_t *address() const { return addr_; }
  void setTail(uint8_t a, uint8_t b);
  const uint8_t *tail() const { return tail_; }
  void setChannel(uint8_t ch);
  void setPreambleTwoBytes(bool two);
  bool preambleTwoBytes() const { return preambleTwoBytes_; }
  uint8_t channel() const { return channel_; }

  // --- configuration radio ---
  void prepareToTransfer();  // mode standard : auto-ACK + CRC + payload dynamique
  void prepareToSniff();     // mode ecoute : ni ACK, ni CRC, ni payload dynamique
  // Reset logiciel + reconfiguration complete. Mesure a l'appui : sans reset
  // prealable la puce refuse d'entrer en RX, alors qu'avec elle y tient a 99 %.
  void resetRadio();

  // --- echanges bruts ---
  bool sendWithAck(const uint8_t payload[10]);
  bool readAck(uint8_t out[10]);
  bool sniffOnce(uint8_t payload[10], uint8_t *pcfLen = nullptr, uint8_t *pid = nullptr,
                 uint8_t *noAck = nullptr);

  // --- protocole ---
  // CRC-CCITT (polynome 0x1021), etat initial 0xEFDF avant les quatre octets
  // d'adresse EN ORDRE SUR L'AIR, couvrant adresse + PCF + payload.
  // Modele valide sur les trois vecteurs publies par Termina1 :
  //   PCF 54 -> 20B9 | PCF 50 -> E962 | PCF 50 -> 0241
  uint16_t frameCrc(uint8_t pcf, const uint8_t payload[10]) const;
  // Variante pour une adresse arbitraire, donnee EN ORDRE SUR L'AIR. Sert a
  // valider une adresse candidate pendant la recherche.
  static uint16_t frameCrcFor(const uint8_t airAddr[4], uint8_t pcf, const uint8_t payload[10]);
  // Verifie le CRC d'une trame brute de 13 octets (PCF + payload + CRC).
  bool frameCrcOk(const uint8_t frame13[13]) const;

  void buildPayload(uint8_t cmd, uint8_t out[10], bool autoMode = false) const;
  bool validate(const uint8_t p[10]) const;
  void parseStatus(const uint8_t p[10]);

  // --- haut niveau, non bloquant ---
  void requestPush(uint8_t cmd = HALO_CMD_SET, bool autoMode = false);
  void requestPushThen(uint8_t first, uint8_t second);
  bool pollNow(uint8_t cmd = HALO_CMD_SYNC);
  // true quand aucun envoi n'est en cours depuis assez longtemps pour que
  // refleter l’etat vers Matter ne provoque pas de va-et-vient.
  bool settled() const;

  // --- modes de retro-ingenierie ---
  void setMode(HaloMode mode);
  HaloMode mode() const { return mode_; }
  // addr = nullptr -> adresse configuree ; sinon ecoute sur une autre adresse
  // (typiquement HALO_PAIRING_ADDRESS pendant un appairage).
  void startSniffer(const uint8_t addr[4] = nullptr);
  void findAddressBegin(const uint8_t sync3[3], uint32_t durationMs, bool sweepChannels = false);
  // Nombre de trames brutes sorties de la FIFO depuis le debut du mode courant,
  // sans aucun filtrage applicatif. C'est le seul indicateur qui distingue
  // "la radio n'entend rien" de "elle entend mais le mot de synchro est faux".
  uint32_t rxEvents() const { return rxEvents_; }
  // Balaye toute la bande 2400-2483 MHz et releve le RSSI temps reel de chaque
  // canal. Ne depend d'AUCUNE hypothese de protocole : c'est le seul moyen de
  // savoir si l'etage de reception entend quoi que ce soit.
  void scanSpectrum(Print &out, uint8_t passes = 4);

  // Balaye les 84 canaux deux fois, au repos puis molette en main, et
  // ressort les canaux ou le signal monte. N'a de sens qu'avec AGC_EN.
  void sweepBand(Print &out, uint8_t cycles = 4);

  // Capture sur l'adresse d'appairage, la seule que nous connaissions.
  // Balaye les 3 canaux FCC et les deux ordres d'octets, a 125 kbps, et
  // vide 32 octets par trame pour voir passer une eventuelle adresse de
  // communication negociee pendant l'appairage.
  void capturePairing(Print &out, uint32_t seconds = 180);

  // Cale le correlateur sur le PREAMBULE plutot que sur l'adresse. Avec une
  // adresse de 3 octets valant 'AA AA X', il accroche les deux octets de
  // preambule suivis du premier octet d'adresse : la puce livre alors les
  // trois octets d'adresse restants. Un seul inconnu, X, sur 256 valeurs.
  void huntByPreamble(Print &out, uint32_t dwellMs = 500);

  // Campe sur quelques canaux et compare la DISTRIBUTION du RSSI au repos
  // et molette en main. Un canal temoin, connu pour ne porter que du bruit
  // ambiant, sert de controle : s'il ressort comme les autres, la methode
  // ne discrimine rien et son verdict ne vaut rien.
  void probePresence(Print &out, uint8_t cycles = 3, uint32_t dwellMs = 3000);

  // Mesure la DUREE des rafales sur un canal ou l'on entend la telecommande.
  // Une trame fait 19 octets (2 preambule + 4 adresse + 1 PCF + 10 payload +
  // 2 CRC), soit 152 bits : 1216 us a 125 kbps, 608 a 250, 304 a 500. La
  // duree mesuree donne donc le debit, et recoupe la longueur de trame.
  void measureBursts(Print &out, uint32_t seconds = 20, uint8_t threshold = 60);

  // Etalonnage du recepteur avec le second module. Tant que cette boucle ne
  // rend pas les octets emis, aucun resultat negatif d'une chasse ne porte
  // d'information : un recepteur mort rend zero pour toutes les valeurs.
  void loopbackTest(Print &out, uint16_t frames = 50);

  // Etalonnage a DEUX CARTES : meme firmware des deux cotes, seul le role
  // change. Une carte emet un motif connu, l'autre ecoute puis verifie
  // qu'elle rejette bien une adresse fausse d'un octet.
  void calibrationBeacon(Print &out, uint32_t seconds = 120);
  void calibrationListen(Print &out, uint32_t seconds = 15);

  // Auto-test d'une seule carte, sans partenaire radio : chaque maillon est
  // verifie par une RELECTURE, du bus SPI jusqu'au remplissage de la FIFO
  // d'emission. Repond a la question 'est-ce mon cablage ou mon code ?'.
  void selfTest(Print &out);
  // Trace la machine d'etats de la puce pendant une tentative d'entree en RX,
  // puis en TX pour comparaison. Dit ou exactement la transition echoue.
  void diagnoseRx(Print &out);
  // Compare le spectre au repos et pendant que l'utilisateur actionne la
  // telecommande. Ne suppose RIEN du protocole : on cherche juste ou de
  // l'energie apparait. C'est le seul moyen de connaitre le vrai canal.
  void huntRemote(Print &out);
  // Se gare sur un canal et echantillonne le RSSI au rythme maximal pendant
  // quelques secondes. Adapte aux emetteurs impulsionnels, la ou un balayage
  // rate les salves courtes.
  void watchChannel(Print &out, uint8_t ch, uint32_t durationMs = 8000);
  // Essaie toutes les sequences d'activation plausibles et mesure, pour
  // chacune, si la puce atteint le mode RX et surtout COMBIEN DE TEMPS elle y
  // reste. Un seul passage remplace des dizaines de cycles de flash.
  void probeRxSequences(Print &out);
  // Balaie les 8 valeurs du selecteur GIO2S en comptant les transitions de la
  // broche. Le datasheet n'en documente que 3 et clot par "Others: No function",
  // mais Termina1 a trouve GIO2S=3 (donnees directes en emission) par l'essai.
  // Si une valeur sort des bits demodules en RECEPTION, on capture le flux brut
  // sans connaitre aucune adresse.
  void probeGioFunctions(Print &out);

  // Sonde CFG1.DIR_EN : en mode direct la puce court-circuite le moteur de
  // paquets et delivre les bits demodules bruts, sans correlateur donc sans
  // adresse. Le datasheet ne dit pas sur quelle broche : on les teste toutes.
  void probeDirectMode(Print &out, uint32_t windowMs = 3000);

  // Reception en mode direct. Deux inconnues a lever : comment atteindre le
  // mode RX quand DIR_EN=1, et quel selecteur GIO2 sort les bits demodules.
  // Le depot tiers prouve que GIO2S=3 est DIRECT_TXD et GIO3S=8 TBCLK, deux
  // valeurs que le datasheet range dans 'Others: No function'.
  void probeDirectRx(Print &out, uint32_t windowMs = 2000);
  void printFinderSummary(Print &out);
  void printState(Print &out) const;
  void printInfo(Print &out);

  HaloState desired;
  HaloState reported;
  bool debug = false;
  BC5602 radio;
  BC5602 radio2;  // second module, emetteur d'etalonnage

 private:
  void tickNormal(uint32_t now);
  void tickSniffer(uint32_t now);
  void tickFinder(uint32_t now);
  void sharedRadioConfig(uint8_t addrLenBits, const uint8_t *addr, size_t addrLen);
  void checkTxFifo();
  void adoptReported();
  void noteFinderCandidate(const uint8_t addr[4]);
  void sweepRssi(uint8_t *out84, uint8_t passes);

  Preferences prefs_;

  uint8_t addr_[4] = {0, 0, 0, 0};
  uint8_t tail_[2] = {0x01, 0x02};
  uint8_t channel_ = RF_CHANNEL_1;

  uint8_t sniffAddr_[4] = {0, 0, 0, 0};
  bool sniffOverride_ = false;

  HaloMode mode_ = HaloMode::Normal;
  HaloPhase phase_ = HaloPhase::Idle;
  bool ackMode_ = false;
  bool txBusy_ = false;

  uint8_t pendingCmd_ = HALO_CMD_NONE;
  uint8_t followUpCmd_ = HALO_CMD_NONE;
  bool pendingAuto_ = false;
  uint8_t verifyRef_[9] = {0};
  uint8_t verifyTries_ = 0;

  uint32_t dirtyAt_ = 0;
  uint32_t lastOp_ = 0;
  uint32_t lastPoll_ = 0;
  uint32_t settledAt_ = 0;

  uint32_t rxEvents_ = 0;
  // Longueur de payload programmee dans RXPW0 pour le mode courant. En longueur
  // statique, PKT4/RXDLEN n'est alimente que par le decodeur de payload
  // dynamique : s'y fier ferait jeter des trames parfaitement recues.
  uint8_t staticRxLen_ = 12;
  bool sweepChannels_ = false;
  uint8_t sweepIdx_ = 0;       // index combine : debit x canal
  uint8_t addrLenBits_ = bc5602::ADDR_LEN_4;  // memorise pour pouvoir reecrire DM1
  // Longueur de preambule attendue. Doit etre REAPPLIQUEE apres chaque reset
  // logiciel : celui-ci remet tous les registres aux valeurs de mise sous
  // tension, ce qui effacait silencieusement le reglage.
  bool preambleTwoBytes_ = false;
  uint32_t sweepAt_ = 0;

  // mode Finder
  static constexpr uint8_t kMaxCandidates = 8;
  uint8_t candAddr_[kMaxCandidates][4] = {};
  uint16_t candHits_[kMaxCandidates] = {};
  uint8_t candCount_ = 0;
  uint32_t finderDeadline_ = 0;
  bool finderConfirmed_ = false;
  uint8_t finderConfirmedAddr_[4] = {0};
};

extern BenqHalo halo;
