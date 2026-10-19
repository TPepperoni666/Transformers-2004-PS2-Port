#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include "runtime/gs/gs_raw_record.h"
#include <cstdlib>
#include <cstring>

namespace
{
    // eetrim: per-thread pool of packet buffers. Every GIF packet (XGKICK/PATH2/PATH3, ~2-3k per game frame) used to
    // heap-allocate a fresh std::vector (zero-filled by resize, then overwritten by memcpy) that drain() freed again.
    // Buffers now go back to this pool after drain() and keep their capacity; assign() copies without the zero fill.
    // Pure host-memory reuse: the bytes handed to the GS are identical. PS2X_GIF_POOL=0 restores per-packet allocation.
    const bool g_gifPacketPool = []
    {
        const char *v = std::getenv("PS2X_GIF_POOL");
        return !(v && v[0] == '0');
    }();
    constexpr size_t kGifPoolMax = 32u;               // buffers kept per thread
    constexpr size_t kGifPoolMaxBytes = 1u << 20;     // do not keep unusually large buffers
    thread_local std::vector<std::vector<uint8_t>> t_gifPacketPool;
}

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg >= 2u; // TEMP(transformers): both IMAGE encodings (GoW 353ac83c)
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    if (g_gifPacketPool && !t_gifPacketPool.empty())
    {
        pkt.data.swap(t_gifPacketPool.back());
        t_gifPacketPool.pop_back();
    }
    pkt.data.assign(data, data + sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            gsraw::PathScope rawPath(static_cast<uint32_t>(pkt.pathId)); // TEMP(transformers)
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    if (g_gifPacketPool)
    {
        for (auto &pkt : m_queue)
        {
            if (t_gifPacketPool.size() >= kGifPoolMax)
                break;
            if (pkt.data.capacity() != 0u && pkt.data.capacity() <= kGifPoolMaxBytes)
                t_gifPacketPool.push_back(std::move(pkt.data));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
