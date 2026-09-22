def bits(h): return [int(b) for x in bytes.fromhex(h) for b in f"{x:08b}"]
def crc(bs,s=0xFFFF):
    for b in bs:
        fb=((s>>15)&1)^b; s=(s<<1)&0xFFFF
        if fb: s^=0x1021
    return s
def tob(bs): return ''.join(map(str,bs))
A=bits("63FDF04F")
# fenetre 8FF7C13C = A[2:] + 2 bits suivants
print("fenetre", hex(int(tob(bits("01")+bits("8FF7C13C"))[:32],2)))
for pid in range(4):
    pcf=[int(c) for c in f"{2:06b}{pid:02b}0"]
    c=crc(A+pcf+bits("C4FE"))
    fr=A+pcf+bits("C4FE")+[int(x) for x in f"{c:016b}"]
    after=fr[32:]  # 9+16+16 = 41 bits
    # meme chose vue depuis la fenetre 8F.. : bits apres fenetre = fr[34:]
    w=fr[34:]; 
    print("PID",pid,"CRC %04X"%c, "apres fenetre:", hex(int(tob(w[:40]),2)))
    # octets bruts adresse puis PCF+charge+CRC en octets (pour mode brut)
    raw=fr[32:]+[0]*((8-len(fr[32:])%8)%8)
    print("   brut apres adresse:", bytes(int(tob(raw[i:i+8]),2) for i in range(0,len(raw),8)).hex(' ').upper())
# ACK
for pid in range(4):
    pcf=[int(c) for c in f"{0:06b}{pid:02b}1"]
    c=crc(A+pcf); fr=A+pcf+[int(x) for x in f"{c:016b}"]
    w=fr[34:]
    print("ACK PID",pid,"CRC %04X"%c,"apres fenetre:",hex(int(tob(w[:24]),2)))
print("DFBE?", hex(crc([0,1])), hex(crc([0])))
