import os, time, termios, select
scramble=[0xe3,0xb1,0x4b,0xea,0x85,0xbc,0xe5,0x66,0x0d,0xae,0x8c,0x88,0x12,0x69,0xee,0x1f,
0xc7,0x62,0x97,0xd5,0x0b,0x79,0xca,0xcc,0x1b,0x5d,0x19,0x10,0x24,0xd3,0xdc,0x3f,0x8e,0xc5,0x2f]
xorout_sc=[0x0000,0x3448,0x9BA7,0x8BBB,0x85E1,0x3E8C,0x451E,0x18E6,0x6B24,0xE7AB,0x3828,0x814B,
0xD461,0xF494,0x2503,0x691D,0xFE8B,0x9BA7,0x8B17,0x2920,0x8B5F,0x61B1,0xD391,0x7401,0x2138,0x129F,0xB3A0,0x2988]
def brev(b):
    r=0
    for _ in range(8): r=(r<<1)|(b&1); b>>=1
    return r
def upd(crc,a):
    crc^=(a<<8)&0xffff
    for _ in range(8): crc=((crc<<1)^0x1021)&0xffff if crc&0x8000 else (crc<<1)&0xffff
    return crc
def decode(p):
    crc=0xb5d2
    for i in range(len(p)-2):
        crc=upd(crc,p[i])
        if i>=3 and (i-2)<len(xorout_sc):
            cx=crc^xorout_sc[i-2]
            if (cx>>8)==p[i+1] and (cx&0xff)==p[i+2]: return i+1
    return None
port='/dev/cu.usbmodem1122301'
fd=os.open(port, os.O_RDWR|os.O_NONBLOCK)
a=termios.tcgetattr(fd); a[0]=0;a[1]=0;a[3]=0
a[2]=termios.CREAD|termios.CLOCAL|termios.CS8; a[4]=a[5]=termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a)
os.write(fd, b'a\n'); time.sleep(0.2)
buf=b''; end=time.time()+45; rows=[]
while time.time()<end:
    r,_,_=select.select([fd],[],[],0.3)
    if r:
        try: d=os.read(fd,4096); buf+=d if d else b''
        except OSError: pass
    while b'\n' in buf:
        line,buf=buf.split(b'\n',1)
        s=line.decode('ascii','replace').strip()
        if not s.startswith('C'): continue
        parts=s.split()
        if len(parts)<2: continue
        try: ch=int(parts[0][1:]); raw=bytes.fromhex(parts[1])
        except: continue
        if decode(raw)==5: rows.append((round(time.time()%1000,2),ch,brev(raw[4]^scramble[4]),parts[1][:14]))
os.close(fd)
with open('wheel_map.csv','w') as f:
    f.write("t,ch,payload,raw7\n")
    for t,ch,p,r in rows: f.write(f"{t},{ch},{p},{r}\n")
from collections import Counter
print(f"DONE. valid packets: {len(rows)}")
print("payload histogram:", {f'0x{k:02X}':v for k,v in sorted(Counter(p for _,_,p,_ in rows).items())})
print("channel histogram:", dict(sorted(Counter(c for _,c,_,_ in rows).items())))
