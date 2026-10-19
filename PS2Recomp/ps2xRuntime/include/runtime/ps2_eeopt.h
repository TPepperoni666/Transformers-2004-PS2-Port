#pragma once
// checkpoint fast path adapted from brad-richardson/PS2Recomp (GPL-3.0); budget design after snuri00/ps2-web-recomp (MIT)
#include <cstdlib>
inline bool ps2EeoptEnabled() noexcept {
    static const bool on = [] { const char *e = std::getenv("PS2X_EEOPT"); return !e || e[0] != '0'; }();
    return on;
}
