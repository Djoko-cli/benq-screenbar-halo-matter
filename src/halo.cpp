#include "halo.h"

#include <string.h>

#include "driver/spi_slave.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

using namespace bc5602;

BenqHalo halo;

// Les trois canaux declares dans le dossier FCC. Le Halo 2 n'a ete observe que
// sur le premier, mais rien ne garantit que le Halo 1 fasse pareil.
static constexpr uint8_t kSweepChannels[3] = {RF_CHANNEL_1, RF_CHANNEL_2, RF_CHANNEL_3};
// Le debit n'a jamais ete verifie sur le Halo 1 : il vient du Halo 2. Trois
// valeurs seulement, autant les balayer plutot que de parier.
static constexpr uint8_t kSweepRates[3] = {DATARATE_125K, DATARATE_250K, DATARATE_500K};
static constexpr const char *kSweepRateNames[3] = {"125k", "250k", "500k"};
static constexpr uint8_t kSweepCombos = 9;  // 3 debits x 3 canaux
static constexpr uint32_t kSweepDwellMs = 3000;

// ---------------------------------------------------------------------------
//  Cycle de vie
// ---------------------------------------------------------------------------

bool BenqHalo::begin() {
  loadConfig();
  bool ok = radio.begin(PIN_RF_SCK, PIN_RF_MISO, PIN_RF_MOSI, PIN_RF_CSN, RF_SPI_HZ);
  if (ok) {
    // Une seule calibration, apres que RFCH ait ete ecrit pour la premiere fois
    // (prepareToSniff le fait), puis on n'y retouche plus : la rejouer avant une
    // entree en RX empeche la puce d'y entrer.
    prepareToSniff();
    radio.command(CMD_LIGHT_SLEEP);
    radio.calibrate();
    prepareToSniff();
  }
  lastPoll_ = millis();
  settledAt_ = millis();
  return ok;
}

// ---------------------------------------------------------------------------
//  Configuration persistante
// ---------------------------------------------------------------------------

void BenqHalo::loadConfig() {
  // Ouverture en ecriture meme si on ne fait que lire : en lecture seule, un
  // espace de noms inexistant fait loguer "nvs_open failed: NOT_FOUND" au
  // premier demarrage, ce qui ressemble a une panne alors que tout va bien.
  prefs_.begin("benqhalo", false);
  // isKey() d'abord : interroger une cle absente logue une erreur NVS.
  if (!prefs_.isKey("addr") || prefs_.getBytes("addr", addr_, sizeof(addr_)) != sizeof(addr_))
    memset(addr_, 0, sizeof(addr_));
  if (!prefs_.isKey("tail") || prefs_.getBytes("tail", tail_, sizeof(tail_)) != sizeof(tail_)) {
    tail_[0] = 0x01;
    tail_[1] = 0x02;
  }
  channel_ = prefs_.getUChar("chan", RF_CHANNEL_1);
  dataRate_ = prefs_.getUChar("rate", DATARATE_125K);
  prefs_.end();
}

void BenqHalo::saveConfig() {
  prefs_.begin("benqhalo", false);
  prefs_.putBytes("addr", addr_, sizeof(addr_));
  prefs_.putBytes("tail", tail_, sizeof(tail_));
  prefs_.putUChar("chan", channel_);
  prefs_.putUChar("rate", dataRate_);
  prefs_.end();
}

bool BenqHalo::addressConfigured() const {
  return !(addr_[0] == 0 && addr_[1] == 0 && addr_[2] == 0 && addr_[3] == 0);
}

void BenqHalo::setAddress(const uint8_t addr[4]) {
  memcpy(addr_, addr, 4);
  saveConfig();
  if (radio.present() && mode_ == HaloMode::Normal) prepareToSniff();
}

void BenqHalo::setTail(uint8_t a, uint8_t b) {
  tail_[0] = a;
  tail_[1] = b;
  saveConfig();
}

void BenqHalo::setPreambleTwoBytes(bool two) {
  preambleTwoBytes_ = two;
  if (radio.present() && mode_ == HaloMode::Normal) prepareToSniff();
}

void BenqHalo::setChannel(uint8_t ch) {
  channel_ = ch;
  saveConfig();
  if (radio.present() && mode_ == HaloMode::Normal) prepareToSniff();
}

// ---------------------------------------------------------------------------
//  Configuration radio
// ---------------------------------------------------------------------------

void BenqHalo::sharedRadioConfig(uint8_t addrLenBits, const uint8_t *addr, size_t addrLen) {
  radio.command(CMD_LIGHT_SLEEP);
  radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);

  // AGC_EN : reapplique ici, car tout reset logiciel remet CFG1 a 0x00. Mesure
  // a l'appui (commande 'rxdirect') : avec AGC_EN=0 le plus fort signal recu
  // plafonnait a 85 dB, avec AGC_EN=1 il descend a 41 dB. Toutes les chasses a
  // l'adresse anterieures ont donc tourne avec un recepteur sourd d'environ
  // quarante decibels. DIR_EN est efface au passage : il met la puce en Light
  // Sleep et l'empeche de recevoir.
  uint8_t cfg1 = radio.readRegister(REG_CFG1 | CMD_READ_REGISTER);
  cfg1 = (uint8_t)((cfg1 | CFG1_AGC_EN) & (uint8_t)~CFG1_DIR_EN);
  radio.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, cfg1);
  radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, channel_);
  addrLenBits_ = addrLenBits;
  radio.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, (uint8_t)(dataRate_ | addrLenBits));

  // PAS de calibration ici. Mesure a l'appui (commande 'rxseq') : calibrer juste
  // avant d'entrer en reception empeche la puce d'atteindre le mode RX. Sans
  // calibration, 8 sequences d'activation sur 9 tiennent le RX a 99 % ; avec,
  // l'entree echoue. C'est exactement ce que le projet amont avait constate en
  // la qualifiant d'"unstable" et en la desactivant.
  // La calibration a lieu une seule fois, dans begin(), apres le premier
  // reglage de frequence.

  radio.writeCommandData(CMD_WRITE_PTX_ADDRESS, addr, addrLen);

  // CFO1 bit 6 (AMBLE2) : reapplique ici, car tout reset logiciel le remet a 0.
  uint8_t cfo1 = radio.readRegister(B0_CFO1 | CMD_READ_REGISTER);
  cfo1 = preambleTwoBytes_ ? (uint8_t)(cfo1 | 0x40) : (uint8_t)(cfo1 & ~0x40);
  radio.writeRegister(B0_CFO1 | CMD_WRITE_REGISTER, cfo1);
}

void BenqHalo::prepareToTransfer() {
  sharedRadioConfig(ADDR_LEN_4, addr_, 4);

  // PRM_RX = 0 : emetteur primaire.
  uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
  radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, mask & (uint8_t)~MASK_PRM_RX);

  radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0b00000001);  // payload dynamique
  radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0b00000100);
  radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, PKT1_CRC_ENABLE);
  radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, ENAA_ALL_PIPES);
  radio.writeRegister(REG_RT1 | CMD_WRITE_REGISTER, 0x72);  // 2 ms de delai, 2 retransmissions

  // CE = 1 : meme defaut que celui trouve a l'etalonnage. Sans lui la puce
  // reste en Light Sleep, FIFO pleine, et n'emet jamais (ds.txt:711).
  radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x01);

  ackMode_ = true;
}

void BenqHalo::resetRadio() {
  radio.softwareReset();
  radio.registerConfigure();  // le reset remet les registres aux valeurs de POR
  radio.clearInterrupts();
  prepareToSniff();
}

void BenqHalo::prepareToSniff() {
  // Sans auto-ACK, le materiel perd aussi le CRC et la longueur dynamique : le
  // PCF de 9 bits reste dans le flux et decale tout d'un bit (corrige a la
  // lecture de la FIFO). Indispensable : si on gardait l'auto-ACK, notre module
  // acquitterait les trames de la telecommande en meme temps que la lampe.
  sharedRadioConfig(ADDR_LEN_4, sniffOverride_ ? sniffAddr_ : addr_, 4);

  uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
  radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, mask | MASK_PRM_RX);

  radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
  radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x00);
  // 13 = PCF(1) + payload(10) + CRC(2). Le projet Halo 2 comptait 12 en
  // supposant un PCF de 9 bits ; la trame reelle publiee par Termina1
  // (54 04 10 0C 0F 55 5B 0F 55 01 02 20 B9) montre un PCF d'un octet plein
  // suivi immediatement du payload, CRC inclus dans la FIFO.
  radio.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, 13);
  staticRxLen_ = 13;
  radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);
  radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);

  radio.clearInterrupts();
  radio.command(CMD_FLUSH_RX_FIFO);
  radio.enterRxMode();

  ackMode_ = false;
}

// ---------------------------------------------------------------------------
//  Echanges bruts
// ---------------------------------------------------------------------------

bool BenqHalo::sendWithAck(const uint8_t payload[10]) {
  radio.command(CMD_FLUSH_TX_FIFO);
  delay(1);
  radio.writeCommandData(CMD_WRITE_TX_FIFO_WITH_ACK, payload, 10);
  radio.command(CMD_TX_MODE);
  delay(5);

  // La FIFO TX vide signale que la trame est partie et a ete acquittee.
  if (!(radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_TX_FIFO_EMPTY)) {
    radio.command(CMD_TX_MODE);
    delay(5);
  }
  bool sent = radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_TX_FIFO_EMPTY;
  if (debug) {
    Serial.print(sent ? "TX  > " : "TX! > ");
    for (int i = 0; i < 10; i++) Serial.printf("%02X ", payload[i]);
    Serial.println();
  }
  return sent;
}

bool BenqHalo::readAck(uint8_t out[10]) {
  radio.readFifo(out, 10, false);
  if (debug) {
    Serial.print("ACK < ");
    for (int i = 0; i < 10; i++) Serial.printf("%02X ", out[i]);
    Serial.println();
  }
  return true;
}

bool BenqHalo::sniffOnce(uint8_t payload[10], uint8_t *pcfLen, uint8_t *pid, uint8_t *noAck) {
  if (txBusy_) return false;
  if (ackMode_) prepareToSniff();
  if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);

  // Bit RX_DR actif a l'etat bas : 0 = des donnees attendent.
  if (radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR) return false;

  // Compte des que le materiel signale une trame, AVANT tout filtrage : c'est
  // le seul moyen que "0 trame" veuille vraiment dire "rien recu".
  rxEvents_++;

  uint8_t len = radio.readRegister(REG_PKT4 | CMD_READ_REGISTER);
  if (len < 11 || len > 32) len = staticRxLen_;  // PKT4 non fiable en longueur statique

  uint8_t buf[32];
  // PAS de decalage d'un bit : le PCF occupe un octet plein et le payload suit
  // immediatement. Le recalage herite du projet Halo 2 corrompait les donnees.
  radio.readFifo(buf, len, false);
  memcpy(payload, buf + 1, 10);

  uint8_t pcf = buf[0];
  if (pcfLen) *pcfLen = (pcf & 0b11111000) >> 3;
  if (pid) *pid = (pcf & 0b00000110) >> 1;
  if (noAck) *noAck = pcf & 0b00000001;

  radio.clearInterrupts();
  radio.command(CMD_FLUSH_TX_FIFO);
  radio.command(CMD_FLUSH_RX_FIFO);
  radio.enterRxMode();
  return true;
}

// ---------------------------------------------------------------------------
//  Protocole
// ---------------------------------------------------------------------------

void BenqHalo::buildPayload(uint8_t cmd, uint8_t out[10], bool autoMode) const {
  // 0 = avant seule, 1 = arriere seule, 2 = les deux
  uint8_t lamps = 0;
  if (desired.back) lamps = (uint8_t)(1 + (desired.front ? 1 : 0));

  uint8_t control = 0;
  if (desired.power) control |= 0b00000001;
  if (autoMode) control |= 0b00000010;
  control |= (uint8_t)(lamps << 3);
  if (desired.sensor) control |= 0b00100000;

  out[0] = cmd;
  out[1] = control;
  out[2] = constrain((int)desired.frontBrightness, HALO_BRIGHT_MIN, HALO_BRIGHT_MAX);
  out[3] = (uint8_t)(desired.colorTempK >> 8);
  out[4] = (uint8_t)(desired.colorTempK & 0xFF);
  out[5] = constrain((int)desired.backBrightness, HALO_BRIGHT_MIN, HALO_BRIGHT_MAX);
  out[6] = out[3];
  out[7] = out[4];
  out[8] = tail_[0];
  out[9] = tail_[1];
}

uint16_t BenqHalo::frameCrcFor(const uint8_t airAddr[4], uint8_t pcf, const uint8_t payload[10]) {
  auto feed = [](uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (uint8_t i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
  };
  uint16_t crc = 0xEFDF;
  for (uint8_t i = 0; i < 4; i++) crc = feed(crc, airAddr[i]);
  crc = feed(crc, pcf);
  for (uint8_t i = 0; i < 10; i++) crc = feed(crc, payload[i]);
  return crc;
}

uint16_t BenqHalo::frameCrc(uint8_t pcf, const uint8_t payload[10]) const {
  auto feed = [](uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (uint8_t i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
  };
  uint16_t crc = 0xEFDF;
  // addr_ est en ordre d'ecriture registre ; le CRC couvre l'ordre SUR L'AIR,
  // qui en est l'inverse.
  for (uint8_t i = 4; i > 0; i--) crc = feed(crc, addr_[i - 1]);
  crc = feed(crc, pcf);
  for (uint8_t i = 0; i < 10; i++) crc = feed(crc, payload[i]);
  return crc;
}

bool BenqHalo::frameCrcOk(const uint8_t f[13]) const {
  uint16_t expected = (uint16_t)((f[11] << 8) | f[12]);
  return frameCrc(f[0], f + 1) == expected;
}

bool BenqHalo::validate(const uint8_t p[10]) const {
  if (p[0] > HALO_CMD_SLEEP) return false;
  // Seule la borne haute est controlee : rien ne garantit que la lampe ne
  // rapporte pas 0 % quand elle est eteinte.
  if (p[2] > HALO_BRIGHT_MAX || p[5] > HALO_BRIGHT_MAX) return false;

  // 0xFF 0xFF = joker, utile tant que les octets de queue du Halo 1 ne sont pas
  // confirmes (voir la commande CLI "tail").
  if (!(tail_[0] == 0xFF && tail_[1] == 0xFF)) {
    if (p[8] != tail_[0] || p[9] != tail_[1]) return false;
  }

  uint16_t ct = (uint16_t)(p[3] << 8) | p[4];
  if (ct < HALO_CT_MIN_K || ct > HALO_CT_MAX_K) return false;
  ct = (uint16_t)(p[6] << 8) | p[7];
  if (ct < HALO_CT_MIN_K || ct > HALO_CT_MAX_K) return false;
  return true;
}

void BenqHalo::parseStatus(const uint8_t p[10]) {
  reported.power = p[1] & 0b00000001;
  reported.sensor = (p[1] & 0b00100000) >> 5;
  uint8_t lamps = (p[1] & 0b00011000) >> 3;
  reported.front = (lamps == 0) || (lamps == 2);
  reported.back = (lamps == 1) || (lamps == 2);
  reported.frontBrightness = p[2];
  reported.backBrightness = p[5];
  reported.colorTempK = (uint16_t)(p[3] << 8) | p[4];
}

// ---------------------------------------------------------------------------
//  Haut niveau
// ---------------------------------------------------------------------------

void BenqHalo::requestPush(uint8_t cmd, bool autoMode) {
  pendingCmd_ = cmd;
  pendingAuto_ = autoMode;
  // Une nouvelle demande annule l'enchainement eventuellement en attente.
  // requestPushThen() repositionne followUpCmd_ juste apres.
  followUpCmd_ = HALO_CMD_NONE;
  dirtyAt_ = millis();
  phase_ = HaloPhase::Push;
}

void BenqHalo::requestPushThen(uint8_t first, uint8_t second) {
  requestPush(first);
  followUpCmd_ = second;
}

bool BenqHalo::pollNow(uint8_t cmd) {
  if (!radio.present() || !addressConfigured()) return false;

  txBusy_ = true;
  uint8_t pkt[10], ack[10];
  buildPayload(cmd, pkt);
  prepareToTransfer();
  sendWithAck(pkt);
  bool ok = readAck(ack) && validate(ack);
  if (ok) parseStatus(ack);
  checkTxFifo();
  txBusy_ = false;

  // On repasse en ecoute : la lampe n'emet rien d'elle-meme, mais la
  // telecommande physique, si.
  prepareToSniff();
  return ok;
}

bool BenqHalo::settled() const {
  return phase_ == HaloPhase::Idle && (millis() - settledAt_) > HALO_SETTLE_MS;
}

void BenqHalo::checkTxFifo() {
  uint8_t status = radio.readRegister(REG_STATUS | CMD_READ_REGISTER);
  if (!(status & STATUS_TX_FIFO_EMPTY) || (status & STATUS_TX_FIFO_FULL)) {
    radio.softwareReset();
    prepareToTransfer();
    if (debug) Serial.println("BC5602 : reset logiciel (FIFO TX bloquee)");
  }
}

void BenqHalo::adoptReported() {
  // La telecommande physique a peut-etre change l'etat : on aligne la consigne
  // sur le reel, sinon la prochaine commande venue de Matter annulerait ce changement.
  if ((millis() - settledAt_) < HALO_ADOPT_MS) return;
  desired = reported;
}

// ---------------------------------------------------------------------------
//  Boucle principale
// ---------------------------------------------------------------------------

void BenqHalo::tick() {
  if (!radio.present()) return;
  uint32_t now = millis();
  switch (mode_) {
    case HaloMode::Sniffer: tickSniffer(now); break;
    case HaloMode::Finder: tickFinder(now); break;
    default: tickNormal(now); break;
  }
}

void BenqHalo::tickNormal(uint32_t now) {
  if (!addressConfigured()) return;

  switch (phase_) {
    case HaloPhase::Push: {
      // Regroupe les rafales : bouger un curseur dans une app genere des dizaines
      // d'updates, la lampe n'en supporte pas le rythme.
      if (now - dirtyAt_ < HALO_COALESCE_MS) return;

      txBusy_ = true;
      uint8_t pkt[10];
      buildPayload(pendingCmd_ == HALO_CMD_NONE ? HALO_CMD_SET : pendingCmd_, pkt, pendingAuto_);
      prepareToTransfer();
      sendWithAck(pkt);
      memcpy(verifyRef_, pkt + 1, 9);
      pendingAuto_ = false;
      verifyTries_ = 0;
      lastOp_ = now;
      lastPoll_ = now;
      txBusy_ = false;
      phase_ = HaloPhase::Verify;
      return;
    }

    case HaloPhase::Verify: {
      // La lampe applique les changements en fondu : on redemande l'etat
      // jusqu'a convergence plutot que de bloquer sur un delai fixe.
      if (now - lastOp_ < HALO_VERIFY_INTERVAL_MS) return;
      lastOp_ = now;

      txBusy_ = true;
      uint8_t pkt[10], ack[10];
      buildPayload(HALO_CMD_SYNC, pkt);
      prepareToTransfer();
      sendWithAck(pkt);
      bool done = false;
      if (readAck(ack) && validate(ack)) {
        parseStatus(ack);
        done = (memcmp(ack + 1, verifyRef_, 9) == 0);
      }
      checkTxFifo();
      txBusy_ = false;

      if (done || ++verifyTries_ >= HALO_VERIFY_MAX_TRIES) {
        if (debug && !done) Serial.println("Halo : convergence non atteinte, on abandonne");
        settledAt_ = now;
        lastPoll_ = now;
        if (followUpCmd_ != HALO_CMD_NONE) {
          uint8_t next = followUpCmd_;
          followUpCmd_ = HALO_CMD_NONE;
          requestPush(next);
        } else {
          pendingCmd_ = HALO_CMD_NONE;
          phase_ = HaloPhase::Idle;
          prepareToSniff();
        }
      }
      return;
    }

    case HaloPhase::Idle:
    default: {
      if (now - lastPoll_ >= HALO_POLL_INTERVAL_MS) {
        lastPoll_ = now;
        if (pollNow()) adoptReported();
        return;
      }
      // Ecoute passive de la telecommande physique.
      uint8_t pkt[10];
      uint8_t noAck = 0;
      if (sniffOnce(pkt, nullptr, nullptr, &noAck) && validate(pkt)) {
        parseStatus(pkt);
        adoptReported();
        lastPoll_ = now;
        if (debug) {
          Serial.print("RC  < ");
          for (int i = 0; i < 10; i++) Serial.printf("%02X ", pkt[i]);
          Serial.println();
        }
      }
      return;
    }
  }
}

void BenqHalo::tickSniffer(uint32_t now) {
  (void)now;
  uint8_t pkt[10], len = 0, pid = 0, noAck = 0;
  if (!sniffOnce(pkt, &len, &pid, &noAck)) return;
  Serial.printf("[sniff] len=%2u pid=%u noack=%u  ", len, pid, noAck);
  for (int i = 0; i < 10; i++) Serial.printf("%02X ", pkt[i]);
  Serial.printf(" %s\n", validate(pkt) ? "(format plausible)" : "");
}

// ---------------------------------------------------------------------------
//  Recherche d'adresse
//
//  Principe (repris de find_halo2_address.py, generalise) : on regle une
//  pseudo-adresse de 3 octets sur une sequence du PAYLOAD dont on connait la
//  valeur, parce qu'on vient de la regler a la telecommande. Le recepteur se
//  verrouille donc au milieu d'une trame, puis continue a echantillonner : les
//  retransmissions automatiques font apparaitre le DEBUT de la trame suivante,
//  c'est-a-dire le preambule 0xAA suivi de la vraie adresse.
//
//  Le script d'origine lisait l'adresse a un offset fixe. Ici on balaie les 8
//  alignements de bits et toute la fenetre capturee, puis on compte les
//  occurrences : le bon candidat ressort, le bruit non. Necessaire car le
//  Halo 1 n'a aucune raison d'avoir exactement le meme timing inter-trames.
// ---------------------------------------------------------------------------

void BenqHalo::findAddressBegin(const uint8_t sync3[3], uint32_t durationMs, bool sweepChannels) {
  resetRadio();  // sans cela la puce peut refuser d'entrer en RX pour toute la capture
  candCount_ = 0;
  memset(candHits_, 0, sizeof(candHits_));
  finderDeadline_ = millis() + durationMs;
  rxEvents_ = 0;
  sweepChannels_ = sweepChannels;
  sweepIdx_ = 0;
  sweepAt_ = millis() + kSweepDwellMs;

  sharedRadioConfig(ADDR_LEN_3, sync3, 3);

  uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
  radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, mask | MASK_PRM_RX);
  radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
  radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x00);
  // 32 octets : de quoi contenir la fin de la trame accrochee, le preambule et
  // l'adresse de la suivante, PUIS sa trame complete (PCF + payload + CRC).
  // C'est ce qui permet de valider une adresse candidate par son CRC.
  radio.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, 32);
  staticRxLen_ = 32;
  finderConfirmed_ = false;
  radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);
  radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);
  radio.command(CMD_FLUSH_RX_FIFO);
  bool rx = radio.enterRxMode();
  Serial.printf("  entree en reception : %s (OMST=%u)\n", rx ? "OK" : "ECHEC",
                radio.operationMode());

  ackMode_ = false;
  mode_ = HaloMode::Finder;
}

void BenqHalo::noteFinderCandidate(const uint8_t addr[4]) {
  for (uint8_t i = 0; i < candCount_; i++) {
    if (memcmp(candAddr_[i], addr, 4) == 0) {
      candHits_[i]++;
      return;
    }
  }
  if (candCount_ < kMaxCandidates) {
    memcpy(candAddr_[candCount_], addr, 4);
    candHits_[candCount_] = 1;
    candCount_++;
    return;
  }
  // Table pleine : on remplace le candidat le plus faible.
  uint8_t weakest = 0;
  for (uint8_t i = 1; i < candCount_; i++)
    if (candHits_[i] < candHits_[weakest]) weakest = i;
  if (candHits_[weakest] <= 1) {
    memcpy(candAddr_[weakest], addr, 4);
    candHits_[weakest] = 1;
  }
}

void BenqHalo::tickFinder(uint32_t now) {
  if ((int32_t)(now - finderDeadline_) >= 0) {
    printFinderSummary(Serial);
    setMode(HaloMode::Normal);
    return;
  }

  // Le canal du Halo 1 n'est pas confirme : le dossier FCC en mentionne trois.
  if (sweepChannels_ && (int32_t)(now - sweepAt_) >= 0) {
    sweepIdx_ = (uint8_t)((sweepIdx_ + 1) % kSweepCombos);
    uint8_t ch = kSweepChannels[sweepIdx_ % 3];
    uint8_t rateIdx = (uint8_t)(sweepIdx_ / 3);
    // Changer de frequence ou de debit invalide la courbe VCO : repasser en
    // Light Sleep, regler, recalibrer, puis seulement revenir en reception.
    radio.softwareReset();
    radio.registerConfigure();
    radio.clearInterrupts();
    radio.command(CMD_LIGHT_SLEEP);
    radio.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, (uint8_t)(kSweepRates[rateIdx] | addrLenBits_));
    radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, ch);
    radio.command(CMD_FLUSH_RX_FIFO);
    bool rx = radio.enterRxMode();
    sweepAt_ = now + kSweepDwellMs;
    Serial.printf("  find : %s @ %u MHz, RX=%s, %lu trame(s) brute(s)\n", kSweepRateNames[rateIdx],
                  2400 + ch, rx ? "ok" : "ECHEC", (unsigned long)rxEvents_);
  }

  // Rearmement permanent : sans lui la puce retombe en Light Sleep et on
  // n'ecoute qu'une fraction du temps de capture.
  if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);

  if (radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR) return;

  rxEvents_++;  // compte avant tout filtrage, cf. sniffOnce()

  uint8_t len = radio.readRegister(REG_PKT4 | CMD_READ_REGISTER);
  if (len == 0 || len > 32) len = staticRxLen_;

  uint8_t raw[32];
  radio.readFifo(raw, len, false);

  uint8_t buf[32];
  for (uint8_t shift = 0; shift < 8; shift++) {
    memcpy(buf, raw, len);
    for (uint8_t s = 0; s < shift; s++) BC5602::shiftRightOneBit(buf, len);

    for (uint8_t i = 0; i + 5 < len; i++) {
      if (buf[i] != 0xAA) continue;
      if (buf[i + 1] == 0xAA) continue;  // on veut la FIN du preambule
      if (buf[i + 1] == 0x00 && buf[i + 2] == 0x00 && buf[i + 3] == 0x00 && buf[i + 4] == 0x00) continue;
      // Ordre d'ecriture registre = inverse de l'ordre sur l'air.
      uint8_t cand[4] = {buf[i + 4], buf[i + 3], buf[i + 2], buf[i + 1]};

      // Verification decisive : si la trame qui suit l'adresse candidate tient
      // dans la fenetre, son CRC ne peut tomber juste que pour la BONNE adresse.
      if (!finderConfirmed_ && (uint16_t)(i + 18) < len) {
        const uint8_t *air = &buf[i + 1];
        uint8_t pcf = buf[i + 5];
        const uint8_t *pl = &buf[i + 6];
        uint16_t got = (uint16_t)((buf[i + 16] << 8) | buf[i + 17]);
        if (frameCrcFor(air, pcf, pl) == got) {
          finderConfirmed_ = true;
          memcpy(finderConfirmedAddr_, cand, 4);
          Serial.println();
          Serial.printf("  *** ADRESSE CONFIRMEE PAR CRC : %02X %02X %02X %02X\n", cand[0], cand[1],
                        cand[2], cand[3]);
          Serial.printf("  *** applique-la avec :  addr %02X%02X%02X%02X\n", cand[0], cand[1], cand[2],
                        cand[3]);
          Serial.print("  *** trame :");
          for (uint8_t k = 5; k < 18; k++) Serial.printf(" %02X", buf[i + k]);
          Serial.println();
          Serial.flush();
        }
      }

      noteFinderCandidate(cand);
    }
  }

  radio.clearInterrupts();
  radio.command(CMD_FLUSH_RX_FIFO);
  radio.enterRxMode();
}

static const char *omstName(uint8_t m) {
  switch (m) {
    case 0: return "Deep Sleep";
    case 1: return "Middle Sleep";
    case OMST_LIGHT_SLEEP: return "Light Sleep";
    case OMST_STANDBY: return "Standby";
    case OMST_TX: return "TX";
    case OMST_RX: return "RX";
    case OMST_CALIB: return "Calibration";
    default: return "?";
  }
}

// Echantillonne OMST pendant `durationUs` et affiche la suite des modes
// traverses, sans les repetitions.
void BenqHalo::diagnoseRx(Print &out) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  auto trace = [&](const char *label, uint8_t strobe) {
    uint8_t seen[12];
    uint32_t firstAt[12];
    uint8_t n = 0;
    uint32_t t0 = micros();

    radio.command(strobe);
    while (micros() - t0 < 20000) {  // 20 ms
      uint8_t m = radio.operationMode();
      if (n == 0 || seen[n - 1] != m) {
        if (n < 12) {
          seen[n] = m;
          firstAt[n] = micros() - t0;
          n++;
        }
      }
      delayMicroseconds(40);
    }

    out.printf("  %s : ", label);
    for (uint8_t i = 0; i < n; i++) {
      out.printf("%s(%lu us)", omstName(seen[i]), (unsigned long)firstAt[i]);
      if (i + 1 < n) out.print(" -> ");
    }
    if (n == 0) out.print("aucun echantillon");
    out.println();
  };

  out.println();
  out.println("=== Diagnostic d'entree en reception ===");

  prepareToSniff();

  uint8_t rc1 = radio.readRegister(REG_RC1 | CMD_READ_REGISTER);
  uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
  uint8_t ce = radio.readRegister(REG_CE | CMD_READ_REGISTER);
  uint8_t sta1 = radio.readRegister(B0_STA1 | CMD_READ_REGISTER);
  uint8_t cfg1 = radio.readRegister(REG_CFG1 | CMD_READ_REGISTER);
  out.printf("  RC1=0x%02X  MASK=0x%02X (PRM_RX=%d)  CE=0x%02X  STA1=0x%02X  CFG1=0x%02X (banque %d)\n",
             rc1, mask, mask & MASK_PRM_RX ? 1 : 0, ce, sta1, cfg1, cfg1 & 0x03);
  out.printf("  mode au repos : %s\n", omstName(radio.operationMode()));

  // 1. CE seul, sans strobe : condition 1 du datasheet (PRM_RX=1 et CE=1).
  radio.command(CMD_LIGHT_SLEEP);
  radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
  delayMicroseconds(200);
  out.printf("  apres CE=1 seul : %s\n", omstName(radio.operationMode()));

  // 2. Strobe RX.
  radio.command(CMD_LIGHT_SLEEP);
  trace("strobe RX ", CMD_RX_MODE);

  // 3. Strobe TX pour comparaison : isole "la puce ne change pas de mode" de
  //    "elle n'entre pas en RX en particulier".
  radio.command(CMD_LIGHT_SLEEP);
  radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask & ~MASK_PRM_RX));  // PTX
  trace("strobe TX ", CMD_TX_MODE);

  // 4. Commande 0x0D. PRM_RX doit etre RESTAURE : le test TX precedent l'avait
  //    mis a 0, ce qui faussait l'interpretation de ce dernier essai.
  radio.command(CMD_LIGHT_SLEEP);
  radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask | MASK_PRM_RX));
  radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
  trace("cmd 0x0D  ", CMD_STANDBY);

  prepareToSniff();  // remet un etat connu
  out.println("  (TX a vide peut rester en Light Sleep : la FIFO TX est vide)");
}

// Releve le RSSI le plus fort de chaque canal (valeur la PLUS PETITE, l'unite
// etant le -dB). Laisse la radio en reception.
void BenqHalo::sweepRssi(uint8_t *out84, uint8_t passes) {
  memset(out84, 0xFF, 84);
  for (uint8_t p = 0; p < passes; p++) {
    for (uint8_t ch = 0; ch < 84; ch++) {
      radio.command(CMD_LIGHT_SLEEP);
      radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, ch);
      radio.enterRxMode();
      uint8_t v = radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER);
      if (v < out84[ch]) out84[ch] = v;
    }
  }
}

void BenqHalo::probeGioFunctions(Print &out) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  out.println();
  out.println("=== Sondage des fonctions cachees de GIO2 ===");
  out.println("  GIO2S : 001 = SDO documente. On cherche une valeur qui sorte");
  out.println("  des bits demodules -- elle se trahira par un fort taux de");
  out.println("  transitions sur la broche pendant la reception.");
  Serial.flush();

  const uint8_t base = IO1_4WIRE_SPI & 0b11000111;  // conserve PADDS et GIO1S

  for (uint8_t sel = 0; sel < 8; sel++) {
    // Repartir d'une reception propre avec le selecteur documente.
    resetRadio();
    radio.enterRxMode();

    // Ecriture a l'aveugle : elle passe par SDIO, MISO n'est pas requis.
    radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, (uint8_t)(base | (sel << 3)));

    // MISO est de toute facon une entree cote ESP32 : on peut l'echantillonner
    // sans perturber le peripherique SPI.
    pinMode(PIN_RF_MISO, INPUT);
    uint32_t transitions = 0, samples = 0, high = 0;
    int last = digitalRead(PIN_RF_MISO);
    uint32_t t0 = micros();
    while (micros() - t0 < 20000) {  // 20 ms
      int v = digitalRead(PIN_RF_MISO);
      if (v != last) {
        transitions++;
        last = v;
      }
      if (v) high++;
      samples++;
    }

    // Restaurer SDO pour retrouver la lecture SPI.
    radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
    delayMicroseconds(200);

    uint32_t pct = samples ? (high * 100UL / samples) : 0;
    out.printf("  GIO2S=%u (0b%u%u%u) : %lu transitions / %lu ech., %lu%% a l'etat haut%s\n", sel,
               (sel >> 2) & 1, (sel >> 1) & 1, sel & 1, (unsigned long)transitions,
               (unsigned long)samples, (unsigned long)pct,
               sel == 1 ? "   <- SDO documente" : (transitions > 100 ? "   <<< ACTIVITE" : ""));
    Serial.flush();
  }

  resetRadio();
  out.println("  Une valeur avec beaucoup de transitions est candidate a une");
  out.println("  sortie de donnees demodulees. Refais le test en emettant avec");
  out.println("  la telecommande : si le taux change, c'est elle.");
}

void BenqHalo::probeRxSequences(Print &out) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  struct Seq {
    const char *name;
    void (*apply)(BC5602 &r);
  };

  static const Seq kSeqs[] = {
      {"LS,CE,0x0D",
       [](BC5602 &r) {
         r.command(CMD_LIGHT_SLEEP);
         r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
         r.command(CMD_STANDBY);
       }},
      {"LS,CE,0x8E",
       [](BC5602 &r) {
         r.command(CMD_LIGHT_SLEEP);
         r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
         r.command(CMD_RX_MODE);
       }},
      {"LS,IRQ,CE,0x8E",
       [](BC5602 &r) {
         r.command(CMD_LIGHT_SLEEP);
         r.clearInterrupts();
         r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
         r.command(CMD_RX_MODE);
       }},
      {"LS,IRQ,CE,0x0D",
       [](BC5602 &r) {
         r.command(CMD_LIGHT_SLEEP);
         r.clearInterrupts();
         r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
         r.command(CMD_STANDBY);
       }},
      {"CE seul",
       [](BC5602 &r) { r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE); }},
      {"0x8E seul", [](BC5602 &r) { r.command(CMD_RX_MODE); }},
      {"0x0D seul", [](BC5602 &r) { r.command(CMD_STANDBY); }},
      {"LS,0x8E,CE",
       [](BC5602 &r) {
         r.command(CMD_LIGHT_SLEEP);
         r.command(CMD_RX_MODE);
         r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
       }},
      {"flushRX,IRQ,CE,0x8E",
       [](BC5602 &r) {
         r.command(CMD_FLUSH_RX_FIFO);
         r.clearInterrupts();
         r.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
         r.command(CMD_RX_MODE);
       }},
  };

  out.println();
  out.println("=== Sondage des sequences d'entree en RX ===");
  out.println("  atteint = OMST a valu 5 au moins une fois");
  out.println("  tenue   = pourcentage du temps passe en RX sur 200 ms");
  Serial.flush();

  for (const Seq &sq : kSeqs) {
    // Repartir d'un etat connu entre chaque essai.
    radio.softwareReset();
    prepareToSniff();
    radio.command(CMD_LIGHT_SLEEP);
    delay(2);

    sq.apply(radio);

    uint16_t inRx = 0, total = 0;
    bool reached = false;
    uint32_t t0 = millis();
    while (millis() - t0 < 200) {
      if (radio.operationMode() == OMST_RX) {
        inRx++;
        reached = true;
      }
      total++;
    }
    out.printf("  %-20s atteint=%s  tenue=%u%%\n", sq.name, reached ? "oui" : "NON",
               total ? (unsigned)(inRx * 100UL / total) : 0);
    Serial.flush();
  }

  radio.softwareReset();
  prepareToSniff();
  out.println("  (la bonne sequence est celle qui tient un pourcentage eleve)");
}

void BenqHalo::watchChannel(Print &out, uint8_t ch, uint32_t durationMs) {
  // On se cale UNE fois, puis on ne touche plus a rien : chaque recalibration
  // ou reentree en RX serait du temps d'ecoute perdu.
  resetRadio();
  radio.command(CMD_LIGHT_SLEEP);
  radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, ch);
  if (!radio.enterRxMode()) {
    out.printf("  %u MHz : impossible d'entrer en reception\n", 2400 + ch);
    return;
  }

  // Histogramme grossier du RSSI, en -dB. Index = valeur, borne a 127.
  static uint16_t hist[128];
  static uint8_t exitIrq[64], exitStatus[64], exitCe[64];
  memset(hist, 0, sizeof(hist));
  memset(exitIrq, 0, sizeof(exitIrq));
  memset(exitStatus, 0, sizeof(exitStatus));
  memset(exitCe, 0, sizeof(exitCe));
  uint32_t samples = 0, rearms = 0;
  uint8_t dumped = 0;
  constexpr uint8_t kMaxDump = 16;  // de quoi voir un motif sans noyer la console
  uint8_t strongest = 0xFF;
  uint32_t t0 = millis();

  while (millis() - t0 < durationMs) {
    // La puce quitte le RX d'elle-meme des qu'un evenement RX survient (time
    // out compris) et le RSSI gele alors. Sans ce rearmement on n'ecoute que
    // quelques millisecondes au total.
    if (radio.operationMode() != OMST_RX) {
      // Si une trame est entree dans la FIFO, la LIRE au lieu de la jeter.
      // C'est tout l'interet : ces sorties de RX sont des receptions reelles.
      uint8_t irqNow = radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
      if ((irqNow & IRQ_RX_DR) && dumped < kMaxDump) {
        uint8_t len = radio.readRegister(REG_PKT4 | CMD_READ_REGISTER);
        if (len == 0 || len > 32) len = staticRxLen_;
        uint8_t buf[32];
        radio.readFifo(buf, len, false);
        out.printf("    trame %2u (len=%2u) :", dumped + 1, len);
        for (uint8_t i = 0; i < len && i < 20; i++) out.printf(" %02X", buf[i]);
        if (len >= 13 && frameCrcOk(buf)) out.print("   <<< CRC VALIDE : trame BenQ authentique");
        out.println();
        Serial.flush();
        dumped++;
      }
      // Releve POURQUOI on est sorti du RX, avant de rearmer : quel drapeau
      // d'interruption est pose, et CE est-il reste a 1 ?
      if (rearms < 64) {
        exitIrq[rearms] = radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
        exitStatus[rearms] = radio.readRegister(REG_STATUS | CMD_READ_REGISTER);
        exitCe[rearms] = radio.readRegister(REG_CE | CMD_READ_REGISTER);
      }
      radio.enterRxMode(300);
      rearms++;
    }
    uint8_t v = radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER);
    if (v < 128) hist[v]++;
    if (v && v < strongest) strongest = v;
    samples++;
  }

  // Plancher de bruit = valeur la plus frequente.
  uint8_t floorVal = 0;
  uint16_t best = 0;
  for (uint8_t i = 1; i < 128; i++) {
    if (hist[i] > best) {
      best = hist[i];
      floorVal = i;
    }
  }

  // Compte les echantillons nettement plus forts que le plancher.
  uint32_t spikes6 = 0, spikes12 = 0;
  for (uint8_t i = 1; i < 128; i++) {
    if (floorVal >= i + 12) spikes12 += hist[i];
    else if (floorVal >= i + 6) spikes6 += hist[i];
  }

  if (rearms) {
    uint8_t n = rearms < 64 ? (uint8_t)rearms : 64;
    // Valeurs les plus frequentes a la sortie du RX.
    uint8_t irq = exitIrq[0], st = exitStatus[0], ce = exitCe[0];
    out.printf("  %u MHz : a la sortie du RX -> IRQ1=0x%02X (RX_DR=%d TX_DS=%d MAX_RT=%d) STATUS=0x%02X CE=0x%02X",
               2400 + ch, irq, (irq & 0x40) ? 1 : 0, (irq & 0x20) ? 1 : 0, (irq & 0x10) ? 1 : 0, st, ce);
    bool varies = false;
    for (uint8_t i = 1; i < n; i++)
      if (exitIrq[i] != irq || exitCe[i] != ce) varies = true;
    out.printf("%s\n", varies ? "  (variable)" : "  (constant)");
    Serial.flush();
  }
  if (dumped) {
    out.printf("  %u MHz : %u trame(s) brute(s) affichee(s) ci-dessus\n", 2400 + ch, dumped);
    Serial.flush();
  }
  out.printf("  %u MHz : %lu ech., %lu rearmements, plancher %u dB, plus fort %u dB, pics +6dB=%lu +12dB=%lu\n",
             2400 + ch, (unsigned long)samples, (unsigned long)rearms, floorVal, strongest,
             (unsigned long)spikes6, (unsigned long)spikes12);
  Serial.flush();
}

void BenqHalo::huntRemote(Print &out) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  static uint8_t idle[84], active[84];

  out.println();
  out.println("=== Chasse a la telecommande ===");
  out.println("  1/2 : releve de reference, NE TOUCHE A RIEN pendant 15 s...");
  Serial.flush();
  prepareToSniff();
  sweepRssi(idle, 6);

  out.println("  2/2 : ACTIONNE LA TELECOMMANDE SANS ARRET pendant 15 s !");
  out.println("        tourne la molette en continu, maintenant.");
  Serial.flush();
  delay(2000);  // le temps d'attraper la telecommande
  sweepRssi(active, 6);

  out.println();
  out.println("  canaux ou le signal a MONTE pendant la manipulation :");
  uint8_t found = 0;
  for (uint8_t ch = 0; ch < 84; ch++) {
    // idle et active sont des -dB : plus petit = plus fort.
    if (idle[ch] == 0xFF || active[ch] == 0xFF) continue;
    int gain = (int)idle[ch] - (int)active[ch];
    if (gain >= 3) {
      out.printf("    %4u MHz : %u -> %u dB  (+%d)\n", 2400 + ch, idle[ch], active[ch], gain);
      Serial.flush();
      found++;
    }
  }
  if (!found) {
    out.println("    aucun. Soit la telecommande n'emettait pas, soit elle est trop");
    out.println("    faible a cette distance : rapproche-la du module et recommence.");
  }
  prepareToSniff();
}

void BenqHalo::scanSpectrum(Print &out, uint8_t passes) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  constexpr uint8_t kChannels = 84;  // 2400 a 2483 MHz
  uint8_t strongest[kChannels];      // RSSI_NEGDB : unite -dB, donc PLUS PETIT = PLUS FORT
  memset(strongest, 0xFF, sizeof(strongest));

  prepareToSniff();  // place la radio en reception
  uint16_t rxReached = 0;

  for (uint8_t p = 0; p < passes; p++) {
    for (uint8_t ch = 0; ch < kChannels; ch++) {
      radio.command(CMD_LIGHT_SLEEP);
      radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, ch);
      if (radio.enterRxMode()) rxReached++;
      uint8_t v = radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER);
      if (v < strongest[ch]) strongest[ch] = v;
    }
  }

  uint8_t globalMin = 0xFF, globalMax = 0;
  for (uint8_t ch = 0; ch < kChannels; ch++) {
    if (strongest[ch] < globalMin) globalMin = strongest[ch];
    if (strongest[ch] > globalMax) globalMax = strongest[ch];
  }

  out.println();
  uint8_t omst = radio.readRegister(B0_STA1 | CMD_READ_REGISTER) & 0b00000111;
  const char *modeName = omst == 2   ? "Light Sleep"
                         : omst == 3 ? "Standby"
                         : omst == 4 ? "TX"
                         : omst == 5 ? "RX"
                         : omst == 6 ? "Calibration"
                                     : "?";
  out.printf("  mode reel de la puce (OMST) : %u = %s\n", omst, modeName);
  out.printf("  entrees en RX confirmees : %u sur %u mesures\n", rxReached,
             (unsigned)(passes * kChannels));
  out.printf("  balayage 2400-2483 MHz, %u passes, valeur = RSSI_NEGDB (unite -dB)\n", passes);
  out.printf("  plus petit = plus fort. min=%u max=%u\n", globalMin, globalMax);
  out.println();

  if (globalMin == globalMax) {
    out.println("  !! Le RSSI est IDENTIQUE sur les 84 canaux.");
    out.println("  !! L'etage de reception ne mesure rien : panne materielle ou");
    out.println("  !! chaine RF non alimentee. Inutile de chercher plus loin cote protocole.");
    prepareToSniff();
    return;
  }

  // Echelle relative entre le plus fort et le plus faible releve.
  uint8_t span = (uint8_t)(globalMax - globalMin);
  for (uint8_t ch = 0; ch < kChannels; ch++) {
    uint8_t bars = span ? (uint8_t)(((globalMax - strongest[ch]) * 20) / span) : 0;
    out.printf("  %4u MHz  %3u  ", 2400 + ch, strongest[ch]);
    for (uint8_t b = 0; b < bars; b++) out.print('#');
    // Reperes : centres Wi-Fi 1 / 6 / 11
    if (ch == 12 || ch == 37 || ch == 62) out.print("   <- Wi-Fi");
    if (ch == RF_CHANNEL_1 || ch == RF_CHANNEL_2 || ch == RF_CHANNEL_3) out.print("   <- BenQ");
    out.println();
  }

  prepareToSniff();  // remet la radio dans un etat connu
}

void BenqHalo::printFinderSummary(Print &out) {
  out.println();
  out.println("=== Recherche d'adresse : resultats ===");
  if (finderConfirmed_) {
    out.printf("ADRESSE CONFIRMEE PAR CRC : %02X %02X %02X %02X  ->  addr %02X%02X%02X%02X\n",
               finderConfirmedAddr_[0], finderConfirmedAddr_[1], finderConfirmedAddr_[2],
               finderConfirmedAddr_[3], finderConfirmedAddr_[0], finderConfirmedAddr_[1],
               finderConfirmedAddr_[2], finderConfirmedAddr_[3]);
  }
  out.printf("Trames brutes recues : %lu\n", (unsigned long)rxEvents_);
  if (candCount_ == 0) {
    if (rxEvents_ == 0) {
      out.println("Aucune trame recue du tout : le recepteur ne s'est jamais accroche.");
      out.println("Le mot de synchro, le canal ou le debit sont faux -- ou l'etage RF");
      out.println("n'entend rien. Teste 'pair' : l'adresse d'appairage E2 08 00 B0 est");
      out.println("fixe et connue, donc une capture qui reste vide la-bas innocente");
      out.println("definitivement le mot de synchro et accuse le materiel ou le canal.");
    } else {
      out.println("Des trames sont bien arrivees, mais aucune n'expose de preambule 0xAA");
      out.println("suivi d'une adresse plausible. Le recepteur entend donc quelque chose :");
      out.println("c'est la fenetre de capture ou l'alignement qu'il faut ajuster.");
    }
    return;
  }
  for (uint8_t pass = 0; pass < candCount_; pass++) {
    int best = -1;
    for (uint8_t i = 0; i < candCount_; i++) {
      if (candHits_[i] == 0xFFFF) continue;
      if (best < 0 || candHits_[i] > candHits_[best]) best = i;
    }
    if (best < 0) break;
    out.printf("  %2u occurrence(s)  adresse = %02X %02X %02X %02X   ->  addr %02X%02X%02X%02X\n",
               candHits_[best], candAddr_[best][0], candAddr_[best][1], candAddr_[best][2],
               candAddr_[best][3], candAddr_[best][0], candAddr_[best][1], candAddr_[best][2],
               candAddr_[best][3]);
    candHits_[best] = 0xFFFF;  // marque comme deja affiche
  }
  out.println("Le bon candidat est celui qui revient le plus souvent.");
  out.println("Applique-le avec 'addr <hex8>', puis verifie avec 'poll'.");
  candCount_ = 0;
}

// ---------------------------------------------------------------------------
//  Modes et affichage
// ---------------------------------------------------------------------------

void BenqHalo::startSniffer(const uint8_t addr[4]) {
  if (addr) {
    memcpy(sniffAddr_, addr, 4);
    sniffOverride_ = true;
  } else {
    sniffOverride_ = false;
  }
  mode_ = HaloMode::Sniffer;
  phase_ = HaloPhase::Idle;
  rxEvents_ = 0;
  sweepChannels_ = false;
  if (radio.present()) prepareToSniff();
}

void BenqHalo::setMode(HaloMode mode) {
  if (mode != HaloMode::Sniffer) sniffOverride_ = false;
  mode_ = mode;
  phase_ = HaloPhase::Idle;
  followUpCmd_ = HALO_CMD_NONE;
  pendingCmd_ = HALO_CMD_NONE;
  settledAt_ = millis();
  lastPoll_ = millis();
  if (!radio.present()) return;
  if (mode == HaloMode::Normal || mode == HaloMode::Sniffer) prepareToSniff();
}

void BenqHalo::printState(Print &out) const {
  auto dump = [&out](const char *label, const HaloState &s) {
    out.printf("%s general=%s  avant=%s %3u%%  arriere=%s %3u%%  %4u K  capteur=%s\n", label,
               s.power ? "ON " : "OFF", s.front ? "ON " : "OFF", s.frontBrightness,
               s.back ? "ON " : "OFF", s.backBrightness, s.colorTempK, s.sensor ? "ON" : "OFF");
  };
  dump("  lampe   :", reported);
  dump("  consigne:", desired);
}

void BenqHalo::printInfo(Print &out) {
  out.println();
  out.println("=== BenQ ScreenBar Halo -> Matter ===");
  out.printf("  firmware      : %s\n", FW_VERSION);
  // La version brute est affichee meme quand le module est absent : c'est elle
  // qui distingue un probleme de MISO d'un probleme d'alimentation.
  uint32_t version = radio.chipVersion();
  out.printf("  BM5602        : %s (version puce 0x%06lX)\n", radio.present() ? "detecte" : "ABSENT",
             (unsigned long)version);
  if (!radio.present()) {
    if (version == 0x00FFFFFFUL)
      out.println("                  0xFFFFFF -> MISO muet : GIO2 non relie (ou relie ailleurs).");
    else if (version == 0x000000UL)
      out.println("                  0x000000 -> pas d'alim, CSN non relie, ou SCK/MOSI inverses.");
    out.println("                  Corrige le cablage puis tape 'rfinit' (inutile de reflasher).");
  }
  out.printf("  broches SPI   : SCK=%d MISO=%d MOSI=%d CSN=%d @ %lu Hz\n", PIN_RF_SCK, PIN_RF_MISO,
             PIN_RF_MOSI, PIN_RF_CSN, (unsigned long)RF_SPI_HZ);
  out.printf("  canal         : %u (%u MHz)\n", channel_, 2400 + channel_);
  out.printf("  adresse       : %02X %02X %02X %02X %s\n", addr_[0], addr_[1], addr_[2], addr_[3],
             addressConfigured() ? "" : "  <-- NON CONFIGUREE, lance 'find'");
  out.printf("  sur l'air     : %02X %02X %02X %02X\n", addr_[3], addr_[2], addr_[1], addr_[0]);
  out.printf("  octets queue  : %02X %02X%s\n", tail_[0], tail_[1],
             (tail_[0] == 0xFF && tail_[1] == 0xFF) ? "  (joker : controle desactive)" : "");
  out.printf("  mode          : %s\n", mode_ == HaloMode::Normal    ? "normal"
                                       : mode_ == HaloMode::Sniffer ? "sniffer"
                                                                    : "recherche d'adresse");
  out.printf("  debug         : %s\n", debug ? "on" : "off");
  out.printf("  trames brutes : %lu (depuis le debut du mode courant)\n", (unsigned long)rxEvents_);
  if (radio.present()) {
    uint8_t rc1 = radio.readRegister(REG_RC1 | CMD_READ_REGISTER);
    out.printf("  registres RF  : valeurs Holtek chargees, %u ecart(s) de relecture\n",
               radio.regCfgMismatches());
    out.printf("  calibration   : %s (OM apres calib = 0x%02X, quartz pret au demarrage : %s)\n",
               radio.calibrated() ? "VCO calibre" : "NON CALIBRE", radio.lastCalibOM(),
               radio.crystalReady() ? "oui" : "NON");
    out.printf("  RC1           : 0x%02X  PWRON=%d XCLK_RDY=%d XCLK_EN=%d FSYCK_RDY=%d\n", rc1,
               (rc1 & RC1_PWRON) ? 1 : 0, (rc1 & RC1_XCLK_RDY) ? 1 : 0, (rc1 & RC1_XCLK_EN) ? 1 : 0,
               (rc1 & RC1_FSYCK_RDY) ? 1 : 0);
  }
  printState(out);
}

// ---------------------------------------------------------------------------
//  Mode direct : lire les bits sans adresse
// ---------------------------------------------------------------------------

// Distingue une broche pilotee d'une broche flottante. On la tire vers le bas
// puis vers le haut : une broche laissee libre suit docilement les resistances
// internes, une broche que la puce pilote les ignore largement.
// Releve le niveau moyen d'une broche sous tirage bas puis haut. Ne sert plus
// qu'a documenter l'etat statique : le BC5602 a ses propres pull-ups (SPIPU et
// GIOPU valent 1 au reset), qui ecrasent ceux de l'ESP32. Le niveau ne dit donc
// rien du pilotage, seules les transitions comptent.
static void pinLevels(uint8_t pin, uint8_t &pctPulldown, uint8_t &pctPullup) {
  auto highPercent = [](uint8_t p, uint8_t mode) -> uint8_t {
    pinMode(p, mode);
    delayMicroseconds(500);
    uint32_t high = 0;
    for (uint32_t i = 0; i < 2000; i++)
      if (digitalRead(p)) high++;
    return (uint8_t)((high * 100UL) / 2000UL);
  };
  pctPulldown = highPercent(pin, INPUT_PULLDOWN);
  pctPullup = highPercent(pin, INPUT_PULLUP);
}

// Echantillonne les trois broches SIMULTANEMENT : une seule lecture du registre
// GPIO_IN les capture toutes d'un coup. Indispensable ici, parce que la
// telecommande emet par impulsions : une fenetre par broche obligerait a
// declencher une emission pendant chacune d'elles.
static void countTransitions3(const uint8_t pin[3], uint32_t windowMs, uint32_t trans[3],
                              uint32_t &samples) {
  uint32_t mask[3];
  for (uint8_t i = 0; i < 3; i++) {
    pinMode(pin[i], INPUT);
    mask[i] = 1UL << pin[i];
    trans[i] = 0;
  }
  samples = 0;
  uint32_t last = REG_READ(GPIO_IN_REG);
  const uint32_t t0 = millis();
  for (;;) {
    // Le test d'horloge est sorti de la boucle serree : il coute plus cher que
    // la lecture elle-meme et ecraserait la cadence d'echantillonnage.
    for (uint16_t burst = 0; burst < 512; burst++) {
      uint32_t now = REG_READ(GPIO_IN_REG);
      uint32_t diff = now ^ last;
      if (diff) {
        for (uint8_t i = 0; i < 3; i++)
          if (diff & mask[i]) trans[i]++;
        last = now;
      }
    }
    samples += 512;
    if (millis() - t0 >= windowMs) break;
  }
}

void BenqHalo::probeDirectMode(Print &out, uint32_t windowMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  out.println();
  out.println("=== Sondage du mode direct (CFG1 bit 4, DIR_EN) ===");
  out.println("  DIR_EN=1 : 'TX/RX data from/to external MCU directly'. Le moteur");
  out.println("  de paquets est court-circuite : plus de correlateur, donc plus");
  out.println("  besoin de connaitre l'adresse.");
  out.println("  On teste les deux ordres possibles -- activer DIR_EN avant");
  out.println("  d'entrer en RX, ou entrer en RX puis activer DIR_EN -- et on");
  out.println("  releve OMST de part et d'autre de l'ecriture du bit.");
  out.println("  Les trois broches sont lues simultanement via GPIO_IN.");
  out.println("  Le niveau haut ne prouve rien : le BC5602 a ses propres");
  out.println("  pull-ups. Seul le compteur de transitions fait foi.");
  out.println();
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER PENDANT TOUTE LA MESURE.");
  out.println("  Les boutons tactiles n'emettent qu'une impulsion au toucher :");
  out.println("  les maintenir ne produit rien. La molette, elle, emet en");
  out.println("  continu tant qu'on la tourne.");
  Serial.flush();

  const uint8_t pinList[3] = {(uint8_t)PIN_RF_SCK, (uint8_t)PIN_RF_MISO, (uint8_t)PIN_RF_MOSI};
  const char *const pinNames[3] = {"SCK ", "MISO", "MOSI"};

  struct PhaseDef {
    const char *name;
    bool dirEn;
    bool rxFirst;
    bool csnLow;
  };
  const PhaseDef phases[5] = {
      {"reference : DIR_EN=0", false, false, false},
      {"DIR_EN=1 puis RX, CSN haut", true, false, false},
      {"DIR_EN=1 puis RX, CSN bas", true, false, true},
      {"RX puis DIR_EN=1, CSN haut", true, true, false},
      {"RX puis DIR_EN=1, CSN bas", true, true, true},
  };
  const uint8_t kPhases = 5;

  auto setDirEn = [&](bool on) -> uint8_t {
    uint8_t cfg1 = radio.readRegister(REG_CFG1 | CMD_READ_REGISTER);
    uint8_t want = on ? (uint8_t)(cfg1 | 0x10) : (uint8_t)(cfg1 & (uint8_t)~0x10);
    radio.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, want);
    return radio.readRegister(REG_CFG1 | CMD_READ_REGISTER);
  };

  uint32_t trans[kPhases][3];
  for (uint8_t i = 0; i < kPhases; i++)
    for (uint8_t j = 0; j < 3; j++) trans[i][j] = 0;

  for (uint8_t phase = 0; phase < kPhases; phase++) {
    const PhaseDef &ph = phases[phase];

    resetRadio();

    uint8_t cfgBack = 0, omstBefore = 0xFF, omstAfter = 0xFF;
    if (ph.rxFirst) {
      radio.enterRxMode();
      omstBefore = radio.operationMode();
      cfgBack = setDirEn(ph.dirEn);
      omstAfter = radio.operationMode();
    } else {
      cfgBack = setDirEn(ph.dirEn);
      omstBefore = radio.operationMode();
      radio.enterRxMode();
      omstAfter = radio.operationMode();
    }

    out.println();
    out.print("  phase : ");
    out.println(ph.name);
    out.print("    CFG1 relu 0x");
    out.print(cfgBack, HEX);
    out.print(", DIR_EN effectif ");
    out.print((cfgBack & 0x10) ? "1" : "0");
    out.print(", OMST avant ");
    out.print(omstBefore);
    out.print(" -> apres ");
    out.print(omstAfter);
    out.print("  (5 = RX) : ");
    out.println(omstAfter == OMST_RX ? "en reception" : "PAS en reception");
    out.println("    ... tourne la molette maintenant ...");
    Serial.flush();

    uint8_t pd[3] = {0, 0, 0}, pu[3] = {0, 0, 0};

    radio.suspendBus();
    if (ph.csnLow) digitalWrite(PIN_RF_CSN, LOW);

    for (uint8_t i = 0; i < 3; i++) pinLevels(pinList[i], pd[i], pu[i]);
    uint32_t samples = 0;
    countTransitions3(pinList, windowMs, trans[phase], samples);

    digitalWrite(PIN_RF_CSN, HIGH);
    radio.resumeBus();

    for (uint8_t i = 0; i < 3; i++) {
      out.print("    ");
      out.print(pinNames[i]);
      out.print(" : transitions ");
      out.print(trans[phase][i]);
      out.print(" sur ");
      out.print(samples);
      out.print(" ech.  (niveau haut ");
      out.print(pd[i]);
      out.print("% en tirage bas, ");
      out.print(pu[i]);
      out.println("% en tirage haut)");
    }
    Serial.flush();
  }

  out.println();
  out.println("  --- lecture du resultat ---");
  bool found = false;
  for (uint8_t i = 0; i < 3; i++) {
    for (uint8_t phase = 1; phase < kPhases; phase++) {
      if (trans[phase][i] > (trans[0][i] * 4 + 50)) {
        found = true;
        out.print("  piste : ");
        out.print(pinNames[i]);
        out.print(" s'anime en phase '");
        out.print(phases[phase].name);
        out.println("'");
      }
    }
  }
  if (!found) {
    out.println("  aucune broche ne s'anime, quel que soit l'ordre.");
    out.println("  Regarde la colonne OMST : si la puce n'atteint jamais 5 avec");
    out.println("  DIR_EN=1, elle n'ecoutait pas, et l'absence de bits ne prouve");
    out.println("  rien sur le mode direct lui-meme.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Reception en mode direct
// ---------------------------------------------------------------------------

void BenqHalo::probeDirectRx(Print &out, uint32_t windowMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  auto setCfg1 = [&](uint8_t value) -> uint8_t {
    radio.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, value);
    return radio.readRegister(REG_CFG1 | CMD_READ_REGISTER);
  };

  auto waitOmst = [&](uint8_t want, uint32_t timeoutUs) -> uint8_t {
    for (uint32_t waited = 0; waited < timeoutUs; waited += 50) {
      uint8_t m = radio.operationMode();
      if (m == want) return m;
      delayMicroseconds(50);
    }
    return radio.operationMode();
  };

  // Configuration de reception commune, sans rien qui depende d'une adresse :
  // en mode direct le correlateur est hors du chemin, l'adresse ecrite dans le
  // registre n'a plus de role.
  auto configRx = [&]() {
    resetRadio();
    uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
    radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask | MASK_PRM_RX));
    radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);
    radio.clearInterrupts();
    radio.command(CMD_FLUSH_RX_FIFO);
  };

  out.println();
  out.println("=== Reception en mode direct ===");
  out.println();
  out.println("  --- 1. comment atteindre le RX avec DIR_EN=1 ---");
  out.println("  CFG1=0x50 reprend la valeur du driver tiers : AGC_EN + DIR_EN.");
  out.println("  Notre configuration tournait jusqu'ici avec AGC_EN=0.");
  out.println("  ds.txt:717 documente une entree en RX par le registre CE, qui");
  out.println("  ne passe pas par le decodeur de commandes strobe.");
  out.println("  RSSI2 sert de temoin independant : OMST decrit le moteur de");
  out.println("  paquets, soit exactement ce que DIR_EN debranche.");
  out.println();
  out.println("  >>> TOURNE LA MOLETTE DES MAINTENANT ET JUSQU A LA FIN.");
  Serial.flush();

  struct EntryDef {
    const char *name;
    uint8_t cfg1;
    uint8_t method;  // 0 = registre CE, 1 = strobe 0x8E, 2 = OM 0x03 puis 0x07
  };
  const EntryDef entries[6] = {
      {"DIR_EN=0, entree par registre CE", 0x40, 0},
      {"DIR_EN=0, entree par strobe 0x8E", 0x40, 1},
      {"DIR_EN=1, entree par registre CE", 0x50, 0},
      {"DIR_EN=1, entree par strobe 0x8E", 0x50, 1},
      {"DIR_EN=1, OM 0x03 puis 0x07", 0x50, 2},
      {"DIR_EN=1, registre CE puis OM 0x07", 0x50, 3},
  };

  uint8_t bestEntry = 0xFF;
  for (uint8_t e = 0; e < 6; e++) {
    configRx();
    uint8_t cfgBack = setCfg1(entries[e].cfg1);

    switch (entries[e].method) {
      case 0:
        radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x01);
        break;
      case 1:
        radio.command(CMD_RX_MODE);
        break;
      case 2:
        radio.writeRegister(B0_OM | CMD_WRITE_REGISTER, 0x03);
        delayMicroseconds(50);
        radio.writeRegister(B0_OM | CMD_WRITE_REGISTER, 0x07);
        break;
      default:
        radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x01);
        delayMicroseconds(50);
        radio.writeRegister(B0_OM | CMD_WRITE_REGISTER, 0x07);
        break;
    }

    uint8_t omst = waitOmst(OMST_RX, 20000);
    uint8_t ce = radio.readRegister(REG_CE | CMD_READ_REGISTER);

    // Temoin independant du moteur de paquets : RSSI2 est decrit comme une
    // mesure temps reel (ds.txt:824). Si le plancher descend pendant que la
    // molette tourne, le demodulateur travaille -- meme si OMST dit le
    // contraire, puisque OMST rapporte l'etat de ce que DIR_EN debranche.
    uint8_t rssiMin = 0xFF, rssiMax = 0x00;
    for (uint16_t i = 0; i < 400; i++) {
      uint8_t r = radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER);
      if (r < rssiMin) rssiMin = r;
      if (r > rssiMax) rssiMax = r;
      delayMicroseconds(1000);
    }

    out.print("    ");
    out.print(entries[e].name);
    out.print("  -> CFG1 0x");
    out.print(cfgBack, HEX);
    out.print(", CE 0x");
    out.print(ce, HEX);
    out.print(", OMST ");
    out.print(omst);
    out.print(", RSSI de ");
    out.print(rssiMax);
    out.print(" a ");
    out.print(rssiMin);
    out.print(" dB");
    out.println(omst == OMST_RX ? "  *** EN RECEPTION" : "");
    Serial.flush();

    if (omst == OMST_RX && entries[e].cfg1 == 0x50 && bestEntry == 0xFF) bestEntry = e;
  }

  if (bestEntry == 0xFF) {
    out.println();
    out.println("  Aucune methode n'amene la puce en RX avec DIR_EN=1.");
    out.println("  Le balayage des selecteurs est lance quand meme : OMST est un");
    out.println("  indicateur du moteur de paquets, et c'est precisement lui que");
    out.println("  DIR_EN court-circuite. Il peut donc mentir ici.");
    bestEntry = 2;  // registre CE, le chemin documente
  } else {
    out.println();
    out.print("  Methode retenue pour la suite : ");
    out.println(entries[bestEntry].name);
  }

  out.println();
  out.println("  --- 2. quel selecteur GIO2 sort les bits recus ---");
  out.println("  GIO2S documente : 0 rien, 1 SDO, 5 IRQ. Le driver tiers prouve");
  out.println("  que 3 est DIRECT_TXD. On cherche l'equivalent en reception");
  out.println("  parmi les valeurs que le datasheet declare sans fonction.");
  out.println("  Attention : avec GIO2S different de 1, SDO disparait, donc plus");
  out.println("  aucune lecture SPI n'est possible -- on ecrit a l'aveugle.");
  out.println();
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER PENDANT TOUT LE BALAYAGE.");
  Serial.flush();

  const uint8_t pinList[3] = {(uint8_t)PIN_RF_SCK, (uint8_t)PIN_RF_MISO, (uint8_t)PIN_RF_MOSI};
  uint32_t trans[8][3];

  for (uint8_t sel = 0; sel < 8; sel++) {
    configRx();
    setCfg1(entries[bestEntry].cfg1);
    if (entries[bestEntry].method == 0 || entries[bestEntry].method == 3)
      radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x01);
    else if (entries[bestEntry].method == 1)
      radio.command(CMD_RX_MODE);
    else {
      radio.writeRegister(B0_OM | CMD_WRITE_REGISTER, 0x03);
      delayMicroseconds(50);
      radio.writeRegister(B0_OM | CMD_WRITE_REGISTER, 0x07);
    }

    // Ecriture a l'aveugle : elle part par SDIO, elle n'a pas besoin de SDO.
    radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, (uint8_t)(0x40 | (sel << 3)));

    uint32_t samples = 0;
    radio.suspendBus();
    countTransitions3(pinList, windowMs, trans[sel], samples);
    radio.resumeBus();

    out.print("    GIO2S=");
    out.print(sel);
    out.print(sel == 1 ? " (SDO documente)  " : (sel == 3 ? " (DIRECT_TXD)     " : "                  "));
    out.print("MISO ");
    out.print(trans[sel][1]);
    out.print(" transitions, SCK ");
    out.print(trans[sel][0]);
    out.print(", MOSI ");
    out.print(trans[sel][2]);
    out.print("  sur ");
    out.print(samples);
    out.println(" ech.");
    Serial.flush();
  }

  // Remettre SDO, sinon plus aucune lecture SPI ne fonctionne apres la commande.
  radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
  setCfg1(0x40);
  resetRadio();

  out.println();
  out.println("  --- lecture du resultat ---");
  uint32_t quietest = 0xFFFFFFFF;
  for (uint8_t sel = 0; sel < 8; sel++)
    if (trans[sel][1] < quietest) quietest = trans[sel][1];

  bool found = false;
  for (uint8_t sel = 0; sel < 8; sel++) {
    if (trans[sel][1] > (quietest * 4 + 100)) {
      found = true;
      out.print("  piste : GIO2S=");
      out.print(sel);
      out.print(" fait s'animer MISO (");
      out.print(trans[sel][1]);
      out.println(" transitions)");
    }
  }
  if (!found) {
    out.println("  aucun selecteur ne fait sortir de donnees sur GIO2.");
    out.println("  Il reste GIO3, broche 8 du module, non cablee ici : c'est la");
    out.println("  que le driver tiers recupere TBCLK, l'horloge bit du mode");
    out.println("  direct. Un fil de plus permettrait de la tester.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Ou emet la telecommande
// ---------------------------------------------------------------------------

void BenqHalo::sweepBand(Print &out, uint8_t cycles) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  constexpr uint8_t kChannels = 84;    // 2400 a 2483 MHz
  constexpr uint32_t kDwellUs = 2000;  // temps passe sur chaque canal a chaque passe
  constexpr uint8_t kPassesPerSweep = 6;
  constexpr int kMinGainDb = 6;

  if (cycles < 2) cycles = 2;
  if (cycles > 8) cycles = 8;

  uint8_t score[kChannels];   // nombre de cycles ou la molette a fait monter le signal
  uint8_t bestActive[kChannels];
  int bestGain[kChannels];
  for (uint8_t ch = 0; ch < kChannels; ch++) {
    score[ch] = 0;
    bestActive[ch] = 0xFF;
    bestGain[ch] = 0;
  }

  out.println();
  out.println("=== Ou emet la telecommande ===");
  out.println("  RSSI_NEGDB : unite -dB, donc PLUS PETIT = PLUS FORT.");
  out.println();
  out.println("  Une seule comparaison repos/actif ne vaut rien : entre les deux");
  out.println("  mesures, le Wi-Fi ambiant peut se mettre a emettre et produire");
  out.println("  exactement la meme signature. On alterne donc plusieurs fois,");
  out.println("  et on ne retient qu'un canal qui gagne a TOUS les cycles --");
  out.println("  le Wi-Fi ne sait pas quand tu tournes la molette.");
  out.println("  Les canaux qui recouvrent une porteuse Wi-Fi sont signales.");
  Serial.flush();

  prepareToSniff();

  auto sweepInto = [&](uint8_t *dest) {
    memset(dest, 0xFF, kChannels);
    for (uint8_t p = 0; p < kPassesPerSweep; p++) {
      for (uint8_t ch = 0; ch < kChannels; ch++) {
        radio.command(CMD_LIGHT_SLEEP);
        radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, ch);
        radio.enterRxMode();
        uint32_t t0 = micros();
        while (micros() - t0 < kDwellUs) {
          uint8_t v = radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER);
          if (v < dest[ch]) dest[ch] = v;
        }

        // Rendre la main regulierement. Sans cela la tache au repos est
        // affamee pendant tout le balayage et le chien de garde redemarre la
        // carte -- ce qui, sur l'USB natif, se traduit par un silence complet
        // puisque le moniteur ne se rattache pas apres la re-enumeration.
        if ((ch & 0x07) == 0x07) delay(1);
      }
    }
  };

  auto countdown = [&](const char *what, uint8_t seconds) {
    out.print("    ");
    out.println(what);
    for (uint8_t t = seconds; t >= 1; t--) {
      out.print("      ");
      out.print(t);
      out.println("...");
      Serial.flush();
      delay(1000);
    }
  };

  uint8_t quiet[kChannels], active[kChannels];

  for (uint8_t cycle = 0; cycle < cycles; cycle++) {
    out.println();
    out.print("  cycle ");
    out.print(cycle + 1);
    out.print(" sur ");
    out.println(cycles);
    Serial.flush();

    countdown("LACHE TOUT, ne touche a rien :", 3);
    sweepInto(quiet);

    countdown("TOURNE LA MOLETTE, maintenant :", 3);
    sweepInto(active);

    uint8_t won = 0;
    for (uint8_t ch = 0; ch < kChannels; ch++) {
      int gain = (int)quiet[ch] - (int)active[ch];
      if (gain >= kMinGainDb) {
        score[ch]++;
        won++;
        if (gain > bestGain[ch]) bestGain[ch] = gain;
        if (active[ch] < bestActive[ch]) bestActive[ch] = active[ch];
      }
    }
    out.print("      canaux en hausse a ce cycle : ");
    out.println(won);
    Serial.flush();
  }

  out.println();
  out.println("  --- resultat ---");

  auto wifiOverlap = [](uint8_t ch) -> int {
    const int mhz = 2400 + (int)ch;
    const int centres[3] = {2412, 2437, 2462};
    const int noms[3] = {1, 6, 11};
    for (uint8_t i = 0; i < 3; i++) {
      int d = mhz - centres[i];
      if (d < 0) d = -d;
      if (d <= 11) return noms[i];
    }
    return 0;
  };

  bool any = false;
  for (uint8_t ch = 0; ch < kChannels; ch++) {
    if (score[ch] < cycles) continue;
    any = true;
    out.print("    canal ");
    out.print(ch);
    out.print(" = ");
    out.print(2400 + ch);
    out.print(" MHz : en hausse aux ");
    out.print(cycles);
    out.print(" cycles, jusqu'a ");
    out.print(bestActive[ch]);
    out.print(" dB, gain max ");
    out.print(bestGain[ch]);
    out.print(" dB");
    int w = wifiOverlap(ch);
    if (w) {
      out.print("   [recouvre le Wi-Fi ");
      out.print(w);
      out.print("]");
    }
    out.println();
    Serial.flush();
  }

  if (!any) {
    out.println("    aucun canal ne gagne a tous les cycles.");
    out.println("    Les meilleurs partiels, pour information :");
    for (uint8_t rank = 0; rank < 6; rank++) {
      uint8_t bestCh = 0xFF, bestScore = 0;
      for (uint8_t ch = 0; ch < kChannels; ch++)
        if (score[ch] > bestScore) {
          bestScore = score[ch];
          bestCh = ch;
        }
      if (bestCh == 0xFF || bestScore == 0) break;
      out.print("      canal ");
      out.print(bestCh);
      out.print(" = ");
      out.print(2400 + bestCh);
      out.print(" MHz : ");
      out.print(bestScore);
      out.print(" cycles sur ");
      out.print(cycles);
      out.print(", gain max ");
      out.print(bestGain[bestCh]);
      out.print(" dB");
      int w = wifiOverlap(bestCh);
      if (w) {
        out.print("   [recouvre le Wi-Fi ");
        out.print(w);
        out.print("]");
      }
      out.println();
      score[bestCh] = 0;
      Serial.flush();
    }
  }
  out.println();
}

// Les trois vecteurs de validation du CRC partagent tous la MEME adresse, donc
// la contribution de celle-ci s'y reduit a une constante : deux modeles sont
// indiscernables sur ces donnees. Modele A, l'adresse est couverte et l'etat
// initial vaut 0xEFDF ; modele B, elle ne l'est pas et l'etat initial vaut
// 0x5042, qui n'est autre que crc(adresse Halo 2, 0xEFDF). Tant que le doute
// n'est pas leve, on teste les deux -- croire le seul modele A ferait rejeter
// une trame authentique du Halo 1.
static uint16_t crcOverFrame(uint16_t init, uint8_t pcf, const uint8_t *payload) {
  auto feed = [](uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (uint8_t i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
  };
  uint16_t crc = feed(init, pcf);
  for (uint8_t i = 0; i < 10; i++) crc = feed(crc, payload[i]);
  return crc;
}

// ---------------------------------------------------------------------------
//  Capture pendant l'appairage
// ---------------------------------------------------------------------------

void BenqHalo::capturePairing(Print &out, uint32_t seconds, uint8_t onlyChannel) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[128];

  // Ordre d'ecriture dans le registre ; sur l'air c'est l'inverse.
  const uint8_t addrA[4] = {0xB0, 0x00, 0x08, 0xE2};  // sur l'air : E2 08 00 B0
  const uint8_t addrB[4] = {0xE2, 0x08, 0x00, 0xB0};  // sur l'air : B0 00 08 E2
  const uint8_t channels[3] = {RF_CHANNEL_1, RF_CHANNEL_2, RF_CHANNEL_3};

  struct Combo {
    const uint8_t *addr;
    const char *addrName;
    uint8_t channel;
  };
  // En campant sur un seul canal on multiplie par trois le temps passe sur la
  // combinaison la plus probable -- et le canal 5 n'est pas une supposition,
  // il a ete mesure (commande 'presence').
  Combo combos[6];
  uint8_t comboCount = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (onlyChannel && channels[i] != onlyChannel) continue;
    combos[comboCount++] = {addrA, "E2 08 00 B0", channels[i]};
    combos[comboCount++] = {addrB, "B0 00 08 E2", channels[i]};
  }
  if (comboCount == 0) {  // canal demande hors du dossier FCC : on le prend tel quel
    combos[comboCount++] = {addrA, "E2 08 00 B0", onlyChannel};
    combos[comboCount++] = {addrB, "B0 00 08 E2", onlyChannel};
  }

  out.println();
  out.println("=== Capture pendant l'appairage ===");
  out.println("  L'adresse d'appairage est la seule que nous connaissions. Si la");
  out.println("  telecommande et la lampe negocient une adresse de communication,");
  out.println("  c'est forcement la qu'elle transite : c'est le seul moment ou");
  out.println("  elles se parlent sans deja se connaitre.");
  out.println();
  snprintf(line, sizeof(line), "  Debit %s, adresse de 4 octets.", dataRateName(dataRate_));
  out.println(line);
  int n = snprintf(line, sizeof(line), "  %u combinaison(s) de 3 s en boucle, canal/canaux :",
                   (unsigned)comboCount);
  for (uint8_t i = 0; i < comboCount; i += 2)
    n += snprintf(line + n, sizeof(line) - n, " %u", (unsigned)combos[i].channel);
  out.println(line);
  out.println("  32 octets vides par trame, sans filtrage de CRC materiel.");
  out.println();
  out.println("  MANIP A FAIRE, en boucle pendant toute la capture :");
  out.println("   1. debranche l'USB de la lampe");
  out.println("   2. appuie sur Favori + bouton de selection de lampe ~5 s");
  out.println("   3. couvre le capteur de lumiere au dos");
  out.println("   4. rebranche dans les 15 s");
  out.println("   5. recommence tant que la capture tourne");
  out.print("  Duree : ");
  out.print(seconds);
  out.println(" s. Chaque trame recue est affichee telle quelle.");
  Serial.flush();

  const uint32_t deadline = millis() + seconds * 1000UL;
  uint32_t frames = 0, valid = 0;
  uint8_t comboIdx = 0;

  while ((int32_t)(millis() - deadline) < 0) {
    const Combo &cb = combos[comboIdx];

    channel_ = cb.channel;
    sharedRadioConfig(ADDR_LEN_4, cb.addr, 4);

    uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
    radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask | MASK_PRM_RX));
    radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
    radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x00);
    radio.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, 32);
    radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);
    radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);
    radio.clearInterrupts();
    radio.command(CMD_FLUSH_RX_FIFO);
    radio.enterRxMode();

    // L'adresse telle qu'elle circule sur l'air, pour le calcul de CRC.
    uint8_t airAddr[4];
    for (uint8_t i = 0; i < 4; i++) airAddr[i] = cb.addr[3 - i];

    const uint32_t until = millis() + 3000;
    while ((int32_t)(millis() - until) < 0) {
      if ((int32_t)(millis() - deadline) >= 0) break;

      if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);

      // RX_DR actif a l'etat bas dans STATUS : 0 = une trame attend.
      if (radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR) {
        delayMicroseconds(200);
        continue;
      }

      uint8_t buf[32];
      radio.readFifo(buf, 32, false);
      radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
      radio.command(CMD_FLUSH_RX_FIFO);
      frames++;

      // Chaque ligne est composee en memoire puis ecrite d'un seul bloc : le
      // filtre 'time' du moniteur horodate chaque morceau recu, et une ligne
      // ecrite en plusieurs print() ressort eclatee. Sur un vidage hexadecimal
      // ce serait illisible, donc dangereux.
      snprintf(line, sizeof(line), "  trame %lu [%u MHz, air %s]", (unsigned long)frames,
               (unsigned)(2400 + cb.channel), cb.addrName);
      out.println(line);

      for (uint8_t half = 0; half < 2; half++) {
        int n = snprintf(line, sizeof(line), "   ");
        for (uint8_t i = 0; i < 16; i++)
          n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[half * 16 + i]);
        out.println(line);
      }

      // Verification : le premier octet est le PCF, les dix suivants le
      // payload, les deux d'apres le CRC. Si ca colle, la trame est authentique
      // et l'adresse d'appairage est la bonne.
      const uint16_t wantA = frameCrcFor(airAddr, buf[0], buf + 1);
      const uint16_t wantB = crcOverFrame(0x5042, buf[0], buf + 1);
      const uint16_t got = (uint16_t)((buf[11] << 8) | buf[12]);
      const bool okA = (wantA == got), okB = (wantB == got);
      if (okA || okB) valid++;
      snprintf(line, sizeof(line), "   CRC lu %04X | modele A %04X%s | modele B %04X%s", got,
               wantA, okA ? " <<< VALIDE" : "", wantB, okB ? " <<< VALIDE" : "");
      out.println(line);
      Serial.flush();
    }

    comboIdx = (uint8_t)((comboIdx + 1) % comboCount);
    delay(1);  // rendre la main : le chien de garde veille
  }

  channel_ = RF_CHANNEL_1;
  out.println();
  out.print("  Termine : ");
  out.print(frames);
  out.print(" trame(s) brute(s), dont ");
  out.print(valid);
  out.println(" avec un CRC valide.");
  if (frames == 0) {
    out.println("  Aucune trame sur l'adresse d'appairage, sur aucune des six");
    out.println("  combinaisons. Soit l'appairage n'utilise pas cette adresse sur");
    out.println("  le Halo 1, soit il n'emet pas pendant la fenetre couverte.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Accrochage sur le preambule
// ---------------------------------------------------------------------------

void BenqHalo::huntByPreamble(Print &out, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[160];

  out.println();
  out.println("=== Chasse a l'adresse par le preambule ===");
  out.println("  Sur l'air : [preambule] [adresse 4 o.] [PCF] [payload] [CRC].");
  out.println("  Le preambule est connu -- c'est AA repete. Le correlateur");
  out.println("  accepte une adresse de 3 octets : on lui donne 'AA AA X', il se");
  out.println("  cale sur le preambule plus le PREMIER octet d'adresse, et nous");
  out.println("  livre les trois octets suivants, qui sont le reste de l'adresse.");
  out.println("  Un seul inconnu : X, sur 256 valeurs. Les deux polarites de");
  out.println("  preambule sont essayees, AA puis 55.");
  out.println("  Canal 5 et 125 kbps, confirmes par le portage qui fonctionne.");
  out.println();
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER PENDANT TOUT LE BALAYAGE.");
  snprintf(line, sizeof(line), "  Duree : environ %lu s.",
           (unsigned long)((512UL * dwellMs) / 1000UL));
  out.println(line);
  Serial.flush();

  const uint8_t polarities[2] = {0xAA, 0x55};
  uint32_t hits = 0, confirmed = 0;

  channel_ = RF_CHANNEL_1;

  for (uint8_t pol = 0; pol < 2; pol++) {
    const uint8_t P = polarities[pol];

    for (uint16_t x = 0; x < 256; x++) {
      // Sur l'air on veut P P X ; le registre se remplit dans l'ordre inverse.
      const uint8_t reg3[3] = {(uint8_t)x, P, P};
      sharedRadioConfig(ADDR_LEN_3, reg3, 3);

      uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
      radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask | MASK_PRM_RX));
      radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
      radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x00);
      radio.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, 32);
      radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);
      radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);
      radio.clearInterrupts();
      radio.command(CMD_FLUSH_RX_FIFO);
      radio.enterRxMode();

      const uint32_t until = millis() + dwellMs;
      while ((int32_t)(millis() - until) < 0) {
        if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);

        // RX_DR est actif a l'etat bas dans STATUS : 0 = une trame attend.
        if (radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR) {
          delayMicroseconds(200);
          continue;
        }

        uint8_t buf[32];
        radio.readFifo(buf, 32, false);
        radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
        radio.command(CMD_FLUSH_RX_FIFO);
        hits++;

        // Accroche sur P P X : la FIFO commence donc au DEUXIEME octet de
        // l'adresse. L'adresse complete sur l'air est X puis les trois suivants.
        const uint8_t airAddr[4] = {(uint8_t)x, buf[0], buf[1], buf[2]};
        const uint8_t pcf = buf[3];
        const uint8_t *payload = buf + 4;
        const uint16_t wantA = frameCrcFor(airAddr, pcf, payload);
        const uint16_t wantB = crcOverFrame(0x5042, pcf, payload);
        const uint16_t got = (uint16_t)((buf[14] << 8) | buf[15]);
        const bool okA = (wantA == got), okB = (wantB == got);

        snprintf(line, sizeof(line), "  accroche sur preambule %02X, X=%02X", P, (unsigned)x);
        out.println(line);
        int n = snprintf(line, sizeof(line), "   FIFO ");
        for (uint8_t i = 0; i < 16; i++)
          n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[i]);
        out.println(line);
        snprintf(line, sizeof(line), "   adresse supposee (sur l'air) %02X %02X %02X %02X",
                 airAddr[0], airAddr[1], airAddr[2], airAddr[3]);
        out.println(line);
        snprintf(line, sizeof(line), "   PCF %02X, CRC lu %04X | A %04X%s | B %04X%s", pcf, got,
                 wantA, okA ? " <<< CONFIRME" : "", wantB, okB ? " <<< CONFIRME" : "");
        out.println(line);
        if (okA || okB) {
          confirmed++;
          snprintf(line, sizeof(line),
                   "   *** ADRESSE TROUVEE : ecris-la avec 'addr %02X%02X%02X%02X'",
                   airAddr[3], airAddr[2], airAddr[1], airAddr[0]);
          out.println(line);
        }
        Serial.flush();
      }

      // Rendre la main : le chien de garde veille.
      if ((x & 0x07) == 0x07) delay(1);
    }

    snprintf(line, sizeof(line), "  polarite %02X terminee.", P);
    out.println(line);
    Serial.flush();
  }

  out.println();
  snprintf(line, sizeof(line), "  Termine : %lu accroche(s), dont %lu confirmee(s) par CRC.",
           (unsigned long)hits, (unsigned long)confirmed);
  out.println(line);
  if (hits == 0) {
    out.println("  Aucune accroche. Cela invalide l'hypothese d'un preambule de");
    out.println("  deux octets : avec un preambule d'un seul octet il faudrait");
    out.println("  chercher 'AA X Y', soit 65536 combinaisons, hors de portee.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Entend-on la telecommande, et ou ?
// ---------------------------------------------------------------------------

void BenqHalo::probePresence(Print &out, uint8_t cycles, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  // Bornes en RSSI_NEGDB : unite -dB, donc plus petit = plus fort.
  constexpr uint8_t kBands = 5;
  const uint8_t bandMax[kBands] = {49, 69, 89, 109, 255};
  const char *const bandName[kBands] = {"tres fort", "fort", "moyen", "faible", "plancher"};

  struct ChanDef {
    uint8_t ch;
    const char *note;
  };
  const ChanDef chans[4] = {
      {RF_CHANNEL_1, "canal du portage qui fonctionne"},
      {RF_CHANNEL_2, "dossier FCC"},
      {RF_CHANNEL_3, "dossier FCC"},
      {80, "TEMOIN : balise BLE ambiante, ne doit PAS ressortir"},
  };

  char line[160];

  out.println();
  out.println("=== Entend-on la telecommande ? ===");
  out.println("  On campe sur chaque canal au lieu de balayer, et on compare la");
  out.println("  DISTRIBUTION du RSSI plutot que son minimum : un minimum se fait");
  out.println("  piéger par un seul pic isole, une distribution non.");
  out.println("  Le dernier canal est un TEMOIN connu pour ne porter que du bruit");
  out.println("  ambiant. S'il ressort comme les autres, c'est que la methode ne");
  out.println("  discrimine rien et qu'il ne faut croire aucun de ses verdicts.");
  snprintf(line, sizeof(line), "  %u cycles de %lu ms par canal, repos puis molette.",
           (unsigned)cycles, (unsigned long)dwellMs);
  out.println(line);
  Serial.flush();

  for (uint8_t ci = 0; ci < 4; ci++) {
    uint32_t hist[2][kBands];
    for (uint8_t p = 0; p < 2; p++)
      for (uint8_t b = 0; b < kBands; b++) hist[p][b] = 0;

    out.println();
    snprintf(line, sizeof(line), "  canal %u = %u MHz  (%s)", (unsigned)chans[ci].ch,
             (unsigned)(2400 + chans[ci].ch), chans[ci].note);
    out.println(line);
    Serial.flush();

    channel_ = chans[ci].ch;

    for (uint8_t cycle = 0; cycle < cycles; cycle++) {
      for (uint8_t phase = 0; phase < 2; phase++) {
        snprintf(line, sizeof(line), "    cycle %u : %s", (unsigned)(cycle + 1),
                 phase == 0 ? "LACHE TOUT" : "TOURNE LA MOLETTE");
        out.println(line);
        Serial.flush();
        delay(1500);  // le temps de reagir

        prepareToSniff();

        const uint32_t until = millis() + dwellMs;
        uint32_t guard = 0;
        while ((int32_t)(millis() - until) < 0) {
          if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
          uint8_t v = radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER);
          for (uint8_t b = 0; b < kBands; b++)
            if (v <= bandMax[b]) {
              hist[phase][b]++;
              break;
            }
          if ((++guard & 0x03FF) == 0) delay(1);  // le chien de garde veille
        }
      }
    }

    for (uint8_t p = 0; p < 2; p++) {
      int n = snprintf(line, sizeof(line), "    %s :", p == 0 ? "repos " : "actif ");
      for (uint8_t b = 0; b < kBands; b++)
        n += snprintf(line + n, sizeof(line) - n, "  %s %lu", bandName[b],
                      (unsigned long)hist[p][b]);
      out.println(line);
    }

    // Verdict : une population FORTE (les deux premieres bandes) qui apparait
    // en actif et n'existait pas au repos.
    const uint32_t quietStrong = hist[0][0] + hist[0][1];
    const uint32_t activeStrong = hist[1][0] + hist[1][1];
    if (activeStrong > quietStrong * 4 + 50)
      snprintf(line, sizeof(line),
               "    verdict : population forte apparue (repos %lu -> actif %lu)",
               (unsigned long)quietStrong, (unsigned long)activeStrong);
    else
      snprintf(line, sizeof(line), "    verdict : rien de probant (repos %lu, actif %lu)",
               (unsigned long)quietStrong, (unsigned long)activeStrong);
    out.println(line);
    Serial.flush();
  }

  channel_ = RF_CHANNEL_1;
  out.println();
  out.println("  Lis d'abord le TEMOIN. S'il annonce lui aussi une population");
  out.println("  forte apparue, la mesure est aveugle et les autres verdicts");
  out.println("  sont a jeter.");
  out.println();
}

// ---------------------------------------------------------------------------
//  Duree des rafales : en deduire le debit
// ---------------------------------------------------------------------------

void BenqHalo::measureBursts(Print &out, uint32_t seconds, uint8_t threshold) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  constexpr uint8_t kBins = 8;
  // Bornes hautes en microsecondes.
  const uint32_t binMax[kBins] = {99, 199, 399, 799, 1499, 2499, 4999, 0xFFFFFFFF};
  const char *const binName[kBins] = {"<100us",    "100-199",   "200-399",  "400-799",
                                      "800-1499",  "1500-2499", "2500-4999", ">=5000"};
  uint32_t bins[kBins];
  for (uint8_t i = 0; i < kBins; i++) bins[i] = 0;

  char line[176];

  out.println();
  out.println("=== Duree des rafales ===");
  snprintf(line, sizeof(line), "  Canal %u = %u MHz, seuil %u dB, pendant %lu s.",
           (unsigned)channel_, (unsigned)(2400 + channel_), (unsigned)threshold,
           (unsigned long)seconds);
  out.println(line);
  out.println("  Une trame fait 19 octets : 2 de preambule, 4 d'adresse, 1 de PCF,");
  out.println("  10 de payload et 2 de CRC, soit 152 bits. Donc 1216 us a");
  out.println("  125 kbps, 608 us a 250 kbps, 304 us a 500 kbps. La duree");
  out.println("  mesuree tranche le debit sans avoir a le deviner.");
  out.println();
  out.println("  >>> Il faut une source qui emette pendant toute la mesure :");
  out.println("      soit la balise d'etalonnage sur l'autre carte, soit la");
  out.println("      molette de la telecommande. PAS LES DEUX -- leurs rafales");
  out.println("      se melangeraient et la mesure ne voudrait plus rien dire.");
  Serial.flush();

  prepareToSniff();

  uint32_t bursts = 0, samples = 0;
  uint32_t shortest = 0xFFFFFFFF, longest = 0;
  uint8_t strongest = 0xFF;
  uint32_t sumUs = 0;

  const uint32_t deadline = millis() + seconds * 1000UL;
  bool inBurst = false;
  uint32_t burstStart = 0;

  while ((int32_t)(millis() - deadline) < 0) {
    // Fenetres de 100 ms entre lesquelles on rend la main : une rafale a cheval
    // sur une pause serait coupee en deux, mais c'est rare et sans consequence
    // sur la distribution.
    const uint32_t windowEnd = millis() + 100;
    while ((int32_t)(millis() - windowEnd) < 0) {
      if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
      const uint8_t v = radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER);
      const uint32_t now = micros();
      samples++;

      if (v <= threshold) {
        if (!inBurst) {
          inBurst = true;
          burstStart = now;
        }
        if (v < strongest) strongest = v;
      } else if (inBurst) {
        inBurst = false;
        const uint32_t dur = now - burstStart;
        bursts++;
        sumUs += dur;
        if (dur < shortest) shortest = dur;
        if (dur > longest) longest = dur;
        for (uint8_t b = 0; b < kBins; b++)
          if (dur <= binMax[b]) {
            bins[b]++;
            break;
          }
      }
    }
    inBurst = false;  // la pause casse la continuite, on repart proprement
    delay(1);
  }

  out.println();
  snprintf(line, sizeof(line), "  %lu rafale(s) sur %lu echantillons",
           (unsigned long)bursts, (unsigned long)samples);
  out.println(line);
  if (samples)
    snprintf(line, sizeof(line), "  cadence d'echantillonnage : un point toutes les %lu us",
             (unsigned long)((seconds * 1000000UL) / samples));
  out.println(line);

  if (bursts == 0) {
    out.println("  Aucune rafale sous ce seuil. Reessaie avec un seuil plus haut,");
    out.println("  par exemple 'rafale 20 75'.");
    out.println();
    return;
  }

  int n = snprintf(line, sizeof(line), "  durees :");
  for (uint8_t b = 0; b < kBins; b++)
    n += snprintf(line + n, sizeof(line) - n, "  %s %lu", binName[b], (unsigned long)bins[b]);
  out.println(line);

  snprintf(line, sizeof(line), "  plus courte %lu us, plus longue %lu us, moyenne %lu us",
           (unsigned long)shortest, (unsigned long)longest, (unsigned long)(sumUs / bursts));
  out.println(line);
  snprintf(line, sizeof(line), "  signal le plus fort : %u dB", (unsigned)strongest);
  out.println(line);

  // La bande la plus peuplee designe le debit, si la trame fait bien 19 octets.
  uint8_t top = 0;
  for (uint8_t b = 1; b < kBins; b++)
    if (bins[b] > bins[top]) top = b;
  out.println();
  snprintf(line, sizeof(line), "  duree dominante : %s", binName[top]);
  out.println(line);
  if (top == 4)
    out.println("  compatible avec 125 kbps sur une trame de 19 octets.");
  else if (top == 3)
    out.println("  compatible avec 250 kbps sur une trame de 19 octets.");
  else if (top == 2)
    out.println("  compatible avec 500 kbps sur une trame de 19 octets.");
  else
    out.println("  ne correspond a aucun des trois debits sur une trame de 19");
  if (top != 2 && top != 3 && top != 4)
    out.println("  octets : soit la trame n'a pas cette longueur, soit ces rafales");
  if (top != 2 && top != 3 && top != 4)
    out.println("  ne sont pas la telecommande.");
  out.println();
}

// ---------------------------------------------------------------------------
//  Etalonnage : le recepteur fonctionne-t-il ?
// ---------------------------------------------------------------------------

// Parametres d'etalonnage, communs a l'emetteur et au recepteur : les deux
// cartes doivent s'accorder sans se concerter. Premier octet sur l'air en 0xE_,
// conforme a la regle du quartet de poids fort du datasheet (ds.txt:1374).
static const uint8_t kCalAirAddr[4] = {0xE1, 0x22, 0x33, 0x44};
static const uint8_t kCalRegAddr[4] = {0x44, 0x33, 0x22, 0xE1};
static const uint8_t kCalWrongReg[4] = {0x44, 0x33, 0x22, 0xE2};  // un seul octet change
static const uint8_t kCalPattern[10] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

// Configure une puce pour la boucle locale. Volontairement identique a ce que
// font les commandes de chasse -- CRC materiel desactive en reception, payload
// statique, pas d'auto-ACK -- car etalonner une autre configuration que celle
// qu'on utilise ne prouverait rien.
static void configForLoopback(BC5602 &r, uint8_t channel, const uint8_t addr[4], bool receiver,
                              uint8_t rate) {
  r.softwareReset();
  delay(20);
  r.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
  r.setBank(0);
  r.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, CFG1_AGC_EN);
  r.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, channel);
  r.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, (uint8_t)(rate | ADDR_LEN_4));
  r.writeCommandData(CMD_WRITE_PTX_ADDRESS, addr, 4);

  uint8_t mask = r.readRegister(REG_MASK | CMD_READ_REGISTER);
  mask = receiver ? (uint8_t)(mask | MASK_PRM_RX) : (uint8_t)(mask & (uint8_t)~MASK_PRM_RX);
  r.writeRegister(REG_MASK | CMD_WRITE_REGISTER, mask);

  r.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
  // DPL2 bit 0 = EN_DYN_ACK : "PTX write TX FIFO with No-Auto-ACK command
  // enable" (ds.txt:869). Sans ce bit, la commande d'ecriture FIFO sans
  // auto-ACK est refusee EN SILENCE : la FIFO reste vide et la puce n'emet
  // jamais. Mesure a l'appui (commande 'autotest') : EN_DYN_ACK=0 laisse
  // TX_EMPTY a 1, EN_DYN_ACK=1 remplit la FIFO et TX_DS tombe.
  r.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, receiver ? 0x00 : 0x01);
  r.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);
  // L'emetteur fabrique un CRC, le recepteur le laisse passer dans la FIFO :
  // c'est exactement le reglage des commandes de chasse.
  r.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, receiver ? 0x00 : PKT1_CRC_ENABLE);
  if (receiver) r.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, 13);
  r.clearInterrupts();
  r.command(CMD_FLUSH_RX_FIFO);

  // CE = 1, sans quoi la machine d'etats ne quitte jamais le Light Sleep.
  // ds.txt:711 : "If the device is set as a PTX device and the CE bit is set
  // high [...] The PTX device will enter the TX mode automatically once the TX
  // FIFO is not empty." L'emission ne se declenche donc PAS par la commande
  // 0x0E mais par le remplissage de la FIFO, CE pose. Mesure a l'appui : sans
  // CE, 2553 trames ecrites, zero TX_DS, OMST bloque a 2.
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x01);
}

void BenqHalo::loopbackTest(Print &out, uint16_t frames) {
  char line[176];

  out.println();
  out.println("=== Etalonnage du recepteur ===");
  out.println("  Le second module emet une trame connue, le premier l'ecoute.");
  out.println("  Tant que ces octets ne reviennent pas, aucun resultat negatif");
  out.println("  d'une chasse ne veut rien dire : un recepteur mort rend zero");
  out.println("  pour tous les debits, tous les canaux et toutes les adresses.");
  Serial.flush();

  if (!radio.present()) {
    out.println("  Module 1 absent.");
    return;
  }

  if (!radio2.begin(PIN_RF_SCK, PIN_RF_MISO, PIN_RF_MOSI, PIN_RF_CSN2, 1000000UL)) {
    snprintf(line, sizeof(line), "  Module 2 muet sur CSN=IO%u (version lue 0x%06lX).",
             (unsigned)PIN_RF_CSN2, (unsigned long)radio2.chipVersion());
    out.println(line);
    out.println("  Verifie l'alimentation, la masse, et que CSN va bien sur cette");
    out.println("  broche. Les trois autres fils sont partages avec le module 1.");
    return;
  }
  snprintf(line, sizeof(line), "  Module 2 present, version de puce 0x%06lX.",
           (unsigned long)radio2.chipVersion());
  out.println(line);


  snprintf(line, sizeof(line), "  Adresse sur l'air %02X %02X %02X %02X, canal %u, 125 kbps.",
           kCalAirAddr[0], kCalAirAddr[1], kCalAirAddr[2], kCalAirAddr[3], (unsigned)RF_CHANNEL_1);
  out.println(line);

  for (uint8_t pass = 0; pass < 2; pass++) {
    const bool matching = (pass == 0);
    out.println();
    out.println(matching ? "  --- 1. memes adresses : on doit recevoir ---"
                         : "  --- 2. adresse du recepteur fausse d'un octet : on doit RIEN recevoir ---");
    Serial.flush();

    configForLoopback(radio2, RF_CHANNEL_1, kCalRegAddr, false, dataRate_);
    configForLoopback(radio, RF_CHANNEL_1, matching ? kCalRegAddr : kCalWrongReg, true, dataRate_);
    radio.enterRxMode();

    uint16_t sent = 0, got = 0, good = 0;
    bool shown = false;

    for (uint16_t i = 0; i < frames; i++) {
      radio2.command(CMD_FLUSH_TX_FIFO);
      radio2.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
      radio2.writeCommandData(CMD_WRITE_TX_FIFO_NO_ACK, kCalPattern, 10);
      radio2.command(CMD_TX_MODE);
      sent++;

      const uint32_t until = millis() + 30;
      while ((int32_t)(millis() - until) < 0) {
        if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
        if (radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR) continue;

        uint8_t buf[13];
        radio.readFifo(buf, 13, false);
        radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
        radio.command(CMD_FLUSH_RX_FIFO);
        got++;

        if (memcmp(buf + 1, kCalPattern, 10) == 0) good++;

        if (!shown) {
          shown = true;
          int n = snprintf(line, sizeof(line), "   premiere trame recue : ");
          for (uint8_t k = 0; k < 13; k++)
            n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[k]);
          out.println(line);
        }
        break;
      }
      if ((i & 0x0F) == 0x0F) delay(1);
    }

    snprintf(line, sizeof(line), "   %u emise(s), %u recue(s), %u conforme(s) au motif", sent, got,
             good);
    out.println(line);

    if (matching) {
      if (good > 0)
        out.println("   >>> LE RECEPTEUR FONCTIONNE. Les chasses deviennent interpretables.");
      else if (got > 0)
        out.println("   >>> Des trames arrivent mais deformees : cadrage ou longueur a revoir.");
      else
        out.println("   >>> RIEN. Le chemin de reception est en cause, pas l'adresse.");
    } else {
      if (got == 0)
        out.println("   >>> Le correlateur rejette bien une mauvaise adresse : selectivite OK.");
      else
        out.println("   >>> Il accepte une adresse FAUSSE : le filtrage ne marche pas.");
    }
    Serial.flush();
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Etalonnage a deux cartes
// ---------------------------------------------------------------------------

void BenqHalo::calibrationBeacon(Print &out, uint32_t seconds, uint8_t preambleBytes) {
  char line[176];
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  out.println();
  out.println("=== Balise d'etalonnage (role EMETTEUR) ===");
  snprintf(line, sizeof(line), "  Adresse sur l'air %02X %02X %02X %02X, canal %u, 125 kbps.",
           kCalAirAddr[0], kCalAirAddr[1], kCalAirAddr[2], kCalAirAddr[3],
           (unsigned)RF_CHANNEL_1);
  out.println(line);
  int n = snprintf(line, sizeof(line), "  Motif emis : ");
  for (uint8_t i = 0; i < 10; i++)
    n += snprintf(line + n, sizeof(line) - n, "%02X ", kCalPattern[i]);
  out.println(line);
  snprintf(line, sizeof(line), "  Emission toutes les 20 ms pendant %lu s.",
           (unsigned long)seconds);
  out.println(line);
  out.println("  Lance 'etalon rx' sur l'AUTRE carte pendant que ceci tourne.");
  Serial.flush();

  configForLoopback(radio, RF_CHANNEL_1, kCalRegAddr, false, dataRate_);

  // Longueur du preambule EMIS (CFO1 bit 6). Elle conditionne la chasse par le
  // preambule, qui cherche 'AA AA X' : avec un preambule d'un seul octet ce
  // motif n'existe pas sur l'air. La rendre explicite permet de tester la
  // technique sur un emetteur dont on connait deja l'adresse.
  {
    uint8_t cfo1 = radio.readRegister(B0_CFO1 | CMD_READ_REGISTER);
    cfo1 = (preambleBytes >= 2) ? (uint8_t)(cfo1 | 0x40) : (uint8_t)(cfo1 & (uint8_t)~0x40);
    radio.writeRegister(B0_CFO1 | CMD_WRITE_REGISTER, cfo1);
    const uint8_t back = radio.readRegister(B0_CFO1 | CMD_READ_REGISTER);
    snprintf(line, sizeof(line), "  Preambule : %u octet(s) demande, %u effectif (CFO1 0x%02X).",
             (unsigned)preambleBytes, (back & 0x40) ? 2u : 1u, (unsigned)back);
    out.println(line);
    // Le premier bit de l'adresse fixe la polarite du preambule (ds.txt:1409).
    snprintf(line, sizeof(line), "  Motif attendu sur l'air : %s %s %02X",
             (kCalAirAddr[0] & 0x80) ? "AA" : "55", (kCalAirAddr[0] & 0x80) ? "AA" : "55",
             kCalAirAddr[0]);
    out.println(line);
    Serial.flush();
  }

  const uint32_t deadline = millis() + seconds * 1000UL;
  uint32_t sent = 0, nextTick = millis() + 5000;

  uint32_t acked = 0, sawTxMode = 0;

  while ((int32_t)(millis() - deadline) < 0) {
    radio.command(CMD_FLUSH_TX_FIFO);
    radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
    radio.writeCommandData(CMD_WRITE_TX_FIFO_NO_ACK, kCalPattern, 10);
    radio.command(CMD_TX_MODE);
    sent++;

    // 'Ecrire dans la FIFO' ne prouve rien : seul TX_DS atteste que la puce a
    // reellement emis. Sans ce controle, une panne d'emission se lit comme une
    // panne de reception.
    bool done = false;
    for (uint16_t w = 0; w < 400 && !done; w++) {
      if (radio.operationMode() == OMST_TX) sawTxMode++;
      if (radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER) & IRQ_TX_DS) done = true;
      else delayMicroseconds(10);
    }
    if (done) acked++;

    delay(18);

    if ((int32_t)(millis() - nextTick) >= 0) {
      nextTick += 5000;
      snprintf(line, sizeof(line), "  %lu emise(s), %lu confirmee(s) par TX_DS",
               (unsigned long)sent, (unsigned long)acked);
      out.println(line);
      Serial.flush();
    }
  }

  snprintf(line, sizeof(line), "  Termine : %lu emise(s), %lu confirmee(s) par TX_DS.",
           (unsigned long)sent, (unsigned long)acked);
  out.println(line);
  snprintf(line, sizeof(line), "  Mode TX observe %lu fois ; OMST final %u, IRQ1 0x%02X.",
           (unsigned long)sawTxMode, (unsigned)radio.operationMode(),
           (unsigned)radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER));
  out.println(line);
  if (acked == 0)
    out.println("  >>> AUCUNE confirmation : la puce n'emet pas. La panne est ICI.");
  else if (acked < sent / 2)
    out.println("  >>> Emission intermittente.");
  else
    out.println("  >>> L'emetteur fonctionne : chercher la panne cote reception.");
  out.println();
}

void BenqHalo::calibrationListen(Print &out, uint32_t seconds) {
  char line[176];
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  out.println();
  out.println("=== Ecoute d'etalonnage (role RECEPTEUR) ===");
  out.println("  La balise doit deja tourner sur l'autre carte, meme adresse et");
  out.println("  meme canal. On balaie plusieurs configurations de reception :");
  out.println("  celle qui recoit designe le reglage qui manquait aux chasses.");
  Serial.flush();

  struct RxCfg {
    const char *name;
    uint8_t pkt1;    // CRC materiel
    uint8_t dpl1;    // payload dynamique, pipe 0
    uint8_t dpl2;
    uint8_t rxpw0;   // longueur statique
    uint8_t enaa;
    uint8_t readLen;
  };
  const RxCfg cfgs[6] = {
      {"CRC off, statique 13 (config des chasses)", 0x00, 0x00, 0x00, 13, 0x00, 13},
      {"CRC ON, statique 10", PKT1_CRC_ENABLE, 0x00, 0x00, 10, 0x00, 10},
      {"CRC ON, statique 13", PKT1_CRC_ENABLE, 0x00, 0x00, 13, 0x00, 13},
      {"CRC ON, payload dynamique", PKT1_CRC_ENABLE, 0x01, 0x04, 32, 0x00, 16},
      {"CRC off, statique 10", 0x00, 0x00, 0x00, 10, 0x00, 10},
      {"CRC ON, statique 10, auto-ACK actif", PKT1_CRC_ENABLE, 0x00, 0x00, 10, ENAA_ALL_PIPES, 10},
  };

  const uint32_t per = seconds;
  uint8_t winner = 0xFF;

  for (uint8_t i = 0; i < 6; i++) {
    const RxCfg &cfg = cfgs[i];

    configForLoopback(radio, RF_CHANNEL_1, kCalRegAddr, true, dataRate_);
    radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, cfg.pkt1);
    radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, cfg.dpl1);
    radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, cfg.dpl2);
    radio.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, cfg.rxpw0);
    radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, cfg.enaa);
    radio.clearInterrupts();
    radio.command(CMD_FLUSH_RX_FIFO);
    const bool inRx = radio.enterRxMode();

    uint32_t got = 0, good = 0, reArm = 0;
    bool shown = false;
    const uint32_t until = millis() + per * 1000UL;

    while ((int32_t)(millis() - until) < 0) {
      if (radio.operationMode() != OMST_RX) {
        reArm++;
        radio.enterRxMode(300);
      }
      if (radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR) {
        delayMicroseconds(200);
        continue;
      }

      uint8_t buf[32];
      radio.readFifo(buf, cfg.readLen, false);
      radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
      radio.command(CMD_FLUSH_RX_FIFO);
      got++;

      // Le motif peut commencer au premier octet ou apres le PCF selon la
      // configuration : on le cherche aux deux places.
      if (memcmp(buf, kCalPattern, 10) == 0 || memcmp(buf + 1, kCalPattern, 10) == 0) good++;

      if (!shown) {
        shown = true;
        int n = snprintf(line, sizeof(line), "     recu : ");
        for (uint8_t k = 0; k < cfg.readLen && n < 150; k++)
          n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[k]);
        out.println(line);
        Serial.flush();
      }
      if ((got & 0x1F) == 0) delay(1);
    }

    snprintf(line, sizeof(line), "   %s", cfg.name);
    out.println(line);
    snprintf(line, sizeof(line), "     RX tenu %s, %lu rearmement(s) -> %lu recue(s), %lu conforme(s)",
             inRx ? "oui" : "NON", (unsigned long)reArm, (unsigned long)got, (unsigned long)good);
    out.println(line);
    Serial.flush();

    if (good > 0 && winner == 0xFF) winner = i;
  }

  out.println();
  if (winner != 0xFF) {
    snprintf(line, sizeof(line), "  >>> CONFIGURATION QUI RECOIT : %s", cfgs[winner].name);
    out.println(line);
    out.println("  C'est ce reglage qui manquait aux commandes de chasse.");
  } else {
    out.println("  >>> Aucune configuration ne recoit. Le defaut est en amont du");
    out.println("  format de paquet : verifie d'abord que la balise confirme ses");
    out.println("  emissions par TX_DS sur l'autre carte.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Auto-test : cablage ou code ?
// ---------------------------------------------------------------------------

void BenqHalo::selfTest(Print &out) {
  char line[176];
  uint8_t failures = 0;

  // Lignes courtes : au-dela d'environ 80 caracteres le moniteur les fragmente
  // et le resultat devient illisible.
  auto verdict = [&](const char *what, bool ok, const char *detail) {
    snprintf(line, sizeof(line), "  [%s] %-34.34s %.28s", ok ? "OK " : "NON", what, detail);
    out.println(line);
    if (!ok) failures++;
    Serial.flush();
  };

  out.println();
  out.println("=== Auto-test de la carte ===");
  out.println("  Une seule carte, aucun partenaire radio. Chaque etape est");
  out.println("  verifiee par une relecture : si le bus SPI repond juste, le");
  out.println("  cablage Dupont est hors de cause.");
  out.println();

  // --- 1. le module repond-il ? ---
  // Deux lectures : un fil desserre rend une valeur differente a chaque fois,
  // et une version non nulle mais instable passerait pour un module present.
  const uint32_t ver = radio.chipVersion();
  const uint32_t ver2 = radio.chipVersion();
  const bool plausible = ver != 0x000000UL && ver != 0xFFFFFFUL && ver == ver2;
  snprintf(line, sizeof(line), "0x%06lX puis 0x%06lX", (unsigned long)ver, (unsigned long)ver2);
  verdict("module present et stable", plausible, line);
  if (!plausible) {
    out.println();
    out.println("  Le module ne repond pas de facon fiable : c'est physique.");
    out.println("  Debranche l'USB, reenfonce les six fils des DEUX cotes, et");
    out.println("  surtout GIO2 (la sortie de donnees du module) et CSN.");
    return;
  }

  // --- 2. le bus est-il fiable dans les DEUX sens ? ---
  // Un aller-retour sur un registre banal prouve SCK, SDIO, GIO2 et CSN a la
  // fois : si un seul fil etait mauvais, la valeur relue serait fausse.
  const uint8_t saved = radio.readRegister(REG_RFCH | CMD_READ_REGISTER);
  uint8_t bad = 0;
  const uint8_t probes[4] = {0x05, 0x2A, 0x55, 0x4B};
  for (uint8_t i = 0; i < 4; i++) {
    radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, probes[i]);
    if (radio.readRegister(REG_RFCH | CMD_READ_REGISTER) != probes[i]) bad++;
  }
  radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, saved);
  snprintf(line, sizeof(line), "%u motif(s) sur 4 mal relu(s)", (unsigned)bad);
  verdict("aller-retour SPI sur un registre", bad == 0, line);

  // --- 3. reglages analogiques recommandes par Holtek ---
  snprintf(line, sizeof(line), "%u registre(s) hors spec", (unsigned)radio.regCfgMismatches());
  verdict("valeurs recommandees ecrites", radio.regCfgMismatches() == 0, line);

  // --- 4. quartz et calibration ---
  const uint8_t rc1 = radio.readRegister(REG_RC1 | CMD_READ_REGISTER);
  snprintf(line, sizeof(line), "RC1 0x%02X", (unsigned)rc1);
  verdict("quartz stabilise (XCLK_RDY)", (rc1 & RC1_XCLK_RDY) != 0, line);
  verdict("calibration VCO effectuee", radio.calibrated(), radio.calibrated() ? "" : "jamais lancee");

  // --- 5. les bits de controle tiennent-ils ? ---
  radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x01);
  const uint8_t ce = radio.readRegister(REG_CE | CMD_READ_REGISTER);
  snprintf(line, sizeof(line), "CE relu 0x%02X", (unsigned)ce);
  verdict("CE accepte la valeur 1", (ce & 0x01) != 0, line);

  // --- 6. la machine d'etats suit-elle les commandes ? ---
  struct ModeTest {
    const char *name;
    uint8_t cmd;
    uint8_t expect;
  };
  // Pas de test du Standby : la commande 0x0D ne figure pas dans la table des
  // commandes du datasheet, son echec ne signalerait aucun defaut.
  const ModeTest modes[1] = {
      {"passage en Light Sleep", CMD_LIGHT_SLEEP, OMST_LIGHT_SLEEP},
  };
  for (uint8_t i = 0; i < 1; i++) {
    radio.command(modes[i].cmd);
    uint8_t got = 0;
    for (uint8_t w = 0; w < 40; w++) {
      got = radio.operationMode();
      if (got == modes[i].expect) break;
      delayMicroseconds(100);
    }
    snprintf(line, sizeof(line), "OMST %u, attendu %u", (unsigned)got, (unsigned)modes[i].expect);
    verdict(modes[i].name, got == modes[i].expect, line);
  }

  // L'entree en RX passe par la sequence eprouvee, pas par une commande brute :
  // sans PRM_RX pose, la puce ne peut pas devenir recepteur, et apres une
  // coupure d'alimentation ce bit revient a zero. Un test qui l'ignore mesure
  // un etat herite de la commande precedente.
  configForLoopback(radio, channel_, kCalRegAddr, true, dataRate_);
  const bool inRx = radio.enterRxMode();
  snprintf(line, sizeof(line), "OMST %u, attendu 5", (unsigned)radio.operationMode());
  verdict("passage en RX", inRx, line);

  // --- 7. LE POINT CRITIQUE : la FIFO d'emission se remplit-elle ? ---
  out.println();
  out.println("  --- remplissage de la FIFO d'emission ---");
  out.println("  STATUS bit 4 = TX_EMPTY. S'il reste a 1 apres l'ecriture, la");
  out.println("  commande d'ecriture n'a pas ete acceptee par la puce.");

  // Meme logique : on repart de la configuration d'emission qui a reellement
  // transmis 9473 trames sur 9473, au lieu d'un assemblage de registres isoles.
  configForLoopback(radio, channel_, kCalRegAddr, false, dataRate_);

  struct FifoTest {
    const char *name;
    uint8_t dpl2;
    uint8_t writeCmd;
  };
  // La premiere ligne est un TEMOIN : elle doit echouer. C'est elle qui
  // documente que la commande d'ecriture sans auto-ACK exige EN_DYN_ACK
  // (ds.txt:869). La compter comme un defaut ferait croire a une panne.
  const FifoTest fifos[4] = {
      {"temoin EN_DYN_ACK=0 (doit echouer)", 0x00, CMD_WRITE_TX_FIFO_NO_ACK},
      {"EN_DYN_ACK=1, ecriture sans auto-ACK", 0x01, CMD_WRITE_TX_FIFO_NO_ACK},
      {"EN_DYN_ACK=0, ecriture avec auto-ACK", 0x00, CMD_WRITE_TX_FIFO_WITH_ACK},
      {"EN_DYN_ACK=1, ecriture avec auto-ACK", 0x01, CMD_WRITE_TX_FIFO_WITH_ACK},
  };
  int8_t working = -1;

  for (uint8_t i = 0; i < 4; i++) {
    radio.command(CMD_LIGHT_SLEEP);
    radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, fifos[i].dpl2);
    radio.command(CMD_FLUSH_TX_FIFO);
    radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);

    const uint8_t before = radio.readRegister(REG_STATUS | CMD_READ_REGISTER);
    radio.writeCommandData(fifos[i].writeCmd, kCalPattern, 10);
    const uint8_t after = radio.readRegister(REG_STATUS | CMD_READ_REGISTER);
    const bool filled = (after & STATUS_TX_FIFO_EMPTY) == 0;

    snprintf(line, sizeof(line), "STATUS %02X -> %02X", (unsigned)before, (unsigned)after);
    if (i == 0) {
      // Temoin : l'echec est le resultat attendu, il ne compte pas.
      snprintf(line, sizeof(line), "  [%s] %-34.34s STATUS %02X -> %02X",
               filled ? "?? " : "att", fifos[i].name, (unsigned)before, (unsigned)after);
      out.println(line);
      Serial.flush();
    } else {
      verdict(fifos[i].name, filled, line);
    }

    if (filled && working < 0) {
      working = (int8_t)i;
      // La FIFO est pleine et CE est pose : la puce doit partir en TX seule.
      radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x01);
      uint8_t omst = 0, irq = 0;
      bool sawTx = false, sawDone = false;
      for (uint16_t w = 0; w < 600; w++) {
        omst = radio.operationMode();
        if (omst == OMST_TX) sawTx = true;
        irq = radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
        if (irq & IRQ_TX_DS) {
          sawDone = true;
          break;
        }
        delayMicroseconds(10);
      }
      snprintf(line, sizeof(line), "TX %s, IRQ1 %02X, OMST %u", sawTx ? "vu" : "jamais",
               (unsigned)irq, (unsigned)omst);
      verdict("emission reellement effectuee (TX_DS)", sawDone, line);
    }
    radio.command(CMD_FLUSH_TX_FIFO);
  }

  out.println();
  if (working >= 0) {
    snprintf(line, sizeof(line), "  La FIFO se remplit avec : %s", fifos[working].name);
    out.println(line);
  } else {
    out.println("  AUCUNE des quatre ecritures ne remplit la FIFO.");
    out.println("  Comme l'aller-retour SPI est bon, ce n'est pas le cablage :");
    out.println("  c'est l'opcode d'ecriture ou un prerequis de configuration.");
  }

  snprintf(line, sizeof(line), "  %u echec(s) au total.", (unsigned)failures);
  out.println(line);
  out.println();
}

const char *BenqHalo::dataRateName(uint8_t rate) {
  if (rate == DATARATE_250K) return "250 kbps";
  if (rate == DATARATE_500K) return "500 kbps";
  return "125 kbps";
}

void BenqHalo::setDataRate(uint8_t rate) {
  dataRate_ = rate;
  saveConfig();
}

// ---------------------------------------------------------------------------
//  Ecoute passive d'un bus SPI tiers
// ---------------------------------------------------------------------------

void BenqHalo::sniffSpiBus(Print &out, uint32_t seconds) {
  char line[176];

  out.println();
  out.println("=== Ecoute du bus SPI de la telecommande ===");
  out.println("  Le peripherique SPI est mis en ESCLAVE : c'est l'horloge de la");
  out.println("  telecommande qui le cadence, donc les octets sont reconstitues");
  out.println("  exactement au lieu d'etre echantillonnes.");
  out.println("  MISO n'est pas assigne : cette carte n'emet RIEN sur le bus");
  out.println("  observe, elle se contente d'ecouter.");
  snprintf(line, sizeof(line), "  Branchements : SCK -> IO%u, SDIO -> IO%u, CSN -> IO%u,",
           (unsigned)PIN_TAP_SCK, (unsigned)PIN_TAP_MOSI, (unsigned)PIN_TAP_CS);
  out.println(line);
  out.println("  et une MASSE COMMUNE, indispensable.");
  out.println("  On guette la commande 0x10 (write PTX address) suivie des");
  out.println("  octets d'adresse.");
  snprintf(line, sizeof(line), "  Duree : %lu s.", (unsigned long)seconds);
  out.println(line);
  out.println("  Une ligne d'etat par seconde : ajuste les fils en la regardant.");
  out.println("  Au repos on attend CSN proche de 100%h et SCK proche de 0%h.");
  out.println("  CSN a 0%h = son fil touche la masse ou une pastille voisine.");
  Serial.flush();

  // Liberer le bus maitre : le C6 n'a qu'un seul peripherique SPI utilisable.
  radio.suspendBus();

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_TAP_MOSI;
  bus.miso_io_num = -1;  // jamais pilote
  bus.sclk_io_num = PIN_TAP_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 64;

  // Plusieurs transactions pre-armees : une initialisation de BC5602 est une
  // rafale de dizaines d'echanges colles les uns aux autres. Avec une seule
  // transaction armee a la fois, on attrape la premiere et on dort pendant
  // toute la suite.
  constexpr uint8_t kQueue = 6;
  constexpr uint16_t kMaxStored = 400;
  constexpr uint8_t kKeepBytes = 32;  // la transaction entiere, sans troncature

  spi_slave_interface_config_t slave = {};
  slave.spics_io_num = PIN_TAP_CS;
  slave.flags = 0;
  slave.queue_size = kQueue;
  slave.mode = 0;  // comme le BC5602 : CPOL=0, CPHA=0

  if (spi_slave_initialize(SPI2_HOST, &bus, &slave, SPI_DMA_CH_AUTO) != ESP_OK) {
    out.println("  Impossible d'initialiser le SPI en esclave.");
    radio.resumeBus();
    return;
  }

  static WORD_ALIGNED_ATTR uint8_t pool[kQueue][32];
  static spi_slave_transaction_t descs[kQueue];
  static uint8_t store[kMaxStored][kKeepBytes];
  static uint8_t storeLen[kMaxStored];

  uint8_t armed = 0;
  for (uint8_t i = 0; i < kQueue; i++) {
    descs[i] = {};
    descs[i].length = 8 * 32;
    descs[i].rx_buffer = pool[i];
    descs[i].tx_buffer = nullptr;
    if (spi_slave_queue_trans(SPI2_HOST, &descs[i], portMAX_DELAY) == ESP_OK) armed++;
  }

  uint32_t frames = 0, addressWrites = 0, noise = 0, stored = 0, dropped = 0;
  const uint32_t deadline = millis() + seconds * 1000UL;

  // Une ligne d'etat par seconde, et RIEN d'autre : afficher chaque
  // transaction rendrait la carte sourde pendant l'essentiel de la rafale.
  // Les niveaux sont lus a meme le registre GPIO, sans prendre le controle des
  // broches -- le peripherique SPI continue de les utiliser normalement.
  const uint32_t maskCs = 1UL << PIN_TAP_CS;
  const uint32_t maskSck = 1UL << PIN_TAP_SCK;
  const uint32_t maskSdio = 1UL << PIN_TAP_MOSI;
  uint32_t nextStatus = millis() + 1000;
  const uint32_t started = millis();

  out.println("  temps   CSN    SCK    SDIO   |  trames  bruit  gardees");
  Serial.flush();

  while ((int32_t)(millis() - deadline) < 0) {
    if ((int32_t)(millis() - nextStatus) >= 0) {
      nextStatus += 1000;
      uint16_t hiCs = 0, hiSck = 0, hiSdio = 0;
      for (uint16_t k = 0; k < 200; k++) {
        const uint32_t g = REG_READ(GPIO_IN_REG);
        if (g & maskCs) hiCs++;
        if (g & maskSck) hiSck++;
        if (g & maskSdio) hiSdio++;
      }
      snprintf(line, sizeof(line),
               "  %3lus  %3u%%h  %3u%%h  %3u%%h  |  %5lu  %5lu  %5lu",
               (unsigned long)((millis() - started) / 1000), (unsigned)(hiCs / 2),
               (unsigned)(hiSck / 2), (unsigned)(hiSdio / 2), (unsigned long)frames,
               (unsigned long)noise, (unsigned long)stored);
      // Sans flush : l'ecriture part dans le tampon et rend la main tout de
      // suite, la carte ne devient pas sourde.
      out.println(line);
    }

    spi_slave_transaction_t *done = nullptr;
    if (spi_slave_get_trans_result(SPI2_HOST, &done, pdMS_TO_TICKS(200)) != ESP_OK) continue;

    const uint32_t bits = done->trans_len;
    const uint8_t bytes = (bits >= 8) ? (uint8_t)(bits / 8) : 0;
    if (bytes >= 2) {
      frames++;
      const uint8_t *b = (const uint8_t *)done->rx_buffer;

      // Le tri se fait ICI, pas a l'affichage : sinon le tampon se remplit de
      // bruit dans les premieres secondes et la rafale de demarrage, qui
      // arrive ensuite, est jetee faute de place. Un contact intermittent
      // produit des transactions uniformes, ou des suites de uns puis de
      // zeros -- signature d'un registre a decalage cadence au hasard. Une
      // vraie commande comme 0x10 suivie d'octets varies n'est jamais ecartee.
      bool uniform = true;
      for (uint8_t i = 1; i < bytes; i++)
        if (b[i] != b[0]) {
          uniform = false;
          break;
        }
      bool tailAllZero = true;
      for (uint8_t i = 1; i < bytes; i++)
        if (b[i] != 0x00) {
          tailAllZero = false;
          break;
        }
      const bool leadingOnes = tailAllZero && (b[0] == 0x00 || (uint8_t)(b[0] + 1) == 0x00 ||
                                               (b[0] & (uint8_t)(b[0] + 1)) == 0);

      if (uniform || leadingOnes) {
        noise++;
      } else if (stored < kMaxStored) {
        const uint8_t keep = (bytes < kKeepBytes) ? bytes : kKeepBytes;
        memcpy(store[stored], b, keep);
        storeLen[stored] = keep;
        stored++;
      } else {
        dropped++;
      }
    }
    done->trans_len = 0;
    // Le rearmement peut echouer si la file est pleine ; l'ignorer viderait
    // silencieusement la file et rendrait la carte sourde pour la suite.
    if (spi_slave_queue_trans(SPI2_HOST, done, 0) != ESP_OK) armed--;
    if (armed == 0) break;
  }

  // L'affichage vient AVANT le demontage : un incident en liberant le
  // peripherique ne doit jamais emporter une capture reussie.
  // Restitution, une fois la capture terminee.
  for (uint16_t k = 0; k < stored; k++) {
    const uint8_t *b = store[k];
    const uint8_t bytes = storeLen[k];

    bool tailAllZero = true;
    for (uint8_t i = 1; i < bytes; i++)
      if (b[i] != 0x00) {
        tailAllZero = false;
        break;
      }

    const bool isAddress = (b[0] == 0x10 && bytes >= 5 && !tailAllZero);
    if (isAddress) addressWrites++;

    // Seize octets par ligne : au-dela, le moniteur fragmente et l'hexa
    // devient illisible. Et surtout, plus de troncature -- c'est elle qui
    // masquait le contenu discriminant au-dela du vingtieme octet.
    for (uint8_t off = 0; off < bytes; off += 16) {
      int n = (off == 0) ? snprintf(line, sizeof(line), "  %3u:", (unsigned)(k + 1))
                         : snprintf(line, sizeof(line), "      ");
      for (uint8_t i = off; i < bytes && i < (uint8_t)(off + 16); i++)
        n += snprintf(line + n, sizeof(line) - n, " %02X", b[i]);
      if (off == 0 && isAddress) snprintf(line + n, sizeof(line) - n, "   <<< ADRESSE");
      out.println(line);
    }
    Serial.flush();
  }

  // Vider la file avant de liberer : liberer le pilote avec des transactions
  // encore armees lui fait relacher des descripteurs DMA toujours en usage,
  // ce qui provoque une exception (Load access fault a l'adresse 0x70).
  for (;;) {
    spi_slave_transaction_t *leftover = nullptr;
    if (spi_slave_get_trans_result(SPI2_HOST, &leftover, pdMS_TO_TICKS(50)) != ESP_OK) break;
  }
  spi_slave_free(SPI2_HOST);
  radio.resumeBus();

  out.println();
  snprintf(line, sizeof(line), "  Termine : %lu transaction(s), %lu de bruit ecartee(s),",
           (unsigned long)frames, (unsigned long)noise);
  out.println(line);
  snprintf(line, sizeof(line), "  %lu ecriture(s) d'adresse plausible(s), %lu perdue(s) faute de place.",
           (unsigned long)addressWrites, (unsigned long)dropped);
  out.println(line);
  if (frames == 0) {
    out.println("  Rien capte. Verifie la masse commune, puis lance 'taptest'");
    out.println("  pendant que les fils sont en place.");
  } else if (noise == frames) {
    out.println("  Tout etait du bruit : le contact n'a pas tenu. Relance");
    out.println("  'taptest' sans rien deplacer.");
  }
  out.println();
}

void BenqHalo::tapTest(Print &out, uint32_t seconds) {
  char line[176];

  out.println();
  out.println("=== Le contact tient-il ? (suivi en direct) ===");
  out.println("  Ajuste les fils en regardant cette sortie. Au repos, pile en");
  out.println("  place, CSN doit etre HAUTE, SCK BASSE. Une ligne qui suit nos");
  out.println("  resistances internes est une ligne qui flotte.");
  snprintf(line, sizeof(line), "  Suivi pendant %lu s, une ligne par seconde.",
           (unsigned long)seconds);
  out.println(line);
  out.println();
  Serial.flush();

  radio.suspendBus();

  const uint8_t pins[3] = {(uint8_t)PIN_TAP_CS, (uint8_t)PIN_TAP_SCK, (uint8_t)PIN_TAP_MOSI};
  const char *const names[3] = {"CSN", "SCK", "SDIO"};

  const uint32_t deadline = millis() + seconds * 1000UL;
  uint32_t allGood = 0, rounds = 0;

  while ((int32_t)(millis() - deadline) < 0) {
    int n = snprintf(line, sizeof(line), " ");
    uint8_t good = 0;

    for (uint8_t i = 0; i < 3; i++) {
      uint8_t pd = 0, pu = 0;
      pinLevels(pins[i], pd, pu);
      const int delta = (int)pu - (int)pd;
      const bool driven = (delta < 30);
      const bool high = (pu > 50);

      // CSN au repos doit etre HAUTE. Sans cette exigence, une telecommande
      // non alimentee -- toutes lignes tirees vers la masse -- passerait pour
      // un montage correct.
      const char *state;
      if (!driven) state = "flotte";
      else if (i == 0 && !high) state = "BAS !";
      else state = high ? "haut" : "bas";

      const bool ok = driven && (i != 0 || high);
      if (ok) good++;
      n += snprintf(line + n, sizeof(line) - n, "  %-4s %-6s %s", names[i], state,
                    ok ? "ok" : "NON");
    }

    rounds++;
    if (good == 3) {
      allGood++;
      snprintf(line + n, sizeof(line) - n, "   <<< LES TROIS TIENNENT");
    }
    out.println(line);
    Serial.flush();
    delay(900);
  }

  radio.resumeBus();

  out.println();
  snprintf(line, sizeof(line), "  %lu seconde(s) sur %lu avec les trois contacts.",
           (unsigned long)allGood, (unsigned long)rounds);
  out.println(line);
  if (allGood == rounds && rounds > 0) {
    out.println("  Contact stable : enchaine 'sniffspi 30' sans rien bouger.");
  } else if (allGood == 0) {
    out.println("  Jamais les trois en meme temps. Reprends la ligne marquee NON");
    out.println("  le plus souvent ; si CSN affiche 'BAS !', son fil touche");
    out.println("  probablement la masse ou une pastille voisine.");
  } else {
    out.println("  Contact intermittent : cale mieux le fil qui decroche avant");
    out.println("  de lancer la capture.");
  }
  out.println();
}
