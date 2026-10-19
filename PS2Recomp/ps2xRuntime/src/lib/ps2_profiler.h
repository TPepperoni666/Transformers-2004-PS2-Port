// TEMP(transformers): optional Windows host RIP sampler; no runtime/PCH dependencies.
#pragma once
namespace ps2_profile {
void start();
void registerThread(const char* label);
void stop();
struct Session {
    Session() { start(); }
    ~Session() { stop(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};
}
