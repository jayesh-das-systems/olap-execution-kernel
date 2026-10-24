#pragma once

#if !defined(__cplusplus) || __cplusplus < 202302L
#error "FATAL: Strict ISO C++23 required (-std=c++23)."
#endif

#if !defined(__linux__) && !defined(__unix__)
#error "FATAL: This execution engine requires a POSIX/Linux environment."
#endif

#include <version>
#include <new>
#include <memory>
#include <cstdint>
#include <cstddef>

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error "FATAL: ISO C++23 <expected> (P0323R12) required."
#endif

#if !defined(__cpp_explicit_this_parameter) || __cpp_explicit_this_parameter < 202110L
#error "FATAL: ISO C++23 Deducing this (P0847R7) required."
#endif

namespace engine {

// Polyfill std::start_lifetime_as for libstdc++ implementations (like GCC 14) 
// that support C++23 core language but lack WG21 P2590R2 library macros.
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
    using std::start_lifetime_as;
    using std::start_lifetime_as_array;
#else
    template <typename T>
    [[nodiscard]] inline T* start_lifetime_as(void* p) noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        return std::launder(reinterpret_cast<T*>(p));
    }

    template <typename T>
    [[nodiscard]] inline const T* start_lifetime_as(const void* p) noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        return std::launder(reinterpret_cast<const T*>(p));
    }

    template <typename T>
    [[nodiscard]] inline T* start_lifetime_as_array(void* p, size_t n) noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        (void)n;
        return std::launder(reinterpret_cast<T*>(p));
    }

    template <typename T>
    [[nodiscard]] inline const T* start_lifetime_as_array(const void* p, size_t n) noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        (void)n;
        return std::launder(reinterpret_cast<const T*>(p));
    }
#endif

// =============================================================================
// Platform & Address Space Invariants
// =============================================================================
static_assert(sizeof(void*) == 8uz, "Engine requires a 64-bit address space");
static_assert(sizeof(size_t) == 8uz, "Engine requires 64-bit size_t (LP64/LLP64)");

inline constexpr size_t CACHE_LINE_SIZE = 64uz;
inline constexpr size_t CACHE_LINE_MASK = CACHE_LINE_SIZE - 1uz;

inline constexpr size_t DUAL_CACHE_LINE_SIZE = 128uz;
inline constexpr size_t DEFAULT_PAGE_SIZE = 4096uz;
inline constexpr size_t HUGE_PAGE_SIZE    = 2uz * 1024uz * 1024uz;

inline constexpr size_t VECTOR_BATCH_SIZE = 1024uz;
inline constexpr size_t VECTOR_BATCH_MASK = VECTOR_BATCH_SIZE - 1uz;

inline constexpr uint64_t MAX_STRING_POOL_CAPACITY = 0xFFFFFFFFuz;

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
