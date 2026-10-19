// TEMP(transformers): output surface belongs to the render thread, independently of GS SSAA.
#include "runtime/ps2_resolution.h"
#include "runtime/ps2_settings.h"
#include "ps2_host_backend.h"
#include "rlgl.h"
#include <cstdlib>
#include <cstdio>
void ps2PresentUpscaleDraw(const Texture2D&,Rectangle,Rectangle);
namespace { RenderTexture2D output{}; std::string applied="desktop",attempted; uint64_t generation=0,attemptedVersion=0; }
const char* ps2ResolutionOutputApplied(){return applied.c_str();}
void ps2ResolutionPresent(const Texture2D& texture,Rectangle src,Rectangle dst) {
 namespace st=ps2x::settings; namespace res=ps2x::resolution;
 const auto wanted=st::get("display.output_resolution");
 if(wanted!=applied && (wanted!=attempted || attemptedVersion!=st::version())) {
    attempted=wanted;attemptedVersion=st::version(); ++generation; res::Mode mode;
    RenderTexture2D replacement{};bool valid=wanted=="desktop";
    if(!valid && res::parseMode(wanted,mode)) {
       const char* fail=std::getenv("PS2X_RESOPT2_FAIL_OUTPUT_GENERATION");
       if(!fail || std::strtoull(fail,nullptr,10)!=generation) {
           replacement=LoadRenderTexture(mode.width,mode.height);
           valid=IsRenderTextureValid(replacement) && rlFramebufferComplete(replacement.id);
       }
    }
    if(valid) {
       rlDrawRenderBatchActive();
       const auto old=output;output=replacement;applied=wanted;
       if(old.id)UnloadRenderTexture(old);
       std::fprintf(stderr,"[resopt2:output] generation=%llu resolution=%s applied\n",(unsigned long long)generation,wanted.c_str());
    } else {
       if(replacement.id)UnloadRenderTexture(replacement);
       auto old=st::entry("display.output_resolution");old.value=applied;st::restoreHostEntries({old});
       std::fprintf(stderr,"[resopt2:output] generation=%llu resolution=%s rejected retained=%s\n",(unsigned long long)generation,wanted.c_str(),applied.c_str());
    }
 }
 if(wanted=="desktop" || !output.id) {ps2PresentUpscaleDraw(texture,src,dst);return;}
 // Output has the same content aspect as the destination; HUD stays part of the guest image.
 // No nested upscale render targets: materialize first, then run the ordinary FSR/CAS pass.
 BeginTextureMode(output);ClearBackground(BLACK);
 SetTextureFilter(texture,st::get("display.scaling_filter")=="nearest"?TEXTURE_FILTER_POINT:TEXTURE_FILTER_BILINEAR);
 DrawTexturePro(texture,src,Rectangle{0,0,float(output.texture.width),float(output.texture.height)},Vector2{0,0},0,WHITE);
 EndTextureMode();
 ps2PresentUpscaleDraw(output.texture,Rectangle{0,0,float(output.texture.width),-float(output.texture.height)},dst);
}
void ps2ResolutionPresentShutdown(){if(output.id)UnloadRenderTexture(output);output={};}
