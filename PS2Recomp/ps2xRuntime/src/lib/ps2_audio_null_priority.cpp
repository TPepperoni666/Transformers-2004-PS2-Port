// TEMP(transformers): null observer must use the same worker priority as raylib's miniaudio default.
// miniaudio.h: ma_thread_priority_default=highest; Windows maps it to THREAD_PRIORITY_HIGHEST.
// This changes only the diagnostic consumer, never the real audio device or the game thread.
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif
int ps2AudioNullSetPriority() {
#if defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) ? 2 : -1;
#else
    return 0;
#endif
}
