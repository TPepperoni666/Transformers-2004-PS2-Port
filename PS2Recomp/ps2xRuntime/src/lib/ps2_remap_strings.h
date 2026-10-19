#pragma once
// GAME(transformers): rewrite only the game's returned English copy; no localised text assets.
#include "ps2_glyphs.h"
#include "ps2_remap_ids.h"
#include <string_view>
namespace ps2_remap {
inline bool affected(std::string_view key) {
    for(const char* k:affectedKeys)if(key==k)return true;
    return key=="HQ_BRIEF_EP01_RobotTraining_Line01" || key=="HQ_BRIEF_EP01_RobotTraining_Line02" ||
           key=="HQ_BRIEF_EP01_ToWaterfall_Line02";
}
struct WordRule { const char* word; int button; };
// Longest spellings first. Match a complete token, never narrative words such as Restart.
inline constexpr WordRule wordRules[]={{"START",3},{"SELECT",0},{"L1",10},{"L2",8},
    {"R1",11},{"R2",9},{"L3",1},{"R3",2},{"X",14}};
inline bool asciiWord(unsigned char c){return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_';}
inline void replaceAll(std::string &s,std::string_view from,std::string_view to) {
    size_t p=0;while((p=s.find(from,p))!=s.npos){s.replace(p,from.size(),to);p+=to.size();}
}
// qualification F18: prose key names for worded strings (the font cells keep the 3-letter caps).
inline std::string keyWord(int k) {
    switch(k) {case 32:return "Space";case 257:return "Enter";case 258:return "Tab";case 259:return "Backspace";
    case 340:return "Left Shift";case 344:return "Right Shift";case 341:return "Left Ctrl";case 345:return "Right Ctrl";
    case 342:return "Left Alt";case 346:return "Right Alt";case 265:return "Up";case 264:return "Down";case 263:return "Left";
    case 262:return "Right";case 260:return "Insert";case 261:return "Delete";case 268:return "Home";case 269:return "End";
    case 266:return "Page Up";case 267:return "Page Down";case 335:return "Keypad Enter";default:break;}
    if(k>=320 && k<=329)return "Keypad "+std::to_string(k-320);
    return ps2_glyphs::keyLabel(k);
}
inline std::string rewriteMode(std::string_view key,const std::string &original,const ps2_glyphs::Input &in,int layout,bool proseKeys);
inline std::string rewrite(std::string_view key,const std::string &original,const ps2_glyphs::Input &in,int layout=0) {
    if(in.style==ps2_glyphs::Style::Keyboard) {
        auto s=rewriteMode(key,original,in,layout,true);
        if(s!=original)return s;
    }
    return rewriteMode(key,original,in,layout,false);
}
inline std::string rewriteMode(std::string_view key,const std::string &original,const ps2_glyphs::Input &in,int layout,bool proseKeys) {
    using namespace ps2_glyphs;
    if(!affected(key) || in.style==Style::Off)return original;
    std::string s=original;
    // The three layout labels use guest symbols; the font hook performs host remapping.
    // Layout changes are read from the active manager; this code never selects a layout.
    constexpr char layoutGlyphs[4][3]={{'|','{','~'},{'|','~','{'},{'~','|','{'},{'{','~','|'}};
    if(layout>=0 && layout<4 && !s.empty()) {
        int n=key=="KEY_CTL_CHANGE"?0:key=="KEY_CTL_POWERLINK"?1:key=="KEY_CTL_LOOK"?2:-1;
        if(n>=0)s[0]=layoutGlyphs[layout][n];
    }
    // qualification F19/F20: PS words follow bindings; scan input once so a replacement
    // like L1 -> R1 or a keyboard X is never processed again as another guest token.
    std::string words;
    for(size_t p=0;p<s.size();) {
        const WordRule* rule=nullptr;size_t n=0;
        for(const auto&r:wordRules) {
            const size_t len=std::strlen(r.word);
            if(s.compare(p,len,r.word)==0 && (!p || !asciiWord(static_cast<unsigned char>(s[p-1]))) &&
               (p+len==s.size() || !asciiWord(static_cast<unsigned char>(s[p+len])))){rule=&r;n=len;break;}
        }
        if(!rule){words+=s[p++];continue;}
        size_t count=n;std::string suffix;
        if(s.compare(p+n,7," button")==0 || s.compare(p+n,7," Button")==0){suffix=s.substr(p+n,7);count+=7;}
        std::string to=proseKeys && in.style==Style::Keyboard?keyWord(in.binding[rule->button].key):label(in.style,in.binding[rule->button]);
        if(to.find('%')!=to.npos)to="KEY";
        if(!suffix.empty()) {
            if(in.style==Style::Keyboard)suffix=suffix[1]=='B'?" Key":" key";
            if(to.size()+suffix.size()<=count)to+=suffix;
        }
        words+=to;p+=count;
    }
    s=std::move(words);
    if(in.style!=Style::PS) {
        replaceAll(s,"DUALSHOCK(R)2 analog controller","controller");
        replaceAll(s,"DUALSHOCK\xae" "2 analog controller","controller");
        replaceAll(s,"Left analog stick","Left stick");replaceAll(s,"Right analog stick","Right stick");
        replaceAll(s,"left analog stick","left stick");replaceAll(s,"right analog stick","right stick");
    }
    // Never truncate. Keep the original allocation, length, terminator and downstream byte-loop counts.
    // All supported labels fit; an unusually long future key name leaves the full original intact.
    if(s.size()>original.size())return original;
    // qualification F15: pad where the screen aligns the string (centred title prompts, right-aligned option rows).
    const size_t pad=original.size()-s.size();
    if(key=="KEY_PRESSSTART")return std::string(pad/2,' ')+s+std::string(pad-pad/2,' ');
    if(key=="KEY_R3RECON")return std::string(pad,' ')+s;
    s.append(pad,' ');return s;
}
// Game string ABI: short = first byte length*2, chars at +1 (capacity 10);
// long = capacity|1 at +0, length +4, owned pointer +8 (00102990/00102c40).
struct GuestString { uint32_t chars=0,length=0; bool valid=false; };
inline uint32_t read32(const uint8_t* ram,uint32_t p){uint32_t v;std::memcpy(&v,ram+p,4);return v;}
inline bool ramRange(uint32_t p,uint32_t n){return p<0x2000000u && n<=0x2000000u-p;}
inline GuestString view(const uint8_t* ram,uint32_t obj) {
    if(!ramRange(obj,12))return {};
    uint32_t p=obj+1,n=ram[obj]>>1;
    if(ram[obj]&1){n=read32(ram,obj+4);p=read32(ram,obj+8);uint32_t cap=read32(ram,obj)&~1u;if(!cap || n>=cap)return {};}
    else if(n>10)return {};
    if(n>16384 || !ramRange(p,n+1) || ram[p+n]!=0)return {};
    return {p,n,true};
}
inline int activeLayout(const uint8_t*ram,uint32_t gp) {
    // TEMP(transformers) qualification F14: test-only display override (A..D = 0..3); never writes guest memory.
    static const int forced=[](){const char*p=std::getenv("PS2X_GLYPHS_TEST_LAYOUT");return p && p[0]>='0' && p[0]<='3' && !p[1]?p[0]-'0':-1;}();
    if(forced>=0)return forced;
    if(gp<0x3e9c || !ramRange(gp-0x3e9c,4))return 0;
    uint32_t mgr=read32(ram,gp-0x3e9c);if(!mgr || !ramRange(mgr,0x534))return 0;
    int32_t port=int32_t(read32(ram,mgr+0x530));if(port<0 || port>1)return 0;
    uint32_t row=mgr+uint32_t(port)*0x524+0xc;
    if(!ramRange(row,4))return 0;uint32_t layout=read32(ram,row);return layout<4?int(layout):0;
}
inline bool rewriteGuest(uint8_t*ram,uint32_t dst,const std::string&key,const ps2_glyphs::Input&in,int layout) {
    auto v=view(ram,dst);if(!v.valid)return false;
    std::string original(reinterpret_cast<char*>(ram+v.chars),v.length);
    auto s=rewrite(key,original,in,layout);if(s==original)return false;
    std::memcpy(ram+v.chars,s.data(),v.length);return true;
}
} // namespace ps2_remap
