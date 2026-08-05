// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <iostream>
#include <cassert>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>

#include "uballoc.hpp"

// Uses the backend's ShmProvider for proper naming conventions.
// On UBSE, shm names use "ub-" prefix; on POSIX, they use "/" prefix.
// This function handles both transparently.
static void cleanup_all_distributed_shms(int max_procs, const std::string& heap_id = "heap") {
    using ShmProvider = uballoc::DistributedShmBackend<>::ShmProvider;
    // Register no-op log callback before any UBSE calls to silence
    // verbose UBSE runtime logs during cleanup.
    ShmProvider::provider_init();
    ShmProvider::shm_unlink(ShmProvider::bootstrap_shm_name(heap_id));
    for (int pid = 0; pid < max_procs; ++pid) {
        for (int r = 0; r < 4; ++r) {
            for (int seg = 0; seg < 8; ++seg) {
                ShmProvider::shm_unlink(
                    ShmProvider::shm_name(heap_id, pid, r, seg));
            }
        }
    }
}

static inline void check_child(int status) {
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "  CHILD FAILED: ";
        if (WIFSIGNALED(status)) std::cerr << "signal " << WTERMSIG(status);
        else std::cerr << "exit " << WEXITSTATUS(status);
        std::cerr << std::endl;
        assert(0);
    }
}
