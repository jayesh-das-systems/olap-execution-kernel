#include "common/exit_codes.hpp"
#include "common/hardware_config.hpp"

#include <iostream>
#include <unistd.h>
#include <sys/mman.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cerrno>

int main() {
    // 1. Enforce 64-bit Address Space
    if constexpr (sizeof(void*) != 8uz || sizeof(size_t) != 8uz) {
        std::cerr << "[GATE FAULT] Non-64-bit architecture: sizeof(void*)=" 
                  << sizeof(void*) << ", sizeof(size_t)=" << sizeof(size_t) << "\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_SYS_HARDWARE_INVARIANT);
    }

    // 2. Enforce Standard 4096-Byte Page Size
    const long sys_page_size = ::sysconf(_SC_PAGESIZE);
    if (sys_page_size != static_cast<long>(engine::DEFAULT_PAGE_SIZE)) {
        std::cerr << "[GATE FAULT] Virtual memory page size mismatch: expected "
                  << engine::DEFAULT_PAGE_SIZE << " bytes, found " << sys_page_size << " bytes.\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_PAGE_SIZE_MISMATCH);
    }

    // 3. Verify Cache Line Alignment Guarantees
    alignas(engine::CACHE_LINE_SIZE) uint8_t test_buffer[engine::CACHE_LINE_SIZE];
    if (!engine::is_cache_aligned(test_buffer)) {
        std::cerr << "[GATE FAULT] Compiler/runtime failed to align buffer to 64-byte boundary.\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_ALIGNMENT_FAULT);
    }

    // 4. Test POSIX mmap & madvise Capability in Current Container
    void* probe_addr = ::mmap(nullptr, engine::DEFAULT_PAGE_SIZE,
                              PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe_addr == MAP_FAILED) {
        const int err = errno;
        std::cerr << "[GATE FAULT] POSIX mmap rejected by host kernel: " 
                  << std::strerror(err) << " (errno " << err << ")\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_MMAP_FAILED);
    }

    if (::posix_madvise(probe_addr, engine::DEFAULT_PAGE_SIZE, POSIX_MADV_SEQUENTIAL) != 0) {
        const int err = errno;
        std::cerr << "[GATE FAULT] posix_madvise rejected by host kernel: " 
                  << std::strerror(err) << " (errno " << err << ")\n";
        ::munmap(probe_addr, engine::DEFAULT_PAGE_SIZE);
        return engine::to_underlying(engine::EngineExitCode::ERR_MADVISE_FAILED);
    }

    if (::munmap(probe_addr, engine::DEFAULT_PAGE_SIZE) != 0) {
        const int err = errno;
        std::cerr << "[GATE FAULT] munmap probe failed: " 
                  << std::strerror(err) << " (errno " << err << ")\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_MUNMAP_FAILED);
    }

    std::cout << "[GATE PASS] Tier 1: Hardware invariants, 4 KiB pages, and POSIX mmap verified.\n";
    return engine::to_underlying(engine::EngineExitCode::SUCCESS);
}
