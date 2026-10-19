from priority import low_priority
"""Transformers port: private disc-only, relocatable single-pair backend.
No captured image or path profile is consumed by generation.
"""
import ctypes, hashlib, importlib.util, json, shutil, subprocess, sys, os
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
S=Path(__file__).parent/'toolchain'; O=Path.cwd()/'offline'; G=O/'generated'
def main():
 low_priority()
 G.mkdir(parents=True,exist_ok=True)
 for d in ('native','reference','shim'):shutil.copytree(S/d,O/d,dirs_exist_ok=True)
 for n in ('translate.py','census.py','reference.cpp','native.cpp','api.h','harness.cpp'):shutil.copy2(S/n,O/n)
 shutil.copy2(Path(os.environ.get('VU1DISC_DECODER',S/'decode.exe')),O/'decode.exe')
 # Generate the current generic pair templates from a source-only seed.
 for n in ('upper_template.inc','lower_template.inc'):shutil.copy2(S/'generated'/n,G/n)
 manifest=Path(os.environ.get('VU1DISC_MANIFEST',Path.cwd()/'manifest.json'))
 M=json.loads(manifest.read_text())
 p=M['programs'][0]; b=(manifest.parent/p['file']).read_bytes()
 (G/'seed.code').write_bytes(b+bytes(16384-len(b)))
 (O/'census.json').write_text(json.dumps({'images':[{'id':'seed','entries':list(range(0,len(b),8))}],'dumps':[]}))
 subprocess.run(['python',str(O/'translate.py'),'--static-ready','--event-clock'],cwd=O,check=True)
 sys.path.insert(0,str(O))
 spec=importlib.util.spec_from_file_location('local_translate',O/'translate.py'); mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
 pairs={}; programs=[]
 for p in M['programs']:
  b=(manifest.parent/p['file']).read_bytes(); h=hashlib.sha256(b).hexdigest(); assert h==p['sha256']
  f=G/(h+'.code');f.write_bytes(b+bytes(16384-len(b)))
  ds=json.loads(subprocess.run([str(O/'decode.exe'),str(f)],capture_output=True,text=True,check=True).stdout)
  ids=[]
  for d in ds[:len(b)//8]:
   key=(d['lo'],d['up']); idx=len(pairs)
   if key not in pairs:pairs[key]=(idx,d)
   ids.append(pairs[key][0])
  programs.append({'sha256':h,'name':p['name'],'bytes':len(b),'pairs':ids})
 sources=[]; items=list(pairs.items())
 for start in range(0,len(items),48):
  lines=['#include "common.h"']
  for (lo,up),(i,d) in items[start:start+48]:
   lines += [f'static constexpr StaticVU::DecodedInstructionPair d{i}={mod.meta(d)};',
    f'bool sp{i}(StaticVU& v,uint32_t cs,uint8_t* data,uint32_t size,GS& gs,PS2Memory* memory,uint64_t end,bool& ended){{return v.pairStatic<d{i}>(cs,data,size,gs,memory,end,ended,0);}}']
  n=f'static_{start//48:03d}.cpp';(G/n).write_text('\n'.join(lines));sources.append(n)
 registry=['#include "runtime/ps2_vu1.h"','#include "runtime/ps2_memory.h"','#include "runtime/gs/gs_frontend.h"','#include <array>','#include <cstring>','#include <unordered_map>','#include <vector>',
 'using Block=bool(*)(StaticVU&,uint32_t,uint8_t*,uint32_t,GS&,PS2Memory*,uint64_t,bool&);']
 registry += [f'bool sp{i}(StaticVU&,uint32_t,uint8_t*,uint32_t,GS&,PS2Memory*,uint64_t,bool&);' for _,(i,d) in items]
 registry += ['static const Block blocks[]={'+','.join('sp'+str(i) for _,(i,d) in items)+'};',
 'static const uint64_t words[]={'+','.join(str(lo|(up<<32))+'ull' for (lo,up),_ in items)+'};',
 'static const bool reservedPairs[]={'+','.join('true' if d['l']['reserved'] or d['u']['reserved'] else 'false' for _,(i,d) in items)+'};']
 for n,p in enumerate(programs):registry += [f'static const unsigned program{n}[]={{'+','.join(map(str,p['pairs']))+'};']
 registry += ['struct Program{const unsigned* pairs;unsigned size;};','static const Program programs[]={'+','.join('{program'+str(n)+','+str(len(p['pairs']))+'}' for n,p in enumerate(programs))+'};',
 '''// Transformers port: range residency is refreshed after MPG/direct-store generations.
 // Partial/evicted programs are admitted only as byte-identical contiguous spans.
 struct Binding{int pair=-1;int program=-1;unsigned relative=0;unsigned spanEnd=0;};
 static thread_local std::array<Binding,2048> resident;
 static thread_local const uint8_t* activeCode=nullptr;
 static thread_local const PS2Memory* activeMemory=nullptr;
 static thread_local uint64_t activeGeneration=0;
 static const auto candidates=[]{
  std::unordered_map<uint64_t,std::vector<std::pair<unsigned,unsigned>>> out;
  for(unsigned p=0;p<std::size(programs);++p)for(unsigned r=0;r<programs[p].size;++r)
   out[words[programs[p].pairs[r]]].push_back({p,r});return out;
 }();
 static uint64_t word(const uint8_t* code,unsigned pc){uint64_t w;std::memcpy(&w,code+pc*8,8);return w;}
 bool ps2Vu1StaticPrepare(const uint8_t* code,unsigned size,const PS2Memory* memory,uint64_t generation,unsigned pc){
  if(!code||size!=16384||pc>=16384||(pc&7))return false;
  if(code!=activeCode||memory!=activeMemory||generation!=activeGeneration){
   activeCode=code;activeMemory=memory;activeGeneration=generation;resident.fill(Binding{});
   for(unsigned at=0;at<2048;){
    auto it=candidates.find(word(code,at));unsigned best=0,bp=0,br=0;
    if(it!=candidates.end())for(auto [p,r]:it->second){
     unsigned len=0;while(at+len<2048&&r+len<programs[p].size&&word(code,at+len)==words[programs[p].pairs[r+len]])++len;
     if(len>best){best=len;bp=p;br=r;}
    }
    if(!best){++at;continue;}
    for(unsigned j=0;j<best;++j)resident[at+j]={int(programs[bp].pairs[br+j]),int(bp),br+j,at+best};
    at+=best;
   }
  }
  return resident[pc/8].pair>=0 && !reservedPairs[resident[pc/8].pair];
 }
 bool StaticVU::aotDispatch(uint32_t cs,uint8_t* data,uint32_t size,GS& gs,PS2Memory* memory,uint64_t end,bool& ended){
  unsigned pc=m_state.pc;if(cs!=16384||pc>=cs||(pc&7)||!activeCode)return false;
  const auto& b=resident[pc/8];
  // One pair is the complete compiled span. Never read a neighbour or masked tail.
  if(b.pair<0||reservedPairs[b.pair]||pc/8>=b.spanEnd||word(activeCode,pc/8)!=words[b.pair])return false;
  return blocks[b.pair](*this,cs,data,size,gs,memory,end,ended);
 }
 ''']
 # Keep the proven fast-register visibility, scheduler exit, and safe interpreter handover.
 original=(G/'registry.cpp').read_text(); fast=original[original.index('void AotVU::aotFast'):]
 fast=fast.replace('aotFlagsImmediate=flagImmediate[aotImage];','aotFlagsImmediate=false;')
 registry.append(fast)
 (G/'registry.cpp').write_text('\n'.join(registry));sources.append('registry.cpp')
 # A distinct class and external API permits coexistence with the shipping package.
 for base in (O/'native',G):
  for f in base.rglob('*'):
   if f.suffix in ('.cpp','.h','.inc'):f.write_text(f.read_text().replace('AotVU','StaticVU'))
 core=O/'native/src/lib/vu/ps2_vu1_core.cpp';s=core.read_text()
 s=s.replace('#include "../../../../generated/batch7.h" // Transformers port: private package only','// Transformers port: static fragments do not use recorded batch DAGs.')
 s=s.replace('aot7Batch(*this,vuData,dataSize,budgetEnd-kFastTailCycles,programEnded)','false')
 core.write_text(s)
 native=(O/'native.cpp').read_text().replace('AotVU','StaticVU').replace('v.aotImage=image;', 'v.aotImage=0;ps2Vu1StaticPrepare(stable.data(),16384,&m,m.gen1,(h[0]&0x80000000)?*reinterpret_cast<const uint32_t*>(initial+608):h[0]);')
 native=native.replace('Output native(', 'bool ps2Vu1StaticPrepare(const uint8_t*,unsigned,const PS2Memory*,uint64_t,unsigned);\nOutput native(')
 (O/'native.cpp').write_text(native)
 (G/'manifest.json').write_text(json.dumps({'sources':sources,'programs':programs,'unique_pairs':len(pairs)},indent=2))
 print('STATIC',len(programs),'programs',len(pairs),'unique pairs',len(sources),'TUs')
if __name__=='__main__':main()
