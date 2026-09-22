#include "swd.h"

namespace swd {

static uint8_t clk_ = 0xFF;
static uint8_t io_ = 0xFF;
static uint16_t half_ = 2;

// --- primitives de ligne ---------------------------------------------------
// La cible echantillonne SWDIO sur le front MONTANT de SWCLK et y place ses
// propres bits sur le front descendant. On ecrit donc pendant que l'horloge est
// basse, et on lit juste avant de la relever.

static inline void ioOutput() { pinMode(io_, OUTPUT); }
static inline void ioInput() { pinMode(io_, INPUT_PULLUP); }

static inline void writeBit(bool bit) {
  digitalWrite(io_, bit ? HIGH : LOW);
  delayMicroseconds(half_);
  digitalWrite(clk_, HIGH);
  delayMicroseconds(half_);
  digitalWrite(clk_, LOW);
}

static inline bool readBit() {
  delayMicroseconds(half_);
  const bool bit = digitalRead(io_) != 0;
  digitalWrite(clk_, HIGH);
  delayMicroseconds(half_);
  digitalWrite(clk_, LOW);
  return bit;
}

// Un cycle de retournement : personne ne pilote la ligne, elle change de sens.
static inline void turnaround() {
  ioInput();
  delayMicroseconds(half_);
  digitalWrite(clk_, HIGH);
  delayMicroseconds(half_);
  digitalWrite(clk_, LOW);
}

static void writeBits(uint32_t value, uint8_t count) {
  ioOutput();
  for (uint8_t i = 0; i < count; i++) writeBit((value >> i) & 1U);
}

static void idleCycles(uint8_t count) {
  ioOutput();
  for (uint8_t i = 0; i < count; i++) writeBit(false);
}

static void lineReset() {
  // Au moins 50 cycles SWDIO haut remettent la cible dans un etat connu.
  ioOutput();
  for (uint8_t i = 0; i < 56; i++) writeBit(true);
}

// --- interface publique ----------------------------------------------------

void begin(uint8_t clkPin, uint8_t ioPin, uint16_t halfPeriodUs) {
  clk_ = clkPin;
  io_ = ioPin;
  half_ = halfPeriodUs ? halfPeriodUs : 1;
  pinMode(clk_, OUTPUT);
  digitalWrite(clk_, LOW);
  ioOutput();
  digitalWrite(io_, HIGH);
}

void connect() {
  lineReset();
  // Sequence de bascule JTAG -> SWD : 0xE79E, poids faible en tete.
  writeBits(0xE79EU, 16);
  lineReset();
  idleCycles(8);
}

uint8_t transfer(bool accessPort, bool read, uint8_t addr, uint32_t *data) {
  const bool a2 = (addr >> 2) & 1U;
  const bool a3 = (addr >> 3) & 1U;
  const bool parity = accessPort ^ read ^ a2 ^ a3;

  uint8_t request = 0;
  request |= 1U << 0;                  // Start
  request |= (accessPort ? 1U : 0) << 1;
  request |= (read ? 1U : 0) << 2;
  request |= (a2 ? 1U : 0) << 3;
  request |= (a3 ? 1U : 0) << 4;
  request |= (parity ? 1U : 0) << 5;
  // bit 6 : Stop, toujours 0
  request |= 1U << 7;                  // Park

  writeBits(request, 8);
  turnaround();

  uint8_t ack = 0;
  for (uint8_t i = 0; i < 3; i++) ack |= (readBit() ? 1U : 0) << i;

  if (ack != ACK_OK) {
    // Sur WAIT ou FAULT la cible ne transmet pas de donnee : un seul cycle de
    // retournement suffit pour reprendre la main.
    turnaround();
    ioOutput();
    idleCycles(8);
    return ack;
  }

  if (read) {
    uint32_t value = 0;
    bool computed = false;
    for (uint8_t i = 0; i < 32; i++) {
      const bool bit = readBit();
      if (bit) {
        value |= 1UL << i;
        computed = !computed;
      }
    }
    const bool parityBit = readBit();
    turnaround();
    ioOutput();
    if (data) *data = value;
    // Une parite fausse signale un echange corrompu, pas un refus de la cible.
    if (parityBit != computed) return 0xFF;
  } else {
    turnaround();
    ioOutput();
    const uint32_t value = data ? *data : 0;
    bool computed = false;
    for (uint8_t i = 0; i < 32; i++) {
      const bool bit = (value >> i) & 1U;
      if (bit) computed = !computed;
      writeBit(bit);
    }
    writeBit(computed);
  }

  idleCycles(8);
  return ack;
}

bool readIdcode(uint32_t *idcode, uint8_t *ack) {
  uint32_t value = 0;
  const uint8_t a = transfer(false, true, DP_IDCODE, &value);
  if (ack) *ack = a;
  if (idcode) *idcode = value;
  // Un IDCODE valide a toujours son bit 0 a 1, et ne vaut ni 0 ni tout-a-un.
  return a == ACK_OK && (value & 1U) && value != 0xFFFFFFFFUL;
}

bool powerUpDebug(uint32_t *ctrlStat) {
  // Effacer d'abord les erreurs collantes, sinon toute requete suivante est
  // refusee sans explication.
  uint32_t abort = 0x1E;
  transfer(false, false, DP_IDCODE, &abort);

  uint32_t select = 0;
  transfer(false, false, DP_SELECT, &select);

  uint32_t req = (1UL << 28) | (1UL << 30);  // CDBGPWRUPREQ | CSYSPWRUPREQ
  transfer(false, false, DP_CTRLSTAT, &req);

  for (uint8_t attempt = 0; attempt < 20; attempt++) {
    uint32_t value = 0;
    if (transfer(false, true, DP_CTRLSTAT, &value) == ACK_OK) {
      if (ctrlStat) *ctrlStat = value;
      // La cible confirme en levant ses deux bits d'acquittement.
      if ((value & (1UL << 29)) && (value & (1UL << 31))) return true;
    }
    delay(1);
  }
  return false;
}

void probeCandidates(Print &out, uint8_t clkPin, const uint8_t *candidates, uint8_t count) {
  char line[160];

  for (uint8_t i = 0; i < count; i++) {
    const uint8_t pin = candidates[i];
    if (pin == clkPin) continue;

    begin(clkPin, pin);
    connect();

    uint32_t idcode = 0;
    uint8_t ack = 0;
    const bool ok = readIdcode(&idcode, &ack);

    const char *ackName = (ack == ACK_OK)      ? "OK"
                          : (ack == ACK_WAIT)  ? "WAIT"
                          : (ack == ACK_FAULT) ? "FAULT"
                          : (ack == 0xFF)      ? "parite"
                                               : "aucun";

    snprintf(line, sizeof(line), "  SWDIO sur IO%-2u : ack %-6s  IDCODE 0x%08lX%s", (unsigned)pin,
             ackName, (unsigned long)idcode, ok ? "   <<< LA CIBLE REPOND" : "");
    out.println(line);
    Serial.flush();

    if (!ok) continue;

    uint32_t ctrlStat = 0;
    if (powerUpDebug(&ctrlStat)) {
      snprintf(line, sizeof(line), "     domaine de debug sous tension, CTRL/STAT 0x%08lX",
               (unsigned long)ctrlStat);
      out.println(line);
      out.println("     La liaison est reellement etablie, pas un hasard de lecture.");
    } else {
      snprintf(line, sizeof(line), "     mise sous tension du debug REFUSEE, CTRL/STAT 0x%08lX",
               (unsigned long)ctrlStat);
      out.println(line);
    }
    Serial.flush();
    return;
  }
}

}  // namespace swd
