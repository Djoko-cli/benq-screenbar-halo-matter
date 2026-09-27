[Français](BRIEF-BOITIER.fr.md) · **English**

# Hand-off brief: 3D-printed case for the Halo module (ESP32-C6 SuperMini + BM5602)

> Recipient: a Claude (Opus) instance tasked with modeling the case and writing
> the soldering and assembly instructions. Written on Sep 23, 2026 by the instance
> that developed the firmware. Contact: Majid (French speaker, comfortable with
> electronics; he long preferred to avoid soldering; he now accepts clean,
> permanent soldering for the final version).

## 1. Context in three lines

- The module drives a BenQ ScreenBar Halo (1st gen) lamp over 2.4 GHz radio
  (channel 5 = 2405 MHz, via the Holtek BM5602-60-1 transceiver) and exposes it to
  Apple Home over **Matter over Thread** (the ESP32-C6's 802.15.4 radio, channel 25 = 2475 MHz).
- The firmware is finished and validated on the real lamp (repository: this folder; build
  `esp32c6thread` in `platformio.ini`). **No pin may change**: they are
  frozen in the `build_flags`.
- The goal of your mission: a **small, compact case, screwed under the desk, powered
  over USB-C**, plus the **final soldering and assembly instructions**, with the
  BM5602 mounted as a "HAT" above the ESP32.

## 2. The two boards

**ESP32-C6 SuperMini** (Majid's board, 4 MB flash):
- approximately 22.5 x 18 mm (**to be measured**: exact dimensions, PCB thickness, height of
  the components on top and bottom);
- USB-C on one short side, **ceramic/PCB antenna on the other short side** (to
  be confirmed on the real board);
- **BOOT (IO9)** and **RESET** buttons near the USB; firmware status LED:
  the **WS2812 on IO8** (the simple LED on IO15 stays off);
- left outer header, 2.54 mm pitch, in this order:
  `6 · 14 · 15 · 18 · 19 · 20 · 3V3 · GND · 5V`.
  **IO21 and IO22 are inner holes**: do not use them.

**BM5602-60-1** (Holtek module, integrated printed antenna):
- dimensions **to be measured** (no reliable datasheet);
- no silkscreen. Antenna at the top, `BM5602-60-1 V1.0` text readable: the
  9 pads on the bottom edge are, left to right,
  `VSS · VDD · GIO1 · CSN · SCK · GIO2 · SDIO · GIO3 · GIO4`.
  Two isolated pads near the antenna, on the left and right, are extra VSS
  pads (not needed).
- **3.3 V only**: never connect it to 5 V.

## 3. FINAL pinout (six wires, nothing else)

| BM5602 (pad #) | Signal | ESP32-C6 SuperMini |
|---|---|---|
| VSS (1) | ground | **GND** |
| VDD (2) | 3.3 V | **3V3** |
| CSN (4) | chip select | **IO14** |
| SCK (5) | SPI clock | **IO18** |
| GIO2 (6) | MISO (the firmware switches the module to 4-wire SPI) | **IO19** |
| SDIO (7) | MOSI | **IO20** |

Do **not** wire in the final version: GIO1, GIO3, GIO4 (GIO3 -> IO3 was only
used for diagnostics), IO10 (second module on the bench), the CC2500 wires. The WS2812
(IO8) and IO9 (BOOT) are on the board: nothing to solder, only to make accessible
(section 5).

**Decoupling: OPTIONAL.** All of the project's measurements were made WITHOUT
an added capacitor (0 loss on the bench outside Thread) and the module very
likely has its own decoupling. A **10 uF + 100 nF** ceramic pair as close as possible
to the VDD/VSS pads remains a low-cost good practice; do not present it
as mandatory, and do not attribute to it any unmeasured effect.

## 4. Radio constraints (the most important in the project)

Two 2.4 GHz radios coexist a few millimeters apart: the BM5602 (2405 MHz, must
hear the lamp's weak acknowledgements and the remote) and the ESP32-C6 (Thread
at +20 dBm, 2475 MHz). On the bench, packets to the lamp are lost in episodes
when Thread is active; the cause is not proven, **but keeping the antennas apart
is the only hardware countermeasure**. Rules:

1. **The BM5602's printed antenna must overhang NEITHER the C6's antenna, NOR a
   ground plane, NOR a component.** It must extend past the C6's PCB, into open air.
2. Put the two antennas at **opposite ends** of the case; aim for
   **at least 25-30 mm** between them (more if the case allows), and ideally
   **perpendicular** orientations.
3. Recommended "HAT" layout: BM5602 above the USB half of the C6,
   **rotated 90°** so its antenna extends past a long side of the C6, on the USB
   side, far from both the C6's antenna (the other end) and the metal shell
   of the USB-C connector. Also propose a side-by-side variant if it keeps the
   antennas further apart for a comparable footprint, with a diagram, and let
   Majid choose.
4. **No metal** in the case near the antennas: no brass insert or screw
   within ~10 mm of an antenna, no metallic paint. Solid plastic
   (PETG or PLA), wall 1.6 to 2 mm thick in front of the antennas.
5. Mounting under the desk: a wooden desktop barely gets in the way. **A metal
   desktop or frame does**: mention it in the instructions (mount near the front
   edge, antennas clear of the metal). The lamp sits on the monitor, above the
   desk, at ~1 m.
6. The SPI wires must not run over the antennas.

## 5. Case requirements

- **Compact**: as small as possible while respecting section 4; give the final
  outer dimensions.
- **Mounting under the desk**: two ears with countersunk holes for ~3.5 mm
  wood screws (heads far from the antennas), or keyhole slots; optionally,
  a flat face for double-sided tape.
- **USB-C**: opening sized to the board's connector (actual measurement + 0.3 mm),
  the plug must be able to seat fully; provide a pass-through and a small
  strain relief for the cable. The case must stay **reflashable over USB**.
- **Status LED (WS2812, IO8)** visible: thin window or light guide above
  the WS2812 (not the LED on IO15). Meaning: blinking blue = not yet
  paired, slow orange = no network, off with a brief white glow every
  10 s = all is well (details: README, "Status LED").
- **BOOT button (IO9)** accessible through a pinhole: **short press =
  reboot**, **8 s then release = removal from Matter** (decommission);
  between 2 and 8 s, nothing (details: README, "BOOT button"). Optional RESET
  hole: the BOOT button is enough to reboot.
- Holding the boards without glue on the antennas: cradles, ribs, clips;
  insulating spacer (printed standoff or Kapton) between the C6 and the BM5602.
- Lid with clips or M2 screws (outside the antenna zone). Clearances of 0.2 to 0.3 mm.
- Recommended material: **PETG** (heat resistance under a desk), 0.2 mm layers,
  printed without supports if possible; specify the print orientation.
- Discreet embossed marking ("Halo") and a mounting-orientation reference mark.

## 6. Soldering and assembly instructions to write (for Majid)

Write step-by-step instructions, illustrated if possible, covering at minimum:

1. **Tools and supplies**: fine-tip soldering iron (1-1.5 mm bevel tip), 320-340 °C;
   0.5 mm solder (63/37 leaded is easier, or lead-free at 350 °C); flux; **30 AWG**
   wire (flexible silicone or Kynar wire-wrap) in at least 3 colors; fine
   wire strippers; **Kapton** tape; desoldering braid; multimeter; ESD
   wrist strap or precautions.
2. **Preparation**: remove old wires and bench pins if needed (braid),
   clean with flux, **pre-tin** the pads and wire ends.
3. **Lengths**: wires as short as the layout allows (typically
   3 to 6 cm), with a small service loop; two ground wires make no
   sense (a single VSS is enough).
4. **Recommended order**: capacitors on the BM5602 first if they are used, then the six wires on the
   BM5602 side (fragile pads: brief heat, no pulling), then the C6 side into the
   holes of the outer header (from above or below depending on the chosen
   layout). No soldering on or near the antennas.
5. **Checks before power-up**: continuity of each wire end to end,
   **no 3V3-GND short circuit**, no bridge between neighboring pads
   (magnifier); wires not under tension.
6. **Assembly**: insulating spacer, boards in their cradles, wires routed clear of
   the antennas, closing without pinching the wires.
7. **Tests after assembly** (USB serial console, 115,200 baud):
   - `info` must show `BM5602 : detecte (version puce 0x01000F)`. `ABSENT`
     with `0xFFFFFF`: MISO (GIO2 -> IO19) disconnected; with `0x000000`: power,
     CSN, or SCK/MOSI swapped;
   - `lampe autotest` must reply `ok`; `lampe regs`: `RFCH 05 DM1 82 RT1 73`;
   - `matter`: always `mise en service : faite` and `Thread : role child` (
     soldering does not erase the pairing);
   - real-world test: a command from Apple Home, then `lampe stats`: note the
     packets/acknowledgements ratio and compare it to the reference below.

Performance reference measured on Sep 23 (bench setup, Dupont wires): without
Thread, 0 packets lost out of several hundred; with Thread active, 0 to 18%
of packets unacknowledged depending on the phase, always recovered by
retries. A good case should not do worse; if it does, it is the antenna
layout that needs revisiting.

**Mandatory check before/after the case (Majid's request, Sep 23).** In the
reference setup, the BM5602 hangs **~12-15 cm above the C6** on Dupont
wires, antenna facing up: a gap FAR greater than the 25-30 mm targeted
here. The compact case will therefore bring the antennas closer together.
Protocol:
1. BEFORE disassembly, current setup: `lampe stats raz`, then ~20 commands from
   Apple Home and ~1 min of remote use, then `lampe stats`; note packets,
   acknowledgements, MAX_RT, frames received, and bad CRCs.
2. AFTER assembly in the case: same protocol, same conditions (same
   lamp location, phone in the same spot).
3. If the rate of unacknowledged packets or bad CRCs degrades noticeably:
   **lengthen the case** (more spacing between antennas) rather than making it more
   compact. The power lever is already maxed out: the BM5602 transmits at +6 dBm,
   its maximum (RFTXP_1 = 0xAF, RFTXP_2 = 0x21, table 1 of Holtek application note
   AN0560).

## 7. What you need to ask Majid before modeling

With calipers (in mm, to the tenth) -- **board unplugged, and preferably
with plastic calipers**: on Sep 24, the tip of a metal caliper, magnetized and
left resting on the BM5602's crystal, put the radio into an abnormal state
(no transmission ever completing, noisy reception) for 70 minutes, until the
module was fully reset:
- SuperMini: length, width, PCB thickness, max height of the components
  on top and bottom, position and dimensions of the USB-C, position of the BOOT
  and RESET buttons and the LEDs, position of the antenna area;
- BM5602-60-1: length, width, thickness, position and length of the antenna
  area, position of the row of pads;
- the desk: desktop material (wood, glass, metal?), thickness, whether it has a
  metal frame, intended mounting spot, preferred screw type;
- a top-down photo of each board lying flat, with a ruler.

## 8. Expected deliverables

1. **Parametric** model (OpenSCAD, or CadQuery/build123d) with all the measured
   dimensions at the top of the file and commented.
2. **STL/3MF** files for the bottom and the lid, plus an exploded render of
   the assembly showing the two antennas and the distance between them.
3. The **soldering and assembly instructions** (section 6), in French.
4. The bill of materials (wire, screws, optional capacitors) with part numbers.

Do not touch the firmware or `platformio.ini`. The bench's reference wiring is
described in `docs/WIRING.md`; in case of disagreement, **this brief is the
authority for the final version** (six wires, pins from section 3).
