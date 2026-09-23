#pragma once
// Correspondances Matter <-> charge Halo 1 et regles d'intention Matter. Code pur.
#include <stdint.h>
#include "halo1_proto.h"

namespace halo1 {
constexpr uint16_t kMiredCold = 153;  // temp 0x00 (le plus froid) ; ~6536 K NOMINAL, non mesure
constexpr uint16_t kMiredWarm = 370;  // temp 0x64 (le plus chaud) ; ~2703 K NOMINAL, non mesure

// Plancher des niveaux RAPPORTES a Matter. Apple Home affiche CurrentLevel en
// pourcentage entier de 254 : le niveau 1 (0,39 %) y devient 0 %, et une
// lumiere allumee a 0 % s'y affiche au MAXIMUM (terrain du 23/09 : lampe au
// minimum 4C, reglee a la molette, montree pleine). Le niveau 2 (0,79 %)
// depend du sens de l'arrondi ; 3 (1,18 %) donne 1 %, arrondi ou tronque.
// Les niveaux 0..3 ecrits par un controleur donnent tous 4C, le minimum.
constexpr uint8_t kMatterLevelFloor = 3;

// Tant que mapInit n'a pas ete appele, la table est construite a gamma 2,0
// (decision A3) au premier usage. Ce repli ignore HALO1_LEVEL_GAMMA : c'est
// Halo1Lamp::begin (C4, les deux builds) qui appelle mapInit(HALO1_LEVEL_GAMMA).
void mapInit(float gamma);            // table 254 entrees ; gamma 1.0 = formule lineaire exacte
float mapGamma();
uint8_t rawFromLevel(uint8_t level);  // 0..254 -> 0x4C..0xFE (0..kMatterLevelFloor -> 0x4C)
// Niveau rapporte : plus petit L >= kMatterLevelFloor tel que rawFromLevel(L) >= raw.
uint8_t levelFromRaw(uint8_t raw);
uint8_t tempFromMired(uint16_t m);    // ((clamp(m,153,370) - 153) * 100 + 108) / 217
uint16_t miredFromTemp(uint8_t t);    // 153 + (min(t,100) * 217 + 50) / 100
// Affichage stable (E.2) : la valeur de l'attribut si elle donne deja la consigne,
// sinon la valeur canonique. Idempotent ; hors plage (niveau sous le plancher,
// mireds hors 153..370) -> canonique. Jamais un niveau sous kMatterLevelFloor.
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
// Regles E.4. Un niveau venu avec EP1 off, ou avec EP1 on sans rien changer a
// la consigne (meme valeur brute, ou niveau affiche pour elle), est ecarte :
// c'est la pile qui l'ecrit (LevelControl avec la fonction OnOff).
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
