#include "cc2500.h"

namespace cc2500 {

const char *marcStateName(uint8_t state) {
  switch (state) {
    case 0x00: return "SLEEP";
    case 0x01: return "IDLE";
    case 0x02: return "XOFF";
    case 0x03: case 0x04: case 0x05: return "VCOON_MC/REGON_MC/MANCAL";
    case 0x06: case 0x07: return "VCOON/REGON";
    case 0x08: return "STARTCAL";
    case 0x09: case 0x0A: case 0x0B: return "BWBOOST/FS_LOCK/IFADCON";
    case 0x0C: return "ENDCAL";
    case 0x0D: return "RX";
    case 0x0E: return "RX_END";
    case 0x0F: return "RX_RST";
    case 0x10: return "TXRX_SWITCH";
    case 0x11: return "RXFIFO_OVERFLOW";
    case 0x12: return "FSTXON";
    case 0x13: return "TX";
    case 0x14: return "TX_END";
    case 0x15: return "RXTX_SWITCH";
    case 0x16: return "TXFIFO_UNDERFLOW";
    default: return "inconnu";
  }
}

// Le datasheet impose d'attendre que SO retombe apres l'abaissement de CSN :
// c'est la puce qui signale ainsi que son regulateur et son quartz sont
// stabilises. Sauter cette attente donne des lectures fantaisistes au reveil.
bool CC2500::waitReady(uint32_t timeoutMs) {
  const uint32_t until = millis() + timeoutMs;
  while (digitalRead(miso_) == HIGH) {
    if ((int32_t)(millis() - until) >= 0) return false;
    delayMicroseconds(50);
  }
  return true;
}

void CC2500::select() {
  digitalWrite(csn_, LOW);
  waitReady();
}

void CC2500::deselect() { digitalWrite(csn_, HIGH); }

void CC2500::strobe(uint8_t cmd) {
  spi_.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  select();
  spi_.transfer(cmd);
  deselect();
  spi_.endTransaction();
}

void CC2500::writeRegister(uint8_t reg, uint8_t value) {
  spi_.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  select();
  spi_.transfer((uint8_t)(HDR_WRITE | reg));
  spi_.transfer(value);
  deselect();
  spi_.endTransaction();
}

uint8_t CC2500::readRegister(uint8_t reg) {
  spi_.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  select();
  spi_.transfer((uint8_t)(HDR_READ | reg));
  const uint8_t v = spi_.transfer(0x00);
  deselect();
  spi_.endTransaction();
  return v;
}

// Les registres 0x30 a 0x3D sont a la fois des commandes strobe et des
// registres d'etat : seul le bit de rafale les distingue. Lire PARTNUM sans
// HDR_BURST declencherait un reset logiciel au lieu d'une lecture.
uint8_t CC2500::readStatus(uint8_t reg) {
  spi_.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  select();
  spi_.transfer((uint8_t)(HDR_READ | HDR_BURST | reg));
  const uint8_t v = spi_.transfer(0x00);
  deselect();
  spi_.endTransaction();
  return v;
}

void CC2500::writeBurst(uint8_t reg, const uint8_t *data, size_t n) {
  spi_.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  select();
  spi_.transfer((uint8_t)(HDR_WRITE | HDR_BURST | reg));
  for (size_t i = 0; i < n; i++) spi_.transfer(data[i]);
  deselect();
  spi_.endTransaction();
}

void CC2500::readBurst(uint8_t reg, uint8_t *data, size_t n) {
  spi_.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  select();
  spi_.transfer((uint8_t)(HDR_READ | HDR_BURST | reg));
  for (size_t i = 0; i < n; i++) data[i] = spi_.transfer(0x00);
  deselect();
  spi_.endTransaction();
}

void CC2500::setFrontEnd(bool paEnable, bool rxEnable) {
  digitalWrite(paEn_, paEnable ? HIGH : LOW);
  digitalWrite(rxEn_, rxEnable ? HIGH : LOW);
}

bool CC2500::reset() {
  // Sequence de reveil manuel du datasheet : CSN bas puis haut brievement,
  // avant le strobe de reset. Sans elle, une puce sortie d'un etat inconnu
  // peut ne pas repondre.
  digitalWrite(csn_, LOW);
  delayMicroseconds(10);
  digitalWrite(csn_, HIGH);
  delayMicroseconds(45);

  spi_.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(csn_, LOW);
  const bool ready = waitReady();
  spi_.transfer(STROBE_SRES);
  // Apres SRES, SO remonte puis retombe quand le reset est termine.
  const bool done = waitReady(50);
  digitalWrite(csn_, HIGH);
  spi_.endTransaction();
  return ready && done;
}

bool CC2500::begin(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t csn, uint8_t paEn,
                   uint8_t rxEn) {
  csn_ = csn;
  miso_ = miso;
  paEn_ = paEn;
  rxEn_ = rxEn;

  pinMode(csn_, OUTPUT);
  digitalWrite(csn_, HIGH);
  pinMode(paEn_, OUTPUT);
  pinMode(rxEn_, OUTPUT);
  // Au repos, etage d'entree au neutre : ni emission, ni amplification.
  setFrontEnd(false, false);

  spi_.begin(sck, miso, mosi, -1);
  delay(10);

  if (!reset()) {
    present_ = false;
    return false;
  }
  delay(5);

  const uint8_t part = partNumber();
  const uint8_t ver = version();
  // 0x80 = CC2500, 0x00 = CC1101. Les deux valeurs extremes trahissent un bus
  // muet (rien branche) ou court-circuite.
  present_ = (part != 0xFF) && !(part == 0x00 && ver == 0x00);
  return present_;
}

}  // namespace cc2500

cc2500::CC2500 radio2;
