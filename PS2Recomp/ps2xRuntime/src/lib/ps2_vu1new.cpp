// TEMP(transformers): log-only ELF microcode coverage detector. PS2X_VU1NEW defaults ON.
#include "ps2_vu1new.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ps2_vu1new {
bool enabled() { static const bool on=[] { const char* e=std::getenv("PS2X_VU1NEW"); return !e || std::strcmp(e,"0")!=0; }(); return on; }
namespace {
std::atomic<uint64_t> guestVs{0};
struct Entry { uint64_t hash; uint32_t offset; uint16_t pairs, program; };
static_assert(sizeof(Entry)==16);
struct File { uint32_t lsn, sectors; std::string path; };
struct Observer {
    std::vector<Entry> entries;
    std::vector<uint8_t> code;
    std::vector<File> files;
    std::mutex mx;
    const char* lastcd="-"; // points into immutable map storage, or lastLsnText
    char lastLsnText[32]{};
    std::unordered_set<uint64_t> unknown, ranges;
    uint64_t uploads=0, stores=0, first=UINT64_MAX, lastSummary=0;
    std::array<uint64_t,6> printed{};
    bool havePrinted=false, ready=false;
    Observer() {
        if(!enabled())return;
        std::filesystem::path path;
        if(const char* e=std::getenv("PS2X_VU1NEW_TABLE"))path=e;
        else {
#ifdef _WIN32
            wchar_t exe[32768]; const DWORD n=GetModuleFileNameW(nullptr,exe,32768);
            if(n && n<32768)path=std::filesystem::path(std::wstring(exe,n)).parent_path()/"vu1known.bin";
#else
            std::error_code ec;
            path=std::filesystem::read_symlink("/proc/self/exe",ec).parent_path()/"vu1known.bin";
#endif
        }
        std::ifstream in(path,std::ios::binary);
        char magic[8]{}; uint32_t n=0,b=0;
        in.read(magic,8); in.read(reinterpret_cast<char*>(&n),4); in.read(reinterpret_cast<char*>(&b),4);
        if(in && !std::memcmp(magic,"VU1NEW1\0",8) && n && n<10000000 && b && b<1048576) {
            entries.resize(n); code.resize(b);
            in.read(reinterpret_cast<char*>(entries.data()),size_t(n)*sizeof(Entry));
            in.read(reinterpret_cast<char*>(code.data()),b);
            ready=bool(in) && std::is_sorted(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){return a.hash<b.hash;});
            for(const auto& e:entries)if(!e.pairs || e.pairs>256 || uint64_t(e.offset)+e.pairs*8>code.size())ready=false;
        }
        if(!ready) { entries.clear(); code.clear(); std::fprintf(stderr,"[vu1new] table missing\n"); std::fflush(stderr); return; }
        if(const char* map=std::getenv("PS2X_ISOMAP")) {
            std::ifstream iso(map); std::string line;
            while(std::getline(iso,line)) {
                if(line.empty() || line[0]=='#')continue;
                const auto a=line.find('\t'), b=line.find('\t',a==std::string::npos?line.size():a+1);
                if(b==std::string::npos)continue;
                File f{uint32_t(std::strtoul(line.substr(0,a).c_str(),nullptr,0)),uint32_t(std::strtoul(line.substr(a+1,b-a-1).c_str(),nullptr,0)),line.substr(b+1)};
                if(!f.path.empty() && f.path.back()=='\r')f.path.pop_back();
                if(f.sectors)files.push_back(std::move(f));
            }
            std::sort(files.begin(),files.end(),[](const File& a,const File& b){return a.lsn<b.lsn;});
        }
    }
    void summary(bool exit=false) {
        if(!ready)return;
        uint64_t d=0,u=0,m=0; aotCounters(d,u,m);
        const uint64_t vs=guestVs.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mx);
        const std::array<uint64_t,6> now{uint64_t(unknown.size()),uploads,stores,d,u,m};
        if(!exit && ((havePrinted && now==printed) || vs-lastSummary<600))return;
        char firstText[32]="-";
        if(first!=UINT64_MAX)std::snprintf(firstText,sizeof(firstText),"%llu",(unsigned long long)first);
        std::fprintf(stderr,"[vu1new] summary unknown_mpg_unique=%llu unknown_mpg_uploads=%llu direct_ee_code_stores=%llu first_unknown_vs=%s lastcd=%s declined=%llu unknownImages=%llu midcall=",
            (unsigned long long)now[0],(unsigned long long)uploads,(unsigned long long)stores,firstText,lastcd,(unsigned long long)d,(unsigned long long)u);
        if(m==UINT64_MAX)std::fprintf(stderr,"n/a\n"); else std::fprintf(stderr,"%llu\n",(unsigned long long)m);
        std::fflush(stderr); printed=now; havePrinted=true; lastSummary=vs;
    }
    ~Observer(){summary(true);}
};
Observer& observer(){static Observer o;return o;}
uint64_t hash(const uint8_t* p,uint32_t n) {
    uint64_t h=0x6a09e667f3bcc909ull;
    for(uint32_t i=0;i<n;++i) { uint64_t w;std::memcpy(&w,p+i*8,8);h=(h^w)*0x9e3779b185ebca87ull; }
    return h;
}
}
void mpg(const uint8_t* p,uint32_t n,uint32_t dest) {
    if(!enabled())return;
    auto& o=observer(); if(!o.ready)return;
    const uint64_t h=hash(p,n);
    // TLS cache avoids repeated binary searches, retaining exact byte validation.
    struct Cache { uint64_t hash=0; const Entry* entry=nullptr; };
    static thread_local std::array<Cache,256> cache{};
    auto& c=cache[(h^(h>>32))&255];
    if(c.entry && c.hash==h && c.entry->pairs==n && !std::memcmp(p,o.code.data()+c.entry->offset,n*8))return;
    auto it=std::lower_bound(o.entries.begin(),o.entries.end(),h,[](const Entry& e,uint64_t h){return e.hash<h;});
    for(;it!=o.entries.end() && it->hash==h;++it)
        if(it->pairs==n && !std::memcmp(p,o.code.data()+it->offset,n*8)) { c={h,&*it}; return; }
    const uint64_t vs=guestVs.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(o.mx);
    ++o.uploads;
    if(o.unknown.insert(h).second) {
        if(o.first==UINT64_MAX)o.first=vs;
        std::fprintf(stderr,"[vu1new] unknown MPG hash=%016llx pairs=%u dest=0x%04x vs=%llu lastcd=%s\n",(unsigned long long)h,n,dest,(unsigned long long)vs,o.lastcd);
        std::fflush(stderr);
    }
}
void directStore(uint32_t address,uint32_t bytes) {
    if(!enabled())return;
    auto& o=observer();if(!o.ready)return;
    std::lock_guard<std::mutex> lock(o.mx);++o.stores;
    if(o.ranges.insert((uint64_t(address)<<32)|bytes).second) {
        std::fprintf(stderr,"[vu1new] direct EE code store address=0x%08x bytes=%u vs=%llu lastcd=%s\n",address,bytes,(unsigned long long)guestVs.load(std::memory_order_relaxed),o.lastcd);std::fflush(stderr);
    }
}
void cdRead(uint32_t lsn) {
    if(!enabled())return;
    auto& o=observer();if(!o.ready)return;
    auto it=std::upper_bound(o.files.begin(),o.files.end(),lsn,[](uint32_t l,const File& f){return l<f.lsn;});
    const char* path=nullptr;
    if(it!=o.files.begin()) { --it;if(uint64_t(lsn)<uint64_t(it->lsn)+it->sectors)path=it->path.c_str(); }
    std::lock_guard<std::mutex> lock(o.mx);
    if(path)o.lastcd=path;
    else { std::snprintf(o.lastLsnText,sizeof(o.lastLsnText),"lsn:0x%x",lsn);o.lastcd=o.lastLsnText; }
}
void vsync(uint64_t vs) { if(!enabled())return;guestVs.store(vs,std::memory_order_relaxed);if(vs%600==0)observer().summary(); }
void shutdown() { if(enabled())observer().summary(true); }
#ifndef PS2X_WITH_VU1_AOT
void aotCounters(uint64_t& d,uint64_t& u,uint64_t& m){d=u=0;m=UINT64_MAX;}
#endif
}
