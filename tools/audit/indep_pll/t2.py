from pll import *
from collections import Counter
fr = frames()
cmd = [r for r in fr if toint(r['f'][:4])==2]
ack = [r for r in fr if toint(r['f'][:4])==0]
print("commandes", len(cmd), "accuses", len(ack))
def Kof(sel, Ld):
    return Counter(toint(r['f'][Ld:Ld+16]) ^ crc(r['f'][:Ld], 0) for r in sel)
PRE = [0,1,0,1,0,1,0,1]    # 8 bits avant 8F, identiques sur toutes les trames (a verifier)
print("8 bits avant l'adresse :", Counter("".join(map(str,r['pre'][-12:])) for r in fr))
for name, sel, Lds in (("CMD", cmd, (22,23,24)), ("ACK", ack, (5,6,7,8,9))):
    for Ld in Lds:
        K, c = Kof(sel, Ld).most_common(1)[0]
        # etat juste apres les 32 bits de 8FF7C13C : remonter Ld zeros
        S = K
        for _ in range(Ld): S = unstep(S, 0)
        # remonter l'adresse puis le preambule 01010101, afficher les etats
        states = [S]; c2 = S
        seq = ADDR[::-1] + PRE[::-1]
        for b in seq:
            c2 = unstep(c2, b); states.append(c2)
        # states[j] = etat avant les j derniers bits (adresse+preambule)
        marks = [j for j,x in enumerate(states) if x in (0xFFFF, 0xEFDF, 0x0000)]
        print("%s Ld=%2d K=%04X x%d/%d  etat apres adresse=%04X  avant adresse=%04X  avant adr-1bit=%04X avant adr-2bits=%04X  | FFFF/EFDF/0000 atteint a j=%s" % (
            name, Ld, K, c, len(sel), S, states[32], states[33], states[34], [(j, "%04X"%states[j]) for j in marks]))
