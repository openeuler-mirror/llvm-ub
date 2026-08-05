// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

#include "ubse/ubs_engine_mem.h"
#include "ubse/ubs_error.h"
#include "ubse/ubs_engine_log.h"

#include "error.hpp"
#include "log.hpp"

namespace uballoc {

struct UBShmProvider {
    static constexpr bool has_reliable_unreferenced_check = true;
    static constexpr bool is_single_node = false;
    static constexpr size_t min_shm_size = 4 * 1024 * 1024;
    static constexpr size_t shm_size_granularity = 2 * 1024 * 1024;

    // Cached local node slot_id (R7). Set once in provider_init() via
    // ubs_topo_node_local_get. Used by shm_is_unreferenced (§5a) to filter
    // out the local node's own borrow. 0 = unset (will match node 0's
    // slot_id if slot_id is 0, which is fine — UBSE slot_ids start at 1).
    static inline std::atomic<uint32_t> s_local_node_slot_id{0};

    static uint32_t get_local_node_slot_id() {
        return s_local_node_slot_id.load(std::memory_order_acquire);
    }

    // Kept for backwards-compatibility. init() no longer takes
    // these as parameters — they are derived from va_base at runtime.
    static constexpr uintptr_t metadata_base    = DEFAULT_VA_BASE;
    static constexpr uintptr_t process_stride   = PROCESS_STRIDE_SMALL;
    static constexpr uintptr_t data_small_base  = DEFAULT_VA_BASE + METADATA_STRIDE;
    static constexpr uintptr_t data_large_base  = DEFAULT_VA_BASE + METADATA_STRIDE + DATA_SMALL_STRIDE;
    static constexpr uintptr_t data_huge_base   = DEFAULT_VA_BASE + METADATA_STRIDE + DATA_SMALL_STRIDE + DATA_LARGE_STRIDE;

    static constexpr std::string_view device_path_prefix = "/dev/obmm_shmdev";

    struct ShmHandle {
        int fd = -1;
        char name[64] = {};

        bool operator==(const ShmHandle& other) const {
            return fd == other.fd;
        }

        bool operator!=(const ShmHandle& other) const {
            return !(*this == other);
        }
    };

    static constexpr ShmHandle INVALID_HANDLE = {-1, {}};

    static void FileLogCallbackNull([[maybe_unused]] uint32_t level, [[maybe_unused]] const char *msg) {
        return;
    }

    static void FileLogCallbackPrint([[maybe_unused]] uint32_t level, const char *msg) {
        if (msg && msg[0]) {
            size_t len = std::strlen(msg);
            if (msg[len - 1] == '\n') {
                std::cout << "[UBSE] " << msg << std::flush;
            } else {
                std::cout << "[UBSE] " << msg << "\n" << std::flush;
            }
        }
    }

    static void provider_init() {
        // Silence UBSE runtime logs unless the user has explicitly raised the
        // log level to Ubse (the most verbose level). When LOG level < Ubse,
        // register a no-op callback so UBSE doesn't print to screen.
        reregister_log_callback();
        // NOTE: ubs_topo_node_local_get is called in cache_local_node_slot_id()
        // AFTER bootstrap_join (which initializes the UBSE runtime via
        // shm_create). Calling it here (before runtime init) may interfere
        // with subsequent shm_create calls.
    }

    // Cache local node slot_id (R7). Called from do_lazy_init AFTER
    // bootstrap_join (which initializes the UBSE runtime via shm_create).
    // ubs_topo_node_local_get may need the runtime to be initialized.
    static void cache_local_node_slot_id() {
        ubs_topo_node_t local;
        if (ubs_topo_node_local_get(&local) == 0) {
            s_local_node_slot_id.store(local.slot_id, std::memory_order_release);
            LOG_INFO("cache_local_node_slot_id: slot_id=" << local.slot_id);
        } else {
            LOG_WARN("cache_local_node_slot_id: ubs_topo_node_local_get failed");
        }
    }

    // Re-register the no-op log callback. Called by provider_init() and
    // after every ubs_mem_shm_create()/ubs_mem_shm_attach() call, because
    // the UBSE runtime may reset the callback to a verbose default during
    // cluster-wide shm creation (the first ubs_mem_shm_create in a process
    // initializes the runtime and overwrites any previously-registered
    // callback).
    static void reregister_log_callback() {
        if (::uballoc::log::should_log(::uballoc::LogLevel::Ubse)) {
            ubs_engine_log_callback_register(FileLogCallbackPrint);
        } else {
            ubs_engine_log_callback_register(FileLogCallbackNull);
        }
    }

    // Bootstrap shm name (UBSE convention: no leading "/", "ub-" prefix).
    static std::string bootstrap_shm_name(const std::string& heap_id) {
        return "ub-" + heap_id + "-bootstrap";
    }

    static std::string shm_name(const std::string& heap_id, int rank, int region_type, int segment_idx = 0) {
        static const char * suffixes[] = {"-m", "-ds", "-dl", "-dh"};
        std::string name = "ub-" + heap_id + "-p" + std::to_string(rank) + suffixes[region_type];
        if (region_type > 0 && segment_idx > 0) {
            name += "-s" + std::to_string(segment_idx);
        }
        return name;
    }

    static Result<ShmHandle> shm_create(const std::string&name, size_t size) {
        uint8_t usr_info[UBS_MEM_MAX_USR_INFO_LEN] = {0};

        ubs_topo_node_t *node_list = nullptr;
        uint32_t node_cnt = 0;
        int ret = ubs_topo_node_list(&node_list, &node_cnt);
        if (UBS_SUCCESS != ret) {
            LOG_WARN("get nodes failed");
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        }

        ubs_topo_node_t local_node;
        ret = ubs_topo_node_local_get(&local_node);
        if (UBS_SUCCESS != ret) {
            LOG_WARN("get local node failed");
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        }

        ubs_mem_nodes_t region;
        region.node_cnt = node_cnt;
        for (uint32_t i = 0; i < node_cnt; ++i)
            region.slot_ids[i] = node_list[i].slot_id;

        ubs_mem_nodes_t provider;
        provider.slot_ids[0] = local_node.slot_id;
        provider.node_cnt = 1;

        uint64_t flag = UBS_MEM_FLAG_NO_WR_DELAY | UBS_MEM_FLAG_SHM_ANONYMOUS;

        ret = ubs_mem_shm_create(name.c_str(), size, usr_info, flag, &region, &provider);
        if (ret == static_cast<int>(UBS_ENGINE_ERR_EXISTED)) {
            // Stale shm from a previous crashed run. Try to clean up.
            ubs_mem_shm_detach(name.c_str());
            int del_ret = ubs_mem_shm_delete(name.c_str());
            if (del_ret == 0) {
                // Delete succeeded, retry create.
                ret = ubs_mem_shm_create(name.c_str(), size, usr_info, flag, &region, &provider);
            } else {
                // Can't delete (e.g., error 1024: shm still in use by
                // daemon from a crashed previous process). Fall back to
                // attaching to the existing shm.
                return shm_attach_inner(name, false);
            }
        }
        if (ret != 0) {
            LOG_WARN("ubs_mem_shm_create failed, ret=" << ret);
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        } else {
            LOG_INFO("ubs_mem_shm_create success, name=" << name.c_str());
        }

        // Re-register the no-op log callback after ubs_mem_shm_create.
        // The UBSE runtime may reset the callback to a verbose default
        // during cluster-wide shm creation (the first create in a
        // process initializes the runtime and overwrites the callback).
        reregister_log_callback();

        return shm_attach_inner(name, false);
    }

    static Result<ShmHandle> shm_try_create(const std::string& name, size_t size) {
        uint8_t usr_info[UBS_MEM_MAX_USR_INFO_LEN] = {0};

        ubs_topo_node_t *node_list = nullptr;
        uint32_t node_cnt = 0;
        int ret = ubs_topo_node_list(&node_list, &node_cnt);
        if (UBS_SUCCESS != ret) {
            LOG_WARN("get nodes failed");
        }

        ubs_topo_node_t local_node;
        ret = ubs_topo_node_local_get(&local_node);
        if (UBS_SUCCESS != ret) {
            LOG_WARN("get local node failed");
        }

        ubs_mem_nodes_t region;
        region.node_cnt = node_cnt;
        for (uint32_t i = 0; i < node_cnt; ++i)
            region.slot_ids[i] = node_list[i].slot_id;

        ubs_mem_nodes_t provider;
        provider.slot_ids[0] = local_node.slot_id;
        provider.node_cnt = 1;

        uint64_t flag = UBS_MEM_FLAG_NO_WR_DELAY | UBS_MEM_FLAG_SHM_ANONYMOUS;

        ret = ubs_mem_shm_create(name.c_str(), size, usr_info, flag, &region, &provider);
        if (ret == static_cast<int>(UBS_ENGINE_ERR_EXISTED)) {
            return Error(ErrorKind::ShmExists);
        }
        if (ret != 0) {
            LOG_WARN("ubs_mem_shm_create failed, ret=" << ret);
        }

        // Re-register the no-op log callback after ubs_mem_shm_create
        // (see reregister_log_callback() comment for rationale).
        reregister_log_callback();

        return shm_attach_inner(name, false);
    }

    static Result<ShmHandle> shm_attach_inner(const std::string& name, bool nc) {
        ubs_mem_shm_desc_t *shm_descs = nullptr;

        int ret = ubs_mem_shm_attach(name.c_str(), nullptr, 0666, &shm_descs);

        // On UBSE, the "borrow relationship" (借用关系) is per-NODE, not
        // per-process. When ubs_mem_shm_attach returns EXISTED, it means
        // this node already has a borrow relationship for this shm. This
        // is normal when multiple threads on the same node attach the
        // same shm (e.g., do_init on the main thread + uffd handler
        // thread both attaching P0's metadata segment).
        //
        // The SDK fills shm_descs with valid import descriptors even on
        // EXISTED (see ubs_engine_mem.cpp: ubse_mem_shm_attach_resp_unpack
        // is called before returning EXISTED). So we treat EXISTED as
        // success and proceed to open the device fd.
        //
        // DO NOT detach+retry: detaching invalidates the node-level borrow
        // relationship, which breaks other threads' mappings on the same
        // node. The retry would also get EXISTED again (daemon-side state
        // isn't cleared synchronously).
        //
        // O_SYNC handling (see stage_a_ubse_test.cpp:442-446,
        // 5ad7670, 56b5610):
        //   - Cross-node borrow: O_SYNC is REQUIRED for open() — plain
        //     O_RDWR causes EPERM on the UBSE device driver for non-creator
        //     processes.
        //   - Same-node self-map (local node is the lender): O_SYNC causes
        //     mmap EPERM. Must disable O_SYNC.
        // Distinguish by comparing export_node.slot_id (lender) with the
        // local node's slot_id. If they match, it's a self-map → disable
        // O_SYNC. If they differ, it's a cross-node borrow → keep O_SYNC.
        //
        // This check MUST run for BOTH EXISTED and SUCCESS returns.
        // When shm_attach returns SUCCESS (fresh borrow after a previous
        // detach or crash), the node no longer has an existing borrow,
        // but the self-map/cross-node distinction is still the same
        // (determined by lender's node vs local node, not by borrow
        // existence). If we only check for EXISTED, a SUCCESS return
        // on a self-map leaves O_SYNC enabled → mmap EPERM.
        if (ret == static_cast<int>(UBS_ENGINE_ERR_EXISTED)) {
            LOG_INFO("ubs_mem_shm_attach returned EXISTED for " << name
                     << " (node already has borrow relationship — reusing)");
            ret = 0;
        }

        // Always check self-map, regardless of EXISTED/SUCCESS return.
        // When shm_attach returns SUCCESS (fresh borrow), the self-map
        // check is still needed to disable O_SYNC for same-node attaches.
        ubs_topo_node_t local_node;
        if (shm_descs && ubs_topo_node_local_get(&local_node) == 0 &&
            shm_descs->export_node.slot_id == local_node.slot_id) {
            nc = false;
        }

        // Re-register the no-op log callback after ubs_mem_shm_attach.
        // Defensive: attach may reset the callback on some UBSE versions
        // (same as ubs_mem_shm_create — see reregister_log_callback()).
        reregister_log_callback();
        if (ret != 0) {
            LOG_WARN("ubs_mem_shm_attach failed, ret=" << ret
                     << " name=" << name);
            if (shm_descs) ::free(shm_descs);
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        }

        if (!shm_descs || shm_descs->import_desc_cnt == 0) {
            LOG_WARN("ubs_mem_shm_attach returned success but shm_descs is empty"
                     << " name=" << name);
            if (shm_descs) ::free(shm_descs);
            return Error(ErrorKind::ShmOpen, "shm_descs is empty");
        }

        LOG_INFO("ubs_mem_shm_attach success, name=" << name.c_str());

        uint64_t mem_id = shm_descs->import_desc[0].memids[0];
        ::free(shm_descs);  // SDK docs: caller must free shm_descs

        char device_path[50];
        ret = snprintf(device_path, sizeof(device_path),
                       "%s%lu", std::string(device_path_prefix).c_str(), mem_id);
        if (ret == -1) {
            LOG_WARN("Failed to format device path (truncated or error)");
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        }

        int flags = O_RDWR;

        if (nc)
            flags |= O_SYNC;
        int fd = open(device_path, flags);
        if (fd == -1) {
            LOG_WARN("Failed to open device " << device_path
                     << " flags=O_RDWR" << (nc ? "|O_SYNC" : "")
                     << " errno=" << errno << " (" << strerror(errno) << ")");
            return Error(ErrorKind::ShmOpen, std::error_code(errno, std::system_category()));
        }

        ShmHandle handle{fd, {}};
        snprintf(handle.name, sizeof(handle.name), "%.*s", static_cast<int>(name.size()), name.c_str());
        return handle;
    }

    static Result<ShmHandle> shm_attach(const std::string& name) {
        return shm_attach_inner(name, true);
    }

    static void shm_unlink(const std::string& name) {
        // Detach first (release the node-level borrow), then immediately
        // retry shm_delete. The daemon processes detach asynchronously —
        // there's a window between "detach processed" (no borrows) and
        // "provider relationship released" (shm gone). We race to call
        // shm_delete in that window.
        //
        // shm_detach_by_name is safe to call even if already detached
        // (returns NO_ATTACH, treated as success).
        shm_detach_by_name(name);

        for (int attempt = 0; attempt < 100; ++attempt) {
            int ret = ubs_mem_shm_delete(name.c_str());
            if (ret == 0) {
                LOG_INFO("ubs_mem_shm_delete success, name=" << name.c_str()
                         << " (after " << attempt << " retries)");
                return;
            }
            if (ret == static_cast<int>(UBS_ENGINE_ERR_NOT_EXIST)) {
                // Shm already deleted (by auto-cleanup or another process),
                // or never existed (e.g., data segment index not used).
                LOG_TRACE("ubs_mem_shm_delete: " << name
                         << " already gone (NOT_EXIST)");
                return;
            }
            if (ret != 1024) {
                // 1005 INTERNAL, etc. — no point retrying.
                LOG_WARN("ubs_mem_shm_delete failed, ret=" << ret
                         << " name=" << name);
                return;
            }
            // 1024 (ATTACH_USING) — daemon hasn't processed our detach
            // yet, or other nodes still have active borrows. Retry.
            ::usleep(10000);  // 10ms
        }
        LOG_WARN("ubs_mem_shm_delete: TIMEOUT retrying " << name
                 << " (still ATTACH_USING after 1s — "
                 << "relying on SHM_ANONYMOUS auto-cleanup)");
    }

    // List all shms whose name starts with the given prefix. Returns
    // names in a vector for the caller to iterate and delete.
    // Wraps ubs_mem_shm_list_with_prefix (UBSE SDK).
    static std::vector<std::string> shm_list_with_prefix(const std::string& prefix) {
        ubs_mem_shm_desc_t* shm_descs = nullptr;
        uint32_t shm_desc_cnt = 0;
        int ret = ubs_mem_shm_list_with_prefix(prefix.c_str(), &shm_descs, &shm_desc_cnt);
        if (ret != 0) {
            if (ret != static_cast<int>(UBS_ENGINE_ERR_NOT_EXIST)) {
                LOG_WARN("ubs_mem_shm_list_with_prefix failed, ret=" << ret
                         << " prefix=" << prefix);
            }
            if (shm_descs) ::free(shm_descs);
            return {};
        }

        std::vector<std::string> names;
        for (uint32_t i = 0; i < shm_desc_cnt; ++i) {
            names.emplace_back(shm_descs[i].name);
        }
        ::free(shm_descs);
        return names;
    }

    static bool shm_exists(const std::string& name) {
        ubs_mem_shm_desc_t *shm_descs = nullptr;
        const int ret = ubs_mem_shm_get(name.c_str(), &shm_descs);
        if (ret != 0) {
            LOG_TRACE("ubs_mem_shm_get for " << name.c_str() << " failed, ret=" << ret);
            free(shm_descs);
            return false;
        }

        ubs_mem_stage stage = shm_descs->mem_stage;
        free(shm_descs);
        return stage == UBSE_EXIST;
    }

    static int shm_handle_fd(ShmHandle h) { return h.fd; }

    // Close the device fd only. Does NOT release the node-level borrow
    // (does NOT call ubs_mem_shm_detach). Safe to call when other
    // processes on the same node still hold an active borrow/mapping.
    static void shm_close_fd(ShmHandle handle) {
        if (handle.fd >= 0) {
            ::close(handle.fd);
        }
    }

    // Release the node-level borrow by name. Only call when the per-node
    // refcount == 0 (last process on this node for this shm). Safe to call
    // when the borrow was already released — UBS_ENGINE_ERR_SHM_NO_ATTACH
    // is treated as success (another process already released it).
    static void shm_detach_by_name(const std::string& name) {
        int ret = ubs_mem_shm_detach(name.c_str());
        if (ret != 0 && ret != (int)UBS_ENGINE_ERR_SHM_NO_ATTACH) {
            LOG_WARN("ubs_mem_shm_detach failed, ret=" << ret
                     << " name=" << name);
            return;
        }
        LOG_INFO("ubs_mem_shm_detach success, name=" << name);
    }

    // DEPRECATED — wrapper kept for backward compat. New code should use
    // shm_close_fd + shm_detach_by_name (typically via the
    // attach_with_refcount / detach_with_refcount wrappers in
    // DistributedShmBackend). This calls detach unconditionally, which
    // is unsafe for multi-proc-per-node (releases the node borrow even
    // when other procs on the same node still hold an attach).
    static void shm_close(ShmHandle handle) {
        shm_close_fd(handle);
        int ret = ubs_mem_shm_detach(handle.name);
        if (ret != 0) {
            LOG_WARN("ubs_mem_shm_detach failed, ret=" << ret);
            return;
        }
        LOG_INFO("ubs_mem_shm_detach success, name=" << handle.name);
    }

    // §5a: Returns true when no OTHER nodes are borrowing this shm.
    // The local node's own borrow (if any) is filtered out via
    // s_local_node_slot_id. This lets the lender call shm_delete even
    // while local processes still have an active borrow — the deletion
    // is deferred by the daemon until all borrows (including local) are
    // released, which happens naturally as local processes exit.
    static bool shm_is_unreferenced(const std::string& name) {
        ubs_mem_shm_desc_t *shm_descs = nullptr;
        const int ret = ubs_mem_shm_get(name.c_str(), &shm_descs);
        if (ret != 0) {
            free(shm_descs);
            // NOT_EXIST (1007) means the shm was already deleted —
            // definitely unreferenced. Return true so the poll exits.
            if (ret == static_cast<int>(UBS_ENGINE_ERR_NOT_EXIST)) {
                return true;
            }
            // Other errors (1005 INTERNAL, 1013 ALLOCATE, etc.) are
            // transient — return false to retry.
            static thread_local uint64_t fail_count = 0;
            if ((++fail_count & 0x3FF) == 0)
                LOG_TRACE("ubs_mem_shm_get for " << name.c_str()
                          << " still failing (ret=" << ret << ", attempt #" << fail_count << ")");
            return false;
        }

        uint32_t count = shm_descs->import_desc_cnt;
        uint32_t local_slot = s_local_node_slot_id.load(std::memory_order_acquire);
        uint32_t other_node_count = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (shm_descs->import_desc[i].import_node.slot_id != local_slot) {
                other_node_count++;
            }
        }
        if (other_node_count > 0) {
            static thread_local uint64_t rpt_count = 0;
            if ((++rpt_count & 0xF) == 1) {
                std::string nodes;
                for (uint32_t i = 0; i < count; ++i) {
                    if (shm_descs->import_desc[i].import_node.slot_id != local_slot) {
                        if (!nodes.empty()) nodes += ",";
                        nodes += std::to_string(shm_descs->import_desc[i].import_node.slot_id);
                    }
                }
                LOG_INFO("shm_is_unreferenced: " << name.c_str()
                         << " still borrowed by node(s): " << nodes
                         << " (import_desc_cnt=" << count << ")");
            }
        }
        free(shm_descs);
        return other_node_count == 0;
    }

    // Cross-node dead-rank detection (step 7). kill(pid, 0) returns ESRCH
    // for ALL cross-node processes (alive or dead), so it can't distinguish.
    // Instead, check if the rank's metadata shm still exists:
    //   UBSE_NOT_EXIST / UBSE_DELETING → rank is dead
    //   UBSE_EXIST / UBSE_CREATING    → rank is alive (or starting up)
    // This is a dependent name (ShmProviderT::is_rank_dead_cross_node),
    // so it's not instantiated for PosixShmProvider (the if constexpr
    // guard in claim_rank ensures it's only called for UBSE).
    static bool is_rank_dead_cross_node(const std::string& heap_id, int rank) {
        std::string name = shm_name(heap_id, rank, 0, 0);  // METADATA = 0
        ubs_mem_shm_desc_t* desc = nullptr;
        int ret = ubs_mem_shm_get(name.c_str(), &desc);
        ubs_mem_stage stage = UBSE_NOT_EXIST;
        if (ret == 0 && desc) {
            stage = desc->mem_stage;
        }
        if (desc) ::free(desc);
        return (stage == UBSE_NOT_EXIST || stage == UBSE_DELETING);
    }
};

}
