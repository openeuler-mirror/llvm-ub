// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <optional>
#include <cassert>
#include <vector>
#include <algorithm>
#include <set>
#include <memory>

#include "packed.hpp"
#include "thread.hpp"
#include "size.hpp"
#include "data.hpp"
#include "bitset.hpp"
#include "slab.hpp"
#include "cas.hpp"
#include "cache.hpp"
#include "stat.hpp"
#include "log.hpp"
#include "segment.hpp"
#include "reclaim_mode.hpp"
#include <chrono>

struct RecoverState;

#include "recover.hpp"

namespace uballoc {

inline std::atomic<size_t>& count_cache_slab() {
    static std::atomic<size_t> val{0};
    return val;
}

inline std::atomic<size_t>& batch_global_push() {
    static std::atomic<size_t> val{1};
    return val;
}

inline std::atomic<size_t>& batch_bump_pop();

template<typename B>
struct HeapShared {
    SlabStackGlobal<B> free;
    std::atomic<uint64_t> bump_raw;
    SlabStackGlobal<B>* cross_free_ptrs_[MAX_PROCESSES];
    int total_processes_;
    int rank_;
    size_t bump_start_;
    std::atomic<size_t> bump_end_atomic_;
    std::atomic<uint32_t> growing_;
    std::atomic<uint32_t> segment_count_;
    char _pad[40];

    HeapShared() : total_processes_(1), rank_(0), bump_start_(0),
                   bump_end_atomic_(0), growing_(0), segment_count_(1) {
        for (int i = 0; i < MAX_PROCESSES; ++i) {
            cross_free_ptrs_[i] = nullptr;
        }
    }

    void set_cross_free(int pid, SlabStackGlobal<B>* ptr) {
        cross_free_ptrs_[pid] = ptr;
    }

    void set_bump_bounds(size_t start, size_t end) {
        bump_start_ = start;
        bump_end_atomic_.store(end, std::memory_order_release);
    }

    std::optional<SlabIndex<B>> bump_load() {
        uint64_t raw = bump_raw.load(std::memory_order_relaxed);
        if (raw == 0) return std::nullopt;
        return SlabIndex<B>::unpack(raw);
    }

    void bump_store(std::optional<SlabIndex<B>> idx) {
        bump_raw.store(idx ? idx->internal() : 0, std::memory_order_relaxed);
    }

    void init_bump() {
        bump_raw.store(SlabIndex<B>(bump_start_).internal(), std::memory_order_relaxed);
    }

    void init_growth(uint32_t seg_count) {
        growing_.store(0, std::memory_order_relaxed);
        segment_count_.store(seg_count, std::memory_order_relaxed);
    }

    bool detect_bump(ThreadId id, Version version) {
        uint64_t raw = bump_raw.load(std::memory_order_acquire);
        auto state = CasState<std::optional<SlabIndex<B>>>::unpack(raw);
        return state.id && *state.id == id && state.version == version;
    }

    bool detect_global(ThreadId id, Version version) {
        return free.head.detect(id, version);
    }

    std::optional<SlabIndex<B>> pop(ThreadId id, Slab<B>& slabs,
                                     bool allow_cross_process = true) {
        if (!free.is_empty(id)) {
            return free.pop(id, slabs);
        }

        if (!allow_cross_process) return std::nullopt;

        for (int j = 0; j < total_processes_; ++j) {
            if (j == rank_ || !cross_free_ptrs_[j]) continue;
            if (!cross_free_ptrs_[j]->is_empty(id)) {
                auto result = cross_free_ptrs_[j]->pop(id, slabs);
                if (result) return result;
            }
        }

        return std::nullopt;
    }

    void push(ThreadId id, Slab<B>& slabs, SlabIndex<B> head, SlabIndex<B> tail) {
        free.push(id, slabs, head, tail);
    }

    struct BumpRange {
        SlabIndex<B> start;
        SlabIndex<B> end;
    };

    std::optional<BumpRange> bump([[maybe_unused]] ThreadId id, [[maybe_unused]] Version version, size_t slab_capacity) {
        (void)slab_capacity;
        uint32_t batch = batch_bump_pop().load(std::memory_order_relaxed);

        uint64_t old_raw = bump_raw.fetch_add(batch, std::memory_order_acq_rel);
        SlabIndex<B> start = SlabIndex<B>::unpack(old_raw);
        SlabIndex<B> end = start.unchecked_add(batch);

        size_t effective_end = bump_end_atomic_.load(std::memory_order_acquire);
        if (end.get() > effective_end) return std::nullopt;

        return BumpRange{start, end};
    }

    bool try_grow(GrowFn fn) {
        uint32_t state = growing_.load(std::memory_order_acquire);
        if (state == 2) return false;

        uint32_t expected = 0;
        if (growing_.compare_exchange_strong(expected, 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            bool success = fn && fn();
            growing_.store(success ? 0u : 2u, std::memory_order_release);
            return success;
        } else {
            while (growing_.load(std::memory_order_acquire) == 1) {
                __asm__ __volatile__("" ::: "memory");
            }
            return growing_.load(std::memory_order_acquire) == 0;
        }
    }
};

template<typename B, size_t Count>
struct SizeArray {
    std::array<SlabStackLocal<B>, Count> inner;

    SlabStackLocal<B>& operator[](B class_) {
        return inner[class_.index()];
    }

    const SlabStackLocal<B>& operator[](B class_) const {
        return inner[class_.index()];
    }

    auto iter() {
        return inner;
    }
};

template<typename B>
struct HeapOwned {
    SlabStackLocal<B> unsized;
    SizeArray<B, B::COUNT> sized;
    RecoverState state;

    void clear() {
        unsized.set(std::nullopt, 0);
        for (size_t i = 0; i < B::COUNT; ++i) {
            sized.inner[i].set(std::nullopt, 0);
        }
        state.clear();
    }

    bool is_valid(ThreadId id, Slab<B>& slabs) {
        std::set<uint32_t> seen;

        if (!unsized.is_valid(slabs)) return false;

        for (auto idx : unsized.trace(slabs)) {
            if (seen.count(idx.get())) return false;
            seen.insert(idx.get());

            auto owner = slabs.local(idx).get_owner();
            if (!owner || *owner != id) return false;
        }

        for (size_t i = 0; i < B::COUNT; ++i) {
            auto class_ = B::from_index(i);
            if (class_ && !class_->is_zero()) {
                if (!sized[*class_].is_valid(slabs)) return false;

                for (auto idx : sized[*class_].trace(slabs)) {
                    if (seen.count(idx.get())) return false;
                    seen.insert(idx.get());

                    auto& local = slabs.local(idx);
                    if (local.free.is_empty()) return false;
                    if (local.class_.load(std::memory_order_relaxed) != i) return false;

                    auto owner = local.get_owner();
                    if (!owner || *owner != id) return false;
                }
            }
        }

        return true;
    }

    bool unsized_to_sized(ThreadId id, [[maybe_unused]] RecoverState& state, Slab<B>& slabs, B class_) {
        auto idx = unsized.peek();
        if (!idx) return false;

        LOG_INFO("Transfer: " << idx->get() << " from unsized to sized (" << class_.size() << ")");

        SlabLocal<B>& local = slabs.local(*idx);

        uint32_t unsized_next = local.next.load(std::memory_order_relaxed);

        local.class_.store(class_.index(), std::memory_order_relaxed);
        local.free.fill(class_.count());

        flush(&local, Invalidate::No);

        sized[class_].push(slabs, *idx);

        auto& remote = slabs.remote(*idx);
        remote.raw_.store(CasState<Remote>::pack(CasState<Remote>{std::optional<ThreadId>(id), Version(), Remote{static_cast<uint16_t>(class_.count())}}), std::memory_order_relaxed);

        unsized.set((unsized_next > 0) ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(unsized_next)) : std::nullopt, unsized.len() - 1);

        return true;
    }

    void sized_to_unsized([[maybe_unused]] ThreadId id, Slab<B>& slabs, B class_, SlabIndex<B> index) {
        uint32_t next = slabs.local(index).next.load(std::memory_order_relaxed);

        auto peek = sized[class_].peek();
        if (peek && *peek == index) {
            sized[class_].set((next > 0) ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(next)) : std::nullopt, sized[class_].len() - 1);
        } else {
            auto walk = peek;
            while (walk) {
                auto& local = slabs.local(*walk);
                uint32_t walk_next = local.next.load(std::memory_order_relaxed);
                if (walk_next == index.internal()) {
                    local.next.store(next, std::memory_order_relaxed);
                    flush(&local, Invalidate::No);
                    break;
                }
                walk = (walk_next > 0) ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(walk_next)) : std::nullopt;
            }
        }

        unsized.push(slabs, index);
    }
};

template<typename B>
struct Heap {
    HeapShared<B>* shared;
    HeapOwned<B>* owned;
    Slab<B>* slabs;
    Data<B>* data;
    std::unique_ptr<stat::thread::Recorder<B>> stat_recorder;

    Heap() : shared(nullptr), owned(nullptr), slabs(nullptr), data(nullptr),
             stat_recorder(std::make_unique<stat::thread::Recorder<B>>()) {}
    Heap(HeapShared<B>* sh, HeapOwned<B>* ow, Slab<B>* sl, Data<B>* dt)
        : shared(sh), owned(ow), slabs(sl), data(dt),
          stat_recorder(std::make_unique<stat::thread::Recorder<B>>()) {}
    Heap(Heap&& other) noexcept
        : shared(other.shared), owned(other.owned), slabs(other.slabs),
          data(other.data), stat_recorder(std::move(other.stat_recorder)) {
        other.shared = nullptr;
        other.owned = nullptr;
        other.slabs = nullptr;
        other.data = nullptr;
    }
    Heap& operator=(Heap&& other) noexcept {
        shared = other.shared;
        owned = other.owned;
        slabs = other.slabs;
        data = other.data;
        stat_recorder = std::move(other.stat_recorder);
        other.shared = nullptr;
        other.owned = nullptr;
        other.slabs = nullptr;
        other.data = nullptr;
        return *this;
    }

    std::optional<Offset<B>> checked_pointer_to_offset(void* ptr) {
        return data->checked_pointer_to_offset(ptr);
    }

    B get_class(Offset<B> offset) {
        SlabIndex<B> idx = data->into_index(offset);
        uint8_t c = slabs->local(idx).class_.load(std::memory_order_relaxed);
        auto result = B::from_index(c);
        return result ? *result : B();
    }

    void* pop(ThreadId id, B class_, SlabIndex<B> idx, Bit block) {
        SlabLocal<B>& local = slabs->local(idx);

        // Reclaim safety (see delete_segment race analysis): (idx, block)
        // may be stale if the segment was reclaimed between peek() and
        // pop() — the caller may even have been preempted in between.
        //   - Old device content (fault-handler rescued): detached==1.
        //   - Anon zero-page window: detached==0 but the bit is cleared.
        // Either way the slab is being reclaimed → return nullptr and let
        // the caller retry peek() with a fresh slab.
        if (local.detached.load(std::memory_order_acquire) != 0) return nullptr;
        if ((local.free.dense[block.row] & (1ULL << block.col)) == 0) return nullptr;

        stat_recorder->record(id, stat::thread::EventType::Allocate, class_.size(), class_);

        // Track bytes allocated to app
        if (uballoc::reclaim_stats_enabled().load(std::memory_order_relaxed)) {
            uballoc::stats_allocated_to_app().fetch_add(
                class_.size(), std::memory_order_relaxed);
        }

        assert((local.free.dense[block.row] & (1ULL << block.col)) != 0);

        size_t count_before = local.free.len();
        local.free.unset(block);

        if (count_before == class_.count()) {
            SegmentHeader* hdr = slab_segment_header(*slabs, idx);
            if (hdr) {
                uint32_t prev = hdr->live_slab_count.fetch_add(1, std::memory_order_acq_rel);
                if (prev == 0) {
                    hdr->free_since_ns.store(0, std::memory_order_release);
                }
            }
        }

        if (local.free.is_empty()) {
            owned->sized[class_].pop(*slabs);
            detach(id, class_, idx);
        }

        Offset<B> offset = data->from_block(class_, idx, block);
        return data->template offset_to_pointer<char>(offset);
    }

    std::optional<std::pair<SlabIndex<B>, Bit>> peek(ThreadId id, B class_) {
        while (true) {
            auto idx = owned->sized[class_].peek();
            if (!idx) {
                idx = allocate(id, class_);
                if (!idx) return std::nullopt;
            }

            SlabLocal<B>& local = slabs->local(*idx);
            // Reclaim safety (see delete_segment race analysis):
            //  - detached==1: segment was reclaimed; purge stale head, retry
            //  - detached==0 but the peeked free-bit is not set: transient
            //    zero-page window of a segment being reclaimed (anon remap
            //    happened, detached=1 store not yet observed) — treat as
            //    stale, purge, retry. For a healthy slab the bit from
            //    peek_unchecked is always set, so this never misfires.
            if (local.detached.load(std::memory_order_acquire) != 0 ||
                (local.free.dense[local.free.peek_unchecked().row] &
                 (1ULL << local.free.peek_unchecked().col)) == 0) {
                owned->sized[class_].pop(*slabs);  // purge stale head
                continue;
            }
            Bit block = local.free.peek_unchecked();
            return std::make_pair(*idx, block);
        }
    }

    std::optional<SlabIndex<B>> allocate(ThreadId id, B class_) {
        if (class_.is_zero()) return std::nullopt;

        if (reclaim_mode() == ReclaimMode::SYNC) {
            if constexpr (std::is_same_v<B, Small>) {
                auto fn = reclaim_fn_small().load(std::memory_order_acquire);
                if (fn) fn();
            } else if constexpr (std::is_same_v<B, Large>) {
                auto fn = reclaim_fn_large().load(std::memory_order_acquire);
                if (fn) fn();
            }
        }

        if (owned->unsized_to_sized(id, owned->state, *slabs, class_)) {
            stat_recorder->record(id, stat::thread::EventType::UnsizedToSized, 0, class_);
            return owned->sized[class_].peek();
        }

        bool reuse_first = (affinity_mode().load(std::memory_order_acquire) ==
                            static_cast<size_t>(AffinityMode::ReuseFirst));

        auto global_idx = shared->pop(id, *slabs,
                                      /*allow_cross_process=*/reuse_first);
        if (global_idx) {
            stat_recorder->record(id, stat::thread::EventType::GlobalToUnsized);
            LOG_INFO("Transfer: " << global_idx->get() << " from global to unsized");

            slabs->local(*global_idx).steal(id);
            owned->unsized.push(*slabs, *global_idx);
        } else {
            auto range = shared->bump(id, Version(), data->slab_capacity_);
            if (!range) {
                auto check_fn = check_remote_fn().load(std::memory_order_acquire);
                if (check_fn) check_fn();

                GrowFn fn = nullptr;
                if constexpr (std::is_same_v<B, Small>) fn = grow_fn_small().load(std::memory_order_acquire);
                else if constexpr (std::is_same_v<B, Large>) fn = grow_fn_large().load(std::memory_order_acquire);
                if (fn && shared->try_grow(fn)) {
                    range = shared->bump(id, Version(), data->slab_capacity_);
                }
            }

            if (!range && !reuse_first) {
                global_idx = shared->pop(id, *slabs,
                                         /*allow_cross_process=*/true);
                if (global_idx) {
                    stat_recorder->record(id, stat::thread::EventType::GlobalToUnsized);
                    LOG_INFO("Transfer: " << global_idx->get()
                             << " from global to unsized (borrow after growth)");

                    slabs->local(*global_idx).steal(id);
                    owned->unsized.push(*slabs, *global_idx);
                }
            }

            if (!range && !global_idx) return std::nullopt;

            if (range) {
                stat_recorder->record(id, stat::thread::EventType::Bump);
                LOG_INFO("Transfer: " << range->start.get() << ".." << range->end.get() << " from bump to unsized");

                uint32_t batch = batch_bump_pop().load(std::memory_order_relaxed);
                slabs->link(id, range->start, range->end, std::nullopt);
                owned->unsized.set(range->start, batch);
            }
        }

        owned->unsized_to_sized(id, owned->state, *slabs, class_);
        stat_recorder->record(id, stat::thread::EventType::UnsizedToSized, 0, class_);

        return owned->sized[class_].peek();
    }

    void detach(ThreadId id, B class_, SlabIndex<B> idx) {
        stat_recorder->record(id, stat::thread::EventType::Detach, 0, class_);

        auto& remote = slabs->remote(idx);
        uint64_t remote_raw = remote.raw_.load(std::memory_order_relaxed);
        auto meta = CasState<Remote>::unpack(remote_raw);

        if (meta.inner.free < class_.count()) {
            stat_recorder->record(id, stat::thread::EventType::Disown, 0, class_);
            LOG_INFO("Transfer: disown " << idx.get() << " (" << class_.size() << ")");

            auto& local = slabs->local(idx);
            local.disown(id);
        } else {
            LOG_INFO("Transfer: detach " << idx.get() << " (" << class_.size() << ")");
        }
    }

    void attach(ThreadId id, B class_, SlabIndex<B> idx) {
        stat_recorder->record(id, stat::thread::EventType::Attach, 0, class_);
        LOG_INFO("Transfer: attach " << idx.get() << " (" << class_.size() << ")");

        owned->sized[class_].push(*slabs, idx);
    }

    void free_offset(ThreadId id, Offset<B> offset) {
        SlabIndex<B> idx = data->into_index(offset);

        int pid = slabs->find_process(idx.get());
        size_t local_idx = idx.get() - slabs->cumulative[pid];
        size_t seg = (slabs->slabs_per_segment_ > 0)
            ? local_idx / slabs->slabs_per_segment_ : 0;

        if (slabs->local_slice(pid, seg).base_ == nullptr) {
            auto check_fn = check_remote_fn().load(std::memory_order_acquire);
            if (check_fn) check_fn();
            if (slabs->local_slice(pid, seg).base_ == nullptr) {
                LOG_WARN("free_offset: slab segment not attached (pid=" << pid
                         << " seg=" << seg << "), skipping free");
                return;
            }
        }

        SlabLocal<B>& local = slabs->local(idx);

        uint16_t expected = id.internal();
        if (local.owner.compare_exchange_strong(expected, expected,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            free_local(id, idx, offset);
        } else {
            free_remote(id, idx);
        }

        if (reclaim_mode() == ReclaimMode::SYNC) {
            if constexpr (std::is_same_v<B, Small>) {
                auto fn = reclaim_fn_small().load(std::memory_order_acquire);
                if (fn) fn();
            } else if constexpr (std::is_same_v<B, Large>) {
                auto fn = reclaim_fn_large().load(std::memory_order_acquire);
                if (fn) fn();
            }
        }
    }

    void free_local(ThreadId id, SlabIndex<B> idx, Offset<B> offset) {
        B class_ = get_class(offset);
        Bit block = data->into_block(offset, class_);

        stat_recorder->record(id, stat::thread::EventType::Free, class_.size(), class_);

        // Track bytes freed from app
        if (uballoc::reclaim_stats_enabled().load(std::memory_order_relaxed)) {
            uballoc::stats_freed_from_app().fetch_add(
                class_.size(), std::memory_order_relaxed);
        }

        SlabLocal<B>& local = slabs->local(idx);
        local.free.set(block);
        size_t count = local.free.len();

        if (count == 1) {
            attach(id, class_, idx);
        }

        if (count < class_.count()) return;

        owned->sized_to_unsized(id, *slabs, class_, idx);
        stat_recorder->record(id, stat::thread::EventType::SizedToUnsized, 0, class_);
        LOG_INFO("Transfer: " << idx.get() << " from sized (" << class_.size() << ") to unsized");

        SegmentHeader* hdr = slab_segment_header(*slabs, idx);
        if (hdr) {
            uint32_t prev = hdr->live_slab_count.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                auto tp = std::chrono::steady_clock::now().time_since_epoch();
                uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count();
                hdr->free_since_ns.store(now_ns, std::memory_order_release);
                auto trigger = reclaim_trigger_fn().load(std::memory_order_acquire);
                if (trigger) trigger();
            }
        }

        unsized_to_global(id);
    }

    void free_remote(ThreadId id, SlabIndex<B> idx) {
        uint8_t class_raw = slabs->local(idx).class_.load(std::memory_order_relaxed);
        B class_ = B::from_index(class_raw).value_or(B());

        stat_recorder->record(id, stat::thread::EventType::Free, class_.size(), class_);

        // Track bytes freed from app (remote free path)
        if (class_.size() > 0 && uballoc::reclaim_stats_enabled().load(std::memory_order_relaxed)) {
            uballoc::stats_freed_from_app().fetch_add(
                class_.size(), std::memory_order_relaxed);
        }

        auto& remote = slabs->remote(idx);

        bool done = false;
        bool reclaimed = false;
        while (!done) {
            uint64_t remote_raw = remote.raw_.load(std::memory_order_acquire);
            auto meta = CasState<Remote>::unpack(remote_raw);

            if (meta.inner.free > 0) {
                Remote new_meta{static_cast<uint16_t>(meta.inner.free - 1)};
                uint64_t new_raw = CasState<Remote>::pack(CasState<Remote>{std::optional<ThreadId>(id), Version(), new_meta});

                if (remote.raw_.compare_exchange_weak(remote_raw, new_raw,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    done = true;
                    if (new_meta.free == 0) {
                        reclaimed = true;
                    }
                }
            } else {
                done = true;
            }
        }

        if (reclaimed) {
            LOG_INFO("Reclaim: slab " << idx.get() << " fully freed remotely");
            slabs->local(idx).free.fill(class_.count());
            shared->push(id, *slabs, idx, idx);

            SegmentHeader* hdr = slab_segment_header(*slabs, idx);
            if (hdr) {
                uint32_t prev = hdr->live_slab_count.fetch_sub(1, std::memory_order_acq_rel);
                if (prev == 1) {
                    auto tp = std::chrono::steady_clock::now().time_since_epoch();
                    uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count();
                    hdr->free_since_ns.store(now_ns, std::memory_order_release);
                    auto trigger = reclaim_trigger_fn().load(std::memory_order_acquire);
                    if (trigger) trigger();
                }
            }
        }
    }

    void claim(ThreadId id, B class_, SlabIndex<B> idx) {
        stat_recorder->record(id, stat::thread::EventType::Claim, 0, class_);

        slabs->local(idx).steal(id);
        owned->unsized.push(*slabs, idx);

        unsized_to_global(id);
    }

    void unsized_to_global(ThreadId id) {
        size_t count = owned->unsized.len();
        if (count <= count_cache_slab().load(std::memory_order_relaxed)) return;

        stat_recorder->record(id, stat::thread::EventType::UnsizedToGlobal);

        size_t batch = batch_global_push().load(std::memory_order_relaxed);
        auto trace_vec = owned->unsized.trace(*slabs);

        if (trace_vec.empty()) return;

        size_t take = std::min(batch, trace_vec.size());
        SlabIndex<B> head = trace_vec[0];
        SlabIndex<B> tail = trace_vec[take - 1];

        LOG_INFO("Transfer: " << head.get() << "..=" << tail.get() << " from unsized to global");

        uint32_t next = slabs->local(tail).next.load(std::memory_order_relaxed);
        owned->unsized.set((next > 0) ? std::optional<SlabIndex<B>>(SlabIndex<B>::unpack(next)) : std::nullopt,
                          count - batch);

        shared->push(id, *slabs, head, tail);
    }

    std::vector<stat::Report> report(ThreadId id) {
        return stat_recorder->report(id);
    }
};

}
