#pragma once
#include "ps2_carstick_state.h"
// TEMP(transformers): carstick2 read-only player/axis tracking. Included only by ps2_runtime.cpp after runtime types.
namespace ps2_carstick {
static uint32_t controller=0,axisX=0,axisY=0;
static uint64_t controllerSeenVs=UINT64_MAX;
static PS2Runtime::RecompiledFunction originalController=nullptr,originalAxis=nullptr;
static bool span(uint32_t a,uint32_t n){return a>=0x100000 && n<=0x2000000 && a<=0x2000000-n;}
static uint32_t rd(const uint8_t*r,uint32_t a){uint32_t v=0;if(span(a,4))std::memcpy(&v,r+a,4);return v;}
static float rf(const uint8_t*r,uint32_t a){uint32_t v=rd(r,a);float f;std::memcpy(&f,&v,4);return f;}
static uint32_t playerActor(const uint8_t*r,uint32_t ctl){
 if(!span(ctl,12)||rd(r,ctl)!=0x6b8000)return 0;
 auto creature=rd(r,ctl+8);if(!span(creature,0x58))return 0;
 auto binding=rd(r,creature+0x38);if(!span(binding,4)||rd(r,binding)!=0)return 0;
 auto actor=rd(r,creature+0x54);if(!span(actor,0xfc))return 0;
 auto context=rd(r,actor+0x7c);if(!span(context,0xd8))return 0;
 return rd(r,context)==actor && rd(r,context+4)==creature && rd(r,context+0xd4)==ctl ? actor : 0;
}
static void controllerGetter(uint8_t*r,R5900Context*c,PS2Runtime*runtime){
 if(c->pc==0x34b040 && playerActor(r,getRegU32(c,4))){controller=getRegU32(c,4);controllerSeenVs=runtime->eeScheduler().currentVSyncTick();}
 originalController(r,c,runtime);
}
static void axisGetter(uint8_t*r,R5900Context*c,PS2Runtime*runtime){
 if(c->pc==0x197dc0){auto a=getRegU32(c,4);if(span(a,0x38)&&rd(r,a+0xc)==0x6b14a0){auto mask=rd(r,a+0x10);if(mask==0x20000)axisX=a;else if(mask==0x40000)axisY=a;}}
 originalAxis(r,c,runtime);
}
static Mode classify(uint32_t vt){switch(vt){case 0x6bfa20:return Mode::Vehicle;case 0x6bfe80:return Mode::ToVehicle;case 0x6bfed0:return Mode::ToRobot;case 0x6c4850:case 0x6c48a0:case 0x6c48f0:case 0x6bfd40:case 0x6bfd90:return Mode::Unavailable;case 0x6bf9d0:case 0x6bfa70:case 0x6bfac0:case 0x6bfb10:case 0x6bfb60:case 0x6bfbb0:case 0x6bfc00:case 0x6bfc50:case 0x6bfca0:case 0x6bfcf0:case 0x6bfde0:case 0x6bfe30:case 0x6bff20:case 0x6bff70:case 0x6bffe0:case 0x6c0030:case 0x6c0080:case 0x6c00d0:case 0x6c0120:case 0x6c4940:return Mode::Robot;default:return Mode::Unknown;}}
static bool liveAxis(const uint8_t*r,uint32_t a,uint32_t mask,float&dz){if(!span(a,0x38)||rd(r,a+0xc)!=0x6b14a0||rd(r,a+0x10)!=mask)return false;dz=rf(r,a+0x20);return std::isfinite(dz)&&dz>=0&&dz<0.9f;}
static void install(PS2Runtime&runtime){
 originalController=runtime.lookupFunction(0x34b040);originalAxis=runtime.lookupFunction(0x197dc0);
 if(originalController&&originalAxis){runtime.replaceFunction(0x34b040,controllerGetter);runtime.replaceFunction(0x197dc0,axisGetter);}
}
}
void ps2CarstickSample(const uint8_t*r,uint64_t vs){
 using namespace ps2_carstick;static uint64_t lastVs=UINT64_MAX;if(vs==lastVs)return;lastVs=vs;
 Snapshot s;s.vs=vs;s.sampled=std::chrono::steady_clock::now();s.actor=playerActor(r,controller);s.controller=s.actor?controller:0;
 float dy=0;bool axes=liveAxis(r,axisX,0x20000,s.deadzone)&&liveAxis(r,axisY,0x40000,dy)&&dy==s.deadzone;
 if(s.actor){auto ix=rd(r,s.actor+0xf8);auto state=ix<25?rd(r,s.actor+0x80+4*ix):0;if(span(state,0x1c)){s.vtable=rd(r,state+0x18);s.mode=classify(s.vtable);}if(r[controller+4] || !axes || controllerSeenVs==UINT64_MAX || vs<controllerSeenVs || vs-controllerSeenVs>4)s.mode=Mode::Unavailable;}
 else controller=0;
 Snapshot prior;{std::lock_guard<std::mutex> l(snapshotMutex);prior=latest;}
 float previous=(s.actor==prior.actor && s.controller==prior.controller)?prior.weight:(s.mode==Mode::ToRobot?1.0f:0.0f);
 s.weight=blend(s.mode,previous);
 {std::lock_guard<std::mutex> l(snapshotMutex);latest=s;}
 static const bool log=[](){const char*p=std::getenv("PS2X_PAD_LOG");return p&&*p=='1';}();static uint32_t priorActor=0,priorVT=0;static Mode priorMode=Mode::Unknown;static float priorWeight=0;
 if(log&&(s.actor!=priorActor||s.vtable!=priorVT||s.mode!=priorMode||s.weight!=priorWeight))std::fprintf(stderr,"[carstick] vs=%llu actor=%08x controller=%08x mode=%s vt=%08x weight=%.6f dz=%.9f axisX=%08x axisY=%08x\n",(unsigned long long)vs,s.actor,controller,name(s.mode),s.vtable,s.weight,s.deadzone,axisX,axisY);
 priorActor=s.actor;priorVT=s.vtable;priorMode=s.mode;priorWeight=s.weight;
}
