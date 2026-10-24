#include "storage/schema.hpp"
#include "execution/pipeline.hpp"
#include "common/exit_codes.hpp"
#include "common/hardware_config.hpp"

#include <iostream>
#include <vector>
#include <cassert>

using namespace engine;

int main() {
    std::cout << "[TEST] Initializing in-memory mutable GlobalExecutionState...\n";

    constexpr uint64_t CAPACITY = 4096uz;
    constexpr uint64_t VALIDITY_BYTES = calculate_validity_bytes(CAPACITY);

    std::vector<int64_t>   int_col(CAPACITY, 0);
    std::vector<char>      char_col(CAPACITY, 0);
    std::vector<StringRef> str_col(CAPACITY, StringRef{0, 0});
    std::vector<int64_t>   metric_col(CAPACITY, 0);

    std::vector<uint8_t>   int_val(VALIDITY_BYTES, 0);
    std::vector<uint8_t>   char_val(VALIDITY_BYTES, 0);
    std::vector<uint8_t>   str_val(VALIDITY_BYTES, 0);
    std::vector<uint8_t>   metric_val(VALIDITY_BYTES, 0);
    std::vector<uint8_t>   live_mask(VALIDITY_BYTES, 0);

    std::vector<char>      string_pool(65536, '\0');

    // Bind mutable state
    g_state.reset();
    g_state.capacity = CAPACITY;
    g_state.is_read_only = false;

    g_state.int_col = int_col.data();
    g_state.char_col = char_col.data();
    g_state.str_col = str_col.data();
    g_state.metric_col = metric_col.data();

    g_state.int_validity = int_val.data();
    g_state.char_validity = char_val.data();
    g_state.str_validity = str_val.data();
    g_state.metric_validity = metric_val.data();
    g_state.live_mask = live_mask.data();

    g_state.string_pool_base = string_pool.data();
    g_state.string_pool_offset = 0;
    g_state.string_pool_capacity = string_pool.size();

    // 1. Test append_row
    std::cout << "[TEST 1] Appending 3 rows...\n";
    assert(append_row(100, true, 'A', true, "APAC_1", true, 50, true));
    assert(append_row(200, true, 'B', true, "EMEA_1", true, 75, true));
    assert(append_row(300, true, 'A', true, "APAC_2", true, 25, true));
    assert(g_state.row_count == 3);
    assert(g_state.tail_idx == 3);
    assert(is_row_live(0) && is_row_live(1) && is_row_live(2));

    // 2. Query before soft-delete
    ExecutionPlan plan{
        .predicate = {.int_min = 0, .int_max = 500},
        .group_strategy = GroupingStrategy::DIRECT_CHAR,
        .group_by_char = true,
        .order_by = {.target = OrderTarget::AGG_RESULT, .direction = SortDirection::DESCENDING}
    };
    std::vector<OutputRow> results;
    assert(ExecutionPipeline::execute(plan, results) == EngineExitCode::SUCCESS);
    assert(results.size() == 2);
    std::cout << "[PASS] Append and baseline aggregation verified.\n";

    // 3. Test delete_row (soft delete row 0 -> 'A')
    std::cout << "[TEST 2] Soft-deleting row 0 ('A', metric 50)...\n";
    delete_row(0);
    assert(!is_row_live(0));
    assert(g_state.row_count == 2);

    results.clear();
    assert(ExecutionPipeline::execute(plan, results) == EngineExitCode::SUCCESS);
    assert(results[0].key_hash == static_cast<uint64_t>('B') && results[0].agg_result == 75);
    assert(results[1].key_hash == static_cast<uint64_t>('A') && results[1].agg_result == 25);
    std::cout << "[PASS] Soft-delete bitmask exclusion verified.\n";

    // 4. Test global_shift_window
    std::cout << "[TEST 3] Window shifting head by 2 rows...\n";
    global_shift_window(2);
    assert(g_state.head_idx == 2);
    assert(!is_row_live(0) && !is_row_live(1));
    assert(is_row_live(2));

    results.clear();
    assert(ExecutionPipeline::execute(plan, results) == EngineExitCode::SUCCESS);
    assert(results.size() == 1);
    assert(results[0].key_hash == static_cast<uint64_t>('A') && results[0].agg_result == 25);
    std::cout << "[PASS] Sliding window head boundary verified.\n";

    std::cout << "[ALL MUTATION TESTS PASSED]\n";
    return 0;
}
