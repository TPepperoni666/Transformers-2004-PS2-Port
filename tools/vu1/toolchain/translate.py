"""Conservative image AOT: bounded TUs, constant opcode bodies, block issue plans.

The worktree decoder and float helpers are authoritative. The block entry guard
resolves internal stalls statically; scalar resources and unknown control flow
retain exact interpreter progression. Generated microcode must remain private.
"""
import hashlib,json,subprocess,re,sys
from pathlib import Path
from census import low_priority
R=Path(__file__).resolve().parent
G=R/'generated'
def lanes(m):return [c for c in range(4) if m&(8>>c)]
def bits(m):return [i for i in range(1,16) if m&(1<<i)]
def refs(u):return [('vf',r,c) for r,m in u['read'][:u['count']] for c in lanes(m)]+[('vi',i) for i in bits(u['vr'])]+[('acc',c) for c in lanes(u['ar'])]
def writes(d):
    out={}
    for u,suppress in [(d['l'],d['suppress']),(d['u'],0)]:
        r,m=u['write']
        if r and r!=suppress:
            for c in lanes(m):out['vf',r,c]=u['vfl'] or u['lat']
    for i in bits(d['l']['vw']):out['vi',i]=d['l']['vil'] or d['l']['lat']
    for c in lanes(d['u']['aw']):out['acc',c]=1
    return out
def expr(k):return 'v.m_'+{'vf':'vfReady','vi':'viReady','acc':'accReady'}[k[0]]+''.join(f'[{i}]' for i in k[1:])
def usage(u):
    nums=[u['count'],u['vr'],u['vw'],u['ar'],u['aw'],u['lat'],u['vfl'],u['vil']]
    return 'usage('+','.join('{%s,%s}'%tuple(x) for x in u['read']+[u['write']])+','+','.join(map(str,nums))+f",AotVU::Pipeline({u['pipe']}),"+','.join(str(u[x]) for x in ['wq','wp','rc','wc','delay','reserved'])+')'
def meta(d):return '{'+f"{d['lo']}u,{d['up']}u,"+usage(d['l'])+','+usage(d['u'])+','+','.join(str(d[k]) for k in ['i','e','m','d','t','shadow','suppress'])+'}'
def main():
    low_priority(); census=json.loads((R/'census.json').read_text()); images=census['images']
    # Extract the worktree's fast pair body, retaining exact cross/VI visibility logs.
    s=(R/'native/src/lib/vu/ps2_vu1_core.cpp').read_text()
    original=(R/'reference/src/lib/vu/ps2_vu1_core.cpp').read_text().replace('VU1Interpreter','AotVU')
    start=original.index('        const bool takingPriorBranch',original.index('void AotVU::runFast('))
    end=original.index('        advanceOneCycle();',start)
    body=original[start:end]
    seqstart=body.index('        if (m_cycle < queuedWritesDue)')
    seqend=body.index('        // Scoreboard:',seqstart)
    body=body[:seqstart]+body[seqend:]
    body=body.replace('maxReady = std::max(maxReady, ready);','')
    body=body.replace('fastFmac(upper, nullptr, nullptr)','fmacStatic<D.upper>(nullptr,nullptr)').replace('execUpper(upper);','upperStatic<D.upper>();')
    body=body.replace('execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);','lowerStatic<D.lower,D.upper>(vuData,dataSize,gs,memory);')
    prefix='''template<const AotVU::DecodedInstructionPair& D>
inline bool AotVU::pairStatic(uint32_t codeSize,uint8_t* vuData,uint32_t dataSize,GS& gs,PS2Memory* memory,uint64_t end,bool& programEnded,uint64_t planned) {
    constexpr auto decoded=D;
    uint64_t ready=planned ? planned : readyStatic<D>();
    while (ready>m_cycle) {
        if(ready>=end)return false;
        m_viBranchBackupValid=false;advanceTo(ready);
        ready=planned ? planned : readyStatic<D>();
    }
    if(m_cycle>=end || m_stopRequested)return false;
    // Transformers port: optional standalone coverage counters; absent from production timing.
    #if defined(VU1_AOT_STATS)
    ++aotPairs;
    if(planned)++staticPairs;
    #endif
'''
    clock='''
 ++m_cycle;
 if(m_commitCalendar[m_cycle&63u])commitReadyPipelines();
 if(m_xgkick.active)progressXgkick();
 return true;
}
'''
    if '--event-clock' not in sys.argv:clock='\n advanceOneCycle();return true;\n}\n'
    if '--static-ready' not in sys.argv:prefix=prefix.replace('readyStatic<D>()','calculatePairReadyCycle(D)')
    (G/'pair_template.inc').write_text(prefix+body+clock)
    # Specialize only individual callable FMAC operations, never inline megafunctions.
    begin=original.index('namespace',original.index('// C1: VU1 fast path'))
    fn=original.index('bool AotVU::fastFmac(',begin)
    stop=original.index('// The flag entry',fn)
    fmac=original[begin:stop]
    fmac=fmac.replace('bool AotVU::fastFmac(uint32_t instr, float *target, const float *given)', 'template<uint32_t U,bool NS,bool NT,bool NA>\ninline bool AotVU::fmacStatic(float* target,const float* given)')
    fmac=re.sub(r'\binstr\b','U',fmac)
    fmac=fmac[:fmac.index('    // Double path:')]+'''    // Cold operand classes reuse the shipping exact SIMD/double helper.
    return fastFmac(U,target,given);
}
'''
    commit_start=s.index('void AotVU::fastFmacCommit(')
    commit_begin=s.index('{',commit_start);commit_end=s.index('// calculatePairReadyCycle for the fast path',commit_begin)
    commit_body=s[commit_begin:commit_end]
    commit_code='''template<uint32_t U>
inline void AotVU::commitStatic(float *target,bool toAcc,uint32_t destBits,const uint32_t *valueBits,
 uint32_t zeroOrUnder,uint32_t sign,uint32_t under,uint32_t over,uint32_t extraSticky)
'''+re.sub(r'\binstr\b','U',commit_body)
    local='--inline-commit' in sys.argv
    if local:fmac=fmac.replace('fastFmacCommit(U, target,','commitStatic<U>(target,')
    fmac=fmac.replace('vuNormalize4(_mm_loadu_ps(m_state.vf[fs]))','(NS ? _mm_loadu_ps(m_state.vf[fs]) : vuNormalize4(_mm_loadu_ps(m_state.vf[fs])))').replace('vuNormalize4(_mm_loadu_ps(m_state.vf[ft]))','(NT ? _mm_loadu_ps(m_state.vf[ft]) : vuNormalize4(_mm_loadu_ps(m_state.vf[ft])))').replace('vuNormalize4(_mm_loadu_ps(m_state.acc))','(NA ? _mm_loadu_ps(m_state.acc) : vuNormalize4(_mm_loadu_ps(m_state.acc)))').replace('vuNormalizeOperand(', 'aotNormalizeOperand(')
    insert=fmac.index('template<uint32_t U,bool NS')
    fmac=fmac[:insert]+commit_code+fmac[insert:]
    (G/'fmac_template.inc').write_text(fmac)
    for op in ['lower','upper']:
        p=G/f'{op}_template.inc';text=p.read_text().replace('__declspec(noinline) void AotVU::','inline void AotVU::')
        if local or '--inline-memory' in sys.argv:text=text.replace('queueStore(addr, words, dest);','storeStatic(addr,words,dest);').replace('applyDest(', 'destStatic(')
        else:text=text.replace('storeStatic(addr,words,dest);','queueStore(addr, words, dest);').replace('destStatic(', 'applyDest(')
        text=text.replace('microAddressMask()', '0x3fffu')
        p.write_text(text)
    h=R/'native/include/runtime/ps2_vu1.h';hs=h.read_text()
    if 'public:\n    struct DecodedInstructionPair;' not in hs:
        hs=hs.replace('public:\n','public:\n    struct DecodedInstructionPair;\n',1)
        h.write_text(hs)
    if 'fmacStatic' not in hs:
        hs=hs.replace('    bool aotEnabled', '    template<uint32_t U,bool NS=false,bool NT=false,bool NA=false> bool fmacStatic(float*,const float*);\n    bool aotFlagsImmediate = false;\n    bool aotEnabled')
        h.write_text(hs)
    if 'commitStatic' not in hs:
        hs=hs.replace('    bool aotFlagsImmediate', '''    template<uint32_t U> void commitStatic(float*,bool,uint32_t,const uint32_t*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
    void storeStatic(uint32_t,const uint32_t*,uint8_t);
    void destStatic(float*,const float*,uint8_t);
    bool aotFlagsImmediate''')
        h.write_text(hs)
    if 'readyStatic' not in hs:
        hs=hs.replace('    bool aotFlagsImmediate','    template<const DecodedInstructionPair& D> uint64_t readyStatic() const;\n    bool aotFlagsImmediate')
        h.write_text(hs)
    start=original.index('uint64_t AotVU::calculatePairReadyCycle(');begin=original.index('{',start);end=original.index('\nvoid AotVU::markPairWrites',begin)
    ready_code='template<const AotVU::DecodedInstructionPair& D>\ninline uint64_t AotVU::readyStatic() const '+re.sub(r'\bdecoded\b','D',original[begin:end])
    (G/'ready_template.inc').write_text(ready_code)
    # Make the shared commit callable from the small generated TUs.
    s=s.replace('inline void AotVU::fastFmacCommit','__declspec(noinline) void AotVU::fastFmacCommit')
    commit=s.index('    // updateFmacFlags: MAC',s.index('void AotVU::fastFmacCommit'))
    last=s.index('    // applyDest:',commit)
    if 'if (aotFlagsImmediate)' not in s[s.index('void AotVU::fastFmacCommit'):last]:
        immediate='''    // Transformers port: no MAC/STATUS readers or FSSET anywhere in this image.
    // Keep final MAC/current bits and all sticky bits, without +4 queue history.
    if (aotFlagsImmediate) {
        m_state.mac=kVuRev4[zeroOrUnder] | (kVuRev4[sign]<<4) | (kVuRev4[under]<<8) | (kVuRev4[over]<<12);
        const uint32_t current=(zeroOrUnder?1u:0u)|(sign?2u:0u)|(under?4u:0u)|(over?8u:0u);
        m_state.status=(m_state.status&0xFF0u)|current|((current|extraSticky)<<6);
        m_fastMaxWriteReady=std::max(m_fastMaxWriteReady,m_cycle+kFmacLatency);
    } else {
'''
        s=s[:commit]+immediate+s[commit:last]+'    }\n'+s[last:]
    (R/'native/src/lib/vu/ps2_vu1_core.cpp').write_text(s)
    common='''#pragma once
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "../native/src/lib/vu/ps2_vu1_detail.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <emmintrin.h>
constexpr uint8_t laneForComponent(uint32_t c){return uint8_t(1u<<(3u-c));}
constexpr AotVU::InstructionUsage usage(AotVU::VfAccess a,AotVU::VfAccess b,AotVU::VfAccess w,uint8_t n,uint16_t vr,uint16_t vw,uint8_t ar,uint8_t aw,uint8_t l,uint8_t vl,uint8_t il,AotVU::Pipeline p,bool q,bool e,bool rc,bool wc,bool delay,bool reserved) {
AotVU::InstructionUsage u{};u.vfRead={a,b};u.vfWrite=w;u.vfReadCount=n;u.viRead=vr;u.viWrite=vw;u.accRead=ar;u.accWrite=aw;u.latency=l;u.vfLatency=vl;u.viLatency=il;u.pipeline=p;u.waitQ=q;u.waitP=e;u.readsClip=rc;u.writesClip=wc;u.delaysNextBranchRead=delay;u.reserved=reserved;return u;}
inline void AotVU::destStatic(float* dst,const float* src,uint8_t mask){
 if(dst==m_state.vf[0])return;
 for(unsigned c=0;c<4;++c)if(mask&(8u>>c))dst[c]=src[c];
}
inline void AotVU::storeStatic(uint32_t address,const uint32_t* words,uint8_t mask){
 if(m_activeVuData && address+16u<=m_activeVuDataSize){
  uint32_t value[4];std::memcpy(value,m_activeVuData+address,16);
  for(unsigned c=0;c<4;++c)if(mask&(8u>>c))value[c]=words[c];
  std::memcpy(m_activeVuData+address,value,16);
 }
}
#include "upper_template.inc"
#include "lower_template.inc"
inline float aotNormalizeOperand(float value){
 uint32_t b;std::memcpy(&b,&value,4);uint32_t e=b&0x7f800000u;
 if(e==0)b&=0x80000000u;else if(e==0x7f800000u)b=(b&0x80000000u)|0x7f7fffffu;
 std::memcpy(&value,&b,4);return value;
}
#include "fmac_template.inc"
#include "ready_template.inc"
#include "pair_template.inc"
'''
    (G/'common.h').write_text(common)
    blocks={}; allplans=[]; image_tables=[]; flag_modes=[]
    coverage=json.loads((R/'coverage.json').read_text()) if (R/'coverage.json').exists() else {}
    for image_index,image in enumerate(images):
        name=image['id']; raw=subprocess.run([str(R/'decode.exe'),str(G/f'{name}.code')],capture_output=True,text=True,check=True).stdout
        ds=json.loads(raw);(G/f'{name}_decoded.json').write_text(json.dumps(ds))
        status_readers=[d['pc'] for d in ds if not d['i'] and d['lo']>>25 in (0x14,0x15,0x16,0x17,0x18,0x1a,0x1b)]
        flag_modes.append(False) # Block coalescing keeps delayed tail flags exact across readers and exits.
        # Delay-state CFG closure of all entries and recorded indirect destinations.
        reached=set(coverage.get(str(image_index),[]));todo=[(pc,None,False) for pc in reached|set(map(int,image['entries']))];seen=set();edges={}
        while todo:
            pc,pending,halt=todo.pop();pc&=0x3ff8
            if (pc,pending,halt) in seen:continue
            seen.add((pc,pending,halt));reached.add(pc);d=ds[pc//8]
            if halt or d['l']['reserved'] or d['u']['reserved']:continue
            op=d['lo']>>25;targets=[None]
            if not d['i'] and op in (0x20,0x21,0x28,0x29,0x2c,0x2d,0x2e,0x2f):
                imm=d['lo']&2047;imm=imm-2048 if imm&1024 else imm;target=(pc+8+imm*8)&0x3fff
                targets=[target] if op in (0x20,0x21) else [None,target]
            if not d['i'] and op in (0x24,0x25):continue # unobserved indirect destination dispatches to batch 3
            nxt=(pc+8)&0x3fff if pending is None else pending
            edges.setdefault(str(pc),set()).add(nxt)
            for target in targets:todo.append((nxt,target,bool(d['e'])))
        ranges=[];curr=[];waiting_delay=False;branch_units='--branch-units' in sys.argv
        leaders=set(map(int,image['entries']))
        if branch_units:
            for d in ds:
                if d['pc'] not in reached or d['i']:continue
                op=d['lo']>>25
                if op in (0x20,0x21,0x28,0x29,0x2c,0x2d,0x2e,0x2f):
                    imm=d['lo']&2047;imm=imm-2048 if imm&1024 else imm
                    leaders.update(((d['pc']+8+imm*8)&0x3fff,(d['pc']+16)&0x3fff))
        for d in ds:
            if d['pc'] not in reached:
                if curr:ranges.append(curr);curr=[]
                continue
            barrier=d['l']['pipe'] in (3,4,7) or d['l']['wq'] or d['l']['wp']
            if d['l']['reserved'] or d['u']['reserved']:
                if curr:ranges.append(curr);curr=[]
                continue
            if curr and ((barrier or d['pc'] in leaders) and not waiting_delay):ranges.append(curr);curr=[]
            curr.append(d)
            if waiting_delay:
                ranges.append(curr);curr=[];waiting_delay=False;continue
            if branch_units and (d['l']['pipe']==6 or d['e']) and not (d['d'] or d['t'] or d['m']):
                waiting_delay=True;continue
            # A branch and its delay pair must not share a fixed-plan continuation.
            block_cap=int(sys.argv[sys.argv.index('--pairs-per-block')+1]) if '--pairs-per-block' in sys.argv else 16
            if len(curr)==block_cap or barrier or d['l']['pipe']==6 or d['e'] or d['d'] or d['t'] or d['m']:
                ranges.append(curr);curr=[]
        if curr:ranges.append(curr)
        table=[None]*2048
        for dsblock in ranges:
            fingerprint=hashlib.sha256(json.dumps(dsblock,sort_keys=True).encode()).hexdigest()[:16]
            nameblock='block_'+fingerprint
            for d in dsblock:table[d['pc']//8]=nameblock
            if nameblock in blocks:continue
            ready={};guards={};issues=[];offset=0
            dynamic=any(d['l']['pipe'] in (3,4,7) or d['l']['wq'] or d['l']['wp'] for d in dsblock)
            for d in dsblock:
                reads=refs(d['u'])+refs(d['l']); issue=max([offset]+[ready[k] for k in reads if k in ready]);issues.append(issue)
                for k in reads:
                    if k not in ready:guards[k]=min(guards.get(k,issue),issue)
                for k,l in writes(d).items():ready[k]=issue+l
                offset=issue+1
            lines=[f'bool {nameblock}(AotVU& v,uint32_t cs,uint8_t* data,uint32_t size,GS& gs,PS2Memory* memory,uint64_t end,bool& ended) {{',
                   'uint64_t base=v.m_cycle;bool scheduled=false;', 'switch(v.m_state.pc) {']
            for n,(d,issue) in enumerate(zip(dsblock,issues)):
                lines += [f"case {d['pc']}u:"]
                if n==0:
                    cond=' && '.join(expr(k)+f'<=base+{at}u' for k,at in sorted(guards.items())) or 'true'
                    lines.append('scheduled='+('false;' if dynamic else cond+';'))
                lines += [f"if(!v.pairStatic<{nameblock}_d[{n}]>(cs,data,size,gs,memory,end,ended,scheduled?base+{issue}u:0u))return false;",'if(ended)return true;']
                if n!=len(dsblock)-1:lines += [f"if(v.m_state.pc!={dsblock[n+1]['pc']}u)return true;",'[[fallthrough]];']
                else:lines += ['return true;']
            lines+=['default:return false;','}','}']
            blocks[nameblock]='static constexpr AotVU::DecodedInstructionPair '+nameblock+'_d[]={\n'+',\n'.join(meta(d) for d in dsblock)+'\n};\n'+'\n'.join(lines)
            tail_pairs=2 if branch_units and len(dsblock)>=2 and (dsblock[-2]['l']['pipe']==6 or dsblock[-2]['e']) else 1
            allplans.append(dict(block=nameblock,pcs=[d['pc'] for d in dsblock],issues=issues,guards={str(k):v for k,v in guards.items()},dynamic=bool(dynamic),tail_pairs=tail_pairs))
        image_tables.append(table)
        (G/f"{name}_cfg.json").write_text(json.dumps(dict(entries=image['entries'],status_readers=status_readers,reachable=sorted(reached),edges={k:sorted(v) for k,v in edges.items()},closure='delay-state direct CFG plus observed indirect entries; unknown destinations use batch 3',blocks=len(ranges)),indent=2))
    # Both local and slow twins contribute compiler IR; keep the cap configurable.
    pair_cap=int(sys.argv[sys.argv.index('--pairs-per-tu')+1]) if '--pairs-per-tu' in sys.argv else 48
    chunks=[];current=[];size=0
    plans={p['block']:p for p in allplans}
    for name,body in blocks.items():
        n=len(plans[name]['pcs'])
        if current and size+n>pair_cap:chunks.append(current);current=[];size=0
        current.append((name,body));size+=n
    if current:chunks.append(current)
    sources=[]
    for i,chunk in enumerate(chunks):
        file=f'unit_{i:03d}.cpp'; sources.append(file);(G/file).write_text('#include "common.h"\n'+'\n'.join(b for n,b in chunk))
    registry=['#include "runtime/ps2_vu1.h"','#include "runtime/ps2_memory.h"','#include "runtime/gs/gs_frontend.h"', 'using Block=bool(*)(AotVU&,uint32_t,uint8_t*,uint32_t,GS&,PS2Memory*,uint64_t,bool&);']
    registry += [f'bool {n}(AotVU&,uint32_t,uint8_t*,uint32_t,GS&,PS2Memory*,uint64_t,bool&);' for n in blocks]
    for i,t in enumerate(image_tables):registry += [f'static const Block image_{i}[2048]={{',','.join(n or 'nullptr' for n in t),'};']
    registry += ['static const Block* tables[]={'+','.join('image_'+str(i) for i in range(len(images)))+'};',
                 'bool AotVU::aotDispatch(uint32_t cs,uint8_t*data,uint32_t size,GS&gs,PS2Memory*memory,uint64_t end,bool&ended){',
                 'if(aotImage<0 || aotImage>='+str(len(images))+' || (m_state.pc&7u) || m_state.pc>=16384)return false;',
                 'auto f=tables[aotImage][m_state.pc/8];return f && f(*this,cs,data,size,gs,memory,end,ended);}',
                 'static const bool flagImmediate[]={'+','.join('true' if x else 'false' for x in flag_modes)+'};']
    # Enter only with empty VF/VI/ACC/store queues. Unknown/partial states use batch 3.
    registry += ['''void AotVU::aotFast(uint8_t* code,uint32_t cs,uint8_t*data,uint32_t size,GS&gs,PS2Memory*memory,uint64_t end,bool&ended) {
for(const auto& x:m_vfWritePipeline)if(x.valid){runFast(code,cs,data,size,gs,memory,end,ended);return;}
for(const auto& x:m_viWritePipeline)if(x.valid){runFast(code,cs,data,size,gs,memory,end,ended);return;}
for(const auto& x:m_accWritePipeline)if(x.valid){runFast(code,cs,data,size,gs,memory,end,ended);return;}
    m_fastVfLogCount = 0;
    m_fastViLogCount = 0;
    for (uint32_t r = 0; r < 32u; ++r)
        for (uint32_t c = 0; c < 4u; ++c)
        {
            m_fastVisVf[r][c] = m_state.vf[r][c];
            m_fastHasVf[r][c] = false;
        }
    for (uint32_t r = 0; r < 16u; ++r)
    {
        m_fastVisVi[r] = m_state.vi[r];
        m_fastHasVi[r] = false;
    }
m_fast=true;
aotFlagsImmediate=flagImmediate[aotImage];
commitReadyPipelines();
while(m_cycle<end&&!m_stopRequested&&!ended) {
 if(!aotDispatch(cs,data,size,gs,memory,end,ended)) {
   aotFlagsImmediate=false;m_fast=false;fastMaterializeVf();fastMaterializeVi();
   ++fallbackPairs;runFast(code,cs,data,size,gs,memory,end,ended);return;
 }
}
aotFlagsImmediate=false;m_fast=false;
if(!ended){fastMaterializeVf();fastMaterializeVi();}
}
''']
    (G/'registry.cpp').write_text('\n'.join(registry))
    (G/'schedule.json').write_text(json.dumps(allplans,indent=2))
    (G/'manifest.json').write_text(json.dumps(dict(images=images,sources=sources+['registry.cpp'],blocks=len(blocks),max_pairs_per_tu=pair_cap,max_pairs_per_block=block_cap,flag_immediate=flag_modes),indent=2))
    print('Generated',len(images),'images',len(blocks),'blocks',len(chunks),'bounded TUs')
if __name__=='__main__':main()
