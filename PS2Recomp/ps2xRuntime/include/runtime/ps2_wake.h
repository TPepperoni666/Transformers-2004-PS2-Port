#pragma once
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
// Synchronization policy is immutable for the lifetime of every queue.
namespace ps2Wake {
inline const bool lazy=[] {const char *p=std::getenv("PS2X_WAKE_LAZY");return !p || (p[0]=='1'&&!p[1]);}();
// TEMP(transformers): a separate counter build guarantees zero observer cost
// in shipping code. Define PS2X_WAKE_STATS_BUILD=1 for attribution builds.
#if defined(PS2X_WAKE_STATS_BUILD) && PS2X_WAKE_STATS_BUILD
inline const bool stats=[] {const char *p=std::getenv("PS2X_WAKE_STATS");return !p || (p[0]=='1'&&!p[1]);}();
struct Site {std::atomic<uint64_t> locks{0},notifies{0},waiting{0};};
inline Site sites[2][1024];
inline thread_local unsigned role=1;
inline void bindEE(){role=0;}
class ConditionVariable : public std::condition_variable {
    std::atomic<unsigned> waiters{0};
    struct Waiting {std::atomic<unsigned> &n;Waiting(std::atomic<unsigned> &v):n(v){++n;}~Waiting(){--n;}};
public:
    bool mayWait()const{return waiters.load()!=0;}
    template<class L> void wait(L &l){Waiting w(waiters);std::condition_variable::wait(l);}
    template<class L,class P> void wait(L &l,P p){while(!p())wait(l);}
    template<class L,class... A> auto wait_for(L &l,A&&... a){Waiting w(waiters);return std::condition_variable::wait_for(l,std::forward<A>(a)...);}
    template<class L,class... A> auto wait_until(L &l,A&&... a){Waiting w(waiters);return std::condition_variable::wait_until(l,std::forward<A>(a)...);}
};
inline void lock(unsigned id){if(stats)sites[role][id].locks.fetch_add(1,std::memory_order_relaxed);}
inline void notify(unsigned id,bool waiting){if(stats){sites[role][id].notifies.fetch_add(1,std::memory_order_relaxed);if(waiting)sites[role][id].waiting.fetch_add(1,std::memory_order_relaxed);}}
inline void vsync(uint64_t vs){if(stats)for(unsigned r=0;r<2;++r)for(unsigned i=0;i<1024;++i){auto l=sites[r][i].locks.exchange(0);auto n=sites[r][i].notifies.exchange(0);auto w=sites[r][i].waiting.exchange(0);if(l||n||w)std::fprintf(stderr,"[wake] vs=%llu role=%s site=%u locks=%llu notifies=%llu waiting=%llu\n",(unsigned long long)vs,r?"helper":"EE",i,(unsigned long long)l,(unsigned long long)n,(unsigned long long)w);}}
inline void signal(ConditionVariable &cv,unsigned id,bool all){notify(id,cv.mayWait());if(all)cv.notify_all();else cv.notify_one();}
#else
using ConditionVariable=std::condition_variable;
inline void bindEE(){}
inline void lock(unsigned){}
inline void notify(unsigned,bool){}
inline void vsync(uint64_t){}
inline void signal(ConditionVariable &cv,unsigned,bool all){if(all)cv.notify_all();else cv.notify_one();}
#endif
}
