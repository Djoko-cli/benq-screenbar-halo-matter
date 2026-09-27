[Français](WIRING.fr.md) · **English**

# Wiring

## BM5602-60-1 module → ESP32

The BM5602 starts up in **3-wire** SPI (`SDIO` bidirectional). The firmware
switches the `IO1` register to **4-wire** mode at init: `GIO2` then becomes
the data output, i.e., the MISO on the ESP32 side.

## BM5602-60-1 module pinout

The module **has no silkscreen markings** on its pads. Orient it with the
antenna at the top and the `BM5602-60-1 V1.0` text readable: the 9 pads on
the bottom edge are then, **left to right**:

| # | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|
| | `VSS` | `VDD` | `GIO1` | `CSN` | `SCK` | `GIO2` | `SDIO` | `GIO3` | `GIO4` |
| | GND | 3V3 | — | CS | clock | **MISO** | **MOSI** | — | — |

`GIO1`, `GIO3`, and `GIO4` are not used. The two isolated pads on the left
and right edges, near the antenna, are extra `VSS` pads.

Source: the wiring diagram from the
[Halo 2 project](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration/blob/main/img/connection_diagramm.png).

| BM5602-60-1 | SPI role | **C6 SuperMini** | ESP32-C3 | ESP32-S3 | ESP32 (WROOM) |
|---|---|---|---|---|---|
| `VDD`  | 3.3 V | 3V3 | 3V3 | 3V3 | 3V3 |
| `VSS`  | GND | GND | GND | GND | GND |
| `SCK`  | clock | **IO18** | GPIO 4 | GPIO 12 | GPIO 18 |
| `GIO2` | MISO | **IO19** | GPIO 5 | GPIO 13 | GPIO 19 |
| `SDIO` | MOSI | **IO20** | GPIO 6 | GPIO 11 | GPIO 23 |
| `CSN`  | chip select | **IO14** | GPIO 7 | GPIO 10 | GPIO 5 |

### Why IO14 and IO18–20 on the C6 SuperMini

This board has few truly free pins:

- **IO12 / IO13** carry the native USB — using them cuts off the serial port;
- **IO2, IO4, IO5, IO8, IO9, IO15** are *strapping* pins: their level is
  sampled at reset. A module driving one of them during boot-up can prevent
  the board from booting;
- **IO8** is an addressable WS2812 LED: it is the firmware's status LED;
- **IO15** carries a simple LED, left as an input (so off) by the firmware;
  **IO9** is the BOOT button — the firmware already uses it.

That leaves **IO14** and **IO18–IO20**, all on the **left outer** header,
whose order is `6 · 14 · 15 · 18 · 19 · 20 · 3V3 · GND · 5V`. The six wires,
power included, therefore fit on a single row:

```
IO14 ── CSN
IO15    (simple LED, skipped)
IO18 ── SCK
IO19 ── GIO2
IO20 ── SDIO
3V3  ── VDD
GND  ── VSS
```

IO21 and IO22 also exist, but as **inner holes** rather than on the
castellated edge: painful to solder, to avoid.

The pins are defined by `build_flags` in [platformio.ini](../platformio.ini) —
change them there rather than in the code.

> **3.3 V only.** The BC5602 does not tolerate 5 V. Since the ESP32 is also
> 3.3 V, no level shifter is necessary.

## Points not to overlook

**Decoupling (optional).** A 100 nF **plus** a 10 µF as close as possible to
`VDD` is a low-cost good practice. But the whole project was measured
**without** it (0 loss on the bench outside Thread, Sep 23), and the module
most likely has its own decoupling: nothing has ever shown it to be
necessary.

**2.4 GHz coexistence.** The ESP32 transmits Wi-Fi up to +20 dBm; the BenQ
operates at 2405 MHz, right on Wi-Fi channel 1. Two precautions:

- physically move the module away from the ESP32's antenna (10–15 cm of
  ribbon cable is enough, 1 MHz SPI handles it fine);
- put your Wi-Fi access point on channel 11 (2462 MHz) if you can.

Without that, the BM5602's reception is desensitized by every Wi-Fi
transmission, and the remote's frames fall through the cracks.

**SPI bus length.** 1 MHz by default (`RF_SPI_HZ` in
[src/config.h](../src/config.h)). No need to go higher: a Halo 1 command
carries a 2-byte payload. If you use a long ribbon cable, go down to
500 kHz instead.

**Antenna.** The BM5602-60-1 has a built-in printed antenna. Do not place it
against a ground plane, a shield, or a metal enclosure.

## Verification

At boot, the serial monitor should show:

```
=== BenQ ScreenBar Halo -> Matter ===
firmware 0.2.0
```

then, with `info`:

```
  BM5602        : detecte (version puce 0x......)
```

`ABSENT` means the SPI read returns `0x000000` or `0xFFFFFF`:
- `0xFFFFFF` → MISO is not connected, or connected somewhere other than
  `GIO2`;
- `0x000000` → no power, `CSN` not connected, or SCK/MOSI swapped.
