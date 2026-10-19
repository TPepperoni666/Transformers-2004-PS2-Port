from priority import low_priority
"""Transformers port: generation-cached full-span guards and direct CFG edges.
Consumes only the disc-derived region plan, never runtime profiles.
"""
from pathlib import Path
import json,re
R=Path(__file__).resolve().parents[2];G=Path.cwd()/'offline/generated'
def main():
 m=json.loads((G/'manifest.json').read_text());plans=m['regions'];out=[]
 bodies='\n'.join((G/n).read_text() for n in m['sources'] if n.startswith('spans_'))
 for i,x in enumerate(plans):
  body=re.search(r'bool sr'+str(i)+r'\(.*?(?=\nbool sr|\Z)',bodies,re.S)[0]
  x.setdefault('kick_count',body.count('#define AOT4_XGKICK'))
  if 'exits' not in x:
   x['exits']=list(dict.fromkeys((x['relative']*8+int(n))&0x3fff for n in re.findall(r'v\.m_state\.pc=\(\(entryPc\+(\d+)u\)',body)))
 for pi,p in enumerate(m['programs']):
  regs=[(i,x) for i,x in enumerate(plans) if x['program']==pi]
  starts={x['relative']*8:i for i,x in regs}
  # Source-relative exit addresses are supplied by the accepted emitter.
  lines=['#include "common.h"','bool ps2Vu1StaticSpan(unsigned,unsigned,unsigned,unsigned);']
  lines += [f'bool sr{i}(StaticVU&,uint8_t*,uint32_t,uint64_t,bool&);' for i,x in regs]
  lines += [f'bool sc{pi}(StaticVU& v,uint8_t* data,uint32_t size,uint64_t end,bool& ended,unsigned relative){{',
            'const uint32_t base=(v.m_state.pc-relative*8u)&0x3fffu;', 'bool progressed=false;', 'switch(relative){']
  lines += [f'case {x["relative"]}:goto r{i};' for i,x in regs]
  lines += ['default:return false;}']
  for i,x in regs:
   rel=x['relative'];n=x['pairs']
   lines += [f'r{i}:',f'if(v.m_cycle>=end||ended||v.m_stopRequested)return progressed;',
    f'if(!ps2Vu1StaticSpan(v.m_state.pc,{pi}u,{rel}u,{n}u)||!sr{i}(v,data,size,end,ended))return progressed;',
    'progressed=true;', 'if(ended||v.m_cycle>=end||v.m_stopRequested)return true;']
   # All edges are conditional: a side exit must never execute the guessed successor.
   for pc in x.get('exits',[]):
    if pc in starts:lines.append(f'if(v.m_state.pc==((base+{pc}u)&0x3fffu))goto r{starts[pc]};')
   lines.append('return true;')
  lines.append('}')
  name=f'chain_{pi:03d}.cpp';(G/name).write_text('\n'.join(lines)+'\n');out.append(name)
 reg=(G/'registry.cpp').read_text()
 head=['using Chain=bool(*)(StaticVU&,uint8_t*,uint32_t,uint64_t,bool&,unsigned);']
 head += [f'bool sc{pi}(StaticVU&,uint8_t*,uint32_t,uint64_t,bool&,unsigned);' for pi in range(len(m['programs']))]
 head += ['static const Chain chains[]={'+','.join('sc'+str(pi) for pi in range(len(m['programs'])))+'};']
 head += ['static const bool spanEligible[]={'+','.join('false' if x['kick_count']>1 else 'true' for x in plans)+'};']
 reg=reg.replace('struct Binding{','\n'.join(head)+'\nstruct Binding{',1)
 reg=reg.replace('unsigned spanEnd=0;','unsigned spanEnd=0;bool spanChecked=false;',1)
 a=reg.index('  // Transformers port: full-span byte guard')
 b=reg.index('  return blocks[b.pair]',a)
 reg=reg[:a]+'''  if(chains[b.program](*this,data,size,end,ended,b.relative))return true;
'''+reg[b:]
 marker=' bool StaticVU::aotDispatch('
 helper=''' // Transformers port: generation cache validates the WHOLE consumed region once.
 // Every supported MPG/direct-store/clear changes generation; cheap entry equality also
 // rejects stale-generation entry mutations. No span crosses a residency boundary.
 bool ps2Vu1StaticSpan(unsigned pc,unsigned program,unsigned relative,unsigned count){
  if(pc>=16384||(pc&7)||!activeCode)return false;
  auto& b=resident[pc/8];
  if(b.program!=int(program)||b.relative!=relative||pc/8+count>b.spanEnd)return false;
  const auto rb=regionTables[program][relative];
  if(rb.id<0||!spanEligible[rb.id])return false;
  if(word(activeCode,pc/8)!=words[b.pair])return false;
  if(!b.spanChecked){
   for(unsigned j=0;j<count;++j)
    if(word(activeCode,pc/8+j)!=words[programs[program].pairs[relative+j]])return false;
   b.spanChecked=true;
  }
  return true;
 }
'''
 reg=reg.replace(marker,helper+marker,1)
 (G/'registry.cpp').write_text(reg)
 m['sources']+=out;(G/'manifest.json').write_text(json.dumps(m,indent=2))
 print('Direct CFG chains',len(out))
if __name__=='__main__':main()
