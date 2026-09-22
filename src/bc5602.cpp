#include "bc5602.h"

using namespace bc5602;

bool BC5602::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t csn, uint32_t hz) {
  csn_ = csn;
  sck_ = sck;
  miso_ = miso;
  mosi_ = mosi;
  settings_ = SPISettings(hz, MSBFIRST, SPI_MODE0);

  pinMode(csn_, OUTPUT);
  digitalWrite(csn_, HIGH);
  spi_.begin(sck, miso, mosi, -1);

  // Reset logiciel systematique. Le reset de l'ESP32 ne coupe PAS le 3,3 V du
  // module : sans cela la puce conserve d'un flash a l'autre tout l'etat laisse
  // par la session precedente, y compris des configurations a moitie faites.
  command(CMD_SOFTWARE_RESET);
  delay(20);

  // En sortie de reset le BC5602 est en SPI 3 fils : SDIO est bidirectionnel et
  // GIO2 n'emet rien. L'ecriture fonctionne quand meme, donc on bascule en 4
  // fils AVANT toute lecture.
  writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);

  command(CMD_LIGHT_SLEEP);
  delay(1);

  uint32_t version = chipVersion();
  present_ = (version != 0x000000UL) && (version != 0x00FFFFFFUL);
  if (!present_) return false;

  setBank(0);

  // Avant toute chose : charger les reglages analogiques recommandes. Fait
  // AVANT la calibration, pour que l'ACAL du VCO tourne avec la PLL reglee.
  regCfgMismatches_ = registerConfigure();
  clearInterrupts();

  // PRM_RX = 1 : on demarre en recepteur primaire.
  uint8_t mask = readRegister(REG_MASK | CMD_READ_REGISTER);
  writeRegister(REG_MASK | CMD_WRITE_REGISTER, mask | MASK_PRM_RX);

  // RC1 bit 7 (PWRON) signale une mise sous tension a froid. Le datasheet est
  // explicite : ce drapeau sert a decider s'il faut lancer l'auto-calibration
  // du VCO, en Light Sleep. Sans elle le synthetiseur RF ne se cale pas et la
  // radio reste sourde -- alors que le SPI, lui, repond parfaitement, ce qui
  // rend la panne trompeuse. Le projet amont sautait cette etape.
  // On calibre a CHAQUE demarrage, sans conditionner a PWRON. Ce drapeau ne se
  // repose qu'a une coupure d'alimentation du BM5602 : un simple reset de
  // l'ESP32 ne coupe pas le module, donc PWRON resterait a 0 pour toujours et
  // la calibration ne serait plus jamais rejouee. Elle coute quelques ms.
  uint8_t rc1 = readRegister(REG_RC1 | CMD_READ_REGISTER);
  command(CMD_LIGHT_SLEEP);
  // Sans cette attente on calibrait 1 ms apres la mise sous tension, quartz
  // non stabilise : la calibration ne pouvait pas aboutir.
  crystalReady_ = waitCrystalReady();
  calibrated_ = crystalReady_ && calibrate();
  if (rc1 & RC1_PWRON) writeRegister(REG_RC1 | CMD_WRITE_REGISTER, (uint8_t)(rc1 & ~RC1_PWRON));

  return true;
}

// ---------------------------------------------------------------------------
//  Valeurs recommandees par le datasheet BC5602 v1.20
//    zone commune p.6 | banque 1 p.22 | banque 2 p.23
//  0x34 / 0x35 (RFTXP, puissance d'emission) sont volontairement exclus : ce ne
//  sont pas des registres "recommended", les toucher changerait la puissance TX.
// ---------------------------------------------------------------------------

namespace {
struct RegInit {
  uint8_t bank;
  uint8_t reg;
  uint8_t val;
};
const RegInit kRecommended[] = {
    {0, 0x0D, 0x20}, {0, 0x0F, 0x14}, {0, 0x16, 0x66}, {0, 0x17, 0xAA}, {0, 0x18, 0x45},
    {1, 0x20, 0x0C}, {1, 0x21, 0x03}, {1, 0x23, 0x10}, {1, 0x25, 0xCC}, {1, 0x26, 0x4C},
    {1, 0x27, 0x80}, {2, 0x28, 0xA0}, {2, 0x2D, 0x18}, {2, 0x2E, 0xEC}, {2, 0x36, 0x03},
    {2, 0x38, 0x0A}, {2, 0x39, 0x12}, {2, 0x3B, 0x94}, {2, 0x3C, 0x43},
};
}  // namespace

// Relecture seule : combien des valeurs recommandees sont REELLEMENT en place
// a cet instant ? Sert de temoin a une mesure -- un resultat nul obtenu avec un
// modem revenu a ses valeurs d'usine ne dit rien du signal cherche.
uint8_t BC5602::registerVerify(uint8_t *total) {
  const uint8_t saved = bank();
  uint8_t ok = 0;
  for (const RegInit &r : kRecommended) {
    setBank(r.bank);
    if (readRegister(r.reg | CMD_READ_REGISTER) == r.val) ok++;
  }
  setBank(saved);
  if (total) *total = (uint8_t)(sizeof(kRecommended) / sizeof(kRecommended[0]));
  return ok;
}

uint8_t BC5602::registerConfigure(Print *out) {
  uint8_t bank = 0xFF;
  for (const RegInit &r : kRecommended) {
    if (r.bank != bank) {
      setBank(r.bank);
      bank = r.bank;
    }
    writeRegister(r.reg | CMD_WRITE_REGISTER, r.val);
  }

  uint8_t mismatches = 0;
  bank = 0xFF;
  for (const RegInit &r : kRecommended) {
    if (r.bank != bank) {
      setBank(r.bank);
      bank = r.bank;
    }
    uint8_t got = readRegister(r.reg | CMD_READ_REGISTER);
    if (got != r.val) {
      mismatches++;
      if (out) out->printf("  banque %u reg 0x%02X : ecrit 0x%02X, relu 0x%02X\n", r.bank, r.reg, r.val, got);
    }
  }

  // Imperatif, pas cosmetique : calibrate() ecrit OM (0x20), qui en banque 2
  // designerait SET1.
  setBank(0);
  return mismatches;
}

// ---------------------------------------------------------------------------
//  Primitives SPI
// ---------------------------------------------------------------------------

void BC5602::command(uint8_t cmd) {
  spi_.beginTransaction(settings_);
  select();
  spi_.transfer(cmd);
  deselect();
  spi_.endTransaction();
}

uint8_t BC5602::readRegister(uint8_t reg) {
  uint8_t value = 0;
  readRegister(reg, &value, 1);
  return value;
}

void BC5602::readRegister(uint8_t reg, uint8_t *out, size_t n) {
  spi_.beginTransaction(settings_);
  select();
  spi_.transfer(reg);
  for (size_t i = 0; i < n; i++) out[i] = spi_.transfer(0x00);
  deselect();
  spi_.endTransaction();
}

void BC5602::writeRegister(uint8_t reg, uint8_t value) {
  spi_.beginTransaction(settings_);
  select();
  spi_.transfer(reg);
  spi_.transfer(value);
  deselect();
  spi_.endTransaction();
}

void BC5602::writeCommandData(uint8_t cmd, const uint8_t *data, size_t n) {
  spi_.beginTransaction(settings_);
  select();
  spi_.transfer(cmd);
  for (size_t i = 0; i < n; i++) spi_.transfer(data[i]);
  deselect();
  spi_.endTransaction();
}

void BC5602::readFifo(uint8_t *out, size_t n, bool shiftLeft) {
  spi_.beginTransaction(settings_);
  select();
  spi_.transfer(CMD_READ_RX_FIFO);
  for (size_t i = 0; i < n; i++) out[i] = spi_.transfer(0x00);
  deselect();
  spi_.endTransaction();
  if (shiftLeft) shiftLeftOneBit(out, n);
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

uint8_t BC5602::bank() { return readRegister(REG_CFG1 | CMD_READ_REGISTER) & 0b00000011; }

void BC5602::setBank(uint8_t bank) {
  uint8_t cfg1 = readRegister(REG_CFG1 | CMD_READ_REGISTER) & 0b11111100;
  writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, cfg1 | (bank & 0b11));
}

uint32_t BC5602::chipVersion() {
  uint8_t buf[3] = {0, 0, 0};
  readRegister(CMD_READ_CHIP_VERSION | CMD_READ_REGISTER, buf, 3);
  return ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
}

void BC5602::suspendBus() { spi_.end(); }

void BC5602::resumeBus() { spi_.begin(sck_, miso_, mosi_, -1); }

void BC5602::softwareReset() {
  command(CMD_SOFTWARE_RESET);
  delay(20);
}

uint8_t BC5602::operationMode() {
  return readRegister(B0_STA1 | CMD_READ_REGISTER) & OMST_MASK;
}

bool BC5602::enterRxMode(uint32_t timeoutUs) {
  // Sequence reprise de l'implementation ESPHome de Termina1, qui recoit
  // effectivement des trames du Halo 2 :
  //   PRM_RX=1, vider la FIFO RX, acquitter RX_DR, puis RX_MODE (0x8E).
  // Ni CE, ni 0x0D, ni reset prealable. Mes tentatives precedentes echouaient
  // faute d'acquitter RX_DR, pas faute de la bonne commande.
  uint8_t mask = readRegister(REG_MASK | CMD_READ_REGISTER);
  writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask | MASK_PRM_RX));

  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    if (operationMode() == OMST_RX) return true;
    command(CMD_FLUSH_RX_FIFO);
    writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
    command(CMD_RX_MODE);
    for (uint32_t waited = 0; waited < timeoutUs; waited += 50) {
      if (operationMode() == OMST_RX) return true;
      delayMicroseconds(50);
    }
  }
  return operationMode() == OMST_RX;
}

bool BC5602::waitCrystalReady(uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (!(readRegister(REG_RC1 | CMD_READ_REGISTER) & RC1_XCLK_RDY)) {
    if ((int32_t)(millis() - deadline) >= 0) {
      crystalReady_ = false;
      return false;
    }
    delayMicroseconds(200);
  }
  crystalReady_ = true;
  return true;
}

bool BC5602::calibrate() {
  setBank(0);
  writeRegister(B0_OM | CMD_WRITE_REGISTER, ACAL_ENABLE);
  delayMicroseconds(100);

  // Le materiel remet ACAL_EN a zero quand la calibration du VCO est terminee.
  // On teste le bit au masque : l'egalite stricte du pilote d'origine sortait
  // de la boucle des qu'un autre bit d'OM etait pose.
  uint32_t deadline = millis() + 200;
  while ((readRegister(B0_OM | CMD_READ_REGISTER) & ACAL_ENABLE) &&
         (int32_t)(millis() - deadline) < 0) {
    delayMicroseconds(200);
  }
  lastCalibOM_ = readRegister(B0_OM | CMD_READ_REGISTER);
  calibrated_ = !(lastCalibOM_ & ACAL_ENABLE);
  return calibrated_;
}

// ---------------------------------------------------------------------------
//  Decalage d'un bit (compensation du PCF sur 9 bits en mode sans auto-ACK)
// ---------------------------------------------------------------------------

void BC5602::shiftLeftOneBit(uint8_t *data, size_t n) {
  if (n == 0) return;
  for (size_t i = 0; i + 1 < n; i++) data[i] = (uint8_t)((data[i] << 1) | (data[i + 1] >> 7));
  data[n - 1] = (uint8_t)(data[n - 1] << 1);
}

void BC5602::shiftRightOneBit(uint8_t *data, size_t n) {
  if (n == 0) return;
  for (size_t i = n; i-- > 0;) {
    uint8_t carry = (i > 0) ? (uint8_t)((data[i - 1] & 0x01) << 7) : 0;
    data[i] = (uint8_t)((data[i] >> 1) | carry);
  }
}

// ---------------------------------------------------------------------------
//  Dump de registres (commande CLI "regs")
// ---------------------------------------------------------------------------

void BC5602::dumpBank(Print &out, uint8_t bank, const uint8_t *regs, const char *const *names, size_t n) {
  setBank(bank);
  out.printf("--- Banque %u ---\n", bank);
  for (size_t i = 0; i < n; i++) {
    uint8_t v = readRegister(regs[i] | CMD_READ_REGISTER);
    out.printf("  %-8s (0x%02X) = 0x%02X  0b" BYTE_TO_BINARY_PATTERN "\n", names[i], regs[i], v,
               BYTE_TO_BINARY(v));
  }
}

void BC5602::dumpRegisters(Print &out) {
  uint8_t saved = bank();

  static const uint8_t commonRegs[] = {0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
                                       0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
  static const char *const commonNames[] = {"CFG1", "RC1",  "MASK", "IRQ1", "STATUS", "IO1",  "IO2", "IO3",
                                            "PKT1", "PKT2", "PKT3", "PKT4", "RSV2",   "RSV3", "RFCH", "DM1",
                                            "RT1",  "RT2",  "CE",   "RSV4", "RSV5",   "RSV6"};

  out.println("--- Zone commune ---");
  for (size_t i = 0; i < sizeof(commonRegs); i++) {
    uint8_t v = readRegister(commonRegs[i] | CMD_READ_REGISTER);
    out.printf("  %-8s (0x%02X) = 0x%02X  0b" BYTE_TO_BINARY_PATTERN "\n", commonNames[i], commonRegs[i], v,
               BYTE_TO_BINARY(v));
  }

  static const uint8_t b0[] = {0x20, 0x21, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x32, 0x37, 0x38};
  static const char *const b0n[] = {"OM",   "CFO1", "STA1", "RSSI1", "RSSI2", "RSSI3",
                                    "DPL1", "DPL2", "RXPW0", "ENAA", "PEN",  "XO1"};
  dumpBank(out, 0, b0, b0n, sizeof(b0));

  static const uint8_t b1[] = {0x20, 0x21, 0x23, 0x25, 0x26, 0x27};
  static const char *const b1n[] = {"SET0", "SET1", "SET2", "RSV1", "RSV2", "RSV3"};
  dumpBank(out, 1, b1, b1n, sizeof(b1));

  static const uint8_t b2[] = {0x28, 0x2D, 0x2E, 0x34, 0x35, 0x36, 0x38, 0x39, 0x3B, 0x3C};
  static const char *const b2n[] = {"SET1", "RSV1", "RSV2", "RFTXP_1", "RFTXP_2",
                                    "SET2", "RSV3", "RSV4", "RSV5",    "RSV6"};
  dumpBank(out, 2, b2, b2n, sizeof(b2));

  setBank(saved);
}
