// Third-party adaptation notice: GPL-3.0; libgs and block-address fixes adapted from Scotho/socom-unzipped (release fix a81eb747d1c7).
// See CREDITS.md and NOTICE/THIRD_PARTY.md; original inline provenance retained.
#include "runtime/gs/gs_raw_record.h" // TEMP(transformers)
#include "Common.h"
#include "GS.h"
#include "ps2_log.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_threaded_backend.h"
#include <mutex>
#include <fstream>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace ps2_stubs
{
    namespace
    {
        bool g_storeImageTraceFinished = false; // TEMP(transformers): EE-owned verdict endpoint

        // TEMP(transformers): readback2 byte oracle at the exact EE-visible destination write.
        // A new, private file path per run; never commit/upload the resulting game bytes.
        void traceStoreImageBytes(const uint8_t *bytes, uint32_t length, uint32_t base, uint32_t bw,
                                  uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t lagged)
        {
            static const char *path = std::getenv("PS2X_STOREIMAGE_BYTE_TRACE");
            if (g_storeImageTraceFinished || !path || !*path) return;
            static std::mutex mutex;
            std::lock_guard<std::mutex> lock(mutex);
            static std::ofstream out(path, std::ios::binary | std::ios::out);
            static uint32_t sequence = 0;
            const uint32_t header[] = {0x32524253u, sequence++, length, base, bw, x, y, w, h, lagged};
            out.write(reinterpret_cast<const char *>(header), sizeof(header));
            out.write(reinterpret_cast<const char *>(bytes), length);
            // TEMP(transformers): exit bypasses static destructors; complete each byte-gate record.
            out.flush();
        }

        // GAME(transformers): the level's sun-occlusion readback (vlDepthBufferPS2 0x262D70 -> 0x2556B0 -> 0x255A90 ->
        // sceGsExecStoreImage, 64x64 CT32 "OcclusionBuffer" at GS block 12288, consumer taSun alpha; private qualification evidence)
        // drains the whole GS worker once per game frame (~132 ms EE idle with the CPU GS, ~20% of level time).
        // PS2X_STOREIMAGE_LAG=1: small CT32 readbacks are snapshotted in GS stream order on the worker and the game gets
        // the PREVIOUS call's bytes (the first call per rect stays synchronous) - the sun flare visibility lags one frame.
        struct LaggedStoreImage
        {
            std::mutex mutex;
            std::unordered_map<uint64_t, std::vector<uint8_t>> latest;
        };
        LaggedStoreImage &laggedStoreImage()
        {
            static LaggedStoreImage s_state;
            return s_state;
        }
        bool storeImageLagEnabled()
        {
            static const bool s_on = []
            {
                const char *v = std::getenv("PS2X_STOREIMAGE_LAG");
                const bool on = v && v[0] == '1';
                if (on)
                    std::cout << "[gs:storeimage] lagged readback enabled (PS2X_STOREIMAGE_LAG=1)" << std::endl;
                return on;
            }();
            return s_on;
        }

        // qualification / CROSSOVER3 H2 kill switch: PS2X_GS_IMAGE_X8=1 = the old (wrong) x8 block address in
        // sceGsExecLoadImage/sceGsExecStoreImage, for A/B runs only.
        bool gsImageX8()
        {
            static const bool s_on = []
            {
                const char *v = std::getenv("PS2X_GS_IMAGE_X8");
                return v && v[0] == '1';
            }();
            return s_on;
        }

        uint64_t makeClearPrim(bool useContext2)
        {
            return static_cast<uint64_t>(GS_PRIM_SPRITE) |
                   (static_cast<uint64_t>(useContext2 ? 1u : 0u) << 9);
        }

        uint64_t makeClearRgbaq(uint32_t rgba)
        {
            // Ported from Scotho/socom-unzipped third_party/ps2recomp/ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp
            // (sceGsSetDefDBuff, `rgbaq.value |= 0x3f80000000000000`) (GPL-3.0). The real libgraph sceGsSetDefClear
            // (SLUS_206.68 0x11D528) always builds RGBAQ = r | g<<8 | b<<16 | a<<24 | Q=1.0f<<32.
            return static_cast<uint64_t>(rgba) | 0x3F80000000000000ULL;
        }

        uint64_t makeClearXyz(int32_t x, int32_t y)
        {
            return static_cast<uint64_t>(static_cast<uint16_t>(x << 4)) |
                   (static_cast<uint64_t>(static_cast<uint16_t>(y << 4)) << 16);
        }

        void seedGsClearPacket(GsClearMem &clear,
                               int32_t width,
                               int32_t height,
                               uint32_t rgba,
                               uint32_t ztest,
                               bool useContext2)
        {
            const int32_t offX = 0x800 - (width >> 1);
            const int32_t offY = 0x800 - (height >> 1);
            const uint64_t clearTest = makeTest(0u);
            const uint64_t restoreTest = makeTest(ztest);
            const uint64_t prim = makeClearPrim(useContext2);
            const uint64_t rgbaq = makeClearRgbaq(rgba);
            const uint64_t xyz0 = makeClearXyz(offX, offY);
            const uint64_t xyz1 = makeClearXyz(offX + width, offY + height);
            const uint64_t testReg = useContext2 ? GS_REG_TEST_2 : GS_REG_TEST_1;

            clear.testa = {clearTest, testReg};
            clear.prim = {prim, GS_REG_PRIM};
            clear.rgbaq = {rgbaq, GS_REG_RGBAQ};
            clear.xyz2a = {xyz0, GS_REG_XYZ2};
            clear.xyz2b = {xyz1, GS_REG_XYZ2};
            clear.testb = {restoreTest, testReg};
        }

        bool hasSeededGsClearPacket(const GsClearMem &clear)
        {
            return clear.rgbaq.reg == GS_REG_RGBAQ &&
                   clear.xyz2a.reg == GS_REG_XYZ2 &&
                   clear.xyz2b.reg == GS_REG_XYZ2;
        }

        struct GsTrailingArgs2
        {
            uint32_t arg0 = 0u;
            uint32_t arg1 = 0u;
        };

        struct GsTrailingArgs3
        {
            uint32_t arg0 = 0u;
            uint32_t arg1 = 0u;
            uint32_t arg2 = 0u;
        };

        // Args 5-7 of the libgraph setters. EE EABI passes them in t0-t2 - never at the O32 home-area offsets
        // sp+16/20/24, which are the CALLER's frame words: Transformers' 0x265640 calls sceGsSetDefDispEnv with
        // dx = dy = 0 (t0/t1) and has its saved $s1 (sq at sp+0x10) there, so the old "all regs zero -> use stack"
        // fallback turned a saved pointer into DISPLAY.DX/DY.
        GsTrailingArgs2 decodeGsTrailingArgs2(uint8_t *rdram, R5900Context *ctx)
        {
            (void)rdram;
            return {getRegU32(ctx, 8), getRegU32(ctx, 9)};
        }

        GsTrailingArgs3 decodeGsTrailingArgs3(uint8_t *rdram, R5900Context *ctx)
        {
            (void)rdram;
            return {getRegU32(ctx, 8), getRegU32(ctx, 9), getRegU32(ctx, 10)};
        }

        // libgraph frame-buffer layout, from the libgraph linked into SLUS_206.68 (sceGszbufaddr 0x11D278,
        // sceGsSetDefDBuff 0x11D630): interlaced FIELD mode (INT=1, FFMD=0) uses ONE full-height frame buffer, every
        // other mode double-buffers. Reads the HLE gparam state set by sceGsResetGraph (guest memory is not touched).
        bool gsDefIsSingleBuffered()
        {
            return g_gparam.interlace == 1u && g_gparam.ffmode == 0u;
        }

        // sceGszbufaddr(psm, w, h): first page after the frame buffer(s) = fbw*fbh pages (single buffer) or 2*fbw*fbh.
        int32_t gsDefZbufPages(uint32_t psm, int32_t w, int32_t h)
        {
            const int32_t fbw = (w + 63 < 0) ? ((w + 126) >> 6) : ((w + 63) >> 6);
            int32_t fbh = 0;
            if ((psm & 2u) != 0u) // 16-bit formats: 64x64 pages
            {
                fbh = (h + 63 < 0) ? ((h + 126) >> 6) : ((h + 63) >> 6);
            }
            else // 32-bit formats: 64x32 pages
            {
                fbh = (h + 31 < 0) ? ((h + 62) >> 5) : ((h + 31) >> 5);
            }
            const int32_t pages = fbw * fbh;
            return static_cast<int16_t>(gsDefIsSingleBuffered() ? pages : (pages << 1));
        }

        // sceGsSetDefDispEnv's PMODE (0x11CF00 stores 0x66): read circuit 2 only (EN2), CRTMD=1, MMOD=1, AMOD=1, ALP=0.
        uint64_t makeLibgraphDefaultPmode()
        {
            return makePmode(0u, 1u, 1u, 1u, 0u, 0u);
        }

        // sceGsSetDefDispEnv's SMODE2 (0x11CFB0-0x11CFD4): interlaced ? (FRAME ? INT|FFMD : INT) : FFMD.
        uint64_t makeLibgraphDefaultSmode2()
        {
            return (g_gparam.interlace != 0u) ? ((g_gparam.ffmode != 0u) ? 3u : 1u) : 2u;
        }

        // GAME(transformers) NIGHT_B: sceGsSetDefDispEnv's DISPLAY (libgraph 0x11CF00-0x11D1C4, disassembled; the
        // BIOS-version offsets from 0x11CE50/0x11CEF0 taken as 0): MAGH+1 = (w+2559)/w, DW = (MAGH+1)*w-1,
        // DX = base + dx*(MAGH+1), DY = base + dy; NTSC DX 636, DY 50 interlaced / 25 not; PAL DX 656, DY 72 / 36;
        // DH = 2h-1 for interlaced FRAME mode, else h-1. Other video modes keep the old simple value.
        uint64_t makeLibgraphDefaultDisplay(uint32_t w, uint32_t h, uint32_t dx, uint32_t dy)
        {
            const bool interlaced = g_gparam.interlace == 1u;
            uint32_t baseDx = 0u, baseDy = 0u;
            if (g_gparam.omode == 2u)
            {
                baseDx = 636u;
                baseDy = interlaced ? 50u : 25u;
            }
            else if (g_gparam.omode == 3u)
            {
                baseDx = 656u;
                baseDy = interlaced ? 72u : 36u;
            }
            else
            {
                return makeDisplay(636u, 32u, 0u, 0u, w - 1u, h - 1u);
            }
            const uint32_t maghPlus1 = std::max<uint32_t>(1u, (w + 2559u) / std::max<uint32_t>(1u, w));
            const uint32_t dh = (interlaced && g_gparam.ffmode != 0u) ? (2u * h - 1u) : (h - 1u);
            return makeDisplay(baseDx + dx * maghPlus1, baseDy + dy, maghPlus1 - 1u, 0u, maghPlus1 * w - 1u, dh);
        }

        void applyGsClearPacket(PS2Runtime *runtime, const GsClearMem &clear)
        {
            if (!runtime->syncCoreSubsystems() || !hasSeededGsClearPacket(clear))
            {
                return;
            }

            runtime->gs().writeRegister(static_cast<uint8_t>(clear.testa.reg & 0xFFu), clear.testa.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.prim.reg & 0xFFu), clear.prim.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.rgbaq.reg & 0xFFu), clear.rgbaq.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.xyz2a.reg & 0xFFu), clear.xyz2a.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.xyz2b.reg & 0xFFu), clear.xyz2b.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.testb.reg & 0xFFu), clear.testb.value);
        }

        void refreshPacketBuilderPendingCount(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr);
        void writePacketBuilderCurrent(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t currentAddr);

        void initPacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            const uint32_t baseAddr = getRegU32(ctx, 5);
            const uint32_t words[4] = {baseAddr, baseAddr, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
        }

        uint32_t terminatePacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return 0u;
            }

            const uint32_t zero = 0u;
            while ((currentAddr & 0xCu) != 0u)
            {
                writeGuestBytes(rdram,
                                runtime,
                                currentAddr,
                                reinterpret_cast<const uint8_t *>(&zero),
                                sizeof(zero));
                currentAddr += 4u;
            }

            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr + 8u,
                            reinterpret_cast<const uint8_t *>(&zero),
                            sizeof(zero));
            return currentAddr;
        }

        void resetPacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            uint32_t baseAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr + 4u, baseAddr))
            {
                setReturnU32(ctx, 0u);
                return;
            }

            const uint32_t words[4] = {baseAddr, baseAddr, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            setReturnU32(ctx, baseAddr);
        }

        bool tryReadQwordFromGuest(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint64_t &outQword)
        {
            uint32_t low = 0u;
            uint32_t high = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, addr, low) ||
                !tryReadWordFromGuest(rdram, runtime, addr + 4u, high))
            {
                return false;
            }

            outQword = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32u);
            return true;
        }

        void writeGuestU32(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint32_t value)
        {
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&value),
                            sizeof(value));
        }

        void writeGuestU64(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint64_t value)
        {
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&value),
                            sizeof(value));
        }

        void writeGuestVec128(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, __m128i value)
        {
            alignas(16) __m128i temp = value;
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&temp),
                            sizeof(temp));
        }

        void refreshPacketBuilderPendingCount(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr)
        {
            uint32_t currentAddr = 0u;
            uint32_t pendingCountAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
                !tryReadWordFromGuest(rdram, runtime, stateAddr + 8u, pendingCountAddr) ||
                pendingCountAddr == 0u ||
                currentAddr <= pendingCountAddr)
            {
                return;
            }

            uint32_t countWord = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, pendingCountAddr, countWord))
            {
                return;
            }

            const uint32_t deltaBytes = currentAddr - pendingCountAddr;
            uint32_t deltaQwords = 0u;
            if (deltaBytes >= 16u)
            {
                deltaQwords = (deltaBytes >> 4u) - 1u;
            }

            countWord = (countWord & 0xFFFF0000u) | (deltaQwords & 0xFFFFu);
            writeGuestU32(rdram, runtime, pendingCountAddr, countWord);
        }

        void writePacketBuilderCurrent(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t currentAddr)
        {
            writeGuestU32(rdram, runtime, stateAddr, currentAddr);
            refreshPacketBuilderPendingCount(rdram, runtime, stateAddr);
        }

        // Moves the builder's current pointer WITHOUT touching the open DMA tag. The real libvifpk/libgifpk
        // Open/CloseGifTag, Open/CloseDirectCode and Align never write the DMA tag's QWC - only Terminate/End do, and the
        // real Terminate is ADDITIVE (*pDmaTag += qwc). Games that bind only some Pk functions to HLE and run the guest
        // Terminate (Transformers: OpenDirectCode/CloseGifTag HLE, Terminate guest) otherwise get the QWC twice: every
        // 2D menu record said 12 qwords instead of 6, the VIF1 chain walk overshot into vertex data and never reached
        // the frame's END/i-bit (3 s watchdog per frame on the Options and globe screens).
        void setPacketBuilderCurrent(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t currentAddr)
        {
            writeGuestU32(rdram, runtime, stateAddr, currentAddr);
        }

        uint32_t reservePacketBuilderWords(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t wordCount)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return 0u;
            }

            const uint32_t reservedAddr = currentAddr;
            currentAddr += wordCount * 4u;
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
            return reservedAddr;
        }

        void alignPacketBuilderState(uint8_t *rdram,
                                     PS2Runtime *runtime,
                                     uint32_t stateAddr,
                                     uint32_t alignMode,
                                     uint32_t reserveWords)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return;
            }

            const uint32_t adjusted = (alignMode + 2u) & 31u;
            const uint32_t shift = (32u - adjusted) & 31u;
            const uint32_t lowMask = 0xFFFFFFFFu >> shift;
            const uint32_t alignedBase = currentAddr & ~lowMask;
            uint32_t targetAddr = alignedBase + (reserveWords << 2u);
            if (targetAddr < currentAddr)
            {
                targetAddr = (targetAddr + 1u) + lowMask;
            }

            const uint32_t zero = 0u;
            while (currentAddr < targetAddr)
            {
                writeGuestU32(rdram, runtime, currentAddr, zero);
                currentAddr += 4u;
            }
            setPacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
        }

        void openPacketGifTag(uint8_t *rdram,
                              R5900Context *ctx,
                              PS2Runtime *runtime,
                              uint32_t stateAddr,
                              uint32_t openAddrOffset)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return;
            }

            writeGuestVec128(rdram, runtime, currentAddr, GPR_VEC(ctx, 5));
            setPacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
            writeGuestU32(rdram, runtime, stateAddr + openAddrOffset, currentAddr);
        }

        void closePacketGifTag(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t openAddrOffset)
        {
            uint32_t openAddr = 0u;
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr + openAddrOffset, openAddr) ||
                !tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
                openAddr == 0u)
            {
                return;
            }

            uint64_t tagValue = 0u;
            if (!tryReadQwordFromGuest(rdram, runtime, openAddr, tagValue))
            {
                return;
            }

            uint32_t packetQwords = ((currentAddr - openAddr) >> 3u) - 2u;
            const uint32_t flag = static_cast<uint32_t>((tagValue >> 58u) & 0x3u);
            if (flag != 1u)
            {
                packetQwords >>= 1u;
            }
            if (flag != 2u)
            {
                uint32_t nreg = static_cast<uint32_t>((tagValue >> 60u) & 0xFu);
                if (nreg == 0u)
                {
                    nreg = 16u;
                }
                packetQwords = (packetQwords + nreg - 1u) / nreg;
            }

            tagValue += static_cast<uint64_t>(packetQwords);
            writeGuestU32(rdram, runtime, stateAddr + openAddrOffset, 0u);
            writeGuestU64(rdram, runtime, openAddr, tagValue);

            while ((currentAddr & 0xCu) != 0u)
            {
                writeGuestU32(rdram, runtime, currentAddr, 0u);
                currentAddr += 4u;
            }
            setPacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
        }
    }

    void sceGifPkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint64_t dataValue = GPR_U64(ctx, 6);
        const uint64_t regValue = static_cast<uint64_t>(getRegU32(ctx, 5));
        writeGuestU64(rdram, runtime, currentAddr, dataValue);
        writeGuestU64(rdram, runtime, currentAddr + 8u, regValue);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
    }

    void sceGifPkAddGsData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        writeGuestU64(rdram, runtime, currentAddr, GPR_U64(ctx, 5));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 8u);
    }

    void sceGifPkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        closePacketGifTag(rdram, runtime, getRegU32(ctx, 4), 12u);
    }

    void sceGifPkCnt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t countValue = getRegU32(ctx, 5);
        const uint32_t extraValue = getRegU32(ctx, 6);
        const uint32_t tagWord = getRegU32(ctx, 7) | 0x10000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, 0u, countValue, extraValue};
        const uint32_t nextAddr = packetAddr + 16u;

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, nextAddr);
    }

    void sceGifPkEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t countValue = getRegU32(ctx, 5);
        const uint32_t extraValue = getRegU32(ctx, 6);
        const uint32_t tagWord = getRegU32(ctx, 7) | 0x70000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, countValue, extraValue, 0u};
        const uint32_t nextAddr = packetAddr + 16u;

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, nextAddr);
    }

    void sceGifPkInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        initPacketBuilderState(rdram, ctx, runtime);
    }

    void sceGifPkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        openPacketGifTag(rdram, ctx, runtime, getRegU32(ctx, 4), 12u);
    }

    void sceGifPkRef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t refAddr = getRegU32(ctx, 5) & 0x9FFFFFFFu;
        const uint32_t tagWord = getRegU32(ctx, 9) | getRegU32(ctx, 6) | 0x30000000u;
        const uint32_t extra0 = getRegU32(ctx, 7);
        const uint32_t extra1 = getRegU32(ctx, 8);
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, refAddr, extra0, extra1};

        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);
    }

    void sceGifPkRefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t dbp = getRegU32(ctx, 5) & 0xFFFFu;
        const uint32_t dpsm = getRegU32(ctx, 6) & 0xFFu;
        const uint32_t dbw = getRegU32(ctx, 7) & 0xFFFFu;
        uint32_t dataAddr = getRegU32(ctx, 8);
        uint32_t qwcRemaining = getRegU32(ctx, 9);
        const uint32_t dsax = getRegU32(ctx, 10);
        const uint32_t dsay = getRegU32(ctx, 11);
        const uint32_t width = readStackU32(rdram, ctx, 0);
        const uint32_t height = readStackU32(rdram, ctx, 8);

        // Open a 4-register A+D GIF tag and emit the GS load-image setup.
        {
            const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t words[4] = {0x10000000u, 0u, 0u, 0u};
            writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
            writeGuestBytes(rdram,
                            runtime,
                            packetAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);

            // Seed an open A+D tag (nloop=0, EOP clear): closePacketGifTag adds the true
            // appended qword count, so a pre-set nloop would double-count. Open variant
            // (not makeGiftagAplusD) because that always sets EOP on this chained tag.
            const uint64_t giftag[2] = {makeGiftagAplusDOpen(0u), 0xEULL};
            uint32_t currentAddr = packetAddr + 16u;
            writeGuestBytes(rdram, runtime, currentAddr, reinterpret_cast<const uint8_t *>(giftag), sizeof(giftag));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
            writeGuestU32(rdram, runtime, stateAddr + 12u, currentAddr);

            const uint64_t bitbltbuf =
                (static_cast<uint64_t>(dbp) << 32u) |
                (static_cast<uint64_t>(dbw & 0xFFu) << 48u) |
                (static_cast<uint64_t>(dpsm) << 56u);
            const uint64_t trxpos =
                (static_cast<uint64_t>(dsax) << 32u) |
                (static_cast<uint64_t>(dsay) << 48u);
            const uint64_t trxreg =
                static_cast<uint64_t>(width) |
                (static_cast<uint64_t>(height) << 32u);

            {
                uint32_t addr = 0u;
                if (!tryReadWordFromGuest(rdram, runtime, stateAddr, addr))
                {
                    return;
                }
                writeGuestU64(rdram, runtime, addr, bitbltbuf);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_BITBLTBUF));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, trxpos);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXPOS));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, trxreg);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXREG));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, 0u);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXDIR));
                addr += 16u;
                writePacketBuilderCurrent(rdram, runtime, stateAddr, addr);
                closePacketGifTag(rdram, runtime, stateAddr, 12u);
            }
        }

        while (qwcRemaining != 0u)
        {
            const uint32_t chunkQwc = std::min<uint32_t>(qwcRemaining, 32767u);

            const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t words[4] = {0x10000000u, 0u, 0u, 0u};
            writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
            writeGuestBytes(rdram,
                            runtime,
                            packetAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);

            const uint32_t reservedAddr = reservePacketBuilderWords(rdram, runtime, stateAddr, 4u);
            const bool isLastChunk = (chunkQwc == qwcRemaining);
            const uint64_t gifTag =
                static_cast<uint64_t>(chunkQwc) |
                (isLastChunk ? 0x0800000000008000ULL : 0x0800000000000000ULL);
            writeGuestU64(rdram, runtime, reservedAddr, gifTag);
            writeGuestU64(rdram, runtime, reservedAddr + 8u, 0u);

            const uint32_t refPacketAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t refWords[4] = {0x30000000u | chunkQwc, dataAddr & 0x9FFFFFFFu, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            refPacketAddr,
                            reinterpret_cast<const uint8_t *>(refWords),
                            sizeof(refWords));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, refPacketAddr + 16u);

            qwcRemaining -= chunkQwc;
            dataAddr += chunkQwc * 16u;
        }
    }

    void sceGifPkReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resetPacketBuilderState(rdram, ctx, runtime);
    }

    void sceGifPkReserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, reservePacketBuilderWords(rdram, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5)));
    }

    void sceGifPkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, terminatePacketBuilderState(rdram, ctx, runtime));
    }

    void sceGsExecLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        uint32_t srcAddr = getRegU32(ctx, 5);

        GsImageMem img{};
        if (!runtime || !runtime->syncCoreSubsystems() || !readGsImage(rdram, imgAddr, img))
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t rowBytes = bytesForPixels(img.psm, static_cast<uint32_t>(img.width));
        if (rowBytes == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t fbw = img.vram_width ? img.vram_width : std::max<uint32_t>(1, (img.width + 63) / 64);
        const uint32_t totalImageBytes = rowBytes * static_cast<uint32_t>(img.height);
        const uint32_t headerQwc = 6u;
        const uint32_t imageQwc = (totalImageBytes + 15u) / 16u;
        const uint32_t totalQwc = headerQwc + imageQwc;

        uint32_t pktAddr = runtime->guestMalloc(totalQwc * 16u, 16u);
        if (pktAddr == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint8_t *pkt = getMemPtr(rdram, pktAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (!pkt || !src)
        {
            runtime->guestFree(pktAddr);
            setReturnS32(ctx, -1);
            return;
        }

        // qualification / CROSSOVER3 H2 (Scotho/socom-unzipped a81eb747d1c7): libgraph's vram_addr is already the BITBLTBUF
        // block field (256-byte blocks, 14 bits) - sceGsSetDefLoadImage(lp, dbp, dbw, dpsm, x, y, w, h) takes block units.
        // The former `* 2048 / 256` (x8) sent every upload to 8x its block: Transformers' VRAM clear fn_0025E670 (16384
        // 8x8 CT32 uploads, dbp 0..0x3FFF) only reached every 8th block. PS2X_GS_IMAGE_X8=1 restores the old address.
        uint32_t dbp = gsImageX8() ? (static_cast<uint32_t>(img.vram_addr) * 2048u) / 256u
                                   : static_cast<uint32_t>(img.vram_addr) & 0x3FFFu;
        { // TEMP(transformers) qualification: first load-image calls, to show the block address in the run log.
            static uint32_t s_logged = 0u;
            if (s_logged < 3u)
            {
                ++s_logged;
                std::cout << "[gs:loadimage] vram_addr=0x" << std::hex << img.vram_addr << " -> dbp=0x" << dbp << " (x8 would be 0x"
                          << ((static_cast<uint32_t>(img.vram_addr) * 8u) & 0x3FFFu) << ") " << std::dec << img.width << "x" << img.height
                          << " psm=" << static_cast<uint32_t>(img.psm) << " fbw=" << fbw << std::endl;
            }
        }
        uint32_t dsax = static_cast<uint32_t>(img.x);
        uint32_t dsay = static_cast<uint32_t>(img.y);

        // Full messy
        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
        q[0] = makeGiftagAplusD(4u);
        q[1] = 0xEULL;
        q[2] = (static_cast<uint64_t>(img.psm & 0x3Fu) << 24) | (static_cast<uint64_t>(1u) << 16) |
               (static_cast<uint64_t>(dbp & 0x3FFFu) << 32) | (static_cast<uint64_t>(fbw & 0x3Fu) << 48) |
               (static_cast<uint64_t>(img.psm & 0x3Fu) << 56);
        q[3] = 0x50ULL;
        q[4] = (static_cast<uint64_t>(dsay & 0x7FFu) << 48) | (static_cast<uint64_t>(dsax & 0x7FFu) << 32);
        q[5] = 0x51ULL;
        q[6] = (static_cast<uint64_t>(img.height) << 32) | static_cast<uint64_t>(img.width);
        q[7] = 0x52ULL;
        q[8] = 0ULL;
        q[9] = 0x53ULL;
        q[10] = (static_cast<uint64_t>(2) << 58) | (static_cast<uint64_t>(imageQwc) & 0x7FFF) |
                (1ULL << 15);
        q[11] = 0ULL;

        std::memcpy(pkt + headerQwc * 16u, src, totalImageBytes);

        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
        auto &mem = runtime->memory();
        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
        mem.writeIORegister(GIF_CHANNEL + 0x20u, totalQwc & 0xFFFFu);
        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
        mem.processPendingTransfers();
        runtime->guestFree(pktAddr);

        setReturnS32(ctx, 0);
    }

    // TEMP(transformers): successful verdict is on the EE thread; prior records are flushed.
    void finishStoreImageByteTrace() { g_storeImageTraceFinished = true; }

    void sceGsExecStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        uint32_t dstAddr = getRegU32(ctx, 5);

        GsImageMem img{};
        if (!runtime || !runtime->syncCoreSubsystems() || !readGsImage(rdram, imgAddr, img))
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t rowBytes = bytesForPixels(img.psm, static_cast<uint32_t>(img.width));
        if (rowBytes == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t fbw = img.vram_width ? img.vram_width : std::max<uint32_t>(1, (img.width + 63) / 64);
        const uint32_t totalImageBytes = rowBytes * static_cast<uint32_t>(img.height);

        uint8_t *dst = getMemPtr(rdram, dstAddr);
        if (!dst)
        {
            setReturnS32(ctx, -1);
            return;
        }

        // qualification / CROSSOVER3 H2: vram_addr is the SBP block field directly (see sceGsExecLoadImage). The sun-occlusion
        // caller sub_00255A90 passes FBP<<5 (lhu 0x10(a0) & 0x1FF, FRAME.FBP of its render target, i.e. block units).
        uint32_t sbp = gsImageX8() ? (static_cast<uint32_t>(img.vram_addr) * 2048u) / 256u
                                   : static_cast<uint32_t>(img.vram_addr) & 0x3FFFu;
        { // TEMP(transformers) qualification: first store-image calls (the per-frame sun-occlusion readback).
            static uint32_t s_logged = 0u;
            if (s_logged < 3u)
            {
                ++s_logged;
                std::cout << "[gs:storeimage] vram_addr=0x" << std::hex << img.vram_addr << " -> sbp=0x" << sbp << " (x8 would be 0x"
                          << ((static_cast<uint32_t>(img.vram_addr) * 8u) & 0x3FFFu) << ") " << std::dec << img.width << "x" << img.height
                          << " at " << img.x << "," << img.y << " psm=" << static_cast<uint32_t>(img.psm) << " fbw=" << fbw << std::endl;
            }
        }
        uint64_t bitbltbuf = (static_cast<uint64_t>(sbp & 0x3FFFu) << 0) |
                             (static_cast<uint64_t>(fbw & 0x3Fu) << 16) |
                             (static_cast<uint64_t>(img.psm & 0x3Fu) << 24) |
                             (static_cast<uint64_t>(0u) << 32) |
                             (static_cast<uint64_t>(1u) << 48) |
                             (static_cast<uint64_t>(0u) << 56);
        uint64_t trxpos = (static_cast<uint64_t>(img.x & 0x7FFu) << 0) |
                          (static_cast<uint64_t>(img.y & 0x7FFu) << 16) |
                          (static_cast<uint64_t>(0u) << 32) |
                          (static_cast<uint64_t>(0u) << 48);
        uint64_t trxreg = static_cast<uint64_t>(img.height) << 32 | static_cast<uint64_t>(img.width);

        // GAME(transformers): lagged sun-occlusion readback, see laggedStoreImage() above.
        if (storeImageLagEnabled() && img.psm == 0u && img.width > 0 && img.height > 0 && img.width <= 128 &&
            img.height <= 128)
        {
            const uint32_t w = static_cast<uint32_t>(img.width), h = static_cast<uint32_t>(img.height);
            const uint32_t x0 = static_cast<uint32_t>(img.x), y0 = static_cast<uint32_t>(img.y);
            const uint32_t base = sbp & 0x3FFFu, bw = fbw & 0x3Fu;
            const uint64_t key = (static_cast<uint64_t>(base) << 46) | (static_cast<uint64_t>(bw) << 40) |
                                 (static_cast<uint64_t>(x0 & 0x7FFu) << 29) | (static_cast<uint64_t>(y0 & 0x7FFu) << 18) |
                                 (static_cast<uint64_t>(w & 0x1FFu) << 9) | static_cast<uint64_t>(h & 0x1FFu);
            std::vector<uint8_t> previous;
            {
                LaggedStoreImage &lag = laggedStoreImage();
                std::lock_guard<std::mutex> lock(lag.mutex);
                auto it = lag.latest.find(key);
                if (it != lag.latest.end() && it->second.size() == totalImageBytes)
                    previous = it->second;
            }
            auto enqueueReadback=[=]() { return GSThreadedBackend::EnqueueOrderedForVram(
                runtime->memory().getGSVRAM(), [=](GSRasterBackend &backend)
                {
                    // TEMP(transformers): raw backends (M1 parallel-gs) read the rect themselves (one GPU page read).
                    if (backend.ReadRectAsync(0u, base, bw, x0, y0, w, h,
                        [key](std::vector<uint8_t> bytes) {
                            LaggedStoreImage &lag = laggedStoreImage();
                            std::lock_guard<std::mutex> lock(lag.mutex);
                            lag.latest[key] = std::move(bytes);
                        })) return;
                    // Same pixel walk/byte order as GSCpuBackend::PerformLocalToHostTransfer (CT32, little endian).
                    std::vector<uint8_t> bytes(static_cast<size_t>(w) * h * 4u);
                    for (uint32_t yy = 0; yy < h; ++yy)
                        for (uint32_t xx = 0; xx < w; ++xx)
                        {
                            const uint32_t v = backend.ReadVram(0u, base, bw, x0 + xx, y0 + yy);
                            uint8_t *out = &bytes[(static_cast<size_t>(yy) * w + xx) * 4u];
                            out[0] = static_cast<uint8_t>(v);
                            out[1] = static_cast<uint8_t>(v >> 8u);
                            out[2] = static_cast<uint8_t>(v >> 16u);
                            out[3] = static_cast<uint8_t>(v >> 24u);
                        }
                    LaggedStoreImage &lag = laggedStoreImage();
                    std::lock_guard<std::mutex> lock(lag.mutex);
                    lag.latest[key] = std::move(bytes);
                });
            };
            bool queued;
            auto &memory=runtime->memory();
            if(memory.m_vif1Instant) {
                memory.m_vif1Instant->enqueue([&memory,enqueueReadback] {
                    memory.m_vif1Instant->emit([enqueueReadback] { if(!enqueueReadback())throw std::runtime_error("ordered lagged readback unavailable"); });
                });
                queued=true;
            }else queued=enqueueReadback();
            if (queued && !previous.empty())
            {
                std::memcpy(dst, previous.data(), totalImageBytes);
                traceStoreImageBytes(dst, totalImageBytes, base, bw, x0, y0, w, h, 1u); // TEMP(transformers)
                setReturnS32(ctx, 0);
                return;
            }
            // First call for this rect (or no GS worker): fall through to the exact synchronous readback.
        }

        uint32_t pktAddr = runtime->guestMalloc(80u, 16u);
        if (pktAddr == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint8_t *pkt = getMemPtr(rdram, pktAddr);
        if (!pkt)
        {
            runtime->guestFree(pktAddr);
            setReturnS32(ctx, -1);
            return;
        }

        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
        q[0] = makeGiftagAplusD(4u);
        q[1] = 0xEULL;
        q[2] = bitbltbuf;
        q[3] = 0x50ULL;
        q[4] = trxpos;
        q[5] = 0x51ULL;
        q[6] = trxreg;
        q[7] = 0x52ULL;
        q[8] = 1ULL;
        q[9] = 0x53ULL;

        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
        auto &mem = runtime->memory();
        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
        mem.writeIORegister(GIF_CHANNEL + 0x20u, 5u);
        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
        mem.processPendingTransfers();

        ps2TraceGuestRangeWrite(rdram, dstAddr, totalImageBytes, "sceGsExecStoreImage", ctx);
        runtime->gs().consumeLocalToHostBytes(dst, totalImageBytes);
        traceStoreImageBytes(dst, totalImageBytes, sbp & 0x3FFFu, fbw & 0x3Fu,
                             uint32_t(img.x), uint32_t(img.y), uint32_t(img.width), uint32_t(img.height), 0u); // TEMP(transformers)
        runtime->guestFree(pktAddr);

        setReturnS32(ctx, 0);
    }

    void sceGsGetGParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t addr = writeGsGParamToScratch(runtime);
        setReturnU32(ctx, addr);
    }

    void sceGsPutDispEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        GsDispEnvMem env{};
        if (!readGsDispEnv(rdram, envAddr, env))
        {
            setReturnS32(ctx, -1);
            return;
        }
        applyGsDispEnv(runtime, env);
        setReturnS32(ctx, 0);
    }

    void sceGsPutDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        GsRegPairMem pairs[8]{};
        if (!readGsRegPairs(rdram, envAddr, pairs, 8u))
        {
            setReturnS32(ctx, -1);
            return;
        }
        applyGsRegPairs(runtime, pairs, 8u);
        setReturnS32(ctx, 0);
    }

    void sceGsResetGraph(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t mode = getRegU32(ctx, 4);
        uint32_t interlace = getRegU32(ctx, 5);
        uint32_t omode = getRegU32(ctx, 6);
        uint32_t ffmode = getRegU32(ctx, 7);

        if (mode == 0)
        {
            if (runtime && !runtime->syncCoreSubsystems())
            {
                setReturnS32(ctx, -1);
                return;
            }

            g_gparam.interlace = static_cast<uint8_t>(interlace & 0x1);
            g_gparam.omode = static_cast<uint8_t>(omode & 0xFF);
            g_gparam.ffmode = static_cast<uint8_t>(ffmode & 0x1);
            // No writeGsGParamToScratch() here any more: it stored these 4 bytes at guest scratchpad 0x70000100 on
            // every ResetGraph. Transformers keeps its GS register shadow table at SPR 0x60 + reg*8 (setter 0x225D60),
            // so 0x100 is its TEX1_1 shadow, which it saves/restores around draws (0x270E80, 0x42E488, 0x42A824) - the
            // stub corrupted it at boot and at every loading screen. sceGszbufaddr now reads g_gparam directly;
            // the HLE sceGsGetGParam still publishes the copy when a game binds it.
            uint64_t pmode = makePmode(1, 0, 0, 0, 0, 0x80);
            uint64_t smode2 = (interlace & 0x1) | ((ffmode & 0x1) << 1);
            uint64_t dispfb = makeDispFb(0, 10, 0, 0, 0);
            uint64_t display = makeDisplay(0, 0, 0, 0, 639, 447);
            uint64_t bgcolor = 0ULL;

            if (runtime)
            {
                // Ported from ran-j/PS2Recomp PR #185 (GPL-3.0): PMODE/SMODE2 are privileged registers, not GIF A+D
                // registers. Their old A+D addresses 0x41/0x42 are SCISSOR_2/ALPHA_1, so the packet clobbered the
                // context-2 scissor and context-1 alpha on every ResetGraph (Transformers: boot 0x25FB44, video-mode
                // change 0x25A368, every loading screen 0x26F4DC) and never set PMODE/SMODE2. Write them straight to
                // privileged state (as applyGsDispEnv does); SMODE2 INT/FFMD is what the real SetGsCrt programs.
                auto &privRegs = runtime->memory().gs();
                gsraw::PrivWrite(&privRegs,0x12000000u+0x0u,pmode); // TEMP(transformers)
                privRegs.pmode = pmode;
                gsraw::PrivWrite(&privRegs,0x12000000u+0x20u,smode2); // TEMP(transformers)
                privRegs.smode2 = smode2;
                // TEMP(transformers): the real SetGsCrt (called by sceGsResetGraph) also programs SMODE1/SYNCV for the video
                // mode (omode 2 = NTSC, 3 = PAL). Left at 0, parallel-gs (PS2X_GS_BACKEND=PGS) produces no scanout; the CPU
                // presenter does not read them. Values: runtime/gs/ps2_gs_common.h GSCrtDefaults.
                const bool pal = (omode & 0xFFu) == 3u;
                const uint64_t smode1 = pal ? GSCrtDefaults::kSmode1Pal : GSCrtDefaults::kSmode1Ntsc;
                const uint64_t syncv = pal ? GSCrtDefaults::kSyncvPal : GSCrtDefaults::kSyncvNtsc;
                gsraw::PrivWrite(&privRegs,0x12000000u+0x10u,smode1); // TEMP(transformers)
                privRegs.smode1 = smode1;
                gsraw::PrivWrite(&privRegs,0x12000000u+0x60u,syncv); // TEMP(transformers)
                privRegs.syncv = syncv;

                uint32_t pktAddr = runtime->guestMalloc(96u, 16u);
                if (pktAddr != 0u)
                {
                    uint8_t *pkt = getMemPtr(rdram, pktAddr);
                    if (pkt)
                    {
                        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
                        q[0] = makeGiftagAplusD(5u);
                        q[1] = 0xEULL;
                        q[2] = dispfb;
                        q[3] = 0x59ULL;
                        q[4] = display;
                        q[5] = 0x5aULL;
                        q[6] = dispfb;
                        q[7] = 0x5bULL;
                        q[8] = display;
                        q[9] = 0x5cULL;
                        q[10] = bgcolor;
                        q[11] = 0x5fULL;
                        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
                        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
                        auto &mem = runtime->memory();
                        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
                        mem.writeIORegister(GIF_CHANNEL + 0x20u, 6u);
                        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
                        mem.processPendingTransfers();
                        runtime->guestFree(pktAddr);
                    }
                    else
                    {
                        runtime->guestFree(pktAddr);
                    }
                }
            }
        }

        setReturnS32(ctx, 0);
    }

    void sceGsResetPath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefClear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t psm = getRegU32(ctx, 5);
        uint32_t w = getRegU32(ctx, 6);
        uint32_t h = getRegU32(ctx, 7);
        const GsTrailingArgs3 trailing = decodeGsTrailingArgs3(rdram, ctx);
        const uint32_t ztest = trailing.arg0;
        const uint32_t zpsm = trailing.arg1;
        const uint32_t clear = trailing.arg2;

        if (w == 0u)
        {
            w = 640u;
        }
        if (h == 0u)
        {
            h = 448u;
        }

        const uint32_t fbw = std::max<uint32_t>(1u, (w + 63u) / 64u);
        // Ported from ran-j/PS2Recomp PR #152 (GPL-3.0): seed read circuit 2 only (EN2) - the swap path programs
        // DISPFB2/DISPLAY2 and never circuit 1. Value = libgraph sceGsSetDefDispEnv's PMODE 0x66.
        const uint64_t pmode = makeLibgraphDefaultPmode();
        const uint64_t smode2 =
            (static_cast<uint64_t>(g_gparam.interlace & 0x1u) << 0) |
            (static_cast<uint64_t>(g_gparam.ffmode & 0x1u) << 1);
        const uint64_t display = makeDisplay(636u, 32u, 0u, 0u, w - 1u, h - 1u);

        const int32_t drawWidth = static_cast<int32_t>(w);
        const int32_t drawHeight = static_cast<int32_t>(h);

        uint32_t zbufAddr = 0u;
        {
            R5900Context temp = *ctx;
            sceGszbufaddr(rdram, &temp, runtime);
            zbufAddr = getRegU32(&temp, 2);
        }

        const uint32_t fbp1 = zbufAddr;
        const uint64_t dispfb0 = makeDispFb(fbp1, fbw, psm, 0u, 0u);
        const uint64_t dispfb1 = makeDispFb(0u, fbw, psm, 0u, 0u);

        GsDBuffDcMem db{};
        db.disp[0].pmode = pmode;
        db.disp[0].smode2 = smode2;
        db.disp[0].dispfb = dispfb0;
        db.disp[0].display = display;
        db.disp[0].bgcolor = 0u;
        db.disp[1] = db.disp[0];
        db.disp[1].dispfb = dispfb1;

        const bool seedClear = clear != 0u;
        db.giftag0 = {makeGiftagAplusD(seedClear ? 22u : 16u), 0xEULL};
        seedGsDrawEnv1(db.draw01, drawWidth, drawHeight, 0u, fbw, psm, zbufAddr, zpsm, ztest, false);
        seedGsDrawEnv2(db.draw02, drawWidth, drawHeight, 0u, fbw, psm, zbufAddr, zpsm, ztest, false);
        db.giftag1 = db.giftag0;
        seedGsDrawEnv1(db.draw11, drawWidth, drawHeight, fbp1, fbw, psm, zbufAddr, zpsm, ztest, false);
        seedGsDrawEnv2(db.draw12, drawWidth, drawHeight, fbp1, fbw, psm, zbufAddr, zpsm, ztest, false);
        if (seedClear)
        {
            seedGsClearPacket(db.clear0, drawWidth, drawHeight, 0u, ztest, false);
            seedGsClearPacket(db.clear1, drawWidth, drawHeight, 0u, ztest, true);
        }

        if (!writeGsDBuffDc(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }
        setReturnS32(ctx, 0);
    }

    // sceGsSetDefDBuff(sceGsDBuff *db, short psm, short w, short h, short ztest, short zpsm, short clear)
    // Ported from Scotho/socom-unzipped third_party/ps2recomp/ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp:953-1013
    // (trailing args from registers + seeded clear packets) and ran-j/PS2Recomp PR #186 (args) / PR #152 (PMODE EN2)
    // (GPL-3.0). Buffer-1 FBP offset, giftag and clear layout follow the libgraph sceGsSetDefDBuff linked into
    // SLUS_206.68 (0x11D630): SetDefDispEnv x2, SetDefDrawEnv x2, SetDefClear x2 (both context 1) when clear != 0,
    // giftag NLOOP = clear ? 14 : 8 (EOP, NREG=1, A+D), then draw0.FRAME.FBP = disp[1].DISPFB.FBP = zbufaddr/2 unless
    // interlaced FIELD mode (single buffer).
    // Transformers: only caller 0x26F4FC (loading-screen setup 0x26F470, right after sceGsResetGraph(0,x,2|3,FRAME))
    // passes t0=3 (ZGREATER), t1=0x31 (PSMZ24), t2=1. Its vsync handler (0x26E990 -> 0x26F1F0) then shows disp[field]
    // and pushes giftag[field]+draw[field]+clear[field] (NLOOP forced to 14) through the GIF FIFO, so the old stub
    // (TEST/ZBUF from caller-stack garbage, zeroed clear pairs, both buffers at FBP 0) drew every field into the
    // displayed buffer with no colour/Z clear.
    void sceGsSetDefDBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t envAddr = getRegU32(ctx, 4);
        // libgraph takes shorts (sll/sra 16) and masks the pixel formats with 0xF.
        const uint32_t psm = static_cast<uint32_t>(static_cast<int16_t>(getRegU32(ctx, 5) & 0xFFFFu)) & 0xFu;
        int32_t w = static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFFu);
        int32_t h = static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFFu);
        // EE ABI: args 5-7 are passed in $t0-$t2 (GPR 8-10), never on the stack.
        const int32_t ztestArg = static_cast<int16_t>(getRegU32(ctx, 8) & 0xFFFFu);
        const int32_t zpsmArg = static_cast<int16_t>(getRegU32(ctx, 9) & 0xFFFFu);
        const int32_t clearArg = static_cast<int16_t>(getRegU32(ctx, 10) & 0xFFFFu);
        const uint32_t ztest = static_cast<uint32_t>(ztestArg);
        const uint32_t zpsm = static_cast<uint32_t>(zpsmArg) & 0xFu;
        const bool seedClear = clearArg != 0;

        if (w <= 0)
        {
            w = 640;
        }
        if (h <= 0)
        {
            h = 448;
        }

        const uint32_t fbw = std::max<uint32_t>(1u, (static_cast<uint32_t>(w) + 63u) / 64u);
        const uint64_t pmode = makeLibgraphDefaultPmode();
        const uint64_t smode2 = makeLibgraphDefaultSmode2();
        // GAME(transformers) NIGHT_B: libgraph's DISPLAY (sceGsSetDefDispEnv 0x11CF00, dx=dy=0): for the loading screens
        // (640x224 interlaced FRAME) DX 636 DY 50 MAGH 3 DW 2559 DH 447 - the same circuit-1 DISPLAY the game's vsync
        // handler 0x26F1F0 writes itself. The old simple value (DX 636 DY 32 MAGH 0 DW w-1 DH h-1) made circuit 2 only
        // 112 field rows tall once the CPU presenter honours SMODE2 FFMD. PS2X_GS_DBUFF_DISPLAY_LEGACY=1 = old value.
        static const bool s_legacyDBuffDisplay = []()
        {
            const char *value = std::getenv("PS2X_GS_DBUFF_DISPLAY_LEGACY");
            return value != nullptr && value[0] == '1';
        }();
        const uint64_t display = s_legacyDBuffDisplay
                                     ? makeDisplay(636u, 32u, 0u, 0u, static_cast<uint32_t>(w) - 1u, static_cast<uint32_t>(h) - 1u)
                                     : makeLibgraphDefaultDisplay(static_cast<uint32_t>(w), static_cast<uint32_t>(h), 0u, 0u);

        const uint32_t zbufAddr = static_cast<uint32_t>(gsDefZbufPages(psm, w, h)) & 0x1FFu;
        const uint32_t buffer1Fbp = gsDefIsSingleBuffered() ? 0u : ((zbufAddr >> 1) & 0x1FFu);
        const bool dthe = (psm & 2u) != 0u; // sceGsSetDefDrawEnv: DTHE on for 16-bit frame formats

        // Start from the guest bytes: libgraph leaves clear0/clear1 untouched when clear == 0.
        GsDBuffMem db{};
        (void)readGsDBuff(rdram, envAddr, db);

        db.disp[0].pmode = pmode;
        db.disp[0].smode2 = smode2;
        db.disp[0].dispfb = makeDispFb(0u, fbw, psm, 0u, 0u);
        db.disp[0].display = display;
        db.disp[0].bgcolor = 0u;
        db.disp[1] = db.disp[0];
        db.disp[1].dispfb = makeDispFb(buffer1Fbp, fbw, psm, 0u, 0u);

        // GIFtag: NLOOP 8 (draw env) or 14 (draw env + clear), EOP, PACKED, NREG=1, REGS[0]=A+D.
        db.giftag0 = {makeGiftagAplusD(seedClear ? 14u : 8u), 0xEULL};
        db.giftag1 = db.giftag0;
        // Swap id 0 shows disp[0] (FBP 0) and draws with draw0 into the other buffer; id 1 the reverse.
        seedGsDrawEnv1(db.draw0, w, h, buffer1Fbp, fbw, psm, zbufAddr, zpsm, ztest, dthe);
        seedGsDrawEnv1(db.draw1, w, h, 0u, fbw, psm, zbufAddr, zpsm, ztest, dthe);
        if (seedClear)
        {
            // SetDefClear(&clearN, ztest, 0x800 - w/2, 0x800 - h/2, w, h, 0,0,0,0, z=0), both in context 1 (TEST_1).
            seedGsClearPacket(db.clear0, w, h, 0u, ztest, false);
            db.clear1 = db.clear0;
        }

        if (!writeGsDBuff(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefDispEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t psm = getRegU32(ctx, 5);
        uint32_t w = getRegU32(ctx, 6);
        uint32_t h = getRegU32(ctx, 7);
        const GsTrailingArgs2 trailing = decodeGsTrailingArgs2(rdram, ctx);
        uint32_t dx = trailing.arg0;
        uint32_t dy = trailing.arg1;

        if (w == 0)
            w = 640;
        if (h == 0)
            h = 448;

        uint32_t fbw = (w + 63) / 64;
        uint64_t dispfb = makeDispFb(0, fbw, psm, 0, 0);
        uint64_t display = makeDisplay(dx, dy, 0, 0, w - 1, h - 1);

        // Same family as PR #152 (ran-j/PS2Recomp, GPL-3.0): the libgraph sceGsSetDefDispEnv linked into SLUS_206.68
        // (0x11CF00) writes ALL five fields - PMODE 0x66 (EN2, MMOD, AMOD, ALP 0), SMODE2 from gparam, DISPFB,
        // DISPLAY, BGCOLOR 0. The stub only replaced DISPFB/DISPLAY, so PMODE/SMODE2/BGCOLOR kept whatever was in
        // guest memory. Transformers' display builder 0x265640 (hub/menus, consumed by 0x263740) calls this on an
        // uninitialised stack temp, rewrites only EN1/EN2/ALP(/SLBG), FFMD, DISPFB and DISPLAY, and keeps PMODE
        // MMOD/AMOD(/SLBG), SMODE2 INT and BGCOLOR from here - i.e. stack garbage decided how our presenter blended
        // circuit 1 (its flicker-filter copy) over circuit 2 / BGCOLOR. DISPLAY stays the stub's simple value (that
        // caller recomputes every DISPLAY field itself).
        // TEMP(transformers) REVIEW(port-gow batch 2): PS2X_GS_DEFDISPENV_LEGACY=1 restores the old stub (DISPFB/DISPLAY
        // only) for an A/B. With the full write the hub/menu path above gets SMODE2 INT=1 (the game then sets FFMD=0), so
        // the presenter applies field bob (applyFieldPresentation), plus PMODE 0x8067 = 50% CRT1-over-CRT2 blend.
        static const bool s_legacyDefDispEnv = []()
        {
            const char *value = std::getenv("PS2X_GS_DEFDISPENV_LEGACY");
            return value != nullptr && value[0] == '1';
        }();
        if (s_legacyDefDispEnv)
        {
            writeGsDispEnv(rdram, envAddr, display, dispfb);
            setReturnS32(ctx, 0);
            return;
        }
        uint8_t *const envPtr = getMemPtr(rdram, envAddr);
        if (envPtr)
        {
            const GsDispEnvMem env{makeLibgraphDefaultPmode(), makeLibgraphDefaultSmode2(), dispfb, display, 0u};
            std::memcpy(envPtr, &env, sizeof(env));
        }
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t param_2 = getRegU32(ctx, 5);
        int32_t w = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFF));
        int32_t h = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFF));
        const GsTrailingArgs2 trailing = decodeGsTrailingArgs2(rdram, ctx);
        uint32_t param_5 = trailing.arg0;
        uint32_t param_6 = trailing.arg1;

        if (w <= 0)
            w = 640;
        if (h <= 0)
            h = 448;

        uint32_t psm = param_2 & 0xFU;
        uint32_t fbw = ((static_cast<uint32_t>(w) + 63u) >> 6) & 0x3FU;
        sceGszbufaddr(rdram, ctx, runtime);
        int32_t zbuf = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 2) & 0xFFFF));

        GsDrawEnv1Mem env{};
        seedGsDrawEnv1(env,
                       w,
                       h,
                       0u,
                       fbw,
                       psm,
                       static_cast<uint32_t>(zbuf),
                       param_6 & 0xFu,
                       param_5 & 0x3u,
                       (param_2 & 2u) != 0u);

        uint8_t *const ptr = getMemPtr(rdram, envAddr);
        if (!ptr)
        {
            setReturnS32(ctx, 8);
            return;
        }
        std::memcpy(ptr, &env, sizeof(env));

        setReturnS32(ctx, 8);
    }

    void sceGsSetDefDrawEnv2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t param_2 = getRegU32(ctx, 5);
        int32_t w = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFF));
        int32_t h = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFF));
        const GsTrailingArgs2 trailing = decodeGsTrailingArgs2(rdram, ctx);
        uint32_t param_5 = trailing.arg0;
        uint32_t param_6 = trailing.arg1;

        if (w <= 0)
            w = 640;
        if (h <= 0)
            h = 448;

        uint32_t psm = param_2 & 0xFU;
        uint32_t fbw = ((static_cast<uint32_t>(w) + 63u) >> 6) & 0x3FU;
        sceGszbufaddr(rdram, ctx, runtime);
        int32_t zbuf = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 2) & 0xFFFF));

        GsDrawEnv2Mem env{};
        seedGsDrawEnv2(env,
                       w,
                       h,
                       0u,
                       fbw,
                       psm,
                       static_cast<uint32_t>(zbuf),
                       param_6 & 0xFu,
                       param_5 & 0x3u,
                       (param_2 & 2u) != 0u);

        uint8_t *const ptr = getMemPtr(rdram, envAddr);
        if (!ptr)
        {
            setReturnS32(ctx, 8);
            return;
        }

        std::memcpy(ptr, &env, sizeof(env));
        setReturnS32(ctx, 8);
    }

    void sceGsSetDefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        const GsSetDefImageArgs args = decodeGsSetDefImageArgs(rdram, ctx);

        GsImageMem img{};
        img.x = static_cast<uint16_t>(args.x);
        img.y = static_cast<uint16_t>(args.y);
        img.width = static_cast<uint16_t>(args.width);
        img.height = static_cast<uint16_t>(args.height);
        img.vram_addr = static_cast<uint16_t>(args.vramAddr);
        img.vram_width = static_cast<uint8_t>(args.vramWidth);
        img.psm = static_cast<uint8_t>(args.psm);

        writeGsImage(rdram, imgAddr, img);
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceGsSetDefLoadImage(rdram, ctx, runtime);
    }

    void sceGsSwapDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        const uint32_t which = getRegU32(ctx, 5) & 1u;

        GsDBuffDcMem db{};
        if (!runtime || !readGsDBuffDc(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }

        applyGsDispEnv(runtime, db.disp[which]);
        static uint32_t s_swapDbuffLogCount = 0u;
        if (s_swapDbuffLogCount < 32u)
        {
            const uint32_t dispFbp = static_cast<uint32_t>(db.disp[which].dispfb & 0x1FFu);
            const uint32_t clearContext = (which == 0u)
                                              ? static_cast<uint32_t>((db.clear0.prim.value >> 9) & 0x1u)
                                              : static_cast<uint32_t>((db.clear1.prim.value >> 9) & 0x1u);
            PS2_IF_AGRESSIVE_LOGS({
                RUNTIME_LOG("[gs:swapdbuff] which=" << which
                                                    << " env=0x" << std::hex << envAddr
                                                    << " dispfb=0x" << db.disp[which].dispfb
                                                    << " display=0x" << db.disp[which].display
                                                    << " pmode=0x" << db.disp[which].pmode
                                                    << " dispFbp=" << dispFbp
                                                    << " clearCtxt=" << clearContext
                                                    << std::dec << std::endl);
            });
            ++s_swapDbuffLogCount;
        }
        if (which == 0u)
        {
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw01), 8u);
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw02), 8u);
            if (hasSeededGsClearPacket(db.clear0))
            {
                const uint32_t clearContext = static_cast<uint32_t>((db.clear0.prim.value >> 9) & 0x1u);
                runtime->gs().clearFramebufferContext(clearContext, static_cast<uint32_t>(db.clear0.rgbaq.value));
            }
            applyGsClearPacket(runtime, db.clear0);
        }
        else
        {
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw11), 8u);
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw12), 8u);
            if (hasSeededGsClearPacket(db.clear1))
            {
                const uint32_t clearContext = static_cast<uint32_t>((db.clear1.prim.value >> 9) & 0x1u);
                runtime->gs().clearFramebufferContext(clearContext, static_cast<uint32_t>(db.clear1.rgbaq.value));
            }
            applyGsClearPacket(runtime, db.clear1);
        }

        setReturnS32(ctx, static_cast<int32_t>(which ^ 1u));
    }

    void sceGsSwapDBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        const uint32_t which = getRegU32(ctx, 5) & 1u;

        GsDBuffMem db{};
        if (!runtime || !readGsDBuff(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }

        applyGsDispEnv(runtime, db.disp[which]);
        if (which == 0u)
        {
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw0), 8u);
        }
        else
        {
            applyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw1), 8u);
        }

        setReturnS32(ctx, static_cast<int32_t>(which ^ 1u));
    }

    void sceGsSyncPath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int32_t mode = static_cast<int32_t>(getRegU32(ctx, 4));
        auto &mem = runtime->memory();

        if (mode == 0)
        {
            mem.processPendingTransfers();

            uint32_t count = 0;
            constexpr uint32_t kTimeout = 0x1000000;

            while ((mem.readIORegister(0x10009000) & 0x100) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x1000A000) & 0x100) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x10003C00) & 0x1F000003) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x10003020) & 0xC00) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            setReturnS32(ctx, 0);
        }
        else
        {
            uint32_t result = 0;

            if ((mem.readIORegister(0x10009000) & 0x100) != 0)
                result |= 1;
            if ((mem.readIORegister(0x1000A000) & 0x100) != 0)
                result |= 2;
            if ((mem.readIORegister(0x10003C00) & 0x1F000003) != 0)
                result |= 4;
            if ((mem.readIORegister(0x10003020) & 0xC00) != 0)
                result |= 0x10;

            setReturnS32(ctx, result);
        }
    }

    void sceGsSyncV(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::WaitVSyncTick(rdram,
                                    ctx,
                                    runtime,
                                    g_gparam.interlace != 0u ? -1 : 1);
    }

    void sceGsSyncVCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newCallback = getRegU32(ctx, 4);
        const uint32_t callerPc = ctx ? ctx->pc : 0u;
        const uint32_t callerRa = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t sp = getRegU32(ctx, 29);

        EeScheduler &ee = runtime->eeScheduler();
        ee.bindMainContextForSyscall(*ctx, rdram);
        const uint32_t oldCallback = ee.setGsVSyncCallback(newCallback, gp, sp);

        static uint32_t s_syncVCallbackLogCount = 0u;
        if (s_syncVCallbackLogCount < 128u)
        {
            PS2_IF_AGRESSIVE_LOGS({
                RUNTIME_LOG("[sceGsSyncVCallback:set] new=0x" << std::hex << newCallback
                                                              << " old=0x" << oldCallback
                                                              << " callerPc=0x" << callerPc
                                                              << " callerRa=0x" << callerRa
                                                              << " gp=0x" << gp
                                                              << " sp=0x" << sp
                                                              << std::dec << std::endl);
            });
            ++s_syncVCallbackLogCount;
        }

        setReturnU32(ctx, oldCallback);
    }

    void sceGszbufaddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        uint32_t param_1 = getRegU32(ctx, 4);
        int32_t w = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFF));
        int32_t h = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFF));

        int32_t width_blocks = (w + 63) >> 6;
        if (w + 63 < 0)
            width_blocks = (w + 126) >> 6;

        int32_t height_blocks;
        if ((param_1 & 2) != 0)
        {
            int32_t v = (h + 63) >> 6;
            if (h + 63 < 0)
                v = (h + 126) >> 6;
            height_blocks = v;
        }
        else
        {
            int32_t v = (h + 31) >> 5;
            if (h + 31 < 0)
                v = (h + 62) >> 5;
            height_blocks = v;
        }

        int32_t product = width_blocks * height_blocks;

        // libgraph tests gparam {interlace, ffmode} == {1, FIELD} (single full-height buffer) else doubles. This used
        // to read the gparam copy that sceGsResetGraph wrote into guest scratchpad 0x70000100 - i.e. whatever the
        // game keeps there (Transformers: its TEX1_1 shadow), and the uint8 copy could never match the short-wide
        // test anyway. Use the HLE gparam state instead (same predicate as gsDefZbufPages / sceGsSetDefDBuff).
        (void)runtime;
        if (gsDefIsSingleBuffered())
            product = (product * 0x10000) >> 16;
        else
            product = (product * 0x20000) >> 16;

        setReturnS32(ctx, product);
    }

    void Ps2SwapDBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int logCount = 0;
        if (logCount < 8)
        {
            RUNTIME_LOG("ps2_stub Ps2SwapDBuff");
            ++logCount;
        }
        setReturnS32(ctx, 0);
    }

    void sceVif1PkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint64_t dataValue = GPR_U64(ctx, 6);
        const uint32_t words[4] = {
            static_cast<uint32_t>(dataValue & 0xFFFFFFFFu),
            static_cast<uint32_t>(dataValue >> 32u),
            getRegU32(ctx, 5),
            0u,
        };
        writeGuestBytes(rdram,
                        runtime,
                        currentAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
    }

    void sceVif1PkAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        alignPacketBuilderState(rdram,
                                runtime,
                                getRegU32(ctx, 4),
                                getRegU32(ctx, 5),
                                getRegU32(ctx, 6));
    }

    void sceVif1PkCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t refAddr = getRegU32(ctx, 5) & 0x9FFFFFFFu;
        const uint32_t tagWord = getRegU32(ctx, 6) | 0x50000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, refAddr};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
    }

    void sceVif1PkCloseDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        uint32_t openAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
            !tryReadWordFromGuest(rdram, runtime, stateAddr + 12u, openAddr) ||
            openAddr == 0u)
        {
            return;
        }

        const uint32_t currentMinusTag = currentAddr - 4u;
        const uint32_t wordCount = (currentMinusTag - openAddr) >> 2u;
        const uint32_t qwordCount = wordCount >> 2u;
        uint32_t tagWord = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, openAddr, tagWord))
        {
            return;
        }

        tagWord += qwordCount;
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writeGuestU32(rdram, runtime, openAddr, tagWord);
    }

    void sceVif1PkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        closePacketGifTag(rdram, runtime, getRegU32(ctx, 4), 20u);
    }

    void sceVif1PkCnt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t tagWord = getRegU32(ctx, 5) | 0x10000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, 0u};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
    }

    void sceVif1PkEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t tagWord = getRegU32(ctx, 5) | 0x70000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, 0u};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
    }

    void sceVif1PkInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        initPacketBuilderState(rdram, ctx, runtime);
        writeGuestU32(rdram, runtime, getRegU32(ctx, 4) + 20u, 0u);
    }

    void sceVif1PkOpenDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        alignPacketBuilderState(rdram, runtime, stateAddr, 2u, 3u);

        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint32_t tagWord = (getRegU32(ctx, 5) != 0u) ? 0xD0000000u : 0x50000000u;
        writeGuestU32(rdram, runtime, currentAddr, tagWord);
        setPacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 4u);
        writeGuestU32(rdram, runtime, stateAddr + 12u, currentAddr);
    }

    void sceVif1PkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        openPacketGifTag(rdram, ctx, runtime, getRegU32(ctx, 4), 20u);
    }

    void sceVif1PkReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resetPacketBuilderState(rdram, ctx, runtime);
        writeGuestU32(rdram, runtime, getRegU32(ctx, 4) + 20u, 0u);
    }

    void sceVif1PkReserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t wordCount = getRegU32(ctx, 5);
        uint32_t currentAddr = 0u;
        tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr);
        const uint32_t reservedAddr = reservePacketBuilderWords(rdram, runtime, stateAddr, wordCount);
        setReturnU32(ctx, reservedAddr);
    }

    void sceVif1PkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, terminatePacketBuilderState(rdram, ctx, runtime));
    }
}
