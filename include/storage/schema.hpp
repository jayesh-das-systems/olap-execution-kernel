#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <memory>
#include <string_view>

namespace engine {

enum class ColumnType : uint8_t {
    INT64  = 1,
    CHAR   = 2,
    STRING = 3
};

struct StringRef {
    uint32_t offset;
    uint32_t length;

    constexpr bool operator==(const StringRef& other) const noexcept = default;

    [[nodiscard]] constexpr std::string_view view(const char* pool_base) const noexcept {
        if (pool_base == nullptr || length == 0u) return {};
        return {pool_base + offset, length};
    }
};

struct alignas(64) ColumnarFileHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t row_count;
    
    uint64_t int_col_offset;
    uint64_t char_col_offset;
    uint64_t str_col_offset;
    uint64_t metric_col_offset;
    uint64_t string_pool_offset;
    uint64_t string_pool_size;

    uint64_t int_validity_offset;
    uint64_t char_validity_offset;
    uint64_t str_validity_offset;
    uint64_t metric_validity_offset;
    uint64_t live_mask_offset;
    uint64_t reserved[3];
};

struct OutputRow {
    uint64_t key_hash;
    int64_t  agg_result;
    uint64_t agg_count;

    constexpr auto operator<=>(const OutputRow&) const noexcept = default;
};

static_assert(sizeof(StringRef) == 8uz, "StringRef ABI broken");
static_assert(alignof(StringRef) == 4uz, "StringRef alignment broken");
static_assert(sizeof(ColumnarFileHeader) == 128uz, "Header must be cache-aligned 128 bytes");
static_assert(alignof(ColumnarFileHeader) == 64uz, "Header must maintain 64-byte alignment");
static_assert(sizeof(OutputRow) == 24uz, "OutputRow size mismatch");
static_assert(alignof(OutputRow) == 8uz, "OutputRow must have natural 8-byte alignment");

[[nodiscard]] inline constexpr uint64_t calculate_validity_bytes(uint64_t row_count) noexcept {
    return ((row_count + 511uz) / 512uz) * 64uz;
}

[[nodiscard]] inline constexpr bool is_row_valid(const uint8_t* __restrict__ validity_mask, uint64_t row_idx) noexcept {
    if (validity_mask == nullptr) return false;
    return (validity_mask[row_idx >> 3uz] >> (row_idx & 7uz)) & 1uz;
}

inline constexpr void set_validity_bit(uint8_t* __restrict__ mask, uint64_t row_idx, bool valid) noexcept {
    if (mask == nullptr) return;
    const uint64_t byte_idx = row_idx >> 3uz;
    const uint8_t  bit_mask = static_cast<uint8_t>(1u << (row_idx & 7uz));
    if (valid) {
        mask[byte_idx] |= bit_mask;
    } else {
        mask[byte_idx] &= static_cast<uint8_t>(~bit_mask);
    }
}

struct alignas(64) GlobalExecutionState {
    uint64_t row_count = 0;
    uint64_t head_idx = 0;
    uint64_t tail_idx = 0;
    uint64_t capacity = 0;
    bool     is_read_only = false;

    int64_t*   int_col = nullptr;
    char*      char_col = nullptr;
    StringRef* str_col = nullptr;
    int64_t*   metric_col = nullptr;

    uint8_t*   int_validity = nullptr;
    uint8_t*   char_validity = nullptr;
    uint8_t*   str_validity = nullptr;
    uint8_t*   metric_validity = nullptr;
    uint8_t*   live_mask = nullptr;

    char*      string_pool_base = nullptr;
    uint64_t   string_pool_offset = 0;
    uint64_t   string_pool_capacity = 0;

    void reset() noexcept {
        row_count = 0;
        head_idx = 0;
        tail_idx = 0;
        capacity = 0;
        is_read_only = false;

        int_col = nullptr;
        char_col = nullptr;
        str_col = nullptr;
        metric_col = nullptr;

        int_validity = nullptr;
        char_validity = nullptr;
        str_validity = nullptr;
        metric_validity = nullptr;
        live_mask = nullptr;

        string_pool_base = nullptr;
        string_pool_offset = 0;
        string_pool_capacity = 0;
    }
};

inline GlobalExecutionState g_state{};

[[nodiscard]] inline constexpr bool is_row_live(uint64_t row) noexcept {
    if (g_state.live_mask == nullptr) return false;
    return (row >= g_state.head_idx) &&
           (row < g_state.tail_idx) &&
           is_row_valid(g_state.live_mask, row);
}

[[nodiscard]] inline bool global_pool_append(std::string_view str, StringRef& out_ref) noexcept {
    if (g_state.is_read_only || g_state.string_pool_base == nullptr) return false;
    if (g_state.string_pool_offset + str.size() > g_state.string_pool_capacity) return false;
    if (g_state.string_pool_offset > UINT32_MAX) return false;

    const auto offset = static_cast<uint32_t>(g_state.string_pool_offset);
    std::memcpy(g_state.string_pool_base + offset, str.data(), str.size());
    g_state.string_pool_offset += str.size();
    out_ref = StringRef{.offset = offset, .length = static_cast<uint32_t>(str.size())};
    return true;
}

inline bool append_row(int64_t int_val, bool int_valid,
                       char char_val, bool char_valid,
                       std::string_view str_val, bool str_valid,
                       int64_t metric_val, bool metric_valid) noexcept {
    if (g_state.is_read_only || g_state.tail_idx >= g_state.capacity) return false;
    if (g_state.live_mask == nullptr) return false;

    const uint64_t row = g_state.tail_idx;

    if (str_valid && !str_val.empty()) {
        StringRef ref;
        if (!global_pool_append(str_val, ref)) {
            return false;
        }
        g_state.str_col[row] = ref;
    } else {
        g_state.str_col[row] = StringRef{0, 0};
    }

    g_state.int_col[row]    = int_val;
    g_state.char_col[row]   = char_val;
    g_state.metric_col[row] = metric_val;

    set_validity_bit(g_state.int_validity, row, int_valid);
    set_validity_bit(g_state.char_validity, row, char_valid);
    set_validity_bit(g_state.str_validity, row, str_valid);
    set_validity_bit(g_state.metric_validity, row, metric_valid);
    set_validity_bit(g_state.live_mask, row, true);

    ++g_state.tail_idx;
    ++g_state.row_count;
    return true;
}

inline void delete_row(uint64_t row_idx) noexcept {
    if (g_state.is_read_only) return;
    if (row_idx >= g_state.tail_idx || row_idx < g_state.head_idx) return;
    if (g_state.live_mask == nullptr) return;

    if (is_row_valid(g_state.live_mask, row_idx)) {
        set_validity_bit(g_state.live_mask, row_idx, false);
        if (g_state.row_count > 0uz) {
            --g_state.row_count;
        }
    }
}

inline void global_shift_window(uint64_t count) noexcept {
    g_state.head_idx += count;
    if (g_state.head_idx > g_state.tail_idx) {
        g_state.head_idx = g_state.tail_idx;
    }
    g_state.row_count = (g_state.row_count >= count) ? (g_state.row_count - count) : 0uz;
}

} // namespace engine
