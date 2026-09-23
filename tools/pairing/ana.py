#!/usr/bin/env python3
"""Analyse des fenetres brutes de cctrig (23/09).

Pour chaque fenetre : zone ou la porteuse est vue (lignes CSB), histogramme
des paliers de la donnee brute DANS cette zone (-> duree d'un bit, donc le
debit, sans hypothese), puis decoupage a plusieurs debits et recherche :
  - de l'adresse connue 63 FD F0 4F (temoin) ;
  - de toute trame au format BC5602 standard dont le CRC-16/CCITT (init FFFF,
    sur adresse + PCF + charge) est juste, pour une adresse INCONNUE : on
    essaie chaque position de depart apres un preambule 0101/1010.
Usage : ana.py <log> [debits kbps, ex. 125,250,1000]
"""
import re, sys
from collections import Counter, defaultdict
sys.path.insert(0, "/Users/Majid/Documents/Dev/esp32/benq/tools/audit/indep_pll")
from pll import load, slice_bits, toint, hexb, crc, find, tobits

def load_cs(path):
    wins = defaultdict(dict)
    for line in open(path, errors="replace"):
        m = re.match(r"\s*CSB\s+(\d+)\s+(\d+)\s+([0-9A-Fa-f]{64})\s*$", line)
        if m: wins[int(m[1])][int(m[2])] = m[3]
    out = {}
    for k, d in wins.items():
        s = []
        for o in sorted(d):
            h = d[o]
            for i in range(0, 64, 8):
                w = int(h[i:i+8], 16)
                s.extend((w >> b) & 1 for b in range(31, -1, -1))
        out[k] = s
    return out

def runs(s):
    out, i = [], 0
    while i < len(s):
        j = i
        while j < len(s) and s[j] == s[i]: j += 1
        out.append(j - i); i = j
    return out

def try_frames(bits, maxlen=32):
    """Trames standard a CRC juste, adresse quelconque de 4 octets."""
    hits = []
    n = len(bits)
    # Pas d'exigence de preambule : la porteuse n'est vue qu'apres lui (temoin
    # du 23/09). Le CRC-16 suffit : ~1 fausse alarme pour 300 fenetres.
    for i in range(0, n - 32 - 9 - 16):
        addr = bits[i:i+32]
        pcf = bits[i+32:i+41]
        ln = toint(pcf[:6])
        if ln > maxlen: continue
        end = i + 41 + 8*ln
        if end + 16 > n: continue
        if crc(addr + pcf + bits[i+41:end]) == toint(bits[end:end+16]):
            hits.append((i, toint(addr), ln, toint(pcf[6:8]), pcf[8], bits[i+41:end]))
    return hits

path = sys.argv[1]
rates = [int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "125,250,500,1000").split(",")]
W, CS = load(path), load_cs(path)
known = tobits(0x63FDF04F, 32)
allrun = Counter()
for k in sorted(W):
    s, ns = W[k]
    cs = CS.get(k, [1]*len(s))
    idx = [i for i, v in enumerate(cs) if v]
    a, b = (idx[0], idx[-1]) if idx else (0, len(s)-1)
    zone = s[max(0, a-200):b+200]
    r = runs(zone)
    rus = Counter(round(x * ns / 1000) for x in r if x * ns < 40000)   # en microsecondes
    allrun.update(rus)
    print(f"fenetre {k}: {ns} ns/ech, porteuse {len(idx)} ech. ({len(idx)*ns/1000:.0f} us) de {a} a {b}")
    print("   paliers (us:nb) les plus frequents :", ", ".join(f"{u}:{c}" for u, c in sorted(rus.most_common(8))))
    for kb in rates:
        bits, conf, pos = slice_bits(zone, ns, bitns=1e6/kb)
        f = find(bits, known, 2)
        fr = try_frames(bits)
        msg = []
        if f: msg.append(f"adresse connue a {[(i,e) for i,e in f][:3]}")
        for (i, addr, ln, pid, noack, pay) in fr:
            msg.append(f"TRAME CRC OK @bit{i}: adr {addr:08X} len {ln} PID {pid} NO_ACK {noack} charge [{hexb(pay)}]")
        print(f"   {kb:5d} kbps : {len(bits)} bits ; " + (" | ".join(msg) if msg else "rien"))
print("\nPaliers cumules (us:nb) :", ", ".join(f"{u}:{c}" for u, c in sorted(allrun.most_common(12))))
