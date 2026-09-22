from pll import *
from collections import Counter
TRUE_ADDR = [0,1] + ADDR[:30]
# trame que txraw aurait du porter pour etre valide (PID 0, charge C4 FE)
for pid in range(4):
    pcf = tobits(2,6) + tobits(pid,2) + [0]
    pay = tobits(0xC4,8) + tobits(0xFE,8)
    c = crc(TRUE_ADDR + pcf + pay)
    full = ADDR[30:] + pcf[2:] if False else None
    stream = pcf + pay + tobits(c,16)
    # vu depuis 8F : les 2 premiers bits du PCF sont les 2 derniers de 3C (00) ; on affiche la suite
    print("PID %d : CRC=%04X ; octets apres 8FF7C13C (alignes comme txraw) : %s" % (pid, c, hexb(stream[2:] + [0]*8)))
emis = tobits(0x2189FC0C32, 40)
stream = ADDR[30:] + emis
print("txraw 21 89 FC 0C 32 relu en modele PCF : len=%d PID=%d NOACK=%d charge=%s CRC porte=%04X CRC attendu=%04X" % (
    toint(stream[:6]), toint(stream[6:8]), stream[8], hexb(stream[9:25]), toint(stream[25:41]), crc(TRUE_ADDR + stream[:25])))
for path in ("/Users/Majid/Documents/Dev/esp32/benq/logs/async-txraw.log", "/Users/Majid/Documents/Dev/esp32/benq/logs/async-txraw2.log"):
    W = load(path); n = 0; pre = Counter(); bod = Counter()
    for k,(s,ns) in W.items():
        bits,conf,pos = slice_bits(s,ns)
        for i,e in find(bits, ADDR, 2):
            n += 1; pre["".join(map(str,bits[max(0,i-12):i]))] += 1; bod[hexb(bits[i+32:i+72])] += 1
    print(path.split("/")[-1], "fenetres", len(W), "adresses 8FF7C13C (<=2 err)", n, "bits avant:", dict(pre), "contenu:", dict(bod))
