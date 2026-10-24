#pragma once

#include "storage/schema.hpp"
#include <algorithm>
#include <vector>
#include <cstdint>

namespace engine {

enum class OrderTarget : uint8_t {
    KEY_HASH,
    AGG_RESULT
};

enum class SortDirection : uint8_t {
    ASCENDING,
    DESCENDING
};

struct OrderByClause {
    OrderTarget   target    = OrderTarget::AGG_RESULT;
    SortDirection direction = SortDirection::DESCENDING;
};

[[nodiscard]] inline constexpr bool compare_rows(const OutputRow& a,
                                                 const OutputRow& b,
                                                 const OrderByClause& clause) noexcept {
    if (clause.target == OrderTarget::AGG_RESULT) {
        if (a.agg_result != b.agg_result) {
            return (clause.direction == SortDirection::ASCENDING)
                ? (a.agg_result < b.agg_result)
                : (a.agg_result > b.agg_result);
        }
        return a.key_hash < b.key_hash;
    }

    if (a.key_hash != b.key_hash) {
        return (clause.direction == SortDirection::ASCENDING)
            ? (a.key_hash < b.key_hash)
            : (a.key_hash > b.key_hash);
    }
    return a.agg_result < b.agg_result;
}

inline void sort_records(std::vector<OutputRow>& records, const OrderByClause& clause) noexcept {
    if (records.size() <= 1uz) return;

    std::ranges::sort(records, [&](const OutputRow& a, const OutputRow& b) noexcept {
        return compare_rows(a, b, clause);
    });
}

} // namespace engine
