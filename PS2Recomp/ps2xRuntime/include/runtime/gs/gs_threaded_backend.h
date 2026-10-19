#pragma once
#include "runtime/ps2_wake.h"
#include "runtime/ps2_gs_timeline.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_record.h"
#include "../../../src/lib/ps2_perfscope.h" // TEMP(transformers): qualification (GS worker busy/idle, producer waits)
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <cfenv>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#if defined(_M_X64) || defined(__SSE__)
#include <xmmintrin.h>
#endif

// Ordered, bounded, multiple-producer queue. Only the worker touches the wrapped
// backend. Synchronous commands occupy a queue slot too, so a render readback
// cannot race an EE upload arriving after its fence.
// TEMP(transformers): off path takes one flag branch; recording serializes producers.
#define GSREC_LOCK std::unique_lock<std::recursive_mutex> recLock(m_recordMutex, std::defer_lock)
class GSThreadedBackend final : public GSRasterBackend
{
public:
    explicit GSThreadedBackend(std::unique_ptr<GSRasterBackend> backend, bool threaded = true)
        : m_backend(std::move(backend)), m_threaded(threaded), m_record(std::getenv("PS2X_GS_RECORD")), m_worker(threaded ? std::thread([this] { Run(); }) : std::thread{})
    {
        // TEMP(transformers): set in the body - m_backend is declared after these flags. The worker does not read them.
        m_rawGif = m_backend->UsesRawGIF();
        m_decoded = m_backend->WantsDecodedRaster();
    }
    ~GSThreadedBackend() override
    {
        {
            std::lock_guard<std::mutex> registryLock(RegistryMutex());
            if (m_vram) Registry().erase(m_vram);
        }
        // TEMP(transformers): destroy a raw (Vulkan) backend on its own worker after every queued command; Granite
        // objects must be released on the thread that registered thread index 0.
        if (m_rawGif && m_threaded) {
            try { Call([this] { m_backend.reset(); }); } catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_work.notify_one();
        if (m_worker.joinable()) m_worker.join(); // drain before backend/VRAM owner destruction
    }

    // Used by privileged MMIO paths that cannot access the GS frontend.
    static void SyncVram(const uint8_t *vram)
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto it = Registry().find(vram);
        if (it != Registry().end()) it->second->Sync(GSSyncReason::DebugReadback);
    }
    // TEMP(transformers): run `fn` on the worker that owns `vram`, in GS stream order, without waiting (see
    // EnqueueOrdered). Returns false when no worker owns `vram` (PS2X_GS_THREAD=0). Used by the lagged
    // sceGsExecStoreImage (Kernel/Stubs/GS.cpp, PS2X_STOREIMAGE_LAG).
    static bool EnqueueOrderedForVram(const uint8_t *vram, std::function<void(GSRasterBackend &)> fn)
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        auto it = Registry().find(vram);
        if (it == Registry().end()) return false;
        it->second->EnqueueOrderedReadOnly(std::move(fn));
        return true;
    }
    // TEMP(transformers): M1 raw-stream forwarding (gs_pgs_backend.cpp). Copies, in stream order, no waits.
    // GIF packets are appended to a POD batch ([path u32][size u32][bytes, padded to 16]) that becomes ONE queued
    // command when it reaches kRawBatchBytes or before any other command is enqueued (Enqueue flushes it first), so
    // stream order is unchanged. The game's loading screens send ~400k tiny packets/s; one std::function + heap copy +
    // worker wake-up per packet made the warp-gate load 10x slower (PGS_M1.md run pgs_m1a).
    bool UsesRawGIF() const override { return m_rawGif; }
    void RawGIF(uint32_t path, const uint8_t *data, uint32_t size) override
    {
        if (!data || !size) return;
        if (!m_threaded) { m_backend->RawGIF(path, data, size); return; }
        std::lock_guard<std::mutex> lock(m_rawBatchMutex);
        const size_t at = m_rawBatch.size();
        const size_t padded = (size_t(size) + 15u) & ~size_t(15);
        m_rawBatch.resize(at + 16u + padded);
        uint8_t *dst = m_rawBatch.data() + at;
        std::memcpy(dst, &path, 4); std::memcpy(dst + 4, &size, 4); std::memset(dst + 8, 0, 8);
        std::memcpy(dst + 16, data, size);
        if (m_rawBatch.size() >= kRawBatchBytes) FlushRawBatchLocked();
    }
    void RawAD(uint8_t reg, uint64_t value) override
    { Enqueue([=, this] { m_backend->RawAD(reg, value); }); }
    void RawVsync(const std::array<uint8_t,8192> &priv) override
    { Enqueue([this, priv] { m_backend->RawVsync(priv); }); }
    bool WantsDecodedRaster() const override { return m_decoded; }
    void Initialize(uint8_t *vram, uint32_t size) override
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); if (m_record.Active()) { Call([this] { m_backend->Sync(GSSyncReason::DebugReadback); }); m_record.Event(GSRecOp::Initialize, vram, vram ? size : 0); } }
        Call([=, this] { m_backend->Initialize(vram, size); }, 0);
        if (m_vram) Registry().erase(m_vram);
        m_vram = vram;
        if (vram) Registry()[vram] = this;
    }
    void Reset() override {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Event(GSRecOp::Reset); }  Call([this] { m_backend->Reset(); }, 1); }
    void Submit(const GSPrimitiveBatch &batch) override
    { if (!WantsDecodedRaster()) return; GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Submit, batch); }  Enqueue([this, batch] { m_backend->Submit(batch); }); }
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
    { if (!WantsDecodedRaster()) return; GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Clut, GSRecClut{tex0, texclut}); }  Enqueue([=, this] { m_backend->LoadClut(tex0, texclut); }); }
    void BeginTransfer(const GSTransferCommand &command) override
    {
        GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Transfer, command); }
        if (m_rawGif && !m_decoded && command.direction != 1u) return; // TEMP(transformers): raw stream carries uploads/copies
        if (command.direction == 1u && !m_rawGif) // raw: ConsumeLocalToHostBytes is a Call ordered after it anyway
            Call([=, this] { m_backend->BeginTransfer(command); }, 2);
        else Enqueue([=, this] { m_backend->BeginTransfer(command); });
    }
    void UploadImage(const uint8_t *data, uint32_t size) override
    {
        GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); if (data && size) m_record.Event(GSRecOp::Upload, data, size); }
        if (!data || !size || !WantsDecodedRaster()) return;
        std::vector<uint8_t> bytes(data, data + size);
        Enqueue([this, bytes = std::move(bytes)] {
            m_backend->UploadImage(bytes.data(), static_cast<uint32_t>(bytes.size()));
        }, size);
    }
    // TEMP(transformers): GS FINISH/SIGNAL/LABEL used to drain the whole worker queue (Flush + Sync(Finish) as
    // blocking Calls) - in the level ~2 drains per game frame, ~24% of wall time with the EE idle. Every caller that
    // needs completion follows Flush with a blocking Sync (Reset/Presentation/DebugReadback) or reads VRAM via
    // Call/SyncVram, and the CPU backend's Flush/Sync are no-ops, so enqueueing them in stream order changes timing
    // only. PS2X_GS_ASYNC_FINISH=0 restores the blocking drains.
    static bool AsyncFinish()
    {
        static const bool s_on = [] { const char *v = std::getenv("PS2X_GS_ASYNC_FINISH"); return !(v && v[0] == '0'); }();
        return s_on;
    }
    void Flush() override
    {
        GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Event(GSRecOp::Flush); } 
        if (AsyncFinish()) Enqueue([this] { m_backend->Flush(); });
        else Call([this] { m_backend->Flush(); }, 3);
    }
    void TextureFlush() override {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Event(GSRecOp::TextureFlush); }  if (m_rawGif && !m_decoded) return; /* TEXFLUSH is in the raw stream; the shadow CPU backend still needs it */ Enqueue([this] { m_backend->TextureFlush(); }); }
    void Sync(GSSyncReason reason) override
    {
        GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Sync, reason); } 
        if (reason == GSSyncReason::Finish && AsyncFinish()) Enqueue([=, this] { m_backend->Sync(reason); });
        else Call([=, this] { m_backend->Sync(reason); }, 4);
    }
    PresentationFrame Present(const GSPresentationRequest &request) override
    {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Present, request); }  if (m_rawGif) return m_backend->Present(request); /* render thread: mailbox only */ return Call([=, this] { return m_backend->Present(request); }, 5); }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
    {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Clear, GSRecClear{context, rgba}); }
     // TEMP(transformers): raw backends get the GIF clear sprite in the stream; only the shadow CPU backend needs this.
     if (m_rawGif) { if (m_decoded) Enqueue([=, this] { m_backend->ClearFramebuffer(context, rgba); }); return true; }
     return Call([=, this] { return m_backend->ClearFramebuffer(context, rgba); }, 6); }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t size) override
    {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Consume, size); }  return Call([=, this] { return m_backend->ConsumeLocalToHostBytes(dst, size); }, 7); }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
    {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Read, GSRecPixel{psm, base, bw, x, y, 0}); }  return Call([=, this] { return m_backend->ReadVram(psm, base, bw, x, y); }, 8); }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
    {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Pod(GSRecOp::Write, GSRecPixel{psm, base, bw, x, y, value}); }  Enqueue([=, this] { m_backend->WriteVram(psm, base, bw, x, y, value); }); }
    void SnapshotVram(std::vector<uint8_t> &out) const override
    {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Event(GSRecOp::Snapshot); }  Call([this, &out] { m_backend->SnapshotVram(out); }); }
    GSTransferSnapshot GetTransferSnapshot() const override
    {GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Event(GSRecOp::TransferSnapshot); }  return Call([this] { return m_backend->GetTransferSnapshot(); }); }

    // TEMP(transformers): PORT_LIST rank 3 (presentation latch at guest VBlankStart, gs_frontend.cpp
    // ps2PresentLatchOnVBlank). Runs `fn` on the worker against the wrapped backend in stream order -
    // after every command enqueued before it, before every command enqueued after it - and returns
    // without waiting. The EE executor uses it to take the presentation VRAM snapshot at the exact
    // queue position of the guest vsync without draining the worker.
    // Ported from ran-j/PS2Recomp PR #254 (Sinan-Karakaya/PS2Recomp@5d5dfe3,
    // ps2xRuntime/src/lib/gs/gs_threaded_backend.cpp Op::Prepare: Flush + Sync(Presentation) +
    // PreparePresentation queued as an ordered command, displayed later) (GPL-3.0).
    void EnqueueOrdered(std::function<void(GSRasterBackend &)> fn)
    {
        GSREC_LOCK; if (m_record.Enabled()) { recLock.lock(); m_record.Event(GSRecOp::Latch); }
        if (!fn) return;
        Enqueue([this, fn = std::move(fn)] { fn(*m_backend); });
    }

    // TEMP(transformers): lagged readback callback reads VRAM only; no persistent GS effect.
    void EnqueueOrderedReadOnly(std::function<void(GSRasterBackend &)> fn)
    {
        GSREC_LOCK; if (m_record.Enabled()) recLock.lock();
        if (fn) Enqueue([this, fn = std::move(fn)] { fn(*m_backend); });
    }

    // TEMP(transformers): boundary on the EE producer, independent of present mailbox/mode.
    void RecordVsync(const GSPresentationRequest &request)
    {
        if (!m_record.Enabled()) return;
        GSREC_LOCK; recLock.lock();
        if (!m_record.Active() && request.vsyncTick >= m_record.Start() && !m_record.Done())
        {
            auto *cpu = dynamic_cast<GSCpuBackend *>(m_backend.get());
            if (!cpu) { m_record.Fail("CPU backend required"); return; }
            auto state = Call([cpu] { return cpu->RecordState(); });
            std::vector<uint8_t> vram;
            Call([&] { m_backend->SnapshotVram(vram); });
            m_record.Begin(request, state, vram);
            return;
        }
        if (m_record.Active())
        {
            m_record.Pod(GSRecOp::Vsync, request);
            if (m_record.Boundary())
            {
                std::vector<uint8_t> vram;
                Call([&] { m_backend->SnapshotVram(vram); });
                m_record.Finish(vram);
            }
        }
    }
    // TEMP(transformers): one worker fence takes persistent state and VRAM at the same stream position.
    void RawSnapshot(std::vector<uint8_t>& state,std::vector<uint8_t>& portable,std::vector<uint8_t>& vram)
    {
        GSREC_LOCK; recLock.lock();
        Call([&] { auto* cpu=dynamic_cast<GSCpuBackend*>(m_backend.get());
            if(!cpu) throw std::runtime_error("GSRAW requires CPU backend");
            state=cpu->RecordState(); portable=cpu->RawPortableState();cpu->SnapshotVram(vram); });
    }
    uint64_t DecodedRecordCount() const { GSREC_LOCK; recLock.lock();return m_record.Count(); }
private:
    bool m_threaded;
    bool m_rawGif = false;  // TEMP(transformers): wrapped backend consumes the raw GIF stream (PGS)
    bool m_decoded = true;  // wrapped backend wants decoded draws/uploads/CLUT loads
    static constexpr size_t kRawBatchBytes = 256u * 1024u; // TEMP(transformers): raw GIF batch (RawGIF)
    mutable std::mutex m_rawBatchMutex;
    mutable std::vector<uint8_t> m_rawBatch;
    mutable std::mutex m_rawSpareMutex;                  // eetrim: recycled raw batch buffers (FlushRawBatchLocked)
    mutable std::vector<std::vector<uint8_t>> m_rawSpare;
    static bool RawRecycle()
    {
        static const bool s_on = [] { const char *v = std::getenv("PS2X_GS_RAW_RECYCLE"); return !(v && v[0] == '0'); }();
        return s_on;
    }
    mutable GSRecorder m_record;
    mutable std::recursive_mutex m_recordMutex;
    struct Command
    {
        std::function<void()> execute;
        std::fenv_t environment{};
        size_t payloadBytes = 0;
#if defined(_M_X64) || defined(__SSE__)
        uint32_t mxcsr = 0;
        gsTimeline::Context timeline{};
#endif
    };
    static constexpr size_t Capacity = 1024;
    static constexpr size_t MaxQueuedBytes = 64u * 1024u * 1024u;
    std::unique_ptr<GSRasterBackend> m_backend;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_work, m_space;
    mutable std::atomic<bool> m_workWaiting{false};
    mutable unsigned m_spaceWaiters=0; // m_mutex held; multiple producers
    mutable std::array<Command, Capacity> m_ring{};
    mutable size_t m_head = 0, m_tail = 0, m_count = 0;
    mutable size_t m_queuedBytes = 0;
    bool m_stopping = false;
    mutable std::exception_ptr m_error;
    uint8_t *m_vram = nullptr;
    // TEMP(transformers): throughput/stall statistics, printed by the worker
    // every ~5 s while busy as "[gs:thread] ..." (PS2X_GS_THREAD_STATS=0 disables).
    mutable std::atomic<uint64_t> m_statEnqueued{0}, m_statCalls{0}, m_statCallWaitUs{0},
        m_statFullWaits{0}, m_statFullWaitUs{0}, m_statMaxDepth{0}, m_statExecuted{0};
    mutable std::array<std::atomic<uint64_t>, 10> m_statSiteUs{}, m_statSiteCalls{}; // TEMP(transformers)
    // Must be last: thread starts after all state has been initialized.
    std::thread m_worker;
    static std::unordered_map<const uint8_t *, GSThreadedBackend *> &Registry()
    { static std::unordered_map<const uint8_t *, GSThreadedBackend *> value; return value; }
    static std::mutex &RegistryMutex() { static std::mutex value; return value; }
    // TEMP(transformers): raw GIF batch (see RawGIF). Caller holds m_rawBatchMutex.
    void FlushRawBatchLocked() const
    {
        if (m_rawBatch.empty()) return;
        std::vector<uint8_t> batch;
        batch.swap(m_rawBatch);
        // eetrim: reuse batch buffers the worker has finished with instead of a fresh 260 KB allocation per flush
        // (fresh large blocks also page-fault on first touch). PS2X_GS_RAW_RECYCLE=0 restores the old behaviour.
        if (RawRecycle())
        {
            std::lock_guard<std::mutex> spareLock(m_rawSpareMutex);
            if (!m_rawSpare.empty())
            {
                m_rawBatch.swap(m_rawSpare.back());
                m_rawSpare.pop_back();
            }
        }
        m_rawBatch.clear();
        m_rawBatch.reserve(kRawBatchBytes + 4096u);
        const size_t bytes = batch.size();
        EnqueueImpl([this, batch = std::move(batch)]() mutable {
            const uint8_t *p = batch.data(), *end = p + batch.size();
            while (p + 16 <= end)
            {
                uint32_t path, size;
                std::memcpy(&path, p, 4); std::memcpy(&size, p + 4, 4);
                m_backend->RawGIF(path, p + 16, size);
                p += 16u + ((size_t(size) + 15u) & ~size_t(15));
            }
            if (RawRecycle() && batch.capacity() <= 2u * (kRawBatchBytes + 4096u))
            {
                batch.clear();
                std::lock_guard<std::mutex> spareLock(m_rawSpareMutex);
                if (m_rawSpare.size() < 16u) m_rawSpare.push_back(std::move(batch));
            }
        }, bytes);
    }
    void Enqueue(std::function<void()> execute, size_t payloadBytes = 0) const
    {
        if (m_rawGif && m_threaded)
        {
            // Keep stream order: pending raw GIF packets go first. Holding the batch lock across both enqueues keeps
            // a concurrent producer's batch from overtaking this one.
            std::lock_guard<std::mutex> lock(m_rawBatchMutex);
            FlushRawBatchLocked();
            EnqueueImpl(std::move(execute), payloadBytes);
            return;
        }
        EnqueueImpl(std::move(execute), payloadBytes);
    }
    void EnqueueImpl(std::function<void()> execute, size_t payloadBytes = 0) const
    {
        if (!m_threaded) { execute(); return; } // TEMP(transformers): preserve serial execution/environment.
        Command command{std::move(execute)};
        command.payloadBytes = payloadBytes;
        // XGKICK runs with FE_TOWARDZERO. Host/render calls may use a different
        // environment; capturing it per command preserves the serial results.
        std::fegetenv(&command.environment);
#if defined(_M_X64) || defined(__SSE__)
        command.timeline=gsTimeline::current;
        command.mxcsr = _mm_getcsr(); // includes DAZ/FTZ and SSE exception masks
#endif
        std::unique_lock<std::mutex> lock(m_mutex);
        // Admit an oversized individual upload only to an empty ring.
        // The limit excludes the command executing and the caller's copy.
        const auto admissible = [this, payloadBytes] {
            return m_error || (m_count < Capacity && (m_count == 0 ||
                (m_queuedBytes <= MaxQueuedBytes && payloadBytes <= MaxQueuedBytes - m_queuedBytes)));
        };
        if (!admissible()) // TEMP(transformers): count producer stalls on a full ring
        {
            const auto waitStart = std::chrono::steady_clock::now();
            ps2_perfscope::Scope perfWait(ps2_perfscope::EeWaitGsFull); // TEMP(transformers) perfscope
            ps2_perfscope::noteGsWaitCount(true);
            ++m_spaceWaiters;
            m_space.wait(lock, admissible);
            --m_spaceWaiters;
            m_statFullWaits.fetch_add(1, std::memory_order_relaxed);
            m_statFullWaitUs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - waitStart).count()), std::memory_order_relaxed);
        }
        if (m_error) std::rethrow_exception(m_error);
        m_ring[m_tail] = std::move(command);
        m_tail = (m_tail + 1) % Capacity;
        ++m_count;
        m_queuedBytes += payloadBytes;
        m_statEnqueued.fetch_add(1, std::memory_order_relaxed);
        if (m_count > m_statMaxDepth.load(std::memory_order_relaxed))
            m_statMaxDepth.store(m_count, std::memory_order_relaxed);
        const bool wake=!ps2Wake::lazy || m_workWaiting.load();
        
        lock.unlock();
        if(wake)m_work.notify_one();
    }
    // TEMP(transformers): site = which method blocked (0 Init 1 Reset 2 Local->host 3 Flush 4 Sync 5 Present 6 Clear
    // 7 ConsumeL2H 8 ReadVram 9 other), wait time per site is printed in the [gs:thread] line as sitesMs=.
    template<class F> auto Call(F function, int site = 9) const -> decltype(function())
    {
        auto task = std::make_shared<std::packaged_task<decltype(function())()>>(std::move(function));
        auto result = task->get_future();
        // Release the caller's task reference before waiting. If an earlier
        // asynchronous command fails, clearing the queued task must break its
        // promise and wake this waiter.
        Enqueue([task = std::move(task)] { (*task)(); });
        const auto waitStart = std::chrono::steady_clock::now(); // TEMP(transformers)
        {
            ps2_perfscope::Scope perfWait(ps2_perfscope::EeWaitGsSync); // TEMP(transformers) qualification
            ps2_perfscope::noteGsWaitCount(false);
            result.wait();
        }
        m_statCalls.fetch_add(1, std::memory_order_relaxed);
        const uint64_t waitedUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - waitStart).count());
        m_statCallWaitUs.fetch_add(waitedUs, std::memory_order_relaxed);
        m_statSiteUs[static_cast<size_t>(site) % m_statSiteUs.size()].fetch_add(waitedUs, std::memory_order_relaxed);
        m_statSiteCalls[static_cast<size_t>(site) % m_statSiteCalls.size()].fetch_add(1, std::memory_order_relaxed);
        return result.get();
    }
    void MaybePrintStats(std::chrono::steady_clock::time_point &last) // TEMP(transformers)
    {
        static const bool s_enabled = [] {
            const char *v = std::getenv("PS2X_GS_THREAD_STATS");
            return !(v && v[0] == '0');
        }();
        if (!s_enabled)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(5))
            return;
        last = now;
        std::cout << "[gs:thread] executed=" << m_statExecuted.load(std::memory_order_relaxed)
                  << " enqueued=" << m_statEnqueued.load(std::memory_order_relaxed)
                  << " calls=" << m_statCalls.load(std::memory_order_relaxed)
                  << " callWaitMs=" << m_statCallWaitUs.load(std::memory_order_relaxed) / 1000u
                  << " fullWaits=" << m_statFullWaits.load(std::memory_order_relaxed)
                  << " fullWaitMs=" << m_statFullWaitUs.load(std::memory_order_relaxed) / 1000u
                  << " maxDepth=" << m_statMaxDepth.exchange(0, std::memory_order_relaxed) << " sitesMs=";
        for (size_t i = 0; i < m_statSiteUs.size(); ++i)
            std::cout << (i ? "," : "") << i << ":" << m_statSiteCalls[i].load(std::memory_order_relaxed) << "/"
                      << m_statSiteUs[i].load(std::memory_order_relaxed) / 1000u;
        std::cout << std::endl;
    }
    // Adaptive prefixes inspired by Robert Holz, silentsudin/PS2Recomp 37834d6e (GPL-3.0).
    void RunLazy()
    {
        ps2_perfscope::registerThread(ps2_perfscope::LaneGS);
        auto lastStats=std::chrono::steady_clock::now();
        size_t handoffAt=24;
        for(;;) {
            std::array<Command,384> batch;
            size_t count=0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                bool slept=false;
                if(!m_count && !m_stopping) {
                    ps2_perfscope::Scope perfIdle(ps2_perfscope::GsIdle);
                    m_workWaiting.store(true);slept=true;
                    m_work.wait(lock,[this]{return m_count||m_stopping;});
                    m_workWaiting.store(false);
                }
                if(!m_count)return;
                // Removing an ordered prefix never delays a fence behind later work.
                // No fill threshold: a partial batch executes immediately.
                const size_t n=(std::min)(m_count,handoffAt);
                for(;count<n;++count) {
                    batch[count]=std::move(m_ring[m_head]);
                    m_ring[m_head]={};
                    m_head=(m_head+1)%Capacity;--m_count;
                    m_queuedBytes-=batch[count].payloadBytes;
                }
                handoffAt=slept?std::min<size_t>(384,handoffAt*2):std::max<size_t>(24,handoffAt/2);
                // Heterogeneous byte requirements require broadcasting to all
                // blocked producers, rather than waking an inadmissible one.
                if(m_spaceWaiters){m_space.notify_all();}
            }
            try {
                for(size_t i=0;i<count;++i) {
                    auto &command=batch[i];
                    std::fesetenv(&command.environment);
#if defined(_M_X64) || defined(__SSE__)
                    auto timeline=command.timeline;timeline.mxcsr=command.mxcsr;
                    gsTimeline::Scope timelineScope(timeline);
                    _mm_setcsr(command.mxcsr);
#endif
                    command.execute();command.execute={};
                    m_statExecuted.fetch_add(1,std::memory_order_relaxed);
                    MaybePrintStats(lastStats);
                }
            } catch(...) {
                // Destroy every unexecuted packaged_task, including the detached
                // prefix: result.wait() must see broken_promise on worker failure.
                for(auto &command:batch)command={};
                std::lock_guard<std::mutex> lock(m_mutex);
                m_error=std::current_exception();
                while(m_count){m_queuedBytes-=m_ring[m_head].payloadBytes;m_ring[m_head]={};m_head=(m_head+1)%Capacity;--m_count;}
                m_space.notify_all();
                std::cerr<<"[gs:thread] FATAL worker exception (worker wake)"<<std::endl;
                return;
            }
        }
    }
    void Run()
    {
        if(ps2Wake::lazy){RunLazy();return;}
        ps2_perfscope::registerThread(ps2_perfscope::LaneGS); // TEMP(transformers) perfscope
        auto lastStats = std::chrono::steady_clock::now();
        for (;;)
        {
            Command command;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (!m_count && !m_stopping) // TEMP(transformers) qualification: idle = waiting for work
                {
                    ps2_perfscope::Scope perfIdle(ps2_perfscope::GsIdle);
                    m_work.wait(lock, [this] { return m_count || m_stopping; });
                }
                if (!m_count) return;
                command = std::move(m_ring[m_head]);
                m_head = (m_head + 1) % Capacity;
                --m_count;
                m_queuedBytes -= command.payloadBytes;
            }
            m_space.notify_one();
            try
            {
                std::fesetenv(&command.environment);
#if defined(_M_X64) || defined(__SSE__)
                auto timeline=command.timeline;timeline.mxcsr=command.mxcsr;
                gsTimeline::Scope timelineScope(timeline);
                _mm_setcsr(command.mxcsr);
#endif
                command.execute();
                m_statExecuted.fetch_add(1, std::memory_order_relaxed);
                MaybePrintStats(lastStats);
            }
            catch (...)
            {
                // TEMP(transformers): a dead worker must be loud, not a silent black screen.
                try { throw; }
                catch (const std::exception &e) { std::cerr << "[gs:thread] FATAL worker exception: " << e.what() << std::endl; }
                catch (...) { std::cerr << "[gs:thread] FATAL worker exception (non-std)" << std::endl; }
                std::lock_guard<std::mutex> lock(m_mutex);
                m_error = std::current_exception();
                // Discard queued tasks: their futures become broken promises.
                while (m_count)
                {
                    m_queuedBytes -= m_ring[m_head].payloadBytes;
                    m_ring[m_head] = {};
                    m_head = (m_head + 1) % Capacity;
                    --m_count;
                }
                m_space.notify_all();
                return;
            }
        }
    }
};

#undef GSREC_LOCK
inline std::unique_ptr<GSRasterBackend> MakeGSBackend(std::unique_ptr<GSRasterBackend> backend)
{
    const char *setting = std::getenv("PS2X_GS_THREAD");
    // Opt-in pending live-game validation. Capture selection uses host frame
    // counters: keep diagnostic runs serial to retain their exact boundaries.
    // TEMP(transformers)
    const bool capture = std::getenv("PS2X_GSCAPTURE") || std::getenv("PS2X_GSCAPTURE_VS") ||
                         std::getenv("PS2X_TEX_DEBUG_DUMPS");
    // TEMP(transformers): a raw (Vulkan) backend always gets its own worker thread: it owns the device.
    const bool threaded = backend->UsesRawGIF() || (setting && setting[0] == '1' && !capture);
    if ((threaded || std::getenv("PS2X_GS_RECORD") || std::getenv("PS2X_GS_RAWREC")) && !dynamic_cast<GSThreadedBackend *>(backend.get()))
        return std::make_unique<GSThreadedBackend>(std::move(backend), threaded);
    return backend;
}
