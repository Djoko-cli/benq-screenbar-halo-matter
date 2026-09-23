<!-- Plan produit par le workflow halo1-driver-design (23/09/2026) : 4 lecteurs,
3 conceptions, 2 juges, 1 synthese. Relu par Claude. -->

> **Decisions de Majid (23/09/2026)** :
> - A1 = **(a)** : EP1 « Halo » (lumiere a temperature de couleur), EP2 « Halo avant »
>   et EP3 « Halo arriere » en `MatterOnOffLight`, EP4 « Halo auto » momentane.
> - A2 = **(a)** : bouton A expose, avec le garde-fou de groupe.
> - A3 = **(a)** (choix technique de Claude) : table gamma 2,0, reglable au banc.
> - A4 = **(a)** (choix technique de Claude) : allumage et changement de lampes portes
>   par la trame de luminosite affichee ; repli (b) si T2 echoue.
> - Suite (23/09, firmware 0.3.0) : EP4 « Halo auto » **desactive pour l'instant**
>   (`HALO1_EXPOSE_AUTO 0` par defaut, code garde, EP1 a EP3 inchanges) ; identite
>   du noeud posee avant `Matter.begin()` (fabricant `Djoko-CLI`, produit `Pont
>   ScreenBar Halo`, NodeLabel `Halo`, numero de serie `HALO1-<MAC>`, materiel 1
>   `ESP32-C6 SuperMini + BM5602`) et version logicielle `0.3.0-<commit>` par le
>   descripteur d'application (`src/app_desc.c`). Detail : README.

# Plan final : pilote produit Halo 1 et couche Matter

Ce plan part de D1 pour le coeur radio : on ne rejoue que des sequences prouvees et on touche au minimum le code existant. La couche Matter vient de D3 : boite d'intentions, `resolveMatter` pur et disposition A. Plusieurs idees de robustesse viennent de D2 :
- reset non bloquant ;
- attente apres la telecommande ;
- instantanes de registres ;
- trame etrangere recue a la place de l'accuse ;
- traces qui ne bloquent pas ;
- tests hote avec un simple script `clang++`.

J'ai reverifie dans le code et les logs chaque point ou les juges divergeaient.

- **`enterRxMode` ne rearme pas une puce deja en reception.** Il rend la main tout de suite si STA1 dit RX (bc5602.cpp:225-226). Le rearmement de sniffStd toutes les 100 ms se reduit donc a CE=0 plus une reecriture de MASK. C'est ce comportement qu'il faut reproduire, sans strobe force.
- **Les trames de reveil demandent un accuse.** `FF/FE/FD 00` ont NO_ACK=0, et seule `FA xx` a NO_ACK=1 (btn-A*.log). Les trames de service se reconnaissent donc aux bits 3/4, pas a NO_ACK.
- **Une trame A peut avoir le bit 7 a zero.** On a vu `60 01` (PROTOCOL.md:1328). Une trame A ne doit jamais modifier l'etat marche/lampes.
- **L'ecart reel des essais tx-sem etait de 500 ms, pas 300.** La CLI force un ecart d'au moins 500 ms sur le canal 5 (cli.cpp:660-662). L'ecart de 100 ms n'a jamais ete essaye depuis l'ESP32.
- **Les vecteurs de CRC sont bons.** Je les ai tous recalcules. L'accuse reel de la lampe a NO_ACK=1 : PID1, CRC 5C90, brut `01 AE 48 00 00 00 00 00`.
- **Mettre a jour un attribut par `updateAttributeVal` garde le cache de la bibliotheque a jour.** L'appel passe par `attribute::update`, puis PRE_UPDATE, puis `attributeChangeCB`, qui met le cache a jour quand le callback renvoie true (MatterColorTemperatureLight.cpp).

Le nom `halo1` sert uniquement d'espace de noms. L'objet global s'appelle `lamp`, ce qui evite l'erreur de compilation de D2. Les macros `HALO_*` restent en place jusqu'a l'etape C6 : chaque commit compile.

---

## A. Decisions a prendre par l'utilisateur

### A1. Disposition des endpoints Matter

Elle impose de remettre le noeud en service : le retirer de l'app, puis lancer `decommission`.

- **(a) Recommandee.**
  - EP1 « Halo » = `MatterColorTemperatureLight` : marche, luminosite unique, temperature.
  - EP2 « Halo avant » et EP3 « Halo arriere » = `MatterOnOffLight`. Chacun signifie « cette lampe est allumee » (marche ET bit de lampe).
  - EP4 « Halo auto » = `MatterOnOffPlugin` momentane, qui envoie un appui sur A.
- **(b)** Comme (a), mais EP2 et EP3 en `MatterOnOffPlugin`. Un ordre « eteins toutes les lumieres » donne a la piece n'y touche plus, mais les deux lampes apparaissent comme des prises.
- **(c)** EP1 seul (plus EP4). Le choix des lampes ne se fait qu'a la telecommande ou a la CLI.
- **(d)** Garder la disposition actuelle (prise maitre, lumiere avant, lumiere arriere variable). Rejetee : deux curseurs de luminosite qui ne peuvent pas etre independants, puisque chaque trame ne porte qu'une valeur.

Pourquoi (a) :
- la disposition dit vrai : une seule luminosite ;
- les commandes vocales par lampe marchent ;
- « Allume la Halo » retrouve la derniere selection, comme le bouton marche de la telecommande.

Le choix entre (a) et (b) se regle par `HALO1_SELECTORS_AS_LIGHTS` (1 ou 0).

### A2. Exposer le bouton A (mode auto) dans Matter

- **(a) Recommandee.** EP4 momentane, avec un garde-fou : on ignore l'appui s'il arrive dans la meme fenetre de coalescence qu'une intention marche ou lampe. C'est la signature d'une commande de groupe ou d'une tuile Apple regroupee.
- **(b)** Ne pas l'exposer (`HALO1_EXPOSE_AUTO 0`) tant qu'on ne sait pas si A bascule le mode auto ou le relance (Q6).

(a) coute peu et le garde-fou couvre le seul cas dangereux.

### A3. Courbe de luminosite Matter vers valeur brute

- **(a) Recommandee.** Table gamma avec γ=2,0 (`HALO1_LEVEL_GAMMA`). Elle se regle au banc a l'etape T14.
- **(b)** Lineaire.

PROTOCOL.md note deja une perception logarithmique et demande une « conversion non lineaire ». Avec γ=2, 0x80 correspond au niveau 138, soit a peu pres la moitie, ce qui colle a l'observation.

### A4. Ce qui accompagne un allumage ou un changement de lampes

- **(a) Recommandee.** La trame de luminosite, avec la valeur affichee par Matter : par exemple `C5 A5`. Une seule trame, et la lampe se cale sur le curseur. La telecommande fait deja pareil (`C4 ED` / `44 ED`).
- **(b)** La trame de temperature, avec la temperature crue, comme notre essai `C3 35`. La lampe garde sa propre luminosite, et le curseur Matter peut alors etre faux.

(a) garde l'app en accord avec la lampe, y compris si la lampe memorise une luminosite par lampe (Q3). Seul point a verifier : un changement de lampes porte par une trame luminosite n'a jamais ete emis. T2 l'essaie ; en cas d'echec, on repasse a (b), qui ne change qu'une ligne de `plan()` et une de `dueFields()`.

---

## B. Architecture

### B.1 Fichiers

| Action | Fichier | Contenu |
|---|---|---|
| creer | `src/halo1_proto.h/.cpp` | code pur : bits, constructeurs, classification, decodage et CRC, `applyState`, `plan`, auto-test |
| creer | `src/halo1_map.h/.cpp` | code pur : table gamma, conversions mired, `resolveMatter`, `SelectionMemory` |
| creer | `src/halo1_radio.h/.cpp` | sequences BC5602 prouvees (deplacees de halo.cpp) et `Halo1Radio` non bloquant |
| creer | `src/halo1_lamp.h/.cpp` | pilote `Halo1Lamp` : etat, tranches, ordonnanceur TX, suivi de la telecommande, NVS |
| creer (24/09) | `src/halo1_watch.h/.cpp` | code pur : `ChipWatch`, relance du module sur symptome de puce et ses limites (C.5) |
| creer | `src/cli_lampe.cpp` | famille de commandes `lampe ...` |
| creer | `tools/host_tests/test_halo1.cpp`, `tools/test_halo1.sh` | tests hote sans carte (clang++) |
| modifier | `src/halo.cpp` | `tick()` ne fait plus rien (C1) ; `configStdAutoAck` deplace ; `applyXoTrim` n'est plus `static` (l.12 et 2442) (C3) ; nettoyage (C6) |
| modifier | `src/main.cpp` | HELLO supprime (C1) ; `lamp.begin` / `lamp.tick` et `delay(1)` (C4) |
| modifier | `src/cli.cpp`, `src/cli.h` | commandes Halo 2 retirees (C1) ; `lampe`, invalidation de la radio, B11 (C3/C4) |
| modifier | `src/config.h` | bloc `HALO1_*` ajoute (C4) ; `HALO_*` supprimes (C6) |
| reecrire | `src/matter_bridge.cpp` | 4 endpoints, boite d'intentions, reflet sous verrou (C5) |
| modifier | `src/bc5602.h` | commentaires (B15, l.179-180), alias `STATUS_RX_EMPTY` |
| docs | `README.md`, `docs/PROTOCOL.md`, `docs/AUDIT-2026-09-23.md` | endpoints, en-tete du protocole, ecart de 500 ms, statut des bogues |

`halo1_*.cpp` et `cli_lampe.cpp` ne dependent pas de Matter. Ils se compilent dans les deux environnements, sans toucher a `build_src_filter`.

### B.2 `src/halo1_proto.h`

```cpp
#pragma once
// Protocole Halo 1 (1re generation), etabli puis verifie par emission sur la
// lampe le 23/09/2026 (docs/PROTOCOL.md). Code pur : aucun include Arduino,
// compile aussi sur l'hote (tools/test_halo1.sh).
#include <stddef.h>
#include <stdint.h>

namespace halo1 {

// ---- Lien radio : constantes, la lampe n'en connait pas d'autre ----------
constexpr uint8_t kChannel = 5;  // RFCH, 2405 MHz ; debit bc5602::DATARATE_125K
constexpr uint8_t kDefaultAddrReg[4] = {0x4F, 0xF0, 0xFD, 0x63};  // air 63 FD F0 4F
constexpr uint8_t kPairingH1Reg[4] = {0xB0, 0x00, 0x01, 0x59};    // air 59 01 00 B0 : jamais
constexpr uint8_t kPairingH2Reg[4] = {0xB0, 0x00, 0x08, 0xE2};    // air E2 08 00 B0 : jamais
bool addressAllowed(const uint8_t addrReg[4]);  // refuse 0, FFFFFFFF, appairages (2 ordres)
void airOrder(const uint8_t addrReg[4], uint8_t air[4]);  // ordre registre -> ordre air

// ---- 1er octet de charge ----------------------------------------------------
constexpr uint8_t F_POWER = 0x80;   // 1 = allumee
constexpr uint8_t F_FRONT = 0x40;   // lampe avant
constexpr uint8_t F_AUTO = 0x20;    // bouton A : valeur = numero d'appui
constexpr uint8_t F_RSV4 = 0x10;    // favori seulement, sans effet visible : jamais emis
constexpr uint8_t F_RSV3 = 0x08;    // idem
constexpr uint8_t F_BRIGHT = 0x04;  // valeur = luminosite 0x4C..0xFE
constexpr uint8_t F_TEMP = 0x02;    // valeur = temperature 0x00 (froid)..0x64 (chaud)
constexpr uint8_t F_BACK = 0x01;    // lampe arriere
constexpr uint8_t F_LAMPS = F_FRONT | F_BACK;
constexpr uint8_t F_SELECT = F_AUTO | F_BRIGHT | F_TEMP;
constexpr uint8_t F_RSV = F_RSV4 | F_RSV3;
constexpr uint8_t kBrightMin = 0x4C;  // la lampe plafonne en dessous (C5 20 sans effet)
constexpr uint8_t kBrightMax = 0xFE;
constexpr uint8_t kTempMax = 0x64;
constexpr uint8_t kBrightDefault = 0xA5;
constexpr uint8_t kTempDefault = 0x35;  // valeur la plus vue sur la telecommande

struct Payload { uint8_t flags; uint8_t value; };
inline bool operator==(Payload a, Payload b) { return a.flags == b.flags && a.value == b.value; }
inline bool operator!=(Payload a, Payload b) { return !(a == b); }

uint8_t clampBright(uint8_t v);  // 0x4C..0xFE
uint8_t clampTemp(uint8_t v);    // 0..0x64
// Exactement un selecteur, bits 3/4 nuls ; (lamps & F_LAMPS) == 0 -> F_FRONT (garde-fou).
Payload makeTemp(bool on, uint8_t lamps, uint8_t temp);
Payload makeBright(bool on, uint8_t lamps, uint8_t bright);
Payload makeAuto(bool on, uint8_t lamps, uint8_t counter);  // counter 0 -> 1

enum class Kind : uint8_t { Temp, Bright, Auto, LampAck, Service, Reserved, Invalid, CrcBad };
// Charge seule. (f & 0xF8) == 0xF8 -> Service (FF/FE/FD/FA) ; f & F_RSV -> Reserved (91, 89) ;
// aucune lampe ou nb de selecteurs != 1 -> Invalid ; sinon Temp / Bright / Auto.
Kind kindOf(Payload p);

// ---- Trame lue en ecoute passive (RXPW0 = 8 : 64 bits apres l'adresse) ----
struct AirFrame { bool crcOk; uint8_t len, pid, noAck; uint8_t pay[4]; uint16_t crc; };
AirFrame decodeAir(const uint8_t raw[8], const uint8_t air[4]);  // len > 4 -> crcOk = false
void encodeAir(const uint8_t air[4], uint8_t pid, bool noAck, const uint8_t *pay, uint8_t len,
               uint8_t raw[8]);  // tests et 'lampe decode'
// !crcOk -> CrcBad ; len == 0 -> LampAck ; noAck -> Service ; len != 2 -> Invalid ; sinon kindOf.
Kind classify(const AirFrame &f);

// ---- Etat --------------------------------------------------------------------
struct State {
  bool power = false;
  uint8_t lamps = F_LAMPS;  // jamais 0
  uint8_t bright = kBrightDefault;
  uint8_t temp = kTempDefault;
};
inline bool operator==(const State &a, const State &b) {
  return a.power == b.power && a.lamps == b.lamps && a.bright == b.bright && a.temp == b.temp;
}
inline bool operator!=(const State &a, const State &b) { return !(a == b); }
enum : uint8_t { FLD_FLAGS = 1, FLD_BRIGHT = 2, FLD_TEMP = 4, FLD_ALL = 7 };
// Temp/Bright : ecrit marche, lampes et la valeur du selecteur (bornee). Auto et le
// reste : RIEN (sens des bits de mode d'une trame A inconnu, '60 01' observe).
// Renvoie les FLD_* dont la valeur a change.
uint8_t applyState(State &s, Payload p);
// Champs de la consigne qu'une trame livree satisfait (efface 'dirty'). La consigne
// est bornee comme par les constructeurs : une trame de plan() couvre toujours ses champs.
uint8_t coveredBy(Payload sent, const State &target);

// ---- Planification (voir D.3) ---------------------------------------------
struct Plan { bool bright = false, temp = false; Payload pb{0, 0}, pt{0, 0}; };
Plan plan(const State &target, const State &believed, uint8_t dirty);
// Champs a livrer pour une consigne (Halo1Lamp::request : dirty_ |= dueFields(...)).
// A4 (a) : allumee, FLAGS ajoute BRIGHT.
uint8_t dueFields(const State &target, uint8_t fields);

uint8_t nextAuto(uint8_t last);            // 0 ou 255 -> 1, sinon last + 1
// Appuis A entendus de la telecommande (D.6) : nouvel appui si premiere trame
// depuis reset(), numero change, ou trame precedente a >= 1 s (avant ou apres).
struct AutoPressFilter {
  static constexpr uint32_t kRepeatMs = 1000;
  bool feed(uint8_t value, uint32_t nowMs);  // true : nouvel appui
  void reset();                              // autre commande de la telecommande
};
uint8_t crc8(const uint8_t *p, size_t n);  // poly 0x07, init 0 : blob NVS
int selfTest(char *msg, size_t n);         // 0 = ok, sinon nb d'echecs (1er dans msg)
}  // namespace halo1
```

### B.3 `src/halo1_map.h`

```cpp
#pragma once
// Correspondances Matter <-> charge Halo 1 et regles d'intention Matter. Code pur.
#include <stdint.h>
#include "halo1_proto.h"

namespace halo1 {
constexpr uint16_t kMiredCold = 153;  // temp 0x00 (le plus froid) ; ~6536 K NOMINAL, non mesure
constexpr uint16_t kMiredWarm = 370;  // temp 0x64 (le plus chaud) ; ~2703 K NOMINAL, non mesure

constexpr uint8_t kMatterLevelFloor = 4;  // plus petit niveau RAPPORTE (Apple Home, E.2)

void mapInit(float gamma);            // table 254 entrees ; gamma 1.0 = formule lineaire exacte
float mapGamma();
uint8_t rawFromLevel(uint8_t level);  // 0..254 -> 0x4C..0xFE (0..kMatterLevelFloor -> 0x4C)
// Niveau rapporte : plus petit L >= kMatterLevelFloor tel que rawFromLevel(L) >= raw.
uint8_t levelFromRaw(uint8_t raw);
uint8_t tempFromMired(uint16_t m);    // ((clamp(m,153,370) - 153) * 100 + 108) / 217
uint16_t miredFromTemp(uint8_t t);    // 153 + (min(t,100) * 217 + 50) / 100
// Affichage stable (E.2), jamais un niveau sous kMatterLevelFloor.
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
  void reset(uint8_t lamps) { cur_ = stable_ = lamps ? lamps : F_LAMPS; since_ = 0; }
  void update(const State &target, uint32_t nowMs, uint32_t stableMs);
  uint8_t memory(const State &target) const;  // allumee : stable_ ; eteinte : target.lamps
 private:
  uint8_t cur_ = F_LAMPS, stable_ = F_LAMPS;
  uint32_t since_ = 0;
};
}  // namespace halo1
```

`SelectionMemory::update` :
- lampe allumee et `lamps != cur_` : `cur_ = lamps`, `since_ = now` ;
- allumee, meme selection depuis au moins `stableMs` : `stable_ = cur_` ;
- eteinte : `cur_ = stable_ = target.lamps`.

### B.4 `src/halo1_radio.h`

```cpp
#pragma once
#include "bc5602.h"
#include "halo1_proto.h"

// Corps de l'ancien configStdAutoAck (halo.cpp:3776-3809), decoupe autour de ses
// 2 x 20 ms d'attente. Comportement sur l'air IDENTIQUE.
void halo1StdReset(BC5602 &r);  // CMD_SOFTWARE_RESET seul, sans attente
void halo1StdConfigure(BC5602 &r, const uint8_t addrReg[4], uint8_t ch, uint8_t rate, bool receiver);
void halo1PassiveOverrides(BC5602 &r);  // ENAA 0, DPL2 0, DPL1 0, PKT1 0, RXPW0 8 (= armer 3955-3959)
// Version bloquante pour txAck / sniffStd / prxAck : reset + delay(40) + configure.
void configStdAutoAck(BC5602 &r, const uint8_t addrReg[4], uint8_t ch, uint8_t rate, bool receiver);
void applyXoTrim(BC5602 &r);  // defini dans halo.cpp ('static' retire)

class Halo1Radio {
 public:
  enum class Mode : uint8_t { Unknown, Resetting, Tx, Rx, Sleep };
  enum class Verdict : uint8_t { Ack, AckForeign, MaxRt, Timeout, FifoRefused };
  struct TxReport {
    Verdict v; uint8_t irq1, rt2, status; uint16_t us;
    uint8_t fLen; uint8_t fPay[4];  // trame recue a la place de l'accuse (AckForeign)
  };
  struct Tuning {
    uint16_t resetWaitMs = HALO1_RESET_WAIT_MS;  // 40 : chemin prouve
    uint16_t rearmMs = HALO1_RX_REARM_MS;        // 100 (= sniffStd)
    uint16_t silenceMs = HALO1_RX_SILENCE_MS;    // 500 (= sniffStd, 06a3185)
    bool strongRearm = false;  // LIGHT_SLEEP puis RX : NON PROUVE (banc T10)
    bool lightSwitch = false;  // bascule TX<->RX sans reset : NON PROUVE (banc T10)
  };
  struct Stats {
    uint32_t fullConfigs, silenceReconf, txReconf, verifyFail, rearms, rearmsOffRx, rxRaw, lightSwitches;
  };
  struct Snapshot {  // pris avant chaque reconfiguration de silence ou d'echec TX
    uint32_t atMs; uint8_t why, sta1, irq1, status, mask, ce, cfg1, rfch, dm1, pkt1, enaa,
        dpl1, dpl2, rxpw0, rt1;
  };

  void begin(BC5602 &chip, const uint8_t addrReg[4]);  // aucun acces SPI
  void setAddress(const uint8_t addrReg[4]);           // + invalidate()
  const uint8_t *air() const { return air_; }
  bool present() const { return chip_ && chip_->present(); }
  void invalidate() { mode_ = Mode::Unknown; }         // un outil CLI a touche la puce
  void request(Mode m, uint32_t nowMs);                // Tx, Rx ou Sleep
  bool ready(Mode m) const { return mode_ == m; }
  Mode mode() const { return mode_; }
  void service(uint32_t nowMs);                        // acheve un reset apres resetWaitMs
  bool restartWanted() const { return restartWanted_; }
  void restartDone() { restartWanted_ = false; verifyFails_ = 0; mode_ = Mode::Unknown; }
  // Exige ready(Tx). Bloquant : <= 30 ms d'attente active (1,6-1,7 ms mesures).
  // Apres tout verdict autre que Ack/AckForeign : lance une reconfiguration vers Tx.
  TxReport sendOne(const uint8_t *pay, uint8_t len, uint32_t nowMs);
  // Exige ready(Rx). Une iteration de la boucle de sniffStd ; true = raw[8] rempli.
  bool pollRx(uint32_t nowMs, uint8_t raw[8]);
  uint8_t snapshots(Snapshot *out, uint8_t max) const;
  bool readConfig(uint8_t out[3]);  // RFCH, DM1, RT1 relus

  Tuning tuning;
  Stats stats{};

 private:
  void beginReset(Mode target, uint32_t nowMs, uint8_t why);
  void finishReset(uint32_t nowMs);
  bool verify();  // RFCH == 5, DM1 == 0x82, RT1 == 0x73
  void takeSnapshot(uint8_t why, uint32_t nowMs);
  void lightToTx();
  void lightToRx();
  BC5602 *chip_ = nullptr;
  Mode mode_ = Mode::Unknown, target_ = Mode::Unknown;
  uint8_t addrReg_[4] = {0x4F, 0xF0, 0xFD, 0x63}, air_[4] = {0x63, 0xFD, 0xF0, 0x4F};
  uint32_t resetAt_ = 0, lastArm_ = 0, lastFrame_ = 0, lastFull_ = 0;
  uint8_t verifyFails_ = 0;
  bool restartWanted_ = false;
  Snapshot snaps_[4] = {};
  uint8_t snapIdx_ = 0;
};
```

`config.h` doit etre inclus avant ce fichier, pour les `HALO1_*`.

### B.5 `src/halo1_lamp.h`

```cpp
#pragma once
#include <Arduino.h>
#include "config.h"
#include "halo1_map.h"
#include "halo1_radio.h"

enum class Halo1Link : uint8_t { Unknown, Ok, Lost };

// Tout se passe dans la tache loop() : pont Matter (apres coalescence), CLI, tick().
// Aucune section critique : les callbacks Matter n'appellent jamais cet objet.
class Halo1Lamp {
 public:
  using RestartFn = bool (*)();  // relance complete du module (halo.begin())
  void begin(BC5602 &chip, bool listen, RestartFn restart);  // NVS -> cru = consigne ; N'EMET RIEN
  void tick();                    // <= ~35 ms au pire (un paquet), typiquement < 1 ms
  void invalidateRadio() { radio.invalidate(); }

  // --- consignes ---
  void request(const halo1::State &t, uint8_t fields);  // fields = FLD_* a livrer : TOUJOURS emis
  bool pressAuto();                                     // false si consigne eteinte
  void reassert() { request(target_, halo1::FLD_ALL); }
  const char *sendRaw(halo1::Payload p, bool force, uint8_t packets, uint16_t gapMs);  // nullptr = accepte
  void believe(halo1::Payload p);                       // cru + consigne, sans emettre

  // --- lecture ---
  const halo1::State &target() const { return target_; }
  const halo1::State &believed() const { return believed_; }
  uint8_t memoryLamps() const { return selMem_.memory(target_); }
  uint32_t version() const { return version_; }  // +1 a chaque changement de consigne
  bool busy() const;                             // tranche active ou attente de reprise
  Halo1Link link() const { return link_; }
  uint8_t lastAuto() const { return lastAuto_; }

  // --- banc ---
  bool waitIdle(uint32_t maxMs);  // fait tourner tick() (+ delay(1)) jusqu'au repos
  void setListening(bool on);
  bool listening() const { return listening_; }
  void setTrace(bool on) { trace_ = on; }
  bool setAddress(const uint8_t addrReg[4]);  // NVS "halo1/addr" ; refuse les interdites
  const uint8_t *address() const { return addrReg_; }
  void forget();       // efface "halo1/etat", cru = consigne = valeurs par defaut
  void persistNow();
  void printStatus(Print &out) const;
  void printStats(Print &out) const;
  void clearStats();

  struct Tuning {
    uint8_t repeats = HALO1_REPEATS;           // 3 paquets par trame
    uint8_t minAcks = HALO1_MIN_ACKS;          // 2 accuses pour reussir
    uint8_t maxAttempts = HALO1_MAX_ATTEMPTS;  // 5 paquets au plus par trame
    uint16_t gapMs = HALO1_GAP_MS;             // 100 entre deux paquets
    uint16_t retryMs = HALO1_RETRY_MS;         // 1000 x rang de l'echec
    uint8_t planRetries = HALO1_PLAN_RETRIES;  // 2 reprises avant abandon
  } tuning;
  struct Stats {
    uint32_t requests, packets, acks, ackForeign, maxRt, timeouts, fifoRefused, weakFails,
        preempted, cancelled, retries, giveUps, autoSent, autoIgnoredOff, rxFrames, rxCrcBad,
        rxLampAcks, rxState, rxAuto, rxService, rxReserved, rxInvalid, holdoffs, persisted,
        traceDropped, restarts;
  } stats{};
  Halo1Radio radio;

 private:
  enum class Phase : uint8_t { Idle, Burst, Backoff };
  enum SlotId : uint8_t { SLOT_BRIGHT, SLOT_TEMP, SLOT_AUTO, SLOT_RAW, SLOT_N };
  struct Slot { bool active; halo1::Payload pay; uint8_t attempts, acks, repeats; };
  bool anyActive() const;
  void replan(uint32_t now);
  void setSlot(SlotId id, bool want, halo1::Payload p);
  int8_t pickSlot();
  void sendPacket(uint32_t now);
  void onVerdict(SlotId id, const Halo1Radio::TxReport &r, uint32_t now);
  void complete(SlotId id, uint32_t now);
  void fail(SlotId id, uint32_t now);
  void giveUp(uint32_t now);
  void onAir(const halo1::AirFrame &f, uint32_t now);
  void onRemotePayload(halo1::Payload p, uint32_t now);
  void applyBelieved(halo1::Payload p, uint32_t now);
  void maybePersist(uint32_t now);
  void loadNvs();
  void saveState();
  void trace(const char *fmt, ...);  // rien si !trace_ ; perdu si Serial.availableForWrite() < 96

  halo1::State target_, believed_;
  uint8_t dirty_ = 0, confirmed_ = 0, lastAuto_ = 0, rr_ = 0, failures_ = 0;
  Slot slots_[SLOT_N] = {};
  Phase phase_ = Phase::Idle;
  uint32_t version_ = 1, nextTxAt_ = 0, retryAt_ = 0, remoteAt_ = 0, pendingSince_ = 0,
           lastTxEndAt_ = 0, persistFirst_ = 0, persistDue_ = 0, lastAckAt_ = 0;
  uint16_t rawGapMs_ = 0;
  bool listening_ = false, trace_ = false, persistDirty_ = false;
  Halo1Link link_ = Halo1Link::Unknown;
  uint8_t addrReg_[4] = {0x4F, 0xF0, 0xFD, 0x63};
  halo1::SelectionMemory selMem_;
  uint8_t saved_[8] = {};
  RestartFn restart_ = nullptr;
};
extern Halo1Lamp lamp;
```

### B.6 `src/config.h` : bloc ajoute en C4

Les `HALO_*` restent jusqu'a C6. Chaque constante est protegee par `#ifndef` pour pouvoir etre surchargee par `-D`.

```c
// ===== Halo 1 : pilote produit (prouve, ou reglable au banc via 'lampe') =====
#define HALO1_REPEATS 3                // comme la telecommande ; une trame seule a ete ignoree
#define HALO1_MIN_ACKS 2
#define HALO1_MAX_ATTEMPTS 5
#define HALO1_GAP_MS 100               // telecommande ~100 ; seul 500 est prouve depuis l'ESP32
#define HALO1_RETRY_MS 1000
#define HALO1_PLAN_RETRIES 2
#define HALO1_RESET_WAIT_MS 40         // 2 x 20 ms du chemin prouve
#define HALO1_RX_REARM_MS 100
#define HALO1_RX_SILENCE_MS 500
#define HALO1_REMOTE_HOLDOFF_MS 250
#define HALO1_REMOTE_HOLDOFF_MAX_MS 2000
#define HALO1_SELECTION_STABLE_MS 2000
#define HALO1_PERSIST_DELAY_MS 10000
#define HALO1_PERSIST_MAX_MS 60000
#define HALO1_COALESCE_QUIET_MS 120
#define HALO1_COALESCE_MAX_MS 400
#define HALO1_REFLECT_MIN_MS 250
#define HALO1_AUTO_PULSE_MS 1000
#define HALO1_BOOT_IGNORE_MS 2000
#define HALO1_LEVEL_GAMMA 2.0f
#define HALO1_SELECTORS_AS_LIGHTS 1
#define HALO1_EXPOSE_AUTO 1
#ifdef DIAG_ONLY
#define HALO1_LISTEN_DEFAULT false     // diag : aucune activite radio de fond
#else
#define HALO1_LISTEN_DEFAULT true
#endif
```

---

## C. Sequencement radio

Toutes les sequences ci-dessous sont des sequences prouvees, sauf C.6 et C.7, desactivees par defaut.

### C.1 Demarrage

1. `halo.begin()`, inchange. Il enchaine :
   - `loadConfig` ;
   - `radio.begin(18, 19, 20, 14, 1 MHz)` : reset + 20 ms, SPI 4 fils, version, `registerConfigure`, PRM_RX, `waitCrystalReady` (au plus 50 ms), `calibrate` (au plus 200 ms) ;
   - `prepareToSniff`, `LIGHT_SLEEP`, `calibrate`, `prepareToSniff`.

   Il laisse la puce en reception passive avec ENAA=0 : elle n'accuse rien.
2. `lamp.begin(halo.radio, HALO1_LISTEN_DEFAULT, []{ return halo.begin(); })` :
   - `halo1::mapInit(HALO1_LEVEL_GAMMA)`, dans les deux builds : le build diag n'a pas de pont Matter, et le repli de halo1_map (gamma 2,0) ignore `HALO1_LEVEL_GAMMA` ;
   - lit NVS « halo1 » ;
   - `radio.begin()`, sans SPI ;
   - `radio.request(listen ? Rx : Sleep)`.
3. **Rien n'est emis.** La branche HELLO de main.cpp:184-193 est supprimee en C1.

### C.2 Reconfiguration complete, non bloquante

C'est la meme suite que configStdAutoAck, dans le meme ordre.

1. **t0 :** `CMD_SOFTWARE_RESET` (0x08). Mode `Resetting`, `resetAt_ = t0`. Rien d'autre ne se passe pendant 40 ms. Les registres suivants sont effaces par le reset : 15 des 19 valeurs Holtek, CFG1 (donc l'AGC), XO, PID, RT1, ENAA=0x3F.
2. **t0 + 40 ms :** `halo1StdConfigure`.
   1. IO1 ← 0x48.
   2. `registerConfigure(nullptr)` : 19 ecritures et relectures, se termine en banque 0.
   3. `applyXoTrim` : XO1 bits 4-0, seulement si `gXoTrim >= 0`.
   4. `setBank(0)`, CFG1 ← 0x40 (AGC).
   5. RFCH ← 0x05, DM1 ← 0x82.
   6. Adresse PTX ← `4F F0 FD 63`.
   7. CFO1 &= ~0x40 : preambule d'un octet.
   8. MASK.PRM_RX ← `receiver`.
   9. PKT1 ← 0x20 (CRC), PKT2 &= 0x7F (pas de blanchiment).
   10. DPL1 ← 0x01, DPL2 ← 0x04, ENAA ← 0x01.
   11. RT1 ← 0x73 (ARD 2 ms, ARC 3).
   12. IRQ1 ← 0x70, FLUSH_TX, FLUSH_RX, CE ← 0.
3. **Verification :** relire RFCH, DM1 et RT1 ; on attend 05, 82 et 73.
   - Si ca ne correspond pas, `verifyFail++` et nouveau reset.
   - Apres 3 echecs de suite, `restartWanted_` est leve (niveau L2).
   - Si un de ces registres ne se relit pas a l'identique des le premier flash (bit en lecture seule), on ne compare plus que RFCH et RT1. `lampe regs` le montre.
4. **Si la cible est Tx :** mode `Tx`, CE=0, en attente.
5. **Si la cible est Rx :**
   - `halo1PassiveOverrides` : ENAA ← 0, DPL2 ← 0, DPL1 ← 0, PKT1 ← 0, RXPW0 ← 8.
   - `enterRxMode()` : MASK |= PRM_RX ; si STA1.OMST ≠ 5, FLUSH_RX, IRQ1 ← RX_DR, `CMD_RX_MODE` (0x8E), puis attente de OMST=5, au plus 3 × 1,5 ms.
   - `lastFull_ = lastArm_ = now`. `lastFrame_` n'est pas remis a zero, comme dans sniffStd.
   - Mode `Rx`.

`request(m)` :
- deja dans le mode `m` : rien ;
- reset en cours : on change seulement la cible ;
- `Sleep` : CE ← 0 et `CMD_LIGHT_SLEEP`, sans reset ;
- Tx ↔ Rx : reconfiguration complete, ou bascule legere si `tuning.lightSwitch` (C.6).

### C.3 Emettre une trame trois fois avec accuse

La tranche est une file par selecteur (D.4). Le paquet est envoye par `sendOne`, qui reprend txAck:3832-3864 sans rien afficher.

1. IRQ1 ← 0x70, puis FLUSH_TX.
2. `writeCommandData(0x11, {flags, value}, 2)`. Le materiel ajoute le PCF (longueur 2, PID, NO_ACK=0) et le CRC.
3. Lire STATUS. Si `STATUS_TX_FIFO_EMPTY`, le verdict est **FifoRefused** : aller a l'etape 6.
4. CE ← 1 et `t0 = micros()`. Boucle : lire IRQ1 ; sortir si TX_DS (0x20) ou MAX_RT (0x10) ; sinon `delayMicroseconds(20)`. Au bout de 30 000 µs, **Timeout**.
5. Verdict.
   - **TX_DS sans RX_DR : Ack.** Un succes normal lit IRQ1=0x2E et STATUS=0x11.
   - **TX_DS avec RX_DR (0x40) : AckForeign.** Une trame avec charge, probablement de la telecommande, est arrivee dans notre fenetre d'accuse. On lit `len = PKT4` ; si 1 ≤ len ≤ 4, `readFifo(fPay, len)` avant le vidage. Ce cas n'est jamais observe et jamais essaye.
   - **MAX_RT : MaxRt.**
6. Lire RT2 et STATUS, puis **CE ← 0 tout de suite** (sinon la puce reemet seule), IRQ1 ← 0x70, FLUSH_TX, FLUSH_RX.
7. Si le verdict n'est ni Ack ni AckForeign :
   - instantane des registres ;
   - `beginReset(Tx)`, soit le chemin prouve de txAck:3875. Sans lui, les envois suivants echouent en 64 µs (STATUS 21). Le PID repart a 0.
8. Apres un succes, **rien** : le PID avance, et les paquets d'une meme rafale portent 0, 1, 2...

Pendant toute la rafale, la puce reste PTX avec CE=0. **Pas d'ecoute entre les paquets** tant que T10 n'a pas montre que le PID survit a une bascule PRM_RX. En fin de rafale, `request(listening_ ? Rx : Sleep)`.

### C.4 Ecoute passive

`pollRx` est appele a chaque `tick()`, soit environ 1 kHz avec le `delay(1)` de `loop()`. C'est une iteration de sniffStd:3970-4040, dans le meme ordre.

1. Lire IRQ1. Sur RX_DR :
   - `readFifo(raw, 8, false)`, IRQ1 ← RX_DR, FLUSH_RX, `lastFrame_ = now` ;
   - CE ← 0 et `enterRxMode()`, qui envoie reellement le strobe ici car la puce est retombee en Light Sleep apres l'evenement ;
   - `lastArm_ = now` (on continue ; la trame est rendue en fin d'appel).
2. Puis, si OMST ≠ RX : `enterRxMode(300)`, `lastArm_ = now`, compte dans `rearmsOffRx` (symptome « ecoute sourde », C.5).
3. Sinon, si `now - lastArm_ > 100` : CE ← 0 et `enterRxMode()`. C'est **le rearmement prouve**, sans strobe (finding 5). Avec `strongRearm`, voir C.7.
4. Si `now - lastFrame_ > 500` et `now - lastFull_ > 500` : instantane (`why=silence`), puis `beginReset(Rx)`. C'est le remede prouve (06a3185) : environ 43 ms de surdite toutes les ~540 ms au calme, soit ~8 %.

Reglage (`lampe rx <rearm> <silence>`, config.h) : rearmement d'au plus 200 ms et silence de plus de 2 rearmements, sinon refuse (`static_assert` pour config.h). Au-dela, une piece calme ne donne plus les 20 rearmements periodiques en 10 s dont a besoin la guerison apres surdite (C.5) : au pire 22 a la limite (199/399, simule avec un tour toutes les 1 a 10 ms), ~74 avec 100/500, mais 18 avec 250/500 et 9 avec un rearmement de 1000 ms.

L'ecoute ne peut jamais accuser : ENAA=0 est ecrit apres chaque configuration, avant l'entree en reception.

### C.5 Niveaux de reprise

| Niveau | Declencheur | Action | Surdite |
|---|---|---|---|
| L0 rearmement | apres une trame, toutes les 100 ms, OMST ≠ RX | C.4 etapes 1 a 3 | < 0,5 ms |
| L1 reconfiguration complete | verdict TX autre que Ack ; 500 ms de silence ; verification ratee ; `invalidate()` | C.2 | 40 ms, non bloquant, + ~3 ms |
| L2 relance du module | 3 verifications ratees de suite, ou version de puce 0/FFFFFF ; **symptome de puce** (24/09) : 3 delais TX de suite, deluge de CRC faux en ecoute (au moins 100 trames brutes en 10 s, dont au moins 90 % de CRC faux), ou ecoute sourde (au moins 1000 rearmements sur OMST ≠ RX en moins de 10 s, fenetre glissante) | `restart_()` = `halo.begin()`, puis `radio.restartDone()` puis L1. Sur symptome : une relance au plus par minute, un essai toutes les 10 min une fois EN PANNE | ~300 ms **bloquant**, jusqu'a ~0,5 s si le quartz ou la calibration ne repondent pas (rare) |
| L3 module perdu | `halo.begin()` echoue, ou configuration rejetee meme apres relance | pilote inactif, `lampe` affiche « BM5602 perdu », nouvel essai L2 toutes les 60 s ; chaque essai impose aux relances sur symptome la meme attente qu'une relance | — |

Pourquoi L2 ne met pas en danger le chien de garde : l'attente active de `calibrate()` ne masque pas les interruptions, donc le chien de garde d'interruption (300 ms) ne s'applique pas. loopTask n'est pas inscrite au chien de garde des taches (5 s).

#### Relance sur symptome de puce (`src/halo1_watch.h`, depuis le 24/09)

**Incident du 24/09** (carte produit, Matter sur Thread) : une pointe de pied a coulisse metallique a touche le quartz du BM5602, bloque ~70 min. Les instantanes lisaient STA1 00, IRQ1 00, STATUS 00, alors que RFCH, DM1 et RT1 se relisaient 05, 82 et 73 : la verification ne voyait donc rien (2 echecs, 0 relance), et la reconfiguration L1 (reset logiciel + configuration) ne guerissait pas. Un `rfinit` a la main (`halo.begin()` : attente du quartz, calibration) a tout gueri d'un coup. Effet visible : chaque commande tournait jusqu'a l'abandon, Maison revenait a l'etat cru pendant que la lampe changeait en partie. Ce que disent les journaux (`logs/live.log`, `logs/bug-radio.log`, hors depot) :

- **Compteurs depuis le demarrage** (au moins ~13 min avant l'incident, `lampe stats`) : 1566 `DELAI` sur 1812 paquets (11 echecs TX au plus avant l'incident) ; 96 403 trames brutes en ecoute, dont 96 227 au CRC faux (99,8 % ; d'ordinaire quelques-unes par heure). Ces trames ne sont pas datees : leur rythme pendant l'incident n'est pas connu, ce n'est **pas** 96 403 / 70 min.
- **Phase finale observee** (trace des ~2 dernieres minutes, puis deux `lampe` a 26 s d'ecart) : 186 `DELAI` de suite, 0 MAX_RT ; en ecoute, **aucune** trame (reconfiguration de silence toutes les ~540 ms, le plus vite que le cycle 500 + 40 ms le permet ; une trame l'aurait repoussee) ; 350 a 450 rearmements par seconde (11 769 en 26 s entre les deux `lampe`), alors que les periodiques en font 10 au plus : presque tous sur OMST ≠ RX (d'ordinaire ~7 par seconde en tout, presque tous periodiques). La puce etait sourde, pas bruyante.

`ChipWatch` (code pur, teste sur l'hote) decide ; `Halo1Lamp::tick()` relance, seul :

- **Delais TX** : 3 verdicts `Timeout` de suite. Un `Ack` (ou `AckForeign`, qui porte aussi TX_DS) ou un `MaxRt` remet la serie a zero ; `FifoRefused` est neutre. **MAX_RT ne fait jamais relancer** : la puce emet et attend un accuse qui ne vient pas (lampe debranchee). Incident : 186 delais de suite a la fin, la serie de 3 tombe des la premiere commande (~0,4 s) ; au milieu d'une rafale, ses paquets restants partent de la puce relancee. Le verdict relit IRQ1 avant de conclure au delai : une preemption de loopTask au-dela des 30 ms ne transforme pas un MAX_RT (lampe debranchee) en delai.
- **Deluge en ecoute** : fenetres consecutives de 10 s ; une fenetre qui atteint **100 trames brutes dont au moins 90 % de CRC faux** declenche tout de suite, sans attendre sa fin, et l'alerte tient jusqu'a la fermeture d'une fenetre calme (ou 20 s sans aucune trame). Il couvre une phase ou la puce malade recoit du bruit (les ~96 000 CRC faux de l'incident sont venus a un moment ou a un autre) ; un deluge dense synthetique (~23 trames par seconde, 99,8 % faux) le declenche en ~4-5 s (essai hote). Usage normal : la molette donne ~9 trames par seconde, et les accuses de la lampe au plus autant, au CRC juste ; le CRC faux se compte en unites par heure. Declencher demande 90 CRC faux en 10 s et une proportion que l'on n'a vue qu'avec la puce malade : une telecommande brouillee a 50 % n'y arrive pas (essai hote).
- **Pas de declencheur « lampe muette »** : une lampe debranchee ne doit jamais faire relancer en boucle.
- **Ecoute sourde** (depuis le 24/09, phase finale de l'incident) : au moins **1000 rearmements sur OMST ≠ RX** dans une fenetre glissante de 10 s. `Halo1Radio::pollRx` compte a part ces rearmements (`rearmsOffRx`, « hors RX » dans `lampe stats`) ; `tick()` passe a chaque tour d'ecoute l'ecart de ce compteur a `ChipWatch`, et celui des rearmements periodiques (ceux faits quand la puce dit RX, qui ne comptent pas pour le symptome). La fenetre est faite de 10 tranches d'une seconde : la somme couvre la tranche en cours et les 9 precedentes, donc toujours moins de 10 s, quelle que soit la phase ou la surdite commence (des fenetres consecutives la verraient jusqu'a ~13 s plus tard). Au rythme de l'incident (350 a 450 par seconde), elle est vue en **2,2 a 2,9 s** (essai hote), sans attendre la commande suivante : si la relance guerit, comme `rfinit` l'a fait, la telecommande redevient audible aussitot. D'ordinaire : ~7 rearmements par seconde en tout, presque tous periodiques ; les hors RX, bien moins d'un par seconde (deduit des totaux : en piece calme, ~4 rearmements par reconfiguration de silence, 1784 pour 446 dans `logs/live.log`, soit les periodiques vers 100, 200, 300 et 400 ms de chaque cycle d'ecoute de 500 ms ; le compteur « hors RX » le mesurera directement). Le seuil, 100 par seconde en moyenne, est plus de 10 fois au-dessus de tous les rearmements normaux reunis et 3,5 fois sous l'incident (essais hote : 99 par seconde pendant 10 min, une heure d'ecoute normale, une lampe debranchee commandee toutes les 2 s : jamais). Une lampe debranchee donne des MAX_RT, l'ecoute reste normale. En diag, l'ecoute coupee par defaut ne nourrit pas ce symptome.
- **Limites** : une relance sur symptome au plus 60 s apres la precedente, quelle qu'en soit la cause (une relance sur verification ratee n'attend pas, car la radio reste inerte sans elle et L3 arrete deja sa boucle ; elle compte comme les autres). Un essai L3 impose la meme attente (sans compter comme relance) : pas de relance sur symptome juste derriere lui. Apres 3 relances de suite sans signe de guerison, si le symptome revient : **module EN PANNE**, un essai toutes les 10 min. Signe de guerison : un accuse, ou une fenetre d'ecoute close sans deluge et **majoritairement** au CRC juste. Une trame juste isolee ne suffit pas : un CRC-16 laisse passer du bruit, et une puce malade peut rester sous le seuil du deluge (60 fausses et une juste dans une fenetre : pas de guerison, essai hote), ce qui remettrait sinon le compte a zero a chaque essai et relancerait toutes les minutes sans fin. Le silence ne prouve rien, **sauf apres une relance pour surdite** : une fenetre d'ecoute de 10 s close sans CRC faux, ou la puce a dit RX au moins 20 fois (rearmements periodiques ; ~75 d'ordinaire en piece calme) et en est retombee 10 fois au plus, vaut guerison, si l'emission ne dit pas le contraire : aucun delai dans la fenetre, et pas de serie de delais en cours (close par un accuse ou un MAX_RT). Sinon, une relance qui guerit l'ecoute mais pas l'emission (l'incident montrait les deux) leverait EN PANNE et eteindrait la LED pendant que chaque envoi echoue, et ramenerait les relances a 60 s (essai hote : relance 4 d'un module EN PANNE, ecoute revenue, envois en delai : EN PANNE et 10 min tiennent). Elle suppose le reglage d'ecoute de C.4 (rearmement ≤ 200 ms, silence > 2 rearmements), que `lampe rx` impose. Choix du 24/09 : sans elle, une relance qui a gueri la surdite laisserait, en piece calme (lampe debranchee ou pas commandee, telecommande posee), le module EN PANNE et la LED rouge jusqu'a la prochaine commande accusee ou le prochain usage de la telecommande, et les relances suivantes a 10 min. Apres une autre cause (delais, bruit, verification), elle ne compte pas : une puce qui reste en RX peut encore mal emettre, et les delais perdraient leur limite de 10 min. Sans ecoute (diag), elle ne vient jamais. L'etat EN PANNE dure jusqu'a un signe de guerison.
- **Preuves effacees** a chaque relance (la serie, la fenetre d'ecoute et celle de la surdite repartent de zero : le symptome doit reapparaitre apres elle), a chaque outil de banc (`invalidateRadio()` : l'outil a pu tout changer, `rfinit` compris), et a chaque essai L3. Les limites et les compteurs, eux, restent.
- **Outils de banc et build diag** : seul `tick()` relance ; un outil de banc (`txack`, `ecoute`, `xo`...) tourne dans la CLI sans `tick()`, puis invalide la radio. En diag, l'ecoute est coupee par defaut : seuls les envois du pilote (`lampe ...`, `lampe brut` compris) nourrissent la serie de delais ; ni deluge ni surdite sans `lampe ecoute 1`.
- **Comptes et traces** : chaque relance L2 est annoncee par une ligne affichee meme sans trace (jamais bloquante : perdue et comptee dans « traces perdues » si le tampon serie est plein), par exemple `[lampe] BM5602 : 3 paquets de suite sans TX_DS ni MAX_RT en 30 ms : relance automatique du module (1 depuis la derniere guerison)` ou `[lampe] BM5602 : ecoute sourde (1000 rearmements hors RX en 2856 ms au plus) : relance automatique du module (1 depuis la derniere guerison)` (duree comptee du debut de la plus ancienne tranche non vide : une seconde de trop au plus), puis, apres `halo.begin()`, `[lampe] BM5602 relance : quartz pret, calibration faite` (`BC5602::begin()` reussit des que la version se lit, quartz pret ou non : cette ligne dit ce qui a vraiment ete refait) ; de meme le passage EN PANNE et le retour. `lampe` montre la serie de delais, la fenetre d'ecoute en cours, les rearmements hors RX de la fenetre glissante, l'etat EN PANNE et la derniere relance datee (lisible meme si sa ligne a ete perdue) ; `lampe stats` les relances par cause (verif., delais, bruit, sourde), les 4 dernieres datees, les relances de suite sans guerison et l'attente avant la prochaine permise. `lampe stats raz` remet les compteurs a zero, pas l'attente ni l'etat EN PANNE.
- **LED d'etat** : rouge fixe tant que le module est EN PANNE ou perdu (L3) ; les trois clignements rouges d'un abandon restent visibles par-dessus (README).

### C.6 Option « bascule legere » (`lampe leger 1`, desactivee par defaut, non prouvee)

- **Vers Tx :** CE ← 0, `LIGHT_SLEEP`, CFG1 ← 0x40, IRQ1 ← 0x70, FLUSH_RX, FLUSH_TX, MASK &= ~PRM_RX, PKT1 ← 0x20, DPL1 ← 0x01, DPL2 ← 0x04, ENAA ← 0x01. Environ 0,3 ms.
- **Vers Rx :** CE ← 0, `LIGHT_SLEEP`, CFG1 ← 0x40, ENAA ← 0, DPL2 ← 0, DPL1 ← 0, PKT1 ← 0, RXPW0 ← 8, IRQ1 ← 0x70, FLUSH_TX, `enterRxMode()`. Environ 0,5 ms.

On ne l'adopte (valeur par defaut, et ecoute dans les ecarts d'une rafale) qu'apres T10 : le PID doit continuer d'une rafale a l'autre et l'accuse doit rester a ~1,6 ms.

### C.7 Option « rearmement fort » (`lampe rx fort 1`, desactivee par defaut)

Pour le rearmement de 100 ms : CE ← 0, `LIGHT_SLEEP`, attente d'au plus 200 µs que OMST ≠ RX, puis `enterRxMode()`. C'est la cause probable de la surdite (finding 5). Mesure en T10, avec `lampe rx 100 5000`.

### C.8 Durees

Les valeurs « mesure » viennent des logs tx-sem et ecoute-banc ; les autres sont des estimations.

| Etape | Duree | Source |
|---|---|---|
| reset jusqu'a la configuration possible | 40 ms, non bloquant | chemin prouve |
| `halo1StdConfigure` + verification | ~2-3 ms | ~75 transactions SPI a 1 MHz (estime) |
| passage en passif + entree RX | ~0,2 ms + ~130 µs (au plus 4,5 ms) | |
| paquet accuse | 1,6-1,7 ms | mesure |
| MAX_RT | 11,5 ms, puis reconfiguration de 43 ms | mesure |
| delai maximal d'attente | 30 ms | |
| `pollRx` sans trame | ~60 µs | |
| une trame ×3 | 43 + 3 paquets a 100 ms d'ecart + 43 (retour a l'ecoute) : surdite ~0,3 s | |
| deux trames entrelacees | 6 paquets : surdite ~0,55 s | |
| ecriture Matter jusqu'au 1er paquet | 120 ms de calme (400 ms au plus) + 43 ms, soit ~165 ms | |
| lampe debranchee jusqu'au retour de l'app | 3 tours × 5 essais × ~100 ms + 1 s + 2 s, soit ~4,5-5 s | |

### C.9 Budget de blocage de `tick()`

- Typiquement moins de 1 ms.
- Au pire :
  - `sendOne` MAX_RT : 11,5 ms ;
  - delai maximal d'attente : 30 ms ;
  - `enterRxMode` : 4,5 ms ;
  - garde Thread avant chaque paquet (build Thread) : verrou OpenThread <= 20 ms (deux mutex, 10 ms chacun), puis fin d'une trame 802.15.4 deja partie <= 6 ms ; verrou rendu au plus 13 ms apres CE=1 (MAX_RT : 11,5 ms), donc tenu <= ~19 ms ;
  - ecriture NVS : quelques dizaines de ms ;
  - L2 : ~300 ms, jusqu'a ~0,5 s si le quartz ou la calibration ne repondent pas (`waitCrystalReady` 50 ms, deux `calibrate()` de 200 ms au plus), rare ; sur symptome, une fois par minute au plus, puis toutes les 10 min une fois EN PANNE (C.5).
- Aucun masquage d'interruption, aucun `Serial.flush()` dans le pilote.
- `loop()` se termine par `delay(1)` : cede la main a IDLE et aux taches moins prioritaires.

---

## D. Modele d'etat et machine a etats

### D.1 Donnees (tache loop uniquement)

- **`target_` (consigne)** : marche, lampes (jamais 0), luminosite, temperature. **C'est ce que Matter affiche.**
- **`believed_` (etat cru)** : mis a jour quand une tranche reussit, et par les trames entendues de la telecommande.
- **`dirty_` (FLD_*)** : champs demandes par l'utilisateur et pas encore livres. Une commande utilisateur est **toujours emise**, meme si la lampe est crue deja dans cet etat : c'est la resynchronisation.
- **`confirmed_` (FLD_*)** : champs confirmes depuis le demarrage. Sert a l'affichage seulement.
- **Tranches** `SLOT_BRIGHT`, `SLOT_TEMP`, `SLOT_AUTO`, `SLOT_RAW` : `{active, pay, attempts, acks, repeats}`.
- **`lastAuto_`** : dernier numero A vu (le notre ou celui de la telecommande).
- **`selMem_`** : memoire de selection des lampes.
- **`link_`** : Unknown, Ok ou Lost.
- **`version_`** : +1 a chaque changement de `target_`. Le pont Matter s'en sert pour savoir quand refleter.

### D.2 Entrees

**`request(t, fields)`** :
- copie dans `target_` les champs de `fields` (FLAGS = marche et lampes), bornes (lampes jamais 0, luminosite 0x4C..0xFE, temperature 0..0x64) ;
- `dirty_ |= dueFields(target_, fields)` : allumee, FLAGS rend aussi la luminosite due (A4 (a), voir D.3) ;
- `pendingSince_` s'il etait vide ;
- `failures_ = 0` ; si `Backoff`, retour a `Idle` (une nouvelle intention relance tout de suite) ;
- `version_++` si la consigne a change ;
- `replan()`.

**`pressAuto()`** :
- consigne eteinte : `autoIgnoredOff++` et renvoie false. L'effet de A lampe eteinte n'est pas connu.
- tranche AUTO deja active : les appuis fusionnent ; renvoie true.
- sinon : `c = nextAuto(lastAuto_)`, `lastAuto_ = c` (numero reserve), `SLOT_AUTO = makeAuto(true, target_.lamps, c)`.

**`sendRaw(p, force, n, gap)`** :
- refuse toujours le premier octet 0x0A et `FA` ;
- sans `force`, refuse aussi Service, Reserved et Invalid ;
- sinon `SLOT_RAW = {p, repeats = n}` et `rawGapMs_ = gap`. Cette tranche passe avant toutes les autres et fait exactement `n` paquets.

**`believe(p)`** : applique `p` comme une trame de la telecommande, sans rien emettre.

### D.3 Planification (`halo1::plan`, pure)

```
plan(t, b, dirty):
  if !t.power:
     if dirty & FLAGS: TEMP = makeTemp(false, t.lamps, b.temp)   // temperature CRUE : un reglage
                                                                   // differe ne fuit pas dans l'extinction
     return                                                        // BRIGHT/TEMP restent a livrer
  if dirty & (BRIGHT | FLAGS): BRIGHT = makeBright(true, t.lamps, t.bright)   // decision A4 (a)
  if dirty & TEMP:             TEMP   = makeTemp(true, t.lamps, t.temp)
```

Avec A4 (b), la ligne BRIGHT devient : FLAGS seul donne `TEMP = makeTemp(true, t.lamps, b.temp)`, et `dueFields` renvoie `fields` tel quel.

`dueFields(t, fields)` : si `t.power` et `fields & FLAGS`, ajoute `FLD_BRIGHT`. Sans cela, quand la tranche de temperature finit avant celle de luminosite (un paquet perdu), `C3 xx` couvre FLAGS, la luminosite n'est plus due et la lampe s'allume a sa propre luminosite : l'inconvenient de A4 (b).

`coveredBy(p, t)`, avec `t` borne comme par les constructeurs (lampes 0 → avant) :
- `FLD_FLAGS` si la marche et les lampes de `p` egalent celles de `t` ;
- `FLD_BRIGHT` si `p` est une trame luminosite allumee avec la valeur `t.bright` ;
- `FLD_TEMP` si `p` est une trame temperature allumee avec la valeur `t.temp`.

Une trame d'extinction n'efface que FLAGS : la luminosite ou la temperature differee reste a livrer. Une trame tiree de `plan()` couvre toujours ses champs : sinon la tranche serait rearmee apres chaque rafale, sans fin (test hote de livraison).

Cas testes sur l'hote :

| Cru | Consigne, champs a livrer | Trames |
|---|---|---|
| allumee, deux, A5, 35 | eteinte (FLAGS) | `43 35` |
| eteinte, deux, temp 64 | allumee (FLAGS) | `C5 A5` |
| allumee, deux | avant seule (FLAGS) | `C4 A5` |
| allumee, deux | lum C0 (BRIGHT) | `C5 C0` |
| allumee, deux | temp 00 + lum 4C | `C5 4C` et `C3 00`, entrelacees |
| eteinte | lum 60 (BRIGHT), reste eteinte | aucune (differe) ; puis allumee donne `C5 60` |
| eteinte, temp crue 64 | temp 10 differee, puis eteinte (FLAGS) | `43 64`, pas `43 10` |
| allumee, avant seule | A (compteur 3) | `E0 03`, apres les tranches d'etat |

### D.4 Tranches, ordonnancement et preemption

**`replan()`** :
- `p = plan(target_, believed_, dirty_)` ;
- `setSlot(BRIGHT, p.bright, p.pb)` et `setSlot(TEMP, p.temp, p.pt)`.

**`setSlot(s, want, pay)`** :
- `!want` et tranche active : si `acks > 0`, `applyBelieved(ancienne)` ; desactiver ; `cancelled++`.
- tranche active avec la meme charge : on garde les compteurs.
- tranche active avec une autre charge (un curseur qui bouge) : si `acks > 0`, `applyBelieved(ancienne)` ; `preempted++` ; nouvelle charge, compteurs a 0.

La valeur finale obtient donc toujours sa rafale complete. Les valeurs intermediaires peuvent partir une ou deux fois, comme les trames isolees de la molette.

**`pickSlot()`** :
- RAW en premier, seule ;
- sinon alternance BRIGHT / TEMP entre les tranches actives ;
- AUTO seulement quand ni BRIGHT ni TEMP n'est active. Au premier paquet de AUTO, on refait ses drapeaux avec les lampes de la consigne, sans changer le numero (Q8).

**`tick()`** :

```
if (!radio.present()) return;
radio.service(now);
if (radio.restartWanted()) { if (restart_ && restart_()) stats.restarts++; radio.restartDone(); }
if (phase_ == Backoff && now >= retryAt_) phase_ = Idle;
if (phase_ == Idle && anyActive()):
   if (now - remoteAt_ < 250 && now - pendingSince_ < 2000) -> attendre (holdoffs++)
   else { radio.request(Tx, now); phase_ = Burst; nextTxAt_ = now; }
if (phase_ == Burst):
   if (!anyActive()) { phase_ = Idle; lastTxEndAt_ = now; pendingSince_ = 0; }
   else if (radio.ready(Tx) && now >= nextTxAt_) sendPacket(now);  // nextTxAt_ = debut + ecart
if (phase_ != Burst):
   if (listening_) { radio.request(Rx, now); if (radio.ready(Rx) && radio.pollRx(now, raw)) onAir(decodeAir(raw, radio.air()), now); }
   else radio.request(Sleep, now);
selMem_.update(target_, now, HALO1_SELECTION_STABLE_MS);
maybePersist(now);
```

**`onVerdict`** :
- `attempts++`.
- Ack : `acks++`, `link_ = Ok`, `lastAckAt_ = now`.
- AckForeign : non compte ; si `fLen == 2`, `onRemotePayload`.
- La tranche est terminee quand :
  - `attempts >= repeats` et `acks >= min(minAcks, repeats)`, ou
  - `attempts >= max(maxAttempts, repeats)`.
  - Pour RAW : exactement `repeats` paquets.

**`complete`** :
- **Reussite** (`acks >= min(minAcks, repeats)`) :
  - AUTO : `autoSent++`, `confirmed_ &= ~FLD_BRIGHT` (le mode auto fait deriver la luminosite).
  - RAW Temp/Bright : applique comme une trame de la telecommande (cru, consigne, `dirty_`).
  - Sinon : `applyBelieved(pay)`, puis `dirty_ &= ~coveredBy(pay, target_)`.
  - Dans tous les cas : `failures_ = 0`, puis `replan()`.
- **Echec** : voir D.5. Si `acks >= 1`, `applyBelieved(pay)` quand meme : c'est la meilleure estimation, sachant qu'un accuse ne prouve pas que la trame a ete appliquee (tx-sem-1).

### D.5 Echecs

**`fail()`** :
- `failures_++`.
- Si `failures_ <= planRetries` (2) :
  - la tranche est rearmee (compteurs a 0) ;
  - `phase_ = Backoff`, `retryAt_ = now + retryMs × failures_` (1 s puis 2 s) ;
  - on ecoute pendant l'attente.
- Sinon, `giveUp()` :
  - `target_ = believed_`, `dirty_ = 0`, toutes les tranches desactivees (A abandonne) ;
  - `link_ = Lost`, `version_++`, `giveUps++` ;
  - message toujours affiche : `[lampe] injoignable : consigne abandonnee`.
  - Matter revient alors a l'etat cru en ~5 s.

Un accuse de la lampe entendu (longueur 0) pendant `Backoff` fait `retryAt_ = now`, et `link_` passe de Lost a Unknown.

### D.6 Suivi de la telecommande (`onAir`)

| `classify` | Action |
|---|---|
| CrcBad | `rxCrcBad++` |
| LampAck (len 0, NO_ACK 1) | `rxLampAcks++` ; relance d'une reprise en attente (D.5) |
| Service (FF/FE/FD 00 a NO_ACK=0, FA xx a NO_ACK=1) | `rxService++`, `remoteAt_ = now` |
| Reserved (91 xx, 89 xx : favori) | `rxReserved++`, `remoteAt_ = now`, `remoteAuto_.reset()` |
| Invalid | `rxInvalid++` |
| Auto | voir ci-dessous |
| Temp / Bright | `rxState++`, `remoteAt_ = now`, `remoteAuto_.reset()`, `onRemotePayload` |

Trame **Auto** :
- `rxAuto++`, `remoteAt_ = now`, `lastAuto_ = valeur` ;
- compteur d'appuis : `remoteAutoPresses_++` si `remoteAuto_.feed(valeur, now)` (`AutoPressFilter`, B.2). La telecommande emet chaque appui en 3 copies du meme numero a ~100 ms : une copie (meme numero, moins de 1 s avant ou apres la precedente) ne compte pas. Une trame Temp, Bright ou favori entre deux A fait repartir le numero a 01 (PROTOCOL.md) : `remoteAuto_.reset()`, et le A suivant compte. Le pont reflete chaque changement de `remoteAutoCount()` par une impulsion d'EP4 (E.5), sans rien emettre ; nos propres trames A ne passent jamais par `onAir` ;
- si notre tranche AUTO a le meme numero et 0 accuse, on lui en donne un nouveau : `nextAuto(valeur)` ;
- `confirmed_ &= ~FLD_BRIGHT` ;
- **aucun changement de marche ou de lampes**.

`onRemotePayload(p)` :
- `applyState(believed_, p)` et `applyState(target_, p)`, avec bornes (luminosite < 0x4C ramenee a 0x4C, temperature > 100 ramenee a 100) ;
- `dirty_ &= ~(FLD_FLAGS | champ du selecteur)` : la telecommande gagne champ par champ, et un reglage Matter en attente sur un autre champ part avec les nouveaux drapeaux ;
- `confirmed_ |= ...` ;
- si la consigne a change, `version_++` ; si l'etat cru a change, sauvegarde programmee ;
- `replan()`.

Pas de filtre de doublons pour les trames d'etat : elles sont absolues et `applyState` ne signale un changement que s'il y en a un.

Nos propres trames ne sont jamais entendues : un seul emetteur, en PTX pendant l'emission.

### D.7 Numero du bouton A

- `nextAuto(last)` : 0 ou 255 donne 1, sinon `last + 1`. Jamais 0, jamais `last`.
- Le numero est reserve au moment de l'appui et reutilise tel quel a chaque reprise : la lampe ignore un numero deja traite, donc jamais de double declenchement.
- Numero inconnu (NVS vide) : 1. La telecommande repart elle aussi a 01 apres une pause ; un doublon est donc possible, et l'appui serait ignore. T7 verifie que des numeros > 5 sont acceptes. Si oui, on pourra commencer plus haut (C7).

### D.8 Coalescence et attente apres la telecommande

- **Cote Matter** (pont, E.3) : 120 ms de calme ou 400 ms au plus, puis un seul `request()`.
- **Cote CLI** : aucune coalescence, chaque commande attend `waitIdle`.
- **Dans le pilote** : on n'ouvre pas de rafale moins de 250 ms apres une trame de la telecommande, reveils compris, tant que la demande en attente a moins de 2 s. On ne se bat pas avec la molette : elle envoie une trame isolee toutes les ~112 ms (pair-5-verif.log).
- **Pendant une rafale** : les nouvelles consignes prennent effet au paquet suivant, par preemption (D.4).

### D.9 Demarrage

- `believed_ = target_ =` blob NVS s'il est valide.
- Sinon valeurs par defaut : eteinte, deux lampes, A5, 35 ; `lastAuto_ = 0`.
- `confirmed_ = 0`, `selMem_.reset(target_.lamps)`.
- **Rien n'est emis.**
- Pas de reaffirmation automatique : elle annulerait un reglage fait a la telecommande pendant que l'ESP32 etait arrete. La premiere commande de l'utilisateur envoie les drapeaux, plus la luminosite affichee si c'est un allumage (A4).

### D.10 NVS

**Espace de noms `halo1`**, nouveau. On ignore `benqhalo`, dont `chan` et `rate` peuvent contenir des valeurs d'essai ; les outils de banc continuent de s'en servir.

- **`addr`** : 4 octets en ordre registre. Absent : `kDefaultAddrReg`. Ecrit seulement par `lampe adresse`, qui refuse ce que refuse `addressAllowed`.
- **`etat`** : 8 octets `{ver=1, power<<7 | lamps, bright, temp, lastAuto, 0, 0, crc8(7 premiers)}`. Rejete (valeurs par defaut) si la version, le CRC ou les plages sont faux.

Regles d'ecriture de `etat` :
- l'echeance est a +10 s du dernier changement de l'etat cru, et jamais plus de 60 s apres le premier changement non sauvegarde ;
- on n'ecrit qu'au repos (pas de tranche active), au moins 500 ms apres une emission, et seulement si le blob differe du dernier ecrit ;
- ecriture immediate sur `reboot` et `lampe sauve`.

L'usure est de quelques dizaines d'entrees par jour, dans une partition de 20 Ko partagee avec Matter : negligeable.

`decommission` appelle `factory_reset()`, qui efface probablement toute la partition « nvs ». Consequence : adresse par defaut et etat par defaut, sans danger. A verifier en T13.

---

## E. Couche Matter (`matter_bridge.cpp` reecrit, meme `matter_bridge.h`)

### E.1 Endpoints

Ils sont crees dans cet ordre, ce qui donne les numeros 1 a 4 sur un noeud neuf.

| EP | Classe | Attributs |
|---|---|---|
| 1 | `MatterColorTemperatureLight mainLight` | OnOff = `t.power` ; CurrentLevel suit `t.bright` ; ColorTemperatureMireds suit `t.temp` ; PhysicalMin/MaxMireds = 153/370 via `setAttributeVal` (comme l'actuel 163-168) |
| 2 | `MatterOnOffLight frontLamp` (ou Plugin si `HALO1_SELECTORS_AS_LIGHTS 0`) | OnOff = `t.power && (t.lamps & F_FRONT)` |
| 3 | `MatterOnOffLight backLamp` (idem) | OnOff = `t.power && (t.lamps & F_BACK)` |
| 4 | `MatterOnOffPlugin autoButton` (`HALO1_EXPOSE_AUTO`, a 0 par defaut depuis le 23/09 : EP4 absent) | passe a on sur ecriture, repasse a off apres l'impulsion (1 s par defaut, `matter impulsion <300..15000>` en NVS `halo1/impulsion`) ; un A de la telecommande entendu fait la meme impulsion, sans rien emettre (E.5) |

On supprime l'interrupteur capteur (le Halo 1 n'a pas de capteur de presence), la prise maitre et la luminosite arriere.

### E.2 Correspondances

**Luminosite.** Table construite au demarrage (`mapInit(HALO1_LEVEL_GAMMA)`) :
- `raw(L) = 0x4C + round(178 × ((L-1)/253)^γ)` pour L = 1..254, et `raw(0) = raw(1)`.
- **Plancher `kMatterLevelFloor = 4`** (terrain du 23/09) : Apple Home affiche CurrentLevel en pourcentage entier ; le niveau 1 y devient 0 %, et une lumiere allumee a 0 % s'affiche au maximum (lampe a 0x4C, reglee a la molette, montree pleine). Sa formule n'est pas connue : `L/254`, ou `(L-1)/253` si la plage part de MinLevel = 1, arrondi ou tronque. 3 tomberait a 0 % en `(L-1)/253` tronque (0,79 %) ; 4 donne au moins 1 % dans les quatre cas (1,57 % et 1,19 %). `raw(0..4) = 0x4C` quel que soit γ (rien ne change a γ = 2, ou les niveaux 1..14 donnent deja 0x4C), et aucun niveau sous 4 n'est jamais rapporte.
- Avec γ = 1, formule entiere exacte au-dessus du plancher : `raw(L) = 0x4C + ((L-1)*178 + 126)/253`, inverse `L(r) = 1 + ((r-0x4C)*253 + 89)/178`. L'aller-retour est l'identite pour toute valeur atteinte depuis le plancher ; seules 0x4D et 0x4E (niveaux 3 et 4 avant le plancher) ne le sont plus : elles se rapportent au niveau 5 (0x4F).
- Inverse general (niveau rapporte) : `levelFromRaw(r)` = plus petit L ≥ 4 tel que `raw(L) ≥ r`.
- Points a γ=2 (verifies) : L64 = 0x57, L127 = 0x78, L138 = 0x80, L171 = 0x9C, L191 = 0xB0, L254 = 0xFE.
- 15 valeurs brutes du haut sont inaccessibles depuis Matter (pas de 2). La telecommande peut les atteindre.

**Temperature.** Lineaire en mireds :
- `temp = ((clamp(m,153,370) - 153)*100 + 108)/217` ;
- `m = 153 + (min(t,100)*217 + 50)/100` ;
- aller-retour temp → mired → temp exact pour les 101 valeurs ;
- 0x00 (le plus froid) = 153 mireds, 0x64 (le plus chaud) = 370 mireds.
- Les Kelvin reels ne sont pas mesures.

**Affichage stable.** Au moment de refleter :
- `L_affiche = (L_attribut ≥ 4 && raw(L_attribut) == t.bright) ? L_attribut : levelFromRaw(t.bright)` ;
- meme regle pour les mireds (sans plancher).

C'est idempotent, et la valeur qu'un controleur a ecrite ne « saute » jamais vers une voisine, sauf sous le plancher : 1 a 3 (0x4C) s'affichent 4.

### E.3 Callbacks et boite d'intentions

Les callbacks tournent dans la tache CHIP. Ils ne touchent ni au SPI ni a `lamp`, et renvoient toujours true.

```cpp
static TaskHandle_t sLoopTask;  // pris dans matterBridgeBegin() (setup = tache loop)
static portMUX_TYPE sInboxMux = portMUX_INITIALIZER_UNLOCKED;
static halo1::MatterIntents sInbox;
static uint32_t sInFirst, sInLast, sBootMs, sAutoPulseAt, sSeenVersion, sLastReflect;
static bool sForceReflect = true;
// Nos reflets passent par attribute::update() et redeclenchent les callbacks, mais
// toujours dans la tache loop ; les ordres des controleurs arrivent dans la tache CHIP.
static inline bool ownEcho() { return xTaskGetCurrentTaskHandle() == sLoopTask; }
template <class F> static bool post(uint8_t bit, F set) {
  if (ownEcho()) return true;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sInboxMux);
  if (!sInbox.has) sInFirst = now;
  sInLast = now; sInbox.has |= bit; set(sInbox);
  portEXIT_CRITICAL(&sInboxMux);
  return true;
}
static bool onMainOnOff(bool on) { return post(halo1::IN_POWER, [=](halo1::MatterIntents &i) { i.power = on; }); }
static bool onMainLevel(uint8_t l) { return post(halo1::IN_LEVEL, [=](halo1::MatterIntents &i) { i.level = l; }); }
static bool onMainMired(uint16_t m) { return post(halo1::IN_MIREDS, [=](halo1::MatterIntents &i) { i.mireds = m; }); }
static bool onFront(bool on) { return post(halo1::IN_FRONT, [=](halo1::MatterIntents &i) { i.front = on; }); }
static bool onBack(bool on) { return post(halo1::IN_BACK, [=](halo1::MatterIntents &i) { i.back = on; }); }
static bool onAuto(bool on) { return on ? post(halo1::IN_AUTO, [](halo1::MatterIntents &) {}) : true; }
```

On enregistre seulement les callbacks par attribut : `onChangeOnOff`, `onChangeBrightness`, `onChangeColorTemperature`. **Jamais `onChange`**, qui transmet les valeurs en cache des autres attributs.

### E.4 Regles d'intention (`resolveMatter`, pure, testee)

Notations : `baseF = base.power && (base.lamps & F_FRONT)`, et de meme `baseB`. `f` = intention avant si presente, sinon `baseF` ; `b` = intention arriere si presente, sinon `baseB`. `sel` = une intention avant ou arriere est presente. `mem = memoryLamps`, jamais 0.

| Regle | Condition | Resultat |
|---|---|---|
| R1 | EP1 eteint | `power = false`, `lamps = mem` : l'extinction gagne toujours |
| R2 | `sel` et (`f` ou `b`) | `power = true`, `lamps` = lampes allumees |
| R2' | pas de `sel`, et deja allumee | marche et lampes inchangees |
| R3a | EP1 allume | `power = true`, `lamps = mem` |
| R3b | `sel` et aucune lampe allumee | `power = false`, `lamps = mem` (jamais 0) |

- Toute intention marche ou lampe ajoute `fields |= FLD_FLAGS` : la commande est toujours emise.
- Niveau : `bright = rawFromLevel(max(1, L))`, `FLD_BRIGHT`, **y compris lampe eteinte** (differe jusqu'a l'allumage). Venu avec EP1 on, il est ecarte s'il n'ajoute rien : meme valeur brute que la consigne (1..3 pour 0x4C), ou niveau affiche pour elle.
- Mireds : `temp = tempFromMired(m)`, `FLD_TEMP`, meme regle.
- A : `fireAuto` seulement si la consigne finale est allumee **et** que la fenetre ne contient aucune intention marche ou lampe (garde-fou de groupe, A2).

Cas testes :
- « tout eteindre » envoye dans les deux ordres, dans une ou deux fenetres : la memoire garde « deux » grace a `SelectionMemory` (2 s) ;
- scene {EP1 on, avant on, arriere off} dans n'importe quel ordre : avant seule ;
- niveau ecrit lampe eteinte : differe ;
- A dans la meme fenetre que EP1 on : ignore.

### E.5 `matterBridgePoll()` (tache loop)

1. **Boite d'intentions.** Sous `sInboxMux`, on la vide si `now - sInLast ≥ 120` ou `now - sInFirst ≥ 400`.
   - Si `now - sBootMs < 2000` : intentions ignorees, journal, `sForceReflect = true`. Un controleur ne peut pas ecrire aussi tot ; c'est un garde-fou pour ne jamais emettre au demarrage, et T11 verifie que le compteur reste a 0.
   - Sinon : `r = resolveMatter(lamp.target(), in, lamp.memoryLamps())`.
     - Si `r.fields`, `lamp.request(r.target, r.fields)`.
     - Si `r.fireAuto && lamp.pressAuto()`, `sAutoPulseAt = now`.
   - **A de la telecommande** (`HALO1_EXPOSE_AUTO`) : si `lamp.remoteAutoCount()` a change (appuis entendus dans `onAir`, les 3 copies d'un appui comptees une fois, D.6), meme impulsion d'EP4, que le reflet met a on. Rien n'est emis, aucune intention : notre ecriture d'EP4 est ecartee par `ownEcho()`. L'impulsion part de la montee reellement ecrite (ou d'EP4 deja a on), pas de l'appui entendu : la boite d'intentions (jusqu'a 400 ms), le verrou de la pile (CASE) ou un echec d'ecriture la retardent, et un echec est retente au passage suivant. Montee pas faite 3 s apres l'appui : abandon (compteur « non reflete(s) » de `matter`).
     - Dans tous les cas, `sForceReflect = true` : on realigne sur la consigne resolue, par exemple EP4 repasse a off tout de suite si A est refuse.
2. **Reflet**, seulement si la boite est vide et si `sForceReflect` ou (`lamp.version() != sSeenVersion` et `now - sLastReflect ≥ 250`), ou si l'impulsion auto est echue :
   - `st = esp_matter::lock::chip_stack_lock(portMAX_DELAY)` ; si FAILED, on retente au passage suivant ;
   - pour chaque attribut (EP1 OnOff, CurrentLevel, Mireds ; EP2, EP3 OnOff ; EP4 OnOff = impulsion en cours) : `getAttributeVal`, calcul de la valeur voulue (E.2), et si elle differe, `updateAttributeVal` en gardant le type de la valeur lue ;
   - `sSeenVersion = lamp.version()`, `sForceReflect = false` ;
   - `chip_stack_unlock()` seulement si `st == SUCCESS`.

   On utilise `updateAttributeVal` et pas les setters : l'appel passe par PRE_UPDATE, met le cache de la bibliotheque a jour et corrige aussi une valeur restauree depuis NVS que les setters sauteraient (cache egal). Le verrou garantit qu'aucune ecriture d'un controleur ne s'intercale pendant le reflet.
3. **`matterBridgeBegin()`**, dans cet ordre :
   1. `sLoopTask = xTaskGetCurrentTaskHandle()`, `sBootMs = millis()`. La table gamma est deja construite par `lamp.begin` (C.1).
   2. `t = lamp.target()`.
   3. `mainLight.begin(t.power, levelFromRaw(t.bright), miredFromTemp(t.temp))`.
   4. Mireds physiques 153/370.
   5. `frontLamp.begin(...)`, `backLamp.begin(...)`, `autoButton.begin(false)`.
   6. Enregistrement des callbacks.
   7. `Matter.begin()`.
   8. `sForceReflect = true`.

### E.6 Invariants

1. Seule une intention validee ou une commande CLI fait emettre : rien au demarrage, a la mise en service, sur minuterie ou en reaction a une trame entendue.
2. Callbacks : jamais de SPI ni d'appel a `lamp` ; toujours true.
3. `lamp`, la radio et la NVS pilote ne sont utilises que depuis la tache loop.
4. Matter affiche la consigne. Elle ne change hors ecriture Matter que sur une trame de la telecommande, un abandon ou une commande CLI.
5. On ne reflete jamais tant que la boite contient des intentions : un curseur en cours ne revient pas en arriere.
6. EP1 OnOff = `t.power`, EP2 = `power && front`, EP3 = `power && back`, EP4 vaut false sauf pendant l'impulsion (1 s par defaut, `matter impulsion`). CurrentLevel jamais sous 4.

Identify : arc-en-ciel sur la WS2812 (src/status_led.*), jamais la lampe, ce qui voudrait dire emettre.

---

## F. Sort du code Halo 2 et des diagnostics

### F.1 C1 : neutraliser

- `BenqHalo::tick()` fait `return;` dans tous les builds, avec un commentaire.
- main.cpp:184-193 : suppression de la branche HELLO (`pollNow`, `desired = reported`, `printState`) et du message « lance 'find' ».
- CLI `poll`, `send`, `find`, `pair`, `sniff`, `tail` : remplaces par « commande Halo 2 retiree : voir 'txack', 'ecoute' (puis 'lampe') ». Pour `pair`, suggerer `ecoute B0000159 5`.
- Garde de `txack` (cli.cpp:648-656) : refuser aussi `B0000159` et `590100B0`.

### F.2 C6 : supprimer, une fois le pilote valide au banc

- **halo.h :**
  - l.8-50 : en-tete, `HALO_CMD_*`, `HALO_PAIRING_ADDRESS` ;
  - l.52-70 : `HaloState`, `HaloPhase` ;
  - `desired`, `reported`, `setTail`/`tail`/`tail_`, membres du finder et du sniffer, `txBusy_`, `kBurstGapMs`, `rxEvents()`, `mode()`.
- **halo.cpp :**
  - `setTail` 87-91, `prepareToTransfer` 150-168, `resetRadio` 170-175 ;
  - `sendWithAck` 209-228 (**B3**), `readAck`, `sniffOnce` 240-271 ;
  - `buildPayload` 277-298, `frameCrc*` 300-331, `validate`/`parseStatus` 333-361 ;
  - `requestPush*`, `pollNow`, `settled`, `checkTxFifo`, `adoptReported` 367-419 ;
  - `tick`/`tickNormal`/`tickSniffer`/`tickFinder` 425-535 ;
  - finder 553-691 et 1154-1194, `startSniffer` 1200-1212, `printState` 1226-1234.
- **B1 :** `listenHalo1` 3473-3647, `txHalo1` 3664-3705, `halo1Crc` 4060-4070, `groupVerdict` 4075-4117, et les commandes `benq` et `tx6`.
- **B13 :** `txRaw` 3722-3758 et `txraw`.
- `probeRxSequences` : retirer l'appel a `frameCrcOk` (986) et afficher les octets bruts.
- **config.h :** les `HALO_*` 116-142 et le commentaire perime sur `homeSpan.poll()`.
- **NVS :** `prefs.remove("tail")` une fois, dans `loadConfig`.

### F.3 A garder, dans les deux builds

- Tous les autres outils de la liste (c) du lecteur.
- `setMode(HaloMode::Normal)` → `prepareToSniff` reste la base des diagnostics : `sharedRadioConfig` et `prepareToSniff` sont conserves.
- `txAck`, `sniffStd`, `prxAck` gardent leurs traces et appellent le `configStdAutoAck` de halo1_radio.cpp, dont le comportement est identique. En C6, `sniffStd` passe a `halo1::decodeAir` : un seul decodeur.
- `BenqHalo::begin` est garde : c'est le demarrage prouve, et il sert de relance L2.
- `printInfo` est allege en C6 (plus d'extremite ni d'etat Halo 2).

### F.4 Bogues d'audit corriges au passage

- **B3, B1, B13 :** supprimes avec leur code (C6).
- **B11** (C3) : `xo` lit avec `strtol(arg, nullptr, 0)` et accepte `off` / `-1`, qui appellent `setXoTrim(-1)`. Le pilote applique `gXoTrim`.
- **B15** (C3) : deplacer le commentaire RSSI_NEGDB de la ligne `B0_XO1` vers `B0_RSSI2`.
- **bc5602.h:179-180** (C3) : `enterRxMode` ne touche pas a CE et fait 3 tentatives ; ajouter l'alias `STATUS_RX_EMPTY` (le « RX_DR » de STATUS vaut 0 quand des donnees sont la).
- **halo.h:219** (C6) : commentaire perime « CRC verifie par le materiel ».
- **Docs :**
  - PROTOCOL.md : les essais tx-sem « 3 300 » ont tourne a **500 ms** (bornage de la CLI) ; refaire l'en-tete (l.3-75 : semantique confirmee, retirer les hypotheses refutees et le materiel Halo 2 presente comme actuel) ;
  - AUDIT : B2 et B3 fermes, B1 et B13 retires ;
  - README:12-18 : nouveaux endpoints.
- **Hors perimetre** (outils CC2500, tache a part) : B6, B7, B8, B12, B14, B16.

---

## G. Commandes CLI du chemin produit (`lampe ...`, dans `cli_lampe.cpp`)

### G.1 Invalidation de la radio

A la fin de `handleLine` (cli.cpp, apres la chaine de `if`, qui n'a pas d'autre `return`) :

```
if (!radioFree(line)) lamp.invalidateRadio();
```

Liste blanche : `lampe help ? matter debug chiplog cause wifi decommission reboot`.

Toute autre commande (txack, ecoute, rfinit, regs, xo, cc*, swd...) force une reconfiguration complete au prochain usage. Sans risque :
- la CLI tourne dans la meme tache que `tick()` ;
- `sendOne` est atomique ;
- une rafale interrompue reprend apres reconfiguration (PID a 0, trames absolues).

### G.2 Commandes d'etat

Elles mettent a jour la consigne comme Matter, appellent `waitIdle(6000)` et affichent **une** ligne de bilan, par exemple :

```
ok C5 A0 3/3 accuses (1650 1602 1611 us) -> cru : allumee deux lum A0 temp 35
```

| Commande | Effet |
|---|---|
| `lampe` ou `lampe etat` | adresse (registre et air), canal 5, 125 kbps, ecoute, trace ; consigne et champs a livrer ; etat cru et champs confirmes ; tranches (charge, essais/accuses) ; lien et dernier accuse ; mode radio et compteurs ; dernier A ; memoire des lampes ; ecarts avec NVS `benqhalo/chan`, `rate` |
| `lampe on` / `lampe off` | via `resolveMatter` (intention EP1) : memes regles que Matter |
| `lampe avant on\|off`, `lampe arriere on\|off` | via `resolveMatter` (EP2, EP3) |
| `lampe mode avant\|arriere\|deux` | lampes absolues et allumage (FLAGS) |
| `lampe lum <4C..FE hex>` / `lampe niveau <1..254>` | brut / par la table gamma |
| `lampe temp <0..100 decimal>` / `lampe mired <153..370>` | brut / par la conversion |
| `lampe auto` | `pressAuto` ; « refuse : lampe eteinte » si la consigne est eteinte |
| `lampe sync` | `reassert()` : tout ce qui est connu est renvoye |
| `lampe rampe <de> <a> <pas> <ms>` | simule un curseur : `lum` tous les `ms` en faisant tourner `tick()` (teste la preemption) |
| `lampe brut <XXYY> [n 1..10] [ecart 5..2000] [force]` | charge brute, n paquets exactement, verdict par paquet (IRQ1, RT2, µs). Toujours refuse : 1er octet 0x0A, `FA`. Sans `force`, refuse aussi Service, Reserved et Invalid |
| `lampe croire <XXYY>` | cru et consigne, sans emettre |

### G.3 Commandes de banc et de reglage

| Commande | Effet |
|---|---|
| `lampe rafale <n> [min_accuses] [max]`, `lampe ecart <5..2000>` | reglages en RAM |
| `lampe ecoute 0\|1` | ecoute de fond (diag 0, produit 1) |
| `lampe attends <ms>` | fait tourner `tick()` seulement (observer l'ecoute) |
| `lampe rx <rearm ms> <silence ms>`, `lampe rx fort 0\|1`, `lampe leger 0\|1` | options C.4 (rearmement 10..200, silence > 2 rearmements), C.7, C.6 |
| `lampe gamma <x.x>` | reconstruit la table (RAM) |
| `lampe garde 0\|1` | build Thread : verrou OpenThread tenu pendant chaque paquet, apres la fin d'une trame Thread en cours (defaut 1, RAM) ; ailleurs absente. Compteurs dans `lampe stats` |
| `lampe trace 0\|1` | journal par evenement, sans jamais bloquer : `[lampe] TX C5 A0 #2/3 ACK 1650 us RT2 00`, `[lampe] RX tele PID 2 C4 BC -> allumee avant lum BC`, `[lampe] RADIO reconf silence #118` |
| `lampe stats [raz]` | compteurs |
| `lampe regs` | RFCH/DM1/RT1 relus, version de puce, 4 instantanes |
| `lampe decode <16 hex>` | ex. `08627F030C800000` donne len 2, PID 0, C4 FE, CRC 0619 OK |
| `lampe autotest` | `halo1::selfTest`, sans radio |
| `lampe adresse [8 hex]` | lecture / ecriture NVS `halo1/addr` |
| `lampe oublie` / `lampe sauve` | effacer / ecrire le blob `etat` |

Autres changements CLI :
- `info` affiche en plus `lamp.printStatus` ;
- `reboot` appelle `lamp.persistNow()` avant le redemarrage ;
- l'aide perd les lignes Halo 2 (151-170) et gagne les lignes `lampe`.

---

## H. Etapes d'implementation

Chaque etape se termine par :

```
tools/test_halo1.sh            # a partir de C2
pio run -e esp32c6diag
pio run -e esp32c6supermini    # taille < 3 Mo (marge actuelle ~0,7 Mo)
```

Pour flasher : `pio run -e esp32c6diag -t upload --upload-port /dev/cu.usbmodem144401`.

On ne committe pas le `.pyc` modifie : `git checkout -- tools/audit/indep_pll/__pycache__/pll.cpython-311.pyc`, et ajouter `__pycache__/` au `.gitignore`.

**C1 « Neutraliser la couche Halo 2 du chemin produit »**
- Contenu : F.1.
- A partir d'ici, le build produit n'emet plus rien de lui-meme.
- Banc T0a, sans emission : B lance `ecoute 4FF0FD63 5 90000` pendant que A redemarre 3 fois ; 0 COMMANDE attendue.

**C2 « Protocole Halo 1 pur et tests hote »**
- Fichiers : `halo1_proto.*`, `halo1_map.*`, `tools/host_tests/test_halo1.cpp`, `tools/test_halo1.sh` :

  ```sh
  clang++ -std=c++17 -Wall -Wextra -Werror -Isrc src/halo1_proto.cpp src/halo1_map.cpp tools/host_tests/test_halo1.cpp -o "${TMPDIR:-/tmp}/test_halo1" && "${TMPDIR:-/tmp}/test_halo1"
  ```

- Vecteurs dores (adresse sur l'air 63 FD F0 4F), en PID/NO_ACK, charge, CRC, brut :
  - 0/0 `C3 35` F7A9 `08 61 9A FB D4 80 00 00`
  - 1/0 `C3 35` 99C9 `09 61 9A CC E4 80 00 00`
  - 1/0 `C4 BC` 00FF `09 62 5E 00 7F 80 00 00` (valide sur l'air)
  - 0/0 `C4 FE` 0619 `08 62 7F 03 0C 80 00 00`
  - 2/0 `C4 FE` DAD9 `0A 62 7F 6D 6C 80 00 00`
  - 0/0 `E1 01` E1FA `08 70 80 F0 FD 00 00 00`
  - 2/0 `E1 01` 3D3A `0A 70 80 9E 9D 00 00 00`
  - 0/0 `42 35` DF00 `08 21 1A EF 80 00 00 00`
  - 0/0 `C5 A0` 8E13 `08 62 D0 47 09 80 00 00`
  - accuse reel len0 1/1 5C90 `01 AE 48 00 00 00 00 00`
  - accuse len0 3/1 1C14 `03 8E 0A 00 00 00 00 00`
- Autres tests :
  - basculer un bit donne un CRC faux ;
  - `encodeAir` et `decodeAir` font l'aller-retour pour PID 0-3 et len 0-4 ;
  - table de `classify` : FF/FE/FD 00 a NO_ACK=0 → Service ; FA A8 a NO_ACK=1 → Service ; 91 00, 89 58 → Reserved ; 00 00, C6 10 → Invalid ; E1 01 → Auto ; C3 35 → Temp ;
  - tables de D.3 et E.4 ; `coveredBy` ; bornes ; `nextAuto` ; conversions (γ=1 exact, γ=2 monotone, affichage stable idempotent) ; `SelectionMemory`.

**C3 « Extraire la radio Halo 1 »**
- `halo1_radio.*` : decoupage de configStdAutoAck, deplace de halo.cpp:3776-3809 (le `static` disparait) ; `applyXoTrim` n'est plus `static` (l.12 et 2442) ; classe `Halo1Radio`.
- B11, B15, commentaires de bc5602.h.
- Banc R0.

**C4 « Pilote Halo 1 et commandes 'lampe' »**
- Fichiers : `halo1_lamp.*`, `cli_lampe.cpp`, `cli.h` (`void cmdLampe(char *arg);`), bloc `HALO1_*` de config.h, invalidation dans `handleLine`, `info`, `reboot`.
- main.cpp : `lamp.begin(...)` apres `halo.begin()`, `lamp.tick()` a la place de `halo.tick()`, `delay(1)` en fin de `loop()`.
- `matter_bridge.cpp` n'est pas encore touche : il compile, mais **on ne flashe pas le produit**.
- Banc T0 a T10 en diag.

**C5 « Pont Matter Halo 1 »**
- Section E, README.
- Build produit, puis banc T11 a T13, apres retrait de l'ancien noeud et `decommission`.

**C6 « Retirer la couche Halo 2 et les outils refutes »**
- Section F.2, `sniffStd` passe a `decodeAir`, docs (F.4).
- Les deux builds et les tests, puis R0 rapide.

**C7 « Regler les valeurs par defaut d'apres le banc »**
- Ecart, silence, rearmement fort, bascule legere, γ, numero A de depart.

---

## I. Essais sur la vraie lampe

### I.1 Installation

- **Carte A** (`/dev/cu.usbmodem144401`) : le pilote.
- **Carte B** (`/dev/cu.usbmodem11301`) : **temoin independant**. Elle reste sur le firmware C1, dont `ecoute` est l'outil prouve, et lance `ecoute 4FF0FD63 5 <ms>`. B est sourde ~8 % du temps et peut donc manquer une copie : on juge sur les 3 copies.
- Journaux : `scratchpad/serial_run.py <port> logs/drv-Tn-{A,B}.log <duree> "cmd" ...`.
- **Majid est present** pour tout essai qui emet. On ecrit la prediction dans le journal **avant** l'essai.
- La lampe est dans un etat reconnaissable.
- **Piles de la telecommande retirees**, sauf pour T8, T9 et le volet ecoute de T10.

### I.2 Liste des essais

| # | Build, etape | Procedure | Attendu |
|---|---|---|---|
| R0 | diag C3 sur A | A `txack 4FF0FD63 5 C335 3 500`, B ecoute. Puis inverser : B `txack ... C235 3 500`, A `ecoute ... 20000` | 3/3 TX_DS en 1,6-1,7 ms ; lampe conforme ; B voit 3× `C3 35`, chacune suivie d'un accuse, **PID 0, 1, 2** (premiere preuve sur l'air que le PID avance). A, avec le reset decoupe, decode les 3 `C2 35` et leurs accuses |
| T0 | diag C4 | `lampe autotest` ; `lampe decode 08627F030C800000` ; A redemarre 3 fois ; `lampe ecoute 1` pendant 60 s ; `lampe regs` | 0 echec ; decodage conforme ; **B : 0 COMMANDE, 0 accuse** (l'ecoute n'accuse jamais) ; RFCH/DM1/RT1 = 05/82/73 |
| T1 | diag | `lampe oublie`, reboot, `lampe trace 1`, `lampe on` | B : `C5 A5` ×3, PID 0/1/2, ~100 ms d'ecart, chacune accusee (**premier ecart de 100 ms depuis l'ESP32**) ; A `ok 3/3`. Les deux lampes a A5. En cas d'echec : `lampe ecart 300` puis `500` |
| T2 | diag | Enchainement (B entre crochets) : `temp 0` [`C3 00`, le plus froid] ; `temp 100` [`C3 64`] ; `mode avant` [`C4 A5`, avant seule : **premier changement de lampes porte par une trame luminosite**, A4] ; `lum 4C` [`C4 4C`] ; `lum FE` [`C4 FE`] ; attendre 3 s (`@3` pour serial_run.py : la selection avant seule doit tenir 2 s, sinon `off` donne `43 64` puis `on` `C5 60`) ; `off` [`42 64`] ; `lum 60` [**rien**] ; `on` [`C4 60`, avant a 60] ; `arriere on` [`C5 60`] ; attendre 3 s ; `avant off` [`85 60`] ; attendre 3 s ; `arriere off` [`03 64`, eteinte, memoire arriere] ; `on` [`85 60`] ; `sync` [`85 60` et `83 64` entrelacees] ; `rampe 4C FE 16 60` | Chaque prediction est tenue. Rampe : valeurs intermediaires 1 ou 2 fois, `FE` 3 fois, `preempted > 0`, lampe au maximum. Si `mode avant` ne change pas les lampes : A4 (b) |
| T3 | diag, deux lampes | `auto` ; 6 s ; `auto` ; `off` ; `auto` | `E1 01` ×3 puis la lampe baisse et remonte ; `E1 02` et nouvelle reaction ; puis « refuse : lampe eteinte », rien sur B |
| T4 (Q2) | diag | `ecart 20`, alterner `temp 0` / `temp 100` 10 fois (3 s d'ecart) ; idem `ecart 5` | Taux visible par ecart. On garde 100, sauf si 20 donne 10/10 (moins de surdite) |
| T5 (Q1) | diag | `rafale 1 1 1` ; alterner `brut C200` / `C264` 5 fois (2 s) ; un paquet apres 60 s de silence, puis apres 5 min | Informatif, le pilote garde 3. Si seuls les paquets apres un silence echouent : hypothese du reveil ; essayer alors `brut FF00 1 5 force` puis un paquet seul |
| T6 | diag | USB de la lampe debranche, `lum 80` ; rebrancher ; `lum 80` ; noter l'etat apres la coupure (Q11) ; `sync` | Trace MAX_RT, reconfiguration, reprise +1 s puis +2 s, « injoignable » ~5 s, consigne revenue a l'etat cru. Apres rebranchement : ok du premier coup, pas de STATUS 21 bloque |
| T7 | diag, `brut` | **Q3** `C335`, `C44C`, `85FE`, `C335` : chaque lampe garde-t-elle sa luminosite ? **Q4** `8300` / `8364` : la lampe arriere change-t-elle de couleur ? **Q5** `4264`, `444C`, `C235` : l'avant s'allume-t-elle au minimum ? **Q6** avant seule `C4FE`, `E0 nn`, capteur couvert puis eclaire 10 s ; `E0 nn+1` idem ; `C480` ; `D100` / `D101 force`. **Q7** `E1 n`, `C335`, `E1 n` ; `E1 n`, 60 s, `E1 n`. **Q8** avant seule, `E1 n+1`. **Numeros** `E107`, `E181`, `E1FE` | Resultats consignes dans PROTOCOL.md ; ils decident C7 (A4, depart du numero A, disposition arriere) |
| T8 | diag, **piles remises**, A en ecoute seule (`ecoute 1`, `trace 1`) | Majid : switch ×3, molette min→max, temperature, marche/arret, favori, A. `lampe` apres chaque geste | Etat cru = lampe apres chaque geste. Trames decodees par A ≈ celles de B. Favori → `91`/`89` comptes Reserved, etat final juste. A de la telecommande → `lastAuto` mis a jour, lampes inchangees |
| T9 | diag, piles | geste, puis `lum A0`, puis geste, ×5 ; Majid tourne la molette sans arret pendant `temp 0` | Suivi toujours juste apres nos rafales ; `holdoffs > 0` ; A emet au plus 2 s apres le debut de sa demande ; lampe froide a la luminosite de la molette |
| T10 | diag | `leger 1` : 20 cycles `temp 0` / `temp 100` (2 s), sans piles. Puis `rx fort 1` + `rx 100 5000`, piles, 10 min de telecommande | Bascule legere : ACK 100 %, PID qui continue d'une rafale a l'autre sur B. Rearmement fort : trames manquees ≤ T8, reconfigurations de silence en forte baisse. Adoption seulement sur succes |
| T11 | **produit** sur A | 3 redemarrages (B : 0 trame) ; mise en service ; chaque endpoint (EP1 on/off, 1/50/100 %, extremes de mireds ; EP2, EP3 ; EP4) ; tuile groupee et « eteins les lumieres » puis « allume » ; glisser un curseur HA ; gestes a la telecommande ; lampe debranchee | Trames conformes a D.3 ; memoire des lampes gardee ; curseur sans oscillation, valeur finale juste ; app a jour en moins de 1 s ; retour de l'app en ~5 s ; compteur « ignore au demarrage » a 0 |
| T12 | produit | Point d'acces Wi-Fi sur le canal 1, puis 6, puis 11 (ou Thread ≠ 11) : 20 commandes + 20 gestes chacun | % d'ACK et % de trames manquees par canal : donne la consigne de canal |
| T13 | produit | changer l'etat, attendre 15 s, redemarrer ; puis `decommission` | Meme etat dans `lampe` et dans l'app, B : 0 trame. Apres decommission : adresse par defaut (`lampe adresse`) |
| T14 | diag | luxmetre de telephone a `4C/60/80/A0/C0/E0/FE`, avant seule puis les deux | Ajustement de γ (clarte L*), `lampe gamma` puis `HALO1_LEVEL_GAMMA` |
| T15 | produit | 12 h avec B qui enregistre | `cause` sans chien de garde ; `lampe stats` ; instantanes avant les reconfigurations de silence (cause de la surdite) |

---

## J. Risques et questions ouvertes

| # | Sujet | Traitement |
|---|---|---|
| 1 | Paquet seul ignore (reveil du MCU, ou doublon PID+CRC) | 3 paquets et ≥ 2 accuses ; T5 ; essai de reveil `FF 00` si besoin |
| 2 | Ecart de 100 ms jamais essaye depuis l'ESP32 (500 ms prouve) | T1 et T4 ; repli `lampe ecart 300/500` |
| 3 | Un accuse ne prouve pas que la trame est appliquee | plusieurs paquets accuses a PID distincts ; trames absolues, renvoi sans danger |
| 4 | Surdite de l'ecoute, cause inconnue | reconfiguration prouvee a 500 ms (~8 % sourd) ; instantanes ; rearmement fort en T10 |
| 5 | PID a travers une bascule PRM_RX | reset par defaut ; bascule legere seulement apres T10 |
| 6 | ~0,3-0,55 s sourd par commande : trames de la telecommande perdues | ses 3 copies ; attente apres la telecommande ; ecoute dans les ecarts si T10 reussit |
| 7 | Trame de la telecommande dans notre fenetre d'accuse | AckForeign non compte et transmis au suivi (non teste ; PKT4 en DPL non verifie) |
| 8 | Semantique de A (bascule ou relance, remise a zero du doublon, bits de mode) | endpoint momentane, numero `last + 1` reutilise aux reprises, A refuse lampe eteinte, trames A sans effet sur l'etat ; T7 |
| 9 | Memoire par lampe (Q3), temperature de la lampe arriere (Q4), luminosite lampe eteinte (Q5) | A4 (a) impose la luminosite affichee ; aucune trame de valeur lampe eteinte ; differe jusqu'a l'allumage |
| 10 | Derive de l'etat (mode auto, coupure de courant, trames manquees) ; pas de lecture possible | pas de reaffirmation automatique ; `lampe sync` ; chaque commande utilisateur resynchronise ; Q11 en T6 |
| 11 | Coexistence : Wi-Fi 1-3, Thread canal 11 = 2405 MHz, BLE 2402 pendant la mise en service | T12 ; recommander un point d'acces sur le canal ≥ 6 et Thread ≠ 11 ; module a quelques cm par les fils existants (sans soudure) |
| 12 | Tuile Apple regroupee : « on » allume les deux lampes, A declenche avec la tuile | garde-fou A ; conseiller « afficher en tuiles separees » ; option (b) de A1 |
| 13 | Restauration des attributs depuis NVS par esp-matter, `factory_reset` qui efface « halo1 » | reflet force au demarrage par `updateAttributeVal` ; adresse par defaut ; T13 |
| 14 | Remise en service obligatoire (changement de disposition) | a documenter dans le README |
| 15 | Kelvin reels de 0x00 et 0x64 inconnus ; perception logarithmique | 153/370 nominaux ; γ reglable ; T14 |
| 16 | Relecture de DM1 et RT1 non verifiee | repli sur RFCH et RT1 (C.2) ; `lampe regs` en T0 |
| 17 | L2 bloquant (~300 ms, jusqu'a ~0,5 s si le quartz ou la calibration ne repondent pas) ; ramasse-miettes NVS | rare ; aucune interruption masquee ; ecritures au repos seulement ; L2 sur symptome limitee a une par minute, puis une toutes les 10 min (C.5) |
| 18 | Re-appairage en rejouant la balise (non essaye, fenetre d'appairage necessaire) | hors perimetre ; adresse d'appairage refusee partout |
| 19 | Outils de banc (`txack`, `xo`) qui modifient la puce ou `gXoTrim` | invalidation apres chaque commande hors liste blanche ; B11 corrige |
| 20 | Puce bloquee que la verification ne voit pas (incident du 24/09 : quartz touche, registres conformes, tous les envois en delai) | L2 sur symptome : 3 delais TX de suite, deluge de CRC faux en ecoute, ou ecoute sourde (1000 rearmements hors RX en moins de 10 s, vue en ~2-3 s au rythme de l'incident) ; jamais sur une lampe muette (C.5) |
---

## Resultats du banc (23/09/2026, build diag, carte A = 144401, temoin B = 11301)

Prediction annoncee a Majid avant chaque essai ; journaux `logs/drv-T*.log`.

| Essai | Resultat |
|---|---|
| T0 | `lampe autotest` ok, `lampe decode` conforme ; B : **0 trame, 0 accuse** en 75 s (3 redemarrages de A puis 40 s d'ecoute) ; RFCH/DM1/RT1 relus 05/82/73 ; instantane passif ENAA 00 |
| T1 | `lampe on` : `C5 A5` 3/3 accuses a **100 ms** d'ecart (1,6-1,7 ms) ; B voit PID 1 puis 2 : le PID avance sur l'air. Lampe : deux lampes a A5 (Majid percoit un bref passage par l'etat memorise avant d'appliquer la trame) |
| T2 | `temp 0` (`C3 00`, plus froid), `temp 100` (`C3 64`), **`mode avant` = `C4 A5`** (changement de lampes porte par une trame luminosite : A4 (a) valide), `lum 4C`/`FE`, `off` (`42 64`), `lum 60` **differe** (rien sur l'air), `on` (`C4 60`), `arriere on` (`C5 60`), `avant off` (`85 60`), `arriere off` (`03 64`, eteinte), `on` (`85 60`, memoire de selection), `sync` (`85 60` + `83 64` entrelacees), `rampe 4C FE 16 60` (valeurs intermediaires x1, `FE` x3, 9 preemptions) : **tout conforme, vu par Majid** |
| T3 | `mode deux` (`C5 FE`), `auto` (`E1 01`) puis `auto` (`E1 02`) : baisse puis remontee a chaque fois ; `off` (`43 64`) ; `auto` refuse lampe eteinte, rien sur l'air |
| T6 | lampe debranchee : 3 tours x 5 paquets MAX_RT (11,4 ms), reprises +1 s et +2 s, « injoignable », consigne ramenee a l'etat cru. Rebranchee : la lampe **reste eteinte et garde ses reglages** (temperature chaude conservee) ; `on` repart du premier coup (3/3) |
| T8 | piles remises, `lampe ecoute 1` : switch puis molette suivis trame a trame (28 trames d'etat, 0 CRC faux) ; etat cru final arriere seule, FB, temp 64 = **ce que Majid voit**. Le switch renvoie le DERNIER type de reglage de la telecommande (ici `85 86`, luminosite), avec les nouveaux bits de lampe |

Restent : T4/T5/T7/T9/T10 (reglages et questions ouvertes), T11-T13 (produit Matter), T14 (gamma), T15 (12 h).
