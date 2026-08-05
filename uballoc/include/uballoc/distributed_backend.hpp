// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sys/mman.h>
#include <errno.h>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <signal.h>

#include "region.hpp"
#include "log.hpp"
#include "thread.hpp"
#include "size.hpp"
#include "cache.hpp"
#include "shm_provider.hpp"
#include "fault_handler.hpp"
#ifdef UBALLOC_USE_UBSE
#include "ubshm_provider.hpp"
#endif
#include "shared_layout.hpp"
#include "uffd_handler.hpp"   // g_uffd_attached (cleanup in detach_all_regions)

namespace uballoc {

struct DistributedConfig {
    int rank;
    int total_processes;
    std::array<size_t, MAX_PROCESSES> slab_count_small;
    std::array<size_t, MAX_PROCESSES> slab_count_large;
    std::array<size_t, MAX_PROCESSES> huge_slots;
};

struct DistributedVALayout {
    std::array<uintptr_t, MAX_PROCESSES> metadata_va;

    uintptr_t data_small_start;
    std::array<size_t, MAX_PROCESSES> data_small_size;
    std::array<uintptr_t, MAX_PROCESSES> data_small_va;

    uintptr_t data_large_start;
    std::array<size_t, MAX_PROCESSES> data_large_size;
    std::array<uintptr_t, MAX_PROCESSES> data_large_va;

    uintptr_t data_huge_start;
    std::array<size_t, MAX_PROCESSES> data_huge_size;
    std::array<uintptr_t, MAX_PROCESSES> data_huge_va;

    std::array<size_t, MAX_PROCESSES + 1> cumulative_small;
    std::array<size_t, MAX_PROCESSES + 1> cumulative_large;

    size_t slabs_per_process_small;
    size_t slabs_per_process_large;
    size_t slabs_per_segment_small;
    size_t slabs_per_segment_large;

    static DistributedVALayout compute(const DistributedConfig& config,
                                        size_t min_shm_size,
                                        uintptr_t va_base) {
        DistributedVALayout layout;

        const uintptr_t metadata_base   = va_base;
        const uintptr_t data_small_base = va_base + METADATA_STRIDE;
        const uintptr_t data_large_base = va_base + METADATA_STRIDE + DATA_SMALL_STRIDE;
        const uintptr_t data_huge_base  = va_base + METADATA_STRIDE + DATA_SMALL_STRIDE + DATA_LARGE_STRIDE;

        for (int k = 0; k < config.total_processes; ++k) {
            layout.metadata_va[k] = metadata_base + k * PROCESS_STRIDE_META;
        }

        layout.slabs_per_process_small = PROCESS_STRIDE_SMALL / Small::SLAB_SIZE;
        layout.slabs_per_process_large = PROCESS_STRIDE_LARGE / Large::SLAB_SIZE;
        layout.slabs_per_segment_small = SegmentLayout<Small>::virtual_slabs_per_segment();
        layout.slabs_per_segment_large = SegmentLayout<Large>::virtual_slabs_per_segment();

        layout.cumulative_small[0] = 0;
        layout.cumulative_large[0] = 0;
        for (int k = 0; k < config.total_processes; ++k) {
            layout.cumulative_small[k + 1] = layout.cumulative_small[k] + layout.slabs_per_process_small;
            layout.cumulative_large[k + 1] = layout.cumulative_large[k] + layout.slabs_per_process_large;
        }

        layout.data_small_start = data_small_base;
        for (int k = 0; k < config.total_processes; ++k) {
            layout.data_small_va[k] = data_small_base + k * PROCESS_STRIDE_SMALL;
            if (config.slab_count_small[k] > 0) {
                auto sl = SegmentLayout<Small>::compute(config.slab_count_small[k]);
                layout.data_small_size[k] = std::max(sl.total_size, min_shm_size);
            } else {
                layout.data_small_size[k] = 0;
            }
        }

        layout.data_large_start = data_large_base;
        for (int k = 0; k < config.total_processes; ++k) {
            layout.data_large_va[k] = data_large_base + k * PROCESS_STRIDE_LARGE;
            if (config.slab_count_large[k] > 0) {
                auto sl = SegmentLayout<Large>::compute(config.slab_count_large[k]);
                layout.data_large_size[k] = std::max(sl.total_size, min_shm_size);
            } else {
                layout.data_large_size[k] = 0;
            }
        }

        layout.data_huge_start = data_huge_base;
        for (int k = 0; k < config.total_processes; ++k) {
            layout.data_huge_va[k] = data_huge_base + k * PROCESS_STRIDE_HUGE;
            if (config.huge_slots[k] > 0) {
                layout.data_huge_size[k] = std::max(
                    config.huge_slots[k] * HugeSize::SLAB_SIZE, min_shm_size);
            } else {
                layout.data_huge_size[k] = 0;
            }
        }

        layout.validate(config, min_shm_size,
                        metadata_base,
                        data_small_base, data_large_base, data_huge_base);

        return layout;
    }

    void validate(const DistributedConfig& config,
                  size_t /*min_shm_size*/,
                  uintptr_t metadata_base,
                  uintptr_t data_small_base,
                  uintptr_t data_large_base,
                  uintptr_t data_huge_base) const {
        uintptr_t metadata_end = metadata_base + config.total_processes * PROCESS_STRIDE_META;
        if (metadata_end > data_small_base) {
            LOG_ERROR("VA layout: metadata zone [0x" << std::hex << metadata_base
                      << "-0x" << metadata_end << "] overlaps data_small_base 0x"
                      << data_small_base << std::dec);
        }

        uintptr_t small_end = data_small_base + config.total_processes * PROCESS_STRIDE_SMALL;
        if (small_end > data_large_base) {
            LOG_ERROR("VA layout: data_small zone [0x" << std::hex << data_small_base
                      << "-0x" << small_end << "] overlaps data_large_base 0x"
                      << data_large_base << std::dec);
        }

        uintptr_t large_end = data_large_base + config.total_processes * PROCESS_STRIDE_LARGE;
        if (large_end > data_huge_base) {
            LOG_ERROR("VA layout: data_large zone [0x" << std::hex << data_large_base
                      << "-0x" << large_end << "] overlaps data_huge_base 0x"
                      << data_huge_base << std::dec);
        }
    }

    int find_process_small(size_t global_idx, int total_processes) const {
        if (slabs_per_process_small > 0) {
            return static_cast<int>(global_idx / slabs_per_process_small);
        }
        return find_process_binary(cumulative_small, global_idx, total_processes);
    }

    int find_process_large(size_t global_idx, int total_processes) const {
        if (slabs_per_process_large > 0) {
            return static_cast<int>(global_idx / slabs_per_process_large);
        }
        return find_process_binary(cumulative_large, global_idx, total_processes);
    }

private:
    int find_process_binary(const std::array<size_t, MAX_PROCESSES + 1>& cum,
                            size_t global_idx, int total_processes) const {
        int lo = 0, hi = total_processes - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (cum[mid] <= global_idx) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    }

public:

    void print_ascii_layout(int this_pid, int total_procs) const {
        const int W = 18;

        auto fmt_size = [](size_t bytes) -> std::string {
            char buf[32];
            if (bytes >= (1ULL << 30))
                snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / (1ULL << 30));
            else if (bytes >= (1ULL << 20))
                snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1ULL << 20));
            else if (bytes >= (1ULL << 10))
                snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / (1ULL << 10));
            else
                snprintf(buf, sizeof(buf), "%zu B", bytes);
            return std::string(buf);
        };

        auto center = [&W](const std::string& s) -> std::string {
            int pad = W - static_cast<int>(s.size());
            if (pad <= 0) return s.substr(0, W);
            int left = pad / 2;
            return std::string(left, ' ') + s + std::string(pad - left, ' ');
        };

        auto fmt_va = [](uintptr_t va) -> std::string {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%012llx", static_cast<unsigned long long>(va));
            return std::string(buf);
        };

        auto sep = [&]() {
            for (int k = 0; k < total_procs; ++k) {
                std::cout << "  +" << std::string(W, '-') << "+";
                std::cout << (k + 1 < total_procs ? "   " : "\n");
            }
        };

        auto print_region = [&](const char* name,
                                const std::array<uintptr_t, MAX_PROCESSES>& vas,
                                const std::array<size_t, MAX_PROCESSES>* sizes) {
            std::cout << "\n  " << name << "\n";
            sep();
            for (int k = 0; k < total_procs; ++k) {
                std::string label = "P" + std::to_string(k) + "  " + name;
                if (k == this_pid) label += " *";
                std::cout << "  |" << center(label) << "|";
                std::cout << (k + 1 < total_procs ? "   " : "\n");
            }
            for (int k = 0; k < total_procs; ++k) {
                std::cout << "  |" << center(fmt_va(vas[k])) << "|";
                std::cout << (k + 1 < total_procs ? "   " : "\n");
            }
            if (sizes) {
                for (int k = 0; k < total_procs; ++k) {
                    std::cout << "  |" << center(fmt_size((*sizes)[k])) << "|";
                    std::cout << (k + 1 < total_procs ? "   " : "\n");
                }
            }
            sep();
        };

        std::cout << "\n"
                  << "=========================================================================\n"
                  << "               uballoc Virtual Address Space Layout\n"
                  << "                   " << total_procs
                  << " Processes  (this: P" << this_pid << ")\n"
                  << "=========================================================================\n";

        print_region("Metadata", metadata_va,   nullptr);
        print_region("Small",    data_small_va, &data_small_size);
        print_region("Large",    data_large_va, &data_large_size);
        print_region("Huge",     data_huge_va,  &data_huge_size);

        std::cout << "=========================================================================\n";
    }
};

#ifdef UBALLOC_USE_UBSE
template<typename ShmProviderT = UBShmProvider>
#else
template<typename ShmProviderT = PosixShmProvider>
#endif
class DistributedShmBackend {
public:
    using ShmProvider = ShmProviderT;
    using ShmHandle = typename ShmProviderT::ShmHandle;
    using Config = DistributedConfig;
    using VALayout = DistributedVALayout;
    using SharedRegionLayout = DistributedSharedRegionLayout;
    using AllocatorShared = DistributedAllocatorShared;
    static constexpr ShmHandle INVALID_HANDLE = ShmProviderT::INVALID_HANDLE;

    static constexpr int REGION_COUNT = 4;
    static constexpr int DATA_BRACKETS = 3;

    static size_t round_up_shm_size(size_t size) {
        size_t min_size = ShmProviderT::min_shm_size;
        if (size <= min_size) return min_size;
        return align_up(size, ShmProviderT::shm_size_granularity);
    }

    enum RegionType {
        METADATA     = 0,
        DATA_SMALL   = 1,
        DATA_LARGE   = 2,
        DATA_HUGE    = 3
    };

    struct SegmentEntry {
        Region region;
        ShmHandle handle;
        enum class State : uint8_t {
            LIVE,
            DETACHING,
            DETACHED,
            RETURNING,
            RETURNED
        };
        State state = State::LIVE;
        uint64_t detached_since_ns = 0;

        SegmentEntry() : handle(ShmProviderT::INVALID_HANDLE) {}
    };

    int rank_ = -1;
    int total_processes_ = 0;
    DistributedConfig config_;
    DistributedVALayout va_layout_;

    // VA reservation state. Set by init() before any segment mmap.
    // When va_reserved_ is true, create_and_map_own_region / attach_and_map_remote_region
    // use MAP_FIXED (overwrite our own PROT_NONE reservation) instead of MAP_FIXED_NOREPLACE.
    uintptr_t va_base_ = 0;
    bool va_reserved_ = false;
    std::string heap_id_;

    std::array<ShmHandle, MAX_PROCESSES> metadata_handles_;
    std::array<Region, MAX_PROCESSES> metadata_regions_;

    std::array<std::array<std::vector<SegmentEntry>, DATA_BRACKETS>, MAX_PROCESSES> segments_;

    std::array<bool, MAX_PROCESSES> is_owner_for_process_;

    bool initialized_ = false;
    bool own_created_ = false;
    bool all_attached_ = false;

    // Collected before detach_all_regions clears segments_/metadata.
    // Used by unlink_node_shms to delete exactly the shms that exist,
    // instead of blindly trying 48 indices per rank.
    std::vector<std::string> node_shm_names_;

    // Dynamic discovery state (Phase 1+). bootstrap_block_ is non-null only
    // when the process joined via init/bootstrap_join. The explicit
    // init path leaves these null, and attach_new_processes()
    // is a no-op (it checks bootstrap_block_ != nullptr).
    void* bootstrap_ptr_ = nullptr;
    ShmHandle bootstrap_handle_ = ShmProviderT::INVALID_HANDLE;
    BootstrapBlock* bootstrap_block_ = nullptr;
    std::mutex remote_attach_mutex_;
    std::array<uint32_t, MAX_PROCESSES> last_seen_generation_;

    // NEW (multi-proc/node): cached UBSE slot_id of the local node, set
    // in do_lazy_init via ubs_topo_node_local_get (R7: avoid calling
    // topo_node_local_get on every shm_is_unreferenced poll). For POSIX
    // (is_single_node=true), stays at 0.
    uint32_t local_node_slot_id_ = 0;
    // NEW (multi-proc/node): logical node index (0..MAX_PROCESSES-1) for
    // this process. Assigned via CAS scan-and-claim in do_lazy_init after
    // claim_rank. UINT32_MAX = unassigned (refcount code asserts this is
    // set before any refcount operation). For POSIX (is_single_node=true),
    // stays at 0 (single node = index 0).
    uint32_t my_node_index_ = 0;

    // Freed-memory-return parameters (read from env vars in init_reclaim_params).
    bool return_enabled_ = true;
    uint64_t decay1_ns_ = 5'000'000'000ULL;
    uint64_t decay2_ns_ = 60'000'000'000ULL;
    size_t reclaim_scan_small_cursor_ = 0;
    size_t reclaim_scan_large_cursor_ = 0;
    size_t total_detached_count_ = 0;
    size_t total_returned_count_ = 0;
    size_t total_recreated_count_ = 0;

    void init_reclaim_params() {
        const char* enabled = std::getenv("UBALLOC_RETURN_ENABLED");
        if (enabled) return_enabled_ = (std::atoi(enabled) != 0);

        const char* d1 = std::getenv("UBALLOC_RETURN_DECAY1_MS");
        if (d1) {
            int ms = std::atoi(d1);
            if (ms >= 0) decay1_ns_ = static_cast<uint64_t>(ms) * 1'000'000ULL;
        }

        const char* d2 = std::getenv("UBALLOC_RETURN_DECAY2_MS");
        if (d2) {
            int ms = std::atoi(d2);
            if (ms >= 0) decay2_ns_ = static_cast<uint64_t>(ms) * 1'000'000ULL;
        }

        LOG_INFO("init_reclaim_params: enabled=" << return_enabled_
                 << " decay1_ns=" << decay1_ns_
                 << " decay2_ns=" << decay2_ns_);
    }

    static uint64_t steady_now_ns() {
        auto tp = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
    }

    DistributedShmBackend() {
        for (size_t i = 0; i < MAX_PROCESSES; ++i) {
            metadata_handles_[i] = ShmProviderT::INVALID_HANDLE;
            last_seen_generation_[i] = 0;
        }
    }

    void* metadata_address(int pid) const {
        return metadata_regions_[pid].address;
    }

    void* data_address(int pid, int bracket, size_t seg = 0) const {
        if (seg < segments_[pid][bracket].size())
            return segments_[pid][bracket][seg].region.address;
        return nullptr;
    }

    SegmentDirectory* segment_directory(int pid, int bracket) {
        char* meta = static_cast<char*>(metadata_regions_[pid].address);
        if (!meta) return nullptr;
        DistributedSharedRegionLayout sl = compute_distributed_shared_region_layout<DistributedAllocatorShared>(
            pid, total_processes_, config_.huge_slots[pid],
            config_.slab_count_small[pid], config_.slab_count_large[pid]);
        size_t offsets[] = {sl.seg_dir_small_offset, sl.seg_dir_large_offset, sl.seg_dir_huge_offset};
        return reinterpret_cast<SegmentDirectory*>(meta + offsets[bracket]);
    }

    std::atomic<uint64_t>* huge_slots_ptr(int pid) {
        char* meta = static_cast<char*>(metadata_regions_[pid].address);
        if (!meta) return nullptr;
        DistributedSharedRegionLayout sl = compute_distributed_shared_region_layout<DistributedAllocatorShared>(
            pid, total_processes_, config_.huge_slots[pid],
            config_.slab_count_small[pid], config_.slab_count_large[pid]);
        return reinterpret_cast<std::atomic<uint64_t>*>(meta + sl.huge_slots_offset);
    }

    bool is_owner_impl() const {
        return is_owner_for_process_[rank_];
    }

    void mark_not_owner_impl() {
        is_owner_for_process_[rank_] = false;
    }

    bool is_owner_for_process(int pid) const {
        return is_owner_for_process_[pid];
    }

    uintptr_t segment_va(int pid, int bracket, int seg_idx) const {
        uintptr_t base_vas[] = {
            va_layout_.data_small_va[pid],
            va_layout_.data_large_va[pid],
            va_layout_.data_huge_va[pid]
        };
        return base_vas[bracket] + static_cast<uintptr_t>(seg_idx) * SEGMENT_VA_SIZE;
    }

    // -----------------------------------------------------------------------
    // Bootstrap shm: P0 publishes chosen va_base; other processes read it.
    // The bootstrap shm is small (one page), name-based, and mapped at a
    // kernel-chosen address (no fixed VA required).
    // -----------------------------------------------------------------------
    // §9d: publish_va_base_to_bootstrap and read_va_base_from_bootstrap
    // were REMOVED (dead code — superseded by bootstrap_join, which handles
    // both P0 create and non-P0 attach paths with proper P0 zero-init of
    // all new BootstrapBlock fields). cleanup_bootstrap was also REMOVED
    // (R5: don't shm_delete the bootstrap shm in the destructor — let the
    // cleanup script handle it; the lender's self-map keeps import_desc_cnt
    // > 0 forever, so shm_delete never completes, and force-deleting would
    // SIGBUS other procs on the lender's node still mapping the bootstrap).

    void release_reservation() {
        if (va_reserved_ && va_base_ != 0) {
            release_va_reservation(va_base_);
            va_reserved_ = false;
            LOG_INFO("release_va_reservation: base=0x" << std::hex << va_base_ << std::dec);
        }
    }

    // -----------------------------------------------------------------------
    // Dynamic discovery: bootstrap join, rank claiming, lazy attach, stale
    // slot reclamation. These are only used by the init() path.
    // The explicit init() path does not call these.
    // -----------------------------------------------------------------------

    struct BootstrapJoinResult {
        uintptr_t va_base;
        bool reserved;
        bool is_p0;
    };

    Result<BootstrapJoinResult> bootstrap_join(const std::string& heap_id) {
        std::string name = ShmProviderT::bootstrap_shm_name(heap_id);
        const size_t effective_size = round_up_shm_size(BOOTSTRAP_SIZE);

        bool is_p0 = false;
        ShmHandle handle = ShmProviderT::INVALID_HANDLE;

        auto create_res = ShmProviderT::shm_try_create(name, effective_size);
        if (is_ok(create_res)) {
            handle = unwrap(create_res);
            is_p0 = true;
        } else {
            Error& err = unwrap_err(create_res);
            if (err.kind != ErrorKind::ShmExists) {
                return err;
            }
            auto attach_res = ShmProviderT::shm_attach(name);
            if (is_err(attach_res)) {
                return unwrap_err(attach_res);
            }
            handle = unwrap(attach_res);
            is_p0 = false;
        }

        void* p = ::mmap(nullptr, effective_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, ShmProviderT::shm_handle_fd(handle), 0);
        if (p == MAP_FAILED) {
            ShmProviderT::shm_close(handle);
            if (is_p0) ShmProviderT::shm_unlink(name);
            return Error(ErrorKind::Mmap, std::error_code(errno, std::system_category()));
        }

        bootstrap_ptr_ = p;
        bootstrap_handle_ = handle;
        bootstrap_block_ = static_cast<BootstrapBlock*>(p);

        uintptr_t va_base = 0;
        bool reserved = false;

        if (is_p0) {
            if (g_early_va.attempted && g_early_va.reserved) {
                va_base = g_early_va.base;
                reserved = true;
                g_va_reservation_active = true;
                g_va_reservation_base = va_base;
                LOG_INFO("bootstrap_join: adopted early VA reservation at 0x"
                         << std::hex << va_base << std::dec);
            } else {
                va_base = probe_and_reserve_va();
                reserved = (va_base != 0);
                if (va_base == 0) {
                    LOG_WARN("bootstrap_join: all VA candidates exhausted, per-segment fallback");
                    va_base = DEFAULT_VA_BASE;
                }
            }
            bootstrap_block_->va_base.store(va_base, std::memory_order_relaxed);
            bootstrap_block_->reserved.store(reserved ? 1u : 0u, std::memory_order_relaxed);
            for (size_t i = 0; i < MAX_PROCESSES; ++i) {
                bootstrap_block_->slots[i].pid.store(0, std::memory_order_relaxed);
                bootstrap_block_->slots[i].generation.store(0, std::memory_order_relaxed);
                bootstrap_block_->slots[i].ready.store(0, std::memory_order_relaxed);
                bootstrap_block_->slots[i].node_slot_id.store(0, std::memory_order_relaxed);
                bootstrap_block_->slots[i].node_index.store(UINT32_MAX, std::memory_order_relaxed);
                bootstrap_block_->metadata_sizes[i].store(0, std::memory_order_relaxed);
                bootstrap_block_->node_proc_count[i].store(0, std::memory_order_relaxed);
                bootstrap_block_->bootstrap_refcount[i].store(0, std::memory_order_relaxed);
                for (size_t j = 0; j < MAX_PROCESSES; ++j) {
                    bootstrap_block_->metadata_refcount[i][j].store(0, std::memory_order_relaxed);
                }
            }
            bootstrap_block_->magic.store(BOOTSTRAP_MAGIC, std::memory_order_release);
            bootstrap_block_->ready.store(1, std::memory_order_release);
            ::msync(p, effective_size, MS_SYNC);

            LOG_INFO("bootstrap_join: created bootstrap as P0, va_base=0x"
                     << std::hex << va_base << " reserved=" << reserved << std::dec);
        } else {
            int elapsed = 0;
            constexpr int timeout_ms = 30000;
            while (bootstrap_block_->ready.load(std::memory_order_acquire) != 1 ||
                   bootstrap_block_->magic.load(std::memory_order_relaxed) != BOOTSTRAP_MAGIC) {
                ::usleep(1000);
                elapsed++;
                if (elapsed >= timeout_ms) {
                    size_t bs_size = round_up_shm_size(BOOTSTRAP_SIZE);
                    ::munmap(p, bs_size);
                    ShmProviderT::shm_close(handle);
                    bootstrap_ptr_ = nullptr;
                    bootstrap_block_ = nullptr;
                    bootstrap_handle_ = ShmProviderT::INVALID_HANDLE;
                    return Error(ErrorKind::Shm, "bootstrap timeout waiting for P0");
                }
            }
            va_base = bootstrap_block_->va_base.load(std::memory_order_relaxed);
            reserved = bootstrap_block_->reserved.load(std::memory_order_relaxed) != 0;

            LOG_INFO("bootstrap_join: attached to bootstrap as non-P0, va_base=0x"
                     << std::hex << va_base << std::dec);
        }

        return BootstrapJoinResult{va_base, reserved, is_p0};
    }

    int claim_rank() {
        int32_t my_pid = static_cast<int32_t>(::getpid());
        for (int i = 0; i < static_cast<int>(MAX_PROCESSES); ++i) {
            int32_t pid_in_slot = bootstrap_block_->slots[i].pid.load(std::memory_order_acquire);

            int32_t expected = 0;
            if (pid_in_slot == 0) {
                if (bootstrap_block_->slots[i].pid.compare_exchange_strong(
                        expected, my_pid,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    uint32_t gen = bootstrap_block_->slots[i].generation.load(std::memory_order_relaxed);
                    bootstrap_block_->slots[i].generation.store(gen + 1, std::memory_order_release);
                    bootstrap_block_->slots[i].ready.store(0, std::memory_order_relaxed);
                    last_seen_generation_[i] = gen + 1;
                    LOG_INFO("claim_rank: claimed slot " << i << " pid=" << my_pid << " gen=" << (gen + 1));
                    return i;
                }
                continue;
            }

            // Dead-slot detection (step 7). For POSIX (is_single_node),
            // all processes are same-node → kill(pid, 0) is reliable.
            // For UBSE, kill(pid, 0) returns ESRCH for ALL cross-node
            // processes (alive or dead), so we use ubs_mem_shm_get to
            // check the rank's metadata shm stage instead.
            uint32_t slot_node = bootstrap_block_->slots[i].node_slot_id
                .load(std::memory_order_acquire);

            bool is_dead = false;
            if (slot_node == local_node_slot_id_ || slot_node == 0) {
                // Same node, or slot_node unset (process may have died
                // before completing assign_node_index). kill(pid, 0) is
                // reliable for same-node processes. For slot_node==0 on
                // UBSE, this is a fallback — it works for same-node dead
                // processes, and for cross-node it returns ESRCH (which
                // we double-check below via ubs_mem_shm_get).
                is_dead = (pid_in_slot != my_pid &&
                           ::kill(pid_in_slot, 0) == -1 && errno == ESRCH);
            }

            if (!is_dead && slot_node != local_node_slot_id_ && slot_node != 0) {
                // Different node (UBSE only). This branch references
                // ShmProviderT::is_rank_dead_cross_node, a dependent name
                // that is only instantiated for UBSE (is_single_node=false).
                // For POSIX, this branch is discarded at compile time.
                if constexpr (!ShmProviderT::is_single_node) {
                    is_dead = ShmProviderT::is_rank_dead_cross_node(heap_id_, i);
                }
            }

            if (!is_dead && slot_node == 0 && pid_in_slot != my_pid) {
                // slot_node==0 and kill returned ESRCH. For UBSE, this
                // could be a cross-node alive process (kill always returns
                // ESRCH cross-node). Double-check with ubs_mem_shm_get.
                if constexpr (!ShmProviderT::is_single_node) {
                    is_dead = ShmProviderT::is_rank_dead_cross_node(heap_id_, i);
                }
            }

            if (is_dead) {
                // R3: Fix stale counters before reclaiming the slot.
                // Decrement node_proc_count for the dead rank's node.
                uint32_t stale_node_idx = bootstrap_block_->slots[i].node_index
                    .load(std::memory_order_acquire);
                if (stale_node_idx < MAX_PROCESSES) {
                    bootstrap_block_->node_proc_count[stale_node_idx]
                        .fetch_sub(1, std::memory_order_acq_rel);
                }
                // Reset metadata_refcount for this rank (the dead process
                // may have left stale refcounts).
                for (int j = 0; j < static_cast<int>(MAX_PROCESSES); ++j) {
                    bootstrap_block_->metadata_refcount[i][j]
                        .store(0, std::memory_order_relaxed);
                }

                LOG_INFO("claim_rank: slot " << i << " pid=" << pid_in_slot
                         << " node_slot=" << slot_node << " is dead, reclaiming");

                unlink_process_shms(i);

                expected = pid_in_slot;
                if (bootstrap_block_->slots[i].pid.compare_exchange_strong(
                        expected, my_pid,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    uint32_t gen = bootstrap_block_->slots[i].generation.load(std::memory_order_relaxed);
                    bootstrap_block_->slots[i].generation.store(gen + 1, std::memory_order_release);
                    bootstrap_block_->slots[i].ready.store(0, std::memory_order_relaxed);
                    bootstrap_block_->slots[i].node_slot_id.store(0, std::memory_order_relaxed);
                    bootstrap_block_->slots[i].node_index.store(UINT32_MAX, std::memory_order_relaxed);
                    last_seen_generation_[i] = gen + 1;
                    LOG_INFO("claim_rank: reclaimed slot " << i << " pid=" << my_pid << " gen=" << (gen + 1));
                    return i;
                }
            }
        }
        LOG_WARN("claim_rank: no empty or stale slot found");
        return -1;
    }

    // §1: Assign node_index (steps 7-8). Called from do_lazy_init AFTER
    // claim_rank, BEFORE incrementing node_proc_count/bootstrap_refcount.
    // Uses CAS to avoid two processes on the same node claiming different
    // node_index values. For POSIX (is_single_node=true), node_index stays
    // at 0 (no-op). local_node_slot_id_ is already cached in do_lazy_init
    // before this method is called.
    void assign_node_index() {
        if constexpr (ShmProviderT::is_single_node) {
            my_node_index_ = 0;
            return;
        }

        // local_node_slot_id_ is already set in do_lazy_init (from
        // ShmProvider::get_local_node_slot_id()). Publish it to our slot.
        bootstrap_block_->slots[rank_].node_slot_id.store(
            local_node_slot_id_, std::memory_order_release);

        // Step 1: Scan for an existing slot on my node (reuse its node_index)
        uint32_t my_node_index = UINT32_MAX;
        for (int k = 0; k < static_cast<int>(MAX_PROCESSES); ++k) {
            if (k == rank_) continue;
            uint32_t sid = bootstrap_block_->slots[k].node_slot_id
                .load(std::memory_order_acquire);
            if (sid == local_node_slot_id_ && sid != 0) {
                uint32_t idx = bootstrap_block_->slots[k].node_index
                    .load(std::memory_order_acquire);
                if (idx != UINT32_MAX) {
                    my_node_index = idx;
                    break;
                }
            }
        }

        // Step 2: If not found, scan for the lowest unused node_index
        if (my_node_index == UINT32_MAX) {
            bool used[MAX_PROCESSES] = {};
            for (int k = 0; k < static_cast<int>(MAX_PROCESSES); ++k) {
                uint32_t idx = bootstrap_block_->slots[k].node_index
                    .load(std::memory_order_acquire);
                if (idx < MAX_PROCESSES) used[idx] = true;
            }
            for (uint32_t i = 0; i < MAX_PROCESSES; ++i) {
                if (!used[i]) {
                    my_node_index = i;
                    break;
                }
            }
        }

        // Step 3: CAS — only the first writer wins; losers adopt the winner
        uint32_t expected = UINT32_MAX;
        if (!bootstrap_block_->slots[rank_].node_index
                .compare_exchange_strong(expected, my_node_index,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            my_node_index = expected;  // adopt the winner's value
        }

        // Step 4: Write-then-verify — re-scan for conflicting node_index
        // on same node. Another process on my node may have won the race
        // with a different index (e.g., step 2 picked different lowest-
        // unused values due to interleaving). Adopt the existing value.
        for (int k = 0; k < static_cast<int>(MAX_PROCESSES); ++k) {
            if (k == rank_) continue;
            uint32_t sid = bootstrap_block_->slots[k].node_slot_id
                .load(std::memory_order_acquire);
            uint32_t idx = bootstrap_block_->slots[k].node_index
                .load(std::memory_order_acquire);
            if (sid == local_node_slot_id_ && sid != 0 &&
                idx != UINT32_MAX && idx != my_node_index) {
                my_node_index = idx;
                bootstrap_block_->slots[rank_].node_index
                    .store(my_node_index, std::memory_order_release);
                break;
            }
        }

        my_node_index_ = my_node_index;
        LOG_INFO("assign_node_index: rank=" << rank_
                 << " node_slot_id=" << local_node_slot_id_
                 << " node_index=" << my_node_index_);
    }

    int reclaim_dead_slot() {
        int32_t my_pid = static_cast<int32_t>(::getpid());
        for (int i = 0; i < static_cast<int>(MAX_PROCESSES); ++i) {
            int32_t pid_in_slot = bootstrap_block_->slots[i].pid.load(std::memory_order_acquire);
            if (pid_in_slot == 0 || pid_in_slot == my_pid) continue;

            if (::kill(pid_in_slot, 0) == 0 || errno != ESRCH) continue;

            LOG_INFO("reclaim_dead_slot: slot " << i << " pid=" << pid_in_slot << " is dead, reclaiming");
            unlink_process_shms(i);

            int32_t expected = pid_in_slot;
            if (bootstrap_block_->slots[i].pid.compare_exchange_strong(
                    expected, my_pid,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                uint32_t gen = bootstrap_block_->slots[i].generation.load(std::memory_order_relaxed);
                bootstrap_block_->slots[i].generation.store(gen + 1, std::memory_order_release);
                bootstrap_block_->slots[i].ready.store(0, std::memory_order_relaxed);
                last_seen_generation_[i] = gen + 1;
                LOG_INFO("reclaim_dead_slot: claimed slot " << i << " gen=" << (gen + 1));
                return i;
            }
        }
        return -1;
    }

    void set_slot_ready(int rank, uint64_t metadata_size = 0) {
        if (bootstrap_block_ && rank >= 0) {
            if (metadata_size > 0) {
                bootstrap_block_->metadata_sizes[rank].store(
                    metadata_size, std::memory_order_release);
            }
            bootstrap_block_->slots[rank].ready.store(1, std::memory_order_release);
            if (bootstrap_ptr_) {
                size_t bs_size = round_up_shm_size(BOOTSTRAP_SIZE);
                ::msync(bootstrap_ptr_, bs_size, MS_SYNC);
            }
        }
    }

    bool wait_for_slot_ready(int rank, int timeout_ms = 30000) {
        if (!bootstrap_block_ || rank < 0) return false;
        int elapsed = 0;
        while (bootstrap_block_->slots[rank].ready.load(std::memory_order_acquire) != 1) {
            ::usleep(1000);
            elapsed++;
            if (elapsed >= timeout_ms) return false;
        }
        return true;
    }

    void attach_new_processes() {
        if (!bootstrap_block_) return;

        std::lock_guard<std::mutex> lock(remote_attach_mutex_);

        for (int k = 0; k < static_cast<int>(MAX_PROCESSES); ++k) {
            if (k == rank_) continue;

            int32_t pid_in_slot = bootstrap_block_->slots[k].pid.load(std::memory_order_acquire);
            if (pid_in_slot == 0) {
                // Slot cleared — process exited. Release our borrows on
                // its METADATA only (not data segments) so the exiting
                // process's wait_for_unreferenced() can progress.
                //
                // Data segment borrows are NOT released here because:
                //   - The local allocator may still reference the remote
                //     slabs (from cross-process free → attach → sized queue)
                //   - Munmapping data segments would cause SIGSEGV when
                //     the allocator accesses the slab data
                //   - Data segment borrows are released when the local
                //     process exits (detach_all_regions) or when UBSE
                //     auto-cleans on crash
                if (metadata_regions_[k].address != nullptr) {
                    LOG_INFO("attach_new_processes: P" << k
                             << " slot cleared (pid=0), releasing metadata borrow");
                    if (metadata_regions_[k].address && metadata_regions_[k].size > 0) {
                        ::munmap(metadata_regions_[k].address, metadata_regions_[k].size);
                        metadata_regions_[k] = Region{};
                    }
                    if (metadata_handles_[k] != ShmProviderT::INVALID_HANDLE) {
                        detach_with_refcount(k, METADATA, 0, metadata_handles_[k]);
                        metadata_handles_[k] = ShmProviderT::INVALID_HANDLE;
                    }
                    is_owner_for_process_[k] = false;
                }
                continue;
            }
            uint32_t slot_ready = bootstrap_block_->slots[k].ready.load(std::memory_order_acquire);
            if (slot_ready != 1) continue;

            uint32_t current_gen = bootstrap_block_->slots[k].generation.load(std::memory_order_acquire);

            if (metadata_regions_[k].address != nullptr) {
                if (current_gen == last_seen_generation_[k]) {
                    continue;
                }
                LOG_INFO("attach_new_processes: generation changed for P" << k
                         << " (" << last_seen_generation_[k] << " -> " << current_gen << "), re-attaching");
                if (metadata_regions_[k].address && metadata_regions_[k].size > 0) {
                    ::munmap(metadata_regions_[k].address, metadata_regions_[k].size);
                    metadata_regions_[k] = Region{};
                }
                if (metadata_handles_[k] != ShmProviderT::INVALID_HANDLE) {
                    // Use detach_with_refcount (NOT shm_close) to
                    // properly decrement the refcount that was
                    // incremented by the original attach_with_refcount.
                    // shm_close detaches without decrementing, causing
                    // a refcount leak → double-increment on re-attach.
                    detach_with_refcount(k, METADATA, 0, metadata_handles_[k]);
                    metadata_handles_[k] = ShmProviderT::INVALID_HANDLE;
                }
                for (int b = 0; b < DATA_BRACKETS; ++b) {
                    int seg_idx = 0;
                    for (auto& seg : segments_[k][b]) {
                        if (seg.region.address && seg.region.size > 0) {
                            ::munmap(seg.region.address, seg.region.size);
                        }
                        if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                            detach_with_refcount(k, static_cast<RegionType>(b + 1),
                                                 seg_idx, seg.handle);
                        }
                        ++seg_idx;
                    }
                    segments_[k][b].clear();
                }
                is_owner_for_process_[k] = false;
            }

            typename DistributedShmBackend<ShmProviderT>::SharedRegionLayout remote_sl =
                compute_distributed_shared_region_layout<DistributedAllocatorShared>(
                    k, MAX_PROCESSES, config_.huge_slots[k],
                    config_.slab_count_small[k], config_.slab_count_large[k]);

            auto res = attach_and_map_remote_region(k, METADATA, remote_sl.total_size,
                reinterpret_cast<void*>(va_layout_.metadata_va[k]), 0, &metadata_handles_[k]);
            if (is_err(res)) {
                LOG_WARN("attach_new_processes: failed to attach P" << k << " metadata");
                continue;
            }
            metadata_regions_[k] = unwrap(res);
            last_seen_generation_[k] = current_gen;
            LOG_INFO("attach_new_processes: attached P" << k << " metadata");
        }
    }

    void clear_bootstrap_slot() {
        LOG_INFO("clear_bootstrap_slot: entering (block=" << bootstrap_block_
                 << " pid=" << rank_ << ")");
        if (bootstrap_block_ && rank_ >= 0) {
            int32_t my_pid = static_cast<int32_t>(::getpid());
            int32_t expected = my_pid;
            LOG_INFO("clear_bootstrap_slot: writing slots[" << rank_ << "]");
            bootstrap_block_->slots[rank_].pid.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
            bootstrap_block_->slots[rank_].ready.store(0, std::memory_order_release);
            LOG_INFO("clear_bootstrap_slot: cleared slot " << rank_);
        }
    }

    // §9c: cleanup_bootstrap_mapping — munmap + close fd + decrement
    // bootstrap_refcount + conditional shm_detach_by_name.
    // For POSIX (is_single_node=true): just munmap + close fd (no
    // refcount, no detach — POSIX has no per-node borrow concept).
    // For UBSE (is_single_node=false): decrement bootstrap_refcount,
    // then munmap + close fd. If refcount==0 (last process on this
    // node with a bootstrap attach), call shm_detach_by_name.
    // For the lender's own node (self-map), the detach is a no-op at
    // OBMM level but still decrements the daemon's import_desc_cnt.
    //
    // The refcount decrement MUST happen BEFORE munmap — the pointer
    // into bootstrap shared memory is invalid after munmap.
    // The detach happens AFTER close_fd (UBSE returns internal error
    // 1005 if you detach while there's an active mmap on the
    // device-backed shm).
    // §9c: release_bootstrap_borrow — decrement refcount + close fd.
    // Does NOT munmap and does NOT shm_detach_by_name — those happen in
    // unmap_bootstrap_mapping() AFTER clear_bootstrap_slot().
    // UBSE requires munmap BEFORE shm_detach_by_name (error 1005 if
    // detach is called while an active mmap exists). So the detach info
    // (name + should_detach flag) is returned via out-params for the
    // caller to pass to unmap_bootstrap_mapping().
    void release_bootstrap_borrow(std::string& out_detach_name,
                                   bool& out_should_detach) {
        out_should_detach = false;
        if (bootstrap_block_ && !heap_id_.empty()) {
            out_detach_name = ShmProviderT::bootstrap_shm_name(heap_id_);
            if constexpr (!ShmProviderT::is_single_node) {
                if (my_node_index_ < MAX_PROCESSES) {
                    uint16_t bs_prev = bootstrap_block_
                        ->bootstrap_refcount[my_node_index_]
                        .fetch_sub(1, std::memory_order_acq_rel);
                    out_should_detach = (bs_prev == 1);
                }
            }
        }
        if (bootstrap_handle_ != ShmProviderT::INVALID_HANDLE) {
            ShmProviderT::shm_close_fd(bootstrap_handle_);
            bootstrap_handle_ = ShmProviderT::INVALID_HANDLE;
        }
    }

    // §9c: unmap_bootstrap_mapping — munmap + nullify + deferred detach.
    // Called AFTER clear_bootstrap_slot() (which needs bootstrap_block_
    // to be valid). munmap happens FIRST, then shm_detach_by_name
    // (UBSE: detach while active mmap → error 1005).
    // detach_name + should_detach are from release_bootstrap_borrow().
    void unmap_bootstrap_mapping(const std::string& detach_name,
                                 bool should_detach) {
        if (bootstrap_ptr_) {
            size_t bs_size = round_up_shm_size(BOOTSTRAP_SIZE);
            ::munmap(bootstrap_ptr_, bs_size);
            bootstrap_ptr_ = nullptr;
            bootstrap_block_ = nullptr;
        }
        if constexpr (!ShmProviderT::is_single_node) {
            if (should_detach && !detach_name.empty()) {
                ShmProviderT::shm_detach_by_name(detach_name);
            }
        }
    }

    // §9c: cleanup_bootstrap_mapping — convenience wrapper that calls
    // release_bootstrap_borrow() + unmap_bootstrap_mapping(). Used by
    // the POSIX path (where clear_bootstrap_slot() runs BEFORE this).
    // The UBSE path uses the split functions to keep bootstrap_block_
    // valid between borrow release and clear_bootstrap_slot().
    void cleanup_bootstrap_mapping() {
        std::string detach_name;
        bool should_detach = false;
        release_bootstrap_borrow(detach_name, should_detach);
        unmap_bootstrap_mapping(detach_name, should_detach);
    }

    Result<int> create_data_segment(int bracket, size_t requested_slabs) {
        int seg_idx = static_cast<int>(segments_[rank_][bracket].size());

        size_t seg_size;
        if (bracket == 0) {
            auto sl = SegmentLayout<Small>::compute(requested_slabs);
            seg_size = sl.total_size;
        } else if (bracket == 1) {
            auto sl = SegmentLayout<Large>::compute(requested_slabs);
            seg_size = sl.total_size;
        } else {
            seg_size = requested_slabs * HugeSize::SLAB_SIZE;
        }

        uintptr_t va = segment_va(rank_, bracket, seg_idx);
        RegionType rt = static_cast<RegionType>(bracket + 1);

        SegmentEntry entry;
        auto res = create_and_map_own_region(rt, seg_size,
            reinterpret_cast<void*>(va), seg_idx, &entry.handle);
        if (is_err(res)) return unwrap_err(res);
        entry.region = unwrap(res);
        segments_[rank_][bracket].push_back(std::move(entry));

        char* seg_base = static_cast<char*>(data_address(rank_, bracket, seg_idx));
        if (seg_base) {
            SegmentHeader* hdr = reinterpret_cast<SegmentHeader*>(seg_base);
            hdr->live_slab_count.store(0, std::memory_order_release);
            hdr->free_since_ns.store(0, std::memory_order_release);
        }

        SegmentDirectory* dir = segment_directory(rank_, bracket);
        if (dir) {
            dir->count.store(static_cast<uint32_t>(seg_idx) + 1, std::memory_order_release);
            if (static_cast<size_t>(seg_idx) < MAX_SEGMENTS) {
                std::string name = ShmProviderT::shm_name(heap_id_, rank_, rt, seg_idx);
                std::strncpy(dir->descs[seg_idx].name, name.c_str(), 63);
                dir->descs[seg_idx].name[63] = '\0';
                dir->descs[seg_idx].total_size = seg_size;
                dir->descs[seg_idx].slab_count = requested_slabs;
                dir->descs[seg_idx].ready.store(1, std::memory_order_release);
            }
        }

        LOG_INFO("create_data_segment: bracket=" << bracket << " seg=" << seg_idx
                 << " slabs=" << requested_slabs << " size=" << seg_size);
        return seg_idx;
    }

    void detach_segment(int bracket, int seg_idx) {
        if (rank_ < 0 || bracket < 0 || bracket >= DATA_BRACKETS) return;
        if (seg_idx < 0 || seg_idx >= static_cast<int>(segments_[rank_][bracket].size())) return;

        SegmentEntry& entry = segments_[rank_][bracket][seg_idx];
        if (entry.state != SegmentEntry::State::LIVE) return;

        entry.state = SegmentEntry::State::DETACHING;

        char* seg_base = static_cast<char*>(entry.region.address);
        if (seg_base) {
            SegmentHeader* hdr = reinterpret_cast<SegmentHeader*>(seg_base);
            (void)hdr;

            if (bracket == 0) {
                auto sl = SegmentLayout<Small>::compute(SegmentLayout<Small>::virtual_slabs_per_segment());
                SlabLocal<Small>* slab_arr = reinterpret_cast<SlabLocal<Small>*>(seg_base + sl.slab_local_offset);
                for (size_t i = 0; i < sl.slab_count; ++i) {
                    slab_arr[i].detached.store(1, std::memory_order_release);
                }
            } else if (bracket == 1) {
                auto sl = SegmentLayout<Large>::compute(SegmentLayout<Large>::virtual_slabs_per_segment());
                SlabLocal<Large>* slab_arr = reinterpret_cast<SlabLocal<Large>*>(seg_base + sl.slab_local_offset);
                for (size_t i = 0; i < sl.slab_count; ++i) {
                    slab_arr[i].detached.store(1, std::memory_order_release);
                }
            }
        }

        // On UBSE, we CANNOT munmap here — the inline reclaim check
        // runs inside allocate()/free(), and the caller may access slab
        // metadata through *slabs after the reclaim returns. Munmap'ing
        // here would leave dangling pointers.
        // Instead: just close the fd and mark slabs detached. The actual
        // munmap + shm_detach_by_name happens in delete_segment (when
        // decay2 expires), which is called from a SEPARATE reclaim cycle
        // (not inline in allocate/free).
        if (entry.handle != ShmProviderT::INVALID_HANDLE) {
            // Close fd only — don't call shm_detach_by_name yet (needs
            // munmap first on UBSE to avoid 1005). The munmap +
            // shm_detach_by_name happens later in delete_segment.
            ShmProviderT::shm_close_fd(entry.handle);
            entry.handle = ShmProviderT::INVALID_HANDLE;
        }

        SegmentDirectory* dir = segment_directory(rank_, bracket);
        if (dir && seg_idx < static_cast<int>(MAX_SEGMENTS)) {
            dir->descs[seg_idx].ready.store(0, std::memory_order_release);
        }

        entry.detached_since_ns = steady_now_ns();
        entry.state = SegmentEntry::State::DETACHED;
        total_detached_count_++;

        LOG_INFO("detach_segment: bracket=" << bracket << " seg=" << seg_idx
                 << " (LIVE->DETACHED)");
    }

    void delete_segment(int bracket, int seg_idx) {
        if (rank_ < 0 || bracket < 0 || bracket >= DATA_BRACKETS) return;
        if (seg_idx < 0 || seg_idx >= static_cast<int>(segments_[rank_][bracket].size())) return;

        SegmentEntry& entry = segments_[rank_][bracket][seg_idx];
        if (entry.state != SegmentEntry::State::DETACHED) return;

        entry.state = SegmentEntry::State::RETURNING;

        RegionType rt = static_cast<RegionType>(bracket + 1);
        std::string name = ShmProviderT::shm_name(heap_id_, rank_, rt, seg_idx);

        // On UBSE: munmap + shm_detach_by_name BEFORE shm_delete.
        // munmap must happen before shm_detach (1005 if active mmap).
        // shm_detach must happen before shm_delete (1024 if import active).
        if constexpr (!ShmProviderT::is_single_node) {
            void* addr = entry.region.address;
            size_t size = entry.region.size;

            if (addr && size > 0) {
                ::munmap(addr, size);

                // Re-mmap anonymous at the same address (MAP_FIXED) so the
                // slab layer can safely access SlabLocal (which lives in
                // the data segment at slab_local_offset). Without this,
                // pop() would segfault when accessing slabs.local(idx).next
                // for a slab whose SlabLocal was in the just-munmap'd
                // segment. The new mapping is zero-filled; we set
                // detached=1 + next=0 below so pop skips these slabs.
                void* new_addr = ::mmap(addr, size, PROT_READ | PROT_WRITE,
                                        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
                                        -1, 0);
                if (new_addr != MAP_FAILED) {
                    char* seg_base = static_cast<char*>(new_addr);
                    if (bracket == 0) {
                        auto sl = SegmentLayout<Small>::compute(
                            SegmentLayout<Small>::virtual_slabs_per_segment());
                        SlabLocal<Small>* slab_arr =
                            reinterpret_cast<SlabLocal<Small>*>(
                                seg_base + sl.slab_local_offset);
                        for (size_t i = 0; i < sl.slab_count; ++i) {
                            slab_arr[i].next.store(0, std::memory_order_relaxed);
                            slab_arr[i].detached.store(1, std::memory_order_release);
                        }
                    } else if (bracket == 1) {
                        auto sl = SegmentLayout<Large>::compute(
                            SegmentLayout<Large>::virtual_slabs_per_segment());
                        SlabLocal<Large>* slab_arr =
                            reinterpret_cast<SlabLocal<Large>*>(
                                seg_base + sl.slab_local_offset);
                        for (size_t i = 0; i < sl.slab_count; ++i) {
                            slab_arr[i].next.store(0, std::memory_order_relaxed);
                            slab_arr[i].detached.store(1, std::memory_order_release);
                        }
                    }
                } else {
                    LOG_ERROR("delete_segment: re-mmap anonymous failed, "
                             << "bracket=" << bracket << " seg=" << seg_idx
                             << " errno=" << errno);
                }
            }

            // Now safe to detach the borrow (no active device mmap — the
            // mapping is now anonymous, not device-backed).
            ShmProviderT::shm_detach_by_name(name);
        }

        ShmProviderT::shm_unlink(name);

        // On UBSE, shm_delete may fail with 1024 (ATTACH_USING) if
        // cross-node borrowers are still attached. Check if the delete
        // actually succeeded. If not, keep DETACHED state and retry on
        // the next check_one_segment_for_reclaim / return_segment_purge.
        if constexpr (!ShmProviderT::is_single_node) {
            if (ShmProviderT::shm_exists(name)) {
                LOG_INFO("delete_segment: shm_delete pending (borrowers still "
                         "attached), bracket=" << bracket << " seg=" << seg_idx);
                entry.state = SegmentEntry::State::DETACHED;
                return;
            }
        }

        // Don't clear entry.region.address — it now points to the
        // anonymous mapping kept alive for the slab layer.
        entry.state = SegmentEntry::State::RETURNED;
        total_returned_count_++;

        LOG_INFO("delete_segment: bracket=" << bracket << " seg=" << seg_idx
                 << " (DETACHED->RETURNED)");
    }

    void check_one_segment_for_reclaim(int bracket) {
        if (!return_enabled_ || rank_ < 0) return;
        if (bracket < 0 || bracket >= 2) return;

        size_t& cursor = (bracket == 0) ? reclaim_scan_small_cursor_ : reclaim_scan_large_cursor_;
        auto& segs = segments_[rank_][bracket];
        if (segs.empty()) return;

        size_t idx = cursor % segs.size();
        cursor++;

        SegmentEntry& entry = segs[idx];
        auto state = entry.state;
        if (state != SegmentEntry::State::LIVE && state != SegmentEntry::State::DETACHED) return;

        uint64_t now = steady_now_ns();

        if (state == SegmentEntry::State::LIVE) {
            char* seg_base = static_cast<char*>(entry.region.address);
            if (!seg_base) return;
            SegmentHeader* hdr = reinterpret_cast<SegmentHeader*>(seg_base);
            if (hdr->live_slab_count.load(std::memory_order_acquire) > 0) return;
            uint64_t since = hdr->free_since_ns.load(std::memory_order_acquire);
            if (since == 0 || now - since < decay1_ns_) return;
            detach_segment(bracket, static_cast<int>(idx));
        }
        // NOTE: DETACHED→RETURNED is NOT done here. delete_segment munmaps
        // the data segment, but this function is called inline inside
        // allocate()/free_offset() (via reclaim_fn_small/large). After the
        // reclaim returns, the caller may access slab metadata through
        // *slabs which points into the just-munmap'd segment → segfault.
        // The DETACHED→RETURNED transition (munmap + shm_detach + shm_delete)
        // happens only in return_segment_purge, called via purge() from
        // OUTSIDE allocate/free.
    }

    void return_segment_purge() {
        if (rank_ < 0) return;
        for (int b = 0; b < 2; ++b) {
            for (size_t s = 0; s < segments_[rank_][b].size(); ++s) {
                auto& entry = segments_[rank_][b][s];
                if (entry.state == SegmentEntry::State::LIVE) {
                    char* seg_base = static_cast<char*>(entry.region.address);
                    if (seg_base) {
                        SegmentHeader* hdr = reinterpret_cast<SegmentHeader*>(seg_base);
                        if (hdr->live_slab_count.load(std::memory_order_acquire) == 0) {
                            detach_segment(b, static_cast<int>(s));
                            delete_segment(b, static_cast<int>(s));
                        }
                    }
                } else if (entry.state == SegmentEntry::State::DETACHED) {
                    delete_segment(b, static_cast<int>(s));
                }
            }
        }
    }

    ReturnStats return_stats() const {
        ReturnStats stats;
        if (rank_ < 0) return stats;
        for (int b = 0; b < 2; ++b) {
            for (size_t s = 0; s < segments_[rank_][b].size(); ++s) {
                auto state = segments_[rank_][b][s].state;
                if (state == SegmentEntry::State::LIVE) stats.segments_live++;
                else if (state == SegmentEntry::State::DETACHED) stats.segments_detached++;
                else if (state == SegmentEntry::State::RETURNED) stats.segments_returned++;
            }
        }
        stats.total_detached_count = total_detached_count_;
        stats.total_returned_count = total_returned_count_;
        stats.total_recreated_count = total_recreated_count_;
        return stats;
    }

    Result<int> attach_data_segment(int remote_pid, int bracket, int seg_idx) {
        RegionType rt = static_cast<RegionType>(bracket + 1);
        uintptr_t va = segment_va(remote_pid, bracket, seg_idx);

        SegmentDirectory* dir = segment_directory(remote_pid, bracket);
        if (!dir || seg_idx >= static_cast<int>(dir->count.load(std::memory_order_acquire))) {
            return Error(ErrorKind::Shm, "segment not ready");
        }

        size_t seg_size = dir->descs[seg_idx].total_size;
        if (seg_size == 0) return Error(ErrorKind::Shm, "segment size is 0");

        size_t effective_size = round_up_shm_size(seg_size);
        std::string name = ShmProviderT::shm_name(heap_id_, remote_pid, static_cast<int>(rt), seg_idx);
        // §9a: Use attach_with_refcount (optimistic increment BEFORE attach).
        // On mmap failure below, detach_with_refcount decrements and
        // conditionally calls shm_detach_by_name (only when refcount==0).
        auto handle_res = attach_with_refcount(remote_pid, rt, seg_idx);
        if (is_err(handle_res)) return unwrap_err(handle_res);
        ShmHandle handle = unwrap(handle_res);

        // Register with the fault handler BEFORE the mmap. This ensures that
        // if another thread faults on the segment VA between the fd-open and
        // the mmap (the eager-attach race window), the handler can use the
        // cached fd to mmap the segment. No-op if the handler isn't active.
        fh_register_segment(va, effective_size,
                            ShmProviderT::shm_handle_fd(handle),
                            /*already_attached=*/false);

        int flags = MAP_SHARED | (va_reserved_ ? MAP_FIXED : MAP_FIXED_NOREPLACE);
        if (detect_overcommit() && rt >= DATA_SMALL) {
            flags |= MAP_NORESERVE;
        }

        void* ptr = ::mmap(reinterpret_cast<void*>(va), effective_size,
                           PROT_READ | PROT_WRITE, flags,
                           ShmProviderT::shm_handle_fd(handle), 0);
        if (ptr == MAP_FAILED) {
            LOG_WARN("attach_data_segment mmap failed for " << name
                      << " at VA " << reinterpret_cast<void*>(va)
                      << ": " << strerror(errno));
            // §9a: detach_with_refcount (NOT shm_close) on mmap failure.
            // shm_close would call shm_detach unconditionally, releasing
            // the node borrow even when other procs on the same node
            // still hold an attach.
            detach_with_refcount(remote_pid, rt, seg_idx, handle);
            return Error(ErrorKind::Mmap, std::error_code(errno, std::system_category()));
        }

        // Mark the segment as attached in the fault handler registry.
        // The eager mmap just succeeded, so the handler now knows this
        // segment is backed.
        fh_mark_attached(va);

        SegmentEntry entry;
        entry.region.id = RegionId(name);
        entry.region.address = ptr;
        entry.region.size = effective_size;
        entry.region.capacity = effective_size;
        entry.region.created = false;
        entry.handle = handle;

        while (static_cast<int>(segments_[remote_pid][bracket].size()) <= seg_idx) {
            segments_[remote_pid][bracket].push_back(SegmentEntry());
        }
        segments_[remote_pid][bracket][seg_idx] = std::move(entry);

        LOG_INFO("attach_data_segment: pid=" << remote_pid << " bracket=" << bracket
                 << " seg=" << seg_idx << " addr=" << ptr);
        return seg_idx;
    }

    void check_remote_segments() {
        for (int k = 0; k < total_processes_; ++k) {
            if (k == rank_) continue;
            for (int b = 0; b < DATA_BRACKETS; ++b) {
                SegmentDirectory* dir = segment_directory(k, b);
                if (!dir) continue;
                uint32_t dir_count = reinterpret_cast<const volatile uint32_t*>(&dir->count)[0];
                int current = static_cast<int>(segments_[k][b].size());
                for (int s = current; s < static_cast<int>(dir_count); ++s) {
                    if (reinterpret_cast<const volatile uint32_t*>(&dir->descs[s].ready)[0] == 1) {
                        auto res = attach_data_segment(k, b, s);
                        if (is_err(res)) {
                            LOG_WARN("check_remote_segments: failed to attach P" << k
                                     << " bracket=" << b << " seg=" << s);
                        }
                    }
                }
                int avail = static_cast<int>(segments_[k][b].size());
                for (int s = 0; s < avail; ++s) {
                    auto& seg = segments_[k][b][s];

                    RegionType rt = static_cast<RegionType>(b + 1);
                    std::string name = ShmProviderT::shm_name(heap_id_, k,
                        static_cast<int>(rt), s);

                    if (seg.state == SegmentEntry::State::LIVE) {
                        if (reinterpret_cast<const volatile uint32_t*>(&dir->descs[s].ready)[0] != 0) continue;

                        // ready==0: lender has detached (DETACHED on lender side).
                        if constexpr (ShmProviderT::is_single_node) {
                            // POSIX: shm_unlink is immediate, check if gone.
                            bool exists = ShmProviderT::shm_exists(name);
                            if (!exists) {
                                LOG_INFO("check_remote_segments: P" << k << " b=" << b
                                         << " s=" << s << " returned by owner (shm unlinked)");
                                if (seg.region.address && seg.region.size > 0) {
                                    ::munmap(seg.region.address, seg.region.size);
                                    seg.region.address = nullptr;
                                    seg.region.size = 0;
                                }
                                if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                                    detach_with_refcount(k, rt, s, seg.handle);
                                    seg.handle = ShmProviderT::INVALID_HANDLE;
                                }
                                seg.state = SegmentEntry::State::RETURNED;
                            }
                        } else {
                            // UBSE: lender's shm_delete can't succeed while
                            // we're attached (1024 ATTACH_USING). We must
                            // detach our own import so the lender can delete.
                            // Transition: LIVE -> DETACHED (munmap + detach)
                            // Then on next check: DETACHED -> RETURNED
                            LOG_INFO("check_remote_segments: P" << k << " b=" << b
                                     << " s=" << s << " lender ready=0, detaching our import");
                            if (seg.region.address && seg.region.size > 0) {
                                ::munmap(seg.region.address, seg.region.size);
                                seg.region.address = nullptr;
                                seg.region.size = 0;
                            }
                            if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                                detach_with_refcount(k, rt, s, seg.handle);
                                seg.handle = ShmProviderT::INVALID_HANDLE;
                            }
                            seg.state = SegmentEntry::State::DETACHED;
                            seg.detached_since_ns = steady_now_ns();
                        }
                    } else if (seg.state == SegmentEntry::State::DETACHED) {
                        // Check if lender has deleted the shm (after we
                        // released our import in the LIVE->DETACHED transition).
                        if constexpr (!ShmProviderT::is_single_node) {
                            bool exists = ShmProviderT::shm_exists(name);
                            if (!exists) {
                                LOG_INFO("check_remote_segments: P" << k << " b=" << b
                                         << " s=" << s << " returned (shm deleted by owner)");
                                seg.state = SegmentEntry::State::RETURNED;
                            }
                        }
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Multi-proc/node refcount wrappers (§2c, §9a, §9b)
    // -----------------------------------------------------------------------
    //
    // These wrappers centralize the per-node, per-shm refcount logic so
    // the ~20 existing shm_close call sites don't need individual audits
    // (R6). The pattern is:
    //
    //   attach_with_refcount:  optimistic increment BEFORE shm_attach,
    //                          undo on attach failure (R1).
    //   create_with_refcount:  optimistic increment BEFORE shm_try_create,
    //                          undo on create failure (§9b, self-borrow).
    //   detach_with_refcount:  shm_close_fd + decrement + conditional
    //                          shm_detach_by_name (only when refcount==0).
    //
    // For POSIX (is_single_node=true), the refcount logic is skipped
    // entirely (R11) — the wrappers degenerate to plain attach/create/close.

    // Get pointer to the per-node refcount atomic for a given shm.
    // For METADATA: refcount is in BootstrapBlock.metadata_refcount[lender][*]
    // For DATA_*:   refcount is in BootstrapBlock.data_refcount[lender][bracket][seg][*]
    // Both are in the bootstrap block, which is always mapped — no dependency
    // on lender's metadata. This prevents the "null refcount ptr" issue when
    // attach_new_processes munmaps lender's metadata.
    std::atomic<uint16_t>* get_refcount_ptr(int lender_rank, int type,
                                             int seg_idx) {
        if (!bootstrap_block_) return nullptr;
        if (type == static_cast<int>(METADATA)) {
            return &bootstrap_block_->metadata_refcount[lender_rank][0];
        } else {
            int bracket = static_cast<int>(type) - 1;  // DATA_SMALL=1→0, DATA_LARGE=2→1
            size_t idx = ((static_cast<size_t>(lender_rank) * BootstrapBlock::DATA_REFCOUNT_BRACKETS + bracket)
                          * MAX_SEGMENTS + seg_idx) * MAX_PROCESSES;
            return &bootstrap_block_->data_refcount[idx];
        }
    }

    // attach_with_refcount — increments refcount BEFORE attach (R1, §9a)
    // Used by attach_and_map_remote_region and attach_data_segment.
    Result<ShmHandle> attach_with_refcount(int lender_rank, RegionType type,
                                            int seg_idx) {
        std::string name = ShmProviderT::shm_name(heap_id_, lender_rank,
                                                   static_cast<int>(type), seg_idx);

        if constexpr (ShmProviderT::is_single_node) {
            // POSIX: no refcount, just attach
            return ShmProviderT::shm_attach(name);
        }

        // UBSE: optimistic increment — reserve our slot in the refcount
        // BEFORE shm_attach. If a concurrent exiting process on the same
        // node decrements to 0 between our increment and our attach, it
        // will call shm_detach_by_name, releasing the node borrow. Our
        // subsequent shm_attach will get EXISTED (re-import) or succeed
        // freshly. The R1 race is closed: the increment is visible to
        // the decrementer before we attach.
        std::atomic<uint16_t>* refc = get_refcount_ptr(lender_rank, type, seg_idx);
        uint16_t before = refc[my_node_index_].load(std::memory_order_acquire);
        refc[my_node_index_].fetch_add(1, std::memory_order_acq_rel);
        LOG_INFO("attach_with_refcount INC: name=" << name
                 << " lender=" << lender_rank << " type=" << static_cast<int>(type)
                 << " seg=" << seg_idx << " node=" << my_node_index_
                 << " before=" << before << " -> " << (before + 1));

        auto res = ShmProviderT::shm_attach(name);
        if (is_err(res)) {
            // Attach failed — undo the optimistic increment
            uint16_t prev = refc[my_node_index_]
                .fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                // We were the last refcount holder, but we never actually
                // attached. The borrow might have been released by a
                // concurrent detach. Try to release — NO_ATTACH is
                // harmless (shm_detach_by_name treats it as success).
                ShmProviderT::shm_detach_by_name(name);
            }
            return res;
        }
        return res;
    }

    // create_with_refcount — for lender's own shm (self-borrow path, §9b)
    // Used by create_and_map_own_region. The lender's own node gets a
    // self-map borrow from shm_create (obmm_import on lender's own node).
    // The self-map detach is a no-op at OBMM level, but the daemon's
    // import_desc_cnt is still decremented — so refcounting is MANDATORY
    // to prevent premature shm_delete completion.
    Result<ShmHandle> create_with_refcount(int lender_rank, RegionType type,
                                             int seg_idx, size_t effective_size) {
        std::string name = ShmProviderT::shm_name(heap_id_, lender_rank,
                                                   static_cast<int>(type), seg_idx);

        if constexpr (ShmProviderT::is_single_node) {
            // POSIX: no refcount, just create
            return ShmProviderT::shm_try_create(name, effective_size);
        }

        // UBSE: optimistic increment BEFORE shm_try_create
        std::atomic<uint16_t>* refc = get_refcount_ptr(lender_rank, type, seg_idx);
        uint16_t before = refc[my_node_index_].load(std::memory_order_acquire);
        refc[my_node_index_].fetch_add(1, std::memory_order_acq_rel);
        LOG_INFO("create_with_refcount INC: name=" << name
                 << " lender=" << lender_rank << " type=" << static_cast<int>(type)
                 << " seg=" << seg_idx << " node=" << my_node_index_
                 << " before=" << before << " -> " << (before + 1));

        auto res = ShmProviderT::shm_try_create(name, effective_size);
        if (is_err(res)) {
            // Create failed — undo the optimistic increment
            uint16_t prev = refc[my_node_index_]
                .fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                // We were the last refcount holder, but we never actually
                // created/attached. Try to release — NO_ATTACH is harmless.
                // For self-map (lender's own node), this is a no-op at OBMM
                // level (MemShmUnImportExecutor skips self-map).
                ShmProviderT::shm_detach_by_name(name);
            }
            return res;
        }
        return res;
    }

    // detach_with_refcount — close fd, decrement, conditional detach (§2c, §9a)
    // Used by detach_all_regions, error paths in attach/create, and the
    // uffd-attached cleanup. The handle's fd is always closed; the
    // shm_detach_by_name is only called when refcount==0 (last on node).
    void detach_with_refcount(int lender_rank, RegionType type, int seg_idx,
                               ShmHandle& handle, bool do_detach = true) {
        ShmProviderT::shm_close_fd(handle);

        if constexpr (ShmProviderT::is_single_node) {
            return;  // POSIX: no detach needed
        }

        std::atomic<uint16_t>* refc = get_refcount_ptr(lender_rank, type, seg_idx);
        if (!refc) {
            LOG_WARN("detach_with_refcount: null refcount ptr for lender=" << lender_rank
                     << " type=" << static_cast<int>(type) << " seg=" << seg_idx);
            return;
        }
        uint16_t prev = refc[my_node_index_]
            .fetch_sub(1, std::memory_order_acq_rel);
        std::string name = ShmProviderT::shm_name(heap_id_, lender_rank,
                                                   static_cast<int>(type), seg_idx);
        LOG_INFO("detach_with_refcount: name=" << name
                 << " my_node_index=" << my_node_index_
                 << " prev=" << prev << " do_detach=" << do_detach);
        if (do_detach && prev == 1) {
            // Last process on this node — release the node-level borrow
            ShmProviderT::shm_detach_by_name(name);
        } else if (do_detach && prev != 1) {
            LOG_WARN("detach_with_refcount: refcount was " << prev
                     << " (expected 1), NOT detaching " << name);
        }
    }

    Result<Region> create_and_map_own_region(RegionType type, size_t size, void* fixed_va,
                                                 int segment_idx = 0, ShmHandle* out_handle = nullptr) {
        size_t effective_size = round_up_shm_size(size);
        std::string name = ShmProviderT::shm_name(heap_id_, rank_, static_cast<int>(type), segment_idx);
        LOG_INFO("create_own_region: name=" << name << " logical=" << size << " effective=" << effective_size << " VA=" << fixed_va);
        // §9b: Use create_with_refcount (optimistic increment BEFORE
        // shm_try_create). The lender's own node gets a self-map borrow
        // from shm_create (obmm_import on lender's own node). The
        // self-map detach is a no-op at OBMM level, but the daemon's
        // import_desc_cnt is still decremented — so refcounting is
        // MANDATORY to prevent premature shm_delete completion.
        auto handle_res = create_with_refcount(rank_, type, segment_idx, effective_size);
        if (is_err(handle_res)) {
            auto err = unwrap_err(handle_res);
            if (err.kind == ErrorKind::ShmExists) {
                LOG_WARN("create_own_region: shm " << name << " already exists (stale from dead process?), unlinking and retrying");
                ShmProviderT::shm_unlink(name);
                // Retry with create_with_refcount (the optimistic increment
                // was already undone inside the previous create_with_refcount
                // failure path).
                handle_res = create_with_refcount(rank_, type, segment_idx, effective_size);
                if (is_err(handle_res)) {
                    auto err2 = unwrap_err(handle_res);
                    if (err2.kind == ErrorKind::ShmExists) {
                        // shm_delete failed (1024 — daemon still has the
                        // stale shm attached). Fall back to attaching the
                        // existing shm. P0 will reinitialize the metadata
                        // region, overwriting the stale data. This is safe
                        // because the VA layout (sizes, offsets) is
                        // determined by compile-time constants — same
                        // heap_id → same layout.
                        LOG_WARN("create_own_region: can't delete stale shm "
                                 << name << " (1024), falling back to attach");
                        handle_res = attach_with_refcount(rank_, type, segment_idx);
                    }
                }
            }
            if (is_err(handle_res)) {
                return unwrap_err(handle_res);
            }
        }
        ShmHandle handle = unwrap(handle_res);

        uintptr_t va = reinterpret_cast<uintptr_t>(fixed_va);
        // Register with fault handler before mmap (covers the race window
        // between fd-open and mmap for own-segment creation).
        fh_register_segment(va, effective_size,
                            ShmProviderT::shm_handle_fd(handle),
                            /*already_attached=*/false);

        int flags = MAP_SHARED | (va_reserved_ ? MAP_FIXED : MAP_FIXED_NOREPLACE);
        if (detect_overcommit() && type >= DATA_SMALL) {
            flags |= MAP_NORESERVE;
        }

        void* ptr = ::mmap(fixed_va, effective_size, PROT_READ | PROT_WRITE, flags, ShmProviderT::shm_handle_fd(handle), 0);
        if (ptr == MAP_FAILED) {
            LOG_WARN("mmap MAP_FIXED_NOREPLACE failed for " << name
                      << " at VA " << fixed_va << ": " << strerror(errno));
            // §9b: detach_with_refcount (NOT shm_close) + shm_unlink on
            // mmap failure. The refcount was incremented in
            // create_with_refcount; detach_with_refcount decrements and
            // only calls shm_detach_by_name when refcount==0.
            detach_with_refcount(rank_, type, segment_idx, handle);
            ShmProviderT::shm_unlink(name);
            return Error(ErrorKind::Mmap, std::error_code(errno, std::system_category()));
        }

        fh_mark_attached(va);

        LOG_TRACE("create_own_region ok: name=" << name << " addr=" << ptr << " size=" << effective_size);

        if (out_handle) {
            *out_handle = handle;
        } else {
            // §9b: shm_close_fd only — refcount stays at 1, decremented
            // in detach_all_regions when the process exits.
            ShmProviderT::shm_close_fd(handle);
        }

        Region region;
        region.id = RegionId(name);
        region.address = ptr;
        region.size = effective_size;
        region.capacity = effective_size;
        region.created = true;
        return region;
    }

    Result<Region> attach_and_map_remote_region(int remote_pid, RegionType type,
                                                     size_t expected_size, void* fixed_va,
                                                     int segment_idx = 0, ShmHandle* out_handle = nullptr) {
        size_t effective_size = round_up_shm_size(expected_size);
        std::string name = ShmProviderT::shm_name(heap_id_, remote_pid, static_cast<int>(type), segment_idx);
        LOG_INFO("attach_remote_region: name=" << name << " expected=" << expected_size << " effective=" << effective_size << " VA=" << fixed_va);
        // §9a: Use attach_with_refcount (optimistic increment BEFORE attach).
        // On mmap failure below, detach_with_refcount decrements and
        // conditionally calls shm_detach_by_name (only when refcount==0).
        auto handle_res = attach_with_refcount(remote_pid, type, segment_idx);
        if (is_err(handle_res)) {
            return unwrap_err(handle_res);
        }
        ShmHandle handle = unwrap(handle_res);

        uintptr_t va = reinterpret_cast<uintptr_t>(fixed_va);
        // Register with fault handler before mmap (covers the race window
        // between fd-open and mmap for remote-segment attach).
        fh_register_segment(va, effective_size,
                            ShmProviderT::shm_handle_fd(handle),
                            /*already_attached=*/false);

        int flags = MAP_SHARED | (va_reserved_ ? MAP_FIXED : MAP_FIXED_NOREPLACE);
        if (detect_overcommit() && type >= DATA_SMALL) {
            flags |= MAP_NORESERVE;
        }

        void* ptr = ::mmap(fixed_va, effective_size, PROT_READ | PROT_WRITE, flags, ShmProviderT::shm_handle_fd(handle), 0);
        if (ptr == MAP_FAILED) {
            LOG_WARN("mmap MAP_FIXED_NOREPLACE attach failed for " << name
                      << " at VA " << fixed_va << ": " << strerror(errno));
            // §9a: detach_with_refcount (NOT shm_close) on mmap failure.
            detach_with_refcount(remote_pid, type, segment_idx, handle);
            return Error(ErrorKind::Mmap, std::error_code(errno, std::system_category()));
        }

        fh_mark_attached(va);

        LOG_TRACE("attach_remote_region ok: name=" << name << " addr=" << ptr << " size=" << effective_size);

        if (out_handle) {
            *out_handle = handle;
        } else {
            // R6: Use shm_close_fd (not shm_close) to avoid premature detach.
            // The refcount is still 1 (incremented in attach_with_refcount).
            // It will be decremented in detach_all_regions when the process exits.
            ShmProviderT::shm_close_fd(handle);
        }

        Region region;
        region.id = RegionId(name);
        region.address = ptr;
        region.size = effective_size;
        region.capacity = effective_size;
        region.created = false;
        return region;
    }

    static bool detect_overcommit() {
        int fd = ::open("/proc/sys/vm/overcommit_memory", O_RDONLY);
        if (fd >= 0) {
            char buf[32];
            ssize_t n = ::read(fd, buf, sizeof(buf));
            ::close(fd);
            if (n >= 1) {
                return (buf[0] == '0' || buf[0] == '1');
            }
        }
        return true;
    }

    Result<bool> create_own_regions(size_t metadata_size,
                                     size_t data_small_slabs,
                                     size_t data_large_slabs,
                                     size_t data_huge_slots) {
        LOG_INFO("create_own_regions: pid=" << rank_ << " metadata=" << metadata_size
                 << " small_slabs=" << data_small_slabs << " large_slabs=" << data_large_slabs
                 << " huge_slots=" << data_huge_slots);

        if (rank_ == 0) {
            auto res = create_and_map_own_region(METADATA, metadata_size,
                reinterpret_cast<void*>(va_layout_.metadata_va[0]), 0, &metadata_handles_[0]);
            if (is_err(res)) return unwrap_err(res);
            metadata_regions_[0] = unwrap(res);
        }

        size_t slab_counts[] = {data_small_slabs, data_large_slabs, data_huge_slots};
        RegionType types[] = {DATA_SMALL, DATA_LARGE, DATA_HUGE};
        uintptr_t base_vas[] = {
            va_layout_.data_small_va[rank_],
            va_layout_.data_large_va[rank_],
            va_layout_.data_huge_va[rank_]
        };

        for (int b = 0; b < DATA_BRACKETS; ++b) {
            if (slab_counts[b] == 0) continue;

            size_t seg_size;
            if (b < 2) {
                if (b == 0) {
                    auto sl = SegmentLayout<Small>::compute(slab_counts[b]);
                    seg_size = sl.total_size;
                } else {
                    auto sl = SegmentLayout<Large>::compute(slab_counts[b]);
                    seg_size = sl.total_size;
                }
            } else {
                seg_size = HUGE_SLOTS_PER_SEGMENT * HugeSize::SLAB_SIZE;
            }

            SegmentEntry entry;
            auto res = create_and_map_own_region(types[b], seg_size,
                reinterpret_cast<void*>(base_vas[b]), 0, &entry.handle);
            if (is_err(res)) return unwrap_err(res);
            entry.region = unwrap(res);
            segments_[rank_][b].push_back(std::move(entry));
        }

        if (rank_ != 0) {
            auto res = create_and_map_own_region(METADATA, metadata_size,
                reinterpret_cast<void*>(va_layout_.metadata_va[rank_]), 0, &metadata_handles_[rank_]);
            if (is_err(res)) return unwrap_err(res);
            metadata_regions_[rank_] = unwrap(res);
        }

        own_created_ = true;
        is_owner_for_process_[rank_] = true;
        LOG_INFO("create_own_regions complete: pid=" << rank_);
        return true;
    }

    bool wait_for_all_shms(int timeout_ms = 30000) {
        int total_expected = 0;
        for (int k = 0; k < total_processes_; ++k) {
            total_expected++;
            if (config_.slab_count_small[k] > 0) total_expected++;
            if (config_.slab_count_large[k] > 0) total_expected++;
            if (config_.huge_slots[k] > 0) total_expected++;
        }
        LOG_INFO("wait_for_all_shms: expected=" << total_expected << " timeout=" << timeout_ms << "ms");

        int check_interval_ms = 100;
        int elapsed = 0;

        while (elapsed < timeout_ms) {
            int found = 0;
            for (int k = 0; k < total_processes_; ++k) {
                std::string name = ShmProviderT::shm_name(heap_id_, k, METADATA, 0);
                if (ShmProviderT::shm_exists(name)) found++;

                if (config_.slab_count_small[k] > 0) {
                    name = ShmProviderT::shm_name(heap_id_, k, DATA_SMALL, 0);
                    if (ShmProviderT::shm_exists(name)) found++;
                }
                if (config_.slab_count_large[k] > 0) {
                    name = ShmProviderT::shm_name(heap_id_, k, DATA_LARGE, 0);
                    if (ShmProviderT::shm_exists(name)) found++;
                }
                if (config_.huge_slots[k] > 0) {
                    name = ShmProviderT::shm_name(heap_id_, k, DATA_HUGE, 0);
                    if (ShmProviderT::shm_exists(name)) found++;
                }
            }
            if (found >= total_expected) {
                LOG_INFO("wait_for_all_shms: found " << found << "/" << total_expected << " after " << elapsed << "ms");
                return true;
            }
            LOG_TRACE("wait_for_all_shms: found " << found << "/" << total_expected << " elapsed=" << elapsed << "ms");
            ::usleep(check_interval_ms * 1000);
            elapsed += check_interval_ms;
        }

        LOG_WARN("Timeout waiting for all shm objects");
        return false;
    }

    Result<bool> attach_all_remote_regions() {
        LOG_INFO("attach_all_remote_regions: pid=" << rank_ << " total=" << total_processes_);
        for (int k = 0; k < total_processes_; ++k) {
            if (k == rank_) continue;

            is_owner_for_process_[k] = false;

            typename DistributedShmBackend<ShmProviderT>::SharedRegionLayout remote_sl = compute_distributed_shared_region_layout<DistributedAllocatorShared>(
                k, total_processes_, config_.huge_slots[k],
                config_.slab_count_small[k], config_.slab_count_large[k]);

            LOG_INFO("attach_remote: k=" << k << " metadata_size=" << remote_sl.total_size);

            auto res = attach_and_map_remote_region(k, METADATA, remote_sl.total_size,
                reinterpret_cast<void*>(va_layout_.metadata_va[k]), 0, &metadata_handles_[k]);
            if (is_err(res)) return unwrap_err(res);
            metadata_regions_[k] = unwrap(res);

            if (config_.slab_count_small[k] > 0) {
                auto sl = SegmentLayout<Small>::compute(config_.slab_count_small[k]);
                size_t seg_size = std::max(sl.total_size, static_cast<size_t>(ShmProviderT::min_shm_size));
                SegmentEntry entry;
                res = attach_and_map_remote_region(k, DATA_SMALL, seg_size,
                    reinterpret_cast<void*>(va_layout_.data_small_va[k]), 0, &entry.handle);
                if (is_err(res)) return unwrap_err(res);
                entry.region = unwrap(res);
                segments_[k][0].push_back(std::move(entry));
            }

            if (config_.slab_count_large[k] > 0) {
                auto sl = SegmentLayout<Large>::compute(config_.slab_count_large[k]);
                size_t seg_size = std::max(sl.total_size, static_cast<size_t>(ShmProviderT::min_shm_size));
                SegmentEntry entry;
                res = attach_and_map_remote_region(k, DATA_LARGE, seg_size,
                    reinterpret_cast<void*>(va_layout_.data_large_va[k]), 0, &entry.handle);
                if (is_err(res)) return unwrap_err(res);
                entry.region = unwrap(res);
                segments_[k][1].push_back(std::move(entry));
            }

            if (config_.huge_slots[k] > 0) {
                size_t remote_data_huge_size = config_.huge_slots[k] * HugeSize::SLAB_SIZE;
                SegmentEntry entry;
                res = attach_and_map_remote_region(k, DATA_HUGE, remote_data_huge_size,
                    reinterpret_cast<void*>(va_layout_.data_huge_va[k]), 0, &entry.handle);
                if (is_err(res)) return unwrap_err(res);
                entry.region = unwrap(res);
                segments_[k][2].push_back(std::move(entry));
            }
        }

        all_attached_ = true;
        return true;
    }

    // Collect shm names for all ranks on this node, by reading the
    // segment directories from each rank's metadata (if still mapped)
    // and from segments_ (which tracks segments we attached/created).
    // Must be called BEFORE detach_all_regions (which munmaps everything).
    void collect_node_shm_names() {
        node_shm_names_.clear();
        if (!bootstrap_block_) return;

        uint32_t local_slot = local_node_slot_id_;
        for (int k = 0; k < static_cast<int>(MAX_PROCESSES); ++k) {
            // Check if this rank is on our node
            uint32_t slot_node = bootstrap_block_->slots[k].node_slot_id
                .load(std::memory_order_acquire);
            if (slot_node != local_slot && local_slot != 0) {
                // Also include our own rank even if node_slot_id
                // wasn't set yet (edge case)
                if (k != rank_) continue;
            }

            // Metadata name (always one per rank)
            node_shm_names_.push_back(
                ShmProviderT::shm_name(heap_id_, k, METADATA, 0));

            // Data segments: read from segment directory if mapped,
            // otherwise fall back to segments_ entries.
            for (int b = 0; b < DATA_BRACKETS; ++b) {
                SegmentDirectory* dir = segment_directory(k, b);
                if (dir) {
                    // Read from segment directory — has ALL segments
                    // for this rank, not just the ones we attached.
                    for (size_t s = 0; s < MAX_SEGMENTS; ++s) {
                        if (dir->descs[s].ready.load(std::memory_order_acquire) == 1 &&
                            dir->descs[s].name[0] != '\0') {
                            node_shm_names_.push_back(dir->descs[s].name);
                        }
                    }
                } else {
                    // Metadata not mapped — fall back to segments_
                    for (auto& seg : segments_[k][b]) {
                        if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                            node_shm_names_.push_back(
                                ShmProviderT::shm_name(heap_id_, k,
                                    static_cast<RegionType>(b + 1),
                                    static_cast<int>(
                                        &seg - segments_[k][b].data())));
                        }
                    }
                }
            }
        }

        LOG_INFO("collect_node_shm_names: collected " << node_shm_names_.size()
                 << " shm names for cleanup");
    }

    void detach_all_regions() {
        if constexpr (ShmProviderT::has_reliable_unreferenced_check) {
            // When am_last_on_node, skip detaching OWN regions —
            // unlink_node_shms will detach+delete them via shm_unlink
            // (which needs the self-map borrow active to race the
            // daemon's delete window). When NOT last, detach all
            // (including own) since unlink_node_shms won't run.
            bool skip_own = false;
            if (bootstrap_block_ && my_node_index_ < MAX_PROCESSES) {
                skip_own = (bootstrap_block_->node_proc_count[my_node_index_]
                            .load(std::memory_order_acquire) == 0);
            }
            LOG_INFO("detach_all_regions: pid=" << rank_
                     << " skip_own=" << skip_own);
            // Remote regions (k != rank_): metadata + data segments.
            // Use detach_with_refcount (§2c, §9a): close fd + decrement
            // refcount + conditional shm_detach_by_name (only when
            // refcount==0, i.e., last process on this node for this shm).
            for (int k = 0; k < total_processes_; ++k) {
                if (k == rank_) continue;
                for (int b = 0; b < DATA_BRACKETS; ++b) {
                    int seg_idx = 0;
                    for (auto& seg : segments_[k][b]) {
                        if (seg.region.address && seg.region.size > 0) {
                            ::munmap(seg.region.address, seg.region.size);
                            seg.region.address = nullptr;
                            seg.region.size = 0;
                        }
                        if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                            detach_with_refcount(k, static_cast<RegionType>(b + 1),
                                                 seg_idx, seg.handle);
                            seg.handle = ShmProviderT::INVALID_HANDLE;
                        }
                        ++seg_idx;
                    }
                }
                if (metadata_regions_[k].address && metadata_regions_[k].size > 0) {
                    ::munmap(metadata_regions_[k].address, metadata_regions_[k].size);
                    metadata_regions_[k].address = nullptr;
                    metadata_regions_[k].size = 0;
                }
                if (metadata_handles_[k] != ShmProviderT::INVALID_HANDLE) {
                    detach_with_refcount(k, METADATA, 0, metadata_handles_[k]);
                    metadata_handles_[k] = ShmProviderT::INVALID_HANDLE;
                }
            }
            // Own regions (k == rank_): always munmap + close fd +
            // decrement refcount. Skip the shm_detach_by_name when
            // am_last_on_node (unlink_node_shms will detach+delete
            // via shm_unlink, which needs the borrow active to race
            // the daemon's delete window).
            for (int b = 0; b < DATA_BRACKETS; ++b) {
                int seg_idx = 0;
                for (auto& seg : segments_[rank_][b]) {
                    if (seg.region.address && seg.region.size > 0) {
                        ::munmap(seg.region.address, seg.region.size);
                        seg.region.address = nullptr;
                    }
                    if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                        detach_with_refcount(rank_, static_cast<RegionType>(b + 1),
                                             seg_idx, seg.handle, !skip_own);
                        seg.handle = ShmProviderT::INVALID_HANDLE;
                    }
                    ++seg_idx;
                }
            }
            if (metadata_regions_[rank_].address && metadata_regions_[rank_].size > 0) {
                ::munmap(metadata_regions_[rank_].address, metadata_regions_[rank_].size);
                metadata_regions_[rank_].address = nullptr;
            }
            if (metadata_handles_[rank_] != ShmProviderT::INVALID_HANDLE) {
                detach_with_refcount(rank_, METADATA, 0, metadata_handles_[rank_], !skip_own);
                metadata_handles_[rank_] = ShmProviderT::INVALID_HANDLE;
            }

            // Detach segments that were attached on-demand by the uffd
            // handler. These are NOT in segments_[k][b] (the uffd handler
            // avoids touching that vector to prevent data races). They are
            // tracked in g_uffd_attached. The handler thread is already
            // stopped (uffd_uninstall ran earlier in the destructor), so
            // no race on the vector — but we lock anyway for safety.
            //
            // The uffd handler calls ShmProviderT::shm_attach directly
            // (NOT attach_with_refcount), so the refcount was never
            // incremented. We munmap + shm_close_fd here. The node-level
            // borrow is released later by detach_uffd_attached() — only
            // when this is the last process on the node (am_last_on_node).
            // This avoids prematurely releasing the borrow when other
            // processes on the same node still have active mappings.
            {
                std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
                for (auto& e : g_uffd_attached) {
                    if (e.addr) {
                        ::munmap(e.addr, e.size);
                    }
                    if (e.size > 0) {
                        ShmHandle h;
                        std::memcpy(&h, e.handle_buf, sizeof(ShmHandle));
                        if (h != ShmProviderT::INVALID_HANDLE) {
                            ShmProviderT::shm_close_fd(h);
                        }
                    }
                }
                // DON'T clear g_uffd_attached — detach_uffd_attached()
                // needs the names to call shm_detach_by_name.
                LOG_INFO("detach_all_regions: uffd-attached segments munmapped+fd-closed");
            }
        }
    }

    // Release node-level borrows for uffd-attached segments. Called
    // ONLY when am_last_on_node (last process on this node). Iterates
    // g_uffd_attached and calls shm_detach_by_name for each segment's
    // shm name. Without this, cross-node borrows established by the
    // uffd handler are never released, causing the peer process to
    // hang in wait_for_unreferenced().
    void detach_uffd_attached() {
        if constexpr (ShmProviderT::has_reliable_unreferenced_check) {
            std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
            for (auto& e : g_uffd_attached) {
                if (e.size > 0) {
                    ShmHandle h;
                    std::memcpy(&h, e.handle_buf, sizeof(ShmHandle));
                    if (h != ShmProviderT::INVALID_HANDLE) {
                        std::string name(h.name);
                        ShmProviderT::shm_detach_by_name(name);
                    }
                }
            }
            g_uffd_attached.clear();
            LOG_INFO("detach_uffd_attached: node-level borrows released");
        } else {
            // POSIX: no node-level borrow to release. Just clear the vector.
            std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
            g_uffd_attached.clear();
        }
    }

    // Clear uffd-attached tracking without releasing node-level borrows.
    // Used when NOT am_last_on_node (the last process on this node will
    // call detach_uffd_attached to release the borrows).
    void clear_uffd_attached() {
        std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
        g_uffd_attached.clear();
    }

    void wait_for_unreferenced() {
        if constexpr (ShmProviderT::has_reliable_unreferenced_check) {
            // No timeout: wait indefinitely for other nodes to release
            // their borrows. If a remote process crashes, this process
            // hangs (expected behavior — user kills it, cleanup_all_
            // distributed_shms handles stale shms). A timeout would
            // cause premature exit → leftover shms when all processes
            // exit successfully.
            auto wait_one = [&](const std::string& name) {
                while (!ShmProviderT::shm_is_unreferenced(name)) {
                    ::usleep(1000);
                }
            };
            // Own metadata + data segments
            std::string name = ShmProviderT::shm_name(heap_id_, rank_, METADATA, 0);
            wait_one(name);
            for (int b = 0; b < DATA_BRACKETS; ++b) {
                for (size_t s = 0; s < segments_[rank_][b].size(); ++s) {
                    name = ShmProviderT::shm_name(heap_id_, rank_, static_cast<RegionType>(b + 1), static_cast<int>(s));
                    wait_one(name);
                }
            }
            // Bootstrap shm: only the owner (rank 0) waits.
            // Non-owners (rank > 0) just release their own borrow
            // (in release_bootstrap_borrow, before this call) and
            // exit — they don't delete the bootstrap and have no
            // need to wait for other processes' borrows.
            // The owner waits to ensure all other nodes released
            // their borrows before the bootstrap is cleaned up.
            // Without this, a non-owner hangs if another process
            // (e.g., a consumer stuck in lookup_by_type_blocking)
            // still holds a bootstrap borrow.
            if (rank_ == 0) {
                name = ShmProviderT::bootstrap_shm_name(heap_id_);
                wait_one(name);
            }
        }
    }

    void unlink_own_shms() {
        if constexpr (ShmProviderT::has_reliable_unreferenced_check) {
            if (is_owner_for_process_[rank_]) {
                std::string name = ShmProviderT::shm_name(heap_id_, rank_, METADATA, 0);
                ShmProviderT::shm_unlink(name);
                for (int b = 0; b < DATA_BRACKETS; ++b) {
                    for (size_t s = 0; s < segments_[rank_][b].size(); ++s) {
                        name = ShmProviderT::shm_name(heap_id_, rank_, static_cast<RegionType>(b + 1), static_cast<int>(s));
                        ShmProviderT::shm_unlink(name);
                    }
                }
            }
        }
    }

    // §5d: unlink_node_shms — delete ALL shms created by ANY rank on this
    // node. Called by the destructor when am_last_on_node (§5c). Scans
    // bootstrap slots to find all ranks on this node, then constructs shm
    // names directly (shm_list_with_prefix returns empty after the local
    // node's borrows are released by detach_all_regions). Also detaches +
    // deletes the bootstrap shm (its borrow is released in
    // cleanup_bootstrap_mapping, which runs AFTER this — so we must detach
    // it here before deleting).
    void unlink_node_shms() {
        if constexpr (ShmProviderT::has_reliable_unreferenced_check) {
            // Use collected names (from collect_node_shm_names, called
            // before detach_all_regions). Falls back to blind guess
            // if collection wasn't done (node_shm_names_ empty).
            if (!node_shm_names_.empty()) {
                for (const auto& name : node_shm_names_) {
                    LOG_INFO("unlink_node_shms: deleting " << name);
                    ShmProviderT::shm_unlink(name);
                }
            } else {
                // Fallback: blind-guess metadata + 48 data segment
                // indices per rank on this node.
                uint32_t local_slot = local_node_slot_id_;
                for (int k = 0; k < static_cast<int>(MAX_PROCESSES); ++k) {
                    uint32_t slot_node = bootstrap_block_->slots[k].node_slot_id
                        .load(std::memory_order_acquire);
                    if (slot_node != local_slot && local_slot != 0) {
                        if (k != rank_) continue;
                    }
                    ShmProviderT::shm_unlink(
                        ShmProviderT::shm_name(heap_id_, k, METADATA, 0));
                    for (int b = 0; b < 3; ++b) {
                        for (int s = 0; s < 16; ++s) {
                            ShmProviderT::shm_unlink(
                                ShmProviderT::shm_name(heap_id_, k,
                                    static_cast<RegionType>(b + 1), s));
                        }
                    }
                }
            }

            // Bootstrap shm: munmap + close fd first (UBSE returns
            // 1005 if shm_detach is called while there's an active
            // mmap or open device fd), then shm_unlink (which
            // detaches + retries delete).
            if (bootstrap_ptr_) {
                size_t bs_mmap_size = round_up_shm_size(BOOTSTRAP_SIZE);
                ::munmap(bootstrap_ptr_, bs_mmap_size);
                bootstrap_ptr_ = nullptr;
                bootstrap_block_ = nullptr;
            }
            if (bootstrap_handle_ != ShmProviderT::INVALID_HANDLE) {
                ShmProviderT::shm_close_fd(bootstrap_handle_);
                bootstrap_handle_ = ShmProviderT::INVALID_HANDLE;
            }
            std::string bs_name = ShmProviderT::bootstrap_shm_name(heap_id_);
            ShmProviderT::shm_unlink(bs_name);
        }
    }

    void cleanup_remote_regions() {
        if constexpr (!ShmProviderT::has_reliable_unreferenced_check) {
            LOG_INFO("cleanup_remote_regions: pid=" << rank_);
            for (int k = 0; k < total_processes_; ++k) {
                if (k == rank_) continue;
                for (int b = 0; b < DATA_BRACKETS; ++b) {
                    for (auto& seg : segments_[k][b]) {
                        if (seg.region.address && seg.region.size > 0) {
                            ::munmap(seg.region.address, seg.region.size);
                            seg.region.address = nullptr;
                            seg.region.size = 0;
                        }
                        if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                            ShmProviderT::shm_close(seg.handle);
                            seg.handle = ShmProviderT::INVALID_HANDLE;
                        }
                    }
                }
                if (metadata_regions_[k].address && metadata_regions_[k].size > 0) {
                    ::munmap(metadata_regions_[k].address, metadata_regions_[k].size);
                    metadata_regions_[k].address = nullptr;
                    metadata_regions_[k].size = 0;
                }
                if (metadata_handles_[k] != ShmProviderT::INVALID_HANDLE) {
                    ShmProviderT::shm_close(metadata_handles_[k]);
                    metadata_handles_[k] = ShmProviderT::INVALID_HANDLE;
                }
            }
        }
    }

    void cleanup_own_shms() {
        if constexpr (!ShmProviderT::has_reliable_unreferenced_check) {
            LOG_INFO("cleanup_own_shms: pid=" << rank_);
            for (int b = 0; b < DATA_BRACKETS; ++b) {
                RegionType rt = static_cast<RegionType>(b + 1);
                for (size_t s = 0; s < segments_[rank_][b].size(); ++s) {
                    std::string name = ShmProviderT::shm_name(heap_id_, rank_, rt, static_cast<int>(s));
                    auto& seg = segments_[rank_][b][s];
                    if (seg.region.address && seg.region.size > 0) {
                        ::munmap(seg.region.address, seg.region.size);
                        seg.region.address = nullptr;
                    }
                    if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                        ShmProviderT::shm_close(seg.handle);
                        seg.handle = ShmProviderT::INVALID_HANDLE;
                    }
                    if (is_owner_for_process_[rank_]) {
                        ShmProviderT::shm_unlink(name);
                    }
                }
            }
            std::string mname = ShmProviderT::shm_name(heap_id_, rank_, METADATA, 0);
            if (metadata_regions_[rank_].address && metadata_regions_[rank_].size > 0) {
                ::munmap(metadata_regions_[rank_].address, metadata_regions_[rank_].size);
                metadata_regions_[rank_].address = nullptr;
            }
            if (metadata_handles_[rank_] != ShmProviderT::INVALID_HANDLE) {
                ShmProviderT::shm_close(metadata_handles_[rank_]);
                metadata_handles_[rank_] = ShmProviderT::INVALID_HANDLE;
            }
            if (is_owner_for_process_[rank_]) {
                ShmProviderT::shm_unlink(mname);
            }
        }
    }

    void unlink_process_shms(int dead_pid) {
        std::string name = ShmProviderT::shm_name(heap_id_, dead_pid, METADATA, 0);
        if (metadata_regions_[dead_pid].address && metadata_regions_[dead_pid].size > 0) {
            ::munmap(metadata_regions_[dead_pid].address, metadata_regions_[dead_pid].size);
            metadata_regions_[dead_pid].address = nullptr;
            metadata_regions_[dead_pid].size = 0;
        }
        if (metadata_handles_[dead_pid] != ShmProviderT::INVALID_HANDLE) {
            ShmProviderT::shm_close(metadata_handles_[dead_pid]);
            metadata_handles_[dead_pid] = ShmProviderT::INVALID_HANDLE;
        }
        ShmProviderT::shm_unlink(name);

        for (int b = 0; b < DATA_BRACKETS; ++b) {
            RegionType rt = static_cast<RegionType>(b + 1);
            for (size_t s = 0; s < segments_[dead_pid][b].size(); ++s) {
                name = ShmProviderT::shm_name(heap_id_, dead_pid, rt, static_cast<int>(s));
                auto& seg = segments_[dead_pid][b][s];
                if (seg.region.address && seg.region.size > 0) {
                    ::munmap(seg.region.address, seg.region.size);
                    seg.region.address = nullptr;
                    seg.region.size = 0;
                }
                if (seg.handle != ShmProviderT::INVALID_HANDLE) {
                    ShmProviderT::shm_close(seg.handle);
                    seg.handle = ShmProviderT::INVALID_HANDLE;
                }
                if constexpr (ShmProviderT::has_reliable_unreferenced_check) {
                    while (!ShmProviderT::shm_is_unreferenced(name)) {
                        ::usleep(1000);
                    }
                }
                ShmProviderT::shm_unlink(name);
            }
        }

        is_owner_for_process_[dead_pid] = false;
    }

    ~DistributedShmBackend() {
        if (rank_ < 0) return;
        LOG_INFO("~DistributedShmBackend: starting cleanup (pid=" << rank_ << ")");
        uffd_uninstall();
        LOG_INFO("~DistributedShmBackend: uffd_uninstall done");
        fh_uninstall();
        LOG_INFO("~DistributedShmBackend: fh_uninstall done");

        if constexpr (ShmProviderT::has_reliable_unreferenced_check) {
            // §5c cleanup flow for UBSE (multi-proc/node aware):
            // 1. Decrement node_proc_count FIRST — detach_all_regions
            //    checks it to decide whether to skip own regions
            //    (am_last → skip own, let unlink_node_shms handle them).
            // 2. detach_all_regions (remote always; own only if NOT last).
            // 3. clear_bootstrap_slot.
            // 4. If am_last: detach_uffd_attached + wait + unlink_node_shms.
            // 5. cleanup_bootstrap_mapping.
            bool am_last_on_node = false;
            if (bootstrap_block_ && my_node_index_ < MAX_PROCESSES) {
                uint32_t prev = bootstrap_block_->node_proc_count[my_node_index_]
                    .fetch_sub(1, std::memory_order_acq_rel);
                am_last_on_node = (prev == 1);
            }

            // Collect shm names BEFORE detach_all_regions (which
            // munmaps metadata, making segment directories unreadable).
            collect_node_shm_names();

            detach_all_regions();
            LOG_INFO("~DistributedShmBackend: detach_all_regions done");

            // Clear bootstrap slot BEFORE munmap + detach + wait.
            // bootstrap_block_ is still valid (munmap happens in
            // unmap_bootstrap_mapping below).
            // Discovery deadlock caveat: clearing the slot before
            // wait_for_unreferenced blocks late discovery. Accepted
            // edge case — see reset() in global.hpp for full rationale.
            clear_bootstrap_slot();
            LOG_INFO("~DistributedShmBackend: clear_bootstrap_slot done");

            // Release own bootstrap borrow (munmap + shm_detach_by_name).
            // shm_detach_by_name MUST be after munmap (UBSE 1005 if
            // active mmap) and BEFORE wait_for_unreferenced (releases
            // own borrow so wait only blocks on OTHERS).
            std::string bs_detach_name;
            bool bs_should_detach = false;
            release_bootstrap_borrow(bs_detach_name, bs_should_detach);
            unmap_bootstrap_mapping(bs_detach_name, bs_should_detach);
            LOG_INFO("~DistributedShmBackend: bootstrap borrow released + unmapped");

            if (am_last_on_node) {
                detach_uffd_attached();
                LOG_INFO("~DistributedShmBackend: detach_uffd_attached done");
                wait_for_unreferenced();
                LOG_INFO("~DistributedShmBackend: wait_for_unreferenced done");
                unlink_node_shms();
                LOG_INFO("~DistributedShmBackend: unlink_node_shms done");
            } else {
                {
                    std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
                    g_uffd_attached.clear();
                }
            }
        } else {
            // POSIX path (is_single_node=true, no refcount):
            clear_bootstrap_slot();
            LOG_INFO("~DistributedShmBackend: clear_bootstrap_slot done");
            cleanup_remote_regions();
            cleanup_own_shms();
            // POSIX: also clean up uffd-attached segments (handles are just
            // fds, shm_close = close(fd). Not strictly necessary since
            // process exit closes fds, but keeps /proc/self/maps clean
            // and consistent with the UBSE path.
            {
                std::lock_guard<std::mutex> lk(g_uffd_attached_mtx);
                for (auto& e : g_uffd_attached) {
                    if (e.addr) {
                        ::munmap(e.addr, e.size);
                    }
                    if (e.size > 0) {
                        ShmHandle h;
                        std::memcpy(&h, e.handle_buf, sizeof(ShmHandle));
                        if (h != ShmProviderT::INVALID_HANDLE) {
                            // §10: shm_close_fd (POSIX: same as shm_close).
                            ShmProviderT::shm_close_fd(h);
                        }
                    }
                }
                g_uffd_attached.clear();
            }
            cleanup_bootstrap_mapping();
            LOG_INFO("~DistributedShmBackend: cleanup_bootstrap_mapping done");
            // R5: Don't call cleanup_bootstrap(heap_id_) — the bootstrap shm
            // is shared across all nodes. Let the cleanup script handle it.
            // (The old code called cleanup_bootstrap for rank_==0, which
            // could SIGBUS other procs on the same node if they're still
            // accessing the bootstrap shm.)
        }
        LOG_INFO("~DistributedShmBackend: releasing reservation");
        release_reservation();
        LOG_INFO("~DistributedShmBackend: cleanup complete");
    }
};

// -------------------------------------------------------------------------
// extern template declarations — suppress implicit instantiation in user
// code. The actual instantiations live in libuballoc.so
// (src/uballoc_instances.cpp), compiled once.
//
// Users who want a custom ShmProvider should #define UBALLOC_NO_EXTERN_TEMPLATE
// before including any uballoc header to remove these declarations and
// restore implicit-instantiation (header-only) behavior.
// -------------------------------------------------------------------------
#ifndef UBALLOC_NO_EXTERN_TEMPLATE
#ifdef UBALLOC_USE_UBSE
extern template class DistributedShmBackend<UBShmProvider>;
#else
extern template class DistributedShmBackend<PosixShmProvider>;
#endif
#endif

}
