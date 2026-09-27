[Français](PROTOCOL.fr.md) · **English**

# BenQ ScreenBar Halo radio protocol

## STATE AS OF Sep 24, 2026 -- READ BEFORE ANYTHING ELSE

This block is authoritative. The rest of the document is the chronological log
of the reverse engineering, Halo 2 then Halo 1: it contains conclusions since
refuted (list at the end of the block). Evidence: [AUDIT-2026-09-23.md](AUDIT-2026-09-23.md)
and `tools/audit/` (unified model `indep_pll/t3.py`, expected frames
`synthese/chk.py`) for the format; sections "Semantics CONFIRMED by
transmission" and "Halo 1 pairing" below for the payload; bench results at
the end of [PLAN-PILOTE-HALO1.md](PLAN-PILOTE-HALO1.md) for the driver.

**Radio link: standard BC5602 format (ShockBurst type), established by the
audit then verified by transmission on the lamp.**

```
preambule 01010101 | adresse 63 FD F0 4F | PCF 9 bits | charge | CRC-16
```

- Channel 5 (2405 MHz), 125 kbps, one-byte preamble.
- **Address on air `63 FD F0 4F`, to be written as `4F F0 FD 63`** in the
  BM5602 (reverse order). It is specific to the remote/lamp pair and comes
  from pairing; `lampe adresse` changes it. `8F F7 C1 3C`, used until Sep 22,
  is only a view of it shifted by two bits: good for correlating on
  reception, wrong for transmitting (0 acknowledgements out of 10).
- **9-bit PCF**: payload length (6 bits), PID (2 bits), NO_ACK (1 bit).
- **CRC-16/CCITT 0x1021, initial state 0xFFFF**, over address + PCF +
  payload: the BC5602's hardware CRC. The states 0xDFBE, 0xF55A (Halo 1) and
  0xEFDF (Halo 2 project) are only 0xFFFF advanced by one or two bits.
- **Command** (remote -> lamp): length 2, NO_ACK=0.
  **Acknowledgement** (lamp -> remote): length 0, same PID, NO_ACK=1. The
  acknowledgement is **empty**: the lamp's state cannot be read from it. The
  driver tracks what it sends and what it hears from the remote; the lamp
  emits nothing other than its acknowledgements.
- The PID advances after each acknowledged frame (seen on air, bench T1). Per
  the datasheet, a receiver discards a frame with the same PID and same CRC
  as the previous one, while still acknowledging it.
- Sep 23, remote without batteries: `txack` of `C4 FE` on `4F F0 FD 63`,
  10 acknowledgements out of 10 and the lamp goes from minimum to
  near-maximum; control on the shifted address `3C C1 F7 8F`, 0 out of 10.

**Payload: two bytes, flags then value. Semantics confirmed by transmission
(Sep 23) and by the driver at the bench (T1-T3, T8).**

| bit of the 1st byte | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|---|---|---|---|---|---|---|---|---|
| meaning | on | front lamp | button A | reserved | reserved | brightness | temperature | back lamp |

- Every state frame is **absolute**: on/off and lamps every time, plus
  exactly one selector (bit 5, 2, or 1) that says what the 2nd byte carries.
  Examples: `C3 35` both lamps, temperature 35; `C2 35` front only; `83 35`
  back only; `C5 A5` both, brightness A5; `42 64` off. The remote's on/off
  button replays its last state frame with bit 7 flipped; the lamp switch,
  its last setting type with the new lamp bits.
- **Brightness** (bit 2): `4C` to `FE`. `4C` is the lamp's actual floor
  (nothing changes below it); perception is logarithmic. Switching lamps via
  a brightness frame works (`C4 A5` = front only, T2). Whether each lamp
  keeps its own brightness: open (bench T7).
- **Temperature** (bit 1): `00` coldest to `64` (100) warmest.
- **Button A** (bit 5): the 2nd byte is a press number (01, 02...). A number
  already handled is ignored, a new one re-triggers the effect: the lamp dims
  then comes back up (automatic mode; toggle or re-trigger, not settled). An
  A frame does not carry state (seen `60 01`, bit 7 at zero). `E1 01`, never
  emitted by the remote, is understood correctly: it really is a bit field.
- **Bits 3 and 4**: seen only in the favorite recall (`91 00`, `89 xx`), with
  no visible effect when transmitted: never emitted [otherwise]. The
  favorite replays a complete state (mode, brightness, then `91`, `89`).
- **Service frames**: `FF 00`, `FE 00`, `FD 00` (wake-up, NO_ACK=0) and
  `FA xx`, the only frame with NO_ACK=1 (`A8`, then `F8` after the batteries
  are replaced).
- Rate: the buttons emit each state in 3 copies at ~100 ms, the dial a single
  frame every ~112 ms. A single frame was not enough once (test 1), three
  was: the driver emits each frame 3 times at 100 ms (T1). The
  "`txack ... 3 300`" tests from Sep 23 actually ran at **500 ms**: the CLI
  clamps the gap to 500 ms on channel 5.
- After a power cut (USB unplugged), the lamp stays off and keeps its
  settings (T6).

**Pairing.** The remote emits a beacon on `59 01 00 B0` (to be written as
`B0 00 01 59`), channel 5, 125 kbps, payloads `5A 5A`, `F5 C3`, `CF 49`
cycling; the lamp in pairing mode acknowledges it (empty acknowledgement).
The link address derives from it via an unknown function; it did not change
after a re-pairing. Never transmit or acknowledge on a pairing address
(Halo 1, nor Halo 2's `E2 08 00 B0`): `txack`, `prxack`, `addr`, and the
driver refuse them. To observe: `ecoute B0000159 5`.

**Tools.** Product: the driver (`lampe ...`, `src/halo1_*`) and the Matter
bridge. Bench: `txack` (standard transmission, automatic acknowledgement,
TX_DS/MAX_RT verdict), `ecoute` (passive reception that never acknowledges,
decoded by `halo1::decodeAir`, the driver's decoder), `prxack` (bench
receiver that acknowledges). The original Halo 2 layer (10-byte payload,
state read back from the acknowledgement, polling every 5 s) and its tools
are removed (step C6, Sep 24): `poll`, `send`, `find`, `pair`, `sniff`,
`tail`, `debug`, `normal`, the hunts `appaire`, `preambule`, `ancre`, and
`benq`, `tx6`, `txraw`, based on refuted models.

**Open questions**: exact meaning of button A and of bits 3/4; brightness
specific to each lamp (T7); why a single frame was ignored (lamp wake-up or
PID duplicate); the function that derives the link address from the beacon.

**Passages of the log below that are REFUTED** (no longer to be cited):

| Passage | What is wrong |
|---|---|
| "Status of the information", "Radio layer", "Payload (10 bytes)", "Pairing" (`E2 08 00 B0`), "Recovering the address" | Halo 2: 10-byte payload, state read from the acknowledgement, tail bytes, sensor, polling every 5 s. None of that holds for the Halo 1; the commands cited no longer exist. |
| "Halo 1 frame structure, confirmed without the CRC" (6 bytes, 72 = 48+16+8) | The command carries 2 bytes, the acknowledgement none; the second address is the lamp's acknowledgement, not a retransmission; `7A FF` was a false positive. |
| "Two frame families: commands and acknowledgements" (header [length 4][counter 2][type 2], states 0xDFBE / 0xF55A, "19 exact commands") | Header straddling the PCF and the payload; a single initial state 0xFFFF; about 31 commands out of 32 are exact with the right model. |
| "The lock: obtaining an exact B frame" | Wrong: the frames were exact, it was our slicing that was offset. The lock was transmission, lifted on Sep 23. |
| "Error bias: 100% of 1s read as 0" | Observed on 7 cases, against a reference that was itself offset. Not established. |
| Crystal trim, analog values, distance "ruled out" | Each condition had only 7 to 9 frames, judged with a wrong CRC model, and the "holtek 0" arm was running without AGC (bug B5). Only an effect of a factor of 3 or more is excluded. |
| Any passage about the PCF being "a full byte" | The PCF is 9 bits; the "full byte" came from the shifted address. |
| "The remote's bitrate is not 125 kbps" | 125 kbps, confirmed by transmission. |
| GIO3 "upstream of the correlator", "RF path closed", "contradiction established" | Already refuted further down in this document; explained by the address. |
| "First reading of the payload" (provisional readings, "to be confirmed") | Settled by the "Semantics CONFIRMED by transmission" section and by the table above. |
| "The pairing lead is closed" ("no pairing exchange to capture") | Listening on the Halo 2 pairing address (`E2 08 00 B0`) and at 250 kbps, a refuted bitrate. The pairing beacon `59 01 00 B0` exists and the lamp in pairing mode acknowledges it (section "Halo 1 pairing"). |
| "Why an nRF52840 is needed" | Superseded: the CC2500 found the address on Sep 22 (a shifted view, corrected by the audit) and the BM5602 has been transmitting to the lamp since Sep 23. No nRF52840 is necessary. |


## Status of the information

> **HISTORY.** From here on, chronological log. The sections that follow, up
> to "What the Sep 21, 2026 measurement campaign established", describe the
> Halo 2 and the old firmware: they do not hold for the lamp (Halo 1), and
> the CLI commands cited (`find`, `pair`, `sniff`, `send`, `tail`, `benq`,
> `tx6`, `txraw`, `appaire`, `preambule`, `ancre`...) no longer exist. The
> Sep 24 state is the header block.

This whole document comes from the reverse engineering of the
**ScreenBar Halo 2** by
[kuzmin-no](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration),
cross-checked against the FCC filings and the
[Benq Screenbar support](https://community.home-assistant.io/t/benq-screenbar-support/490864)
thread on the Home Assistant community.

| Element | Halo 2 | Halo 1 (this project) |
|---|---|---|
| RF transceiver | BC5602 | **BC5602 — confirmed** (FCC + PCB teardown) |
| Band | 2405–2475 MHz | **2405–2475 MHz — confirmed** (FCC `JVPCR20CCTR`) |
| Modulation / bitrate | GFSK 125 kbps | Very likely (same chip, same band) |
| Frame format | ESB: preamble + 4-byte address + 9-bit PCF + payload + CRC | Very likely (imposed by the BC5602) |
| Payload structure | 10 bytes, documented below | **To be confirmed** |
| Tail bytes | `01 02` | **To be confirmed** (`tail` to change them) |
| "Sensor" bit | ultrasound | **To be confirmed** (the Halo 1 has no presence sensor) |

In other words: the radio layer is established, the application layer
remains to be verified. The `sniff`, `pair`, and `send` CLI commands are
there for that.

The Halo 1's PCB (documented by `b4shful` on the HA thread): PSoC Cypress
**CY8C4125LQI-483** + **BC5602** transceiver + **TCA9539PWR** I²C expander.

## Radio layer

- Channel 1: **2405 MHz** (`RFCH = 5`) — the only one observed in practice
- Channel 2: 2446 MHz (`RFCH = 46`)
- Channel 3: 2475 MHz (`RFCH = 75`)
- GFSK modulation, **125 kbps** (`DM1 = 0b10`)
- 4-byte address, **written in the reverse order** of the over-the-air order
  (*Bit ordering* section of the BC5602 datasheet)

Frame format, inherited from Enhanced ShockBurst:

```
préambule 0xAA │ adresse 4 octets │ PCF 9 bits │ payload 10 octets │ CRC
```

The PCF (Packet Control Field) contains: length on 5 bits (offset by 3), PID
on 2 bits, NO_ACK flag on 1 bit.

**The lamp never transmits spontaneously.** It only replies in the hardware
ACK slot that follows a received frame. Hence the firmware's strategy:
polling every 5 s, and passive listening to the remote between two polls.

### Auto-ACK and listening

The BC5602 handles auto-ACK in hardware. Two consequences:

- to **drive** the lamp, we enable auto-ACK: the lamp's reply arrives in the
  RX FIFO right after transmission;
- to **listen to** the remote, it must be **disabled** — otherwise our
  module would acknowledge frames at the same time as the lamp, and the
  remote would stop working.

Disabling auto-ACK also disables the hardware CRC and dynamic payload
length. The 9-bit PCF is then no longer stripped from the stream, which
shifts every byte by one bit: the firmware re-aligns it on read
(`BC5602::shiftLeftOneBit`).

## Payload (10 bytes)

| Byte | Content |
|---|---|
| 0 | Command |
| 1 | Control register (bits, see below) |
| 2 | Front lamp brightness, `0x01`–`0x64` (1–100%) |
| 3 | Color temperature, high byte |
| 4 | Color temperature, low byte |
| 5 | Back lamp brightness, `0x01`–`0x64` |
| 6 | Back color temperature, high byte (identical to the front) |
| 7 | Back color temperature, low byte |
| 8 | Tail byte 0 — `0x01` on the observed Halo 2 |
| 9 | Tail byte 1 — `0x02` on the observed Halo 2 |

The color temperature is transmitted **in Kelvin, in the clear**: 2700 K =
`0x0A8C`, 4000 K = `0x0FA0`, 6500 K = `0x1964`.

The back lamp has no temperature of its own: both share the value.

### Control register (byte 1)

| Bit | Role |
|---|---|
| 0 | General on/off |
| 1 | Auto mode |
| 2 | Favorite |
| 3 | ┐ `0` = front only, `1` = back only, `2` = both |
| 4 | ┘ |
| 5 | Sensor (ultrasound on the Halo 2) |
| 6–7 | Unused |

### Commands (byte 0)

| Value | Meaning |
|---|---|
| `0x00` | The remote wakes up and contacts the lamp |
| `0x02` | General on/off |
| `0x03` | Brightness + color temperature setting |
| `0x04` | State sync request |
| `0x05` | The remote goes to sleep and informs the lamp |
| `0x0A` | Pairing mode |

The lamp applies changes **as a gradual fade**. A `0x03` frame is therefore
not immediately reflected: the firmware re-polls with `0x04` every 400 ms
until convergence (12 attempts max, ~5 s).

## Pairing

During pairing, remote and lamp communicate on a fixed address:
**`E2 08 00 B0`** on air (i.e., `B0 00 08 E2` in register write order). The
command is always `0x0A`.

The final communication address seems to be transmitted during this exchange
in encoded form — it has not been decoded. That is why it must be recovered
by capture (see below).

The `pair` CLI command puts the module in listening mode on this address: it
is the best way to obtain usable Halo 1 frames **without knowing the
communication address**, and thus to first check whether the payload
structure above holds.

## Recovering the communication address

### Method 1 — the sync word trick (`find` command)

This is the method from the `find_halo2_address.py` script, generalized.

We tune the receiver to a 3-byte pseudo-address matching a **payload**
sequence whose value we know, because we just set it on the remote. Bytes
5-6-7 work well: back brightness + color temperature.

Example with 10% and 3925 K:

```
sur l'air        : 0A 0F 55
ordre d'écriture : 55 0F 0A
```

The receiver thus locks **in the middle** of a frame, then keeps sampling.
Automatic retransmissions make the start of the next frame appear within the
same capture window: preamble `0xAA` followed by the real address.

The original script read the address at a fixed offset, tied to the Halo 2's
inter-frame timing. Here the firmware **sweeps all 8 bit alignments across
the whole captured window**, then counts the occurrences of each candidate:
the right one stands out through repetition, the noise does not. This is
what makes the method portable to the Halo 1, whose timing has no reason to
be identical.

```
find            # 10 % arrière + 3925 K (valeurs par défaut)
find 25 4000    # autres valeurs : luminosité %, Kelvin
find x550f0a    # mot de synchro brut de 3 octets
```

### Method 2 — sniff the remote's SPI bus

The method that cannot fail, suggested by `b4shful` on the HA thread. Open
the remote, hook a logic analyzer to the BC5602's `CSN` / `SCK` / `SDIO`,
and capture. The `0x10` command (`WRITE_PTX_ADDRESS`) is followed by the 4
address bytes, in the clear.

A significant bonus: the same capture also gives the Halo 1's real payloads,
hence the frame's exact structure — which answers, in one shot, every "to be
confirmed" box in the table at the top of the page.

### Method 3 — HackRF One + Universal Radio Hacker

Capture at 2405 MHz, GFSK demodulation at 125 kbps, manual frame decoding.
This is the method that was used to establish the payload table for the
Halo 2. Heavier to set up, but it does not require opening up the hardware.

## References

- [BC5602 datasheet v1.20](https://www.holtek.com/webapi/116711/BC5602v120.pdf)
- [BM5602-60-1 module](https://www.holtek.com/page/vg/BM5602-60-1)
- [kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration)
- [Home Assistant thread "Benq Screenbar support"](https://community.home-assistant.io/t/benq-screenbar-support/490864)
- FCC: [`JVPCR20CCTR`](https://fccid.io/JVPCR20CCTR) (Halo 1 remote),
  [`JVPCR20C`](https://fccid.io/JVPCR20C) (Halo 1 lamp)

---

# What the Sep 21, 2026 measurement campaign established

## Real frame (Halo 2, published by Termina1)

```
54  04 10 0C 0F 55 5B 0F 55 01 02  20 B9
↑   └──────── payload 10 octets ────┘  └CRC┘
PCF
```

The payload structure documented above is thus **confirmed on a real
frame**: command `04`, control `10`, front brightness `0C`, temperature
`0F 55` (3925 K), back brightness `5B`, temperature repeated, tail `01 02`.

The frame is **13 bytes** on reception: `PCF(1) + payload(10) + CRC(2)`. The
PCF occupies **a full byte** — there is **no** one-bit shift on read,
contrary to what the initial port assumed.

## CRC — verified model

```
algorithme    : CRC-CCITT
polynôme      : 0x1021
état initial  : 0xEFDF avant les 4 octets d'adresse EN ORDRE SUR L'AIR
couverture    : adresse + PCF + payload (10 octets)
```

Validated on three vectors, intermediate state included:

| PCF | Payload | Expected CRC |
|---|---|---|
| `54` | `04 10 0C 0F 55 5B 0F 55 01 02` | `20B9` |
| `50` | `02 11 0C 0F 55 5B 0F 55 01 02` | `E962` |
| `50` | `02 10 0C 0F 55 5B 0F 55 01 02` | `0241` |

After the 4 address bytes `86 BB EA 9C`, the state equals `0x5042`.

Implemented in `BenqHalo::frameCrc()` and `frameCrcFor()` (Halo 2 model;
functions removed at step C6, Sep 24).

## Constraint on the address

BC5602 datasheet v1.20 p.25, below the packet format diagram:

> `Note: * MSB high 4-bit must be 0001xxxx or 1110xxxx`

The first byte of the address **on air** must be of the form `0x1X` or
`0xEX`. The pairing address `E2 08 00 B0` conforms to this. Note that
Termina1's address `86 BB EA 9C` does not conform to it while still working:
the exact scope of the rule remains uncertain.

## Correct receive configuration

Aligned with Termina1's ESPHome implementation, which actually receives:

```
DPL1 = 0x00, DPL2 = 0x00     payload statique
RXPW0 = 13                   PCF + payload + CRC
PKT1 = 0x00                  CRC matériel désactivé
ENAA = 0x00                  auto-ACK désactivé
IRQ1 = 0x40                  acquitter RX_DR — INDISPENSABLE
puis commande 0x8E           RX Mode Trigger
```

`RX_DR` latches and **must** be acknowledged by writing 1 to it, otherwise
the chip stops delivering frames.

## Variables ruled out by measurement

Bitrate (the 3 existing values), channel (the 3 from the FCC filing),
address byte order (both), preamble length (1 and 2 bytes), address length
(3 and 4 bytes), and the existence of a demodulated-bit output on `GIO2`
(8 selectors swept, no activity).

## What is blocking

The **communication address** of the lamp/remote pair remains unknown, and
the BC5602's correlator cannot capture anything without it. The method of
locking onto a payload sequence as a pseudo-address has never caught,
despite a receiver whose operation is measured (RX mode confirmed by `OMST`,
RSSI with 17 dB of dynamic range, clean RF environment).

The only two remaining paths require hardware:

1. **Logic analyzer** on the remote's BC5602 `CSN`/`SCK`/`SDIO`: the `0x10`
   command carries the address there in the clear, and the same capture
   gives the Halo 1's real payload structure.
2. **Second MCU** to build the independent receiver that Termina1 mentions
   in its implementation notes.

## Remote behavior (measurement)

The controls are **touch-based**: a touch emits **one pulse**, holding the
finger down emits nothing more. Only the **dial** produces a continuous
stream while it is turned — making it the only usable traffic source for a
fixed-window capture.

## BC5602 direct mode — undocumented selectors

The `DIR_EN` bit (`CFG1` 0x00, bit 4) switches the chip into direct mode:
*"TX/RX data from/to external MCU directly"*. The datasheet mentions it
**only once** and says neither which pin carries the data nor how to engage
the mode. The Holtek application guide does not mention it at all.

Termina1's ESPHome port, which works, reveals two selector values that the
datasheet nonetheless files under "Others: No function, input":

| Register | Value | Actual function |
|---|---|---|
| `IO1` (0x06), `GIO2S` | `3` | `DIRECT_TXD` — data, MCU to chip |
| `IO2` (0x07), `GIO3S` | `8` | `TBCLK_OUTPUT` — bit clock, chip to MCU |

Its arming sequence: `IO2=0x08`, `IO1=0x58`, `CFG1=0x50` (**`AGC_EN` +
`DIR_EN`**), then `OM=0x03`, 50 µs, `OM=0x07`. Bits 2~0 of `OM` are declared
"Reserved, must be kept unchanged after power on": they are in fact hidden
command bits.

`GIO3` is **pin 8** of the BM5602 module — the "seventh wire" of its wiring.

Two consequences for this project:

1. Our configuration was running with **`AGC_EN` at 0** (`CFG1` read back as
   `0x00`).
2. It has **never attempted reception in direct mode**: its RX path resets
   `GIO2S=1` and goes back through the packet engine. The `DIRECT_RXD`
   selector, if it exists, is to be looked for among the remaining `GIO2S`
   values (2, 4, 6, 7).

Entering reception without a strobe command, which IS documented
(`ds.txt:717`):

> If the device is set as a PRX device, it will enter the RX mode when the CE bit
> is set high by using register or using Strobe RX command.

## Confirmed radio parameters

Read from the ESPHome port that actually drives a lamp:

```
RADIO_CHANNEL = 5        ->  2405 MHz
write_reg(0x11, 0x82)    ->  DM1 : AW=10 (4 octets) + 010 (125 kbps)
```

Bitrate **125 kbps**, **4-byte** address, channel **5**. These are already
the default values of `sharedRadioConfig()`.

Defect fixed on Sep 21, 2026: `AGC_EN` (`CFG1` bit 6) was never enabled. The
third-party port writes `CFG1 = 0x50` (`AGC_EN` + `DIR_EN`). Measured on our
board, dial turning: strongest received signal **85 dB without AGC, 41 dB
with**. Since any software reset resets `CFG1` to `0x00`, the bit is
reapplied in `sharedRadioConfig()`, the same as the preamble bit.

## Transmission requires `CE`

Measured on Sep 22, 2026 on a two-board bench: 2553 frames written to the
transmit FIFO, **zero** `TX_DS`, `OMST` stuck at `2` (Light Sleep), TX mode
never observed. The `0x0E` strobe command is not enough.

The datasheet explains it (`ds.txt:711`):

> If the device is set as a PTX device and the CE bit is set high, it will stay in
> the Light Sleep mode when the TX FIFO is empty. **The PTX device will enter the
> TX mode automatically once the TX FIFO is not empty.**

Transmission is therefore not triggered by a command but by the **FIFO
filling up**, provided `CE` (register `0x15`, bit 0) is at `1`. Without it,
the chip waits indefinitely, FIFO full.

`CE` is now set in `configForLoopback()` and in `prepareToTransfer()`.
Reminder: `CE` is **cleared by the hardware** at the end of every reception,
so it must be set again on every attempt to enter RX.

## `EN_DYN_ACK`: the FIFO write silently refused

The datasheet (`ds.txt:869`), in the description of `DPL2` (register
`0x2B`):

> Bit 0 **`EN_DYN_ACK`**: PTX "write TX FIFO with No-Auto-ACK" command enable

As long as this bit is `0`, the FIFO write command **without** auto-ACK is
**ignored with no error signal whatsoever**: the FIFO stays empty, the chip
has nothing to transmit and remains in Light Sleep.

Measured with the `autotest` command, four combinations on silicon:

| `EN_DYN_ACK` | write command | FIFO filled? |
|---|---|---|
| 0 | without auto-ACK | **no** |
| 1 | without auto-ACK | yes, and `TX_DS` fires |
| 0 | with auto-ACK | yes |
| 1 | with auto-ACK | yes |

This is the defect that explained 2553 "transmitted" frames without a single
real transmission. The correct transmit sequence is therefore: `DPL2` bit 0
to `1`, write the FIFO, `CE` to `1` — after which the chip goes into TX **by
itself**, with no strobe command.

## Locking onto the preamble is impossible — demonstrated

The idea: since the preamble is known (`AA` repeated), give the correlator a
3-byte address of `AA AA X` so it locks onto the preamble plus the first
address byte, and delivers the next three bytes — that is, the rest of the
address.

**Tested on a transmitter whose address was known in advance**, on
Sep 22, 2026:

| | |
|---|---|
| beacon | 4474 frames, **100% confirmed by `TX_DS`** |
| preamble sent | 2 bytes, verified effective (`CFO1 = 0x4F`) |
| address | `E1 22 33 44` on air, channel 5, 125 kbps |
| receiver | validated at the same time: 421 frames received out of 421 |
| **result** | **0 locks out of 512 configurations** |

The correlator **cannot lock onto a pattern containing the preamble**, most
likely because it only arms after detecting a valid preamble. The method is
closed: do not retry it.

Methodological corollary: this two-board bench makes it possible to
**validate a discovery technique on a known address** before running it
against the lamp. Every future method must go through this first.

## The remote's bitrate is not 125 kbps

Burst-duration measurement from Sep 22, 2026, channel 5, 70 dB threshold.

**Calibration** against the beacon, whose bitrate (125 kbps) and frame
(19 bytes = 152 bits = 1216 µs theoretical) are known:

```
1620 rafales — dominante 800-1499 µs, moyenne 913 µs, plus longue 1755 µs
```

The instrument reads about a quarter short (913 for 1216): RSSI register
inertia and a threshold that clips the edges.

**The lamp's remote**, dial turning:

```
817 rafales — dominante 400-799 µs, moyenne 531 µs, plus longue 785 µs
bande 800-1499 : ZERO
```

No burst in the band where the beacon placed 929 of them, and a maximum
burst of 785 µs versus 1755 µs. Corrected for the bias, the measured 531 µs
is worth about **708 µs real** — i.e., 608 µs for 19 bytes at 250 kbps,
versus 1216 µs at 125 kbps.

**125 kbps on a 19-byte frame is ruled out by the measurement.** Duration
alone does not formally separate "19 bytes at 250 kbps" from "11 bytes at
125 kbps," the Halo 1's payload length being unknown — but every hunt run so
far was listening at 125 kbps, which is enough to explain their zeros.

The bitrate is now adjustable and **persisted** (`debit 125|250|500`),
instead of being hard-coded in `sharedRadioConfig()`.

## The pairing lead is closed

Capture on the pairing address `E2 08 00 B0`, with, for the first time, a
**validated** receiver (421 frames out of 421 at the same moment), at the
**measured bitrate** (250 kbps) and on the **measured channel** (5): **zero
frames**, including while camping on channel 5 alone with three times more
time per combination.

The explanation is not instrumental. Observed on Sep 22, 2026: the pairing
procedure, which the day before had concluded with the remote returning
within two seconds, **now systematically times out after ten**, and this
**with both ESP32s unplugged**. The remote otherwise continues to drive the
lamp.

In other words: the pair is already bonded, the procedure has nothing to
renegotiate, and **there is no pairing exchange to capture**. Do not revisit
this lead.

## What remains

The address must be read where it is written in the clear: on the
**internal SPI bus** of the remote or the lamp, at startup, when the
microcontroller loads it into its BC5602 ("write PTX address" command).

The lamp is probably the simplest target: bulkier, USB-powered and thus easy
to restart at will, and it carries the same address as the remote. One
ESP32 is enough to capture this bus.

## Tapping the remote's SPI bus — status update

The remote has been opened. Its board carries a **bare `BC5602`** (designator
`U4`, QFN-16), the `Y1` crystal to its left, and the antenna silkscreened
above — two physical landmarks that confirm the case's orientation.

Pin numbering inferred from the silkscreened corner markers (`4/5`, `8/9`,
`12/13`, `16/1`), antenna at the top and crystal on the left: top edge `1-4`
right to left, left edge `5-8` top to bottom, bottom edge `9-12` left to
right, right edge `13-16` bottom to top.

**The three signals come out on test pads**, verified with a multimeter — no
need to solder onto the QFN:

| Pin | Signal | Pad |
|---:|---|---|
| 11 | `CSN` | the lowest one in the right-hand column |
| 12 | `SCK` | the middle one, near `C34` |
| 14 | `SDIO` | the highest one |

A ground is available on a pad above and to the left of the chip.

Idle levels read with the `taptest` command, battery in place: `CSN` held
**high**, `SCK` held **low**, `SDIO` high. These are the idle states of a
healthy mode-0 SPI bus, and they cannot come from floating lines: the pads
are therefore the right ones.

**What is blocking is purely mechanical.** A contact held with tape and by
hand does not survive a capture: the log fills up with `FF`, `00`, `80`,
`C0`, `E0`, `F8`, `FC` — runs of ones then zeros, the signature of a shift
register clocked by a line toggling at random. Capture now filters out this
noise, and only reports an address write if `0x10` is followed by at least
four bytes that are not all zero.

**Spring-loaded test probes** are needed to hold the four contacts while the
battery is removed and reinserted.

### Capture tooling — five defects fixed

The first version of `sniffspi` could not have worked. Five defects, all on
the software side, found thanks to field observations:

1. **Deaf between two transactions.** Only one transaction armed, and a
   multi-millisecond `Serial.flush()` between each one. We caught the first
   frame of a burst and slept through all the rest. The symptom that put us
   on the trail: "when the battery is inserted, the counter goes up by 1" —
   whereas an initialization involves dozens of exchanges.
2. **Noise stored instead of discarded.** Filtering only happened at display
   time: the buffer filled up with noise in the first few seconds, and the
   useful burst was lost for lack of space.
3. **Releasing the peripheral with transactions still armed**, hence a
   `Load access fault` that took down the capture. The queue must be
   drained first — and displayed **before** tearing down.
4. **Display truncated to 20 bytes** while transactions go up to 32: the
   discriminating content was invisible.
5. **Misleading contact test.** Without the battery, all lines are pulled
   toward ground and no longer follow the internal resistors: they read as
   "driven." The discriminant is `CSN`, which the microcontroller holds
   **high** at rest.

Worth noting for later: the slave SPI driver enables internal pull-up
resistors, so **a disconnected pin reads 100% high**. Three lines at 100%
high do not mean "everything is fine" but "nothing is touching."

Final state of the tooling: capture with six pre-armed transactions, noise
filtered on the fly, state of the three lines and counters displayed once a
second, full readout over 32 bytes. **All that is missing is a reliable
mechanical contact** — three one-millimeter pads cannot be held by hand.

## `J5`: the microcontroller's programming connector

Six plated through-holes, designator `J5`, between the touch controller `U2`
and the board's cutout. They accept a Dupont pin by simple friction — the
one spot on the board where mechanical contact poses no problem at all.

Readings with a multimeter, remote powered, ground on hole 1:

| Hole | Reading | Interpretation |
|---:|---|---|
| 1 | continuity with ground | `GND` |
| 2, 4, 5, 6 | 2.49 V | pulled high |
| **3** | **0.001 V** | **`SWCLK`** — pulled low by its internal resistor |

None of the five carries `CSN`, `SCK`, or `SDIO`: this is not a radio test
connector. The signature — a single hole low while the others are high — is
that of an `SWD` port: `SWDIO` and reset sit high, `SWCLK` low.

**Consequence for logic levels**: the remote's logic runs at **2.5 V**, not
3.3 V, which is consistent with batteries wired in parallel and stepped up
by a converter. The ESP32-C6's high-level threshold is about 2.48 V: the
signals therefore arrive ten millivolts above the threshold. It works, but
with no margin — worth keeping in mind for any noisy capture.

An `ST-Link V2` probe working at 3.3 V will need to go through two series
resistors of a few hundred ohms on `SWDIO` and `SWCLK`, so as not to turn on
the target's protection diodes.

### Bit-banged `SWD` implementation

`src/swd.cpp` generates the protocol without dedicated hardware: line reset,
JTAG-to-SWD switchover, then exchanges of 8 request bits, 3 acknowledgement
bits, and 32 data bits with their turnaround cycles.

The `swd` command **figures out on its own which hole is `SWDIO`** among
several ESP32 pins wired all at once to the unknown holes — no rewiring
between attempts. It does not stop at the `IDCODE`: it then requests
power-up of the debug domain and checks the hardware acknowledgement, which
distinguishes a real link from a lucky read.

Known caveat: Artery often ships its microcontrollers with flash read
protection locked. The link can therefore be established without the
content being accessible.

## Mid-frame locking IS possible — provided there is an anchor

**Correction of an erroneous conclusion.** A previous version of this
section claimed the opposite, backed by "demonstrated." The demonstration
was worthless.

The preamble detector arms on an **alternating sequence**. A 3-byte window
taken from the payload can therefore only be locked onto if the byte that
**precedes** it looks like a preamble, i.e., equals `0x55` or `0xAA`.

kuzmin's `find_addr.py` script, whose Home Assistant integration works, says
so explicitly:

```python
HALO2_ADDRESS = [0x55, 0x0f, 0x0a]
# 0x55, 0x0f -> Color temperature 3925K
#               (reverse byte order; used as preambule and part of the sync word)
```

Hence its instruction to tune the lamp to exactly **3925 K**: it is the
value whose low byte equals `0x55`, and that byte **serves as the
preamble**.

The first control test used the beacon payload
`DE AD BE EF 01 02 03 04 05 06`, whose longest alternating run is seven
bits — too short to arm the detector. **None of its eight windows was
anchored: the experiment could not have locked, regardless of the chip's
actual capability.**

Redone with a payload carrying an anchor, `DE AD 55 0F A0 3C 01 02 03 04`:

| Window | Bytes | Anchored | Frames |
|---|---|---:|---:|
| 0-2 | `DE AD 55` | no | 0 |
| 1-3 | `AD 55 0F` | no | 0 |
| 2-4 | `55 0F A0` | no | 0 |
| **3-5** | **`0F A0 3C`** | **yes** | **158** |
| 4-6 | `A0 3C 01` | no | 0 |
| 5-7 | `3C 01 02` | no | 0 |
| 6-8 | `01 02 03` | no | 0 |
| 7-9 | `02 03 04` | no | 0 |

One positive, seven negatives — including for the windows that **contain**
`0x55` without being preceded by it. And the FIFO returns `01 02 03 04 …`,
exactly the bytes following the anchored window.

**The `find` procedure is therefore valid.** Its failures are explained by
two causes identified since: the **bitrate** — every hunt was running at
125 kbps, whereas the burst-duration measurement rules out that bitrate for
the Halo 1 — and the **anchor**, which requires tuning the lamp to a value
with a byte equal to `0x55` or `0xAA`.

Usable anchor values: a temperature whose low byte is `0x55` (2645, 2901,
3157, 3413, 3669, **3925**, 4181, 4437 … K) or `0xAA` (2730, 2986, 3242,
3498, 3754, 4010 … K); or a **brightness of 85%**, which equals `0x55`.

Note: locking onto the **preamble itself** remains impossible, and for a
reason that follows from the same mechanism — nothing precedes the
preamble, so it cannot be anchored.

## The anchored hunt applied to the Halo 1 — no result

The anchoring mechanism is **validated on the bench** (see above: one
positive, seven negatives). Applied to the Halo 1's remote, it yields
nothing.

Measurements from Sep 22, 2026, remote reassembled and the dial turned
continuously, with a positive traffic control each time (12 to 13 per mille
of strong signal, peaks at 19-20 dB):

| Configuration | Candidates | Locks |
|---|---:|---:|
| 32 anchor temperatures × 101 brightness levels | 4995 | 1 |
| back lamp off, 32 temperatures | 3000 (94 passes) | 1 |
| temperature fixed at 3925 K, 101 brightness levels | 5000 (50 passes) | 2 |

A 24-bit pattern recurs by chance about once every 16 million positions, and
tens of millions of them go by per minute: **one or two locks per run is the
expected noise**, not a clue. Their contents are, moreover, unreadable and
their brightness values scattered.

Three earlier locks, all reported at 3925 K, had seemed to correlate with
the remote's display. The focused test above — temperature fixed, 50
complete sweeps over brightness — would have produced dozens of locks had
that correlation been real. It was not.

**What this leaves open**: the Halo 1's payload structure has never been
verified, it is assumed identical to the Halo 2's. If it differs, the whole
anchored-window approach collapses — and nothing in our measurements can
settle it.

## Address-less detectors — what they say

Two instruments have been built that require no address, exploiting the
fact that `GIO3` only stirs if a **preamble has been detected**:

- `debitgio` — sweeps the three bitrates. **Validated on the beacon**: 434
  and 442 transitions at its real bitrate, zero at the other two. Perfect
  discrimination.
- `canalgio` — sweeps the 84 channels. **Validated on the beacon**: only its
  channel stands out, accompanied by its **image** sixteen channels higher
  (8 MHz intermediate frequency), which the RSSI can distinguish from the
  real channel.

Applied to the remote, with confirmed traffic: **no transition, at any
bitrate, on any channel**, with a one-byte or two-byte preamble alike.

The chip therefore never locks onto the remote's preamble, even though its
board also carries a `BC5602`. This finding is solid and remains
unexplained.

## The bitrate, settled by the FCC report

The remote's lab report (`scratchpad/ctr_test_report.txt`) gives, for the
three channels 2405 / 2446 / 2475 MHz:

| Frequency | 20 dB width | 99% occupied bandwidth |
|---|---|---|
| 2405 MHz | 0.504 MHz | **0.430 MHz** |
| 2446 MHz | 0.508 MHz | **0.434 MHz** |
| 2475 MHz | 0.508 MHz | **0.447 MHz** |

The datasheet gives the deviations applied by the chip (`ds.txt:198-200`):
160 kHz at 125 and 250 kbps, 250 kHz at 500 kbps. For GFSK, the occupied
bandwidth is approximately `2 × fDEV + bitrate`:

| Bitrate | Expected width | Verdict |
|---|---|---|
| **125 kbps** | **445 kHz** | **compatible** |
| 250 kbps | 570 kHz | excluded |
| 500 kbps | 1000 kHz | excluded |

**The remote transmits at 125 kbps.** This contradicts the burst-duration
estimate, which pointed to 250 kbps assuming a 19-byte frame.

### Consequence: the Halo 1 frame is short

At 125 kbps, the corrected burst duration (~708 µs) corresponds to about
**11 bytes**, not 19. Preamble, address, PCF, and CRC consume 8 or 9 of
them: leaving only **2 to 4 payload bytes**, where the Halo 2 has ten.

**The Halo 1's frame structure is therefore not the Halo 2's** — an
assumption that underpinned every payload-window hunt, and which explains
their failure.

## The open contradiction

Two sets of facts that cannot both be true if the two chips are configured
the same way:

- the remote **transmits** (the lamp replies to it) and carries a `BC5602`;
- our `BC5602` **never detects its preamble**, after sweeping 84 channels ×
  3 bitrates × 2 preamble lengths × 3 address widths × 2 initialization
  sequences, with the instrument validated against the beacon every time.

**Caveat**: these sweeps allot only 0.7 to 3 s per channel, and the RSSI
control never saw channel 5 stand out during these runs — whereas the
`presence` measurement saw it jump by a factor of seven that same morning. A
source with a ~1% duty cycle can be missed by a sweep.

**Top priority for the next session**: camp a full minute on channel 5
rather than sweep.

```
debit 125
canalgio 60000 5 5
```

This settles between "the chip cannot demodulate this source" and "we were
simply never there at the right moment." The forty previous sweeps could
not separate these two readings.

Variables still untested if this stakeout yields nothing: the **frequency
deviation** and the undocumented **modem** settings. Only the remote's
firmware, readable via `SWD` on `J5`, can deliver them.

### The contradiction, locked down by two controlled measurements

`presence` measurement from Sep 22, 2026 at 19:24, "very strong" band
(RSSI ≤ 49 dB), three alternating rest / dial-in-hand cycles:

| Channel | Rest | Dial in hand | |
|---|---:|---:|---|
| **5 — 2405 MHz** | **5** | **847** | **×169** |
| 46 — 2446 MHz | 0 | 0 | — |
| 75 — 2475 MHz | 9 | 14 | — |
| 80 — BLE control | 159 | 146 | flat |

The control does not move, nor do the other two FCC channels. **The remote
transmits on channel 5, and the receiver hears it very strongly.**

Two minutes earlier, a full minute camped on that same channel 5 at
125 kbps: **1.29% strong signal, zero preamble detections.** About eleven
hundred bursts went past the demodulator without a single one being
recognized.

Both measurements have their own control and were made in the same session.
**The contradiction is thus established, not assumed**: the signal is
there, strong, on the right channel, and one `BC5602` does not recognize the
preamble of another `BC5602`.

Everything that could be swept has been: 84 channels, 3 bitrates, 2 preamble
lengths, 3 address widths, 2 initialization sequences. The remaining
variables — **frequency deviation** and undocumented **modem** settings —
are not accessible by sweeping. They are in the remote's firmware, readable
via `SWD` on `J5`.

## The software reset clears the analog settings (Sep 22, 2026)

`survie` measurement: of the 19 values recommended by Holtek, **15 are reset
to their factory value by a software reset**. Yet `resetRadio()`,
`configForLoopback()`, and `sharedRadioConfig()` all begin with a reset.

Consequence: from when these values were written in `begin()` until Sep 22,
**they were never active during a listening session**. Every sweep of
channels, bitrates, address widths, and preamble lengths ran on a modem at
factory values. Exception: the `modem` sweep, which writes its register
after the configuration.

Fixed: `registerConfigure()` is replayed after every reset, in both
configuration paths, selectable via `holtek 0|1`. The control is printed by
the measurement itself: **18 out of 19** in place (the 19th is the known
anomaly of bank 2 register 0x2D, written as 0x18 and read back as 0x58).

First hunt with the settings active, channels 3 to 7, 125 kbps, 30 s each:
channel 5 has 2433 strong signals out of 145713 (16.7 per mille versus 7
average over the band, and 4.5 at rest) -- the remote is indeed heard -- but
**zero transitions on GIO3**.

## The GIO3 wire is electrically valid (Sep 22, 2026)

`fil` command: we force the pad to drive a level, selector by selector, and
check whether the pin overrides the ESP32's internal pull resistors. All 16
selectors drive the pin to a firm level (0/20 or 20/20 against BOTH pulls),
and **the level changes with the selector**. This validates the entire
chain: SPI write -> pad -> wire -> ESP32 read.

This test requires no radio source, unlike an edge count: it distinguishes a
disconnected wire from an absence of signal, which a zero transition count
cannot do.

Still to be validated functionally: that a selector MOVES when a frame
arrives. This requires the beacon on the second board, currently unplugged.
Selectors 2, 4, 9, and 14 had been seen active -- with the beacon on.

## Three checks that lock down the contradiction (evening of Sep 22, 2026)

**1. Channel 5 really does carry the remote, not Wi-Fi.** Channel 5
(2405 MHz) falls inside Wi-Fi channel 1, 20 MHz wide (2401-2423). A Wi-Fi
transmitter therefore deposits as much energy at 2420 as at 2405; the
remote, 0.43 MHz wide (FCC filing), can only be at one of the two spots.
`discrimine` command, channels 5 / 20 / 78 sampled alternately, rest then
dial phases:

| channel | role | rest | dial | ratio |
|---|---|---|---|---|
| 5 = 2405 MHz | target, inside Wi-Fi 1 | 3.92 0/00 | 16.66 0/00 | **x4.25** |
| 20 = 2420 MHz | Wi-Fi 1 control | 0.96 0/00 | 0.97 0/00 | x1.01 |
| 78 = 2478 MHz | outside Wi-Fi | 1.68 0/00 | 1.84 0/00 | x1.09 |

Instrument first validated against the beacon (known narrow source on
channel 5): x21.68 on channel 5, x1.36 and x0.85 on the other two.

**First version of this measurement: wrong.** It changed channel by writing
only `RFCH` then waiting 1.5 ms. With the beacon on channel 5 alone, all
three channels read 992 per mille -- including a channel 73 MHz away. The
PLL does not retune in 1.5 ms: a full reconfiguration is needed on every
visit.

**2. GIO3S=14 is upstream of the correlator.** Redone with an address wrong
by one byte, beacon on: 624 edges with the correct address, **624 edges
with the wrong one**, while accepted frames drop from 210 to 13. The output
thus reflects preamble detection alone. A zero transition count means "no
preamble recognized," not "address unknown."

Measured yield: **3 edges per recognized frame** (624 edges / 210 frames). A
remote at ~20 frames/s should therefore give ~1200 edges in 20 s.

**3. Preamble shape is exhausted.** The BC5602 derives the preamble's
polarity from the first address bit sent, and the address goes out in
reverse of the table's order: it is the LAST byte of the table that comes
out first. Our probes had therefore only ever tested one polarity. `forme`
command: 2 polarities x 2 lengths x 3 bitrates = 12 configurations, 20 s
each, dial turning, Holtek settings active. **Zero transitions across all
twelve**, traffic control at ~20 per mille versus 3.9 at rest.

**The frequency deviation is not adjustable** and it is already the right
one. The datasheet fixes it by the bitrate: fDEV=160 kHz at 125 and
250 kbps, 250 kHz at 500 kbps. Carson's rule at 125 kbps: 2x160 + 125 =
**445 kHz**, against **430, 434, and 447 kHz** measured in the remote's FCC
filing. It is the same modulation to the kilohertz. The six low bits of
`CFO1`, despite the register's name, are marked "reserved, must be kept
unchanged."

State of the contradiction: channel confirmed, bitrate confirmed by two
independent means, deviation confirmed, preamble exhausted, detector
calibrated, analog settings active -- and still no preamble recognized.

## The RF path is closed, and we know why (Sep 22, 2026, summary)

Full sweep of the **84 channels**, 125 kbps, analog settings active,
detector calibrated, dial turned nonstop: **zero transitions**. The noisiest
channel is 59 (2459 MHz), right in the middle of Wi-Fi 11 -- not the remote.

**Direct mode on reception is dead, confirmed with a strong source.**
Replayed with the beacon and the settings active:

| configuration | OMST | RSSI |
|---|---|---|
| DIR_EN=0, entry via CE register | 5 (RX) | 123 -> **31 dB** |
| DIR_EN=0, entry via strobe 0x8E | 5 (RX) | 118 -> **31 dB** |
| DIR_EN=1, entry via CE register | 2 (Light Sleep) | 127 -> 120 dB |
| DIR_EN=1, OM 0x03 then 0x07 | 4 (TX) | 127 -> 118 dB |

The receiver is perfectly alive in normal mode; `DIR_EN=1` makes it deaf
through every entry method. The datasheet nonetheless claims "TX/RX data
from/to external MCU directly" (line 377): the implementation does not
follow through.

**This chip cannot deliver undecoded bits.** The eight GIO2 selectors output
nothing, and GIO3's yield already said as much: **3 edges per frame**
(624 edges for 210 frames), where a 125 kbps bit stream would give half a
million. GIO3 is an event pulse, not a data stream.

**Consequence.** There is no way to listen to the air without knowing the
address in advance: the packet engine is the only path to the data, and it
refuses to open. Method 1 (mid-frame locking on a pseudo-address drawn from
the payload) cannot work any better either, since the preamble detector
never arms on this signal, even at the start of a frame.

What remains established and does not need to be redone:

- the remote transmits a **narrow source at 2405 MHz** (Wi-Fi check);
- at **125 kbps, fDEV 160 kHz** (Carson vs. FCC filing, to the kilohertz);
- our receiver **works** (31 dB on the beacon, 210 frames decoded);
- the detector is **upstream of the correlator** (624 edges with a wrong
  address);
- channel, bitrate, deviation, polarity, and preamble length are
  **exhausted**.

**Next: reading the AT32F421 firmware via SWD.** It gives both the address
AND the real frame structure, i.e., everything that is missing. J5 pinout:
hole 1 = ground, hole 3 = 0.001 V (SWCLK candidate), the others at 2.49 V.
Series resistors of 220 to 470 ohms on SWDIO/SWCLK, the target running at
2.5 V.

## IMPORTANT CORRECTION: GIO3S=14 is NOT upstream of the correlator

The entry above, "GIO3S=14 is upstream of the correlator," is **wrong**, and
with it the conclusion "the RF path is closed." The check was confounded:
the address called "wrong" (`44 33 22 E2`) differs from the real one
(`44 33 22 E1`) by only **two bits**, and the BC5602's correlator tolerates
a few bit errors. It was therefore still accepting frames, which gave the
illusion of an output independent of the address.

Redone with the upstream project's receive sequence, beacon on:

| receiver address | deviation | GIO3 transitions | frames |
|---|---|---|---|
| `44 33 22 E1` | none | 28,506 | 789 |
| `44 33 22 E2` | 2 bits, first byte on air | 1,806 | 50 |
| `11 22 33 E2` | totally different | **0** | **0** |
| `11 22 33 44` | totally different, opposite polarity | **0** | **0** |

**Consequence.** Every null result from that day -- 84 channels, 3 bitrates,
2 polarities, 2 preamble lengths -- means "we don't have the right address,"
not "the signal is undetectable." The RF path is not closed.

**Methodological lesson.** A negative control must be TRULY negative.
Choosing an address two bits off from the real one meant testing the
correlator's tolerance while believing we were testing its existence.

## The upstream project's receive path (which works)

Taken from `prepare_halo_receive()` in Termina1/benq-screenbar-halo2-esphome.
Two fundamental differences from ours, `amont` command:

1. **No software reset.** Its comment: "Literal Pico lifecycle: no software
   reset during normal initialization. Hidden packet/PID/RF state is
   allowed to continue from hardware POR." Both of our paths began with a
   reset, which clears 15 of the 19 recommended values. Without a reset, the
   ones written by `begin()` survive: the control shows **18 out of 19**.
2. **Passive reception**: CRC disabled (`PKT1=0x00`), auto-ACK disabled
   (`ENAA=0x00`), dynamic payload disabled, static length of 13 bytes.

Measurement: **789 frames in 15 s** on the beacon, exact payload
`DE AD 55 0F A0 3C 01 02 03 04` followed by CRC `C2 BA`. And **28,506 GIO3
transitions, i.e., 36 per frame**, versus 3 per frame with our old path.

Upstream project parameters, identical to what we had deduced by
measurement: address `9C EA BB 86` (4 bytes, **hard-coded**, it's a Halo 2),
channel 5, `DM1 = 0x82` (125 kbps, 4-byte address).

It also writes `XO1` (bank 0, register `0x38`) to `0x15` before VCO
calibration, in its direct-mode transmit path -- the **crystal trim**, which
we have never touched. Default value after reset: `0x10`.

## Why an nRF52840 is needed, not just an ESP32 (Sep 22, 2026)

**The method that actually found the Halo 2's address** was a **HackRF One +
Universal Radio Hacker**, on 2026-04-14, by kuzmin-no (SK2024 on the HA
forum). Its original README says so, and two screenshots in the repo show
the session: 2.405 GHz, 2 MSps, FSK, 16 samples per symbol. The
`find_halo2_address.py` script only shows up 2.5 months later, presented as
a way to do without the SDR, and **no log has ever been published proving
that it works**.

The `Termina1/benq-screenbar-halo2-esphome` project does not solve address
discovery: `RADIO_ADDRESS{0x9C,0xEA,0xBB,0x86}` is hard-coded and the README
refers the user to "capture your own traffic." **The address is specific to
each lamp/remote pair**; only the pairing one (`E2 08 00 B0`) is universal.
Verified: the three known addresses give 0 frames on our Halo 1, on a path
that nonetheless decodes 789 from the beacon.

**The nRF52840 path** (`xf_bc5602.py`): listening at **1 Mbps** to a signal
transmitted at 125 kbps oversamples each bit by **8**. The sync word is then
pointed at the **oversampled preamble** -- `0xAA` at 125 kbps becomes
`FF 00 FF 00` at 1 Mbps, a universal pattern, identical on every unit --
instead of the address we don't know. The decoder decimates by 8, searches
for the position where the CRC comes out right, and reads the address from
the stream:

```
Frame: address(32) + PCF(9) + payload(80) + CRC-16(16)
CRC-16/CCITT (poly 0x1021, init 0xFFFF) over address + PCF + payload
"address": bytes(_byte(bits, o - 41 + 8 * i) for i in range(4))
```

**The BC5602 cannot do this, it has been measured.** Its preamble detector
must arm on an alternation AT THE CONFIGURED BITRATE, ahead of the address
correlator. Beacon at 125 kbps, oversampling receiver, 32-byte payload, CRC
cut off:

| RX bitrate | factor | sync word | frames | control |
|---|---|---|---|---|
| 500 kbps | x4 | `F0 F0 F0 F0` | **0** | 1293/11999 |
| 500 kbps | x4 | `0F 0F 0F 0F` | **0** | 1186/12000 |
| 250 kbps | x2 | `CC CC CC CC` | **0** | 695/12000 |
| 250 kbps | x2 | `33 33 33 33` | **0** | 692/12000 |

And the BC5602 tops out at 500 kbps, i.e., x4 at best. The ESP32-C6,
meanwhile, exposes no raw PHY interface: its radio only does Wi-Fi, BLE, and
802.15.4.

**Conclusion: an nRF52840 board (Seeed XIAO, ~$13) is the cheapest path to
the address.** Check that it carries a ceramic antenna and not a bare u.FL
connector.

## Raw capture with the CC2500, and the first Halo 1 address (Sep 22, 2026)

The CC2500 gives what the BC5602 refuses: a SERIAL MODE where the packet
engine is disconnected. `PKTCTRL0.PKT_FORMAT=01` outputs the demodulated
bits on GDO0 with the recovered bit clock on GDO2, and
`MDMCFG2.SYNC_MODE=000` removes any requirement for a preamble or sync word.
The datasheet plans for exactly this use: "The MCU must then handle preamble
and sync word insertion and detection in software."

Module: 24TRGC5-V4 (GC-02), CC2500 + RFX2402E (PA/LNA), 26 MHz crystal,
u.FL. PARTNUM 0x80, VERSION 0x03. MEASURED truth table of the front-end
stage: `PA_EN=0, RX_EN=1` gives 18 dB more than the other three
combinations.

**The driver is bit-banged**, deliberately: the ESP32-C6 only has one
general-purpose SPI controller, already taken by the BM5602, and it cannot
be re-routed afterward. Backed by measurement, the peripheral returned a
0x00 status byte where bit-banging returned 0x0F on the SAME pins.

**Chain validated end to end on the beacon.** A frame read in the raw
stream, by a chip that was given no address at all:

```
FF FF FF C0 2A AA | E1 22 33 44 | DE AD 55 0F A0 3C 01 02 03 04 | C2 BA
     repos          adresse            charge utile                CRC
```

**What has been measured on the remote, and no longer just deduced:**

- it transmits on 2405 MHz: strong-signal bins x8.76 when the dial turns, a
  flat floor at rest (`ccpres` command);
- at **125 kbps**: bit duration measured in asynchronous mode, peak at
  8.08 us, versus 8.09 us for the beacon at a known 125 kbps (`ccbit`
  command);
- its carrier is **well centered**: FREQEST gives a mass at 0-31 kHz, versus
  0-15 kHz for the beacon (`ccoff` command);
- it is **strong**: peak at -19 dBm.

**Address found: `8F F7 C1 3C` on air, i.e., `3C C1 F7 8F` in write order.**
Found by searching for repeated sequences within 956,672 bits of raw
stream, with no assumption about CRC or structure whatsoever. Four
occurrences, three preceded by a preamble, all followed by DIFFERENT
payloads:

```
FC 00 55 | 8F F7 C1 3C | 53 13 11 6E 07 CF DF ...
FC 00 55 | 8F F7 C1 3C | 25 89 67 E2 20 BE 7F ...
7F 7C 00 05 | 8F F7 C1 3C | 06 B9 21 BD FF FE ...
77 B6 01 55 | 8F F7 C1 3C | 06 B9 21 BF FF FE ...
```

The preamble is **one byte** (`55`), where our beacon transmits two.

**ADDRESS CONFIRMED** by the BM5602's hardware correlator, with its control:

| address written | duration | GIO3 transitions | frames |
|---|---|---|---|
| `3C C1 F7 8F` | 90 s | 200 | **7** |
| `3C C1 F7 8E` (one bit off) | 40 s | 0 | **0** |
| `8F F7 C1 3C` (reverse order) | 30 s | 0 | **0** |

And cross-corroboration between two radios and two independent analysis
chains: the payload prefix `06 B9 21 B*` appears both in the CC2500's raw
stream (passes 21 and 25: `06 B9 21 BD`, `06 B9 21 BF`) and in a frame
decoded by the BM5602's packet engine (`06 B9 21 BB`).

Seven frames in 90 s is still few: bit errors reject most of them. It is now
a matter of signal-to-noise ratio, no longer of protocol. This is the
harshest judge we have -- it decodes 789 frames from the beacon and
strictly none with a wrong address.

**No CRC model validates these frames**: 84 combinations of polynomial,
initial state, and bit order, over seven starting points. The frames
therefore carry bit errors, which also explains their rarity.

**Methods tried and RULED OUT, each by a check against the beacon:**
anchoring on the preamble alternation (mostly pulls out the `0x55`s from the
payload), consensus over active regions (37 unanimous bits on the beacon
too, hence worthless), and any capture TRIGGERED on RSSI -- reading the RSSI
costs 190 us when preamble and address together only last 384: the address
has gone by before we sample. Only the CRC hunt and the search for repeated
sequences survived their check.

## Halo 1 frame structure, confirmed without the CRC (Sep 22, 2026, night)

Analysis of the BM5602's raw 32-byte captures gives an INDEPENDENT
confirmation of the CRC: the address of the next retransmission is
systematically found at bit **72 or 73** after that of the current frame,
twice with **zero wrong bits**. Now, 72 = 48 + 16 + 8:

```
| adresse 32 b | charge utile 48 b | CRC 16 b | preambule 8 b | adresse suivante...
```

That is six bytes of payload, two of CRC, one preamble byte. This matches
exactly the only frame whose CRC validated (`06 B9 21 BB 98 FF` + `7A FF`)
and confirms that the Halo 1 uses six bytes where the Halo 2 uses ten.

**Hypotheses ruled out by measurement:**

- *Bitrate offset.* Fine CC2500 sweep, DRATE_M from 46 to 72, i.e., 119.8 to
  130.1 kbit/s in 0.32% steps: address detections are spread evenly across
  the whole range, **with no peak**, and no CRC validates anywhere. A
  bitrate offset would have produced a clear maximum.
- *Distance.* The two modules are 25 cm apart: the remote was already close
  to the BM5602.
- *Majority vote.* Moot as things stand: the bursts only deliver one or two
  copies, never the three that a vote requires.

**Anomaly to revisit as a priority.** The same command gives
`06 B9 21 BB 98 FF` + `7A FF` (CRC VALID) in a 13-byte read, and
`06 B9 21 BB FF 3F` + `7F 9B` in a 32-byte read. The first four bytes agree,
the rest do not. **Changing RXPW0 changes the content received**, which
should not happen and probably explains the low yield. Resume with
`RXPW0 = 8`, the value that produced the only valid frame.

**The interrupt watchdog** was tripping during captures: 32768 bits at
125 kbit/s make 260 ms of masked interrupts against a 300 ms threshold. All
captures now go through `ccSampleBits`, which masks in chunks of 8192 bits.

## Two frame families: commands and receive acknowledgements (Sep 23, 2026)

The payload's first byte sorts into two families, and within each one bits
3-2 form a **2-bit sequence counter**:

```
famille A : 02 06 0A 0E          = 0000 PP 10
famille B : 21 25 29 2D, puis 89 = 0010 PP 01
```

In the 32-byte captures, a B frame is **always** followed, 72 bits later, by
an A frame carrying **the same counter**: 21->02, 25->06, 29->0A, 2D->0E.
These are question-answer pairs, in the order B then A.

Interpretation adopted, based on three converging clues:

- **the order**: B precedes A;
- **the content**: in A frames, bytes 2-3 depend only on the counter (`06`
  -> `B9 21`, `0A` -> `78 AC`, `02` -> `F9 A5`, `0E` -> `38 28`), like an
  acknowledgement; in B frames, byte 2 is always `89` and byte 3 varies with
  each capture, like a dial value;
- **reception quality**: A frames are received markedly better -- the only
  frame with a valid CRC that evening was an A -- which points to two
  different transmitters.

So **B = command from the remote, A = acknowledgement from the lamp**. This
is the OPPOSITE of the Halo 2 convention ("odd PID frames are lamp
replies").

**Measured consequence.** The replayed frame `06 B9 21 BB 98 FF` (verified
bit for bit by the CC2500, CRC `7A FF`) produced no reaction from the lamp,
set to minimum for the occasion: it was a receive acknowledgement, not a
command.

**Repair via the bias: no result.** Since the errors were 100% ones read as
zero, we tried to repair 49 captures by setting up to three zeros back to
one. Three frames pass the CRC, but each at the maximum number of
corrections, with no repetition, and chance predicts about four over this
volume: false positives.

**Correction of an earlier conclusion.** The hardware CRC's rejection of
every frame was not due to a different model: during this session, no frame
passed the software CRC either. On the beacon, the hardware CRC produces
exactly our model, and we now use it on transmission.

**Reception hypotheses eliminated by measurement** (reference A frame or
frame count, alternating when possible): read length, distance and
saturation, crystal trim (calibrated: about 3 kHz per notch, 87 kHz range),
default versus recommended analog values.

**What is blocking now**: obtaining an exact B frame. Our radios decode our
own transmissions cleanly and the lamp's fairly well; it is the remote's
signal that they digest poorly.

## First reading of the payload (listening to the remote, Sep 23)

Capture `logs/ecoute-tele2.log` (tool `ecoute 4FF0FD63 5`), gestures:
minimum, maximum, one notch down, turn off/on, temperature, other buttons
(no pauses). Provisional reading, to be confirmed one by one via `txack`:

| payload | observed | provisional reading |
|---|---|---|
| `C4 xx` | xx sweeps `4C` (minimum held) to `FE` (maximum held) with the dial | brightness, lamp on |
| `44 xx` / `C4 xx` alternating, same xx | during turn off/on | bit 7 of the 1st byte = on/off? |
| `C2 xx` | xx sweeps `00` to `64` (0 to 100) | color temperature as a percentage? |
| `83 xx`, `C3 xx` | same xx as `C2` | variants of `C2` (on/off?) |
| `FF 00`, `FE 00`, `FD 00` | at the start and between gesture groups | wake-up / state? |
| `E0 01`, `E0 02`, `FA A8`, `83 35`, `85 A7`, `91 00`, `89 58`, `89 E0` | "other buttons" | unknown |

**Lamp switch button** (capture `logs/btn-switch.log`, repeated presses; the
lamp cycles front -> back -> both -> front). Decoded sequence: `C3 35`,
`C2 35`, `C2 35` (after a wake-up `FF 00 FD 00 FF 00`), `83 35`, `C3 35` --
each state emitted 3 times. The order C3 -> C2 -> 83 -> C3 is indeed the
cycle both -> front -> back -> both. The 2nd byte (`35` = 53) does not move:
it is the current color temperature, sent back along with the mode.

| 1st byte | bits | lamp |
|---|---|---|
| `C2` | `1100 0010` | front only |
| `83` | `1000 0011` | back only |
| `C3` | `1100 0011` | both |

Reading of the 1st byte as a bit field, **to be confirmed by transmission**:
bit 7 = on, bit 6 = front lamp, bit 0 = back lamp, bit 1 = the 2nd byte is
the temperature, bit 2 = the 2nd byte is the brightness. It explains every
value already seen: `C4` (brightness, front), `C5` (brightness, both), `85`
(brightness, back), `44` (off, front), `C2/83/C3` (temperature + mode). The
remote therefore sends ABSOLUTE STATES rather than toggles -- hence the
risk-free resending of the same state (`C2 35` twice).

**Button A and favorite button** (captures `logs/btn-A.log`, `btn-A2.log`,
`btn-A3.log`; capture now timestamps every frame to the millisecond).
Timeline of `btn-A3.log` (wake-up via the favorite button, then button A):

| t (ms) | payload | reading |
|---|---|---|
| 2532-2940 | `FF 00` `FF 00` `FE 00` `FF 00` `FD 00` `FF 00` | wake-up |
| 3043 | `FA A8`, NO_ACK=1 | only frame with no acknowledgement request |
| 3143 | `83 35` | back-only mode, temperature 53 |
| 3244-3345 | `85 A7` x2 | brightness A7, back only |
| 3446-3548 | `91 00` x2 | bit 4, back only |
| 4048-4250 | `89 58`, `89 E0` x2 | bit 3, back only |
| 7588 | `A1 01` | button A, counter 1 |
| 10892-11304 | wake-up | |
| 13202-13405 | `A1 02` x2 | button A, counter 2 |
| 16445-16824 | wake-up | |
| 17042-17244 | `A1 03` x3 | button A, counter 3 |

The 3143-4250 block (favorite) replays a complete state: mode, brightness,
then two still-unknown settings (bits 3 and 4). This is exactly the "other
buttons" list from the first capture. Button A emits `E0 nn` when
front-only is active, `A1 nn` in back-only, `60 01` once (bit 7 at zero):
the top and bottom of the byte follow the lamp mode, bit 5 designates
button A. The 2nd byte counts successive presses on A (01, 02, 03... up to
05 seen) and resets to 01 after a pause or another command. User's
observation on `btn-A3.log`: after the favorite, the lamp did indeed switch
to **back-only** (confirming the reading of `83`, predicted before its
return); **two brief presses** on A, **nothing visible** -- yet three
frames `A1 01/02/03`. A 1-to-1 press/frame mapping is therefore not
established: the remote might emit A on its own (an ongoing automatic
mode?). To be settled by a single press followed by a long silence.

Working reading of the 1st byte (to be confirmed by transmission):

| bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|---|---|---|---|---|---|---|---|---|
| meaning | on | front | button A | setting? | setting? | brightness | temperature | back |

`FF`, `FE`, `FD`, `FA` (all high bits at 1) fall outside this scheme:
service frames (wake-up, announcement).

**Single press on A** (`logs/btn-A4.log`, lamp in front-only, then 21 s
without touching the remote): a single `E0 01` x3 event at 9.8 s, then a
wake-up with no command at 13.1 s, then silence. The remote therefore does
not emit A on its own; the extra frames from earlier tests were double
detections by the touch button. Effect observed: the front lamp **dims then
comes back up**, the temperature also seems to move (uncertain). Reading:
A = toggle of the automatic mode (sensor), 2nd byte = press number so the
lamp ignores repeats.

**Check across all captures** (every `logs/*.log`, 18 distinct 1st-byte
values): `05 42 43 44 60 83 85 89 91 A1 C2 C3 C4 E0` all have exactly one
setting bit among bits 1 to 5, and at least one lamp (bit 6 or bit 0); the
only exceptions are `FA FD FE FF`. The on/off button replays the last state
frame with bit 7 flipped, in every mode: `C4 ED`/`44 ED` (front, after the
dial), `85 A7`/`05 A7` (back), `C3 35`/`43 35` (both), `C2 35`/`42 35`
(front). The favorite was replayed twice in `ecoute-tele2.log`: `83 35`,
`85 A7`, `91 00`, then `89 58` or `89 00`, then `89 E0`.

### Semantics CONFIRMED by transmission (Sep 23, remote without batteries)

Tool: `txack 4FF0FD63 5 <charge> 3 300` (three frames, 300 ms apart, like
the remote). Logs `logs/tx-sem-*.log`. Every line: 3 acknowledgements out of
3, then the user's observation, with the prediction written BEFORE.

| test | payload | prediction | observed |
|---|---|---|---|
| 1 | `C3 35` x1 | both lamps | **nothing** (see below) |
| 1 bis | `C3 35` x3 | both lamps | both lamps |
| 2 | `83 35` | back only | back only |
| 3 | `C2 35` | front only | front only |
| 4a | `C2 00` | one temperature extreme | **the coldest** |
| 4b | `C2 64` | the other extreme | **the warmest** |
| 5a | `42 64` | turn off | off |
| 5b | `C3 35` | turn back on, both, mid temperature | all three at once |
| 6a | `E1 01` (never seen, constructed) | A: dims then comes back up, both stay | as expected |
| 6b | `E1 01` sent again twice | nothing (number already handled) | nothing, twice |
| 6c | `E1 02` | new reaction | dims then comes back up |

Established:
- 1st byte = bit field: b7 on, b6 front, b0 back, b5 button A, b2
  brightness, b1 temperature (b3, b4: favorite, not tested). A value never
  emitted by the remote (`E1`) is understood correctly: the bit-field
  reading is the right one, not a code table.
- State frames are ABSOLUTE: mode, on/off, and temperature take effect
  regardless of the previous state, and several at once in one frame (test
  5b).
- Temperature: `00` = coldest, `64` (100) = warmest.
- Button A: an event, 2nd byte = press number; a number already handled is
  ignored (6b). Visible effect identical for each new number (dims then
  comes back up): toggle or re-trigger of the automatic setting, not
  settled.
- **A single frame was not enough** (test 1), three was. Hypotheses: the
  first frame wakes up the lamp's microcontroller, or the lamp discards a
  frame whose PID equals that of the last one received. To be studied; in
  the meantime, transmit each command three times like the remote.

Brightness with both lamps (`C5`, bits on + front + brightness + back):

| test | payload | observed |
|---|---|---|
| 7 | `C5 60` | slight dimming, both lamps stay on |
| 7b | `C5 4C` | dial minimum |
| 7c/7d | `C5 20`, then `4C`/`20` alternating every 4 s | no visible change |

- The lamp **caps below `4C`**: that is its actual minimum, not just the
  dial's. Usable range `4C`-`FE`.
- Perception (user): the 0 -> 100 curve appears logarithmic; with both
  lamps on, each shines less than alone (shared power). For Matter:
  non-linear conversion of the level to `4C`-`FE`.

Favorite and bits 3 / 4:

| test | payload | observed |
|---|---|---|
| 8 | favorite burst `83 35`, `85 A7`, `91 00`, (`89 E0` uncertain) | back only, stronger brightness: the favorite replays on its own without the remote |
| 9a | `C3 35`, 4 s, `C9 58` | switch to both lamps, then nothing |
| 9b/9c | `C9 E0` / `C9 00` alternating every 4 s | nothing (twice, lamp watched) |
| 10a/10b | `D1 01`, `D1 00`, `D1 64` every 6 s | nothing (twice, lamp watched) |

Bits 3 and 4: **no visible effect**, both lamps on, opposite values.
Internal settings (automatic-mode target or state? favorite memory?), of no
use for commanding from Matter. Not pursued further.

### Halo 1 pairing (Sep 23, frame format now correct)

Procedure: hold **favorite + lamp switch for ~5 s**; all the remote's LEDs
blink. A long press on favorite = save the preset **in the remote** (LED
feedback; the lamp only receives the recall's state frames).

| capture | address, channel | during the procedure |
|---|---|---|
| `logs/pair-1.log` | `63 FD F0 4F`, 5 (normal link) | nothing; normal traffic before and after |
| `logs/pair-2.log` | `E2 08 00 B0`, 5 (Halo 2 pairing) | 0 frames, 0 rejects in 40 s |

- Halo 1 pairing goes through neither the normal link nor the Halo 2 pairing
  address on channel 5: a different address and/or a different channel.
  Only remaining path: CC2500 sweep (energy per channel, then raw capture).
  Not necessary to drive the lamp; left as an option.
- **The address did not change**: right after the procedure, the dial
  transmits on `63 FD F0 4F` (`85 A6` -> `85 FE`, pair-1.log, 36-40 s).
- The `FA xx` service frame (NO_ACK) read `A8` everywhere before, `F8` right
  after the battery swap and the procedure (bits 6 and 4): battery level or
  flag, not settled.

**Halo 1 pairing address FOUND** (Sep 23, CC2500,
`logs/trig-2-appairage.log`, analysis `tools/pairing/ana.py`):
- Energy sweep (`ccscan`): nothing clear-cut outside channel 5; then raw
  capture on 2405 MHz gated on carrier detection (`cctrig`, RELATIVE
  threshold +14 dB: in absolute terms, even +7 dB left 18% carrier at
  rest).
- Control (dial, `trig-1-molette.log`): the blind analysis, without knowing
  the address, produces frames with a correct CRC on `63 FD F0 4F` /
  `C4 D3`, `C4 CB`.
- During the favorite + switch procedure: standard frames, **125 kbps,
  channel 5, address on air `59 01 00 B0`** (to be written as `B0 00 01 59`),
  length 2, PID 0, NO_ACK 0, correct CRC. Payload cycling, one per burst
  about every 200 ms: `5A 5A` -> `F5 C3` -> `CF 49` -> ... Each payload goes
  out up to 3 times about 1.85 ms apart: these are RETRANSMISSIONS for lack
  of an acknowledgement -- the lamp, not in pairing mode, does not reply.
- Kinship with the Halo 2: pairing address `E2 08 00 B0`, same ending
  `00 B0`.
- `F5 C3 CF 49` (the remote's identifier?) has no simple relationship with
  `63 FD F0 4F` (XOR, inversions, complement, CRC-16 tried). The link
  address may come from the lamp, in its acknowledgement, during a real
  pairing.
- The `FFFF0000` length-0 "frames" seen at 1000 kbps are oversampling
  artifacts (constant stream), to be ignored.

**COMPLETE pairing captured** (Sep 23, manual procedure: lamp unplugged,
switch + favorite for 5 s, sensor covered, USB reconnected within 15 s;
BM5602 on `59 01 00 B0` = `logs/pair-4-bm.log`, raw CC2500 =
`logs/pair-4-cc.log`, listed with
`tools/pairing/frames.py logs/pair-4-cc.log 125 99.84`):

| t (s) | traffic |
|---|---|
| 0-2.2 and 5.7-8.0 | normal link `63 FD F0 4F`: `FF 00`/`FE 00`/`FD 00` about every 49 ms (procedure in progress), then `85 E2` x3 |
| 8.5-14.6 | beacon `59 01 00 B0`: `5A 5A`, `F5 C3`, `CF 49` every 200 ms, 2-3 tries each, PID frozen: no acknowledgement |
| 14.76 | **lamp's first acknowledgement** (USB reconnected): length 0, PID 1, NO_ACK bit at 1 |
| 14.8-17.7 | beacon sent ONCE per payload, PID advancing: every frame is acknowledged (empty acknowledgement, ~0.83 ms later) |
| 17.75 | end: the remote stops (pairing successful) |

- **The lamp's acknowledgement is EMPTY**: the lamp does not assign
  anything. It learns the remote's identity from the beacon (consistent
  with the manual: one remote, several lamps). The link address
  `63 FD F0 4F` is therefore a function of `5A 5A / F5 C3 / CF 49` (or of
  the remote's internal identifier) -- function not identified.
- Practical consequence: the ESP32 can RE-PAIR the lamp to the known
  address by replaying the beacon (`59 01 00 B0`, channel 5, payloads in
  order, acknowledgement requested) during the lamp's pairing window.
- **The address survives pairing** (`logs/pair-5-verif.log`): right after,
  the dial transmits 121 frames on `63 FD F0 4F` and the PID advances on
  every frame (single transmission, hence acknowledged); the lamp obeys.
  Indicators off at the end of the procedure: pairing successful. Pairing
  lead CLOSED.
