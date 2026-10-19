#pragma once
#include "ps2_menustick.h"
// TEMP(transformers): read-only common screen-manager tracking, EE thread only.
namespace ps2_menustick {
static uint32_t manager=0;
static bool installed=false;

static bool span(uint32_t a,uint32_t n){return a>=0x100000 && n<=0x2000000 && a<=0x2000000-n;}
static uint32_t rd(const uint8_t*r,uint32_t a){uint32_t v=0;if(span(a,4))std::memcpy(&v,r+a,4);return v;}
static const char* screenName(uint32_t vt){switch(vt){
case 0x6b6da0:return "taScreenInGConfirm";
case 0x6b6e60:return "taScreenInGDataconPickupMenu";
case 0x6b6ee0:return "taScreenInGMiniconPickupMenu";
case 0x6b6f60:return "taScreenInGMiniconExchangeMenu";
case 0x6b6fe0:return "taScreenInGPause";
case 0x6b7060:return "taScreenInGMenu";
case 0x6b75a0:return "taScreen";
case 0x6b7610:return "taScreenComCardSpaceCheck";
case 0x6b77c0:return "taScreenPrGTitle";
case 0x6b88b0:return "taScreenInGEpisodeClear";
case 0x6b8950:return "taScreenInGEpisodeLost";
case 0x6b9720:return "taScreenPrGManagement";
case 0x6b9e00:return "taScreenExit";
case 0x6b9ed0:return "taScreenControlConfigOptions";
case 0x6b9f50:return "taScreenControlOptions";
case 0x6b9fd0:return "taScreenAudioOptions";
case 0x6ba050:return "taScreenOptions";
case 0x6bd760:return "taScreenPrGHQCommPC";
case 0x6bd810:return "taScreenPrGHQComm";
case 0x6bd880:return "taScreenPrGRingCharacterSelect";
case 0x6bd8f0:return "taScreenPrGRing";
case 0x6c2d30:return "taScreenMenu";
case 0x6c2dc0:return "uiScreenMenuBG";
case 0x6c2e40:return "taScreenFrequencyTest";
case 0x6c2ee0:return "taScreenFrequencySelect";
case 0x6c3e40:return "taScreenPrGRingMiniconSelect";
case 0x6c3ed0:return "taScreenPrGRingMiniconButtonSelect";
case 0x6c4360:return "taScreenPrGWarpGate";
case 0x6c4410:return "taScreenPrGRingLevelSelect";
case 0x6c7240:return "taScreenStartLoadGame";
case 0x6c7340:return "taScreenStartInitCardCheck";
case 0x6c73c0:return "taScreenStartSaveOverwrite";
case 0x6c7440:return "taScreenStartNewGameSelect";
case 0x6c74c0:return "taScreenStartDifficulty";
case 0x6c7540:return "taScreenStartVideoOptions";
case 0x6c75c0:return "taScreenStartOptionsMenu";
case 0x6c7640:return "taScreenStartMenu";
case 0x6c9920:return "taScreenExtrasPlayAllMovies";
case 0x6c9a20:return "taScreenExtrasBonusMovie";
case 0x6c9a90:return "taScreenExtrasAudioPlayer";
case 0x6c9b20:return "taScreenExtrasEpisodes";
case 0x6c9bb0:return "taScreenExtrasArtViewer";
case 0x6c9c20:return "taScreenExtrasMenuListArt";
case 0x6c9cb0:return "taScreenExtrasMenuListMovies";
case 0x6c9d40:return "taScreenExtrasMenuList";
case 0x6c9dd0:return "taScreenExtrasSubMenu";
case 0x6c9e60:return "taScreenExtrasMenu";
case 0x6ca390:return "taScreenAutoSaveError";
case 0x6ca430:return "taScreenAutoSave";
case 0x6cac10:return "taScreenOptionSaveError";
case 0x6cacb0:return "taScreenOptionSave";
case 0x6caeb0:return "taScreenPrGLegal";
case 0x6caf30:return "taScreenPrGControllerCheck";
default:return nullptr;}}
static void enable(){installed=true;}

}
void ps2MenustickSample(const uint8_t*r){
 using namespace ps2_menustick;
 if(!installed)return;
 // taUISystem is installed at gp-0x3b38 by 0x316bf0; the secondary/background system is separate.
 auto owner=rd(r,0x6d30f0-0x3b38);
 manager=span(owner,8)&&rd(r,owner)==0x6b7160?rd(r,owner+4):0;
 auto active=span(manager,0x44)?rd(r,manager+0x3c):0;
 auto vt=span(active,0x58)?rd(r,active):0;
 const char*name=screenName(vt);
 // Screen+8 back-links to its manager (krScreen dispatcher 0x1a09f8).
 auto identity=name&&rd(r,active+8)==manager&&rd(r,active+0x54)==2?active:0;
 context(identity);
 static const bool log=[](){const char*p=std::getenv("PS2X_PAD_LOG");return p&&*p=='1';}();
 static uint32_t previous=UINT32_MAX,previousVT=UINT32_MAX;
 if(log&&!ps2MenustickScriptedInput()&&(identity!=previous||vt!=previousVT))std::fprintf(stderr,"[menustick] manager=%08x screen=%08x vt=%08x class=%s active=%d\n",manager,active,vt,name?name:"none",identity!=0);
 previous=identity;previousVT=vt;
}
