#pragma once
// GAME(transformers): left-stick menu gestures only; guest/D-pad/gameplay semantics stay in the game.
#include <chrono>
#include <mutex>
#include <cmath>
#include <cstdint>
namespace ps2_menustick {
struct Direction { int x=0,y=0; };
inline double repeatDelay=0.500; // maintainer 14 Oct: 400 ms felt slightly quick before the continuous repeat
struct Navigation {
    Direction held{},pending{};
    double started=0;
    bool waitNeutral=true, delivered=false;
    void observe(float x,float y,double now) {
        const float radius=std::hypot(x,y);
        if(!std::isfinite(radius)||radius<=0.30f) {
            held={};waitNeutral=false;delivered=false;return;
        }
        if(waitNeutral)return;
        bool reversed=false;
        if(held.x||held.y) {
            const float along=held.x?x*held.x:y*held.y;
            const float across=held.x?std::fabs(y):std::fabs(x);
            // GAME(transformers): sub-threshold spring-back preserves held and pending.
            // Deliberate reversals engage at 0.55; diagonal noise retains the latch.
            reversed=along <= -0.55f;
            const bool turned=across>=0.55f && across>=std::fabs(along)+0.35f;
            if(!reversed&&!turned)return;
            held={};pending={};delivered=false;
        }
        if(radius<0.55f)return;
        held=std::fabs(x)>std::fabs(y)?Direction{x<0?-1:1,0}:Direction{0,y<0?-1:1};
        pending=held;started=now;delivered=false;
    }
    Direction consume(double now) {
        if(pending.x||pending.y){auto result=pending;pending={};delivered=true;return result;}
        // A neutral packet after the initial step clears the guest's 175 ms repeat latch.
        // After repeatDelay (default 500 ms, controls.ini [sticks] menu_repeat_delay_ms), expose the held direction again
        // and let the original repeat rate run.
        return delivered&&now-started>=repeatDelay?held:Direction{};
    }
};
inline double seconds(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline std::mutex mutex;
inline Navigation navigation;
inline uint32_t screen=0;
inline bool connected=false,keyboardLeft=false,enabled=true;
inline void setEnabled(bool value){std::lock_guard<std::mutex> lock(mutex);enabled=value;navigation={};}
inline void context(uint32_t identity){std::lock_guard<std::mutex> lock(mutex);if(identity!=screen){screen=identity;navigation={};}}
inline void observe(float x,float y,bool present,bool keyboard){
    std::lock_guard<std::mutex> lock(mutex);connected=present;keyboardLeft=keyboard;
    if(!enabled||!present||keyboard){navigation={};return;}
    if(screen)navigation.observe(x,y,seconds());
}
inline void apply(uint8_t&x,uint8_t&y){
    std::lock_guard<std::mutex> lock(mutex);
    if(!enabled||!screen||!connected||keyboardLeft)return;
    auto d=navigation.consume(seconds());x=d.x<0?0:d.x>0?255:128;y=d.y<0?0:d.y>0?255:128;
}
}
bool ps2MenustickScriptedInput();
void ps2MenustickSample(const uint8_t* ram);
