#pragma once
#include <Arduino.h>
#include <Preferences.h>

#include "bc5602.h"
#include "config.h"

// ===========================================================================
//  Demarrage du module BM5602 et outils de banc
//
//  begin() est le demarrage prouve du module (une seule calibration du VCO) ;
//  le pilote Halo 1 s'en sert aussi de relance complete (niveau L2). Le reste
//  sert au banc et a la retro-ingenierie : emission et ecoute au format
//  standard de la puce (txack, ecoute, prxack) et sondes de la puce, du
//  spectre et de GIO3. Le protocole de la lampe est dans halo1_proto.*, son
//  pilote dans halo1_lamp.*.
//
//  La couche Halo 2 d'origine (charge de 10 octets, etat relu dans l'accuse,
//  interrogation toutes les 5 s) a ete retiree a l'etape C6 : la lampe est un
//  Halo 1, dont l'accuse est vide (docs/PROTOCOL.md).
// ===========================================================================

class BenqHalo {
 public:
  // --- cycle de vie ---
  bool begin();

  // --- configuration persistante des outils (NVS benqhalo : addr, chan, rate) ---
  // Le pilote Halo 1 a la sienne (NVS halo1, 'lampe adresse').
  void loadConfig();
  void saveConfig();
  bool addressConfigured() const;
  void setAddress(const uint8_t addr[4]);
  const uint8_t *address() const { return addr_; }
  void setChannel(uint8_t ch);
  void setPreambleTwoBytes(bool two);
  bool preambleTwoBytes() const { return preambleTwoBytes_; }

  // Debit radio des outils, persiste : l'oublier apres un flash rendrait une
  // ecoute sourde sans le dire. La lampe est a 125 kbps (confirme le 23/09) ;
  // les autres debits ne servent qu'aux sondes.
  uint8_t dataRate() const { return dataRate_; }
  void setDataRate(uint8_t rate);

  // Reappliquer ou non les reglages analogiques Holtek apres chaque reset.
  // Ils etaient effaces par le reset et donc jamais actifs en ecoute.
  void setHoltekTuning(bool on);
  bool holtekTuning() const { return applyHoltekTuning_; }
  static const char *dataRateName(uint8_t rate);
  uint8_t channel() const { return channel_; }

  // Base de tous les outils : la puce en ecoute passive (ni accuse, ni CRC)
  // sur l'adresse et le canal enregistres. Sans effet si le module est absent.
  void prepareForTool();

  // --- outils de banc ---
  // Balaye toute la bande 2400-2483 MHz et releve le RSSI temps reel de chaque
  // canal. Ne depend d'AUCUNE hypothese de protocole : c'est le seul moyen de
  // savoir si l'etage de reception entend quoi que ce soit.
  void scanSpectrum(Print &out, uint8_t passes = 4);

  // Balaye les 84 canaux deux fois, au repos puis molette en main, et
  // ressort les canaux ou le signal monte. N'a de sens qu'avec AGC_EN.
  void sweepBand(Print &out, uint8_t cycles = 4);

  // Campe sur quelques canaux et compare la DISTRIBUTION du RSSI au repos
  // et molette en main. Un canal temoin, connu pour ne porter que du bruit
  // ambiant, sert de controle : s'il ressort comme les autres, la methode
  // ne discrimine rien et son verdict ne vaut rien.
  void probePresence(Print &out, uint8_t cycles = 3, uint32_t dwellMs = 3000);

  // Mesure la DUREE des rafales sur un canal ou l'on entend la telecommande.
  // Une trame Halo 2 fait 19 octets (2 preambule + 4 adresse + 1 PCF + 10 payload +
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
  void calibrationBeacon(Print &out, uint32_t seconds = 120, uint8_t preambleBytes = 2);
  void calibrationListen(Print &out, uint32_t seconds = 15);

  // Verifie si le correlateur sait se caler au MILIEU d'une trame, sur trois
  // octets de payload servant de pseudo-adresse. C'etait le principe de
  // l'ancienne commande 'find' (Halo 2, retiree) : on le teste ici contre une
  // balise dont le payload est connu, donc avec la reponse d'avance.
  void validatePayloadSync(Print &out, uint32_t dwellMs = 3000);

  // Format BC5602 standard, accuse automatique (audit du 23/09).
  void txAck(Print &out, const uint8_t addrReg[4], uint8_t channel, const uint8_t *payload,
             uint8_t len, uint8_t trials, uint16_t gapMs);
  void prxAck(Print &out, const uint8_t addrReg[4], uint8_t channel, uint32_t ms);
  // Ecoute passive, decodage logiciel du PCF et du CRC (halo1::decodeAir),
  // jamais d'accuse.
  void sniffStd(Print &out, const uint8_t addrReg[4], uint8_t channel, uint32_t ms);

  // Trim du quartz reapplique apres chaque reset logiciel ; -1 = ne pas toucher.
  void setXoTrim(int16_t trim);

  // La sequence de reception du projet amont, sans reset logiciel.
  void listenLikeUpstream(Print &out, uint32_t dwellMs, const uint8_t addr[4],
                          uint8_t payloadLen = 13);

  // Polarite et longueur du preambule : douze formes.
  void probePreambleShape(Print &out, uint32_t dwellMs);

  // Le canal 5 porte-t-il la telecommande, ou le Wi-Fi 1 ?
  void discriminateWifi(Print &out, uint32_t phaseMs);

  // Test electrique du fil GIO3, sans la radio.
  void checkGio3Wire(Print &out);
  // Balaie les seize valeurs du selecteur GIO3 pendant que la balise emet,
  // a la recherche d'une sortie de donnees ou d'horloge en RECEPTION. Le
  // datasheet n'en documente que cinq, mais GIO3S=8 (TBCLK) prouve qu'il
  // omet des fonctions reelles. Les valeurs 9 a 15 n'ont jamais ete testees.
  void sweepGio3(Print &out, uint32_t dwellMs = 1500);

  // Determine le DEBIT de la source sans connaitre son adresse. GIO3 ne
  // s'anime que si la puce a detecte un preambule, et la detection de
  // preambule depend du debit : le debit qui fait sortir des transitions
  // est celui de l'emetteur.
  void probeRateByGio3(Print &out, uint32_t dwellMs = 3000);

  // Meme principe pour le CANAL : le RSSI voit large, le demodulateur est
  // etroit. Un signal audible a 2405 MHz peut tres bien etre demodulable
  // seulement deux megahertz plus loin. On balaie donc la bande en
  // regardant GIO3, toujours sans connaitre l'adresse.
  void probeChannelByGio3(Print &out, uint8_t from = 0, uint8_t to = 83,
                          uint32_t dwellMs = 700);

  // Meme balayage, mais avec la sequence d'initialisation EXACTE du pilote
  // tiers, qui recoit reellement d'une Halo : aucun reset logiciel, et
  // aucune des 'valeurs recommandees' de banque 1 et 2. Notre banc prouve
  // que notre emetteur et notre recepteur s'accordent -- pas qu'ils sont
  // regles comme le monde reel.
  void sweepChannelsPico(Print &out, uint8_t from = 0, uint8_t to = 83,
                         uint32_t dwellMs = 1500);

  // Balaie les trois LARGEURS D'ADRESSE croisees avec les canaux. Une
  // largeur fausse fait decouper la trame au mauvais endroit : le
  // correlateur ne peut alors rien accrocher, quel que soit le reste.
  void sweepAddressWidths(Print &out, uint32_t dwellMs = 600);

  // Balaie un registre de modem en surveillant GIO3. Sans argument, cible
  // les six bits de poids faible de CFO1 -- registre litteralement nomme
  // 'Carrier Frequency Offset', dont ces bits sont marques reserves. Un
  // decalage de porteuse expliquerait qu'un emetteur audible reste
  // indemodulable.
  void sweepModemRegister(Print &out, int bank = -1, uint8_t reg = 0,
                          uint32_t dwellMs = 800);

  // Les 'valeurs recommandees' survivent-elles au reset logiciel ? Si non,
  // elles ne sont jamais actives pendant les chasses -- qui commencent
  // toutes par un reset -- et les balayer serait explorer du vide.
  void compareAfterReset(Print &out);

  // LA question decisive : les sorties trouvees sur GIO3 sont-elles AVANT
  // ou APRES le correlateur ? On refait la mesure avec une adresse fausse.
  // Si l'activite persiste alors qu'aucune trame n'est acceptee, la sortie
  // est en amont du filtrage -- donc exploitable sans connaitre l'adresse.
  void checkGio3Correlator(Print &out, uint32_t dwellMs = 2000);

  // Capture le flux de bits demodule sur GIO3 et y cherche le preambule,
  // puis lit les octets qui suivent -- c'est-a-dire l'ADRESSE. Valide
  // d'abord contre la balise, dont l'adresse est connue d'avance.
  void captureGio3Bits(Print &out, uint8_t selector = 14, uint32_t attempts = 40);

  // Auto-test d'une seule carte, sans partenaire radio : chaque maillon est
  // verifie par une RELECTURE, du bus SPI jusqu'au remplissage de la FIFO
  // d'emission. Repond a la question 'est-ce mon cablage ou mon code ?'.
  void selfTest(Print &out);

  // Ecoute passive du bus SPI d'un appareil tiers. Le peripherique SPI de
  // l'ESP32 est mis en ESCLAVE : il est cadence par l'horloge observee et
  // reconstitue les octets exactement, la ou un echantillonnage par boucle
  // raterait des bits. MISO reste non assigne : on n'emet rien sur le bus.
  void sniffSpiBus(Print &out, uint32_t seconds = 60);

  // Rend les broches SPI a un autre usage -- le C6 n'a qu'un peripherique
  // utilisable, et le bit-banging SWD reclame les memes broches.
  void releaseSpiBus() { radio.suspendBus(); }
  void restoreSpiBus() { radio.resumeBus(); }

  // Dit, ligne par ligne, si le contact tient. Une ligne reellement pilotee
  // par l'appareil observe ignore les resistances internes de l'ESP32 ; une
  // ligne qui flotte les suit docilement. Sans ce controle, une capture de
  // bruit ressemble a une capture ratee pour une toute autre raison.
  void tapTest(Print &out, uint32_t seconds = 20);
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
  void printInfo(Print &out);

  BC5602 radio;
  BC5602 radio2;  // second module, emetteur d'etalonnage

 private:
  void sharedRadioConfig(uint8_t addrLenBits, const uint8_t *addr, size_t addrLen);
  // Ecoute passive sur l'adresse enregistree, ni accuse ni CRC (base des outils).
  void prepareToSniff();
  // Reset logiciel + reconfiguration complete. Mesure a l'appui : sans reset
  // prealable la puce refuse d'entrer en RX, alors qu'avec elle y tient a 99 %.
  void resetRadio();
  void sweepRssi(uint8_t *out84, uint8_t passes);

  Preferences prefs_;

  uint8_t addr_[4] = {0, 0, 0, 0};
  uint8_t channel_ = RF_CHANNEL_1;

  // Longueur de preambule attendue. Doit etre REAPPLIQUEE apres chaque reset
  // logiciel : celui-ci remet tous les registres aux valeurs de mise sous
  // tension, ce qui effacait silencieusement le reglage.
  bool preambleTwoBytes_ = false;
  uint8_t dataRate_ = bc5602::DATARATE_125K;
  bool applyHoltekTuning_ = true;
};

extern BenqHalo halo;
