from pll import *
TRUE_ADDR=[0,1]+ADDR[:30]
W = load(TELE)
for gain in (0.1, 0.25, 0.5):
    for bias in (-0.1, 0.0, 0.1):
        n=v=0
        for k,(s,ns) in W.items():
            bits,conf,pos = slice_bits(s,ns,gain=gain,bias=bias)
            for i,e in find(bits, ADDR, 0):
                n+=1; st = ADDR[30:]+bits[i+32:i+32+60]; ln=toint(st[:6])
                if ln<=3 and crc(TRUE_ADDR+st[:9+8*ln])==toint(st[9+8*ln:25+8*ln]): v+=1
        print("gain %.2f biais %+.1f : %d adresses exactes, %d CRC FFFF valides" % (gain,bias,n,v))
