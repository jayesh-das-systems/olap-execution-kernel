#pragma once

#if !defined(__cplusplus) || __cplusplus < 202302L
#error "FATAL: Strict ISO C++23 required. Refusing to compile under older standards."
#endif

#if !defined(__linux__) && !defined(__unix__)
#error "FATAL: This execution engine requires a POSIX/Linux environment."
#endif

#include <version>

#if !defined(__cpp_lib_start_lifetime_as) || __cpp_lib_start_lifetime_as < 202207L
#error "FATAL: ISO C++23 <memory> std::start_lifetime_as (P2590R2) required."
#endif

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error "FATAL: ISO C++23 <expected> (P0323R12) required."
#endif

#if !defined(__cpp_explicit_this_parameter) || __cpp_explicit_this_parameter < 202110L
#error "FATAL: ISO C++23 Deducing this (P0847R7) required."
#endif

#include <cstdint>
#include <cstddef>
#include <new>

namespace engine {

// =============================================================================
// Platform & Address Space Invariants
// =============================================================================
static_assert(sizeof(void*) == 8uz, "Engine requires a 64-bit address space");
static_assert(sizeof(size_t) == 8uz, "Engine requires 64-bit size_t (LP64/LLP64)");

// Hardware Cache Line Boundaries (Standard x86-64 / AArch64)
inline constexpr size_t CACHE_LINE_SIZE = 64uz;
inline constexpr size_t CACHE_LINE_MASK = CACHE_LINE_SIZE - 1uz;

inline constexpr size_t DUAL_CACHE_LINE_SIZE = 128uz;
inline constexpr size_t DEFAULT_PAGE_SIZE = 4096uz;
inline constexpr size_t HUGE_PAGE_SIZE    = 2uz * 1024uz * 1024uz;

inline constexpr size_t VECTOR_BATCH_SIZE = 1024uz;
inline constexpr size_t VECTOR_BATCH_MASK = VECTOR_BATCH_SIZE - 1uz;

inline constexpr uint64_t MAX_STRING_POOL_CAPACITY = 0xFFFFFFFFuz;

// =============================================================================
// Alignment Helper Invariants
// =============================================================================
[[nodiscard]] inline constexpr bool is_cache_aligned(const void* ptr) noexcept {
    return (reinterpret_cast<uintptr_t>(ptr) & CACHE_LINE_MASK) == 0uz;
}

[[nodiscard]] inline constexpr size_t align_to_cache_line(size_t bytes) noexcept {
    return (bytes + CACHE_LINE_MASK) & ~CACHE_LINE_MASK;
}

[[nodiscard]] inline constexpr size_t align_to_page(size_t bytes) noexcept {
    return (bytes + (DEFAULT_PAGE_SIZE - 1uz)) & ~(DEFAULT_PAGE_SIZE - 1uz);
}

} // namespace engine