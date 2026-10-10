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

namespace uballoc {

// HeartbeatThread: independent background thread for dead-peer detection.
//
// A dedicated thread that periodically calls write_heartbeat() on the backend,
// the heartbeat is always written as long as the process is alive and
// heartbeat_interval > 0.
//
// Fork safety: same pattern as ReclaimThread. stop() detects fork-child
// (getpid() != creator_pid_) and detaches the inherited thread object
// without destroying the condition_variable (which has stale __wrefs).
class HeartbeatThread {
public:
    using WriteFn = void(*)();

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::condition_variable cv_;
    std::mutex mtx_;

    // PID recorded at start() time. Used by stop() to detect fork-child.
    pid_t creator_pid_ = 0;

    // Heartbeat write interval (ns). 0 = disabled (thread not started).
    uint64_t interval_ns_ = 5'000'000'000ULL;

    // Function to call each cycle (backend_.write_heartbeat).
    WriteFn write_fn_ = nullptr;

public:
    ~HeartbeatThread() {
        stop();
    }

    void set_write_fn(WriteFn fn) { write_fn_ = fn; }

    void start(uint64_t interval_ms) {
        interval_ns_ = interval_ms * 1'000'000ULL;
        if (interval_ns_ == 0) return;  // disabled
        running_ = true;
        stop_requested_ = false;
        creator_pid_ = ::getpid();
        thread_ = std::thread(&HeartbeatThread::run, this);
        LOG_INFO("HeartbeatThread started, interval=" << interval_ns_ << "ns");
    }

    // Returns true if a fork-child was detected (same semantics as
    // ReclaimThread::stop). See reclaim_thread.hpp for the full
    // explanation of why fork-child must not destroy cv_.
    bool stop() {
        stop_requested_.store(true, std::memory_order_release);
        bool fork_child = (creator_pid_ != 0 && ::getpid() != creator_pid_);
        if (fork_child) {
            {
                std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
                if (lk.owns_lock()) {
                    cv_.notify_all();
                }
            }
            if (thread_.joinable()) {
                try { thread_.detach(); } catch (...) {}
            }
            running_ = false;
            LOG_INFO("HeartbeatThread stopped (fork_child=true)");
            return true;
        }
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.notify_all();
        }
        if (thread_.joinable()) {
            try {
                thread_.join();
            } catch (const std::system_error& e) {
                LOG_INFO("HeartbeatThread::stop: join failed: " << e.what());
                try { thread_.detach(); } catch (...) {}
            }
        }
        running_ = false;
        LOG_INFO("HeartbeatThread stopped (fork_child=false)");
        return false;
    }

    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void run() {
        // Sleep first, then write: init() already wrote the initial
        // heartbeat, so the first periodic write should happen after
        // one full interval, not immediately at thread start.
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::nanoseconds(interval_ns_),
                         [this] {
                             return stop_requested_.load(std::memory_order_acquire);
                         });
            if (stop_requested_.load(std::memory_order_acquire)) break;

            // Write heartbeat after sleeping for one interval
            if (write_fn_) {
                write_fn_();
            }
        }
    }
};

} // namespace uballoc
