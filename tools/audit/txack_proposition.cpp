// PROPOSITION, NON INTEGREE AU DEPOT. Test d'emission Halo 1 en mode normal
// du BC5602 : PCF 9 bits genere par la puce, charge dynamique, CRC materiel,
// accuse automatique demande, verdict par TX_DS / MAX_RT.
//
// Adresse : A' = 63 FD F0 4F sur l'air (2 bits AVANT la fenetre 8F F7 C1 3C),
// ecrite 4F F0 FD 63 (ordre inverse, comme kCalRegAddr -> kCalAirAddr).
// Charge : 2 octets captures tels quels (ex. C4 FE), la puce ajoute le PCF
// (longueur 2, PID automatique, NO_ACK=0) et le CRC-16 init FFFF.

#include "bc5602.h"
using namespace bc5602;

static const uint8_t kHalo1TrueAddrReg[4] = {0x4F, 0xF0, 0xFD, 0x63};  // air : 63 FD F0 4F
static const uint8_t kHalo1WindowReg[4]   = {0x3C, 0xC1, 0xF7, 0x8F};  // temoin negatif (ancienne)

enum class TxVerdict : uint8_t { Acked, MaxRt, Timeout, FifoRefused };

struct TxResult {
  TxVerdict verdict;
  uint8_t irq1, rt2, status, omst;
  uint32_t us;  // duree jusqu'au verdict
};

// Configuration PTX + auto-ACK. Tout reset logiciel efface ces reglages : on
// les rejoue integralement a chaque appel (lecon de configForLoopback).
static void configPtxAutoAck(BC5602 &r, const uint8_t addrReg[4], uint8_t channel) {
  r.softwareReset();
  delay(20);
  r.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);   // SDO sur GIO2
  r.registerConfigure(nullptr);                                  // valeurs Holtek (effacees par le reset)
  // applyXoTrim(r);  // si un trim a ete choisi
  r.setBank(0);
  r.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, CFG1_AGC_EN);    // AGC indispensable pour recevoir l'ACK
  r.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, channel);        // 5 -> 2405 MHz (ds.txt RFCH)
  r.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, ADDR_LEN_4 | DATARATE_125K);  // 0x82
  r.writeCommandData(CMD_WRITE_PTX_ADDRESS, addrReg, 4);          // PTX = pipe 0 (ds.txt:1134, 1626)

  uint8_t cfo1 = r.readRegister(B0_CFO1 | CMD_READ_REGISTER);     // preambule 1 octet, comme la telecommande
  r.writeRegister(B0_CFO1 | CMD_WRITE_REGISTER, (uint8_t)(cfo1 & ~0x40));

  uint8_t mask = r.readRegister(REG_MASK | CMD_READ_REGISTER);    // PRM_RX = 0 : PTX
  r.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask & ~MASK_PRM_RX));

  r.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, PKT1_CRC_ENABLE); // 0x20 : CRC16 CCITT init FFFF
  r.writeRegister(REG_PKT2 | CMD_WRITE_REGISTER,                  // blanchiment OFF (WHT_EN=0)
                  (uint8_t)(r.readRegister(REG_PKT2 | CMD_READ_REGISTER) & 0x7F));
  r.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x01);            // DPL_P0
  r.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x04);            // EN_DPL seul ; INV_NOACK=0, EN_DYN_ACK=0
  r.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x01);            // ENAAP0 : attendre l'ACK sur le pipe 0
  r.writeRegister(REG_RT1 | CMD_WRITE_REGISTER, 0x73);            // ARD 2000 us, ARC 3 (reset = 0x03 : ARD 250 us, trop court)

  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);  // 0x70
  r.command(CMD_FLUSH_TX_FIFO);
  r.command(CMD_FLUSH_RX_FIFO);
  // CE reste a 0 ici : l'emission part au moment ou on pose CE apres
  // remplissage de la FIFO (ds.txt:711-716, continuous mode ds.txt:1334).
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
}

// Une emission, un verdict. N'emet QUE des charges capturees telles quelles.
static TxResult sendHalo1WithAck(BC5602 &r, const uint8_t *payload, uint8_t len,
                                 uint32_t timeoutUs = 30000) {
  TxResult res{TxVerdict::Timeout, 0, 0, 0, 0, 0};
  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
  r.command(CMD_FLUSH_TX_FIFO);
  r.writeCommandData(CMD_WRITE_TX_FIFO_WITH_ACK, payload, len);    // 0x11, PAS 0x13
  res.status = r.readRegister(REG_STATUS | CMD_READ_REGISTER);
  if (res.status & STATUS_TX_FIFO_EMPTY) {                          // ecriture refusee en silence ?
    res.verdict = TxVerdict::FifoRefused;
    return res;
  }
  const uint32_t t0 = micros();
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);         // depart
  while ((uint32_t)(micros() - t0) < timeoutUs) {
    const uint8_t irq = r.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
    if (irq & (IRQ_TX_DS | IRQ_MAX_RT)) {
      res.irq1 = irq;
      res.verdict = (irq & IRQ_TX_DS) ? TxVerdict::Acked : TxVerdict::MaxRt;
      break;
    }
    delayMicroseconds(20);
  }
  res.us = micros() - t0;
  res.rt2 = r.readRegister(REG_RT2 | CMD_READ_REGISTER);            // CNT_PLOS | CNT_ARC
  res.status = r.readRegister(REG_STATUS | CMD_READ_REGISTER);
  res.omst = r.operationMode();

  // Remise a zero dans tous les cas : CE=0 (sinon la puce repart seule tant
  // que la FIFO n'est pas vide), acquittement des drapeaux en ecrivant 1
  // (ds.txt:470-481), vidage des FIFO (ds.txt:1588-1593), Light Sleep.
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
  r.command(CMD_FLUSH_TX_FIFO);   // apres MAX_RT la charge reste probablement en FIFO (suppose, comme nRF24)
  r.command(CMD_FLUSH_RX_FIFO);   // ACK sans charge : rien a lire, par precaution
  r.command(CMD_LIGHT_SLEEP);
  return res;
}

// Protocole complet : temoin negatif, puis essai reel, espaces.
void BenqHalo::txAckTest(Print &out, uint8_t trials, uint16_t gapMs) {
  static const uint8_t kPayload[2] = {0xC4, 0xFE};  // commande molette capturee 3 fois (async + FIFO)
  char line[160];
  const struct { const char *name; const uint8_t *addr; } legs[2] = {
      {"TEMOIN adresse fenetre 3C C1 F7 8F (attendu : MAX_RT)", kHalo1WindowReg},
      {"ESSAI adresse vraie 4F F0 FD 63 (attendu : TX_DS)", kHalo1TrueAddrReg},
  };
  if (trials > 10) trials = 10;  // garde-fou : pas de rafale sur la lampe
  for (uint8_t l = 0; l < 2; l++) {
    out.println(legs[l].name);
    configPtxAutoAck(radio, legs[l].addr, 5);
    uint8_t acked = 0, maxrt = 0;
    for (uint8_t i = 0; i < trials; i++) {
      const TxResult r = sendHalo1WithAck(radio, kPayload, 2);
      if (r.verdict == TxVerdict::Acked) acked++;
      if (r.verdict == TxVerdict::MaxRt) maxrt++;
      snprintf(line, sizeof(line), "  %u : %s  IRQ1 %02X RT2 %02X STATUS %02X OMST %u  %lu us", i,
               r.verdict == TxVerdict::Acked ? "TX_DS" :
               r.verdict == TxVerdict::MaxRt ? "MAX_RT" :
               r.verdict == TxVerdict::FifoRefused ? "FIFO REFUSEE" : "DELAI",
               r.irq1, r.rt2, r.status, r.omst, (unsigned long)r.us);
      out.println(line);
      delay(gapMs);  // >= 500 ms : laisser la lampe finir sa transition
    }
    snprintf(line, sizeof(line), "  bilan : %u TX_DS, %u MAX_RT sur %u", acked, maxrt, trials);
    out.println(line);
  }
}
