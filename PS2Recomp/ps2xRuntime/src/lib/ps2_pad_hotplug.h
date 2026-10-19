#pragma once
// TEMP(transformers): padhot1 pure host selector; no GLFW or guest state.
#include <array>
#include <cstdint>
#include <string>
#include <algorithm>
namespace ps2_padhot {
// TEMP(transformers): no enumeration while any mapped pad exists; restart at 3 s after loss.
struct Backoff {
    double next=3, interval=3; bool mapped=false;
    bool due(double now,bool anyMapped) {
        if(anyMapped) { mapped=true; interval=3; next=now+3; return false; }
        if(mapped) { mapped=false; interval=3; next=now+3; }
        return now>=next;
    }
    void completed(double now) { interval=(std::min)(60.0,interval*2); next=now+interval; }
};
struct Slot { bool mapped=false; uint32_t buttons=0; float left=0, right=0; std::string identity; };
struct State {
    int active=-1; bool started=false, pinnedMissing=false;
    uint64_t tick=0;
    std::array<uint64_t,4> used{};
    std::array<uint32_t,4> buttons{};
    std::array<bool,4> leftArmed{{true,true,true,true}}, rightArmed{{true,true,true,true}};
};
struct Choice { State state; const char* reason; };
inline Choice select(const std::array<Slot,4>& slots, int pinned, State s) {
    const int old=s.active; const bool startup=!s.started;
    const bool missing=pinned>=0 && (pinned>=4 || !slots[pinned].mapped);
    const bool missingEvent=missing && !s.pinnedMissing;
    const bool back=pinned>=0 && !missing && s.pinnedMissing;
    ++s.tick;
    int fresh=-1, freshCount=0;
    std::array<bool,4> activity{};
    for(int i=0;i<4;++i) {
        const auto& p=slots[i];
        if(!p.mapped) { s.buttons[i]=0; s.leftArmed[i]=s.rightArmed[i]=true; continue; }
        const bool edge=(p.buttons & ~s.buttons[i])!=0;
        const bool move=(s.leftArmed[i] && p.left>0.5f) || (s.rightArmed[i] && p.right>0.5f);
        if(edge || move) { s.used[i]=s.tick; activity[i]=true; ++freshCount; if(fresh<0) fresh=i; }
        s.buttons[i]=p.buttons;
        if(p.left>0.5f) s.leftArmed[i]=false; else if(p.left<=0.25f) s.leftArmed[i]=true;
        if(p.right>0.5f) s.rightArmed[i]=false; else if(p.right<=0.25f) s.rightArmed[i]=true;
    }
    const bool lost=old>=0 && !slots[old].mapped;
    int pick=old;
    if(pinned>=0 && !missing) pick=pinned;
    else {
        if(pick<0 || !slots[pick].mapped) {
            pick=-1;
            for(int i=0;i<4;++i) if(slots[i].mapped && (pick<0 || s.used[i]>s.used[pick])) pick=i;
        }
        // Missing pinned slot temporarily behaves like auto, including activity handover.
        if(freshCount==1 && fresh>=0 && fresh!=pick && (pick<0 || !activity[pick])) {
            const bool duplicate=pick>=0 && !slots[pick].identity.empty() &&
                slots[pick].identity==slots[fresh].identity &&
                slots[pick].buttons==slots[fresh].buttons &&
                slots[pick].left==slots[fresh].left && slots[pick].right==slots[fresh].right;
            if(!duplicate) pick=fresh;
        }
    }
    const char* why=missingEvent?"pinned-missing":startup?"startup":back?"pinned-back":
        lost?"disconnect":pick!=old?(old<0?"reconnect":"input-switch"):"reconnect";
    s.active=pick; s.started=true; s.pinnedMissing=missing;
    return {s,why};
}
}

