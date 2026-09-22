from pll import *
# 1. balise
A = tobits(0xE1223344, 32)
n=0
for k,(s,ns) in load(BAL).items():
    bits,conf,pos = slice_bits(s,ns)
    for i,e in find(bits,A,2):
        p=bits[i+32:i+112]; c=toint(bits[i+112:i+128]); n+=1
        print("balise fen %d bit %d err %d : %s CRC %04X calc FFFF(adr+charge)=%04X" % (k,i,e,hexb(p),c,crc(A+p)))
print("balise:",n)
fr = frames()
print("tele: adresses exactes", len(fr), "; <=2 erreurs", len(frames(maxerr=2)))
