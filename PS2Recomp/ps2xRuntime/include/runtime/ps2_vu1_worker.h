#pragma once
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
// Implemented in ps2_vu1_core.cpp: keep Windows macros out of runtime/PCH headers.
void ps2Vu1WorkerPriority() noexcept;

// One EE producer, one persistent worker, at most one outstanding program.
// Host waits never advance the deterministic guest scheduler.
class Vu1Worker
{
public:
    static uint64_t currentThreadToken() {
        static std::atomic<uint64_t> next{1};
        static thread_local const uint64_t token = next.fetch_add(1, std::memory_order_relaxed);
        return token;
    }
    static bool enabled() {
        static const bool on = [] { const char *p = std::getenv("PS2X_VU1_THREAD"); return p && p[0] == '1' && p[1] == 0; }();
        return on;
    }
    Vu1Worker() : m_thread([this] { loop(); }) {}
    ~Vu1Worker() {
        finishNoThrow();
        { std::lock_guard<std::mutex> lock(m_mutex); m_stop = true; }
        m_ready.notify_one();
        m_thread.join();
    }
    void submit(std::function<void()> job) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending) throw std::logic_error("VU1 worker submit without fence");
        m_error = nullptr; m_job = std::move(job); m_pending = true;
        m_ready.notify_one();
    }
    void finish() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this] { return !m_pending; });
        auto error = m_error; m_error = nullptr;
        lock.unlock();
        if (error) std::rethrow_exception(error);
    }
    void finishNoThrow() noexcept { try { finish(); } catch (...) {} }
private:
    // Declare thread last: all synchronization state exists before it starts.
    std::mutex m_mutex;
    std::condition_variable m_ready, m_done;
    std::function<void()> m_job;
    std::exception_ptr m_error;
    bool m_pending = false, m_stop = false;
    std::thread m_thread;
    void loop() {
        ps2Vu1WorkerPriority();
        for (;;) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait(lock, [this] { return m_stop || bool(m_job); });
            if (m_stop) return;
            auto job = std::move(m_job); m_job = {};
            lock.unlock();
            std::exception_ptr error;
            try { job(); } catch (...) { error = std::current_exception(); }
            lock.lock(); m_error = error; m_pending = false;
            lock.unlock(); m_done.notify_one();
        }
    }
};
