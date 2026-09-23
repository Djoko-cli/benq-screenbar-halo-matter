#pragma once
// Correspondances Matter <-> charge Halo 1 et regles d'intention Matter. Code pur.
#include <stdint.h>
#include "halo1_proto.h"

namespace halo1 {
constexpr uint16_t kMiredCold = 153;  // temp 0x00 (le plus froid) ; ~6536 K NOMINAL, non mesure
constexpr uint16_t kMiredWarm = 370;  // temp 0x64 (le plus chaud) ; ~2703 K NOMINAL, non mesure

// Tant que mapInit n'a pas ete appele, la table est construite a gamma 2,0
// (decision A3) au premier usage. Ce repli ignore HALO1_LEVEL_GAMMA : c'est
// Halo1Lamp::begin (C4, les deux builds) qui appelle mapInit(HALO1_LEVEL_GAMMA).
void mapInit(float gamma);            // table 254 entrees ; gamma 1.0 = formule lineaire exacte
float mapGamma();
uint8_t rawFromLevel(uint8_t level);  // 0..254 -> 0x4C..0xFE (0 traite comme 1)
uint8_t levelFromRaw(uint8_t raw);    // plus petit L tel que rawFromLevel(L) >= raw
uint8_t tempFromMired(uint16_t m);    // ((clamp(m,153,370) - 153) * 100 + 108) / 217
uint16_t miredFromTemp(uint8_t t);    // 153 + (min(t,100) * 217 + 50) / 100
// Affichage stable (E.2) : la valeur de l'attribut si elle donne deja la consigne,
// sinon la valeur canonique. Idempotent ; hors plage (niveau 0, mireds hors
// 153..370) -> canonique.
uint8_t displayLevel(uint8_t attr, uint8_t bright);
uint16_t displayMired(uint16_t attr, uint8_t temp);

enum : uint8_t { IN_POWER = 1, IN_FRONT = 2, IN_BACK = 4, IN_LEVEL = 8, IN_MIREDS = 16, IN_AUTO = 32 };
struct MatterIntents {  // derniere valeur gagne dans la fenetre de coalescence
  uint8_t has = 0;
  bool power = false, front = false, back = false;
  uint8_t level = 0;
  uint16_t mireds = 0;
};
struct Resolution { State target; uint8_t fields = 0; bool fireAuto = false; };
Resolution resolveMatter(const State &base, const MatterIntents &in, uint8_t memoryLamps);

// Memoire de selection : derniere selection restee allumee >= stableMs.
class SelectionMemory {
 public:
  void reset(uint8_t lamps) {
    cur_ = stable_ = (lamps & F_LAMPS) ? (uint8_t)(lamps & F_LAMPS) : F_LAMPS;
    since_ = 0;
  }
  void update(const State &target, uint32_t nowMs, uint32_t stableMs);
  uint8_t memory(const State &target) const;  // allumee : stable_ ; eteinte : target.lamps
 private:
  uint8_t cur_ = F_LAMPS, stable_ = F_LAMPS;
  uint32_t since_ = 0;
};
}  // namespace halo1
