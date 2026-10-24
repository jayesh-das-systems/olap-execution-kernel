#pragma once

#include "schema.hpp"
#include "vector_chunk.hpp"
#include <cstdint>
#include <cassert>
#include <string_view>
#include <algorithm>
#include <limits>
#include <expected>
#include <span>

namespace engine {

enum class ArithmeticOp : uint8_t {
    ADD = 1,
    SUB = 2,
    MUL = 3,
    DIV = 4
};

enum class ArithmeticError : uint8_t {
    OVERFLOW,
    DIVISION_BY_ZERO
};

[[nodiscard]] inline constexpr std::expected<int64_t, ArithmeticError>
evaluate_arithmetic(int64_t base, int64_t operand, ArithmeticOp op) noexcept {
    switch (op) {
        case ArithmeticOp::ADD: {
            if ((operand > 0 && base > std::numeric_limits<int64_t>::max() - operand) ||
                (operand < 0 && base < std::numeric_limits<int64_t>::min() - operand)) [[unlikely]] {
                return std::unexpected(ArithmeticError::OVERFLOW);
            }
            return base + operand;
        }
        case ArithmeticOp::SUB: {
            if ((operand < 0 && base > std::numeric_limits<int64_t>::max() + operand) ||
                (operand > 0 && base < std::numeric_limits<int64_t>::min() + operand)) [[unlikely]] {
                return std::unexpected(ArithmeticError::OVERFLOW);
            }
            return base - operand;
        }
        case ArithmeticOp::MUL: {
            if (base > 0) {
                if (operand > 0 && base > std::numeric_limits<int64_t>::max() / operand) [[unlikely]] {
                    return std::unexpected(ArithmeticError::OVERFLOW);
                }
                if (operand < 0 && operand < std::numeric_limits<int64_t>::min() / base) [[unlikely]] {
                    return std::unexpected(ArithmeticError::OVERFLOW);
                }
            } else if (base < 0) {
                if (operand > 0 && base < std::numeric_limits<int64_t>::min() / operand) [[unlikely]] {
                    return std::unexpected(ArithmeticError::OVERFLOW);
                }
                if (operand < 0 && operand < std::numeric_limits<int64_t>::max() / base) [[unlikely]] {
                    return std::unexpected(ArithmeticError::OVERFLOW);
                }
            }
            return base * operand;
        }
        case ArithmeticOp::DIV: {
            if (operand == 0) [[unlikely]] {
                return std::unexpected(ArithmeticError::DIVISION_BY_ZERO);
            }
            if (base == std::numeric_limits<int64_t>::min() && operand == -1) [[unlikely]] {
                return std::unexpected(ArithmeticError::OVERFLOW);
            }
            return base / operand;
        }
    }
    return base;
}

// Vectorizer-friendly contiguous array filter
inline size_t filter_int_batch(size_t batch_offset,
                               size_t batch_count,
                               int64_t min_v,
                               int64_t max_v,
                               SelectionVector& sel) noexcept {
    const size_t bounded_count = std::min(batch_count, VECTOR_SIZE);
    sel.clear();

    const int64_t* __restrict__ col = g_state.int_col + batch_offset;
    const uint8_t* __restrict__ val = g_state.int_validity;
    const uint8_t* __restrict__ live = g_state.live_mask;

    for (size_t i = 0uz; i < bounded_count; ++i) {
        const size_t row = batch_offset + i;
        const bool row_live = (row >= g_state.head_idx) &&
                              (row < g_state.tail_idx) &&
                              ((live[row >> 3uz] >> (row & 7uz)) & 1uz);

        const bool row_val = (val[row >> 3uz] >> (row & 7uz)) & 1uz;
        const int64_t item = col[i];

        if (row_live && row_val && (item >= min_v && item <= max_v)) {
            sel.add(static_cast<uint16_t>(i));
        }
    }
    return bounded_count;
}

inline void refine_filter_char(size_t batch_offset,
                               char match_char,
                               SelectionVector& sel) noexcept {
    if (sel.empty()) return;

    uint16_t matches = 0;
    const auto active_span = sel.active();

    for (size_t i = 0uz; i < active_span.size(); ++i) {
        const uint16_t idx = active_span[i];
        const size_t row = batch_offset + idx;

        if (is_row_live(row) && is_row_valid(g_state.char_validity, row) && g_state.char_col[row] == match_char) {
            sel.indices[matches++] = idx;
        }
    }
    sel.count = matches;
}

inline void refine_filter_string(size_t batch_offset,
                                 std::string_view prefix,
                                 SelectionVector& sel) noexcept {
    if (prefix.empty() || sel.empty()) return;

    const char* pool = g_state.string_pool_base;
    const uint64_t active_pool_bytes = g_state.string_pool_offset;

    if (pool == nullptr || active_pool_bytes == 0uz) {
        sel.clear();
        return;
    }

    uint16_t matches = 0;
    const auto active_span = sel.active();

    for (size_t i = 0uz; i < active_span.size(); ++i) {
        const uint16_t idx = active_span[i];
        const size_t row = batch_offset + idx;

        if (!is_row_live(row) || !is_row_valid(g_state.str_validity, row)) {
            continue;
        }

        const StringRef& ref = g_state.str_col[row];
        if (static_cast<uint64_t>(ref.offset) + ref.length <= active_pool_bytes) {
            std::string_view val_view{pool + ref.offset, ref.length};
            if (val_view.starts_with(prefix)) {
                sel.indices[matches++] = idx;
            }
        }
    }
    sel.count = matches;
}

} // namespace engine
