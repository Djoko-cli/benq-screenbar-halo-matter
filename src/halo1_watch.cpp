#include "halo1_watch.h"

namespace halo1 {

const char *relaunchText(Relaunch c) {
  switch (c) {
    case Relaunch::None: return "-";
    case Relaunch::Verify: return "verif.";
    case Relaunch::TxTimeout: return "delais";
    case Relaunch::RxNoise: return "bruit";
  }
  return "?";
}

void ChipWatch::txVerdict(TxSeen v) {
  switch (v) {
    case TxSeen::Ack:  // TX_DS : la puce emet et la lampe repond
      timeouts_ = 0;
      recovered();
      break;
    case TxSeen::MaxRt:  // puce saine, pas d'accuse : jamais un symptome
      timeouts_ = 0;
      break;
    case TxSeen::Timeout:
      if (timeouts_ < 0xFF) timeouts_++;
      break;
    case TxSeen::Refused: break;  // rien d'emis : ni pour ni contre
  }
}

void ChipWatch::rxFrame(bool crcOk, uint32_t nowMs) {
  roll(nowMs);
  if (winFrames_ < 0xFFFF) winFrames_++;
  if (!crcOk && winBad_ < 0xFFFF) winBad_++;
  // Des le seuil atteint, sans attendre la fin de la fenetre ; la fenetre reste
  // un deluge jusqu'a sa fin, meme si des trames justes la diluent ensuite.
  if (!winMet_ && winFrames_ >= kNoiseMinFrames &&
      (uint32_t)winBad_ * 100u >= (uint32_t)winFrames_ * kNoiseBadPct) {
    winMet_ = noisy_ = true;
    flood_ = Flood{winFrames_, winBad_, nowMs - winAt_};
  }
}

void ChipWatch::openWindow(uint32_t nowMs) {
  winOpen_ = true;
  winMet_ = false;
  winAt_ = nowMs;
  winFrames_ = winBad_ = 0;
}

// Fenetres consecutives de kNoiseWindowMs (la suivante part a la fermeture de
// la precedente, due() tournant a chaque tour). Une fenetre close decide : un
// deluge garde l'alerte, une fenetre calme la leve, et elle vaut guerison si
// plus de la moitie de ses trames ont le CRC juste (60 fausses et une juste :
// une puce malade sous le seuil du deluge, pas une guerison). Plus d'une
// fenetre ecoulee depuis l'ouverture : la plus recente etait vide, l'alerte
// tombe.
void ChipWatch::roll(uint32_t nowMs) {
  if (!winOpen_) {
    openWindow(nowMs);
    return;
  }
  const uint32_t age = nowMs - winAt_;
  if (age < kNoiseWindowMs) return;
  noisy_ = winMet_ && age < 2 * kNoiseWindowMs;
  if (!winMet_ && (uint32_t)winBad_ * 2u < winFrames_) recovered();
  openWindow(nowMs);
}

void ChipWatch::recovered() {
  unrecovered_ = 0;
  failed_ = false;
}

Relaunch ChipWatch::symptom() const {
  if (timeouts_ >= kTimeoutRun) return Relaunch::TxTimeout;
  if (noisy_) return Relaunch::RxNoise;
  return Relaunch::None;
}

Relaunch ChipWatch::due(uint32_t nowMs) {
  roll(nowMs);
  // Attente echue oubliee : elle ne revient pas 49,7 jours plus tard.
  if (holding_ && nowMs - lastAt_ >= gapMs()) holding_ = false;
  const Relaunch c = symptom();
  if (c == Relaunch::None) return c;
  if (unrecovered_ >= kFruitless) failed_ = true;  // le symptome revient malgre les relances
  return holding_ ? Relaunch::None : c;
}

void ChipWatch::relaunched(Relaunch cause, uint32_t nowMs) {
  holding_ = true;
  lastAt_ = nowMs;
  if (unrecovered_ < 0xFF) unrecovered_++;
  if (cause != Relaunch::None) {
    counts_[(uint8_t)cause]++;
    hist_[histIdx_] = Entry{cause, nowMs};
    histIdx_ = (uint8_t)((histIdx_ + 1) % kHistN);
    if (histN_ < kHistN) histN_++;
  }
  forget(nowMs);
}

void ChipWatch::forget(uint32_t nowMs) {
  timeouts_ = 0;
  noisy_ = false;
  openWindow(nowMs);
}

void ChipWatch::clearCounts() {
  for (uint32_t &n : counts_) n = 0;
  histIdx_ = histN_ = 0;
}

uint32_t ChipWatch::waitMs(uint32_t nowMs) const {
  if (!holding_) return 0;
  const uint32_t gone = nowMs - lastAt_, gap = gapMs();
  return gone >= gap ? 0 : gap - gone;
}

uint32_t ChipWatch::total() const {
  uint32_t n = 0;
  for (uint32_t c : counts_) n += c;
  return n;
}

uint8_t ChipWatch::history(Entry *out, uint8_t max) const {
  uint8_t n = 0;
  for (; n < histN_ && n < max; n++) out[n] = hist_[(histIdx_ + kHistN - 1 - n) % kHistN];
  return n;
}

}  // namespace halo1
