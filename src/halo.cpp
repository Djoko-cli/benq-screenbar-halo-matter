#include "halo.h"

#include <string.h>

#include "driver/spi_slave.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

using namespace bc5602;

// Defini plus bas, pres de configForLoopback ; utilise des sharedRadioConfig.
static void applyXoTrim(BC5602 &r);

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

  // Reappliquer les reglages analogiques recommandes par Holtek. Mesure a
  // l'appui (commande 'survie') : le reset logiciel en efface 15 sur 19, et
  // comme toute configuration commence par un reset, ils n'ont JAMAIS ete
  // actifs pendant une ecoute. Ce sont pourtant les reglages du modem --
  // filtre de canal, LNA, PLL -- donc exactement ce qui decide si un signal
  // se demodule.
  if (applyHoltekTuning_) radio.registerConfigure(nullptr);
  applyXoTrim(radio);

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
#ifdef DIAG_ONLY
  // Build de diagnostic : AUCUNE activite radio de fond. Mesure a l'appui
  // (audit du 23/09) : en mode normal, cette boucle appelait pollNow() toutes
  // les 5 s, qui EMET une trame vers l'adresse enregistree -- elle a tourne
  // toute la nuit en arriere-plan des mesures, et emettait vers la lampe sans
  // qu'on l'ait demande. En diagnostic, la radio ne bouge que sur commande.
  return;
#endif
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
    out.println("  Il reste GIO3, broche 8 du module, desormais cablee sur IO3 : c'est la");
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
// Le payload de la balise porte deliberement un 0x55 en position 2. Le
// detecteur de preambule s'arme sur une suite alternee : un octet 0x55 ou 0xAA
// place juste avant une fenetre de trois octets permet au correlateur de s'y
// caler en PLEIN PAYLOAD. C'est exactement le mecanisme qu'exploite le script
// de kuzmin, dont le commentaire dit "used as preambule and part of the sync
// word" a propos du 0x55 issu d'une temperature de 3925 K.
// L'ancien motif DE AD BE EF ... n'avait aucune suite alternee de plus de sept
// bits : le test de validation ne pouvait pas accrocher, et son echec ne
// prouvait rien.
static const uint8_t kCalPattern[10] = {0xDE, 0xAD, 0x55, 0x0F, 0xA0, 0x3C, 0x01, 0x02, 0x03, 0x04};

// Configure une puce pour la boucle locale. Volontairement identique a ce que
// font les commandes de chasse -- CRC materiel desactive en reception, payload
// statique, pas d'auto-ACK -- car etalonner une autre configuration que celle
// qu'on utilise ne prouverait rien.
static bool gApplyHoltekTuning = true;

// Trim du quartz (XO1, banque 0, bits 4-0), reapplique apres chaque reset
// logiciel -- sans quoi le reset le ramene a 0x10 et la mesure porte sur autre
// chose que ce qu'on croit regler. -1 = ne pas toucher.
static int16_t gXoTrim = -1;

static void applyXoTrim(BC5602 &r) {
  if (gXoTrim < 0) return;
  const uint8_t saved = r.bank();
  r.setBank(0);
  const uint8_t xo = r.readRegister(B0_XO1 | CMD_READ_REGISTER);
  r.writeRegister(B0_XO1 | CMD_WRITE_REGISTER, (uint8_t)((xo & 0xE0) | (gXoTrim & 0x1F)));
  r.setBank(saved);
}

void BenqHalo::setXoTrim(int16_t trim) {
  gXoTrim = (trim >= 0 && trim <= 31) ? trim : -1;
  applyXoTrim(radio);
}



static void configForLoopback(BC5602 &r, uint8_t channel, const uint8_t addr[4], bool receiver,
                              uint8_t rate) {
  r.softwareReset();
  delay(20);
  r.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
  // Le reset vient d'effacer les reglages analogiques : les remettre ici, sans
  // quoi toute la mesure tourne sur les valeurs d'usine.
  if (gApplyHoltekTuning) r.registerConfigure(nullptr);
  applyXoTrim(r);
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
  snprintf(line, sizeof(line), "  Adresse sur l'air %02X %02X %02X %02X, canal %u, %s.",
           kCalAirAddr[0], kCalAirAddr[1], kCalAirAddr[2], kCalAirAddr[3],
           (unsigned)RF_CHANNEL_1, dataRateName(dataRate_));
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

void BenqHalo::setHoltekTuning(bool on) {
  applyHoltekTuning_ = on;
  gApplyHoltekTuning = on;
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

// ---------------------------------------------------------------------------
//  Le correlateur sait-il se caler au milieu d'une trame ?
// ---------------------------------------------------------------------------

void BenqHalo::validatePayloadSync(Print &out, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];

  out.println();
  out.println("=== Accrochage sur le payload : le procede marche-t-il ? ===");
  out.println("  La commande 'find' donne au correlateur trois octets pris dans");
  out.println("  le payload, en esperant qu'il s'y cale et livre la suite. Ce");
  out.println("  principe n'a jamais ete verifie -- seulement lance contre la");
  out.println("  lampe, sans jamais savoir s'il pouvait fonctionner.");
  out.println();
  out.println("  Ici on l'essaie contre la balise, dont on connait le payload :");
  int n = snprintf(line, sizeof(line), "    ");
  for (uint8_t i = 0; i < 10; i++)
    n += snprintf(line + n, sizeof(line) - n, "%02X ", kCalPattern[i]);
  out.println(line);
  out.println("  Une seule de ces fenetres est ANCREE : celle precedee du 0x55.");
  out.println("  Le detecteur de preambule s'arme sur une suite alternee, donc");
  out.println("  seule une fenetre precedee de 0x55 ou 0xAA peut accrocher. Les");
  out.println("  sept autres servent de temoins negatifs.");
  out.println("  >>> Il faut une source qui emette : la balise sur l'autre carte,");
  out.println("      ou la molette de la telecommande tournee sans arret.");
  Serial.flush();

  uint8_t hits = 0;

  for (uint8_t off = 0; off + 2 < 10; off++) {
    // Sur l'air les octets defilent dans l'ordre du payload ; le registre se
    // remplit a l'envers.
    const uint8_t reg3[3] = {kCalPattern[off + 2], kCalPattern[off + 1], kCalPattern[off]};

    channel_ = RF_CHANNEL_1;
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

    uint32_t got = 0;
    bool shown = false;
    const uint32_t until = millis() + dwellMs;

    while ((int32_t)(millis() - until) < 0) {
      if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
      if (radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR) {
        delayMicroseconds(200);
        continue;
      }

      uint8_t buf[32];
      radio.readFifo(buf, 32, false);
      radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
      radio.command(CMD_FLUSH_RX_FIFO);
      got++;

      if (!shown) {
        shown = true;
        int k = snprintf(line, sizeof(line), "     recu : ");
        for (uint8_t i = 0; i < 12; i++)
          k += snprintf(line + k, sizeof(line) - k, "%02X ", buf[i]);
        out.println(line);
      }
      if ((got & 0x1F) == 0) delay(1);
    }

    // Une fenetre n'a de chance d'accrocher que si l'octet qui la precede
    // ressemble a un preambule.
    const bool anchored = (off > 0) && (kCalPattern[off - 1] == 0x55 || kCalPattern[off - 1] == 0xAA);
    snprintf(line, sizeof(line), "  octets %u-%u  (%02X %02X %02X)%s : %lu trame(s)%s",
             (unsigned)off, (unsigned)(off + 2), kCalPattern[off], kCalPattern[off + 1],
             kCalPattern[off + 2], anchored ? " ANCREE" : "       ", (unsigned long)got,
             got ? "   <<< ACCROCHE" : "");
    out.println(line);
    Serial.flush();
    if (got) hits++;
  }

  out.println();
  if (hits) {
    out.println("  Le correlateur SAIT se caler en plein payload, pourvu qu'un");
    out.println("  octet d'ancrage le precede. Le procede de 'find' est donc");
    out.println("  valide : il n'a jamais echoue que faute d'ancre ou de debit.");
  } else {
    out.println("  Meme la fenetre ancree n'accroche pas. Verifie d'abord que la");
    out.println("  balise tourne et que les deux cartes sont au meme debit, car");
    out.println("  sans trafic ce resultat ne prouverait rien.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Les fonctions cachees de GIO3
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  Le fil GIO3 fait-il contact ? Test purement electrique, sans la radio.
//  On tire la broche vers le haut puis vers le bas avec les resistances
//  internes de l'ESP32 (~45 kOhm). Si rien n'est branche, la broche suit
//  docilement les deux. Si la pastille du module la pilote, elle resiste a au
//  moins une des deux tractions. Ce test ne peut pas etre trompe par l'absence
//  de signal radio, contrairement a un comptage de fronts.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Ce qu'on entend sur le canal 5, est-ce la telecommande ou le Wi-Fi ?
//  Le canal 5 (2405 MHz) tombe dans le Wi-Fi 1, large de 20 MHz (2401-2423).
//  Un emetteur Wi-Fi depose donc autant d'energie a 2420 qu'a 2405. La
//  telecommande, elle, ne fait que 0,43 MHz de large (dossier FCC) : elle ne
//  peut etre qu'a UN de ces deux endroits.
//    canal  5 = 2405 MHz : cible presumee, dans le Wi-Fi 1
//    canal 20 = 2420 MHz : dans le Wi-Fi 1, hors de la cible
//    canal 78 = 2478 MHz : hors de tout canal Wi-Fi, bruit de fond
//  On alterne les trois toutes les quelques millisecondes : une rafale Wi-Fi
//  ne peut pas favoriser l'un plutot que l'autre a cette echelle de temps.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Polarite et longueur du preambule : les deux dimensions que les sondes GIO3
//  n'avaient jamais balayees. configForLoopback ecrit une adresse fixe et ne
//  touche pas a CFO1, or le BC5602 deduit la POLARITE du preambule du premier
//  bit d'adresse emis (ds.txt:1414) : un 0 donne 01010101, un 1 donne 10101010.
//  L'adresse est ecrite a l'envers de l'ordre sur l'air, donc c'est le DERNIER
//  octet du tableau qui part en premier. Toutes nos chasses ont donc tourne
//  avec une seule des deux polarites, et une seule des deux longueurs.
//  L'adresse elle-meme n'importe pas ici : GIO3S=14 s'anime des la detection du
//  preambule, avant toute comparaison d'adresse.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  La sequence de reception du projet amont, reproduite a l'identique.
//  Termina1/benq-screenbar-halo2-esphome, fonction prepare_halo_receive().
//  Deux differences de fond avec tout ce qu'on a essaye jusqu'ici :
//
//   1. AUCUN RESET LOGICIEL. Son commentaire est explicite : "Literal Pico
//      lifecycle: no software reset during normal initialization. Hidden
//      packet/PID/RF state is allowed to continue from hardware POR." Nos deux
//      chemins de configuration commencent au contraire par un reset -- et on a
//      mesure ce matin (commande 'survie') qu'il efface 15 des 19 valeurs
//      recommandees Holtek. Sans reset, celles ecrites par begin() survivent.
//   2. Reception PASSIVE : CRC desactive, auto-ACK desactive, payload
//      dynamique desactive, longueur statique de 13 octets. Une trame entre
//      dans la FIFO meme si son CRC est faux.
//
//  Le temoin de trafic reste GIO3S=14, prouve en amont du correlateur.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Reception Halo 1, structure de trame etablie.
//
//  Trame : adresse 4 octets + charge utile 6 octets + CRC-16/CCITT (0x1021,
//  init 0xFFFF) couvrant l'adresse ET la charge utile. Etabli le 2026-09-22 sur
//  une trame recue a la fois par le CC2500 en flux brut et par le moteur de
//  paquets du BM5602 : 06 B9 21 BB 98 FF suivi de 7A FF.
//
//  Le Halo 2 utilise dix octets de charge utile ; le Halo 1 en utilise six.
//  C'est pourquoi les lectures a treize octets debordaient sur la retransmission
//  suivante -- on y lisait son preambule et son adresse, decales d'un bit.
//
//  Le CRC est verifie PAR LE MATERIEL : toute trame rendue ici est exacte, ce
//  qui evite d'avoir a filtrer les erreurs binaires en logiciel.
// ---------------------------------------------------------------------------
void BenqHalo::listenHalo1(Print &out, uint32_t dwellMs, uint8_t rxLen) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }
  char line[176];
  out.println();
  out.println("=== Reception Halo 1 avec vote majoritaire ===");
  snprintf(line, sizeof(line), "  Canal %u, %s, adresse %02X %02X %02X %02X, lecture de %u octets.",
           (unsigned)channel_, dataRateName(dataRate_), addr_[0], addr_[1], addr_[2], addr_[3],
           (unsigned)rxLen);
  out.println(line);
  out.println("  La telecommande RETRANSMET chaque trame plusieurs fois d'affilee.");
  out.println("  On lit donc 32 octets d'un coup -- le maximum de la FIFO -- pour");
  out.println("  capturer la premiere copie ET les suivantes sans rearmer entre");
  out.println("  elles, puis on les retrouve en cherchant l'adresse bit a bit.");
  out.println("  Les erreurs binaires ne tombant pas");
  out.println("  au meme endroit d'une copie a l'autre, un vote bit a bit sur");
  out.println("  les copies d'une meme rafale reconstitue la trame exacte --");
  out.println("  sans gagner un seul decibel.");
  out.println("  >>> AGIS SUR LA TELECOMMANDE : molette, boutons, allumage.");
  out.println();
  Serial.flush();

  // Chemin sans reset logiciel : le reset efface 15 des 19 valeurs recommandees.
  // Valeurs analogiques : recommandees Holtek (ecrites par begin()), ou valeurs
  // par defaut de la puce. Le projet Termina1, le seul qui recoive une vraie
  // telecommande BenQ, ne fait ni reset ni ecriture de ces valeurs : il tourne
  // sur les valeurs de mise sous tension. Le reset logiciel est ce qui s'en
  // approche le plus -- on ne peut pas couper l'alimentation du module.
  if (!applyHoltekTuning_) {
    radio.softwareReset();
    out.println("  Valeurs analogiques PAR DEFAUT (reset logiciel, rien de reecrit).");
  } else {
    // Reecrites ICI et non laissees a begin() : une passe precedente en mode
    // par defaut les a effacees par son reset, et l'alternance serait faussee.
    radio.registerConfigure(nullptr);
    out.println("  Valeurs analogiques RECOMMANDEES Holtek (reecrites a l'instant).");
  }
  radio.command(CMD_LIGHT_SLEEP);
  radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
  applyXoTrim(radio);
  radio.setBank(0);
  radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, channel_);
  radio.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, (uint8_t)(ADDR_LEN_4 | dataRate_));
  radio.writeCommandData(CMD_WRITE_PTX_ADDRESS, addr_, 4);
  uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
  radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask | MASK_PRM_RX));
  radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
  radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x00);
  // 8 octets : 6 de charge utile plus les 2 du CRC, qui passent dans la FIFO
  // quand le CRC materiel est desactive. Mesure a l'appui : avec le CRC
  // materiel actif la puce rejette TOUTES les trames alors que le modele
  // logiciel en valide -- son moteur de paquets ne couvre donc pas le meme
  // champ. On verifie nous-memes, ce qui laisse en outre voir les rejets.
  // 32 octets, le maximum de la FIFO. Mesure a l'appui : la puce accroche la
  // PREMIERE trame d'une rafale, et pendant qu'on la lit et qu'on rearme, les
  // retransmissions passent -- elles se suivent a quelques centaines de
  // microsecondes. En lisant large, on capture la premiere trame ET les
  // suivantes dans la meme lecture, sans aucun rearmement entre elles. On les
  // retrouve ensuite en cherchant l'adresse dans le flux de bits.
  // Longueur de lecture pilotable, pour un test A/B : une trame a valide son
  // CRC en lecture courte, aucune en lecture de 32 octets. Hypothese a
  // verifier, pas un fait : les deux trames comparees n'etaient peut-etre pas
  // la meme commande.
  if (rxLen < 8) rxLen = 8;
  if (rxLen > 32) rxLen = 32;
  radio.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, rxLen);
  radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);
  radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);
  radio.clearInterrupts();
  radio.command(CMD_FLUSH_RX_FIFO);
  radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, 0x40);
  radio.command(CMD_RX_MODE);

  uint8_t total = 0;
  const uint8_t regOk = radio.registerVerify(&total);
  radio.setBank(0);
  snprintf(line, sizeof(line), "  Reglages analogiques en place : %u sur %u. OMST %u.", regOk, total,
           (unsigned)radio.operationMode());
  out.println(line);
  Serial.flush();

  static uint8_t group[12][8];
  uint8_t nGroup = 0;
  uint32_t lastFrameMs = 0;
  uint32_t seen = 0, exact = 0, repaired = 0, groups = 0, strong = 0, checks = 0;

  const uint32_t until = millis() + dwellMs;
  uint32_t spin = 0;
  while ((int32_t)(millis() - until) < 0) {
    // Boucle serree : les retransmissions se suivent de pres, et toute lecture
    // superflue entre deux armements en fait rater.
    const uint8_t irq = radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
    if (irq & IRQ_RX_DR) {
      uint8_t buf[32];
      memset(buf, 0, sizeof(buf));
      radio.readFifo(buf, rxLen, false);
      radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, 0x40);
      radio.command(CMD_FLUSH_RX_FIFO);
      radio.command(CMD_RX_MODE);

      // Vidage brut des 32 octets vers l'ordinateur. L'analyse embarquee
      // suppose une charge utile de 6 octets ; rien ne garantit qu'elle soit
      // constante, et sur le Mac on peut essayer toutes les longueurs sans
      // refaire la manipulation.
      {
        char raw[100];
        size_t w = (size_t)snprintf(raw, sizeof(raw), "BRUT ");
        for (uint8_t k = 0; k < rxLen; k++)
          w += (size_t)snprintf(raw + w, sizeof(raw) - w, "%02X", buf[k]);
        out.println(raw);
        Serial.flush();
      }

      // La premiere copie commence juste apres l'adresse, deja consommee par
      // le correlateur.
      nGroup = 0;
      memcpy(group[nGroup++], buf, 8);
      seen++;

      // Les copies suivantes sont quelque part dans les 24 octets restants,
      // precedees de leur propre adresse. On la cherche bit a bit : rien ne
      // garantit qu'une retransmission tombe sur une frontiere d'octet.
      const uint8_t air[4] = {addr_[3], addr_[2], addr_[1], addr_[0]};
      for (uint16_t bit = 64; bit + 32 + 64 <= (uint16_t)rxLen * 8 && nGroup < 12;) {
        bool match = true;
        for (uint8_t k = 0; k < 32 && match; k++) {
          const uint16_t b = bit + k;
          const uint8_t got = (buf[b >> 3] >> (7 - (b & 7))) & 1;
          const uint8_t want = (air[k >> 3] >> (7 - (k & 7))) & 1;
          if (got != want) match = false;
        }
        if (!match) {
          bit++;
          continue;
        }
        uint8_t copy[8];
        for (uint8_t q = 0; q < 8; q++) {
          uint8_t v = 0;
          for (uint8_t k = 0; k < 8; k++) {
            const uint16_t b = bit + 32 + q * 8 + k;
            v = (uint8_t)((v << 1) | ((buf[b >> 3] >> (7 - (b & 7))) & 1));
          }
          copy[q] = v;
        }
        memcpy(group[nGroup++], copy, 8);
        seen++;
        bit += 32 + 64;
      }

      groupVerdict(out, group, nGroup, exact, repaired);
      groups++;
      nGroup = 0;
      lastFrameMs = millis();
      continue;
    }

    if ((spin++ & 0xFF) == 0) {
      if (radio.operationMode() != OMST_RX) radio.command(CMD_RX_MODE);
      if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
      checks++;
      delay(1);
    }
  }
  out.println();
  snprintf(line, sizeof(line),
           "  %lu rafale(s), %lu trame(s) recue(s), %lu exacte(s) d'emblee, %lu reparee(s).",
           (unsigned long)groups, (unsigned long)seen, (unsigned long)exact,
           (unsigned long)repaired);
  out.println(line);
  snprintf(line, sizeof(line), "  Signal fort %lu/%lu.", (unsigned long)strong,
           (unsigned long)checks);
  out.println(line);
}

// CRC-16/CCITT 0x1021, init 0xFFFF, sur l'adresse SUR L'AIR puis la charge
// utile. L'adresse est stockee a l'envers de son ordre d'emission.
// ---------------------------------------------------------------------------
//  Emettre une trame Halo 1 : adresse enregistree + 6 octets + CRC materiel.
//
//  Chemin de la balise d'etalonnage, qui marche : moteur de paquets, charge
//  utile statique, ecriture FIFO sans acquittement, CRC produit par la puce.
//  Mesure a l'appui, ce CRC materiel est EXACTEMENT le modele observe sur la
//  telecommande -- CRC-16/CCITT 0x1021, init 0xFFFF, couvrant adresse et
//  charge utile : la balise produisait C2BA, et le calcul logiciel aussi.
//
//  Seule difference connue avec la telecommande : la polarite du preambule.
//  Le moteur de paquets la deduit du premier bit d'adresse (1 -> 10101010),
//  alors que la telecommande emet ...0101 1 juste avant l'adresse.
// ---------------------------------------------------------------------------
void BenqHalo::txHalo1(Print &out, const uint8_t payload[6], uint16_t count, uint16_t gapMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }
  char line[176];
  const uint16_t crc = halo1Crc(payload);
  snprintf(line, sizeof(line),
           "  Trame attendue sur l'air : %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X | %02X %02X",
           addr_[3], addr_[2], addr_[1], addr_[0], payload[0], payload[1], payload[2], payload[3],
           payload[4], payload[5], (unsigned)(crc >> 8), (unsigned)(crc & 0xFF));
  out.println(line);
  snprintf(line, sizeof(line), "  Canal %u, %s, %u emission(s), %u ms d'intervalle.",
           (unsigned)channel_, dataRateName(dataRate_), count, gapMs);
  out.println(line);
  Serial.flush();

  configForLoopback(radio, channel_, addr_, false, dataRate_);

  uint16_t sent = 0, confirmed = 0;
  for (uint16_t i = 0; i < count; i++) {
    radio.command(CMD_FLUSH_TX_FIFO);
    radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
    radio.writeCommandData(CMD_WRITE_TX_FIFO_NO_ACK, payload, 6);
    sent++;
    // Seul TX_DS atteste que la puce a reellement emis.
    const uint32_t until = micros() + 5000;
    while ((int32_t)(micros() - until) < 0) {
      if (radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER) & IRQ_TX_DS) {
        confirmed++;
        break;
      }
      delayMicroseconds(10);
    }
    if (gapMs) delay(gapMs);
    if ((i & 0x3F) == 0x3F) delay(1);
  }
  radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
  radio.command(CMD_LIGHT_SLEEP);
  snprintf(line, sizeof(line), "  %u emise(s), %u confirmee(s) par TX_DS.", sent, confirmed);
  out.println(line);
}


// ---------------------------------------------------------------------------
//  Emettre des octets BRUTS apres l'adresse, CRC materiel desactive.
//
//  Structure etablie le 2026-09-23 par la linearite du CRC, sur des captures
//  asynchrones du CC2500 :
//    commande (telecommande -> lampe) : adresse | en-tete | 2 octets | CRC
//    accuse   (lampe -> telecommande) : adresse | en-tete | CRC
//  en-tete = [longueur 4 bits][compteur 2 bits][type 2 bits].
//  CRC-16/CCITT 0x1021 couvrant adresse + en-tete + charge, etat initial
//  0xDFBE pour la telecommande et 0xF55A pour la lampe.
//
//  Le CRC materiel du BC5602 ne calcule pas avec ces etats initiaux : on le
//  coupe et on fournit les deux octets de CRC nous-memes, en fin de charge.
// ---------------------------------------------------------------------------
void BenqHalo::txRaw(Print &out, const uint8_t *bytes, uint8_t len, uint16_t count,
                     uint16_t gapMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }
  char line[176];
  size_t w = (size_t)snprintf(line, sizeof(line), "  Sur l'air : %02X %02X %02X %02X |", addr_[3],
                              addr_[2], addr_[1], addr_[0]);
  for (uint8_t i = 0; i < len && w + 4 < sizeof(line); i++)
    w += (size_t)snprintf(line + w, sizeof(line) - w, " %02X", bytes[i]);
  out.println(line);
  Serial.flush();

  configForLoopback(radio, channel_, addr_, false, dataRate_);
  radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);  // CRC materiel coupe

  uint16_t confirmed = 0;
  for (uint16_t i = 0; i < count; i++) {
    radio.command(CMD_FLUSH_TX_FIFO);
    radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
    radio.writeCommandData(CMD_WRITE_TX_FIFO_NO_ACK, bytes, len);
    const uint32_t until = micros() + 5000;
    while ((int32_t)(micros() - until) < 0) {
      if (radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER) & IRQ_TX_DS) {
        confirmed++;
        break;
      }
      delayMicroseconds(10);
    }
    if (gapMs) delay(gapMs);
  }
  radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
  radio.command(CMD_LIGHT_SLEEP);
  snprintf(line, sizeof(line), "  %u emise(s), %u confirmee(s) par TX_DS.", count, confirmed);
  out.println(line);
}


// ---------------------------------------------------------------------------
//  Emission et reception au FORMAT BC5602 STANDARD, accuse automatique.
//
//  Audit du 23/09 : la telecommande et la lampe Halo 1 utilisent le format
//  standard de la puce -- preambule, adresse, PCF de 9 bits (longueur, PID,
//  NO_ACK), charge dynamique, CRC-16/CCITT init 0xFFFF -- avec accuse
//  automatique. La puce genere elle-meme PCF et CRC ; avec l'accuse demande,
//  elle dit d'elle-meme si le destinataire a repondu : TX_DS = accuse recu,
//  MAX_RT = echec apres les retransmissions. C'est une preuve radio objective,
//  independante du sens de la charge.
//
//  Toute la configuration est rejouee apres le reset logiciel, qui efface les
//  reglages analogiques et CFG1 (dont l'AGC, indispensable pour recevoir
//  l'accuse).
// ---------------------------------------------------------------------------
static void configStdAutoAck(BC5602 &r, const uint8_t addrReg[4], uint8_t channel, uint8_t rate,
                             bool receiver) {
  r.softwareReset();
  delay(20);
  r.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
  r.registerConfigure(nullptr);
  applyXoTrim(r);
  r.setBank(0);
  r.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, CFG1_AGC_EN);
  r.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, channel);
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
  r.writeRegister(REG_RT1 | CMD_WRITE_REGISTER, 0x73);
  r.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
  r.command(CMD_FLUSH_TX_FIFO);
  r.command(CMD_FLUSH_RX_FIFO);
  r.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
}

void BenqHalo::txAck(Print &out, const uint8_t addrReg[4], uint8_t channel, const uint8_t *payload,
                     uint8_t len, uint8_t trials, uint16_t gapMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }
  char line[176];
  size_t w = (size_t)snprintf(line, sizeof(line),
                              "  Emission avec accuse : adresse %02X %02X %02X %02X (sur l'air "
                              "%02X %02X %02X %02X), canal %u, charge",
                              addrReg[0], addrReg[1], addrReg[2], addrReg[3], addrReg[3], addrReg[2],
                              addrReg[1], addrReg[0], (unsigned)channel);
  for (uint8_t i = 0; i < len && w + 4 < sizeof(line); i++)
    w += (size_t)snprintf(line + w, sizeof(line) - w, " %02X", payload[i]);
  out.println(line);
  Serial.flush();

  configStdAutoAck(radio, addrReg, channel, dataRate_, false);
  uint8_t acked = 0, maxrt = 0;
  for (uint8_t i = 0; i < trials; i++) {
    radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
    radio.command(CMD_FLUSH_TX_FIFO);
    radio.writeCommandData(CMD_WRITE_TX_FIFO_WITH_ACK, payload, len);  // 0x11 : AVEC accuse
    uint8_t status = radio.readRegister(REG_STATUS | CMD_READ_REGISTER);
    const char *verdict = "DELAI";
    uint8_t irq = 0;
    uint32_t us = 0;
    if (status & STATUS_TX_FIFO_EMPTY) {
      verdict = "FIFO REFUSEE";
    } else {
      const uint32_t t0 = micros();
      radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, CE_ENABLE);
      while ((uint32_t)(micros() - t0) < 30000) {
        irq = radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
        if (irq & (IRQ_TX_DS | IRQ_MAX_RT)) break;
        delayMicroseconds(20);
      }
      us = micros() - t0;
      if (irq & IRQ_TX_DS) {
        verdict = "TX_DS (accuse recu)";
        acked++;
      } else if (irq & IRQ_MAX_RT) {
        verdict = "MAX_RT (pas d'accuse)";
        maxrt++;
      }
    }
    const uint8_t rt2 = radio.readRegister(REG_RT2 | CMD_READ_REGISTER);
    status = radio.readRegister(REG_STATUS | CMD_READ_REGISTER);
    // Remise a zero : CE=0, sinon la puce repart seule tant que la FIFO n'est
    // pas vide ; drapeaux acquittes en ecrivant 1 ; FIFO videes.
    radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
    radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_CLEAR_ALL);
    radio.command(CMD_FLUSH_TX_FIFO);
    radio.command(CMD_FLUSH_RX_FIFO);
    snprintf(line, sizeof(line), "  essai %2u : %-22s IRQ1 %02X  RT2 %02X  STATUS %02X  %5lu us", i + 1,
             verdict, irq, rt2, status, (unsigned long)us);
    out.println(line);
    Serial.flush();
    if (gapMs) delay(gapMs);
  }
  radio.command(CMD_LIGHT_SLEEP);
  snprintf(line, sizeof(line), "  Bilan : %u accuse(s) recu(s), %u echec(s), sur %u essai(s).", acked,
           maxrt, trials);
  out.println(line);
}

// Recepteur de banc : accuse automatiquement tout ce qui arrive a son adresse,
// et affiche la charge avec la longueur lue dans le PCF par la puce.
void BenqHalo::prxAck(Print &out, const uint8_t addrReg[4], uint8_t channel, uint32_t ms) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }
  char line[176];
  snprintf(line, sizeof(line),
           "  Recepteur avec accuse : adresse %02X %02X %02X %02X, canal %u, pendant %lu ms.",
           addrReg[0], addrReg[1], addrReg[2], addrReg[3], (unsigned)channel, (unsigned long)ms);
  out.println(line);
  Serial.flush();
  configStdAutoAck(radio, addrReg, channel, dataRate_, true);
  radio.enterRxMode();
  uint32_t got = 0;
  const uint32_t until = millis() + ms;
  uint32_t spin = 0;
  while ((int32_t)(millis() - until) < 0) {
    const uint8_t irq = radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
    if (irq & IRQ_RX_DR) {
      uint8_t len = radio.readRegister(REG_PKT4 | CMD_READ_REGISTER);
      uint8_t buf[32];
      if (len > 32) len = 32;
      if (len) radio.readFifo(buf, len, false);
      got++;
      size_t w = (size_t)snprintf(line, sizeof(line), "  RECU %3lu : longueur %u :", (unsigned long)got,
                                  len);
      for (uint8_t i = 0; i < len && w + 4 < sizeof(line); i++)
        w += (size_t)snprintf(line + w, sizeof(line) - w, " %02X", buf[i]);
      out.println(line);
      Serial.flush();
      radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
      radio.command(CMD_FLUSH_RX_FIFO);
    }
    if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
    if ((spin++ & 0x3FF) == 0) delay(1);
  }
  radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x00);
  radio.command(CMD_LIGHT_SLEEP);
  snprintf(line, sizeof(line), "  %lu trame(s) recue(s) et accusee(s).", (unsigned long)got);
  out.println(line);
}


uint16_t BenqHalo::halo1Crc(const uint8_t payload[6]) const {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < 10; i++) {
    const uint8_t b = (i < 4) ? addr_[3 - i] : payload[i - 4];
    for (int8_t k = 7; k >= 0; k--) {
      crc ^= (uint16_t)(((b >> k) & 1) << 15);
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// Verdict d'une rafale : d'abord chercher une copie deja exacte, sinon voter
// bit a bit. Les erreurs ne tombant pas au meme endroit d'une copie a l'autre,
// la majorite reconstitue l'original des trois copies environ.
void BenqHalo::groupVerdict(Print &out, const uint8_t group[][8], uint8_t n, uint32_t &exact,
                            uint32_t &repaired) {
  char line[176];
  for (uint8_t i = 0; i < n; i++) {
    const uint16_t got = (uint16_t)((group[i][6] << 8) | group[i][7]);
    if (halo1Crc(group[i]) == got) {
      exact++;
      snprintf(line, sizeof(line),
               "  TRAME (copie exacte, rafale de %u) : %02X %02X %02X %02X %02X %02X", n,
               group[i][0], group[i][1], group[i][2], group[i][3], group[i][4], group[i][5]);
      out.println(line);
      Serial.flush();
      return;
    }
  }
  if (n < 3) {
    snprintf(line, sizeof(line), "  rafale de %u copie(s) : trop peu pour voter", n);
    out.println(line);
    Serial.flush();
    return;
  }

  uint8_t voted[8];
  for (uint8_t b = 0; b < 8; b++) {
    voted[b] = 0;
    for (int8_t k = 7; k >= 0; k--) {
      uint8_t ones = 0;
      for (uint8_t i = 0; i < n; i++) ones = (uint8_t)(ones + ((group[i][b] >> k) & 1));
      if (ones * 2 > n) voted[b] |= (uint8_t)(1 << k);
    }
  }
  const uint16_t got = (uint16_t)((voted[6] << 8) | voted[7]);
  if (halo1Crc(voted) == got) {
    repaired++;
    snprintf(line, sizeof(line), "  TRAME (reparee par vote sur %u copies) : %02X %02X %02X %02X %02X %02X",
             n, voted[0], voted[1], voted[2], voted[3], voted[4], voted[5]);
  } else {
    snprintf(line, sizeof(line), "  rafale de %u : vote insuffisant (%02X %02X %02X %02X %02X %02X)",
             n, voted[0], voted[1], voted[2], voted[3], voted[4], voted[5]);
  }
  out.println(line);
  Serial.flush();
}


void BenqHalo::listenLikeUpstream(Print &out, uint32_t dwellMs, const uint8_t addr[4],
                                  uint8_t payloadLen) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }
  char line[176];

  out.println();
  out.println("=== Ecoute a la maniere du projet amont (sans reset) ===");
  out.println("  Reproduction de prepare_halo_receive() : pas de reset");
  out.println("  logiciel, CRC et auto-ACK desactives, payload statique de");
  out.println("  13 octets. Les valeurs Holtek ecrites au demarrage survivent,");
  out.println("  puisque rien ne les efface.");
  snprintf(line, sizeof(line), "  Canal %u, %s, adresse %02X %02X %02X %02X, %u octets.",
           (unsigned)channel_, dataRateName(dataRate_), addr[0], addr[1], addr[2], addr[3],
           (unsigned)payloadLen);
  out.println(line);
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  out.println();
  Serial.flush();

  // --- prepare_halo_receive(), pas a pas, sans reset ---
  radio.command(CMD_LIGHT_SLEEP);
  radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);  // 0x06 <- 0x48
  radio.setBank(0);
  radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, channel_);      // 0x10
  // Debit pilotable : a 500 kbps on sur-echantillonne x4 un signal a 125
  // kbps, ce qui permet de viser le PREAMBULE sur-echantillonne au lieu de
  // l'adresse. C'est la ruse du nRF52840, tentee ici a x4 faute de 1 Mbps.
  radio.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, (uint8_t)(ADDR_LEN_4 | dataRate_));
  radio.writeCommandData(CMD_WRITE_PTX_ADDRESS, addr, 4);

  uint8_t mask = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
  radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mask | MASK_PRM_RX));  // PRX

  radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
  radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x00);
  radio.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, payloadLen);
  radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);   // CRC desactive
  radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);    // auto-ACK desactive
  radio.command(CMD_FLUSH_RX_FIFO);                           // 0x89
  radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, 0x40);   // acquitter RX_DR
  radio.command(CMD_RX_MODE);                                 // 0x8E

  // Temoin de reglage : sans reset, les valeurs de begin() doivent etre la.
  uint8_t total = 0;
  const uint8_t ok = radio.registerVerify(&total);
  snprintf(line, sizeof(line), "  Reglages analogiques en place : %u sur %u.", (unsigned)ok,
           (unsigned)total);
  out.println(line);
  radio.setBank(0);

  const uint8_t omst = radio.operationMode();
  snprintf(line, sizeof(line), "  Mode apres armement : OMST %u (%s).", (unsigned)omst,
           omst == OMST_RX ? "EN RECEPTION" : "PAS en reception");
  out.println(line);
  Serial.flush();

  // GIO3S=14 : temoin de detection de preambule, en amont du correlateur.
  const uint8_t io2Base = radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0;
  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, (uint8_t)(io2Base | 14));
  pinMode(PIN_GIO3_TAP, INPUT);

  const uint32_t mask3 = 1UL << PIN_GIO3_TAP;
  uint32_t edges = 0, strong = 0, checks = 0, frames = 0;
  uint32_t last = REG_READ(GPIO_IN_REG) & mask3;
  const uint32_t until = millis() + dwellMs;
  uint8_t dumped = 0;

  while ((int32_t)(millis() - until) < 0) {
    for (uint16_t burst = 0; burst < 512; burst++) {
      const uint32_t now = REG_READ(GPIO_IN_REG) & mask3;
      if (now != last) {
        edges++;
        last = now;
      }
    }
    // Reception reelle : une trame qui arrive est lue, CRC ou pas.
    const uint8_t irq = radio.readRegister(REG_IRQ1 | CMD_READ_REGISTER);
    if (irq & IRQ_RX_DR) {
      uint8_t buf[32];
      const uint8_t n = payloadLen > 32 ? 32 : payloadLen;
      radio.readFifo(buf, n, false);
      frames++;
      if (dumped < 12) {
        size_t w = (size_t)snprintf(line, sizeof(line), "    trame %2u :", (unsigned)frames);
        for (uint8_t i = 0; i < n && w + 4 < sizeof(line); i++)
          w += (size_t)snprintf(line + w, sizeof(line) - w, " %02X", buf[i]);
        out.println(line);
        Serial.flush();
        dumped++;
      }
      radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, 0x40);
      radio.command(CMD_FLUSH_RX_FIFO);
    }
    // Rearmement, sans reset : c'est tout l'interet de cette voie.
    if (radio.operationMode() != OMST_RX) radio.command(CMD_RX_MODE);
    if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
    checks++;
    delay(1);
  }

  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, io2Base);

  out.println();
  snprintf(line, sizeof(line), "  %lu transitions GIO3, %lu trame(s), signal fort %lu/%lu.",
           (unsigned long)edges, (unsigned long)frames, (unsigned long)strong,
           (unsigned long)checks);
  out.println(line);
  if (edges > 50)
    out.println("  DES PREAMBULES SONT RECONNUS. La voie sans reset change tout.");
  else if (strong * 200 > checks)
    out.println("  Toujours aucun preambule, alors que la source est bien la.");
  else out.println("  Aucun preambule, mais le temoin de trafic est faible : refaire.");
}


void BenqHalo::probePreambleShape(Print &out, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }
  char line[176];
  // Dernier octet a 1 en poids fort -> preambule 10101010 ; a 0 -> 01010101.
  static const uint8_t addrAA[4] = {0x44, 0x33, 0x22, 0xE1};
  static const uint8_t addr55[4] = {0x44, 0x33, 0x22, 0x61};
  const uint8_t *addrs[2] = {addrAA, addr55};
  const char *polName[2] = {"10101010", "01010101"};
  const uint8_t rates[3] = {DATARATE_125K, DATARATE_250K, DATARATE_500K};

  out.println();
  out.println("=== Forme du preambule : polarite et longueur ===");
  out.println("  Le BC5602 deduit la polarite du preambule du premier bit");
  out.println("  d'adresse emis. Nos sondes n'ont jamais teste qu'une des deux,");
  out.println("  ni qu'une des deux longueurs. Douze configurations.");
  snprintf(line, sizeof(line), "  Canal %u, %lu ms chacune. Reference : 10101010 / 1 octet /",
           (unsigned)channel_, (unsigned long)dwellMs);
  out.println(line);
  out.println("  125 kbps a donne 482 transitions sur la balise.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER.");
  out.println();
  Serial.flush();

  const uint8_t io2Base = radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0;
  const uint32_t mask = 1UL << PIN_GIO3_TAP;
  uint32_t best = 0;
  char bestName[48] = "aucune";

  for (uint8_t pol = 0; pol < 2; pol++) {
    for (uint8_t two = 0; two < 2; two++) {
      for (uint8_t r = 0; r < 3; r++) {
        configForLoopback(radio, channel_, addrs[pol], true, rates[r]);
        uint8_t cfo1 = radio.readRegister(B0_CFO1 | CMD_READ_REGISTER);
        cfo1 = two ? (uint8_t)(cfo1 | 0x40) : (uint8_t)(cfo1 & (uint8_t)~0x40);
        radio.writeRegister(B0_CFO1 | CMD_WRITE_REGISTER, cfo1);
        radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, (uint8_t)(io2Base | 14));
        radio.enterRxMode();
        pinMode(PIN_GIO3_TAP, INPUT);

        uint32_t edges = 0, strong = 0, checks = 0;
        uint32_t last = REG_READ(GPIO_IN_REG) & mask;
        const uint32_t until = millis() + dwellMs;
        while ((int32_t)(millis() - until) < 0) {
          for (uint16_t burst = 0; burst < 512; burst++) {
            const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
            if (now != last) {
              edges++;
              last = now;
            }
          }
          if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
          if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
          checks++;
        }

        snprintf(line, sizeof(line), "  %s  %u octet%s  %-8s : %lu transitions, signal fort %lu/%lu",
                 polName[pol], (unsigned)(two + 1), two ? "s" : " ", dataRateName(rates[r]),
                 (unsigned long)edges, (unsigned long)strong, (unsigned long)checks);
        out.println(line);
        Serial.flush();

        if (edges > best) {
          best = edges;
          snprintf(bestName, sizeof(bestName), "%s / %u octet(s) / %s", polName[pol],
                   (unsigned)(two + 1), dataRateName(rates[r]));
        }
        delay(1);
      }
    }
  }

  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, io2Base);
  out.println();
  if (best > 50) {
    snprintf(line, sizeof(line), "  La source s'accroche en %s (%lu transitions).", bestName,
             (unsigned long)best);
    out.println(line);
  } else {
    out.println("  Aucune des douze formes ne fait sortir de transitions. La");
    out.println("  polarite et la longueur du preambule sont donc epuisees :");
    out.println("  ce n'est pas la mise en forme du preambule qui bloque.");
  }
}


void BenqHalo::discriminateWifi(Print &out, uint32_t phaseMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }
  char line[176];
  const uint8_t chans[3] = {5, 20, 78};
  const char *what[3] = {"cible, dans le Wi-Fi 1", "temoin Wi-Fi 1", "hors Wi-Fi"};

  out.println();
  out.println("=== Canal 5 : la telecommande, ou ton Wi-Fi ? ===");
  out.println("  Le Wi-Fi 1 est large de 20 MHz : il depose autant d'energie a");
  out.println("  2420 MHz qu'a 2405. La telecommande ne fait que 0,43 MHz : elle");
  out.println("  ne peut etre qu'a UN des deux endroits. On alterne les canaux");
  out.println("  toutes les quelques millisecondes, trop vite pour qu'une rafale");
  out.println("  Wi-Fi en favorise un.");
  out.println();
  Serial.flush();

  const uint8_t savedChannel = channel_;
  uint32_t strong[2][3] = {{0, 0, 0}, {0, 0, 0}};
  uint32_t checks[2][3] = {{0, 0, 0}, {0, 0, 0}};

  for (uint8_t phase = 0; phase < 2; phase++) {
    if (phase == 0) out.println("  Phase 1 sur 2 -- NE TOUCHE A RIEN, lache la telecommande :");
    else out.println("  Phase 2 sur 2 -- TOURNE LA MOLETTE SANS T'ARRETER :");
    for (uint8_t k = 3; k >= 1; k--) {
      snprintf(line, sizeof(line), "    %u...", (unsigned)k);
      out.println(line);
      Serial.flush();
      delay(1000);
    }

    const uint32_t until = millis() + phaseMs;
    while ((int32_t)(millis() - until) < 0) {
      for (uint8_t c = 0; c < 3; c++) {
        // Reconfiguration COMPLETE a chaque visite. Mesure a l'appui : ecrire
        // seulement RFCH et attendre 1,5 ms ne retune pas la PLL -- avec la
        // balise sur le seul canal 5, les trois canaux lisaient 992 pour mille,
        // y compris un canal a 73 MHz de distance. Le RSSI ne suivait pas.
        configForLoopback(radio, chans[c], kCalRegAddr, true, dataRate_);
        radio.enterRxMode();

        // Visites de 200 ms : assez long pour amortir les 20 ms de
        // reconfiguration, assez court pour qu'une rafale Wi-Fi ne puisse pas
        // favoriser un canal plutot qu'un autre.
        const uint32_t leave = millis() + 200;
        while ((int32_t)(millis() - leave) < 0) {
          if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
          if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong[phase][c]++;
          checks[phase][c]++;
        }
        delay(1);
      }
    }
    out.println();
  }

  channel_ = savedChannel;

  out.println("  canal            role                     repos      molette   rapport");
  float ratio[3] = {0, 0, 0};
  for (uint8_t c = 0; c < 3; c++) {
    const float r0 = checks[0][c] ? (1000.0f * strong[0][c] / checks[0][c]) : 0.0f;
    const float r1 = checks[1][c] ? (1000.0f * strong[1][c] / checks[1][c]) : 0.0f;
    ratio[c] = (r0 > 0.05f) ? (r1 / r0) : (r1 > 0.05f ? 999.0f : 1.0f);
    snprintf(line, sizeof(line), "  %-2u = %u MHz  %-22s  %7.2f0/00 %7.2f0/00   x%.2f",
             (unsigned)chans[c], (unsigned)(2400 + chans[c]), what[c], (double)r0, (double)r1,
             (double)ratio[c]);
    out.println(line);
  }

  out.println();
  if (ratio[0] > 1.6f && ratio[0] > 2.0f * ratio[1]) {
    out.println("  Le canal 5 monte et le canal 20 ne suit PAS : la source est");
    out.println("  etroite et centree sur 2405 MHz. C'est bien la telecommande.");
  } else if (ratio[0] > 1.6f && ratio[1] > 1.6f) {
    out.println("  Les canaux 5 ET 20 montent ensemble : c'est une source LARGE,");
    out.println("  donc du Wi-Fi, pas la telecommande. Le canal 5 est une");
    out.println("  fausse piste depuis le debut -- il faut rechercher la");
    out.println("  telecommande ailleurs dans la bande.");
  } else if (ratio[0] <= 1.6f) {
    out.println("  Le canal 5 ne monte pas quand tu tournes la molette. Soit la");
    out.println("  telecommande n'a pas emis pendant la phase 2, soit elle");
    out.println("  n'est pas sur ce canal.");
  } else {
    out.println("  Resultat ambigu : relance avec une phase plus longue.");
  }
}


void BenqHalo::checkGio3Wire(Print &out) {
  char line[168];
  out.println();
  out.println("=== Le fil GIO3 fait-il contact ? ===");
  out.println("  Pastille 8 du module -> IO3. Test electrique, radio hors jeu.");
  out.println("  On force la pastille a sortir un niveau, selecteur par");
  out.println("  selecteur, et on regarde si la broche resiste aux resistances");
  out.println("  internes de l'ESP32 (~45 kOhm). Une broche qui suit les DEUX");
  out.println("  tractions n'est reliee a rien.");
  out.println();

  const uint8_t io2Base = radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0;
  uint8_t driven = 0;

  for (uint8_t sel = 0; sel < 16; sel++) {
    radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, (uint8_t)(io2Base | sel));
    delay(2);

    pinMode(PIN_GIO3_TAP, INPUT_PULLUP);
    delay(3);
    uint8_t up = 0;
    for (uint8_t i = 0; i < 20; i++) { up += digitalRead(PIN_GIO3_TAP) ? 1 : 0; delayMicroseconds(200); }

    pinMode(PIN_GIO3_TAP, INPUT_PULLDOWN);
    delay(3);
    uint8_t dn = 0;
    for (uint8_t i = 0; i < 20; i++) { dn += digitalRead(PIN_GIO3_TAP) ? 1 : 0; delayMicroseconds(200); }

    // Pilotee = elle tient un niveau CONTRE la resistance qui tire a l'oppose.
    const bool heldHigh = (dn >= 18);
    const bool heldLow = (up <= 2);
    if (heldHigh || heldLow) driven++;
    snprintf(line, sizeof(line), "  GIO3S=%-2u  tirage haut %2u/20   tirage bas %2u/20   %s", (unsigned)sel,
             up, dn, heldHigh ? "PILOTEE a 1" : (heldLow ? "PILOTEE a 0" : "libre"));
    out.println(line);
    delay(1);
  }

  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, io2Base);
  pinMode(PIN_GIO3_TAP, INPUT);

  out.println();
  if (driven == 0) {
    out.println("  Aucun selecteur ne tient la broche contre les resistances");
    out.println("  internes. LE FIL NE FAIT PAS CONTACT. Toute mesure GIO3");
    out.println("  faite dans cet etat est nulle et non avenue.");
  } else {
    snprintf(line, sizeof(line), "  %u selecteur(s) pilotent la broche : le fil fait contact.",
             (unsigned)driven);
    out.println(line);
  }
}

void BenqHalo::sweepGio3(Print &out, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];

  out.println();
  out.println("=== Fonctions cachees du selecteur GIO3 ===");
  out.println("  Le datasheet documente cinq valeurs sur seize. Mais GIO3S=8 est");
  out.println("  TBCLK_OUTPUT, absent de sa table : il omet donc des fonctions");
  out.println("  reelles. Les valeurs 9 a 15 n'ont jamais ete testees, par");
  out.println("  personne. On cherche une sortie de donnees demodulees ou une");
  out.println("  horloge utilisable EN RECEPTION.");
  out.println();
  snprintf(line, sizeof(line), "  Cablage : pastille 8 du module (GIO3) -> IO%u.",
           (unsigned)PIN_GIO3_TAP);
  out.println(line);
  out.println("  Contrairement a GIO2, GIO3 ne sert pas au SPI : la liaison avec");
  out.println("  la puce reste vivante. Le trafic est atteste par le RSSI, qui ne");
  out.println("  depend d'aucune adresse -- donc valable pour la telecommande.");
  out.println("  >>> Il faut une source qui emette : la balise sur l'autre carte,");
  out.println("      ou la molette de la telecommande tournee sans arret.");
  Serial.flush();

  // Conserve PADDS et GIO4S, ne touche qu'aux quatre bits de GIO3S.
  const uint8_t io2Base = radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0;

  uint32_t quietest = 0xFFFFFFFF;
  uint32_t trans[16];
  uint32_t frames[16];

  for (uint8_t sel = 0; sel < 16; sel++) {
    configForLoopback(radio, RF_CHANNEL_1, kCalRegAddr, true, dataRate_);
    radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, (uint8_t)(io2Base | sel));
    radio.enterRxMode();

    pinMode(PIN_GIO3_TAP, INPUT);
    uint32_t edges = 0, samples = 0, got = 0;
    int last = digitalRead(PIN_GIO3_TAP);
    const uint32_t until = millis() + dwellMs;

    while ((int32_t)(millis() - until) < 0) {
      for (uint16_t burst = 0; burst < 256; burst++) {
        const int now = digitalRead(PIN_GIO3_TAP);
        if (now != last) {
          edges++;
          last = now;
        }
      }
      samples += 256;

      // Temoin de trafic par le RSSI, et non par un compteur de trames : il ne
      // depend d'aucune adresse, donc il vaut aussi bien pour la balise que pour
      // la telecommande. Compter les trames n'aurait mesure que la balise.
      if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
      if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) got++;
    }

    trans[sel] = edges;
    frames[sel] = got;
    if (edges < quietest) quietest = edges;

    const char *known = (sel == 0)    ? "rien"
                        : (sel == 1)  ? "SDO"
                        : (sel == 5)  ? "IRQ"
                        : (sel == 8)  ? "TBCLK"
                        : (sel == 12) ? "EPA_EN"
                        : (sel == 13) ? "ELAN_EN"
                                      : "non documente";
    snprintf(line, sizeof(line),
             "  GIO3S=%-2u %-14s : %lu transitions / %lu ech., signal fort %lu",
             (unsigned)sel, known, (unsigned long)edges, (unsigned long)samples,
             (unsigned long)got);
    out.println(line);
    Serial.flush();
  }

  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, io2Base);

  uint32_t totalFrames = 0;
  for (uint8_t sel = 0; sel < 16; sel++) totalFrames += frames[sel];

  out.println();
  if (totalFrames == 0) {
    out.println("  AUCUN signal fort pendant toute la mesure : rien n'emettait.");
    out.println("  Fais tourner la molette, ou lance la balise, avant de conclure.");
    out.println();
    return;
  }

  bool found = false;
  for (uint8_t sel = 0; sel < 16; sel++) {
    if (trans[sel] > quietest * 4 + 200) {
      found = true;
      snprintf(line, sizeof(line), "  piste : GIO3S=%u s'anime (%lu transitions contre %lu au repos)",
               (unsigned)sel, (unsigned long)trans[sel], (unsigned long)quietest);
      out.println(line);
    }
  }
  if (!found) {
    out.println("  Aucun selecteur ne fait sortir quoi que ce soit sur GIO3,");
    out.println("  alors que des trames arrivaient bien pendant la mesure.");
    out.println("  Les seize valeurs sont donc epuisees.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  GIO3 : avant ou apres le correlateur ?
// ---------------------------------------------------------------------------

// Echantillonnage rapide d'une seule broche, a meme le registre GPIO. La boucle
// en digitalRead du balayage precedent tournait a 835 kHz, soit trois points par
// bit a 250 kbps : suffisant pour detecter une activite, trop lent pour la
// compter fidelement.
static uint32_t fastEdgeCount(uint8_t pin, uint32_t dwellMs, uint32_t *samples) {
  const uint32_t mask = 1UL << pin;
  pinMode(pin, INPUT);
  uint32_t edges = 0, n = 0;
  uint32_t last = REG_READ(GPIO_IN_REG) & mask;
  const uint32_t until = millis() + dwellMs;
  while ((int32_t)(millis() - until) < 0) {
    for (uint16_t burst = 0; burst < 512; burst++) {
      const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
      if (now != last) {
        edges++;
        last = now;
      }
    }
    n += 512;
  }
  if (samples) *samples = n;
  return edges;
}

void BenqHalo::checkGio3Correlator(Print &out, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];

  out.println();
  out.println("=== GIO3 : avant ou apres le correlateur ? ===");
  out.println("  Quatre selecteurs s'animent quand des trames sont acceptees.");
  out.println("  Reste a savoir s'ils refletent le signal demodule BRUT, ou");
  out.println("  seulement les trames ayant passe le filtrage d'adresse.");
  out.println("  On refait donc la mesure avec une adresse FAUSSE d'un octet :");
  out.println("  aucune trame ne doit plus etre acceptee. Si la broche continue");
  out.println("  de s'agiter, elle est EN AMONT du correlateur -- et l'adresse");
  out.println("  devient lisible sans la connaitre.");
  out.println("  >>> La balise doit tourner sur l'autre carte.");
  out.println();
  Serial.flush();

  const uint8_t selectors[4] = {2, 4, 9, 14};
  const uint8_t io2Base = radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0;
  bool anyPreCorrelator = false;

  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t sel = selectors[i];
    uint32_t edges[2] = {0, 0};
    uint32_t got[2] = {0, 0};

    for (uint8_t pass = 0; pass < 2; pass++) {
      const bool matching = (pass == 0);
      configForLoopback(radio, RF_CHANNEL_1, matching ? kCalRegAddr : kCalWrongReg, true,
                        dataRate_);
      radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, (uint8_t)(io2Base | sel));
      radio.enterRxMode();

      // Compter les trames d'abord, sur une fenetre courte, puis mesurer les
      // transitions sans rien faire d'autre : les lectures SPI perturberaient
      // le comptage des fronts.
      const uint32_t until = millis() + dwellMs / 2;
      while ((int32_t)(millis() - until) < 0) {
        if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
        if (!(radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR)) {
          uint8_t buf[13];
          radio.readFifo(buf, 13, false);
          radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
          radio.command(CMD_FLUSH_RX_FIFO);
          got[pass]++;
        }
        delayMicroseconds(200);
      }

      uint32_t samples = 0;
      edges[pass] = fastEdgeCount(PIN_GIO3_TAP, dwellMs / 2, &samples);
    }

    // Une activite qui SURVIT a une adresse fausse ne peut pas venir du
    // moteur de paquets : elle vient du demodulateur.
    // Ne PAS exiger zero trame acceptee : quelques evenements parasites passent
    // toujours, et cette exigence trop stricte avait fait conclure l'inverse de
    // ce que la mesure montrait. Ce qui compte est l'effondrement des trames
    // acceptees face a des fronts qui, eux, ne bougent pas.
    const bool framesCollapsed = (got[1] * 4 < got[0]) || (got[0] == 0);
    const bool edgesSurvive = (edges[1] * 2 >= edges[0]) && edges[1] > 50;
    const bool survives = framesCollapsed && edgesSurvive;
    if (survives) anyPreCorrelator = true;

    snprintf(line, sizeof(line), "  GIO3S=%-2u  adresse juste : %lu fronts, %lu trame(s)",
             (unsigned)sel, (unsigned long)edges[0], (unsigned long)got[0]);
    out.println(line);
    snprintf(line, sizeof(line), "            adresse fausse : %lu fronts, %lu trame(s)%s",
             (unsigned long)edges[1], (unsigned long)got[1],
             survives ? "   <<< EN AMONT DU CORRELATEUR" : "");
    out.println(line);
    Serial.flush();
  }

  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, io2Base);

  out.println();
  if (anyPreCorrelator) {
    out.println("  Au moins une sortie reste active sans qu'aucune trame ne soit");
    out.println("  acceptee : elle reflete le signal demodule BRUT, avant tout");
    out.println("  filtrage d'adresse.");
    out.println("  C'est exactement ce qu'il nous fallait : en echantillonnant");
    out.println("  cette broche on lit le preambule, puis l'adresse, puis la");
    out.println("  trame -- sans avoir besoin de connaitre l'adresse d'avance.");
  } else {
    out.println("  Toutes les sorties s'eteignent avec une adresse fausse : elles");
    out.println("  sont en aval du correlateur et ne servent qu'a signaler une");
    out.println("  trame deja acceptee. Sans l'adresse, elles ne donnent rien.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Lire l'adresse dans le flux de bits demodule
// ---------------------------------------------------------------------------

void BenqHalo::captureGio3Bits(Print &out, uint8_t selector, uint32_t attempts) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];
  constexpr uint16_t kSamples = 6000;  // ~2 ms a 0,34 us par point
  static uint8_t raw[kSamples];

  out.println();
  out.println("=== Lecture de l'adresse dans le flux demodule ===");
  snprintf(line, sizeof(line), "  Selecteur GIO3S=%u, broche IO%u.", (unsigned)selector,
           (unsigned)PIN_GIO3_TAP);
  out.println(line);
  out.println("  Le recepteur est cale sur une adresse quelconque : cette sortie");
  out.println("  etant en amont du filtrage, l'adresse programmee n'a aucune");
  out.println("  importance. On lit les bits tels qu'ils arrivent.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER : c'est la seule source");
  out.println("      de trafic.");

  Serial.flush();

  const uint8_t io2Base = radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0;
  configForLoopback(radio, RF_CHANNEL_1, kCalWrongReg, true, dataRate_);
  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, (uint8_t)(io2Base | selector));
  radio.enterRxMode();

  const uint32_t mask = 1UL << PIN_GIO3_TAP;
  pinMode(PIN_GIO3_TAP, INPUT);

  uint32_t triggers = 0, shortRuns = 0, noPreamble = 0, strong = 0;
  uint8_t cand[24][4];
  uint8_t candHits[24] = {0};
  uint8_t candCount = 0;

  for (uint32_t attempt = 0; attempt < attempts; attempt++) {
    // CE est efface par le materiel a chaque fin de reception : sans rearmement
    // a chaque tour, la puce sort du mode RX apres le premier evenement et la
    // broche se tait pour de bon.
    if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
    radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
    radio.command(CMD_FLUSH_RX_FIFO);
    radio.writeRegister(REG_CE | CMD_WRITE_REGISTER, 0x01);

    // Attendre un front, puis capturer d'un trait : la rafale dure moins d'une
    // milliseconde, il ne faut surtout rien faire d'autre pendant ce temps.
    uint32_t last = REG_READ(GPIO_IN_REG) & mask;
    const uint32_t guard = millis() + 300;
    bool triggered = false;
    while ((int32_t)(millis() - guard) < 0) {
      const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
      if (now != last) {
        triggered = true;
        break;
      }
    }
    if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
    if (!triggered) continue;
    triggers++;

    for (uint16_t i = 0; i < kSamples; i++)
      raw[i] = (REG_READ(GPIO_IN_REG) & mask) ? 1 : 0;

    // Mesurer la duree du plus court palier : c'est la duree d'un bit, donc le
    // facteur de sur-echantillonnage. Le deduire des donnees evite de dependre
    // d'un debit suppose.
    uint16_t shortest = 0xFFFF, runs = 0;
    uint16_t run = 1;
    for (uint16_t i = 1; i < kSamples; i++) {
      if (raw[i] == raw[i - 1]) {
        run++;
      } else {
        if (run < shortest) shortest = run;
        runs++;
        run = 1;
      }
    }
    if (runs < 40 || shortest < 2 || shortest > 200) {
      shortRuns++;
      // Montrer les premiers echecs : ne rien afficher empeche de comprendre si
      // la broche porte des donnees trop lentes, trop rapides, ou rien du tout.
      if (shortRuns <= 3) {
        snprintf(line, sizeof(line), "  essai %lu : %u paliers, plus court %u points -- rejete",
                 (unsigned long)attempt + 1, (unsigned)runs, (unsigned)shortest);
        out.println(line);
        Serial.flush();
      }
      continue;
    }

    // Rechantillonner au milieu de chaque bit.
    uint8_t bits[260];
    uint16_t nbits = 0;
    for (uint32_t pos = shortest / 2; pos < kSamples && nbits < sizeof(bits); pos += shortest)
      bits[nbits++] = raw[pos];

    // Chercher le preambule : au moins douze alternances consecutives.
    int16_t start = -1;
    for (uint16_t i = 0; i + 12 < nbits; i++) {
      bool alternating = true;
      for (uint8_t k = 1; k < 12; k++)
        if (bits[i + k] == bits[i + k - 1]) {
          alternating = false;
          break;
        }
      if (alternating) {
        // Avancer jusqu'a la fin des alternances : l'adresse commence la.
        uint16_t j = i + 1;
        while (j + 1 < nbits && bits[j + 1] != bits[j]) j++;
        start = (int16_t)(j + 1);
        break;
      }
    }
    if (start < 0 || start + 48 > nbits) {
      noPreamble++;
      if (noPreamble <= 3) {
        int n = snprintf(line, sizeof(line),
                         "  essai %lu : %u bits, pas d'alternance nette. Debut :",
                         (unsigned long)attempt + 1, (unsigned)nbits);
        for (uint8_t k = 0; k < 40 && k < nbits; k++)
          n += snprintf(line + n, sizeof(line) - n, "%u", (unsigned)bits[k]);
        out.println(line);
        Serial.flush();
      }
      continue;
    }

    snprintf(line, sizeof(line),
             "  essai %lu : %u paliers, 1 bit = %u points, preambule a %d",
             (unsigned long)attempt + 1, (unsigned)runs, (unsigned)shortest, (int)start);
    out.println(line);

    // Les six octets qui suivent le preambule : adresse puis PCF. On ne compare
    // a rien -- on tient un decompte, car une adresse reelle se repete d'une
    // capture a l'autre alors que le bruit ne se repete jamais.
    for (uint8_t polarity = 0; polarity < 2; polarity++) {
      uint8_t bytes[6] = {0};
      for (uint8_t b = 0; b < 6; b++)
        for (uint8_t k = 0; k < 8; k++) {
          const uint8_t bit = bits[start + b * 8 + k] ^ polarity;
          bytes[b] = (uint8_t)((bytes[b] << 1) | bit);  // MSB en tete
        }
      int n = snprintf(line, sizeof(line), "    %s :", polarity ? "inverse" : "direct ");
      for (uint8_t b = 0; b < 6; b++)
        n += snprintf(line + n, sizeof(line) - n, " %02X", bytes[b]);
      if (memcmp(bytes, kCalAirAddr, 4) == 0)
        n += snprintf(line + n, sizeof(line) - n, "   <<< ADRESSE DE LA BALISE");
      out.println(line);

      // Decompte des quatre premiers octets.
      bool seen = false;
      for (uint8_t c = 0; c < candCount; c++)
        if (memcmp(cand[c], bytes, 4) == 0) {
          candHits[c]++;
          seen = true;
          break;
        }
      if (!seen && candCount < 24) {
        memcpy(cand[candCount], bytes, 4);
        candHits[candCount] = 1;
        candCount++;
      }
    }
    Serial.flush();
  }

  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, io2Base);
  out.println();
  snprintf(line, sizeof(line),
           "  Bilan : %lu declenchement(s) sur %lu, %lu rejet(s) de forme, %lu sans preambule.",
           (unsigned long)triggers, (unsigned long)attempts, (unsigned long)shortRuns,
           (unsigned long)noPreamble);
  out.println(line);
  snprintf(line, sizeof(line), "  Signal fort detecte %lu fois sur %lu tentatives.",
           (unsigned long)strong, (unsigned long)attempts);
  out.println(line);
  if (triggers == 0 && strong == 0) {
    out.println("  Rien n'emettait : le resultat ne dit rien de ce selecteur.");
  } else if (triggers == 0) {
    out.println("  La source emettait bien, mais GIO3 est reste muet. Le");
    out.println("  demodulateur n'accroche donc pas -- essaie l'autre debit avec");
    out.println("  'debit 125' ou 'debit 250', puis relance.");
  }
  out.println();
  if (candCount) {
    out.println("  Candidats, du plus frequent au moins frequent. Une adresse");
    out.println("  reelle se repete d'une capture a l'autre ; le bruit, jamais.");
    for (uint8_t rank = 0; rank < 6; rank++) {
      uint8_t best = 0xFF, bestHits = 0;
      for (uint8_t c = 0; c < candCount; c++)
        if (candHits[c] > bestHits) {
          bestHits = candHits[c];
          best = c;
        }
      if (best == 0xFF || bestHits == 0) break;
      snprintf(line, sizeof(line), "    %02X %02X %02X %02X  vu %u fois%s", cand[best][0],
               cand[best][1], cand[best][2], cand[best][3], (unsigned)bestHits,
               (bestHits >= 3) ? "   <<< SE REPETE" : "");
      out.println(line);
      candHits[best] = 0;
    }
    out.println();
    out.println("  Un candidat qui revient trois fois ou plus merite d'etre");
    out.println("  essaye : 'addr' dans l'ordre inverse, puis 'sniff'.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Chasse a l'adresse par mot de synchro ancre
// ---------------------------------------------------------------------------

// Examine une capture et tente d'y lire une adresse. Apres un accrochage en
// plein payload, la FIFO contient la fin de la trame courante, puis le silence,
// puis le PREAMBULE et l'ADRESSE de la suivante -- qui ne tombent pas sur la
// grille d'octets. On balaie donc les huit decalages de bit.
bool BenqHalo::scanCaptureForAddress(Print &out, const uint8_t *buf, uint8_t len,
                                     const char *context) {
  char line[176];
  bool found = false;

  for (uint8_t shift = 0; shift < 8; shift++) {
    uint8_t s[32];
    for (uint8_t i = 0; i < len; i++) {
      const uint8_t hi = (uint8_t)(buf[i] << shift);
      const uint8_t lo = (i + 1 < len) ? (uint8_t)(buf[i + 1] >> (8 - shift)) : 0;
      s[i] = shift ? (uint8_t)(hi | lo) : buf[i];
    }

    // La longueur du payload n'est pas connue. Le rapport FCC donne une trame
    // d'environ onze octets pour le Halo 1, contre dix-neuf pour le Halo 2 :
    // il ne reste donc que deux a quatre octets de payload. On essaie ces
    // longueurs-la en plus de celle du Halo 2.
    const uint8_t lengths[5] = {2, 3, 4, 6, 10};

    for (uint8_t p = 0; p < len; p++) {
      if (s[p] != 0xAA && s[p] != 0x55) continue;

      for (uint8_t li = 0; li < 5; li++) {
        const uint8_t plen = lengths[li];
        // preambule(1) + adresse(4) + PCF(1) + payload + CRC(2)
        if ((uint16_t)p + 8 + plen > len) continue;

        const uint8_t *air = &s[p + 1];  // les quatre octets d'adresse, sur l'air
        const uint8_t pcf = s[p + 5];
        const uint8_t *payload = &s[p + 6];
        const uint16_t got = (uint16_t)((s[p + 6 + plen] << 8) | s[p + 7 + plen]);

        uint16_t wantA = frameCrcFor(air, pcf, payload);
        uint16_t wantB = crcOverFrame(0x5042, pcf, payload);
        if (plen != 10) {
          // Recalculer sur la longueur reelle plutot que sur dix octets.
          auto feed = [](uint16_t crc, uint8_t b) {
            crc ^= (uint16_t)b << 8;
            for (uint8_t i = 0; i < 8; i++)
              crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
            return crc;
          };
          uint16_t a = 0xEFDF;
          for (uint8_t i = 0; i < 4; i++) a = feed(a, air[i]);
          a = feed(a, pcf);
          uint16_t b = feed(0x5042, pcf);
          for (uint8_t i = 0; i < plen; i++) {
            a = feed(a, payload[i]);
            b = feed(b, payload[i]);
          }
          wantA = a;
          wantB = b;
        }
        if (got != wantA && got != wantB) continue;

        snprintf(line, sizeof(line), "      payload de %u octet(s)", (unsigned)plen);
        out.println(line);
        found = true;
      snprintf(line, sizeof(line), "  *** ADRESSE CONFIRMEE PAR CRC  (%s)", context);
      out.println(line);
      snprintf(line, sizeof(line), "      sur l'air        : %02X %02X %02X %02X", air[0], air[1],
               air[2], air[3]);
      out.println(line);
      snprintf(line, sizeof(line), "      a saisir         : addr %02X%02X%02X%02X", air[3], air[2],
               air[1], air[0]);
      out.println(line);
        int n = snprintf(line, sizeof(line), "      PCF %02X, payload", pcf);
        for (uint8_t i = 0; i < plen; i++)
          n += snprintf(line + n, sizeof(line) - n, " %02X", payload[i]);
        out.println(line);
        snprintf(line, sizeof(line), "      CRC %04X, modele %s", got,
                 (got == wantA) ? "A (adresse couverte)" : "B (adresse non couverte)");
        out.println(line);
        Serial.flush();
        return true;
      }
    }
  }
  return found;
}

void BenqHalo::huntAnchored(Print &out, uint32_t seconds, uint8_t group, uint32_t dwellMs,
                            uint16_t fixedKelvin, int16_t fixedBack) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];

  out.println();
  out.println("=== Chasse a l'adresse par mot de synchro ancre ===");
  out.println("  Le detecteur de preambule ne s'arme que sur une suite alternee :");
  out.println("  la fenetre visee doit etre precedee d'un octet 0x55 ou 0xAA.");
  out.println("  Payload suppose, repris du Halo 2 :");
  out.println("    [cmd][ctrl][lum_avant][ct_hi][ct_lo][lum_arriere][ct_hi][ct_lo][01][02]");
  out.println();
  if (group == 0) {
    out.println("  GROUPE 1 -- ancre sur la LUMINOSITE ARRIERE a 85 % (=0x55).");
    out.println("  Fenetre visee : [ct_hi][ct_lo][01]. Une seule inconnue, la");
    out.println("  temperature, balayee OCTET PAR OCTET sans presumer d'un pas :");
    out.println("  4096 candidats couvrant 2560 a 6655 K.");
    out.println("  >>> Regle la lampe ARRIERE au maximum puis redescends d'un ou");
    out.println("      deux crans, et tourne lentement la molette de TEMPERATURE.");
  } else {
    out.println("  GROUPE 2 -- ancre sur la TEMPERATURE dont l'octet bas vaut");
    out.println("  0x55 ou 0xAA. Fenetre : [lum_arriere][ct_hi][ct_lo].");
    if (fixedBack >= 0) {
      snprintf(line, sizeof(line), "  Luminosite arriere IMPOSEE a %d %% : le balayage tombe a",
               (int)fixedBack);
      out.println(line);
      out.println("  32 candidats, soit un passage complet toutes les 2 secondes.");
      out.println("  >>> ETEINS LA LAMPE ARRIERE (elle vaut alors 0) et tourne la");
      out.println("      molette de luminosite AVANT sans t'arreter.");
    } else {
      out.println("  32 octets hauts x 2 ancres x 101 luminosites = 3232 candidats.");
    }
    out.println();
    out.println("  >>> ETEINS LA LAMPE ARRIERE et ne garde que l'avant allumee.");
    out.println("  >>> Puis tourne la molette de LUMINOSITE sans t'arreter.");
    out.println("  La luminosite AVANT ne figure pas dans la fenetre visee : elle");
    out.println("  fournit donc du trafic en continu pendant que la temperature et");
    out.println("  la luminosite arriere, elles, restent FIGEES. C'est ce qui rend");
    out.println("  le balayage utile : sans cela, la cible bouge en meme temps que");
    out.println("  les hypotheses et les deux ne se croisent jamais.");
    out.println("  Si rien ne sort, decale la temperature d'un cran et recommence :");
    out.println("  une valeur sur cinq environ produit une ancre.");
  }
  snprintf(line, sizeof(line), "  Duree : %lu s, %lu ms par candidat, debit %s, canal %u.",
           (unsigned long)seconds, (unsigned long)dwellMs, dataRateName(dataRate_),
           (unsigned)channel_);
  out.println(line);
  out.println("  La molette doit tourner SANS ARRET : c'est la seule source de");
  out.println("  trafic, et chaque candidat n'a que quelques dizaines de ms.");
  Serial.flush();

  const uint32_t deadline = millis() + seconds * 1000UL;
  uint32_t tried = 0, frames = 0, passes = 0;
  uint32_t strongSamples = 0, rssiSamples = 0;
  uint8_t bestRssi = 0xFF;
  bool solved = false;

  while (!solved && (int32_t)(millis() - deadline) < 0) {
    passes++;

    // On balaie les OCTETS de temperature, sans presumer d'un pas. Generer les
    // candidats par pas de 25 K etait une hypothese heritee du Halo 2, et elle
    // etait ruineuse : sur cette grille, une seule valeur de toute la plage a
    // un octet bas valant 0x55 ou 0xAA. Le groupe 2 ne testait donc qu'une
    // temperature sur les trente-et-une possibles.
    // 2700 a 6500 K couvre 0x0A8C a 0x1964 : l'octet haut va de 0x0A a 0x19.
    // Une temperature imposee concentre tout le temps de mesure sur une seule
    // hypothese, au lieu de le diluer sur trente-deux. A utiliser des qu'un
    // indice designe une valeur precise.
    const uint16_t hiFrom = fixedKelvin ? (uint16_t)(fixedKelvin >> 8) : 0x0A;
    const uint16_t hiTo = fixedKelvin ? (uint16_t)(fixedKelvin >> 8) : 0x19;

    for (uint16_t ctHi = hiFrom; ctHi <= hiTo && !solved; ctHi++) {
      const uint16_t loStep = (group == 0) ? 1 : 85;  // groupe 2 : 0x55 puis 0xAA
      const uint16_t loFrom = fixedKelvin ? (uint16_t)(fixedKelvin & 0xFF)
                                          : ((group == 0) ? 0 : 85);
      const uint16_t loTo = fixedKelvin ? (uint16_t)(fixedKelvin & 0xFF) : 255;
      for (uint16_t ctLoI = loFrom; ctLoI <= loTo && !solved; ctLoI += loStep) {
        const uint8_t ctLo = (uint8_t)ctLoI;
        if (!fixedKelvin && group != 0 && ctLo != 0x55 && ctLo != 0xAA) continue;

      // Figer la luminosite arriere reduit le balayage d'un facteur cent et un.
      // Eteindre la lampe arriere la met a zero : une valeur connue, sans avoir
      // a la deviner, et la luminosite AVANT reste libre pour fournir du trafic.
      const uint8_t backFrom = (group == 0) ? 0 : ((fixedBack >= 0) ? (uint8_t)fixedBack : 0);
      const uint8_t backMax =
          (group == 0) ? 0 : ((fixedBack >= 0) ? (uint8_t)fixedBack : 100);
      for (uint8_t back = backFrom; back <= backMax && !solved; back++) {
        if ((int32_t)(millis() - deadline) >= 0) break;

        // Sur l'air la fenetre defile dans l'ordre du payload ; le registre se
        // remplit a l'envers.
        uint8_t reg3[3];
        if (group == 0) {
          reg3[0] = 0x01;
          reg3[1] = ctLo;
          reg3[2] = ctHi;
        } else {
          reg3[0] = ctLo;
          reg3[1] = ctHi;
          reg3[2] = back;
        }

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
        tried++;

        const uint32_t until = millis() + dwellMs;
        while ((int32_t)(millis() - until) < 0) {
          if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);

          // Temoin de trafic, independant du correlateur : RSSI2 est une mesure
          // temps reel. Sans lui, "zero accroche" ne distingue pas une mauvaise
          // hypothese d'une telecommande muette.
          const uint8_t rssi = radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER);
          if (rssi < 70) strongSamples++;
          rssiSamples++;
          if (rssi < bestRssi) bestRssi = rssi;

          if (radio.readRegister(REG_STATUS | CMD_READ_REGISTER) & STATUS_RX_DR) continue;

          uint8_t buf[32];
          radio.readFifo(buf, 32, false);
          radio.writeRegister(REG_IRQ1 | CMD_WRITE_REGISTER, IRQ_RX_DR);
          radio.command(CMD_FLUSH_RX_FIFO);
          frames++;

          snprintf(line, sizeof(line), "  accroche : ct %02X %02X, arriere %u %%",
                   (unsigned)ctHi, (unsigned)ctLo, (unsigned)back);
          out.println(line);
          for (uint8_t half = 0; half < 2; half++) {
            int n = snprintf(line, sizeof(line), half ? "        " : "    FIFO");
            for (uint8_t i = half * 16; i < (uint8_t)(half * 16 + 16); i++)
              n += snprintf(line + n, sizeof(line) - n, " %02X", buf[i]);
            out.println(line);
          }
          Serial.flush();

          snprintf(line, sizeof(line), "ct %02X %02X (%u K), arriere %u %%", (unsigned)ctHi,
                   (unsigned)ctLo, (unsigned)((ctHi << 8) | ctLo), (unsigned)back);
          if (scanCaptureForAddress(out, buf, 32, line)) solved = true;
        }
        if ((tried & 0x0F) == 0) delay(1);
      }
      }
    }
  }

  out.println();
  snprintf(line, sizeof(line), "  Bilan : %lu candidat(s) essaye(s) en %lu passe(s), %lu accroche(s).",
           (unsigned long)tried, (unsigned long)passes, (unsigned long)frames);
  out.println(line);
  const uint32_t strongPerMille = rssiSamples ? (strongSamples * 1000UL / rssiSamples) : 0;
  snprintf(line, sizeof(line), "  Trafic : signal fort sur %lu pour mille des mesures, pic %u dB.",
           (unsigned long)strongPerMille, (unsigned)bestRssi);
  out.println(line);
  if (strongPerMille < 5) {
    out.println("  >>> Quasiment aucun signal fort : la telecommande n'emettait");
    out.println("  pas, ou pas sur ce canal. Le resultat ne dit RIEN sur les");
    out.println("  hypotheses testees. Verifie la molette avant de recommencer.");
  }
  if (solved) {
    out.println("  ADRESSE TROUVEE. Enregistre-la avec la commande 'addr' affichee");
    out.println("  ci-dessus, puis 'sniff' pour verifier qu'on suit la lampe.");
  } else if (frames > 6) {
    out.println("  Beaucoup d'accroches sans adresse valide : la fenetre mord sur");
    out.println("  quelque chose de reel mais la lecture echoue. Envoie les vidages.");
  } else if (frames) {
    out.println("  Quelques accroches seulement. Un motif de 24 bits se retrouve");
    out.println("  par hasard environ une fois sur 16 millions de positions, et il");
    out.println("  en defile des dizaines de millions par minute : une poignee");
    out.println("  d'accroches sans CRC valide est le bruit attendu, pas un indice.");
  } else {
    out.println("  Aucune accroche. Verifie que la molette tournait, puis essaie");
    out.println("  l'autre groupe et l'autre debit.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Trouver le debit sans connaitre l'adresse
// ---------------------------------------------------------------------------

void BenqHalo::probeRateByGio3(Print &out, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];

  out.println();
  out.println("=== Quel est le debit de la source ? ===");
  out.println("  GIO3 ne s'anime que si la puce a DETECTE UN PREAMBULE, et cette");
  out.println("  detection depend du debit. Le debit qui fait sortir des");
  out.println("  transitions est donc celui de l'emetteur -- et cette mesure ne");
  out.println("  demande aucune adresse.");
  out.println("  Verifie par la balise : elle avait donne 14000 transitions.");
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  out.println();
  Serial.flush();

  const uint8_t io2Base = radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0;
  const uint8_t rates[3] = {DATARATE_125K, DATARATE_250K, DATARATE_500K};
  const uint8_t selectors[2] = {9, 14};
  const uint8_t saved = dataRate_;
  const uint32_t mask = 1UL << PIN_GIO3_TAP;

  uint32_t best = 0;
  uint8_t bestRate = saved;

  for (uint8_t r = 0; r < 3; r++) {
    dataRate_ = rates[r];

    for (uint8_t i = 0; i < 2; i++) {
      configForLoopback(radio, channel_, kCalRegAddr, true, dataRate_);
      radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, (uint8_t)(io2Base | selectors[i]));
      radio.enterRxMode();
      pinMode(PIN_GIO3_TAP, INPUT);

      uint32_t edges = 0, strong = 0, checks = 0;
      uint32_t last = REG_READ(GPIO_IN_REG) & mask;
      const uint32_t until = millis() + dwellMs;

      while ((int32_t)(millis() - until) < 0) {
        for (uint16_t burst = 0; burst < 512; burst++) {
          const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
          if (now != last) {
            edges++;
            last = now;
          }
        }
        if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
        if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
        checks++;
      }

      snprintf(line, sizeof(line), "  %-8s  GIO3S=%-2u : %lu transitions, signal fort %lu/%lu",
               dataRateName(rates[r]), (unsigned)selectors[i], (unsigned long)edges,
               (unsigned long)strong, (unsigned long)checks);
      out.println(line);
      Serial.flush();

      if (edges > best) {
        best = edges;
        bestRate = rates[r];
      }
    }
  }

  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, io2Base);
  dataRate_ = saved;

  out.println();
  if (best > 100) {
    snprintf(line, sizeof(line), "  Le debit de la source est %s (%lu transitions).",
             dataRateName(bestRate), (unsigned long)best);
    out.println(line);
    out.println("  Fixe-le avec 'debit', puis relance les chasses : jusqu'ici");
    out.println("  elles ecoutaient peut-etre au mauvais debit.");
  } else {
    out.println("  Aucun debit ne fait sortir de transitions. Si le signal fort");
    out.println("  etait bien present, alors la puce ne reconnait le preambule de");
    out.println("  cette source a aucun des trois debits -- ce qui pointerait vers");
    out.println("  un format de trame different, pas vers un reglage a corriger.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Trouver le canal sans connaitre l'adresse
// ---------------------------------------------------------------------------

void BenqHalo::probeChannelByGio3(Print &out, uint8_t from, uint8_t to, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];
  if (to > 83) to = 83;
  if (from > to) from = 0;

  out.println();
  out.println("=== Sur quel canal la source est-elle DEMODULABLE ? ===");
  out.println("  Le RSSI mesure une bande large : il entend un emetteur voisin.");
  out.println("  Le demodulateur, lui, est etroit. Un signal audible peut donc");
  out.println("  n'etre demodulable que sur un autre canal -- et GIO3 ne s'anime");
  out.println("  que si un preambule a ete reconnu, donc que si le canal est le");
  out.println("  bon. Cette mesure ne demande toujours aucune adresse.");
  snprintf(line, sizeof(line), "  Canaux %u a %u, debit %s, %lu ms chacun.", (unsigned)from,
           (unsigned)to, dataRateName(dataRate_), (unsigned long)dwellMs);
  out.println(line);
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER, ou lance la balise.");
  out.println();
  Serial.flush();

  const uint8_t io2Base = radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0;
  const uint8_t savedChannel = channel_;
  const uint32_t mask = 1UL << PIN_GIO3_TAP;

  // Temoin de reglage du modem. Le reset logiciel efface 15 des 19 valeurs
  // recommandees ; comme toute configuration commence par un reset, la mesure
  // doit dire elle-meme sur quels reglages elle a tourne.
  configForLoopback(radio, channel_, kCalRegAddr, true, dataRate_);
  uint8_t tunedTotal = 0;
  const uint8_t tunedOk = radio.registerVerify(&tunedTotal);
  snprintf(line, sizeof(line), "  Reglages analogiques en place au moment de la mesure : %u sur %u.",
           (unsigned)tunedOk, (unsigned)tunedTotal);
  out.println(line);
  out.println();
  Serial.flush();

  uint32_t best = 0, bestStrong = 0, bestStrongChecks = 1, totalStrong = 0, totalChecks = 1;
  uint8_t bestChannel = 0, bestStrongChannel = 0;
  uint8_t hits = 0;
  static uint32_t edgesPerChannel[84];
  static uint32_t strongPerChannel[84];
  memset(edgesPerChannel, 0, sizeof(edgesPerChannel));
  memset(strongPerChannel, 0, sizeof(strongPerChannel));

  for (uint16_t ch = from; ch <= to; ch++) {
    channel_ = (uint8_t)ch;
    configForLoopback(radio, channel_, kCalRegAddr, true, dataRate_);
    radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, (uint8_t)(io2Base | 14));
    radio.enterRxMode();
    pinMode(PIN_GIO3_TAP, INPUT);

    uint32_t edges = 0, strong = 0, checks = 0;
    uint32_t last = REG_READ(GPIO_IN_REG) & mask;
    const uint32_t until = millis() + dwellMs;

    while ((int32_t)(millis() - until) < 0) {
      for (uint16_t burst = 0; burst < 512; burst++) {
        const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
        if (now != last) {
          edges++;
          last = now;
        }
      }
      if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
      if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
      checks++;
    }

    // Memoriser tous les canaux : un seuil d'affichage masquerait une source a
    // faible rapport cyclique. La telecommande n'emet qu'environ 1 % du temps,
    // soit une trentaine de fronts par canal la ou la balise en donne 434.
    if (ch - from < 84) {
      edgesPerChannel[ch - from] = edges;
      strongPerChannel[ch - from] = strong;
    }
    if (edges > 50) hits++;
    if (edges > best) {
      best = edges;
      bestChannel = (uint8_t)ch;
    }
    // Le temoin doit etre rapporte SURTOUT quand le resultat principal est nul :
    // sans lui, "aucun canal" ne distingue pas une source muette d'une source
    // que la puce ne sait pas demoduler.
    if (strong > bestStrong) {
      bestStrong = strong;
      bestStrongChannel = (uint8_t)ch;
      bestStrongChecks = checks;
    }
    totalStrong += strong;
    totalChecks += checks;
    delay(1);
  }

  radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER, io2Base);
  channel_ = savedChannel;

  // Les cinq meilleurs canaux par nombre de fronts, sans aucun seuil.
  out.println("  Cinq meilleurs canaux par transitions, sans seuil :");
  for (uint8_t rank = 0; rank < 5; rank++) {
    uint8_t bestIdx = 0xFF;
    uint32_t bestEdges = 0;
    for (uint16_t i = 0; i <= (uint16_t)(to - from) && i < 84; i++)
      if (edgesPerChannel[i] > bestEdges) {
        bestEdges = edgesPerChannel[i];
        bestIdx = (uint8_t)i;
      }
    if (bestIdx == 0xFF) break;
    snprintf(line, sizeof(line), "    canal %-2u = %u MHz : %lu transitions, signal fort %lu",
             (unsigned)(from + bestIdx), (unsigned)(2400 + from + bestIdx),
             (unsigned long)bestEdges, (unsigned long)strongPerChannel[bestIdx]);
    out.println(line);
    edgesPerChannel[bestIdx] = 0;
  }
  Serial.flush();

  out.println();
  snprintf(line, sizeof(line),
           "  Canal le plus bruyant : %u (%u MHz), signal fort %lu/%lu.",
           (unsigned)bestStrongChannel, (unsigned)(2400 + bestStrongChannel),
           (unsigned long)bestStrong, (unsigned long)bestStrongChecks);
  out.println(line);
  snprintf(line, sizeof(line), "  Moyenne sur la bande : %lu pour mille.",
           (unsigned long)(totalStrong * 1000UL / totalChecks));
  out.println(line);
  out.println();
  if (best > 20) {
    snprintf(line, sizeof(line), "  Meilleur canal : %u (%u MHz), %lu transitions.",
             (unsigned)bestChannel, (unsigned)(2400 + bestChannel), (unsigned long)best);
    out.println(line);
    snprintf(line, sizeof(line), "  %u canal/canaux au-dessus du seuil. Fixe-le avec 'chan %u'.",
             (unsigned)hits, (unsigned)bestChannel);
    out.println(line);
  } else {
    out.println("  Aucun canal ne fait sortir de transitions. Si le signal fort");
    out.println("  etait present, la puce ne reconnait le preambule de cette");
    out.println("  source nulle part sur la bande, a ce debit. Essaie les deux");
    out.println("  autres debits avant de conclure.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  La sequence du pilote qui recoit vraiment
// ---------------------------------------------------------------------------

void BenqHalo::sweepChannelsPico(Print &out, uint8_t from, uint8_t to, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];
  if (to > 83) to = 83;
  if (from > to) from = 0;

  out.println();
  out.println("=== Balayage avec la sequence du pilote tiers ===");
  out.println("  Notre banc prouve que notre emetteur et notre recepteur");
  out.println("  s'accordent -- il ne prouve pas qu'ils sont regles comme une");
  out.println("  vraie BenQ. Une erreur commune aux deux passerait inapercue.");
  out.println();
  out.println("  On reprend donc ici la sequence exacte du pilote qui recoit");
  out.println("  reellement d'une Halo : AUCUN reset logiciel, et aucune des");
  out.println("  valeurs recommandees de banque 1 et 2. La puce garde l'etat");
  out.println("  qu'elle a en sortant de sa mise sous tension.");
  snprintf(line, sizeof(line), "  Canaux %u a %u, %lu ms chacun, 125 kbps.", (unsigned)from,
           (unsigned)to, (unsigned long)dwellMs);
  out.println(line);
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER.");
  out.println();
  Serial.flush();

  const uint32_t mask = 1UL << PIN_GIO3_TAP;
  uint32_t best = 0;
  uint8_t bestChannel = 0;

  for (uint16_t ch = from; ch <= to; ch++) {
    // Sequence de Termina1, a l'identique, sans reset logiciel.
    radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, 0x48);
    radio.command(CMD_LIGHT_SLEEP);
    delayMicroseconds(1000);
    radio.setBank(0);

    uint8_t mk = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
    radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mk | 0x01));
    const uint8_t rc1 = radio.readRegister(REG_RC1 | CMD_READ_REGISTER);
    if (rc1 & 0x80) radio.writeRegister(REG_RC1 | CMD_WRITE_REGISTER, (uint8_t)(rc1 & 0x7F));

    radio.command(CMD_LIGHT_SLEEP);
    radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, 0x48);
    radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, (uint8_t)ch);
    radio.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, 0x82);  // 125 kbps, adresse 4 octets
    radio.writeCommandData(CMD_WRITE_PTX_ADDRESS, kCalRegAddr, 4);
    radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x01);
    radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x04);
    radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x20);
    radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x3F);

    // Puis son chemin de reception : PRM_RX a 1 et entree en RX.
    mk = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
    radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mk | 0x01));
    radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER,
                        (uint8_t)((radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0) | 14));
    radio.clearInterrupts();
    radio.command(CMD_FLUSH_RX_FIFO);
    radio.command(CMD_RX_MODE);

    pinMode(PIN_GIO3_TAP, INPUT);
    uint32_t edges = 0, strong = 0, checks = 0;
    uint32_t last = REG_READ(GPIO_IN_REG) & mask;
    const uint32_t until = millis() + dwellMs;

    while ((int32_t)(millis() - until) < 0) {
      for (uint16_t burst = 0; burst < 512; burst++) {
        const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
        if (now != last) {
          edges++;
          last = now;
        }
      }
      if (radio.operationMode() != OMST_RX) radio.command(CMD_RX_MODE);
      if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
      checks++;
    }

    if (edges > 10) {
      snprintf(line, sizeof(line), "  canal %-2u = %u MHz : %lu transitions, signal fort %lu/%lu",
               (unsigned)ch, (unsigned)(2400 + ch), (unsigned long)edges, (unsigned long)strong,
               (unsigned long)checks);
      out.println(line);
      Serial.flush();
    }
    if (edges > best) {
      best = edges;
      bestChannel = (uint8_t)ch;
    }
    delay(1);
  }

  out.println();
  if (best > 10) {
    snprintf(line, sizeof(line), "  Meilleur canal : %u (%u MHz), %lu transitions.",
             (unsigned)bestChannel, (unsigned)(2400 + bestChannel), (unsigned long)best);
    out.println(line);
    out.println("  Cette sequence accroche la ou la notre echouait : c'est donc");
    out.println("  notre initialisation qui etait en cause, pas la lampe.");
  } else {
    out.println("  Rien non plus avec cette sequence. La difference entre nos");
    out.println("  deux initialisations n'explique donc pas le silence.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Largeur d'adresse : 3, 4 ou 5 octets
// ---------------------------------------------------------------------------

void BenqHalo::sweepAddressWidths(Print &out, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];

  out.println();
  out.println("=== Largeur d'adresse croisee avec le canal ===");
  out.println("  DM1 bits 7~6 : 01 = 3 octets, 10 = 4, 11 = 5. Une largeur fausse");
  out.println("  fait decouper la trame au mauvais endroit, et le correlateur ne");
  out.println("  peut alors rien accrocher -- quels que soient le canal et le");
  out.println("  debit. C'est une des trois variables qui restent.");
  snprintf(line, sizeof(line), "  3 largeurs x 84 canaux x %lu ms, debit %s.",
           (unsigned long)dwellMs, dataRateName(dataRate_));
  out.println(line);
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER.");
  out.println();
  Serial.flush();

  const uint8_t widths[3] = {3, 4, 5};
  const uint8_t awBits[3] = {0x40, 0x80, 0xC0};
  const uint8_t addr5[5] = {0x44, 0x33, 0x22, 0xE1, 0x11};
  const uint32_t mask = 1UL << PIN_GIO3_TAP;
  const uint8_t savedChannel = channel_;

  uint32_t best = 0;
  uint8_t bestChannel = 0, bestWidth = 0;

  for (uint8_t w = 0; w < 3; w++) {
    uint32_t widthBest = 0;
    uint8_t widthBestCh = 0;

    for (uint16_t ch = 0; ch <= 83; ch++) {
      radio.softwareReset();
      delay(2);
      radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
      radio.setBank(0);
      radio.writeRegister(REG_CFG1 | CMD_WRITE_REGISTER, CFG1_AGC_EN);
      radio.writeRegister(REG_RFCH | CMD_WRITE_REGISTER, (uint8_t)ch);
      radio.writeRegister(REG_DM1 | CMD_WRITE_REGISTER, (uint8_t)(awBits[w] | dataRate_));
      radio.writeCommandData(CMD_WRITE_PTX_ADDRESS, addr5, widths[w]);

      uint8_t mk = radio.readRegister(REG_MASK | CMD_READ_REGISTER);
      radio.writeRegister(REG_MASK | CMD_WRITE_REGISTER, (uint8_t)(mk | MASK_PRM_RX));
      radio.writeRegister(B0_DPL1 | CMD_WRITE_REGISTER, 0x00);
      radio.writeRegister(B0_DPL2 | CMD_WRITE_REGISTER, 0x00);
      radio.writeRegister(B0_RXPW0 | CMD_WRITE_REGISTER, 32);
      radio.writeRegister(REG_PKT1 | CMD_WRITE_REGISTER, 0x00);
      radio.writeRegister(B0_ENAA | CMD_WRITE_REGISTER, 0x00);
      radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER,
                          (uint8_t)((radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0) | 14));
      radio.clearInterrupts();
      radio.command(CMD_FLUSH_RX_FIFO);
      radio.enterRxMode();

      pinMode(PIN_GIO3_TAP, INPUT);
      uint32_t edges = 0, strong = 0, checks = 0;
      uint32_t last = REG_READ(GPIO_IN_REG) & mask;
      const uint32_t until = millis() + dwellMs;

      while ((int32_t)(millis() - until) < 0) {
        for (uint16_t burst = 0; burst < 512; burst++) {
          const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
          if (now != last) {
            edges++;
            last = now;
          }
        }
        if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
        if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
        checks++;
      }

      if (edges > 10) {
        snprintf(line, sizeof(line),
                 "  %u octets, canal %-2u = %u MHz : %lu transitions, fort %lu/%lu",
                 (unsigned)widths[w], (unsigned)ch, (unsigned)(2400 + ch), (unsigned long)edges,
                 (unsigned long)strong, (unsigned long)checks);
        out.println(line);
        Serial.flush();
      }
      if (edges > widthBest) {
        widthBest = edges;
        widthBestCh = (uint8_t)ch;
      }
      if (edges > best) {
        best = edges;
        bestChannel = (uint8_t)ch;
        bestWidth = widths[w];
      }
      delay(1);
    }

    snprintf(line, sizeof(line), "  -- adresse de %u octets : maximum %lu transitions (canal %u)",
             (unsigned)widths[w], (unsigned long)widthBest, (unsigned)widthBestCh);
    out.println(line);
    Serial.flush();
  }

  channel_ = savedChannel;

  out.println();
  if (best > 10) {
    snprintf(line, sizeof(line), "  Meilleur : adresse de %u octets, canal %u (%u MHz), %lu fronts.",
             (unsigned)bestWidth, (unsigned)bestChannel, (unsigned)(2400 + bestChannel),
             (unsigned long)best);
    out.println(line);
  } else {
    out.println("  Aucune largeur ne change quoi que ce soit. Cette variable est");
    out.println("  eliminee : il reste l'excursion de frequence et les reglages de");
    out.println("  modem non documentes.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Balayage d'un registre de modem
// ---------------------------------------------------------------------------

void BenqHalo::sweepModemRegister(Print &out, int bank, uint8_t reg, uint32_t dwellMs) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];
  const bool cfoMode = (bank < 0);
  const uint16_t count = cfoMode ? 64 : 256;

  out.println();
  out.println("=== Balayage d'un registre de modem ===");
  if (cfoMode) {
    out.println("  Cible : les six bits de poids faible de CFO1 (0x21), registre");
    out.println("  nomme 'Carrier Frequency Offset Control'. Le datasheet les dit");
    out.println("  reserves, mais l'intitule du registre suggere un decalage de");
    out.println("  porteuse -- ce qui expliquerait qu'un emetteur audible reste");
    out.println("  indemodulable. 64 valeurs.");
  } else {
    snprintf(line, sizeof(line), "  Cible : banque %d, registre 0x%02X, 256 valeurs.", bank,
             (unsigned)reg);
    out.println(line);
  }
  snprintf(line, sizeof(line), "  Canal %u, debit %s, %lu ms par valeur.", (unsigned)channel_,
           dataRateName(dataRate_), (unsigned long)dwellMs);
  out.println(line);
  out.println("  >>> TOURNE LA MOLETTE SANS T'ARRETER PENDANT TOUT LE BALAYAGE.");
  out.println();
  Serial.flush();

  const uint32_t mask = 1UL << PIN_GIO3_TAP;
  uint32_t best = 0;
  uint16_t bestValue = 0;
  uint32_t totalStrong = 0, totalChecks = 1;

  for (uint16_t v = 0; v < count; v++) {
    configForLoopback(radio, channel_, kCalRegAddr, true, dataRate_);

    if (cfoMode) {
      const uint8_t cfo = radio.readRegister(B0_CFO1 | CMD_READ_REGISTER);
      // Conserver AMBLE2 (bit 6) et ne toucher qu'aux six bits bas.
      radio.writeRegister(B0_CFO1 | CMD_WRITE_REGISTER,
                          (uint8_t)((cfo & 0xC0) | (uint8_t)(v & 0x3F)));
    } else {
      radio.setBank((uint8_t)bank);
      radio.writeRegister(reg | CMD_WRITE_REGISTER, (uint8_t)v);
      radio.setBank(0);
    }

    radio.writeRegister(REG_IO2 | CMD_WRITE_REGISTER,
                        (uint8_t)((radio.readRegister(REG_IO2 | CMD_READ_REGISTER) & 0xF0) | 14));
    radio.enterRxMode();
    pinMode(PIN_GIO3_TAP, INPUT);

    uint32_t edges = 0, strong = 0, checks = 0;
    uint32_t last = REG_READ(GPIO_IN_REG) & mask;
    const uint32_t until = millis() + dwellMs;

    while ((int32_t)(millis() - until) < 0) {
      for (uint16_t burst = 0; burst < 512; burst++) {
        const uint32_t now = REG_READ(GPIO_IN_REG) & mask;
        if (now != last) {
          edges++;
          last = now;
        }
      }
      if (radio.operationMode() != OMST_RX) radio.enterRxMode(300);
      if (radio.readRegister(B0_RSSI2 | CMD_READ_REGISTER) < 70) strong++;
      checks++;
    }

    totalStrong += strong;
    totalChecks += checks;

    if (edges > 10) {
      snprintf(line, sizeof(line), "  valeur 0x%02X : %lu transitions, signal fort %lu/%lu   <<<",
               (unsigned)v, (unsigned long)edges, (unsigned long)strong, (unsigned long)checks);
      out.println(line);
      Serial.flush();
    }
    if (edges > best) {
      best = edges;
      bestValue = v;
    }
    delay(1);
  }

  // Remettre la puce dans un etat sain.
  configForLoopback(radio, channel_, kCalRegAddr, true, dataRate_);

  out.println();
  snprintf(line, sizeof(line), "  Trafic pendant le balayage : %lu pour mille de signal fort.",
           (unsigned long)(totalStrong * 1000UL / totalChecks));
  out.println(line);
  if (best > 10) {
    snprintf(line, sizeof(line), "  Meilleure valeur : 0x%02X, %lu transitions.",
             (unsigned)bestValue, (unsigned long)best);
    out.println(line);
    out.println("  Ce reglage fait accrocher le demodulateur : c'est lui qu'on");
    out.println("  cherchait. Note-le, il conditionne tout le reste.");
  } else {
    out.println("  Aucune valeur ne fait accrocher. Si le trafic etait present,");
    out.println("  ce registre n'est pas en cause.");
  }
  out.println();
}

// ---------------------------------------------------------------------------
//  Les valeurs recommandees survivent-elles au reset logiciel ?
// ---------------------------------------------------------------------------

void BenqHalo::compareAfterReset(Print &out) {
  if (!radio.present()) {
    out.println("BM5602 absent.");
    return;
  }

  char line[176];

  struct RegRef {
    uint8_t bank;
    uint8_t addr;
    uint8_t recommended;
  };
  // Les valeurs recommandees par Holtek, telles que registerConfigure() les ecrit.
  const RegRef refs[] = {
      {0, 0x0D, 0x20}, {0, 0x0F, 0x14}, {0, 0x16, 0x66}, {0, 0x17, 0xAA}, {0, 0x18, 0x45},
      {1, 0x20, 0x0C}, {1, 0x21, 0x03}, {1, 0x23, 0x10}, {1, 0x25, 0xCC}, {1, 0x26, 0x4C},
      {1, 0x27, 0x80}, {2, 0x28, 0xA0}, {2, 0x2D, 0x18}, {2, 0x2E, 0xEC}, {2, 0x36, 0x03},
      {2, 0x38, 0x0A}, {2, 0x39, 0x12}, {2, 0x3B, 0x94}, {2, 0x3C, 0x43},
  };
  const uint8_t n = sizeof(refs) / sizeof(refs[0]);

  out.println();
  out.println("=== Les valeurs recommandees survivent-elles au reset ? ===");
  out.println("  Toutes nos configurations commencent par un reset logiciel. Si");
  out.println("  celui-ci restaure les valeurs d'usine, alors les valeurs que nous");
  out.println("  ecrivons au demarrage ne sont JAMAIS actives pendant les chasses,");
  out.println("  et les balayer reviendrait a explorer quelque chose d'inerte.");
  out.println();

  uint8_t before[32];
  for (uint8_t i = 0; i < n; i++) {
    radio.setBank(refs[i].bank);
    before[i] = radio.readRegister(refs[i].addr | CMD_READ_REGISTER);
  }
  radio.setBank(0);

  radio.softwareReset();
  delay(20);
  radio.writeRegister(REG_IO1 | CMD_WRITE_REGISTER, IO1_4WIRE_SPI);
  radio.setBank(0);

  uint8_t changed = 0, matchesRecommended = 0;
  out.println("  banque reg   avant   apres reset   recommande");
  for (uint8_t i = 0; i < n; i++) {
    radio.setBank(refs[i].bank);
    const uint8_t after = radio.readRegister(refs[i].addr | CMD_READ_REGISTER);
    radio.setBank(0);

    if (after != before[i]) changed++;
    if (after == refs[i].recommended) matchesRecommended++;

    snprintf(line, sizeof(line), "    %u    0x%02X   0x%02X      0x%02X          0x%02X   %s",
             (unsigned)refs[i].bank, (unsigned)refs[i].addr, (unsigned)before[i], (unsigned)after,
             (unsigned)refs[i].recommended, (after != before[i]) ? "EFFACE" : "");
    out.println(line);
    Serial.flush();
  }

  out.println();
  snprintf(line, sizeof(line), "  %u registre(s) sur %u modifies par le reset ;", (unsigned)changed,
           (unsigned)n);
  out.println(line);
  snprintf(line, sizeof(line), "  %u sur %u portent encore la valeur recommandee apres reset.",
           (unsigned)matchesRecommended, (unsigned)n);
  out.println(line);
  out.println();
  if (changed > n / 2) {
    out.println("  Le reset logiciel efface l'essentiel de ces reglages. Ils ne");
    out.println("  sont donc PAS actifs pendant les chasses, qui commencent toutes");
    out.println("  par un reset -- inutile de les balayer.");
  } else {
    out.println("  Ces reglages survivent au reset : ils sont bien actifs pendant");
    out.println("  les chasses, et les balayer a donc un sens.");
  }
  out.println();
}
