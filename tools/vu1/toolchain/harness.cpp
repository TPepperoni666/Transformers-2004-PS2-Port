#include <windows.h>
#include <stdlib.h>
// Transformers port: standalone observable oracle and alternating thread-cycle bench.
#include "api.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <set>
#include <thread>
#include <atomic>
#include <map>
// Transformers port: sample only the standalone main thread we started.
static std::atomic<bool> profileStop{false};
static std::map<uint64_t,unsigned> profileSamples;
static std::thread profileThread;
static HANDLE profileMain=nullptr;
// Transformers port lane aot5: 1 ms timer resolution for the RIP sampler (default 15.6 ms gave ~1 sample per 60 ms).
#include <timeapi.h>
#pragma comment(lib,"winmm.lib")
#if defined(AOT5_RSTATS)
void aot4DumpRegionStats(const char*);void aot5DumpR3Stats(const char*);void aot5DumpProfile(const char*);
#endif
static void profileStart(){timeBeginPeriod(1);
 DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&profileMain,0,FALSE,DUPLICATE_SAME_ACCESS);
 profileThread=std::thread([]{while(!profileStop){if(SuspendThread(profileMain)!=DWORD(-1)){CONTEXT c{};c.ContextFlags=CONTEXT_CONTROL;if(GetThreadContext(profileMain,&c))++profileSamples[c.Rip-(uint64_t)GetModuleHandleA(nullptr)];ResumeThread(profileMain);}Sleep(1);}});
}
static void profileEnd(){profileStop=true;profileThread.join();CloseHandle(profileMain);for(auto [r,n]:profileSamples)printf("RIP %llx %u\n",r,n);}
static std::vector<uint8_t> packets;
static bool capture=true, tracing=false;
static std::set<uint32_t> pcs;
void tracePc(uint32_t pc){if(tracing)pcs.insert(pc);}
void GS::processGIFPacket(const uint8_t*p,uint32_t n){if(capture)packets.insert(packets.end(),p,p+n);}
void PS2Memory::submitGifPacket(GifPathId path,const uint8_t*p,uint32_t n,bool a,bool b){if(!capture)return;uint32_t h[]={uint32_t(path),n,uint32_t(a),uint32_t(b)};auto q=(uint8_t*)h;packets.insert(packets.end(),q,q+16);packets.insert(packets.end(),p,p+n);}
struct Dump {std::string set,path;int image;uint32_t h[4];std::vector<uint8_t>code,data;std::array<uint8_t,664>state;
bool load(){std::ifstream f(path,std::ios::binary);code.resize(16384);data.resize(16384);f.read((char*)h,16);f.read((char*)code.data(),16384);f.read((char*)data.data(),16384);f.read((char*)state.data(),664);return bool(f)&&h[3]==664;}};
static uint64_t ticks(){ULONG64 c=0;QueryThreadCycleTime(GetCurrentThread(),&c);return c;}
static bool regs(const Output&a,const Output&b){
 const auto* x=(const VU1State*)a.state.data();const auto*y=(const VU1State*)b.state.data();
 return !memcmp(x->vf,y->vf,sizeof(x->vf))&&!memcmp(x->vi,y->vi,sizeof(x->vi))&&!memcmp(x->acc,y->acc,16)&&!memcmp(&x->q,&y->q,12)&&x->r==y->r&&x->mac==y->mac&&x->status==y->status&&x->clip==y->clip&&x->stoppedByD==y->stoppedByD&&x->stoppedByT==y->stoppedByT&&x->pc==y->pc&&x->ebit==y->ebit&&x->haltAfterDelaySlot==y->haltAfterDelaySlot&&x->branchPending==y->branchPending&&x->branchTarget==y->branchTarget&&x->branchDelay==y->branchDelay;
}
static uint64_t cycles(const Output&o){uint64_t c;memcpy(&c,o.state.data()+624,8);return c;}
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
 _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

 SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e)->LONG{fprintf(stderr,"CRASH code=%lx rip=%llx rva=%llx addr=%llx\n",e->ExceptionRecord->ExceptionCode,e->ContextRecord->Rip,e->ContextRecord->Rip-(uint64_t)GetModuleHandleA(nullptr),e->ExceptionRecord->ExceptionInformation[1]);fflush(stderr);return EXCEPTION_EXECUTE_HANDLER;});
 SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
 _putenv_s("PS2X_VU1_FAST","1");_putenv_s("PS2X_VU1_RUN_TO_END","0");
 if(argc<3)return 2;bool trace=std::string(argv[1])=="trace";bool profile=std::string(argv[1])=="profile";if(profile)profileStart();std::ifstream list(argv[2]);std::string line;
 unsigned total=0,fail=0,postCount=0,postFail=0;uint64_t ap=0,fp=0,sp=0;
 while(std::getline(list,line)){
  std::istringstream in(line);Dump d;in>>d.image>>d.set;std::getline(in,d.path);if(d.path.size())d.path.erase(0,1);if(!d.load()){printf("LOAD_FAIL %s\n",d.path.c_str());return 2;}
  uint32_t budget=d.set=="levelcuts"?1048576:40000000; // Transformers port lane aotcov2: was 8000000 - the player-route warp-load images run 23.4M cycles per call
  if(d.set.rfind("budget",0)==0)budget=uint32_t(strtoul(d.set.c_str()+6,nullptr,10));
  if(profile){capture=false;auto data=d.data;for(int n=0;n<2000;++n){data=d.data;native(d.image,d.code.data(),data.data(),d.state.data(),d.h,budget);}++total;continue;}
  packets.clear();pcs.clear();tracing=trace;auto rd=d.data;auto a=reference(d.code.data(),rd.data(),d.state.data(),d.h,budget);tracing=false;
  if(trace){printf("TRACE %d",d.image);for(auto p:pcs)printf(" %u",p);puts("");++total;continue;}
  auto pa=packets;packets.clear();auto nd=d.data;auto b=native(d.image,d.code.data(),nd.data(),d.state.data(),d.h,budget);
  auto postPath=d.path;auto marker=postPath.rfind("_pre.bin");
  if(marker!=std::string::npos){postPath.replace(marker,8,"_post.bin");std::ifstream f(postPath,std::ios::binary);if(f){std::array<uint8_t,16400>skip;std::vector<uint8_t>pd(16384);Output po;f.read((char*)skip.data(),16400);f.read((char*)pd.data(),16384);f.read((char*)po.state.data(),664);uint64_t initialCycle;memcpy(&initialCycle,d.state.data()+624,8);uint64_t delta=cycles(po)-initialCycle;memcpy(po.state.data()+624,&delta,8);++postCount;if(!f||pd!=rd||!regs(a,po)||cycles(a)!=delta){++postFail;printf("POST_FAIL %s\n",d.path.c_str());}}}
  bool dm=rd==nd,rm=regs(a,b),pm=pa==packets,cm=cycles(a)==cycles(b);bool ok=dm&&rm&&pm&&cm;fail+=!ok;total++;ap+=b.pairs;fp+=b.fallback;sp+=b.scheduled;
  if(!ok){auto*x=(VU1State*)a.state.data();auto*y=(VU1State*)b.state.data();{auto*X=(VU1State*)a.state.data();auto*Y=(VU1State*)b.state.data();for(int r=0;r<32;++r)for(int c=0;c<4;++c)if(memcmp(&X->vf[r][c],&Y->vf[r][c],4))printf(" DIFF vf%d.%d %08x/%08x",r,c,*(uint32_t*)&X->vf[r][c],*(uint32_t*)&Y->vf[r][c]);for(int r=0;r<16;++r)if(X->vi[r]!=Y->vi[r])printf(" DIFF vi%d %d/%d",r,X->vi[r],Y->vi[r]);for(int c=0;c<4;++c)if(memcmp(&X->acc[c],&Y->acc[c],4))printf(" DIFF acc%d",c);if(memcmp(&X->q,&Y->q,12))printf(" DIFF qpi");if(X->r!=Y->r)printf(" DIFF r");if(X->branchTarget!=Y->branchTarget)printf(" DIFF bt %u/%u",X->branchTarget,Y->branchTarget);if(X->branchDelay!=Y->branchDelay)printf(" DIFF bd %u/%u",X->branchDelay,Y->branchDelay);if(X->branchPending!=Y->branchPending)printf(" DIFF bp");if(X->ebit!=Y->ebit)printf(" DIFF ebit");if(X->haltAfterDelaySlot!=Y->haltAfterDelaySlot)printf(" DIFF halt");if(X->stoppedByD!=Y->stoppedByD||X->stoppedByT!=Y->stoppedByT)printf(" DIFF stopDT");printf("\n");}printf("FAIL %s data=%d regs=%d packets=%d cycles=%llu/%llu pc=%x/%x mac=%x/%x status=%x/%x clip=%x/%x\n",d.path.c_str(),dm,rm,pm,cycles(a),cycles(b),x->pc,y->pc,x->mac,y->mac,x->status,y->status,x->clip,y->clip);}
  capture=false;std::vector<uint64_t>rt,nt;int rounds=argc>3?atoi(argv[3]):5;
  for(int r=0;r<rounds;++r){for(int j=0;j<2;++j){bool n=(j==(r&1));auto data=d.data;auto o=n?native(d.image,d.code.data(),data.data(),d.state.data(),d.h,budget):reference(d.code.data(),data.data(),d.state.data(),d.h,budget);(n?nt:rt).push_back(o.hostCycles);}}
  capture=true;std::sort(rt.begin(),rt.end());std::sort(nt.begin(),nt.end());
  if(rounds)printf("ROW %d %s %u %llu %llu %llu %llu %llu %llu %llu %d %s\n",d.image,d.set.c_str(),d.h[0]&0x80000000?*(uint32_t*)(d.state.data()+608):d.h[0],cycles(a),rt[rt.size()/2],nt[nt.size()/2],b.pairs,b.scheduled,b.fallback,uint64_t(pa.size()),ok,d.path.c_str());
 }
 #if defined(AOT5_RSTATS)
 aot4DumpRegionStats("rstats_regions.txt");aot5DumpR3Stats("rstats_r3.txt");aot5DumpProfile("rstats_profile.txt");
 #endif
 if(profile)profileEnd();printf("POST dumps=%u fail=%u\n",postCount,postFail);printf("TOTAL dumps=%u fail=%u pairs=%llu static=%llu fallback=%llu\n",total,fail,ap,sp,fp);return fail||postFail?1:0;
}
