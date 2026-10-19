"""Emit explicit __m128/int locals and value-based FMAC helpers.

Run immediately after block_optimize, not after the aggregate-local experiment.
"""
from pathlib import Path
import re,json,sys
from census import low_priority
from translate import refs,writes,bits
R=Path(__file__).resolve().parent;G=R/'generated'

def body_at(s,a):
 if re.match(r'if\s*\(',s[a:]):
  paren=s.index('(',a);depth=1;n=paren+1
  while depth:
   if s[n]=='(':depth+=1
   elif s[n]==')':depth-=1
   n+=1
  b=s.index('{',n)
 else:b=s.index('{',a)
 depth=1;n=b+1
 while depth:
  if s[n]=='{':depth+=1
  elif s[n]=='}':depth-=1
  n+=1
 return b,n

def select_switch(s,key,value):
 marker='switch ('+key+')'
 a=s.index(marker);b,end=body_at(s,a)
 # Locate only labels at this switch's own brace depth.
 labels=[];depth=0
 for m in re.finditer(r'[{}]|\bcase\s+(0x[0-9A-Fa-f]+|\d+)\s*:|\bdefault\s*:',s[b+1:end-1]):
  token=m[0]
  if token=='{':depth+=1
  elif token=='}':depth-=1
  elif depth==0:labels.append((b+1+m.start(),b+1+m.end(),int(m[1],0) if m[1] else None))
 idx=next((i for i,l in enumerate(labels) if l[2]==value),next((i for i,l in enumerate(labels) if l[2] is None),-1))
 if idx<0:raise ValueError((key,value))
 start=labels[idx][1];finish=end-1
 # Grouped labels fall through only to their common body.
 for following in labels[idx+1:]:
  gap=re.sub(r'//[^\n]*|/\*.*?\*/','',s[start:following[0]],flags=re.S).strip()
  if not gap:start=following[1]
  else:finish=following[0];break
 return s[:a]+'{'+s[start:finish]+'}'+s[end:]

def fmacc(u):
 op=u&63;sel=((u&3)|((u>>4)&124)) if op>=60 else op
 return (op<=47 or op>=60) and (sel<=15 or sel in (24,25,26,27,28,30,32,33,34,35,36,37,38,39,40,41,42,44,45,46))

def lower_code(d,n):
 l=d['lo'];u=d['up']
 if d['i']:return f'local_i=std::bit_cast<float>({l}u);'
 if l in (0,0x8000033c):return ''
 s=(G/'lower_template.inc').read_text();s=s[s.index('{',s.index('inline void AotVU::lowerStatic'))+1:s.rindex('}')]
 s=select_switch(s,'opHi',(l>>25)&127)
 if 'switch (funct)' in s:s=select_switch(s,'funct',l&63)
 if 'switch (funct2)' in s:s=select_switch(s,'funct2',(l&3)|((l>>4)&124))
 # All selected bodies use only compile-time register indices.
 reg={'it':(l>>16)&31,'is':(l>>11)&31,'vfT':(l>>16)&31,'vfS':(l>>11)&31,'viT':(l>>16)&15,'viS':(l>>11)&15,'viD':(l>>6)&15}
 # it/is mean VI in most primaries; the VF operand lives only in dest/memcpy contexts.
 def vf(m):
  key=m[1];r=reg[key] if key in reg else int(key);return f'vf_{r}'
 s=re.sub(r'm_state\.vf\[(\w+)\]',vf,s)
 def vi(m):
  key=m[1];r=(reg[key]&15) if key in reg else int(key);return f'vi_{r}'
 s=re.sub(r'm_state\.vi\[(\w+)\]',vi,s)
 for key,field in [('p','local_p'),('r','local_r'),('top','local_top'),('itop','local_itop')]:s=s.replace('m_state.'+key,field)
 # SIMD registers never have their addresses passed to a helper.
 s=re.sub(r'std::memcpy\((tmp|words),\s*(vf_\d+),\s*(16|sizeof\(words\))\);',lambda m:f'_mm_storeu_ps(reinterpret_cast<float*>({m[1]}),{m[2]});',s)
 s=re.sub(r'std::memcpy\(&fval,\s*&(vf_\d+)\[comp\],\s*4\);',lambda m:f'fval=vectorLaneBits({m[1]},comp);',s)
 s=re.sub(r'std::memcpy\(&bits,\s*&(vf_\d+)\[component\],\s*sizeof\(bits\)\);',lambda m:f'bits=vectorLaneBits({m[1]},component);',s)
 s=re.sub(r'(vf_\d+)\[([^\]]+)\]',r'vectorLane(\1,\2)',s)
 def dest(m):
  if m[1]=='vf_0':return '(void)0;'
  return f'{m[1]}=vectorMerge({m[1]},_mm_loadu_ps({m[2]}),dest);'
 s=re.sub(r'destStatic\((vf_\d+),\s*(\w+),\s*dest\);',dest,s)
 s=s.replace('storeStatic(','v.storeStatic(')
 for helper in ['queueFcset','queueFsset']:
  s=s.replace(helper+'(','v.'+helper+'(')
 for key in ('mac','clip','status'):s=s.replace('m_state.'+key,'v.m_state.'+key)
 for helper in ['queueQ','queueP','startXgkick','readBranchVi']:
  if helper+'(' in s:raise ValueError('unsupported lower '+helper)
 if re.search(r'(?<!v\.)m_state\.',s):raise ValueError('unsupported lower flags/control')
 s=re.sub(r'\bL\b',str(l)+'u',s);s=re.sub(r'\bU\b',str(u)+'u',s)
 s=s.replace('dataSize','size').replace('vuData','data').replace('return;',f'goto lower_done_{n};')
 return '{\n'+s+f'\n}}lower_done_{n}:;'

def main():
 low_priority()
 f=(G/'fmac_template.inc').read_text();fn=f[f.index('template<uint32_t U,bool NS'):]
 # Strip state/commit handling: arithmetic returns a value and classifications.
 a=fn.index('    m_currentUpperInstruction = U;',fn.index('    m_currentUpperInstruction = U;')+1)
 fn=fn[a:];fn=fn.replace('    m_currentUpperInstruction = U;','')
 fn=fn.replace('        return true; // reference: result computed, no flag entry, no lane written','        return {};')
 fn=fn.replace('_mm_loadu_ps(m_state.vf[fs])','rawS').replace('_mm_loadu_ps(m_state.vf[ft])','rawT').replace('_mm_loadu_ps(m_state.acc)','rawA')
 fn=fn.replace('m_state.vf[ft][sel & 3u]','vectorLane(rawT,sel&3u)').replace('m_state.q','rawQ').replace('m_state.i','rawI')
 fn=fn.replace('if (given)\n        f = _mm_loadu_ps(given);','')
 a=fn.index('            alignas(16) uint32_t valueBits[4];');b=fn.index('            return true;',a)+len('            return true;')
 fn=fn[:a]+'            return {f,0u,sign,0u,0u,extraSticky};'+fn[b:]
 fn=fn.replace('return fastFmac(U,target,given);','return vectorCold(s,t,a,f,product,kind,src,destBits);')
 helpers='''#include <bit>
struct VectorResult {__m128 value;uint32_t zero,sign,under,over,extra;};
__declspec(noinline) VectorResult vectorCold(__m128,__m128,__m128,__m128,__m128,uint8_t,uint8_t,uint32_t);
__forceinline float vectorLane(__m128 v,unsigned lane){alignas(16) float x[4];_mm_store_ps(x,v);return x[lane];}
__forceinline uint32_t vectorLaneBits(__m128 v,unsigned lane){return std::bit_cast<uint32_t>(vectorLane(v,lane));}
__forceinline __m128 vectorMerge(__m128 old,__m128 value,unsigned mask){const __m128 m=_mm_castsi128_ps(vuLaneMask(kVuRev4[mask]));return _mm_or_ps(_mm_andnot_ps(m,old),_mm_and_ps(m,value));}
template<uint32_t U,bool NS,bool NT,bool NA>
__forceinline VectorResult vectorFmac(__m128 rawS,__m128 rawT,__m128 rawA,float rawQ,float rawI){
 VuFmacKind kind;VuFmacSrc src;uint8_t sel=0;bool toAcc=false;vuDecodeFmac(U,kind,src,sel,toAcc);
'''
 (G/'vector_template.inc').write_text(helpers+fn)
 core=(R/'native/src/lib/vu/ps2_vu1_core.cpp').read_text();a=core.index('    // Double path:',core.index('bool AotVU::fastFmac'));b=core.index('\n}\n',a)
 cold=core[a:b];cold=cold[:cold.index('    alignas(16) uint32_t valueBits[4];')]+'''    return {_mm_castsi128_ps(value),zeroOrUnder,sign,under,over,extraSticky};
'''
 (G/'vector_cold.cpp').write_text('#include "common.h"\nVectorResult vectorCold(__m128 s,__m128 t,__m128 a,__m128 f,__m128 product,uint8_t kind,uint8_t src,uint32_t destBits){\n const bool productSum=kind==VuMadd||kind==VuMsub;\n'+cold+'}\n')
 p=G/'common.h';s=p.read_text();s+='\n#include "vector_template.inc"\n';p.write_text(s)
 ds={}
 for p in G.glob('*_decoded.json'):
  for d in json.loads(p.read_text()):ds.setdefault((d['pc'],d['lo'],d['up']),d)
 plans={p['block']:p for p in json.loads((G/'schedule.json').read_text())};manifest=json.loads((G/'manifest.json').read_text());manifest['sources'].append('vector_cold.cpp')
 optimized=0;npairs=0;rejected={};seeds={}
 for file in manifest['sources']:
  if not file.startswith('unit_'):continue
  p=G/file;s=p.read_text()
  for name,plan in plans.items():
   if f'v.pairBlock<{name}_d[0]' not in s:continue
   a=s.index('static constexpr AotVU::DecodedInstructionPair '+name+'_d[]');b=s.index('};',a)
   metas=re.findall(r'\{(\d+)u,(\d+)u,usage',s[a:b]);block=[ds[(pc,int(lo),int(up))] for pc,(lo,up) in zip(plan['pcs'],metas)][:-plan.get('tail_pairs',1)]
   try:
    lowers=[lower_code(d,n) for n,d in enumerate(block)]
    for d in block:
     u=d['up'];op=u&63;sel=((u&3)|((u>>4)&124)) if op>=60 else op
     raw=(op<60 and (16<=op<=23 or op in (29,31,43,47))) or (op>=60 and sel in range(16,24)) or (op>=60 and sel in (29,31))
     if not fmacc(u) and not (op>=60 and sel in (47,48)) and not raw:raise ValueError('non-FMAC upper')
   except ValueError as e:rejected[str(e)]=rejected.get(str(e),0)+1;continue
   a=s.index('if(v.m_state.pc==',s.index('bool '+name+'('));b=s.index('\n}',a)+2;twin=s[a:b]
   twin=twin.replace(' && !v.m_stopRequested',' && !v.m_fdiv.valid && !v.m_efu[0].valid && !v.m_efu[1].valid && !v.m_stopRequested')
   vf={0};vi={0};wv=set();wi=set();wa=False;loi=False
   for d in block:
    for u in (d['u'],d['l']):
     vf.update(r for r,m in u['read']);vf.add(u['write'][0]);vi.update(bits(u['vr']|u['vw']))
    vf.update((d['up']>>sh)&31 for sh in (11,16))
    for k in writes(d):
     if k[0]=='vf':wv.add(k[1])
     elif k[0]=='vi':wi.add(k[1])
     else:wa=True
    loi|=bool(d['i'])
   # Transformers port lane aot: a lower op with an empty dest mask (e.g. LQ 0x00030000 in the warp-load microcode)
   # reports no VF write but its exact body still names vf_<ft>; raw upper ops name vf_<fd> regardless of dest.
   # Load every named register so the local exists (read-only unless written; exports still use wv only).
   for lc in lowers:vf.update(int(x) for x in re.findall(r'\bvf_(\d+)\b',lc))
   for d in block:
    if not fmacc(d['up']):vf.add((d['up']>>6)&31)
   load=[f'__m128 vf_{r}=_mm_loadu_ps(v.m_state.vf[{r}]);' for r in sorted(vf)]+[f'int32_t vi_{r}=v.m_state.vi[{r}];' for r in sorted(vi)]
   load+=['__m128 local_acc=_mm_loadu_ps(v.m_state.acc);']+[f'auto local_{r}=v.m_state.{r};' for r in ('q','p','i','r','top','itop')]
   at=twin.index(' {')+2;twin=twin[:at]+'\n'+'\n'.join(load)+twin[at:]
   twin=re.sub(r'=v.m_state.vf\[(\d+)\]\[(\d+)\];',r'=vectorLane(vf_\1,\2);',twin)
   for n,d in enumerate(block):
    pattern=r'v\.pairBlock<'+name+r'_d\['+str(n)+r'\],(true|false),(true|false),(true|false)>\(data,size,gs,memory\);'
    m=re.search(pattern,twin);assert m
    code=[];u=d['up'];dest=(u>>21)&15;fs=(u>>11)&31;ft=(u>>16)&31;fd=(u>>6)&31;toacc=(u&63)>=60
    viwrites=[k[1] for k in writes(d) if k[0]=='vi']
    if viwrites:
     r=viwrites[0];lat=d['l']['vil'] or d['l']['lat']
     code += [f'const int32_t old_vi_{n}=vi_{r};',f'const int32_t branch_vi_{n}=v.m_viReady[{r}]>v.m_cycle?v.fastViVisible({r}):old_vi_{n};',f'if(!v.m_fastHasVi[{r}] || v.m_cycle>=v.m_fastReadyVi[{r}])v.m_fastVisVi[{r}]=vi_{r};',f'v.m_fastReadyVi[{r}]=v.m_cycle+{lat}u;v.m_fastHasVi[{r}]=true;']
    target='local_acc' if toacc else f'vf_{fd}'
    if fmacc(u) and dest:
     code += [f'const auto upper_{n}=vectorFmac<{u}u,{m[1]},{m[2]},{m[3]}>(vf_{fs},vf_{ft},local_acc,local_q,local_i);',f'const __m128 merged_{n}=vectorMerge({target},upper_{n}.value,{dest}u);',f'alignas(16) uint32_t value_{n}[4];_mm_store_si128(reinterpret_cast<__m128i*>(value_{n}),_mm_castps_si128(upper_{n}.value));',f'v.commitStatic<{u}u>(v.m_state.vf[0],{str(toacc).lower()},kVuRev4[{dest}],value_{n},upper_{n}.zero,upper_{n}.sign,upper_{n}.under,upper_{n}.over,upper_{n}.extra);']
    raw_target=None
    if not fmacc(u):
     op=u&63;sel=((u&3)|((u>>4)&124)) if op>=60 else op
     if op<60 and (16<=op<=23 or op in (29,31,43,47)):
      rhs=f'_mm_shuffle_ps(vf_{ft},vf_{ft},_MM_SHUFFLE({op&3},{op&3},{op&3},{op&3}))' if 16<=op<=23 else ('_mm_set1_ps(local_i)' if op in (29,31) else f'vf_{ft}')
      maximum=16<=op<=19 or op in (29,43)
      # The exact scalar helper preserves raw encodings, including GIF fields.
      values=','.join(f'vuMinMax(vectorLane(vf_{fs},{c}),vectorLane({rhs},{c}),{str(maximum).lower()})' for c in (3,2,1,0))
      raw_target=f'vf_{fd}' if fd else None
      if raw_target:code += [f'const __m128 merged_{n}=vectorMerge({raw_target},_mm_set_ps({values}),{dest}u);']
     elif op>=60 and sel==29:
      raw_target=f'vf_{ft}' if ft else None
      if raw_target:code += [f'const __m128 merged_{n}=vectorMerge({raw_target},_mm_and_ps(vf_{fs},_mm_castsi128_ps(_mm_set1_epi32(0x7fffffff))),{dest}u);']
     elif op>=60 and 16<=sel<=23:
      shift=(0,4,12,15)[sel&3];raw_target=f'vf_{ft}' if ft else None
      if sel<=19:value=f'_mm_mul_ps(_mm_cvtepi32_ps(_mm_castps_si128(vf_{fs})),_mm_set1_ps(1.0f/{1<<shift}.0f))'
      else:
       vals=','.join(f'vuFloatToInt(vectorLane(vuNormalize4(vf_{fs}),{c}),{1<<shift}.0f)' for c in (3,2,1,0));value=f'_mm_castsi128_ps(_mm_set_epi32({vals}))'
      if raw_target:code += [f'const __m128 merged_{n}=vectorMerge({raw_target},{value},{dest}u);']
     elif op>=60 and sel==31:
      code += [f'const uint32_t limit_bits_{n}=vectorLaneBits(vf_{ft},3);',f'const int32_t limit_{n}=(limit_bits_{n}&0x7f800000u)?int32_t(limit_bits_{n}&0x7fffffffu):0x007fffff;',f'uint32_t clip_{n}=0;']
      for c in range(3):
       for sign in range(2):code += [f'if(std::bit_cast<int32_t>(vectorLaneBits(vf_{fs},{c})^{sign<<31}u)>limit_{n})clip_{n}|={1<<(c*2+sign)}u;']
      code += [f'v.queueClip(clip_{n});']
    code += [lowers[n]]
    if fmacc(u) and dest and (toacc or fd):code += [f'{target}=merged_{n};']
    if raw_target:code += [f'{raw_target}=merged_{n};']
    if viwrites:
     r=viwrites[0];lat=d['l']['vil'] or d['l']['lat']
     code += [f'vi_{r}=static_cast<int16_t>(vi_{r});',f'v.m_viReady[{r}]=v.m_cycle+{lat}u;v.m_fastMaxWriteReady=std::max(v.m_fastMaxWriteReady,v.m_viReady[{r}]);']
     if d['l']['delay']:code += [f'v.recordViWriteForBranch({r},branch_vi_{n});']
     else:code += ['v.m_viBranchBackupValid=false;']
    else:code += ['v.m_viBranchBackupValid=false;']
    code += ['++v.m_cycle;if(v.m_commitCalendar[v.m_cycle&63u])v.commitReadyPipelines();if(v.m_xgkick.active)v.progressXgkick();']
    twin=twin[:m.start()]+'{\n'+'\n'.join(code)+'\n}'+twin[m.end():]
   export=[f'_mm_storeu_ps(v.m_state.vf[{r}],vf_{r});' for r in sorted(wv)]+[f'v.m_state.vi[{r}]=vi_{r};' for r in sorted(wi)]
   if wa:export+=['_mm_storeu_ps(v.m_state.acc,local_acc);']
   if loi:export+=['v.m_state.i=local_i;']
   export+=['v.m_state.r=local_r;']
   at=twin.rindex('v.m_state.pc=');twin=twin[:at]+'\n'.join(export)+'\n'+twin[at:]
   seeds[name]=twin
   s=s[:a]+twin+s[b:];optimized+=1;npairs+=len(block)
  p.write_text(s)
 (G/'manifest.json').write_text(json.dumps(manifest,indent=2));(G/'vector_seed.json').write_text(json.dumps(seeds));(G/'vector_optimization.json').write_text(json.dumps(dict(blocks=optimized,pairs=npairs,rejected=rejected),indent=2));print('Vector twins',optimized,'pairs',npairs,'rejected',rejected)
if __name__=='__main__':main()
