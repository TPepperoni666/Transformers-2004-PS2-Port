from priority import low_priority
"""Transformers port: disc-only CFG spans, relocatable fused regions.

Uses the accepted generic region emitter, never its captured-image tables or profiles.
Single-pair fragments remain the exact fallback for partial residency/control state.
"""
import ctypes, hashlib, importlib.util, json, re, shutil, subprocess, sys
from pathlib import Path
R=Path(__file__).resolve().parents[2]
O=Path.cwd()/'offline';G=O/'generated'
C=Path(__file__).parent/'toolchain'
sys.path.insert(0,str(C))
spec=importlib.util.spec_from_file_location('region_emitter',C/'aot4_gen.py')
e=importlib.util.module_from_spec(spec);spec.loader.exec_module(e)

def main():
 low_priority()
 m=json.loads((G/'manifest.json').read_text());bodies=[];maps=[];plans=[];decl=[]
 for pi,p in enumerate(m['programs']):
  ds=json.loads(subprocess.run([str(O/'decode.exe'),str(G/(p['sha256']+'.code'))],capture_output=True,text=True,check=True).stdout)[:p['bytes']//8]
  leaders={0};facts={}
  for i,d in enumerate(ds):
   try:facts[i]=e.pair_facts(d)
   except e.Unsupported:leaders.add(i+1);continue
   if facts[i]['lk']=='branch':
    leaders.add(i+2)
    if facts[i]['op'] not in (0x24,0x25):leaders.add(((d['pc']+8+e.imm11(d['lo'])*8)&0x3fff)//8)
   if facts[i]['lk']=='xgkick':leaders.add(i)
   if d['e']:leaders.add(i+2)
  mapping=[(-1,0)]*len(ds);todo=sorted(x for x in leaders if x<len(ds));seen=set()
  while todo:
   at=todo.pop(0)
   if at in seen:continue
   seen.add(at);pairs=[];i=at
   while i<len(ds) and len(pairs)<64 and i in facts:
    d=ds[i];f=facts[i]
    if f['lk']=='branch' or d['e']:
     if i+1 not in facts or facts[i+1]['lk']=='branch' or ds[i+1]['e'] or (d['e'] and f['lk']=='branch'):break
     pairs.extend((d,ds[i+1]));i+=2;break
    if i!=at and i in leaders:break
    pairs.append(d);i+=1
   if i<len(ds) and i>at:todo.append(i);todo.sort()
   if len(pairs)<2:continue
   region=dict(img=pi,pc0=at*8,pairs=pairs,facts=[e.pair_facts(d) for d in pairs],stop='unit' if pairs[-2]['e'] or e.lower_kind(pairs[-2])=='branch' else 'cap',id=len(bodies))
   e.schedule(region);region['loop']=e.loop_ok(region) and sum(f['lk']=='xgkick' for f in region['facts'])<=1
   name='sr'+str(len(bodies))
   try:body=e.emit_region(region,name)
   except e.Unsupported:continue
   # Source PCs become displacement from the admitted guest entry. JR/JALR targets stay absolute.
   body=body.replace('uint64_t base=v.m_cycle;','const uint32_t entryPc=v.m_state.pc;\n uint64_t base=v.m_cycle;',1)
   body=body.replace(f'v.m_state.pc!={at*8}u','v.m_state.pc!=entryPc')
   body=re.sub(r'v\.m_state\.(pc|branchTarget)=(\d+)u;',lambda x:f'v.m_state.{x[1]}=((entryPc+{(int(x[2])-at*8)&0x3fff}u)&0x3fffu);',body)
   for d in pairs:
    op=d['lo']>>25;it=(d['lo']>>16)&15
    if not d['i'] and op in (0x21,0x25) and it:
     body=body.replace(f'vi_{it}={(d["pc"]+16)//8};',f'vi_{it}=int16_t(((entryPc+{d["pc"]+16-at*8}u)&0x3fffu)/8u);')
   body=body.replace('AotVU','StaticVU')
   # The old pair counter only sees residual single-pair work. Count fused pairs too.
   body=re.sub(r'AOT4_CYC\((\d+)u\);',lambda x:x[0]+'AOT_STATIC_PAIRS('+str(sum(t<int(x[1]) for t in region['issues']))+');',body)
   mapping[at]=(len(bodies),len(pairs));bodies.append(body)
   decl.append(f'bool {name}(StaticVU&,uint8_t*,uint32_t,uint64_t,bool&);')
   plans.append(dict(program=pi,relative=at,pairs=len(pairs),loop=region['loop'],cycles=region['L'],exits=region['exits'],kick_count=sum(f['lk']=='xgkick' for f in region['facts'])))
  maps.append(mapping)
 # Generic helpers only: no recorded region bodies or image tables are read here.
 donor=C/'generated'
 for n in ('vector_template.inc','s4_common.h','vector_cold.cpp'):
  (G/n).write_text((donor/n).read_text().replace('AotVU','StaticVU'))
 common=(G/'common.h').read_text()
 common+='''
#if defined(_MSC_VER)
#pragma float_control(precise, on)
#endif
#if defined(VU1_AOT_STATS)
#define AOT_STATIC_PAIRS(n) (v.aotPairs+=(n),v.staticPairs+=(n))
#else
#define AOT_STATIC_PAIRS(n) ((void)0)
#endif
#include "vector_template.inc"
'''
 (G/'common.h').write_text(common)
 units=[]
 for start in range(0,len(bodies),4):
  n=f'spans_{start//4:03d}.cpp';(G/n).write_text('#include "s4_common.h"\n'+'\n'.join(bodies[start:start+4]));units.append(n)
 reg=(G/'registry.cpp').read_text()
 extra=['using Region=bool(*)(StaticVU&,uint8_t*,uint32_t,uint64_t,bool&);']+decl
 extra+=['static const Region regions[]={'+','.join('sr'+str(i) for i in range(len(bodies)))+'};','struct RegionBinding{int id;unsigned pairs;};']
 for pi,mapping in enumerate(maps):extra+=['static const RegionBinding region'+str(pi)+'[]={'+','.join('{'+str(i)+','+str(n)+'}' for i,n in mapping)+'};']
 extra+=['static const RegionBinding* regionTables[]={'+','.join('region'+str(i) for i in range(len(maps)))+'};']
 reg=reg.replace('struct Binding{','\n'.join(extra)+'\nstruct Binding{',1)
 old='return blocks[b.pair](*this,cs,data,size,gs,memory,end,ended);'
 new='''// Transformers port: full-span byte guard before the fused scheduler touches state.
  const auto rb=regionTables[b.program][b.relative];
  if(rb.id>=0 && pc/8+rb.pairs<=b.spanEnd){
   bool exact=true;
   for(unsigned j=0;j<rb.pairs;++j)if(word(activeCode,pc/8+j)!=words[programs[b.program].pairs[b.relative+j]]){exact=false;break;}
   if(exact && regions[rb.id](*this,data,size,end,ended))return true;
  }
  return blocks[b.pair](*this,cs,data,size,gs,memory,end,ended);'''
 assert old in reg;reg=reg.replace(old,new)
 (G/'registry.cpp').write_text(reg)
 m['sources']+=units+['vector_cold.cpp'];m['regions']=plans
 (G/'manifest.json').write_text(json.dumps(m,indent=2))
 print('SPANS',len(bodies),'regions',len(units),'TUs')
if __name__=='__main__':main()
