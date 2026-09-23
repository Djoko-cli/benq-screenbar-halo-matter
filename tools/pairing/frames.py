#!/usr/bin/env python3
"""Liste compacte des trames CRC justes de chaque fenetre cctrig, avec l'heure
de la fenetre (CSWIN) et la position de la trame en microsecondes."""
import re, sys
sys.path.insert(0, "/Users/Majid/Documents/Dev/esp32/benq/tools/audit/indep_pll")
sys.path.insert(0, "/Users/Majid/Documents/Dev/esp32/benq/tools/pairing")
from pll import load, slice_bits, toint, hexb, crc
src = open("/Users/Majid/Documents/Dev/esp32/benq/tools/pairing/ana.py").read().split("path = sys.argv[1]")[0]
exec(src)
path = sys.argv[1]; kb = int(sys.argv[2]) if len(sys.argv) > 2 else 125
off = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
tw = {}
for line in open(path, errors="replace"):
    m = re.match(r"CSWIN (\d+) t=(\d+) ms porteuse=(\d+)", line)
    if m: tw[int(m[1])] = (int(m[2]), int(m[3]))
W, CS = load(path), load_cs(path)
for k in sorted(W):
    s, ns = W[k]
    cs = CS.get(k)
    idx = [i for i, v in enumerate(cs) if v]
    a, b = idx[0], idx[-1]
    lo = max(0, a - 300); zone = s[lo:b + 300]
    bits, conf, pos = slice_bits(zone, ns, bitns=1e6 / kb)
    fr = try_frames(bits)
    t, c = tw.get(k, (0, 0))
    desc = []
    for (i, addr, ln, pid, noack, pay) in fr:
        us = (lo + pos[i]) * ns / 1000
        desc.append(f"@{us:6.0f}us {addr:08X} L{ln} P{pid} N{noack} [{hexb(pay)}]")
    print(f"{k:3d} t={t/1000 - off:8.3f}s cs={c:5d} " + (" | ".join(desc) if desc else "-"))
