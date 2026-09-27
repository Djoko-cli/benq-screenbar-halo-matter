[Français](PISTES-FUTURES.fr.md) · **English**

# Future directions

Ideas noted during the project, outside the scope of the current product. None
have been started unless otherwise noted.

## For the community: a single CC2500 to discover AND drive (Sep 23, 2026)

The product uses a BM5602 (Holtek BC5602), which handles the lamp's whole
frame format in hardware (address, 9-bit header, CRC-16/CCITT, acknowledgement,
retries), but which can only listen on a known address. Each Halo 1 has its
own link address: another owner would have to redo our investigation to find
it.

A **CC2500** (TI) could do both:
- **discover** the address without knowing it: that is exactly what our
  `cctrig` + `tools/pairing/ana.py` tools did (raw capture on the carrier,
  then a search for frames with a correct CRC at any position);
- **drive** the lamp by emulating the BC5602 format in software: sync on
  16 bits of address, the rest of the address, a 9-bit header (shifted by
  one bit), payload and CRC-16/CCITT (init FFFF) built bit by bit;
  acknowledgement, PID, and retries handled by the firmware (TX -> RX switch
  in ~21 us, the lamp's acknowledgement arrives 150-250 us after the frame).

Precedent: the DIY Multiprotocol project already emulates the nRF24 / XN297
family of chips on CC2500 (same 9-bit header, same acknowledgement scheme).

Advantages: a single module for everything, better range (the 24TRGC5-V4
module has an RFX2402E PA/LNA), a very well documented chip. Costs: more
tightly timed radio software, a bigger module with a u.FL antenna. Ideal for
a "consumer" version of the project.

## Halo remote in Apple Home

Expose the buttons of the real remote (A, favorite) as a Matter **Generic
Switch** (stateless buttons): Home could trigger automations on a press,
like with a Hue switch. The driver already hears these frames (A: `E0/E1
nn`; favorite: a burst containing `91`/`89`).

## Fast feedback for "Halo auto" after a tap in the app (pending)

Pending as long as EP4 is disabled (`HALO1_EXPOSE_AUTO 0` since 0.3.0,
decision from Sep 23): to revisit if it is brought back. Home keeps showing
the requested state for ~10 s after a tap in the app, while the board's
reports show up within 1 s. Look for a workaround (a separate task was
proposed on Sep 23).

## The product's LED signature (implemented on Sep 23: src/status_led.*, README "Status LED")

WS2812 (IO8): blinking blue = not paired; slow orange = no Thread; off +
brief white glow every 10 s = OK; green flash = command sent; red x3 = lamp
unreachable; "Identify" from Home = rainbow.

## Re-pairing the lamp from the ESP32

Replay the pairing beacon (`59 01 00 B0`, channel 5, `5A 5A / F5 C3 / CF 49`,
acknowledgement requested) during the lamp's pairing window (unplugged,
sensor covered, plugged back in): the lamp would pick up the address
`63 FD F0 4F` without the remote.

## App's network source: small deferred items (Sep 25-27, 2026)

Found during the phase 2 reviews and on the bench; nothing blocking.

Firmware:
- `udp.terminees` in the `reseau` `ip` block: sessions ended by `json 0`
  still counted in `udp.sessions` (rev 4).
- Other line losses without `n` consumed (the `fin` of `leaveMachine`, events
  when a line is already being formatted): never seen, to be aligned with
  `replyEmit` (2.3).

macOS app:
- Retry without `hello` every ~37 s while the banner says "30 s", and ~7
  console lines per round.
- New key: the app recreates the keychain item, so macOS asks again for
  access for `security` (`halo_udp.py`); `SecItemUpdate` would keep the ACL.
- Keychain reads on the main actor: a keychain prompt freezes the interface
  for as long as it takes to answer it.
- Datagrams dropped by the H1 envelope (bad MAC, replay) not shown.
- No injection seam for the transport in `Pont`: end-to-end network
  scenarios can only be tested on the bench.
- Leftover key fragment from a corrupted response split across several log
  lines (classified as text after two lines).
- `NSLocalNetworkUsageDescription` duplicated (project.yml and
  InfoPlist.xcstrings) with no equality check.
