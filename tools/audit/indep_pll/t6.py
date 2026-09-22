import glob, re
from pll import *
from collections import Counter
fr = frames()
print("accuses f[22], f[23] :", Counter((r['f'][22], r['f'][23]) for r in fr if toint(r['f'][:4])==0))
print("commandes f[39] :", Counter(r['f'][39] for r in fr if toint(r['f'][:4])==2))
# modele de la nuit sur async et FIFO
def night(bits):
    h = toint(bits[:4])
    if h == 2:  return crc(ADDR + bits[:24], 0xDFBE) == toint(bits[24:40])
    if h == 0:  return crc(ADDR + bits[:8], 0xF55A) == toint(bits[8:24])
TRUE_ADDR=[0,1]+ADDR[:30]
def new(bits):
    s = ADDR[30:] + bits; ln = toint(s[:6])
    if ln > 3: return None
    return crc(TRUE_ADDR + s[:9+8*ln]) == toint(s[9+8*ln:25+8*ln])
c = Counter(); 
for r in fr: c[("async", toint(r['f'][:4]), night(r['f']), new(r['f']))] += 1
seen=set()
for p in glob.glob("/Users/Majid/Documents/Dev/esp32/benq/logs/*.log"):
    for line in open(p, errors="replace"):
        m = re.search(r"BRUT\s+([0-9A-Fa-f]{64})", line)
        if m and m.group(1).upper() not in seen:
            seen.add(m.group(1).upper())
            b = [(x >> k) & 1 for x in bytes.fromhex(m.group(1)) for k in range(7,-1,-1)]
            c[("fifo", toint(b[:4]), night(b), new(b))] += 1
for k,v in sorted(c.items(), key=str): print("  source=%s quartet=%d  modele_nuit=%s  modele_PCF_FFFF=%s : %d" % (k+(v,)))
