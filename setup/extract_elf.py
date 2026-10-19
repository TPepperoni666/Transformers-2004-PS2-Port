"""Extract only the NTSC-U boot ELF from a user's ISO9660 disc image.

No mounting, network, whole-disc extraction or third-party Python dependency.
Raw 2352-byte images, compressed formats and other game versions are unsupported.
"""
from pathlib import Path
import argparse, struct, hashlib

SECTOR=2048
def extract(iso,out):
    with iso.open('rb') as identity:
        digest=hashlib.file_digest(identity,'sha256').hexdigest()
    if digest!='89fc6ee694303bac0993b72bbdd89be8cc58d25af831b7072b6ec67fad2b37cf':
        raise SystemExit('Refusing unsupported disc SHA256; no output written')
    with iso.open('rb') as f:
        total=iso.stat().st_size
        def read(offset,length):
            if offset<0 or length<0 or offset+length>total:raise ValueError('ISO extent outside image')
            f.seek(offset);data=f.read(length)
            if len(data)!=length:raise ValueError('Short ISO read')
            return data
        def extent(record):
            if len(record)<34:raise ValueError('Truncated directory record')
            start=struct.unpack_from('<I',record,2)[0]*SECTOR
            length=struct.unpack_from('<I',record,10)[0]
            if record[25]&0x80:raise ValueError('Multi-extent ELF unsupported')
            return start,length
        pvd=None
        for sector in range(16,80):
            block=read(sector*SECTOR,SECTOR)
            if block[1:6]!=b'CD001' or block[6]!=1:raise ValueError('Expected an ISO9660 image with 2048-byte sectors')
            if block[0]==1:pvd=block;break
            if block[0]==255:break
        if pvd is None:raise ValueError('No primary volume descriptor')
        root=extent(pvd[156:156+pvd[156]])
        if root[1]>16*1024*1024:raise ValueError('Implausible ISO root directory')
        directory=read(*root);pos=0;found=[]
        while pos<len(directory):
            length=directory[pos]
            if not length:pos=((pos//SECTOR)+1)*SECTOR;continue
            rec=directory[pos:pos+length]
            if len(rec)!=length or length<34:raise ValueError('Malformed ISO directory')
            name=rec[33:33+rec[32]].decode('ascii',errors='strict').split(';')[0]
            if name=='SLUS_206.68' and not rec[25]&2:found.append(extent(rec))
            pos+=length
        if len(found)!=1:raise ValueError('Expected exactly one SLUS_206.68 boot file (Transformers NTSC-U)')
        start,length=found[0]
        if length>64*1024*1024 or length<52:raise ValueError('Implausible boot ELF size')
        data=read(start,length)
        if data[:4]!=b'\x7fELF' or data[4:6]!=b'\x01\x01' or struct.unpack_from('<H',data,18)[0]!=8:
            raise ValueError('Boot file is not a 32-bit little-endian MIPS ELF')
        out.parent.mkdir(parents=True,exist_ok=True)
        with out.open('xb') as target:target.write(data)
        print(f'Extracted {length} bytes into private work directory')
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--iso',type=Path,required=True);a.add_argument('--out',type=Path,required=True);args=a.parse_args()
    source=Path(__file__).resolve().parents[1];dest=args.out.resolve()
    if dest==source or source in dest.parents:raise SystemExit('Extract outside the source tree')
    extract(args.iso.resolve(),dest)
