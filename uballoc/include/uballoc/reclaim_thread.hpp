// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <unistd.h>

#include "log.hpp"
#include "reclaim_mode.hpp"

namespace uballoc {

// ReclaimThread: background thread for async memory reclaim.
//
// jemalloc-style wake mechanism (background_thread.c:302):
//   - compute next earliest decay deadline (LIVE-idle or DETACHED segment)
//   - if deadline exists: cond_timedwait(deadline - now, clamped to [100ms, 5s])
//   - if no pending work:  cond_wait (indefinite sleep, 0% CPU)
//   - free path calls trigger_now() when live_slab_count drops to 0,
//     waking the thread from indefinite sleep to re-evaluate its deadline
//
// One thread per process. Started in do_lazy_init (async mode only),
// stopped in soft_reset/reset.
class ReclaimThread {
public:
    using ScanFn = void(*)();

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::condition_variable cv_;
    std::mutex mtx_;
    std::atomic<bool> indefinite_sleep_{false};

    // PID recorded at start() time. Used by stop() to detect fork-child
    // (getpid() != creator_pid_): the thread object was inherited from
    // the parent, but the actual thread doesn't exist in this process.
    pid_t creator_pid_ = 0;

    // Scan interval (5s default, jemalloc MIN_INTERVAL=100ms clamp)
    uint64_t scan_interval_ns_ = 5'000'000'000ULL;
    uint64_t min_interval_ns_ = 100'000'000ULL;

    // Function to call each scan cycle (GlobalAllocator::scan_all_segments_static)
    ScanFn scan_fn_ = nullptr;

    // Deadline query function (backend.compute_next_reclaim_deadline_ns)
    using DeadlineFn = uint64_t(*)();
    DeadlineFn deadline_fn_ = nullptr;

public:
    // Destructor: ensure thread is stopped before backend_ destructor runs.
    // GlobalAllocator member destruction order: reclaim_thread_ (later declared)
    // destroyed before backend_ (earlier declared), so this runs first.
    ~ReclaimThread() {
        stop();
    }

    void set_scan_fn(ScanFn fn) { scan_fn_ = fn; }
    void set_deadline_fn(DeadlineFn fn) { deadline_fn_ = fn; }

    void start(uint64_t scan_interval_ms = 5000) {
        scan_interval_ns_ = scan_interval_ms * 1'000'000ULL;
        running_ = true;
        stop_requested_ = false;
        creator_pid_ = ::getpid();
        thread_ = std::thread(&ReclaimThread::run, this);
        LOG_INFO("ReclaimThread started, scan_interval=" << scan_interval_ns_ << "ns");
    }

    // Returns true if a fork-child was detected. In that case the caller
    // MUST NOT destroy this ReclaimThread object — the inherited
    // condition_variable has stale __wrefs from the parent's waiters, and
    // ~condition_variable() → pthread_cond_destroy() would block forever
    // waiting for them to drain. The caller should release() the unique_ptr
    // to leak the object (acceptable: fork children are short-lived and
    // typically exit via _exit(), which skips destructors).
    bool stop() {
        stop_requested_.store(true, std::memory_order_release);
        // Fork-child detection: the thread was started in a parent process
        // (creator_pid_), but getpid() in the child differs after fork.
        // The thread object is inherited but the actual thread doesn't
        // exist in this process. We must NOT destroy cv_ (inherited
        // __wrefs would block pthread_cond_destroy forever).
        bool fork_child = (creator_pid_ != 0 && ::getpid() != creator_pid_);
        if (fork_child) {
            // fork-safe: after fork, child inherits locked mutexes but not
            // the thread that locked them. Use try_lock to avoid deadlock.
            {
                std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
                if (lk.owns_lock()) {
                    indefinite_sleep_.store(false, std::memory_order_release);
                }
            }
            cv_.notify_all();  // no-op: no thread waiting in this process
            // The thread doesn't exist in this process. detach() makes the
            // thread object non-joinable without touching the non-existent
            // pthread (pthread_detach error is ignored by std::thread).
            if (thread_.joinable()) {
                try { thread_.detach(); } catch (...) {}
            }
            running_ = false;
            LOG_INFO("ReclaimThread stopped (fork_child=true)");
            return true;
        }
        // Normal (same-process) stop path.
        {
            std::unique_lock<std::mutex> lk(mtx_);
            indefinite_sleep_.store(false, std::memory_order_release);
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            try {
                thread_.join();
            } catch (const std::system_error& e) {
                LOG_INFO("ReclaimThread::stop: join failed: " << e.what());
                try { thread_.detach(); } catch (...) {}
            }
        }
        running_ = false;
        LOG_INFO("ReclaimThread stopped (fork_child=false)");
        return false;
    }

    // Called from heap.hpp free_offset when a segment becomes idle
    // (live_slab_count drops to 0). This is the jemalloc
    // "pac_maybe_wake_bg" pattern: the hot path doesn't scan, it just
    // pokes the background thread to re-evaluate its deadline and wake
    // from indefinite sleep. Also called from stop() to wake for shutdown.
    // Always notifies unconditionally — even if try_lock fails (fork
    // child inherited locked mutex), the notify is harmless (no waiter
    // in that process). The flag is set atomically so the wait predicate
    // will see it on the next iteration even without holding the mutex.
    void trigger_now() {
        indefinite_sleep_.store(false, std::memory_order_release);
        {
            std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
            // Even if we don't get the lock, the flag store above ensures
            // the thread wakes (predicate checks !indefinite_sleep_).
        }
        cv_.notify_one();
    }

    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void run() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            // Do the scan work
            if (scan_fn_) {
                scan_fn_();
            }

            // jemalloc-style sleep
            uint64_t deadline = deadline_fn_ ? deadline_fn_() : UINT64_MAX;
            sleep_until_deadline_or_signal(deadline);
        }
        // After stop_requested: one final check to skip any pending work.
        // This prevents scan from racing with destructor cleanup.
    }

    void sleep_until_deadline_or_signal(uint64_t deadline_ns) {
        std::unique_lock<std::mutex> lk(mtx_);

        if (deadline_ns == UINT64_MAX) {
            // No pending work → indefinite sleep (0% CPU)
            indefinite_sleep_.store(true, std::memory_order_release);
            cv_.wait(lk, [this] {
                return stop_requested_.load(std::memory_order_acquire) ||
                       !indefinite_sleep_.load(std::memory_order_acquire);
            });
            indefinite_sleep_.store(false, std::memory_order_release);
        } else {
            // Pending work → sleep until deadline, clamped to [min, scan_interval]
            auto now_ns = []() -> uint64_t {
                auto tp = std::chrono::steady_clock::now().time_since_epoch();
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
            }();

            uint64_t sleep_ns;
            if (deadline_ns <= now_ns) {
                sleep_ns = min_interval_ns_;
            } else {
                sleep_ns = deadline_ns - now_ns;
                if (sleep_ns < min_interval_ns_) sleep_ns = min_interval_ns_;
                if (sleep_ns > scan_interval_ns_) sleep_ns = scan_interval_ns_;
            }
            cv_.wait_for(lk, std::chrono::nanoseconds(sleep_ns));
        }
    }
};

} // namespace uballoc
