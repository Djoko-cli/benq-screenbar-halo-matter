[Français](PLAN-PILOTE-HALO1.fr.md) · **English**

<!-- Plan produced by the halo1-driver-design workflow (Sep 23, 2026): 4 readers,
3 designs, 2 judges, 1 synthesis. Reviewed by Claude. -->

> **Djoko's decisions (Sep 23, 2026)**:
> - A1 = **(a)**: EP1 "Halo" (color-temperature light), EP2 "Halo avant"
>   and EP3 "Halo arriere" as `MatterOnOffLight`, EP4 "Halo auto" momentary.
> - A2 = **(a)**: button A exposed, with the group guard.
> - A3 = **(a)** (Claude's technical choice): gamma table 2.0, tunable on the bench.
> - A4 = **(a)** (Claude's technical choice): turning on and changing lamps carried
>   by the displayed brightness frame; fallback to (b) if T2 fails.
> - Follow-up (Sep 23, firmware 0.3.0): EP4 "Halo auto" **disabled for now**
>   (`HALO1_EXPOSE_AUTO 0` by default, code kept, EP1 to EP3 unchanged); node
>   identity set before `Matter.begin()` (vendor `Djoko-CLI`, product `Pont
>   ScreenBar Halo`, NodeLabel `Halo`, serial number `HALO1-<MAC>`, hardware 1
>   `ESP32-C6 SuperMini + BM5602`) and software version `0.3.0-<commit>` via the
>   application descriptor (`src/app_desc.c`). Details: README.

# Final plan: Halo 1 product driver and Matter layer

This plan takes D1 as its starting point for the radio core: we only replay proven sequences and touch the existing code as little as possible. The Matter layer comes from D3: the intent inbox, pure `resolveMatter`, and layout A. Several robustness ideas come from D2:
- non-blocking reset;
- the wait after the remote;
- register snapshots;
- a foreign frame received in place of the acknowledgement;
- traces that never block;
- host tests with a simple `clang++` script.

I double-checked every point where the judges disagreed, against the code and the logs.

- **`enterRxMode` does not re-arm a chip already in reception.** It returns immediately if STA1 says RX (bc5602.cpp:225-226). sniffStd's re-arm every 100 ms therefore comes down to CE=0 plus a MASK rewrite. That is the behavior to reproduce, without a forced strobe.
- **Wake-up frames require an acknowledgement.** `FF/FE/FD 00` have NO_ACK=0, and only `FA xx` has NO_ACK=1 (btn-A*.log). Service frames are therefore recognized by bits 3/4, not by NO_ACK.
- **An A frame can have bit 7 at zero.** We saw `60 01` (PROTOCOL.md:1328). An A frame must never change the power/lamps state.
- **The actual gap in the tx-sem tests was 500 ms, not 300.** The CLI forces a gap of at least 500 ms on channel 5 (cli.cpp:660-662). A 100 ms gap had never been tried from the ESP32 (since then: T1, 100 ms, 3/3 acknowledgements).
- **The CRC vectors are correct.** I recalculated all of them. The lamp's real acknowledgement has NO_ACK=1: PID1, CRC 5C90, raw `01 AE 48 00 00 00 00 00`.
- **Updating an attribute through `updateAttributeVal` keeps the library's cache up to date.** The call goes through `attribute::update`, then PRE_UPDATE, then `attributeChangeCB`, which updates the cache when the callback returns true (MatterColorTemperatureLight.cpp).

The name `halo1` is only a namespace. The global object is called `lamp`, which avoids D2's compile error. The `HALO_*` macros stay in place until step C6: every commit builds.

---

## A. Decisions for the user to make

### A1. Matter endpoint layout

It requires re-commissioning the node: remove it from the app, then run `decommission`.

- **(a) Recommended.**
  - EP1 "Halo" = `MatterColorTemperatureLight`: power, single brightness, temperature.
  - EP2 "Halo avant" and EP3 "Halo arriere" = `MatterOnOffLight`. Each one means "this lamp is on" (power AND the lamp bit).
  - EP4 "Halo auto" = momentary `MatterOnOffPlugin`, which sends an A press.
- **(b)** Like (a), but EP2 and EP3 as `MatterOnOffPlugin`. A "turn off all the lights" command given to the room no longer affects them, but both lamps then show up as outlets.
- **(c)** EP1 alone (plus EP4). The lamp choice is then made only from the remote or the CLI.
- **(d)** Keep the current layout (master outlet, front light, variable back light). Rejected: two brightness sliders that cannot be independent, since each frame carries only one value.

Why (a):
- the layout tells the truth: a single brightness;
- per-lamp voice commands work;
- "Turn on the Halo" restores the last selection, like the remote's power button.

The choice between (a) and (b) is set by `HALO1_SELECTORS_AS_LIGHTS` (1 or 0).

### A2. Exposing button A (auto mode) in Matter

- **(a) Recommended.** Momentary EP4, with a guard: the press is ignored if it arrives in the same coalescing window as a power or lamp intent. That is the signature of a group command or a grouped Apple tile.
- **(b)** Do not expose it (`HALO1_EXPOSE_AUTO 0`) until we know whether A toggles auto mode or restarts it (Q6).

(a) costs little, and the guard covers the only dangerous case.

### A3. Matter brightness curve to raw value

- **(a) Recommended.** Gamma table with γ=2.0 (`HALO1_LEVEL_GAMMA`). It is tuned on the bench at step T14.
- **(b)** Linear.

PROTOCOL.md already notes a logarithmic perception and asks for a "non-linear conversion". With γ=2, 0x80 corresponds to level 138, roughly the midpoint, which matches the observation.

### A4. What accompanies turning on or changing lamps

- **(a) Recommended.** The brightness frame, with the value Matter displays: for example `C5 A5`. A single frame, and the lamp settles on the slider value. The remote already does the same (`C4 ED` / `44 ED`).
- **(b)** The temperature frame, with the raw temperature, like our `C3 35` test. The lamp keeps its own brightness, and the Matter slider can then be wrong.

(a) keeps the app in agreement with the lamp, including if the lamp remembers a per-lamp brightness (Q3). The only point to verify: a lamp change carried by a brightness frame has never been sent. T2 tries it; on failure, we fall back to (b), which changes only one line of `plan()` and one of `dueFields()`.

---

## B. Architecture

### B.1 Files

| Action | File | Contents |
|---|---|---|
| create | `src/halo1_proto.h/.cpp` | pure code: bits, constructors, classification, decoding and CRC, `applyState`, `plan`, self-test |
| create | `src/halo1_map.h/.cpp` | pure code: gamma table, mired conversions, `resolveMatter`, `SelectionMemory` |
| create | `src/halo1_radio.h/.cpp` | proven BC5602 sequences (moved from halo.cpp) and non-blocking `Halo1Radio` |
| create | `src/halo1_lamp.h/.cpp` | `Halo1Lamp` driver: state, slots, TX scheduler, remote tracking, NVS |
| create (Sep 24) | `src/halo1_watch.h/.cpp` | pure code: `ChipWatch`, restarting the module on a chip symptom, and its limits (C.5) |
| create | `src/cli_lampe.cpp` | `lampe ...` command family |
| create | `tools/host_tests/test_halo1.cpp`, `tools/test_halo1.sh` | host tests without a board (clang++) |
| modify | `src/halo.cpp` | `tick()` no longer does anything (C1); `configStdAutoAck` moved; `applyXoTrim` is no longer `static` (l.12 and 2442) (C3); cleanup (C6) |
| modify | `src/main.cpp` | HELLO removed (C1); `lamp.begin` / `lamp.tick` and `delay(1)` (C4) |
| modify | `src/cli.cpp`, `src/cli.h` | Halo 2 commands removed (C1); `lampe`, radio invalidation, B11 (C3/C4) |
| modify | `src/config.h` | `HALO1_*` block added (C4); `HALO_*` removed (C6) |
| rewrite | `src/matter_bridge.cpp` | 4 endpoints, intent inbox, reflection under lock (C5) |
| modify | `src/bc5602.h` | comments (B15, l.179-180), `STATUS_RX_EMPTY` alias |
| docs | `README.md`, `docs/PROTOCOL.md`, `docs/AUDIT-2026-09-23.md` | endpoints, protocol header, 500 ms gap, bug status |

`halo1_*.cpp` and `cli_lampe.cpp` do not depend on Matter. They compile in both environments, without touching `build_src_filter`.

### B.2 `src/halo1_proto.h`

```cpp
#pragma once
// Halo 1 protocol (1st generation), established then verified by transmitting to the
// lamp on Sep 23, 2026 (docs/PROTOCOL.md). Pure code: no Arduino include,
// also compiles on the host (tools/test_halo1.sh).
#include <stddef.h>
#include <stdint.h>

namespace halo1 {

// ---- Radio link: constants, the lamp knows no other ----------
constexpr uint8_t kChannel = 5;  // RFCH, 2405 MHz; rate bc5602::DATARATE_125K
constexpr uint8_t kDefaultAddrReg[4] = {0x4F, 0xF0, 0xFD, 0x63};  // on air 63 FD F0 4F
constexpr uint8_t kPairingH1Reg[4] = {0xB0, 0x00, 0x01, 0x59};    // on air 59 01 00 B0: never
constexpr uint8_t kPairingH2Reg[4] = {0xB0, 0x00, 0x08, 0xE2};    // on air E2 08 00 B0: never
bool addressAllowed(const uint8_t addrReg[4]);  // rejects 0, FFFFFFFF, pairing addresses (2 orders)
void airOrder(const uint8_t addrReg[4], uint8_t air[4]);  // register order -> air order

// ---- 1st payload byte ----------------------------------------------------
constexpr uint8_t F_POWER = 0x80;   // 1 = on
constexpr uint8_t F_FRONT = 0x40;   // front lamp
constexpr uint8_t F_AUTO = 0x20;    // button A: value = press number
constexpr uint8_t F_RSV4 = 0x10;    // favorite only, no visible effect: never sent
constexpr uint8_t F_RSV3 = 0x08;    // same
constexpr uint8_t F_BRIGHT = 0x04;  // value = brightness 0x4C..0xFE
constexpr uint8_t F_TEMP = 0x02;    // value = temperature 0x00 (cold)..0x64 (warm)
constexpr uint8_t F_BACK = 0x01;    // back lamp
constexpr uint8_t F_LAMPS = F_FRONT | F_BACK;
constexpr uint8_t F_SELECT = F_AUTO | F_BRIGHT | F_TEMP;
constexpr uint8_t F_RSV = F_RSV4 | F_RSV3;
constexpr uint8_t kBrightMin = 0x4C;  // the lamp floors below this (C5 20 has no effect)
constexpr uint8_t kBrightMax = 0xFE;
constexpr uint8_t kTempMax = 0x64;
constexpr uint8_t kBrightDefault = 0xA5;
constexpr uint8_t kTempDefault = 0x35;  // the value most often seen from the remote

struct Payload { uint8_t flags; uint8_t value; };
inline bool operator==(Payload a, Payload b) { return a.flags == b.flags && a.value == b.value; }
inline bool operator!=(Payload a, Payload b) { return !(a == b); }

uint8_t clampBright(uint8_t v);  // 0x4C..0xFE
uint8_t clampTemp(uint8_t v);    // 0..0x64
// Exactly one selector, bits 3/4 zero; (lamps & F_LAMPS) == 0 -> F_FRONT (safety net).
Payload makeTemp(bool on, uint8_t lamps, uint8_t temp);
Payload makeBright(bool on, uint8_t lamps, uint8_t bright);
Payload makeAuto(bool on, uint8_t lamps, uint8_t counter);  // counter 0 -> 1

enum class Kind : uint8_t { Temp, Bright, Auto, LampAck, Service, Reserved, Invalid, CrcBad };
// Payload only. (f & 0xF8) == 0xF8 -> Service (FF/FE/FD/FA); f & F_RSV -> Reserved (91, 89);
// no lamp or selector count != 1 -> Invalid; otherwise Temp / Bright / Auto.
Kind kindOf(Payload p);

// ---- Frame read while passively listening (RXPW0 = 8: 64 bits after the address) ----
struct AirFrame { bool crcOk; uint8_t len, pid, noAck; uint8_t pay[4]; uint16_t crc; };
AirFrame decodeAir(const uint8_t raw[8], const uint8_t air[4]);  // len > 4 -> crcOk = false
void encodeAir(const uint8_t air[4], uint8_t pid, bool noAck, const uint8_t *pay, uint8_t len,
               uint8_t raw[8]);  // tests and 'lampe decode'
// !crcOk -> CrcBad; len == 0 -> LampAck; noAck -> Service; len != 2 -> Invalid; otherwise kindOf.
Kind classify(const AirFrame &f);

// ---- State --------------------------------------------------------------------
struct State {
  bool power = false;
  uint8_t lamps = F_LAMPS;  // never 0
  uint8_t bright = kBrightDefault;
  uint8_t temp = kTempDefault;
};
inline bool operator==(const State &a, const State &b) {
  return a.power == b.power && a.lamps == b.lamps && a.bright == b.bright && a.temp == b.temp;
}
inline bool operator!=(const State &a, const State &b) { return !(a == b); }
enum : uint8_t { FLD_FLAGS = 1, FLD_BRIGHT = 2, FLD_TEMP = 4, FLD_ALL = 7 };
// Temp/Bright: writes power, lamps and the selector's value (clamped). Auto and
// everything else: NOTHING (the meaning of an A frame's mode bits is unknown, '60 01' observed).
// Returns the FLD_* whose value changed.
uint8_t applyState(State &s, Payload p);
// Target fields that a delivered frame satisfies (clears 'dirty'). The target
// is clamped just like by the constructors: a frame from plan() always covers its fields.
uint8_t coveredBy(Payload sent, const State &target);

// ---- Planning (see D.3) ---------------------------------------------
struct Plan { bool bright = false, temp = false; Payload pb{0, 0}, pt{0, 0}; };
Plan plan(const State &target, const State &believed, uint8_t dirty);
// Fields to deliver for a target (Halo1Lamp::request: dirty_ |= dueFields(...)).
// A4 (a): on, FLAGS also adds BRIGHT.
uint8_t dueFields(const State &target, uint8_t fields);

uint8_t nextAuto(uint8_t last);            // 0 or 255 -> 1, otherwise last + 1
// A presses heard from the remote (D.6): a new press if it is the first frame
// since reset(), the number changed, or the previous frame is >= 1 s away (before or after).
struct AutoPressFilter {
  static constexpr uint32_t kRepeatMs = 1000;
  bool feed(uint8_t value, uint32_t nowMs);  // true: new press
  void reset();                              // another remote command
};
uint8_t crc8(const uint8_t *p, size_t n);  // poly 0x07, init 0: NVS blob
int selfTest(char *msg, size_t n);         // 0 = ok, otherwise number of failures (1st in msg)
}  // namespace halo1
```

### B.3 `src/halo1_map.h`

```cpp
#pragma once
// Matter <-> Halo 1 payload correspondences and Matter intent rules. Pure code.
#include <stdint.h>
#include "halo1_proto.h"

namespace halo1 {
constexpr uint16_t kMiredCold = 153;  // temp 0x00 (coldest); ~6536 K NOMINAL, not measured
constexpr uint16_t kMiredWarm = 370;  // temp 0x64 (warmest); ~2703 K NOMINAL, not measured

constexpr uint8_t kMatterLevelFloor = 4;  // smallest REPORTED level (Apple Home, E.2)

void mapInit(float gamma);            // 254-entry table; gamma 1.0 = exact linear formula
float mapGamma();
uint8_t rawFromLevel(uint8_t level);  // 0..254 -> 0x4C..0xFE (0..kMatterLevelFloor -> 0x4C)
// Reported level: smallest L >= kMatterLevelFloor such that rawFromLevel(L) >= raw.
uint8_t levelFromRaw(uint8_t raw);
uint8_t tempFromMired(uint16_t m);    // ((clamp(m,153,370) - 153) * 100 + 108) / 217
uint16_t miredFromTemp(uint8_t t);    // 153 + (min(t,100) * 217 + 50) / 100
// Stable display (E.2), never a level below kMatterLevelFloor.
uint8_t displayLevel(uint8_t attr, uint8_t bright);
uint16_t displayMired(uint16_t attr, uint8_t temp);

enum : uint8_t { IN_POWER = 1, IN_FRONT = 2, IN_BACK = 4, IN_LEVEL = 8, IN_MIREDS = 16, IN_AUTO = 32 };
struct MatterIntents {  // last value wins within the coalescing window
  uint8_t has = 0;
  bool power = false, front = false, back = false;
  uint8_t level = 0;
  uint16_t mireds = 0;
};
struct Resolution { State target; uint8_t fields = 0; bool fireAuto = false; };
Resolution resolveMatter(const State &base, const MatterIntents &in, uint8_t memoryLamps);

// Selection memory: last selection that stayed on for >= stableMs.
class SelectionMemory {
 public:
  void reset(uint8_t lamps) { cur_ = stable_ = lamps ? lamps : F_LAMPS; since_ = 0; }
  void update(const State &target, uint32_t nowMs, uint32_t stableMs);
  uint8_t memory(const State &target) const;  // on: stable_; off: target.lamps
 private:
  uint8_t cur_ = F_LAMPS, stable_ = F_LAMPS;
  uint32_t since_ = 0;
};
}  // namespace halo1
```

`SelectionMemory::update`:
- lamp on and `lamps != cur_`: `cur_ = lamps`, `since_ = now`;
- on, same selection for at least `stableMs`: `stable_ = cur_`;
- off: `cur_ = stable_ = target.lamps`.

### B.4 `src/halo1_radio.h`

```cpp
#pragma once
#include "bc5602.h"
#include "halo1_proto.h"

// Body of the old configStdAutoAck (halo.cpp:3776-3809), split around its
// 2 x 20 ms wait. IDENTICAL over-the-air behavior.
void halo1StdReset(BC5602 &r);  // CMD_SOFTWARE_RESET only, no wait
void halo1StdConfigure(BC5602 &r, const uint8_t addrReg[4], uint8_t ch, uint8_t rate, bool receiver);
void halo1PassiveOverrides(BC5602 &r);  // ENAA 0, DPL2 0, DPL1 0, PKT1 0, RXPW0 8 (= arm 3955-3959)
// Blocking version for txAck / sniffStd / prxAck: reset + delay(40) + configure.
void configStdAutoAck(BC5602 &r, const uint8_t addrReg[4], uint8_t ch, uint8_t rate, bool receiver);
void applyXoTrim(BC5602 &r);  // defined in halo.cpp ('static' removed)

class Halo1Radio {
 public:
  enum class Mode : uint8_t { Unknown, Resetting, Tx, Rx, Sleep };
  enum class Verdict : uint8_t { Ack, AckForeign, MaxRt, Timeout, FifoRefused };
  struct TxReport {
    Verdict v; uint8_t irq1, rt2, status; uint16_t us;
    uint8_t fLen; uint8_t fPay[4];  // frame received in place of the acknowledgement (AckForeign)
  };
  struct Tuning {
    uint16_t resetWaitMs = HALO1_RESET_WAIT_MS;  // 40: proven path
    uint16_t rearmMs = HALO1_RX_REARM_MS;        // 100 (= sniffStd)
    uint16_t silenceMs = HALO1_RX_SILENCE_MS;    // 500 (= sniffStd, c09c544)
    bool strongRearm = false;  // LIGHT_SLEEP then RX: NOT PROVEN (bench T10)
    bool lightSwitch = false;  // TX<->RX switch without reset: NOT PROVEN (bench T10)
  };
  struct Stats {
    uint32_t fullConfigs, silenceReconf, txReconf, verifyFail, rearms, rearmsOffRx, rxRaw, lightSwitches;
  };
  struct Snapshot {  // taken before every silence reconfiguration or TX failure
    uint32_t atMs; uint8_t why, sta1, irq1, status, mask, ce, cfg1, rfch, dm1, pkt1, enaa,
        dpl1, dpl2, rxpw0, rt1;
  };

  void begin(BC5602 &chip, const uint8_t addrReg[4]);  // no SPI access
  void setAddress(const uint8_t addrReg[4]);           // + invalidate()
  const uint8_t *air() const { return air_; }
  bool present() const { return chip_ && chip_->present(); }
  void invalidate() { mode_ = Mode::Unknown; }         // a CLI tool touched the chip
  void request(Mode m, uint32_t nowMs);                // Tx, Rx or Sleep
  bool ready(Mode m) const { return mode_ == m; }
  Mode mode() const { return mode_; }
  void service(uint32_t nowMs);                        // finishes a reset after resetWaitMs
  bool restartWanted() const { return restartWanted_; }
  void restartDone() { restartWanted_ = false; verifyFails_ = 0; mode_ = Mode::Unknown; }
  // Requires ready(Tx). Blocking: <= 30 ms of active waiting (1.6-1.7 ms measured).
  // After any verdict other than Ack/AckForeign: starts a reconfiguration to Tx.
  TxReport sendOne(const uint8_t *pay, uint8_t len, uint32_t nowMs);
  // Requires ready(Rx). One iteration of the sniffStd loop; true = raw[8] filled.
  bool pollRx(uint32_t nowMs, uint8_t raw[8]);
  uint8_t snapshots(Snapshot *out, uint8_t max) const;
  bool readConfig(uint8_t out[3]);  // RFCH, DM1, RT1 read back

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

`config.h` must be included before this file, for the `HALO1_*`.

### B.5 `src/halo1_lamp.h`

```cpp
#pragma once
#include <Arduino.h>
#include "config.h"
#include "halo1_map.h"
#include "halo1_radio.h"

enum class Halo1Link : uint8_t { Unknown, Ok, Lost };

// Everything happens in the loop() task: Matter bridge (after coalescing), CLI, tick().
// No critical section: the Matter callbacks never call this object.
class Halo1Lamp {
 public:
  using RestartFn = bool (*)();  // full module restart (halo.begin())
  void begin(BC5602 &chip, bool listen, RestartFn restart);  // NVS -> believed = target; SENDS NOTHING
  void tick();                    // <= ~35 ms worst case (one packet), typically < 1 ms
  void invalidateRadio() { radio.invalidate(); }

  // --- targets ---
  void request(const halo1::State &t, uint8_t fields);  // fields = FLD_* to deliver: ALWAYS sent
  bool pressAuto();                                     // false if the target is off
  void reassert() { request(target_, halo1::FLD_ALL); }
  const char *sendRaw(halo1::Payload p, bool force, uint8_t packets, uint16_t gapMs);  // nullptr = accepted
  void believe(halo1::Payload p);                       // believed + target, without sending

  // --- reading ---
  const halo1::State &target() const { return target_; }
  const halo1::State &believed() const { return believed_; }
  uint8_t memoryLamps() const { return selMem_.memory(target_); }
  uint32_t version() const { return version_; }  // +1 on every target change
  bool busy() const;                             // active slot or waiting to retry
  Halo1Link link() const { return link_; }
  uint8_t lastAuto() const { return lastAuto_; }

  // --- bench ---
  bool waitIdle(uint32_t maxMs);  // runs tick() (+ delay(1)) until idle
  void setListening(bool on);
  bool listening() const { return listening_; }
  void setTrace(bool on) { trace_ = on; }
  bool setAddress(const uint8_t addrReg[4]);  // NVS "halo1/addr"; rejects forbidden ones
  const uint8_t *address() const { return addrReg_; }
  void forget();       // erases "halo1/etat", believed = target = default values
  void persistNow();
  void printStatus(Print &out) const;
  void printStats(Print &out) const;
  void clearStats();

  struct Tuning {
    uint8_t repeats = HALO1_REPEATS;           // 3 packets per frame
    uint8_t minAcks = HALO1_MIN_ACKS;          // 2 acknowledgements to succeed
    uint8_t maxAttempts = HALO1_MAX_ATTEMPTS;  // 5 packets at most per frame
    uint16_t gapMs = HALO1_GAP_MS;             // 100 between two packets
    uint16_t retryMs = HALO1_RETRY_MS;         // 1000 x failure rank
    uint8_t planRetries = HALO1_PLAN_RETRIES;  // 2 retries before giving up
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
  void trace(const char *fmt, ...);  // nothing if !trace_; dropped if Serial.availableForWrite() < 96

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

### B.6 `src/config.h`: block added in C4

The `HALO_*` stay until C6. Each constant is guarded by `#ifndef` so it can be overridden with `-D`.

```c
// ===== Halo 1: product driver (proven, or tunable on the bench via 'lampe') =====
#define HALO1_REPEATS 3                // like the remote; a single frame was once ignored
#define HALO1_MIN_ACKS 2
#define HALO1_MAX_ATTEMPTS 5
#define HALO1_GAP_MS 100               // remote ~100; only 500 is proven from the ESP32
#define HALO1_RETRY_MS 1000
#define HALO1_PLAN_RETRIES 2
#define HALO1_RESET_WAIT_MS 40         // 2 x 20 ms from the proven path
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
#define HALO1_LISTEN_DEFAULT false     // diag: no background radio activity
#else
#define HALO1_LISTEN_DEFAULT true
#endif
```

---

## C. Radio sequencing

All the sequences below are proven sequences, except C.6 and C.7, disabled by default.

### C.1 Startup

1. `halo.begin()`, unchanged. It chains:
   - `loadConfig`;
   - `radio.begin(18, 19, 20, 14, 1 MHz)`: reset + 20 ms, 4-wire SPI, version, `registerConfigure`, PRM_RX, `waitCrystalReady` (at most 50 ms), `calibrate` (at most 200 ms);
   - `prepareToSniff`, `LIGHT_SLEEP`, `calibrate`, `prepareToSniff`.

   It leaves the chip in passive reception with ENAA=0: it acknowledges nothing.
2. `lamp.begin(halo.radio, HALO1_LISTEN_DEFAULT, []{ return halo.begin(); })`:
   - `halo1::mapInit(HALO1_LEVEL_GAMMA)`, in both builds: the diag build has no Matter bridge, and halo1_map's fallback (gamma 2.0) ignores `HALO1_LEVEL_GAMMA`;
   - reads NVS `halo1`;
   - `radio.begin()`, without SPI;
   - `radio.request(listen ? Rx : Sleep)`.
3. **Nothing is sent.** The HELLO branch in main.cpp:184-193 is removed in C1.

### C.2 Full, non-blocking reconfiguration

This is the same sequence as configStdAutoAck, in the same order.

1. **t0:** `CMD_SOFTWARE_RESET` (0x08). Mode `Resetting`, `resetAt_ = t0`. Nothing else happens for 40 ms. The following registers are cleared by the reset: 15 of the 19 Holtek values, CFG1 (hence the AGC), XO, PID, RT1, ENAA=0x3F.
2. **t0 + 40 ms:** `halo1StdConfigure`.
   1. IO1 ← 0x48.
   2. `registerConfigure(nullptr)`: 19 writes and read-backs, ends on bank 0.
   3. `applyXoTrim`: XO1 bits 4-0, only if `gXoTrim >= 0`.
   4. `setBank(0)`, CFG1 ← 0x40 (AGC).
   5. RFCH ← 0x05, DM1 ← 0x82.
   6. PTX address ← `4F F0 FD 63`.
   7. CFO1 &= ~0x40: one-byte preamble.
   8. MASK.PRM_RX ← `receiver`.
   9. PKT1 ← 0x20 (CRC), PKT2 &= 0x7F (no whitening).
   10. DPL1 ← 0x01, DPL2 ← 0x04, ENAA ← 0x01.
   11. RT1 ← 0x73 (ARD 2 ms, ARC 3).
   12. IRQ1 ← 0x70, FLUSH_TX, FLUSH_RX, CE ← 0.
3. **Verification:** read back RFCH, DM1 and RT1; we expect 05, 82 and 73.
   - If it does not match, `verifyFail++` and a new reset.
   - After 3 failures in a row, `restartWanted_` is raised (level L2).
   - If one of these registers does not read back identically from the very first flash (a read-only bit), only RFCH and RT1 are compared from then on. `lampe regs` shows it.
4. **If the target is Tx:** mode `Tx`, CE=0, waiting.
5. **If the target is Rx:**
   - `halo1PassiveOverrides`: ENAA ← 0, DPL2 ← 0, DPL1 ← 0, PKT1 ← 0, RXPW0 ← 8.
   - `enterRxMode()`: MASK |= PRM_RX; if STA1.OMST ≠ 5, FLUSH_RX, IRQ1 ← RX_DR, `CMD_RX_MODE` (0x8E), then wait for OMST=5, at most 3 × 1.5 ms.
   - `lastFull_ = lastArm_ = now`. `lastFrame_` is not reset to zero, as in sniffStd.
   - Mode `Rx`.

`request(m)`:
- already in mode `m`: nothing;
- reset in progress: only the target changes;
- `Sleep`: CE ← 0 and `CMD_LIGHT_SLEEP`, without a reset;
- Tx ↔ Rx: full reconfiguration, or a light switch if `tuning.lightSwitch` (C.6).

### C.3 Sending a frame three times with acknowledgement

The slot is a queue per selector (D.4). The packet is sent by `sendOne`, which reuses txAck:3832-3864 without printing anything.

1. IRQ1 ← 0x70, then FLUSH_TX.
2. `writeCommandData(0x11, {flags, value}, 2)`. The hardware adds the PCF (length 2, PID, NO_ACK=0) and the CRC.
3. Read STATUS. If `STATUS_TX_FIFO_EMPTY`, the verdict is **FifoRefused**: go to step 6.
4. CE ← 1 and `t0 = micros()`. Loop: read IRQ1; exit on TX_DS (0x20) or MAX_RT (0x10); otherwise `delayMicroseconds(20)`. After 30,000 µs, **Timeout**.
5. Verdict.
   - **TX_DS without RX_DR: Ack.** A normal success reads IRQ1=0x2E and STATUS=0x11.
   - **TX_DS with RX_DR (0x40): AckForeign.** A frame with a payload, probably from the remote, arrived within our acknowledgement window. We read `len = PKT4`; if 1 ≤ len ≤ 4, `readFifo(fPay, len)` before flushing. This case has never been observed and never been tried.
   - **MAX_RT: MaxRt.**
6. Read RT2 and STATUS, then **CE ← 0 immediately** (otherwise the chip keeps retransmitting on its own), IRQ1 ← 0x70, FLUSH_TX, FLUSH_RX.
7. If the verdict is neither Ack nor AckForeign:
   - a register snapshot;
   - `beginReset(Tx)`, i.e. the proven path from txAck:3875. Without it, the following sends fail in 64 µs (STATUS 21). The PID restarts at 0.
8. After a success, **nothing**: the PID advances, and the packets of the same burst carry 0, 1, 2...

Throughout the burst, the chip stays PTX with CE=0. **No listening between packets** until T10 has shown that the PID survives a PRM_RX switch. At the end of the burst, `request(listening_ ? Rx : Sleep)`.

### C.4 Passive listening

`pollRx` is called on every `tick()`, i.e. roughly 1 kHz with `loop()`'s `delay(1)`. It is one iteration of sniffStd:3970-4040, in the same order.

1. Read IRQ1. On RX_DR:
   - `readFifo(raw, 8, false)`, IRQ1 ← RX_DR, FLUSH_RX, `lastFrame_ = now`;
   - CE ← 0 and `enterRxMode()`, which actually sends the strobe here because the chip has dropped back into Light Sleep after the event;
   - `lastArm_ = now` (we continue; the frame is returned at the end of the call).
2. Then, if OMST ≠ RX: `enterRxMode(300)`, `lastArm_ = now`, counted in `rearmsOffRx` (the "deaf listening" symptom, C.5).
3. Otherwise, if `now - lastArm_ > 100`: CE ← 0 and `enterRxMode()`. This is **the proven re-arm**, without a strobe (finding 5). With `strongRearm`, see C.7.
4. If `now - lastFrame_ > 500` and `now - lastFull_ > 500`: a snapshot (`why=silence`), then `beginReset(Rx)`. This is the proven remedy (c09c544): about 43 ms of deafness every ~540 ms when quiet, i.e. ~8%.

Setting (`lampe rx <rearm> <silence>`, config.h): a re-arm of at most 200 ms and a silence of more than 2 re-arms, otherwise rejected (`static_assert` for config.h). Beyond that, a quiet room no longer gives the 20 periodic re-arms in 10 s that recovery from deafness needs (C.5): 22 at worst at the limit (199/399, simulated with one pass every 1 to 10 ms), ~74 with 100/500, but 18 with 250/500 and 9 with a 1000 ms re-arm.

Listening can never acknowledge: ENAA=0 is written after every configuration, before entering reception.

### C.5 Recovery levels

| Level | Trigger | Action | Deafness |
|---|---|---|---|
| L0 re-arm | after a frame, every 100 ms, OMST ≠ RX | C.4 steps 1 to 3 | < 0.5 ms |
| L1 full reconfiguration | TX verdict other than Ack; 500 ms of silence; failed verification; `invalidate()` | C.2 | 40 ms, non-blocking, + ~3 ms |
| L2 module restart | 3 failed verifications in a row, or chip version 0/FFFFFF; **chip symptom** (Sep 24): 3 TX timeouts in a row, a flood of bad CRCs while listening (at least 100 raw frames in 10 s, at least 90% with a bad CRC), or deaf listening (at least 1000 re-arms on OMST ≠ RX in under 10 s, sliding window) | `restart_()` = `halo.begin()`, then `radio.restartDone()` then L1. On a symptom: at most one restart per minute, one attempt every 10 min once DOWN | ~300 ms **blocking**, up to ~0.5 s if the crystal or the calibration does not respond (rare) |
| L3 module lost | `halo.begin()` fails, or configuration rejected even after a restart | driver inactive, `lampe` displays « BM5602 perdu » (BM5602 lost), a new L2 attempt every 60 s; each attempt imposes on symptom-triggered restarts the same wait as a restart | — |

Why L2 does not endanger the watchdog: `calibrate()`'s active wait does not mask interrupts, so the interrupt watchdog (300 ms) does not apply. loopTask is not registered with the task watchdog (5 s).

#### Restarting on a chip symptom (`src/halo1_watch.h`, since Sep 24)

**Sep 24 incident** (product board, Matter over Thread): the metal tip of a caliper touched the BM5602's crystal, jamming it for ~70 min. The snapshots read STA1 00, IRQ1 00, STATUS 00, while RFCH, DM1 and RT1 read back 05, 82 and 73: verification therefore saw nothing (2 failures, 0 restarts), and the L1 reconfiguration (software reset + configuration) did not cure it. A manual `rfinit` (`halo.begin()`: waiting for the crystal, calibration) cured everything at once. Visible effect: every command ran until it gave up, and Home reverted to the raw state while the lamp changed only partway. What the logs say (`logs/live.log`, `logs/bug-radio.log`, outside the repo):

- **Counters since startup** (at least ~13 min before the incident, `lampe stats`): 1566 `DELAI` (timeout) out of 1812 packets (11 TX failures at most before the incident); 96,403 raw frames while listening, of which 96,227 with a bad CRC (99.8%; ordinarily a handful per hour). These frames are not timestamped: their rate during the incident is not known, it is **not** 96,403 / 70 min.
- **Final phase observed** (trace of the last ~2 minutes, then two `lampe` calls 26 s apart): 186 `DELAI` in a row, 0 MAX_RT; while listening, **no** frame at all (silence reconfiguration every ~540 ms, the fastest the 500 + 40 ms cycle allows; a frame would have pushed it back); 350 to 450 re-arms per second (11,769 in 26 s between the two `lampe` calls), while the periodic ones produce 10 at most: almost all on OMST ≠ RX (ordinarily ~7 per second in total, almost all periodic). The chip was deaf, not noisy.

`ChipWatch` (pure code, tested on the host) decides; `Halo1Lamp::tick()` alone restarts:

- **TX timeouts**: 3 `Timeout` verdicts in a row. An `Ack` (or `AckForeign`, which also carries TX_DS) or a `MaxRt` resets the streak to zero; `FifoRefused` is neutral. **MAX_RT never triggers a restart**: the chip transmits and waits for an acknowledgement that never comes (lamp unplugged). Incident: 186 timeouts in a row at the end, the streak of 3 was reached on the very first command (~0.4 s); in the middle of a burst, its remaining packets go out from the restarted chip. The verdict re-reads IRQ1 before concluding a timeout: a loopTask preemption beyond 30 ms does not turn a MAX_RT (lamp unplugged) into a timeout.
- **Listening flood**: consecutive 10 s windows; a window that reaches **100 raw frames with at least 90% bad CRCs** triggers immediately, without waiting for it to close, and the alert holds until a quiet window closes (or 20 s with no frame at all). It covers a phase where the sick chip receives noise (the incident's ~96,000 bad CRCs came in at some point or other); a synthetic dense flood (~23 frames per second, 99.8% bad) triggers it in ~4-5 s (host test). Normal use: the dial gives ~9 frames per second, and the lamp's acknowledgements at most as many, with a good CRC; bad CRCs are counted in single digits per hour. Triggering it needs 90 bad CRCs in 10 s and a proportion only ever seen with the sick chip: a remote jammed 50% of the time does not reach it (host test).
- **No "silent lamp" trigger**: a disconnected lamp must never cause a restart loop.
- **Deaf listening** (since Sep 24, the incident's final phase): at least **1000 re-arms with OMST ≠ RX** within a 10 s sliding window. `Halo1Radio::pollRx` counts these re-arms separately (`rearmsOffRx`, shown as « hors RX » in `lampe stats`, i.e. off-RX); on every listening pass, `tick()` passes this counter's delta to `ChipWatch`, along with that of the periodic re-arms (the ones done when the chip says RX, which do not count toward the symptom). The window is made of 10 one-second slots: the sum covers the current slot and the 9 before it, so always under 10 s, whatever phase the deafness starts in (consecutive windows would see it up to ~13 s later). At the incident's rate (350 to 450 per second), it is seen in **2.2 to 2.9 s** (host test), without waiting for the next command: if the restart cures it, as `rfinit` did, the remote becomes audible again right away. Ordinarily: ~7 re-arms per second in total, almost all periodic; the off-RX ones, well under one per second (deduced from the totals: in a quiet room, ~4 re-arms per silence reconfiguration, 1784 for 446 in `logs/live.log`, i.e. the periodic ones around 100, 200, 300 and 400 ms of each 500 ms listening cycle; the "off-RX" counter will measure it directly). The threshold, 100 per second on average, is more than 10 times above all normal re-arms combined and 3.5 times below the incident (host tests: 99 per second for 10 min, an hour of normal listening, a disconnected lamp commanded every 2 s: never). A disconnected lamp produces MAX_RTs, listening stays normal. In diag, listening being off by default starves this symptom.
- **Limits**: a symptom-triggered restart at most 60 s after the previous one, whatever its cause (a restart on failed verification does not wait, since the radio stays inert without it and L3 already stops its own loop; it counts like the others). An L3 attempt imposes the same wait (without counting as a restart): no symptom-triggered restart right behind it. After 3 restarts in a row with no sign of recovery, if the symptom comes back: **module DOWN**, one attempt every 10 min. Sign of recovery: an acknowledgement, or a listening window that closes without a flood and **mostly** with a good CRC. A single good frame is not enough: a CRC-16 lets some noise through, and a sick chip can stay under the flood threshold (60 bad and one good in a window: not a recovery, host test), which would otherwise reset the count on every attempt and keep restarting every minute forever. Silence proves nothing, **except after a restart for deafness**: a 10 s listening window that closes with no bad CRC, or where the chip said RX at least 20 times (periodic re-arms; ~75 ordinarily in a quiet room) and dropped out of it 10 times at most, counts as a recovery, provided transmission does not say otherwise: no timeout in the window, and no timeout streak in progress (closed by an acknowledgement or a MAX_RT). Otherwise, a restart that cures listening but not transmission (the incident showed both) would raise DOWN and turn off the LED while every send fails, and would bring restarts back down to 60 s (host test: restart 4 of a DOWN module, listening back, sends timing out: DOWN and 10 min hold). It assumes the C.4 listening setting (re-arm ≤ 200 ms, silence > 2 re-arms), which `lampe rx` enforces. Sep 24 choice: without it, a restart that cured the deafness would leave, in a quiet room (lamp unplugged or not commanded, remote set down), the module DOWN and the LED red until the next acknowledged command or the next use of the remote, with subsequent restarts every 10 min. After another cause (timeouts, noise, verification), it does not count: a chip that stays in RX can still transmit poorly, and timeouts would lose their 10 min limit. Without listening (diag), it never happens. The DOWN state lasts until a sign of recovery.
- **Evidence cleared** on every restart (the streak, the listening window and the deafness window all restart from zero: the symptom has to reappear after it), on every bench tool (`invalidateRadio()`: the tool may have changed anything, including `rfinit`), and on every L3 attempt. The limits and the counters, however, remain.
- **Bench tools and the diag build**: only `tick()` restarts; a bench tool (`txack`, `ecoute`, `xo`...) runs in the CLI without `tick()`, then invalidates the radio. In diag, listening is off by default: only the driver's sends (`lampe ...`, including `lampe brut`) feed the timeout streak; no flood and no deafness without `lampe ecoute 1`.
- **Counts and traces**: every L2 restart is announced by a line printed even without tracing (never blocking: dropped and counted in « traces perdues » (dropped traces) if the serial buffer is full), for example `[lampe] BM5602 : 3 paquets de suite sans TX_DS ni MAX_RT en 30 ms : relance automatique du module (1 depuis la derniere guerison)` or `[lampe] BM5602 : ecoute sourde (1000 rearmements hors RX en 2856 ms au plus) : relance automatique du module (1 depuis la derniere guerison)` (duration counted from the start of the oldest non-empty slot: at most one second too many), then, after `halo.begin()`, `[lampe] BM5602 relance : quartz pret, calibration faite` (`BC5602::begin()` succeeds as soon as the version can be read, crystal ready or not: this line says what was actually redone); likewise for entering and leaving DOWN. `lampe` shows the timeout streak, the current listening window, the off-RX re-arms of the sliding window, the DOWN state and the date of the last restart (readable even if its line was dropped); `lampe stats` shows restarts by cause (verif., timeouts, noise, deaf), the last 4 with their dates, restarts in a row without recovery, and the wait before the next one is allowed. `lampe stats raz` resets the counters to zero, but not the wait or the DOWN state.
- **Status LED**: steady red as long as the module is DOWN or lost (L3); the three red blinks of a give-up remain visible on top of it (README).

### C.6 "Light switch" option (`lampe leger 1`, disabled by default, not proven)

- **To Tx:** CE ← 0, `LIGHT_SLEEP`, CFG1 ← 0x40, IRQ1 ← 0x70, FLUSH_RX, FLUSH_TX, MASK &= ~PRM_RX, PKT1 ← 0x20, DPL1 ← 0x01, DPL2 ← 0x04, ENAA ← 0x01. About 0.3 ms.
- **To Rx:** CE ← 0, `LIGHT_SLEEP`, CFG1 ← 0x40, ENAA ← 0, DPL2 ← 0, DPL1 ← 0, PKT1 ← 0, RXPW0 ← 8, IRQ1 ← 0x70, FLUSH_TX, `enterRxMode()`. About 0.5 ms.

We adopt it (as the default, and listening within a burst's gaps) only after T10: the PID must carry on from one burst to the next, and the acknowledgement must stay at ~1.6 ms.

### C.7 "Strong re-arm" option (`lampe rx fort 1`, disabled by default)

For the 100 ms re-arm: CE ← 0, `LIGHT_SLEEP`, wait at most 200 µs for OMST ≠ RX, then `enterRxMode()`. This is the probable cause of the deafness (finding 5). Measured in T10, with `lampe rx 100 5000`.

### C.8 Durations

Values marked "measured" come from the tx-sem and ecoute-banc logs; the others are estimates.

| Step | Duration | Source |
|---|---|---|
| reset until configuration is possible | 40 ms, non-blocking | proven path |
| `halo1StdConfigure` + verification | ~2-3 ms | ~75 SPI transactions at 1 MHz (estimated) |
| switch to passive + enter RX | ~0.2 ms + ~130 µs (at most 4.5 ms) | |
| acknowledged packet | 1.6-1.7 ms | measured |
| MAX_RT | 11.5 ms, then a 43 ms reconfiguration | measured |
| wait timeout | 30 ms | |
| `pollRx` with no frame | ~60 µs | |
| one frame ×3 | 43 + 3 packets 100 ms apart + 43 (return to listening): deafness ~0.3 s | |
| two interleaved frames | 6 packets: deafness ~0.55 s | |
| Matter write to the 1st packet | 120 ms of calm (400 ms at most) + 43 ms, i.e. ~165 ms | |
| lamp unplugged to the app's return | 3 rounds × 5 attempts × ~100 ms + 1 s + 2 s, i.e. ~4.5-5 s | |

### C.9 `tick()`'s blocking budget

- Typically under 1 ms.
- Worst case:
  - `sendOne` MAX_RT: 11.5 ms;
  - wait timeout: 30 ms;
  - `enterRxMode`: 4.5 ms;
  - Thread guard before each packet (Thread build): OpenThread lock <= 20 ms (two mutexes, 10 ms each), then the end of an 802.15.4 frame already under way <= 6 ms; lock released at most 13 ms after CE=1 (MAX_RT: 11.5 ms), so held <= ~19 ms;
  - NVS write: a few dozen ms;
  - L2: ~300 ms, up to ~0.5 s if the crystal or the calibration does not respond (`waitCrystalReady` 50 ms, two `calibrate()` calls of 200 ms at most), rare; on a symptom, at most once a minute, then every 10 min once DOWN (C.5).
- No interrupt masking, no `Serial.flush()` in the driver.
- `loop()` ends with `delay(1)`: yields to IDLE and to lower-priority tasks.

---

## D. State model and state machine

### D.1 Data (loop task only)

- **`target_` (target)**: power, lamps (never 0), brightness, temperature. **This is what Matter displays.**
- **`believed_` (raw state)**: updated when a slot succeeds, and by frames heard from the remote.
- **`dirty_` (FLD_*)**: fields requested by the user and not yet delivered. A user command is **always sent**, even if the lamp is already believed to be in that state: that is the resync.
- **`confirmed_` (FLD_*)**: fields confirmed since startup. Used for display only.
- **Slots** `SLOT_BRIGHT`, `SLOT_TEMP`, `SLOT_AUTO`, `SLOT_RAW`: `{active, pay, attempts, acks, repeats}`.
- **`lastAuto_`**: last A number seen (ours or the remote's).
- **`selMem_`**: lamp selection memory.
- **`link_`**: Unknown, Ok or Lost.
- **`version_`**: +1 on every `target_` change. The Matter bridge uses it to know when to reflect.

### D.2 Inputs

**`request(t, fields)`**:
- copies into `target_` the fields from `fields` (FLAGS = power and lamps), clamped (lamps never 0, brightness 0x4C..0xFE, temperature 0..0x64);
- `dirty_ |= dueFields(target_, fields)`: on, FLAGS also makes brightness due (A4 (a), see D.3);
- `pendingSince_` if it was empty;
- `failures_ = 0`; if `Backoff`, back to `Idle` (a new intent restarts right away);
- `version_++` if the target changed;
- `replan()`.

**`pressAuto()`**:
- target off: `autoIgnoredOff++` and returns false. The effect of A with the lamp off is not known.
- AUTO slot already active: presses merge; returns true.
- otherwise: `c = nextAuto(lastAuto_)`, `lastAuto_ = c` (number reserved), `SLOT_AUTO = makeAuto(true, target_.lamps, c)`.

**`sendRaw(p, force, n, gap)`**:
- always rejects first byte 0x0A and `FA`;
- without `force`, also rejects Service, Reserved and Invalid;
- otherwise `SLOT_RAW = {p, repeats = n}` and `rawGapMs_ = gap`. This slot takes priority over all the others and sends exactly `n` packets.

**`believe(p)`**: applies `p` as if it were a frame from the remote, without sending anything.

### D.3 Planning (`halo1::plan`, pure)

```
plan(t, b, dirty):
  if !t.power:
     if dirty & FLAGS: TEMP = makeTemp(false, t.lamps, b.temp)   // RAW temperature: a deferred
                                                                   // setting does not leak into power-off
     return                                                        // BRIGHT/TEMP remain to deliver
  if dirty & (BRIGHT | FLAGS): BRIGHT = makeBright(true, t.lamps, t.bright)   // decision A4 (a)
  if dirty & TEMP:             TEMP   = makeTemp(true, t.lamps, t.temp)
```

With A4 (b), the BRIGHT line becomes: FLAGS alone gives `TEMP = makeTemp(true, t.lamps, b.temp)`, and `dueFields` returns `fields` unchanged.

`dueFields(t, fields)`: if `t.power` and `fields & FLAGS`, adds `FLD_BRIGHT`. Without this, when the temperature slot finishes before the brightness one (a lost packet), `C3 xx` covers FLAGS, brightness is no longer due, and the lamp turns on at its own brightness: the drawback of A4 (b).

`coveredBy(p, t)`, with `t` clamped as by the constructors (lamps 0 → front):
- `FLD_FLAGS` if `p`'s power and lamps equal those of `t`;
- `FLD_BRIGHT` if `p` is an on brightness frame with value `t.bright`;
- `FLD_TEMP` if `p` is an on temperature frame with value `t.temp`.

A power-off frame clears only FLAGS: a deferred brightness or temperature remains to be delivered. A frame drawn from `plan()` always covers its own fields: otherwise the slot would be re-armed after every burst, forever (host delivery test).

Cases tested on the host:

| Believed | Target, fields to deliver | Frames |
|---|---|---|
| on, two, A5, 35 | off (FLAGS) | `43 35` |
| off, two, temp 64 | on (FLAGS) | `C5 A5` |
| on, two | front only (FLAGS) | `C4 A5` |
| on, two | brightness C0 (BRIGHT) | `C5 C0` |
| on, two | temp 00 + brightness 4C | `C5 4C` and `C3 00`, interleaved |
| off | brightness 60 (BRIGHT), stays off | none (deferred); then turning on gives `C5 60` |
| off, raw temp 64 | temp 10 deferred, then off (FLAGS) | `43 64`, not `43 10` |
| on, front only | A (counter 3) | `E0 03`, after the state slots |

### D.4 Slots, scheduling and preemption

**`replan()`**:
- `p = plan(target_, believed_, dirty_)`;
- `setSlot(BRIGHT, p.bright, p.pb)` and `setSlot(TEMP, p.temp, p.pt)`.

**`setSlot(s, want, pay)`**:
- `!want` and an active slot: if `acks > 0`, `applyBelieved(old one)`; deactivate; `cancelled++`.
- active slot with the same payload: the counters are kept.
- active slot with a different payload (a moving slider): if `acks > 0`, `applyBelieved(old one)`; `preempted++`; new payload, counters at 0.

The final value therefore always gets its full burst. Intermediate values may go out once or twice, like the dial's isolated frames.

**`pickSlot()`**:
- RAW first, alone;
- otherwise alternates BRIGHT / TEMP between the active slots;
- AUTO only when neither BRIGHT nor TEMP is active. On AUTO's first packet, its flags are rebuilt with the target's lamps, without changing the number (Q8).

**`tick()`**:

```
if (!radio.present()) return;
radio.service(now);
if (radio.restartWanted()) { if (restart_ && restart_()) stats.restarts++; radio.restartDone(); }
if (phase_ == Backoff && now >= retryAt_) phase_ = Idle;
if (phase_ == Idle && anyActive()):
   if (now - remoteAt_ < 250 && now - pendingSince_ < 2000) -> wait (holdoffs++)
   else { radio.request(Tx, now); phase_ = Burst; nextTxAt_ = now; }
if (phase_ == Burst):
   if (!anyActive()) { phase_ = Idle; lastTxEndAt_ = now; pendingSince_ = 0; }
   else if (radio.ready(Tx) && now >= nextTxAt_) sendPacket(now);  // nextTxAt_ = start + gap
if (phase_ != Burst):
   if (listening_) { radio.request(Rx, now); if (radio.ready(Rx) && radio.pollRx(now, raw)) onAir(decodeAir(raw, radio.air()), now); }
   else radio.request(Sleep, now);
selMem_.update(target_, now, HALO1_SELECTION_STABLE_MS);
maybePersist(now);
```

**`onVerdict`**:
- `attempts++`.
- Ack: `acks++`, `link_ = Ok`, `lastAckAt_ = now`.
- AckForeign: not counted; if `fLen == 2`, `onRemotePayload`.
- The slot is done when:
  - `attempts >= repeats` and `acks >= min(minAcks, repeats)`, or
  - `attempts >= max(maxAttempts, repeats)`.
  - For RAW: exactly `repeats` packets.

**`complete`**:
- **Success** (`acks >= min(minAcks, repeats)`):
  - AUTO: `autoSent++`, `confirmed_ &= ~FLD_BRIGHT` (auto mode makes brightness drift).
  - RAW Temp/Bright: applied like a frame from the remote (believed, target, `dirty_`).
  - Otherwise: `applyBelieved(pay)`, then `dirty_ &= ~coveredBy(pay, target_)`.
  - In every case: `failures_ = 0`, then `replan()`.
- **Failure**: see D.5. If `acks >= 1`, `applyBelieved(pay)` all the same: it is the best estimate, given that an acknowledgement does not prove the frame was applied (tx-sem-1).

### D.5 Failures

**`fail()`**:
- `failures_++`.
- If `failures_ <= planRetries` (2):
  - the slot is re-armed (counters at 0);
  - `phase_ = Backoff`, `retryAt_ = now + retryMs × failures_` (1 s then 2 s);
  - we listen while waiting.
- Otherwise, `giveUp()`:
  - `target_ = believed_`, `dirty_ = 0`, all slots deactivated (A given up on);
  - `link_ = Lost`, `version_++`, `giveUps++`;
  - message always printed: `[lampe] injoignable : consigne abandonnee` (unreachable: target abandoned).
  - Matter then reverts to the raw state in ~5 s.

An acknowledgement from the lamp heard (length 0) during `Backoff` sets `retryAt_ = now`, and `link_` goes from Lost to Unknown.

### D.6 Tracking the remote (`onAir`)

| `classify` | Action |
|---|---|
| CrcBad | `rxCrcBad++` |
| LampAck (len 0, NO_ACK 1) | `rxLampAcks++`; restarts a pending retry (D.5) |
| Service (FF/FE/FD 00 at NO_ACK=0, FA xx at NO_ACK=1) | `rxService++`, `remoteAt_ = now` |
| Reserved (91 xx, 89 xx: favorite) | `rxReserved++`, `remoteAt_ = now`, `remoteAuto_.reset()` |
| Invalid | `rxInvalid++` |
| Auto | see below |
| Temp / Bright | `rxState++`, `remoteAt_ = now`, `remoteAuto_.reset()`, `onRemotePayload` |

**Auto** frame:
- `rxAuto++`, `remoteAt_ = now`, `lastAuto_ = value`;
- press counter: `remoteAutoPresses_++` if `remoteAuto_.feed(value, now)` (`AutoPressFilter`, B.2). The remote sends each press as 3 copies of the same number ~100 ms apart: a copy (same number, less than 1 s before or after the previous one) does not count. A Temp, Bright or favorite frame between two A's resets the number to 01 (PROTOCOL.md): `remoteAuto_.reset()`, and the next A counts. The bridge reflects every change of `remoteAutoCount()` with an EP4 pulse (E.5), without sending anything; our own A frames never go through `onAir`;
- if our AUTO slot has the same number and 0 acknowledgements, we give it a new one: `nextAuto(value)`;
- `confirmed_ &= ~FLD_BRIGHT`;
- **no change to power or lamps**.

`onRemotePayload(p)`:
- `applyState(believed_, p)` and `applyState(target_, p)`, with clamping (brightness < 0x4C brought back to 0x4C, temperature > 100 brought back to 100);
- `dirty_ &= ~(FLD_FLAGS | selector field)`: the remote wins field by field, and a pending Matter setting on another field goes out with the new flags;
- `confirmed_ |= ...`;
- if the target changed, `version_++`; if the raw state changed, a save is scheduled;
- `replan()`.

No duplicate filter for state frames: they are absolute, and `applyState` only signals a change if there is one.

Our own frames are never heard: a single transmitter, in PTX while sending.

### D.7 Button A's number

- `nextAuto(last)`: 0 or 255 gives 1, otherwise `last + 1`. Never 0, never `last`.
- The number is reserved at the moment of the press and reused as-is on every retry: the lamp ignores a number it has already handled, so there is never a double trigger.
- Unknown number (empty NVS): 1. The remote also restarts at 01 after a pause; a duplicate is therefore possible, and the press would be ignored. T7 checks whether numbers > 5 are accepted. If so, we will be able to start higher (C7).

### D.8 Coalescing and the wait after the remote

- **On the Matter side** (bridge, E.3): 120 ms of calm or 400 ms at most, then a single `request()`.
- **On the CLI side**: no coalescing, every command waits on `waitIdle`.
- **In the driver**: no burst is opened less than 250 ms after a frame from the remote, wake-ups included, as long as the pending request is under 2 s old. We do not fight the dial: it sends an isolated frame every ~112 ms (pair-5-verif.log).
- **During a burst**: new targets take effect on the next packet, through preemption (D.4).

### D.9 Startup

- `believed_ = target_ =` the NVS blob if it is valid.
- Otherwise default values: off, two lamps, A5, 35; `lastAuto_ = 0`.
- `confirmed_ = 0`, `selMem_.reset(target_.lamps)`.
- **Nothing is sent.**
- No automatic reassertion: it would cancel a setting made on the remote while the ESP32 was off. The user's first command sends the flags, plus the displayed brightness if it is a power-on (A4).

### D.10 NVS

**Namespace `halo1`**, new. `benqhalo` is ignored; its `chan` and `rate` may hold test values, and the bench tools continue to use them.

- **`addr`**: 4 bytes in register order. Absent: `kDefaultAddrReg`. Written only by `lampe adresse`, which rejects whatever `addressAllowed` rejects.
- **`etat`**: 8 bytes `{ver=1, power<<7 | lamps, bright, temp, lastAuto, 0, 0, crc8(first 7)}`. Rejected (default values) if the version, the CRC, or the ranges are wrong.

Write rules for `etat`:
- the deadline is +10 s from the last raw-state change, and never more than 60 s after the first unsaved change;
- it is written only at rest (no active slot), at least 500 ms after a transmission, and only if the blob differs from the last one written;
- an immediate write on `reboot`, `lampe sauve`, and the BOOT button's actions (restart, unpairing: C9).

Wear is a few dozen entries per day, in a 20 KB partition shared with Matter: negligible.

`decommission` calls `factory_reset()`, which probably erases the whole `nvs` partition. Consequence: default address and default state, harmless. To be verified in T13.

---

## E. Matter layer (`matter_bridge.cpp` rewritten, same `matter_bridge.h`)

### E.1 Endpoints

They are created in this order, which gives the numbers 1 to 4 on a fresh node.

| EP | Class | Attributes |
|---|---|---|
| 1 | `MatterColorTemperatureLight mainLight` | OnOff = `t.power`; CurrentLevel follows `t.bright`; ColorTemperatureMireds follows `t.temp`; PhysicalMin/MaxMireds = 153/370 via `setAttributeVal` (like the current 163-168) |
| 2 | `MatterOnOffLight frontLamp` (or Plugin if `HALO1_SELECTORS_AS_LIGHTS 0`) | OnOff = `t.power && (t.lamps & F_FRONT)` |
| 3 | `MatterOnOffLight backLamp` (same) | OnOff = `t.power && (t.lamps & F_BACK)` |
| 4 | `MatterOnOffPlugin autoButton` (`HALO1_EXPOSE_AUTO`, 0 by default since Sep 23: EP4 absent) | switches on when written, back off after the pulse (1 s by default, `matter impulsion <300..15000>` in NVS `halo1/impulsion`); an A heard from the remote produces the same pulse, without sending anything (E.5) |

We remove the sensor switch (the Halo 1 has no presence sensor), the master outlet, and the back brightness.

### E.2 Correspondences

**Brightness.** Table built at startup (`mapInit(HALO1_LEVEL_GAMMA)`):
- `raw(L) = 0x4C + round(178 × ((L-1)/253)^γ)` for L = 1..254, and `raw(0) = raw(1)`.
- **Floor `kMatterLevelFloor = 4`** (field data from Sep 23): Apple Home displays CurrentLevel as a whole percentage; level 1 becomes 0% there, and a light turned on at 0% is shown at maximum (lamp at 0x4C, set with the dial, shown full). Its formula is not known: `L/254`, or `(L-1)/253` if the range starts at MinLevel = 1, rounded or truncated. 3 would fall to 0% under truncated `(L-1)/253` (0.79%); 4 gives at least 1% in all four cases (1.57% and 1.19%). `raw(0..4) = 0x4C` regardless of γ (nothing changes at γ = 2, where levels 1..14 already give 0x4C), and no level below 4 is ever reported.
- With γ = 1, an exact integer formula above the floor: `raw(L) = 0x4C + ((L-1)*178 + 126)/253`, inverse `L(r) = 1 + ((r-0x4C)*253 + 89)/178`. The round trip is the identity for any value reached from the floor upward; only 0x4D and 0x4E (levels 3 and 4, below the floor) no longer are: they are reported back as level 5 (0x4F).
- General inverse (reported level): `levelFromRaw(r)` = smallest L ≥ 4 such that `raw(L) ≥ r`.
- Points at γ=2 (verified): L64 = 0x57, L127 = 0x78, L138 = 0x80, L171 = 0x9C, L191 = 0xB0, L254 = 0xFE.
- 15 raw values at the top are unreachable from Matter (no factor of 2). The remote can reach them.

**Temperature.** Linear in mireds:
- `temp = ((clamp(m,153,370) - 153)*100 + 108)/217`;
- `m = 153 + (min(t,100)*217 + 50)/100`;
- the temp → mired → temp round trip is exact for all 101 values;
- 0x00 (the coldest) = 153 mireds, 0x64 (the warmest) = 370 mireds.
- The actual Kelvin values are not measured.

**Stable display.** At the moment of reflecting:
- `L_displayed = (L_attribute ≥ 4 && raw(L_attribute) == t.bright) ? L_attribute : levelFromRaw(t.bright)`;
- same rule for mireds (without a floor).

This is idempotent, and a value written by a controller never "jumps" to a neighboring one, except below the floor: 1 to 3 (0x4C) are displayed as 4.

### E.3 Callbacks and the intent inbox

The callbacks run in the CHIP task. They touch neither SPI nor `lamp`, and always return true.

```cpp
static TaskHandle_t sLoopTask;  // captured in matterBridgeBegin() (setup = loop task)
static portMUX_TYPE sInboxMux = portMUX_INITIALIZER_UNLOCKED;
static halo1::MatterIntents sInbox;
static uint32_t sInFirst, sInLast, sBootMs, sAutoPulseAt, sSeenVersion, sLastReflect;
static bool sForceReflect = true;
// Our own reflections go through attribute::update() and re-trigger the callbacks, but
// always from the loop task; controller commands arrive in the CHIP task.
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

Only per-attribute callbacks are registered: `onChangeOnOff`, `onChangeBrightness`, `onChangeColorTemperature`. **Never `onChange`**, which passes the cached values of the other attributes.

### E.4 Intent rules (`resolveMatter`, pure, tested)

Notation: `baseF = base.power && (base.lamps & F_FRONT)`, and likewise `baseB`. `f` = front intent if present, otherwise `baseF`; `b` = back intent if present, otherwise `baseB`. `sel` = a front or back intent is present. `mem = memoryLamps`, never 0.

| Rule | Condition | Result |
|---|---|---|
| R1 | EP1 off | `power = false`, `lamps = mem`: turning off always wins |
| R2 | `sel` and (`f` or `b`) | `power = true`, `lamps` = the lamps that are on |
| R2' | no `sel`, and already on | power and lamps unchanged |
| R3a | EP1 on | `power = true`, `lamps = mem` |
| R3b | `sel` and no lamp on | `power = false`, `lamps = mem` (never 0) |

- Any power or lamp intent adds `fields |= FLD_FLAGS`: the command is always sent.
- Level: `bright = rawFromLevel(max(1, L))`, `FLD_BRIGHT`, **including with the lamp off** (deferred until power-on). When it comes with EP1 on, it is dropped if it adds nothing: the same raw value as the target (1..3 for 0x4C), or the level displayed for it.
- Mireds: `temp = tempFromMired(m)`, `FLD_TEMP`, same rule.
- A: `fireAuto` only if the final target is on **and** the window contains no power or lamp intent (group guard, A2).

Cases tested:
- "turn everything off" sent in both orders, in one or two windows: the memory keeps "two" thanks to `SelectionMemory` (2 s);
- scene {EP1 on, front on, back off} in any order: front only;
- level written with the lamp off: deferred;
- A in the same window as EP1 on: ignored.

### E.5 `matterBridgePoll()` (loop task)

1. **Intent inbox.** Under `sInboxMux`, it is flushed if `now - sInLast ≥ 120` or `now - sInFirst ≥ 400`.
   - If `now - sBootMs < 2000`: intents ignored, logged, `sForceReflect = true`. A controller cannot write this early; this is a safety net to never send anything at startup, and T11 checks that the counter stays at 0.
   - Otherwise: `r = resolveMatter(lamp.target(), in, lamp.memoryLamps())`.
     - If `r.fields`, `lamp.request(r.target, r.fields)`.
     - If `r.fireAuto && lamp.pressAuto()`, `sAutoPulseAt = now`.
   - **A from the remote** (`HALO1_EXPOSE_AUTO`): if `lamp.remoteAutoCount()` changed (presses heard in `onAir`, the 3 copies of a press counted once, D.6), the same EP4 pulse, which the reflection turns on. Nothing is sent, no intent: our own EP4 write is filtered out by `ownEcho()`. The pulse starts from the rising edge actually written (or from EP4 already on), not from the press being heard: the intent inbox (up to 400 ms), the stack lock (CASE), or a write failure can delay it, and a failure is retried on the next pass. Rising edge not done 3 s after the press: give up (`matter`'s « non reflete(s) » counter, i.e. not reflected).
   - In every case, `sForceReflect = true`: we realign on the resolved target, for example EP4 goes back off right away if A is rejected.
2. **Reflection**, only if the inbox is empty and if `sForceReflect` or (`lamp.version() != sSeenVersion` and `now - sLastReflect ≥ 250`), or if the auto pulse is due:
   - `st = esp_matter::lock::chip_stack_lock(portMAX_DELAY)`; if FAILED, retry on the next pass;
   - for each attribute (EP1 OnOff, CurrentLevel, Mireds; EP2, EP3 OnOff; EP4 OnOff = pulse in progress): `getAttributeVal`, compute the desired value (E.2), and if it differs, `updateAttributeVal` while keeping the type of the value read;
   - `sSeenVersion = lamp.version()`, `sForceReflect = false`;
   - `chip_stack_unlock()` only if `st == SUCCESS`.

   We use `updateAttributeVal` and not the setters: the call goes through PRE_UPDATE, updates the library's cache, and also corrects a value restored from NVS that the setters would skip (cache equal). The lock guarantees that no controller write slips in during the reflection.
3. **`matterBridgeBegin()`**, in this order:
   1. `sLoopTask = xTaskGetCurrentTaskHandle()`, `sBootMs = millis()`. The gamma table is already built by `lamp.begin` (C.1).
   2. `t = lamp.target()`.
   3. `mainLight.begin(t.power, levelFromRaw(t.bright), miredFromTemp(t.temp))`.
   4. Physical mireds 153/370.
   5. `frontLamp.begin(...)`, `backLamp.begin(...)`, `autoButton.begin(false)`.
   6. Callback registration.
   7. `Matter.begin()`.
   8. `sForceReflect = true`.

### E.6 Invariants

1. Only a validated intent or a CLI command triggers a send: nothing at startup, during commissioning, on a timer, or in reaction to a frame that was heard.
2. Callbacks: never any SPI, never a call to `lamp`; always true.
3. `lamp`, the radio and the driver's NVS are only ever used from the loop task.
4. Matter displays the target. It changes outside a Matter write only on a frame from the remote, a give-up, or a CLI command.
5. We never reflect while the inbox holds intents: a slider in motion never jumps backward.
6. EP1 OnOff = `t.power`, EP2 = `power && front`, EP3 = `power && back`, EP4 is false except during the pulse (1 s by default, `matter impulsion`). CurrentLevel never below 4.

Identify: a rainbow on the WS2812 (src/status_led.*), never the lamp, which would mean transmitting.

---

## F. Fate of the Halo 2 code and diagnostics

### F.1 C1: neutralize

- `BenqHalo::tick()` does `return;` in every build, with a comment.
- main.cpp:184-193: removal of the HELLO branch (`pollNow`, `desired = reported`, `printState`) and of the « lance 'find' » (run 'find') message.
- CLI `poll`, `send`, `find`, `pair`, `sniff`, `tail`: replaced with « commande Halo 2 retiree : voir 'txack', 'ecoute' (puis 'lampe') » (Halo 2 command removed: see 'txack', 'ecoute' (then 'lampe')). For `pair`, suggest `ecoute B0000159 5`.
- `txack` guard (cli.cpp:648-656): also reject `B0000159` and `590100B0`.

### F.2 C6: remove, once the driver is validated on the bench

Done on Sep 24, 2026; deviations in H, C6.

- **halo.h:**
  - l.8-50: header, `HALO_CMD_*`, `HALO_PAIRING_ADDRESS`;
  - l.52-70: `HaloState`, `HaloPhase`;
  - `desired`, `reported`, `setTail`/`tail`/`tail_`, finder and sniffer members, `txBusy_`, `kBurstGapMs`, `rxEvents()`, `mode()`.
- **halo.cpp:**
  - `setTail` 87-91, `prepareToTransfer` 150-168, `resetRadio` 170-175;
  - `sendWithAck` 209-228 (**B3**), `readAck`, `sniffOnce` 240-271;
  - `buildPayload` 277-298, `frameCrc*` 300-331, `validate`/`parseStatus` 333-361;
  - `requestPush*`, `pollNow`, `settled`, `checkTxFifo`, `adoptReported` 367-419;
  - `tick`/`tickNormal`/`tickSniffer`/`tickFinder` 425-535;
  - finder 553-691 and 1154-1194, `startSniffer` 1200-1212, `printState` 1226-1234.
- **B1:** `listenHalo1` 3473-3647, `txHalo1` 3664-3705, `halo1Crc` 4060-4070, `groupVerdict` 4075-4117, and the `benq` and `tx6` commands.
- **B13:** `txRaw` 3722-3758 and `txraw`.
- `probeRxSequences`: remove the call to `frameCrcOk` (986) and print the raw bytes.
- **config.h:** the `HALO_*` 116-142 and the stale comment about `homeSpan.poll()`.
- **NVS:** `prefs.remove("tail")` once, in `loadConfig`.

### F.3 Keep, in both builds

- All the other tools from the reader's list (c).
- `setMode(HaloMode::Normal)` → `prepareToSniff` remains the basis for diagnostics: `sharedRadioConfig` and `prepareToSniff` are kept.
- `txAck`, `sniffStd`, `prxAck` keep their traces and call halo1_radio.cpp's `configStdAutoAck`, whose behavior is identical. In C6, `sniffStd` switches to `halo1::decodeAir`: a single decoder.
- `BenqHalo::begin` is kept: it is the proven startup, and it serves as the L2 restart.
- `printInfo` is slimmed down in C6 (no more endpoint or Halo 2 state).

### F.4 Audit bugs fixed along the way

- **B3, B1, B13:** removed along with their code (C6).
- **B11** (C3): `xo` reads with `strtol(arg, nullptr, 0)` and accepts `off` / `-1`, which call `setXoTrim(-1)`. The driver applies `gXoTrim`.
- **B15** (C3): move the RSSI_NEGDB comment from the `B0_XO1` line to `B0_RSSI2`.
- **bc5602.h:179-180** (C3): `enterRxMode` does not touch CE and makes 3 attempts; add the `STATUS_RX_EMPTY` alias (STATUS's RX_DR reads 0 when data is present).
- **halo.h:219** (C6): stale comment « CRC verifie par le materiel » (CRC verified by the hardware).
- **Docs:**
  - PROTOCOL.md: the tx-sem « 3 300 » tests actually ran at **500 ms** (the CLI's bound); redo the header (l.3-75: confirmed semantics, remove the disproven hypotheses and the Halo 2 hardware presented as current);
  - AUDIT: B2 and B3 closed, B1 and B13 removed;
  - README:12-18: new endpoints.
- **Out of scope** (CC2500 tools, a separate task): B6, B7, B8, B12, B14, B16.

---

## G. Product-path CLI commands (`lampe ...`, in `cli_lampe.cpp`)

### G.1 Radio invalidation

At the end of `handleLine` (cli.cpp, after the `if` chain, which has no other `return`):

```
if (!radioFree(line)) lamp.invalidateRadio();
```

Allowlist: `lampe help ? matter debug chiplog cause wifi decommission reboot`.

Any other command (txack, ecoute, rfinit, regs, xo, cc*, swd...) forces a full reconfiguration on the next use. Without risk:
- the CLI runs in the same task as `tick()`;
- `sendOne` is atomic;
- an interrupted burst resumes after reconfiguration (PID at 0, absolute frames).

### G.2 State commands

They update the target just like Matter, call `waitIdle(6000)`, and print **one** summary line, for example:

```
ok C5 A0 3/3 accuses (1650 1602 1611 us) -> cru : allumee deux lum A0 temp 35
```

| Command | Effect |
|---|---|
| `lampe` or `lampe etat` | address (register and air), channel 5, 125 kbps, listening, tracing; target and fields to deliver; raw state and confirmed fields; slots (payload, attempts/acknowledgements); link and last acknowledgement; radio mode and counters; last A; lamp memory; deviations from NVS `benqhalo/chan`, `rate` |
| `lampe on` / `lampe off` | via `resolveMatter` (EP1 intent): same rules as Matter |
| `lampe avant on\|off`, `lampe arriere on\|off` | via `resolveMatter` (EP2, EP3) |
| `lampe mode avant\|arriere\|deux` | absolute lamps and power-on (FLAGS) |
| `lampe lum <4C..FE hex>` / `lampe niveau <1..254>` | raw / through the gamma table |
| `lampe temp <0..100 decimal>` / `lampe mired <153..370>` | raw / through the conversion |
| `lampe auto` | `pressAuto`; « refuse : lampe eteinte » (rejected: lamp off) if the target is off |
| `lampe sync` | `reassert()`: everything known is resent |
| `lampe rampe <de> <a> <pas> <ms>` | simulates a slider: `lum` every `ms` while running `tick()` (tests preemption) |
| `lampe brut <XXYY> [n 1..10] [ecart 5..2000] [force]` | raw payload, exactly n packets, per-packet verdict (IRQ1, RT2, µs). Always rejected: 1st byte 0x0A, `FA`. Without `force`, also rejects Service, Reserved and Invalid |
| `lampe croire <XXYY>` | believed and target, without sending |

### G.3 Bench and tuning commands

| Command | Effect |
|---|---|
| `lampe rafale <n> [min_accuses] [max]`, `lampe ecart <5..2000>` | in-RAM settings |
| `lampe ecoute 0\|1` | background listening (diag 0, product 1) |
| `lampe attends <ms>` | only runs `tick()` (to observe listening) |
| `lampe rx <rearm ms> <silence ms>`, `lampe rx fort 0\|1`, `lampe leger 0\|1` | C.4 options (re-arm 10..200, silence > 2 re-arms), C.7, C.6 |
| `lampe gamma <x.x>` | rebuilds the table (RAM) |
| `lampe garde 0\|1` | Thread build: OpenThread lock held during each packet, after the end of an in-progress Thread frame (default 1, RAM); absent elsewhere. Counters in `lampe stats` |
| `lampe trace 0\|1` | per-event log, never blocking: `[lampe] TX C5 A0 #2/3 ACK 1650 us RT2 00`, `[lampe] RX tele PID 2 C4 BC -> allumee avant lum BC`, `[lampe] RADIO reconf silence #118` |
| `lampe stats [raz]` | counters |
| `lampe regs` | RFCH/DM1/RT1 read back, chip version, 4 snapshots |
| `lampe decode <16 hex>` | e.g. `08627F030C800000` gives len 2, PID 0, C4 FE, CRC 0619 OK |
| `lampe autotest` | `halo1::selfTest`, without the radio |
| `lampe adresse [8 hex]` | read / write NVS `halo1/addr` |
| `lampe oublie` / `lampe sauve` | erase / write the `etat` blob |

Other CLI changes:
- `info` additionally prints `lamp.printStatus`;
- `reboot` calls `lamp.persistNow()` before restarting;
- help loses the Halo 2 lines (151-170) and gains the `lampe` lines.

---

## H. Implementation steps

Each step ends with:

```
tools/test_halo1.sh            # from C2 onward
pio run -e esp32c6diag
pio run -e esp32c6supermini    # size < 3 MB (current margin ~0.7 MB)
```

To flash: `pio run -e esp32c6diag -t upload --upload-port /dev/cu.usbmodem144401`.

We do not commit the modified `.pyc`: `git checkout -- tools/audit/indep_pll/__pycache__/pll.cpython-311.pyc`, and add `__pycache__/` to `.gitignore`. This `.pyc` was later removed from the whole history when it was rewritten on 2026-10-05.

**C1 "Neutralize the Halo 2 layer of the product path"**
- Content: F.1.
- From here on, the product build no longer sends anything on its own.
- Bench T0a, with no transmission: B runs `ecoute 4FF0FD63 5 90000` while A restarts 3 times; 0 COMMANDE expected.

**C2 "Pure Halo 1 protocol and host tests"**
- Files: `halo1_proto.*`, `halo1_map.*`, `tools/host_tests/test_halo1.cpp`, `tools/test_halo1.sh`:

  ```sh
  clang++ -std=c++17 -Wall -Wextra -Werror -Isrc src/halo1_proto.cpp src/halo1_map.cpp tools/host_tests/test_halo1.cpp -o "${TMPDIR:-/tmp}/test_halo1" && "${TMPDIR:-/tmp}/test_halo1"
  ```

- Golden vectors (address on air 63 FD F0 4F), as PID/NO_ACK, payload, CRC, raw:
  - 0/0 `C3 35` F7A9 `08 61 9A FB D4 80 00 00`
  - 1/0 `C3 35` 99C9 `09 61 9A CC E4 80 00 00`
  - 1/0 `C4 BC` 00FF `09 62 5E 00 7F 80 00 00` (valid on air)
  - 0/0 `C4 FE` 0619 `08 62 7F 03 0C 80 00 00`
  - 2/0 `C4 FE` DAD9 `0A 62 7F 6D 6C 80 00 00`
  - 0/0 `E1 01` E1FA `08 70 80 F0 FD 00 00 00`
  - 2/0 `E1 01` 3D3A `0A 70 80 9E 9D 00 00 00`
  - 0/0 `42 35` DF00 `08 21 1A EF 80 00 00 00`
  - 0/0 `C5 A0` 8E13 `08 62 D0 47 09 80 00 00`
  - real acknowledgement len0 1/1 5C90 `01 AE 48 00 00 00 00 00`
  - acknowledgement len0 3/1 1C14 `03 8E 0A 00 00 00 00 00`
- Other tests:
  - flipping one bit gives a bad CRC;
  - `encodeAir` and `decodeAir` round-trip for PID 0-3 and len 0-4;
  - `classify` table: FF/FE/FD 00 at NO_ACK=0 → Service; FA A8 at NO_ACK=1 → Service; 91 00, 89 58 → Reserved; 00 00, C6 10 → Invalid; E1 01 → Auto; C3 35 → Temp;
  - D.3 and E.4 tables; `coveredBy`; clamping; `nextAuto`; conversions (γ=1 exact, γ=2 monotonic, stable display idempotent); `SelectionMemory`.

**C3 "Extract the Halo 1 radio"**
- `halo1_radio.*`: split out of configStdAutoAck, moved from halo.cpp:3776-3809 (the `static` goes away); `applyXoTrim` is no longer `static` (l.12 and 2442); `Halo1Radio` class.
- B11, B15, bc5602.h comments.
- Bench R0.

**C4 "Halo 1 driver and 'lampe' commands"**
- Files: `halo1_lamp.*`, `cli_lampe.cpp`, `cli.h` (`void cmdLampe(char *arg);`), the `HALO1_*` block of config.h, invalidation in `handleLine`, `info`, `reboot`.
- main.cpp: `lamp.begin(...)` after `halo.begin()`, `lamp.tick()` in place of `halo.tick()`, `delay(1)` at the end of `loop()`.
- `matter_bridge.cpp` is not touched yet: it compiles, but **the product is not flashed**.
- Bench T0 to T10 in diag.

**C5 "Halo 1 Matter bridge"**
- Section E, README.
- Product build, then bench T11 to T13, after removing the old node and `decommission`.

**C6 "Remove the Halo 2 layer and the disproven tools"**
- Section F.2, `sniffStd` switches to `decodeAir`, docs (F.4).
- Both builds and the tests, then a quick R0.
- **Done on Sep 24, 2026.** Thread, supermini and diag builds with no warnings,
  host tests. `ecoute` decodes via `halo1::decodeAir`, identical output
  (5 million frames compared against the old decoder, on the host). Deviations
  from F.2:
  - `resetRadio` is kept (private): four generic tools use it
    (`gio`, `guet`, `direct`, `rxdirect`);
  - the call to `frameCrcOk` was in `watchChannel` (`guet`), not in
    `probeRxSequences`: `guet` prints the raw bytes;
  - also removed, since they relied on the Halo 2 CRC and only served
    the Halo 2 hunt: `capturePairing` (`appaire`), `huntByPreamble`
    (`preambule`), `huntAnchored` and `scanCaptureForAddress` (`ancre`).
    Rendered moot: `debug` (read only by the Halo 2 layer), `normal`
    (output of the sniffer and finder modes), and the messages of the
    commands removed in C1;
  - `setMode(HaloMode::Normal)` becomes `prepareForTool()`, with the same
    effect (`prepareToSniff` if the module responds);
  - R0 is still to be done on the bench: nothing was flashed at this step.

**C7 "Set default values from the bench"**
- Gap, silence, strong re-arm, light switch, γ, starting A number.

**C8 "Machine mode for the companion app" (firmware 0.4.0)**
- Contract: [PROTOCOLE-JSON.md](PROTOCOLE-JSON.md), section 11. Files:
  `json_out.*` (pure, tested on the host), `json_mode.*`, `halo1_events.h`,
  driver hooks (`Halo1Lamp::setHooks`), LED hooks
  (`statusLedSetObserver`) and bridge hooks; `cli.cpp` (`id=` prefix, `json` in
  `kFree`, echo, prompt and flush cut off in machine mode, `trop_long` and
  `cadence` rejections, Ctrl-U); `cli_lampe.cpp` (asynchronous state commands).
- The driver knows nothing about JSON: it passes events as plain data
  (frame heard, packet sent, restart, module state, log line);
  `Halo1Lamp` exposes for reading its phase, its slots, its failures,
  the age of the last acknowledgement, the cause of the last give-up, the slot that closed
  the last delivery, and the `raz` counter.
- **Done on Sep 24, 2026**, without flashing anything: thread, supermini and diag
  builds with no warnings; host tests (messages compared against the section 12
  examples, worst cases under the 896-byte budget); `tools/json_check.py`
  validates the specification's 43 examples and the tests' messages.
- Follow-ups from review (Sep 24): a historical command's `reponse fin` that
  fills the send buffer (`help`, ~7 KB) and the deferred responses of the
  snapshots go through the queue without ever being dropped for lateness;
  an accepted id always receives its `livraison` (`annulee` if the busy period
  ends before the next round); `heap_bloc` re-read at most every
  10 s (a critical-section heap walk); `boucle_max_ms` excludes rounds
  from before the session. Delivery watcher, queue lateness and lease
  are pure and tested on the host (`DeliveryWatch`, `Queue::dropLate`,
  `leaseExpired`).
- Still on the bench: U1 to U10 (PROTOCOLE-JSON.md, section 11), captures
  verified with `python3 tools/json_check.py <capture>`.

**C9 "BOOT button: restart and unpairing" (Sep 24, 2026)**
- Djoko's request: in the printed enclosure, only BOOT (IO9, labeled B) is
  accessible, not RST. Short press (< 2 s): a white flash then a restart, on
  release; released between 2 and 8 s: canceled; held 8 s: fast red/violet
  (« relache pour desappairer », release to unpair), then on release Matter
  removal (`matterDecommissionNow()`) and a restart. Replaces the old 5 s
  press, which unpaired while the button was still held down.
- Constraint: IO9 is a strapping pin. Low at reset, the C6 boots
  into download mode and stays inert until a power cycle. No action
  before release is seen (30 ms debounce) AND 100 ms of uninterrupted
  high readings; `pinSettled()` re-reads the pin again 100 ms right before
  `ESP.restart()` or `matterDecommissionNow()` (aborted if
  it does not hold high within that second).
- Files: `boot_button.*` (pure `bootbtn::Machine` state machine, tested on
  the host: 1999/2000 and 7999/8000 ms thresholds, bounces, held at startup,
  gaps in readings, millis() wraparound, properties on random readings),
  `status_led.*` (`ButtonUnpair` and `ButtonReboot` patterns, right
  below Identify, above `led test`), `main.cpp` (`bootButtonPoll()`
  before `statusLedPoll()`), `config.h` (`DECOMMISSION_HOLD_MS` removed),
  JSON protocol rev 1 (`desappairage` and `redemarrage` patterns, `log` with `src`
  `bouton`).
- Driver state saved before every restart (`lamp.persistNow()`, like
  `reboot`). Diagnostic build: short press = restart, long press
  explained on the console, without doing anything.
- A press's duration = first high reading minus first low reading; arming
  at 8 s requires a reading still low at +7999 ms with no gap in readings
  longer than 100 ms since the start. A gap longer than 100 ms during the press or at either
  of its edges (loop() blocked: bench tools, module restart) makes the
  duration uncertain: the press is ignored, unless a long press was already armed.
- **Done on Sep 24, 2026**, without flashing anything: thread, supermini and diag
  builds with no warnings, host tests. Still on the bench, on the board in its
  enclosure:
  - B1: brief press, then ~1.5 s: white flash, restart, `cause` =
    software restart, lamp state kept;
  - B2: 3 s, then 7.5 s: nothing, `[bouton] ... annule` (canceled) line;
  - B3: held 8 s: red/violet at 8 s; release: unpairing, the accessory
    disappears from Apple Home, blinking blue after the restart;
  - B4: release and press again right after a short press: never a download
    mode (the board always responds over USB);
  - B5: button held during startup (after the bootloader): ignored
    until release.
- Follow-ups from review (Sep 24, 2026):
  - `esp_matter::factory_reset()` only erases the node's NVS namespace, then
    `chip::Server::ScheduleFactoryReset()`: the CHIP task removes the
    fabrics, then `DoFactoryReset` erases the network and calls
    `esp_restart()`, two jobs queued with flash erases coming
    later (not "a few ms"). The button was already free by then: a new
    press held at that moment would have put the C6 into download mode.
  - Guarding every reset: `bootButtonBegin()` registers
    `waitBootHigh()` via `esp_register_shutdown_handler()`, before
    `netBegin()` and `matterBridgeBegin()` (handlers run from the
    last registered to the first: this one follows Wi-Fi shutdown). It
    waits for IO9 high for 50 ms in a row, with no limit, via `vTaskDelay(1)`,
    feeding the watchdog if the task is registered with it (panic at 5 s:
    a reset with no handlers). Also covers `reboot` and `decommission`.
  - During unpairing: button inert, `Unpair` phase held (LED
    red/violet until the reset); a 10 s safety net if nothing restarts
    (`Matter.decommission()` never returns).
  - Button announcements outside the `log` cap (20/s shared with the
    lamp's traces); the phase -> LED mapping is a pure function
    (`statusled::buttonFor`), tested on the host, as is the state machine's
    recovery after a last failed guard.
  - On the bench, in addition: B6: release after 8 s then press and hold
    right away: the LED stays red/violet, the `[bouton] tenu pendant un
    redemarrage` (held during a restart) line appears, the board restarts on
    release (never a download mode); B7: `reboot` typed with the button held:
    the same thing.

---

## I. Tests on the real lamp

### I.1 Setup

- **Board A** (`/dev/cu.usbmodem144401`): the driver.
- **Board B** (`/dev/cu.usbmodem11301`): **independent witness**. It stays on the C1 firmware, whose `ecoute` is the proven tool, and runs `ecoute 4FF0FD63 5 <ms>`. B is deaf ~8% of the time and can therefore miss one copy: we judge on the 3 copies.
- Logs: `scratchpad/serial_run.py <port> logs/drv-Tn-{A,B}.log <duration> "cmd" ...`.
- **Djoko is present** for every test that transmits. The prediction is written to the log **before** the test.
- The lamp is in a recognizable state.
- **Remote batteries removed**, except for T8, T9 and T10's listening leg.

### I.2 List of tests

| # | Build, step | Procedure | Expected |
|---|---|---|---|
| R0 | diag C3 on A | A `txack 4FF0FD63 5 C335 3 500`, B listens. Then swap: B `txack ... C235 3 500`, A `ecoute ... 20000` | 3/3 TX_DS in 1.6-1.7 ms; lamp behaves correctly; B sees 3× `C3 35`, each followed by an acknowledgement, **PID 0, 1, 2** (first over-the-air proof that the PID advances). A, with the split reset, decodes the 3 `C2 35` and their acknowledgements |
| T0 | diag C4 | `lampe autotest`; `lampe decode 08627F030C800000`; A restarts 3 times; `lampe ecoute 1` for 60 s; `lampe regs` | 0 failures; correct decoding; **B: 0 COMMANDE, 0 acknowledgements** (listening never acknowledges); RFCH/DM1/RT1 = 05/82/73 |
| T1 | diag | `lampe oublie`, reboot, `lampe trace 1`, `lampe on` | B: `C5 A5` ×3, PID 0/1/2, ~100 ms apart, each acknowledged (**first 100 ms gap from the ESP32**); A `ok 3/3`. Both lamps at A5. On failure: `lampe ecart 300` then `500` |
| T2 | diag | Sequence (B in brackets): `temp 0` [`C3 00`, the coldest]; `temp 100` [`C3 64`]; `mode avant` [`C4 A5`, front only: **first lamp change carried by a brightness frame**, A4]; `lum 4C` [`C4 4C`]; `lum FE` [`C4 FE`]; wait 3 s (`@3` for serial_run.py: the front-only selection must hold for 2 s, otherwise `off` gives `43 64` then `on` `C5 60`); `off` [`42 64`]; `lum 60` [**nothing**]; `on` [`C4 60`, front at 60]; `arriere on` [`C5 60`]; wait 3 s; `avant off` [`85 60`]; wait 3 s; `arriere off` [`03 64`, off, back memory]; `on` [`85 60`]; `sync` [`85 60` and `83 64` interleaved]; `rampe 4C FE 16 60` | Every prediction holds. Ramp: intermediate values 1 or 2 times, `FE` 3 times, `preempted > 0`, lamp at maximum. If `mode avant` does not change the lamps: A4 (b) |
| T3 | diag, two lamps | `auto`; 6 s; `auto`; `off`; `auto` | `E1 01` ×3 then the lamp dims and comes back up; `E1 02` and a new reaction; then « refuse : lampe eteinte » (rejected: lamp off), nothing on B |
| T4 (Q2) | diag | `ecart 20`, alternate `temp 0` / `temp 100` 10 times (3 s apart); same with `ecart 5` | Success rate per gap. We keep 100, unless 20 gives 10/10 (less deafness) |
| T5 (Q1) | diag | `rafale 1 1 1`; alternate `brut C200` / `C264` 5 times (2 s); one packet after 60 s of silence, then after 5 min | Informative, the driver keeps 3. If only the packets after a silence fail: wake-up hypothesis; then try `brut FF00 1 5 force` followed by a single packet |
| T6 | diag | Lamp's USB unplugged, `lum 80`; plug back in; `lum 80`; note the state after the power cut (Q11); `sync` | Trace shows MAX_RT, reconfiguration, retry +1 s then +2 s, « injoignable » (unreachable) ~5 s, target reverted to the raw state. After plugging back in: ok on the first try, no stuck STATUS 21 |
| T7 | diag, `brut` | **Q3** `C335`, `C44C`, `85FE`, `C335`: does each lamp keep its own brightness? **Q4** `8300` / `8364`: does the back lamp change color? **Q5** `4264`, `444C`, `C235`: does the front one turn on at minimum? **Q6** front only `C4FE`, `E0 nn`, sensor covered then lit for 10 s; `E0 nn+1` same; `C480`; `D100` / `D101 force`. **Q7** `E1 n`, `C335`, `E1 n`; `E1 n`, 60 s, `E1 n`. **Q8** front only, `E1 n+1`. **Numbers** `E107`, `E181`, `E1FE` | Results logged in PROTOCOL.md; they decide C7 (A4, A's starting number, back layout) |
| T8 | diag, **batteries back in**, A listening only (`ecoute 1`, `trace 1`) | Djoko: switch ×3, dial min→max, temperature, on/off, favorite, A. `lampe` after each gesture | Raw state = lamp after each gesture. Frames decoded by A ≈ those from B. Favorite → `91`/`89` counted as Reserved, final state correct. A from the remote → `lastAuto` updated, lamps unchanged |
| T9 | diag, batteries | gesture, then `lum A0`, then gesture, ×5; Djoko keeps turning the dial without stopping during `temp 0` | Tracking always correct after our bursts; `holdoffs > 0`; A sends at most 2 s after its request starts; lamp cold at the dial's brightness |
| T10 | diag | `leger 1`: 20 cycles of `temp 0` / `temp 100` (2 s), without batteries. Then `rx fort 1` + `rx 100 5000`, batteries, 10 min with the remote | Light switch: 100% ACK, PID that carries on from one burst to the next on B. Strong re-arm: missed frames ≤ T8, silence reconfigurations sharply down. Adopted only on success |
| T11 | **product** on A | 3 restarts (B: 0 frames); commissioning; every endpoint (EP1 on/off, 1/50/100%, mired extremes; EP2, EP3; EP4); grouped tile and "turn off the lights" then "turn on"; dragging an HA slider; gestures on the remote; lamp unplugged | Frames match D.3; lamp memory kept; slider with no oscillation, correct final value; app up to date in under 1 s; app recovers in ~5 s; « ignore au demarrage » (ignored at startup) counter at 0 |
| T12 | product | Wi-Fi access point on channel 1, then 6, then 11 (or Thread ≠ 11): 20 commands + 20 gestures each | % ACK and % missed frames per channel: gives the channel recommendation |
| T13 | product | change the state, wait 15 s, restart; then `decommission` | Same state in `lampe` and in the app, B: 0 frames. After decommissioning: default address (`lampe adresse`) |
| T14 | diag | phone light meter at `4C/60/80/A0/C0/E0/FE`, front only then both | Adjusting γ (L* lightness), `lampe gamma` then `HALO1_LEVEL_GAMMA` |
| T15 | product | 12 h with B recording | `cause` with no watchdog; `lampe stats`; snapshots before silence reconfigurations (cause of the deafness) |

---

## J. Risks and open questions

| # | Topic | Treatment |
|---|---|---|
| 1 | A single packet ignored (MCU wake-up, or PID+CRC duplicate) | 3 packets and ≥ 2 acknowledgements; T5; a `FF 00` wake-up test if needed |
| 2 | A 100 ms gap never tried from the ESP32 (500 ms proven) | T1 and T4; fallback `lampe ecart 300/500`. Resolved by T1: 3/3 acknowledgements at 100 ms |
| 3 | An acknowledgement does not prove the frame was applied | several acknowledged packets with distinct PIDs; absolute frames, resending is harmless |
| 4 | Listening deafness, cause unknown | proven 500 ms reconfiguration (~8% deaf); snapshots; strong re-arm in T10 |
| 5 | PID across a PRM_RX switch | reset by default; light switch only after T10 |
| 6 | ~0.3-0.55 s deaf per command: remote frames lost | its 3 copies; the wait after the remote; listening within the gaps if T10 succeeds |
| 7 | A remote frame within our acknowledgement window | AckForeign not counted and passed to tracking (not tested; PKT4 in DPL not verified) |
| 8 | Semantics of A (toggle or restart, duplicate reset, mode bits) | momentary endpoint, number `last + 1` reused on retries, A rejected with the lamp off, A frames have no effect on the state; T7 |
| 9 | Per-lamp memory (Q3), back lamp's temperature (Q4), brightness with the lamp off (Q5) | A4 (a) forces the displayed brightness; no value frame sent with the lamp off; deferred until power-on |
| 10 | State drift (auto mode, power outage, missed frames); no readback possible | no automatic reassertion; `lampe sync`; every user command resynchronizes; Q11 in T6 |
| 11 | Coexistence: Wi-Fi 1-3, Thread channel 11 = 2405 MHz, BLE 2402 during commissioning | T12; recommend an access point on channel ≥ 6 and Thread ≠ 11; the module is a few cm away via the existing wires (no soldering) |
| 12 | Grouped Apple tile: "on" turns on both lamps, A fires along with the tile | A's guard; recommend "show the accessories as separate tiles"; A1's option (b) |
| 13 | Attribute restoration from NVS by esp-matter, `factory_reset` erasing `halo1` | forced reflection at startup via `updateAttributeVal`; default address; T13 |
| 14 | Mandatory re-commissioning (layout change) | to document in the README |
| 15 | Actual Kelvin values of 0x00 and 0x64 unknown; logarithmic perception | 153/370 nominal; γ tunable; T14 |
| 16 | DM1 and RT1 read-back not verified | fallback to RFCH and RT1 (C.2); `lampe regs` in T0 |
| 17 | L2 blocking (~300 ms, up to ~0.5 s if the crystal or the calibration does not respond); NVS garbage collection | rare; no interrupt masked; writes only at rest; symptom-triggered L2 limited to one per minute, then one every 10 min (C.5) |
| 18 | Re-pairing by replaying the beacon (not tried, needs a pairing window) | out of scope; pairing address rejected everywhere |
| 19 | Bench tools (`txack`, `xo`) that change the chip or `gXoTrim` | invalidation after every command outside the allowlist; B11 fixed |
| 20 | A jammed chip that verification does not see (Sep 24 incident: crystal touched, registers correct, every send timing out) | symptom-triggered L2: 3 TX timeouts in a row, a flood of bad CRCs while listening, or deaf listening (1000 off-RX re-arms in under 10 s, seen in ~2-3 s at the incident's rate); never on a silent lamp (C.5) |
| 21 | A reset with IO9 (BOOT) held low: download mode, board inert until a power cycle | button actions only on release, after 100 ms high with no interruption, and a re-read right before the action; every `esp_restart()` (button, `reboot`, `decommission`, the end of unpairing by the CHIP task, which comes well after `matterDecommissionNow()`) waits for IO9 high for 50 ms in a row in a shutdown handler, with no limit; button inert during unpairing (C9). Remaining: a reset without these handlers (panic, watchdog) with the button held |
---

## Bench results (Sep 23, 2026, diag build, board A = 144401, witness B = 11301)

Prediction announced to Djoko before each test; logs `logs/drv-T*.log`.

| Test | Result |
|---|---|
| T0 | `lampe autotest` ok, `lampe decode` correct; B: **0 frames, 0 acknowledgements** in 75 s (3 restarts of A then 40 s of listening); RFCH/DM1/RT1 read back 05/82/73; passive snapshot ENAA 00 |
| T1 | `lampe on`: `C5 A5` 3/3 acknowledged, **100 ms** apart (1.6-1.7 ms); B sees PID 1 then 2: the PID advances over the air. Lamp: both lamps at A5 (Djoko notices a brief pass through the remembered state before the frame is applied) |
| T2 | `temp 0` (`C3 00`, coldest), `temp 100` (`C3 64`), **`mode avant` = `C4 A5`** (lamp change carried by a brightness frame: A4 (a) confirmed), `lum 4C`/`FE`, `off` (`42 64`), `lum 60` **deferred** (nothing on air), `on` (`C4 60`), `arriere on` (`C5 60`), `avant off` (`85 60`), `arriere off` (`03 64`, off), `on` (`85 60`, selection memory), `sync` (`85 60` + `83 64` interleaved), `rampe 4C FE 16 60` (intermediate values x1, `FE` x3, 9 preemptions): **everything correct, witnessed by Djoko** |
| T3 | `mode deux` (`C5 FE`), `auto` (`E1 01`) then `auto` (`E1 02`): dims then comes back up each time; `off` (`43 64`); `auto` rejected with the lamp off, nothing on air |
| T6 | lamp unplugged: 3 rounds x 5 packets MAX_RT (11.4 ms), retries +1 s and +2 s, « injoignable » (unreachable), target reverted to the raw state. Plugged back in: the lamp **stays off and keeps its settings** (warm temperature kept); `on` succeeds on the first try (3/3) |
| T8 | batteries back in, `lampe ecoute 1`: switch then dial tracked frame by frame (28 state frames, 0 bad CRCs); final raw state back only, FB, temp 64 = **what Djoko sees**. The switch resends the remote's LAST setting type (here `85 86`, brightness), with the new lamp bits |

Still remaining: T4/T5/T7/T9/T10 (settings and open questions), T11-T13 (Matter product), T14 (gamma), T15 (12 h).
