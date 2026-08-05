// SPDX-License-Identifier: Apache-2.0

// Explicit template instantiation for the active backend.
//
// This file compiles ~10k LOC of CRTP templates ONCE into libuballoc.so,
// so downstream C++ consumers no longer re-instantiate them in every
// translation unit. The corresponding `extern template class` declarations
// in global.hpp / distributed_backend.hpp suppress implicit instantiation
// in user code.
//
// Only the two "blessed" backend combinations are instantiated:
//   - POSIX build:  DistributedShmBackend<PosixShmProvider>
//   - UBSE build:   DistributedShmBackend<UBShmProvider>
// A user who wants a custom ShmProvider must #define UBALLOC_NO_EXTERN_TEMPLATE
// before including uballoc.hpp, which removes the extern declarations and
// restores the implicit-instantiation (header-only) behavior.

#include "uballoc/global.hpp"
#include "uballoc/distributed_backend.hpp"

namespace uballoc {

#ifdef UBALLOC_USE_UBSE
template class DistributedShmBackend<UBShmProvider>;
template class GlobalAllocator<DistributedShmBackend<>>;
#else
template class DistributedShmBackend<PosixShmProvider>;
template class GlobalAllocator<DistributedShmBackend<>>;
#endif

} // namespace uballoc
