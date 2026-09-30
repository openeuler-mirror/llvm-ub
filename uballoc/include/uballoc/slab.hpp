// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <optional>
#include <cassert>
#include <functional>
#include <vector>
#include <array>
#include <cstring>
#include <algorithm>

#include "packed.hpp"
#include "thread.hpp"
#include "size.hpp"
#include "data.hpp"
#include "bitset.hpp"
#include "cas.hpp"
#include "cache.hpp"

namespace uballoc {

constexpr size_t SLAB_LOCAL_METADATA_SIZE = 8;

inline std::atomic<uint32_t>& debug_slab_steal_count() {
    static std::atomic<uint32_t> val{0};
    return val;
}

template<typename B>
struct SlabLocal {
    std::atomic<uint32_t> next;
    std::atomic<uint16_t> owner;
    std::atomic<uint8_t> class_;
    typename B::BitSetType free;
    std::atomic<uint8_t> detached;

    std::optional<ThreadId> get_owner() {
        uint16_t o = owner.load(std::memory_order_relaxed);
        if (o == 0) return std::nullopt;
        return ThreadId(o - 1);
    }

    void own(ThreadId id) {
        assert(owner.load(std::memory_order_relaxed) == 0);
        owner.store(id.internal(), std::memory_order_relaxed);
    }

    void steal(ThreadId id) {
        owner.store(id.internal(), std::memory_order_relaxed);
    }

    void disown(ThreadId id) {
        assert(owner.load(std::memory_order_relaxed) == id.internal());
        owner.store(0, std::memory_order_relaxed);
    }
};

template<typename B>
struct SlabRemote {
    Detectable<Remote> meta;
};

template<typename B>
struct Slab {
    std::array<std::array<SlabSlice<B, SlabLocal<B>>, MAX_SEGMENTS>, MAX_PROCESSES> local_slices;
    std::array<std::array<SlabSlice<B, Detectable<Remote>>, MAX_SEGMENTS>, MAX_PROCESSES> remote_slices;
    std::array<size_t, MAX_PROCESSES + 1> cumulative;
    std::array<uint32_t, MAX_PROCESSES> segment_counts;
    // Per-process per-segment slab counts for non-uniform segment sizes.
    std::array<std::array<size_t, MAX_SEGMENTS>, MAX_PROCESSES> seg_slab_counts_;
    int total_processes;
    size_t slabs_per_process_;
    size_t slabs_per_segment_;

    Slab() : total_processes(1), slabs_per_process_(0), slabs_per_segment_(0) {
        cumulative[0] = 0;
        cumulative[1] = 0;
        for (size_t i = 0; i < MAX_PROCESSES; ++i) segment_counts[i] = 0;
        for (size_t i = 0; i < MAX_PROCESSES; ++i)
            for (size_t j = 0; j < MAX_SEGMENTS; ++j)
                seg_slab_counts_[i][j] = 0;
    }

    int find_process(size_t global_idx) const {
        if (slabs_per_process_ > 0) {
            return static_cast<int>(global_idx / slabs_per_process_);
        }
        int lo = 0, hi = total_processes - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (cumulative[mid] <= global_idx) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    }

    // Linear-scan localization of segment from local slab index.
    struct SegLoc { size_t seg; size_t seg_local; };
    SegLoc find_segment(int pid, size_t local_idx) const {
        // Fast path: no per-seg counts registered → uniform division
        if (segment_counts[pid] == 0 || seg_slab_counts_[pid][0] == 0) {
            size_t seg = (slabs_per_segment_ > 0)
                ? local_idx / slabs_per_segment_ : 0;
            size_t seg_local = (slabs_per_segment_ > 0)
                ? local_idx % slabs_per_segment_ : local_idx;
            return {seg, seg_local};
        }
        // Linear scan: accumulate slab counts
        size_t acc = 0;
        size_t seg = 0;
        uint32_t max_seg = segment_counts[pid];
        while (seg < max_seg) {
            size_t count = seg_slab_counts_[pid][seg];
            if (count == 0) count = slabs_per_segment_;
            if (acc + count > local_idx) return {seg, local_idx - acc};
            acc += count;
            ++seg;
        }
        // Beyond known segments → uniform for remaining
        size_t remaining = local_idx - acc;
        size_t full_seg = (slabs_per_segment_ > 0)
            ? remaining / slabs_per_segment_ : 0;
        size_t full_seg_local = (slabs_per_segment_ > 0)
            ? remaining % slabs_per_segment_ : remaining;
        return {seg + full_seg, full_seg_local};
    }

    // Returns global slab index of the first slab in segment `seg` for process `pid`.
    size_t segment_global_start(int pid, size_t seg) const {
        if (seg == 0) return cumulative[pid];
        size_t acc = cumulative[pid];
        uint32_t max_seg = std::min(static_cast<uint32_t>(seg), segment_counts[pid]);
        for (uint32_t i = 0; i < max_seg; ++i) {
            size_t count = seg_slab_counts_[pid][i];
            if (count == 0) count = slabs_per_segment_;
            acc += count;
        }
        if (seg > max_seg) acc += (seg - max_seg) * slabs_per_segment_;
        return acc;
    }

    SlabLocal<B>& local(SlabIndex<B> global_idx) {
        int pid = find_process(global_idx.get());
        size_t local_idx = global_idx.get() - cumulative[pid];
        auto [seg, seg_local] = find_segment(pid, local_idx);
        return local_slices[pid][seg][SlabIndex<B>(seg_local)];
    }

    const SlabLocal<B>& local(SlabIndex<B> global_idx) const {
        int pid = find_process(global_idx.get());
        size_t local_idx = global_idx.get() - cumulative[pid];
        auto [seg, seg_local] = find_segment(pid, local_idx);
        return local_slices[pid][seg][SlabIndex<B>(seg_local)];
    }

    Detectable<Remote>& remote(SlabIndex<B> global_idx) {
        int pid = find_process(global_idx.get());
        size_t local_idx = global_idx.get() - cumulative[pid];
        auto [seg, seg_local] = find_segment(pid, local_idx);
        return remote_slices[pid][seg][SlabIndex<B>(seg_local)];
    }

    const Detectable<Remote>& remote(SlabIndex<B> global_idx) const {
        int pid = find_process(global_idx.get());
        size_t local_idx = global_idx.get() - cumulative[pid];
        auto [seg, seg_local] = find_segment(pid, local_idx);
        return remote_slices[pid][seg][SlabIndex<B>(seg_local)];
    }

    void register_segment(int pid, size_t seg_idx,
                           SlabLocal<B>* local_base,
                           Detectable<Remote>* remote_base) {
        local_slices[pid][seg_idx] = SlabSlice<B, SlabLocal<B>>(local_base);
        remote_slices[pid][seg_idx] = SlabSlice<B, Detectable<Remote>>(remote_base);
        if (seg_idx + 1 > segment_counts[pid]) {
            segment_counts[pid] = seg_idx + 1;
        }
    }

    void register_segment(int pid, size_t seg_idx,
                           SlabLocal<B>* local_base,
                           Detectable<Remote>* remote_base,
                           size_t slab_count) {
        register_segment(pid, seg_idx, local_base, remote_base);
        seg_slab_counts_[pid][seg_idx] = slab_count;
    }

    SlabSlice<B, SlabLocal<B>>& local_slice(int pid, size_t seg) { return local_slices[pid][seg]; }
    SlabSlice<B, Detectable<Remote>>& remote_slice(int pid, size_t seg) { return remote_slices[pid][seg]; }

    void set_cumulative(const size_t* cum, int n) {
        total_processes = n;
        for (int i = 0; i <= n; ++i) {
            cumulative[i] = cum[i];
        }
    }

    void set_segment_params(size_t slabs_per_proc, size_t slabs_per_seg) {
        slabs_per_process_ = slabs_per_proc;
        slabs_per_segment_ = slabs_per_seg;
    }

    void link(ThreadId id, SlabIndex<B> start, SlabIndex<B> end, std::optional<SlabIndex<B>> head) {
        for (uint32_t i = start.get(); i < end.get(); ++i) {
            SlabIndex<B> idx(i);
            uint32_t next_val;
            if (i + 1 < end.get()) {
                next_val = SlabIndex<B>(i + 1).internal();
            } else if (head) {
                next_val = head->internal();
            } else {
                next_val = 0;
            }

            SlabLocal<B>& l = local(idx);
            l.own(id);
            l.next.store(next_val, std::memory_order_relaxed);
            flush(&l.next, Invalidate::No);
        }
    }

    auto trace(std::optional<SlabIndex<B>> head) {
        std::vector<SlabIndex<B>> result;
        std::optional<SlabIndex<B>> current = head;
        while (current) {
            result.push_back(*current);
            uint32_t next = local(*current).next.load(std::memory_order_relaxed);
            current = (next > 0) ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(next)) : std::nullopt;
        }
        return result;
    }
};

template<typename B>
class SlabStackLocal {
private:
    std::optional<SlabIndex<B>> head_;
    size_t len_;

public:
    SlabStackLocal() : head_(std::nullopt), len_(0) {}

    std::optional<SlabIndex<B>> peek() const { return head_; }
    size_t len() const { return len_; }

    void set(std::optional<SlabIndex<B>> head, size_t len) {
        head_ = head;
        flush(&head_, Invalidate::No);
        len_ = len;
    }

    std::optional<SlabIndex<B>> pop(Slab<B>& slabs) {
        while (head_) {
            SlabIndex<B> h = *head_;
            SlabLocal<B>& l = slabs.local(h);
            if (l.detached.load(std::memory_order_relaxed) != 0) {
                uint32_t next = l.next.load(std::memory_order_relaxed);
                head_ = (next > 0) ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(next)) : std::nullopt;
                flush(&head_, Invalidate::No);
                len_--;
                continue;
            }
            uint32_t next = slabs.local(h).next.load(std::memory_order_relaxed);
            head_ = (next > 0) ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(next)) : std::nullopt;
            flush(&head_, Invalidate::No);
            len_--;
            return h;
        }
        return std::nullopt;
    }

    void push(Slab<B>& slabs, SlabIndex<B> idx) {
        SlabLocal<B>& l = slabs.local(idx);
        l.next.store(head_ ? head_->internal() : 0, std::memory_order_relaxed);
        flush(&l.next, Invalidate::No);
        fence();
        head_ = idx;
        flush(&head_, Invalidate::No);
        len_++;
    }

    void recover_push(Slab<B>& slabs, SlabIndex<B> idx) {
        if (head_ != idx) {
            push(slabs, idx);
        }
        recover_len(slabs);
    }

    void recover_len(Slab<B>& slabs) {
        len_ = trace(slabs).size();
    }

    auto trace(Slab<B>& slabs) {
        std::vector<SlabIndex<B>> result;
        std::optional<SlabIndex<B>> current = head_;
        while (current) {
            result.push_back(*current);
            uint32_t next = slabs.local(*current).next.load(std::memory_order_relaxed);
            current = (next > 0) ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(next)) : std::nullopt;
        }
        return result;
    }

    bool is_valid(Slab<B>& slabs) {
        size_t count = 0;
        for (auto idx : trace(slabs)) {
            count++;
            if (count > len_) return false;
        }
        return count == len_;
    }
};

template<typename B>
struct SlabStackGlobal {
    Detectable<std::optional<SlabIndex<B>>> head;

    void push([[maybe_unused]] ThreadId id, Slab<B>& slabs, SlabIndex<B> head_idx, SlabIndex<B> tail_idx) {
        uint64_t old_raw;
        uint64_t desired_raw;
        do {
            old_raw = head.raw_.load(std::memory_order_acquire);
            auto old_state = CasState<std::optional<SlabIndex<B>>>::unpack(old_raw);

            uint32_t next_val = old_state.inner ? old_state.inner->internal() : 0;
            slabs.local(tail_idx).next.store(next_val, std::memory_order_relaxed);
            flush(&slabs.local(tail_idx).next, Invalidate::No);

            uint16_t new_version = old_state.version.value_ + 1;
            CasState<std::optional<SlabIndex<B>>> desired{id, Version(new_version), std::optional<SlabIndex<B>>(head_idx)};
            desired_raw = CasState<std::optional<SlabIndex<B>>>::pack(desired);
        } while (!head.raw_.compare_exchange_weak(old_raw, desired_raw,
                 std::memory_order_acq_rel, std::memory_order_relaxed));
    }

    std::optional<SlabIndex<B>> pop([[maybe_unused]] ThreadId id, Slab<B>& slabs) {
        while (true) {
            uint64_t old_raw;
            uint64_t new_raw;
            SlabIndex<B> idx{0};

            do {
                old_raw = head.raw_.load(std::memory_order_acquire);
                auto old_state = CasState<std::optional<SlabIndex<B>>>::unpack(old_raw);

                if (!old_state.inner) return std::nullopt;

                idx = *old_state.inner;
                uint32_t next = slabs.local(idx).next.load(std::memory_order_relaxed);
                std::optional<SlabIndex<B>> new_inner = (next > 0) ?
                    std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(next)) : std::nullopt;

                uint16_t new_version = old_state.version.value_ + 1;
                CasState<std::optional<SlabIndex<B>>> desired{id, Version(new_version), new_inner};
                new_raw = CasState<std::optional<SlabIndex<B>>>::pack(desired);
            } while (!head.raw_.compare_exchange_weak(old_raw, new_raw,
                     std::memory_order_acq_rel, std::memory_order_relaxed));

            if (slabs.local(idx).detached.load(std::memory_order_relaxed) == 0) {
                return idx;
            }
        }
    }

    bool is_empty([[maybe_unused]] ThreadId id) {
        uint64_t raw = head.raw_.load(std::memory_order_acquire);
        auto state = CasState<std::optional<SlabIndex<B>>>::unpack(raw);
        return !state.inner;
    }

    bool detect(ThreadId id, Version version, HelpArray* help) {
        return head.detect(id, version, help);
    }
};

}
