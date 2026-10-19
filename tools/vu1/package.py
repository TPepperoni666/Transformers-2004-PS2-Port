from priority import low_priority
"""One disc-only deterministic extraction/generation command. Outputs stay LOCAL.
Transformers-specific: ELF identity, registration recognizer, dedicated context.
Generic: ISO file access, pair decoder/emitter, residency/CFG/relocation/guards.
"""
import argparse,ctypes,hashlib,importlib.util,json,os,shutil,struct,subprocess,sys
from pathlib import Path
R=Path(__file__).resolve().parents[2]
ELF_SHA256='ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb'
ISO_SHA256='89fc6ee694303bac0993b72bbdd89be8cc58d25af831b7072b6ec67fad2b37cf'
def decoder(work):
 tool=Path(__file__).parent/'toolchain';build=work/'decoder_build';build.mkdir(parents=True,exist_ok=True)
 exe=build/'decode.exe';stamp=build/'source.sha256'
 digest=hashlib.sha256((tool/'source_manifest.json').read_bytes()).hexdigest()
 if exe.exists() and stamp.exists() and stamp.read_text()==digest:return exe
 env={key.upper():value for key,value in os.environ.items()}
 if 'VCTOOLSINSTALLDIR' not in env:
  vcvars=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat')
  if not vcvars.exists():raise SystemExit('Run from an MSVC x64 developer shell; cl.exe and INCLUDE/LIB are required')
  batch=build/'compiler_env.cmd'
  batch.write_text('@echo off\ncall "'+str(vcvars)+'" >nul\nset VCToolsInstallDir\nset INCLUDE\nset LIB\nset LIBPATH\nset PATH\n')
  r=subprocess.run(['cmd.exe','/d','/c',str(batch)],capture_output=True,text=True,check=True,creationflags=0x08004000)
  for line in r.stdout.splitlines():
   if '=' in line and not line.startswith('='):
    key,value=line.split('=',1)
    if key.upper() in ('PATH','LIB','LIBPATH','INCLUDE','VCTOOLSINSTALLDIR'):env[key.upper()]=value
 cl=Path(env['VCTOOLSINSTALLDIR'])/'bin/HostX64/x64/cl.exe'
 sources=[tool/'decode.cpp']+[tool/('reference/src/lib/vu/ps2_vu1_'+x+'.cpp') for x in ('core','upper','lower')];objects=[]
 flags=['/nologo','/std:c++20','/O2','/fp:precise','/EHsc','/DNDEBUG','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX','/I'+str(tool/'shim'),'/I'+str(tool/'reference/include'),'/I'+str(tool/'reference/src/lib/vu')]
 for source in sources:
  obj=build/(source.stem+'.obj');objects.append(obj)
  with obj.with_suffix('.log').open('w') as log:subprocess.run([str(cl)]+flags+['/c',str(source),'/Fo'+str(obj)],env=env,cwd=build,stdout=log,stderr=log,check=True,creationflags=0x4000)
 with (build/'link.log').open('w') as log:subprocess.run([str(cl.with_name('link.exe')),'/nologo','/OUT:'+str(exe)]+list(map(str,objects)),env=env,cwd=build,stdout=log,stderr=log,check=True,creationflags=0x4000)
 stamp.write_text(digest);return exe
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def read_elf(path):
 with path.open('rb') as f:
  if f.read(4)==b'\x7fELF':f.seek(0);return f.read()
  # ISO9660 cooked 2048-byte sectors, no mounting or writes to the image.
  f.seek(16*2048);pvd=f.read(2048)
  if pvd[:7]!=b'\x01CD001\x01':raise ValueError('Expected ELF or cooked ISO9660 image')
  root=pvd[156:];lba,size=struct.unpack_from('<I',root,2)[0],struct.unpack_from('<I',root,10)[0]
  todo=[(lba,size)];seen=set()
  while todo:
   lba,size=todo.pop(0)
   if (lba,size) in seen:continue
   seen.add((lba,size));f.seek(lba*2048);data=f.read(size);at=0
   while at<len(data):
    n=data[at]
    if not n:at=(at//2048+1)*2048;continue
    rec=data[at:at+n];at+=n
    if len(rec)<34:raise ValueError('Truncated directory record')
    name=rec[33:33+rec[32]]
    if name in (b'\0',b'\1'):continue
    start,length=struct.unpack_from('<I',rec,2)[0],struct.unpack_from('<I',rec,10)[0]
    if name.split(b';')[0].upper()==b'SLUS_206.68':f.seek(start*2048);return f.read(length)
    if rec[25]&2:todo.append((start,length))
 raise ValueError('SLUS_206.68 absent from image')
def extract(data,folder):
 x=module('disc_extractor',Path(__file__).with_name('transformers_extract.py'));e=x.Elf(data);setups=[];programs=x.registrations(e,setups)
 lo=min(p['source_address'] for p in programs);hi=max(p['source_address']+p['size'] for p in programs)
 for p in setups:
  if not any(q['source_address']==p['source_address'] and q['size']==p['size'] for q in programs):programs.append(dict(p,name='constant_descriptor_setup'))
 programs.append(dict(source_address=0x606b00,size=0x1420,name='dedicated_context_program',load_address=0))
 for at in range(0,len(data)-8,4):
  a,b=struct.unpack_from('<II',data,at)
  if lo<=a<b<=hi and a%16==0 and b%16==0 and b-a<=32768 and not any(p['source_address']==a and p['size']==b-a for p in programs):programs.append(dict(source_address=a,size=b-a,name='descriptor_candidate'))
 folder.mkdir(parents=True,exist_ok=True);(folder/'programs').mkdir(exist_ok=True)
 for p in programs:
  b=e.data(p['source_address'],p['size']);assert len(b)==p['size'] and len(b)%8==0
  h=hashlib.sha256(b).hexdigest();p.update(offset=e.offset(p['source_address']),sha256=h,file='programs/'+h+'.bin');(folder/p['file']).write_bytes(b)
 manifest=folder/'manifest.json';manifest.write_text(json.dumps(dict(elf_sha256=ELF_SHA256,programs=programs),sort_keys=True,indent=2)+'\n');return manifest
def stage_static(offline,out):
 g=offline/'generated';m=json.loads((g/'manifest.json').read_text());out.mkdir(parents=True,exist_ok=True)
 shutil.copytree(offline/'native',out/'native',dirs_exist_ok=True);(out/'generated').mkdir(exist_ok=True)
 names=m['sources']+[p.name for p in sorted(g.glob('*_template.inc'))]+['common.h','s4_common.h']
 for n in names:
  text=(g/n).read_text()
  if n=='registry.cpp':text=text.split('\n#include <fstream>')[0]
  text=text.replace('../../lib/vu/ps2_vu1_static_api.h','runtime/ps2_vu1.h')
  (out/'generated'/n).write_text(text)
 backend=(Path(__file__).parent/'toolchain/backend_run.inc').read_text().replace('AotVU','StaticVU').replace('ps2Vu1AotRun','ps2Vu1StaticRun').replace('g_aotFallbackCalls','g_staticFallbackCalls')
 (out/'generated/backend.cpp').write_text('#include "runtime/ps2_vu1.h"\n#include "runtime/ps2_memory.h"\n#include "runtime/gs/gs_frontend.h"\n#include <cstring>\n#include <atomic>\nstatic std::atomic<uint64_t> g_staticFallbackCalls{0};\nuint64_t ps2Vu1StaticFallbackCalls(){return g_staticFallbackCalls.load(std::memory_order_relaxed);}\n'+backend)
 sources=['native/src/lib/vu/ps2_vu1_'+x+'.cpp' for x in ('core','upper','lower')]+['generated/'+n for n in m['sources']]+['generated/backend.cpp']
 (out/'sources.cmake').write_text('set(PS2X_VU1_STATIC_SOURCES\n'+'\n'.join('  "${CMAKE_CURRENT_LIST_DIR}/'+n+'"' for n in sources)+'\n)\n')
 hashes={str(p.relative_to(out)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.rglob('*')) if p.is_file() and p.name!='package_manifest.json'}
 (out/'package_manifest.json').write_text(json.dumps(dict(elf_sha256=ELF_SHA256,files=hashes),sort_keys=True,indent=2)+'\n')
 return hashes
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('input',type=Path);ap.add_argument('--out',type=Path);ap.add_argument('--work',type=Path);ap.add_argument('--verify-only',action='store_true');args=ap.parse_args()
 if hasattr(ctypes,'windll'):low_priority()
 with args.input.open('rb') as f:
  is_elf=f.read(4)==b'\x7fELF'
  if not is_elf:
   f.seek(0);digest=hashlib.sha256()
   for chunk in iter(lambda:f.read(8*1024*1024),b''):digest.update(chunk)
   if digest.hexdigest()!=ISO_SHA256:raise SystemExit('Refusing mismatching full disc SHA256; no output written')
 data=read_elf(args.input)
 if hashlib.sha256(data).hexdigest()!=ELF_SHA256:raise SystemExit('Refusing mismatching disc/ELF identity; no output written')
 if args.verify_only:print('INPUT HASH PASS');return
 if args.out is None or args.work is None:ap.error('--out and --work are required for generation')
 out=args.out.resolve();work=args.work.resolve()
 if out==work or out in work.parents or work in out.parents:raise SystemExit('Output and work directories must be separate and disjoint')
 for p in (out,work):
  if p==R or R in p.parents:raise SystemExit('Generated output/work must be outside the source checkout')
  if p.exists() and any(p.iterdir()):raise SystemExit('Use fresh empty output/work directories')
 os.environ['VU1DISC_MANIFEST']=str(extract(data,work/'extracted'))
 os.environ['VU1DISC_DECODER']=str(decoder(work))
 gen=module('pair_generation',Path(__file__).with_name('generate.py'));gen.O=work/'offline';gen.G=gen.O/'generated';gen.main()
 spans=module('span_generation',Path(__file__).with_name('spans.py'));spans.O=gen.O;spans.G=gen.G;spans.main()
 chains=module('span_linking',Path(__file__).with_name('link_spans.py'));chains.G=gen.G;chains.main()
 hashes=stage_static(gen.O,out);print('PACKAGE',len(hashes),'files; deterministic SHA256 inventory written')
if __name__=='__main__':main()
