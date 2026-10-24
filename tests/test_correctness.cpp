#include "common/exit_codes.hpp"
#include "common/hardware_config.hpp"
#include "storage/schema.hpp"
#include "execution/vector_chunk.hpp"
#include "execution/expression.hpp"
#include "execution/accumulator.hpp"
#include "execution/pipeline.hpp"

#include <iostream>
#include <vector>
#include <cstdint>

namespace {

bool verify_abi_layouts() {
    bool ok = true;
    if (sizeof(engine::ColumnarFileHeader) != 128uz) {
        std::cerr << "  [ABI MISMATCH] ColumnarFileHeader: expected 128 bytes, got " 
                  << sizeof(engine::ColumnarFileHeader) << "\n";
        ok = false;
    }
    if (alignof(engine::ColumnarFileHeader) != 64uz) {
        std::cerr << "  [ABI MISMATCH] ColumnarFileHeader align: expected 64, got " 
                  << alignof(engine::ColumnarFileHeader) << "\n";
        ok = false;
    }
    if (sizeof(engine::OutputRow) != 24uz) {
        std::cerr << "  [ABI MISMATCH] OutputRow: expected 24 bytes, got " 
                  << sizeof(engine::OutputRow) << "\n";
        ok = false;
    }
    if (sizeof(engine::StringRef) != 8uz) {
        std::cerr << "  [ABI MISMATCH] StringRef: expected 8 bytes, got " 
                  << sizeof(engine::StringRef) << "\n";
        ok = false;
    }
    if (sizeof(engine::HashSlot) != 32uz) {
        std::cerr << "  [ABI MISMATCH] HashSlot: expected 32 bytes, got " 
                  << sizeof(engine::HashSlot) << "\n";
        ok = false;
    }
    return ok;
}

bool test_bitmask_operations() {
    alignas(64) uint8_t mask[64]{};

    engine::set_validity_bit(mask, 0uz, true);
    engine::set_validity_bit(mask, 7uz, true);
    engine::set_validity_bit(mask, 8uz, true);

    if (!engine::is_row_valid(mask, 0uz)) {
        std::cerr << "  [BITMASK FAULT] Row 0 should be true.\n";
        return false;
    }
    if (!engine::is_row_valid(mask, 7uz)) {
        std::cerr << "  [BITMASK FAULT] Row 7 should be true.\n";
        return false;
    }
    if (!engine::is_row_valid(mask, 8uz)) {
        std::cerr << "  [BITMASK FAULT] Row 8 should be true.\n";
        return false;
    }
    if (engine::is_row_valid(mask, 1uz)) {
        std::cerr << "  [BITMASK FAULT] Row 1 should be false.\n";
        return false;
    }

    engine::set_validity_bit(mask, 7uz, false);
    if (engine::is_row_valid(mask, 7uz)) {
        std::cerr << "  [BITMASK FAULT] Row 7 toggle-off failed.\n";
        return false;
    }

    return true;
}

bool test_synthetic_pipeline_execution() {
    constexpr size_t TEST_ROWS = 2048uz;
    constexpr uint64_t val_bytes = engine::calculate_validity_bytes(TEST_ROWS);

    std::vector<int64_t> int_col(TEST_ROWS);
    std::vector<char> char_col(TEST_ROWS);
    std::vector<engine::StringRef> str_col(TEST_ROWS);
    std::vector<int64_t> metric_col(TEST_ROWS);

    std::vector<uint8_t> int_val(val_bytes, 0xFF);
    std::vector<uint8_t> char_val(val_bytes, 0xFF);
    std::vector<uint8_t> str_val(val_bytes, 0xFF);
    std::vector<uint8_t> metric_val(val_bytes, 0xFF);
    std::vector<uint8_t> live(val_bytes, 0xFF);

    int64_t expected_sum_a = 0;
    int64_t expected_sum_b = 0;

    for (size_t i = 0; i < TEST_ROWS; ++i) {
        int_col[i] = static_cast<int64_t>(i);
        if (i % 2 == 0) {
            char_col[i] = 'A';
            metric_col[i] = 10;
            expected_sum_a += 10;
        } else {
            char_col[i] = 'B';
            metric_col[i] = 20;
            expected_sum_b += 20;
        }
    }

    engine::g_state.reset();
    engine::g_state.row_count = TEST_ROWS;
    engine::g_state.head_idx = 0uz;
    engine::g_state.tail_idx = TEST_ROWS;
    engine::g_state.capacity = TEST_ROWS;

    engine::g_state.int_col = int_col.data();
    engine::g_state.char_col = char_col.data();
    engine::g_state.str_col = str_col.data();
    engine::g_state.metric_col = metric_col.data();

    engine::g_state.int_validity = int_val.data();
    engine::g_state.char_validity = char_val.data();
    engine::g_state.str_validity = str_val.data();
    engine::g_state.metric_validity = metric_val.data();
    engine::g_state.live_mask = live.data();

    engine::ExecutionPlan plan{};
    plan.group_strategy = engine::GroupingStrategy::DIRECT_CHAR;
    plan.group_by_char = true;

    std::vector<engine::OutputRow> results;
    const auto status = engine::ExecutionPipeline::execute(plan, results);

    if (status != engine::EngineExitCode::SUCCESS) {
        std::cerr << "  [PIPELINE FAULT] Execution failed with code: " 
                  << engine::exit_code_to_string(status) << "\n";
        return false;
    }

    if (results.size() != 2uz) {
        std::cerr << "  [MATH FAULT] Expected 2 group results, got " << results.size() << "\n";
        return false;
    }

    bool found_a = false, found_b = false;
    for (const auto& r : results) {
        if (r.key_hash == static_cast<uint64_t>('A')) {
            if (r.agg_result != expected_sum_a || r.agg_count != TEST_ROWS / 2uz) {
                std::cerr << "  [MATH FAULT] Group A mismatch: sum=" << r.agg_result 
                          << " (exp " << expected_sum_a << "), count=" << r.agg_count 
                          << " (exp " << (TEST_ROWS / 2uz) << ")\n";
                return false;
            }
            found_a = true;
        } else if (r.key_hash == static_cast<uint64_t>('B')) {
            if (r.agg_result != expected_sum_b || r.agg_count != TEST_ROWS / 2uz) {
                std::cerr << "  [MATH FAULT] Group B mismatch: sum=" << r.agg_result 
                          << " (exp " << expected_sum_b << "), count=" << r.agg_count 
                          << " (exp " << (TEST_ROWS / 2uz) << ")\n";
                return false;
            }
            found_b = true;
        }
    }

    engine::g_state.reset();
    return found_a && found_b;
}

} // namespace

int main() {
    std::cout << "[TIER 2 CHECK] Verifying schema ABI struct layouts...\n";
    if (!verify_abi_layouts()) {
        std::cerr << "[GATE FAULT] Struct ABI or cache alignment invariant violated.\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_SYS_HARDWARE_INVARIANT);
    }

    std::cout << "[TIER 2 CHECK] Verifying bitmask operations...\n";
    if (!test_bitmask_operations()) {
        std::cerr << "[GATE FAULT] Bitmask logic failed validation.\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_EXEC_CORRUPT_STATE);
    }

    std::cout << "[TIER 2 CHECK] Verifying 2-batch synthetic pipeline arithmetic...\n";
    if (!test_synthetic_pipeline_execution()) {
        std::cerr << "[GATE FAULT] Pipeline vector arithmetic produced invalid results.\n";
        return engine::to_underlying(engine::EngineExitCode::ERR_EXEC_CORRUPT_STATE);
    }

    std::cout << "[GATE PASS] Tier 2: Schema ABI, bitmasks, and pipeline reduction math verified.\n";
    return engine::to_underlying(engine::EngineExitCode::SUCCESS);
}
