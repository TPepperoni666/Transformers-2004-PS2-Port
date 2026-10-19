#pragma once
#include <atomic>
namespace ps2x::ui {
extern std::atomic<float> g_pcAspect;
extern std::atomic<float> g_pcAspectPending;
extern std::atomic<int> g_pcPillarbox;
}
