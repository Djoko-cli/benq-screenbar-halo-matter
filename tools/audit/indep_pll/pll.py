#!/usr/bin/env python3
"""Decodeur d'audit independant (dossier prive audit/indep_pll).
Recuperation d'horloge par DPLL sur les fronts, decision par moyenne des
echantillons au centre du bit (20 %-80 % de la cellule). Pas de paliers arrondis."""
import re
from collections import defaultdict

def load(path):
    wins, ns = defaultdict(dict), {}
    for line in open(path, errors="replace"):
        m = re.match(r"\s*ASYNC\s+(\d+)\s+(\d+)\s+(\d+)", line)
        if m: ns[int(m.group(1))] = int(m.group(2)); continue
        m = re.match(r"\s*AS\s+(\d+)\s+(\d+)\s+([0-9A-Fa-f]{64})\s*$", line)
        if m: wins[int(m.group(1))][int(m.group(2))] = m.group(3)
    out = {}
    for k in sorted(wins):
        offs = sorted(wins[k])
        assert offs == list(range(0, 8*len(offs), 8)), (k, "trou")
        s = []
        for o in offs:
            h = wins[k][o]
            for i in range(0, 64, 8):
                w = int(h[i:i+8], 16)
                s.extend((w >> b) & 1 for b in range(31, -1, -1))
        out[k] = (s, ns[k])
    return out

def slice_bits(s, ns, bitns=8000.0, gain=0.25, bias=0.0):
    T = bitns / ns
    bits, conf, pos = [], [], []
    i = 1
    while i < len(s) and s[i] == s[i-1]: i += 1
    t = float(i); n = len(s)
    while t + T < n:
        a, b = int(t + 0.2*T), int(t + 0.8*T)
        seg = s[a:b] if b > a else [s[a]]
        m = sum(seg) / len(seg)
        bits.append(1 if m + bias > 0.5 else 0); conf.append(abs(m - 0.5)*2); pos.append(t)
        nxt = t + T
        lo, hi = int(nxt - T/2), int(nxt + T/2)
        best = None
        for j in range(max(lo,1), min(hi, n)):
            if s[j] != s[j-1]:
                if best is None or abs(j - nxt) < abs(best - nxt): best = j
        if best is not None: nxt += gain * (best - nxt)
        t = nxt
    return bits, conf, pos

def tobits(val, n): return [(val >> k) & 1 for k in range(n-1, -1, -1)]
def toint(bl):
    v = 0
    for b in bl: v = (v << 1) | b
    return v
def hexb(bl): return " ".join("%02X" % toint(bl[i:i+8]) for i in range(0, len(bl) - 7, 8))
def step(c, b, poly=0x1021):
    top = ((c >> 15) & 1) ^ b
    c = (c << 1) & 0xFFFF
    return c ^ poly if top else c
def unstep(c2, b, poly=0x1021):
    top = c2 & 1
    x = c2 ^ (poly if top else 0)
    return (x >> 1) | ((top ^ b) << 15)
def crc(bl, init=0xFFFF, poly=0x1021):
    c = init
    for b in bl: c = step(c, b, poly)
    return c
def find(bits, pat, maxerr=0):
    L = len(pat); out = []
    for i in range(len(bits) - L + 1):
        e = 0
        for a, b in zip(bits[i:i+L], pat):
            if a != b:
                e += 1
                if e > maxerr: break
        if e <= maxerr: out.append((i, e))
    return out

TELE = "/Users/Majid/Documents/Dev/esp32/benq/logs/async-tele.log"
BAL = "/Users/Majid/Documents/Dev/esp32/benq/logs/async-bal.log"
ADDR = tobits(0x8FF7C13C, 32)

def frames(path=TELE, maxerr=0, before=40, after=130):
    out = []
    for k, (s, ns) in load(path).items():
        bits, conf, pos = slice_bits(s, ns)
        for i, e in find(bits, ADDR, maxerr):
            if i - before < 0: pre = [None]*(before-i) + bits[:i]
            else: pre = bits[i-before:i]
            out.append(dict(k=k, i=i, e=e, pre=pre, f=bits[i+32:i+32+after], conf=conf[i+32:i+32+after]))
    return out
