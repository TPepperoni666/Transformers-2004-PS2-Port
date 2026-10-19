"""VU1 AOT round 4: statically scheduled regions on top of the round-3 backend.

Run as the LAST generation step (after bulk_kick.py, before build.py). Reads the round-3 outputs in generated/
(decoded images, schedule.json, registry.cpp image tables, direct_chain.cpp, lower_template.inc,
vector_template.inc) and adds:
  generated/s4_common.h        helpers (exact old-event drain, flag-slot push)
  generated/s4_unit_NNN.cpp    one function per region: bool s4_<img>_<pc>(AotVU&,uint8_t*,uint32_t,uint64_t,bool&)
  generated/direct_chain.cpp   each region start label first tries the region, else runs the round-3 block
  generated/aot4_regions.json  region list / reasons
Round-3 code is untouched and stays the exact fallback for every entry a region declines.

A region is a straight run of pairs from a leader to the end of a branch(+delay) or E(+delay) unit. Its cycle
schedule is computed here, at translate time, from the same rules as VU1Interpreter::runFast/fastReadyCycle:
  issue_i = max(issue_(i-1)+1, in-region producer ready, FDIV/EFU resource ready).
Entry state is only consulted where it could change that schedule, through cheap guards (no live-in stall:
every pending VF/VI write was issued <= base-1 with latency <= 4, so only reads at offsets 0..2 need a check;
no FDIV/EFU/XGKICK resource stall). If a guard fails the region declines with no side effect.
Inside a region VF/VI/ACC/I/R live in host locals, the clock is not stepped, FMAC MAC/STATUS and CLIP results are
kept in locals and only materialised where they are observable (flag-reading instructions, region exit), Q/P
are locals with statically known commit points. Events that were already pending at entry (flag entries,
Q, P) are committed by aot4Drain in cycle order exactly when something could observe them (a reader, the
exit); every region event is later than every older event of the same kind, or commutes with it, so
"drain old events up to t, then overlay region events committed by t" equals the sequential commit order.
At exit the scheduler state is reproduced as runFast would leave it: m_cycle, m_state.cycles, registers, ready
stamps of writes still in flight, fast-path visibility (m_fastHasVf/Vi ...), branch-VI backup, flag/Q/P entries
still in flight (pipeline + calendar), m_efuResourceReady, working clip, XGKICK progress.

Further pieces (all exact; each verified by the harness and the aot4_shadow.py differential fuzzer):
  * pair-0 pre-stall: if only the first pair would stall at entry, runFast's stall (advanceTo + branch-VI bypass
    expiry) is performed in the region; a later decline is still exact (round 3 then finds pair 0 ready).
  * in-place self loops (loop_ok): a region ending in a branch to its own start whose next iteration provably has
    the same schedule keeps all registers in locals across iterations; per iteration only the event part runs,
    in-flight FMAC flag entries are carried in locals when the body has no flag readers.
  * normalisation facts: FMAC/ITOF results are known normal-or-zero, so vuNormalize4 is skipped when they are re-read;
    live-in FMAC operand lanes are checked once at entry (aot4NormOk) and assumed normal (loop fixpoint), a region
    whose live-in operand is denormal/inf/nan declines.
  * vectorFmac4 / aot4Norm1: private copy of the round-3 vectorFmac with force-inlined scalar normalisation and the
    STATUS current bits precomputed per path.
  * AOT4_STATS (standalone harness only, never in the game build): per-region call/enter/decline-reason counters.
Options: --max-pairs=N (48), --tu-pairs=N (96), --no-loops, --no-prestall, --no-specnorm; AOT4_DRY=1 writes nothing.
"""
from pathlib import Path
import json,re,sys,hashlib
from census import low_priority
from vector_optimize import select_switch,fmacc
R=Path(__file__).resolve().parent;G=R/'generated'

MAX_PAIRS=int(next((a.split('=')[1] for a in sys.argv if a.startswith('--max-pairs=')),'128'))   # lane aot5: 48 -> 128 (cap cuts caused live-in declines; TU peak 1.5 GB)
TU_PAIRS=int(next((a.split('=')[1] for a in sys.argv if a.startswith('--tu-pairs=')),'128'))

def lanes(m):return [c for c in range(4) if m&(8>>c)]
REV4=[0x0,0x8,0x4,0xC,0x2,0xA,0x6,0xE,0x1,0x9,0x5,0xD,0x3,0xB,0x7,0xF]
def blend(old,new,dest):
 # lane aot5: vectorMerge(old,new,dest) with a compile-time dest = SSE4.1 blendps (component c = SSE lane c)
 dest&=15
 if dest==15:return new
 if dest==0:return old
 return f'_mm_blend_ps({old},{new},{REV4[dest]})'
MASK_LANES='--no-mask-lanes' not in sys.argv
def vibits(m):return [i for i in range(1,16) if m&(1<<i)]
def imm11(lo):
 x=lo&2047;return x-2048 if x&1024 else x

BRANCH_OPS={0x20,0x21,0x24,0x25,0x28,0x29,0x2c,0x2d,0x2e,0x2f}
COND_OPS={0x28,0x29,0x2c,0x2d,0x2e,0x2f}
FLAG_READ={0x10:'clip',0x12:'clip',0x13:'clip',0x1c:'clip',0x14:'status',0x16:'status',0x17:'status',0x18:'mac',0x1a:'mac',0x1b:'mac'}
EFU_LAT={0x70:11,0x71:18,0x72:18,0x73:24,0x74:54,0x75:54,0x76:12,0x78:12,0x79:18,0x7a:12,0x7c:29,0x7d:54,0x7e:44}  # lane xo3tf / CROSSOVER3 H1: manual slots (0x77/0x7F unassigned)

class Unsupported(Exception):pass

def lower_kind(d):
 lo=d['lo']
 if d['i']:return 'loi'
 if lo in (0,0x8000033c):return 'nop'
 op=(lo>>25)&127
 if op in BRANCH_OPS:return 'branch'
 if op in FLAG_READ:return 'flagread'
 if op in (0x11,0x15):raise Unsupported('fcset/fsset')
 if op==0x40:
  f=lo&63
  if f>=0x3c:
   f2=(lo&3)|((lo>>4)&124)
   if f2 in (0x38,0x39,0x3a):return 'fdiv'
   if f2==0x3b:return 'waitq'
   if f2==0x64:return 'mfp'
   if f2==0x6c:return 'xgkick'
   if f2 in EFU_LAT:return 'efu'
   if f2==0x7b:return 'waitp'
 return 'plain'

def is_store(d):
 lo=d['lo']
 if d['i'] or lo in (0,0x8000033c):return False
 op=(lo>>25)&127
 if op in (0x01,0x05):return True
 if op==0x40 and (lo&63)>=0x3c:
  return ((lo&3)|((lo>>4)&124)) in (0x35,0x37,0x3f)
 return False

def upper_kind(u):
 op=u&63;sel=((u&3)|((u>>4)&124)) if op>=60 else op
 if op>=60 and sel in (47,48):return 'nop'
 if fmacc(u):return 'cross' if sel==46 else 'fmac'
 if op<60 and (16<=op<=23 or op in (29,31,43,47)):return 'minmax'
 if op>=60 and sel==29:return 'abs'
 if op>=60 and 16<=sel<=23:return 'cvt'
 if op>=60 and sel==31:return 'clip'
 raise Unsupported('upper %x'%u)

def fmac_src(u):
 """'q','i','bc','vt','cross' for an arithmetic FMAC upper (vuDecodeFmac)."""
 op=u&63;toacc=op>=60;sel=((u&3)|((u>>4)&124)) if toacc else op
 if sel<=15 or sel in (24,25,26,27):return 'bc'
 if sel in (28,32,33,36,37):return 'q'
 if sel in (30,34,35,38,39):return 'i'
 if sel in (40,41,42,44,45):return 'vt'
 if sel==46:return 'cross'
 raise Unsupported('fmac src')

# ---------------------------------------------------------------- lower body text (round-3 template, locals)
_lower_template=None
_lower6_cache={}
def lower_text(d,n):
 global _lower_template
 key6=(d['lo'],d['up'],n)
 if key6 in _lower6_cache:return _lower6_cache[key6]
 l=d['lo'];u=d['up']
 if _lower_template is None:
  s=(G/'lower_template.inc').read_text();_lower_template=s[s.index('{',s.index('inline void AotVU::lowerStatic'))+1:s.rindex('}')]
 s=_lower_template
 s=select_switch(s,'opHi',(l>>25)&127)
 if 'switch (funct)' in s:s=select_switch(s,'funct',l&63)
 if 'switch (funct2)' in s:s=select_switch(s,'funct2',(l&3)|((l>>4)&124))
 reg={'it':(l>>16)&31,'is':(l>>11)&31,'vfT':(l>>16)&31,'vfS':(l>>11)&31,'viT':(l>>16)&15,'viS':(l>>11)&15,'viD':(l>>6)&15}
 def vf(m):
  key=m[1];r=reg[key] if key in reg else int(key);return f'vf_{r}'
 s=re.sub(r'm_state\.vf\[(\w+)\]',vf,s)
 def vi(m):
  key=m[1];r=(reg[key]&15) if key in reg else int(key);return f'vi_{r}'
 s=re.sub(r'm_state\.vi\[(\w+)\]',vi,s)
 for key,field in [('p','local_p'),('r','local_r'),('top','local_top'),('itop','local_itop')]:s=s.replace('m_state.'+key,field)
 s=re.sub(r'std::memcpy\((tmp|words),\s*(vf_\d+),\s*(16|sizeof\(words\))\);',lambda m:f'_mm_storeu_ps(reinterpret_cast<float*>({m[1]}),{m[2]});',s)
 s=re.sub(r'std::memcpy\(&fval,\s*&(vf_\d+)\[comp\],\s*4\);',lambda m:f'fval=vectorLaneBits({m[1]},comp);',s)
 s=re.sub(r'std::memcpy\(&bits,\s*&(vf_\d+)\[component\],\s*sizeof\(bits\)\);',lambda m:f'bits=vectorLaneBits({m[1]},component);',s)
 s=re.sub(r'(vf_\d+)\[([^\]]+)\]',r'vectorLane(\1,\2)',s)
 def dest(m):
  if m[1]=='vf_0':return '(void)0;'
  return f'{m[1]}={blend(m[1],"_mm_loadu_ps("+m[2]+")",(l>>21)&15)};'
 s=re.sub(r'destStatic\((vf_\d+),\s*(\w+),\s*dest\);',dest,s)
 s=s.replace('storeStatic(addr,words,dest);','AOT4_STORE(addr,words,dest);')
 s=re.sub(r'queueQ\((.*?),\s*(\d+)u,\s*(.*?)\);',lambda m:f'q4_val_{n}=aot4Norm1({m[1]});q4_di_{n}=({m[3]})&0x30u;',s,flags=re.S)
 s=re.sub(r'queueP\((.*),\s*(\d+)u\);',lambda m:f'p4_val_{n}=({m[1]});',s)
 s=re.sub(r'startXgkick\((.*)\);',lambda m:f'AOT4_XGKICK({m[1]});',s)
 s=re.sub(r'v?\.?normalizeResult\((\w+),\s*\w+\)',r'aot4Norm1(\1)',s)   # same bits as normalizeResult's value
 s=s.replace('vuNormalizeOperand(','aot4Norm1(').replace('aotNormalizeOperand(','aot4Norm1(')   # identical bit function, always inlined
 for key in ('mac','clip','status'):s=s.replace('m_state.'+key,f'flag4_{key}_{n}')
 for helper in ['queueFcset','queueFsset','readBranchVi','queueQ','queueP','startXgkick']:
  if re.search(r'(?<![\w.])'+helper+r'\(',s):raise Unsupported('lower helper '+helper)
 if re.search(r'm_state\.',s):raise Unsupported('lower control')
 s=re.sub(r'\bL\b',str(l)+'u',s);s=re.sub(r'\bU\b',str(u)+'u',s)
 s=s.replace('dataSize','size').replace('vuData','data').replace('return;',f'goto lower4_done_{n};')
 result6='{\n'+s+f'\n}}lower4_done_{n}:;'
 _lower6_cache[key6]=result6
 return result6

def simd_lower(d):
 """lane aot5: C++ for MOVE/MR32/MFIR/MFP written with SSE (None = use the round-3 template body)."""
 lo=d['lo']
 if d['i'] or lo in (0,0x8000033c) or (lo>>25)&127!=0x40 or (lo&63)<0x3c:return None
 f2=(lo&3)|((lo>>4)&124);ft=(lo>>16)&31;fs=(lo>>11)&31;dest=(lo>>21)&15;vis=(lo>>11)&15
 if f2 not in (0x30,0x31,0x3d,0x64):return None
 if ft==0:return ''          # writes to vf0 are dropped (destStatic)
 if f2==0x30:val=f'vf_{fs}'
 elif f2==0x31:val=f'_mm_shuffle_ps(vf_{fs},vf_{fs},_MM_SHUFFLE(0,3,2,1))'
 elif f2==0x3d:val=f'_mm_castsi128_ps(_mm_set1_epi32(int32_t(int16_t(vi_{vis}))))'
 else:val='_mm_set1_ps(local_p)'
 return f' vf_{ft}={blend("vf_"+str(ft),val,dest)};'

# ---------------------------------------------------------------- per-pair facts
def pair_facts(d):
 f={}
 f['lk']=lower_kind(d);f['uk']=upper_kind(d['up'])
 if d['d'] or d['t']:raise Unsupported('d/t bit')
 if d['l']['reserved'] or d['u']['reserved']:raise Unsupported('reserved')
 lo=d['lo']
 f['op']=(lo>>25)&127
 f['store']=is_store(d)
 if f['uk'] in ('fmac','cross'):f['src']=fmac_src(d['up'])
 return f

# VF/VI reads/writes as the scheduler sees them (usage tables from the worktree decoder)
def vf_reads(d):
 out=[]
 for u in (d['u'],d['l']):
  for r,m in u['read'][:u['count']]:
   if r:out+= [(r,c) for c in lanes(m)]
 return out
def vi_reads(d):return vibits(d['l']['vr'])
def vf_writes(d):
 out=[]
 lr,lm=d['l']['write']
 if lr and lr!=d['suppress']:out.append(('l',lr,lm,d['l']['vfl'] or d['l']['lat']))
 ur,um=d['u']['write']
 if ur:out.append(('u',ur,um,d['u']['vfl'] or d['u']['lat']))
 return out
def vi_write(d):
 m=d['l']['vw']&0xfffe
 if not m:return None
 r=min(vibits(m));return r,(d['l']['vil'] or d['l']['lat'])

# ---------------------------------------------------------------- region formation
def load():
 man=json.loads((G/'manifest.json').read_text())
 plans={p['block']:p for p in json.loads((G/'schedule.json').read_text())}
 reg=(G/'registry.cpp').read_text()
 tables={int(i):raw.strip().split(',') for i,raw in re.findall(r'static const Block image_(\d+)\[2048\]=\{\s*(.*?)\};',reg,re.S)}
 dec=[json.loads((G/(im['id']+'_decoded.json')).read_text()) for im in man['images']]
 return man,plans,tables,dec

def form_regions(img,image,table,ds,plans,extra=frozenset()):
 starts={}
 for n,name in enumerate(table):
  if name!='nullptr' and plans[name]['pcs'][0]==n*8:starts[n*8]=name
 leaders=set(int(e)&0x3ff8 for e in image['entries'])|set(extra)   # lane aot5: + profiled chain entry pcs
 for n,name in enumerate(table):
  if name=='nullptr':continue
  d=ds[n];op=(d['lo']>>25)&127
  if not d['i'] and op in BRANCH_OPS:
   if op in (0x20,0x21) or op in COND_OPS:leaders.add((d['pc']+8+imm11(d['lo'])*8)&0x3fff)
   leaders.add((d['pc']+16)&0x3fff)
 regions=[];reasons={}
 todo=sorted(leaders);done=set()
 while todo:
  pc0=todo.pop(0)
  if pc0 in done or (pc0 not in starts and pc0 not in extra):continue
  done.add(pc0)
  pairs=[];pc=pc0;stop=None;xg=0
  while True:
   n=pc//8
   if pc>=16376 or table[n]=='nullptr':stop='unreachable/wrap';break
   d=ds[n]
   try:f=pair_facts(d)
   except Unsupported as e:stop=str(e);break
   # lane aot5: cross ops are region code; a second XGKICK outside a delay slot gets a guard exit (emit_region)
   unit=f['lk']=='branch' or d['e']
   if f['lk']=='branch' and d['e']:stop='E on branch';break
   if unit:
    if pc+8>=16376 or table[(pc+8)//8]=='nullptr':stop='delay unreachable';break
    dd=ds[(pc+8)//8]
    try:ff=pair_facts(dd)
    except Unsupported as e:stop='delay '+str(e);break
    if ff['lk']=='branch' or dd['e']:stop='branch/E in delay slot';break
    if ff['lk']=='xgkick' and (xg or f['lk']=='xgkick'):stop='second xgkick';break
    if len(pairs)+2>MAX_PAIRS:stop='cap';break
    pairs+=[d,dd];stop='unit';break
   if f['lk']=='xgkick':xg+=1
   pairs.append(d);pc+=8
   if len(pairs)>=MAX_PAIRS:stop='cap';break
  if stop=='cap':
   # cut at the last round-3 block start in the second half so the remainder has a chain label
   cut=max([k for k in range(max(1,len(pairs)//2),len(pairs)) if pairs[k]['pc'] in starts],default=None)
   if cut is not None:pc=pairs[cut]['pc'];pairs=pairs[:cut]
  reasons[stop]=reasons.get(stop,0)+1
  if stop!='unit' and stop!='unreachable/wrap':
   # restart at the next round-3 block start after the pair that ended this region
   nxt=pc+8 if stop!='cap' else pc
   while nxt<16384 and nxt not in starts:nxt+=8
   if nxt<16384:todo.append(nxt);todo.sort()
  if pairs:regions.append(dict(img=img,pc0=pc0,pairs=pairs,stop=stop))
 return regions,reasons

# ---------------------------------------------------------------- static schedule
def schedule(region):
 pairs=region['pairs']
 vfr={};vir={}                  # (r,c)/r -> ready offset of in-region writes
 fdiv=None                      # (issue,ready) of the latest region FDIV op
 efu=[];efu_res=None            # region EFU (issue,ready); resource ready offset
 issues=[];guards=[];t=0
 first_fdiv=first_efu=first_waitp=None;stalls=[]
 for i,d in enumerate(pairs):
  f=region['facts'][i];ready=t
  live=[]
  for r,c in vf_reads(d):
   if (r,c) in vfr:ready=max(ready,vfr[r,c])
   else:live.append(('vf',r,c))
  for r in vi_reads(d):
   if r in vir:ready=max(ready,vir[r])
   else:live.append(('vi',r))
  pipe=d['l']['pipe']
  if pipe==3:  # FDIV pipe: DIV/SQRT/RSQRT/WAITQ
   if fdiv is not None and fdiv[1]>t:ready=max(ready,fdiv[1])
   if fdiv is None and first_fdiv is None:first_fdiv=('pending',i)
  if pipe==4:  # EFU pipe incl. WAITP
   if efu_res is not None:ready=max(ready,efu_res)
   elif first_efu is None:first_efu=('pending',i)
  if d['l']['wp']:
   if efu:ready=max([ready]+[e[1] for e in efu if e[1]>t])
   elif first_waitp is None:first_waitp=('pending',i)
  issue=ready
  if issue>t:stalls.append(i)
  issues.append(issue)
  # guards that keep entry state from changing the schedule
  for k in live:
   if issue<3:guards.append((k,issue))
  if first_fdiv==('pending',i):first_fdiv=issue
  if first_efu==('pending',i):first_efu=issue
  if first_waitp==('pending',i):first_waitp=issue
  for _,r,m,lat in vf_writes(d):
   for c in lanes(m):vfr[r,c]=issue+lat
  w=vi_write(d)
  if w:vir[w[0]]=issue+w[1]
  if f['lk']=='fdiv':fdiv=(issue,issue+d['l']['lat'])
  if f['lk']=='efu':
   lat=d['l']['lat'];efu.append((issue,issue+lat));efu_res=issue+lat-1
  t=issue+1
 region['issues']=issues;region['L']=t;region['guards']=guards;region['stalls']=stalls
 region['first_fdiv']=first_fdiv if isinstance(first_fdiv,int) else None
 region['first_efu']=first_efu if isinstance(first_efu,int) else None
 region['first_waitp']=first_waitp if isinstance(first_waitp,int) else None

def loop_ok(region):
 """True if the region is a self loop whose next iteration, entered from this iteration's exit state, has the
 same schedule (no loop-carried stall, no FDIV/EFU/WAITP resource stall). XGKICK and budget are checked at run time."""
 pairs=region['pairs'];facts=region['facts'];S=region['issues'];L=region['L']
 br=None
 for i,f in enumerate(facts):
  if f['lk']=='branch':br=i
 if br is None or br!=len(pairs)-2:return False
 d=pairs[br];op=facts[br]['op']
 if op not in (0x20,)+tuple(COND_OPS):return False
 if (d['pc']+8+imm11(d['lo'])*8)&0x3fff!=region['pc0']:return False
 lastw={}
 for i,dd in enumerate(pairs):
  for _,r,m,lat in vf_writes(dd):
   for c in lanes(m):lastw['vf',r,c]=S[i]+lat
  w=vi_write(dd)
  if w:lastw['vi',w[0]]=S[i]+w[1]
 for k,at in region['guards']:
  if k in lastw and lastw[k]-L>at:return False
 fd=[i for i,f in enumerate(facts) if f['lk']=='fdiv']
 if fd and region['first_fdiv'] is not None:
  if S[fd[-1]]+pairs[fd[-1]]['l']['lat']-L>region['first_fdiv']:return False
 ef=[i for i,f in enumerate(facts) if f['lk']=='efu']
 if ef and region['first_efu'] is not None:
  if S[ef[-1]]+pairs[ef[-1]]['l']['lat']-1-L>region['first_efu']:return False
 if ef and region['first_waitp'] is not None:
  for j in ef:
   for k in range(1,60):
    rd=S[j]+pairs[j]['l']['lat']-k*L
    if rd<=0:break
    if rd>region['first_waitp']:return False
 if ef:
  # at most two EFU results may be in flight (m_efu has two slots)
  for t in range(L):
   live=sum(1 for j in ef for k in range(0,60) if S[j]-k*L<=t<S[j]+pairs[j]['l']['lat']-k*L)
   if live>2:return False
 return True

PRODUCT_SUM_SELS=set(range(8,16))|{0x21,0x23,0x25,0x27,0x29,0x2d}
def norm_step(d,f,norm,normacc):
 """Normalisation facts across one pair: returns ((NS,NT,NA) or None, norm', normacc')."""
 u=d['up'];lo=d['lo'];uk=f['uk'];dest=(u>>21)&15;fs=(u>>11)&31;ft=(u>>16)&31;fd=(u>>6)&31;op=u&63;toacc=op>=60
 dec=None
 if uk in ('fmac','cross'):
  src=f['src'];sel=((u&3)|((u>>4)&124)) if toacc else op
  NS=(norm.get(fs,0)&dest)==dest and uk=='fmac'      # cross: shuffled lanes, always normalise
  NT=bool(norm.get(ft,0)&(8>>(sel&3))) if src=='bc' else ((norm.get(ft,0)&dest)==dest if src=='vt' else False)
  NA=(normacc&dest)==dest
  dec=(NS,NT,NA)
 pre_norm=dict(norm);pre_acc=normacc;norm=dict(norm)
 for side,r,m,lat in vf_writes(d):
  if side!='l':continue
  f2=((lo&3)|((lo>>4)&124)) if (not d['i'] and (lo>>25)&127==0x40 and (lo&63)>=0x3c) else -1
  src_=pre_norm.get((lo>>11)&31,0)
  if f2==0x30:nn=src_
  elif f2==0x31:nn=((src_<<1)&0xE)|((src_>>3)&1)
  else:nn=0
  norm[r]=(norm.get(r,0)&~m)|(m&nn)
 if uk in ('fmac','cross','minmax','abs','cvt') and dest:
  if uk in ('fmac','cross'):nn=0xF
  elif uk=='minmax':
   if 16<=op<=23:rh=0xF if pre_norm.get(ft,0)&(8>>(op&3)) else 0
   elif op in (29,31):rh=0
   else:rh=pre_norm.get(ft,0)
   nn=pre_norm.get(fs,0)&rh
  elif uk=='abs':nn=pre_norm.get(fs,0)
  else:nn=0xF if ((u&3)|((u>>4)&124))<=19 else 0
  if uk in ('fmac','cross') and toacc:normacc=(pre_acc&~dest)|dest
  else:
   r=fd if uk in ('fmac','cross','minmax') else ft
   if r:norm[r]=(pre_norm.get(r,0)&~dest)|(dest&nn)
 norm[0]=0xF
 return dec,norm,normacc

def norm_plan(region):
 """Live-in FMAC operand lanes to verify at entry, and the resulting initial facts (fixpoint for loops)."""
 pairs=region['pairs'];facts=region['facts']
 written={};accw=0;cand={};candacc=0
 for d,f in zip(pairs,facts):
  u=d['up']
  if f['uk']=='fmac':
   dest=(u>>21)&15;fs=(u>>11)&31;ft=(u>>16)&31;op=u&63;toacc=op>=60;sel=((u&3)|((u>>4)&124)) if toacc else op
   need=[(fs,dest)]
   if f['src']=='bc':need.append((ft,8>>(sel&3)))
   elif f['src']=='vt':need.append((ft,dest))
   for r,m in need:
    if r:cand[r]=cand.get(r,0)|(m&~written.get(r,0))
   if sel in PRODUCT_SUM_SELS:candacc|=dest&~accw
  for side,r,m,lat in vf_writes(d):written[r]=written.get(r,0)|m
  if d['u']['aw']:accw|=d['u']['aw']
 cand={r:m for r,m in cand.items() if m}
 if region.get('loop'):
  while True:
   norm={0:0xF,**cand};acc=candacc
   for d,f in zip(pairs,facts):_,norm,acc=norm_step(d,f,norm,acc)
   new={r:(m&norm.get(r,0)) if written.get(r,0) else m for r,m in cand.items()}
   new={r:m for r,m in new.items() if m};newacc=candacc&acc if accw else candacc
   if new==cand and newacc==candacc:break
   cand,candacc=new,newacc
 return cand,candacc

# ---------------------------------------------------------------- emission
def norm_bits(bits):
 e=bits&0x7f800000
 return bits&0x80000000 if not e else ((bits&0x80000000)|0x7f7fffff if e==0x7f800000 else bits)

def emit_region(region,fname):
 """Round 4 region / round 5 trace emitter. A trace is a concatenation of base regions along static or profiled
 successor edges (lane aot5): one static schedule over all pairs, registers in locals throughout, and SIDE EXITS:
   * after the delay pair of an internal conditional branch whose direction is not the traced one;
   * before an XGKICK pair if PATH1 is still busy at that cycle (runFast would stall on it).
 Every exit materialises exactly the state runFast leaves at that cycle (same code as the region end, computed for
 the prefix of pairs before the exit)."""
 pairs=region['pairs'];facts=region['facts'];S=region['issues'];L=region['L']
 pc0=region['pc0'];n_pairs=len(pairs)
 internal=region.get('internal',{})   # branch pair index -> dict(follow=bool taken direction followed, tgt, nxt, cond)
 out=[]
 E=out.append
 E(f'bool {fname}(AotVU& v,uint8_t* data,uint32_t size,uint64_t end,bool& ended){{')
 loop=region.get('loop',False)
 E(' uint64_t base=v.m_cycle;')
 rid=region['id'];E(f' AOT4_RCALL({rid});')
 E(f' if(v.m_state.pc!={pc0}u||v.m_state.branchPending||v.m_state.ebit||v.m_state.haltAfterDelaySlot||v.m_stopRequested){{AOT5_EREC({region["img"]},{pc0});AOT4_FAIL({rid},0);}}')
 # pair 0 entry stall (the previous region's result or resource still busy): perform runFast's stall right here
 # (advanceTo + branch-VI bypass expiry). Exact even if a later guard declines: round 3 then finds pair 0 ready.
 st0=[f'v.m_vfReady[{k[1]}][{k[2]}]' if k[0]=='vf' else f'v.m_viReady[{k[1]}]' for k,at in region['guards'] if at==0]
 if region['first_fdiv']==0:st0.append('(v.m_fdiv.valid?v.m_fdiv.readyCycle:0)')
 if region['first_efu']==0:st0.append('v.m_efuResourceReady')
 if region['first_waitp']==0:st0+=['(v.m_efu[0].valid?v.m_efu[0].readyCycle:0)','(v.m_efu[1].valid?v.m_efu[1].readyCycle:0)']
 if st0 and not ('--no-prestall' in sys.argv):
  E(' {uint64_t st=base;'+''.join(f'st=std::max<uint64_t>(st,{x});' for x in sorted(set(st0)))+f'if(st>base){{if(st>=end)AOT4_FAIL({rid},7);v.m_viBranchBackupValid=false;v.advanceTo(st);base=v.m_cycle;AOT4_PRESTALL({rid});}}}}')
 # lane aot5: XGKICK at pair 0 with PATH1 busy: runFast's stall, in bulk (aot5StallXgkick), before the budget check
 if facts[0]['lk']=='xgkick' and '--no-xg-stall' not in sys.argv:E(f' if(v.m_xgkick.active){{if(!aot5StallXgkick(v,end))AOT4_FAIL({rid},6);base=v.m_cycle;}}')
 E(f' if(base+{L}u>end)AOT4_FAIL({rid},1);')
 # live-in readiness (offsets 0..2 only; see module doc)
 conds={}
 for k,at in region['guards']:
  key=(k[0],k[1]);
  if k[0]=='vf':expr=f'v.m_vfReady[{k[1]}][{k[2]}]'
  else:expr=f'v.m_viReady[{k[1]}]'
  conds[expr]=min(conds.get(expr,at),at)
 if conds:
  mins=min(conds.values())
  E(f' if(v.m_fastMaxWriteReady>base+{mins}u){{')
  E('  if('+'||'.join(f'{e}>base+{a}u' for e,a in sorted(conds.items()))+f')AOT4_FAIL({rid},2);')
  E(' }')
 if region['first_fdiv'] is not None:E(f' if(v.m_fdiv.valid&&v.m_fdiv.readyCycle>base+{region["first_fdiv"]}u)AOT4_FAIL({rid},3);')
 if region['first_efu'] is not None:E(f' if(v.m_efuResourceReady>base+{region["first_efu"]}u)AOT4_FAIL({rid},4);')
 if region['first_waitp'] is not None:
  w=region['first_waitp'];E(f' if((v.m_efu[0].valid&&v.m_efu[0].readyCycle>base+{w}u)||(v.m_efu[1].valid&&v.m_efu[1].readyCycle>base+{w}u))AOT4_FAIL({rid},5);')
 xg_pairs=[i for i,f in enumerate(facts) if f['lk']=='xgkick']
 has_xg=bool(xg_pairs)
 has_store=any(f['store'] for f in facts)
 # lane aot5: an XGKICK at pair k>0 of a non-loop region gets a guard exit before it (PATH1 busy at that cycle);
 # only a kick at pair 0 (or any kick in a loop region) still declines the whole region at entry.
 xg_exits=[k for k in xg_pairs if k>0 and facts[k-1]['lk']!='branch' and (not loop or S[k-1]+1>=4) and '--no-xg-exits' not in sys.argv]   # never between a branch and its delay slot
 xg0=bool(xg_pairs) and xg_pairs[0]==0 and '--no-xg-stall' not in sys.argv
 xg_cont=[k for k in xg_pairs if k not in xg_exits]                  # kicks the loop continuation must see idle
 if [k for k in xg_cont if not (k==0 and xg0)]:E(f' if(v.m_xgkick.active)AOT4_FAIL({rid},6);')
 E(f' AOT4_COUNT(0);AOT4_RENTER({rid});')
 # ---- static analysis of what is needed
 vf_used={0};vi_used=set();vi_written=set();vf_written=set()
 acc_used=False;acc_written=False;i_used=False;i_written=False;r_used=False;top_used=False;itop_used=False
 q_reads=[];p_reads=[];flag_reads=[];clips=[];fmacs=[]
 for i,(d,f) in enumerate(zip(pairs,facts)):
  u=d['up'];lo=d['lo']
  for uu in (d['u'],d['l']):
   for r,m in uu['read'][:uu['count']]:vf_used.add(r)
   vf_used.add(uu['write'][0])
   vi_used.update(vibits(uu['vr']|uu['vw']))
  vf_used.update(((u>>11)&31,(u>>16)&31,(u>>6)&31))
  if f['lk'] not in ('loi','nop'):vf_used.update(((lo>>11)&31,(lo>>16)&31));vi_used.update(((lo>>11)&15,(lo>>16)&15,(lo>>6)&15))
  for _,r,m,lat in vf_writes(d):vf_written.add(r)
  w=vi_write(d)
  if w:vi_written.add(w[0])
  if d['u']['ar'] or d['u']['aw'] or f['uk'] in ('fmac',):acc_used=True
  if d['u']['aw']:acc_written=True
  if f['uk'] in ('fmac','cross') and f.get('src')=='i':i_used=True
  if f['uk']=='minmax' and (u&63) in (29,31):i_used=True
  if f['lk']=='loi':i_written=True;i_used=True
  if f['uk'] in ('fmac','cross') and f.get('src')=='q':q_reads.append(i)
  if f['lk']=='mfp':p_reads.append(i)
  if f['lk']=='flagread':flag_reads.append(i)
  if f['uk']=='clip':clips.append(i)
  if f['uk'] in ('fmac','cross') and ((u>>21)&15):fmacs.append(i)
  if f['lk']=='plain' or f['lk']=='flagread':
   txt=lower_text(d,i)
   r_used|='local_r' in txt;top_used|='local_top' in txt;itop_used|='local_itop' in txt
 vf_used.discard(0);vi_used.discard(0)
 fdivs=[i for i,f in enumerate(facts) if f['lk']=='fdiv']
 efus=[i for i,f in enumerate(facts) if f['lk']=='efu']
 # Q value seen by a reader at offset s: latest region FDIV committed by s, else the old one (captured).
 def q_expr(s):
  mine=[j for j in fdivs if S[j]+pairs[j]['l']['lat']<=s]
  if mine:return f'q4_val_{mine[-1]}'
  before=[j for j in fdivs if S[j]<s]
  if before:return '(ofv?ofval:q0)'      # the guard put the old FDIV result before the region's DIV
  return f'((ofv&&ofr<=base+{s}u)?ofval:q0)'
 def p_expr(s):
  mine=[j for j in efus if S[j]+pairs[j]['l']['lat']<=s]
  if mine:return f'p4_val_{mine[-1]}'
  before=[j for j in efus if S[j]<s]
  if before:return 'aot4OldP(oe0v,oe0r,oe0val,oe1v,oe1r,oe1val,p0,~uint64_t(0))'
  return f'aot4OldP(oe0v,oe0r,oe0val,oe1v,oe1r,oe1val,p0,base+{s}u)'
 need_q_old=any(not [j for j in fdivs if S[j]+pairs[j]['l']['lat']<=S[i]] for i in q_reads) or bool(fdivs)
 need_p_old=any(not [j for j in efus if S[j]+pairs[j]['l']['lat']<=S[i]] for i in p_reads)
 cq='' if loop else 'const '
 if need_q_old:E(f' {cq}bool ofv=v.m_fdiv.valid;{cq}uint64_t ofr=v.m_fdiv.readyCycle;{cq}float ofval=v.m_fdiv.value;{cq}float q0=v.m_state.q;(void)ofv;(void)ofr;(void)ofval;(void)q0;')
 if need_p_old:E(f' {cq}bool oe0v=v.m_efu[0].valid,oe1v=v.m_efu[1].valid;{cq}uint64_t oe0r=v.m_efu[0].readyCycle,oe1r=v.m_efu[1].readyCycle;{cq}float oe0val=v.m_efu[0].value,oe1val=v.m_efu[1].value,p0=v.m_state.p;')
 # loads
 E(' const __m128 vf_0=_mm_loadu_ps(v.m_state.vf[0]);   // lane aot5: opaque (a constant vf0 lets MSVC fold x+0 -> x, wrong for -0)')
 for r in sorted(vf_used):E(f' __m128 vf_{r}=_mm_loadu_ps(v.m_state.vf[{r}]);')
 E(' int32_t vi_0=0;(void)vi_0;')
 for r in sorted(vi_used):E(f' int32_t vi_{r}=v.m_state.vi[{r}];')
 if acc_used:E(' __m128 local_acc=_mm_loadu_ps(v.m_state.acc);')
 ncand,ncandacc=norm_plan(region) if '--no-specnorm' not in sys.argv else ({},0)
 if not acc_used:ncandacc=0
 # Round 6: combine independent normalisation guards before one scalar branch.
 checks=[(f'vf_{r}',m) for r,m in sorted(ncand.items())]
 if ncandacc:checks.append(('local_acc',ncandacc))
 if checks:
  E(' __m128i norm_bad=_mm_setzero_si128();')
  for name,m in checks:E(f' norm_bad=_mm_or_si128(norm_bad,aot6NormBad({name},{m}u));')
  E(f' if(!_mm_testz_si128(norm_bad,norm_bad))AOT4_FAIL({rid},8);')
 if i_used:E(' float local_i=v.m_state.i;')
 if r_used:E(' uint32_t local_r=v.m_state.r;')
 if top_used:E(' const uint32_t local_top=v.m_state.top;')
 if itop_used:E(' const uint32_t local_itop=v.m_state.itop;')
 if p_reads:E(' float local_p=0.0f;')
 E(' bool bk_valid=v.m_viBranchBackupValid;uint8_t bk_reg=v.m_viBranchBackupReg;int32_t bk_val=v.m_viBranchBackupValue;')
 for r in sorted(vi_written):
  E(f' bool vh_{r}=v.m_fastHasVi[{r}];uint64_t vr_{r}=v.m_fastReadyVi[{r}];int32_t vv_{r}=v.m_fastVisVi[{r}];uint64_t vrdy_{r}=v.m_viReady[{r}];')
 if clips:E(' uint32_t wclip=v.m_workingClip;')
 use_xgd=has_store or has_xg or loop
 if use_xgd:E(' uint64_t xgd=base;(void)xgd;')   # aot4rev fix: loops track PATH1 progress too (no double progress on exit)
 for j in fdivs:E(f' float q4_val_{j}=0.0f;uint32_t q4_di_{j}=0;')
 for j in efus:E(f' float p4_val_{j}=0.0f;')
 # ---- exit points: (k = number of pairs executed, X = exit offset)
 exit_points={}
 for b,info in internal.items():
  if info.get('cond'):exit_points[b+2]=S[b+1]+1
 for k in xg_exits:exit_points[k]=S[k-1]+1
 exit_points[n_pairs]=L
 # VF lane write history for exit visibility: only lanes whose last write is still in flight at the exit
 def vis_plan_for(k,X):
  lane_writes={}
  for i in range(k):
   for _,r,m,lat in vf_writes(pairs[i]):
    for c in lanes(m):lane_writes.setdefault((r,c),[]).append(i)
  pending={kk:w for kk,w in lane_writes.items() if S[w[-1]]+4>X}
  plan={};pre=set()
  for (r,c),w in pending.items():
   # walk back: vis at write w_j = pre-write value if previous write committed by then, else vis at previous
   j=len(w)-1
   while j>0 and S[w[j]]<S[w[j-1]]+4:j-=1
   if j>0:plan[r,c]=('pre',w[j]);pre.add((w[j],r))
   else:
    # first write of the lane: depends on the entry state when it is issued within 3 cycles
    if S[w[0]]>=3:plan[r,c]=('pre',w[0]);pre.add((w[0],r))
    else:plan[r,c]=('entry',w[0]);pre.add((w[0],r))
  return plan,pending,pre
 plans_={}
 need_pre=set();entry_lanes=set()
 for k,X in exit_points.items():
  plans_[k]=vis_plan_for(k,X);need_pre|=plans_[k][2]
  entry_lanes|={rc for rc,(kind,i) in plans_[k][0].items() if kind=='entry'}
 vis_plan,pending_lanes,_=plans_[n_pairs]
 for (r,c) in sorted(entry_lanes):
  E(f' {cq}bool vfh_{r}_{c}=v.m_fastHasVf[{r}][{c}];{cq}uint64_t vfi_{r}_{c}=v.m_fastIssueVf[{r}][{c}];{cq}float vfv_{r}_{c}=v.m_fastVisVf[{r}][{c}];')
 pend0=[];has_carry=False
 if loop and fmacs and not flag_reads:
  pend0=[j for j in fmacs if S[j]+4>L]
  if pend0 and all(S[j]+4<=2*L for j in pend0):E(' bool cvalid=false;'+''.join(f'uint32_t cfm_{j}=0,cfx_{j}=0;' for j in pend0));has_carry=True
 # static branch-target state: a taken branch leaves branchTarget=tgt and branchDelay=0 (until the next taken one)
 def bt_code(k):
  t=None
  for b in sorted(internal):
   if b+1<k and internal[b]['follow']:t=internal[b]['tgt']
  return f'v.m_state.branchTarget={t}u;v.m_state.branchDelay=0;' if t is not None else ''
 done='xgd' if use_xgd else 'base'
 def vis_val(r,c,kind,i):
  if kind=='pre':return f'vectorLane(pre_{i}_{r},{c})'
  return f'((!vfh_{r}_{c}||base+{S[i]}u>=vfi_{r}_{c}+4u)?vectorLane(pre_{i}_{r},{c}):vfv_{r}_{c})'
 carry_ok=loop and not flag_reads
 def stk_or(committed):
  # lane aot5 p8: prefix of the FMAC list -> running accumulator sk_<last>
  if committed==fmacs[:len(committed)]:return f'stk6_{committed[-1]}'   # Preserve the running sticky accumulator
  return '|'.join(f'up_{j}.m|up_{j}.x' for j in committed)
 def events_part(EE,k,X,cont=False):
  fm=[j for j in fmacs if j<k];committed=[j for j in fm if S[j]+4<=X];pend=[j for j in fm if S[j]+4>X]
  carry=carry_ok and bool(pend) and all(S[j]+4<=2*X for j in pend)
  EE(f' if(v.m_commitCycles)aot4Drain(v,base+{X}u);')
  EE(f' v.m_cycle=base+{X}u;v.m_state.cycles=base+{X}u;')
  # overlay region events committed by X, then queue the ones still in flight
  cstk='|'.join(f'cfm_{j}|cfx_{j}' for j in pend)
  if has_carry and k<n_pairs:
   # lane aot5: mid-iteration exit of an in-place loop: the previous iteration's carried FMAC flag entries (pend0)
   # committed at base+1..3 (<= X, X>=4), before every flag event of this iteration
   c0='|'.join(f'cfm_{j}|cfx_{j}' for j in pend0)
   if committed:
    stk_parts=stk_or(committed)
    EE(f' {{uint32_t stk={stk_parts};if(cvalid)stk|={c0};v.m_state.mac=aot5Mac(up_{committed[-1]}.m);v.m_state.status=(v.m_state.status&0xFF0u)|aot5Cur(up_{committed[-1]}.m)|(aot5Cur(stk)<<6);}}')
   else:
    EE(f' if(cvalid){{v.m_state.mac=aot5Mac(cfm_{pend0[-1]});v.m_state.status=(v.m_state.status&0xFF0u)|aot5Cur(cfm_{pend0[-1]})|(aot5Cur({c0})<<6);}}')
   committed=[];carry=False
  if committed:
   stk_parts=stk_or(committed)
   if carry:EE(f' {{uint32_t stk={stk_parts};if(cvalid)stk|={cstk};v.m_state.mac=aot5Mac(up_{committed[-1]}.m);v.m_state.status=(v.m_state.status&0xFF0u)|aot5Cur(up_{committed[-1]}.m)|(aot5Cur(stk)<<6);}}')
   else:
    EE(f' v.m_state.mac=aot5Mac(up_{committed[-1]}.m);')
    EE(f' v.m_state.status=(v.m_state.status&0xFF0u)|aot5Cur(up_{committed[-1]}.m)|(aot5Cur({stk_parts})<<6);')
  elif carry:
   EE(f' if(cvalid){{v.m_state.mac=aot5Mac(cfm_{pend[-1]});v.m_state.status=(v.m_state.status&0xFF0u)|aot5Cur(cfm_{pend[-1]})|(aot5Cur({cstk})<<6);}}')
  fd=[j for j in fdivs if j<k]
  qc=[j for j in fd if S[j]+pairs[j]['l']['lat']<=X];qp=[j for j in fd if S[j]+pairs[j]['l']['lat']>X]
  if qc:
    j=qc[-1];EE(f' v.m_state.q=q4_val_{j};v.m_state.status=(v.m_state.status&0xFCFu)|q4_di_{j}|(('+'|'.join(f'q4_di_{kk}' for kk in qc)+')<<6);')
  if qp:
   j=qp[-1];EE(f' v.m_fdiv.valid=true;v.m_fdiv.readyCycle=base+{S[j]+pairs[j]["l"]["lat"]}u;v.m_fdiv.value=q4_val_{j};v.m_fdiv.statusDi=q4_di_{j};v.scheduleCommit(base+{S[j]+pairs[j]["l"]["lat"]}u,8u);')
  ef=[j for j in efus if j<k]
  pc_=[j for j in ef if S[j]+pairs[j]['l']['lat']<=X];pp=[j for j in ef if S[j]+pairs[j]['l']['lat']>X]
  if pc_:EE(f' v.m_state.p=p4_val_{pc_[-1]};')
  for j in pp:EE(f' aot4PushP(v,base+{S[j]+pairs[j]["l"]["lat"]}u,p4_val_{j});')
  if ef:j=ef[-1];EE(f' v.m_efuResourceReady=base+{S[j]+pairs[j]["l"]["lat"]-1}u;')
  cl=[j for j in clips if j<k]
  ccl=[j for j in cl if S[j]+4<=X];cpl=[j for j in cl if S[j]+4>X]
  if ccl:EE(f' v.m_state.clip=cv_{ccl[-1]};')
  # in-flight flag entries in issue order (FMAC and CLIP never share a pair)
  for j in sorted(([] if (carry and cont) else pend)+cpl):
   if j in pend:EE(f' aot4PushFlag(v,base+{S[j]}u,aot5Mac(up_{j}.m),aot5Cur(up_{j}.m),aot5Cur(up_{j}.x));')
   else:EE(f' aot4PushClip(v,base+{S[j]}u,cv_{j});')
  if carry and cont:EE(' '+''.join(f'cfm_{j}=up_{j}.m;cfx_{j}=up_{j}.x;' for j in pend)+'cvalid=true;')
 def exit_code(EE,k,X,control):
  """Materialise the state after pairs[:k] at offset X (runFast's state at that cycle), then return."""
  plan,pend_l,_=plans_[k]
  events_part(EE,k,X)
  midloop=loop and k<n_pairs   # lane aot5 p7fix: the previous iteration's tail pairs[k:] wrote registers kept in locals
  if any(j<k or midloop for j in clips):EE(' v.m_workingClip=wclip;')
  vfw=set();viw=set();accw=False;iw=False;lastw={};maxready=0
  for i in range(n_pairs if midloop else k):
   d=pairs[i]
   if i>=k:
    # tail of the previous iteration: values only (ready by base+3 <= X; VI visibility locals are absolute)
    for _,r,m,lat in vf_writes(d):
     vfw.add(r);assert S[i]+lat-L<=X,(rid,i,lat)
    w=vi_write(d)
    if w:viw.add(w[0])
    if d['u']['aw']:accw=True
    if facts[i]['lk']=='loi':iw=True
    if facts[i]['lk']=='branch' and facts[i]['op'] in (0x21,0x25) and (d['lo']>>16)&15:viw.add((d['lo']>>16)&15)
    continue
   for _,r,m,lat in vf_writes(d):
    vfw.add(r)
    for c in lanes(m):lastw[r,c]=S[i]+lat;maxready=max(maxready,S[i]+lat)
   w=vi_write(d)
   if w:viw.add(w[0]);maxready=max(maxready,S[i]+w[1])
   if d['u']['aw']:accw=True;maxready=max(maxready,S[i]+1)
   if facts[i]['lk']=='loi':iw=True
   if facts[i]['lk']=='branch' and facts[i]['op'] in (0x21,0x25) and (d['lo']>>16)&15:viw.add((d['lo']>>16)&15)
  for r in sorted(vfw):EE(f' _mm_storeu_ps(v.m_state.vf[{r}],vf_{r});')
  for r in sorted(viw):EE(f' v.m_state.vi[{r}]=vi_{r};')
  if accw:EE(' _mm_storeu_ps(v.m_state.acc,local_acc);')
  if iw:EE(' v.m_state.i=local_i;')
  if r_used:EE(' v.m_state.r=local_r;')
  for (r,c),rd in sorted(lastw.items()):
   if rd>X:EE(f' v.m_vfReady[{r}][{c}]=base+{rd}u;')
  for r in sorted(viw&vi_written):EE(f' v.m_viReady[{r}]=vrdy_{r};v.m_fastHasVi[{r}]=vh_{r};v.m_fastReadyVi[{r}]=vr_{r};v.m_fastVisVi[{r}]=vv_{r};')
  if maxready:EE(f' if(v.m_fastMaxWriteReady<base+{maxready}u)v.m_fastMaxWriteReady=base+{maxready}u;')
  # VF visibility for lanes still in flight
  for (r,c),(kind,i) in sorted(plan.items()):
   last=pend_l[r,c][-1];val=vis_val(r,c,kind,i)
   EE(f' v.m_fastVisVf[{r}][{c}]={val};v.m_fastIssueVf[{r}][{c}]=base+{S[last]}u;v.m_fastHasVf[{r}][{c}]=true;')
  EE(' v.m_viBranchBackupValid=bk_valid;v.m_viBranchBackupReg=bk_reg;v.m_viBranchBackupValue=bk_val;')
  EE(f' v.m_currentUpperInstruction={pairs[k-1]["up"]}u;')
  EE(f' if(v.m_xgkick.active&&{done}<base+{X}u)v.progressXgkick(base+{X}u-{done});')
  EE(control)
  EE(f' AOT4_COUNT(1);AOT4_CYC({X}u);')
  EE(' return true;')
 region['side_exit_pcs']=[]
 if loop:E(' for(;;){')
 # ---- pairs
 # lanes known to be normalized (no denormal/inf/nan): FMAC/ITOF results; unknown at region entry (also safe for loop mode)
 norm={0:0xF,**ncand};normacc=ncandacc
 branches={}
 for i,(d,f) in enumerate(zip(pairs,facts)):
  s=S[i];u=d['up'];lo=d['lo'];code=[]
  C=code.append
  if i in xg_exits:
   # guard exit before this XGKICK: progress PATH1 to the exit cycle; still busy -> runFast would stall here
   X=exit_points[i]
   C(f' if(v.m_xgkick.active&&xgd<base+{X}u){{v.progressXgkick(base+{X}u-xgd);xgd=base+{X}u;}}')
   C(' if(v.m_xgkick.active){')
   exit_code(C,i,X,f' v.m_state.pc={d["pc"]}u;'+bt_code(i))
   C(' }')
   region['side_exit_pcs'].append(d['pc'])
  C(f' // pc {d["pc"]} +{s}')
  if i in region['stalls']:C(' bk_valid=false;')
  for (j,r) in sorted(need_pre):
   if j==i:C(f' const __m128 pre_{i}_{r}=vf_{r};')
  # values observable at this cycle
  if i in p_reads:C(f' local_p={p_expr(s)};')
  if i in flag_reads:
   kind=FLAG_READ[f['op']]
   C(f' if(v.m_commitCycles)aot4Drain(v,base+{s}u);')
   committed=[j for j in fmacs if S[j]+4<=s]
   if kind=='mac':
    C(f' const uint32_t flag4_mac_{i}='+(f'aot5Mac(up_{committed[-1]}.m)' if committed else 'v.m_state.mac')+';')
   elif kind=='clip':
    cc=[j for j in clips if S[j]+4<=s]
    C(f' const uint32_t flag4_clip_{i}='+(f'cv_{cc[-1]}' if cc else 'v.m_state.clip')+';')
   else:
    st='v.m_state.status'
    if committed:
     stk='aot5Cur('+stk_or(committed)+')<<6'
     st=f'((({st})&0xFF0u)|aot5Cur(up_{committed[-1]}.m)|({stk}))'
    qc=[j for j in fdivs if S[j]+pairs[j]['l']['lat']<=s]
    if qc:st=f'((({st})&0xFCFu)|q4_di_{qc[-1]}|(('+'|'.join(f'q4_di_{j}' for j in qc)+')<<6))'
    C(f' const uint32_t flag4_status_{i}={st};')
  # ---- upper
  uk=f['uk'];dest=(u>>21)&15;fs=(u>>11)&31;ft=(u>>16)&31;fd=(u>>6)&31;op=u&63;toacc=op>=60
  utarget=None
  if uk in ('fmac','cross'):
   src=f['src']
   qn=f'aot4Norm1({q_expr(s)})' if src=='q' else '0.0f'
   inn='aot4Norm1(local_i)' if src=='i' else '0.0f'
   acc='local_acc' if acc_used else '_mm_setzero_ps()'
   NS,NT,NA=norm_step(d,f,norm,normacc)[0]
   if dest:
    C(f' const R5 up_{i}=vectorFmac5<{u}u,{str(NS).lower()},{str(NT).lower()},{str(NA).lower()},{str(MASK_LANES).lower()}>(vf_{fs},vf_{ft},{acc},{qn},{inn});')
    prev=[j for j in fmacs if j<i]
    prefix=f'stk6_{prev[-1]}|' if prev else ''
    C(f' const uint32_t stk6_{i}={prefix}up_{i}.m|up_{i}.x;')
    tgt='local_acc' if toacc else (f'vf_{fd}' if fd else None)
    if tgt:C(f' const __m128 mg_{i}={blend(tgt,"up_"+str(i)+".v",dest)};');utarget=(tgt,f'mg_{i}')
  elif uk=='minmax':
   rhs=f'_mm_shuffle_ps(vf_{ft},vf_{ft},_MM_SHUFFLE({op&3},{op&3},{op&3},{op&3}))' if 16<=op<=23 else ('_mm_set1_ps(local_i)' if op in (29,31) else f'vf_{ft}')
   maximum=16<=op<=19 or op in (29,43)
   if fd:C(f' const __m128 mg_{i}={blend("vf_"+str(fd),"aot5MinMax<"+str(maximum).lower()+">(vf_"+str(fs)+","+rhs+")",dest)};');utarget=(f'vf_{fd}',f'mg_{i}')
  elif uk=='abs':
   if ft:C(f' const __m128 mg_{i}={blend("vf_"+str(ft),"_mm_and_ps(vf_"+str(fs)+",_mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)))",dest)};');utarget=(f'vf_{ft}',f'mg_{i}')
  elif uk=='cvt':
   sel=((u&3)|((u>>4)&124));shift=(0,4,12,15)[sel&3]
   if sel<=19:val=f'_mm_mul_ps(_mm_cvtepi32_ps(_mm_castps_si128(vf_{fs})),_mm_set1_ps(1.0f/{1<<shift}.0f))'
   else:
    val=f'aot5Ftoi<{1<<shift}>(vuNormalize4(vf_{fs}))'
   if ft:C(f' const __m128 mg_{i}={blend("vf_"+str(ft),val,dest)};');utarget=(f'vf_{ft}',f'mg_{i}')
  elif uk=='clip':
   C(f' {{const uint32_t lb=vectorLaneBits(vf_{ft},3);const int32_t lim=(lb&0x7f800000u)?int32_t(lb&0x7fffffffu):0x007fffff;uint32_t cl=0;')
   for c in range(3):
    for sg in range(2):C(f'  if(std::bit_cast<int32_t>(vectorLaneBits(vf_{fs},{c})^{sg<<31}u)>lim)cl|={1<<(c*2+sg)}u;')
   C('  wclip=((wclip<<6)|(cl&0x3Fu))&0xFFFFFFu;}')
   C(f' const uint32_t cv_{i}=wclip;')
  # ---- lower
  lk=f['lk'];w=vi_write(d)
  if w:
   wr,wlat=w
   C(f' const int32_t ovi_{i}=vi_{wr};')
   if d['l']['delay']:
    C(f' const int32_t byp_{i}=(vrdy_{wr}>base+{s}u)?((!vh_{wr}||base+{s}u>=vr_{wr})?vi_{wr}:vv_{wr}):ovi_{i};')
  if lk=='loi':C(f' local_i=std::bit_cast<float>({lo}u);')
  elif lk in ('nop','waitq','waitp'):pass
  elif lk=='branch':
   bop=f['op'];tgt=(d['pc']+8+imm11(lo)*8)&0x3fff
   def bread(reg):
    return '0' if reg==0 else f'(bk_valid&&bk_reg=={reg}?bk_val:vi_{reg})'
   is_=(lo>>11)&15;it=(lo>>16)&15
   tk=f'taken4_{i}'
   if bop in (0x24,0x25):C(f' const uint32_t jr4=((uint32_t)(uint16_t)vi_{is_}*8u)&0x3fffu;')
   if bop in (0x21,0x25) and it:C(f' vi_{it}={(d["pc"]+16)//8};')
   if bop in (0x20,0x21,0x24,0x25):C(f' const bool {tk}=true;')
   elif bop==0x28:C(f' const bool {tk}=int16_t({bread(is_)})==int16_t({bread(it)});')
   elif bop==0x29:C(f' const bool {tk}=int16_t({bread(is_)})!=int16_t({bread(it)});')
   elif bop==0x2c:C(f' const bool {tk}=int16_t({bread(is_)})<0;')
   elif bop==0x2d:C(f' const bool {tk}=int16_t({bread(is_)})>0;')
   elif bop==0x2e:C(f' const bool {tk}=int16_t({bread(is_)})<=0;')
   elif bop==0x2f:C(f' const bool {tk}=int16_t({bread(is_)})>=0;')
   branches[i]=(bop,tgt)
  elif simd_lower(d) is not None:
   code_=simd_lower(d)
   if code_:C(code_)
  else:
   txt=lower_text(d,i)
   if lk=='xgkick':
    C(f' #define AOT4_XGKICK(a) {{if(v.m_commitCycles)aot4Drain(v,base+{s}u);v.m_cycle=base+{s}u;v.startXgkick(a);xgd=base+{s}u;}}')
   if f['store']:
    C(f' #define AOT4_STORE(a,w,m) {{if(v.m_xgkick.active&&xgd<base+{s}u){{v.progressXgkick(base+{s}u-xgd);xgd=base+{s}u;}}aot4Store(data,a,w,m);}}')
   C(txt)
   if lk=='xgkick':C(' #undef AOT4_XGKICK')
   if f['store']:C(' #undef AOT4_STORE')
  if utarget:C(f' {utarget[0]}={utarget[1]};')
  # ---- normalization facts after the pair (upper wins over a lower write to the same register)
  _,norm,normacc=norm_step(d,f,norm,normacc)
  # ---- VI write bookkeeping (runFast order: visibility before exec, narrowing after)
  if w:
   wr,wlat=w
   C(f' vi_{wr}=static_cast<int16_t>(vi_{wr});')
   C(f' if(!vh_{wr}||base+{s}u>=vr_{wr})vv_{wr}=ovi_{i};vr_{wr}=base+{s+wlat}u;vh_{wr}=true;vrdy_{wr}=base+{s+wlat}u;')
  # branch-VI backup
  wv=w[0] if w else 0;delay=bool(d['l']['delay'])
  if wv and delay:
   C(f' bk_valid=bk_valid&&bk_reg=={wv};if(!bk_valid||bk_reg!={wv})bk_val=byp_{i};bk_reg={wv};bk_valid=true;')
  else:C(' bk_valid=false;')
  # ---- side exit after the delay pair of an internal conditional branch (direction not traced)
  if i-1 in internal and internal[i-1].get('cond'):
   info=internal[i-1];X=exit_points[i+1]
   if info['follow']:
    C(f' if(!taken4_{i-1}){{');exit_code(C,i+1,X,f' v.m_state.pc={info["nxt"]}u;'+bt_code(i-1));C(' }')   # not taken: only earlier taken branches count
    region['side_exit_pcs'].append(info['nxt'])
   else:
    C(f' if(taken4_{i-1}){{');exit_code(C,i+1,X,f' v.m_state.pc={info["tgt"]}u;v.m_state.branchTarget={info["tgt"]}u;v.m_state.branchDelay=0;');C(' }')
    region['side_exit_pcs'].append(info['tgt'])
  out+=code
 # ---- final exit (and the in-place loop continuation)
 X=L
 last=pairs[-1];lastpc=last['pc']
 nxt=(lastpc+8)&0x3fff
 bi=max([b for b in branches if b not in internal],default=None)
 br=(bi,)+branches[bi] if bi is not None and bi>=n_pairs-2 else None
 if loop:
  E(f' if(v.m_xgkick.active&&{done}<base+{X}u){{v.progressXgkick(base+{X}u-{done});'+(f'xgd=base+{X}u;' if use_xgd else '')+'}')
  cond=f'taken4_{br[0]}&&base+{2*X}u<=end&&!v.m_stopRequested'+('&&!v.m_xgkick.active' if xg_cont else '')
  E(f' if({cond}){{')
  events_part(E,n_pairs,X,True)
  for (r,c),(kind,i) in sorted(vis_plan.items()):
   # lane aot5: refresh every lane some exit reads as an entry lane (mid-iteration exits included)
   if (r,c) in entry_lanes:E(f' vfv_{r}_{c}={vis_val(r,c,kind,i)};vfi_{r}_{c}=base+{S[pending_lanes[r,c][-1]]}u;vfh_{r}_{c}=true;')
  E(f' v.m_state.branchTarget={region["pc0"]}u;v.m_state.branchDelay=0;')
  E(f' base+={X}u;AOT4_COUNT(2);AOT4_CYC({X}u);')
  if need_q_old:E(' ofv=v.m_fdiv.valid;ofr=v.m_fdiv.readyCycle;ofval=v.m_fdiv.value;q0=v.m_state.q;')
  if need_p_old:E(' oe0v=v.m_efu[0].valid;oe1v=v.m_efu[1].valid;oe0r=v.m_efu[0].readyCycle;oe1r=v.m_efu[1].readyCycle;oe0val=v.m_efu[0].value;oe1val=v.m_efu[1].value;p0=v.m_state.p;')
  E(' continue;')
  E(' }')
 E(f' // exit at +{X}')
 # control
 if len(pairs)>=2 and pairs[-2]['e']:
  ctrl=f' v.m_state.pc={nxt}u;v.m_state.ebit=true;ended=true;'+bt_code(n_pairs)
 elif br:
  bi,bop,tgt=br
  # a taken branch leaves branchTarget set (branchPending/branchDelay cleared by the delay pair)
  if bop in (0x24,0x25):ctrl=' v.m_state.pc=jr4;v.m_state.branchTarget=jr4;v.m_state.branchDelay=0;'
  elif bop in (0x20,0x21):ctrl=f' v.m_state.pc={tgt}u;v.m_state.branchTarget={tgt}u;v.m_state.branchDelay=0;'
  else:ctrl=f' if(taken4_{bi}){{v.m_state.pc={tgt}u;v.m_state.branchTarget={tgt}u;v.m_state.branchDelay=0;}}else{{v.m_state.pc={nxt}u;{bt_code(n_pairs)}}}'
 else:ctrl=f' v.m_state.pc={nxt}u;'+bt_code(n_pairs)
 exit_code(E,n_pairs,X,ctrl)
 if loop:E(' }')
 E('}')
 region['exit_pc']=nxt
 # lane aot5: static exit pcs for the chain (branch target first: loops/back edges are the common case)
 ex=[]
 if br and br[1] not in (0x24,0x25):ex.append(br[2])
 if not (br and br[1] in (0x20,0x21,0x24,0x25)) and not (len(pairs)>=2 and pairs[-2]['e']):ex.append(nxt)
 ex+=region['side_exit_pcs']
 region['exits']=[x for k,x in enumerate(ex) if x not in ex[:k]]
 return '\n'.join(out)

COMMON=r'''#pragma once
// Transformers port: VU1 AOT round 4 helpers. PRIVATE generated support code.
#include "common.h"
#include <bit>
#include <smmintrin.h>
#if defined(AOT4_STATS)   // standalone harness builds only (build.py passes /DAOT4_STATS)
extern uint64_t g_aot4Count[4];
#define AOT4_COUNT(k) (++g_aot4Count[k])
#define AOT4_CYC(n) (g_aot4Count[3]+=(n))
extern uint64_t g_aot4R[][12];
#define AOT4_RCALL(id) (++g_aot4R[id][0])
#define AOT4_RENTER(id) (++g_aot4R[id][1])
#define AOT4_FAIL(id,k) {++g_aot4R[id][2+(k)];return false;}
#define AOT4_PRESTALL(id) (++g_aot4R[id][11])
#else
#define AOT4_COUNT(k) ((void)0)
#define AOT4_CYC(n) ((void)0)
#define AOT4_RCALL(id) ((void)0)
#define AOT4_RENTER(id) ((void)0)
#define AOT4_FAIL(id,k) return false;
#define AOT4_PRESTALL(id) ((void)0)
#endif
#if defined(AOT5_RSTATS)   // lane aot5 profile (standalone harness only): chain entries at non-region pcs
void aot5Erec(int img,uint32_t pc,uint32_t pc0);
#define AOT5_EREC(img,pc0) (v.m_state.pc!=(pc0)?aot5Erec(img,v.m_state.pc,pc0):(void)0)
#else
#define AOT5_EREC(img,pc0) ((void)0)
#endif
// Commit calendar events already pending at region entry, in cycle order, up to target (inclusive).
// Region events are never in the calendar while a region runs, so only older events are committed here.
inline void aot4Drain(AotVU& v,uint64_t target){
 while(v.m_commitCycles){
  const uint64_t upcoming=std::rotr(v.m_commitCycles,int((v.m_cycle+1u)&63u));
  const uint64_t next=v.m_cycle+1u+vuCtz64(upcoming);
  if(next>target)break;
  v.m_cycle=next;v.commitReadyPipelines();
 }
}
inline float aot4OldP(bool a,uint64_t ar,float av,bool b,uint64_t br,float bv,float p,uint64_t t){
 uint64_t best=0;bool have=false;
 if(a&&ar<=t){p=av;best=ar;have=true;}
 if(b&&br<=t&&(!have||br>best))p=bv;
 return p;
}
struct VectorResult4 {__m128 value;uint32_t zero,sign,under,over,extra,cur;};
// vuNormalizeOperand / aotNormalizeOperand, bit for bit, always inlined
__forceinline float aot4Norm1(float value){
 uint32_t b;std::memcpy(&b,&value,4);const uint32_t e=b&0x7f800000u;
 if(e==0)b&=0x80000000u;else if(e==0x7f800000u)b=(b&0x80000000u)|0x7f7fffffu;
 std::memcpy(&value,&b,4);return value;
}
// Round 6: unsigned magnitude range test, normal [min,max] or exact signed zero.
__forceinline __m128i aot6NormBad(__m128 v,unsigned vuLanes){
 const __m128i b=_mm_and_si128(_mm_castps_si128(v),_mm_set1_epi32(0x7FFFFFFF));
 const __m128i nonzero=_mm_cmpgt_epi32(b,_mm_setzero_si128());
 const __m128i small=_mm_cmpgt_epi32(_mm_set1_epi32(0x00800000),b);
 const __m128i large=_mm_cmpgt_epi32(b,_mm_set1_epi32(0x7F7FFFFF));
 return _mm_and_si128(_mm_or_si128(_mm_and_si128(nonzero,small),large),vuLaneMask(kVuRev4[vuLanes&15u]));
}
__forceinline bool aot4NormOk(__m128 v,unsigned vuLanes){
 // vuNormalize4 is the identity on a lane iff its exponent is 1..254 or it is an exact +-0
 const __m128i b=_mm_castps_si128(v);
 const __m128i e=_mm_and_si128(b,_mm_set1_epi32(0x7F800000));
 const __m128i m=_mm_and_si128(b,_mm_set1_epi32(0x007FFFFF));
 const __m128i den=_mm_andnot_si128(_mm_cmpeq_epi32(m,_mm_setzero_si128()),_mm_cmpeq_epi32(e,_mm_setzero_si128()));
 const __m128i bad=_mm_or_si128(den,_mm_cmpeq_epi32(e,_mm_set1_epi32(0x7F800000)));
 return (static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(bad)))&kVuRev4[vuLanes&15u])==0;
}
// ---- lane aot5 (round 5) helpers
// Packed FMAC lane classes, component order (bit c = component c, x = 0): zero[3:0] sign[7:4] under[11:8] over[15:12]
// (zero includes underflow, as VectorResult.zero). x = the product's classes for MADD/MSUB (sticky only), same packing.
struct R5 {__m128 v;uint32_t m,x;};
// Round 6: reverse bits within each class nibble without four dependent table loads.
__forceinline uint32_t aot5Mac(uint32_t m){
 m=((m&0x5555u)<<1)|((m>>1)&0x5555u);
 return ((m&0x3333u)<<2)|((m>>2)&0x3333u);
}
// Round 6: reduce four lane-class nibbles without four scalar test/set chains.
__forceinline uint32_t aot5Cur(uint32_t m){
 uint32_t t=m|(m>>1);t|=t>>2;t&=0x1111u;t|=t>>3;t|=t>>6;return t&15u;
}
// Cold path: flags only, packed in one 64-bit return value (m | x<<32). The value is rebuilt by the caller exactly as
// vectorCold builds it: dest lanes classed zero/under -> signed zero, over -> signed FLT_MAX, else the float result f.
__declspec(noinline) inline uint64_t aot5Cold(__m128 s,__m128 t,__m128 a,__m128 f,__m128 product,uint8_t kind,uint8_t src,uint32_t destBits){
 const VectorResult c=vectorCold(s,t,a,f,product,kind,src,destBits);
 const uint32_t m=c.zero|(c.sign<<4)|(c.under<<8)|(c.over<<12);
 const uint32_t x=(c.extra&1u)|((c.extra&2u)<<3)|((c.extra&4u)<<6)|((c.extra&8u)<<9);
 return uint64_t(m)|(uint64_t(x)<<32);
}
__forceinline __m128 aot5ColdValue(__m128 f,uint32_t m){
 const __m128i zu=vuLaneMask(m&15u),ov=vuLaneMask((m>>12)&15u);
 const __m128i sg=_mm_and_si128(vuLaneMask((m>>4)&15u),_mm_set1_epi32(static_cast<int>(0x80000000u)));
 __m128i v=_mm_andnot_si128(_mm_or_si128(zu,ov),_mm_castps_si128(f));
 v=_mm_or_si128(v,_mm_and_si128(zu,sg));
 v=_mm_or_si128(v,_mm_and_si128(ov,_mm_or_si128(sg,_mm_set1_epi32(0x7F7FFFFF))));
 return _mm_castsi128_ps(v);
}
// vuFloatToInt on 4 lanes, bit for bit: scaled = double(v)*SCALE (exact: SCALE is 2^0/4/12/15); >= INT_MAX -> INT_MAX,
// <= INT_MIN -> INT_MIN, else truncation. Clamping in double then cvttpd gives the same three cases.
template<int SCALE> __forceinline __m128 aot5Ftoi(__m128 v){
 const __m128d sc=_mm_set1_pd(double(SCALE)),hi=_mm_set1_pd(2147483647.0),lo=_mm_set1_pd(-2147483648.0);
 const __m128d a=_mm_max_pd(_mm_min_pd(_mm_mul_pd(_mm_cvtps_pd(v),sc),hi),lo);
 const __m128d b=_mm_max_pd(_mm_min_pd(_mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(v,v)),sc),hi),lo);
 return _mm_castsi128_ps(_mm_unpacklo_epi64(_mm_cvttpd_epi32(a),_mm_cvttpd_epi32(b)));
}
// vuMinMax on 4 lanes, bit for bit: chooseLeft = reverse ? (max ? a<b : a>b) : (max ? a>b : a<b), reverse = a<0 && b<0
template<bool MAXIMUM> __forceinline __m128 aot5MinMax(__m128 l,__m128 r){
 const __m128i a=_mm_castps_si128(l),b=_mm_castps_si128(r);
 const __m128i gt=_mm_cmpgt_epi32(a,b),lt=_mm_cmpgt_epi32(b,a);
 const __m128i rev=_mm_srai_epi32(_mm_and_si128(a,b),31);
 const __m128i left=MAXIMUM?_mm_or_si128(_mm_and_si128(rev,lt),_mm_andnot_si128(rev,gt)):_mm_or_si128(_mm_and_si128(rev,gt),_mm_andnot_si128(rev,lt));
 return _mm_blendv_ps(r,l,_mm_castsi128_ps(left));
}
// Exact FMAC (same results/flags as the round-3 vectorFmac): fast-path test rewritten as integer range compares:
//   single op:  exp(r) in [1,254] && |r| != FLT_MAX   <=>  0x007FFFFF < |r| < 0x7F7FFFFF
//   product-sum: exp(r) in [2,253] <=> 0x00FFFFFF < |r| < 0x7F000000; exp(p) in [1,254] && |p| != FLT_MAX <=> 0x007FFFFF < |p| < 0x7F7FFFFF;
//                exp(p) <= exp(r)+21 <=> |p| < (exp(r)+22)<<23 <=> |p| - 0x0B000000 < (|r| & 0x7F800000)  (no int32 overflow)
// MASK=true zeroes the non-dest lanes of the operands (their results are discarded by the merge; avoids denormal assists).
template<uint32_t U,bool NS,bool NT,bool NA,bool MASK>
__forceinline R5 vectorFmac5(__m128 rawS,__m128 rawT,__m128 rawA,float rawQ,float rawI){
 VuFmacKind kind;VuFmacSrc src;uint8_t sel=0;bool toAcc=false;vuDecodeFmac(U,kind,src,sel,toAcc);
 constexpr uint32_t dest=(U>>21)&15u;constexpr uint32_t destBits=(dest&8u?1u:0u)|(dest&4u?2u:0u)|(dest&2u?4u:0u)|(dest&1u?8u:0u);
 constexpr unsigned BC=U&3u;
 __m128 s=NS?rawS:vuNormalize4(rawS);__m128 t;
 switch(src){
 case VuSrcBc:{const __m128 b=_mm_shuffle_ps(rawT,rawT,_MM_SHUFFLE(BC,BC,BC,BC));t=NT?b:vuNormalize4(b);break;}
 case VuSrcQ:t=_mm_set1_ps(rawQ);break;
 case VuSrcI:t=_mm_set1_ps(rawI);break;
 case VuSrcVt:t=NT?rawT:vuNormalize4(rawT);break;
 default:{const __m128 vt=NT?rawT:vuNormalize4(rawT);s=_mm_shuffle_ps(s,s,_MM_SHUFFLE(3,0,2,1));t=_mm_shuffle_ps(vt,vt,_MM_SHUFFLE(3,1,0,2));break;}
 }
 const bool productSum=kind==VuMadd||kind==VuMsub;
 if constexpr(MASK&&destBits!=15u){const __m128 dm=_mm_castsi128_ps(vuLaneMask(destBits));s=_mm_and_ps(s,dm);t=_mm_and_ps(t,dm);}
 __m128 a=_mm_setzero_ps(),product=_mm_setzero_ps(),f;
 switch(kind){
 case VuAdd:f=_mm_add_ps(s,t);break;
 case VuSub:f=_mm_sub_ps(s,t);break;
 case VuMul:f=_mm_mul_ps(s,t);break;
 default:
  a=NA?rawA:vuNormalize4(rawA);
  if constexpr(MASK&&destBits!=15u)a=_mm_and_ps(a,_mm_castsi128_ps(vuLaneMask(destBits)));
  product=_mm_mul_ps(s,t);f=kind==VuMadd?_mm_add_ps(a,product):_mm_sub_ps(a,product);break;
 }
 if(src==VuSrcCross)f=_mm_castsi128_ps(_mm_and_si128(_mm_castps_si128(f),_mm_set_epi32(0,-1,-1,-1)));
 const __m128i rb=_mm_and_si128(_mm_castps_si128(f),_mm_set1_epi32(0x7FFFFFFF));
 __m128i ok;
 if(!productSum)ok=_mm_and_si128(_mm_cmpgt_epi32(rb,_mm_set1_epi32(0x007FFFFF)),_mm_cmpgt_epi32(_mm_set1_epi32(0x7F7FFFFF),rb));
 else{
  const __m128i pb=_mm_and_si128(_mm_castps_si128(product),_mm_set1_epi32(0x7FFFFFFF));
  ok=_mm_and_si128(_mm_cmpgt_epi32(rb,_mm_set1_epi32(0x00FFFFFF)),_mm_cmpgt_epi32(_mm_set1_epi32(0x7F000000),rb));
  ok=_mm_and_si128(ok,_mm_and_si128(_mm_cmpgt_epi32(pb,_mm_set1_epi32(0x007FFFFF)),_mm_cmpgt_epi32(_mm_set1_epi32(0x7F7FFFFF),pb)));
  ok=_mm_and_si128(ok,_mm_cmpgt_epi32(_mm_and_si128(rb,_mm_set1_epi32(0x7F800000)),_mm_sub_epi32(pb,_mm_set1_epi32(0x0B000000))));
 }
 if(_mm_testc_si128(ok,vuLaneMask(destBits))){
  R5 r;r.v=f;r.m=(static_cast<uint32_t>(_mm_movemask_ps(f))&destBits)<<4;
  r.x=productSum?((static_cast<uint32_t>(_mm_movemask_ps(product))&destBits)<<4):0u;return r;
 }
 if(src!=VuSrcCross){
  // exact-zero lanes (see aot5_patch_p2a.py): z = lanes whose exact result is provably the float result and normal-or-zero
  const __m128i zero=_mm_setzero_si128();
  __m128i z;
  if(kind==VuAdd||kind==VuSub)z=_mm_cmpeq_epi32(rb,zero);
  else{
   const __m128i sa=_mm_and_si128(_mm_castps_si128(s),_mm_set1_epi32(0x7FFFFFFF)),ta=_mm_and_si128(_mm_castps_si128(t),_mm_set1_epi32(0x7FFFFFFF));
   z=_mm_or_si128(_mm_cmpeq_epi32(sa,zero),_mm_cmpeq_epi32(ta,zero));   // exact zero product (operands are normal-or-zero)
  }
  if(_mm_testc_si128(_mm_or_si128(ok,z),vuLaneMask(destBits))){
   R5 r;r.v=f;
   r.m=(static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(rb,zero))))&destBits)|((static_cast<uint32_t>(_mm_movemask_ps(f))&destBits)<<4);
   if(productSum){
    const __m128i pb=_mm_and_si128(_mm_castps_si128(product),_mm_set1_epi32(0x7FFFFFFF));
    r.x=(static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(pb,zero))))&destBits)|((static_cast<uint32_t>(_mm_movemask_ps(product))&destBits)<<4);
   }else r.x=0u;
   return r;
  }
 }
 const uint64_t mx=aot5Cold(s,t,a,f,product,kind,src,destBits);
 R5 r;r.m=uint32_t(mx);r.x=uint32_t(mx>>32);r.v=aot5ColdValue(f,r.m);return r;
}
// runFast's per-cycle XGKICK stall (ready = m_cycle+1 while PATH1 is active), in bulk: step to the cycle where the
// kick can next change state (end of its current tag), never past end-1 (runFast hands back when ready >= end).
// Returns false if the budget end was reached with the kick still active (round 3 continues the stall from there).
__declspec(noinline) inline bool aot5StallXgkick(AotVU& v,uint64_t end){
 v.m_viBranchBackupValid=false;   // a stall expires the integer-branch bypass
 while(v.m_xgkick.active){
  if(v.m_cycle+1u>=end)return false;
  const uint32_t qwords=v.m_xgkick.currentTagEnd?(v.m_xgkick.currentTagEnd-v.m_xgkick.copiedBytes)/16u:1u;
  const uint64_t need=uint64_t(qwords)*2u;
  const uint64_t step=need>v.m_xgkick.cycleCredit?need-v.m_xgkick.cycleCredit:1u;
  uint64_t next=v.m_cycle+step;
  if(next>end-1u)next=end-1u;
  v.advanceTo(next);
 }
 return true;
}
__forceinline void aot4Store(uint8_t* data,uint32_t addr,const uint32_t* words,uint8_t dest){
 const __m128i mask=vuLaneMask(kVuRev4[dest]);
 const __m128i prior=_mm_loadu_si128(reinterpret_cast<const __m128i*>(data+addr));
 const __m128i value=_mm_loadu_si128(reinterpret_cast<const __m128i*>(words));
 _mm_storeu_si128(reinterpret_cast<__m128i*>(data+addr),_mm_or_si128(_mm_andnot_si128(mask,prior),_mm_and_si128(mask,value)));
}
// In-flight FMAC flag entry (updateFmacFlags / fastFmacCommit layout), ready at issue+4.
__declspec(noinline) inline void aot4PushFlag(AotVU& v,uint64_t issue,uint32_t mac,uint32_t cur,uint32_t extra){
 for(auto& e:v.m_flagPipeline)if(!e.valid){
  e.readyCycle=issue+AotVU::kFmacLatency;e.issueCycle=issue;e.mac=mac;e.status=cur;e.extraSticky=extra;e.clip=0u;
  e.valid=true;e.writesMac=true;e.writesStatus=true;e.writesSticky=false;e.writesClip=false;
  v.scheduleCommit(e.readyCycle,static_cast<uint32_t>(&e-v.m_flagPipeline.data()));return;}
 v.reportReservedInstruction(true,0xFFFFFFFFu);
}
__declspec(noinline) inline void aot4PushClip(AotVU& v,uint64_t issue,uint32_t clip){
 for(auto& e:v.m_flagPipeline)if(!e.valid){
  e={};e.valid=true;e.issueCycle=issue;e.readyCycle=issue+AotVU::kFmacLatency;e.clip=clip;e.writesClip=true;
  v.scheduleCommit(e.readyCycle,static_cast<uint32_t>(&e-v.m_flagPipeline.data()));return;}
 v.reportReservedInstruction(true,0xFFFFFFFDu);
}
__declspec(noinline) inline void aot4PushP(AotVU& v,uint64_t ready,float value){
 for(auto& e:v.m_efu)if(!e.valid){
  e.valid=true;e.readyCycle=ready;e.value=value;
  v.scheduleCommit(ready,9u+static_cast<uint32_t>(&e-v.m_efu.data()));return;}
 v.reportReservedInstruction(false,0xFFFFFFF9u);
}
'''

def wr(path,text):
 if __import__('os').environ.get('AOT4_DRY'):return
 if not path.exists() or path.read_text()!=text:path.write_text(text)
def load_profile():
 """lane aot5: branch-direction / entry profile (aot5_profile.py writes aot5_profile.json from AOT5_RSTATS runs of a
 --no-traces build over the dump sets). Keys: exits['<img>:<pc0>'] = {exit_pc: count}; entries['<img>'] = [pc, ...]
 (pcs the chain was entered at that are not region starts, e.g. JALR jump-table targets)."""
 p=R/'aot5_profile.json'
 if '--no-profile' in sys.argv or not p.exists():return {'exits':{},'entries':{}}
 return json.loads(p.read_text())
TRACE_MAX=int(next((a.split('=')[1] for a in sys.argv if a.startswith('--trace-max=')),'128'))
def unguarded_xg(region):
 """XGKICK pairs that cannot get a guard exit (pair 0, or a delay slot)."""
 return [k for k,f in enumerate(region['facts']) if f['lk']=='xgkick' and (k==0 or region['facts'][k-1]['lk']=='branch')]
def build_trace(R,byimg_pc,prof):
 """lane aot5: extend base region R along its successors into a trace (superblock with side exits)."""
 path=[R];total=len(R['pairs']);edges=[]
 has_xg=any(f['lk']=='xgkick' for f in R['facts'])
 while True:
  cur=path[-1];pairs=cur['pairs'];facts=cur['facts']
  kind=None
  if cur['stop']=='cap':succ=(pairs[-1]['pc']+8)&0x3fff;kind='fall'
  elif cur['stop']=='unit' and len(pairs)>=2 and facts[-2]['lk']=='branch' and not pairs[-2]['e']:
   d=pairs[-2];bop=facts[-2]['op'];tgt=(d['pc']+8+imm11(d['lo'])*8)&0x3fff;nxt=(d['pc']+16)&0x3fff
   if bop in (0x20,0x21):succ=tgt;kind='uncond'
   elif bop in COND_OPS:
    ex=prof['exits'].get(f'{cur["img"]}:{cur["pc0"]}',{})
    t=ex.get(str(tgt),0);nt=ex.get(str(nxt),0)
    if t+nt==0 or tgt==nxt:break
    follow=t>=nt;succ=tgt if follow else nxt;kind='cond'
   else:break
  else:break
  S2=byimg_pc.get((cur['img'],succ))
  if S2 is None or S2.get('loop') or S2 in path or total+len(S2['pairs'])>TRACE_MAX:break
  if any(f['lk']=='xgkick' for f in S2['facts']):
   if unguarded_xg(S2) and has_xg:break        # a second kick must be guardable
   has_xg=True
  edges.append((kind,succ,(follow if kind=='cond' else True)))
  path.append(S2);total+=len(S2['pairs'])
 if len(path)==1:return R
 pairs=[];facts=[];internal={}
 for c,comp in enumerate(path):
  off=len(pairs);pairs+=comp['pairs'];facts+=comp['facts']
  if c<len(path)-1:
   kind,succ,follow=edges[c]
   if kind in ('uncond','cond'):
    d=comp['pairs'][-2];tgt=(d['pc']+8+imm11(d['lo'])*8)&0x3fff;nxt=(d['pc']+16)&0x3fff
    internal[off+len(comp['pairs'])-2]=dict(follow=follow,tgt=tgt,nxt=nxt,cond=(kind=='cond'))
 T=dict(img=R['img'],pc0=R['pc0'],pairs=pairs,facts=facts,stop=path[-1]['stop'],internal=internal,components=[f'{c["pc0"]}' for c in path])
 schedule(T);T['loop']=False
 return T
def main():
 low_priority()
 man,plans,tables,dec=load()
 prof=load_profile()
 allregions=[];reasons={}
 for img,image in enumerate(man['images']):
  if img not in tables:continue
  extra=set(int(x) for x in prof['entries'].get(str(img),[]))
  if '--no-xg-leaders' not in sys.argv:
   # lane aot5: every XGKICK pc outside a delay slot starts a region too (entered after a PATH1-busy guard exit)
   ds=dec[img];tb=tables[img]
   for n,name in enumerate(tb):
    if name=='nullptr' or n==0 or tb[n-1]=='nullptr':continue
    try:
     if lower_kind(ds[n])=='xgkick' and lower_kind(ds[n-1])!='branch':extra.add(n*8)
    except Unsupported:pass
  regs,rs=form_regions(img,image,tables[img],dec[img],plans,extra)
  for k,v in rs.items():reasons[k]=reasons.get(k,0)+v
  allregions+=regs
 base=[]
 for region in allregions:
  region['facts']=[pair_facts(d) for d in region['pairs']]
  schedule(region)
  region['loop']=('--no-loops' not in sys.argv) and loop_ok(region) and sum(1 for f in region['facts'] if f['lk']=='xgkick')<=1
  base.append(region)
 byimg_pc={(r['img'],r['pc0']):r for r in base}
 bodies=[];kept=[];ntraces=0
 for region in base:
  cand=[region]
  if '--no-traces' not in sys.argv and not region['loop']:
   T=build_trace(region,byimg_pc,prof)
   if T is not region:cand=[T,region]
  for r in cand:
   fname=f's4_{r["img"]}_{r["pc0"]}';r['id']=len(kept)
   try:body=emit_region(r,fname)
   except Unsupported as e:
    reasons['emit '+str(e)]=reasons.get('emit '+str(e),0)+1;continue
   r['name']=fname;bodies.append((fname,body,len(r['pairs'])));kept.append(r)
   if r is not region:ntraces+=1
   break
 vt=(G/'vector_template.inc').read_text()
 a=vt.index('template<uint32_t U,bool NS,bool NT,bool NA>');fn=vt[a:]
 assert '__forceinline VectorResult vectorFmac(' in fn
 fn=fn.replace('__forceinline VectorResult vectorFmac(','__forceinline VectorResult4 vectorFmac4(',1).replace('aotNormalizeOperand(','aot4Norm1(')
 assert 'return {f,0u,sign,0u,0u,extraSticky};' in fn and 'return vectorCold(s,t,a,f,product,kind,src,destBits);' in fn
 fn=fn.replace('return {f,0u,sign,0u,0u,extraSticky};','return {f,0u,sign,0u,0u,extraSticky,sign?2u:0u};')
 fn=fn.replace('return vectorCold(s,t,a,f,product,kind,src,destBits);','{const VectorResult c=vectorCold(s,t,a,f,product,kind,src,destBits);return {c.value,c.zero,c.sign,c.under,c.over,c.extra,(c.zero?1u:0u)|(c.sign?2u:0u)|(c.under?4u:0u)|(c.over?8u:0u)};}')
 wr(G/'s4_common.h',COMMON+'\n// vectorFmac4: copy of the round-3 vectorFmac (vector_template.inc) with force-inlined scalar normalisation.\n'+fn+'\n')
 # bounded TUs
 units=[];cur=[];size=0
 for fname,body,n in bodies:
  if cur and size+n>TU_PAIRS:units.append(cur);cur=[];size=0
  cur.append(body);size+=n
 if cur:units.append(cur)
 sources=[]
 for k,u in enumerate(units):
  name=f's4_unit_{k:03d}.cpp';sources.append(name)
  wr(G/name,'// PRIVATE generated (lane aot4/aot5): statically scheduled VU1 regions and traces.\n#include "s4_common.h"\n'+'\n'.join(u)+'\n')
 names=','.join(f'"{r["name"]}"' for r in kept)
 stats='#include "s4_common.h"\n#include <cstdio>\nuint64_t g_aot4Count[4]={};\n'+f'uint64_t g_aot4R[{max(1,len(kept))}][12]={{}};\nstatic const char* const g_aot4Names[]={{{names}}};\n'+'void aot4DumpRegionStats(const char* path){FILE*f=fopen(path,"w");if(!f)return;for(unsigned k=0;k<sizeof(g_aot4Names)/sizeof(g_aot4Names[0]);++k){if(!g_aot4R[k][0])continue;fprintf(f,"%s",g_aot4Names[k]);for(int j=0;j<12;++j)fprintf(f," %llu",g_aot4R[k][j]);fprintf(f,"\\n");}fclose(f);}\n'
 wr(G/'s4_stats.cpp',stats);sources.append('s4_stats.cpp')
 # chain integration
 p=G/'direct_chain.cpp';s=p.read_text();pristine=G/'direct_chain.r3.cpp'
 if 'aot4' in s:s=pristine.read_text()   # rerun: start from the saved round-3 chain
 elif not __import__('os').environ.get('AOT4_DRY'):pristine.write_text(s)
 decl=[f'bool {r["name"]}(AotVU&,uint8_t*,uint32_t,uint64_t,bool&);' for r in kept]
 r3names=sorted(set(re.findall(r'(?m)^if\(!(block_\w+)\(v,cs,data,size,gs,memory,end,ended\)\)return false;',s)));r3idx={n:k for k,n in enumerate(r3names)}
 decl+=['#if defined(AOT5_RSTATS)','#include <cstdio>','#include <map>',f'static uint64_t g_aot5R3[{max(1,len(r3names))}][2];static uint64_t g_aot5R3c0;',
  '#define AOT5_R3B(k) (g_aot5R3c0=v.m_cycle)','#define AOT5_R3E(k) (++g_aot5R3[k][0],g_aot5R3[k][1]+=v.m_cycle-g_aot5R3c0)',
  'static const char* const g_aot5R3Names[]={'+','.join(f'"{n}"' for n in r3names)+'};',
  'void aot5DumpR3Stats(const char* path){FILE*f=fopen(path,"w");if(!f)return;for(unsigned k=0;k<sizeof(g_aot5R3Names)/sizeof(g_aot5R3Names[0]);++k)if(g_aot5R3[k][0])fprintf(f,"%s %llu %llu\\n",g_aot5R3Names[k],g_aot5R3[k][0],g_aot5R3[k][1]);fclose(f);}',
  # lane aot5 profile: region exit pcs (branch directions) and chain entries at non-region pcs
  'static const char* const g_aot5RNames[]={'+names+'};',
  'static std::map<uint64_t,uint64_t> g_aot5X,g_aot5E;',
  '#define AOT5_XREC(id) (++g_aot5X[(uint64_t(id)<<32)|v.m_state.pc])',
  'void aot5Erec(int img,uint32_t pc,uint32_t pc0){++g_aot5E[(uint64_t(img)<<32)|pc];}',
  'void aot5DumpProfile(const char* path){FILE*f=fopen(path,"w");if(!f)return;for(auto&[k,n]:g_aot5X)fprintf(f,"X %s %u %llu\\n",g_aot5RNames[k>>32],unsigned(k&0xffffffffu),n);for(auto&[k,n]:g_aot5E)fprintf(f,"E %u %u %llu\\n",unsigned(k>>32),unsigned(k&0xffffffffu),n);fclose(f);}',
  '#else','#define AOT5_R3B(k) ((void)0)','#define AOT5_R3E(k) ((void)0)','#define AOT5_XREC(id) ((void)0)','#endif']
 s=s.replace('#include "runtime/gs/gs_frontend.h"\n','#include "runtime/gs/gs_frontend.h"\n// aot4 regions\n'+'\n'.join(decl)+'\n',1)
 byimg={}
 for r in kept:byimg.setdefault(r['img'],{})[r['pc0']]=r
 parts=re.split(r'(?m)^(bool chain_\d+\(AotVU& v.*\{)$',s)
 out=[parts[0]]
 patched=0;stubs=0
 for k in range(1,len(parts),2):
  head=parts[k];body=parts[k+1];img=int(re.match(r'bool chain_(\d+)',head)[1])
  table=tables[img];regs=byimg.get(img,{})
  stub_pcs={pc0 for pc0 in regs if table[pc0//8]!='nullptr' and plans[table[pc0//8]]['pcs'][0]!=pc0 and re.search(rf'(?m)^case {pc0}u:goto {table[pc0//8]};$',body)}
  def gos_for(r):
   # lane aot5: static successors of the region (fall-through pc, branch target, side exits) -> direct gotos
   gos=''
   for xpc in r.get('exits',[]):
    xn=table[xpc//8] if 0<=xpc<16384 else 'nullptr'
    if xn!='nullptr' and plans[xn]['pcs'][0]==xpc:gos+=f'if(v.m_state.pc=={xpc}u)goto {xn};'
    elif xpc in stub_pcs:gos+=f'if(v.m_state.pc=={xpc}u)goto r5e_{xpc};'
   return gos
  def rep(m):
   nonlocal patched
   name=m[1]
   pc0=plans[name]['pcs'][0]
   r=regs.get(pc0)
   if not r or table[pc0//8]!=name:return m[0]
   patched+=1
   return f'{name}:\nif({r["name"]}(v,data,size,end,ended)){{AOT5_XREC({r["id"]});if(ended||v.m_cycle>=end||v.m_stopRequested)return true;{gos_for(r)}goto entry;}}\nif(!{name}(v,cs,data,size,gs,memory,end,ended))return false;'
  body=re.sub(r'(?m)^(block_\w+):\nif\(!\1\(v,cs,data,size,gs,memory,end,ended\)\)return false;',rep,body)
  # lane aot5: regions at chain entry pcs inside a round-3 block (profiled JALR targets): own label, switch case retargeted
  st=[]
  for pc0 in sorted(stub_pcs):
   r=regs[pc0];name=table[pc0//8]
   body=re.sub(rf'(?m)^case {pc0}u:goto {name};$',f'case {pc0}u:goto r5e_{pc0};',body)
   st.append(f'r5e_{pc0}:\nif({r["name"]}(v,data,size,end,ended)){{AOT5_XREC({r["id"]});if(ended||v.m_cycle>=end||v.m_stopRequested)return true;{gos_for(r)}goto entry;}}\ngoto {name};')
   stubs+=1
  if st:
   body=body.replace('default:return false;}\n','default:return false;}\n'+'\n'.join(st)+'\n',1)
  # lane aot5: round-3 block counters (AOT5_RSTATS harness builds only): calls + guest cycles per round-3 block
  body=re.sub(r'(?m)^if\(!(block_\w+)\(v,cs,data,size,gs,memory,end,ended\)\)return false;',lambda m:f'AOT5_R3B({r3idx[m[1]]});if(!{m[1]}(v,cs,data,size,gs,memory,end,ended))return false;AOT5_R3E({r3idx[m[1]]});',body)
  out+= [head,body]
 s=''.join(out)
 wr(p,s)
 m=json.loads((G/'manifest.json').read_text())
 m['sources']=[x for x in m['sources'] if not x.startswith('s4_')]+sources
 wr(G/'manifest.json',json.dumps(m,indent=2))
 summary=dict(loops=sum(1 for r in kept if r.get('loop')),regions=len(kept),traces=ntraces,pairs=sum(len(r['pairs']) for r in kept),patched_labels=patched,entry_stubs=stubs,units=len(units),reasons=reasons,
  list=[dict(name=r['name'],pcs=[r['pairs'][0]['pc'],r['pairs'][-1]['pc']],n=len(r['pairs']),L=r['L'],stalls=len(r['stalls']),stop=r['stop'],components=r.get('components'),side_exits=len(r.get('side_exit_pcs',[]))) for r in kept])
 wr(G/'aot4_regions.json',json.dumps(summary,indent=1))
 print('aot4/aot5 regions',len(kept),'traces',ntraces,'loops',summary['loops'],'pairs',summary['pairs'],'labels',patched,'entry stubs',stubs,'units',len(units),'stops',reasons)
if __name__=='__main__':main()
