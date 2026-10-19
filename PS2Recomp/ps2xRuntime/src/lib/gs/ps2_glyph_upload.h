#pragma once
#include "../ps2_glyphs.h"
#include "../ps2_remap_picture.h"
#include <cstdio>
// GAME(transformers): upload observer shared by raw PGS, CPU, direct HLE uploads and VU1 replay.
namespace ps2_glyphs {
struct Upload { uint64_t blt=0,pos=0,reg=0,dir=3; };
inline uint64_t word(const uint8_t*p){uint64_t v;std::memcpy(&v,p,8);return v;}
inline bool same(const Input&a,const Input&b){if(a.style!=b.style)return false;for(int i=0;i<16;++i)if(a.binding[i].pad!=b.binding[i].pad || a.binding[i].key!=b.binding[i].key)return false;return true;}
struct Font { Upload upload; std::vector<uint8_t> original; Input applied; bool hasApplied=false, modified=false, picture=false; };
struct UploadState {
 Upload regs,active;
 uint64_t remaining=0;
 bool candidate=false;
 std::vector<uint8_t> bytes;
 Palette pal{};bool havePalette=false, havePicturePalette=false;
 std::vector<Font> fonts;
 bool injecting=false;
 uint64_t version=0;
 // qualification F5: last generated payload (key = input + palette availability + original bytes).
 // qualification F11: one cache entry per kind, [0] = DECK.ITF font, [1] = controller picture (remapce1).
 std::vector<uint8_t> cacheOriginal[2],cacheOut[2]; Input cacheIn[2]{}; bool cacheValid[2]{},cachePal[2]{},cacheModified[2]{};
 static bool enabled(){return selected(Style::Xbox)!=Style::Off;}
 void forgetOverlaps(bool keepExact=false) {
 const uint64_t base=(active.blt>>32)&0x3fff;
 const unsigned w=unsigned(active.reg&4095),h=unsigned((active.reg>>32)&4095),bw=unsigned((active.blt>>48)&63),psm=unsigned((active.blt>>56)&63);
 const bool ct16=psm==2 || psm==10 || psm==0x32 || psm==0x3a;
 const unsigned pageHeight=ct16?64:32,bytesPerPixel=ct16?2:4;
 uint64_t span=(std::max)(uint64_t(32),((uint64_t(h)+pageHeight-1)/pageHeight)*(std::max)(1u,bw)*32);
 span=(std::max)(span,(uint64_t(w)*h*bytesPerPixel+255)/256);
 fonts.erase(std::remove_if(fonts.begin(),fonts.end(),[&](const Font&f){
   if(keepExact && f.upload.blt==active.blt && f.upload.pos==active.pos)return false;
   const uint64_t fb=(f.upload.blt>>32)&0x3fff,blocks=f.original.size()/256;
   return span>=16384 || ((fb-base)&16383)<span || ((base-fb)&16383)<blocks;
 }),fonts.end());
 }
 void reg(uint8_t addr,uint64_t value) {
 if(addr==0x50)regs.blt=value;if(addr==0x51)regs.pos=value;if(addr==0x52)regs.reg=value;
 if(addr!=0x53)return;
 regs.dir=value;active=regs;bytes.clear();candidate=false;remaining=0;
 if((value&3)!=0)return;
 const unsigned psm=unsigned((regs.blt>>56)&63),w=unsigned(regs.reg&4095),h=unsigned((regs.reg>>32)&4095);
 unsigned bits=32;if(psm==1 || psm==0x31)bits=24;else if(psm==2 || psm==10 || psm==0x32 || psm==0x3a)bits=16;else if(psm==0x13 || psm==0x1b)bits=8;else if(psm==0x14 || psm==0x24 || psm==0x2c)bits=4;
 remaining=(uint64_t(w)*h*bits+7)/8;
 // Only exact CT32 origins/stride can be patched with this upload-format permutation.
 candidate=(psm==0 || psm==2) && ((regs.pos>>32)&0x7ff)==0 && ((regs.pos>>48)&0x7ff)==0 && ((regs.pos>>59)&3)==0 &&
   ((psm==0 && remaining==65536 && w==128 && h==128 && ((regs.blt>>48)&63)==2) || (psm==2 && remaining==32768 && w==128 && h==128 && ((regs.blt>>48)&63)==2) || (psm==0 && (remaining==1024 || remaining==64)));
 }
 template<class Emit> void apply(Font&f,Emit&&emit,bool force=false) {
 Input in=snapshot();in.style=selected(in.style);
 if(!force && f.hasApplied && same(in,f.applied))return;
 if(f.picture){if(in.style!=Style::Off && !(in.style==Style::PS && originalPadBindings(in)) && !havePicturePalette)return;}
 else if(in.style!=Style::Off && in.style!=Style::PS && !havePalette)return;
 if(!f.picture && in.style==Style::PS && !havePalette){ // Original-cell copies need no palette, except unsupported/unbound sources.
   for(const auto &c:cells)if(psSource(in.binding[c.button].pad)<0)return;
 }
 const int k=f.picture?1:0;const bool hp=f.picture?havePicturePalette:havePalette;
 if(!(cacheValid[k] && cachePal[k]==hp && same(in,cacheIn[k]) && cacheOriginal[k]==f.original)) {
   cacheOut[k]=f.picture?ps2_remap::buildPicture(f.original,in):build(f.original,pal,in);cacheModified[k]=cacheOut[k]!=f.original;
   cacheOriginal[k]=f.original;cacheIn[k]=in;cachePal[k]=hp;cacheValid[k]=true;
 }
 const auto &out=cacheOut[k];
 const bool modified=cacheModified[k];
 if(modified || f.modified)emit(f.upload,out);
 f.applied=in;f.hasApplied=true;f.modified=modified;
 }
 template<class Emit> void image(const uint8_t*p,size_t n,Emit&&emit) {
 if((active.dir&3)!=0 || remaining==0)return;
 const size_t take=size_t(std::min<uint64_t>(remaining,n));
 if(candidate)bytes.insert(bytes.end(),p,p+take);
 remaining-=take;if(remaining)return;
 bool matched=false;
 if(candidate && bytes.size()==65536 && hash(bytes.data(),bytes.size())==0xf11fc649076ec385ull) {
 forgetOverlaps(true);
 matched=true;
 // TEMP(transformers): opt-in identity log (also reports untouched PS uploads).
 if(const char *log=std::getenv("PS2X_GLYPHS_LOG");log && *log=='1')std::fprintf(stderr,"[glyphs] match deck dbp=%llu bytes=65536\n",static_cast<unsigned long long>((active.blt>>32)&0x3fff));
 Font *f=nullptr;for(auto &v:fonts)if(v.upload.blt==active.blt && v.upload.pos==active.pos)f=&v;
 if(!f){fonts.push_back(Font{});f=&fonts.back();}
 f->upload=active;f->original=bytes;f->hasApplied=false;f->modified=false;apply(*f,emit,true);
 }
 if(candidate && bytes.size()==32768 && ((active.blt>>56)&63)==2 && hash(bytes.data(),bytes.size())==0x0e39608fa85576a3ull) {
 forgetOverlaps(true);
 matched=true;
 // TEMP(transformers): opt-in controller identity log.
 if(const char *log=std::getenv("PS2X_GLYPHS_LOG");log && *log=='1')std::fprintf(stderr,"[glyphs] match controller dbp=%llu bytes=32768\n",static_cast<unsigned long long>((active.blt>>32)&0x3fff));
 Font *f=nullptr;for(auto &v:fonts)if(v.upload.blt==active.blt && v.upload.pos==active.pos)f=&v;
 if(!f){fonts.push_back(Font{});f=&fonts.back();}
 f->upload=active;f->original=bytes;f->picture=true;f->hasApplied=false;f->modified=false;apply(*f,emit,true);
 }
 if(candidate && bytes.size()==64 && ((active.blt>>56)&63)==0 && hash(bytes.data(),bytes.size())==0x6ddeadc43130dd7eull) {
 forgetOverlaps();
 matched=true;
 // qualification F12: the picture does not depend on this palette's colours; re-apply only when it first appears.
 if(!havePicturePalette){havePicturePalette=true;for(auto &f:fonts)if(f.picture)apply(f,emit,true);}
 }
 if(candidate && bytes.size()==1024 && hash(bytes.data(),bytes.size())==0x3f0a542b319223ffull) {
 forgetOverlaps();
 // TEMP(transformers): opt-in palette identity log.
 if(const char *log=std::getenv("PS2X_GLYPHS_LOG");log && *log=='1')std::fprintf(stderr,"[glyphs] match palette bytes=1024\n");
 { const Palette np=palette(bytes.data());const bool changed=!havePalette || np!=pal; // qualification F6
   pal=np;havePalette=true;if(changed){cacheValid[0]=false;for(auto &f:fonts)if(!f.picture)apply(f,emit,true);} }
 matched=true;
 }
 // Forget a cached destination reused for unrelated uploads. Conservative span, in 256-byte GS blocks.
 if(!matched)forgetOverlaps();
 candidate=false;bytes.clear();
 }
 template<class Emit> void refresh(Emit&&emit) {
 // Never interrupt a split IMAGE/HWREG transfer or a readback/local-copy operation.
 if(remaining || ((regs.dir&3)!=0 && (regs.dir&3)!=3))return;
 if(fonts.empty())return;
 const uint64_t v=inputVersion.load(std::memory_order_acquire);if(v==version)return;
 for(auto &f:fonts)apply(f,emit);
 version=v;
 }
 // Observe and split ONLY at complete GIF tag boundaries. Feed original bytes before the overwrite;
 // subsequent drawing/setup in the same packet must reach PGS after the replacement.
 template<class Feed,class Emit> bool packet(const uint8_t*p,uint32_t n,Feed&&feed,Emit&&emit) {
 uint32_t o=0,start=0;
 while(o+16<=n) {
 uint64_t lo=word(p+o),hi=word(p+o+8);unsigned loops=unsigned(lo&32767),fmt=unsigned((lo>>58)&3),nr=unsigned((lo>>60)&15);if(!nr)nr=16;
 uint64_t sz=fmt==0?uint64_t(loops)*nr*16:fmt==1?((uint64_t(loops)*nr+1)&~1ull)*8:uint64_t(loops)*16;
 if(sz>n-o-16)break;
 unsigned begin=o+16,end=begin+unsigned(sz);
 // Collect emissions until this original tag has reached the backend.
 std::vector<std::pair<Upload,std::vector<uint8_t>>> pending;
 auto defer=[&](const Upload&u,const std::vector<uint8_t>&b){pending.emplace_back(u,b);};
 // qualification F3: only tags with an A+D register can carry TRX*/HWREG writes; VU1 geometry tags are skipped whole.
 // qualification F10: visit only the A+D slots (no per-qword modulo); only TRX*/HWREG addresses (0x50..0x54) matter.
 unsigned ad[16],nad=0;if(fmt==0)for(unsigned r=0;r<nr;++r)if(((hi>>(r*4))&15)==14)ad[nad++]=r;
 if(fmt==0 && nad){for(unsigned l=0;l<loops;++l)for(unsigned k=0;k<nad;++k){const uint8_t*q=p+begin+(l*nr+ad[k])*16;const unsigned a=q[8];
   if(a<0x50 || a>0x54)continue;const uint64_t v=word(q);if(a==0x54){uint8_t b[8];std::memcpy(b,&v,8);image(b,8,defer);}else reg(uint8_t(a),v);}}
 else if(fmt==2)image(p+begin,unsigned(sz),defer);
 if(!pending.empty()){feed(p+start,end-start);start=end;for(auto &v:pending)emit(v.first,v.second);}
 o=end;
 }
 if(!start)return false; // Observe-only: caller continues its original path without recursive submission.
 if(start<n)feed(p+start,n-start);
 return true;
 }
};
inline std::vector<uint8_t> uploadPacket(const Upload &u,const std::vector<uint8_t>&b) {
 std::vector<uint8_t> out(96+b.size());auto put=[&](size_t o,uint64_t v){std::memcpy(out.data()+o,&v,8);};
 put(0,4ull|(1ull<<60));put(8,14);const uint64_t values[]={u.blt,u.pos,u.reg,0};
 for(int i=0;i<4;++i){put(16+i*16,values[i]);put(24+i*16,0x50+i);}
 put(80,uint64_t(b.size()/16)|(1ull<<15)|(2ull<<58));std::memcpy(out.data()+96,b.data(),b.size());return out;
}
inline std::vector<uint8_t> restorePacket(const Upload&u) {
 std::vector<uint8_t> out(80);auto put=[&](size_t o,uint64_t v){std::memcpy(out.data()+o,&v,8);};put(0,4ull|(1ull<<15)|(1ull<<60));put(8,14);
 const uint64_t values[]={u.blt,u.pos,u.reg,u.dir};for(int i=0;i<4;++i){put(16+i*16,values[i]);put(24+i*16,0x50+i);}return out;
}
} // namespace
