#pragma once
// TEMP(transformers): human playtest helpers (private qualification evidence, PLAYTEST_README.md). Render thread only.
//   F12 = "flag a problem now": [MARK n] line on stderr + marks.txt + a PNG of the presented game frame, written to
//         PS2X_PLAYTEST_DIR (else PS2X_DUMP_DIR, else the working directory).
//   F11 = toggle a small fps / vsync overlay.
//   F7  = toggle the PAD TEST overlay (ps2_pad.cpp, qualification): raw gamepad -> PS2 bits/pressure -> what the game read.
#include "ps2_host_backend.h" // raylib Texture
#include <cstdint>
#include <string>

namespace ps2_playtest
{
    // Directory for marks, screenshots, crash dumps (no trailing slash; "." when nothing is configured).
    const std::string &outputDir();

    // Called by UploadFrame when the displayed framebuffer page changes (a new game frame was flipped in).
    void noteFlip();

    // Called once per host frame between BeginDrawing and EndDrawing (after the game frame is drawn).
    // describe(): state text for an F12 mark (EE pc/ra, thread, pad count ...), only called when F12 was pressed.
    using DescribeFn = std::string (*)(void *user);
    void frame(const Texture &frameTex, uint32_t presentWidth, uint32_t presentHeight, uint64_t vsyncTick,
               uint64_t padReads, DescribeFn describe, void *user);
}
