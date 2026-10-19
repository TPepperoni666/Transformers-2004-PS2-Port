// TEMP(transformers): policy/transport is host-testable without GS, window or guest execution.
#include "runtime/ps2_resolution.h"
#include <algorithm>
#include <cstdio>
#include <tuple>
namespace ps2x::resolution {
std::vector<Mode> normalizeModes(std::vector<Mode> modes) {
 modes.erase(std::remove_if(modes.begin(),modes.end(),[](Mode m){return m.width<320||m.height<240||m.width>7680||m.height>4320||m.hz<24||m.hz>1000;}),modes.end());
 std::sort(modes.begin(),modes.end(),[](Mode a,Mode b){return std::tie(a.width,a.height,a.hz)<std::tie(b.width,b.height,b.hz);});
 modes.erase(std::unique(modes.begin(),modes.end()),modes.end()); return modes;
}
std::string modeText(Mode m) {return std::to_string(m.width)+"x"+std::to_string(m.height)+"@"+std::to_string(m.hz);}
bool parseMode(const std::string& s,Mode& m,bool refresh) {
 Mode parsed;int n=0;int count=refresh ? std::sscanf(s.c_str(),"%dx%d@%d%n",&parsed.width,&parsed.height,&parsed.hz,&n) : std::sscanf(s.c_str(),"%dx%d%n",&parsed.width,&parsed.height,&n);
 if(!refresh) parsed.hz=60;
 if(count!=(refresh?3:2)||n!=int(s.size())||normalizeModes({parsed}).empty())return false;
 m=parsed;return true;
}
void Mailbox::ready(int maxSamples){std::lock_guard<std::mutex> l(mutex_);state_.ready=true;state_.maxSamples=maxSamples;}
uint64_t Mailbox::request(int samples){std::lock_guard<std::mutex> l(mutex_);state_.wanted=samples;state_.error.clear();return ++state_.requested;}
Status Mailbox::status(){std::lock_guard<std::mutex> l(mutex_);return state_;}
bool Mailbox::next(uint64_t& generation,int& samples){std::lock_guard<std::mutex> l(mutex_);if(!state_.ready||state_.requested==state_.acknowledged)return false;generation=state_.requested;samples=state_.wanted;return true;}
bool Mailbox::complete(uint64_t g,int samples,const std::string& error){std::lock_guard<std::mutex> l(mutex_);if(error.empty() && g>state_.applied){state_.samples=samples;state_.applied=g;}if(g!=state_.requested)return false;state_.acknowledged=g;state_.error=error;return true;}
Mailbox& mailbox(){static Mailbox m;return m;}
}
