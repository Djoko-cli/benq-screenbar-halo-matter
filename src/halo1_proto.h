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
// Champs de la consigne qu'une trame livree satisfait (efface 'dirty').
uint8_t coveredBy(Payload sent, const State &target);

// ---- Planification (voir D.3) ---------------------------------------------
struct Plan { bool bright = false, temp = false; Payload pb{0, 0}, pt{0, 0}; };
Plan plan(const State &target, const State &believed, uint8_t dirty);

uint8_t nextAuto(uint8_t last);            // 0 ou 255 -> 1, sinon last + 1
uint8_t crc8(const uint8_t *p, size_t n);  // poly 0x07, init 0 : blob NVS
int selfTest(char *msg, size_t n);         // 0 = ok, sinon nb d'echecs (1er dans msg)
}  // namespace halo1
