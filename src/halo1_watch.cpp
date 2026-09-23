#include "halo1_watch.h"

namespace halo1 {

const char *relaunchText(Relaunch c) {
  switch (c) {
    case Relaunch::None: return "-";
    case Relaunch::Verify: return "verif.";
    case Relaunch::TxTimeout: return "delais";
    case Relaunch::RxNoise: return "bruit";
    case Relaunch::RxDeaf: return "sourde";
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
      winTimeout_ = true;  // pas de guerison apres surdite sur cette fenetre
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

void ChipWatch::rxRearms(uint32_t offRx, uint32_t inRx, uint32_t nowMs) {
  roll(nowMs);
  uint16_t &s = deafSlice_[deafIdx_];
  s = (uint16_t)(offRx >= 0xFFFFu - s ? 0xFFFFu : s + offRx);
  winOffRx_ = offRx >= 0xFFFFFFFFu - winOffRx_ ? 0xFFFFFFFFu : winOffRx_ + offRx;
  winInRx_ = inRx >= 0xFFFFFFFFu - winInRx_ ? 0xFFFFFFFFu : winInRx_ + inRx;
}

void ChipWatch::openWindow(uint32_t nowMs) {
  winOpen_ = true;
  winMet_ = false;
  winAt_ = nowMs;
  winFrames_ = winBad_ = 0;
  winOffRx_ = winInRx_ = 0;
  winTimeout_ = false;
}

// Fenetre glissante de la surdite : la tranche en cours et les kDeafSlices - 1
// precedentes, soit moins de kDeafWindowMs. Des tranches echues sont remises a
// zero en avancant ; plus d'une fenetre sans appel : tout repart de zero.
void ChipWatch::rollDeaf(uint32_t nowMs) {
  const uint32_t steps = (nowMs - deafAt_) / kDeafSliceMs;
  if (!steps) return;
  if (steps >= kDeafSlices) {
    clearDeaf(nowMs);
    return;
  }
  for (uint32_t i = 0; i < steps; i++) {
    deafIdx_ = (uint8_t)((deafIdx_ + 1) % kDeafSlices);
    deafSlice_[deafIdx_] = 0;
  }
  deafAt_ += steps * kDeafSliceMs;
}

void ChipWatch::clearDeaf(uint32_t nowMs) {
  for (uint16_t &n : deafSlice_) n = 0;
  deafIdx_ = 0;
  deafAt_ = nowMs;
}

uint32_t ChipWatch::deafRearms() const {
  uint32_t n = 0;
  for (uint16_t c : deafSlice_) n += c;
  return n;
}

// Du debut de la plus ancienne tranche non vide a maintenant : les rearmements
// comptes sont tous venus dans cet intervalle (une seconde de trop au plus).
uint32_t ChipWatch::deafSpan(uint32_t nowMs) const {
  for (uint8_t k = kDeafSlices; k-- > 0;) {
    if (deafSlice_[(deafIdx_ + kDeafSlices - k) % kDeafSlices]) return nowMs - (deafAt_ - k * kDeafSliceMs);
  }
  return 0;
}

// Fenetres consecutives de kNoiseWindowMs (la suivante part a la fermeture de
// la precedente, due() tournant a chaque tour). Une fenetre close decide : un
// deluge garde l'alerte, une fenetre calme la leve, et elle vaut guerison si
// plus de la moitie de ses trames ont le CRC juste (60 fausses et une juste :
// une puce malade sous le seuil du deluge, pas une guerison). Plus d'une
// fenetre ecoulee depuis l'ouverture : la plus recente etait vide, l'alerte
// tombe. Apres une relance pour surdite, une fenetre sans CRC faux vaut aussi
// guerison si la puce y a dit RX (kCalmMinInRx rearmements periodiques) sans
// en retomber (kCalmMaxOffRx hors RX au plus) : sans trame, en piece calme,
// c'est la seule preuve que ce symptome est parti. Pas si l'emission reste
// malade : un delai dans la fenetre, ou une serie de delais pas encore close
// par un accuse ou un MAX_RT, l'empeche. La surdite se lit, elle, sur sa
// fenetre glissante.
void ChipWatch::roll(uint32_t nowMs) {
  rollDeaf(nowMs);
  if (!winOpen_) {
    openWindow(nowMs);
    return;
  }
  const uint32_t age = nowMs - winAt_;
  if (age < kNoiseWindowMs) return;
  noisy_ = winMet_ && age < 2 * kNoiseWindowMs;
  if (!winMet_ && (uint32_t)winBad_ * 2u < winFrames_) recovered();
  else if (lastCause_ == Relaunch::RxDeaf && !winTimeout_ && !timeouts_ && !winBad_ && winOffRx_ <= kCalmMaxOffRx &&
           winInRx_ >= kCalmMinInRx)
    recovered();
  openWindow(nowMs);
}

void ChipWatch::recovered() {
  unrecovered_ = 0;
  failed_ = false;
}

Relaunch ChipWatch::symptom() const {
  if (timeouts_ >= kTimeoutRun) return Relaunch::TxTimeout;
  if (noisy_) return Relaunch::RxNoise;
  if (deafRearms() >= kDeafMinRearms) return Relaunch::RxDeaf;
  return Relaunch::None;
}

Relaunch ChipWatch::due(uint32_t nowMs) {
  roll(nowMs);
  // Attente echue oubliee : elle ne revient pas 49,7 jours plus tard.
  if (holding_ && nowMs - lastAt_ >= gapMs()) holding_ = false;
  const Relaunch c = symptom();
  if (c == Relaunch::None) return c;
  if (unrecovered_ >= kFruitless) failed_ = true;  // le symptome revient malgre les relances
  if (holding_) return Relaunch::None;
  if (c == Relaunch::RxDeaf) deaf_ = Deaf{deafRearms(), deafSpan(nowMs)};
  return c;
}

void ChipWatch::relaunched(Relaunch cause, uint32_t nowMs) {
  holding_ = true;
  lastAt_ = nowMs;
  if (unrecovered_ < 0xFF) unrecovered_++;
  if (cause != Relaunch::None) {
    lastCause_ = cause;
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
  clearDeaf(nowMs);
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
