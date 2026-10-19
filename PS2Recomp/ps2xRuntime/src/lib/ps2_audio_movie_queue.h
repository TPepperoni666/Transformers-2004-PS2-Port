#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
// TEMP(transformers): shared host presentation policy, tested without a GPU or game.
namespace ps2AudioMovieQueue {
inline int64_t dueUs(int64_t now, int64_t last, int64_t anchor, uint32_t delay, bool movie, bool lastDelayed=false) {
    // TEMP(transformers): aud3e - keep RC4's 1 ms ordering clamp unless the previous due carried movie AV delay.
    if (!movie || anchor<=0) return (last>0 && !lastDelayed) ? std::max(now,last+1000) : now;
    const int64_t limit=now+500000;
    int64_t due=std::max(now,std::min(limit,anchor+int64_t(delay)));
    if(last>0) due=std::max(due,std::min(limit,last+1000));
    return due;
}
template<class Queue, class Recycle> size_t trim(Queue& q, bool noDrop, Recycle recycle) {
    const bool pending=std::any_of(q.begin(),q.end(),[](const auto& e){return e.audioMovie;});
    const size_t cap=noDrop?128u:pending?64u:8u;
    size_t dropped=0;
    while(q.size()>cap) {
        auto it=std::find_if(q.begin(),q.end(),[](const auto& e){return !e.audioMovie;});
        if(it==q.end()) break;
        recycle(*it); // TEMP(transformers): presentation-diet buffer recycling.
        q.erase(it);++dropped;
    }
    return dropped;
}
template<class Queue> size_t trim(Queue& q, bool noDrop) { return trim(q,noDrop,[](auto&){}); } // TEMP(transformers)

template<class Queue,class Time,class Recycle> size_t discardLate(Queue& q, Time now, bool noDrop, Recycle recycle) {
    size_t dropped=0;
    while(!noDrop && q.size()>=3 && !q.front().audioMovie && q[2].due<=now) {recycle(q.front());q.pop_front();++dropped;} // TEMP(transformers): same victim/order, recycle first.
    return dropped;
}
template<class Queue,class Time> size_t discardLate(Queue& q, Time now, bool noDrop) { return discardLate(q,now,noDrop,[](auto&){}); } // TEMP(transformers)
}
