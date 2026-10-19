from priority import low_priority
"""Local-only SLUS_206.68 VU1 static extraction prototype. No runtime/build use.
Outputs game-derived bytes; keep output private. Standard library only.
"""
import argparse, ctypes, hashlib, json, struct, zlib, time, subprocess, sys
from pathlib import Path

def u32(b,p): return struct.unpack_from('<I',b,p)[0]
def sx(x): return x-65536 if x&32768 else x
def sha(b): return hashlib.sha256(b).hexdigest()

class Elf:
    def __init__(self,b):
        self.b=b; h=struct.unpack_from('<16sHHIIIIIHHHHHH',b)
        self.segments=[struct.unpack_from('<8I',b,h[5]+i*h[9]) for i in range(h[10])]
        self.sections=[struct.unpack_from('<10I',b,h[6]+i*h[11]) for i in range(h[12])]
        names=self.sections[h[13]]; st=b[names[4]:names[4]+names[5]]
        self.section_info=[dict(name=st[s[0]:].split(b'\0')[0].decode(),type=s[1],address=s[3],offset=s[4],size=s[5]) for s in self.sections]
    def offset(self,a):
        for typ,off,va,pa,fs,ms,flags,align in self.segments:
            if typ==1 and va<=a<va+fs:return off+a-va
        return None
    def data(self,a,n):
        o=self.offset(a); return self.b[o:o+n] if o is not None else b''
    def string(self,a):return self.data(a,200).split(b'\0')[0].decode('latin1')

def registrations(e, store_pairs=None):
    """Constant propagation through straight-line setup, never executes guest code.
    Dynamic branch-derived IDs remain unknown; source/length/name constants survive.
    """
    out=[]; reg=[None]*32; reg[0]=0; stores=[]
    for sec in e.section_info:
        if sec['name'] not in ('.text','main'):continue
        for p in range(sec['offset'],sec['offset']+sec['size'],4):
            w=u32(e.b,p);op=w>>26;rs=(w>>21)&31;rt=(w>>16)&31;rd=(w>>11)&31;sa=(w>>6)&31;fn=w&63;imm=w&65535
            pc=sec['address']+p-sec['offset']
            if op==15:reg[rt]=imm<<16
            elif op in (9,25):reg[rt]=None if reg[rs] is None else (reg[rs]+sx(imm))&0xffffffff
            elif op==13:reg[rt]=None if reg[rs] is None else reg[rs]|imm
            elif op==12:reg[rt]=None if reg[rs] is None else reg[rs]&imm
            elif op==0:
                a,b=reg[rs],reg[rt]
                if fn in (33,45):reg[rd]=(a+b)&0xffffffff if a is not None and b is not None else None
                elif fn==35:reg[rd]=(a-b)&0xffffffff if a is not None and b is not None else None
                elif fn==37:reg[rd]=a|b if a is not None and b is not None else None
                elif fn in (0,2,3):reg[rd]=None if b is None else ((b<<sa)&0xffffffff if fn==0 else (b>>sa))
                elif fn not in (8,12,15,24,25,26,27):reg[rd]=None
            elif op in (32,33,35,36,37,55):reg[rt]=None
            if op==43 and reg[rt] is not None and 0x5dcb80<=reg[rt]<=0x604840:
                for sp,base,disp,value in stores[-12:]:
                    if pc-sp<=128 and base==rs and imm==disp+4 and value<reg[rt] and (reg[rt]-value)%16==0 and reg[rt]-value<32768:
                        if store_pairs is not None:store_pairs.append(dict(descriptor_setup_pc=sp,end_setup_pc=pc,source_address=value,size=reg[rt]-value))
                stores.append((pc,rs,imm,reg[rt]))
            if op==3:
                target=(w&0x3ffffff)*4
                # Delay slots in these callers set name or size; apply supported forms.
                d=u32(e.b,p+4);dop=d>>26;dr=(d>>21)&31;dt=(d>>16)&31;di=d&65535
                args=reg.copy()
                if dop==9:args[dt]=None if args[dr] is None else (args[dr]+sx(di))&0xffffffff
                elif dop==0 and (d&63)==3:args[(d>>11)&31]=None if args[dt] is None else args[dt]>>((d>>6)&31)
                if target==0x2256b0:
                    start,qwc,name=args[5],args[6],args[9]
                    if start is not None and qwc is not None and 0<qwc<4096 and e.offset(start) is not None:
                        out.append(dict(registration_pc=pc,source_address=start,size=qwc*16,name=e.string(name) if name else '',registration_id=args[7],fixed_field=args[8]))
                if target==0x251880:
                    start,end=args[10],args[11]
                    if start is not None and end is not None and 0<end-start<32768 and e.offset(start) is not None:
                        out.append(dict(registration_pc=pc,source_address=start,size=end-start,name=e.string(args[5]) if args[5] else '',registration_id=None,fixed_field=0,registration_kind='named_descriptor_factory'))
                # Return values unknown. Keep args to allow a following setup to reuse them.
                reg[2]=reg[3]=None
            reg[0]=0
    return out

