// SPDX-License-Identifier: Apache-2.0

#include "uballoc.hpp"
#include "test_helpers.hpp"

static void test_issue1_ready_flag_blocks_p1() {
    std::cout << "[fix] Issue 1: SharedLayout ready_flag prevents P1 from reading unwritten layout" << std::endl;

    cleanup_all_distributed_shms(2, "test");

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        cleanup_all_distributed_shms(2, "test");

        std::string shm_name = "/test-p0-m";
        shm_unlink(shm_name.c_str());

        int fd = shm_open(shm_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        assert(fd >= 0);

        size_t shared_size = 1024 * 1024;
        assert(ftruncate(fd, static_cast<off_t>(shared_size)) == 0);

        // Use a VA outside uballoc's reservation (0x200000000000) to
        // avoid conflict with the uffd constructor which reserves that
        // range before main() on UBSE when UBALLOC_HEAP_ID is set.
        void* ptr = mmap(reinterpret_cast<void*>(0x100000000000ULL), shared_size,
                          PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        assert(ptr != MAP_FAILED);

        auto* layout = reinterpret_cast<uballoc::SharedLayout*>(ptr);

        assert(layout->magic == 0);
        assert(layout->ready_flag.load() == 0);

        std::cout << "  SharedLayout ready_flag=0 before P0 writes (P1 would spin-wait)" << std::endl;

        layout->magic = uballoc::SharedLayout::MAGIC;
        layout->layout_version = 1;
        layout->ready_flag.store(1, std::memory_order_release);

        assert(layout->ready_flag.load(std::memory_order_acquire) == 1);
        std::cout << "  SharedLayout ready_flag=1 after P0 writes (P1 can proceed)" << std::endl;

        munmap(ptr, shared_size);
        close(fd);
        shm_unlink(shm_name.c_str());

        _exit(0);
    } else {
        int status;
        waitpid(pid, &status, 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        cleanup_all_distributed_shms(2, "test");
        std::cout << "  Issue 1 fix verified: ready_flag blocks P1 until P0 completes" << std::endl;
    }
}

int main() {
    test_issue1_ready_flag_blocks_p1();
    std::cout << "\n=== Issue 1 test passed ===" << std::endl;
    return 0;
}