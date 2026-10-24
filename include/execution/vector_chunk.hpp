#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <cassert>
#include <span>

namespace engine {

inline constexpr size_t VECTOR_SIZE = 1024uz;

struct SelectionVector {
    std::array<uint16_t, VECTOR_SIZE> indices{};
    uint16_t count = 0;

    constexpr void add(uint16_t idx) noexcept {
        assert(count < VECTOR_SIZE && "SelectionVector capacity overflow");
        indices[count++] = idx;
    }

    constexpr void clear() noexcept {
        count = 0;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return count == 0;
    }

    [[nodiscard]] constexpr std::span<const uint16_t> active() const noexcept {
        return {indices.data(), count};
    }

    [[nodiscard]] constexpr uint16_t operator[](size_t idx) const noexcept {
        assert(idx < count);
        return indices[idx];
    }
};

struct HashVector {
    std::array<uint64_t, VECTOR_SIZE> hashes{};

    constexpr void initialize(size_t n, uint64_t seed = 0x9e3779b97f4a7c15uz) noexcept {
        assert(n <= VECTOR_SIZE);
        for (size_t i = 0uz; i < n; ++i) {
            hashes[i] = seed;
        }
    }

    [[nodiscard]] constexpr uint64_t& operator[](size_t idx) noexcept {
        assert(idx < VECTOR_SIZE);
        return hashes[idx];
    }

    [[nodiscard]] constexpr const uint64_t& operator[](size_t idx) const noexcept {
        assert(idx < VECTOR_SIZE);
        return hashes[idx];
    }
};

} // namespace engine
