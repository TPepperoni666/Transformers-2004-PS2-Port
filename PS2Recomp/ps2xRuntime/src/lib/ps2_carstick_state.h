#pragma once
// TEMP(transformers): carstick2 EE-produced, host-only mode snapshot; no guest pointers read by the render thread.
#include <chrono>
#include <mutex>
#include <cstdint>
#include <cmath>
#include <algorithm>
namespace ps2_carstick {
enum class Mode { Unknown, Robot, Vehicle, ToVehicle, ToRobot, Unavailable };
struct Snapshot { float weight=0, deadzone=0.2f; Mode mode=Mode::Unknown; uint32_t actor=0, controller=0, vtable=0; uint64_t vs=0; std::chrono::steady_clock::time_point sampled{}; };
// GAME(transformers): six pad frames (0.2 s at 30 Hz), only while the guest is in a transform state.
inline float blend(Mode m,float previous){
 if(!std::isfinite(previous))previous=0;
 if(m==Mode::Vehicle)return 1;
 if(m==Mode::ToVehicle)return std::min(1.0f,previous+1.0f/6.0f);
 if(m==Mode::ToRobot)return std::max(0.0f,previous-1.0f/6.0f);
 return 0;
}
inline std::mutex snapshotMutex;
inline Snapshot latest;
inline Snapshot snapshot(){std::lock_guard<std::mutex> l(snapshotMutex);auto s=latest;if(std::chrono::steady_clock::now()-s.sampled>std::chrono::milliseconds(500)){s.weight=0;s.mode=Mode::Unknown;}return s;}
inline const char* name(Mode m){switch(m){case Mode::Robot:return "robot";case Mode::Vehicle:return "vehicle";case Mode::ToVehicle:return "to vehicle";case Mode::ToRobot:return "to robot";case Mode::Unavailable:return "unavailable";default:return "unknown";}}
}
void ps2CarstickSample(const uint8_t* ram,uint64_t vs);
