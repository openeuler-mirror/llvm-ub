// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <array>
#include <type_traits>
#include <unistd.h>

#include "allocator.hpp"
#include "allocator_types.hpp"
#include "thread.hpp"
#include "heap.hpp"
#include "huge.hpp"
#include "shared_layout.hpp"
#include "distributed_backend.hpp"
#include "published.hpp"
#include "fault_handler.hpp"
#include "uffd_handler.hpp"
#include "reclaim_mode.hpp"
#include "reclaim_thread.hpp"

namespace uballoc {

// Defrag hint — a live allocation in a low-occupancy memory region.
struct DefragHint {
    void* ptr;
    size_t size;
    double occupancy;
};

template<typename BackendT = DistributedShmBackend<>>
class GlobalAllocator {
private:
    static GlobalAllocator<BackendT>* instance_;

    BackendT backend_;

    std::array<typename BackendT::AllocatorShared*, MAX_PROCESSES> distributed_shared_ptrs_;
    std::array<HeapShared<Small>*, MAX_PROCESSES> small_shared_ptrs_;
    std::array<HeapShared<Large>*, MAX_PROCESSES> large_shared_ptrs_;
    std::array<HugeShared*, MAX_PROCESSES> huge_shared_ptrs_;
    std::array<ThreadArray<AllocatorOwned>*, MAX_PROCESSES> distributed_owned_arrays_;

    std::array<HelpArray*, MAX_PROCESSES> help_array_ptrs_;
    std::array<std::atomic<uint16_t>*, MAX_PROCESSES> help_data_ptrs_;

    HeapOwned<Small> small_owned_[COUNT_THREAD];
    HeapOwned<Large> large_owned_[COUNT_THREAD];

    std::mutex init_mutex_;
    bool initialized_;

    Slab<Small> small_slab_;
    Data<Small> small_data_;
    Slab<Large> large_slab_;
    Data<Large> large_data_;
    Data<HugeSize> huge_data_;

    std::array<size_t, MAX_PROCESSES + 1> cumulative_small_;
    std::array<size_t, MAX_PROCESSES + 1> cumulative_large_;
    int total_processes_;
    int rank_;
    size_t num_cores_;

    char* data_huge_base_;
    std::array<char*, MAX_PROCESSES> data_huge_base_ptrs_;

    std::array<PublishedRegistry*, MAX_PROCESSES> published_registry_ptrs_;
    TypeIdMap* type_id_map_;

    std::unique_ptr<ReclaimThread> reclaim_thread_;
    pid_t reclaim_creator_pid_ = 0;

    static std::atomic<uint16_t> g_next_thread_id_;

    static thread_local uint16_t tl_thread_id_;
    static thread_local Allocator<void, void>* tl_allocator_ptr_;
    static thread_local bool tl_thread_initialized_;

public:
    GlobalAllocator() : initialized_(false),
                        total_processes_(1), rank_(0),
                        num_cores_(MAX_NUM_CORES),
                        data_huge_base_(nullptr),
                        type_id_map_(nullptr) {
        instance_ = this;
        for (size_t i = 0; i < MAX_PROCESSES; ++i) {
            distributed_shared_ptrs_[i] = nullptr;
            small_shared_ptrs_[i] = nullptr;
            large_shared_ptrs_[i] = nullptr;
            huge_shared_ptrs_[i] = nullptr;
            distributed_owned_arrays_[i] = nullptr;
            help_array_ptrs_[i] = nullptr;
            help_data_ptrs_[i] = nullptr;
            published_registry_ptrs_[i] = nullptr;
            data_huge_base_ptrs_[i] = nullptr;
        }
        cumulative_small_[0] = 0;
        cumulative_small_[1] = 0;
        cumulative_large_[0] = 0;
        cumulative_large_[1] = 0;
    }

    static GlobalAllocator<BackendT>& get() {
        if (!instance_) {
            static GlobalAllocator<BackendT> inst;
            instance_ = &inst;
        }
        return *instance_;
    }

    // -----------------------------------------------------------------------
    // init() is the primary entry point. Parameter-free: reads UBALLOC_HEAP_ID
    // from the environment, discovers rank and membership at runtime via the
    // bootstrap PID table. Idempotent — calling it again after initialization
    // is a no-op (just ensures thread init).
    //
    // Contract: applications MUST call uballoc::init() before any direct
    // access to remote shared memory. malloc/free/lookup_by_type auto-call
    // init() on first use as a safety net, but explicit init() is the
    // recommended pattern for code that touches remote shms before any
    // allocator call.
    // -----------------------------------------------------------------------
    void init() {
        if (initialized_) {
            ensure_thread_init();
            return;
        }
        std::lock_guard<std::mutex> lock(init_mutex_);
        if (initialized_) {
            ensure_thread_init();
            return;
        }

        const char* heap_id_env = std::getenv("UBALLOC_HEAP_ID");
        if (!heap_id_env || !*heap_id_env) {
            LOG_WARN("init: UBALLOC_HEAP_ID env var not set");
            return;
        }
        do_lazy_init(std::string(heap_id_env));
    }

    void do_lazy_init(const std::string& heap_id) {
        BackendT::ShmProvider::provider_init();

        auto join_res = backend_.bootstrap_join(heap_id);
        if (is_err(join_res)) {
            return;
        }
        auto [va_base, reserved, is_p0] = unwrap(join_res);

        // R7: Cache local node slot_id AFTER bootstrap_join (which
        // initializes the UBSE runtime via shm_create). claim_rank uses
        // this for same-node vs cross-node dead-slot detection.
        // For POSIX (is_single_node), local_node_slot_id_ stays at 0.
        if constexpr (!BackendT::ShmProvider::is_single_node) {
            BackendT::ShmProvider::cache_local_node_slot_id();
            backend_.local_node_slot_id_ = BackendT::ShmProvider::get_local_node_slot_id();
        }

        if (!is_p0) {
            if (g_early_va.reserved && g_early_va.base != va_base) {
                release_va_reservation(g_early_va.base);
            }
            if (reserved && !try_reserve_va(va_base)) {
                LOG_INFO("init: VA at 0x" << std::hex << va_base
                         << " already mapped (inherited), adopting" << std::dec);
                reserved = (g_va_reservation_active && g_va_reservation_base == va_base);
            }
        }

        backend_.va_base_ = va_base;
        backend_.va_reserved_ = reserved;
        backend_.heap_id_ = heap_id;

        int rank = backend_.claim_rank();
        if (rank < 0) {
            LOG_WARN("init: all " << MAX_PROCESSES << " slots in use");
            return;
        }

        rank_ = rank;
        backend_.rank_ = rank;
        total_processes_ = static_cast<int>(MAX_PROCESSES);

        // §1 (steps 7-8): Assign node_index via CAS. For POSIX, this is
        // a no-op (my_node_index_ stays at 0). For UBSE, determines which
        // node this process is on by scanning bootstrap slots. Must run
        // AFTER claim_rank (needs rank) and BEFORE incrementing
        // node_proc_count / bootstrap_refcount (needs my_node_index_).
        backend_.assign_node_index();

        // §5b: Increment node_proc_count and bootstrap_refcount for this
        // process's node. Indexed by my_node_index_ (set above).
        // node_proc_count: decremented in destructor; reaches 0 → last
        // process on node → triggers unlink_node_shms + wait_for_unreferenced.
        // bootstrap_refcount: decremented in cleanup_bootstrap_mapping;
        // reaches 0 → last process calls shm_detach_by_name(bootstrap).
        if (backend_.bootstrap_block_ && backend_.my_node_index_ < MAX_PROCESSES) {
            backend_.bootstrap_block_->node_proc_count[backend_.my_node_index_]
                .fetch_add(1, std::memory_order_acq_rel);
            backend_.bootstrap_block_->bootstrap_refcount[backend_.my_node_index_]
                .fetch_add(1, std::memory_order_acq_rel);
        }

        long hw_cores = sysconf(_SC_NPROCESSORS_ONLN);
        num_cores_ = (hw_cores > 0) ? static_cast<size_t>(hw_cores) : MAX_NUM_CORES;
        if (num_cores_ > MAX_NUM_CORES) num_cores_ = MAX_NUM_CORES;
        g_next_thread_id_.store(static_cast<uint16_t>(rank * num_cores_),
                                std::memory_order_relaxed);

        LOG_INFO("init: rank=" << rank << " is_p0=" << is_p0
                 << " num_cores=" << num_cores_ << " total=" << total_processes_);

        typename BackendT::Config config;
        config.rank = rank;
        config.total_processes = static_cast<int>(MAX_PROCESSES);
        for (size_t k = 0; k < MAX_PROCESSES; ++k) {
            config.slab_count_small[k] = 0;
            config.slab_count_large[k] = 0;
            config.huge_slots[k] = 0;
        }

        typename BackendT::VALayout va_layout = BackendT::VALayout::compute(
            config, BackendT::ShmProvider::min_shm_size, va_base);

        LOG_INFO("init: VA layout computed, base=0x" << std::hex << va_base
                 << " metadata_va[rank]=0x" << va_layout.metadata_va[rank] << std::dec);

        cumulative_small_[0] = 0;
        cumulative_large_[0] = 0;
        for (size_t k = 0; k < MAX_PROCESSES; ++k) {
            cumulative_small_[k + 1] = (k + 1) * va_layout.slabs_per_process_small;
            cumulative_large_[k + 1] = (k + 1) * va_layout.slabs_per_process_large;
        }

        typename BackendT::SharedRegionLayout own_sl =
            compute_distributed_shared_region_layout<typename BackendT::AllocatorShared>(
                rank, static_cast<int>(MAX_PROCESSES), 0, 0, 0);

        backend_.rank_ = rank;
        backend_.total_processes_ = static_cast<int>(MAX_PROCESSES);
        backend_.config_ = config;
        backend_.va_layout_ = va_layout;

        // Enable uffd BEFORE creating any device-backed mappings in the VA
        // reservation. On UBSE, mprotect(PROT_READ|PROT_WRITE) on the ~28TB
        // PROT_NONE range fails with EOPNOTSUPP if device-backed mappings
        // (from create_own_regions, attach_and_map_remote_region, etc.) are
        // already present. When the constructor(102) finds an existing
        // bootstrap (reused HEAP_ID), it runs mprotect early and succeeds.
        // When the bootstrap doesn't exist yet (fresh HEAP_ID), the
        // constructor defers and try_enable_uffd_locked() must run here,
        // before any segments are created.
        try_enable_uffd_locked();

        auto create_res = backend_.create_own_regions(
            own_sl.total_size, 0, 0, 0);
        if (is_err(create_res)) return;
        LOG_INFO("init: own regions created");

        {
            char* base = static_cast<char*>(backend_.metadata_address(rank));
            auto* layout = reinterpret_cast<SharedLayout*>(base);
            layout->magic = SharedLayout::MAGIC;
            layout->layout_version = SharedLayout::LAYOUT_VERSION;
            layout->_padding0 = 0;
            layout->slab_count_small = 0;
            layout->slab_count_large = 0;
            layout->slab_capacity_small = 0;
            layout->slab_capacity_large = 0;

            typename BackendT::SharedRegionLayout own_offsets =
                compute_distributed_shared_region_layout<typename BackendT::AllocatorShared>(
                    rank, static_cast<int>(MAX_PROCESSES), 0, 0, 0);
            layout->help_array_offset = own_offsets.help_array_offset;
            layout->allocator_shared_offset = own_offsets.allocator_shared_offset;
            layout->small_shared_offset = own_offsets.small_shared_offset;
            layout->large_shared_offset = own_offsets.large_shared_offset;
            layout->huge_shared_offset = own_offsets.huge_shared_offset;
            layout->owned_array_offset = own_offsets.owned_array_offset;
            layout->total_shared_size = own_offsets.total_size;
            layout->seg_dir_small_offset = own_offsets.seg_dir_small_offset;
            layout->seg_dir_large_offset = own_offsets.seg_dir_large_offset;
            layout->seg_dir_huge_offset = own_offsets.seg_dir_huge_offset;

            // type_id_map is now in BootstrapBlock, initialized in
            // bootstrap_join (P0) or available immediately (P1+ attaches
            // bootstrap). No need to initialize it in metadata.
            if (rank == 0) {
                // Initialize type_id_map in bootstrap block
                auto* tid_map = &backend_.bootstrap_block_->type_id_map;
                for (int i = 0; i < TypeIdMap::MAX_TYPE_IDS; ++i) {
                    tid_map->owner_process[i].store(
                        TypeIdMap::UNOWNED, std::memory_order_relaxed);
                }
            }

            for (int b = 0; b < 3; ++b) {
                SegmentDirectory* dir = backend_.segment_directory(rank, b);
                if (dir) dir->init();
            }

            layout->ready_flag.store(1, std::memory_order_release);
            flush(&layout->magic, Invalidate::No);
            fence();
        }

        backend_.set_slot_ready(rank, own_sl.total_size);

        if (rank != 0) {
            if (!backend_.wait_for_slot_ready(0, 30000)) {
                LOG_WARN("init: timeout waiting for P0 slot ready");
                return;
            }
            typename BackendT::SharedRegionLayout p0_sl =
                compute_distributed_shared_region_layout<typename BackendT::AllocatorShared>(
                    0, static_cast<int>(MAX_PROCESSES), 0, 0, 0);
            auto res = backend_.attach_and_map_remote_region(
                0, BackendT::METADATA, p0_sl.total_size,
                reinterpret_cast<void*>(va_layout.metadata_va[0]), 0,
                &backend_.metadata_handles_[0]);
            if (is_err(res)) {
                LOG_WARN("init: failed to attach P0 metadata");
                return;
            }
            backend_.metadata_regions_[0] = unwrap(res);
        }

        wire_up_process(rank);
        if (rank != 0) {
            wire_up_process(0);
        }

        distributed_shared_ptrs_[rank]->init_help(
            rank, static_cast<int>(MAX_PROCESSES), num_cores_);
        for (size_t k = 0; k < MAX_PROCESSES; ++k) {
            if (help_array_ptrs_[k] && help_data_ptrs_[k]) {
                distributed_shared_ptrs_[rank]->set_slice(
                    static_cast<int>(k), help_array_ptrs_[k], help_data_ptrs_[k]);
            }
        }

        small_shared_ptrs_[rank]->init_bump();
        large_shared_ptrs_[rank]->init_bump();
        small_shared_ptrs_[rank]->init_growth(0);
        large_shared_ptrs_[rank]->init_growth(0);
        huge_shared_ptrs_[rank]->init_growth(0);

        grow_fn_small().store(&GlobalAllocator<BackendT>::grow_small_static,
                              std::memory_order_release);
        grow_fn_large().store(&GlobalAllocator<BackendT>::grow_large_static,
                              std::memory_order_release);
        grow_fn_huge().store(&GlobalAllocator<BackendT>::grow_huge_static,
                              std::memory_order_release);
        check_remote_fn().store(&GlobalAllocator<BackendT>::check_remote_segments_static,
                                std::memory_order_release);
        reclaim_fn_small().store(&GlobalAllocator<BackendT>::reclaim_small_static,
                                 std::memory_order_release);
        reclaim_fn_large().store(&GlobalAllocator<BackendT>::reclaim_large_static,
                                 std::memory_order_release);
        reclaim_trigger_fn().store(&GlobalAllocator<BackendT>::reclaim_trigger_static,
                                   std::memory_order_release);
        backend_.init_reclaim_params();

        {
            auto* reg = published_registry_ptrs_[rank];
            if (reg) {
                for (int i = 0; i < PublishedRegistry::MAX_ENTRIES; ++i) {
                    reg->entries[i].address.store(nullptr, std::memory_order_relaxed);
                    reg->entries[i].size.store(0, std::memory_order_relaxed);
                    reg->entries[i].type_id.store(0, std::memory_order_relaxed);
                    reg->entries[i].active.store(PUBLISHED_INACTIVE, std::memory_order_relaxed);
                }
                reg->next_slot.store(0, std::memory_order_relaxed);
            }
        }

        small_slab_ = Slab<Small>();
        large_slab_ = Slab<Large>();
        small_slab_.set_segment_params(va_layout.slabs_per_process_small,
                                       va_layout.slabs_per_segment_small);
        large_slab_.set_segment_params(va_layout.slabs_per_process_large,
                                       va_layout.slabs_per_segment_large);
        small_slab_.set_cumulative(cumulative_small_.data(),
                                   static_cast<int>(MAX_PROCESSES));
        large_slab_.set_cumulative(cumulative_large_.data(),
                                   static_cast<int>(MAX_PROCESSES));

        char* global_data_small_start = reinterpret_cast<char*>(
            backend_.segment_va(0, 0, 0));
        auto small_seg_layout = SegmentLayout<Small>::compute(
            SegmentLayout<Small>::virtual_slabs_per_segment());
        size_t total_slab_count_small = cumulative_small_[MAX_PROCESSES];
        small_data_.init_segmented(
            reinterpret_cast<Page*>(global_data_small_start),
            total_slab_count_small, total_slab_count_small,
            SEGMENT_VA_SIZE, SegmentLayout<Small>::virtual_slabs_per_segment(),
            small_seg_layout.data_offset);
        small_data_.set_slabs(&small_slab_);

        char* global_data_large_start = reinterpret_cast<char*>(
            backend_.segment_va(0, 1, 0));
        auto large_seg_layout = SegmentLayout<Large>::compute(
            SegmentLayout<Large>::virtual_slabs_per_segment());
        size_t total_slab_count_large = cumulative_large_[MAX_PROCESSES];
        large_data_.init_segmented(
            reinterpret_cast<Page*>(global_data_large_start),
            total_slab_count_large, total_slab_count_large,
            SEGMENT_VA_SIZE, SegmentLayout<Large>::virtual_slabs_per_segment(),
            large_seg_layout.data_offset);
        large_data_.set_slabs(&large_slab_);

        char* global_data_huge_start = reinterpret_cast<char*>(
            backend_.segment_va(rank, 2, 0));
        huge_data_ = Data<HugeSize>(
            reinterpret_cast<Page*>(global_data_huge_start));
        data_huge_base_ = global_data_huge_start;

        for (int k = 0; k < static_cast<int>(MAX_PROCESSES); ++k) {
            data_huge_base_ptrs_[k] = reinterpret_cast<char*>(
                backend_.va_layout_.data_huge_va[k]);
        }

        check_remote_segments();
        LOG_INFO("init complete: rank=" << rank_);

        {
            const char* aff_env = std::getenv("UBALLOC_AFFINITY_MODE");
            if (aff_env) {
                std::string s(aff_env);
                if (s == "reuse-first" || s == "reuse_first") {
                    affinity_mode().store(
                        static_cast<size_t>(AffinityMode::ReuseFirst),
                        std::memory_order_release);
                    LOG_INFO("init: affinity_mode=ReuseFirst (borrow early)");
                } else {
                    affinity_mode().store(
                        static_cast<size_t>(AffinityMode::Strict),
                        std::memory_order_release);
                    LOG_INFO("init: affinity_mode=Strict (borrow after growth)");
                }
            } else {
                LOG_INFO("init: affinity_mode=Strict (default, borrow after growth)");
            }
        }

        initialized_ = true;
        ensure_thread_init();

        ReclaimMode mode = parse_reclaim_mode_from_env();
        set_reclaim_mode(mode);
        bool is_fork_child = (reclaim_creator_pid_ != 0 &&
                              ::getpid() != reclaim_creator_pid_);
        reclaim_creator_pid_ = ::getpid();
        // Start ReclaimThread only if: ASYNC mode, reclaim enabled,
        // and not a fork child (short-lived, doesn't need background reclaim).
        if (mode == ReclaimMode::ASYNC && backend_.return_enabled() && !is_fork_child) {
            reclaim_thread_ = std::make_unique<ReclaimThread>();
            reclaim_thread_->set_scan_fn(&GlobalAllocator<BackendT>::scan_all_segments_static);
            reclaim_thread_->set_deadline_fn(&GlobalAllocator<BackendT>::compute_reclaim_deadline_static);
            // Read scan interval from env (default 5s)
            uint64_t scan_ms = 5000;
            const char* si = std::getenv("UBALLOC_RECLAIM_SCAN_INTERVAL_MS");
            if (si) {
                int ms = std::atoi(si);
                if (ms > 0) scan_ms = static_cast<uint64_t>(ms);
            }
            reclaim_thread_->start(scan_ms);
        }
    }

    void wire_up_process(int k) {
        char* metadata_base = static_cast<char*>(backend_.metadata_address(k));
        if (!metadata_base) return;

        if (k == 0 && rank_ != 0) {
            auto* p0_layout = reinterpret_cast<SharedLayout*>(metadata_base);
            while (p0_layout->ready_flag.load(std::memory_order_acquire) != 1) {
                ::usleep(1000);
            }
        }

        typename BackendT::SharedRegionLayout ksl;
        if (k == 0) {
            auto* layout = reinterpret_cast<SharedLayout*>(metadata_base);
            ksl = compute_distributed_shared_region_layout<typename BackendT::AllocatorShared>(
                0, static_cast<int>(MAX_PROCESSES), 0, 0, 0);
            ksl.help_array_offset = layout->help_array_offset;
            ksl.allocator_shared_offset = layout->allocator_shared_offset;
            ksl.small_shared_offset = layout->small_shared_offset;
            ksl.large_shared_offset = layout->large_shared_offset;
            ksl.huge_shared_offset = layout->huge_shared_offset;
            ksl.owned_array_offset = layout->owned_array_offset;
            ksl.seg_dir_small_offset = layout->seg_dir_small_offset;
            ksl.seg_dir_large_offset = layout->seg_dir_large_offset;
            ksl.seg_dir_huge_offset = layout->seg_dir_huge_offset;
            ksl.total_size = layout->total_shared_size;
        } else {
            ksl = compute_distributed_shared_region_layout<typename BackendT::AllocatorShared>(
                k, static_cast<int>(MAX_PROCESSES),
                backend_.config_.huge_slots[k],
                backend_.config_.slab_count_small[k],
                backend_.config_.slab_count_large[k]);
        }

        auto* k_help_data = reinterpret_cast<std::atomic<uint16_t>*>(
            metadata_base + ksl.help_array_offset);
        distributed_shared_ptrs_[k] = reinterpret_cast<typename BackendT::AllocatorShared*>(
            metadata_base + ksl.allocator_shared_offset);
        small_shared_ptrs_[k] = reinterpret_cast<HeapShared<Small>*>(
            metadata_base + ksl.small_shared_offset);
        large_shared_ptrs_[k] = reinterpret_cast<HeapShared<Large>*>(
            metadata_base + ksl.large_shared_offset);
        huge_shared_ptrs_[k] = reinterpret_cast<HugeShared*>(
            metadata_base + ksl.huge_shared_offset);
        distributed_owned_arrays_[k] = reinterpret_cast<ThreadArray<AllocatorOwned>*>(
            metadata_base + ksl.owned_array_offset);
        help_array_ptrs_[k] = reinterpret_cast<HelpArray*>(
            metadata_base + ksl.allocator_shared_offset + sizeof(typename BackendT::AllocatorShared));
        help_data_ptrs_[k] = k_help_data;
        published_registry_ptrs_[k] = reinterpret_cast<PublishedRegistry*>(
            metadata_base + ksl.published_registry_offset);

        // type_id_map is now in the BootstrapBlock, not in per-process
        // metadata. Set it once from bootstrap_block_ (available to all
        // processes without attaching P0's metadata).
        if (k == 0 && !type_id_map_) {
            type_id_map_ = &backend_.bootstrap_block_->type_id_map;
        }

        small_shared_ptrs_[k]->set_cross_free(k, &small_shared_ptrs_[k]->free);
        small_shared_ptrs_[k]->rank_ = k;
        small_shared_ptrs_[k]->total_processes_ = total_processes_;
        small_shared_ptrs_[k]->set_bump_bounds(
            cumulative_small_[k], cumulative_small_[k] + backend_.config_.slab_count_small[k]);

        large_shared_ptrs_[k]->set_cross_free(k, &large_shared_ptrs_[k]->free);
        large_shared_ptrs_[k]->rank_ = k;
        large_shared_ptrs_[k]->total_processes_ = total_processes_;
        large_shared_ptrs_[k]->set_bump_bounds(
            cumulative_large_[k], cumulative_large_[k] + backend_.config_.slab_count_large[k]);

        if (k != rank_ && small_shared_ptrs_[rank_]) {
            small_shared_ptrs_[rank_]->set_cross_free(k, &small_shared_ptrs_[k]->free);
            large_shared_ptrs_[rank_]->set_cross_free(k, &large_shared_ptrs_[k]->free);
        }
    }

    void ensure_thread_init() {
        if (!tl_thread_initialized_) {
            uint16_t tid = g_next_thread_id_.fetch_add(1, std::memory_order_relaxed);
            assert(tid < COUNT_THREAD);
            tl_thread_id_ = tid;
            tl_allocator_ptr_ = nullptr;
            tl_thread_initialized_ = true;
        }
    }

    __attribute__((noinline)) void init_thread(uint16_t thread_id) {
        assert(thread_id < COUNT_THREAD);
        tl_thread_id_ = thread_id;
        tl_allocator_ptr_ = nullptr;
        tl_thread_initialized_ = true;
    }

    void reset() {
        if (!initialized_) return;

        if (reclaim_thread_) {
            bool fork_child = reclaim_thread_->stop();
            // fork_child: inherited condvar has stale __wrefs from parent's
            // waiters; destroying it would block in pthread_cond_destroy.
            // Leak the object — fork children exit via _exit() (skips dtors).
            if (fork_child)
		reclaim_thread_.release();
            else
		reclaim_thread_.reset();
        }

        if constexpr (BackendT::ShmProvider::has_reliable_unreferenced_check) {
            // UBSE: full cleanup (same as destructor, since fork children
            // call _exit which skips destructors). Must run BEFORE
            // soft_reset() clears bootstrap_block_.
            // Decrement node_proc_count FIRST so detach_all_regions
            // knows whether to skip own regions.
            bool am_last_on_node = false;
            if (backend_.bootstrap_block_ && backend_.my_node_index_ < MAX_PROCESSES) {
                uint32_t prev = backend_.bootstrap_block_->node_proc_count[backend_.my_node_index_]
                    .fetch_sub(1, std::memory_order_acq_rel);
                am_last_on_node = (prev == 1);
            }

            backend_.collect_node_shm_names();
            backend_.detach_all_regions();

            // Clear bootstrap slot BEFORE munmap + detach + wait.
            // bootstrap_block_ is still valid (munmap happens in
            // unmap_bootstrap_mapping below).
            // Discovery deadlock caveat: clearing the slot before
            // wait_for_unreferenced blocks late discovery by processes
            // that haven't found us yet. This is an accepted edge case
            // — processes that already discovered us can still read
            // our data (data shms are separate from the bootstrap slot).
            // The alternative (deferring clear to after wait) creates a
            // circular dependency: detach must be after munmap (1005),
            // detach must be before wait (release own borrow), but
            // clear must be before munmap (needs valid bootstrap_block_)
            // and after wait (discovery). Can't satisfy all four.
            backend_.clear_bootstrap_slot();

            // Release own bootstrap borrow (munmap + shm_detach_by_name).
            // shm_detach_by_name MUST be after munmap (UBSE 1005 if
            // active mmap) and BEFORE wait_for_unreferenced (releases
            // own borrow so wait only blocks on OTHERS).
            std::string bs_detach_name;
            bool bs_should_detach = false;
            backend_.release_bootstrap_borrow(bs_detach_name, bs_should_detach);
            backend_.unmap_bootstrap_mapping(bs_detach_name, bs_should_detach);

            if (am_last_on_node) {
                backend_.detach_uffd_attached();
                backend_.wait_for_unreferenced();
                backend_.unlink_node_shms();
            } else {
                backend_.clear_uffd_attached();
            }
        } else {
            // POSIX path
            backend_.clear_bootstrap_slot();
            backend_.cleanup_remote_regions();
            backend_.cleanup_own_shms();
            backend_.cleanup_bootstrap_mapping();
        }

        backend_.release_reservation();
        soft_reset();
    }

    // Clear in-memory state without cleaning up shared memory.
    // Used by fork children that inherited the parent's GlobalAllocator state.
    // The parent's shms must NOT be touched — the child will re-init via init().
    void soft_reset() {
        if (reclaim_thread_) {
            bool fork_child = reclaim_thread_->stop();
            // fork_child: inherited condvar has stale __wrefs from parent's
            // waiters; destroying it would block in pthread_cond_destroy.
            // Leak the object — fork children exit via _exit() (skips dtors).
            if (fork_child)
		reclaim_thread_.release();
            else
		reclaim_thread_.reset();
        }

        // Disable the uffd handler and SIGSEGV handler.
        //
        // is_fork_child=false so uffd_uninstall JOINS the handler thread
        // (instead of detaching). This is critical when soft_reset() is
        // called from the main process (e.g., test_main.cpp before
        // re-init, or parent before fork): a detached thread would
        // survive into try_enable_uffd_locked(), see g_uffd_running
        // flip back to true, and race with the new handler on the same
        // uffd fd.
        //
        // For actual fork children: the parent always calls soft_reset()
        // before forking, so the child inherits a non-joinable
        // g_uffd_thread and g_uffd_active=false — uffd_uninstall skips
        // the thread and returns early. If the parent did NOT call
        // soft_reset() (unexpected), join() throws (thread doesn't
        // exist in child) and the catch-clause falls back to detach().
        uffd_uninstall(/*is_fork_child=*/false);
        fh_uninstall();

        grow_fn_small().store(nullptr, std::memory_order_release);
        grow_fn_large().store(nullptr, std::memory_order_release);
        grow_fn_huge().store(nullptr, std::memory_order_release);
        check_remote_fn().store(nullptr, std::memory_order_release);
        reclaim_fn_small().store(nullptr, std::memory_order_release);
        reclaim_fn_large().store(nullptr, std::memory_order_release);
        reclaim_trigger_fn().store(nullptr, std::memory_order_release);
        set_reclaim_mode(ReclaimMode::ASYNC);
        reclaim_lock().store(false, std::memory_order_release);
        reclaim_creator_pid_ = 0;  // clear so init() won't misdetect fork-child
        // Reset global stats counters so re-init starts fresh.
        uballoc::reclaim_stats_enabled().store(true, std::memory_order_relaxed);
        uballoc::stats_allocated_to_app().store(0, std::memory_order_relaxed);
        uballoc::stats_freed_from_app().store(0, std::memory_order_relaxed);
        affinity_mode().store(static_cast<size_t>(AffinityMode::Strict),
                              std::memory_order_release);

        backend_.rank_ = -1;
        backend_.total_processes_ = 0;
        backend_.heap_id_.clear();
        backend_.va_base_ = 0;
        backend_.va_reserved_ = false;
        backend_.bootstrap_ptr_ = nullptr;
        backend_.bootstrap_block_ = nullptr;
        backend_.bootstrap_handle_ = BackendT::INVALID_HANDLE;
        for (size_t k = 0; k < MAX_PROCESSES; ++k) {
            backend_.metadata_regions_[k] = {};
            backend_.metadata_handles_[k] = BackendT::INVALID_HANDLE;
            for (int b = 0; b < BackendT::DATA_BRACKETS; ++b) {
                backend_.segments_[k][b].clear();
            }
            backend_.is_owner_for_process_[k] = false;
            backend_.last_seen_generation_[k] = 0;
        }
        backend_.all_attached_ = false;
        backend_.own_created_ = false;
        backend_.initialized_ = false;
        initialized_ = false;
        total_processes_ = 1;
        rank_ = 0;
        type_id_map_ = nullptr;
        for (size_t i = 0; i < MAX_PROCESSES; ++i) {
            distributed_shared_ptrs_[i] = nullptr;
            small_shared_ptrs_[i] = nullptr;
            large_shared_ptrs_[i] = nullptr;
            huge_shared_ptrs_[i] = nullptr;
            distributed_owned_arrays_[i] = nullptr;
            help_array_ptrs_[i] = nullptr;
            help_data_ptrs_[i] = nullptr;
            published_registry_ptrs_[i] = nullptr;
            data_huge_base_ptrs_[i] = nullptr;
        }
        cumulative_small_[0] = cumulative_small_[1] = 0;
        cumulative_large_[0] = cumulative_large_[1] = 0;
        for (size_t i = 0; i < COUNT_THREAD; ++i) {
            small_owned_[i].clear();
            large_owned_[i].clear();
        }
        tl_allocator_ptr_ = nullptr;
        tl_thread_initialized_ = false;
    }

    // -----------------------------------------------------------------------
    // Fault handler: SIGSEGV-handler-based lazy-attach for remote segments.
    // -----------------------------------------------------------------------

    // Enable the SIGSEGV fault handler. After this call, if a thread
    // accesses a remote segment's VA before check_remote_segments() has
    // attached it (the eager-attach race window), the handler will mmap
    // the segment using a pre-registered device fd, and the faulting
    // instruction will re-execute successfully.
    //
    // The handler is a SAFETY NET for the race window. The primary
    // attach mechanism is still check_remote_segments() (eager). The
    // handler covers the narrow race between fd-open and mmap in the
    // eager path, plus the case where a segment's mapping was lost but
    // the fd is still open.
    //
    // For segments not yet discovered (created by another process after
    // the last check), the handler cannot help — there is no open fd.
    // Those segments are attached eagerly on the next check_remote_segments().
    //
    // Opt-in: the handler is NOT installed by default. Applications with
    // their own SIGSEGV handler (e.g., language runtimes, profilers) should
    // install theirs BEFORE calling enable_fault_handler() so uballoc sits
    // at the top of the chain and saves + chains to theirs for non-uballoc
    // faults.
    //
    // Must be called AFTER init(). Idempotent:
    // a second call returns true without re-installing — re-installing
    // would save uballoc's own handler into g_fh_old_sa, causing infinite
    // recursion on any subsequent chain. Returns true on success, false on
    // failure (e.g., sigaction failed).
    bool enable_fault_handler() {
        init();
        if (!initialized_) return false;

        std::lock_guard<std::mutex> lock(init_mutex_);
        return enable_fault_handler_locked();
    }

    // Core fault handler setup logic — caller MUST hold init_mutex_.
    // Called by enable_fault_handler() and try_enable_uffd_locked() (as
    // fallback when uffd is unavailable).
    bool enable_fault_handler_locked() {

        // Idempotency guard: if already enabled, return without
        // re-installing. A second sigaction() call would save the
        // currently-installed handler (uballoc's own fh_sigsegv_handler)
        // into g_fh_old_sa, causing infinite recursion on any later chain.
        if (g_fh_active.load(std::memory_order_acquire)) {
            return true;
        }

        // Populate signal-safe globals from backend state.
        uintptr_t va_base = backend_.va_base_;
        uintptr_t va_end = va_base + VA_RESERVATION_SIZE;
        g_fh_va_base.store(va_base, std::memory_order_release);
        g_fh_va_end.store(va_end, std::memory_order_release);

        // Set g_fh_active = true BEFORE populating the registry so that
        // fh_register_segment() is not a no-op. The signal handler is
        // NOT yet installed (fh_install is called below), so a fault
        // during registry population goes to the old handler (safe).
        // This also ensures that future attach_data_segment calls (from
        // check_remote_segments) will register their segments.
        g_fh_active.store(true, std::memory_order_release);

        // Register all currently-attached segments. These are already
        // mmaped by the eager path, so already_attached=true. If a
        // fault occurs on them (mapping was lost), the handler will
        // try to re-mmap using the cached fd.
        for (int k = 0; k < total_processes_; ++k) {
            // Metadata segments.
            if (backend_.metadata_regions_[k].address != nullptr &&
                backend_.metadata_handles_[k] != BackendT::INVALID_HANDLE) {
                fh_register_segment(
                    reinterpret_cast<uintptr_t>(backend_.metadata_regions_[k].address),
                    backend_.metadata_regions_[k].size,
                    BackendT::ShmProvider::shm_handle_fd(backend_.metadata_handles_[k]),
                    /*already_attached=*/true);
            }

            // Data segments.
            for (int b = 0; b < BackendT::DATA_BRACKETS; ++b) {
                for (size_t s = 0; s < backend_.segments_[k][b].size(); ++s) {
                    auto& seg = backend_.segments_[k][b][s];
                    if (seg.region.address && seg.region.size > 0 &&
                        seg.handle != BackendT::INVALID_HANDLE) {
                        fh_register_segment(
                            reinterpret_cast<uintptr_t>(seg.region.address),
                            seg.region.size,
                            BackendT::ShmProvider::shm_handle_fd(seg.handle),
                            /*already_attached=*/true);
                    }
                }
            }
        }

        // Install the signal handler (sigaction only — no sigaltstack)
        // AFTER the registry is populated. This ensures the handler sees
        // a fully-populated registry from the start.
        if (!fh_install()) {
            g_fh_active.store(false, std::memory_order_release);
            fh_clear_all();
            LOG_WARN("enable_fault_handler_locked: failed to install handler");
            return false;
        }

        LOG_INFO("enable_fault_handler_locked: installed SIGSEGV handler for VA region "
                 "[0x" << std::hex << va_base << ", 0x" << va_end
                 << ")" << std::dec);
        return true;
    }

    // Disable the fault handler. Restores the previous SIGSEGV handler
    // and clears the segment registry. Called automatically by soft_reset()
    // (fork children) and reset() (explicit teardown).
    void disable_fault_handler() {
        fh_uninstall();
        LOG_INFO("disable_fault_handler: uninstalled SIGSEGV handler");
    }

    // -----------------------------------------------------------------------
    // userfaultfd handler: primary lazy-attach when uffd is available.
    // -----------------------------------------------------------------------

    // Enable the userfaultfd-based fault handler. Tries uffd first; falls
    // back to the SIGSEGV handler (enable_fault_handler) if uffd is
    // unavailable (ENOSYS or EPERM). Must be called AFTER init().
    // Idempotent: returns true if uffd or SIGSEGV
    // handler is already active.
    bool enable_userfaultfd() {
        init();
        if (!initialized_) return false;

        std::lock_guard<std::mutex> lock(init_mutex_);
        return try_enable_uffd_locked();
    }

    // Core uffd setup logic — caller MUST hold init_mutex_.
    // Called by enable_userfaultfd(), do_init(), and init().
    // Idempotent: returns true if already active.
    bool try_enable_uffd_locked() {
        // Idempotency: already enabled (uffd or SIGSEGV)?
        if (g_uffd_active.load(std::memory_order_acquire)) return true;
        if (g_fh_active.load(std::memory_order_acquire)) return true;

        uintptr_t va_base = backend_.va_base_;

        // Step 2: mprotect PROT_NONE → PROT_READ|PROT_WRITE.
        // uffd requires PROT_READ|PROT_WRITE so that missing PTEs trigger
        // uffd (not SIGSEGV). MAP_NORESERVE prevents OOM for 28TB.
        if (::mprotect(reinterpret_cast<void*>(va_base),
                       VA_RESERVATION_SIZE,
                       PROT_READ | PROT_WRITE) != 0) {
            // mprotect may fail with ENOMEM if the VA range has holes
            // from a previous reset() that munmapped segments
            // (detach_all_regions + release_va_reservation).
            // Re-create the PROT_NONE reservation to repair holes,
            // then retry mprotect.
            void* p = ::mmap(reinterpret_cast<void*>(va_base),
                            VA_RESERVATION_SIZE, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                            -1, 0);
            if (p != MAP_FAILED &&
                ::mprotect(reinterpret_cast<void*>(va_base),
                           VA_RESERVATION_SIZE,
                           PROT_READ | PROT_WRITE) == 0) {
                LOG_INFO("try_enable_uffd_locked: re-created VA reservation at 0x"
                         << std::hex << va_base << std::dec);
            } else {
                LOG_WARN("try_enable_uffd_locked: mprotect failed ("
                         << strerror(errno) << "), falling back to SIGSEGV handler");
                return enable_fault_handler_locked();
            }
        }

        // Step 3: create the uffd fd.
        int uffd = static_cast<int>(
            ::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
        if (uffd < 0) {
            LOG_WARN("try_enable_uffd_locked: userfaultfd() failed ("
                      << strerror(errno) << "), falling back to SIGSEGV");
            ::mprotect(reinterpret_cast<void*>(va_base),
                       VA_RESERVATION_SIZE, PROT_NONE);
            return enable_fault_handler_locked();
        }

        // Step 4: UFFDIO_API.
        struct uffdio_api api;
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_MISSING_SHMEM;
        api.ioctls = 0;
        if (::ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_WARN("try_enable_uffd_locked: UFFDIO_API failed ("
                      << strerror(errno) << "), falling back to SIGSEGV");
            ::close(uffd);
            ::mprotect(reinterpret_cast<void*>(va_base),
                       VA_RESERVATION_SIZE, PROT_NONE);
            return enable_fault_handler_locked();
        }

        // Step 5: UFFDIO_REGISTER the entire VA reservation.
        struct uffdio_register reg;
        reg.range.start = va_base;
        reg.range.len = VA_RESERVATION_SIZE;
        reg.mode = UFFDIO_REGISTER_MODE_MISSING;
        reg.ioctls = 0;
        if (::ioctl(uffd, UFFDIO_REGISTER, &reg) != 0) {
            LOG_WARN("try_enable_uffd_locked: UFFDIO_REGISTER failed ("
                      << strerror(errno) << "), falling back to SIGSEGV");
            ::close(uffd);
            ::mprotect(reinterpret_cast<void*>(va_base),
                       VA_RESERVATION_SIZE, PROT_NONE);
            return enable_fault_handler_locked();
        }

        // Step 6: set runtime globals.
        g_uffd_fd = uffd;
        g_uffd_va_base.store(va_base, std::memory_order_release);
        g_uffd_bootstrap_ptr.store(backend_.bootstrap_block_,
                                     std::memory_order_release);
        g_uffd_heap_id.store(::strdup(backend_.heap_id_.c_str()),
                              std::memory_order_release);

        // Step 7: g_fh_active = true so fh_register_segment works.
        g_fh_active.store(true, std::memory_order_release);

        // Step 8: register all currently-attached segments in g_fh_entries.
        // These are already mmaped by the eager path. The handler needs
        // them in the registry for the fast path and for the "metadata
        // for k in g_fh_entries?" check in the DATA slow path.
        for (int k = 0; k < total_processes_; ++k) {
            if (backend_.metadata_regions_[k].address != nullptr &&
                backend_.metadata_handles_[k] != BackendT::INVALID_HANDLE) {
                fh_register_segment(
                    reinterpret_cast<uintptr_t>(
                        backend_.metadata_regions_[k].address),
                    backend_.metadata_regions_[k].size,
                    BackendT::ShmProvider::shm_handle_fd(
                        backend_.metadata_handles_[k]),
                    /*already_attached=*/true);
            }
            for (int b = 0; b < BackendT::DATA_BRACKETS; ++b) {
                for (size_t s = 0;
                     s < backend_.segments_[k][b].size(); ++s) {
                    auto& seg = backend_.segments_[k][b][s];
                    if (seg.region.address && seg.region.size > 0 &&
                        seg.handle != BackendT::INVALID_HANDLE) {
                        fh_register_segment(
                            reinterpret_cast<uintptr_t>(seg.region.address),
                            seg.region.size,
                            BackendT::ShmProvider::shm_handle_fd(seg.handle),
                            /*already_attached=*/true);
                    }
                }
            }
        }

        // Step 9: spawn the uffd handler thread.
        g_uffd_active.store(true, std::memory_order_release);
        g_uffd_running.store(true, std::memory_order_release);
        g_uffd_thread = std::thread(
            uffd_handler_thread<typename BackendT::ShmProvider>);

        // Step 10: install SIGSEGV+SIGBUS handlers as a PERMANENT SAFETY NET.
        //
        // *** MAJOR KNOWN ISSUE: UBSE VMA DESTRUCTION ***
        // The UBSE device driver DESTROYS existing mmap'd VMAs at fixed
        // addresses as a side effect of cross-process shm operations.
        // uffd CANNOT catch these faults (VMA is destroyed, not "missing
        // page"). Without the SIGSEGV handler, the process crashes.
        // The handler re-mmaps from g_fh_entries (which has the fd).
        // See fault_handler.hpp header comment for full details.
        //
        // MUST set g_fh_va_base/end BEFORE fh_install — the handler reads
        // these to decide if a fault is in our VA range.
        g_fh_va_base.store(va_base, std::memory_order_release);
        g_fh_va_end.store(va_base + VA_RESERVATION_SIZE,
                          std::memory_order_release);
        fh_install();

        LOG_INFO("try_enable_uffd_locked: installed uffd handler for VA "
                 "[0x" << std::hex << va_base << ", 0x"
                 << (va_base + VA_RESERVATION_SIZE) << ")"
                 << std::dec);
        return true;
    }

    // Disable the uffd handler. Stops the handler thread, unregisters the
    // VA range, mprotect back to PROT_NONE, closes the uffd fd. Called
    // automatically by soft_reset() (fork children) and reset() (teardown).
    void disable_userfaultfd() {
        uffd_uninstall();
        LOG_INFO("disable_userfaultfd: uninstalled uffd handler");
    }

    static bool grow_small_static() {
        return GlobalAllocator<BackendT>::get().grow_segment(0);
    }
    static bool grow_large_static() {
        return GlobalAllocator<BackendT>::get().grow_segment(1);
    }
    static bool grow_huge_static() {
        return GlobalAllocator<BackendT>::get().grow_segment(2);
    }

    static bool check_remote_segments_static() {
        GlobalAllocator<BackendT>::get().check_remote_segments();
        return true;
    }

    static void reclaim_small_static() {
        GlobalAllocator<BackendT>::get().backend_.check_one_segment_for_reclaim(0);
    }

    static void reclaim_large_static() {
        GlobalAllocator<BackendT>::get().backend_.check_one_segment_for_reclaim(1);
    }

    static void reclaim_trigger_static() {
        auto* rt = get().reclaim_thread_.get();
        if (rt) rt->trigger_now();
    }

    static void scan_all_segments_static() {
        GlobalAllocator<BackendT>::get().scan_all_segments_impl();
    }

    static uint64_t compute_reclaim_deadline_static() {
        return GlobalAllocator<BackendT>::get().backend_.compute_next_reclaim_deadline_ns();
    }

    // Shared lock for scan_all_segments and check_remote_segments.
    // Uses atomic flag instead of std::mutex — fork-safe because
    // soft_reset() clears the flag (no inherited locked state).
    static std::atomic<bool>& reclaim_lock() {
        static std::atomic<bool> locked{false};
        return locked;
    }

    // Try to acquire reclaim lock (non-blocking).
    bool try_reclaim_lock() {
        bool expected = false;
        return reclaim_lock().compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void release_reclaim_lock() {
        reclaim_lock().store(false, std::memory_order_release);
    }

    void scan_all_segments_impl() {
        if (!try_reclaim_lock()) return;
        // collect() does only state checks — microsecond-level lock hold.
        auto actions = backend_.scan_all_segments_collect();
        release_reclaim_lock();
        // execute() does the slow I/O (munmap, shm_unlink, shm_exists)
        // without holding reclaim_lock, so check_remote_segments and purge
        // don't block.
        backend_.scan_all_segments_execute(actions);
    }

    void purge() {
        init();
        // Block until reclaim_lock is available — purge is user-called and
        // must not race with background scan.
        acquire_reclaim_lock_blocking();
        backend_.return_segment_purge();
        release_reclaim_lock();
    }

    ReturnStats return_stats() {
        init();
        ReturnStats stats = backend_.return_stats();

        return stats;
    }

    // Collect defrag hints — live allocations in slabs with
    // occupancy < threshold. The application should relocate these
    // (malloc + memcpy + fix refs + free) to consolidate sparse slabs.
    std::vector<DefragHint> defrag_hints(double threshold, size_t max_hints = SIZE_MAX) {
        init();
        std::vector<DefragHint> hints;
        hints.reserve(std::min(max_hints, static_cast<size_t>(1024)));

        collect_defrag_hints<Small>(
            small_slab_, small_data_, small_shared_ptrs_[rank_],
            SegmentLayout<Small>::virtual_slabs_per_segment(),
            threshold, max_hints, hints);

        if (hints.size() < max_hints) {
            collect_defrag_hints<Large>(
                large_slab_, large_data_, large_shared_ptrs_[rank_],
                SegmentLayout<Large>::virtual_slabs_per_segment(),
                threshold, max_hints - hints.size(), hints);
        }

        return hints;
    }

    // Template helper — traverse SlabLocal array, find low-occupancy
    // slabs, enumerate live blocks (BitSet bit=0), generate hints.
    template<typename B>
    void collect_defrag_hints(Slab<B>& slabs, Data<B>& data,
                              HeapShared<B>* shared,
                              size_t sps,
                              double threshold, size_t max_hints,
                              std::vector<DefragHint>& hints) {
        if (!shared) return;

        for (uint32_t seg = 0; seg < slabs.segment_counts[rank_]; ++seg) {
            if (hints.size() >= max_hints) return;
            auto& slice = slabs.local_slices[rank_][seg];
            auto* base = slice.data();
            if (!base) continue;

            // Use per-segment slab_count (not sps)
            size_t seg_slab_count = slabs.seg_slab_counts_[rank_][seg];
            if (seg_slab_count == 0) seg_slab_count = sps;

            for (size_t i = 0; i < seg_slab_count; ++i) {
                SlabLocal<B>& sl = base[i];

                // Skip detached slabs (being reclaimed)
                if (sl.detached.load(std::memory_order_relaxed) != 0) continue;

                uint8_t class_idx = sl.class_.load(std::memory_order_relaxed);
                if (class_idx == 0) continue;  // skip uninit slabs
                auto cls_opt = B::from_index(class_idx);
                if (!cls_opt) continue;
                B class_ = *cls_opt;

                size_t total_blocks = class_.count();
                if (total_blocks == 0) continue;

                size_t free_blocks = sl.free.len();
                size_t used_blocks = total_blocks - free_blocks;
                double occupancy = (double)used_blocks / total_blocks;

                // Only collect from low-occupancy slabs that have live blocks
                if (occupancy >= threshold || used_blocks == 0) continue;

                // Enumerate live blocks (BitSet: bit=1 means free, bit=0 means used)
                // We need to find blocks where the bit is NOT set (used).
                for (size_t row = 0; row < B::BitSetType::SIZE_DATA; ++row) {
                    uint64_t bits = sl.free.dense[row];
                    if (bits == ~0ULL) continue;  // all free, skip

                    for (size_t col = 0; col < 64; ++col) {
                        if (bits & (1ULL << col)) continue;  // this block is free

                        // This block is used → generate hint
                        size_t block_index = row * 64 + col;
                        if (block_index >= total_blocks) break;

                        SlabIndex<B> slab_idx(slabs.segment_global_start(rank_, seg) + i);
                        Offset<B> offset = data.from_block(class_, slab_idx,
                            Bit(u6(static_cast<uint8_t>(row)),
                                u6(static_cast<uint8_t>(col))));
                        void* ptr = reinterpret_cast<void*>(
                            reinterpret_cast<char*>(data.base_) + offset.get());

                        hints.push_back({ptr, class_.size(), occupancy});
                        if (hints.size() >= max_hints) return;
                    }
                }
            }
        }
    }

    // Ladder segment size function.
    // Returns the actual slab_count (after compute cap) so that
    // requested_slabs == compute(requested_slabs).slab_count.
    // This avoids confusion where create_data_segment logs a different
    // slabs value than grow_segment.
    static size_t segment_ladder_slab_count(size_t slab_size, size_t sps, uint32_t seg_idx) {
        size_t requested;
        if (seg_idx == 0)      requested = std::max(size_t(1), sps / 32);  // ~4MB
        else if (seg_idx == 1) requested = std::max(size_t(1), sps / 8);   // ~16MB
        else if (seg_idx == 2) requested = std::max(size_t(1), sps / 2);   // ~64MB
        else                   requested = sps;                              // 128MB full
        // Return compute().slab_count so requested == actual
        (void)slab_size;  // slab_size not used; compute uses B::SLAB_SIZE internally
        return requested;
    }

    // Cumulative slab count for the first seg_count segments.
    // Reads from seg_slab_counts_ (clamped slab_count) to match the
    // actual bump allocation ranges (which use sl.slab_count, not sps).
    size_t cumulative_slab_count(int bracket, uint32_t seg_count) {
        if (seg_count == 0) return 0;
        size_t total = 0;
        if (bracket == 0) {
            for (uint32_t i = 0; i < seg_count && i < MAX_SEGMENTS; ++i) {
                size_t sc = small_slab_.seg_slab_counts_[rank_][i];
                total += (sc > 0) ? sc : small_slab_.slabs_per_segment_;
            }
        } else if (bracket == 1) {
            for (uint32_t i = 0; i < seg_count && i < MAX_SEGMENTS; ++i) {
                size_t sc = large_slab_.seg_slab_counts_[rank_][i];
                total += (sc > 0) ? sc : large_slab_.slabs_per_segment_;
            }
        }
        return total;
    }

    bool grow_segment(int bracket) {
        if (bracket == 0) {
            size_t sps = SegmentLayout<Small>::virtual_slabs_per_segment();
            size_t max_slabs = SegmentLayout<Small>::compute(sps).slab_count;
            uint32_t seg_count = small_shared_ptrs_[rank_]->segment_count_.load(std::memory_order_relaxed);

            size_t requested = segment_ladder_slab_count(Small::SLAB_SIZE, max_slabs, seg_count);
            // Use compute().slab_count so create_data_segment sees the
            // actual number of slabs (compute may cap it to fit target size).
            requested = SegmentLayout<Small>::compute(requested).slab_count;
            auto res = backend_.create_data_segment(0, requested);
            if (is_err(res)) return false;
            int seg_idx = unwrap(res);

            char* seg_base = static_cast<char*>(backend_.data_address(rank_, 0, seg_idx));
            auto sl = SegmentLayout<Small>::compute(requested);
            small_slab_.register_segment(rank_, seg_idx,
                sl.slab_local_ptr(seg_base), sl.slab_remote_ptr(seg_base),
                sl.slab_count);

            // Eager touch — fault in the new segment's first data page
            // BEFORE enabling bump. Ensures uffd handler attaches the segment's
            // shm before any bump() returns a slab index in this segment.
            {
                volatile char touch = *(seg_base + sl.data_offset);
                (void)touch;
            }

            size_t global_start = cumulative_small_[rank_] + cumulative_slab_count(0, seg_count);
            small_shared_ptrs_[rank_]->bump_raw.store(
                SlabIndex<Small>(global_start).internal(), std::memory_order_release);
            small_shared_ptrs_[rank_]->bump_end_atomic_.store(
                global_start + sl.slab_count, std::memory_order_release);
            small_shared_ptrs_[rank_]->segment_count_.fetch_add(1, std::memory_order_release);

            LOG_INFO("grow_segment: small seg=" << seg_idx << " slabs=" << sl.slab_count
                     << " bump_start=" << global_start << " bump_end=" << global_start + sl.slab_count);
            check_remote_segments();
            return true;
        } else if (bracket == 1) {
            size_t sps = SegmentLayout<Large>::virtual_slabs_per_segment();
            size_t max_slabs = SegmentLayout<Large>::compute(sps).slab_count;
            uint32_t seg_count = large_shared_ptrs_[rank_]->segment_count_.load(std::memory_order_relaxed);

            size_t requested = segment_ladder_slab_count(Large::SLAB_SIZE, max_slabs, seg_count);
            requested = SegmentLayout<Large>::compute(requested).slab_count;
            auto res = backend_.create_data_segment(1, requested);
            if (is_err(res)) return false;
            int seg_idx = unwrap(res);

            char* seg_base = static_cast<char*>(backend_.data_address(rank_, 1, seg_idx));
            auto sl = SegmentLayout<Large>::compute(requested);
            large_slab_.register_segment(rank_, seg_idx,
                sl.slab_local_ptr(seg_base), sl.slab_remote_ptr(seg_base),
                sl.slab_count);

            // Eager touch (same as Small)
            {
                volatile char touch = *(seg_base + sl.data_offset);
                (void)touch;
            }

            size_t global_start = cumulative_large_[rank_] + cumulative_slab_count(1, seg_count);
            large_shared_ptrs_[rank_]->bump_raw.store(
                SlabIndex<Large>(global_start).internal(), std::memory_order_release);
            large_shared_ptrs_[rank_]->bump_end_atomic_.store(
                global_start + sl.slab_count, std::memory_order_release);
            large_shared_ptrs_[rank_]->segment_count_.fetch_add(1, std::memory_order_release);

            LOG_INFO("grow_segment: large seg=" << seg_idx << " slabs=" << sl.slab_count
                     << " bump_start=" << global_start << " bump_end=" << global_start + sl.slab_count);
            check_remote_segments();
            return true;
        } else {
            // Huge keeps uniform 128MB segments (HUGE_SLOTS_PER_SEGMENT).
            // Unlike Small/Large, Huge slots are addressed as a flat array
            // (base + slot * SLAB_SIZE) which assumes contiguous data across
            // segments at 128MB VA boundaries. Ladder segments would break
            // this flat addressing since slots 2+ would land outside the
            // first segment's 4MB shm range.
            auto res = backend_.create_data_segment(2, HUGE_SLOTS_PER_SEGMENT);
            if (is_err(res)) return false;

            huge_shared_ptrs_[rank_]->segment_count_.fetch_add(1, std::memory_order_release);

            LOG_INFO("grow_segment: huge seg=" << unwrap(res));
            check_remote_segments();
            return true;
        }
    }

    // Blocking acquire of reclaim_lock for cold paths that MUST NOT skip.
    // Used by check_remote_segments and purge — both are cold paths where
    // skipping causes free leaks or state races. Blocks until reclaim_lock
    // is available. Lock hold is microsecond-level (only collect runs under
    // it; I/O executes outside).
    void acquire_reclaim_lock_blocking() {
        while (!try_reclaim_lock()) {
            std::this_thread::yield();
        }
    }

    void check_remote_segments() {
        // Cold path: MUST NOT skip (skipping causes free drops and missed
        // peer discovery). Block until reclaim_lock is available.
        acquire_reclaim_lock_blocking();

        backend_.attach_new_processes();

        for (int k = 0; k < total_processes_; ++k) {
            if (k == rank_) continue;
            // If attach_new_processes released Pk's metadata (slot cleared),
            // clear dangling shared pointers so future check_remote_segments
            // calls will re-wire Pk if it rejoins (new process reclaims slot).
            // Also clear published_registry_ptrs_ to prevent lookup_by_type
            // from dereferencing a dangling pointer (causes SIGSEGV).
            if (small_shared_ptrs_[k] != nullptr &&
                backend_.metadata_address(k) == nullptr) {
                small_shared_ptrs_[k] = nullptr;
                large_shared_ptrs_[k] = nullptr;
                huge_shared_ptrs_[k] = nullptr;
                help_array_ptrs_[k] = nullptr;
                help_data_ptrs_[k] = nullptr;
                published_registry_ptrs_[k] = nullptr;
            }
            if (small_shared_ptrs_[k] != nullptr) continue;
            if (backend_.metadata_address(k) == nullptr) continue;
            wire_up_process(k);
            if (help_array_ptrs_[k] && help_data_ptrs_[k]) {
                distributed_shared_ptrs_[rank_]->set_slice(
                    k, help_array_ptrs_[k], help_data_ptrs_[k]);
            }
        }

        backend_.check_remote_segments();

        for (int k = 0; k < total_processes_; ++k) {
            if (k == rank_) continue;
            for (int b = 0; b < 2; ++b) {
                int known = (b == 0) ? static_cast<int>(small_slab_.segment_counts[k])
                                      : static_cast<int>(large_slab_.segment_counts[k]);
                int available = static_cast<int>(backend_.segments_[k][b].size());
                for (int s = known; s < available; ++s) {
                    char* seg_base = static_cast<char*>(backend_.data_address(k, b, s));
                    if (!seg_base) continue;
                    auto* dir = backend_.segment_directory(k, b);
                    size_t actual_slab_count = (dir && s < static_cast<int>(MAX_SEGMENTS))
                        ? dir->descs[s].slab_count : 0;
                    if (actual_slab_count == 0) {
                        actual_slab_count = (b == 0)
                            ? backend_.config_.slab_count_small[k]
                            : backend_.config_.slab_count_large[k];
                    }
                    if (b == 0) {
                        auto sl = SegmentLayout<Small>::compute(actual_slab_count);
                        small_slab_.register_segment(k, s,
                            sl.slab_local_ptr(seg_base), sl.slab_remote_ptr(seg_base),
                            sl.slab_count);
                    } else {
                        auto sl = SegmentLayout<Large>::compute(actual_slab_count);
                        large_slab_.register_segment(k, s,
                            sl.slab_local_ptr(seg_base), sl.slab_remote_ptr(seg_base),
                            sl.slab_count);
                    }
                }
            }
        }

        release_reclaim_lock();
    }

    Allocator<void, void>& current_allocator() {
        init();
        ensure_thread_init();
        if (!tl_allocator_ptr_) {
            uint16_t tid = tl_thread_id_;
            tl_allocator_ptr_ = new Allocator<void, void>;
            tl_allocator_ptr_->init_distributed(
                static_cast<DistributedAllocatorShared*>(distributed_shared_ptrs_[rank_]),
                distributed_owned_arrays_[rank_],
                Heap<Small>(small_shared_ptrs_[rank_], &small_owned_[tid],
                            &small_slab_, &small_data_),
                Heap<Large>(large_shared_ptrs_[rank_], &large_owned_[tid],
                            &large_slab_, &large_data_),
                Huge(huge_shared_ptrs_[rank_], huge_data_,
                     data_huge_base_,
                     ThreadId(tid),
                     huge_shared_ptrs_.data(),
                     data_huge_base_ptrs_.data(),
                     total_processes_, rank_),
                rank_, total_processes_, num_cores_);

            tl_allocator_ptr_->focus(ThreadId(tid));
        }
        return *tl_allocator_ptr_;
    }

    void* malloc(size_t size) {
        return current_allocator().allocate(size);
    }

    // Combined malloc+publish: allocates `size` bytes and publishes the pointer
    // under `type_id` in one call. On publish failure, the allocation is freed
    // and nullptr is returned.
    //
    // Race condition note: the pointer is published before the caller has a
    // chance to initialize the memory. Callers that need data-readiness sync
    // must use an application-level ready flag stored in the published region
    // itself (e.g., publish a struct with an atomic `ready` field, set the
    // field after this call returns, and have lookers spin on it).
    void* malloc(size_t size, uint32_t type_id) {
        void* ptr = malloc(size);
        if (!ptr) return nullptr;
        int slot = publish(ptr, size, type_id);
        if (slot < 0) {
            free(ptr);
            LOG_WARN("malloc(size, type_id): publish failed for type_id=" << type_id
                     << ", freed allocation at " << ptr);
            return nullptr;
        }
        LOG_DEBUG("malloc(size, type_id): allocated " << size << " bytes at " << ptr
                  << " published as type_id=" << type_id << " slot=" << slot);
        return ptr;
    }

    void free(void* ptr) {
        if (!ptr) return;
        current_allocator().free(ptr);
    }

    void* realloc(void* ptr, size_t size) {
        return current_allocator().realloc(ptr, size);
    }

    // Returns the usable size of the allocation at `ptr`.
    // This is the size class (slab/slot) size, which may be >= the
    // originally requested size (internal fragmentation).
    // Returns 0 for nullptr or unrecognized pointers.
    size_t usable_size(void* ptr) {
        if (!ptr) return 0;
        return current_allocator().class_size(ptr);
    }

    void* data_small_base() { return reinterpret_cast<char*>(small_data_.offset_to_pointer<char>(Offset<Small>(Small::SLAB_SIZE))); }
    void* data_large_base() { return reinterpret_cast<char*>(large_data_.offset_to_pointer<char>(Offset<Large>(Large::SLAB_SIZE))); }

    char* get_data_huge_base() {
        return data_huge_base_;
    }

    void* memalign(size_t size, size_t alignment) {
        size_t aligned_size = size + alignment - 1;
        void* ptr = current_allocator().allocate(aligned_size);
        if (!ptr) return nullptr;

        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
        return reinterpret_cast<void*>(aligned);
    }

    int publish(void* ptr, size_t size, uint32_t type_id) {
        init();
        if (!initialized_ || total_processes_ <= 1) return -1;

        auto* reg = published_registry_ptrs_[rank_];
        if (!reg) return -1;

        uint32_t slot = reg->next_slot.fetch_add(1, std::memory_order_relaxed);
        if (slot >= PublishedRegistry::MAX_ENTRIES) {
            reg->next_slot.store(PublishedRegistry::MAX_ENTRIES, std::memory_order_relaxed);
            return -1;
        }

        if (type_id_map_ && type_id < TypeIdMap::MAX_TYPE_IDS) {
            int32_t expected = TypeIdMap::UNOWNED;
            if (!type_id_map_->owner_process[type_id].compare_exchange_strong(
                    expected, static_cast<int32_t>(rank_),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                reg->next_slot.fetch_sub(1, std::memory_order_relaxed);
                return -1;
            }
        }

        reg->entries[slot].address.store(ptr, std::memory_order_relaxed);
        reg->entries[slot].size.store(size, std::memory_order_relaxed);
        reg->entries[slot].type_id.store(type_id, std::memory_order_relaxed);
        reg->entries[slot].active.exchange(PUBLISHED_ACTIVE, std::memory_order_acq_rel);

        return static_cast<int>(slot);
    }

    bool unpublish(void* ptr) {
        return unpublish(ptr, rank_);
    }

    bool unpublish(void* ptr, int owner_pid) {
        if (!initialized_ || total_processes_ <= 1) return false;
        if (owner_pid < 0 || owner_pid >= total_processes_) return false;

        auto* reg = published_registry_ptrs_[owner_pid];
        if (!reg) return false;

        uint32_t limit = reg->next_slot.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < limit && i < PublishedRegistry::MAX_ENTRIES; ++i) {
            if (reg->entries[i].address.load(std::memory_order_relaxed) == ptr &&
                reg->entries[i].active.load(std::memory_order_acquire) == PUBLISHED_ACTIVE) {
                uint32_t type_id = reg->entries[i].type_id.load(std::memory_order_relaxed);
                if (type_id_map_ && type_id < TypeIdMap::MAX_TYPE_IDS) {
                    int32_t expected = static_cast<int32_t>(owner_pid);
                    type_id_map_->owner_process[type_id].compare_exchange_strong(
                        expected, TypeIdMap::UNOWNED,
                        std::memory_order_acq_rel, std::memory_order_acquire);
                }
                reg->entries[i].active.exchange(PUBLISHED_INACTIVE, std::memory_order_acq_rel);
                return true;
            }
        }
        return false;
    }

    PublishedInfo lookup_by_address(void* ptr) {
        init();
        PublishedInfo result;
        if (!initialized_ || total_processes_ <= 1) return result;

        for (int k = 0; k < total_processes_; ++k) {
            auto* reg = published_registry_ptrs_[k];
            if (!reg) continue;

            uint32_t limit = reg->next_slot.load(std::memory_order_acquire);
            for (uint32_t i = 0; i < limit && i < PublishedRegistry::MAX_ENTRIES; ++i) {
                if (reg->entries[i].address.load(std::memory_order_relaxed) == ptr &&
                    reg->entries[i].active.load(std::memory_order_acquire) == PUBLISHED_ACTIVE) {
                    result.address = reg->entries[i].address.load(std::memory_order_relaxed);
                    result.size = reg->entries[i].size.load(std::memory_order_relaxed);
                    result.type_id = reg->entries[i].type_id.load(std::memory_order_relaxed);
                    result.owner_process = k;
                    return result;
                }
            }
        }
        return result;
    }

    PublishedInfo lookup_by_type(uint32_t type_id) {
        init();
        PublishedInfo result;
        if (!initialized_ || total_processes_ <= 1) {
            LOG_TRACE("lookup_by_type(" << type_id << "): not initialized");
            return result;
        }
        if (!type_id_map_ || type_id >= TypeIdMap::MAX_TYPE_IDS) {
            LOG_TRACE("lookup_by_type(" << type_id << "): type_id_map_=" << type_id_map_ << " type_id=" << type_id);
            return result;
        }

        int32_t owner = type_id_map_->owner_process[type_id].load(std::memory_order_acquire);
        if (owner == TypeIdMap::UNOWNED) {
            check_remote_segments();
            owner = type_id_map_->owner_process[type_id].load(std::memory_order_acquire);
            if (owner == TypeIdMap::UNOWNED) {
                LOG_TRACE("lookup_by_type(" << type_id << "): UNOWNED");
                return result;
            }
        }

        int k = static_cast<int>(owner);
        if (k < 0 || k >= total_processes_) {
            LOG_TRACE("lookup_by_type(" << type_id << "): invalid owner k=" << k);
            return result;
        }

        if (k != rank_) {
            check_remote_segments();
        }

        auto* reg = published_registry_ptrs_[k];
        if (!reg) {
            LOG_TRACE("lookup_by_type(" << type_id << "): registry null for k=" << k);
            return result;
        }

        uint32_t limit = reg->next_slot.load(std::memory_order_acquire);
        LOG_TRACE("lookup_by_type(" << type_id << "): owner=" << owner << " next_slot=" << limit);
        for (uint32_t i = 0; i < limit && i < PublishedRegistry::MAX_ENTRIES; ++i) {
            if (reg->entries[i].type_id.load(std::memory_order_relaxed) == type_id &&
                reg->entries[i].active.load(std::memory_order_acquire) == PUBLISHED_ACTIVE) {
                result.address = reg->entries[i].address.load(std::memory_order_relaxed);
                result.size = reg->entries[i].size.load(std::memory_order_relaxed);
                result.type_id = reg->entries[i].type_id.load(std::memory_order_relaxed);
                result.owner_process = k;
                return result;
            }
        }
        return result;
    }

    // Blocking variant of lookup_by_type. Polls every 100ms until the type_id
    // is found or the timeout expires. A negative timeout_ms means wait
    // forever. Returns PublishedInfo with owner_process >= 0 on success, or
    // an empty PublishedInfo (owner_process == -1) on timeout.
    PublishedInfo lookup_by_type_blocking(uint32_t type_id, int timeout_ms = -1) {
        constexpr int POLL_INTERVAL_MS = 100;
        int elapsed = 0;
        while (true) {
            PublishedInfo info = lookup_by_type(type_id);
            if (info.owner_process >= 0) return info;
            if (timeout_ms >= 0 && elapsed >= timeout_ms) {
                LOG_DEBUG("lookup_by_type_blocking(" << type_id << "): timeout after "
                          << elapsed << "ms");
                return info;
            }
            ::usleep(POLL_INTERVAL_MS * 1000);
            elapsed += POLL_INTERVAL_MS;
        }
    }

    void unlink_process(int dead_pid) {
        backend_.unlink_process_shms(dead_pid);
        distributed_shared_ptrs_[dead_pid] = nullptr;
        small_shared_ptrs_[dead_pid] = nullptr;
        large_shared_ptrs_[dead_pid] = nullptr;
        huge_shared_ptrs_[dead_pid] = nullptr;
        distributed_owned_arrays_[dead_pid] = nullptr;
        help_array_ptrs_[dead_pid] = nullptr;
        help_data_ptrs_[dead_pid] = nullptr;
        published_registry_ptrs_[dead_pid] = nullptr;
    }

    bool is_owner() const { return backend_.is_owner_impl(); }
    bool is_initialized() const { return initialized_; }
    int rank() const { return rank_; }
    int total_processes() const { return total_processes_; }
    size_t num_cores() const { return num_cores_; }

    auto& backend() { return backend_; }
    auto& small_slab() { return small_slab_; }
    auto& large_slab() { return large_slab_; }
    auto small_shared() { return small_shared_ptrs_[rank_]; }
    auto large_shared() { return large_shared_ptrs_[rank_]; }
    auto huge_shared() { return huge_shared_ptrs_[rank_]; }
};

template<typename BackendT>
GlobalAllocator<BackendT>* GlobalAllocator<BackendT>::instance_ = nullptr;

template<typename BackendT>
std::atomic<uint16_t> GlobalAllocator<BackendT>::g_next_thread_id_{0};

// The tls_model("global-dynamic") attribute on these definitions ensures
// the LIBRARY (libuballoc.so, compiled with -fPIC) uses R_AARCH64_TLSDESC
// (General Dynamic) for thread_local access — correct for cross-module TLS.
//
// HOWEVER: executables compiled with -fPIE (the CMake default for
// POSITION_INDEPENDENT_CODE ON) emit R_AARCH64_TLS_TPREL64 (Local Exec)
// for inline functions that access these variables, even with
// -ftls-model=global-dynamic and the attribute on the class-body
// declarations. LE reads the executable's TLS block, but the variables
// are in the library's TLS block → segfault on the first access.
//
// This only manifests when a small inline function (e.g., init_thread())
// is called directly from the binary — the compiler always inlines it.
// Larger functions (init, malloc) are not inlined, so the call goes
// through the library (PLT) where GD/TLSDESC is used.
//
// Fix: consumers that call inline methods directly should define
// UBALLOC_NO_EXTERN_TEMPLATE before including uballoc.hpp. This removes
// the extern template declaration, so the binary defines its own copy
// of GlobalAllocator (including TLS vars in the executable's TLS block),
// making LE access correct.
template<typename BackendT>
__attribute__((tls_model("global-dynamic")))
thread_local uint16_t GlobalAllocator<BackendT>::tl_thread_id_ = 0;

template<typename BackendT>
__attribute__((tls_model("global-dynamic")))
thread_local Allocator<void, void>* GlobalAllocator<BackendT>::tl_allocator_ptr_ = nullptr;

template<typename BackendT>
__attribute__((tls_model("global-dynamic")))
thread_local bool GlobalAllocator<BackendT>::tl_thread_initialized_ = false;

template<typename BackendT = DistributedShmBackend<>>
inline GlobalAllocator<BackendT>& get_global_allocator() {
    return GlobalAllocator<BackendT>::get();
}

// -------------------------------------------------------------------------
// Free-function wrappers for the common case (default backend).
// Eliminates the need to call get_global_allocator() explicitly:
//
//   uballoc::init();          // after setting UBALLOC_HEAP_ID env var
//   void* p = uballoc::malloc(64);
//   uballoc::free(p);
//
// For a non-default backend, use get_global_allocator<YourBackend>().
// -------------------------------------------------------------------------

inline void init() {
    log::flush_deferred_logs();
    get_global_allocator().init();
}

inline void init_thread(uint16_t thread_id) {
    get_global_allocator().init_thread(thread_id);
}

inline void ensure_thread_init() {
    get_global_allocator().ensure_thread_init();
}

inline void* malloc(size_t size) {
    return get_global_allocator().malloc(size);
}

inline void* malloc(size_t size, uint32_t type_id) {
    return get_global_allocator().malloc(size, type_id);
}

inline void free(void* ptr) {
    get_global_allocator().free(ptr);
}

inline void* realloc(void* ptr, size_t size) {
    return get_global_allocator().realloc(ptr, size);
}

inline void* memalign(size_t size, size_t alignment) {
    return get_global_allocator().memalign(size, alignment);
}

inline int publish(void* ptr, size_t size, uint32_t type_id) {
    return get_global_allocator().publish(ptr, size, type_id);
}

inline bool unpublish(void* ptr) {
    return get_global_allocator().unpublish(ptr);
}

inline bool unpublish(void* ptr, int owner_pid) {
    return get_global_allocator().unpublish(ptr, owner_pid);
}

inline PublishedInfo lookup_by_type(uint32_t type_id) {
    return get_global_allocator().lookup_by_type(type_id);
}

inline PublishedInfo lookup_by_type_blocking(uint32_t type_id, int timeout_ms = -1) {
    return get_global_allocator().lookup_by_type_blocking(type_id, timeout_ms);
}

inline PublishedInfo lookup_by_address(void* ptr) {
    return get_global_allocator().lookup_by_address(ptr);
}

inline void reset() {
    get_global_allocator().reset();
}

inline void soft_reset() {
    get_global_allocator().soft_reset();
}

inline bool is_initialized() {
    return get_global_allocator().is_initialized();
}

inline bool is_owner() {
    return get_global_allocator().is_owner();
}

inline int rank() {
    return get_global_allocator().rank();
}

inline int total_processes() {
    return get_global_allocator().total_processes();
}

inline size_t num_cores() {
    return get_global_allocator().num_cores();
}

inline bool enable_fault_handler() {
    return get_global_allocator().enable_fault_handler();
}

inline void disable_fault_handler() {
    get_global_allocator().disable_fault_handler();
}

inline void purge() {
    get_global_allocator().purge();
}

inline ReturnStats return_stats() {
    return get_global_allocator().return_stats();
}

inline size_t usable_size(void* ptr) {
    return get_global_allocator().usable_size(ptr);
}

inline std::vector<DefragHint> defrag_hints(double threshold, size_t max_hints = SIZE_MAX) {
    return get_global_allocator().defrag_hints(threshold, max_hints);
}

inline Allocator<void, void>& current_allocator() {
    return get_global_allocator().current_allocator();
}

// -------------------------------------------------------------------------
// extern template declaration — suppress implicit instantiation of
// GlobalAllocator<DistributedShmBackend<>> in user code. The actual
// instantiation lives in libuballoc.so (src/uballoc_instances.cpp).
//
// Users who want a custom backend should #define UBALLOC_NO_EXTERN_TEMPLATE
// before including any uballoc header to remove this declaration and
// restore implicit-instantiation (header-only) behavior.
// -------------------------------------------------------------------------
#ifndef UBALLOC_NO_EXTERN_TEMPLATE
extern template class GlobalAllocator<DistributedShmBackend<>>;
#endif

}
