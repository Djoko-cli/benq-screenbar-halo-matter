[Français](ETUDE-THREAD-COMPAGNON.fr.md) · **English**

# Feasibility study: companion app over Thread (Sep 24, 2026)

**Feasibility study: reaching the Halo bridge over Thread through the Apple border routers**

# 0. Results of step A (Sep 24, 19 h 30 - 20 h): GO

Mac on USB Ethernet only (Wi-Fi off), Tailscale active.

**A1. Mac route**
- With a single interface, still "not in table". The kernel knows `fd77:9e:f4bb::/64` through 6 routers (5 Apple border routers + the Aqara) and marks one installed (`fe80::42a:d4d9:3614:70d3%en18`), which is absent from the table. A single interface is therefore not enough: the stale mark survives.
- Unplugging then replugging the USB adapter itself (interface detach, `nd6_purge` empties the RTI list): the route came back within a few seconds, `fd77:9e:f4bb::/64 fe80::42a:d4d9:3614:70d3%en18 UGc`.
- The author of the initial removal remains unknown. A filtered `route -n monitor` watch is running (scratchpad `routes.log`) to catch its pid at the next loss.

**A2. Ping**: 5/5, RTT 21 to 27 ms, hlim 254 (one hop through the border router). `561F9A6463953778.local` resolves correctly to `fd77:9e:f4bb:0:6c06:6762:45d6:a3f0`, with 2 fabrics (`309BEA1CCA0C1569`, `20A842B5C3C38A0D`). Confirmation by power-cycling the bridge is still to be done.

**A3. UDP**: 5/5 REFUS (refused) on port 40000. Control: the Matter port 5540 (open) gives DELAI (timeout), as expected for an invalid datagram. An unannounced port therefore does get through the Apple border routers: **GO for a v1 without an SRP service** (1st row of the decision table). Risk 2 is lifted.

**New: the OMR is already moving.** At 19 h 33, the Aqara was announcing `omr=fd77:9e:f4bb::/64` and a low RIO for this prefix. At 19 h 55, it announces `omr=fd0d:eec8:5ef:1::/64` and a medium RIO for this one only. The Apple border routers and all the nodes stay on `fd77`. The Aqara appears to have left the Apple partition. If `fd77` really was its prefix, the Apple border routers will publish their own and the bridge's address will change: that is the R5 case under real conditions, and the monitoring will catch it.

**Phase 1 (firmware) written on the evening of Sep 24, validated on the bench on Sep 25 (R1; R2 partly): docs/PROTOCOLE-JSON.md 10.**
- Reviewed by 4 agents (OpenThread concurrency, H1 security, USB non-regression, robustness), then cross-checked by 2 agents; one major bug found and fixed (responses discarded as stale).
- Mac route: the cause is a macOS kernel bug (removed by the kernel when a border router appears unreachable, never reinstated); only a static route holds. The `tools/macos/halo-routes/` system helper (a root launchd daemon) keeps the route (docs/PROTOCOLE-JSON.md 10.1).
- `src/h1_proto.*` (pure, 78 host-side checks with Python vectors), `src/h1_crypto.cpp` (mbedTLS), `src/net_udp.*` (OpenThread socket, RX/TX queues, NVS key), `json_mode.cpp` now has one session per transport (USB + 2 network), allowlist `jsonp::remoteRefusal`, cache of the last 8 responses per session.
- Deliberate deviations from this study: port **5480**; one line = one datagram (1078 bytes at most, 6LoWPAN fragmented), no splitting at 512 (to revisit after R3); discovery by the SRP name (`reseau` block `ip`, `srp.nom`), no `_halo-pont._udp` service in v1; `json cle nouvelle` requires an `id` but not machine mode.
- Bench client: `tools/halo_udp.py` (key over USB, session, commands, `refus` test).
- Remaining: bench tests for phase 2 (macOS app, "Network" source: implemented on Sep 25 on the `source-reseau` branch, see below and docs/PROTOCOLE-JSON.md 10; bench tests still to come), then phase 3 (iOS).

# 1. Verdict: GO, if the experiment in section 2 passes (passed on Sep 24, see section 0)

Nothing rules out the Mac or iPhone -> LAN -> Apple Thread border router -> node path. The firmware already has the necessary building blocks: OpenThread UDP, SRP client, HMAC with hardware SHA. Two points are not proven, and a third is blocking this Mac today. No code before the experiment.

**State of the Mac recorded at 16 h 44 (read-only)**
- `route -n get -inet6 fd77:9e:f4bb::1` still answers "not in table".
- The kernel's RTI list keeps two entries marked installed:
  - `fe80::cf4:afe0:c89a:397c%en0`
  - `fe80::42a:d4d9:3614:70d3%en18`
- `561F9A6463953778.local` resolves to `fd77:9e:f4bb:0:6c06:6762:45d6:a3f0`. This is probably the bridge: the only Thread node at SII/SAI 2000/2000. To be confirmed in A2.

**Key risks, from most to least blocking**

1. **Mac route (blocking today, certain).**
   - The kernel is in an inconsistent state: 2 RTI routes marked installed, none in the table.
   - XNU will not reinstall it on its own, no matter how long you wait.
   - Probable cause, not proven: Wi-Fi and USB Ethernet plugged into the same link.
   - Evening of Sep 24: with a single interface, the stale mark remained. The dual connection is therefore not the direct cause of the stuck state. Detaching the interface (USB adapter unplugged) fixes it.
   - Consequence for the macOS app: it must detect EHOSTUNREACH/ENETDOWN and explain it to the user (remedy: unplug the adapter, turn Wi-Fi off then back on, or restart).
2. **Port filtering in the border router: lifted on Sep 24 (A3 = 5/5 REFUS).**
   - The Thread Group's Best Practices (section 4.1) allow a border router to let in only the traffic the nodes have asked for.
   - All the public evidence concerns ports registered via SRP (Matter 5540, `_hap._udp`).
   - If the border router filters, our own SRP service becomes mandatory.
3. **Republishing of a third-party DNS-SD type by the HomePods and the Apple TV.** Probable based on the mDNSResponder-2881 source code, not proven on the software actually shipped.
4. **Coexistence with CHIP's SRP client (manageable, but could break Matter).**
   - CHIP clears 628 bytes on every service removed.
   - An instance name left bound to an old host causes the whole update to be rejected with YXDOMAIN. Matter is then stuck until the lease expires.
   - CHIP clears everything on every startup.
   - Our own SRP service therefore remains an option, never the baseline.
5. **Radio.** Every datagram leaves as 802.15.4 frames a few cm from the BM5602, and the parent link is weak (parent_rssi -80). MAX_RT to be measured (R3). Datagrams of 512 bytes at most.
6. **OMR prefix not fixed.** The active prefix `fd77:9e:f4bb::/64` belongs to the Aqara Hub M100 (key `omr=`), not to an Apple border router. It can change. Always resolve a name, never hardcode the address. Confirmed on Sep 24 at 19 h 55: the Aqara now announces `fd0d:eec8:5ef:1::/64`, and the Apple border routers keep `fd77`.
7. **Flash.** 143,248 bytes remain free (4.6%). v1 is estimated at 10 to 20 KB (not measured).
8. **Apple side.**
   - Local network privacy requires an Apple Development signature.
   - Keychain sharing with a free team is to be checked.
   - iOS 18 minimum (Synchronization.Mutex).

# 2. Cheapest decisive experiment

## Step A: without flashing (about 15 min, from the Mac)

**A1. Restore the route (for you to do)**
1. Turn off Wi-Fi AND unplug the USB Ethernet, or restart the Mac.
2. Plug in a single interface and wait 3 min (the announcements arrive roughly every 180 s).
3. Run `route -n get -inet6 fd77:9e:f4bb::1` then `python3 /private/tmp/claude-501/rtilist.py`.
   - Expected: `gateway: fe80::...%enX`, and a single entry with `stateflags=0x1`.
4. If it is still "not in table":
   - restart the Mac;
   - as a last resort, a provisional manual route (lost on restart): `sudo route -n add -inet6 -prefixlen 64 fd77:9e:f4bb:: fe80::cf4:afe0:c89a:397c%en0`
   - to undo it: `sudo route -n delete -inet6 -prefixlen 64 fd77:9e:f4bb::`
   - The rest of the test still holds, but this then becomes a macOS flaw to document.
5. After A3, plug the second interface back in and redo step 3. If the route disappears, the dual connection is the cause: the app will flag it as an unsupported configuration.

**A2. Ping and bridge identification in one go**
1. Terminal 1: `ping6 -i 1 fd77:9e:f4bb:0:6c06:6762:45d6:a3f0`
2. Terminal 2: `dns-sd -B _matter._tcp local.`
3. Cut power to the bridge for 10 s, then restore it (no serial command).

Results:
- **It really is the bridge**: the responses stop at power-off, its `_matter` instances (one per fabric) go to Rmv then Add, and the responses come back 20 to 30 s later (pret_ms observed: about 22 s). The IPv6 LAN -> Thread path works.
- **The responses do not stop**: this is not the bridge. Run `dns-sd -L <instance> _matter._tcp local.` on the instances that went to Rmv/Add, then `dns-sd -G v6 <HOTE>.local`.
- **No response even though the route is valid**: try other border routers as the manual gateway (`fe80::cb5:8b5f:9a0a:cf65`, `fe80::c62:3ffd:fab3:fed`, `fe80::8be:d542:2b01:7560`). If none works even though Apple Home does: NO-GO via the border routers, see section 4.

**A3. UDP to some arbitrary closed port (40000, outside OpenThread's ephemeral range)**
- Command: `python3 /private/tmp/claude-501/halo_udp_test.py refus fd77:9e:f4bb:0:6c06:6762:45d6:a3f0 40000 5` (script verified locally).
- **5/5 "REFUS"**: lwIP returned an ICMPv6 "port unreachable", so an unannounced port does get through the border router. GO for a v1 without an SRP service.
- **"DELAI" while A2 responds**: port filtering likely. This is not conclusive by itself, since the ICMP error can get lost. Step B settles it.
- **"ERREUR 65"**: the route is lost, go back to A1.

## Step B: bench firmware probe

To be done only if A3 did not give "REFUS", or to validate SRP and the radio. You are the one flashing over USB. Size: 2 to 4 KB, under `#if MATTER_NET_THREAD`, human commands only.

**What the probe does**
- `udp test <port>`:
  - opens an otUdp socket bound to `OT_NETIF_THREAD_INTERNAL`, on a fixed port below 49152 (proposed: 5480, to be finalized);
  - the receive callback, which runs inside ot_task, only copies the message into a 2-slot queue;
  - the loop task sends the echo back under `otLockTry(0)`.
- `udp etat` displays:
  - the OMR and ML-EID addresses;
  - the SRP host name (`otSrpClientGetHostInfo()->mName`);
  - the state of our service;
  - the rx, tx, loss, and error counters, and the minimum of free OT buffers.
- `udp srp on|off` adds or removes `_halo-pont._udp`:
  - instance name derived from the SRP host name;
  - service placed in an aligned 768-byte static zone;
  - added only if the host and at least one CHIP service are Registered;
  - if the service stays pending for more than 60 s: `otSrpClientClearService`.
- `udp stop` closes everything.

**B1. Echo without SRP**
- On the bridge: `udp test 5480`.
- On the Mac: `python3 /private/tmp/claude-501/halo_udp_test.py echo <OMR> 5480 16 256 512 1024 1232` (100 sends per size, 1 per second).
- **Echoes received**: any arbitrary port gets through (confirms A3). Note the losses and the RTT per size.
- **Nothing, and `udp etat` shows rx=0**: the border router filters, move on to B2.
- **rx increases but no echo**: source-address problem on the way back (check `mSockAddr`).

**B2. SRP service**
- On the bridge: `udp srp on`. `json reseau` should show the host Registered and 3 out of 3 services registered.
- On the Mac: `dns-sd -B _halo-pont._udp local.` then `dns-sd -L Halo-... _halo-pont._udp local.`
- **Instance visible, port 5480, host `<HOTE>.local`**: the border routers do republish a third-party type, DNS-SD discovery is possible.
- **Registered (3/3) but invisible from the Mac**: the border routers do not republish this type, fall back to F1 or F2 (section 4).
- **Stays in Adding, with "SRP update error" in `chiplog`**: the server rejects the update. Run `udp srp off` right away and check that Apple Home still responds.
- Then redo B1 with the service registered: if the echo only gets through with the service, the SRP service is mandatory.

**B3. Robustness**
- Restart the bridge: our service re-registers on its own in under 60 s, and the Matter services stay intact.
- `udp srp off` does not cause a crash.
- Apple Home stays responsive throughout the test.

**B4. Radio (test R3)**: 512-byte echo once a second for 10 min, then 10 min with no echo. Compare `compteurs.pilote.tx.max_rt` against the lamp's deliveries.

**B5. iPhone (optional here)**: a test build with a UDP `NWConnection` to `<OMR>:5480`, on a real iPhone (the simulator does not handle the local network permission).

## Decision

| Result | Decision |
|---|---|
| A1, A2 OK and A3 "REFUS" (or B1 OK) | GO. v1 with discovery by the host name learned over USB; SRP service optional |
| B1 fails without SRP but passes with it | GO. SRP service mandatory, risks 3 and 4 to be managed |
| B2 invisible and B1 fails | NO-GO via the Apple border routers, see section 4 |
| A2 fails with a valid route | NO-GO, see section 4 |
| Mac route unstable with two interfaces | GO for the iPhone and for a single-interface Mac; message in the app |
| MAX_RT clearly rising in B4 | GO with a reduced remote profile: longer period, smaller datagrams |

# 3. Implementation (if GO)

## Phase 0: documentation

Fix section 10 before writing any code (details at the end of the section).

## Phase 1: firmware

**1a. USB fields, no risk (under 1 KB)**
- Add to `json reseau` and to `matter`: the SRP host name, the addresses (OMR with their origin), and the UDP port.
- Today, only the SRP state is exposed (matter_bridge.cpp:1532 and 2230).

**1b. `src/net_udp.{h,cpp}`**
- otUdp socket on the fixed port.
- 2-slot receive queue of about 560 bytes.
- Transmission from the loop task under `otLockTry(0)`, one in flight at a time, handling `NULL` and `OT_ERROR_NO_BUFS`.
- Counters, and `udp` added to `kFree` (cli.cpp:359).
- In the receive callback: never a CHIP lock, never `Serial`.

**1c. `src/h1.{h,cpp}`: H1 envelope (section 10.4)**
- SALUT/DEFI (hello/challenge) handshake, 1 provisional session and 2 established.
- Window of 32, 2 DEFI per second total, forgotten after 10 min, cache of the last 8 responses.
- HMAC with `mbedtls_md_hmac` (hardware SHA), constant-time comparison written by us over 16 bytes.
- Shared key in NVS `halo1/cle`, commands `json cle nouvelle|cle|efface` over USB (slot already reserved at json_mode.cpp:916).

**1d. `json_mode`: the biggest piece**
- Today, a single machine session writes directly to `Serial` (json_mode.cpp:98-114, 608, 703).
- It needs:
  - one output per session (USB, and 2 network sessions);
  - the remote profile (10.6);
  - the 10.5 allowlist enforced by the board, which is authoritative (`PolitiqueCommandes` on the app side is only a convenience);
  - splitting messages into at most 512 bytes.

**1e. `_halo-pont._udp` service (only if B2 passes)**
- Can be disabled, with the same guards as B2: 768-byte zone, 60 s guard, instance name derived from the host name.
- Re-added after every CHIP `_ClearSrpHost`.

**Estimated size (not measured)**
- Flash: 10 to 20 KB out of the 143 KB free.
- RAM: 5 to 7 KB out of about 128 KB of free heap (measured minimum: 91 KB).
- No extra task with otUdp.

## Phase 2: macOS app

Implemented on Sep 25, 2026 on the `source-reseau` branch, merged and then
bench-tested with Majid on Sep 25 (results below and in the app's README;
see also docs/PROTOCOLE-JSON.md 10). Deliberate deviation from the plan
below: no new, separate `HaloReseau` framework; `EnveloppeH1`, `ErreurReseau`,
and `TransportUDP` live inside `HaloProtocole` (`Reseau/`,
`Transport/TransportUDP.swift`), which will become cross-platform in phase 3.

**Initial plan (Sep 24): new `HaloReseau` framework (macOS and iOS)**
- `EnveloppeH1`, with test vectors shared with the firmware.
- `TransportUDP: Transport`, over `NWConnection`.
- `DecouverteHalo`: `<HOTE>.local` and port learned over USB; `NWBrowser` only if B2 passes.
- `TrousseauCle`.
- The skeleton already compiles: `/private/tmp/halo-apple/verif/TransportUDP.swift` (283 lines).

**`Pont`**
- New source `Source.reseau`.
- Reconnection on Mac wake, on network path change, and after a silence (`.rouvrir`).
- Clear messages for EHOSTUNREACH/ENETDOWN ("no IPv6 route to the Thread network") and for `localNetworkDenied`.

**Project**
- Entitlement `com.apple.security.network.client`.
- `NSLocalNetworkUsageDescription`, plus `NSBonjourServices` in a partial Info.plist if the app browses via DNS-SD.
- Apple Development signing.
- Browsing only starts once the user picks the "Network" source, so as not to show the alert to USB users.

**Estimated size**: 800 to 1200 lines of Swift including tests (not measured).

## Phase 3: iOS

- `HaloProtocole` goes cross-platform, and `Pont` moves out of AppKit and IOKit.
- iOS 18 minimum.
- `json 0` and disconnect when going to the background, reconnect on return.
- Cellular excluded.
- Key via the iCloud keychain, or via QR code failing that.

## Phase 4: tests (to be added to section 11)

- **R1**: discovery, by host name then by `_halo-pont._udp`.
- **R2**: replayed message, wrong key, wrong direction, and command outside the allowlist rejected as `interdite`.
- **R3**: MAX_RT with and without a remote client.
- **R4**: unplug the gateway border router and time the recovery.
- **R5**: bridge restart and OMR change, followed by a new resolution.
- **R6**: Mac connected on two interfaces.
- **R7**: local network permission denied, with a clear message.
- **R8**: 1000 commands (losses, RTT, replayed response without re-execution).

## Changes to docs/PROTOCOLE-JSON.md

- **10.1**
  - The "to check" note is resolved: CHIP already attaches the Thread interface to lwIP.
  - Add that the client must have the RIO route to the OMR (same link as the border routers), and that the OMR can come from a third-party border router and can change.
- **10.2**
  - Fixed port below 49152: 52540 is inside OpenThread's ephemeral range (49152 to 65535).
  - otUdp path instead of `sendto` and a dedicated task: `sendto` waits twice on the OT lock with no time limit, while holding the lwIP core lock.
  - Recommended datagram: 512 bytes, 1232 at most. Handle NO_BUFS.
- **10.3**
  - Remove the "MAX_SERVICES=5" argument: it is only the buffer reserve.
  - Basic discovery: SRP host name `<16 hexa>.local` and port, given by `json reseau` over USB and kept by the app along with the key.
  - `_halo-pont._udp` becomes optional, with its guards.
  - `thread.srp.services` moves to 3.
- **10.4**
  - Truncated MAC compared by our own code on both sides: CryptoKit refuses a truncated MAC.
  - Publish a test vector.
- **10.5**: allowlist enforced by the board; add `udp` to the forbidden commands.
- **10.6**: rate limits and sizes to be fixed after R3.
- **Section 5**: new fields `thread.srp.nom`, `thread.omr`, and a `udp` block (port, sessions, rx, tx, pertes, tampons_min).

# 4. Fallbacks

- **F1. The border router routes but does not republish our service.**
  - Discovery by the SRP host name still works; it is the v1 baseline anyway.
  - The name changes after a new commissioning, maybe also after a Thread network change (to be checked). The app rereads it on every USB connection.
- **F2. Matter's operational registration.**
  - The bridge already publishes one `_matter._tcp` instance per fabric, of the form `<CompressedFabricId>-<NodeId>`, and the border routers do republish it.
  - The board would provide it over USB, from the fabric table.
  - The app does a `-L` on this name to get the host then the AAAA, and then talks to our port.
  - Survives a host-name change as long as the fabric exists, without adding anything to SRP. Assumes the border router does not filter ports.
- **F3. IPv6 address entered by hand, or last OMR read over USB.** Troubleshooting only: breaks when the OMR changes.
- **F4. The border router filters ports AND does not republish our service.**
  - No clean path via the Apple border routers: we keep USB.
  - Last option, to be reassessed: the esp32c6supermini env (Matter over Wi-Fi, direct UDP on the LAN).
  - But Thread is then abandoned.
  - And Wi-Fi channel 1 (2402 to 2422 MHz) overlaps the lamp at 2405 MHz: channels 6 to 11 would need to be forced (to be checked on the router).
- **F5. This Mac does not hold the route.** Go through the iPhone (single interface) or a single-interface Mac. The macOS app falls back to USB and explains why.

No file in the repository was modified, nothing was flashed, and no serial port was opened. Test script added: `/private/tmp/claude-501/halo_udp_test.py`.

---

## Skeptical verification

**Counter-review of the five go/no-go hypotheses (Thread via the Apple border routers)**

Read-only research. I did not modify anything in the repository, flash anything, or open any serial port. No packet reached the node: the only UDP attempt failed at `connect` with "No route to host". My working files are in `/private/tmp/claude-501/skeptic/`.

## Summary

| # | Hypothesis | Verdict |
|---|---|---|
| 1 | The Apple border routers pass any UDP traffic between the LAN and Thread | **Uncertain.** The route announcement is confirmed. Passage to ports registered via SRP (5540) is confirmed elsewhere. Nothing proves passage to an unannounced port. |
| 2 | The border routers' proxy republishes our own SRP service | **Probable, not proven.** The source code does not filter by type; the software shipped on the border routers is not verified. |
| 3 | A second SRP service can live alongside CHIP's | **Yes, under conditions.** The dangers are confirmed, and I add one more. The "apple-br" report's claim on this point is refuted. |
| 4 | lwIP sockets work on this build's Thread interface | **Confirmed**, with the caveats from the "firmware" report, all verified. |
| 5 | Local network privacy rules on iOS and macOS | **Confirmed in substance.** However, the "app" report's tests did not check this point. |
| — | This Mac reaches the Thread network today | **No.** This is a stuck state in the Mac's kernel, and waiting will not fix it. |

Two mistakes in the reports:
- **The "app" report targets the wrong node.** `32E1CCA9C1A0F448` announces SII=6000 SAI=1100 SAT=500, like 10 other hosts (other brands), on a single fabric. This name does not even resolve anymore.
- **The node in the "apple-br" report is probably the right one.** `561F9A6463953778` → `fd77:9e:f4bb:0:6c06:6762:45d6:a3f0` is the only Thread node announcing the sdkconfig's 2000/2000 values. The other host at 2000/2000 is the Aqara hub itself: `54EF448D15E50000.local`, port 5552, T=6, a separate fabric. This is to be confirmed over USB; it is not proven. Script: `/private/tmp/claude-501/skeptic/txt.py`.

## The Mac: why the route is missing, and why waiting is not enough

- Checked again at 16 h: `route -n get -inet6 fd77:9e:f4bb::1` → "not in table". `ping6` and UDP `connect` → EHOSTUNREACH.
- **Two entries marked "installed" at the same time.** The kernel's list of announced routes (`rtilist.py`) has two entries at `stateflags=0x1`, with no "scoped" mark: `fe80::cf4:afe0:c89a:397c%en0` and `fe80::42a:d4d9:3614:70d3%en18`.
- **The kernel keeps only one route per prefix.** XNU installs these routes without tying them to an interface (`nd6_rtr.c`, `defrouter_select`: "XXX For now we treat RTI routes as un-scoped"). Two marks at the same time are therefore inconsistent. `defrouter_select` even logs this case as "this should not happen" ("more than one" router installed).
- **The kernel will never reinstall it on its own.** `defrouter_addreq` bails out on "already installed" as soon as the mark is set, and only sets it after a successful `rtrequest`. The route was therefore removed afterward without the mark being cleared. The announcements refreshed roughly every 180 s will not bring it back: "unplug en18 and wait 3 min" is not enough, because the en0 entry keeps its mark.
- 200 s of listening to `route -n monitor`: no fd77 event.
- **Decisive test (for you to do):**
  1. Turn off Wi‑Fi **and** unplug the USB Ethernet (or restart), then plug in a single interface.
  2. Check that `route -n get -inet6 fd77:9e:f4bb::1` gives a `fe80::…%enX` gateway.
  3. Plug the second interface back in and rerun this test. If the route disappears, the dual connection on the same network is the cause. It will then need to be flagged as a configuration not supported by the macOS app.

## 1. Passage of any UDP traffic through the Apple border routers: uncertain

**Confirmed:**
- The border routers announce the route: `fd77:9e:f4bb::/64`, 5 Apple border routers × 2 interfaces at medium preference, plus the Aqara at low, lifetime 1800 s.
- XNU's support for these announcements is noted as "since xnu-7195" (macOS 11 / iOS 14) in a third-party table.

**What weakens the hypothesis:**
- The "Thread Border Router Best Practices" guide (Thread Group, §4.1) says: "MUST ensure that only ingress traffic enters a Thread network that Thread hosts have expressed interest in". A border router can therefore legitimately let in only the ports announced via SRP.
- All the public evidence of passage through an Apple border router concerns ports registered via SRP: Matter 5540 (python-matter-server) and `_hap._udp` (HomeKit Controller).
- On this network, nothing shows any other port: 57 of the 58 `_matter._tcp` services are on 5540 (the 58th is the Aqara, on the local network), and there is no `_hap._udp` at all (`hap.py`).

**Test without flashing anything, as soon as the route exists:**
- lwIP replies "port unreachable" (ICMPv6) on a closed port. `udp_input` calls `icmp6_dest_unreach` at 0x4204ffea in firmware.elf, and OpenThread hands lwIP any port it does not use itself (`ip6.cpp:986`).
- Command:
  ```
  python3 -c "import socket;s=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM);s.settimeout(3);s.connect(('fd77:9e:f4bb:0:6c06:6762:45d6:a3f0',40000));s.send(b'x');s.recv(9)"
  ```
- Reading the result:
  - `ConnectionRefusedError`: any arbitrary port gets through.
  - Timeout exceeded while `ping6` responds (lwIP answers pings, echo mode `RLOC_ALOC_ONLY`): the border router filters by port.
- **If the border router filters**, the SRP service becomes mandatory, which overturns the v1 recommendation to "resolve `<hôte>.local`".

## 2. Republishing of our own SRP service: probable, not proven

- **Source code:** mDNSResponder-2881.0.25 (commit d4658af). `_matter` and `_hap` are only used for statistics (`srp-mdns-proxy.c:358-372`). There is no limit on services per host. The only type-related check concerns the consistency of `_sub` subtypes.
- **Uncertain:** the software shipped on the HomePod and Apple TV may differ. I found no public report of a third-party type being republished by an Apple border router.
- **Test:** impossible without flashing. It requires the firmware probe (`udp srp on`), then `dns-sd -B _halo-pont._udp local.` and `dns-sd -L`.

## 3. A second SRP service alongside CHIP's: yes, under conditions

**Confirmed:**
- CHIP clears 628 bytes on every service removed: `memset(service,0,sizeof(Service))` (hpp:907), `li a2,628` at 0x421022e0. `mService` is indeed the first member (.h:184).
- `_ClearSrpHost` → `otSrpClientRemoveHostAndServices(false,true)` on every startup: our service falls along with CHIP's.
- `MAX_SERVICES=5` only sizes the buffers (`OPENTHREAD_CONFIG_SRP_CLIENT_BUFFERS_MAX_SERVICES`), it is not a limit.

**Refuted (the "apple-br" report):** `_InvalidateAllSrpServices` and `_RemoveInvalidSrpServices` only walk `mSrpClient.mServices`, CHIP's own array. They do not remove our service.

**New risk, found reading Apple's code:**
- The server rejects the entire update (YXDOMAIN) if an instance name already points to a different host name (`srp-mdns-proxy.c`, `compare_instance`, lines 3309-3352).
- An SRP update carries the host and all its services. A fixed name `Halo-XXXXXX` left under the old host would therefore also block the Matter services until the old lease expires. Typical case: a full erase via esptool followed by a new commissioning, without the deregistration that CHIP sends during a genuine factory reset.
- **Countermeasures:** the "60 s in ToAdd/Adding → `otSrpClientClearService`" watchdog proposed by the "firmware" report becomes mandatory. And the instance name must derive from the SRP host name.
- This is one more reason to make v1 without our own service, pending test 1.

## 4. lwIP sockets on this build's Thread interface: confirmed

- `OpenthreadLauncher.cpp.obj` imports `esp_netif_new`, `esp_netif_attach`, and `esp_openthread_netif_glue_init`.
- `UDPEndPointImplLwIP::SendMsgImpl` is linked, and Matter works through this path.
- **Blocking:** `openthread_netif_transmit` (0x4218b544) and the source-address selection hook (0x4218b3f8) both take the OpenThread lock with no time limit (`li a0,-1`).
- **Filter:** `IsPortInUse` (`ip6.cpp:986`) prevents lwIP from seeing a port held by OpenThread.
- **Port 52540:** it is inside OpenThread's ephemeral range (49152–65535, `udp6.hpp:654`), and `GetEphemeralPort` only skips the reserved ports. A fixed port under 49152 is needed, whichever path is used.
- **Flash:** 3,002,480 bytes for 0x300000, that is, 143,248 bytes free. Confirmed.

## 5. Local network privacy: confirmed, but not tested by the "app" report

**Confirmed (TN3179):**
- A unicast send to an address reached via a router is not "local network": "Traffic to a local network address goes directly; it's not forwarded by a router".
- But "Resolving a local DNS name" (`.local`), as well as Bonjour browsing and resolution, require permission. The alert is therefore unavoidable for both discovery methods.
- `NSBonjourServices` is only needed for browsing.
- Signing with an identity issued by Apple is required.

**The "app" report's tests do not cover this point:**
- These are `sbtest` executables signed ad hoc, without `NSLocalNetworkUsageDescription`.
- Yet TN3179 automatically allows "Command-line tools run from Terminal or over SSH, including any child processes".
- Only the sandbox part is proven: `network.client` is necessary and sufficient for a connected UDP socket.

**Test:** the real app, signed Apple Development, launched from the Finder. Check the alert, then the denial case (`.waiting` with `localNetworkDenied`). Redo on a real iPhone (the simulator does not handle this permission).

**The Sep 25 bench session (macOS 27, app signed Apple Development):**
- The alert does appear on the first network connection.
- Denial: neither `localNetworkDenied` nor `PolicyDenied`. Resolving `<nom>.local` returns `NoSuchRecord` (-65554) in 6 to 12 ms, path `satisfied`. A missing `.local` name, on the other hand, returns nothing (12 s with no answer via `dns-sd`): the app therefore reads this fast `NoSuchRecord` on a `.local` as a denial.
- A session already open continues after the denial: the flow routed to the bridge's ULA is not cut off. Likewise, an open `NWConnection` flow survives the IPv6 route being withdrawn (Network.framework keeps its next hop; `lsof` sees no socket).
- Re-authorization: the pending connection resumes on its own, ready 20 ms later.
- With no route, a new connection: `ENETDOWN` for `NWConnection`, `EHOSTUNREACH` for a plain socket; the pending connection does not resume when the route comes back (no path event).

## Identifying the node without flashing

Cut then restore power to the bridge (not via the serial port) while `dns-sd -B _matter._tcp local.` is running.

At startup, CHIP removes then re-registers its services (`_ClearSrpHost`). The bridge's two instances should therefore appear as "Rmv" then "Add". `dns-sd -L` on either of them will then give the host.

## Sources
- TN3179: https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy
- Thread Group, BR Best Practices: https://www.threadgroup.org/Portals/0/documents/support/ThreadBorderRouterBestPractices_2530_1.pdf
- mDNSResponder: https://github.com/apple-oss-distributions/mDNSResponder
- XNU nd6_rtr.c: https://github.com/apple-oss-distributions/xnu/blob/main/bsd/netinet6/nd6_rtr.c
- RFC 4191 support table: https://github.com/dxdxdt/gists/blob/master/writeups/ipv6/rfc4191/rfc4191.md
- HomeKit Controller: https://www.home-assistant.io/integrations/homekit_controller/
- Home Assistant forum: https://community.home-assistant.io/t/matter-over-thread-devices-periodically-go-unavailable-host-has-no-ipv6-route-to-thread-subnet-homepod-mini-tbr-apple-home-unaffected/1011614

Files in `/private/tmp/claude-501/skeptic/`: `txt.py`, `hap.py`, `routemon.txt`, `tn3179.txt`, `brbp.txt`.
