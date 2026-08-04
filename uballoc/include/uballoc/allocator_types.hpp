// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "cas.hpp"
#include "recover.hpp"
#include "thread.hpp"

namespace uballoc {

struct DistributedAllocatorShared {
    DistributedHelpArray help;

    void init_help(int pid, int total_procs, size_t num_cores) {
        help.rank_ = pid;
        help.total_processes_ = total_procs;
        help.num_cores_ = num_cores;
        help.initialized_ = true;
    }

    void set_slice(int pid, HelpArray* slice, std::atomic<uint16_t>* data_base) {
        help.set_slice(pid, slice, data_base);
    }
};

struct AllocatorOwned {
    RecoverState state;
};

}
