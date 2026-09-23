# uballoc User Manual

**[中文](user_manual.zh.md)** | **English**

This manual helps users fully understand the project background, installation, quick start, and API usage of the uballoc distributed shared-memory allocator.

---

# Project Introduction

## Overview

uballoc is a C++17 shared-memory allocator designed for aarch64-architecture, multi-process, multi-node distributed deployment scenarios under Linux. Using techniques such as fixed virtual address (VA) mapping, transparent huge pages, userfaultfd (uffd), and a SIGSEGV fault handler, uballoc implements unified cross-process allocation/free, type_id-based publish/discovery, dynamic membership discovery, and crash recovery. It minimizes cross-node data-copy overhead and improves distributed-application performance and scalability.

Applicable scenarios: allocation, free, publish, discovery, and cross-node access of distributed shared memory.

## Features

**Table 1** uballoc features

| Feature | Description |
|---------|-------------|
| Three-tier size-class heap | Objects are classified by size into Small (8 B–16 KB, 37 classes, 4-per-doubling spacing, 32 KB slab), Large (16 KB–2 MB, 28 classes, 4-per-doubling spacing, 4 MB slab), and Huge (≥2 MB, 2 MB slot). Each allocation matches the nearest class, reducing internal fragmentation (≤25%). |
| Ladder segment growth | Small/Large segments grow in 4MB→16MB→64MB→128MB steps; first allocation uses only 4MB (not a full 128MB segment), reducing initial memory footprint (12MB total for all three brackets vs 384MB). Huge segments remain 128MB. |
| Fixed-VA cross-process sharing | ~28 TB of virtual address space is reserved; all nodes use the same VA base (`DEFAULT_VA_BASE=0x200000000000`), so the same pointer is semantically equivalent across all processes — no address translation required. |
| type_id-based publish/discovery | `malloc(size, type_id)` performs allocation and publish in one call; any process can discover regions published by others via `lookup_by_type(type_id)` (blocking or non-blocking). `owner_process` is registered atomically via CAS, guaranteeing a single owner per type_id. |
| Dynamic membership discovery | After setting the `UBALLOC_HEAP_ID` environment variable, a process discovers its rank and total membership automatically via a bootstrap shared-memory block — no need to know `rank` or `total_processes` in advance. |
| uffd on by default | A constructor at priority 102 automatically installs the uffd handler thread, which loads remote data segments on demand. When uffd is unavailable, it falls back to the SIGSEGV fault handler. `init()` also calls `try_enable_uffd_locked()` at the end. |
| Asynchronous memory reclaim | Supports SYNC/ASYNC modes for returning idle Small/Large segments to the OS. ASYNC mode (default) uses a background thread with jemalloc-style timed scanning — zero hot-path overhead on malloc/free. SYNC mode triggers inline in malloc/free. The `decay1`/`decay2` timers control the LIVE→DETACHED→RETURNED state machine; `purge()` skips timers for immediate return. |
| Memory statistics & introspection | Query memory flow statistics via `return_stats()`: bytes requested from OS, bytes returned to OS, bytes allocated to app, bytes freed by app, and `fragmentation_ratio`. In ASYNC mode, the background thread auto-emits `[RECLAIM-STATS]` log every 30 scans. |
| Defragmentation | `uballoc::usable_size(ptr)` returns the usable size of an allocation (for right-sizing to avoid unnecessary reallocs); `uballoc::defrag_hints(threshold)` returns live allocations in low-occupancy memory regions; the application performs malloc+memcpy+fixrefs+free for cooperative defragmentation. |
| CRTP zero-overhead backend | The backend dispatches statically via CRTP — no virtual calls on the allocation hot path. ARMv8.1 LSE atomics are optional, compile-time toggleable. |
| Crash recovery | Three optional mechanisms — detectable CAS, state logging, cache flushing — support post-crack scan-and-rebuild of allocator state. |

## Installation

See [Installation Guide](#installation-guide) for verified environments, build options, build artifacts, and usage.

## Quick Start

See [Quick Start](#quick-start) for environment-variable configuration, two-node example execution, and performance verification.

## Documentation

| Resource | Description |
|---------|-------------|
| [Quick Start](#quick-start) | Introduces uballoc's basic concepts, installation, environment variables, two-node example, and performance verification. |
| [Installation Guide](#installation-guide) | Details verified environments, CMake options, build artifacts, tarball generation, downstream consumption, and uninstall. |
| [API Reference](#api-reference) | The API is split into **basic API** (`init`/`malloc`/`malloc_published`/`free`/`realloc`/`lookup_by_type*`/`shm_new`/`shm_delete` — sufficient for the vast majority of apps) and **optional/advanced API** (`memalign`/`publish`/`unpublish`/`lookup_by_address`/`rank`/`total_processes`/`is_owner`/`is_initialized`/`reset`/`enable_*fault_handler`). Detailed descriptions and examples are provided. |
| [FAQ](#faq) | Summarizes high-frequency behavior semantics and usage constraints, including `realloc` shrink semantics, process-lifecycle requirements for publish/lookup, and cross-process free limitations. |

## Contributing

Contributions to the community are welcome. For any issues, suggestions, or feature requests and bug reports, submit via issues or pull requests on the repository.

## Disclaimer

uballoc is a low-level shared-memory primitive library. Its computation involves cross-process memory reads/writes and virtual-address mapping operations. uballoc does not provide or publish an operating system — the OS must be installed by the user, and uballoc does not assume OS security responsibility. Users must harden the OS for their own applications, including not installing or removing unnecessary applications.

## License

This project is licensed under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0).

```
Copyright 2021-2026 uballoc contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

---

# Installation Guide

## Verified Environments

To ensure safe and successful use of uballoc, verify that your environment falls within the verified range.

| Operating System | CPU Type | Compiler |
|---------|--------|--------|
| openEuler 22.03 LTS SP3 | Kunpeng 920 series (aarch64) | GCC 10.3.1 / BiSheng 4.2.0 |
| openEuler 24.03 LTS SP4 | UBSE machine (8 nodes, 1.5 TB physical memory per node) | GCC 12.3.1 |

- uballoc currently supports only the aarch64 architecture; compilation on other architectures may fail or cause runtime crashes.
- The UBShmProvider backend is only available on UBSE machines; on other environments, set `UBALLOC_USE_UBSE=OFF` to use PosixShmProvider (based on `/dev/shm`).
- Building uballoc requires CMake 3.16+ and a C++17 standard library.

## Obtaining the Source

uballoc is currently distributed as source code. Clone from the repository:

```shell
git clone <uballoc-repo-url> uballoc
cd uballoc
```

## Build Options

**Table 2** CMake build options

| Option | Default | Description |
|------|--------|------|
| `UBALLOC_USE_UBSE` | ON | Use UBShmProvider as default backend (requires ubse-client); OFF uses PosixShmProvider (standard Linux `/dev/shm`) |
| `UBALLOC_LSE` | ON | Enable ARMv8.1 LSE atomic instructions |
| `UBALLOC_LOG` | ON | Enable distributed-path logging (compile-time switch; OFF compiles all non-fatal logs to no-ops) |
| `UBALLOC_BUILD_TESTS` | ON | Build unit tests |
| `UBALLOC_BUILD_EXAMPLES` | ON | Build example programs |
| `UBALLOC_RECOVER_CAS` | ON | Enable detectable CAS for crash recovery |
| `UBALLOC_RECOVER_LOG` | ON | Enable state logging for crash recovery |
| `UBALLOC_RECOVER_FLUSH` | OFF | Enable cache flushing for crash recovery |
| `UBALLOC_NO_EXTERN_TEMPLATE` | OFF | User-defined macro (not a CMake option); when defined, cancels `extern template class` declarations, restoring header-only template instantiation for custom ShmProvider backends |

### Build Commands

**Default build (UBShmProvider, UBSE machines only)**

```shell
cmake -B build && cmake --build build -j
```

**POSIX build (standard Linux, no ubse dependency)**

```shell
cmake -B build -DUBALLOC_USE_UBSE=OFF && cmake --build build -j
```

## Build Artifacts

After a successful build, the following artifacts are generated under `build/`:

| Artifact | Description |
|------|------|
| `libuballoc.so` | uballoc shared library, linked against UBShmProvider or PosixShmProvider (depending on `UBALLOC_USE_UBSE`) |
| `ub_example` | Distributed two-node example (publish/lookup, remote free) |
| `published_example` | Demonstrates `malloc(size, type_id)` + blocking lookup |
| `uffd_example` | Demonstrates uffd-on-by-default coexisting with eager attach |
| `fault_handler_example` | Demonstrates SIGSEGV fault handler as uffd fallback |
| `dynamic_discovery_example` | Demonstrates `UBALLOC_HEAP_ID`-driven dynamic membership discovery |
| `distributed_example` | Demonstrates multi-tier allocation, `lookup_by_address`, and cross-process remote free |
| `stl_full_shm_example` | Demonstrates `shm_new<T>(pub_tid{})` merged-publish STL containers (header+data both in shm) |
| `uballoc_test` | Unit tests (`ctest` entry point) |

## Usage

uballoc is consumed as a shared library linked by applications. You can specify the link path at compile time via `-rpath`, or at runtime via `LD_LIBRARY_PATH`.

### Compile-time Linking

```shell
g++ -std=c++17 -o my_app my_app.cpp \
    -I<path to uballoc>/include \
    -L<path to uballoc>/build -luballoc \
    -Wl,-rpath=<path to uballoc>/build
```

### Runtime Path

```shell
export LD_LIBRARY_PATH=/<path to uballoc>/build:$LD_LIBRARY_PATH
./my_app
```

## Verifying the Installation

After setting `LD_LIBRARY_PATH` or `rpath`, use `ldd` to verify that the uballoc library is linked successfully. Expected output:

```text
[root@localhost build]# ldd ./ub_example
        linux-vdso.so.1 (0x0000ffff9ee6a000)
        libuballoc.so => /path/to/uballoc/build/libuballoc.so (0x0000ffff9b400000)
        libstdc++.so.6 => /usr/lib64/libstdc++.so.6 (0x0000ffff9b200000)
        libm.so.6 => /usr/lib64/libm.so.6 (0x0000ffff9b150000)
        libgcc_s.so.1 => /usr/lib64/libgcc_s.so.1 (0x0000ffff9b110000)
        libc.so.6 => /usr/lib64/libc.so.6 (0x0000ffff9af60000)
        /lib/ld-linux-aarch64.so.1 (0x0000ffff9ee2d000)
```

## Distribution Tarball

After building, you can use CPack to generate a distributable tarball for end-user installation.

**Generate the tarball**

```shell
# POSIX build (standard Linux)
cmake -B build -DUBALLOC_USE_UBSE=OFF && cmake --build build -j
(cd build && cpack -G TGZ)

# UBSE build (UBSE machines only)
cmake -B build && cmake --build build -j
(cd build && cpack -G TGZ)
```

Output: `build/uballoc-<version>-linux-<arch>.tar.gz` (e.g. `uballoc-0.6.0-linux-aarch64.tar.gz`, ~135 KB).

**Tarball contents**

| Path | Description |
|------|------|
| `lib/libuballoc.so.<version>` | Actual library file (debug symbols stripped) |
| `lib/libuballoc.so.<SOVERSION>` | SONAME symlink (e.g. `libuballoc.so.0`) |
| `lib/libuballoc.so` | Development symlink (for `-luballoc`) |
| `lib/pkgconfig/uballoc.pc` | pkg-config file, includes correct Cflags/Libs |
| `lib/cmake/uballoc/` | CMake package config files (4), for `find_package(uballoc)` |
| `include/uballoc.h`, `include/uballoc.hpp`, `include/uballoc/*.hpp` | All headers |
| `share/uballoc/examples/` | Example sources (9 `.cpp` files) + standalone `CMakeLists.txt`, for end-user compile verification |
| `share/uballoc/scripts/cleanup_shms.sh` | POSIX residual-shm cleanup script (based on `/dev/shm` file paths) |
| `share/uballoc/scripts/cleanup_shms_ub.sh` | UBSE residual-shm cleanup script (based on `ubsectl` auto-discovery) |

**End-user install**

```shell
# Extract to any prefix (recommend /usr/local or /opt/uballoc)
tar xzf uballoc-0.6.0-linux-aarch64.tar.gz --strip-components=1 -C /opt/uballoc
```

**Downstream consumption (choose one)**

Option 1: pkg-config (no CMake required)

```shell
g++ app.cpp $(PKG_CONFIG_PATH=/opt/uballoc/lib/pkgconfig pkg-config --cflags --libs uballoc) -o app
```

Option 2: CMake `find_package`

```cmake
# Downstream CMakeLists.txt
find_package(uballoc CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE uballoc::uballoc)
```

```shell
cmake -B build -DCMAKE_PREFIX_PATH=/opt/uballoc && cmake --build build
```

> **Note**: The `uballoc::uballoc` imported target automatically propagates the C++17 requirement (`cxx_std_17`); downstream projects do not need to set `CMAKE_CXX_STANDARD` explicitly. For POSIX builds, the pkg-config/CMake config does not define the `UBALLOC_USE_UBSE` macro, so `#ifdef UBALLOC_USE_UBSE` evaluates to false and correctly excludes UBSE-related code.

**Compiling the bundled examples**

The tarball ships with 9 example sources (`share/uballoc/examples/`), which can be compiled in two ways:

Option 1: CMake (recommended)

```shell
cd /opt/uballoc/share/uballoc/examples
cmake -B build -DCMAKE_PREFIX_PATH=/opt/uballoc && cmake --build build -j
```

Option 2: pkg-config (no CMake)

```shell
cd /opt/uballoc/share/uballoc/examples
g++ -std=c++17 distributed_example.cpp \
    $(PKG_CONFIG_PATH=/opt/uballoc/lib/pkgconfig pkg-config --cflags --libs uballoc) \
    -Wl,-rpath,/opt/uballoc/lib -o distributed_example
```

> **Note**: Example sources use `#include <uballoc.hpp>` (angle brackets), which works both when building in-tree (include path passed via `target_link_libraries`) and after install (`-I<prefix>/include`).

## Explicit Template Instantiation

As of V0.6, the library explicitly instantiates backend template combinations in `src/uballoc_instances.cpp`, so downstream consumers only need to include headers and link `-luballoc` — no per-translation-unit template recompilation.

**Supported instantiation combinations**:

| `UBALLOC_USE_UBSE` | Instantiated templates |
|---------------------|-------------|
| `OFF` (POSIX) | `DistributedShmBackend<PosixShmProvider>` + `GlobalAllocator<DistributedShmBackend<PosixShmProvider>>` |
| `ON` (UBSE) | `DistributedShmBackend<UBShmProvider>` + `GlobalAllocator<DistributedShmBackend<UBShmProvider>>` |

Headers suppress downstream compile-time instantiation via `extern template class` declarations. Measured per-TU compilation time drops ~36% (2.24s→1.44s); object-file size drops ~85% (1.33 MB→199 KB).

**Custom backends**: If you use a custom `ShmProvider` or custom allocator backend (other than the two standard combinations above), define the `UBALLOC_NO_EXTERN_TEMPLATE` macro to restore header-only behavior:

```shell
g++ -std=c++17 -DUBALLOC_NO_EXTERN_TEMPLATE -I/opt/uballoc/include \
    my_app.cpp -L/opt/uballoc/lib -luballoc -o my_app
```

This macro cancels all `extern template class` declarations, allowing templates to be instantiated on demand in user code.

## Uninstall

Delete the `build/` directory to uninstall uballoc; shared-memory objects must be released via the cleanup scripts:

```shell
# UBShmProvider (ubse) — discovers existing shms and attacher nodes via ubsectl
./scripts/cleanup_shms_ub.sh                            # default UBALLOC_HEAP_ID=heap
UBALLOC_HEAP_ID=myheap ./scripts/cleanup_shms_ub.sh     # specify heap_id

# PosixShmProvider (/dev/shm)
./scripts/cleanup_shms.sh <total_processes> "" <node>
```

---

# Quick Start

This section helps users quickly understand uballoc's usage and performance verification.

## Installation

For detailed steps to obtain source and build, see [Installation Guide](#installation-guide). Build artifacts:

| Library / Executable | Description |
|-------------------|------|
| `libuballoc.so` | uballoc shared library (UBShmProvider or PosixShmProvider) |
| `ub_example` | Two-node publish/lookup and remote-free example |
| `uffd_example` | uffd-on-by-default coexisting with eager-attach example |
| `dynamic_discovery_example` | `UBALLOC_HEAP_ID` dynamic membership-discovery example |
| `stl_full_shm_example` | `shm_new<T>(pub_tid{})` merged-publish STL container example |
| `reclaim_example` | Async memory reclaim demo (SYNC/ASYNC modes, LIVE→DETACHED→RETURNED, purge immediate return) |
| `uballoc_test` | Unit tests |

## Environment Variables

As shown in [**Table 3** uballoc environment variables](#uballoc-environment-variables), these describe the environment variables supported by uballoc.

**Table 3** uballoc environment variables

| Variable | Description | Default | Options |
|---------|------|--------|--------|
| `UBALLOC_HEAP_ID` | Heap identifier string for the bootstrap shared memory; if unset, `init` prints a warning and returns (no initialization) | empty (unset) | any non-empty string, e.g. `myheap` |
| `UBALLOC_LOG_LEVEL` | Runtime log level (case-insensitive); default `error` to avoid triggering uninitialized iostream during constructor(102) | `error` | `error`/`warn`/`info`/`debug`/`trace` |
| `UBALLOC_LOG` | Compile-time logging master switch; OFF compiles all non-fatal logs to no-ops | `ON` (CMake) | `ON`/`OFF` |
| `UBALLOC_USE_UBSE` | Compile-time backend selection; ON uses UBShmProvider, OFF uses PosixShmProvider | `ON` (CMake) | `ON`/`OFF` |
| `UBALLOC_VALIDATE` | Compile-time validation-assert switch (always on, cannot be disabled) | `ON` (forced) | — |
| `UBALLOC_RECOVER_CAS` | Compile-time detectable-CAS switch (crash recovery) | `ON` (CMake) | `ON`/`OFF` |
| `UBALLOC_RECOVER_LOG` | Compile-time state-logging switch (crash recovery) | `ON` (CMake) | `ON`/`OFF` |
| `UBALLOC_RECOVER_FLUSH` | Compile-time cache-flushing switch (crash recovery) | `OFF` (CMake) | `ON`/`OFF` |
| `UBALLOC_LSE` | Compile-time ARMv8.1 LSE atomic-instruction switch | `ON` (CMake) | `ON`/`OFF` |
| `UBALLOC_RECLAIM_MODE` | Memory reclaim mode: `async` (default) via background thread, `sync` inline in malloc/free | `async` | `async`/`sync` |
| `UBALLOC_RECLAIM_ENABLED` | Reclaim master switch; set to 0 to disable entirely | `1` | `0`/`1` |
| `UBALLOC_RECLAIM_DECAY1_MS` | LIVE→DETACHED timeout (ms); time after segment becomes idle before detach | `5000` | any non-negative integer |
| `UBALLOC_RECLAIM_DECAY2_MS` | DETACHED→RETURNED timeout (ms); time after detach before delete/return to OS | `60000` | any non-negative integer |
| `UBALLOC_RECLAIM_SCAN_INTERVAL_MS` | ASYNC mode background thread scan interval (ms) | `5000` | any positive integer |
| `UBALLOC_RECLAIM_STATS` | Memory statistics switch; set to 0 to disable hot-path byte counting (zero overhead) | `1` | `0`/`1` |
| `UBALLOC_RECLAIM_LOG_STATS_INTERVAL` | ASYNC mode `[RECLAIM-STATS]` log interval (emit every N background scans) | `30` | any positive integer |

### Environment-variable configuration example

Example: configuring `UBALLOC_HEAP_ID` and `UBALLOC_LOG_LEVEL`.

1. Open `/etc/profile`.

    ```shell
    vi /etc/profile
    ```

2. Press `i` to enter edit mode and add the following to `/etc/profile`:

    ```shell
    export UBALLOC_HEAP_ID=myheap
    export UBALLOC_LOG_LEVEL=info
    ```

3. Run `source` to apply the environment variables:

    ```shell
    source /etc/profile
    ```

## Performance Verification

This section uses the `ub_example` two-node publish/lookup test to verify uballoc's cross-node sharing capability.

### Test Environment

- Processor: Kunpeng 920 (aarch64)
- Nodes: 2
- Operating System: openEuler 22.03 LTS SP3
- Build option: `UBALLOC_USE_UBSE=OFF` (POSIX local verification) or `UBALLOC_USE_UBSE=ON` (UBSE machine)
- uballoc version: 0.1.0

### Test Steps

Run P0 and P1 on two nodes (or two terminals):

**Node 0: allocate and publish**

```shell
UBALLOC_HEAP_ID=ubex ./build/ub_example 0
```

**Node 1: discover and verify**

```shell
UBALLOC_HEAP_ID=ubex ./build/ub_example 1
```

> **Note**: As of V0.6, `ub_example` takes only 1 argument (`app_pid`); `total_processes` is no longer needed. uballoc discovers membership automatically via the `UBALLOC_HEAP_ID` bootstrap.

### Results

**Node 0 output**

```text
=== Published Allocator Example ===
Process 0 of 2
  malloc(96, type_id=100)...
  Allocated+published at 0x200000100000
  lookup_by_type_blocking(type_id=101, 10s)...
  Found peer's block: address=0x280000200000 size=96 owner=1
  Peer config: pid=1 values=[1000, 1001, ... 1007]
  Peer data verified OK
  Non-blocking lookup_by_type also found it: OK
  Process 0 complete.
```

**Node 1 output**

```text
=== Published Allocator Example ===
Process 1 of 2
  malloc(96, type_id=101)...
  Allocated+published at 0x280000200000
  lookup_by_type_blocking(type_id=100, 10s)...
  Found peer's block: address=0x200000100000 size=96 owner=0
  Peer config: pid=0 values=[0, 1, ... 7]
  Peer data verified OK
  Non-blocking lookup_by_type also found it: OK
  Process 1 complete.
```

### Performance Analysis

| Metric | uballoc (fixed-VA cross-process sharing) | Traditional socket copy | Improvement |
|------|--------------------------|---------------|------|
| Cross-node data-access latency | Single pointer dereference (remote segment already loaded) | One socket send/recv round-trip | Order-of-magnitude reduction |
| Cross-node bytes copied | 0 (pointer semantics equivalent) | Equal to data size | 100% |
| Multi-node membership-discovery latency | One bootstrap shm attach | Requires external coordination service | Significant reduction |

As the results show, uballoc's fixed-VA and type_id publish/discovery mechanism eliminates cross-node byte copying, dramatically improving both performance and scalability.

---

# API Reference

## Function Overview

uballoc is a C++17 shared-memory allocator designed for aarch64-architecture, multi-process, multi-node distributed deployment under Linux. Using fixed-VA mapping, transparent huge pages, userfaultfd, and a SIGSEGV fault handler, it minimizes cross-node data-copy overhead.

The uballoc API falls into two categories:

- **Basic API**: `init`, `malloc`, `malloc_published`, `free`, `realloc`, `lookup_by_type`, `lookup_by_type_blocking`, `shm_new`/`shm_delete` (including the `pub_tid` merged-publish overload). The vast majority of applications need only this set to allocate, reallocate, publish, discover, and free cross-process shared memory.
- **Optional/advanced API**: `memalign`, `publish`, `unpublish`, `lookup_by_address`, `rank`, `total_processes`, `is_owner`, `is_initialized`, `reset`, `enable_fault_handler`/`disable_fault_handler`, `enable_userfaultfd`/`disable_userfaultfd`. These are not required — they are typically used for aligned allocation, manual publish/unpublish timing, cluster-state queries, fault-handler fallback, or full allocator reset.

Thread IDs are assigned automatically on first use — no explicit `init_thread` call is needed.

The examples below assume these headers are included:

```cpp
#include <uballoc.hpp>   // C++ API
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
```

## Basic API

This section covers the API needed by the vast majority of applications: initializing the allocator, allocating/reallocating/freeing memory, publishing/discovering regions, and constructing/destroying C++ objects in shared memory.

### uballoc_init

**Function**

Initializes this process's allocator. Reads the heap identifier from the `UBALLOC_HEAP_ID` environment variable and dynamically discovers the process rank and total membership via the bootstrap shared-memory PID table. No need to know `rank` or `total_processes` in advance.

**Contract**: Applications **must** call `uballoc_init()` before using any allocator functionality (including direct access to remote shared memory). `uballoc_malloc`/`uballoc_free`/`uballoc_lookup_by_type*` automatically call `uballoc_init()` on first use as a safety net, but explicit `uballoc_init()` is the recommended pattern for code that directly accesses remote shms.

**Definition**

```cpp
// C++ API
void uballoc::init();

// C API
void uballoc_init(void);
```

**Example**

```shell
export UBALLOC_HEAP_ID=myheap
./worker   # triggers init automatically on first malloc
```

```cpp
// Or call once explicitly at program entry:
uballoc::init();
```

### uballoc_malloc

**Function**

Allocates `size` bytes of memory and returns a pointer to the allocated memory.

**Definition**

```cpp
void* uballoc::malloc(size_t size);
void* uballoc_malloc(size_t size);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `size` | Number of bytes to allocate | Non-negative | Input |

**Return Value**

- Success: pointer to the allocated memory.
- Failure: `nullptr`.

**Example**

```cpp
void MallocExample() {
    int8_t* res = static_cast<int8_t*>(uballoc::malloc(64));
    if (res == nullptr) {
        std::cerr << "res is null" << std::endl;
    } else {
        std::cout << "malloc pointer address: " << res << std::endl;
        uballoc::free(res);
    }
}
```

### uballoc_malloc_published

**Function**

Allocates `size` bytes and publishes the pointer to the `type_id` slot via CAS — allocation and publish in one call.

**Definition**

```cpp
void* uballoc::malloc(size_t size, uint32_t type_id);
void* uballoc_malloc_published(size_t size, uint32_t type_id);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `size` | Number of bytes to allocate | Non-negative | Input |
| `type_id` | Application-defined type ID | 0..2^32-1 | Input |

**Return Value**

- Success: the published pointer.
- Failure: `nullptr` (on allocation or publish failure, any allocated memory is freed).

**Example**

```cpp
constexpr uint32_t TYPE_DATA = 200;

struct alignas(64) DataBlock {
    std::atomic<uint32_t> ready;
    uint32_t rank;
    char message[64];
};

void PublishExample() {
    DataBlock* block = static_cast<DataBlock*>(
        uballoc::malloc(sizeof(DataBlock), TYPE_DATA));
    if (!block) {
        std::cerr << "malloc(size, type_id) failed" << std::endl;
        return;
    }
    block->ready.store(0, std::memory_order_relaxed);
    block->rank = 0;
    std::snprintf(block->message, sizeof(block->message), "Hello from P0");
    block->ready.store(1, std::memory_order_release);
    // ... peers will discover this block via lookup_by_type_blocking(TYPE_DATA, ...) ...
    uballoc::free(block);
}
```

> **Data-readiness race note**: The pointer is published before the caller initializes the memory. The caller should place an atomic `ready` flag in the published region and set it after `malloc(size, type_id)` returns; the finder should spin-wait on `ready` after `lookup_by_type_*` returns.

### uballoc_free

**Function**

Frees previously allocated memory. Supports cross-thread and cross-process free (internally routed to the appropriate backend via `owner_process`).

**Definition**

```cpp
void uballoc::free(void* ptr);
void uballoc_free(void* ptr);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `ptr` | Pointer to the memory to free | Non-null | Input |

**Example**

See the `uballoc_malloc` example.

### uballoc_realloc

**Function**

Reallocates the memory pointed to by `old_pointer` to `new_size` bytes. When `new_size > old_size` (grow), allocates a new block, copies data, and frees the old block; when `new_size <= old_size` (shrink or equal), returns the original pointer directly — no new allocation, no free. This is standard C `realloc` semantics (consistent with glibc, jemalloc, mimalloc).

**Definition**

```cpp
void* uballoc::realloc(void* ptr, size_t size);
void* uballoc_realloc(void* ptr, size_t size);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `ptr` | Pointer to the old memory to reallocate | Non-null | Input |
| `size` | New number of bytes | Non-negative | Input |

**Return Value**

- Success: pointer to the allocated memory. On grow, a newly allocated pointer (`!= ptr`); on shrink or equal, the original pointer (`== ptr`).
- Failure: `nullptr`.

### uballoc_lookup_by_type

**Function**

Non-blocking lookup of the publish info for a given `type_id`.

**Definition**

```cpp
uballoc::PublishedInfo uballoc::lookup_by_type(uint32_t type_id);
int uballoc_lookup_by_type(uint32_t type_id, uballoc_published_info_t* out);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `type_id` | Type ID to look up | 0..2^32-1 | Input |
| `out` (C API) | Output parameter, written with the publish-info struct | Non-null | Output |

**Return Value (C API)**

- Success: 0.
- Not found: non-zero.

### uballoc_lookup_by_type_blocking

**Function**

Blocking lookup of the publish info for a given `type_id`. Polls every 100 ms until found or timeout.

**Definition**

```cpp
uballoc::PublishedInfo uballoc::lookup_by_type_blocking(uint32_t type_id, int timeout_ms);
int uballoc_lookup_by_type_blocking(uint32_t type_id, int timeout_ms, uballoc_published_info_t* out);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `type_id` | Type ID to look up | 0..2^32-1 | Input |
| `timeout_ms` | Timeout in milliseconds; negative means wait forever | Any integer | Input |
| `out` (C API) | Output parameter, written with the publish-info struct | Non-null | Output |

**Return Value (C API)**

- Success: 0.
- Timeout: non-zero.

**Example**

```cpp
constexpr uint32_t TYPE_PEER = 101;

auto info = uballoc::lookup_by_type_blocking(TYPE_PEER, 10000);
if (info.owner_process < 0) {
    std::cerr << "Timeout: peer did not publish type " << TYPE_PEER << std::endl;
    return;
}
std::cout << "Found peer at " << info.address
          << " size=" << info.size
          << " owner=" << info.owner_process << std::endl;
```

> **Process-lifecycle requirement**: The publishing process must remain alive while the consumer is looking up. When the publisher exits, its destructor cleans up the bootstrap slot and releases the shared memory; afterward the consumer can no longer discover the publication, and `lookup_by_type_blocking` times out with `owner_process == -1`. If the publisher exits before the consumer's lookup, the timeout path is taken. Callers must always check `owner_process >= 0` and handle timeouts. The recommended publish/consume pattern is described in [FAQ: When can the publisher exit?](#when-can-the-publisher-exit).

**`uballoc_published_info_t` struct definition**

```c
typedef struct {
    void*    address;        /* Pointer to the published memory */
    size_t   size;           /* Number of bytes of the published allocation */
    uint32_t type_id;        /* Application-defined type ID */
    int      owner_process;  /* Owner process ID; -1 if not found */
} uballoc_published_info_t;
```

### shm_new / shm_delete

**Function**

Constructs/destroys C++ objects in shared memory — equivalent to `new`/`delete` but based on uballoc. `shm_new` has two overloads: allocate+construct (no publish), and allocate+construct+publish (distinguished via the `pub_tid` parameter).

**`pub_tid` wrapper type**

`pub_tid` is a distinct user-defined type used to disambiguate the "publish" vs "non-publish" `shm_new` overloads in overload resolution. It wraps a `uint32_t type_id`; its constructor is `explicit` to avoid conflicts with construction arguments.

```cpp
struct pub_tid {
    uint32_t value;
    explicit constexpr pub_tid(uint32_t v) noexcept : value(v) {}
};
```

**Definition**

```cpp
// Allocate + construct (no publish)
template <typename T, typename... Args>
T* shm_new(Args&&... args);

// Allocate + construct + publish (merged API, replaces two-step shm_new + publish)
template <typename T, typename... Args>
T* shm_new(pub_tid type_id, Args&&... args);

// Destruct + free (does not auto-unpublish; call unpublish first if needed)
template <typename T>
void shm_delete(T* p);
```

**Example: allocate only (no publish)**

```cpp
struct MyConfig {
    uint32_t version;
    uint32_t flags;
    uint64_t values[8];
};

MyConfig* cfg = uballoc::shm_new<MyConfig>();
cfg->version = 1;
// ... access cfg across processes via fixed VA ...
uballoc::shm_delete(cfg);
```

**Example: allocate + publish (merged API)**

```cpp
constexpr uint32_t TYPE_CONFIG = 42;

// Allocate + construct + publish in one call, replacing the two-step:
//   auto* cfg = uballoc::shm_new<MyConfig>();
//   uballoc::publish(cfg, sizeof(MyConfig), TYPE_CONFIG);
auto* cfg = uballoc::shm_new<MyConfig>(uballoc::pub_tid{TYPE_CONFIG});
cfg->version = 1;
// ... peers discover cfg via lookup_by_type_blocking(TYPE_CONFIG, ...) ...
uballoc::unpublish(cfg);    // unpublish first
uballoc::shm_delete(cfg);  // then destruct + free
```

> **Data-readiness race note**: Same as `malloc(size, type_id)` — the pointer is published before the caller initializes the memory. Place an atomic `ready` flag in the object, set it after `shm_new` returns; the finder should spin-wait on `ready` after `lookup_by_type_*` returns.
>
> **Limitations**: `shm_new`/`shm_delete` have no array form and no `nothrow` overload. `shm_delete` does not auto-unpublish — if the object was published via `pub_tid`, call `unpublish(ptr)` before `shm_delete(ptr)`.

## Optional / Advanced API

This section covers API that is not required — the vast majority of applications never need them. Use them only when:

- You need aligned allocation (`memalign`);
- You need manual control over publish/unpublish timing (`publish`/`unpublish`);
- You need reverse-lookup publish info by address (`lookup_by_address`);
- You need to query cluster state (`rank`/`total_processes`/`is_owner`/`is_initialized`);
- You need a full allocator reset (`reset`);
- You need to immediately return idle segments to the OS (`purge`) or query memory statistics (`return_stats`);
- uffd is unavailable and you need to manually enable the SIGSEGV fault handler as fallback (`enable_fault_handler`).

### uballoc_memalign

**Function**

Allocates `size` bytes of memory aligned to `alignment`.

**Definition**

```cpp
void* uballoc::memalign(size_t size, size_t alignment);
void* uballoc_memalign(size_t size, size_t alignment);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `size` | Number of bytes to allocate | Non-negative | Input |
| `alignment` | Alignment in bytes; must be a power of two | Power of two | Input |

### uballoc_publish

**Function**

Publishes an already-allocated pointer to the `type_id` slot. Compared to `malloc(size, type_id)`, this is for scenarios where the caller needs to initialize memory before publishing.

**Definition**

```cpp
int uballoc::publish(void* ptr, size_t size, uint32_t type_id);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `ptr` | Pointer to already-allocated memory | Non-null | Input |
| `size` | Number of bytes of the allocated memory | Non-negative | Input |
| `type_id` | Application-defined type ID | 0..2^32-1 | Input |

**Return Value**

- Success: slot index (≥0).
- Failure: negative (e.g. the `type_id` is already occupied by another process).

### uballoc_unpublish

**Function**

Removes `ptr` from its corresponding `type_id` slot.

**Definition**

```cpp
bool uballoc::unpublish(void* ptr);
bool uballoc::unpublish(void* ptr, int owner_pid);
```

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `ptr` | Pointer to the memory to unpublish | Non-null | Input |
| `owner_pid` (optional) | Known owner process ID; if omitted, derived automatically | -1..MAX_PROCESSES-1 | Input |

**Return Value**

- Success: `true`.
- Failure: `false` (pointer was not published or `owner_pid` mismatch).

### uballoc_lookup_by_address

**Function**

Reverse-lookup publish info by address: given a shared-memory pointer, returns its `type_id`, `owner_process`, and allocation size. Commonly used for debugging or verifying cross-process pointer ownership.

**Definition**

```cpp
uballoc::PublishedInfo uballoc::lookup_by_address(void* ptr);
```

> **Note**: C++ API only.

**Parameters**

| Parameter | Description | Range | In/Out |
|--------|------|---------|----------|
| `ptr` | Shared-memory pointer to reverse-lookup | Non-null | Input |

**Return Value**

Returns a `PublishedInfo` struct. If the pointer does not belong to any published region, `owner_process` is -1.

### uballoc_rank

**Function**

Returns this process's logical rank (0-based) in the uballoc cluster. The rank is dynamically assigned by the bootstrap shared-memory PID table at `init()` time, independent of the OS process ID (`getpid()`).

**Definition**

```cpp
int uballoc::rank();
```

> **Note**: C++ API only. Not exposed in the C API.

**Return Value**

- This process's rank (0..MAX_PROCESSES-1).

### uballoc_total_processes

**Function**

Returns the total number of processes in the cluster. Under dynamic discovery, returns `MAX_PROCESSES` (default 8); under explicit configuration, returns the `total_processes` passed at construction.

**Definition**

```cpp
int uballoc::total_processes();
```

> **Note**: C++ API only.

### uballoc_is_owner

**Function**

Returns whether this process is the heap owner (the process with rank 0).

**Definition**

```cpp
bool uballoc::is_owner();
```

> **Note**: C++ API only. The owner creates the bootstrap shm and global metadata segment.

### uballoc_is_initialized

**Function**

Returns whether the allocator has been initialized (`init()` called and succeeded).

**Definition**

```cpp
bool uballoc::is_initialized();
```

> **Note**: C++ API only.

### uballoc_reset

**Function**

Fully cleans up and resets allocator state. Executes `uffd_uninstall()` → `fh_uninstall()` → `clear_bootstrap_slot()` → `detach_all_regions()` → `wait_for_unreferenced()` → `unlink_own_shms()` → `cleanup_bootstrap_mapping()` → `cleanup_bootstrap()` → `release_reservation()`, then calls `soft_reset()` to clear in-memory state. Forked child processes should use `soft_reset()` instead of `reset()`.

**Definition**

```cpp
// C++ API
void uballoc::reset();

// C API
void uballoc_reset(void);
```

### uballoc_enable_fault_handler

**Function**

Enables the SIGSEGV-based lazy-attach handler as a fallback for uffd. On a page fault, the handler loads the remote segment via a pre-registered device fd; the faulting instruction resumes successfully.

**Definition**

```cpp
bool uballoc::enable_fault_handler();
bool uballoc_enable_fault_handler(void);
```

**Return Value**

- Success: `true`.
- Failure: `false` (e.g. alternate-stack allocation failed).

> **Note**: As of V0.4, uffd is on by default. Only when uffd is unavailable (`ENOSYS` or `EPERM`) does the application need to manually enable the fault handler as fallback.

### uballoc_disable_fault_handler

**Function**

Disables the fault handler, restores the previous SIGSEGV handler, and clears the segment registry. Called automatically by `reset()`/`soft_reset()`; safe to call multiple times.

**Definition**

```cpp
void uballoc::disable_fault_handler();
void uballoc_disable_fault_handler(void);
```

### uballoc_enable_userfaultfd

**Function**

Enables the userfaultfd-based lazy-attach handler. Tries uffd first; on failure, falls back to the SIGSEGV handler. The uffd handler runs in a separate thread in a normal (not signal) context, so it can call `ubs_mem_shm_attach` (socket/malloc/connect).

**Definition**

```cpp
bool uballoc::enable_userfaultfd();
bool uballoc_enable_userfaultfd(void);
```

**Return Value**

- Success: `true`.
- Failure: `false`.

> **Note**: As of V0.4, this function is called automatically at constructor priority 102; applications usually do not need to call it explicitly.

### uballoc_disable_userfaultfd

**Function**

Disables the uffd handler: stops the handler thread, closes the uffd fd, and `mprotect`s the reserved region back to `PROT_NONE`. Called automatically by `reset()`/`soft_reset()` and `~DistributedShmBackend()`; safe to call multiple times.

**Definition**

```cpp
void uballoc::disable_userfaultfd();
void uballoc_disable_userfaultfd(void);
```

### uballoc_purge

**Description**

Immediately returns all idle segments to the OS, skipping decay1/decay2 timers. For LIVE segments with `live_slab_count==0`, performs detach+delete in one step. For DETACHED segments, performs delete. Use when the application needs to return memory immediately (e.g., batch-processing idle periods).

**Definition**

```cpp
// C++ API
void uballoc::purge();

// C API
void uballoc_purge(void);
```

**Example**

```cpp
// Allocate and free a large batch
std::vector<void*> ptrs;
for (int i = 0; i < 10000; ++i) ptrs.push_back(uballoc::malloc(1024));
for (void* p : ptrs) uballoc::free(p);
ptrs.clear();

// Immediately return idle segments to OS
uballoc::purge();
```

### uballoc_return_stats

**Description**

Query memory reclaim statistics: segment state snapshot, cumulative transition counts, byte-level memory flow statistics.

**Definition**

```cpp
// C++ API
uballoc::ReturnStats uballoc::return_stats();

// C API
void uballoc_return_stats(uballoc_return_stats_t *out);
```

**Parameters (C API)**

| Parameter | Description | Range | I/O |
|-----------|-------------|-------|-----|
| `out` | Output: filled with stats struct | non-NULL | output |

**`ReturnStats` struct (C++)**

```cpp
struct ReturnStats {
    size_t segments_live;
    size_t segments_detached;
    size_t segments_returned;
    size_t total_detached_count;
    size_t total_returned_count;
    uint64_t requested_from_os_bytes;
    uint64_t returned_to_os_bytes;
    uint64_t allocated_to_app_bytes;
    uint64_t freed_from_app_bytes;
    double fragmentation_ratio;
};
```

**`uballoc_return_stats_t` struct (C API)**

```c
typedef struct {
    size_t segments_live;
    size_t segments_detached;
    size_t segments_returned;
    size_t total_detached_count;
    size_t total_returned_count;
    uint64_t requested_from_os_bytes;
    uint64_t returned_to_os_bytes;
    uint64_t allocated_to_app_bytes;
    uint64_t freed_from_app_bytes;
    double   fragmentation_ratio;
} uballoc_return_stats_t;
```

**Field descriptions**

| Field | Description |
|-------|-------------|
| `segments_live` | LIVE segments (in use) |
| `segments_detached` | DETACHED segments (pending return) |
| `segments_returned` | RETURNED segments (returned to OS) |
| `total_detached_count` | Cumulative detach count |
| `total_returned_count` | Cumulative return count |
| `requested_from_os_bytes` | Total bytes requested from OS |
| `returned_to_os_bytes` | Total bytes returned to OS |
| `allocated_to_app_bytes` | Total bytes allocated to app |
| `freed_from_app_bytes` | Total bytes freed by app |
| `fragmentation_ratio` | Fragmentation ratio = `(requested - returned) / (allocated - freed)`, >1.0 means fragmentation |

**Example (C++)**

```cpp
auto stats = uballoc::return_stats();
std::cout << "frag_ratio=" << stats.fragmentation_ratio
          << " live=" << stats.segments_live
          << " detached=" << stats.segments_detached
          << " returned=" << stats.segments_returned
          << std::endl;
```

**Example (C)**

```c
uballoc_return_stats_t s;
uballoc_return_stats(&s);
printf("frag_ratio=%.2f live=%lu detached=%lu returned=%lu\n",
       s.fragmentation_ratio, s.segments_live, s.segments_detached, s.segments_returned);
```

> **Note**: Set `UBALLOC_RECLAIM_STATS=0` to disable byte-level counting (zero hot-path overhead).
       s.segments_live, s.segments_detached, s.segments_returned);
```

> **Note**: Set `UBALLOC_RECLAIM_STATS=0` to disable byte-level counting (zero hot-path overhead).

---

### uballoc_usable_size

Query the usable size of an allocation.

**C API:**

```c
size_t uballoc_usable_size(void *pointer);
```

**C++ API:**

```cpp
size_t uballoc::usable_size(void* ptr);
```

| Parameter | Description | Range | I/O |
|-----------|-------------|-------|-----|
| `pointer` | Pointer returned by `uballoc_malloc` | non-NULL | input |

Returns: the usable size of the allocation (>= requested size, may be larger due to internal fragmentation). Returns 0 for NULL or unrecognized pointers.

**Use cases**:
- **Right-sizing**: if `usable_size(ptr) >= new_size`, no need to call `realloc` — the extra space can be used directly
- **Fragmentation assessment**: the difference between `usable_size` and the requested size reflects internal allocation waste

**Example**

```cpp
void* p = uballoc::malloc(63);   // requested 63B, allocated 64B
size_t us = uballoc::usable_size(p);  // us = 64
// if you later need 64B: usable_size(64) >= 64, no realloc needed
uballoc::free(p);
```

---

### uballoc_defrag_hints

Get a list of live allocations in low-occupancy memory regions for app-cooperative defragmentation.

**C API:**

```c
size_t uballoc_defrag_hints(double threshold,
                            uballoc_defrag_hint_t *out,
                            size_t max_hints);
```

**C++ API:**

```cpp
std::vector<uballoc::DefragHint> uballoc::defrag_hints(double threshold);
```

| Parameter | Description | Range | I/O |
|-----------|-------------|-------|-----|
| `threshold` | Memory region occupancy threshold | 0.0~1.0 | input |
| `out` (C API) | Output array | non-NULL | output |
| `max_hints` (C API) | Output array capacity | >0 | input |

Returns: C API returns number of hints written to `out`; C++ API returns `std::vector<DefragHint>`.

**`uballoc_defrag_hint_t` / `uballoc::DefragHint` struct**

```c
typedef struct {
    void*  ptr;        /* pointer to the live allocation */
    size_t size;        /* usable size of the allocation */
    double occupancy;   /* memory region occupancy (0.0~1.0) */
} uballoc_defrag_hint_t;
```

| Field | Description |
|-------|-------------|
| `ptr` | Pointer to the allocation to relocate |
| `size` | Usable size of the allocation (same as `usable_size` return) |
| `occupancy` | Block occupancy of the memory region containing this allocation, 0.0=empty, 1.0=full. Only allocations with occupancy < `threshold` are returned |

**Defrag workflow**

1. Call `return_stats` to check `fragmentation_ratio`
2. When fragmentation exceeds threshold, call `defrag_hints(0.5)` to get live allocations in low-occupancy regions
3. For each hint, relocate: `malloc(size)` + `memcpy` + fix references + `free(old_ptr)`
4. After relocation, call `purge()` to reclaim empty memory regions; fragmentation ratio drops

**Example**

```cpp
auto stats = uballoc::return_stats();
double frag = stats.fragmentation_ratio;

if (frag > 1.5) {  // fragmentation ratio > 1.5, start defrag
    auto hints = uballoc::defrag_hints(0.5);  // regions < 50% occupied
    for (auto& h : hints) {
        void* neu = uballoc::malloc(h.size);
        memcpy(neu, h.ptr, h.size);
        /* fix all references from h.ptr → neu */
        uballoc::free(h.ptr);
    }
    uballoc::purge();  // reclaim empty regions
}
```

> **Note**:
> - The allocator does not move data, because a C/C++ allocator does not know pointer reference relationships. The application must fix references itself.
> - `defrag_hints` should be called when no concurrent alloc/free is in progress (e.g., during an application maintenance cycle), to avoid reading memory state being modified by other threads.
> - Pointers returned by `defrag_hints` may include blocks created by other processes via cross-process allocation. The application must coordinate (publish/unpublish) when relocating cross-process referenced blocks.

---

# FAQ

This section summarizes high-frequency behavior semantics and usage constraints to help avoid common pitfalls.

## Does `realloc` allocate new memory and free the old block on shrink?

**No.** When `new_size <= old_size` (shrink or equal), `realloc` returns the original pointer directly — no new allocation, no data copy, no old-block free. This behavior is consistent with standard C `realloc` (glibc, jemalloc, mimalloc all behave this way).

Returning the original pointer on shrink means the excess space in the original slab slot/huge slot is not reclaimed; the slot is still occupied at its original size. To free the original slot for reuse by another process or thread, explicitly do `malloc(new_size)` + `memcpy` + `free(old_ptr)`.

On grow (`new_size > old_size`), `realloc` allocates a new block, copies data, frees the old block, and returns the new pointer (`!= old_ptr`).

**Special realloc behavior for Huge allocations:**

The `class_size` of a Huge allocation is `slot_count * SLAB_SIZE` (SLAB_SIZE=2 MB), not the actual requested allocation size. For example, a 5 MB Huge allocation occupies 3 slots, so `class_size` is 6 MB.

This means:

- **`new_size` <= `class_size` (even if `new_size` > original requested size)**: treated as shrink, returns the original pointer. For example, original allocation 5 MB (`class_size`=6 MB), `realloc` to 6 MB → 6 MB <= 6 MB → returns the original pointer, no new block. The excess slot space (6 MB - 6 MB = 0 MB) is wasted.

- **`new_size` > `class_size`**: treated as grow, allocates a new block, copies data, frees the old block, returns the new pointer (`!= old_ptr`). Since the old slot is still occupied at `allocate` time (CAS already claimed), the new allocation must land on a different slot, so `new_ptr != old_ptr`.

Therefore, when calling `realloc` on a Huge allocation, the old and new pointers may be the same (shrink) or different (grow), depending on the comparison of `new_size` vs `class_size` (not the original requested size).

## Inter-process Coordination and Exit Timing

### Core Principle

**The process that owns shared memory must ensure that all other processes have finished accessing that data before exiting (or calling `reset()`).** Violating this principle leads to data inaccessibility (lookup timeout) or process crashes (SIGBUS).

### Two Phases, Two Consequences

The typical lifecycle of shared data between processes has two phases, each with consequences if exit happens too early:

**Phase 1: Lookup phase** — the consumer discovers the publisher's data via `lookup_by_type_blocking`.

When the publisher process exits, its destructor cleans up the bootstrap slot and releases shared-memory metadata. After that, the consumer can no longer discover the publication; `lookup_by_type_blocking` times out with `owner_process == -1`.

**Phase 2: Access phase** — the consumer reads, modifies data, or calls `shm_delete` on a container.

In a UBSE multi-node environment, when a process exits it calls `shm_detach_by_name` to release shared memory, and the UBSE device **invalidates all mappings of that shm on all nodes**. If other processes are still accessing that data (e.g. an STL container destructor traversing tree nodes), the mappings become invalid and access triggers **SIGBUS**.

```
P0 (Node A)                       P1 (Node B)
──────────                        ──────────
                                  Publish data → wait for consumer ack
Lookup data ← ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ 
Read/modify data → signal "done"  ← Phase 1: publisher must not exit before this
                                  (otherwise lookup times out)
                                   
Verify data → shm_delete → ...    Exit → shm_detach_by_name()
  (destructor traverses P1's        → UBSE invalidates all node mappings ✗
   tree nodes in data segment)
P0 accesses invalid page → SIGBUS ✗ ← Phase 2: P1 must not exit before this
                                  (otherwise SIGBUS)
```

### Key Mechanism Notes

1. **`shm_detach_by_name` is an invalidation operation**: it not only releases this node's borrow but also invalidates all cross-node mappings. This is UBSE device behavior, not uballoc's. On POSIX this function is a no-op; mappings are unaffected.
2. **Lazy loading (uffd) makes refcounts unreliable**: the consumer may not have accessed the data yet (uffd handler hasn't attached), so the refcount is 0. The publisher cannot use refcounts to determine whether the consumer is still using the data — it must wait for an explicit signal.
3. **`shm_delete` accesses cross-process data**: when P0 calls `shm_delete` on an STL container that P1 modified, the destructor traverses the container's internal structures (tree nodes, array buffers), which live in P1's data segment. P1 must stay alive until P0's `shm_delete` completes.
4. **Auto-cleanup on process exit also invalidates mappings**: UBSE automatically releases all borrows on process exit (auto-cleanup), which also invalidates cross-node mappings. So merely deferring `shm_detach_by_name` does not solve the problem.

### Recommended Patterns

**Pattern 1: Both sides sleep (simple, recommended for testing)**

```cpp
// P1: signal "done" then sleep
uballoc::publish(reinterpret_cast<void*>(1), 0, TYPE_P1_DONE);
sleep(2);  // stay alive so P0 can finish shm_delete
return 0;

// P0: after ack, do shm_delete, then sleep
auto done = uballoc::lookup_by_type_blocking(TYPE_P1_DONE, 30000);
// ... verify data ...
uballoc::shm_delete(vec);  // destructor traverses P1's data segment (P1 still alive)
sleep(2);  // stay alive so P1's cleanup runs after P0 exits
return 0;
```

This pattern is verified in `stl_full_shm_example.cpp`.

**Pattern 2: Explicit completion handshake (more robust)**

```cpp
// P1: signal "done" → wait for P0's "shm_delete done" signal
uballoc::publish(&done_flag, sizeof(done_flag), TYPE_P1_DONE);
// wait for P0 to confirm shm_delete completion
auto p0_done = uballoc::lookup_by_type_blocking(TYPE_P0_DELETE_DONE, 30000);
return 0;

// P0: receive P1 done → shm_delete → notify P1 it can exit
auto done = uballoc::lookup_by_type_blocking(TYPE_P1_DONE, 30000);
// ... verify data ...
uballoc::shm_delete(vec);
uballoc::publish(&done_flag, sizeof(done_flag), TYPE_P0_DELETE_DONE);
return 0;
```

**Pattern 3: Publish/consume ready flag (for simple data passing)**

For scenarios where the publisher only passes raw data (not STL containers):

```cpp
// Publisher
auto* cfg = static_cast<ShareBlock*>(uballoc::malloc(sizeof(ShareBlock), type_id));
cfg->ready.store(1, std::memory_order_release);
while (cfg->ready.load(std::memory_order_acquire) != 2) { ::usleep(1000); }
uballoc::free(cfg);  // safe to free

// Consumer
auto info = uballoc::lookup_by_type_blocking(type_id, 10000);
auto* cfg = static_cast<ShareBlock*>(info.address);
while (cfg->ready.load(std::memory_order_acquire) != 1) { ::usleep(1000); }
// ... read and process data ...
cfg->ready.store(2, std::memory_order_release);  // notify publisher
```

Note: This pattern does not cover the `shm_delete` scenario. If the consumer modifies an STL container's contents, the publisher must still use Pattern 1 or 2 when calling `shm_delete`.

### What Not to Do

- Do not exit immediately after signaling "done" — the peer needs time to complete `shm_delete`.
- Do not call `reset()` while other processes are accessing your data.
- Do not assume the library can prevent cross-node mapping invalidation — this is UBSE device behavior the library cannot intercept.

### What the Library Provides

- `publish()` / `lookup_by_type_blocking()` — data discovery and synchronization.
- After the publisher enters cleanup, the consumer can still discover its data (the bootstrap slot stays `ready=1` during `wait_for_unreferenced`).
- `wait_for_unreferenced()` waits indefinitely for other nodes to release borrows — when all processes exit normally, shms are cleaned up automatically, no residue.
- `check_remote_segments()` — explicit (non-lazy) attach of remote segments (thread-safe; concurrent calls from multiple threads do not double-attach).

### What the Library Cannot Provide

- Knowing when other processes have "finished accessing" your data (requires application-level coordination).
- Preventing cross-node mapping invalidation on process exit (UBSE device behavior).
- Automatic recovery after a remote process crashes — the local process will wait indefinitely (hang), requiring manual kill + cleanup scripts to clear residual shms.

### Cleaning Up Residual Shared Memory

After a process crashes or is killed, the shared-memory objects it created may remain. The install package ships two cleanup scripts (in `share/uballoc/scripts/`):

**UBSE environment (`cleanup_shms_ub.sh`):**

Discovers and cleans up automatically via `ubsectl`. The script queries all shms matching the `ub-<heap_id>-*` prefix across the cluster, automatically identifies borrower and lender nodes, and cleans up in two phases (detach then delete).

```bash
# Clean up all residual shms with default heap_id="heap"
./cleanup_shms_ub.sh

# Clean up with a specific heap_id
UBALLOC_HEAP_ID=myheap ./cleanup_shms_ub.sh
```

Requirements:
- `ubsectl` is in `PATH` on this node and all peer nodes.
- Passwordless SSH to all peer nodes (or peers are the local node).

**POSIX environment (`cleanup_shms.sh`):**

Cleans up based on `/dev/shm` file paths. Requires the process count and node list.

```bash
# Single-machine cleanup (2 processes)
./cleanup_shms.sh 2

# Multi-node: P0 local, P1 on node1
./cleanup_shms.sh 2 "" node1

# Use a node-config file
./cleanup_shms.sh -f nodes.txt
```

`nodes.txt` format (one hostname per line; empty or "local" means the local machine):
```
local
10.0.0.2
10.0.0.3
```

The `UBALLOC_HEAP_ID` environment variable can specify the heap_id (default "heap").

**When to use:**
- After a process crash (when `wait_for_unreferenced` hangs → kill leaves residual shms).
- Cleanup between tests (ensure a clean environment).
- Periodic maintenance (clear residual shms accumulated over long runs).

---

The above results are for reference only; actual results may vary.

---

## Memory Reclaim FAQ

### What's the difference between SYNC and ASYNC modes?

| | SYNC mode | ASYNC mode (default) |
|---|---|---|
| Trigger | Inline `check_one_segment_for_reclaim` in malloc/free | Background thread timed `scan_all_segments` |
| Hot-path overhead | One segment scan per malloc/free | Only `trigger_now()` on segment-idle (nanoseconds) |
| Idle-period reclaim | No reclaim (no malloc/free events) | Reclaims (background thread scans autonomously) |
| Background thread | None | One per process (fork children skip it) |
| Use case | Simple scenarios, sustained high throughput | Production, idle-period reclaim |

### Do fork children start a background reclaim thread?

No. Fork children's `init()` detects PID change and skips ReclaimThread creation. Fork children are typically short-lived (alloc+free then `_exit`), so background reclaim is unnecessary. If needed, fork children can use SYNC mode or `purge()`.

---

## Known Issue: UBSE VMA Destruction

**Severity: High** — affects multi-process/multi-thread scenarios on the UBSE backend.

### Problem Description

The UBSE device driver or runtime, when performing cross-process `shm_create`/`shm_attach` operations, **destroys** existing mmap'd VMAs (virtual memory areas) in the target process's address space. The VMA is completely destroyed (not PROT_NONE — it disappears from `/proc/PID/maps`), so subsequent accesses to that address trigger SIGSEGV.

### Impact

- **uffd cannot catch this fault**: uffd only catches "page-missing" faults on uffd-registered VMAs. Once a VMA is destroyed, there is no uffd registration; the fault becomes a raw SIGSEGV.
- **Without handling, the process crashes**: SIGSEGV default behavior terminates the process.
- **Multi-threaded STL-container scenarios are prone to triggering**: observed in `test_multi_thread` (4 producer threads + 4 consumer threads, STL containers clear+refill). Single-threaded tests (e.g. `test_stl`) usually do not trigger due to different timing.

### Current Workaround: SIGSEGV+SIGBUS Handler as Safety Net

A SIGSEGV+SIGBUS handler is installed **even when uffd is active** (in the `uballoc_uffd_auto_init` constructor and `try_enable_uffd_locked`). When the handler detects a fault within the VA range, it looks up the corresponding segment fd from the `g_fh_entries` registry, re-`mmap`s (`MAP_FIXED`, `PROT_RW`) to restore the mapping, and the faulting thread resumes execution.

**Stderr diagnostic output:**
- `fh_sigsegv: resolved remap fault=0x...` — the handler re-mapped a destroyed VMA (normal operation, no need to worry).
- `fh_sigsegv: fault=0x... reason=...` + maps dump — the handler could not recover; the process is about to crash.

### Root Cause

UBSE device-driver behavior, outside uballoc's control. Requires the UBSE team to investigate:
- Do `ubs_mem_shm_attach`/`ubs_mem_shm_create` modify the calling process's existing VMAs?
- Does the UBSE kernel module munmap VMAs in a process's address space when another process creates an shm?

### Mitigation Advice

- Run `cleanup_shms_ub.sh` before each test to clear residual shms.
- In multi-threaded scenarios, start producer threads first, then consumer threads to reduce trigger probability.
- Watch stderr for `fh_sigsegv: resolved` messages — if they appear frequently, VMA destruction is happening.

---

Licensed under the Apache License, Version 2.0.
See [LICENSE](LICENSE) for the full license text.
