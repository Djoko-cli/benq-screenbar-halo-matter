#include "halo1_map.h"

#include <math.h>

namespace halo1 {

// ---------------------------------------------------------------------------
//  Luminosite : niveau Matter 1..254 -> valeur brute 0x4C..0xFE
//
//  raw(L) = 0x4C + round(178 * ((L-1)/253)^gamma), raw(0) = raw(1). La lampe
//  parait logarithmique (PROTOCOL.md) : gamma 2 met 0x80 vers le niveau 138.
//  Calcul en double : en float, l'arrondi de certains points tomberait a
//  moins d'un ulp de 0,5 et pourrait differer de l'hote a la carte.
// ---------------------------------------------------------------------------

static uint8_t sRaw[255];    // indice = niveau Matter 0..254
static float sGamma = 0.0f;  // 0 : table pas encore construite

static void ensureTable() {
  if (sGamma <= 0.0f) mapInit(2.0f);  // repli (decision A3), sans HALO1_LEVEL_GAMMA
}

void mapInit(float gamma) {
  if (!(gamma > 0.0f)) gamma = 1.0f;  // 0, negatif ou NaN : lineaire
  for (unsigned l = 1; l <= 254; l++) {
    unsigned v;
    if (gamma == 1.0f)
      v = ((l - 1) * 178 + 126) / 253;  // formule entiere exacte, sans flottant
    else
      v = (unsigned)floor(178.0 * pow((double)(l - 1) / 253.0, (double)gamma) + 0.5);
    if (v > 178) v = 178;
    sRaw[l] = (uint8_t)(kBrightMin + v);
    if (l > 1 && sRaw[l] < sRaw[l - 1]) sRaw[l] = sRaw[l - 1];  // monotone quoi qu'il arrive
  }
  sRaw[0] = sRaw[1];
  sGamma = gamma;
}

float mapGamma() {
  ensureTable();
  return sGamma;
}

uint8_t rawFromLevel(uint8_t level) {
  ensureTable();
  return sRaw[level > 254 ? 254 : level];
}

uint8_t levelFromRaw(uint8_t raw) {
  ensureTable();
  // Plus petit L tel que raw(L) >= raw ; au-dela de 0xFE, 254.
  uint8_t lo = 1, hi = 254;
  while (lo < hi) {
    const uint8_t mid = (uint8_t)((lo + hi) / 2);
    if (sRaw[mid] >= raw)
      hi = mid;
    else
      lo = (uint8_t)(mid + 1);
  }
  return lo;
}

// ---------------------------------------------------------------------------
//  Temperature : lineaire en mireds, 0x00 (froid) = 153, 0x64 (chaud) = 370.
//  L'aller-retour temp -> mired -> temp est exact pour les 101 valeurs.
// ---------------------------------------------------------------------------

uint8_t tempFromMired(uint16_t m) {
  if (m < kMiredCold) m = kMiredCold;
  if (m > kMiredWarm) m = kMiredWarm;
  return (uint8_t)(((m - kMiredCold) * 100u + 108) / 217);
}

uint16_t miredFromTemp(uint8_t t) {
  if (t > kTempMax) t = kTempMax;
  return (uint16_t)(kMiredCold + (t * 217u + 50) / 100);
}

// Affichage stable : la valeur qu'un controleur a ecrite ne saute jamais vers
// une voisine qui donne la meme valeur brute.
uint8_t displayLevel(uint8_t attr, uint8_t bright) {
  if (attr >= 1 && attr <= 254 && rawFromLevel(attr) == bright) return attr;
  return levelFromRaw(bright);
}

uint16_t displayMired(uint16_t attr, uint8_t temp) {
  if (attr >= kMiredCold && attr <= kMiredWarm && tempFromMired(attr) == temp) return attr;
  return miredFromTemp(temp);
}

// ---------------------------------------------------------------------------
//  Regles d'intention (E.4). f / b : la lampe avant / arriere doit-elle etre
//  allumee, d'apres l'intention si elle est la, sinon d'apres la consigne.
// ---------------------------------------------------------------------------

Resolution resolveMatter(const State &base, const MatterIntents &in, uint8_t memoryLamps) {
  Resolution r;
  r.target = base;
  const uint8_t mem = (memoryLamps & F_LAMPS) ? (uint8_t)(memoryLamps & F_LAMPS) : F_LAMPS;
  const uint8_t flagsIn = in.has & (IN_POWER | IN_FRONT | IN_BACK);
  const bool sel = (in.has & (IN_FRONT | IN_BACK)) != 0;
  const bool baseF = base.power && (base.lamps & F_FRONT);
  const bool baseB = base.power && (base.lamps & F_BACK);
  const bool f = (in.has & IN_FRONT) ? in.front : baseF;
  const bool b = (in.has & IN_BACK) ? in.back : baseB;

  if ((in.has & IN_POWER) && !in.power) {  // R1 : l'extinction gagne toujours
    r.target.power = false;
    r.target.lamps = mem;
  } else if (sel && (f || b)) {  // R2 : les lampes demandees
    r.target.power = true;
    r.target.lamps = (uint8_t)((f ? F_FRONT : 0) | (b ? F_BACK : 0));
  } else if (!sel && base.power) {  // R2' : deja allumee, marche et lampes inchangees
  } else if (in.has & IN_POWER) {   // R3a : EP1 allume, derniere selection
    r.target.power = true;
    r.target.lamps = mem;
  } else if (sel) {  // R3b : plus aucune lampe allumee
    r.target.power = false;
    r.target.lamps = mem;
  }
  if (flagsIn) r.fields |= FLD_FLAGS;  // toujours emis, meme deja dans cet etat
  // Niveau et mireds : retenus aussi lampe eteinte, livres a l'allumage.
  if (in.has & IN_LEVEL) {
    r.target.bright = rawFromLevel(in.level ? in.level : 1);
    r.fields |= FLD_BRIGHT;
  }
  if (in.has & IN_MIREDS) {
    r.target.temp = tempFromMired(in.mireds);
    r.fields |= FLD_TEMP;
  }
  // Garde-fou de groupe (A2) : A arrive avec une intention marche ou lampe dans
  // la meme fenetre -> commande de piece ou tuile regroupee, on l'ignore.
  r.fireAuto = (in.has & IN_AUTO) && r.target.power && !flagsIn;
  return r;
}

// ---------------------------------------------------------------------------
//  Memoire de selection
// ---------------------------------------------------------------------------

void SelectionMemory::update(const State &target, uint32_t nowMs, uint32_t stableMs) {
  const uint8_t lamps = (target.lamps & F_LAMPS) ? (uint8_t)(target.lamps & F_LAMPS) : F_LAMPS;
  if (!target.power) {
    cur_ = stable_ = lamps;
    return;
  }
  if (lamps != cur_) {
    cur_ = lamps;
    since_ = nowMs;
  } else if ((uint32_t)(nowMs - since_) >= stableMs) {
    stable_ = cur_;
  }
}

uint8_t SelectionMemory::memory(const State &target) const {
  if (target.power) return stable_;
  return (target.lamps & F_LAMPS) ? (uint8_t)(target.lamps & F_LAMPS) : F_LAMPS;
}

}  // namespace halo1
