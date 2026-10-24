#pragma once

#if !defined(__cplusplus) || __cplusplus < 202302L
#error "FATAL: This engine strictly requires an ISO C++23 compliant compiler."
#endif

#include "exit_codes.hpp"
#include "hardware_config.hpp"
#include "schema.hpp"
#include "vector_chunk.hpp"
#include "expression.hpp"
#include "accumulator.hpp"
#include "orderby.hpp"

#include <vector>
#include <string_view>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <exception>

namespace engine {

struct QueryPredicate {
    int64_t          int_min = INT64_MIN;
    int64_t          int_max = INT64_MAX;
    bool             has_char_filter = false;
    char             char_match = '\0';
    std::string_view str_prefix{};
};

enum class GroupingStrategy : uint8_t {
    DIRECT_CHAR,
    HASH_GENERAL
};

struct ExecutionPlan {
    QueryPredicate   predicate{};
    GroupingStrategy group_strategy = GroupingStrategy::DIRECT_CHAR;
    bool             group_by_int = false;
    bool             group_by_char = true;
    bool             group_by_str = false;
    OrderByClause    order_by{};
};

class ExecutionPipeline {
public:
    [[nodiscard]] static EngineExitCode execute(const ExecutionPlan& plan,
                                                std::vector<OutputRow>& out_results) noexcept {
        out_results.clear();

        if (g_state.row_count == 0uz) {
            return EngineExitCode::SUCCESS;
        }

        try {
            if (plan.group_strategy == GroupingStrategy::DIRECT_CHAR &&
                !plan.group_by_int && plan.group_by_char && !plan.group_by_str) {
                return execute_direct_char(plan, out_results);
            }
            return execute_vectorized_hash(plan, out_results);
        } catch (const std::bad_alloc& e) {
            std::cerr << "[PIPELINE OOM] Memory allocation failed: " << e.what() << "\n";
            return EngineExitCode::ERR_OOM;
        } catch (const std::exception& e) {
            std::cerr << "[PIPELINE EXCEPTION] Runtime failure: " << e.what() << "\n";
            return EngineExitCode::ERR_EXEC_CORRUPT_STATE;
        } catch (...) {
            std::cerr << "[PIPELINE PANIC] Unhandled non-standard exception trapped.\n";
            return EngineExitCode::ERR_INTERNAL_PANIC;
        }
    }

private:
    [[nodiscard]] static EngineExitCode execute_direct_char(const ExecutionPlan& plan,
                                                            std::vector<OutputRow>& out_results) noexcept {
        DirectCharAccumulator accum;
        SelectionVector sel;

        const size_t total_rows = g_state.tail_idx;
        for (size_t offset = g_state.head_idx; offset < total_rows; offset += VECTOR_SIZE) {
            const size_t batch_size = std::min(VECTOR_SIZE, total_rows - offset);

            filter_int_batch(offset, batch_size, plan.predicate.int_min, plan.predicate.int_max, sel);
            if (sel.empty()) continue;

            if (plan.predicate.has_char_filter) {
                refine_filter_char(offset, plan.predicate.char_match, sel);
                if (sel.empty()) continue;
            }

            if (!plan.predicate.str_prefix.empty()) {
                refine_filter_string(offset, plan.predicate.str_prefix, sel);
                if (sel.empty()) continue;
            }

            accum.update_batch(sel, offset);
        }

        bool had_overflow = false;
        accum.iterate([&](size_t char_code, const GroupState& st) {
            if (st.overflowed) had_overflow = true;
            out_results.push_back(OutputRow{
                .key_hash = static_cast<uint64_t>(char_code),
                .agg_result = st.sum_metric,
                .agg_count = st.count
            });
        });

        if (had_overflow) {
            std::cerr << "[MATH ERROR] Integer arithmetic overflow occurred during direct char aggregation.\n";
            return EngineExitCode::ERR_EXEC_OVERFLOW;
        }

        sort_records(out_results, plan.order_by);
        return EngineExitCode::SUCCESS;
    }

    [[nodiscard]] static EngineExitCode execute_vectorized_hash(const ExecutionPlan& plan,
                                                               std::vector<OutputRow>& out_results) {
        VectorizedHashTable ht(1048576uz);
        SelectionVector sel;
        HashVector hv;

        const size_t total_rows = g_state.tail_idx;
        for (size_t offset = g_state.head_idx; offset < total_rows; offset += VECTOR_SIZE) {
            const size_t batch_size = std::min(VECTOR_SIZE, total_rows - offset);

            filter_int_batch(offset, batch_size, plan.predicate.int_min, plan.predicate.int_max, sel);
            if (sel.empty()) continue;

            if (plan.predicate.has_char_filter) {
                refine_filter_char(offset, plan.predicate.char_match, sel);
                if (sel.empty()) continue;
            }

            if (!plan.predicate.str_prefix.empty()) {
                refine_filter_string(offset, plan.predicate.str_prefix, sel);
                if (sel.empty()) continue;
            }

            hv.initialize(sel.count);
            if (plan.group_by_int)  hash_mix_int_col(hv, sel, offset);
            if (plan.group_by_char) hash_mix_char_col(hv, sel, offset);
            if (plan.group_by_str)  hash_mix_str_col(hv, sel, offset);

            ht.update_batch(sel, hv, offset);
        }

        bool had_overflow = false;
        ht.iterate([&](uint64_t key_hash, const GroupState& st) {
            if (st.overflowed) had_overflow = true;
            out_results.push_back(OutputRow{
                .key_hash = key_hash,
                .agg_result = st.sum_metric,
                .agg_count = st.count
            });
        });

        if (had_overflow) {
            std::cerr << "[MATH ERROR] Integer arithmetic overflow occurred during hash aggregation.\n";
            return EngineExitCode::ERR_EXEC_OVERFLOW;
        }

        sort_records(out_results, plan.order_by);
        return EngineExitCode::SUCCESS;
    }
};

} // namespace engine
