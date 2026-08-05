// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <atomic>
#include <optional>
#include <functional>
#include <iterator>
#include <cassert>
#include <array>

#include "packed.hpp"
#include "thread.hpp"
#include "size.hpp"
#include "bitset.hpp"
#include "data.hpp"
#include "cache.hpp"

namespace uballoc {

struct Version {
    uint16_t value_;

    constexpr Version() : value_(0) {}
    constexpr explicit Version(uint16_t v) : value_(v) {}

    constexpr Version next() const { return Version(value_ + 1); }

    constexpr bool operator==(const Version& other) const { return value_ == other.value_; }
    constexpr bool operator!=(const Version& other) const { return value_ != other.value_; }

    constexpr static uint64_t pack(const Version& v) { return v.value_; }
    constexpr static Version unpack(uint64_t raw) { return Version(raw); }
};

struct VersionTrait {
    static constexpr unsigned total_bits = 16;
};

template<typename T>
struct CasState {
    std::optional<ThreadId> id;
    Version version;
    T inner;

    constexpr CasState() : id(std::nullopt), version(), inner() {}
    constexpr CasState(std::optional<ThreadId> i, Version v, T in) : id(i), version(v), inner(in) {}

    static CasState unpack(uint64_t raw);
    static uint64_t pack(const CasState& s);
};

template<typename T>
struct CasStateTrait {
    static constexpr unsigned total_bits = 64;
};

struct HelpArray {
    std::atomic<uint16_t>* base_;
    bool initialized_;

    HelpArray() : base_(nullptr), initialized_(false) {}

    void init(void* base) {
        base_ = static_cast<std::atomic<uint16_t>*>(base);
        initialized_ = true;
        for (size_t i = 0; i < COUNT_THREAD * COUNT_THREAD; ++i) {
            base_[i].store(0, std::memory_order_relaxed);
        }
    }

    Version load(ThreadId i, ThreadId j) {
        if (!initialized_) return Version();
        size_t idx = i.get() * COUNT_THREAD + j.get();
        return Version(base_[idx].load(std::memory_order_relaxed));
    }

    void store(ThreadId i, ThreadId j, Version ver) {
        if (!initialized_) return;
        size_t idx = i.get() * COUNT_THREAD + j.get();
        base_[idx].store(ver.value_, std::memory_order_relaxed);
        flush(&base_[idx], Invalidate::No);
        fence();
    }

    size_t count_observers(ThreadId target_id, Version ver) {
        if (!initialized_) return 0;
        size_t count = 0;
        for (size_t i = 0; i < COUNT_THREAD; ++i) {
            if (load(ThreadId(i), target_id) == ver) {
                count++;
            }
        }
        return count;
    }

    static constexpr size_t size_bytes() {
        return COUNT_THREAD * COUNT_THREAD * sizeof(std::atomic<uint16_t>);
    }

    static constexpr size_t rows_bytes(size_t num_cores) {
        return num_cores * COUNT_THREAD * sizeof(std::atomic<uint16_t>);
    }
};

struct DistributedHelpArray {
    std::array<HelpArray*, MAX_PROCESSES> slices_;
    std::array<std::atomic<uint16_t>*, MAX_PROCESSES> data_bases_;
    int total_processes_;
    int rank_;
    size_t num_cores_;
    bool initialized_;

    DistributedHelpArray() : total_processes_(1), rank_(0),
                             num_cores_(MAX_NUM_CORES), initialized_(false) {
        for (size_t i = 0; i < MAX_PROCESSES; ++i) {
            slices_[i] = nullptr;
            data_bases_[i] = nullptr;
        }
    }

    void set_slice(int pid, HelpArray* help, std::atomic<uint16_t>* data_base) {
        slices_[pid] = help;
        data_bases_[pid] = data_base;
    }

    Version load(ThreadId i, ThreadId j) {
        if (!initialized_) return Version();
        int pid = i.get() / num_cores_;
        if (pid >= total_processes_ || !slices_[pid]) return Version();
        size_t local_row = i.get() - pid * num_cores_;
        size_t idx = local_row * COUNT_THREAD + j.get();
        return Version(data_bases_[pid][idx].load(std::memory_order_relaxed));
    }

    void store(ThreadId i, ThreadId j, Version ver) {
        if (!initialized_) return;
        int pid = i.get() / num_cores_;
        if (pid >= total_processes_ || !data_bases_[pid]) return;
        size_t local_row = i.get() - pid * num_cores_;
        size_t idx = local_row * COUNT_THREAD + j.get();
        data_bases_[pid][idx].store(ver.value_, std::memory_order_relaxed);
        flush(&data_bases_[pid][idx], Invalidate::No);
        fence();
    }

    size_t count_observers(ThreadId target_id, Version ver) {
        if (!initialized_) return 0;
        size_t count = 0;
        for (int pid = 0; pid < total_processes_; ++pid) {
            if (!data_bases_[pid]) continue;
            for (size_t local_row = 0; local_row < num_cores_; ++local_row) {
                size_t global_tid = pid * num_cores_ + local_row;
                if (global_tid >= COUNT_THREAD) break;
                size_t idx = local_row * COUNT_THREAD + target_id.get();
                Version loaded = Version(data_bases_[pid][idx].load(std::memory_order_relaxed));
                if (loaded == ver) count++;
            }
        }
        return count;
    }

    static constexpr size_t per_process_rows_bytes() {
        return MAX_NUM_CORES * COUNT_THREAD * sizeof(std::atomic<uint16_t>);
    }
};

template<typename T>
struct Detectable {
    std::atomic<uint64_t> raw_;

    Detectable() : raw_(0) {}

    T load(std::memory_order order) {
        uint64_t r = raw_.load(order);
        flush(&raw_, Invalidate::No);
        fence();
        return T::unpack(r);
    }

    T load_with_help(ThreadId tid, HelpArray* help, std::memory_order order) {
        uint64_t r = raw_.load(order);
        flush(&raw_, Invalidate::No);
        fence();

        do_help(tid, help, CasState<T>::unpack(r));
        return T::unpack(r);
    }

    T load_with_help_distributed(ThreadId tid, DistributedHelpArray* help, std::memory_order order) {
        uint64_t r = raw_.load(order);
        flush(&raw_, Invalidate::No);
        fence();

        do_help_distributed(tid, help, CasState<T>::unpack(r));
        return T::unpack(r);
    }

    void do_help(ThreadId tid, HelpArray* help, const CasState<T>& state) {
        if (!help || !state.id) return;
        help->store(tid, *state.id, state.version);
    }

    void do_help_distributed(ThreadId tid, DistributedHelpArray* help, const CasState<T>& state) {
        if (!help || !state.id) return;
        help->store(tid, *state.id, state.version);
    }

    void store(std::optional<ThreadId> id, const T& value, std::memory_order order) {
        CasState<T> state(id, Version(), value);
        raw_.store(CasStateTrait<T>::pack(state), order);
        flush(&raw_, Invalidate::No);
        fence();
    }

    template<typename F, typename B>
    std::optional<T> update(ThreadId tid, HelpArray* help, Version ver,
                            std::memory_order success, std::memory_order failure,
                            F&& next) {
        if (help) {
            Version current = help->load(tid, tid);
            help->store(tid, tid, current.next());
        }

        uint64_t old_raw = raw_.load(std::memory_order_relaxed);

        while (true) {
            CasState<T> old = CasStateTrait<T>::unpack(old_raw);

            do_help(tid, help, old);

            auto result = next(old.inner, ver);
            if (!result) return std::nullopt;

            auto [new_inner, log] = *result;
            (void)log;

            CasState<T> desired(tid, ver, new_inner);
            uint64_t desired_raw = CasStateTrait<T>::pack(desired);

            if (raw_.compare_exchange_strong(old_raw, desired_raw, success, failure)) {
                flush(&raw_, Invalidate::No);
                fence();
                return old.inner;
            }
        }
    }

    template<typename F, typename B>
    std::optional<T> update_distributed(ThreadId tid, DistributedHelpArray* help, Version ver,
                                         std::memory_order success, std::memory_order failure,
                                         F&& next) {
        if (help) {
            Version current = help->load(tid, tid);
            help->store(tid, tid, current.next());
        }

        uint64_t old_raw = raw_.load(std::memory_order_relaxed);

        while (true) {
            CasState<T> old = CasStateTrait<T>::unpack(old_raw);

            do_help_distributed(tid, help, old);

            auto result = next(old.inner, ver);
            if (!result) return std::nullopt;

            auto [new_inner, log] = *result;
            (void)log;

            CasState<T> desired(tid, ver, new_inner);
            uint64_t desired_raw = CasStateTrait<T>::pack(desired);

            if (raw_.compare_exchange_strong(old_raw, desired_raw, success, failure)) {
                flush(&raw_, Invalidate::No);
                fence();
                return old.inner;
            }
        }
    }

    bool detect(ThreadId tid, Version ver, HelpArray* help) {
        uint64_t state_raw = raw_.load(std::memory_order_acquire);
        CasState<T> state = CasState<T>::unpack(state_raw);

        if (state.id && *state.id == tid && state.version == ver) {
            return true;
        }

        if (help) {
            size_t observers = help->count_observers(tid, ver);
            return observers > 1;
        }

        return false;
    }

    bool detect_distributed(ThreadId tid, Version ver, DistributedHelpArray* help) {
        uint64_t state_raw = raw_.load(std::memory_order_acquire);
        CasState<T> state = CasState<T>::unpack(state_raw);

        if (state.id && *state.id == tid && state.version == ver) {
            return true;
        }

        if (help) {
            size_t observers = help->count_observers(tid, ver);
            return observers > 1;
        }

        return false;
    }
};

struct Remote {
    uint16_t free;

    constexpr Remote() : free(0) {}
    constexpr explicit Remote(uint16_t f) : free(f) {}

    constexpr static Remote unpack(uint64_t raw) { return Remote(raw); }
    constexpr static uint64_t pack(const Remote& r) { return r.free; }
};

struct RemoteTrait {
    static constexpr unsigned total_bits = 16;
};

template<typename T>
inline CasState<T> CasState<T>::unpack(uint64_t raw) {
    uint16_t id_raw = raw & 0xFFFF;
    uint16_t ver_raw = (raw >> 16) & 0xFFFF;
    uint32_t inner_raw = (raw >> 32);

    std::optional<ThreadId> id = (id_raw == 0) ? std::nullopt : std::optional<ThreadId>(ThreadId(id_raw - 1));
    Version version(ver_raw);
    T inner = T::unpack(inner_raw);

    return CasState<T>(id, version, inner);
}

template<typename T>
inline uint64_t CasState<T>::pack(const CasState<T>& s) {
    uint16_t id_raw = s.id ? s.id->internal() : 0;
    uint16_t ver_raw = s.version.value_;
    uint32_t inner_raw = T::pack(s.inner);

    return (static_cast<uint64_t>(inner_raw) << 32) |
           (static_cast<uint64_t>(ver_raw) << 16) |
           static_cast<uint64_t>(id_raw);
}

template<typename B>
struct CasState<std::optional<SlabIndex<B>>> {
    std::optional<ThreadId> id;
    Version version;
    std::optional<SlabIndex<B>> inner;

    constexpr CasState() : id(std::nullopt), version(), inner(std::nullopt) {}
    constexpr CasState(std::optional<ThreadId> i, Version v, std::optional<SlabIndex<B>> in)
        : id(i), version(v), inner(in) {}

    static CasState unpack(uint64_t raw) {
        uint16_t id_raw = raw & 0xFFFF;
        uint16_t ver_raw = (raw >> 16) & 0xFFFF;
        uint32_t inner_raw = (raw >> 32);

        std::optional<ThreadId> id = (id_raw == 0) ? std::nullopt : std::optional<ThreadId>(ThreadId(id_raw - 1));
        Version version(ver_raw);
        std::optional<SlabIndex<B>> inner = (inner_raw == 0) ? std::nullopt :
            std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(inner_raw));

        return CasState(id, version, inner);
    }

    static uint64_t pack(const CasState& s) {
        uint16_t id_raw = s.id ? s.id->internal() : 0;
        uint16_t ver_raw = s.version.value_;
        uint32_t inner_raw = s.inner ? s.inner->internal() : 0;

        return (static_cast<uint64_t>(inner_raw) << 32) |
               (static_cast<uint64_t>(ver_raw) << 16) |
               static_cast<uint64_t>(id_raw);
    }
};

}