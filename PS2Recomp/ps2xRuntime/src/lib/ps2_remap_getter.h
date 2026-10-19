#pragma once
#include "ps2_remap_strings.h"
#include <cstdio>
// Included only by ps2_runtime.cpp, after ps2_runtime.h. No PCH change.
namespace ps2_remap {
inline PS2Runtime::RecompiledFunction originalGetter=nullptr;
inline void getter(uint8_t*ram,R5900Context*ctx,PS2Runtime*runtime) {
    if(ctx->pc!=0x001a9410u){originalGetter(ram,ctx,runtime);return;}
    const uint32_t dst=getRegU32(ctx,4),keyObj=getRegU32(ctx,5),gp=getRegU32(ctx,28);
    const uint32_t ra=getRegU32(ctx,31),sp=getRegU32(ctx,29);
    std::string key;auto k=view(ram,keyObj);
    if(k.valid && k.length<256)key.assign(reinterpret_cast<char*>(ram+k.chars),k.length);
    // Run every original instruction/call/allocation before the host-only edit.
    originalGetter(ram,ctx,runtime);
    // A guest exception/yield must never be mistaken for a completed return.
    if(ctx->pc!=ra || getRegU32(ctx,29)!=sp || !affected(key))return;
    auto in=ps2_glyphs::snapshot();in.style=ps2_glyphs::selected(in.style);
    const int layout=activeLayout(ram,gp);
    const bool changed=rewriteGuest(ram,dst,key,in,layout);
    // TEMP(transformers): IDs only, never log the user's localised text.
    static const bool log=[](){const char*p=std::getenv("PS2X_GLYPHS_LOG");return p && *p=='1';}();
    if(log && changed)std::fprintf(stderr,"[glyphs] string %s layout=%d bytes=%u\n",key.c_str(),layout,view(ram,dst).length);
}
inline void installGetter(PS2Runtime&runtime) {
    auto fn=runtime.lookupFunction(0x001a9410u);
    if(fn && fn!=&getter){originalGetter=fn;runtime.replaceFunction(0x001a9410u,&getter);}
}
}
