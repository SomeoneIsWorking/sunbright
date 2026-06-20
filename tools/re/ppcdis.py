#!/usr/bin/env python3
"""PPC (Gekko/BE32) disassembler over the extracted SMS DOL, with branch-target
resolution and symbol names from reference/sms_gmse01_funcs.txt.
Usage: ppcdis.py <addr> [count]   (addr hex, count instrs, default until blr)
       ppcdis.py --data <addr> <nbytes>
"""
import struct, sys, os, bisect
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

ROOT=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOL=os.path.join(ROOT,'scratch/bin/sms.dol')
FUNCS=os.path.join(ROOT,'reference/sms_gmse01_funcs.txt')

d=open(DOL,'rb').read()
off=struct.unpack('>18I', d[0:72]); addr=struct.unpack('>18I', d[72:144]); size=struct.unpack('>18I', d[144:216])
segs=[(addr[i],addr[i]+size[i],off[i]) for i in range(18) if size[i]]

def read(va,n):
    for a0,a1,fo in segs:
        if a0<=va<a1:
            o=fo+(va-a0); return d[o:o+min(n,a1-va)]
    return b''

syms={}
sa=[]
if os.path.exists(FUNCS):
    for line in open(FUNCS):
        p=line.split()
        if len(p)>=2:
            try: a=int(p[0],16)
            except: continue
            syms[a]=p[1]; sa.append(a)
    sa.sort()

def sym(va):
    if va in syms: return syms[va]
    i=bisect.bisect_right(sa,va)-1
    if i>=0 and va-sa[i]<0x4000: return f"{syms[sa[i]]}+{va-sa[i]:#x}"
    return f"{va:#x}"

md=Cs(CS_ARCH_PPC, CS_MODE_32|CS_MODE_BIG_ENDIAN)
md.detail=False

def disasm(va,count):
    n=0
    while True:
        b=read(va,4)
        if len(b)<4: break
        ins=list(md.disasm(b,va))
        if not ins:
            w=struct.unpack('>I',b)[0]; print(f"{va:08x}: {w:08x}  .word"); va+=4; n+=1
            if count and n>=count: break
            continue
        i=ins[0]
        line=f"{va:08x}: {i.mnemonic:<8} {i.op_str}"
        # resolve branch targets
        if i.mnemonic.startswith('b') and '0x' in i.op_str:
            try:
                tgt=int(i.op_str.split('0x')[-1].split(',')[0],16)
                if tgt in syms or (sa and tgt>=sa[0]): line+=f"   ; -> {sym(tgt)}"
            except: pass
        print(line)
        va+=4; n+=1
        if count and n>=count: break
        if not count and i.mnemonic in ('blr','rfi') : break

if sys.argv[1]=='--data':
    va=int(sys.argv[2],16); n=int(sys.argv[3],0); b=read(va,n)
    for o in range(0,len(b),16):
        chunk=b[o:o+16]
        hexs=' '.join(f"{x:02x}" for x in chunk)
        words=' '.join(f"{struct.unpack('>I',chunk[j:j+4])[0]:08x}" for j in range(0,len(chunk)-3,4))
        print(f"{va+o:08x}: {words}    {hexs}")
else:
    va=int(sys.argv[1],16); count=int(sys.argv[2],0) if len(sys.argv)>2 else 0
    disasm(va,count)
