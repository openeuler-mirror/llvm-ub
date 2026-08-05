// SPDX-License-Identifier: Apache-2.0

#include <uballoc.hpp>
#include <uballoc/stl_alloc.hpp>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <unistd.h>

static void fill(void* p, size_t n, uint8_t v) { memset(p, v, n); }

static bool verify(void* p, size_t n, uint8_t v) {
    auto* b = static_cast<uint8_t*>(p);
    for (size_t i = 0; i < n; ++i)
        if (b[i] != v) return false;
    return true;
}

static void section(const char* title) {
    std::cout << "\n---------- " << title << " ----------" << std::endl;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <app_pid>\n"
                  << "  app_pid: application process id (0 or 1)\n"
                  << "Environment: UBALLOC_HEAP_ID must be set.\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 1\n";
        return 1;
    }
    int rank = std::atoi(argv[1]);
    if (rank != 0 && rank != 1) {
        std::cerr << "Error: app_pid must be 0 or 1, got: " << rank << std::endl;
        return 1;
    }

    const char* heap_id = std::getenv("UBALLOC_HEAP_ID");
    if (!heap_id || !*heap_id) {
        std::cerr << "Error: UBALLOC_HEAP_ID not set.\n"
                  << "Usage: export UBALLOC_HEAP_ID=myheap\n"
                  << "  Terminal 1:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 0 &\n"
                  << "  Terminal 2:  UBALLOC_HEAP_ID=myheap " << argv[0] << " 1\n";
        return 1;
    }

    std::cout << "========================================\n"
              << "  uballoc Distributed Demo\n"
              << "  UBALLOC_HEAP_ID=" << heap_id << "\n"
              << "  app_pid=" << rank << "\n"
              << "========================================" << std::endl;

    // Config: memory allocated on demand (no pre-allocation needed)
    // Small: 8B-1016B (32KB slabs), Large: 1KB-16KB (2MB slabs), Huge: >16KB (4MB slots)

    // Explicit init: reads UBALLOC_HEAP_ID, discovers rank and membership
    // at runtime via the bootstrap PID table. Thread ids are auto-assigned
    // lazily on first malloc/free.
    uballoc::init();

    // --- Test: alloc/free small, large, huge ---
    section("Alloc/free across all three size brackets");

    constexpr size_t SZ_S = 64, SZ_L = 32768, SZ_H = 4 * 1024 * 1024;

    void* p_s = uballoc::malloc(SZ_S);
    uint8_t pat = static_cast<uint8_t>(0xA0 | rank);

    std::cout << "Init OK (app_pid=" << rank
              << " uballoc_total=" << uballoc::total_processes() << ")" << std::endl;
    std::cout << "Allocating: small=" << SZ_S << "B large=" << SZ_L
              << "B huge=" << SZ_H << "B (pattern=0x" << std::hex << (int)pat
              << std::dec << ")" << std::endl;

    void* p_l = uballoc::malloc(SZ_L);
    void* p_h = uballoc::malloc(SZ_H);

    std::cout << "  small: ptr=" << p_s << " class_size=" << uballoc::current_allocator().class_size(p_s) << "B" << std::endl;
    std::cout << "  large: ptr=" << p_l << " class_size=" << uballoc::current_allocator().class_size(p_l) << "B" << std::endl;
    std::cout << "  huge:  ptr=" << p_h << " class_size=" << uballoc::current_allocator().class_size(p_h) << "B" << std::endl;

    if (!p_s || !p_l || !p_h) {
        std::cerr << "malloc failed" << std::endl;
        return 1;
    }

    fill(p_s, SZ_S, pat);
    fill(p_l, SZ_L, pat);
    fill(p_h, SZ_H, pat);

    bool ok = verify(p_s, SZ_S, pat) && verify(p_l, SZ_L, pat) && verify(p_h, SZ_H, pat);
    std::cout << "Write+verify all brackets: " << (ok ? "OK" : "FAIL") << std::endl;

    // --- Test: cross-node publish / lookup_by_type ---
    section("Cross-node publish / lookup_by_type");

    constexpr uint32_t TYPE_DATA = 1, TYPE_ACK = 2, TYPE_REMOTE = 3;
    uint32_t my_type = (rank == 0) ? TYPE_DATA : TYPE_ACK;
    uint32_t peer_type = (rank == 0) ? TYPE_ACK : TYPE_DATA;

    std::cout << "malloc(" << SZ_S << ", type=" << my_type << ")..." << std::endl;
    void* pub = uballoc::malloc(SZ_S, my_type);
    if (!pub) {
        std::cerr << "malloc(size, type_id) failed" << std::endl;
        return 1;
    }
    fill(pub, SZ_S, pat);
    std::cout << "  Allocated+published: ptr=" << pub << " size=" << SZ_S << "B" << std::endl;

    std::cout << "Waiting for peer's type " << peer_type << " (up to 30s)..." << std::endl;
    auto peer = uballoc::lookup_by_type_blocking(peer_type, 30000);
    if (peer.owner_process >= 0) {
        uint8_t pp = static_cast<uint8_t>(0xA0 | (1 - rank));
        std::cout << "Found peer block: owner=P" << peer.owner_process
                  << " ptr=" << peer.address << " size=" << peer.size << "B" << std::endl;
        std::cout << "Verifying peer data (pattern=0x" << std::hex << (int)pp
                  << std::dec << "): " << (verify(peer.address, peer.size, pp) ? "OK" : "FAIL")
                  << std::endl;
    } else {
        std::cerr << "Peer type " << peer_type << " not found within timeout" << std::endl;
    }

    // --- Test: cross-node remote free ---
    section("Cross-node remote free");

    if (rank == 0) {
        void* rem = uballoc::malloc(SZ_S, TYPE_REMOTE);
        if (!rem) {
            std::cerr << "malloc(size, type_id) failed for remote block" << std::endl;
            return 1;
        }
        fill(rem, SZ_S, 0xC0);
        std::cout << "  Allocated+published remote-free block (type=" << TYPE_REMOTE
                  << " ptr=" << rem << " size=" << SZ_S << "B pattern=0xC0)" << std::endl;
        std::cout << "  Waiting for P1 to free it..." << std::endl;
        usleep(2000000);
    } else {
        std::cout << "  Looking up remote-free block (type=" << TYPE_REMOTE << ")..." << std::endl;
        auto rem = uballoc::lookup_by_type_blocking(TYPE_REMOTE, 30000);
        if (rem.owner_process >= 0) {
            std::cout << "  Found remote block: owner=P" << rem.owner_process
                      << " ptr=" << rem.address << std::endl;
            bool v = verify(rem.address, SZ_S, 0xC0);
            std::cout << "  Verify pattern 0xC0: " << (v ? "OK" : "FAIL") << std::endl;
            std::cout << "  Freeing remote block..." << std::endl;
            uballoc::free(rem.address);
            std::cout << "  Remote free: OK" << std::endl;
        } else {
            std::cerr << "  Remote-free block not found within timeout" << std::endl;
        }
    }

    // --- Test: cross-process realloc safety with spinlock ---
    section("Cross-process realloc with spinlock");

    // Pattern (spinlock + indirection):
    //   - A SharedBuffer control struct (SpinMutex + data ptr + size +
    //     generation) lives in shm via uballoc::shm_new. Its address is
    //     published with a type_id so peers can look it up.
    //   - Writer (P0) locks the spinlock, calls realloc on the data
    //     pointer, updates ptr/size/generation under the lock, unlocks.
    //   - Reader (P1) locks, snapshots (data, size, generation), unlocks,
    //     then uses the snapshot. The generation counter lets readers
    //     detect that a realloc happened since their last read.
    //
    // Why a spinlock (not pthread_mutex_t with PTHREAD_PROCESS_SHARED)?
    //   pthread_mutex_t uses futex(2) for the contended path. The kernel's
    //   cross-process futex lookup hashes the underlying inode+offset of
    //   the shared page. POSIX shm_open regions are tmpfs-backed (inode
    //   is shareable across processes) so futex works. UBSE shm regions
    //   (ubs_mem_shm_*) are not tmpfs-backed, so the kernel returns
    //   ENOSYS from futex and pthread_mutex_lock fails with "The futex
    //   facility returned an unexpected error code". A spinlock uses only
    //   atomic CAS, which works on any shm backend that supports atomics
    //   (UBSE shm does — the rest of the allocator relies on it).
    //
    // Trade-off: spinlocks busy-wait under contention. For short critical
    // sections (like the realloc snapshot here) this is fine; for
    // long-held locks you'd want futex-based locking on POSIX tmpfs
    // regions only. The generation-counter pattern below is the same
    // regardless of lock type — see dynamic_discovery_example.cpp for
    // an even lighter-weight RCU-style atomic-ready-flag variant that
    // avoids any lock entirely (single-writer only).

    constexpr uint32_t TYPE_SHARED_BUF = 100;

    // Simple spinlock using atomic CAS. NO futex syscall — works on
    // UBSE shm, tmpfs shm, hugetlb, etc.
    class SpinMutex {
    public:
        SpinMutex() : state_(0) {}
        void lock() {
            uint32_t expected = 0;
            while (!state_.compare_exchange_weak(
                       expected, 1,
                       std::memory_order_acquire,
                       std::memory_order_relaxed)) {
                expected = 0;
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                __asm__ __volatile__("yield" ::: "memory");
#endif
            }
        }
        void unlock() {
            state_.store(0, std::memory_order_release);
        }
    private:
        std::atomic<uint32_t> state_;
    };

    struct alignas(64) SharedBuffer {
        SpinMutex mutex;
        void*  data;
        size_t size;
        uint64_t generation;
    };

    if (rank == 0) {
        auto* sb = uballoc::shm_new<SharedBuffer>(uballoc::pub_tid{TYPE_SHARED_BUF});
        sb->data = uballoc::malloc(64);
        sb->size = 64;
        sb->generation = 0;
        fill(sb->data, 64, 0xD0);
        std::cout << "  P0: published SharedBuffer at " << sb
                  << " (gen=0 size=64 data=" << sb->data << ")" << std::endl;

        usleep(3000000);  // give P1 time to read gen 0

        std::cout << "  P0: reallocing 64 -> 256 under spinlock..." << std::endl;
        sb->mutex.lock();
        sb->data = uballoc::realloc(sb->data, 256);
        sb->size = 256;
        sb->generation++;
        fill(sb->data, 256, 0xD1);
        sb->mutex.unlock();
        std::cout << "  P0: realloc done (gen=" << sb->generation
                  << " data=" << sb->data << ")" << std::endl;

        usleep(3000000);  // give P1 time to read gen 1

        sb->mutex.lock();
        uballoc::free(sb->data);
        sb->data = nullptr;
        sb->size = 0;
        sb->generation++;
        sb->mutex.unlock();
        uballoc::unpublish(sb);
        uballoc::shm_delete(sb);
        std::cout << "  P0: cleanup done" << std::endl;
    } else {
        auto info = uballoc::lookup_by_type_blocking(TYPE_SHARED_BUF, 30000);
        if (info.owner_process < 0) {
            std::cerr << "  P1: SharedBuffer not found" << std::endl;
        } else {
            auto* sb = static_cast<SharedBuffer*>(info.address);
            std::cout << "  P1: found SharedBuffer at " << sb << std::endl;

            // First read under spinlock (expecting gen=0)
            sb->mutex.lock();
            void* data = sb->data;
            size_t size = sb->size;
            uint64_t gen = sb->generation;
            sb->mutex.unlock();
            bool ok = verify(data, size, 0xD0);
            std::cout << "  P1: read gen=" << gen << " size=" << size
                      << " data=" << data << " verify(0xD0)=" << (ok ? "OK" : "FAIL") << std::endl;

            // Poll generation until P0's realloc bumps it
            for (int i = 0; i < 50; ++i) {
                sb->mutex.lock();
                uint64_t g = sb->generation;
                sb->mutex.unlock();
                if (g != gen) break;
                usleep(100000);
            }

            // P0's realloc may have moved the data buffer into a new
            // data segment that we haven't attached to yet. Calling
            // lookup_by_type() triggers check_remote_segments() (since
            // the type_id's owner is P0, not us), which auto-attaches
            // any new segments P0 created. Without this, dereferencing
            // sb->data after a realloc would SIGSEGV on the unattached VA.
            uballoc::lookup_by_type(TYPE_SHARED_BUF);

            // Re-read under spinlock (expecting gen=1 with new data)
            sb->mutex.lock();
            data = sb->data;
            size = sb->size;
            gen = sb->generation;
            sb->mutex.unlock();
            ok = verify(data, size, 0xD1);
            std::cout << "  P1: re-read gen=" << gen << " size=" << size
                      << " data=" << data << " verify(0xD1)=" << (ok ? "OK" : "FAIL") << std::endl;
        }
    }

    // --- Cleanup: free all local allocations ---
    section("Cleanup");
    uballoc::free(pub);
    uballoc::free(p_s);
    uballoc::free(p_l);
    uballoc::free(p_h);
    std::cout << "Freed: pub=" << pub << " small=" << p_s << " large=" << p_l << " huge=" << p_h << std::endl;

    std::cout << "\n=== Demo complete (P" << rank << ") ===" << std::endl;
    return 0;
}
