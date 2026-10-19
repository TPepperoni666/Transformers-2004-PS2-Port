#pragma once
// TEMP(transformers): resopt2 host transport; GS owner alone mutates parallel-gs.
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
namespace ps2x::resolution {
struct Mode { int width=0,height=0,hz=0; bool operator==(const Mode& b) const {return width==b.width&&height==b.height&&hz==b.hz;} };
std::vector<Mode> normalizeModes(std::vector<Mode> modes);
std::string modeText(Mode mode);
bool parseMode(const std::string& text,Mode& mode,bool refresh=true);
struct Status { uint64_t requested=0,acknowledged=0,applied=0; int wanted=1,samples=1,maxSamples=1; bool ready=false; std::string error; };
class Mailbox {
 std::mutex mutex_; Status state_;
public:
 void ready(int maxSamples);
 uint64_t request(int samples);
 Status status();
 bool next(uint64_t& generation,int& samples);
 bool complete(uint64_t generation,int samples,const std::string& error={});
};
Mailbox& mailbox();
}
