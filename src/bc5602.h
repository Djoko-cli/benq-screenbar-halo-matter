#pragma once
#include <Arduino.h>
#include <SPI.h>

// ===========================================================================
//  Pilote bas niveau du transceiver 2.4 GHz GFSK Holtek BC5602
//  (module BM5602-60-1). Porte depuis le pilote MicroPython de
//  kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration, references croisees avec
//  le datasheet BC5602 v1.20.
//
//  Le format de paquet est du type Enhanced ShockBurst :
//    preambule 0xAA | adresse 3-5 octets | PCF 9 bits | payload | CRC
//  L'adresse est ecrite en ordre d'octets INVERSE par rapport a l'air.
// ===========================================================================

// Affichage binaire des registres (printf).
#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(b)                                                                       \
  ((b) & 0x80 ? '1' : '0'), ((b) & 0x40 ? '1' : '0'), ((b) & 0x20 ? '1' : '0'),                  \
      ((b) & 0x10 ? '1' : '0'), ((b) & 0x08 ? '1' : '0'), ((b) & 0x04 ? '1' : '0'),              \
      ((b) & 0x02 ? '1' : '0'), ((b) & 0x01 ? '1' : '0')

namespace bc5602 {

// --- Commandes SPI ---------------------------------------------------------
constexpr uint8_t CMD_SET_REGISTER_BANK = 0b00100000;
constexpr uint8_t CMD_WRITE_REGISTER = 0b01000000;
constexpr uint8_t CMD_READ_REGISTER = 0b11000000;
constexpr uint8_t CMD_READ_CHIP_VERSION = 0b10011111;
constexpr uint8_t CMD_SOFTWARE_RESET = 0b00001000;

constexpr uint8_t CMD_WRITE_PTX_ADDRESS = 0b00010000;
constexpr uint8_t CMD_READ_PTX_ADDRESS = 0b10010000;

constexpr uint8_t CMD_WRITE_TX_FIFO_NO_ACK = 0b00010011;
constexpr uint8_t CMD_WRITE_TX_FIFO_WITH_ACK = 0b00010001;

constexpr uint8_t CMD_FLUSH_TX_FIFO = 0b00001001;
constexpr uint8_t CMD_FLUSH_RX_FIFO = 0b10001001;
constexpr uint8_t CMD_READ_RX_FIFO = 0b10111111;

constexpr uint8_t CMD_DEEP_SLEEP = 0b00001010;
constexpr uint8_t CMD_MIDDLE_SLEEP = 0b00001111;  // table des commandes, datasheet p.21
constexpr uint8_t CMD_LIGHT_SLEEP = 0b00001100;
// 0x0D n'apparait PAS dans la table des commandes du datasheet v1.20 (p.21).
// Constate experimentalement : c'est lui qui sort la puce du Light Sleep et
// demarre le synthetiseur (OMST : Calibration -> RX en ~130 us), alors que le
// "RX Mode Trigger" documente (0x8E) laisse la puce en Light Sleep.
constexpr uint8_t CMD_STANDBY = 0b00001101;
constexpr uint8_t CMD_TX_MODE = 0b00001110;
constexpr uint8_t CMD_RX_MODE = 0b10001110;

// --- Registres : zone commune ---------------------------------------------
constexpr uint8_t REG_CFG1 = 0x00;
constexpr uint8_t CFG1_AGC_EN = 0x40;  // controle automatique de gain
constexpr uint8_t CFG1_DIR_EN = 0x10;  // mode direct : moteur de paquets court-circuite
constexpr uint8_t REG_RC1 = 0x01;
constexpr uint8_t REG_MASK = 0x03;
constexpr uint8_t REG_IRQ1 = 0x04;
constexpr uint8_t REG_STATUS = 0x05;
constexpr uint8_t REG_IO1 = 0x06;
constexpr uint8_t REG_IO2 = 0x07;
constexpr uint8_t REG_IO3 = 0x08;
constexpr uint8_t REG_PKT1 = 0x09;
constexpr uint8_t REG_PKT2 = 0x0A;
constexpr uint8_t REG_PKT3 = 0x0B;
constexpr uint8_t REG_PKT4 = 0x0C;   // longueur des donnees dans la FIFO RX
constexpr uint8_t REG_RFCH = 0x10;   // canal = frequence(MHz) - 2400
constexpr uint8_t REG_DM1 = 0x11;    // debit + longueur d'adresse
constexpr uint8_t REG_RT1 = 0x13;    // delai + nombre de retransmissions
constexpr uint8_t REG_RT2 = 0x14;
constexpr uint8_t REG_CE = 0x15;     // bit 0 : CE. PRM_RX=1 + CE=1 => reception continue
constexpr uint8_t CE_ENABLE = 0x01;

// --- Registres : banque 0 --------------------------------------------------
constexpr uint8_t B0_OM = 0x20;
constexpr uint8_t B0_CFO1 = 0x21;
constexpr uint8_t B0_STA1 = 0x26;
constexpr uint8_t B0_RSSI1 = 0x27;   // seuil de detection de porteuse
constexpr uint8_t B0_RSSI2 = 0x28;   // RSSI_NEGDB : mesure temps reel, unite -dB
constexpr uint8_t B0_RSSI3 = 0x29;   // RSSI au moment ou le mot de synchro accroche
constexpr uint8_t B0_DPL1 = 0x2A;    // longueur de payload dynamique
constexpr uint8_t B0_DPL2 = 0x2B;
constexpr uint8_t B0_RXPW0 = 0x2C;   // longueur de payload statique, pipe 0
constexpr uint8_t B0_ENAA = 0x32;    // activation de l'auto-ACK par pipe

// --- Bits utiles -----------------------------------------------------------
constexpr uint8_t ACAL_ENABLE = 0x08;  // OM bit 3 : lance la calibration du VCO

// --- Bits du registre RC1 (0x01), cf. datasheet BC5602 v1.20 p.8 ---
constexpr uint8_t RC1_PWRON = 0x80;      // mise sous tension a froid -> calibration attendue
constexpr uint8_t RC1_FSYCK_RDY = 0x40;  // lecture seule
constexpr uint8_t RC1_XCLK_RDY = 0x20;   // lecture seule : quartz stabilise
constexpr uint8_t RC1_XCLK_EN = 0x10;
constexpr uint8_t RC1_FSYCK_EN = 0x02;

// Datasheet / guide demo : 0 = 500 kbps, 1 = 250 kbps, 2 = 125 kbps.
constexpr uint8_t DATARATE_500K = 0b00000000;
constexpr uint8_t DATARATE_250K = 0b00000001;
constexpr uint8_t DATARATE_125K = 0b00000010;  // valeur heritee du Halo 2
constexpr uint8_t ADDR_LEN_3 = 0b01000000;
constexpr uint8_t ADDR_LEN_4 = 0b10000000;

constexpr uint8_t IO1_4WIRE_SPI = 0b01001000;  // GIO2 devient la sortie donnees SPI

constexpr uint8_t MASK_PRM_RX = 0x01;          // 1 = mode primaire RX
constexpr uint8_t PKT1_CRC_ENABLE = 0b00100000;
constexpr uint8_t ENAA_ALL_PIPES = 0b00111111;

// IRQ1 : RX_DR | TX_DS | MAX_RT. Ces drapeaux se latchent et s'effacent en y
// ecrivant 1 (datasheet v1.20 p.9). Sans acquittement, la puce considere la
// reception terminee et ne delivre plus rien.
constexpr uint8_t IRQ_RX_DR = 0x40;
constexpr uint8_t IRQ_TX_DS = 0x20;
constexpr uint8_t IRQ_MAX_RT = 0x10;
constexpr uint8_t IRQ_CLEAR_ALL = IRQ_RX_DR | IRQ_TX_DS | IRQ_MAX_RT;

constexpr uint8_t OMST_MASK = 0b00000111;  // STA1 bits 2-0 : mode reel de la puce
constexpr uint8_t OMST_LIGHT_SLEEP = 2;
constexpr uint8_t OMST_STANDBY = 3;
constexpr uint8_t OMST_TX = 4;
constexpr uint8_t OMST_RX = 5;
constexpr uint8_t OMST_CALIB = 6;

constexpr uint8_t STATUS_RX_DR = 0x01;         // 0 = donnees disponibles en RX
constexpr uint8_t STATUS_TX_FIFO_EMPTY = 0x10;
constexpr uint8_t STATUS_TX_FIFO_FULL = 0x20;

}  // namespace bc5602

class BC5602 {
 public:
  explicit BC5602(SPIClass &spi = SPI) : spi_(spi), settings_(RF_SPI_HZ_DEFAULT, MSBFIRST, SPI_MODE0) {}

  // Initialise le bus SPI puis le transceiver. Renvoie false si le module ne
  // repond pas (version de puce a 0x000000 ou 0xFFFFFF).
  bool begin(int8_t sck, int8_t miso, int8_t mosi, int8_t csn, uint32_t hz);

  // --- primitives SPI ---
  void command(uint8_t cmd);
  uint8_t readRegister(uint8_t reg);
  void readRegister(uint8_t reg, uint8_t *out, size_t n);
  void writeRegister(uint8_t reg, uint8_t value);
  void writeCommandData(uint8_t cmd, const uint8_t *data, size_t n);
  void readFifo(uint8_t *out, size_t n, bool shiftLeft = false);

  // --- helpers ---
  uint8_t bank();
  void setBank(uint8_t bank);
  uint32_t chipVersion();
  void softwareReset();
  // Charge les valeurs recommandees par Holtek pour les registres reserves
  // (reglages analogiques : LNA, filtre de canal, PLL, modem). Les valeurs de
  // reset ne sont PAS les valeurs operationnelles -- sans cette etape le SPI
  // fonctionne parfaitement et la reception ne demodule rien.
  // Renvoie le nombre de registres qui ne se relisent pas a la valeur ecrite.
  uint8_t registerConfigure(Print *out = nullptr);
  // Lance la calibration du VCO et attend que le materiel remette ACAL_EN a
  // zero. Renvoie false si elle n'a pas abouti dans le delai imparti.
  bool calibrate();
  bool calibrated() const { return calibrated_; }
  // Attend XCLK_RDY (RC1 bit 5). Le quartz met plusieurs ms a se stabiliser :
  // calibrer avant qu'il soit pret ne peut pas aboutir.
  bool waitCrystalReady(uint32_t timeoutMs = 50);
  // Mode reellement rapporte par la puce (STA1/OMST), pas celui qu'on croit
  // avoir demande. Le BC5602 retombe seul en Light Sleep apres un evenement RX.
  uint8_t operationMode();
  // Acquitte les drapeaux d'interruption latches. A appeler apres chaque
  // lecture de FIFO et avant chaque entree en reception.
  void clearInterrupts() {
    writeRegister(bc5602::REG_IRQ1 | bc5602::CMD_WRITE_REGISTER, bc5602::IRQ_CLEAR_ALL);
  }
  // Pose CE=1 puis strobe RX, et attend que OMST rapporte vraiment RX.
  bool enterRxMode(uint32_t timeoutUs = 1500);  // par tentative, 4 tentatives max
  bool crystalReady() const { return crystalReady_; }
  uint8_t lastCalibOM() const { return lastCalibOM_; }
  // Nombre de registres recommandes qui ne se relisent pas a la valeur ecrite.
  uint8_t regCfgMismatches() const { return regCfgMismatches_; }
  void dumpRegisters(Print &out);

  bool present() const { return present_; }

  // Le PCF fait 9 bits : sans auto-ACK le materiel ne le retire pas et tout le
  // flux est decale d'un bit. Ces deux helpers remettent les octets d'aplomb.
  static void shiftLeftOneBit(uint8_t *data, size_t n);
  static void shiftRightOneBit(uint8_t *data, size_t n);

  // Relache les broches SCK/MISO/MOSI pour pouvoir les echantillonner en
  // entree : indispensable pour observer ce que la puce y emet elle-meme.
  void suspendBus();
  void resumeBus();
  int8_t csnPin() const { return csn_; }

 private:
  static constexpr uint32_t RF_SPI_HZ_DEFAULT = 1000000UL;
  void select() { digitalWrite(csn_, LOW); }
  void deselect() { digitalWrite(csn_, HIGH); }
  void dumpBank(Print &out, uint8_t bank, const uint8_t *regs, const char *const *names, size_t n);

  SPIClass &spi_;
  SPISettings settings_;
  int8_t csn_ = -1;
  int8_t sck_ = -1;
  int8_t miso_ = -1;
  int8_t mosi_ = -1;
  bool present_ = false;
  bool calibrated_ = false;
  bool crystalReady_ = false;
  uint8_t lastCalibOM_ = 0xFF;
  uint8_t regCfgMismatches_ = 0xFF;
};
