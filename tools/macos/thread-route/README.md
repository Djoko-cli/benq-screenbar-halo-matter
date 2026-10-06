[Français](README.fr.md) · **English**

# Thread Route: the Mac's system helper for the Thread network

A launchd daemon (root) that keeps the Mac's route to the Thread network: to
the Halo bridge over UDP (docs/PROTOCOLE-JSON.md, section 10, in the Halo
bridge repository) and to Maillage Thread's probe over "Thread Network". It
was called `halo-routes` (`fr.djoko.halo.routes`) until Oct 6, 2026.

Its source lives in the Halo bridge repository (`tools/macos/thread-route`);
Maillage Thread keeps an identical copy (`outils/thread-route`). The commands
below run from this folder.

## Why

The Mac reaches Thread nodes through the OMR prefix (a `/64` ULA) that
border routers (HomePod, Apple TV) advertise on the LAN (the RIO option of
router advertisements). macOS kernel bug (10.1): it removes the route for
this prefix when it switches routers (one of them looks briefly
unreachable, or its advertisement expires), but its list of advertised
routes may still believe it's in place. The route then never gets put
back: `No route to host` until the interface restarts, sometimes even
longer. A static route holds up better, but it takes root to set one, and
an app can't choose its own outbound router on its own.

## What it does

On every kernel message about routes (300 ms later, to debounce bursts)
and every 10 s (a lost router doesn't trigger any message), it re-reads
the kernel's list of advertised routes
(`sysctl net.inet6.icmp6.nd6_rtilist`), the state of the routers (neighbor
cache), and that of the interfaces. For each advertised ULA `/64` prefix:

- no route: it sets one, static, marked `RTF_PROTO1` (flag `1` in
  `netstat -rn`), via the safest router: reachable first (neighbor
  `REACHABLE`, `STALE`, `DELAY`, or `PROBE`, as judged by the kernel), then
  the one the kernel believes it has installed (if that one goes down, the
  kernel itself removes the route and sets its own), then on the main
  interface; never through a downed interface;
- our route goes through a router that no longer advertises the prefix: it
  switches routers; through a router that's unreachable (or whose
  interface is down) while another is reachable: it also switches, if
  that's confirmed over two passes at least 2 s apart. The switch happens
  in place (`route change`: gateway and interface, with no drop); failing
  that, removal then re-addition;
- no more advertisement at all: it removes our route (the advertisement
  having expired, the kernel wouldn't have a route either).

The static route isn't safe from this: when the kernel switches routers or
an advertisement expires, it removes the route for the prefix, whichever
one it is (by prefix and mask), and generally sets its own; otherwise, the
daemon puts its own back on the next pass. At most 6 changes per prefix
per minute; after a failure, nothing before the next minute. On stopping
(`launchctl bootout`, uninstall), it removes the routes it set.

## What it doesn't do

- It doesn't touch any route that isn't its own: the kernel's, ones set by
  hand, and any prefix outside `fc00::/7` or with a length other than 64.
  It only sets what the kernel would have set itself for an advertisement
  it accepted.
- No input from outside the kernel: no network port, no command file, no
  runtime argument. Changes go through `/sbin/route`, with fixed argv, no
  shell.
- The Thread network's prefix isn't hardcoded: if it changes, the daemon
  follows the advertisements.

## Installing

Under your own account, without sudo (the program is built and tested
here; only copying it into place and putting it into service require the
administrator password):

    sh installer.sh

The installer first shows what the daemon would do (a dry run, nothing is
changed). A route set by hand for the same prefix stays with its owner:
the daemon leaves it in place and doesn't take it over. The installer
flags it; remove it for the daemon to take over managing it
(`sudo route -n delete -inet6 -prefixlen 64 <prefixe>`).

**From halo-routes.** The installer first stops and removes the old daemon
(`fr.djoko.halo.routes`, its program and its plist), then puts Thread Route
in place: the two never run together. Between stopping it and removing
its files, the installer checks with `launchctl print` that it has really
left launchd; if not, it stops with a message, having removed and installed
nothing. The old log (`/Library/Logs/fr.djoko.halo.routes.log`) is kept.
`sh installer.sh --plan` tells, without changing anything, what the
installation would do (it shows that check without making it).

**Updating.** A new version of Thread Route installs the same way, by running
`installer.sh` again: an app's automatic update doesn't touch it.

**Why not from the app.** Halo Compagnon and Maillage Thread stay in the
macOS sandbox. Trial of Oct 6, 2026: `SMAppService` refuses to register a
daemon there that isn't sandboxed itself ("SMAppService target executable
must be sandboxed because the app is sandboxed"), and a sandboxed daemon
couldn't keep the routes. Both apps therefore only read its status
(`SMAppService.statusForLegacyPlist`, which the sandbox allows): absent, to
approve, active, or halo-routes still there.

Files: `/Library/PrivilegedHelperTools/fr.djoko.thread.route` (the
program), `/Library/LaunchDaemons/fr.djoko.thread.route.plist` (launches at
startup, restarts if it stops), `/Library/Logs/fr.djoko.thread.route.log`
(log).

## Checking

    tail -f /Library/Logs/fr.djoko.thread.route.log
    netstat -rn -f inet6 | grep '^fd'

A route set by the daemon carries the `S` (static) and `1` (its own
marker) flags. macOS also shows it in System Settings, General, Login Items
& Extensions, "Allow in the Background": turned off there, it no longer runs
("to approve" in the apps). A dry run with nothing installed or changed (no
need to be root); its output shows the Thread network's prefix:

    sh tests.sh

## Uninstalling

    sh desinstaller.sh

The daemon removes its routes as it stops; the logs are kept
(`/Library/Logs/fr.djoko.thread.route.log` and, if it exists,
`/Library/Logs/fr.djoko.halo.routes.log`). A leftover `halo-routes` is
removed too. `sh desinstaller.sh --plan` tells, without changing anything,
what the uninstall would do.

## Limitations

- Killed without a chance to clean up (SIGKILL), it picks its routes back
  up on restart as long as their prefix is still advertised; a route whose
  prefix has since disappeared stays until the Mac reboots.
- A small race: between its reading of the table and a removal or a
  change, the kernel may set its own route for the prefix; `route` targets
  the prefix and mask without checking who owns the route, and would then
  touch the kernel's own (the next pass puts a route back if needed).
- At most 32 ULA `/64` prefixes tracked (and 32 routers per prefix):
  beyond that, the extra prefixes are ignored, and as long as the list
  overflows, the daemon stops removing its routes for lack of an
  advertisement; the log flags it.
- The log isn't rotated: a few lines per kernel incident.
- Works around the bug, doesn't fix it: without the daemon, the kernel's
  route can still disappear.
