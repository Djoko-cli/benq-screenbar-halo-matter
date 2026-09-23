#include "halo1_radio.h"

#include <string.h>

using namespace bc5602;

// Ce que halo1StdConfigure ecrit pour la lampe, relu par la verification.
static constexpr uint8_t kStdRt1 = 0x73;                          // ARD 2000 us, ARC 3
static constexpr uint8_t kHalo1Dm1 = ADDR_LEN_4 | DATARATE_125K;  // 0x82
// Verifications ratees de suite avant de demander la relance du module.
static constexpr uint8_t kMaxVerifyFails = 3;
// Garde d'antenne tenue au plus ce temps apres CE=1 : MAX_RT mesure a 11,4-11,5 ms.
static constexpr uint32_t kGuardHoldUs = 13000;

// ---------------------------------------------------------------------------
//  Sequences prouvees : l'ancien configStdAutoAck de halo.cpp, decoupe.
//
//  Toute la configuration est rejouee apres le reset logiciel, qui efface les
//  reglages analogiques et CFG1 (dont l'AGC, indispensable pour recevoir
//  l'accuse).
// ---------------------------------------------------------------------------

void halo1StdReset(BC5602 &r) { r.command(CMD_SOFTWARE_RESET); }

void halo1StdConfigure(BC5602 &r, const uint8_t addrReg[4], uint8_t ch, uint8_t rate, bool receiver) {
  r.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
  r.registerConfigure(nullptr);
  applyXoTrim(r);
  r.setBank(0);
  r.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, CFG1_AGC_EN);
  r.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, ch);
  r.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, (uint8_t)(ADDR_LEN_4 | rate));
  r.writeCommandData(CMD_WRITE_PTX_ADDRESS, addrReg, 4);
  // Preambule d'un octet, comme la telecommande.
  const uint8_t cfo1 = r.readRegister(B0_CFO1 | CMD_READ_REGISTER);
  r.writeRegister(B0_CFO1 | CMD_WRITE_REGISTER, (uint8_t)(cfo1 & (uint8_t)~0x40));
  uint8_t mask = r.readRegister(REG_MASK | CMD_READ_REGISTER);
  mask = receiver ? (uint8_t)(mask | MASK_PRM_RX) : (uint8_t)(mask & (uint8_t)~MASK_PRM_RX);
  r.writeRegister(REG_MASK | CMD_WRITE_REGISTER, mask);
  r.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, PKT1_CRC_ENABLE);  // CRC16 init FFFF
  // Pas de blanchiment : le CRC des trames reelles se verifie sans.
  r.writeRegister(REG_PKT2 | CMD_WRITE_REGISTER,
                  (uint8_t)(r.readRegister(REG_PKT2 | CMD_READ_REGISTER) & 0x7F));
  r.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x01);   // DPL_P0
  r.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x04);   // EN_DPL seul (ds.txt:853-869)
  r.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x01);   // accuse automatique, pipe 0
  // ARD 2000 us, ARC 3 (ds.txt:667-683). La valeur de reset, 250 us, est plus
  // courte que le silence mesure avant l'accuse de la lampe (~200 us) suivi de
  // l'accuse lui-meme : elle donnerait de faux MAX_RT.
  r.writeRegister(REG_RT1 | CMD_WRITE_REGISTER, kStdRt1);
  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
  r.command(CMD_FLUSH_TX_FIFO);
  r.command(CMD_FLUSH_RX_FIFO);
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
}

// Un recepteur qui accuserait reception entrerait en collision avec l'accuse
// de la lampe. Couper l'accuse coupe aussi, sur cette puce, la charge
// dynamique et donc la lecture materielle du PCF : on lit une longueur fixe.
void halo1PassiveOverrides(BC5602 &r) {
  r.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);   // jamais d'accuse de notre part
  r.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x00);   // charge fixe
  r.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
  r.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);  // CRC verifie en logiciel
  r.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, 8);     // 64 bits apres l'adresse
}

void configStdAutoAck(BC5602 &r, const uint8_t addrReg[4], uint8_t ch, uint8_t rate, bool receiver) {
  halo1StdReset(r);
  delay(40);  // 2 x 20 ms, comme avant le decoupage : softwareReset() puis delay(20)
  halo1StdConfigure(r, addrReg, ch, rate, receiver);
}

// ---------------------------------------------------------------------------
//  Halo1Radio : les memes sequences, sans bloquer sur le reset
// ---------------------------------------------------------------------------

void Halo1Radio::begin(BC5602 &chip, const uint8_t addrReg[4]) {
  chip_ = &chip;
  target_ = Mode::Unknown;
  verifyFails_ = 0;
  restartWanted_ = false;
  spi3Wire_ = false;  // halo.begin() a reecrit IO1
  setAddress(addrReg);  // + invalidate() : puce pas encore configuree par le pilote
}

void Halo1Radio::setAddress(const uint8_t addrReg[4]) {
  memcpy(addrReg_, addrReg, 4);
  halo1::airOrder(addrReg_, air_);
  invalidate();
}

void Halo1Radio::request(Mode m, uint32_t nowMs) {
  if (!present() || restartWanted_) return;
  if (m != Mode::Tx && m != Mode::Rx && m != Mode::Sleep) return;
  if (mode_ == Mode::Resetting) {
    target_ = m;  // la configuration suivra la derniere demande
    return;
  }
  if (mode_ == m) return;
  if (m == Mode::Sleep) {
    chip_->writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
    chip_->command(CMD_LIGHT_SLEEP);
    mode_ = target_ = Mode::Sleep;
    return;
  }
  // Seulement entre deux modes dont on connait la configuration : depuis
  // Sleep ou Unknown, la puce a pu etre laissee dans n'importe quel etat.
  if (tuning.lightSwitch && (mode_ == Mode::Tx || mode_ == Mode::Rx)) {
    if (m == Mode::Tx) {
      lightToTx();
    } else {
      lightToRx();
      // Puce fraichement armee : le delai de silence repart de zero.
      lastFull_ = lastArm_ = nowMs;
    }
    mode_ = target_ = m;
    stats.lightSwitches++;
    return;
  }
  beginReset(m, nowMs, WHY_MODE);
}

void Halo1Radio::service(uint32_t nowMs) {
  if (mode_ != Mode::Resetting || !present()) return;
  // Difference signee : resetAt_ vient de millis() et peut depasser un nowMs
  // pris avant un envoi bloquant.
  if ((int32_t)(nowMs - resetAt_) < (int32_t)tuning.resetWaitMs) return;
  finishReset(nowMs);
}

void Halo1Radio::beginReset(Mode target, uint32_t nowMs, uint8_t why) {
  if (why != WHY_MODE) takeSnapshot(why, nowMs);
  if (why == WHY_SILENCE) stats.silenceReconf++;
  else if (why == WHY_TX) stats.txReconf++;
  halo1StdReset(*chip_);
  spi3Wire_ = true;
  configured_ = false;
  // Horloge reelle, pas nowMs : l'appelant a pu bloquer depuis (envoi, entree
  // en RX), et les 40 ms comptent a partir du reset lui-meme.
  resetAt_ = millis();
  target_ = target;
  mode_ = Mode::Resetting;
}

void Halo1Radio::finishReset(uint32_t nowMs) {
  halo1StdConfigure(*chip_, addrReg_, halo1::kChannel, DATARATE_125K, target_ == Mode::Rx);
  spi3Wire_ = false;  // IO1 reecrit en tete de configuration
  configured_ = true;  // meme si la verification echoue : 'lampe regs' montre l'ecart
  if (!verify()) {
    stats.verifyFail++;
    if (++verifyFails_ >= kMaxVerifyFails) {
      // Niveau L2 : c'est au pilote de relancer le module (halo.begin()).
      takeSnapshot(WHY_VERIFY, nowMs);
      restartWanted_ = true;
      mode_ = Mode::Unknown;
    } else {
      beginReset(target_, nowMs, WHY_VERIFY);
    }
    return;
  }
  verifyFails_ = 0;
  stats.fullConfigs++;
  if (target_ == Mode::Rx) {
    // = 'armer' de sniffStd. lastFrame_ garde sa valeur, comme la-bas.
    halo1PassiveOverrides(*chip_);
    chip_->enterRxMode();
    lastFull_ = lastArm_ = nowMs;
    mode_ = Mode::Rx;
  } else if (target_ == Mode::Sleep) {
    chip_->command(CMD_LIGHT_SLEEP);  // CE deja a 0
    mode_ = Mode::Sleep;
  } else {
    mode_ = Mode::Tx;  // PTX, CE a 0 : pret a emettre
  }
}

bool Halo1Radio::readConfig(uint8_t out[3]) {
  // Pendant les 40 ms du reset, la puce est en SPI 3 fils et GIO2 n'emet rien
  // (bc5602.cpp) : une lecture rendrait n'importe quoi.
  if (!readable()) return false;
  out[0] = chip_->readRegister(REG_RFCH | CMD_READ_REGISTER);
  out[1] = chip_->readRegister(REG_DM1 | CMD_READ_REGISTER);
  out[2] = chip_->readRegister(REG_RT1 | CMD_READ_REGISTER);
  return true;
}

bool Halo1Radio::verify() {
  uint8_t c[3];
  return readConfig(c) && c[0] == halo1::kChannel && c[1] == kHalo1Dm1 && c[2] == kStdRt1;
}

// Reprend txAck ligne a ligne, sans rien afficher.
Halo1Radio::TxReport Halo1Radio::sendOne(const uint8_t *pay, uint8_t len, uint32_t nowMs) {
  TxReport rep{};
  rep.v = Verdict::FifoRefused;
  if (!ready(Mode::Tx) || !present() || !len || len > 32) return rep;
  BC5602 &r = *chip_;
  // Aucun retour entre ici et CE=0 : une garde prise est toujours rendue.
  const Halo1AirGuard *held = guardEnter();
  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
  r.command(CMD_FLUSH_TX_FIFO);
  r.writeCommandData(CMD_WRITE_TX_FIFO_WITH_ACK, pay, len);  // 0x11 : AVEC accuse
  const uint8_t status = r.readRegister(REG_STATUS | CMD_READ_REGISTER);
  uint8_t irq = 0;
  uint32_t us = 0;
  if (!(status & STATUS_TX_FIFO_EMPTY)) {
    const uint32_t t0 = micros();
    r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
    while ((uint32_t)(micros() - t0) < 30000) {
      irq = r.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
      if (irq & (IRQ_TX_DS | IRQ_MAX_RT)) break;
      // MAX_RT tombe avant kGuardHoldUs : au-dela, l'echange est deja perdu et
      // la garde ne protege plus rien. Rendue ici, elle n'est pas tenue 30 ms
      // (Timeout), avec Thread et lwIP suspendus derriere elle.
      if (held && (uint32_t)(micros() - t0) >= kGuardHoldUs) {
        held->leave();
        held = nullptr;
      }
      delayMicroseconds(20);
    }
    // Une preemption entre la derniere lecture et le test d'echeance a pu
    // couvrir TX_DS ou MAX_RT : relu avant de conclure au delai, qui compte
    // pour la relance du module (halo1_watch.h).
    if (!(irq & (IRQ_TX_DS | IRQ_MAX_RT))) irq = r.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
    us = micros() - t0;
    // TX_DS avec RX_DR : une trame avec charge (la telecommande, sans doute)
    // est arrivee dans notre fenetre d'accuse. Jamais observe.
    if (irq & IRQ_TX_DS) rep.v = (irq & IRQ_RX_DR) ? Verdict::AckForeign : Verdict::Ack;
    else if (irq & IRQ_MAX_RT) rep.v = Verdict::MaxRt;
    else rep.v = Verdict::Timeout;
  }
  rep.irq1 = irq;
  rep.us = (uint16_t)(us > 0xFFFF ? 0xFFFF : us);
  rep.rt2 = r.readRegister(REG_RT2 | CMD_READ_REGISTER);
  rep.status = r.readRegister(REG_STATUS | CMD_READ_REGISTER);
  // CE=0 tout de suite, sinon la puce repart seule tant que la FIFO n'est pas vide.
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
  // La puce ne peut plus emettre, meme apres un Timeout : la garde est rendue
  // avant la reconfiguration eventuelle (43 ms) et l'ecart jusqu'au paquet suivant.
  if (held) held->leave();
  if (rep.v == Verdict::AckForeign) {  // avant le vidage des FIFO
    rep.fLen = r.readRegister(REG_PKT4 | CMD_READ_REGISTER);
    if (rep.fLen >= 1 && rep.fLen <= 4) r.readFifo(rep.fPay, rep.fLen, false);
  }
  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
  r.command(CMD_FLUSH_TX_FIFO);
  r.command(CMD_FLUSH_RX_FIFO);
  // Apres un echec, reconfiguration complete : sans elle, les envois suivants
  // echouent en 64 us (STATUS 21, banc du 23/09). Jamais apres un succes : le
  // PID doit avancer d'un paquet au suivant.
  if (rep.v != Verdict::Ack && rep.v != Verdict::AckForeign) beginReset(Mode::Tx, nowMs, WHY_TX);
  return rep;
}

// Refusee (verrou pas obtenu) : on emet quand meme, comme sans garde.
const Halo1AirGuard *Halo1Radio::guardEnter() {
  const Halo1AirGuard *g = guard_;
  if (!g || !tuning.airGuard) return nullptr;
  uint32_t waited = 0;
  if (!g->enter(HALO1_AIR_GUARD_WAIT_US, &waited)) {
    stats.guardRefused++;
    return nullptr;
  }
  stats.guarded++;
  if (waited) {
    stats.guardWaits++;
    if (waited >= HALO1_AIR_GUARD_WAIT_US) stats.guardCapped++;
    if (waited > stats.guardMaxUs) stats.guardMaxUs = waited;
  }
  return g;
}

// Une iteration de la boucle de sniffStd, dans le meme ordre.
bool Halo1Radio::pollRx(uint32_t nowMs, uint8_t raw[8]) {
  if (!ready(Mode::Rx) || !present()) return false;
  BC5602 &r = *chip_;
  bool got = false;
  const uint8_t irq = r.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
  if (irq & IRQ_RX_DR) {
    lastFrame_ = nowMs;
    r.readFifo(raw, 8, false);
    r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
    r.command(CMD_FLUSH_RX_FIFO);
    stats.rxRaw++;
    // Rearmement apres chaque trame : la puce est retombee en Light Sleep,
    // enterRxMode envoie donc reellement le strobe.
    r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
    r.enterRxMode();
    lastArm_ = nowMs;
    got = true;
  }
  if (r.operationMode() != OMST_RX) {
    r.enterRxMode(300);
    lastArm_ = nowMs;
    stats.rearms++;
    stats.rearmsOffRx++;
  } else if ((uint32_t)(nowMs - lastArm_) > tuning.rearmMs) {
    // Rearmement de securite. La puce disant RX, enterRxMode rend la main tout
    // de suite : le rearmement prouve se reduit a CE=0 + reecriture de MASK.
    r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
    if (tuning.strongRearm) {  // NON PROUVE : forcer le strobe en passant par Light Sleep
      r.command(CMD_LIGHT_SLEEP);
      const uint32_t t0 = micros();
      while (r.operationMode() == OMST_RX && (uint32_t)(micros() - t0) < 200) delayMicroseconds(20);
    }
    r.enterRxMode();
    lastArm_ = nowMs;
    stats.rearms++;
  }
  // Reconfiguration complete apres un silence : le remede prouve (06a3185) a
  // une ecoute qui se tait malgre le rearmement.
  if ((uint32_t)(nowMs - lastFrame_) > tuning.silenceMs && (uint32_t)(nowMs - lastFull_) > tuning.silenceMs)
    beginReset(Mode::Rx, nowMs, WHY_SILENCE);
  return got;
}

// Lectures seules, sauf la banque : les registres B0_* sont en banque 0, et un
// reset suit toujours l'instantane.
void Halo1Radio::takeSnapshot(uint8_t why, uint32_t nowMs) {
  BC5602 &r = *chip_;
  // Un silence qui suit un silence reprend sa case : les instantanes d'echec TX
  // ou de verification survivent a l'ecoute qui suit (attente D.5).
  const uint8_t last = (uint8_t)((snapIdx_ + kSnaps - 1) % kSnaps);
  const bool again = why == WHY_SILENCE && snapN_ && snaps_[last].why == WHY_SILENCE;
  Snapshot &s = snaps_[again ? last : snapIdx_];
  if (!again) {
    snapIdx_ = (uint8_t)((snapIdx_ + 1) % kSnaps);
    if (snapN_ < kSnaps) snapN_++;
  }
  s.atMs = nowMs;
  s.why = why;
  s.cfg1 = r.readRegister(REG_CFG1 | CMD_READ_REGISTER);
  if (s.cfg1 & 0x03) r.setBank(0);
  s.sta1 = r.readRegister(B0_STA1 | CMD_READ_REGISTER);
  s.irq1 = r.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
  s.status = r.readRegister(REG_STATUS | CMD_READ_REGISTER);
  s.mask = r.readRegister(REG_MASK | CMD_READ_REGISTER);
  s.ce = r.readRegister(REG_CE | CMD_READ_REGISTER);
  s.rfch = r.readRegister(REG_RFCH | CMD_READ_REGISTER);
  s.dm1 = r.readRegister(REG_DM1 | CMD_READ_REGISTER);
  s.pkt1 = r.readRegister(REG_PKT1 | CMD_READ_REGISTER);
  s.enaa = r.readRegister(B0_ENAA | CMD_READ_REGISTER);
  s.dpl1 = r.readRegister(B0_DPL1 | CMD_READ_REGISTER);
  s.dpl2 = r.readRegister(B0_DPL2 | CMD_READ_REGISTER);
  s.rxpw0 = r.readRegister(B0_RXPW0 | CMD_READ_REGISTER);
  s.rt1 = r.readRegister(REG_RT1 | CMD_READ_REGISTER);
}

uint8_t Halo1Radio::snapshots(Snapshot *out, uint8_t max) const {
  uint8_t n = 0;
  for (; n < snapN_ && n < max; n++) out[n] = snaps_[(snapIdx_ + kSnaps - 1 - n) % kSnaps];
  return n;
}

// ---------------------------------------------------------------------------
//  Bascule legere TX <-> RX, sans reset (NON PROUVEE, desactivee par defaut) :
//  on ne rejoue que les registres qui different entre les deux configurations.
//  Le PID survit-il ? A mesurer au banc avant toute adoption.
// ---------------------------------------------------------------------------

void Halo1Radio::lightToTx() {
  BC5602 &r = *chip_;
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
  r.command(CMD_LIGHT_SLEEP);
  r.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, CFG1_AGC_EN);  // banque 0, AGC
  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
  r.command(CMD_FLUSH_RX_FIFO);
  r.command(CMD_FLUSH_TX_FIFO);
  const uint8_t mask = r.readRegister(REG_MASK | CMD_READ_REGISTER);
  r.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask & (uint8_t)~MASK_PRM_RX));
  r.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, PKT1_CRC_ENABLE);
  r.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x01);
  r.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x04);
  r.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x01);
}

void Halo1Radio::lightToRx() {
  BC5602 &r = *chip_;
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
  r.command(CMD_LIGHT_SLEEP);
  r.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, CFG1_AGC_EN);  // banque 0, AGC
  halo1PassiveOverrides(r);
  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
  r.command(CMD_FLUSH_TX_FIFO);
  r.enterRxMode();
}
