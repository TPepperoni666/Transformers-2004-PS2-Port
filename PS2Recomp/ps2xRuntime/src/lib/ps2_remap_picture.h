#pragma once
#include "ps2_glyphs.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmct16.h"
// GAME(transformers): original neutral pad schematic, stroke drawn. No vendor silhouette/art.
namespace ps2_remap {
inline std::vector<uint8_t> decodePicture(const uint8_t*p) {
    std::vector<uint8_t> mem(32768),idx(65536);
    for(int y=0;y<128;++y)for(int x=0;x<128;++x){auto a=GSPSMCT16::addrPSMCT16(0,2,x,y);size_t q=(y*128+x)*2;mem[a]=p[q];mem[a+1]=p[q+1];}
    for(int y=0;y<256;++y)for(int x=0;x<256;++x){auto a=GSPSMT4::addrPSMT4(0,4,x,y);idx[y*256+x]=(mem[a/2]>>((a&1)*4))&15;}
    return idx;
}
inline std::vector<uint8_t> encodePicture(const std::vector<uint8_t>&idx) {
    std::vector<uint8_t> mem(32768),out(32768);
    for(int y=0;y<256;++y)for(int x=0;x<256;++x){auto a=GSPSMT4::addrPSMT4(0,4,x,y);mem[a/2]|=(idx[y*256+x]&15)<<((a&1)*4);}
    for(int y=0;y<128;++y)for(int x=0;x<128;++x){auto a=GSPSMCT16::addrPSMCT16(0,2,x,y);size_t q=(y*128+x)*2;out[q]=mem[a];out[q+1]=mem[a+1];}
    return out;
}
struct PadDrawing {
    std::vector<uint8_t> idx=std::vector<uint8_t>(65536,0);
    void pixel(int x,int y,uint8_t v=15){if(x>=0 && x<256 && y>=0 && y<256)idx[y*256+x]=v;}
    void line(int x0,int y0,int x1,int y1) {
        int dx=std::abs(x1-x0),sx=x0<x1?1:-1,dy=-std::abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy;
        for(;;){pixel(x0,y0);if(x0==x1 && y0==y1)break;int e2=2*e;if(e2>=dy){e+=dy;x0+=sx;}if(e2<=dx){e+=dx;y0+=sy;}}
    }
    void box(int x0,int y0,int x1,int y1){line(x0,y0,x1,y0);line(x1,y0,x1,y1);line(x1,y1,x0,y1);line(x0,y1,x0,y0);}
    void circle(int x,int y,int r) {int px=x+r,py=y;for(int i=1;i<=96;++i){float t=i*6.2831853f/96;int nx=x+int(std::lround(r*std::cos(t))),ny=y+int(std::lround(r*std::sin(t)));line(px,py,nx,ny);px=nx;py=ny;}}
    void text(int cx,int cy,const std::string&s,int scale=1) {
        int width=int(s.size())*6*scale-scale,x0=cx-width/2,y0=cy-7*scale/2;
        for(size_t n=0;n<s.size();++n)for(int y=0;y<7;++y)for(int x=0;x<5;++x)
            if(ps2_glyphs::grid(s[n])[y*5+x]=='1')for(int a=0;a<scale;++a)for(int b=0;b<scale;++b)pixel(x0+int(n)*6*scale+x*scale+a,y0+y*scale+b);
    }
    // lane glyphsc F19: PS face marks are geometric symbols; long centre names use short caps.
    void binding(int cx,int cy,const ps2_glyphs::Input&in,int button) {
        using namespace ps2_glyphs;
        if(in.style==Style::PS) {
            switch(psSource(in.binding[button].pad)) {
            case 12:line(cx,cy-3,cx-4,cy+3);line(cx-4,cy+3,cx+4,cy+3);line(cx+4,cy+3,cx,cy-3);return;
            case 13:circle(cx,cy,4);return;
            case 14:line(cx-3,cy-3,cx+3,cy+3);line(cx-3,cy+3,cx+3,cy-3);return;
            case 15:box(cx-3,cy-3,cx+3,cy+3);return;
            case 0:text(cx,cy,"SEL");return;case 3:text(cx,cy,"STA");return;
            case 5:text(cx,cy,"RT");return;case 6:text(cx,cy,"DN");return;case 7:text(cx,cy,"LT");return;
            default:break;
            }
        }
        text(cx,cy,label(in.style,in.binding[button]));
    }
};
inline std::vector<uint8_t> pictureIndices(const ps2_glyphs::Input&in) {
    using namespace ps2_glyphs;PadDrawing d;
    // Symmetric angular envelope deliberately differs from any vendor's controller outline.
    constexpr int outline[][2]={{48,23},{208,23},{226,46},{239,126},{222,145},{198,135},{176,112},{80,112},{58,135},{34,145},{17,126},{30,46},{48,23}};
    for(size_t i=1;i<sizeof(outline)/sizeof(*outline);++i)d.line(outline[i-1][0],outline[i-1][1],outline[i][0],outline[i][1]);
    d.box(46,8,82,20);d.box(174,8,210,20);
    d.binding(64,14,in,10);d.binding(192,14,in,11);
    // Keep centres of sticks, d-pad, face diamond, centre buttons and detached shoulder boxes.
    d.box(58,37,70,81);d.box(42,53,86,65);
    d.line(64,41,61,46);d.line(64,41,67,46);d.line(64,77,61,72);d.line(64,77,67,72);
    d.circle(96,95,22);d.circle(159,95,22);
    d.binding(96,95,in,1);d.binding(159,95,in,2);
    const int faces[][3]={{12,191,39},{13,210,59},{14,191,78},{15,172,59}};
    for(auto&f:faces){d.circle(f[1],f[2],9);d.binding(f[1],f[2],in,f[0]);}
    d.box(95,55,123,65);d.box(133,55,161,65); // lane glyphs F13: 28 px, fits VIEW/MENU (23 px)
    d.binding(109,60,in,0);d.binding(147,60,in,3);
    // Fixed white callout anchors in the source picture: Pause, Recon zoom, right-stick rear view.
    d.line(146,20,146,54);d.line(210,45,245,45);d.line(159,117,159,132);d.line(96,117,96,132); // lane glyphs F17: Move callout under LS
    const int detached[][5]={{10,14,200,50,217},{11,77,200,113,217},{8,14,227,50,247},{9,77,227,113,247}};
    for(auto&b:detached){d.box(b[1],b[2],b[3],b[4]);d.binding((b[1]+b[3])/2,(b[2]+b[4])/2,in,b[0]);}
    d.circle(159,226,16);d.binding(159,226,in,1);
    d.circle(223,226,16);d.binding(223,226,in,2);
    return d.idx;
}
inline std::vector<uint8_t> buildPicture(const std::vector<uint8_t>&original,const ps2_glyphs::Input&in) {
    if(in.style==ps2_glyphs::Style::Off || (in.style==ps2_glyphs::Style::PS && ps2_glyphs::originalPadBindings(in)))return original;
    return encodePicture(pictureIndices(in));
}
}
