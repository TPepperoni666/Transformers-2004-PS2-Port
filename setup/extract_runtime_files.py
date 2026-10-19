"""Extract the supported disc's private IOP boot files and scripted-route map."""
from pathlib import Path
import argparse, hashlib, json, struct, os, uuid
ROOT=Path(__file__).resolve().parents[1]
ISO_SHA='89fc6ee694303bac0993b72bbdd89be8cc58d25af831b7072b6ec67fad2b37cf'
SECTOR=2048

def extract(iso,work):
    iso=Path(iso).resolve();work=Path(work).resolve()
    if work==ROOT or ROOT in work.parents:raise ValueError('Runtime files must stay outside the source checkout')
    with iso.open('rb') as f:
        if hashlib.file_digest(f,'sha256').hexdigest()!=ISO_SHA:raise ValueError('Unsupported disc SHA256')
        total=iso.stat().st_size
        def read(start,size):
            if start<0 or size<0 or start+size>total:raise ValueError('ISO extent out of bounds')
            f.seek(start);data=f.read(size)
            if len(data)!=size:raise ValueError('Short ISO read')
            return data
        pvd=read(16*SECTOR,SECTOR)
        if pvd[:7]!=b'\x01CD001\x01' or struct.unpack_from('<H',pvd,128)[0]!=SECTOR:raise ValueError('Expected 2048-byte ISO9660')
        files=[];visited=set()
        def walk(lsn,size,prefix):
            if (lsn,size) in visited:raise ValueError('Repeated ISO directory')
            visited.add((lsn,size))
            if size>16*1024*1024:raise ValueError('Implausible directory')
            data=read(lsn*SECTOR,size);pos=0
            while pos<len(data):
                length=data[pos]
                if not length:pos=(pos//SECTOR+1)*SECTOR;continue
                record=data[pos:pos+length]
                if len(record)!=length or length<34:raise ValueError('Malformed ISO directory')
                name=record[33:33+record[32]]
                if name not in (b'\0',b'\1'):
                    name=name.decode('ascii').split(';')[0]
                    if name in ('.','..') or any(c in name for c in '/\\:'):raise ValueError('Unsafe ISO name')
                    start=struct.unpack_from('<I',record,2)[0];count=struct.unpack_from('<I',record,10)[0]
                    path=prefix+'/'+name
                    if record[25]&0x80:raise ValueError('Multi-extent file unsupported')
                    if record[25]&2:walk(start,count,path)
                    else:files.append((path,start,count))
                pos+=length
        root=pvd[156:190];walk(struct.unpack_from('<I',root,2)[0],struct.unpack_from('<I',root,10)[0],'')
        work.mkdir(parents=True,exist_ok=True);inventory={}
        def retain(relative,data):
            target=work/relative;target.parent.mkdir(parents=True,exist_ok=True)
            if target.exists():
                if target.read_bytes()!=data:raise ValueError('Changed private runtime file: '+relative)
            else:
                # Retain interrupted pending files; publish only complete data.
                pending=target.with_name(target.name+'.'+uuid.uuid4().hex+'.pending')
                with pending.open('xb') as out:
                    out.write(data);out.flush();os.fsync(out.fileno())
                os.replace(pending,target)
            inventory[relative]=hashlib.sha256(data).hexdigest()
        selected=[item for item in files if item[0].startswith('/IOP/')]
        if len(selected)!=9:raise ValueError('Expected nine IOP boot files')
        for path,lsn,count in selected:
            if count>16*1024*1024:raise ValueError('Implausible IOP file')
            retain(path.lstrip('/'),read(lsn*SECTOR,count))
        text=''.join(f'{lsn}\t{(count+SECTOR-1)//SECTOR}\t{path}\n' for path,lsn,count in sorted(files,key=lambda x:x[1]))
        retain('isomap.tsv',text.encode('ascii'))
        ledger=(json.dumps({'iso_sha256':ISO_SHA,'files':inventory},indent=2,sort_keys=True)+'\n').encode('ascii')
        retain('runtime-files.json',ledger)
        print(f'Verified {len(selected)} private IOP boot files and {len(files)} ISO-map entries')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--iso',type=Path,required=True);p.add_argument('--work',type=Path,required=True);a=p.parse_args()
    try:extract(a.iso,a.work)
    except (OSError,ValueError) as error:raise SystemExit(f'extract_runtime_files: error: {error}')
