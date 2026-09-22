// ===========================================================================
//  CC2500 (Texas Instruments) -- pilote SPI minimal, oriente ECOUTE BRUTE.
//
//  Pourquoi cette puce alors qu'on a deja un BM5602 : le BC5602 est une puce
//  de produit, son moteur de paquets ne rend que ce qui lui est adresse, et
//  mesure a l'appui aucun de ses 16 selecteurs GIO ne sort de bits non
//  decodes. Le CC2500, lui, possede un MODE SERIE : PKTCTRL0.PKT_FORMAT
//  debranche le moteur de paquets et GDO0 devient un simple fil de donnees
//  demodulees. Avec MDMCFG2.SYNC_MODE = 0, il n'exige meme plus de preambule
//  ni de mot de synchro. C'est exactement le flux brut qui nous manque pour
//  lire l'adresse de la telecommande sans la connaitre.
//
//  Module utilise : 24TRGC5-V4 (serigraphie GC-02), CC2500 + etage d'entree
//  RFX2402E (PA + LNA), quartz 26 MHz (marquage T260), antenne sur u.FL.
//  Broches sorties : GND VCC SI SCK SO GDO2 GDO0 CSN PA_EN RX_EN.
// ===========================================================================

#pragma once

#include <Arduino.h>

namespace cc2500 {

// --- Bits d'en-tete SPI -----------------------------------------------------
constexpr uint8_t HDR_WRITE = 0x00;
constexpr uint8_t HDR_READ = 0x80;
constexpr uint8_t HDR_BURST = 0x40;

// --- Commandes strobe -------------------------------------------------------
constexpr uint8_t STROBE_SRES = 0x30;    // reset logiciel
constexpr uint8_t STROBE_SFSTXON = 0x31;
constexpr uint8_t STROBE_SXOFF = 0x32;
constexpr uint8_t STROBE_SCAL = 0x33;    // calibration de la synthese
constexpr uint8_t STROBE_SRX = 0x34;     // passage en reception
constexpr uint8_t STROBE_STX = 0x35;
constexpr uint8_t STROBE_SIDLE = 0x36;
constexpr uint8_t STROBE_SFRX = 0x3A;    // vider la FIFO de reception
constexpr uint8_t STROBE_SFTX = 0x3B;
constexpr uint8_t STROBE_SNOP = 0x3D;    // ne rien faire, lire l'octet d'etat

// --- Registres de configuration --------------------------------------------
constexpr uint8_t REG_IOCFG2 = 0x00;
constexpr uint8_t REG_IOCFG1 = 0x01;
constexpr uint8_t REG_IOCFG0 = 0x02;
constexpr uint8_t REG_FIFOTHR = 0x03;
constexpr uint8_t REG_SYNC1 = 0x04;
constexpr uint8_t REG_SYNC0 = 0x05;
constexpr uint8_t REG_PKTLEN = 0x06;
constexpr uint8_t REG_PKTCTRL1 = 0x07;
constexpr uint8_t REG_PKTCTRL0 = 0x08;
constexpr uint8_t REG_ADDR = 0x09;
constexpr uint8_t REG_CHANNR = 0x0A;
constexpr uint8_t REG_FSCTRL1 = 0x0B;
constexpr uint8_t REG_FSCTRL0 = 0x0C;
constexpr uint8_t REG_FREQ2 = 0x0D;
constexpr uint8_t REG_FREQ1 = 0x0E;
constexpr uint8_t REG_FREQ0 = 0x0F;
constexpr uint8_t REG_MDMCFG4 = 0x10;
constexpr uint8_t REG_MDMCFG3 = 0x11;
constexpr uint8_t REG_MDMCFG2 = 0x12;
constexpr uint8_t REG_MDMCFG1 = 0x13;
constexpr uint8_t REG_MDMCFG0 = 0x14;
constexpr uint8_t REG_DEVIATN = 0x15;
constexpr uint8_t REG_MCSM2 = 0x16;
constexpr uint8_t REG_MCSM1 = 0x17;
constexpr uint8_t REG_MCSM0 = 0x18;
constexpr uint8_t REG_FOCCFG = 0x19;
constexpr uint8_t REG_BSCFG = 0x1A;
constexpr uint8_t REG_AGCCTRL2 = 0x1B;
constexpr uint8_t REG_AGCCTRL1 = 0x1C;
constexpr uint8_t REG_AGCCTRL0 = 0x1D;
constexpr uint8_t REG_FREND1 = 0x21;
constexpr uint8_t REG_FREND0 = 0x22;
constexpr uint8_t REG_FSCAL3 = 0x23;
constexpr uint8_t REG_FSCAL2 = 0x24;
constexpr uint8_t REG_FSCAL1 = 0x25;
constexpr uint8_t REG_FSCAL0 = 0x26;
constexpr uint8_t REG_TEST2 = 0x2C;
constexpr uint8_t REG_TEST1 = 0x2D;
constexpr uint8_t REG_TEST0 = 0x2E;

// --- Registres d'etat (lecture en rafale obligatoire, sinon ce sont des
//     commandes strobe : d'ou HDR_READ | HDR_BURST) --------------------------
constexpr uint8_t STA_PARTNUM = 0x30;    // 0x80 pour un CC2500
constexpr uint8_t STA_VERSION = 0x31;
constexpr uint8_t STA_FREQEST = 0x32;
constexpr uint8_t STA_LQI = 0x33;
constexpr uint8_t STA_RSSI = 0x34;
constexpr uint8_t STA_MARCSTATE = 0x35;
constexpr uint8_t STA_PKTSTATUS = 0x38;
constexpr uint8_t STA_RXBYTES = 0x3B;

// Etats de la machine principale, utiles pour savoir si on ecoute vraiment.
constexpr uint8_t MARC_IDLE = 0x01;
constexpr uint8_t MARC_RX = 0x0D;
constexpr uint8_t MARC_TX = 0x13;

const char *marcStateName(uint8_t state);

class CC2500 {
 public:
  // SPI bit-bange, volontairement. L'ESP32-C6 n'a qu'un controleur SPI
  // generaliste, deja pris par le pilote du BM5602 au demarrage, et il ne se
  // re-route pas proprement ensuite : mesure a l'appui, le peripherique rendait
  // un octet d'etat 0x00 la ou le bit-bang rendait 0x0F sur les MEMES broches.
  // Le debit importe peu ici -- la puce ne sert qu'a se configurer, et les
  // donnees brutes arrivent par GDO0 et GDO2, echantillonnees directement.
  bool begin(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t csn, uint8_t paEn, uint8_t rxEn);

  void strobe(uint8_t cmd);
  void writeRegister(uint8_t reg, uint8_t value);
  uint8_t readRegister(uint8_t reg);
  uint8_t readStatus(uint8_t reg);
  void writeBurst(uint8_t reg, const uint8_t *data, size_t n);
  void readBurst(uint8_t reg, uint8_t *data, size_t n);

  // Reset conforme au datasheet : impulsion sur CSN, attente, SRES, puis on
  // attend que SO retombe -- c'est le seul signal que la puce est prete.
  bool reset();

  uint8_t partNumber() { return readStatus(STA_PARTNUM); }
  uint8_t version() { return readStatus(STA_VERSION); }
  uint8_t marcState() { return (uint8_t)(readStatus(STA_MARCSTATE) & 0x1F); }
  bool present() const { return present_; }

  // Etage d'entree RFX2402E du module. La table de verite etant le genre de
  // detail qui rend un module sourd sans rien dire, les deux broches restent
  // pilotables et un balayage des quatre combinaisons est possible.
  void setFrontEnd(bool paEnable, bool rxEnable);

 private:
  uint8_t sck_ = 0, miso_ = 0, mosi_ = 0, csn_ = 0, paEn_ = 0, rxEn_ = 0;
  uint8_t transferByte(uint8_t v);
  bool present_ = false;

  void select();
  void deselect();
  bool waitReady(uint32_t timeoutMs = 20);
};

}  // namespace cc2500

extern cc2500::CC2500 radio2;
