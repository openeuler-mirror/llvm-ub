# uballoc

**[中文](README.md)** | **English**

C++17 shared memory allocator for multi-process, multi-node deployments on ARMv8.

## Features

- **Three size brackets** (4-per-doubling class granularity):
  - Small: 8 B – 16 KB, 32 KB slabs (37 classes, 4-per-doubling)
  - Large: 16 KB – 2 MB, 4 MB slabs (28 classes, 4-per-doubling)
  - Huge: > 2 MB, 2 MB slots (allocated by slot count)
- **Ladder segment growth**: Small/Large segments grow in 4MB→16MB→64MB→128MB steps; first allocation uses only 4MB (not a full 128MB segment)
- **Distributed shared memory**: fixed-VA mmap, cross-process alloc/free,
  cross-node publish/lookup
- **Multi-process per node**: per-node refcounting, `claim_rank` dead-slot
  detection, `node_index` CAS assignment
- **Freed memory return**: idle segments transition LIVE→DETACHED→RETURNED
  and are returned to the UBSE pool for cross-node reuse
- **Introspection & defrag**: `return_stats` provides per-slab occupancy, Huge slot stats, empty-segment detection; `defrag_hints` returns live allocations in low-occupancy slabs for app-cooperative defragmentation
- **Lazy attach**: userfaultfd-based on-demand segment attachment for remote
  data
- **CRTP backends**: no virtual calls in the allocation path
- **ARMv8.1 LSE atomics**: compile-time toggleable
- **Crash recovery**: detectable CAS, state logging, cache flush
- **Multi-threaded**: per-thread allocators, cross-thread free

## Build

### Requirements

- aarch64 (ARMv8.0 minimum; ARMv8.1 LSE optional, on by default)
- GCC 12+ (or Clang 14+)
- CMake 3.16+
- C++17

### Configure

```bash
# POSIX shm (standard Linux /dev/shm, no UBSE dependency)
cmake -B build -DUBALLOC_USE_UBSE=OFF && cmake --build build

# UBSE backend (requires the ubse-client SDK; default)
cmake -B build && cmake --build build
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `UBALLOC_USE_UBSE` | ON | Use `UBShmProvider` as default (requires ubse-client) |
| `UBALLOC_LSE` | ON | ARMv8.1 LSE atomic instructions |
| `UBALLOC_LOG` | ON | Distributed path logging |
| `UBALLOC_BUILD_TESTS` | ON | Build tests |
| `UBALLOC_BUILD_EXAMPLES` | ON | Build examples |
| `UBALLOC_RECOVER_CAS` | ON | Detectable CAS for recovery |
| `UBALLOC_RECOVER_LOG` | ON | State logging for recovery |
| `UBALLOC_RECOVER_FLUSH` | OFF | Cache flushing for recovery |

## Quick Start

```bash
# Same UBALLOC_HEAP_ID on every node — picks the bootstrap PID table
export UBALLOC_HEAP_ID=myheap
```

```cpp
#include "uballoc.hpp"

// init() reads UBALLOC_HEAP_ID from the environment and discovers
// rank and membership at runtime via the bootstrap PID table.
uballoc::init();

void* p = uballoc::malloc(64);
// ... use p ...
uballoc::free(p);
```

Run the distributed example on two nodes:

```bash
UBALLOC_HEAP_ID=ubex ./build/ub_example 0 &   # Node 0
UBALLOC_HEAP_ID=ubex ./build/ub_example 1     # Node 1
```

## Cross-Node Publish/Lookup

```cpp
// Node 0: publish a block
void* p = alloc.malloc(64);
alloc.publish(p, 64, /*type_id=*/1);

// Node 1: find and use it
auto info = alloc.lookup_by_type(1);
// info.address is valid on Node 1 too (same fixed VA)
```

## Shm Providers

| Provider | Header | Use Case |
|----------|--------|----------|
| `UBShmProvider` | `ubshm_provider.hpp` | Custom machine with ubse (default) |
| `PosixShmProvider` | `shm_provider.hpp` | Standard Linux `/dev/shm` |

## Crash Cleanup

On normal process exit, shms are automatically cleaned up (detached and
deleted via `shm_unlink`). The cleanup script is only needed for crashed
processes that didn't run the destructor:

```bash
# UBShmProvider (ubse) — discovers existing shms and attacher nodes via ubsectl
./scripts/cleanup_shms_ub.sh                            # default UBALLOC_HEAP_ID=heap
UBALLOC_HEAP_ID=myheap ./scripts/cleanup_shms_ub.sh   # custom heap_id

# PosixShmProvider
./scripts/cleanup_shms.sh 2 "" node1
```

## Tests

```bash
cd build && ctest --output-on-failure
```

## Documentation

The user manual lives at [`doc/user_manual.en.md`](doc/user_manual.en.md) ([中文版](doc/user_manual.zh.md)).

## Project Layout

```
include/uballoc/    Headers
src/                Source
tests/              Tests
example/            Examples
doc/                User manual
scripts/            Cleanup scripts
```

## License

Apache-2.0. See [`LICENSE`](LICENSE) for the full text.
