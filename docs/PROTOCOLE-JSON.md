[Français](PROTOCOLE-JSON.fr.md) · **English**

# Halo bridge JSON protocol (v1)

Specification of the machine protocol between the firmware (ESP32-C6, product
env `esp32c6thread`, bench env `esp32c6diag`) and the supervision application:
first the native SwiftUI macOS app (`apps/macos/`, USB link), later the iOS
app over the network (Thread -> Apple border router -> LAN). The same
protocol serves both: only the transport and the authentication change.

Status: IMPLEMENTED in firmware 0.4.0 (USB transport validated on the bench
on Sep 24; network transport, section 10, validated on the bench on Sep 25:
R1 and part of R2, see 10). The **(to add)** marks date from the
specification: the fields in question now exist.
Every field cites its source in the code (`file : symbol`), so the
implementation invents nothing.

## 0. Decisions in brief

| Topic | Decision |
|---|---|
| Framing | A machine line = RS byte (0x1E) + a compact JSON object in ASCII + LF. At most 1024 bytes, RS and LF included (worst-case budget: 896). Everything else in the stream is human text or logs, displayed as-is by the console. |
| Direction app -> board | Text lines of the existing CLI, prefixed with `id=<n> ` for correlation. No JSON toward the board. |
| Session | `json 1` (machine mode, with a 30 s lease renewed by `json ping`), `json 0`, `json etat`. Nothing is persisted: every boot starts over in human mode. |
| Responses | Every line carrying an `id` gets a `reponse` message (`fin` step, preceded by a `debut` step for historical commands). Lamp state commands carrying an `id` become asynchronous: `reponse` right away, `livraison` afterward. |
| Periodic state | `etat` (1 Hz), `compteurs` (1 Hz), `reseau` (0.2 Hz), each in several one-line blocks. Events (`rx`, `tx`, ...) are clues; the source of truth is the periodic snapshot. |
| Compatibility | Major version `v` in every line; additions without changing `v`; the app ignores unknown fields, types and values. |
| Network (0.4.0, Thread build) | Same messages in UDP datagrams over Thread IPv6 (port 5480, the node's SRP hostname), shared key + HMAC-SHA256 (H1 envelope), allowlist of remote commands (section 10). |

## 1. Vocabulary and principles

- **target** (*consigne*): the state wanted by Matter, the CLI or the app (`Halo1Lamp::target()`).
- **raw state** (*etat cru*): what the driver believes about the lamp (`Halo1Lamp::believed()`).
- **board** (*carte*): the ESP32-C6 and its firmware; **app**: the client (macOS, iOS).
- **machine line** (*ligne machine*): an RS + JSON line (section 2); **text**: any other line.
- **transport**: the USB serial port (v1), UDP over Thread (section 10).

Principles:

1. **The board never waits for the app.** A machine line that does not fit in
   the send buffer is lost and counted, like the driver's traces
   (`Halo1Lamp::trace`, `Halo1Lamp::notice`: `Serial.availableForWrite()`).
2. **A single producer.** All machine lines are formatted and written in the
   loop task (the one running `loop()`, `tick()` and the CLI). The callbacks
   of the CHIP task and of the IDF event task only set counters, as they do
   today (`matter_bridge.cpp : post`, `sSubMux`); the loop task turns them
   into messages.
3. **State is periodic, events are clues.** A lost or corrupted line never
   throws anything off for long: the next snapshot corrects it. The app never
   rebuilds a state by accumulating events.
4. **Nothing new goes out to the lamp** because of the protocol: the app goes
   through the same targets as Matter and the CLI (`Halo1Lamp::request`,
   `resolveMatter`).

## 2. Framing on the shared serial port

### 2.1 What travels on the port

The C6's USB CDC port (USB Serial/JTAG, `HWCDC`) already carries, interleaved:

| Source | Form | Example |
|---|---|---|
| Output of CLI commands | French ASCII text, CRLF lines | `  consigne    : allumee deux lum A5 temp 35 (a livrer : -)` |
| Echo of typed characters, prompt | isolated characters, `> ` without a line ending | `> lampe` |
| Driver and bridge announcements | `[lampe] ...`, `[matter] ...` (`Halo1Lamp::notice`, `trace`, `bridgeLog`) | `[lampe] injoignable : consigne abandonnee` |
| ESP-IDF logs (other tasks) | `E (12345) tag: ...`; secondary USB Serial/JTAG console (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`), default level ERROR, no colors (`CONFIG_LOG_COLORS` absent) | `E (48213) chip[DL]: ...` (swallowed by `quietVprintf` as long as `chiplog` is off) |
| Arduino core logs | `[    1234][E][fichier.cpp:12] fonction(): ...` | |
| Startup banner, ROM | `=== BenQ ScreenBar Halo -> Matter ===`, `ESP-ROM:esp32c6-...` | rarely seen: the port re-enumerates on reboot |

Two different hardware paths write into the same USB FIFO: `HWCDC`'s circular
buffer (4096 bytes, `Serial.setTxBufferSize(4096)` in `setup()`) and IDF's
secondary console, which writes directly into the FIFO from any task. **An
IDF log can therefore land in the middle of a machine line.** Framing does
not prevent this: it makes it detectable.

### 2.2 The machine line

```
RS  JSON  LF
0x1E {"v":1,"t":"etat","n":42,"ms":61234,...} 0x0A
```

Rules (all mandatory for the board):

1. It starts with the **RS byte (0x1E)**, immediately followed by `{"v":`.
   RS never appears in any other output from the board, IDF, the core or the
   ROM (ANSI colors use ESC, 0x1B). The idea comes from JSON text sequences
   (RFC 7464), without adopting their exclusivity: here, text flows between
   machine lines.
2. A single JSON object, **compact** (no whitespace outside strings), on a
   single line, ending with **LF (0x0A)** alone. Never a CR inside the line
   (the CLI's human text ends in CRLF: the app tolerates a trailing CR).
3. **Printable ASCII only** (0x20 to 0x7E) between RS and LF. In strings,
   `"` and `\` are escaped (`\"`, `\\`); any byte outside 0x20..0x7E is
   replaced with `?`. No `\uXXXX` sequence is ever emitted.
4. **Maximum length: 1024 bytes, RS and LF included.** Worst-case budget
   in v1 (all counters at 4294967295, all strings at their maximum size):
   **896 bytes** per message, to leave 128 bytes for the additions in 9.1;
   a message approaching the budget gains a block (`bloc`), never length.
   More than 1024 bytes remains a bug: not emitted, counted
   (`json_trop_longs`, see `etat.sante.sys`).
5. The first four fields are always `v`, `t`, `n`, `ms`, in that order,
   then `bloc` for messages in blocks (`hello`, `etat`, `compteurs`,
   `reseau`). The app must not depend on this (see 2.4), but it helps when
   reading a capture.

Why RS rather than a printable prefix or `{"v":1` at the start of the line:
a machine line can arrive after a leftover bit of text with no line ending
(the tail of a cut-off log, or the `> ` prompt of a human session); with RS,
the app finds it anywhere in the line. RS is invisible in `pio device
monitor` and in a terminal: the line stays readable there.

### 2.3 Sending, board side

- Formatted into a static 1024-byte buffer, then **a single call** to
  `Serial.write(buf, len)`: `HWCDC::write` holds its lock (`tx_lock`) for the
  whole call, so no other Arduino write can interleave.
- Before writing: `Serial.availableForWrite() >= len`, otherwise the line is
  lost and counted (`json_perdus`). Never a partial write, never a wait: with
  `setTxTimeoutMs(1000)`, an `HWCDC::write` against a host that has stopped
  reading can block for up to 20 x 1000 ms (core 3.3.12,
  `max_consec_timeouts`).
- Periodic lines: at most **one per `loop()` pass**, and only if at least
  1024 free bytes remain afterward (room for one event). Otherwise deferred
  to the next pass; lost (counted) after a 500 ms delay. Full snapshots
  (`json 1`, `json etat`, `json hello`) go through this queue: ~4.6 KB at
  worst for `etat`, `compteurs` and `reseau` (~5.4 KB with the Thread
  build's `ip` block), ~6.3 KB with `hello` and `config` (~7.1 KB), more
  than `HWCDC`'s 4096-byte buffer; written in one go, lines would be lost on
  every `json 1`. Each transport has its own queue (one line per pass and
  per transport); on the network, the room for an event is a free datagram
  slot in the send queue, and the allowed delay is 6 s.
- Events: written right away if they fit, otherwise lost (counted).
- The text the protocol itself emits (end of lease, prompt after `json 0`)
  takes the same non-blocking path (3.5, 3.8).
- `n` is incremented for every line **produced**, whether written or lost: a
  gap in `n` signals a loss, either on the board side (`json_perdus`) or on
  the wire (a corrupted line, see 2.4).
- Never any formatting or writing while holding the air guard
  (`Halo1Radio::sendOne`, OpenThread lock held), under an OpenThread or CHIP
  stack lock, or inside a critical section (`sSubMux`, `sInboxMux`:
  `portENTER_CRITICAL` masks interrupts on this single-core C6). `tx` events
  are built after the verdict; `sSubs`, `sResume`, `sRoles` are copied under
  `sSubMux` and then formatted (like `tracePoll`). Bridge lock ordering
  (`matter_bridge.cpp`): the stack's lock is never taken while holding
  OpenThread's (the CHIP task takes OpenThread's while holding its own). A
  single static buffer: no formatting call may call another.

### 2.4 Receiving, app side

Algorithm (on raw bytes: no character decoding before text and JSON have
been separated):

```
buffer += bytes read
if the buffer exceeds 2048 bytes without an LF: flush it as text, count "debordements"
for each line terminated by LF (0x0A):
    strip a trailing CR if present
    i = position of the LAST RS (0x1E) in the line
    if i is absent:
        if the line ends with '}' and follows a corrupted machine line
           (or is the first one read after the app stopped reading for a
           while: Mac sleep, app suspended):
            fragment -> count "fragments", "System logs" filter only
        else:
            text line -> console (UTF-8 decoding with replacement, never a failure)
    else:
        before = line[0..i[  -> if not empty after trimming spaces: text
        json  = line[i+1..]
        if len(json) > 1022, or json does not start with '{"v":',
           or does not end with '}', or is not a valid JSON object,
           or v/t/n are absent or mistyped:
            count "lignes_abimees", ignore (do NOT display it as text)
        else if v is not a handled version: count "versions_inconnues", ignore
        else: dispatch by t (unknown type: ignore)
```

The **last** RS: if a machine line was cut by a log before its LF got
through, what follows it may contain a valid RS.

A cut machine line leaves its tail on the next line, without an RS: an IDF
log writes directly into the FIFO between two 64-byte packets, or
`flushTXBuffer` drops the oldest bytes (the start of a line) once the host
stops reading. This is a fragment, never command text.

Continuity check: for each transport, the app keeps the last `n` received.
`n` that jumps: losses (count `n_nouveau - n_attendu`). `n` that goes
backward: the board restarted (confirm with `boot`, see 3.6) or stale lines
(see 3.1, buffer at opening).

### 2.5 What the app classifies and ignores

Text lines (other than RS):

| Pattern (after stripping ANSI sequences `ESC [ ... letter`) | Class | Handling |
|---|---|---|
| `^[EWIDV] \(\d+\) [^:]+: ` | IDF log | console, "System logs" filter; level = first letter |
| `^\[\s*\d+\]\[[EWIDV]\]\[` | Arduino core log | same |
| `^\[(lampe\|matter)\] ` | firmware announcement | console; in `json log 1` mode they arrive as `log` and no longer pass through here |
| `^ESP-ROM:`, `^rst:0x`, `^boot:0x`, `^=== BenQ ScreenBar Halo` | startup | console, and a restart clue (see 3.6) |
| `^> ?$` | CLI prompt (human mode) | ignored |
| other | command text | console, attached to the command in flight (see 6.4) |

The app draws **no** state information from text: only from machine lines.
Text is only for the console and the log.

### 2.6 Direction app -> board

- Printable ASCII lines (0x20..0x7E), terminated by LF; a CR before LF is
  ignored by the CLI (`cliPoll`: `if (c == '\r') continue`).
- **127 bytes at most**, `id=` prefix included: the CLI's buffer is 128
  bytes (`cli.cpp : buf[128]`) and today truncates SILENTLY beyond that. In
  v1 the board refuses a line that is too long (`reponse` code `trop_long`,
  nothing is executed) **(to add)**.
- Never an RS byte or JSON toward the board.
- Byte 0x15 (Ctrl-U): clears the line currently being typed **(to add)**.
  The app sends it when opening the port (see 3.3) to erase a leftover line
  left by a previous session, which would otherwise get executed.
- `HWCDC`'s receive buffer is 256 bytes (`setRxBufferSize(256)` in
  `HWCDC::begin`): during a blocking command, at most two lines can wait in
  it. Hence the one-command-in-flight rule (6.5). Beyond 256 pending bytes,
  `HWCDC`'s ISR silently drops the excess (`xQueueSendFromISR` refused): a
  line stripped of its LF would run into the next one; the `0x15` at the
  head of every connection attempt (3.3) clears such a leftover.
- In machine mode, the board ignores any received byte outside 0x20..0x7E,
  except LF, CR, 0x15 and backspace **(to add)**, so that
  `Commande inconnue : "%s"` never sends back an RS.

## 3. Session

### 3.1 Opening the port (macOS, native C6 USB)

The C6 exposes USB Serial/JTAG: VID:PID `303A:1001`. The baud rate (115200)
is ignored by this device; the app sets it anyway (a UART-bridge variant,
like `esp32dev`, needs it: see 9).

1. Open `/dev/cu.usbmodem*` (never `/dev/tty.*`, which waits for DCD) with
   `O_RDWR | O_NOCTTY | O_NONBLOCK`, then `ioctl(TIOCEXCL)`: exclusive
   access, `pio device monitor` will not be able to attach to it at the same
   time. Sandboxed macOS app: entitlement `com.apple.security.device.serial`.
2. `cfmakeraw`, 8N1, `CLOCAL | CREAD`, **`HUPCL` cleared**, 115200.
3. **DTR and RTS**: see 3.2. Recommended: both to 0 in a single
   `ioctl(TIOCMSET)` right after opening, and never touched again.
4. Resynchronization: discard everything before the first LF received.
5. **Stale lines**: while the host had stopped reading, `HWCDC` may have
   held up to 4 KB of output (and, with the cable unplugged, it replaces
   the oldest: `flushTXBuffer`). They arrive first. The app timestamps
   everything with `ms` (and `boot`), never with the arrival time, and
   treats whatever precedes the response to its `json 1` as historical.
6. Opening the port **does not restart** the board if the rules in 3.2 are
   followed. However, any board restart (`reboot`, panic, watchdog, button)
   **re-enumerates the USB**: the `/dev/cu.*` node disappears (read errors
   or EOF) then comes back, often under the same name. The app watches for
   `303A:1001` to appear (IOKit, `IOServiceAddMatchingNotification` on
   `IOSerialBSDClient`) and reopens, with a delay of 300 ms then 1 s, 2 s,
   5 s. The device's USB serial number is the chip's MAC address: it lets
   the app tell the same board apart among several (to be confirmed on the
   bench).
7. A flash (`pio run -t upload`) fails as long as the app holds the port
   (`TIOCEXCL`): the app offers "Release Port" (sends `json 0` and closes;
   DTR and RTS are already at 0) and does not reopen before a click.
8. Board in download mode (after a failed flash, or BOOT held): same
   VID:PID, but the ROM only speaks esptool's protocol. No `hello`: the app
   announces it ("no response: board in download mode?").

### 3.2 DTR and RTS: the C6 trap

The USB Serial/JTAG device interprets DTR and RTS the way esptool uses them,
with no wiring at all: per the technical reference manual (USB Serial/JTAG
chapter, table "CDC-ACM Settings with RTS and DTR", to be re-read in its
latest version) and esptool's `USBJTAGSerialReset` sequence:

| RTS | DTR | Effect |
|---|---|---|
| 0 | 0 | none (clears the download-mode flag) |
| 0 | 1 | sets the download-mode flag |
| 1 | 0 | **RESTARTS THE CHIP** (into download mode if the flag is set) |
| 1 | 1 | none |

Consequences for the app:

- **Never the state RTS=1, DTR=0, not even for an instant.** A program that
  changes DTR and RTS through two separate calls (a library's `rts`/`dtr`
  properties, ORSSerialPort for example) can pass through it. Always
  `ioctl(fd, TIOCMSET, &bits)` with both bits at once.
- On close, a driver that lowers DTR before RTS (`HUPCL`) passes through
  RTS=1, DTR=0: this is the "restart on terminal close" reported on the C3
  (esp-idf, issue 13075). Hence: `HUPCL` cleared, and DTR = RTS = 0 from the
  moment of opening (a direct 1,1 -> 0,0 transition), so that closing
  changes nothing.
- `HWCDC` does not gate sending on DTR (connection is judged from SOF
  frames and FIFO polling: `HWCDC::isCDC_Connected`): DTR at 0 does not cut
  the flow.
- The `USB_SERIAL_JTAG_CHIP_RST_REG` register has a `USB_UART_CHIP_RST_DIS`
  bit that disables this restart on the chip side; it would also disable
  esptool's automatic reset (flashing), and an Espressif forum thread says
  it is ineffective on the C6 against DTR. **Not adopted**; only to be
  tried on the bench.

To be confirmed on the bench before relying on it (tests U1 and U2, section
11): 50 open-close cycles of the app and of `pio device monitor`, `boot`
unchanged and `up_s` continuous.

### 3.3 Connection sequence

```
App                                                  Board
opens the port, DTR=RTS=0, discards up to the 1st LF
sends 0x15 0x0A  ----------------------------------> line being typed cleared, empty line ignored
sends "id=1 json 1\n" -----------------------------> machine mode: echo and prompt disabled
                  <--------------------------------- hello (base, identite)
                  <--------------------------------- config
                  <--------------------------------- etat (lampe, tranches, sante)
                  <--------------------------------- compteurs (pilote, radio, matter)
                  <--------------------------------- reseau (thread, abonnements, ip)
                  <--------------------------------- reponse id=1 fin ok
... then etat + compteurs every second, reseau every 5 s, events
sends "id=k json ping\n" after 10 s with no other command
```

The lines that follow `json 1` go through the periodic queue (2.3), one per
`loop()` pass; the `reponse` is sent after the last one.

No `hello` 2 s after `json 1`: resend (3 times). Then:
- text `Commande inconnue : "id=1". Tape 'help'.` received (the old
  firmware reads `id=1` as a command name; pattern
  `^Commande inconnue : "id=\d+"`): firmware without the JSON protocol; the
  app stays console-only and says so ("flash >= 0.4.0");
- nothing at all, or text without `hello`: wrong port, download mode, or a
  bench command running, launched by a previous session; the app keeps
  listening and resends `\x15\n` then `json 1` every 30 s, no more often:
  during a bench command the CLI stops reading, each send piles up in the
  256-byte receive buffer (2.6), and every queued `json 1` would get
  executed on exit (a full snapshot every time).

### 3.4 The `json` command

A new CLI family **(to add)**, in `cli.cpp : handleLine`. Everything is in
RAM (see 3.7). `json` joins the `radioFree` list (`cli.cpp : kFree`), and
the `id=` prefix is stripped BEFORE that test: otherwise every line from
the app (`id=17 ...`, first word `id=17`) and every `json ping` (every 10 s)
would trigger `lamp.settleRadio()` (up to 200 ms) then
`lamp.invalidateRadio()`, i.e. a full BM5602 reconfiguration and
`ChipWatch::forget()`, which clears the run of delays, the 10 s flood
window and the deafness window: automatic L2 restart would be delayed, or
prevented altogether for a flood that takes more than 10 s to reach its
threshold.

| Command | Effect | Bounds |
|---|---|---|
| `json` | session state, in human text | |
| `json 1 [bail <s>]` | switches this transport to machine mode: echo and prompt disabled, session settings reset to their defaults, then `hello`, `config` and the full snapshot through the periodic queue (~6.3 KB at worst), the `reponse` `fin` after the last line. Idempotent: resending `json 1` resynchronizes. | lease 0 (none) or 10..600 s, default 30 |
| `json 0` | back to human mode: `fin` message, then the `> ` prompt; over the network, the session's slot also becomes available to the next client (rev 4, 10.4) | |
| `json etat` | full snapshot: `etat` (3 blocks), `compteurs` (3 blocks), `reseau` (2 blocks, 3 in the Thread build), placed in the periodic queue (one line per `loop()` pass, 1024 free bytes afterward, 2.3); the `reponse` `fin` is sent after the last line. Cumulative worst case ~4.6 KB (~5.4 KB in the Thread build), more than `HWCDC`'s 4096-byte buffer: never in one go. Also works in human mode (once). | |
| `json hello` | `hello` (2 blocks) and `config`, through the same queue. Also works in human mode. | |
| `json ping` | renews the lease; the `reponse` carries `bail_s` and `up_s` | |
| `json periode <ms>` | period of `etat` | 0 (off) or 200..60000, default 1000 |
| `json compteurs <ms>` | period of `compteurs` | 0 or 200..60000, default 1000 |
| `json reseau <ms>` | period of `reseau` | 0 or 1000..60000, default 5000 |
| `json trames 0\|1` | `rx` and `tx` events | default 1 |
| `json log 0\|1` | firmware announcements and traces as `log` messages instead of text | default 0 |
| `json cle [nouvelle <64 hexa>\|efface]` | key for the network transport (section 10.4), USB only; `nouvelle` requires an `id` (the key is sent in the reponse) | Thread build; elsewhere `refuse` |

Settings are per transport: USB and each network subscriber have their own.

### 3.5 Lease and ping

The app can disappear without sending `json 0` (crash, Mac sleep, cable
pulled, `kill`). Without a safeguard, the board would keep transmitting to
nobody, and a human who then opened `pio device monitor` would receive
JSON.

- The board timestamps the last byte received on the transport and the end
  of the last command executed; the lease runs from whichever of the two
  is more recent (a 60 s bench command therefore does not let the lease
  expire when it exits).
- Lease expired: back to human mode, `fin` message (`cause`: `bail`), then
  the text line `json : mode machine coupe (hote muet depuis 30 s)` (host
  silent for 30 s) and the prompt, written through the same non-blocking
  path as machine lines (`availableForWrite()` first, otherwise lost and
  counted): the host is precisely the one being silent, and a
  `Serial.println` would block `loop()` for up to 20 x 1000 ms.
- The app sends `id=<n> json ping` after 10 s with no other command. On the
  bench, a human who types `json 1 bail 0` keeps machine mode until
  `json 0` or a restart.

### 3.6 Heartbeat, silence, restart

- `etat` blocks act as a heartbeat. If `periode_ms` is 0 or more than 2000,
  the board emits an `hb` every 2000 ms.
- **Silence**: no line (machine or text) for 3 x max(period, 2 s), other
  than a command in flight (6.5). The app resends `json 1`; with no
  response within 5 s, it closes and reopens the port.
- **Restart**: `boot` (8 hex digits drawn at boot time, see `hello.boot`
  **(to add)**) changes, or `up_s` goes backward. The app clears its
  derived state, keeps its chart series (a new segment) and resends
  `json 1` if machine mode has dropped back (it always does: 3.7).

### 3.7 Persistence: none

Machine mode and its settings are **not** written to NVS. At boot, the
board is in human mode, with default periods. Reasons:
- on native USB, every restart re-enumerates the port: the app reconnects
  anyway, and resends `json 1` (idempotent);
- a persisted mode would flood `pio device monitor` with JSON after a
  flash or a restart, or during a bench session;
- zero extra flash writes (the driver already avoids writing close to
  transmissions: `kPersistAfterTxMs`).

### 3.8 Echo and prompt

In machine mode, `cliPoll` no longer echoes characters, no longer emits the
newline after Enter or the `> ` prompt, and no longer calls
`Serial.flush()` after the command **(to add)**: `HWCDC::flush()` waits up
to 1000 ms without progress, then sets `connected` to false and empties the
whole send buffer (`flushTXBuffer(NULL, 0)`), machine lines included,
without `json_perdus` ever seeing it. Only historical commands' text stays
blocking. The app displays the command it sent in the console itself.
`json 0` and lease expiry redisplay the prompt (non-blocking). Backspace is
still handled (useless for the app). To be checked on the bench (U5):
without `flush`, no bytes lost between two commands (the `cliPoll` comment
says the C6's USB CDC used to lose some "if commands followed too
closely").

## 4. Envelope and conventions

Fields common to all machine lines:

| Field | Type | Meaning | Source |
|---|---|---|---|
| `v` | integer | major protocol version, 1 | constant (to add) |
| `t` | string | message type (sections 5 and 6) | |
| `n` | integer 0..4294967295 | line number produced on this transport since boot (not reset to 0 by `json 1`) | counter (to add) |
| `ms` | integer 0..4294967295 | `millis()` when the line was produced (wraps to 0 after 49.7 days) | `millis()` |
| `bloc` | string | for `hello`, `etat`, `compteurs`, `reseau`: part of the message | |

Conventions:

- **Integers only**, never a float: a gamma of 2.00 is written
  `gamma_c: 200` (hundredths). Counters are `uint32_t` values that wrap to 0
  after 4294967295.
- **Units in the name**: `_ms`, `_s`, `_us`, `_dbm`, `_kbps`, `_c`
  (hundredths). No suffix: a count of events.
- **Bytes in hex**: uppercase strings without `0x`, 2 digits per byte, in
  on-air order: payload `"C5A5"`, raw frame `"0962D2D86B000000"`, register
  `"2E"`. Matter node identifiers: `"0x"` + 16 digits (they exceed 2^53, the
  limit for safe JSON numbers), as with `nodeText`.
- **Booleans** `true`/`false`; **`null`** = unknown or not applicable; an
  optional field may be absent.
- **Enumerations**: lowercase ASCII strings without accents, `_` as a
  separator. OpenThread roles and states keep OpenThread's own text
  (`"child"`, `"Registered"`).
- **No wall clock**: the board has no notion of time of day. The app
  timestamps on receiving a `hello` (local time <-> `ms`) and derives the
  rest from that.

**State** object (target or raw state), reused by several messages:

| Field | Type | Range | Source (`halo1::State`, `halo1_map.h`) |
|---|---|---|---|
| `marche` | boolean | | `State::power` |
| `lampes` | string | `avant`, `arriere`, `deux` | `State::lamps & F_LAMPS` (`lampsText`) |
| `lum` | integer | 76..254 (0x4C..0xFE), raw as on the air | `State::bright` |
| `niveau` | integer | 4..254, canonical Matter level | `levelFromRaw(bright)` |
| `temp` | integer | 0 (cold)..100 (warm), raw (0x00..0x64) | `State::temp` |
| `mired` | integer | 153..370 | `miredFromTemp(temp)` |

`niveau` is the canonical value; Apple Home may display a nearby level that
yields the same raw brightness (`displayLevel`, stable display E.2).

Target field codes (`a_livrer`, `confirme`, `champs`): `marche`
(`FLD_FLAGS`: marche and lampes), `lum` (`FLD_BRIGHT`), `temp` (`FLD_TEMP`).

## 5. Periodic and session messages (board -> app)

Each snapshot is a series of lines of the same type, one per `bloc`. The app
replaces the values for a (`t`, `bloc`) pair on every reception; the blocks
of a single cycle are not necessarily under the same `ms`.

### 5.1 `hello`

Sent after `json 1`, on `json hello`, and again if a value changes (never
in practice outside `json 1`). Two blocks: as a single piece, the worst
case (957 bytes) exceeded the 896 budget (2.2).

**Block `base`** (worst case 627 bytes):

| Field | Type | Meaning | Source |
|---|---|---|---|
| `rev` | integer | minor protocol revision: 0 in v1.0; 1 = `led` patterns `desappairage` and `redemarrage`, `log` with `src` `bouton` (BOOT button, Sep 24); 2 = network transport (section 10: caps `udp` and `cle`, `session.transport` `udp`, block `reseau` `ip`, `reponse` `cle` and `empreinte`, code `interdite`); 3 = `reseau.thread.matter.code_manuel` and `qr` also once commissioned (USB); 4 = `led.depuis_ms` (block `sante` and event `led`), `json 0` over the network frees its slot (10.4) | `jsonp::kRev` (`json_out.h`) |
| `fw` | string | full firmware version, e.g. `0.4.0-1a2b3c4` | `FW_VERSION_FULL` (`fw_version.h`) |
| `fw_desc` | string | application descriptor version, the one Matter publishes; must equal `fw` | `esp_app_get_description()->version` |
| `date`, `heure` | strings | build date/time | `esp_app_get_description()->date`, `->time` |
| `env` | string | PlatformIO env, e.g. `esp32c6thread` | `FW_ENV` (to add: `tools/git_rev.py`, `env["PIOENV"]`) |
| `build` | string | `produit` or `diag` | `DIAG_ONLY` |
| `reseau_build` | string | `thread`, `wifi`, `aucun` | `MATTER_NET_THREAD`, `DIAG_ONLY` |
| `puce` | string | `esp32c6` | `CONFIG_IDF_TARGET` |
| `idf` | string | e.g. `v5.5.5` | `esp_get_idf_version()` |
| `arduino` | string | e.g. `3.3.12` | `ESP_ARDUINO_VERSION_STR` |
| `boot` | 8-hex string | identifier for this boot | `esp_random()` in `setup()` before `matterBridgeBegin()` (so before `Matter.begin()`), between `bootloader_random_enable()` and `bootloader_random_disable()` (to add): no radio is active yet, and IDF then only guarantees a pseudo-random value |
| `reset` | string | `mise_sous_tension`, `broche`, `logiciel`, `panique`, `chien_int`, `chien_tache`, `chien`, `baisse_tension`, `usb`, `inconnue` | `esp_reset_reason()` (same table as `resetReasonText`) |
| `reset_n` | integer | raw value of `esp_reset_reason()` | same |
| `up_s` | integer | seconds since boot | `esp_timer_get_time() / 1000000` |
| `session` | object | settings in effect: `transport` (`usb`, `udp`), `periode_ms`, `compteurs_ms`, `reseau_ms`, `bail_s`, `trames`, `log` | session (to add) |
| `limites` | object | `ligne_max` (1024), `cmd_max` (127) | |

**Block `identite`** (worst case 467 bytes):

| Field | Type | Meaning | Source |
|---|---|---|---|
| `boot` | | same as block `base` | |
| `mac` | 12-hex string | factory MAC-48 | `esp_read_mac(mac, ESP_MAC_BASE)` |
| `id.fabricant` | string <= 32 | | `MATTER_VENDOR_NAME` |
| `id.produit` | string <= 32 | | `MATTER_PRODUCT_NAME` |
| `id.serie` | string <= 32 | `HALO1-` + MAC | `sSerial` (`matter_bridge.cpp : applyIdentity`); diag build: same calculation, outside the bridge (to add) |
| `id.nom` | string <= 32 | NodeLabel | `MATTER_NODE_LABEL` |
| `id.hw` | integer | | `MATTER_HW_VERSION` |
| `id.hw_txt` | string <= 64 | | `MATTER_HW_VERSION_STRING` |
| `caps` | array of strings | capabilities of this build (see below) | |

v1 capabilities: `matter` (Matter bridge compiled in), `thread` (Matter over
Thread), `garde` (air guard present: `Halo1Radio::hasAirGuard()`), `ep4`
(`HALO1_EXPOSE_AUTO`), `led` (status indicator: `PIN_RGB_STATUS_LED` or a
plain LED, outside the diag build), `lampe_async` (asynchronous `lampe`
commands with `id`, 6.2), `trames` (`rx`/`tx`), `log`, `udp` (network
transport, section 10), `cle` (key management, 10.4). The app adapts to
`caps`, not to the firmware version.

### 5.2 `config`

Slow-changing settings: sent with `hello`, and after any command that
changes one of them (`lampe rafale|ecart|rx|leger|garde|gamma|adresse`,
`matter med|maxint|reprise auto|impulsion`). Worst case: 696 bytes.

| Field | Type | Source |
|---|---|---|
| `lampe.adresse` | 8 hex, write order | `Halo1Lamp::address()` |
| `lampe.air` | 8 hex, on-air order | `Halo1Radio::air()` |
| `lampe.canal` | integer (5) | `halo1::kChannel` |
| `lampe.debit_kbps` | integer (125) | `bc5602::DATARATE_125K` |
| `reglages.paquets`, `accuses_min`, `paquets_max` | integers | `Halo1Lamp::tuning.repeats`, `minAcks`, `maxAttempts` |
| `reglages.ecart_ms`, `reprise_ms`, `reprises` | integers | `tuning.gapMs`, `retryMs`, `planRetries` |
| `reglages.rearm_ms`, `silence_ms` | integers | `Halo1Radio::tuning.rearmMs`, `silenceMs` |
| `reglages.rearm_fort`, `leger`, `garde` | booleans; `garde`: null if `!radio.hasAirGuard()` (diag, Wi-Fi) | `tuning.strongRearm`, `lightSwitch`, `airGuard` |
| `reglages.gamma_c` | integer (200 = 2.00) | `mapGamma()` |
| `seuils.delais_suite` (3), `deluge_trames` (100), `deluge_pct` (90), `fenetre_ms` (10000), `sourd_hors_rx` (1000), `sans_guerison` (3), `ecart_ms` (60000), `repli_ms` (600000) | integers | `ChipWatch::kTimeoutRun`, `kNoiseMinFrames`, `kNoiseBadPct`, `kNoiseWindowMs`, `kDeafMinRearms`, `kFruitless`, `kGapMs`, `kBackoffMs` |
| `matter.endpoints` | object `principal`, `avant`, `arriere`, `auto` (absent without EP4) | `getEndPointId()` of the endpoints (to add) |
| `matter.lampes_en` | `lumieres` or `prises` | `HALO1_SELECTORS_AS_LIGHTS` |
| `matter.mired_min`, `mired_max`, `niveau_plancher` | integers | `kMiredCold`, `kMiredWarm`, `kMatterLevelFloor` |
| `matter.impulsion_ms` | integer (EP4 only) | `matterAutoPulseMs()` |
| `matter.med`, `matter.med_boot` | 0 router, 1 MED from init, 2 MED after `Matter.begin()` | `matterMedMode()`, `sMedBoot` (static: to expose) |
| `matter.maxint_s` | integer (0 = the controller's) | `matterMaxIntervalCap()` |
| `matter.reprise_auto` | boolean | `matterResumeAuto()` |

The `seuils` are used to draw trigger lines on the charts. `matter` is
`null` in the diag build.

### 5.3 `etat`

Period `periode_ms` (1000 by default). Three blocks: `lampe` as a single
piece (worst case 917 bytes) exceeded the 896 budget (2.2), so its slots
were split out.

**Block `lampe`** (worst case 623 bytes):

| Field | Type | Meaning | Source |
|---|---|---|---|
| `boot`, `up_s` | | same as `hello` | |
| `consigne` | State | wanted state | `Halo1Lamp::target()` |
| `cru` | State | raw state of the lamp | `Halo1Lamp::believed()` |
| `a_livrer` | array of codes | target fields not yet delivered | `Halo1Lamp::dirty()` |
| `confirme` | array of codes | fields confirmed since boot | `Halo1Lamp::confirmed()` |
| `version` | integer | +1 on every target change | `Halo1Lamp::version()` |
| `phase` | `repos`, `rafale`, `reprise` | | `Halo1Lamp::phase_` (to add) |
| `reprise_ms` | integer or null | before the retry, in phase `reprise` | `max(0, (int32_t)(retryAt_ - millis()))` (to add) |
| `echecs` | integer 0..`reglages.reprises` (2) | failed rounds of the current target; the next one abandons it (`giveUp()` resets it to 0) | `failures_` (to add) |
| `lien` | `inconnu`, `ok`, `perdu` | | `Halo1Lamp::link()` |
| `accuse_ms` | integer or null | age of the lamp's last acknowledgement (null: none since boot) | `lastAckAt_`, `acked_` (to add) |
| `dernier_a` | integer 0..255 | last A press number | `Halo1Lamp::lastAuto()` |
| `a_entendus` | integer | A presses heard from the remote (never reset to zero) | `remoteAutoCount()` |
| `memoire` | lampes | selection memory | `memoryLamps()` |
| `livrees` | integer | targets delivered since boot (never reset to zero) | `deliveredCount()` |
| `abandons` | integer | targets abandoned (same) | `giveUpCount()` |
| `sauve_attente` | boolean | raw state not yet written to NVS | `persistDirty_` (to add) |
| `ecoute` | boolean | background listening for the remote | `listening()` |
| `trace` | boolean | driver traces | `tracing()` |

**Block `tranches`** (worst case 422 bytes, 119 with no active slot):

| Field | Type | Meaning | Source |
|---|---|---|---|
| `boot`, `up_s` | | same as `hello` | |
| `tranches` | array of 0 to 4 objects | active slots: `tranche` (`lum`, `temp`, `a`, `brut`), `charge` (hex), `accuses`, `essais`, `paquets` | `slots_` : `Slot.pay`, `acks`, `attempts`, `repeats` (to add) |

**Block `sante`** (worst case 726 bytes):

| Field | Type | Meaning | Source |
|---|---|---|---|
| `boot`, `up_s` | | | |
| `radio.presente` | boolean | BM5602 detected | `Halo1Radio::present()` |
| `radio.perdue` | boolean | L3 level: restart failed or had no effect | `Halo1Lamp::lost()` |
| `radio.mode` | `inconnu`, `reset`, `emission`, `ecoute`, `veille` | | `Halo1Radio::mode()` |
| `radio.configuree` | boolean | the chip holds the driver's configuration | `Halo1Radio::configured()` |
| `radio.quartz`, `radio.calib` | booleans | at the last `halo.begin()` | `radio.chip()->crystalReady()`, `calibrated()` |
| `surveil.panne` | boolean | DOWN | `ChipWatch::failed()` |
| `surveil.defaut` | boolean | criterion for the LED's solid red | `Halo1Lamp::moduleFault()` |
| `surveil.symptome` | `delais`, `bruit`, `sourde` or null | symptom present (`symptom()` never returns `Verify`) | `ChipWatch::symptom()` |
| `surveil.delais_suite` | integer 0..255 | consecutive TX timeouts (threshold 3) | `timeoutRun()` |
| `surveil.fen_trames`, `fen_crc_faux` | integers | current listening window (10 s) | `windowFrames()`, `windowBad()` |
| `surveil.hors_rx_10s` | integer | rearms outside RX over a sliding 10 s (threshold 1000) | `deafRearms()` |
| `surveil.sans_guerison` | integer | consecutive restarts without recovery (DOWN at 3) | `unrecovered()` |
| `surveil.attente_ms` | integer | before the next restart is allowed | `waitMs(millis())` |
| `surveil.relances` | integer | automatic restarts counted | `total()` |
| `surveil.derniere` | object or null | `cause`, `il_y_a_s` | `history(&e, 1)` |
| `led.motif` | see `led` (7.9) | pattern shown; null in diag | `statusLedPoll` : `sFrame.p` (to add) |
| `led.test` | boolean | `led test` in progress; null in diag | `Logic::testing()` |
| `led.depuis_ms` | integer | age of the pattern's phase (ms) at the time of sending: the app syncs its indicator to it (in `operationnel`, a 600 ms glow when `depuis_ms % 10000 < 600`); null in diag (rev 4) | `statusLedState` : `millis()` minus the phase start noted at the last `statusLedPoll` (`Frame.t`) |
| `matter` | object or null | `en_service`, `connecte`, `identify` | `matterIsCommissioned()`, `matterIsConnected()`, `matterIdentifying()`; null in diag |
| `sys.heap`, `heap_min`, `heap_bloc` | bytes | free heap, historical minimum, largest block | `esp_get_free_heap_size()`, `esp_get_minimum_free_heap_size()`, `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` |
| `sys.pile_boucle` | bytes | loop task stack never touched | `uxTaskGetStackHighWaterMark(NULL)` |
| `sys.boucle_max_ms` | integer | longest `loop()` pass since the previous block | measured (to add) |
| `sys.json_perdus`, `json_trop_longs`, `rejets` | integers | per transport (the session receiving the line): machine lines lost (send buffer or datagram queue full, delay, a line already being formatted), too long (bug), host lines refused (`trop_long`, `cadence`, `interdite`, and remotely a line without an `id`) | session counters (`json_mode.cpp : Sink`) |

`matterIsConnected()` over Thread never takes the OpenThread lock with a
wait (`netPoll`: `otLockTry(0)`, once per second): no risk for `tick()`.

### 5.4 `compteurs`

Period `compteurs_ms` (1000). Three blocks, cumulative counters: the app
computes charts from differences (section 8). `lampe stats raz`
(`Halo1Lamp::clearStats`) resets the `pilote` and `radio` blocks to zero
(`relances.*` included, `ChipWatch::clearCounts`), as well as
`etat.sante.surveil.relances` and `surveil.derniere`; but not
`sans_guerison`, `panne`, `attente_ms`, nor `tx.total`, `livrees`,
`abandons`, `a_entendus`, nor the `matter` block. The `raz` field (number of
`lampe stats raz` since boot, **to add**) flags the cut.

**Block `pilote`** (worst case 701) - source `Halo1Lamp::stats` unless noted
otherwise:

| Field | Source |
|---|---|
| `raz` | (to add) |
| `tx.consignes`, `paquets`, `accuses`, `ack_trame`, `max_rt`, `delais`, `fifo` | `requests`, `packets`, `acks`, `ackForeign`, `maxRt`, `timeouts`, `fifoRefused` |
| `tx.total` | `Halo1Lamp::txCount()` (never reset to zero, raw included) |
| `tranches.faibles`, `preemptees`, `annulees`, `reprises`, `abandons`, `attentes` | `weakFails`, `preempted`, `cancelled`, `retries`, `giveUps`, `holdoffs` |
| `a.livres`, `a.refuses` | `autoSent`, `autoIgnoredOff` |
| `rx.trames`, `etat`, `a`, `accuses_lampe`, `service`, `favori`, `invalides`, `crc_faux` | `rxFrames`, `rxState`, `rxAuto`, `rxLampAcks`, `rxService`, `rxReserved`, `rxInvalid`, `rxCrcBad` |
| `divers.sauvegardes`, `traces_perdues`, `relances_module` | `persisted`, `traceDropped`, `restarts` |

**Block `radio`** (worst case 523):

| Field | Source |
|---|---|
| `raz` | (to add) |
| `radio.configs`, `reconf_silence`, `reconf_tx`, `verif_ratees`, `rearm`, `rearm_hors_rx`, `brutes`, `bascules` | `Halo1Radio::stats` : `fullConfigs`, `silenceReconf`, `txReconf`, `verifyFail`, `rearms`, `rearmsOffRx`, `rxRaw`, `lightSwitches` |
| `garde` (null without an air guard): `active`, `gardes`, `refus`, `attentes`, `plafonnees`, `max_us` | `tuning.airGuard` ; `stats.guarded`, `guardRefused`, `guardWaits`, `guardCapped`, `guardMaxUs` |
| `relances.total`, `verif`, `delais`, `bruit`, `sourde` | `ChipWatch::total()`, `count(Relaunch::Verify / TxTimeout / RxNoise / RxDeaf)` |

**Block `matter`** (worst case 341; absent in diag) -
`matter_bridge.cpp : sStats` (static: to expose):

| Field | Source |
|---|---|
| `fenetres`, `ignorees` | `windows`, `bootIgnored` |
| `a_appuis`, `a_refuses`, `a_entendus`, `a_perdus` (EP4 only) | `autoFired`, `autoRefused`, `autoHeard`, `autoLost` |
| `reflets`, `ecritures`, `echecs`, `verrou`, `traces_perdues` | `reflects`, `writes`, `writeFails`, `lockBusy`, `logDropped` |
| `identify` | `sIdentifyCount` |

### 5.5 `reseau`

Period `reseau_ms` (5000). Thread build only (Wi-Fi build: `thread` block
without the `thread` object; diag: never). Three blocks (`ip` since
revision 2). OpenThread reads happen under `otLockTry(0)` (never a wait)
and stack reads under `TryLockChipStack()`: if the lock is held elsewhere,
the board returns the last values it read and `frais_ms` gives their age.

**Block `thread`** (worst case 685):

| Field | Source |
|---|---|
| `frais_ms` | age of the last read under lock (to add) |
| `matter.en_service`, `connecte` | `Matter.isDeviceCommissioned()`, `matterIsConnected()` |
| `matter.reseau` (`thread`, `wifi`), `matter.wifi` | `Matter.getSelectedNetwork()`, `Matter.isWiFiConnected()` |
| `matter.fabriques` | number of fabrics (Apple Home, Google...): `chip::Server::GetInstance().GetFabricTable().FabricCount()` under lock (to add, optional) |
| `matter.code_manuel`, `matter.qr` | the bridge's label, over USB, whether commissioned or not (revision 3; before: only when not commissioned): `Matter.getManualPairingCode()` and the `MT:...` payload = the `data=` parameter of `getOnboardingQRCodeUrl()`, whose `GetQRCodeUrl` encodes `:` as `%3A` (decode the `%XX`); values cached in the library, without a lock. They are only useful during a commissioning window (bridge new, reset, or removed from its last controller). Always null over the network transport. |
| `thread.role` | `otThreadGetDeviceRole` -> `otThreadDeviceRoleToString` (`disabled`, `detached`, `child`, `router`, `leader`) |
| `thread.canal`, `thread.mhz` | `otLinkGetChannel` ; `2405 + 5 x (canal - 11)` |
| `thread.pan` | `otLinkGetPanId`, `"0x%04X"` |
| `thread.tx_dbm` | `otPlatRadioGetTransmitPower` |
| `thread.parent_rssi` | `otThreadGetParentAverageRssi` (null on error) |
| `thread.mode` | `otThreadGetLinkMode` -> `linkModeText` (`rn` = MED, `rdn` = FTD) |
| `thread.type_boot`, `type_suivant` | `routeur`, `med_init`, `med_tard` : `sMedBoot`, `matterMedMode()` |
| `thread.pret_ms` | first moment attached + SRP host registered, or null: `sNet.readyAt` |
| `thread.roles` | role changes since boot: `sRoleChanges` |
| `thread.mle.attaches`, `detache`, `enfant`, `routeur`, `chef`, `parent_change` | `otThreadGetMleCounters` : `mAttachAttempts`, `mDetachedRole`, `mChildRole`, `mRouterRole`, `mLeaderRole`, `mParentChanges` |
| `thread.srp.client`, `hote`, `services`, `enregistres`, `serveur`, `port` | `otSrpClientIsRunning`, `otSrpClientItemStateToString(hote)`, services counted and registered, `otSrpClientGetServerAddress` |

**Block `abonnements`** (worst case 530):

| Field | Source (`matter_bridge.cpp`) |
|---|---|
| `abonnements.actifs`, `lectures` | `sCount.subs`, `sCount.reads` (`countPoll`, every 2 s) |
| `abonnements.sauves` | `collectSaved(...)` total under `TryLockChipStack()` (NVS read of each subscription, stack lock held): at most every 30 s and on `json etat`, last value kept; null if the lock or the iterator is busy |
| `abonnements.demandes`, `neufs`, `repris_pont`, `repris_pile`, `termines`, `plafonnes` | `sSubs.requested`, `fresh`, `byBridge`, `byStack`, `terminated`, `capped` |
| `abonnements.plafond_s`, `reprise_auto` | `sMaxIntCap`, `sResumeAuto` |
| `reprise.passages`, `auto`, `sessions`, `ouvertes`, `echecs`, `sans_nouvelles`, `reprises` | `sResume.runs`, `autoRuns`, `opened`, `ok`, `failed`, `lost`, `resumed` |
| `reprise.en_cours` | `resumeInFlight(millis())` |

**Block `ip`** (Thread build, revision 2; worst case ~770) -
`net_udp.cpp : netUdpJson`, OpenThread reading under `otLockTry(0)` at most
every 5 s, whether there's a key or not (the app reads the name and
addresses over USB before setting a key):

| Field | Source |
|---|---|
| `frais_ms` | age of the reading (null: never read) |
| `srp.nom` | the node's SRP hostname (`otSrpClientGetHostInfo()->mName`, the one Matter registers), to be resolved as `<nom>.local` (10.3); null if unknown |
| `adresses` | at most 4 objects: `adr` (IPv6 text), `type` (`omr`: off-mesh SLAAC, reachable from the LAN; `ml_eid`: mesh-only; `autre`), `pref` (preferred); no RLOC, no ALOC, no link-local (`otIp6GetUnicastAddresses`) |
| `udp.port` | 5480 |
| `udp.ouvert` | socket open (a key exists) |
| `udp.empreinte` | fingerprint of the key (10.4), null without a key: the app checks that it has the right one |
| `udp.sessions`, `udp.provisoire` | established H1 sessions (0..2), handshake in progress |
| `udp.rx`, `udp.rejets`, `udp.rx_perdus`, `udp.defis` | messages accepted; datagrams silently refused (shape, key, sid, MAC, replay, limit, SALUT with no room for its `DEFI`); lost before being read (too large, receive queue full, dropped when the socket closes); `DEFI` sent |
| `udp.tx`, `udp.tx_perdus`, `udp.tx_erreurs` | datagrams handed to OpenThread; lost after being queued (4 s with no departure, OpenThread refusal, queue flushed by a key change, session gone); among them, the ones OpenThread refused (once per datagram). A line that finds no room in the queue is counted in its session's `json_perdus`. |
| `udp.tampons_libres`, `udp.tampons_min` | free OpenThread message buffers at the time of reading, minimum seen before a send (null: unknown) |

### 5.6 `hb` and `fin`

`hb`: heartbeat for when `etat` messages are off or slow (3.6). Fields
`boot`, `up_s`, `json_perdus`.

`fin`: last message of a machine session. `cause`: `commande` (`json 0`) or
`bail`.

## 6. App commands (app -> board)

### 6.1 Choice: existing CLI text prefixed with `id=<n>`

The app sends the existing CLI's commands, exactly as a human would type
them, preceded by `id=<n> `:

```
id=17 lampe niveau 200
```

`n`: decimal integer 1..999999999, increasing per connection, wrapping back
to 1 after 999999999. On the network, a connection is an H1 session: `id`
values increase within it, and a reconnection redoes a handshake (10.2).
The prefix is stripped before dispatch (`handleLine`); no command starts
with `id=`.

Why not JSON commands:
- **a single parser** on the board side, the CLI's, already tested and
  documented (`lampe help`, README); no JSON library to link in (flash: the
  C6 firmware already weighs 2.41 MB for a 3 MB `huge_app` partition,
  `platformio.ini`);
- the app's **raw console** and its buttons' commands are the same stream,
  and every command from the app can be replayed by hand in a terminal;
- commands fit in 127 bytes; there is nothing to escape;
- the network transport carries the same line, inside a signed envelope.

What is lost: argument typing. It is recovered through responses: a
refused argument gives `code` `usage` (JSON commands and asynchronous
`lampe` commands), and the app only builds its lines from values that are
already bounds-checked.

### 6.2 Semantics of a line with `id`

| Command | Flow | Messages |
|---|---|---|
| `json` family | instantaneous; `json 1`, `json etat`, `json hello` go through the periodic queue | lines produced, then `reponse` (`fin`) after the last one |
| `lampe on\|off`, `lampe avant\|arriere on\|off`, `lampe mode ...`, `lampe lum ...`, `lampe niveau ...`, `lampe temp ...`, `lampe mired ...`, `lampe auto`, `lampe sync` | **asynchronous**: same rules as the human command (`runIntent`, `runState`, `pressAuto`, `reassert`), but **without** `waitIdle` or a text summary | `reponse` (`fin`) within a few ms, then `livraison` carrying the `id` once the driver is done |
| any other command (called historical) | unchanged: human text, sometimes blocking | `reponse` `debut`, the text, then `reponse` `fin` |

Without an `id`, nothing changes: human text, blocking `lampe` commands
with a summary (`printBilan`), no `reponse`. Machine mode (`json 1`) only
changes the echo, the prompt and what gets sent; it is the `id` that drives
the semantics.

Asynchronous commands, in detail (cli_lampe.cpp):
- `radioReady()` false: `reponse` `ok:false`, `code` `radio_absente` or
  `radio_perdue`, nothing is requested;
- arguments out of bounds: `code` `usage` (bounds of `cmdLampe`: `lum`
  4C..FE in hex, `niveau` 1..254, `temp` 0..100 in DECIMAL, `mired`
  153..370);
- `lampe auto` with the lamp off (`pressAuto()` false): `code` `refuse`;
- otherwise the target is set; if the driver is busy (`busy()`): `code`
  `accepte`, `suite` `livraison`, and the `id` joins the list of pending
  ids (8 at most, the oldest fall off: `ids_perdus`); a `livraison`
  (`livree`, `abandon` or `annulee`) will always follow;
- not busy but some fields remain to be delivered (`dirty()` non-zero: lamp
  off, brightness will be sent when it turns on): `code` `differe`, `suite`
  `aucune`; no `livraison` will come for this `id`;
- neither of the above: `code` `ok`, `suite` `aucune`.

The next `livraison` (7.3) carries all pending ids: the board merges
targets (a new one preempts a slot in progress), so one delivery covers all
commands accepted since the previous one.

### 6.3 The `reponse` message

| Field | Type | Meaning |
|---|---|---|
| `id` | integer | the line's own |
| `etape` | `debut` or `fin` | `debut`: a historical command starting (the loop may be about to block) |
| `cmd` | string <= 40 | the command received, without the prefix, truncated |
| `ok` | boolean | |
| `code` | string | see below |
| `msg` | string <= 120, optional | explanation in French |
| `duree_ms` | integer (`fin`) | execution duration |
| `suite` | `livraison` or `aucune` (asynchronous `lampe`) | |
| `consigne`, `a_livrer`, `version` | (asynchronous `lampe`) | target after the command |
| `bail_s`, `up_s` | (`json ping`, `json 1`) | |
| `cle` | 64 hex (`json cle nouvelle`, USB, once only) | key for the network transport (10.4): the app masks it everywhere |
| `empreinte` | 8 hex or null (`json cle ...`) | fingerprint of the key, null without a key |

Codes:

| Code | ok | Meaning |
|---|---|---|
| `ok` | yes | executed |
| `accepte` | yes | asynchronous `lampe`: a `livraison` will follow |
| `differe` | yes | asynchronous `lampe`: nothing to send right now |
| `en_cours` | yes | `debut` step |
| `execute` | yes | historical command finished; its result is in the text (the board does not know whether it displayed a `Usage`) |
| `usage` | no | invalid arguments (JSON commands and asynchronous `lampe` commands) |
| `refuse` | no | refused by a rule (`msg` says which) |
| `radio_absente`, `radio_perdue` | no | BM5602 absent or lost (L3) |
| `inconnue` | no | unknown command (final branch of `handleLine`) |
| `trop_long` | no | line over 127 bytes: nothing executed |
| `cadence` | no | more than 20 lines per second on this transport: nothing executed |
| `interdite` | no | forbidden on this transport (10.5) |
| `deja_traite` | no | network: `id` already processed, response no longer available (or a different command under this `id`); nothing is re-executed (10.2) |

### 6.4 Commands used by the app

| Screen | Action | Line sent | Expected follow-up |
|---|---|---|---|
| all | connection | `json 1`, then `json ping` | 3.3, 3.5 |
| dashboard | refresh | `json etat` | full snapshot |
| dashboard | test the LED | `led test` / `led stop` | `led` (events), text |
| commands | on / off | `lampe on` / `lampe off` | `reponse`, `livraison` |
| commands | front / back lamp | `lampe avant on`, `lampe arriere off` | same (Matter rules, selection memory) |
| commands | mode | `lampe mode avant\|arriere\|deux` | same (also turns on) |
| commands | brightness (Matter slider) | `lampe niveau <4..254>` (floor `kMatterLevelFloor`; the CLI accepts 1..254, but 1..3 yield 4C and are reported as 4) | same |
| commands | raw brightness | `lampe lum <4C..FE>` (hex) | same |
| commands | temperature (slider) | `lampe mired <153..370>` | same |
| commands | raw temperature | `lampe temp <0..100>` (decimal) | same |
| commands | button A | `lampe auto` | same, or `refuse` if the lamp is off |
| commands | resync | `lampe sync` | same (everything is resent) |
| frames | pause / resume the stream | `json trames 0\|1` | |
| frames | background listening | `lampe ecoute 0\|1` (bench) | state visible in `etat.lampe.ecoute` |
| charts | reset | `lampe stats raz` (confirmation) | `compteurs` with `raz` + 1 |
| console | everything else | typed line, prefixed with an `id` | `reponse` `debut`, text, `reponse` `fin` |

Sliders: the app sends at most one command every 150 ms while dragging, and
always the final value on release. The board merges targets (the final
value always gets its full burst: `Halo1Lamp::setSlot`); no point going
faster than the remote (~9 frames per second from the dial).

**Raw console.** Every typed line goes out with an `id`, so the app knows
where the output ends: text received between `reponse debut` and
`reponse fin` for that `id` is attached to it (best effort: an IDF log can
slip in). `lampe` state commands typed in the console therefore become
asynchronous (6.2); the app renders `reponse` and `livraison` as readable
text in the console. Confirmation requested by the app before sending:
`reboot`, `decommission`, `erase`, `wifi`, `addr`, `chan`, `xo`, `debit`,
`amble`, `aw`, `holtek`, `regcfg`, `lampe oublie`, `lampe adresse <x>`,
`lampe stats raz`, `matter med|maxint|reprise auto`,
`json cle nouvelle|efface`. After `reboot` or `decommission`, the app waits
for re-enumeration (3.1).

### 6.5 Rate limit and blocking commands

- **One command in flight at a time**: the app waits for the `reponse`
  `fin` of a line before sending the next one (a queue on the app side),
  `json ping` included.
- USB: with no `reponse` within 3 s (and no `debut`): the command is marked
  "no response", the app requests `json etat` and moves to the next one;
  **no automatic resend** (`lampe auto` is not idempotent: one more press
  changes the mode). Network: resent with the same `id`, which the board
  recognizes without re-executing (10.2).
- After `reponse debut`: no silence verdict (3.6) until the `fin`, with no
  hard-coded list. The longest command today is `txack` (200 x 5000 ms,
  off channel 5, ~17 min); `ecoute`, `prxack`, `sniffspi`, `etalon tx`,
  `ccasync` and `lampe attends` run up to 600 s. Beyond 20 min, the app
  offers to close the port (no bench command reads `Serial`: nothing
  interrupts it). During that time the loop is stuck inside the command:
  no `etat`, no heartbeat, no Matter (`matterBridgePoll`), no LED
  (`statusLedPoll`); the CLI stops reading. The app displays "bench command
  running".
- Blocking historical commands: the list follows `cli.cpp`, the app does
  not keep its own and relies on `reponse debut`. Any `lampe` state command
  WITHOUT an `id` blocks for up to 6 s (`kWaitMs`), `lampe brut` for up to
  ~26 s, `lampe rampe` depending on its arguments.
- Board side: at most 20 lines per second and per transport; beyond that,
  `reponse` `cadence` with no execution **(to add)**.

## 7. Events (board -> app)

Emitted the moment the firmware notices them, in the loop task. These are
clues: the exact totals are in `compteurs`. Rate caps per type and per
session (beyond the cap, the event is not produced for that session, does
not consume its `n`, and the next one of the same type carries `sautes`,
the number skipped; lower caps for a network session, 10.5):

| Type | Cap | Conditions |
|---|---|---|
| `rx` | 50 per second, of which 10 `crc_faux` | `json trames 1` |
| `tx` | 50 per second | `json trames 1` |
| `log` | 20 per second, except `src` `bouton` (never capped: a few lines per press, 30 ms debounce; it carries the `sautes` of the others) | `json log 1` |
| others | none (rare by nature) | always |

### 7.1 `rx`: frame heard

Source: `Halo1Lamp::onAir()` (every frame read while passively listening,
bad CRC included), and the `AckForeign` path of `Halo1Lamp::onVerdict()` (a
frame received in place of an acknowledgement, never observed). Worst case
263 bytes.

| Field | Type | Meaning | Source |
|---|---|---|---|
| `source` | `ecoute` or `accuse` | passive listening, or a frame inside an acknowledgement window | call path |
| `brut` | 16 hex or null | the 8 bytes read after the address (null for `accuse`) | `raw` from `Halo1Radio::pollRx`, local to `Halo1Lamp::tick()`: to be passed to `onAir` (to add) |
| `len`, `pid`, `no_ack` | integers | control field (9-bit PCF); `accuse`: `len` is always 2 (the only case transmitted: `TxReport.fLen == 2`, `Halo1Lamp::onVerdict`), `pid` and `no_ack` null | `AirFrame.len`, `pid`, `noAck` (`decodeAir`) |
| `charge` | hex, 0 to 4 bytes | payload (`""`: empty acknowledgement) | `AirFrame.pay[0..len-1]` |
| `crc`, `crc_ok` | 4 hex, boolean | CRC read, and whether it is correct (`accuse`: null and true, CRC verified by the chip) | `AirFrame.crc`, `crcOk` |
| `type` | `lum`, `temp`, `a`, `accuse_lampe`, `service`, `favori`, `invalide`, `crc_faux` | classification | `halo1::classify()` : `Bright`, `Temp`, `Auto`, `LampAck`, `Service`, `Reserved`, `Invalid`, `CrcBad` |
| `sens` | object or null | `lum`/`temp`: `marche`, `lampes`, and `lum` or `temp` (value from the air, unbounded); `a`: `numero`, `copie` (a copy of the same press, `AutoPressFilter::feed` false); otherwise null | flags `F_POWER`, `F_LAMPS`, value |

For `crc_faux`, `len`, `pid`, `charge` are decoded from suspect bits: to be
displayed in gray. `lum`/`temp` frames from the remote change the raw state
and the target (`onRemotePayload`): the next `etat.lampe` block will
reflect it.

### 7.2 `tx`: packet sent

Source: `Halo1Lamp::onVerdict()`, at the point of the `[lampe] TX ...`
trace, before `complete()` (so before the `livraison` it triggers). Worst
case 226.

| Field | Type | Meaning | Source |
|---|---|---|---|
| `num` | integer | packet number since boot (starting at 0) | `txCount() - 1` after `sendPacket` |
| `tranche` | `lum`, `temp`, `a`, `brut` | | `TxLog.slot` (`SLOT_*`) |
| `charge` | 4 hex | | `TxLog.pay` |
| `essai`, `paquets`, `accuses` | integers | this packet's rank within the slot, packets planned, acknowledgements obtained (this one included) | `Slot.attempts`, `repeats`, `acks` |
| `verdict` | `ack`, `ack_trame`, `max_rt`, `delai`, `fifo` | | `Halo1Radio::Verdict` |
| `us` | integer 0..65535 | from CE=1 to TX_DS or MAX_RT; `delai`: 30000 or slightly more; `fifo`: 0; saturates at 65535 | `TxReport.us` |
| `rt2`, `irq1`, `status` | 2 hex | registers read back after the packet | `TxReport.rt2`, `irq1`, `status` |

`paquets` is the planned count (`repeats`): for lack of acknowledgements,
the slot runs on up to `reglages.paquets_max` (`maxAttempts`), so `essai`
can exceed it. Bench reference points: acknowledgement ~1600-1720 us,
`irq1` `2E`, `rt2` `00`; MAX_RT ~11470 us, `irq1` `1E`, `rt2` `10`.

### 7.3 `livraison`: end of a target

Source: an observer in the loop task **(to add)**, right after
`lamp.tick()` and after any target is set (CLI, Matter), before
`statusLedPoll()`, on the `Halo1Lamp::busy()` true -> false edge. `issue` is
read off the counters: `deliveredCount()` changed -> `livree`;
`giveUpCount()` changed -> `abandon`; neither -> `annulee` (a slot removed
without delivery: a frame from the remote that sets the value itself
during the wait or the retry, or during a burst in place of an
acknowledgement (listening is cut during a burst), `setSlot` ->
`stats.cancelled`; `lampe oublie`; A pressed while abandoned with the lamp
off). Hooks in `complete()`/`giveUp()` are not enough: these endings do not
go through them. Special cases:
- a target started and finished inside a blocking command (a human `lampe`
  command, `waitIdle`): no edge seen, but a counter changed; the
  `livraison` is still sent when the command exits;
- both counters changed (a blocking command that chains targets, `lampe
  rampe`): `abandon` wins;
- a period occupied only by the `brut` slot (`lampe brut`): no `livraison`.

Worst case 528.

| Field | Type | Meaning | Source |
|---|---|---|---|
| `issue` | `livree`, `abandon` or `annulee` | | `busy()` edge and counters |
| `cause` | `injoignable`, `module` or absent | abandoned after retries (`fail()`), or module lost / configuration rejected (`tick()`, `restartModule()`) | (to add: reason noted by `giveUp`) |
| `derniere` | `lum`, `temp`, `a`, or null (`abandon`, `annulee`) | slot whose completion closed the target (`livree`) | (to add: slot noted by `complete()` with `delivered_++`), `kSlotText[id]` in lowercase (`"A"` -> `a`) |
| `version` | integer | | `Halo1Lamp::version()` |
| `consigne`, `cru` | State | after the delivery or the abandonment (abandonment: consigne = cru) | `target()`, `believed()` |
| `a_livrer` | codes | what remains (lamp off: brightness deferred) | `dirty()` |
| `ids`, `ids_perdus` | array of integers (8 at most), integer | app commands covered; `[]` if the target came from elsewhere (Matter, remote, human command) | list (to add) |
| `attente_ms` | integer or null | since the first pending request; null if the observer did not see the busy period (blocking command) | `pendingSince_` (to add), read by the observer while `busy()` is true: `endBurst()` and `giveUp()` reset it to 0 before the edge |
| `livrees`, `abandons` | integers | totals (never reset to zero) | `deliveredCount()`, `giveUpCount()` |

### 7.4 `relance`: BM5602 module restart

Source: `Halo1Lamp::restartModule()`, after `restart_()` (`halo.begin()`,
~300 ms, up to ~0.5 s), for L2 restarts (`announceRelaunch`) and L3
attempts (module lost, every 60 s). Sent once the result is known.

| Field | Type | Meaning | Source |
|---|---|---|---|
| `cause` | `verif`, `delais`, `bruit`, `sourde`, `l3` | | `halo1::Relaunch` (`None` = L3 attempt) |
| `rang` | integer or null | consecutive restarts without recovery, this one included (null for `l3`) | `watch_.unrecovered() + 1` before `relaunched()` |
| `detail` | object | `delais`: `suite`; `bruit`: `trames`, `crc_faux`, `ms`; `sourde`: `hors_rx`, `ms`; `verif`: `verif_ratees` | `timeoutRun()` (read BEFORE `relaunched()`, which clears it), `lastFlood()`, `lastDeaf()`, `radio.stats.verifyFail` |
| `ok` | boolean | `halo.begin()` succeeded | return value of `restart_()` |
| `quartz`, `calib` | booleans or null | what was redone (null on failure) | `radio.chip()->crystalReady()`, `calibrated()` |
| `duree_ms` | integer | duration of the restart (loop blocked) | measured (to add) |
| `total`, `panne` | integer, boolean | after this restart | `watch_.total()`, `failed()` |

### 7.5 `module`: module state

Transitions announced today as text by `notice()`.

| `etat` | When | Fields | Source |
|---|---|---|---|
| `panne` | DOWN: 3 restarts without recovery, the symptom returns | `sans_guerison`, `symptome`, `essai_s` (600) | `noteFault()` |
| `retabli` | end of the fault | | `noteFault()` |
| `perdu` | failed restart: module silent (L3) | | `restartModule()` (`lost_`) |
| `retrouve` | successful restart after a loss | | `restartModule()` |
| `config_rejetee` | configuration still rejected after a restart (L3) | `rfch`, `dm1`, `rt1` (2 hex, expected `05`, `82`, `73`) or null if unreadable | `restartModule()` (`stuck_`), `radio.readConfig()` |
| `config_verifiee` | configuration verified after a restart that had failed | | `tick()` (`stuck_` cleared) |

### 7.6 `intent`: resolved Matter commands

Source: `matter_bridge.cpp : applyIntents()`, when a coalescing window
closes (120 ms of quiet, 400 ms at most). Absent in diag.

| Field | Type | Meaning | Source |
|---|---|---|---|
| `recu` | object | the window's commands, last value per field: `ep1` (bool), `niveau` (0..254), `mireds`, `avant`, `arriere` (bool), `a` (true, EP4); only the fields present | `MatterIntents` (`has`, `IN_*`) |
| `fenetre_ms` | integer | from the first command to the close | `now - first` |
| `ignore` | `demarrage` or null | guard for the first 2 seconds; `demarrage`: `champs`, `consigne`, `version` and `a` absent (early return from `applyIntents`) | `sBootGuard`, `HALO1_BOOT_IGNORE_MS` |
| `champs` | codes | fields requested from the driver (empty: command dropped, for example a level written by the stack with EP1 off) | `Resolution.fields` |
| `consigne`, `version` | State, integer | after `lamp.request()` | `target()`, `version()` |
| `a` | `appui`, `ignore`, `refuse` or null | outcome of button A (EP4 only) | `r.fireAuto`, `pressAuto()` |

### 7.7 `abonnement`: Matter subscriptions (Thread build)

Source: `matter_bridge.cpp : tracePoll()` (loop task), which compares the
counters set by the CHIP task (`sSubs`, `sResume`, under `sSubMux`). Two
events between two passes (50 ms) merge into one: the message carries the
details of the last one and the totals, which say how many there were.

| `quoi` | Fields | Source |
|---|---|---|
| `demande` | `abonne` (`0x...`), `plancher_s`, `max_s` (requested), `applique_s` (after cap) | `sSubs.reqPeer`, `reqMin`, `reqMax`, `reqApplied` |
| `etabli` | `origine` (`neuf`, `pont`, `pile`), `min_s`, `max_s` | `sSubs.lastKind`, `lastMin`, `lastMax` |
| `termine` | | `sSubs.terminated` |
| `reprise` | `mode` (`auto`, `manuelle`), `verdict` (`lance`, `rien`, `sans_stockage`, `iterateur_occupe`), `sauves`, `abonnes`, `lances`, `servis`, `en_cours` | `sResume.runKind`, `runVerdict`, `runSaved`, `runPeers`, `runLaunched`, `runServed`, `runBusy` |
| `session` | `abonne`, `ok`, `erreur` (`0x..` or null), `duree_ms` | `sResume.doneNode`, `doneErr`, `doneMs` |
| `reprise_abonne` | `abonne`, `verdict` (`repris`, `deja_servi`, `rien`, `sans_stockage`, `iterateur_occupe`, `file_pleine`), `repris`, `sans_readhandler`, `rates` | `sResume.peerNode`, `peerVerdict`, `peerResumed`, `peerUnsettled`, `peerFailed` |

All of them carry `totaux`: `demandes`, `etablis`, `termines`, `passages`.
As with traces, an automatic pass with no effect, identical to the previous
one, is only emitted once (`quiet`).

### 7.8 `thread`: role change

Source: `sRoles` history (`onOtRoleChanged`), read back by `tracePoll()`.
Fields: `de`, `vers` (OpenThread text), `a_ms` (`RoleChange.ms`), `total`
(`sRoleChanges`). The history keeps 6 changes: if `total` jumps by more
than that, intermediate roles are lost.

### 7.9 `led`: indicator pattern

Source: `statusLedPoll()`, when the chosen pattern (`Logic::frame(now).p`)
changes. Fields: `motif`, `avant` (previous pattern), `test`, `depuis_ms`
(rev 4: age of the pattern's phase, like `sante.led.depuis_ms`; 0 at the
start of an event, but on returning to `operationnel` after a flash, the
glow resumes its earlier phase: `depuis_ms` counts from when it went live).
Also rev 4: the event is sent when the phase restarts without changing
pattern (flash retriggered, end of `led test` on the normal pattern);
`avant` is then equal to `motif`.

| `motif` | `statusled::Pattern` | Indicator |
|---|---|---|
| `identification` | `Identify` | rainbow |
| `desappairage` | `ButtonUnpair` | BOOT button held 8 s, then during unpairing, until the reboot: red, black, purple, black, 100 ms each (rev 1) |
| `redemarrage` | `ButtonReboot` | BOOT button, short press released: white flash (150 ms), black, reboot (rev 1) |
| `injoignable` | `Unreachable` | red, 3 blinks (1200 ms) |
| `panne_radio` | `RadioFault` | solid red |
| `livree` | `Delivered` | green flash (150 ms) |
| `non_appaire` | `Unpaired` | blue blinking 250/250 ms |
| `hors_reseau` | `Offline` | slow orange 1 s/1 s |
| `operationnel` | `Online` | off, white glow every 10 s |

The app animates its icon based on the pattern and the constants in
`status_led.h` (the instantaneous color is not transmitted). Absent in
diag. Pattern priority: the table's order, with `led test` placed just
below the button's own patterns. After `redemarrage`, or `desappairage`
followed by releasing the button, the board generally restarts: USB
re-enumeration (3.1). Otherwise the press was abandoned (a new press,
button pressed again just before the reset: `log` with `src` `bouton` in
`json log 1` mode).

### 7.10 `log`: firmware announcements and traces

Only with `json log 1`. Lines from `Halo1Lamp::notice()` (`niv` `notice`),
`Halo1Lamp::trace()` (`niv` `trace`), `bridgeLog()` (`src` `matter`, `niv`
`notice`) and BOOT button announcements (`src` `bouton`, `niv` `notice`,
`[bouton] ...` lines from `boot_button.cpp`: arming at 8 s, cancellation,
press ignored, reboot, unpairing; rev 1) are then sent as a `log` message
**instead of** text. Fields: `src` (`lampe`, `matter`, `bouton`), `niv`,
`txt` (the line, 191 characters at most, ASCII). IDF logs and command text
never go through `log`.

## 8. Charts: what the app computes

From two successive `compteurs` blocks (differences, over a 10 s or 1 min
window); a negative difference, or a change in `raz`, opens a new segment
(no outlier value):

| Chart | Calculation |
|---|---|
| TX loss rate | `(d max_rt + d delais + d fifo) / d paquets`; as a complement `1 - d accuses / d paquets` |
| Abandoned targets | `d tranches.abandons` (and markers from `livraison` `abandon`) |
| Bad CRC | `d rx.crc_faux` per minute, and `d rx.crc_faux / d rx.trames`; flood threshold: `seuils.deluge_trames` and `deluge_pct` over `fenetre_ms` |
| Reception refusals | `d radio.rearm_hors_rx` (rearms where the chip was not in RX: deafness symptom, threshold `sourd_hors_rx` over 10 s); on the send side `d tx.fifo`; Thread guard `d garde.refus` |
| Restarts | `relances.total` as a step chart, stacked by cause; markers from `relance` and `module` |
| Matter health | `abonnements.actifs`, `thread.parent_rssi`, role changes |

## 9. Versioning, compatibility, throughput

### 9.1 Rules

- `v` (major) changes only for a breaking change: a field removed, a type
  or unit changed, meaning changed, framing changed. The app declares which
  `v` values it handles; a `hello` with another version: it says so and
  stays console-only.
- Everything else is **additive** and keeps `v`: a new field, a new message
  type, a new block, a new enumeration value, a new command, a new
  capability. `rev` (in `hello`) increases with every addition, for display
  purposes only.
- The app **ignores** unknown fields, unknown types and blocks, and files
  an unknown enumeration value under "inconnu" without failing. Any field
  can be `null` or absent if the doc says it is optional; a required field
  that is absent makes the message invalid (ignored, counted).
- The firmware never changes the type, unit or meaning of a field within
  the same `v`: it adds another one and keeps the old one for at least one
  version.
- Swift decoding: one `Codable` per (`t`, `bloc`) with optional properties,
  after a first decode of the envelope (`v`, `t`, `n`, `ms`, `bloc`);
  enumerations with an `inconnu` case.

### 9.2 Throughput on USB CDC

Sizes measured on the examples in section 12 (bytes per line, RS and LF
included): `etat` 509 (`lampe`) + 97 (`tranches`, empty) + 613 (`sante`),
`compteurs` 462 + 350 + 166, `reseau` 568 + 365, `rx` ~200, `tx` ~175,
`hello` 487 + 316, `config` 657.

| Situation | Throughput |
|---|---|
| At rest, default settings (etat and compteurs 1 Hz, reseau 0.2 Hz) | ~2.4 KB/s |
| Remote's dial (~9 frames + ~9 lamp acknowledgements per s) | + ~3.6 KB/s |
| App command (one packet per 100 ms during the burst) | + ~1.8 KB/s for ~0.3 s per frame |
| All caps reached (etat and compteurs at 5 Hz, reseau 1 Hz, rx 50/s, tx 50/s, log 20/s) | ~35 KB/s; ~50 KB/s with worst-case sizes |

USB full-speed is not the bottleneck. The real limits: `HWCDC`'s 4 KB
buffer (half a second of the current peak rate, if the host is slow) and
the formatting done in the loop task (to be measured: a few tens to a few
hundred microseconds per line; `sys.boucle_max_ms` shows it).

On a UART-bridge target (`esp32dev`), `setup()` sets neither
`setTxBufferSize` nor `setTxTimeoutMs` (only under `ARDUINO_USB_MODE == 1`):
no send buffer at all, and `availableForWrite()` returns the room in the
hardware FIFO (128 bytes at most, `uartAvailableForWrite`). Any machine
line longer than that would be lost every time. Before announcing the
protocol there: `Serial.setTxBufferSize(4096)` before `Serial.begin()` for
these targets too; then, at 115200 baud (~11.5 KB/s), keep the defaults and
turn off `trames` when not needed.

## 10. Network transport: UDP over Thread

Implemented in 0.4.0 (build `esp32c6thread`, `src/net_udp.*`,
`src/h1_proto.*`), Sep 24, 2026. Path proven the same day from the Mac
(docs/ETUDE-THREAD-COMPAGNON.md, section 0): ping and UDP to an arbitrary
port get through Apple's border routers. Bench client: `tools/halo_udp.py`
(key over USB, session, commands).

The Sep 25 bench session (product board, Mac on Ethernet and Wi-Fi, static
route):
- R1: key set over USB, session opened, `json 1` (12 lines and the
  response), `lampe niveau 200` and `lampe mired 300` delivered in
  ~250 ms, 65 lines in 30 s with no loss or rejection; free OpenThread
  buffers: at least 42 out of 65;
- port 5480: DELAI (timeout; held by OpenThread, no more "port unreachable");
- R2, partly: `reboot`, `lampe brut`, `json 1 bail 0000000000`, `json
  periode 0000000200` refused remotely (`interdite`).

### 10.1 Path

Mac or iPhone (LAN) -> border router (HomePod, Apple TV) -> Thread -> node
(MED, receiver always on). The node is reached through its OMR address
(off-mesh prefix, `/64` SLAAC), advertised on the LAN by the border routers
(RIO). No dependency on Matter for this channel.

- The client must have the RIO route to the OMR prefix. **macOS kernel
  bug** (xnu, observed on Sep 24 and 25, `route -n monitor`: removed by the
  kernel, pid 0, route `CONDEMNED`). The kernel removes a prefix's RIO
  route through `defrouter_delreq` (a kernel RTM_DELETE, by prefix and
  mask) when `defrouter_select` switches routers for that prefix: a border
  router looks briefly unreachable (a failure of unreachability detection:
  `nd6_free` -> `nd6_router_select_rti_entries` -> `defrouter_select`), or
  its advertisement expires (`defrtrlist_del`). This is not `rt6_flush`,
  which only removes non-static host routes. An entry in the list of
  advertised routes can also be marked installed (`NDDRF_INSTALLED`) with
  no route in the table (observed on Sep 25, `sysctl
  net.inet6.icmp6.nd6_rtilist`): if the kernel goes through it,
  `defrouter_addreq` believes it is already set and sets nothing, and
  `defrouter_select` does not restore the route as long as that router
  stays reachable. Likely origin, read from the code: `nd6_ra_input`
  serves the same `dr0` to every RIO option of one advertisement, and
  `defrtrlist_update_common` copies the flags of an already-known entry
  onto it; the entry then created for another prefix is therefore born
  marked installed (seen on Sep 25 when a router changed its OMR prefix).
  Held for 37 min on Sep 24; on Sep 25, reinstalled over Wi-Fi then
  removed within the same second. Unplugging the interface is therefore
  not always enough. A STATIC route holds up better (`rt6_flush` ignores
  it; as long as it is there, the kernel's add fails and no new entry gets
  marked installed): `sudo route -n add -inet6 -prefixlen 64 <OMR>::
  <a border router's link-local>%<interface>`. But the kernel removes it
  too (prefix and mask, without looking at the gateway) when it switches
  routers or an advertisement expires while an entry is marked installed,
  and then generally sets its own route through another router. Choosing
  the exit router yourself (`IPV6_NEXTHOP`) requires being root: an app
  cannot work around this on its own. On the Mac, the
  `tools/macos/halo-routes/` system helper (a root launchd daemon,
  installed once) keeps the route: it rereads the kernel's list of
  advertised routes, sets a static route for every advertised `/64` ULA
  prefix that no longer has one, and restores it if the kernel removes it
  without setting its own. The app handles EHOSTUNREACH by explaining it.
- The OMR prefix can come from a third-party border router and change
  (observed on Sep 24): the client resolves a name, never a fixed address
  (10.3).

### 10.2 UDP datagrams

- Fixed port **5480** (`kHaloUdpPort`), below OpenThread's ephemeral range
  (49152..65535). The socket is OpenThread's own (`otUdpOpen`, `otUdpBind`
  on `OT_NETIF_THREAD_INTERNAL`): a port held by OpenThread is no longer
  handed back to lwIP. Open only as long as a key exists: without a key,
  lwIP answers "port unreachable" as it would for any closed port.
- One message = one datagram: an H1 header (10.4) then either the board's
  bare JSON object (without RS or LF), or the app's command line. A line
  from the board is at most 1022 bytes of JSON + 56 of header:
  1078 < 1232 (no IPv6 fragmentation on a 1280 MTU; 6LoWPAN fragments
  under Thread). The app sends at most 127 bytes of command; the board
  drops any received datagram over 256 bytes.
- Reception: an OpenThread callback (OT task), which copies the datagram
  into a 4-slot queue, nothing else (no CHIP lock, no Serial); all the
  rest happens in the loop task.
- Sending: a queue of 6 datagrams (shared by the sessions), handed to
  OpenThread from the loop task under the OpenThread lock taken WITHOUT
  waiting (`otLockTry(0)`), at most 2 per pass; not handed over within
  4 s: lost (counted).
  - Throughput capped at **3000 bytes/s** on average (2400-byte credit):
    every datagram becomes 802.15.4 frames a few cm from the BM5602, and
    shares the channel with Matter. The remote profile (10.6) uses
    ~0.8 KB/s of it; a `json 1` snapshot (~6 KB) goes out in ~1.5 s.
  - A datagram only goes out if 24 OpenThread buffers remain free after it
    (65 in total, shared with Matter; a 1 KB line takes about a dozen), at
    **low priority**: faced with a buffer shortage, OpenThread evicts
    ours, never Matter's.
  - Source: the address the app targeted, if it is still on the node (a
    connected UDP socket only keeps that one).
  - A response that finds no room in the datagram queue waits in its
    session's queue (like a deferred response): never lost for that
    reason.
- Why UDP and not TCP: messages are already discrete units; state is
  periodic and commands carry an `id` (a loss is recovered); no connection
  state or TCP buffers in RAM shared with Matter.
- Losses: `id` values **increase strictly** within an H1 session (a
  reconnection redoes a handshake, and the app redoes one before wrapping
  back to 1 after 999999999); only a command left without a response is
  resent with the same `id` (after 2 s, twice at most, with a new `ctr`:
  this is the network's version of rule 6.5, where a loss is ordinary);
  the app applies the same rule to resends of `json 1` (with no `hello`,
  resent 3 times with the same `id`, every 2 s), so that the board returns
  its already-cached or already-queued response instead of replaying its
  ~6 KB snapshot. Only resends keep the `id`: a new `json 1` (30 s retry,
  silence, lease expired, restart seen) takes a new one; remotely, the
  30 s retry with no `hello` first redoes the handshake (the provisional
  session is forgotten 30 s after the SALUT, 10.4). The board keeps the
  last 8 `reponse` (`fin` step) of each session and settles every line by
  its `id` BEFORE anything else, rate limit included
  (`jsonRemoteAdmit`):
  - same `id`, same command (judged on `cmd`'s 40 characters), cached
    response: the same `reponse` is sent again (without its `msg`),
    **nothing is re-executed** (`lampe auto` is not idempotent);
  - a deferred response for that `id` still in the queue (a `json 1` or
    `json etat` snapshot): nothing, it will be sent;
  - `id` lower than or equal to the highest `id` already seen (processed
    but no longer cached, a different command under this `id`, or an `id`
    going backward): `reponse` `deja_traite` (not kept), nothing executed;
    the app refreshes its state (`json etat`);
  - otherwise the `id` is new: the line proceeds normally.
- The periodic queue tolerates 6 s of delay on the network (500 ms on
  USB): a `json 1` snapshot (~6 KB) goes out in ~1.5 s, two simultaneous
  snapshots (Mac and iPhone after a restart) in ~4 s; network sessions
  take turns.
- On a network session, a frequent event (`rx`, `tx`, `log`) is only
  produced if two free datagrams remain in the send queue (room for a
  periodic line or a response after it); otherwise lost (`json_perdus`, a
  gap in `n`). Responses keep their order: an immediate response waits
  behind a deferred response from the same session.
- Only one command in flight at a time (6.5), on the network too: an `id`
  older than the highest one already seen is refused (`deja_traite`), even
  if it merely arrives late.
- The `DEFI` of a handshake jumps to the head of the send queue, outside
  the throughput cap; datagrams already sealed for a session that gets
  replaced or forgotten are pulled from the queue.

### 10.3 Discovery

The basis for v1: the node's **SRP hostname** (`<16 hexa>.local`, the one
Matter registers with the border routers, which publish it over mDNS on
the LAN) and port 5480. The board gives out the name over USB (`reseau`,
block `ip`, `srp.nom`); the app keeps it alongside the key and resolves it
on every connection (`getaddrinfo`, `NWConnection` on
`<nom>.local:5480`). The address thus follows OMR changes.

The name changes on a new commissioning (and possibly on a Thread network
change): the app rereads it on every USB connection. DNS-SD service
`_halo-pont._udp`: not in v1 (risky cohabitation with CHIP's SRP client;
see the study). Fallback: the bridge's `_matter._tcp` instance
(`<CompressedFabricId>-<NodeId>`), whose resolution gives the same host.

iOS app: `NSLocalNetworkUsageDescription` (resolving a `.local` name
requires local network authorization); `NSBonjourServices` only for
browsing. Refusal on macOS (bench R7, Sep 25): resolving `<nom>.local`
immediately returns `NoSuchRecord` (-65554), with neither `PolicyDenied`
nor a `localNetworkDenied` path; an absent `.local` name returns nothing
(it just waits). An already-open session keeps going: the flow routed to
the bridge's ULA is not cut.

### 10.4 Authentication

A 32-byte shared key (PSK), kept in NVS (`halo1/cle`). It **never** travels
over the network.

- `id=<n> json cle nouvelle <64 hexa>` (USB only, with an `id`): the app
  supplies 32 bytes from `SecRandomCopyBytes` (UPPERCASE hex), the board
  computes `cle = HMAC-SHA256(cle = alea_app, message = alea_carte)`
  (`esp_fill_random`, 32 bytes, radio active: hardware randomness), writes
  it to NVS and returns it once, in the `reponse` (`cle`, 64 hex, with
  `empreinte`). The `cmd` field of this response is `json cle nouvelle`
  (the random value is never sent back). Without an `id` (terminal):
  refused as text, the key is never displayed (`log2file` records `pio
  device monitor` sessions at the repository root). All network sessions
  drop. The app masks `cle` in its console and its log. The key is only
  created if the response can go out right away (USB send buffer free
  enough): otherwise `refuse`, nothing changes. If the response is lost
  anyway, `json cle` shows a fingerprint the app does not recognize: it
  redoes `json cle nouvelle`. Key written to NVS but not loaded
  (fingerprint calculation failed, in practice never): `ok` response with
  the key and the `msg` "transport reseau coupe jusqu'au redemarrage"
  (network transport disabled until reboot).
- `id=<n> json cle`: `empreinte` (first 8 hex digits of SHA-256(cle)),
  `null` without a key. `json cle efface`: no more key, no more network
  transport.
- A Matter reset (`decommission`, BOOT button held 8 s) also erases the
  key, as does the departure of the last controller (accessory removed
  from Apple Home: commissioning reopens): a new owner does not leave
  access to the old one. The app then redoes `json cle nouvelle` over USB.
- The macOS app stores the key in the keychain: a generic session
  password, service `fr.djoko.halo.pont`, account = SRP name (without
  `.local`), value = 64 UPPERCASE hex digits, comment = fingerprint, label
  `Halo - pont <nom>`; not synced (iCloud and sharing with iOS: phase 3).
  `tools/halo_udp.py` reads it back from this keychain (`security
  find-generic-password -s fr.djoko.halo.pont -a <nom> -w`).
- NVS not encrypted: whoever holds the board already has everything
  anyway (USB CLI).

Canonical text (the MAC covers it): hexadecimal in **UPPERCASE** only,
`ctr` in decimal with no leading zero (1..4294967295); any other form is
refused. Fields separated by a single space.

Handshake:

```
app  -> board : H1 SALUT <kid> <na> <mac_salut>
board -> app  : H1 DEFI <sid> <nc> <mac_defi>
```

`kid`: fingerprint of the key (8 hex); `na`, `nc`: 16 random bytes (32 hex)
from the app and the board, `na` fresh on every attempt; `mac_salut` =
HMAC-SHA256(PSK, `"H1|SALUT|" kid "|" na`), first 16 bytes (only someone
who has the key can make the board spend a `DEFI`); `sid`: 8 random hex
digits (non-zero); `mac_defi` = HMAC-SHA256(PSK, `"H1|DEFI|" kid "|" na "|"
nc "|" sid`), first 16 bytes as 32 hex digits (the board proves it has the
key; the app verifies it, and ignores a `DEFI` that does not match its
last `na`). Session key Ks = HMAC-SHA256(PSK, `"H1|SESSION|" na "|" nc "|"
sid`). The SALUT (83 bytes) is longer than the DEFI (82): no
amplification.

Messages after that:

```
H1 <sid> <ctr> <mac> <charge>
```

`charge` = everything after the fourth space, byte for byte (the command
line, or the JSON). `ctr`: per direction, starting at 1, strictly
increasing (a sliding window of 32 against reordering, checked AFTER the
MAC: a forged `ctr` never advances the window). `mac` = first 16 bytes, as
32 hex digits, of HMAC-SHA256(Ks, `sens "|" sid "|" ctr "|" charge`),
`sens` = `A` (app -> board) or `C` (board -> app): a message cannot be
bounced back to its own sender. MAC comparisons in constant time.

Sessions:
- a SALUT is only served if its `kid` is right, its MAC correct, its `na`
  not among the last 16 accepted (a replayed SALUT is ignored), if there
  is still `DEFI` credit left and room for the `DEFI` in the send queue;
  the board's randomness (`nc`, `sid`) is only drawn after that;
- a handshake only opens a **provisional** session (1 slot, the next one
  replaces it, forgotten after 30 s); it becomes one of the **2
  established sessions** on the app's first message with a correct MAC
  (usually `id=1 json 1`), into a free slot, otherwise into the slot of a
  session **ended by `json 0`** (rev 4; the least recently active of the
  two if there are two), otherwise into the slot of the least recently
  active one **if it has been silent for 30 s**; otherwise the message is
  ignored (verdict `complet`, nothing changes: the app will resend it):
  three clients for two slots do not chase each other in a loop, and
  `SALUT` messages from the LAN, without the key, touch nothing;
- a session ended by `json 0` stays established (its last lines still go
  out as long as its slot has not been reclaimed, a resend of the
  `json 0` gets its response): only its slot becomes reclaimable right
  away. Any new command on that session (never a resend served from
  cache) lifts the mark: `halo_udp.py session <nom> "json 0" "json 1"`
  keeps its slot. The app and `tools/halo_udp.py` end their sessions with
  `json 0`; a new connection redoes a handshake. Before rev 4, the slot
  only came back after 30 s of silence (the Sep 25 bench session: `hello`
  43 s after a client's `json 0`, instead of at the next attempt);
- **2 `DEFI` per second at most, IN TOTAL** (not per source: addresses can
  be spoofed);
- the app's address and port: those of the most recent message with a
  correct MAC (highest `ctr`; iPhone temporary addresses): an older
  message, delayed or replayed from elsewhere, does not hijack the
  responses;
- a session is forgotten after 10 min with no valid message; a new key or
  an erased key: all of them drop;
- any datagram refused (shape, unknown key, wrong MAC, replay, unknown
  sid, DEFI limit, slots full) is refused silently (counted: `reseau`
  block `ip`, `udp.rejets`).
- Residual risks:
  - whoever has captured enough valid SALUT messages (more than 16) can
    replay them at 2 per second and delay the opening of NEW sessions;
    established sessions are not affected;
  - a last message from the app lost in transit (never received by the
    board) and captured can be delivered later, during the session's
    lifetime (10 min): it executes, and responses go to its sender until
    the app's next message. Any more recent message from the app cancels
    it: the app ends its sessions with `json 0`.

Test vectors (computed in Python, `hmac`/`hashlib`; the same ones in
`tools/host_tests/test_h1.cpp`; `tools/halo_udp.py` does the same
calculations):

```
PSK  000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F
kid  630DCD29
na   A0A1A2A3A4A5A6A7A8A9AAABACADAEAF      nc  505152535455565758595A5B5C5D5E5F      sid 1234ABCD
SALUT  H1 SALUT 630DCD29 A0A1A2A3A4A5A6A7A8A9AAABACADAEAF 52D853E3FFE9E9CCEFFA98BB5304B32D
DEFI   H1 DEFI 1234ABCD 505152535455565758595A5B5C5D5E5F BFF13F71B42243E6017D2807F8E6171F
Ks   20D6D83D97ED44F2BBF8CE56389BD475CBE2B625CE6CE24768B6B4C1C625012F
A 1  H1 1234ABCD 1 FD97A0C9E604524B49C763452D0310CE id=1 json 1
C 1  H1 1234ABCD 1 347A2E6A129BC822ECFF39BEC910451C {"v":1,"t":"hb","n":7,"ms":1234}
```

Integrity and authenticity, **no confidentiality** in v1: the lamp's state
is readable on the LAN. To encrypt later: an `H2` envelope in AEAD
(AES-CCM, the C6's hardware via mbedTLS). Ruled out: DTLS-PSK (buffers and
handshake too heavy alongside Matter), a proprietary Matter cluster.

To be checked (R1): a global OMR prefix (DHCPv6-PD delegation) would make
the node reachable from the Internet; the HMAC protects it, only the cost
of `DEFI` messages is exposed, hence their global limit.

### 10.5 Remote commands: allowlist

Every line received over the network carries an `id` (otherwise: ignored,
counted in `rejets`). Allowed (`jsonp::remoteRefusal`, `json_out.cpp`):
- `json 1 [bail <10..120>]` (never `bail 0` remotely: the board would keep
  transmitting over Thread for an iPhone long gone until the session is
  forgotten, 10 min), `json 0`, `json etat`, `json hello`, `json ping`,
  `json periode` (2000 ms at least), `json compteurs` (0 or 5000 at
  least), `json reseau` (0 or 10000 at least), `json trames 0|1` (turns
  itself off after 60 s), `json log 0|1`;
- `lampe on|off`, `lampe avant|arriere on|off`, `lampe mode ...`, `lampe
  lum|niveau|temp|mired ...`, `lampe auto`, `lampe sync` (asynchronous);
- `led test|stop`: remotely, without the human text (which would block the
  loop if the USB host stops reading); `reponse` `fin` `ok` only.

Everything else: `reponse` `interdite` (with `msg`), nothing executed. In
particular: `json` alone, `json cle ...`, `reboot`, `decommission`,
`erase`, `wifi`, `matter ...`, `chiplog`, all the radio bench tools, `lampe
brut|croire|rafale|...|oublie|sauve`. Flashing stays USB-only (no OTA
partition: `huge_app`).

Network-specific bounds (`json 1 bail`, periods): checked both by the
allowlist AND by the command itself (a number is read everywhere the way
`strtoul` reads it: `bail 0000000000` is worth 0 and stays refused). Events
on a network session are capped lower than on USB: `rx` 10 per second (of
which 5 `crc_faux`), `tx` 10, `log` 10.

`reseau.thread.matter.code_manuel` and `qr` are always null remotely
(anyone who read them could add the bridge to their own controller).

### 10.6 Remote profile

`json 1` received over the network sets: `periode_ms` 2000, `compteurs_ms`
0, `reseau_ms` 30000, `trames` 0, `log` 0 (`hello.base.session.transport`:
`udp`). Reason: every `etat` (~1.2 KB across three datagrams) makes for a
dozen 802.15.4 frames, sent a few centimeters from the BM5602. The air
guard (`Halo1AirGuard`) prevents a new Thread frame during a lamp packet,
but the Sep 23 field test showed bursts of MAX_RT after every Apple Home
command: measure `compteurs.pilote.tx.max_rt` with and without a remote
client before raising the rates (test R3).

Each transport has its own session: USB and each network session have
their own settings, their own `n`, their own queue, their own losses
(`etat.sante.sys`). Events go out to every session in machine mode; a
`reponse` goes only to the source of its command; a `livraison` goes to
every session in machine mode (and to a session outside machine mode that
was waiting on one of its `id` values), carrying only that session's own
`id` values and its own `ids_perdus`.

## 11. To implement and to check on the bench

Firmware (without changing anything about human behavior):
- `src/json_out.{h,cpp}`: the writer (escaping, 1024-byte buffer, single
  write, `n`, losses, per-type caps, periodic queue, which also carries
  the `json 1`, `json etat`, `json hello` snapshots);
- `cli.cpp`: `json` in `kFree`, `id=` prefix stripped before `radioFree`;
  `reponse` `debut`/`fin`, the `json` family, echo, prompt and
  `Serial.flush()` disabled in machine mode, bytes outside 0x20..0x7E
  ignored in machine mode, `trop_long` and `cadence` refusals, Ctrl-U,
  lease (non-blocking end-of-lease text);
- `cli_lampe.cpp`: asynchronous variants with `id`, list of pending ids;
- `Halo1Lamp`: accessors (`phase_`, `retryAt_`, `failures_`, `slots_`,
  `lastAckAt_`/`acked_`, `persistDirty_`, `pendingSince_`), `raw` passed to
  `onAir`, abandonment cause noted by `giveUp()` and last slot by
  `complete()`, event hooks (`rx`, `tx`, `relance`, `module`); `raz`
  counter in `clearStats()`;
- loop task: `livraison` observer (`busy()` edge and counters, 7.3),
  before `statusLedPoll()`;
- `matter_bridge.cpp`: functions that fill `compteurs.matter`, `reseau.*`,
  `config.matter`, and the `intent`, `abonnement`, `thread` events from
  `applyIntents()` and `tracePoll()`;
- `status_led.cpp`: current pattern and changes;
- `setup()`: `boot` (`bootloader_random_enable()`, before
  `Matter.begin()`); `platformio.ini`/`tools/git_rev.py`: `FW_ENV`.

Bench tests:

| Test | Verification |
|---|---|
| U1 | 50 open-close cycles of the port by the app (DTR=RTS=0): `boot` unchanged, `up_s` continuous |
| U2 | same with `pio device monitor`; and a deliberately "bad" close (DTR lowered alone, RTS high): does the board restart? `hello.reset` is `usb` if the board restarted via DTR/RTS |
| U3 | `reboot`: re-enumeration, app reconnects within 5 s, `hello` with `reset` `logiciel` |
| U4 | `chiplog` active (IDF logs in a burst): corrupted lines rejected, gaps in `n` counted, no false value displayed |
| U5 | app suspended for 60 s (host stops reading): `sys.boucle_max_ms` stays low, `json_perdus` climbs, the lease returns to human mode without blocking; on resuming, fragments are classified as such (2.4); and, with no `Serial.flush()` in machine mode, 1000 commands back to back with no byte lost (3.8) |
| U6 | remote's dial for 30 s: `rx` with no `sautes` at default settings; measured throughput matching 9.2 |
| U7 | `id=1 lampe niveau 200`: `reponse` `accepte` within 20 ms, `livraison` with `ids:[1]` ~0.3-0.6 s later |
| U8 | a 140-byte line: `trop_long`, nothing executed |
| U9 | `json ping` every 10 s and `lampe` commands with `id` for 10 min: `compteurs.radio.radio.configs` does not increase on every line (`json` and `id=` outside `invalidateRadio`, 3.4) |
| U10 | lamp unplugged, `id=3 lampe niveau 200`, then the remote's dial during the retry (1 s after the first round): `livraison` `annulee` with `ids:[3]` (7.3) |
| R1 | (network) `tools/halo_udp.py cle <port>` then `session <nom SRP>.local` from the Mac: DEFI verified, `hello` with `transport` `udp`, `etat` every 2 s; `refus <OMR> 5480`: DELAI (timeout; port held by OpenThread, no more REFUS from lwIP) |
| R2 | replayed datagram, wrong MAC, replayed SALUT or SALUT with a wrong MAC, wrong key, missing `id`: nothing happens, `udp.rejets` climbs; `reboot` remotely: `interdite`; same `id` resent (the client does this after 2 s with no response): same `reponse`, nothing re-executed (`lampe auto` twice = a single press); older `id`: `deja_traite`; `json 1 bail 0000000000`: `interdite`; `decommission`: the key is erased |
| R3 | `max_rt` (`compteurs.pilote.tx.max_rt`) over 10 min of dial commands, with and without a remote session (profile 10.6) |
| R4 | unplug the border router carrying the Mac's route: recovery, and how long it takes |
| R5 | bridge restart; OMR change: name re-resolved, session reopened |
| R6 | Mac connected on two interfaces: does the route hold? |
| R7 | signed app, local network authorization refused: clear message |
| R8 | 1000 commands over the network: losses, RTT, `reponse` replayed without re-execution; `json_perdus` and `udp.tx_perdus`; USB in parallel with no gap in `n` |
| R9 | (rev 4) two clients hold the slots, a third one waits (`complet`): `json 0` from one of the two, and the third gets its `hello` on its next attempt, without waiting 30 s |
| U11 | (rev 4) the app's indicator and the board's glow in phase (USB and network), including after a green flash and after `led test` |

## 12. Examples

`<RS>` denotes the 0x1E byte; the trailing LF is omitted. Values consistent
with the bench (on-air address `63 FD F0 4F`, gamma 2: level 180 = lum A5,
level 200 = lum BA; temp 53 = 268 mireds).

### 12.1 Connection

App -> board (`\x15`: byte 0x15, Ctrl-U, followed by LF):

```
\x15
id=1 json 1
```

Board -> app:

```
<RS>{"v":1,"t":"hello","n":0,"ms":83512,"bloc":"base","rev":4,"fw":"0.4.0-1a2b3c4","fw_desc":"0.4.0-1a2b3c4","date":"Sep 24 2026","heure":"14:02:11","env":"esp32c6thread","build":"produit","reseau_build":"thread","puce":"esp32c6","idf":"v5.5.5","arduino":"3.3.12","boot":"3FA2C901","reset":"logiciel","reset_n":3,"up_s":83,"session":{"transport":"usb","periode_ms":1000,"compteurs_ms":1000,"reseau_ms":5000,"bail_s":30,"trames":true,"log":false},"limites":{"ligne_max":1024,"cmd_max":127}}
<RS>{"v":1,"t":"hello","n":1,"ms":83513,"bloc":"identite","boot":"3FA2C901","mac":"F0F5BD012345","id":{"fabricant":"Djoko-CLI","produit":"Pont ScreenBar Halo","serie":"HALO1-F0F5BD012345","nom":"Halo","hw":1,"hw_txt":"ESP32-C6 SuperMini + BM5602"},"caps":["matter","thread","garde","led","lampe_async","trames","log","udp","cle"]}
<RS>{"v":1,"t":"config","n":2,"ms":83514,"lampe":{"adresse":"4FF0FD63","air":"63FDF04F","canal":5,"debit_kbps":125},"reglages":{"paquets":3,"accuses_min":2,"paquets_max":5,"ecart_ms":100,"reprise_ms":1000,"reprises":2,"rearm_ms":100,"silence_ms":500,"rearm_fort":false,"leger":false,"garde":true,"gamma_c":200},"seuils":{"delais_suite":3,"deluge_trames":100,"deluge_pct":90,"fenetre_ms":10000,"sourd_hors_rx":1000,"sans_guerison":3,"ecart_ms":60000,"repli_ms":600000},"matter":{"endpoints":{"principal":1,"avant":2,"arriere":3},"lampes_en":"lumieres","mired_min":153,"mired_max":370,"niveau_plancher":4,"med":1,"med_boot":1,"maxint_s":20,"reprise_auto":true}}
<RS>{"v":1,"t":"etat","n":3,"ms":83515,"bloc":"lampe","boot":"3FA2C901","up_s":83,"consigne":{"marche":true,"lampes":"deux","lum":165,"niveau":180,"temp":53,"mired":268},"cru":{"marche":true,"lampes":"deux","lum":165,"niveau":180,"temp":53,"mired":268},"a_livrer":[],"confirme":["marche","lum","temp"],"version":12,"phase":"repos","reprise_ms":null,"echecs":0,"lien":"ok","accuse_ms":41210,"dernier_a":0,"a_entendus":0,"memoire":"deux","livrees":4,"abandons":0,"sauve_attente":false,"ecoute":true,"trace":false}
<RS>{"v":1,"t":"etat","n":4,"ms":83516,"bloc":"tranches","boot":"3FA2C901","up_s":83,"tranches":[]}
<RS>{"v":1,"t":"etat","n":5,"ms":83517,"bloc":"sante","boot":"3FA2C901","up_s":83,"radio":{"presente":true,"perdue":false,"mode":"ecoute","configuree":true,"quartz":true,"calib":true},"surveil":{"panne":false,"defaut":false,"symptome":null,"delais_suite":0,"fen_trames":0,"fen_crc_faux":0,"hors_rx_10s":0,"sans_guerison":0,"attente_ms":0,"relances":0,"derniere":null},"led":{"motif":"operationnel","test":false,"depuis_ms":62160},"matter":{"en_service":true,"connecte":true,"identify":false},"sys":{"heap":112640,"heap_min":86016,"heap_bloc":45056,"pile_boucle":4380,"boucle_max_ms":3,"json_perdus":0,"json_trop_longs":0,"rejets":0}}
<RS>{"v":1,"t":"compteurs","n":6,"ms":83518,"bloc":"pilote","raz":0,"tx":{"consignes":6,"paquets":21,"accuses":19,"ack_trame":0,"max_rt":2,"delais":0,"fifo":0,"total":21},"tranches":{"faibles":0,"preemptees":1,"annulees":0,"reprises":0,"abandons":0,"attentes":0},"a":{"livres":0,"refuses":0},"rx":{"trames":148,"etat":36,"a":0,"accuses_lampe":36,"service":76,"favori":0,"invalides":0,"crc_faux":0},"divers":{"sauvegardes":3,"traces_perdues":0,"relances_module":0}}
<RS>{"v":1,"t":"compteurs","n":7,"ms":83519,"bloc":"radio","raz":0,"radio":{"configs":161,"reconf_silence":139,"reconf_tx":2,"verif_ratees":0,"rearm":560,"rearm_hors_rx":3,"brutes":148,"bascules":0},"garde":{"active":true,"gardes":21,"refus":0,"attentes":4,"plafonnees":0,"max_us":2380},"relances":{"total":0,"verif":0,"delais":0,"bruit":0,"sourde":0}}
<RS>{"v":1,"t":"compteurs","n":8,"ms":83520,"bloc":"matter","fenetres":5,"ignorees":1,"reflets":11,"ecritures":14,"echecs":0,"verrou":2,"traces_perdues":0,"identify":0}
<RS>{"v":1,"t":"reseau","n":9,"ms":83521,"bloc":"thread","frais_ms":310,"matter":{"en_service":true,"connecte":true,"reseau":"thread","wifi":false,"fabriques":1,"code_manuel":"34970112332","qr":"MT:Y.K9042C00KA0648G00"},"thread":{"role":"child","canal":25,"mhz":2475,"pan":"0x1A2B","tx_dbm":20,"parent_rssi":-48,"mode":"rn","type_boot":"med_init","type_suivant":"med_init","pret_ms":21870,"roles":2,"mle":{"attaches":1,"detache":1,"enfant":1,"routeur":0,"chef":0,"parent_change":0},"srp":{"client":true,"hote":"Registered","services":1,"enregistres":1,"serveur":"fd8e:1c2a:44b0:1::1","port":53535}}}
<RS>{"v":1,"t":"reseau","n":10,"ms":83522,"bloc":"abonnements","frais_ms":1210,"abonnements":{"actifs":1,"lectures":0,"sauves":1,"demandes":1,"neufs":1,"repris_pont":0,"repris_pile":0,"termines":0,"plafond_s":20,"plafonnes":1,"reprise_auto":true},"reprise":{"passages":1,"auto":1,"sessions":0,"ouvertes":0,"echecs":0,"sans_nouvelles":0,"reprises":0,"en_cours":false}}
<RS>{"v":1,"t":"reponse","n":11,"ms":83523,"id":1,"etape":"fin","cmd":"json 1","ok":true,"code":"ok","duree_ms":12,"bail_s":30,"up_s":83}
```

### 12.2 Brightness command from the app

```
id=2 lampe niveau 200
```

```
<RS>{"v":1,"t":"reponse","n":71,"ms":95002,"id":2,"etape":"fin","cmd":"lampe niveau 200","ok":true,"code":"accepte","duree_ms":1,"suite":"livraison","consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":["lum"],"version":13}
<RS>{"v":1,"t":"tx","n":72,"ms":95004,"num":21,"tranche":"lum","charge":"C5BA","essai":1,"paquets":3,"accuses":1,"verdict":"ack","us":1719,"rt2":"00","irq1":"2E","status":"11"}
<RS>{"v":1,"t":"tx","n":73,"ms":95104,"num":22,"tranche":"lum","charge":"C5BA","essai":2,"paquets":3,"accuses":2,"verdict":"ack","us":1612,"rt2":"00","irq1":"2E","status":"11"}
<RS>{"v":1,"t":"tx","n":75,"ms":95204,"num":23,"tranche":"lum","charge":"C5BA","essai":3,"paquets":3,"accuses":3,"verdict":"ack","us":1617,"rt2":"00","irq1":"2E","status":"11"}
<RS>{"v":1,"t":"livraison","n":76,"ms":95206,"issue":"livree","derniere":"lum","version":13,"consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"cru":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":[],"ids":[2],"ids_perdus":0,"attente_ms":204,"livrees":5,"abandons":0}
<RS>{"v":1,"t":"led","n":77,"ms":95207,"motif":"livree","avant":"operationnel","test":false,"depuis_ms":0}
<RS>{"v":1,"t":"led","n":78,"ms":95357,"motif":"operationnel","avant":"livree","test":false,"depuis_ms":74000}
```

The slot finishes on the third packet: 3 packets per frame (`repeats`) and
at least 2 acknowledgements (`minAcks`), `Halo1Lamp::onVerdict`. Guaranteed
order: `tx` of the last packet, `livraison`, then `led` (the indicator
reads `deliveredCount()` later in the same `loop()` pass, in
`statusLedPoll`, after the delivery observer). `n` 74 is missing here: it
is a periodic `etat` block, omitted from the example.

### 12.3 Frames from the remote

```
<RS>{"v":1,"t":"rx","n":105,"ms":101310,"source":"ecoute","brut":"087F8068D3800000","len":2,"pid":0,"no_ack":0,"charge":"FF00","crc":"D1A7","crc_ok":true,"type":"service","sens":null}
<RS>{"v":1,"t":"rx","n":106,"ms":101402,"source":"ecoute","brut":"0962D2D86B000000","len":2,"pid":1,"no_ack":0,"charge":"C5A5","crc":"B0D6","crc_ok":true,"type":"lum","sens":{"marche":true,"lampes":"deux","lum":165}}
<RS>{"v":1,"t":"rx","n":107,"ms":101404,"source":"ecoute","brut":"0126588000000000","len":0,"pid":1,"no_ack":0,"charge":"","crc":"4CB1","crc_ok":true,"type":"accuse_lampe","sens":null}
<RS>{"v":1,"t":"rx","n":109,"ms":101611,"source":"ecoute","brut":"0B619AA284800000","len":2,"pid":3,"no_ack":0,"charge":"C335","crc":"4509","crc_ok":true,"type":"temp","sens":{"marche":true,"lampes":"deux","temp":53}}
<RS>{"v":1,"t":"rx","n":118,"ms":102930,"source":"ecoute","brut":"0A70008705800000","len":2,"pid":2,"no_ack":0,"charge":"E001","crc":"0E0B","crc_ok":true,"type":"a","sens":{"numero":1,"copie":false}}
<RS>{"v":1,"t":"rx","n":121,"ms":103031,"source":"ecoute","brut":"0962D2C86B000000","len":2,"pid":1,"no_ack":0,"charge":"C5A5","crc":"90D6","crc_ok":false,"type":"crc_faux","sens":null,"sautes":0}
```

The `brut` values are what `encodeAir()` produces for address
`63 FD F0 4F` (the last one: one bit flipped in the CRC). `FF00` is a
service frame (wake-up); `E001` is the first press on A (3 copies ~100 ms
apart: the next two would have `copie` true).

### 12.4 Failure, abandonment, restart

Lamp unplugged, after `id=7 lampe lum 80` (target lum 128, version 16):
5 packets in MAX_RT per round, 3 rounds (retries at +1 s and +2 s), then
abandonment: the target reverts to the raw state (version 17).

```
<RS>{"v":1,"t":"tx","n":208,"ms":120450,"num":40,"tranche":"lum","charge":"C580","essai":1,"paquets":3,"accuses":0,"verdict":"max_rt","us":11476,"rt2":"10","irq1":"1E","status":"01"}
<RS>{"v":1,"t":"livraison","n":251,"ms":124970,"issue":"abandon","cause":"injoignable","version":17,"consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"cru":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":[],"ids":[7],"ids_perdus":0,"attente_ms":4520,"livrees":5,"abandons":1}
<RS>{"v":1,"t":"led","n":252,"ms":124971,"motif":"injoignable","avant":"operationnel","test":false,"depuis_ms":0}
```

Deaf chip (Sep 24 incident): a restart, then DOWN later:

```
<RS>{"v":1,"t":"relance","n":19040,"ms":3605120,"cause":"sourde","rang":1,"detail":{"hors_rx":1204,"ms":2870},"ok":true,"quartz":true,"calib":true,"duree_ms":312,"total":1,"panne":false}
<RS>{"v":1,"t":"module","n":28770,"ms":5410022,"etat":"panne","sans_guerison":3,"symptome":"delais","essai_s":600}
```

### 12.5 Matter

```
<RS>{"v":1,"t":"intent","n":315,"ms":140220,"recu":{"ep1":true,"niveau":127},"fenetre_ms":131,"ignore":null,"champs":["marche","lum"],"consigne":{"marche":true,"lampes":"deux","lum":120,"niveau":127,"temp":53,"mired":268},"version":18,"a":null}
<RS>{"v":1,"t":"thread","n":1170,"ms":300480,"de":"child","vers":"detached","a_ms":300402,"total":3}
<RS>{"v":1,"t":"thread","n":1172,"ms":300620,"de":"detached","vers":"child","a_ms":300571,"total":4}
<RS>{"v":1,"t":"abonnement","n":1301,"ms":324210,"quoi":"termine","totaux":{"demandes":1,"etablis":1,"termines":1,"passages":1}}
<RS>{"v":1,"t":"abonnement","n":1340,"ms":331050,"quoi":"demande","abonne":"0x000000000001B669","plancher_s":0,"max_s":600,"applique_s":20,"totaux":{"demandes":2,"etablis":1,"termines":1,"passages":1}}
<RS>{"v":1,"t":"abonnement","n":1343,"ms":331690,"quoi":"etabli","origine":"neuf","min_s":0,"max_s":20,"totaux":{"demandes":2,"etablis":2,"termines":1,"passages":1}}
```

(Level 127 -> lum 0x78 = 120 at gamma 2, reported back as 127.)

### 12.6 Raw console, historical command

```
id=8 lampe stats
```

```
<RS>{"v":1,"t":"reponse","n":530,"ms":180002,"id":8,"etape":"debut","cmd":"lampe stats","ok":true,"code":"en_cours"}
  emission : 7 consignes, 24 paquets, 22 accuses, 0 ACK+TRAME, 2 MAX_RT, 0 delais, 0 FIFO refusees
  ... (texte de Halo1Lamp::printStats)
<RS>{"v":1,"t":"reponse","n":531,"ms":180009,"id":8,"etape":"fin","cmd":"lampe stats","ok":true,"code":"execute","duree_ms":7}
```

Refusal:

```
<RS>{"v":1,"t":"reponse","n":585,"ms":190001,"id":9,"etape":"fin","cmd":"lampe auto","ok":false,"code":"refuse","msg":"lampe eteinte : A n'est pas emis","duree_ms":0}
<RS>{"v":1,"t":"reponse","n":588,"ms":190400,"id":10,"etape":"fin","cmd":"lampe lum 20","ok":false,"code":"usage","msg":"lum : 4C..FE, en hexa","duree_ms":0}
```

### 12.7 `log` mode, heartbeat, end of session

After `id=11 json log 1` then `id=12 json periode 0` and
`id=13 json compteurs 0` (etat and compteurs turned off: `hb` heartbeat
every 2 s), the app goes quiet; 30 s after its last line, the lease
returns to human mode.

```
<RS>{"v":1,"t":"log","n":640,"ms":200100,"src":"lampe","niv":"notice","txt":"[lampe] injoignable : consigne abandonnee"}
<RS>{"v":1,"t":"hb","n":641,"ms":202000,"boot":"3FA2C901","up_s":202,"json_perdus":0}
<RS>{"v":1,"t":"fin","n":662,"ms":231400,"cause":"bail"}
json : mode machine coupe (hote muet depuis 30 s)
>
```

### 12.8 Network transport

H1 session opened from the Mac (10.4); payloads of the board's datagrams
(no RS inside the datagram; `<RS>` is added here so they can be checked
like the other examples). `id=1 json 1` received over the network: remote
profile (10.6). Then a command outside the allowlist, and the same
`lampe auto` command resent (response lost): served from the cache,
nothing re-executed.

```
<RS>{"v":1,"t":"reseau","n":12,"ms":903120,"bloc":"ip","frais_ms":1840,"srp":{"nom":"561F9A6463953778"},"adresses":[{"adr":"fd77:9e:f4bb:0:6c06:6762:45d6:a3f0","type":"omr","pref":true},{"adr":"fd9a:3c2e:1b7:d4e1:8f02:6c4d:19a3:5b70","type":"ml_eid","pref":true}],"udp":{"port":5480,"ouvert":true,"empreinte":"630DCD29","sessions":1,"provisoire":false,"rx":1,"rejets":0,"rx_perdus":0,"defis":1,"tx":9,"tx_perdus":0,"tx_erreurs":0,"tampons_libres":51,"tampons_min":38}}
<RS>{"v":1,"t":"reponse","n":13,"ms":903161,"id":1,"etape":"fin","cmd":"json 1","ok":true,"code":"ok","duree_ms":41,"bail_s":30,"up_s":903}
<RS>{"v":1,"t":"reponse","n":20,"ms":909020,"id":2,"etape":"fin","cmd":"reboot","ok":false,"code":"interdite","msg":"interdite a distance (10.5) : USB seulement","duree_ms":0}
<RS>{"v":1,"t":"reponse","n":21,"ms":911400,"id":3,"etape":"fin","cmd":"lampe auto","ok":true,"code":"accepte","duree_ms":1,"suite":"livraison","consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":[],"version":14}
<RS>{"v":1,"t":"reponse","n":24,"ms":913420,"id":3,"etape":"fin","cmd":"lampe auto","ok":true,"code":"accepte","duree_ms":1,"suite":"livraison","consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":[],"version":14}
```
