#include "Common.h"
#include "LibC.h"
#include "ps2_log.h"

namespace ps2_stubs
{
    namespace
    {
        uint32_t sanitizeMemTransferSize(uint32_t size, const char *op)
        {
            constexpr uint32_t kMaxTransfer = PS2_RAM_SIZE;
            if (size <= kMaxTransfer)
            {
                return size;
            }

            static std::mutex s_warnMutex;
            static std::unordered_map<std::string, uint32_t> s_warnCounts;
            uint32_t warnCount = 0u;
            {
                std::lock_guard<std::mutex> lock(s_warnMutex);
                warnCount = ++s_warnCounts[op ? op : "memop"];
            }
            if (warnCount <= 16u)
            {
                std::cerr << "[" << (op ? op : "memop") << "] size clamp from 0x"
                          << std::hex << size << " to 0x" << kMaxTransfer
                          << std::dec << std::endl;
            }
            return kMaxTransfer;
        }

        uint32_t guestContiguousBytes(uint32_t guestAddr)
        {
            uint32_t offset = 0u;
            bool scratch = false;
            if (!ps2ResolveGuestPointer(guestAddr, offset, scratch))
            {
                return 0u;
            }
            if (scratch)
            {
                return (offset < PS2_SCRATCHPAD_SIZE) ? (PS2_SCRATCHPAD_SIZE - offset) : 0u;
            }
            return (offset < PS2_RAM_SIZE) ? (PS2_RAM_SIZE - offset) : 0u;
        }

        // GENERIC(runtime) warpcrash: getMemPtr()/ps2ResolveGuestPointer() fold every physical address at or above
        // 32 MB back into RDRAM (phys & PS2_RAM_MASK), so a libc stub called on VU memory wrote MAIN RAM instead:
        // Transformers' loading screen (0x26EEC0) does memset(0x1100C000 = VU1 data, 0, n) through the memset stub
        // 0x110340, which zeroed RDRAM 0x0100C000.. and with it live scene nodes there; tearing the level down after
        // the load started then made a virtual call through a zeroed vtable (null JALR at 0x2139a4 in sub_002137D0,
        // playtests #1 and #2). Ranges that touch a special (non-RAM, non-scratchpad) address now go byte by byte
        // through PS2Runtime::Load8/Store8, i.e. PS2Memory's VU/IO mapping, exactly as the recompiled code's
        // READ/WRITE macros do. Plain RAM/scratchpad ranges keep the fast path. PS2X_LIBC_SPECIAL_FIX=0 = old path.
        bool libcSpecialFixEnabled()
        {
            static const bool s_on = []()
            {
                const char *e = std::getenv("PS2X_LIBC_SPECIAL_FIX");
                return !(e && e[0] == '0');
            }();
            return s_on;
        }

        bool rangeTouchesSpecial(uint32_t addr, uint32_t size)
        {
            if (size == 0u)
            {
                return false;
            }
            const uint64_t end = static_cast<uint64_t>(addr) + size;
            if (end <= PS2_RAM_SIZE) // the common case: plain RAM below 32 MB
            {
                return false;
            }
            if (!libcSpecialFixEnabled())
            {
                return false;
            }
            for (uint64_t cur = addr; cur < end; cur = (cur | 0xFFFull) + 1ull) // special regions are 4 KB aligned
            {
                const uint32_t a = static_cast<uint32_t>(cur);
                if (PS2Runtime::isSpecialAddress(a) && !ps2IsScratchpadAddress(a))
                {
                    return true;
                }
            }
            return false;
        }

    }

    void malloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t size = getRegU32(ctx, 4); // $a0
        const uint32_t guestAddr = runtime ? runtime->guestMalloc(size) : 0u;
        setReturnU32(ctx, guestAddr);
    }

    void memalign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t alignment = getRegU32(ctx, 4); // $a0
        const uint32_t size = getRegU32(ctx, 5);      // $a1
        const uint32_t guestAddr = runtime ? runtime->guestMalloc(size, alignment) : 0u;
        setReturnU32(ctx, guestAddr);
    }

    void free(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t guestAddr = getRegU32(ctx, 4); // $a0
        if (runtime && guestAddr != 0u)
        {
            runtime->guestFree(guestAddr);
        }
    }

    void calloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t count = getRegU32(ctx, 4); // $a0
        const uint32_t size = getRegU32(ctx, 5);  // $a1
        const uint32_t guestAddr = runtime ? runtime->guestCalloc(count, size) : 0u;
        setReturnU32(ctx, guestAddr);
    }

    void realloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t oldGuestAddr = getRegU32(ctx, 4); // $a0
        const uint32_t newSize = getRegU32(ctx, 5);      // $a1
        const uint32_t newGuestAddr = runtime ? runtime->guestRealloc(oldGuestAddr, newSize) : 0u;
        setReturnU32(ctx, newGuestAddr);
    }

    void memcpy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t destAddr = getRegU32(ctx, 4); // $a0
        uint32_t srcAddr = getRegU32(ctx, 5);  // $a1
        uint32_t size = getRegU32(ctx, 6);     // $a2
        size = sanitizeMemTransferSize(size, "memcpy");

        if (runtime && (rangeTouchesSpecial(destAddr, size) || rangeTouchesSpecial(srcAddr, size)))
        {
            for (uint32_t i = 0u; i < size; ++i)
            {
                runtime->Store8(rdram, ctx, destAddr + i, runtime->Load8(rdram, ctx, srcAddr + i));
            }
            ctx->r[2] = ctx->r[4];
            return;
        }

        uint32_t copied = 0u;
        uint32_t curDst = destAddr;
        uint32_t curSrc = srcAddr;
        while (copied < size)
        {
            uint8_t *hostDest = getMemPtr(rdram, curDst);
            const uint8_t *hostSrc = getConstMemPtr(rdram, curSrc);
            if (!hostDest || !hostSrc)
            {
                break;
            }

            uint32_t chunk = size - copied;
            chunk = std::min(chunk, guestContiguousBytes(curDst));
            chunk = std::min(chunk, guestContiguousBytes(curSrc));
            if (chunk == 0u)
            {
                break;
            }

            ::memcpy(hostDest, hostSrc, chunk);
            copied += chunk;
            curDst += chunk;
            curSrc += chunk;
        }

        if (copied != 0u)
        {
            ps2TraceGuestRangeWrite(rdram, destAddr, copied, "memcpy", ctx);
        }

        // returns dest pointer ($v0 = $a0)
        ctx->r[2] = ctx->r[4];
    }

    void memset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t destAddr = getRegU32(ctx, 4);       // $a0
        int value = (int)(getRegU32(ctx, 5) & 0xFF); // $a1 (char value)
        uint32_t size = getRegU32(ctx, 6);           // $a2
        size = sanitizeMemTransferSize(size, "memset");

        if (runtime && rangeTouchesSpecial(destAddr, size))
        {
            for (uint32_t i = 0u; i < size; ++i)
            {
                runtime->Store8(rdram, ctx, destAddr + i, static_cast<uint8_t>(value));
            }
            ctx->r[2] = ctx->r[4];
            return;
        }

        uint32_t written = 0u;
        uint32_t curDst = destAddr;
        while (written < size)
        {
            uint8_t *hostDest = getMemPtr(rdram, curDst);
            if (!hostDest)
            {
                break;
            }

            uint32_t chunk = size - written;
            chunk = std::min(chunk, guestContiguousBytes(curDst));
            if (chunk == 0u)
            {
                break;
            }

            ::memset(hostDest, value, chunk);
            written += chunk;
            curDst += chunk;
        }

        if (written != 0u)
        {
            ps2TraceGuestRangeWrite(rdram, destAddr, written, "memset", ctx);
        }

        // returns dest pointer ($v0 = $a0)
        ctx->r[2] = ctx->r[4];
    }

    void memclr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t destAddr = getRegU32(ctx, 4); // $a0
        uint32_t size = getRegU32(ctx, 5);     // $a1
        size = sanitizeMemTransferSize(size, "memclr");

        if (runtime && rangeTouchesSpecial(destAddr, size))
        {
            for (uint32_t i = 0u; i < size; ++i)
            {
                runtime->Store8(rdram, ctx, destAddr + i, 0u);
            }
            ctx->r[2] = ctx->r[4];
            return;
        }

        uint32_t written = 0u;
        uint32_t curDst = destAddr;
        while (written < size)
        {
            uint8_t *hostDest = getMemPtr(rdram, curDst);
            if (!hostDest)
            {
                break;
            }

            uint32_t chunk = size - written;
            chunk = std::min(chunk, guestContiguousBytes(curDst));
            if (chunk == 0u)
            {
                break;
            }

            ::memset(hostDest, 0, chunk);
            written += chunk;
            curDst += chunk;
        }

        if (written != 0u)
        {
            ps2TraceGuestRangeWrite(rdram, destAddr, written, "memclr", ctx);
        }

        ctx->r[2] = ctx->r[4];
    }

    void memmove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t destAddr = getRegU32(ctx, 4); // $a0
        uint32_t srcAddr = getRegU32(ctx, 5);  // $a1
        uint32_t size = getRegU32(ctx, 6);     // $a2
        size = sanitizeMemTransferSize(size, "memmove");

        if (runtime && (rangeTouchesSpecial(destAddr, size) || rangeTouchesSpecial(srcAddr, size)))
        {
            std::vector<uint8_t> bytes(size);
            for (uint32_t i = 0u; i < size; ++i)
            {
                bytes[i] = runtime->Load8(rdram, ctx, srcAddr + i);
            }
            for (uint32_t i = 0u; i < size; ++i)
            {
                runtime->Store8(rdram, ctx, destAddr + i, bytes[i]);
            }
            ctx->r[2] = ctx->r[4];
            return;
        }

        uint32_t copied = 0u;
        std::vector<uint8_t> tmp;
        tmp.reserve(size);
        for (uint32_t i = 0u; i < size; ++i)
        {
            const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
            if (!src)
            {
                break;
            }
            tmp.push_back(*src);
        }

        for (uint32_t i = 0u; i < static_cast<uint32_t>(tmp.size()); ++i)
        {
            uint8_t *dst = getMemPtr(rdram, destAddr + i);
            if (!dst)
            {
                break;
            }
            *dst = tmp[i];
            ++copied;
        }

        if (copied != 0u)
        {
            ps2TraceGuestRangeWrite(rdram, destAddr, copied, "memmove", ctx);
        }

        // returns dest pointer ($v0 = $a0)
        ctx->r[2] = ctx->r[4];
    }

    void memcmp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t ptr1Addr = getRegU32(ctx, 4); // $a0
        uint32_t ptr2Addr = getRegU32(ctx, 5); // $a1
        uint32_t size = getRegU32(ctx, 6);     // $a2
        size = sanitizeMemTransferSize(size, "memcmp");
        int result = 0;

        if (runtime && (rangeTouchesSpecial(ptr1Addr, size) || rangeTouchesSpecial(ptr2Addr, size)))
        {
            for (uint32_t i = 0u; i < size; ++i)
            {
                const uint8_t lhs = runtime->Load8(rdram, ctx, ptr1Addr + i);
                const uint8_t rhs = runtime->Load8(rdram, ctx, ptr2Addr + i);
                if (lhs != rhs)
                {
                    result = static_cast<int>(lhs) - static_cast<int>(rhs);
                    break;
                }
            }
            setReturnS32(ctx, result);
            return;
        }

        for (uint32_t i = 0u; i < size; ++i)
        {
            const uint8_t *lhs = getConstMemPtr(rdram, ptr1Addr + i);
            const uint8_t *rhs = getConstMemPtr(rdram, ptr2Addr + i);
            if (!lhs || !rhs)
            {
                result = (!lhs && !rhs) ? 0 : (lhs ? 1 : -1);
                break;
            }
            if (*lhs != *rhs)
            {
                result = static_cast<int>(*lhs) - static_cast<int>(*rhs);
                break;
            }
        }
        setReturnS32(ctx, result);
    }

    void strcpy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t destAddr = getRegU32(ctx, 4); // $a0
        uint32_t srcAddr = getRegU32(ctx, 5);  // $a1

        char *hostDest = reinterpret_cast<char *>(getMemPtr(rdram, destAddr));
        const char *hostSrc = reinterpret_cast<const char *>(getConstMemPtr(rdram, srcAddr));

        if (hostDest && hostSrc)
        {
            ::strcpy(hostDest, hostSrc);
            ps2TraceGuestRangeWrite(rdram, destAddr, static_cast<uint32_t>(::strlen(hostSrc) + 1u), "strcpy", ctx);
        }
        else
        {
            std::cerr << "strcpy error: Invalid address provided."
                      << " Dest: 0x" << std::hex << destAddr << " (host ptr valid: " << (hostDest != nullptr) << ")"
                      << ", Src: 0x" << srcAddr << " (host ptr valid: " << (hostSrc != nullptr) << ")" << std::dec
                      << std::endl;
        }

        // returns dest pointer ($v0 = $a0)
        ctx->r[2] = ctx->r[4];
    }

    void strncpy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t destAddr = getRegU32(ctx, 4); // $a0
        uint32_t srcAddr = getRegU32(ctx, 5);  // $a1
        uint32_t size = getRegU32(ctx, 6);     // $a2

        char *hostDest = reinterpret_cast<char *>(getMemPtr(rdram, destAddr));
        const char *hostSrc = reinterpret_cast<const char *>(getConstMemPtr(rdram, srcAddr));

        if (hostDest && hostSrc)
        {
            ::strncpy(hostDest, hostSrc, size);
            ps2TraceGuestRangeWrite(rdram, destAddr, size, "strncpy", ctx);
        }
        else
        {
            std::cerr << "strncpy error: Invalid address provided."
                      << " Dest: 0x" << std::hex << destAddr << " (host ptr valid: " << (hostDest != nullptr) << ")"
                      << ", Src: 0x" << srcAddr << " (host ptr valid: " << (hostSrc != nullptr) << ")" << std::dec
                      << std::endl;
        }
        // returns dest pointer ($v0 = $a0)
        ctx->r[2] = ctx->r[4];
    }

    void strlen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t strAddr = getRegU32(ctx, 4); // $a0
        const char *hostStr = reinterpret_cast<const char *>(getConstMemPtr(rdram, strAddr));
        size_t len = 0;

        if (hostStr)
        {
            len = ::strlen(hostStr);
        }
        else
        {
            std::cerr << "strlen error: Invalid address provided: 0x" << std::hex << strAddr << std::dec << std::endl;
        }
        setReturnU32(ctx, (uint32_t)len);
    }

    void strcmp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t str1Addr = getRegU32(ctx, 4); // $a0
        uint32_t str2Addr = getRegU32(ctx, 5); // $a1

        const char *hostStr1 = reinterpret_cast<const char *>(getConstMemPtr(rdram, str1Addr));
        const char *hostStr2 = reinterpret_cast<const char *>(getConstMemPtr(rdram, str2Addr));
        int result = 0;

        if (hostStr1 && hostStr2)
        {
            result = ::strcmp(hostStr1, hostStr2);
        }
        else
        {
            std::cerr << "strcmp error: Invalid address provided."
                      << " Str1: 0x" << std::hex << str1Addr << " (host ptr valid: " << (hostStr1 != nullptr) << ")"
                      << ", Str2: 0x" << str2Addr << " (host ptr valid: " << (hostStr2 != nullptr) << ")" << std::dec
                      << std::endl;
            // Return non-zero on error, consistent with memcmp error handling
            result = (hostStr1 == nullptr) - (hostStr2 == nullptr);
            if (result == 0 && hostStr1 == nullptr)
                result = 1; // Both null -> treat as different? Or 0? Let's say different.
        }
        setReturnS32(ctx, result);
    }

    void strncmp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t str1Addr = getRegU32(ctx, 4); // $a0
        uint32_t str2Addr = getRegU32(ctx, 5); // $a1
        uint32_t size = getRegU32(ctx, 6);     // $a2

        const char *hostStr1 = reinterpret_cast<const char *>(getConstMemPtr(rdram, str1Addr));
        const char *hostStr2 = reinterpret_cast<const char *>(getConstMemPtr(rdram, str2Addr));
        int result = 0;

        if (hostStr1 && hostStr2)
        {
            result = ::strncmp(hostStr1, hostStr2, size);
        }
        else
        {
            std::cerr << "strncmp error: Invalid address provided."
                      << " Str1: 0x" << std::hex << str1Addr << " (host ptr valid: " << (hostStr1 != nullptr) << ")"
                      << ", Str2: 0x" << str2Addr << " (host ptr valid: " << (hostStr2 != nullptr) << ")" << std::dec
                      << std::endl;
            result = (hostStr1 == nullptr) - (hostStr2 == nullptr);
            if (result == 0 && hostStr1 == nullptr)
                result = 1; // Both null -> different
        }
        setReturnS32(ctx, result);
    }

    void strcat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t destAddr = getRegU32(ctx, 4); // $a0
        uint32_t srcAddr = getRegU32(ctx, 5);  // $a1

        char *hostDest = reinterpret_cast<char *>(getMemPtr(rdram, destAddr));
        const char *hostSrc = reinterpret_cast<const char *>(getConstMemPtr(rdram, srcAddr));

        if (hostDest && hostSrc)
        {
            ::strcat(hostDest, hostSrc);
        }
        else
        {
            std::cerr << "strcat error: Invalid address provided."
                      << " Dest: 0x" << std::hex << destAddr << " (host ptr valid: " << (hostDest != nullptr) << ")"
                      << ", Src: 0x" << srcAddr << " (host ptr valid: " << (hostSrc != nullptr) << ")" << std::dec
                      << std::endl;
        }

        // returns dest pointer ($v0 = $a0)
        ctx->r[2] = ctx->r[4];
    }

    void strncat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t destAddr = getRegU32(ctx, 4); // $a0
        uint32_t srcAddr = getRegU32(ctx, 5);  // $a1
        uint32_t size = getRegU32(ctx, 6);     // $a2

        char *hostDest = reinterpret_cast<char *>(getMemPtr(rdram, destAddr));
        const char *hostSrc = reinterpret_cast<const char *>(getConstMemPtr(rdram, srcAddr));

        if (hostDest && hostSrc)
        {
            ::strncat(hostDest, hostSrc, size);
        }
        else
        {
            std::cerr << "strncat error: Invalid address provided."
                      << " Dest: 0x" << std::hex << destAddr << " (host ptr valid: " << (hostDest != nullptr) << ")"
                      << ", Src: 0x" << srcAddr << " (host ptr valid: " << (hostSrc != nullptr) << ")" << std::dec
                      << std::endl;
        }

        // returns dest pointer ($v0 = $a0)
        ctx->r[2] = ctx->r[4];
    }

    void strchr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t strAddr = getRegU32(ctx, 4);            // $a0
        int char_code = (int)(getRegU32(ctx, 5) & 0xFF); // $a1 (char value)

        const char *hostStr = reinterpret_cast<const char *>(getConstMemPtr(rdram, strAddr));
        char *foundPtr = nullptr;
        uint32_t resultAddr = 0;

        if (hostStr)
        {
            foundPtr = ::strchr(const_cast<char *>(hostStr), char_code);
            if (foundPtr)
            {
                // Same segment as the argument (newlib returns s + i): hostPtrToPs2Addr dropped KSEG/uncached bits
                // and returned NULL for scratchpad strings.
                resultAddr = strAddr + static_cast<uint32_t>(foundPtr - hostStr);
            }
        }
        else
        {
            std::cerr << "strchr error: Invalid address provided: 0x" << std::hex << strAddr << std::dec << std::endl;
        }

        // returns PS2 address or 0 (NULL)
        setReturnU32(ctx, resultAddr);
    }

    void strrchr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t strAddr = getRegU32(ctx, 4);            // $a0
        int char_code = (int)(getRegU32(ctx, 5) & 0xFF); // $a1 (char value)

        const char *hostStr = reinterpret_cast<const char *>(getConstMemPtr(rdram, strAddr));
        char *foundPtr = nullptr;
        uint32_t resultAddr = 0;

        if (hostStr)
        {
            foundPtr = ::strrchr(const_cast<char *>(hostStr), char_code); // Use const_cast carefully
            if (foundPtr)
            {
                resultAddr = strAddr + static_cast<uint32_t>(foundPtr - hostStr);
            }
        }
        else
        {
            std::cerr << "strrchr error: Invalid address provided: 0x" << std::hex << strAddr << std::dec << std::endl;
        }

        // returns PS2 address or 0 (NULL)
        setReturnU32(ctx, resultAddr);
    }

    void strstr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t haystackAddr = getRegU32(ctx, 4); // $a0
        uint32_t needleAddr = getRegU32(ctx, 5);   // $a1

        const char *hostHaystack = reinterpret_cast<const char *>(getConstMemPtr(rdram, haystackAddr));
        const char *hostNeedle = reinterpret_cast<const char *>(getConstMemPtr(rdram, needleAddr));
        char *foundPtr = nullptr;
        uint32_t resultAddr = 0;

        if (hostHaystack && hostNeedle)
        {
            foundPtr = ::strstr(const_cast<char *>(hostHaystack), hostNeedle);
            if (foundPtr)
            {
                resultAddr = haystackAddr + static_cast<uint32_t>(foundPtr - hostHaystack);
            }
        }
        else
        {
            std::cerr << "strstr error: Invalid address provided."
                      << " Haystack: 0x" << std::hex << haystackAddr << " (host ptr valid: " << (hostHaystack != nullptr) << ")"
                      << ", Needle: 0x" << needleAddr << " (host ptr valid: " << (hostNeedle != nullptr) << ")" << std::dec
                      << std::endl;
        }

        // returns PS2 address or 0 (NULL)
        setReturnU32(ctx, resultAddr);
    }

    void printf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t format_addr = getRegU32(ctx, 4); // $a0
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        int ret = -1;

        if (format_addr != 0)
        {
            std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 1);
            if (rendered.size() > 2048)
            {
                rendered.resize(2048);
            }
            PS2_IF_AGRESSIVE_LOGS({
                const std::string logLine = sanitizeForLog(rendered);
                uint32_t count = 0;
                {
                    std::lock_guard<std::mutex> lock(g_printfLogMutex);
                    count = ++g_printfLogCount;
                }
                if (count <= kMaxPrintfLogs)
                {
                    RUNTIME_LOG("PS2 printf: " << logLine);
                    RUNTIME_LOG(std::flush);
                }
                else if (count == kMaxPrintfLogs + 1)
                {
                    std::cerr << "PS2 printf logging suppressed after " << kMaxPrintfLogs << " lines" << std::endl;
                }
            });
            ret = static_cast<int>(rendered.size());
        }
        else
        {
            std::cerr << "printf error: Invalid format string address provided: 0x" << std::hex << format_addr << std::dec << std::endl;
        }

        // returns the number of characters written, or negative on error.
        setReturnS32(ctx, ret);
    }

    void sprintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t str_addr = getRegU32(ctx, 4);     // $a0
        uint32_t format_addr = getRegU32(ctx, 5);  // $a1
        // newlib sprintf has no length limit. The old 256-byte cap guarded against garbage output from the O32
        // vararg cursor (fixed); a 255-char cut silently changed legitimate long strings and the return value.
        constexpr size_t kSafeSprintfBytes = kMaxFormattedOutputBytes;

        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        int ret = -1;

        if (format_addr != 0)
        {
            const uint32_t watchBase = ps2PathWatchPhysAddr();
            const uint32_t watchEnd = watchBase + PS2_PATH_WATCH_BYTES;
            const uint32_t dest = str_addr & PS2_RAM_MASK;
            const bool touchesWatch = dest < watchEnd && dest >= watchBase;
            static uint32_t watchSprintfLogCount = 0;
            if (touchesWatch && watchSprintfLogCount < 64u)
            {
                const uint32_t arg0 = getRegU32(ctx, 6);
                const uint32_t arg1 = getRegU32(ctx, 7);
                RUNTIME_LOG("[watch:sprintf] dest=0x" << std::hex << str_addr
                                                      << " fmt@0x" << format_addr
                                                      << " arg0=0x" << arg0
                                                      << " arg1=0x" << arg1
                                                      << " fmt=\"" << sanitizeForLog(readPs2CStringBounded(rdram, runtime, format_addr, 64)) << "\""
                                                      << " s0=\"" << sanitizeForLog(readPs2CStringBounded(rdram, runtime, arg0, 64)) << "\""
                                                      << " s1=\"" << sanitizeForLog(readPs2CStringBounded(rdram, runtime, arg1, 64)) << "\""
                                                      << std::dec << std::endl);
                ++watchSprintfLogCount;
            }

            std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 2);
            if (rendered.size() >= kSafeSprintfBytes)
            {
                rendered.resize(kSafeSprintfBytes - 1);
            }
            const size_t writeLen = rendered.size() + 1u;
            if (writeGuestBytes(rdram, runtime, str_addr, reinterpret_cast<const uint8_t *>(rendered.c_str()), writeLen))
            {
                ps2TraceGuestRangeWrite(rdram, str_addr, static_cast<uint32_t>(writeLen), "sprintf", ctx);
                ret = static_cast<int>(rendered.size());
            }
            else
            {
                std::cerr << "sprintf error: Failed to write destination buffer at 0x"
                          << std::hex << str_addr << std::dec << std::endl;
            }
        }
        else
        {
            std::cerr << "sprintf error: Invalid format address provided."
                      << " Dest: 0x" << std::hex << str_addr
                      << ", Format: 0x" << format_addr << std::dec
                      << std::endl;
        }

        // returns the number of characters written (excluding null), or negative on error.
        setReturnS32(ctx, ret);
    }

    void snprintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t str_addr = getRegU32(ctx, 4);    // $a0
        size_t size = getRegU32(ctx, 5);          // $a1
        uint32_t format_addr = getRegU32(ctx, 6); // $a2
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        int ret = -1;

        if (format_addr != 0)
        {
            std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 3);
            ret = static_cast<int>(rendered.size());

            if (size > 0)
            {
                const size_t copyLen = std::min<size_t>(size - 1, rendered.size());
                std::vector<uint8_t> output(copyLen + 1u, 0u);
                if (copyLen > 0u)
                {
                    std::memcpy(output.data(), rendered.data(), copyLen);
                }
                if (writeGuestBytes(rdram, runtime, str_addr, output.data(), output.size()))
                {
                    ps2TraceGuestRangeWrite(rdram, str_addr, static_cast<uint32_t>(output.size()), "snprintf", ctx);
                }
                else
                {
                    std::cerr << "snprintf error: Failed to write destination buffer at 0x"
                              << std::hex << str_addr << std::dec << std::endl;
                    ret = -1;
                }
            }
        }
        else
        {
            std::cerr << "snprintf error: Invalid address provided or size is zero."
                      << " Dest: 0x" << std::hex << str_addr
                      << ", Format: 0x" << format_addr << std::dec
                      << ", Size: " << size << std::endl;
        }

        // returns the number of characters that *would* have been written
        // if size was large enough (excluding null), or negative on error.
        setReturnS32(ctx, ret);
    }

    void puts(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t strAddr = getRegU32(ctx, 4); // $a0
        const char *hostStr = reinterpret_cast<const char *>(getConstMemPtr(rdram, strAddr));
        int result = EOF;

        if (hostStr)
        {
            result = std::puts(hostStr); // std::puts adds a newline
            std::fflush(stdout);         // Ensure output appears
        }
        else
        {
            std::cerr << "puts error: Invalid address provided: 0x" << std::hex << strAddr << std::dec << std::endl;
        }

        // returns non-negative on success, EOF on error.
        setReturnS32(ctx, result >= 0 ? 0 : -1); // PS2 might expect 0/-1 rather than EOF
    }

    void fopen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        uint32_t modeAddr = getRegU32(ctx, 5); // $a1

        const char *hostPath = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathAddr));
        const char *hostMode = reinterpret_cast<const char *>(getConstMemPtr(rdram, modeAddr));
        uint32_t file_handle = 0;

        if (hostPath && hostMode)
        {
            // TODO: Add translation for PS2 paths like mc0:, host:, cdrom:, etc.
            // treating as direct host path
            RUNTIME_LOG("ps2_stub fopen: path='" << hostPath << "', mode='" << hostMode << "'");
            FILE *fp = ::fopen(hostPath, hostMode);
            if (fp)
            {
                std::lock_guard<std::mutex> lock(g_file_mutex);
                file_handle = generate_file_handle();
                g_file_map[file_handle] = fp;
                RUNTIME_LOG("  -> handle=0x" << std::hex << file_handle << std::dec);
            }
            else
            {
                std::cerr << "ps2_stub fopen error: Failed to open '" << hostPath << "' with mode '" << hostMode << "'. Error: " << strerror(errno) << std::endl;
            }
        }
        else
        {
            std::cerr << "fopen error: Invalid address provided for path or mode."
                      << " Path: 0x" << std::hex << pathAddr << " (host ptr valid: " << (hostPath != nullptr) << ")"
                      << ", Mode: 0x" << modeAddr << " (host ptr valid: " << (hostMode != nullptr) << ")" << std::dec
                      << std::endl;
        }
        // returns a file handle (non-zero) on success, or NULL (0) on error.
        setReturnU32(ctx, file_handle);
    }

    void fclose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t file_handle = getRegU32(ctx, 4); // $a0
        int ret = EOF;                            // Default to error

        if (file_handle != 0)
        {
            std::lock_guard<std::mutex> lock(g_file_mutex);
            auto it = g_file_map.find(file_handle);
            if (it != g_file_map.end())
            {
                FILE *fp = it->second;
                ret = ::fclose(fp);
                g_file_map.erase(it);
            }
            else
            {
                std::cerr << "ps2_stub fclose error: Invalid file handle 0x" << std::hex << file_handle << std::dec << std::endl;
            }
        }
        else
        {
            // Closing NULL handle in Standard C defines this as no-op
            ret = 0;
        }

        // returns 0 on success, EOF on error.
        setReturnS32(ctx, ret);
    }

    void fread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t ptrAddr = getRegU32(ctx, 4);     // $a0 (buffer)
        uint32_t size = getRegU32(ctx, 5);        // $a1 (element size)
        uint32_t count = getRegU32(ctx, 6);       // $a2 (number of elements)
        uint32_t file_handle = getRegU32(ctx, 7); // $a3 (file handle)
        size_t items_read = 0;

        uint8_t *hostPtr = getMemPtr(rdram, ptrAddr);
        FILE *fp = get_file_ptr(file_handle);

        if (hostPtr && fp && size > 0 && count > 0)
        {
            items_read = ::fread(hostPtr, size, count, fp);
        }
        else
        {
            std::cerr << "fread error: Invalid arguments."
                      << " Ptr: 0x" << std::hex << ptrAddr << " (host ptr valid: " << (hostPtr != nullptr) << ")"
                      << ", Handle: 0x" << file_handle << " (file valid: " << (fp != nullptr) << ")" << std::dec
                      << ", Size: " << size << ", Count: " << count << std::endl;
        }
        // returns the number of items successfully read.
        setReturnU32(ctx, (uint32_t)items_read);
    }

    void fwrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t ptrAddr = getRegU32(ctx, 4);     // $a0 (buffer)
        uint32_t size = getRegU32(ctx, 5);        // $a1 (element size)
        uint32_t count = getRegU32(ctx, 6);       // $a2 (number of elements)
        uint32_t file_handle = getRegU32(ctx, 7); // $a3 (file handle)
        size_t items_written = 0;

        const uint8_t *hostPtr = getConstMemPtr(rdram, ptrAddr);
        FILE *fp = get_file_ptr(file_handle);

        if (hostPtr && fp && size > 0 && count > 0)
        {
            items_written = ::fwrite(hostPtr, size, count, fp);
        }
        else
        {
            std::cerr << "fwrite error: Invalid arguments."
                      << " Ptr: 0x" << std::hex << ptrAddr << " (host ptr valid: " << (hostPtr != nullptr) << ")"
                      << ", Handle: 0x" << file_handle << " (file valid: " << (fp != nullptr) << ")" << std::dec
                      << ", Size: " << size << ", Count: " << count << std::endl;
        }
        // returns the number of items successfully written.
        setReturnU32(ctx, (uint32_t)items_written);
    }

    void fprintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t file_handle = getRegU32(ctx, 4); // $a0
        uint32_t format_addr = getRegU32(ctx, 5); // $a1
        FILE *fp = get_file_ptr(file_handle);
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        int ret = -1;

        if (fp && format_addr != 0)
        {
            std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 2);
            ret = std::fprintf(fp, "%s", rendered.c_str());
        }
        else
        {
            std::cerr << "fprintf error: Invalid file handle or format address."
                      << " Handle: 0x" << std::hex << file_handle << " (file valid: " << (fp != nullptr) << ")"
                      << ", Format: 0x" << format_addr << std::dec
                      << std::endl;
        }

        // returns the number of characters written, or negative on error.
        setReturnS32(ctx, ret);
    }

    void fseek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t file_handle = getRegU32(ctx, 4); // $a0
        long offset = (long)getRegU32(ctx, 5);    // $a1 (Note: might need 64-bit for large files?)
        int whence = (int)getRegU32(ctx, 6);      // $a2 (SEEK_SET, SEEK_CUR, SEEK_END)
        int ret = -1;                             // Default error

        FILE *fp = get_file_ptr(file_handle);

        if (fp)
        {
            // Ensure whence is valid (0, 1, 2)
            if (whence >= 0 && whence <= 2)
            {
                ret = ::fseek(fp, offset, whence);
            }
            else
            {
                std::cerr << "fseek error: Invalid whence value: " << whence << std::endl;
            }
        }
        else
        {
            std::cerr << "fseek error: Invalid file handle 0x" << std::hex << file_handle << std::dec << std::endl;
        }

        // returns 0 on success, non-zero on error.
        setReturnS32(ctx, ret);
    }

    void ftell(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t file_handle = getRegU32(ctx, 4); // $a0
        long ret = -1L;

        FILE *fp = get_file_ptr(file_handle);

        if (fp)
        {
            ret = ::ftell(fp);
        }
        else
        {
            std::cerr << "ftell error: Invalid file handle 0x" << std::hex << file_handle << std::dec << std::endl;
        }

        // returns the current position, or -1L on error.
        if (ret > 0xFFFFFFFFL || ret < 0)
        {
            setReturnS32(ctx, -1);
        }
        else
        {
            setReturnU32(ctx, (uint32_t)ret);
        }
    }

    void fflush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t file_handle = getRegU32(ctx, 4); // $a0
        int ret = EOF;                            // Default error

        // If handle is 0 fflush flushes *all* output streams.
        if (file_handle == 0)
        {
            ret = ::fflush(NULL);
        }
        else
        {
            FILE *fp = get_file_ptr(file_handle);
            if (fp)
            {
                ret = ::fflush(fp);
            }
            else
            {
                std::cerr << "fflush error: Invalid file handle 0x" << std::hex << file_handle << std::dec << std::endl;
            }
        }
        // returns 0 on success, EOF on error.
        setReturnS32(ctx, ret);
    }

    void sqrt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::sqrtf(arg);
    }

    void sin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::sinf(arg);
    }

    void __kernel_sinf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const float x = ctx->f[12];
        const float y = ctx->f[13];
        const int32_t iy = static_cast<int32_t>(getRegU32(ctx, 4));
        ctx->f[0] = ::sinf(x + (iy != 0 ? y : 0.0f));
    }

    void cos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::cosf(arg);
    }

    void __kernel_cosf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const float x = ctx->f[12];
        const float y = ctx->f[13];
        ctx->f[0] = ::cosf(x + y);
    }

    void __ieee754_rem_pio2f(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const float x = ctx->f[12];
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kHalfPi = kPi * 0.5f;
        constexpr float kInvHalfPi = 2.0f / kPi;
        const int32_t n = static_cast<int32_t>(std::nearbyintf(x * kInvHalfPi));
        const float y0 = x - (static_cast<float>(n) * kHalfPi);
        const float y1 = 0.0f;

        const uint32_t yOutAddr = getRegU32(ctx, 4);
        if (float *yOut0 = reinterpret_cast<float *>(getMemPtr(rdram, yOutAddr)); yOut0)
        {
            *yOut0 = y0;
        }
        if (float *yOut1 = reinterpret_cast<float *>(getMemPtr(rdram, yOutAddr + 4)); yOut1)
        {
            *yOut1 = y1;
        }

        setReturnS32(ctx, n);
    }

    void tan(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::tanf(arg);
    }

    void atan2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // EE EABI (single-float FPU) passes float args in consecutive FPRs f12, f13, ... (O32 used f12/f14);
        // e.g. game fn 0x1A93A0 (Vector3 set) stores f12/f13/f14 as x/y/z.
        float y = ctx->f[12];
        float x = ctx->f[13];
        ctx->f[0] = ::atan2f(y, x);
    }

    void pow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float base = ctx->f[12];
        float exp = ctx->f[13]; // EABI: 2nd float arg is f13, not O32's f14
        ctx->f[0] = ::powf(base, exp);
    }

    void exp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::expf(arg);
    }

    void log(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::logf(arg);
    }

    void log10(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::log10f(arg);
    }

    void ceil(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::ceilf(arg);
    }

    void floor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::floorf(arg);
    }

    void fabs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float arg = ctx->f[12];
        ctx->f[0] = ::fabsf(arg);
    }
    void abs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t value = static_cast<int32_t>(getRegU32(ctx, 4));
        if (value == std::numeric_limits<int32_t>::min())
        {
            setReturnS32(ctx, std::numeric_limits<int32_t>::max());
            return;
        }
        setReturnS32(ctx, value < 0 ? -value : value);
    }

    void atan(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The float arg is only ever in f12; a0 holds an unrelated value (atan(0.0f) must stay 0).
        const float in = ctx ? ctx->f[12] : 0.0f;
        const float out = std::atan(in);
        if (ctx)
        {
            ctx->f[0] = out;
        }

        uint32_t outRaw = 0u;
        std::memcpy(&outRaw, &out, sizeof(outRaw));
        setReturnU32(ctx, outRaw);
    }

    void memchr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 4);
        const uint8_t needle = static_cast<uint8_t>(getRegU32(ctx, 5) & 0xFFu);
        const uint32_t size = getRegU32(ctx, 6);

        for (uint32_t i = 0; i < size; ++i)
        {
            const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
            if (!src)
            {
                break;
            }
            if (*src == needle)
            {
                setReturnU32(ctx, srcAddr + i);
                return;
            }
        }

        setReturnU32(ctx, 0u);
    }

    namespace
    {
        // newlib's rand state (_reent._new._reent._rand_next), initial value 1.
        std::mutex g_newlibRandMutex;
        uint64_t g_newlibRandNext = 1u;
    }

    void rand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // newlib rand(): 64-bit LCG, RAND_MAX = 0x7FFFFFFF - the game's own copy (SLUS_206.68 0x112020) is
        // next = __muldi3(next, 0x5851F42D4C957F2D) + 1; return (next >> 32) & 0x7FFFFFFF. The host std::rand()
        // & 0x7FFF (MSVC, per-thread state) gave a 2^16 smaller range: e.g. Transformers 0x26F0A0 stores
        // rand() / 432143.2f into VU1 data memory (PS2 range 0..4969, ours 0..0.076). With the same seed (1, the
        // game never calls srand) this reproduces the PS2 sequence exactly.
        std::lock_guard<std::mutex> lock(g_newlibRandMutex);
        g_newlibRandNext = g_newlibRandNext * 0x5851F42D4C957F2Dull + 1u;
        setReturnS32(ctx, static_cast<int32_t>((g_newlibRandNext >> 32) & 0x7FFFFFFFu));
    }

    void srand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // newlib srand stores the seed zero-extended (game 0x112008: dsll32/dsrl32 then sd). Note: a recompiled
        // (non-stubbed) srand writes the guest reent instead and is not seen here.
        std::lock_guard<std::mutex> lock(g_newlibRandMutex);
        g_newlibRandNext = static_cast<uint64_t>(getRegU32(ctx, 4));
    }

    void strcasecmp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t lhsAddr = getRegU32(ctx, 4);
        const uint32_t rhsAddr = getRegU32(ctx, 5);
        const std::string lhs = readPs2CStringBounded(rdram, runtime, lhsAddr, 1024);
        const std::string rhs = readPs2CStringBounded(rdram, runtime, rhsAddr, 1024);

        const size_t n = std::min(lhs.size(), rhs.size());
        for (size_t i = 0; i < n; ++i)
        {
            const int a = std::tolower(static_cast<unsigned char>(lhs[i]));
            const int b = std::tolower(static_cast<unsigned char>(rhs[i]));
            if (a != b)
            {
                setReturnS32(ctx, a - b);
                return;
            }
        }

        setReturnS32(ctx, static_cast<int32_t>(lhs.size()) - static_cast<int32_t>(rhs.size()));
    }

    void vfprintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t file_handle = getRegU32(ctx, 4);  // $a0
        uint32_t format_addr = getRegU32(ctx, 5);  // $a1
        uint32_t va_list_addr = getRegU32(ctx, 6); // $a2
        FILE *fp = get_file_ptr(file_handle);
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        int ret = -1;

        if (fp && format_addr != 0)
        {
            std::string rendered = formatPs2StringWithVaList(rdram, runtime, formatOwned.c_str(), va_list_addr);
            ret = std::fprintf(fp, "%s", rendered.c_str());
        }
        else
        {
            std::cerr << "vfprintf error: Invalid file handle or format address."
                      << " Handle: 0x" << std::hex << file_handle << " (file valid: " << (fp != nullptr) << ")"
                      << ", Format: 0x" << format_addr << std::dec
                      << std::endl;
        }

        setReturnS32(ctx, ret);
    }

    void vsprintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t str_addr = getRegU32(ctx, 4);      // $a0
        uint32_t format_addr = getRegU32(ctx, 5);   // $a1
        uint32_t va_list_addr = getRegU32(ctx, 6);  // $a2
        // newlib vsprintf has no length limit; the 256-byte cap dated from the O32 va_list cursor (fixed), which
        // could render garbage. Keep only a sanity bound.
        constexpr size_t kSafeVsprintfBytes = kMaxFormattedOutputBytes;
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        int ret = -1;

        if (format_addr != 0)
        {
            std::string rendered = formatPs2StringWithVaList(rdram, runtime, formatOwned.c_str(), va_list_addr);
            if (rendered.size() >= kSafeVsprintfBytes)
            {
                rendered.resize(kSafeVsprintfBytes - 1);
            }
            if (writeGuestBytes(rdram, runtime, str_addr, reinterpret_cast<const uint8_t *>(rendered.c_str()), rendered.size() + 1u))
            {
                ret = static_cast<int>(rendered.size());
            }
            else
            {
                std::cerr << "vsprintf error: Failed to write destination buffer at 0x"
                          << std::hex << str_addr << std::dec << std::endl;
            }
        }
        else
        {
            std::cerr << "vsprintf error: Invalid address provided."
                      << " Dest: 0x" << std::hex << str_addr
                      << ", Format: 0x" << format_addr << std::dec
                      << std::endl;
        }

        setReturnS32(ctx, ret);
    }

    void __divdi3(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int64_t num = GPR_S64(ctx, 4);
        const int64_t den = GPR_S64(ctx, 5);
        if (den == 0)
        {
            setReturnU64(ctx, 0u);
            return;
        }
        if (num == std::numeric_limits<int64_t>::min() && den == -1)
        {
            setReturnU64(ctx, static_cast<uint64_t>(num));
            return;
        }
        setReturnU64(ctx, static_cast<uint64_t>(num / den));
    }

}
