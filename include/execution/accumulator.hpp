#pragma once

#include "storage/schema.hpp"
#include "vector_chunk.hpp"
#include <vector>
#include <cstdint>
#include <bit>
#include <cassert>
#include <string_view>
#include <algorithm>
#include <limits>
#include <span>

namespace engine {

constexpr uint64_t NULL_KEY_HASH_SALT = 0x9e3779b97f4a7c15uz;
constexpr uint64_t VALID_KEY_HASH_SALT = 0xbf58476d1ce4e5b9uz;

[[nodiscard]] inline constexpr uint64_t splitmix64(uint64_t z) noexcept {
    z = (z ^ (z >> 30uz)) * 0xbf58476d1ce4e5b9uz;
    z = (z ^ (z >> 27uz)) * 0x94d049bb133111ebuz;
    return z ^ (z >> 31uz);
}

[[nodiscard]] inline constexpr uint64_t hash_bytes(std::string_view bytes) noexcept {
    uint64_t h = 0xcbf29ce484222325uz;
    for (char c : bytes) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001b3uz;
    }
    return splitmix64(h);
}

inline void hash_mix_int_col(HashVector& hv, const SelectionVector& sel, size_t batch_offset) noexcept {
    const auto active_span = sel.active();
    for (size_t i = 0uz; i < active_span.size(); ++i) {
        const size_t row = batch_offset + active_span[i];
        const bool valid = is_row_valid(g_state.int_validity, row);
        
        const uint64_t val = valid 
            ? splitmix64(static_cast<uint64_t>(g_state.int_col[row]) ^ VALID_KEY_HASH_SALT)
            : splitmix64(NULL_KEY_HASH_SALT);

        hv[i] = std::rotl(hv[i] ^ val, 17);
    }
}

inline void hash_mix_char_col(HashVector& hv, const SelectionVector& sel, size_t batch_offset) noexcept {
    const auto active_span = sel.active();
    for (size_t i = 0uz; i < active_span.size(); ++i) {
        const size_t row = batch_offset + active_span[i];
        const bool valid = is_row_valid(g_state.char_validity, row);

        const uint64_t val = valid 
            ? splitmix64(static_cast<uint8_t>(g_state.char_col[row]) ^ VALID_KEY_HASH_SALT)
            : splitmix64(NULL_KEY_HASH_SALT);

        hv[i] = std::rotl(hv[i] ^ val, 11);
    }
}

inline void hash_mix_str_col(HashVector& hv, const SelectionVector& sel, size_t batch_offset) noexcept {
    const char* pool = g_state.string_pool_base;
    const uint64_t active_pool_bytes = g_state.string_pool_offset;
    const auto active_span = sel.active();

    for (size_t i = 0uz; i < active_span.size(); ++i) {
        const size_t row = batch_offset + active_span[i];
        uint64_t val = splitmix64(NULL_KEY_HASH_SALT);

        if (pool != nullptr && is_row_valid(g_state.str_validity, row)) {
            const StringRef& ref = g_state.str_col[row];
            if (static_cast<uint64_t>(ref.offset) + ref.length <= active_pool_bytes) {
                val = hash_bytes({pool + ref.offset, ref.length});
            }
        }
        hv[i] = std::rotl(hv[i] ^ val, 23);
    }
}

struct GroupState {
    int64_t  sum_metric = 0;
    uint64_t count = 0;
    bool     overflowed = false;
};

class DirectCharAccumulator {
private:
    GroupState slots_[257]{};

public:
    inline void update_batch(const SelectionVector& sel, size_t batch_offset) noexcept {
        for (uint16_t idx_val : sel.active()) {
            const size_t row = batch_offset + idx_val;
            if (!is_row_live(row)) continue;

            const bool char_valid   = is_row_valid(g_state.char_validity, row);
            const bool metric_valid = is_row_valid(g_state.metric_validity, row);
            const size_t idx = char_valid ? static_cast<size_t>(static_cast<uint8_t>(g_state.char_col[row])) : 256uz;

            if (metric_valid && !slots_[idx].overflowed) {
                const auto base = slots_[idx].sum_metric;
                const auto operand = g_state.metric_col[row];
                if ((operand > 0 && base > std::numeric_limits<int64_t>::max() - operand) ||
                    (operand < 0 && base < std::numeric_limits<int64_t>::min() - operand)) [[unlikely]] {
                    slots_[idx].overflowed = true;
                } else {
                    slots_[idx].sum_metric = base + operand;
                }
            }
            slots_[idx].count += 1uz;
        }
    }

    template <typename Self, typename Callback>
    void iterate(this Self&& self, Callback&& cb) {
        for (size_t i = 0uz; i < 257uz; ++i) {
            if (self.slots_[i].count > 0uz) {
                cb(i, self.slots_[i]);
            }
        }
    }
};

struct HashSlot {
    uint64_t   key_hash = 0;
    GroupState state{};
};
static_assert(sizeof(HashSlot) == 32uz, "HashSlot ABI corrupted");

class VectorizedHashTable {
private:
    std::vector<HashSlot> slots_;
    size_t capacity_;
    size_t count_;

public:
    explicit VectorizedHashTable(size_t initial_capacity = 1048576uz)
        : capacity_(std::max<size_t>(16uz, std::bit_ceil(initial_capacity))),
          count_(0uz) {
        slots_.resize(capacity_);
    }

    void resize(size_t new_capacity) {
        const size_t target_cap = std::bit_ceil(new_capacity);
        std::vector<HashSlot> new_slots(target_cap);
        const size_t mask = target_cap - 1uz;

        for (const auto& slot : slots_) {
            if (slot.state.count > 0uz) {
                size_t idx = slot.key_hash & mask;
                while (new_slots[idx].state.count > 0uz) {
                    idx = (idx + 1uz) & mask;
                }
                new_slots[idx] = slot;
            }
        }

        slots_ = std::move(new_slots);
        capacity_ = target_cap;
    }

    inline void update_batch(const SelectionVector& sel, const HashVector& hv, size_t batch_offset) {
        if ((count_ + sel.count) * 2uz >= capacity_) {
            resize(capacity_ * 2uz);
        }

        const size_t mask = capacity_ - 1uz;

        for (size_t i = 0uz; i < sel.count; ++i) {
            const size_t row = batch_offset + sel[i];
            if (!is_row_live(row)) continue;

            const bool metric_valid = is_row_valid(g_state.metric_validity, row);
            const int64_t metric = metric_valid ? g_state.metric_col[row] : 0;

            const uint64_t h = splitmix64(hv[i]);
            size_t idx = h & mask;

            while (slots_[idx].state.count > 0uz) {
                if (slots_[idx].key_hash == h) {
                    if (metric_valid && !slots_[idx].state.overflowed) {
                        const auto base = slots_[idx].state.sum_metric;
                        if ((metric > 0 && base > std::numeric_limits<int64_t>::max() - metric) ||
                            (metric < 0 && base < std::numeric_limits<int64_t>::min() - metric)) [[unlikely]] {
                            slots_[idx].state.overflowed = true;
                        } else {
                            slots_[idx].state.sum_metric = base + metric;
                        }
                    }
                    slots_[idx].state.count += 1uz;
                    goto next_key;
                }
                idx = (idx + 1uz) & mask;
            }

            slots_[idx].key_hash = h;
            slots_[idx].state.sum_metric = metric;
            slots_[idx].state.count = 1uz;
            slots_[idx].state.overflowed = false;
            ++count_;

        next_key:;
        }
    }

    template <typename Self, typename Callback>
    void iterate(this Self&& self, Callback&& cb) {
        for (const auto& slot : self.slots_) {
            if (slot.state.count > 0uz) {
                cb(slot.key_hash, slot.state);
            }
        }
    }
};

} // namespace engine
