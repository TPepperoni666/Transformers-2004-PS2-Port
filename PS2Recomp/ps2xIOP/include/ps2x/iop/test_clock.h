#pragma once
#include <cstdlib>
#include <atomic>
#include <ctime>
#include <cstdio>
#include <cstdint>

namespace ps2x::iop {
// TEMP(transformers): qualification fixes the guest-visible RTC as an explicit input.
// Normal play uses the host clock. UTC epoch seconds; conversion to JST stays at the caller.
inline std::time_t testClockNow(const char* source) {
    static const char* epoch = std::getenv("PS2X_TEST_RTC_EPOCH");
    if (!epoch || !*epoch) return std::time(nullptr);
    char* end = nullptr;
    const auto value = std::strtoll(epoch, &end, 10);
    if (end == epoch || *end || value < 0 || value > 4102444799LL) {
        std::fprintf(stderr, "[test:rtc] invalid PS2X_TEST_RTC_EPOCH\n");
        std::abort();
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) { std::fprintf(stderr, "[test:rtc] epoch=%lld source=%s\n", value, source); }
    return static_cast<std::time_t>(value);
}
}
