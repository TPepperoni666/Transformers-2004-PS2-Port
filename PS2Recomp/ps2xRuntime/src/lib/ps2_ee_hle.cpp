// GAME(transformers): exact whole-function EE matrix replacements, opt-in.
#include "ps2_ee_hle.h"
#include "ps2_runtime_macros.h"
#include "ps2_iop_transport.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <stdexcept>
namespace ps2_ee_hle {
namespace { std::atomic<PS2Runtime*> liveRuntime{nullptr}; }
void acquireRuntime(PS2Runtime& rt) {
    PS2Runtime* empty=nullptr;
    if(!liveRuntime.compare_exchange_strong(empty,&rt))
        throw std::runtime_error("PS2Runtime: process-global generated dispatch table permits only one live runtime");
}
void releaseRuntime(PS2Runtime& rt) noexcept {
    PS2Runtime* owner=&rt;
    (void)liveRuntime.compare_exchange_strong(owner,nullptr);
}
void matrixSse(const __m128* a,const __m128* b,__m128 w,__m128* out,__m128* acc,unsigned mode) {
    const __m128 a0=ps2VuOperand(a[0]),a1=ps2VuOperand(a[1]),a2=ps2VuOperand(a[2]),a3=ps2VuOperand(a[3]);
    w=ps2VuOperand(w);
    for(unsigned j=0;j<4;++j) {
        const __m128 v=ps2VuOperand(b[j]);
        __m128 x=ps2VuClampVector(_mm_mul_ps(a0,_mm_shuffle_ps(v,v,0)));
        x=ps2VuClampVector(_mm_add_ps(x,ps2VuClampVector(_mm_mul_ps(a1,_mm_shuffle_ps(v,v,0x55)))));
        x=ps2VuClampVector(_mm_add_ps(x,ps2VuClampVector(_mm_mul_ps(a2,_mm_shuffle_ps(v,v,0xaa)))));
        if(j==3) *acc=x;
        if(!mode || j==3) x=ps2VuClampVector(_mm_add_ps(x,ps2VuClampVector(_mm_mul_ps(a3,mode?w:_mm_shuffle_ps(v,v,0xff)))));
        out[j]=x;
    }
}
Kernel implementation(Level l) {
#ifdef PS2X_EE_HLE_X86
    if(l==Level::avx512) return matrixAvx512;
    if(l==Level::avx2) return matrixAvx2;
#else
    (void)l;
#endif
    return matrixSse;
}
namespace {
using Fn=PS2Runtime::RecompiledFunction;

inline bool normalRange(uint32_t a) {
    if((a&0x1fffffffu)<=PS2_RAM_SIZE-64u &&
       (a<0x40000000u || (a>=0x80000000u && a<0xc0000000u))) return true;
    // READ/WRITE128 align each quad before classifying or wrapping it.
    for(unsigned i=0;i<4;++i) if(PS2Runtime::isSpecialAddress((a+i*16u)&~15u)) return false;
    return true;
}
template<unsigned Mode> void run(uint8_t* rdram,R5900Context* ctx,PS2Runtime* runtime) {
    const State& state=PS2IopTransport::eeHle(*runtime);
#if !PS2X_FPU_OPERAND_CLAMP
    // Result-only clamping admits NaNs; SIMD encoding/compiler operand order can
    // change their sign before saturation. Keep the exact generated function.
    state.originals[Mode](rdram,ctx,runtime);return;
#endif
    const uint32_t dst=GPR_U32(ctx,4), lhs=GPR_U32(ctx,5), rhs=GPR_U32(ctx,6);
    // No shortened IO/scratchpad/VU access sequence: delegate before any mutation.
    if(!normalRange(dst) || !normalRange(lhs) || !normalRange(rhs)) {
        state.originals[Mode](rdram,ctx,runtime); return;
    }
    alignas(64) __m128 a[4],b[4],out[4]; __m128 acc;
    for(unsigned i=0;i<4;++i) {
        a[i]=_mm_castsi128_ps(FAST_READ128(lhs+i*16u));
        b[i]=_mm_castsi128_ps(FAST_READ128(rhs+i*16u));
    }
    state.selected(a,b,_mm_shuffle_ps(ctx->vu0_vf[0],ctx->vu0_vf[0],0xff),out,&acc,Mode);
    for(unsigned i=0;i<4;++i) {
        ctx->vu0_vf[4+i]=a[i];
        ctx->vu0_vf[8+i]=Mode==2?_mm_blend_ps(out[i],b[i],8):out[i];
    }
    ctx->vu0_acc=Mode==2?_mm_blend_ps(acc,ctx->vu0_acc,8):acc;
    constexpr uint32_t stores=Mode==0?0x1b0db0u:Mode==1?0x1b0b84u:0x1b0d34u;
    constexpr uint32_t jr=stores+12;
    for(unsigned i=0;i<3;++i) {
        ctx->pc=stores+i*4;
        WRITE128(dst+i*16u,_mm_castps_si128(ctx->vu0_vf[8+i]));
    }
    const uint32_t target=GPR_U32(ctx,31);
    ctx->pc=jr+4; ctx->in_delay_slot=true; ctx->branch_pc=jr;
    WRITE128(dst+48u,_mm_castps_si128(ctx->vu0_vf[11]));
    ctx->in_delay_slot=false;ctx->pc=target;
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
    (void)runtime->dispatchGuestBranch(rdram,ctx,target,jr,0u,PS2Runtime::GuestBranchKind::Return,"JR $ra");
#endif
    // These leaf functions charge ZERO local cycles. The caller's dispatch charge is unchanged.
}
bool on(const char* key) {const char* v=std::getenv(key);return v && !std::strcmp(v,"1");}
bool off(const char* key) {const char* v=std::getenv(key);return v && !std::strcmp(v,"0");}
}
void full(uint8_t* r,R5900Context* c,PS2Runtime* t) {run<0>(r,c,t);}
void affine(uint8_t* r,R5900Context* c,PS2Runtime* t) {run<1>(r,c,t);}
void affineXyz(uint8_t* r,R5900Context* c,PS2Runtime* t) {run<2>(r,c,t);}
void uninstall(PS2Runtime& rt) {
    State& state=PS2IopTransport::eeHle(rt);
    const uint32_t addresses[]={0x1b0d50,0x1b0b30,0x1b0ce0};
    const Fn hooks[]={full,affine,affineXyz};
    for(unsigned i=0;i<3;++i)
        if(state.originals[i] && rt.lookupFunction(addresses[i])==hooks[i])
            rt.replaceFunction(addresses[i],state.originals[i]);
    state=State{};
}
void install(PS2Runtime& rt,const char* name,uint32_t entry,uint32_t crc,bool valid) {
    // Restore before reloading/reinstalling, disabling or declining identity.
    // The next runtime must never inherit a wrapper with no owned fallback.
    uninstall(rt);
    if(!on("PS2X_EE_HLE")) return;
    if(!acceptedIdentity(name,entry,crc,valid)) {
        // TEMP(transformers): startup-only negative identity evidence.
        std::fprintf(stderr,"[eehle1] declined ELF identity name=%s entry=%08x crc=%08x valid=%d\n",name?name:"",entry,crc,int(valid));
        return;
    }
#if !defined(PS2X_EE_HLE_X86) || defined(USE_SSE2NEON)
    // ARM/SSE2NEON installation awaits release-compiler differential qualification.
    // TEMP(transformers): startup-only architecture evidence.
    std::fprintf(stderr,"[eehle1] declined unqualified architecture\n");
    return;
#endif
    State& state=PS2IopTransport::eeHle(rt);
    state.selected=implementation(choose(std::getenv("PS2X_EE_HLE_ISA"),capabilities()));
    const uint32_t addresses[]={0x1b0d50,0x1b0b30,0x1b0ce0};
    const char* switches[]={"PS2X_EE_HLE_1B0D50","PS2X_EE_HLE_1B0B30","PS2X_EE_HLE_1B0CE0"};
    const Fn hooks[]={full,affine,affineXyz};
    unsigned installed=0;
    for(unsigned i=0;i<3;++i) if(!off(switches[i])) {
        const Fn original=rt.lookupFunction(addresses[i]);
        if(original && original!=hooks[i]) {
            state.originals[i]=original;
            if(rt.replaceFunction(addresses[i],hooks[i])) installed|=1u<<i;
        } else if(original==hooks[i] && state.originals[i]) {
            installed|=1u<<i; // Reinstall retains the real fallback, never the wrapper.
        }
    }
    // TEMP(transformers): integration gate evidence, no hot-path instrumentation.
    std::fprintf(stderr,"[eehle1] enabled mask=0x%x (bit0=1B0D50 bit1=1B0B30 bit2=1B0CE0)\n",installed);
}
}
