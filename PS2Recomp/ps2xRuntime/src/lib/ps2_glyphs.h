#pragma once
// GAME(transformers): Own monochrome stroke art; no shipped font/texture data.
#include <array>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cctype>
#include "runtime/gs/ps2_gs_psmt8.h"
namespace ps2_glyphs {
enum class Style { PS, Xbox, Switch, Keyboard, Off };
struct Binding { int pad=-1, key=-1; };
struct Input { Style style=Style::Xbox; std::array<Binding,16> binding{}; };
inline std::mutex inputMutex;
inline Input input = [] { Input v; const int pads[]={13,16,17,15,1,2,3,4,100,101,9,11,5,6,7,8};
 const int keys[]={258,341,345,257,265,262,264,263,340,344,81,69,86,67,88,90};
 for(int i=0;i<16;++i)v.binding[i]={pads[i],keys[i]};return v; }();
inline std::atomic<uint64_t> inputVersion{1};
inline void publish(const Input &v) { std::lock_guard<std::mutex> l(inputMutex); bool changed=input.style!=v.style;for(int i=0;i<16;++i)changed|=input.binding[i].pad!=v.binding[i].pad || input.binding[i].key!=v.binding[i].key;
 if(changed){input=v;inputVersion.fetch_add(1,std::memory_order_release);} }
inline Input snapshot() { std::lock_guard<std::mutex> l(inputMutex); return input; }
inline Style deviceStyle(std::string name, std::string guid="") {
 for(char &c:name)c=char(std::tolower(static_cast<unsigned char>(c)));
 for(char &c:guid)c=char(std::tolower(static_cast<unsigned char>(c)));
 const std::string vendor=guid.size()>=12?guid.substr(8,4):"";
 if(vendor=="5e04" || name.find("xbox")!=name.npos || name.find("xinput")!=name.npos || name.find("legion")!=name.npos || name.find("steam")!=name.npos)return Style::Xbox;
 if(vendor=="7e05" || name.find("nintendo")!=name.npos || name.find("switch")!=name.npos || name.find("pro controller")!=name.npos)return Style::Switch;
 if(vendor=="4c05")return Style::PS;
 if(name.find("dualsense")!=name.npos || name.find("dualshock")!=name.npos || name.find("playstation")!=name.npos || name.find("ps4")!=name.npos || name.find("ps5")!=name.npos || name.find("wireless controller")!=name.npos)return Style::PS;
 if(name.find("nintendo")!=name.npos || name.find("switch")!=name.npos || name.find("pro controller")!=name.npos)return Style::Switch;
 return Style::Xbox; // XInput, Legion/Steam Input and unknown mapped pads.
}
// GAME(transformers) lane glyphs F1: PS2X_GLYPHS is a startup knob - parse it once (selected() runs per GS packet).
enum class Knob { Auto, Off, PS, Xbox, Switch, Keyboard };
inline Knob knobFrom(const char *e) {
 if(!e || !*e || std::strcmp(e,"auto")==0)return Knob::Auto;
 if(std::strcmp(e,"off")==0)return Knob::Off;
 if(std::strcmp(e,"ps")==0 || std::strcmp(e,"playstation")==0)return Knob::PS;
 if(std::strcmp(e,"xbox")==0)return Knob::Xbox;
 if(std::strcmp(e,"switch")==0)return Knob::Switch;
 if(std::strcmp(e,"keyboard")==0)return Knob::Keyboard;
 return Knob::Auto;
}
inline Knob knob() { static const Knob k=knobFrom(std::getenv("PS2X_GLYPHS")); return k; }
inline Style selected(Style automatic) {
 switch(knob()) {case Knob::Off:return Style::Off;case Knob::PS:return Style::PS;case Knob::Xbox:return Style::Xbox;
 case Knob::Switch:return Style::Switch;case Knob::Keyboard:return Style::Keyboard;default:return automatic;}
}
inline const char *styleName(Style s) {
 switch(s) {case Style::PS:return "ps";case Style::Xbox:return "xbox";case Style::Switch:return "switch";case Style::Keyboard:return "keyboard";default:return "off";}
}
inline uint64_t hash(const uint8_t *b,size_t n) {
 uint64_t h=0xcbf29ce484222325ull;
 for(size_t i=0;i<n;i+=8) { uint64_t w=0; for(size_t j=0;j<8 && i+j<n;++j)w|=uint64_t(b[i+j])<<(8*j); h=(h^w)*0x100000001b3ull; } return h;
}
struct Cell { int button,x0,y0,x1,y1; };
inline constexpr Cell cells[]={{15,51,231,74,254},{12,74,231,97,254},{14,5,231,28,254},{13,28,231,51,254},{0,119,207,159,230},{10,62,183,91,202},{8,1,163,30,191},{11,62,163,91,182},{9,32,163,61,191},{1,1,192,30,221},{2,32,192,61,221}};
inline uint32_t c32(int x,int y) {
 static constexpr uint8_t col[8][8]={{0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15},{16,17,20,21,24,25,28,29},{18,19,22,23,26,27,30,31},{32,33,36,37,40,41,44,45},{34,35,38,39,42,43,46,47},{48,49,52,53,56,57,60,61},{50,51,54,55,58,59,62,63}};
 return (((y/32)*2+x/64)*2048+GSPSMT8::blockTable8[(y>>3)&3][(x>>3)&7]*64+col[y&7][x&7])*4;
}
inline std::vector<uint8_t> decode(const uint8_t *b) {
 std::vector<uint8_t> mem(65536),out(65536);
 for(int y=0;y<128;++y)for(int x=0;x<128;++x)for(int k=0;k<4;++k)mem[c32(x,y)+k]=b[(y*128+x)*4+k];
 for(int y=0;y<256;++y)for(int x=0;x<256;++x)out[y*256+x]=mem[GSPSMT8::addrPSMT8(0,4,x,y)]; return out;
}
inline std::vector<uint8_t> encode(const std::vector<uint8_t>&idx) {
 std::vector<uint8_t> mem(65536),out(65536);
 for(int y=0;y<256;++y)for(int x=0;x<256;++x)mem[GSPSMT8::addrPSMT8(0,4,x,y)]=idx[y*256+x];
 for(int y=0;y<128;++y)for(int x=0;x<128;++x)for(int k=0;k<4;++k)out[(y*128+x)*4+k]=mem[c32(x,y)+k]; return out;
}
inline int psSource(int src) {
 switch(src) {case 7:return 14;case 6:return 13;case 8:return 15;case 5:return 12;case 9:return 10;case 10:case 100:return 8;case 11:return 11;case 12:case 101:return 9;case 13:return 0;case 15:return 3;case 16:return 1;case 17:return 2;case 1:return 4;case 2:return 5;case 3:return 6;case 4:return 7;default:return -1;}
}
inline std::string keyLabel(int k) {
 if((k>=65 && k<=90)||(k>=48 && k<=57))return std::string(1,char(k));
 if(k>=320 && k<=329)return "K"+std::to_string(k-320);
 switch(k) {case 32:return "SPC";case 257:return "ENT";case 258:return "TAB";case 259:return "BSP";case 340:return "LSH";case 344:return "RSH";case 341:return "LCT";case 345:return "RCT";case 342:return "LAL";case 346:return "RAL";case 265:return "UP";case 264:return "DN";case 263:return "LT";case 262:return "RT";case 260:return "INS";case 261:return "DEL";case 268:return "HOM";case 269:return "END";case 266:return "PGU";case 267:return "PGD";case 335:return "KEN";case 334:return "+";case 333:return "-";case 332:return "*";case 331:return "/";case 330:return ".";default:if(k>=33 && k<=96)return std::string(1,char(k));return "-";}
}
// lane glyphsc F19: physical PS labels for prose and neutral remapped diagrams.
inline bool originalPadBindings(const Input &in) {
 for(int i=0;i<16;++i)if(psSource(in.binding[i].pad)!=i)return false;return true;
}
inline std::string label(Style s,const Binding &b) {
 if(s==Style::Keyboard)return keyLabel(b.key);
 if(s==Style::PS) {
  switch(psSource(b.pad)) {
  case 0:return "SELECT";case 1:return "L3";case 2:return "R3";case 3:return "START";
  case 4:return "UP";case 5:return "RIGHT";case 6:return "DOWN";case 7:return "LEFT";
  case 8:return "L2";case 9:return "R2";case 10:return "L1";case 11:return "R1";
  case 12:return "TR";case 13:return "O";case 14:return "X";case 15:return "SQ";default:return "-";}
 }
 switch(b.pad) {
 case 7:return s==Style::Switch?"B":"A";case 6:return s==Style::Switch?"A":"B";
 case 8:return s==Style::Switch?"Y":"X";case 5:return s==Style::Switch?"X":"Y";
 case 9:return s==Style::Switch?"L":"LB";case 11:return s==Style::Switch?"R":"RB";
 case 10:case 100:return s==Style::Switch?"ZL":"LT";case 12:case 101:return s==Style::Switch?"ZR":"RT";
 case 13:return s==Style::Switch?"-":"VIEW";case 15:return s==Style::Switch?"+":"MENU";
 case 16:return "LS";case 17:return "RS";case 1:return "UP";case 2:return "RT";case 3:return "DN";case 4:return "LT";case 14:return "HOME";default:return "-"; }
}
// A compact, original 5x7 stroke grid. Each occupied unit is rendered as a short horizontal stroke.
inline const char *grid(char c) {
 static const char* const rows[]={
 "01110100011000111111100011000110001", // A
 "11110100011000111110100011000111110", // B
 "01111100001000010000100001000001111", // C
 "11110100011000110001100011000111110", // D
 "11111100001000011110100001000011111", // E
 "11111100001000011110100001000010000", // F
 "01111100001000010111100011000101111", // G
 "10001100011000111111100011000110001", // H
 "11111001000010000100001000010011111", // I
 "00111000100001000010100101001001100", // J
 "10001100101010011000101001001010001", // K
 "10000100001000010000100001000011111", // L
 "10001110111010110101100011000110001", // M
 "10001110011010110011100011000110001", // N
 "01110100011000110001100011000101110", // O
 "11110100011000111110100001000010000", // P
 "01110100011000110001101011001001101", // Q
 "11110100011000111110101001001010001", // R
 "01111100001000001110000010000111110", // S
 "11111001000010000100001000010000100", // T
 "10001100011000110001100011000101110", // U
 "10001100011000110001010100101000100", // V
 "10001100011000110101101011101110001", // W
 "10001100010101000100010101000110001", // X
 "10001100010101000100001000010000100", // Y
 "11111000010001000100010001000011111", // Z
 "01110100011001110101110011000101110", // 0
 "00100011000010000100001000010001110", // 1
 "01110100010000100010001000100011111", // 2
 "11110000010000101110000010000111110", // 3
 "00010001100101010010111110001000010", // 4
 "11111100001000011110000010000111110", // 5
 "01110100001000011110100011000101110", // 6
 "11111000010001000100010000100001000", // 7
 "01110100011000101110100011000101110", // 8
 "01110100011000101111000010000101110"}; // 9
 if(c>='A' && c<='Z')return rows[c-'A'];if(c>='0' && c<='9')return rows[26+c-'0'];
 switch(c) {case '-':return "00000000000000011111000000000000000";case '+':return "00000001000010011111001000010000000";case '/':return "00001000010001000100010001000010000";case '.':return "00000000000000000000000000011000110";case '*':return "00000101010111011111011101010100000";
 case ',':return "00000000000000000000001100010001000";
 case ';':return "00000001000000000000001000010001000";
 case '\'':return "00100001000100000000000000000000000";
 case '=':return "00000000001111100000111110000000000";
 case '[':return "01110010000100001000010000100001110";
 case ']':return "01110000100001000010000100001001110";
 case '\\':return "10000100000100000100000100000100001";
 case '`':return "01000001000000000000000000000000000";
 default:return "11111100010001000100001000000000100";}
}
using Palette=std::array<std::array<uint8_t,4>,256>;
inline Palette palette(const uint8_t *raw) {
 Palette p{};for(int i=0;i<256;++i){int j=(i&~24)|((i&8)<<1)|((i&16)>>1);for(int k=0;k<4;++k)p[i][k]=raw[j*4+k];p[i][3]=uint8_t((std::min)(255,int(p[i][3])*2));}return p;
}
inline uint8_t nearest(const Palette&p,float r,float g,float b,float a) {
 float best=1e30f;int bi=0;for(int i=0;i<256;++i){float dr=r-p[i][0],dg=g-p[i][1],db=b-p[i][2],da=a-p[i][3];float d=(dr*dr+dg*dg+db*db)*(a/255)+3*da*da;if(d<best){best=d;bi=i;}}return uint8_t(bi);
}
inline std::vector<uint8_t> build(const std::vector<uint8_t>&original,const Palette&p,const Input&in) {
 if(in.style==Style::Off)return original;
 auto idx=decode(original.data());const auto source=idx;
 for(const Cell &c:cells) {
 const Binding b=in.binding[c.button];
 if(in.style==Style::PS) { // Copy the user's original cell for the physical PS control, including remaps.
 int pb=psSource(b.pad);if(pb==c.button)continue;
 const Cell *from=nullptr;for(const auto &v:cells)if(v.button==pb)from=&v;
 if(from){for(int y=c.y0;y<c.y1;++y)for(int x=c.x0;x<c.x1;++x)idx[y*256+x]=source[(from->y0+(y-c.y0)*(from->y1-from->y0)/(c.y1-c.y0))*256+from->x0+(x-c.x0)*(from->x1-from->x0)/(c.x1-c.x0)];continue;}
 }
 const std::string text=label(in.style,b);int w=c.x1-c.x0,h=c.y1-c.y0;
 bool circle=in.style!=Style::Keyboard && (b.pad==5 || b.pad==6 || b.pad==7 || b.pad==8 || b.pad==16 || b.pad==17);
 for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
 float rr=0,gg=0,bb=0,aa=0;
 for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx) {
 float px=x+(sx+.5f)/4,py=y+(sy+.5f)/4;bool edge,outer,inner;
 // lane glyphs F9: dark outline, light ring, top-lit blue-grey interior (the original buttons' look).
 if(circle){float dx=px-w*.5f,dy=py-h*.5f,d2=dx*dx+dy*dy;float radius=(std::min)(w,h)*.5f-1;edge=d2<=(radius+.9f)*(radius+.9f);outer=d2<=radius*radius;inner=d2<=(radius-1.6f)*(radius-1.6f);}
 else {edge=px>=0 && px<w && py>=0 && py<h;outer=px>=1 && px<w-1 && py>=1 && py<h-1;inner=px>=2.6f && px<w-2.6f && py>=2.6f && py<h-2.6f;}
 const float t=py/h;
 float a=edge?255.f:0.f,r=20.f,g=20.f,bl=20.f;
 if(inner){r=100.f-40.f*t;g=106.f-38.f*t;bl=116.f-38.f*t;} else if(outer){r=228.f;g=230.f;bl=236.f;}
 // lane glyphs F7: advance <= 0.95 x height, text centred (trailing letter gap not counted).
 float th=h*.48f,gwidth=(std::min)(w*(circle?.60f:.78f)/text.size(),th*.95f);float tw=gwidth*text.size()-gwidth/6;
 float tx=px-(w-tw)*.5f,ty=py-(h-th)*.5f;
 if(tx>=0 && tx<tw && ty>=0 && ty<th){int n=int(tx/gwidth);float u=(tx/gwidth-n)*6,v=ty/th*7;int cx=int(u),cy=int(v);
 if(n<int(text.size()) && cx<5 && cy<7){const char *gl=grid(text[n]);
   auto lit=[&](int gx,int gy){return gx>=0 && gx<5 && gy>=0 && gy<7 && gl[gy*5+gx]=='1';};
   const float fx=u-cx,fy=v-cy;bool on=lit(cx,cy);
   // lane glyphs F8: fill the corner triangle between two diagonal neighbours.
   if(!on)on=(lit(cx-1,cy) && lit(cx,cy-1) && fx+fy<.5f) || (lit(cx+1,cy) && lit(cx,cy-1) && (1-fx)+fy<.5f) ||
     (lit(cx-1,cy) && lit(cx,cy+1) && fx+(1-fy)<.5f) || (lit(cx+1,cy) && lit(cx,cy+1) && (1-fx)+(1-fy)<.5f);
   if(on){r=g=bl=255;a=255;}}}
 rr+=r*a/255;gg+=g*a/255;bb+=bl*a/255;aa+=a;
 }
 float a=aa/16;idx[(c.y0+y)*256+c.x0+x]=nearest(p,a?rr*255/aa:0,a?gg*255/aa:0,a?bb*255/aa:0,a);
 }
 }
 return encode(idx);
}
} // namespace ps2_glyphs
