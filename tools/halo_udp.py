#!/usr/bin/env python3
"""Client de banc du transport reseau du pont Halo (docs/PROTOCOLE-JSON.md, 10).

En attendant la source "Reseau" de l'app : regler la cle par USB, puis ouvrir
une session H1 (UDP sur Thread, a travers les routeurs de bordure) et suivre
les lignes JSON du pont, ou lui envoyer des commandes de la liste blanche.

  cle <port serie>
      Nouvelle cle partagee : 'json cle nouvelle <alea>' par l'USB (ouverture
      sure du C6 : DTR = RTS = 0 en un seul appel, jamais RTS=1 DTR=0 qui le
      redemarre). La cle est rangee dans ~/.config/halo-pont/cle (0600), jamais
      affichee ; seule son empreinte l'est. L'app compagnon doit avoir libere
      le port ("Liberer le port"). Toutes les sessions reseau tombent.

  session <hote|adresse> [commande ...] [--duree s] [--brut] [--port p]
      Poignee de main (SALUT signe, DEFI verifie), 'json 1', puis chaque commande
      donnee (entre guillemets, sans id=), 'json ping' toutes les 10 s. Une
      commande sans reponse 2 s est renvoyee avec le meme id (2 fois au plus :
      le pont renvoie sa reponse sans reexecuter). Sans nouvelles 8 s (pont
      redemarre, session evincee), nouvelle poignee de main.
      Affiche un resume de chaque ligne (--brut : le JSON tel quel).
      Hote : le nom SRP du pont (ex. 561F9A6463953778.local), ou son adresse OMR.

  refus <adresse> [port] [essais]
      UDP vers un port : REFUS (ICMPv6 port injoignable), DELAI (rien), pour
      verifier qu'un port traverse la borne. Port 5480 avec une cle en place :
      DELAI attendu (le port est au pont, qui ignore un datagramme invalide).

Exemples :
  python3 tools/halo_udp.py cle /dev/cu.usbmodem101
  python3 tools/halo_udp.py session 561F9A6463953778.local --duree 30
  python3 tools/halo_udp.py session 561F9A6463953778.local "lampe niveau 200" "lampe mired 300"
"""
import fcntl
import hashlib
import hmac
import json
import os
import select
import socket
import struct
import sys
import termios
import time

PORT = 5480
RS = 0x1E
# HALO_CLE : autre fichier de cle (tests, plusieurs ponts).
KEY_PATH = os.path.abspath(os.environ.get("HALO_CLE") or os.path.expanduser("~/.config/halo-pont/cle"))


def mac16(key, text):
    return hmac.new(key, text.encode() if isinstance(text, str) else text, hashlib.sha256).digest()[:16]


def kid_of(key):
    return hashlib.sha256(key).hexdigest().upper()[:8]


# ---------------------------------------------------------------------------
#  Cle par l'USB
# ---------------------------------------------------------------------------


def open_serial(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    fcntl.ioctl(fd, termios.TIOCEXCL)
    # DTR = RTS = 0 dans un seul appel (section 3.2 : RTS=1 DTR=0 redemarre le C6).
    fcntl.ioctl(fd, termios.TIOCMSET, struct.pack("I", 0))
    iflag, oflag, cflag, lflag, _, _, cc = termios.tcgetattr(fd)
    iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR | termios.IGNCR
               | termios.ICRNL | termios.IXON)
    oflag &= ~termios.OPOST
    lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    cflag &= ~(termios.CSIZE | termios.PARENB | termios.HUPCL)
    cflag |= termios.CS8 | termios.CLOCAL | termios.CREAD
    termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, termios.B115200, termios.B115200, cc])
    return fd


def machine_lines(fd, deadline):
    buf = b""
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            raise SystemExit("port ferme (la carte a redemarre ?)")
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            i = line.rfind(bytes([RS]))
            if i < 0:
                continue
            try:
                yield json.loads(line[i + 1:].decode("ascii"))
            except ValueError:
                continue


def cmd_cle(port_path):
    alea = os.urandom(32).hex().upper()
    try:
        fd = open_serial(port_path)
    except OSError as e:
        raise SystemExit(f"{port_path} : {e.strerror} (l'app tient-elle le port ? 'Liberer le port')")
    try:
        os.write(fd, b"\x15\n")  # Ctrl-U : efface un reste de ligne
        time.sleep(0.2)
        ident = 900000 + int.from_bytes(os.urandom(2), "big") % 90000
        os.write(fd, f"id={ident} json cle nouvelle {alea}\n".encode())
        for m in machine_lines(fd, time.time() + 5):
            if m.get("t") == "reponse" and m.get("id") == ident and m.get("etape") == "fin":
                if not m.get("ok") or "cle" not in m:
                    raise SystemExit(f"refuse : {m.get('code')} {m.get('msg', '')}")
                key = bytes.fromhex(m["cle"])
                if kid_of(key) != m.get("empreinte"):
                    raise SystemExit("empreinte incoherente : cle non rangee")
                save_key(m["cle"])
                print(f"cle rangee dans {KEY_PATH} (empreinte {m['empreinte']})")
                if m.get("msg"):
                    print(f"  ({m['msg']})")
                return
        raise SystemExit("aucune reponse en 5 s (firmware sans transport reseau ? carte occupee ?). "
                         "Si la carte a quand meme change de cle, 'json cle' par l'USB montre une autre "
                         "empreinte : relancer 'cle'.")
    finally:
        os.close(fd)


def save_key(hex_key):
    """Ecriture atomique, 0600, sans suivre de lien symbolique."""
    import tempfile
    d = os.path.dirname(KEY_PATH)
    os.makedirs(d, mode=0o700, exist_ok=True)
    fdk, tmp = tempfile.mkstemp(prefix=".cle.", dir=d)  # 0600, nom unique, jamais un lien
    try:
        os.fchmod(fdk, 0o600)
        os.write(fdk, (hex_key + "\n").encode())
        os.fsync(fdk)
    finally:
        os.close(fdk)
    try:
        os.replace(tmp, KEY_PATH)
    except OSError:
        os.unlink(tmp)
        raise


def load_key():
    try:
        with open(KEY_PATH) as f:
            key = bytes.fromhex(f.read().strip())
    except OSError:
        raise SystemExit(f"pas de cle : lancer d'abord 'cle <port serie>' ({KEY_PATH})")
    except ValueError:
        raise SystemExit(f"{KEY_PATH} : cle illisible (64 hexa attendus)")
    if len(key) != 32:
        raise SystemExit(f"{KEY_PATH} : cle illisible (64 hexa attendus)")
    return key


# ---------------------------------------------------------------------------
#  Session H1
# ---------------------------------------------------------------------------


class Window:
    def __init__(self):
        self.top, self.bits = 0, 0

    def accept(self, ctr):
        if ctr == 0:
            return False
        if ctr > self.top:
            shift = ctr - self.top
            self.bits = 0 if shift >= 32 else (self.bits << shift) & 0xFFFFFFFF
            self.bits |= 1
            self.top = ctr
            return True
        back = self.top - ctr
        if back >= 32 or self.bits & (1 << back):
            return False
        self.bits |= 1 << back
        return True


def summary(m):
    t, b = m.get("t"), m.get("bloc")
    head = f"n={m.get('n')} {t}" + (f"/{b}" if b else "")
    if t == "reponse":
        extra = f" id={m.get('id')} {m.get('etape')} {m.get('code')} « {m.get('cmd')} »"
        if m.get("msg"):
            extra += f" : {m['msg']}"
        return head + extra
    if t == "etat" and b == "lampe":
        c, r = m.get("consigne", {}), m.get("cru", {})
        return head + (f" consigne {'on' if c.get('marche') else 'off'} {c.get('lampes')} niveau {c.get('niveau')}"
                       f" mired {c.get('mired')} | cru niveau {r.get('niveau')} | lien {m.get('lien')}")
    if t == "livraison":
        return head + f" {m.get('issue')} ids={m.get('ids')} attente {m.get('attente_ms')} ms"
    if t == "reseau" and b == "ip":
        u = m.get("udp", {})
        adrs = ", ".join(f"{a.get('adr')} ({a.get('type')})" for a in m.get("adresses", []))
        return head + (f" srp {m.get('srp', {}).get('nom')} | {adrs} | udp sessions {u.get('sessions')}"
                       f" rx {u.get('rx')} tx {u.get('tx')} perdus {u.get('tx_perdus')}"
                       f" tampons {u.get('tampons_libres')} (min {u.get('tampons_min')})")
    if t == "hello" and b == "base":
        return head + f" fw {m.get('fw')} rev {m.get('rev')} session {m.get('session')}"
    if t == "fin":
        return head + f" cause {m.get('cause')}"
    return head


def handshake(s, key, kid):
    """SALUT signe, na neuf a chaque essai ; seul un DEFI au MAC juste pour ce na est pris."""
    for essai in range(3):
        na = os.urandom(16).hex().upper()
        mac = mac16(key, f"H1|SALUT|{kid}|{na}").hex().upper()
        try:
            s.send(f"H1 SALUT {kid} {na} {mac}".encode())
        except OSError as e:
            print(f"!! envoi impossible : {e.strerror} (route IPv6 vers le reseau Thread ?)")
            time.sleep(1)
            continue
        deadline = time.time() + 2.0  # le DEFI peut attendre la fin d'un datagramme en cours
        while time.time() < deadline:
            s.settimeout(max(0.05, deadline - time.time()))
            try:
                d = s.recv(2048)
            except socket.timeout:
                break
            except ConnectionRefusedError:
                raise SystemExit("port injoignable : pas de cle sur le pont, ou firmware sans transport reseau")
            except OSError as e:
                print(f"!! reception : {e.strerror}")
                break
            p = d.split(b" ")
            if len(p) != 5 or p[0] != b"H1" or p[1] != b"DEFI":
                continue  # une ligne d'une session precedente : ignoree
            sid, nc, m = p[2].decode("ascii", "replace"), p[3].decode("ascii", "replace"), p[4]
            want = mac16(key, f"H1|DEFI|{kid}|{na}|{nc}|{sid}").hex().upper().encode()
            if not hmac.compare_digest(want, m):
                continue  # DEFI d'un essai precedent, ou faux : ignore
            ks = hmac.new(key, f"H1|SESSION|{na}|{nc}|{sid}".encode(), hashlib.sha256).digest()
            return sid, ks
    return None, None


def cmd_session(host, port, commands, duree, brut):
    key = load_key()
    kid = kid_of(key)
    try:
        ai = socket.getaddrinfo(host, port, socket.AF_INET6, socket.SOCK_DGRAM)
    except socket.gaierror as e:
        raise SystemExit(f"{host} : {e}")
    addr = ai[0][4]
    s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    try:
        s.connect(addr)
    except OSError as e:
        raise SystemExit(f"{addr[0]} : {e.strerror} (route IPv6 vers le reseau Thread ? voir 10.1)")
    print(f"pont {addr[0]} port {addr[1]}, cle {kid}")

    stats = {"lignes": 0, "rejetees": 0, "renvois": 0, "sessions": 0}
    todo = list(commands)
    start = time.time()
    state = {}

    def open_session():
        sid, ks = handshake(s, key, kid)
        if not sid:
            return False
        state.update(sid=sid, ks=ks, ctr=0, ident=0, window=Window(), last_rx=time.time(), last_cmd=0.0,
                     pending=None)
        stats["sessions"] += 1
        print(f"session {sid} ouverte")
        send("json 1")
        return True

    def send_raw(ident, line, ctr_bump=True):
        if ctr_bump:
            state["ctr"] += 1
        text = f"id={ident} {line}"
        m = mac16(state["ks"], f"A|{state['sid']}|{state['ctr']}|{text}").hex().upper()
        try:
            s.send(f"H1 {state['sid']} {state['ctr']} {m} {text}".encode())
        except OSError as e:
            print(f"!! envoi impossible : {e.strerror}")
        return text

    def send(line):
        state["ident"] += 1
        text = send_raw(state["ident"], line)
        state["pending"] = {"id": state["ident"], "line": line, "at": time.time(), "tries": 0}
        state["last_cmd"] = time.time()
        print(f">> {text}")

    if not open_session():
        raise SystemExit("aucun DEFI en 6 s (cle differente ? route IPv6 ? 'refus' pour tester le chemin)")
    try:
        while time.time() - start < duree:
            now = time.time()
            if now - state["last_rx"] > 8:
                print("!! pont muet depuis 8 s : nouvelle poignee de main")
                if not open_session():
                    time.sleep(1)
                    continue
                now = time.time()
            p = state["pending"]
            if p and now - p["at"] > 2:
                if p["tries"] < 2:
                    p["tries"] += 1
                    p["at"] = now
                    stats["renvois"] += 1
                    print(f">> (renvoi) {send_raw(p['id'], p['line'])}")
                else:
                    print(f"!! id={p['id']} sans reponse")
                    state["pending"] = None
            if not state["pending"]:
                if todo and now - state["last_cmd"] >= 1.5:
                    send(todo.pop(0))
                elif now - state["last_cmd"] >= 10:
                    send("json ping")
            s.settimeout(0.3)
            try:
                d = s.recv(2048)
            except socket.timeout:
                continue
            except ConnectionRefusedError:
                print("!! port injoignable (ICMPv6) : cle effacee ? pont redemarre ?")
                continue
            except OSError as e:
                print(f"!! reception : {e.strerror}")
                time.sleep(0.5)
                continue
            parts = d.split(b" ", 4)
            if (len(parts) != 5 or parts[0] != b"H1" or parts[1] != state["sid"].encode()
                    or not parts[2].isdigit()):
                stats["rejetees"] += 1
                continue
            c = int(parts[2])
            want = mac16(state["ks"], b"C|" + parts[1] + b"|" + parts[2] + b"|" + parts[4]).hex().upper().encode()
            if not hmac.compare_digest(want, parts[3]) or not state["window"].accept(c):
                stats["rejetees"] += 1
                print(f"!! datagramme rejete (MAC ou rejeu), ctr {c}")
                continue
            state["last_rx"] = time.time()
            stats["lignes"] += 1
            try:
                m = json.loads(parts[4])
            except ValueError:
                print(f"!! JSON illisible : {parts[4][:80]!r}")
                continue
            if (m.get("t") == "reponse" and m.get("etape") == "fin" and state["pending"]
                    and m.get("id") == state["pending"]["id"]):
                state["pending"] = None
            print(parts[4].decode("ascii", "replace") if brut else summary(m))
    except KeyboardInterrupt:
        pass
    if state.get("sid"):
        state["pending"] = None
        send("json 0")
    print(f"{stats['lignes']} ligne(s) recue(s), {stats['rejetees']} rejetee(s), {stats['renvois']} renvoi(s), "
          f"{stats['sessions']} session(s)")


def cmd_refus(addr, port=40000, essais=5):
    res = {}
    for _ in range(essais):
        s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        s.settimeout(3)
        try:
            s.connect((addr, port))
            s.send(b"x")
            s.recv(64)
            r = "reponse"
        except ConnectionRefusedError:
            r = "REFUS : ICMPv6 port injoignable (le port passe la borne ; ferme sur le pont)"
        except socket.timeout:
            r = "DELAI : rien (port ouvert qui ignore le datagramme, ou filtrage)"
        except OSError as e:
            r = f"ERREUR {e.errno} {e.strerror} (route ?)"
        finally:
            s.close()
        res[r] = res.get(r, 0) + 1
        time.sleep(0.5)
    for r, n in res.items():
        print(f"{n}/{essais}  {r}")


def main(argv):
    if len(argv) < 3 or argv[1] not in ("cle", "session", "refus"):
        print(__doc__)
        return 1
    if argv[1] == "cle":
        cmd_cle(argv[2])
    elif argv[1] == "refus":
        cmd_refus(argv[2], int(argv[3]) if len(argv) > 3 else 40000, int(argv[4]) if len(argv) > 4 else 5)
    else:
        args, duree, brut, port = [], 60.0, False, PORT
        i = 3
        while i < len(argv):
            a = argv[i]
            if a == "--duree":
                duree = float(argv[i + 1])
                i += 2
            elif a == "--port":
                port = int(argv[i + 1])
                i += 2
            elif a == "--brut":
                brut = True
                i += 1
            else:
                args.append(a)
                i += 1
        cmd_session(argv[2], port, args, duree, brut)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
