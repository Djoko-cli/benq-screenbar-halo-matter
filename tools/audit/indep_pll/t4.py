import glob, re
from pll import *
from collections import Counter
TRUE_ADDR = [0,1] + ADDR[:30]
seen = {}
for p in sorted(glob.glob("/Users/Majid/Documents/Dev/esp32/benq/logs/*.log")):
    for line in open(p, errors="replace"):
        m = re.search(r"BRUT\s+([0-9A-Fa-f]{64})", line)
        if m: seen.setdefault(m.group(1).upper(), p.split("/")[-1])
print("lignes BRUT distinctes:", len(seen))
res = Counter(); valid=[]
for h, src in seen.items():
    bits = [(b >> k) & 1 for b in bytes.fromhex(h) for k in range(7,-1,-1)]
    # la FIFO commence juste apres 8FF7C13C ; on teste aussi toutes les positions ou 8FF7C13C reapparait
    starts = [0] + [i+32 for i,e in find(bits, ADDR, 0)]
    for st in starts:
        stream = ADDR[30:] + bits[st:]
        ln, pid, noack = toint(stream[:6]), toint(stream[6:8]), stream[8]
        if ln > 3 or len(stream) < 9+8*ln+16: res[("len?", ln)] += 1; continue
        good = crc(TRUE_ADDR + stream[:9+8*ln]) == toint(stream[9+8*ln:9+8*ln+16])
        res[(ln, good)] += 1
        if good: valid.append((src, st, ln, pid, noack, hexb(stream[9:9+8*ln])))
print(res)
fam = Counter((v[2], v[5]) for v in valid)
for (ln, pay), c in sorted(fam.items()): print("  len=%d charge=[%s] x%d" % (ln, pay, c))
