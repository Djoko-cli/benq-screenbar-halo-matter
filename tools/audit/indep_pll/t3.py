from pll import *
from collections import Counter
P2 = [0,1]                      # 2 derniers bits du preambule observe
TRUE_ADDR = P2 + ADDR[:30]
print("adresse vraie 32 bits (sur l'air) : %08X ; a ecrire dans le BM5602 (ordre inverse) : %s" % (
    toint(TRUE_ADDR), " ".join("%02X" % b for b in toint(TRUE_ADDR).to_bytes(4,'big')[::-1])))
def parse(f_after8F):
    """f = bits apres 8FF7C13C. Trame vraie = TRUE_ADDR + PCF9 + charge + CRC."""
    stream = ADDR[30:] + f_after8F      # 2 derniers bits de 8FF7C13C = debut du PCF
    pcf = stream[:9]
    ln, pid, noack = toint(pcf[:6]), toint(pcf[6:8]), pcf[8]
    return stream, ln, pid, noack
for maxerr in (0, 2):
    fr = frames(maxerr=maxerr)
    ok = Counter(); rows=[]
    for r in fr:
        stream, ln, pid, noack = parse(r['f'])
        if ln > 32: rows.append((r, ln, pid, noack, None, False)); continue
        data = TRUE_ADDR + stream[:9+8*ln]
        c = crc(data, 0xFFFF)
        w = toint(stream[9+8*ln:9+8*ln+16])
        good = (c == w)
        pay = stream[9:9+8*ln]
        rows.append((r, ln, pid, noack, pay, good))
        ok[(ln, good)] += 1
    print("\n=== adresses a <= %d erreur(s) : %d trames ; (longueur PCF, CRC FFFF ok) -> %s" % (maxerr, len(fr), dict(ok)))
    if maxerr == 0:
        for r, ln, pid, noack, pay, good in rows:
            print("  fen %2d bit %4d  PCF len=%d PID=%d NO_ACK=%d  charge=[%s]  CRC=%s  (ancien decoupage %s)" % (
                r['k'], r['i'], ln, pid, noack, hexb(pay) if pay is not None else "?", "OK" if good else "FAUX", hexb(r['f'][:40])))
fr = frames()
starts = sorted((r['k'], r['i'], toint(r['f'][:4])) for r in fr)
print("\necarts commande->accuse (bits entre debuts d'adresse):",
      [b[1]-a[1] for a,b in zip(starts, starts[1:]) if a[0]==b[0] and a[2]==2 and b[2]==0])
