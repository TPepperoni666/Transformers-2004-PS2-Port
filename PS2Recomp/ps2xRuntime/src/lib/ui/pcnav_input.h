#pragma once
#include <cmath>
#include "../ps2_menustick.h"
// GAME(transformers): menustick3 direction latch, with host-owned repeat timing.
namespace pcnav {
enum Direction { None, Up, Down, Left, Right };
struct Input {
    Direction stick=None, repeating=None;
    bool waitNeutral=true;
    double next=0;
    void reset() { *this=Input{}; }
    Direction step(float x,float y,unsigned dpad,double now) {
        const float r=std::hypot(x,y);
        constexpr float release=0.35f;
        if(!std::isfinite(r)||r<release) { stick=None; waitNeutral=false; }
        else if(!waitNeutral) {
            bool engage=stick==None;
            if(stick!=None) {
                const bool horizontal=stick==Left||stick==Right;
                const float along=(horizontal?x:y)*((stick==Left||stick==Up)?-1:1);
                const float across=std::fabs(horizontal?y:x);
                // Reuse menustick3: no subthreshold opposite spring-back or diagonal chatter.
                engage=along<=-0.55f || (across>=0.55f && across>=std::fabs(along)+0.35f);
            }
            if(engage && r>=0.55f)
                stick=std::fabs(x)>std::fabs(y)?(x<0?Left:Right):(y<0?Up:Down);
        }
        Direction held=stick;
        // D-pad wins arbitration; opposing buttons cancel, diagonals choose one axis.
        if(dpad) {
            held=None;
            if(bool(dpad&1)!=bool(dpad&2)) held=dpad&1?Up:Down;
            else if(bool(dpad&4)!=bool(dpad&8)) held=dpad&4?Left:Right;
        }
        if(held!=repeating) { repeating=held; next=now+ps2_menustick::repeatDelay; return held; }
        if(held!=None && now+1e-9>=next) { next=now+0.175; return held; }
        return None;
    }
};
}
