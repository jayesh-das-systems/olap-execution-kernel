#include "include/common/exit_codes.hpp"
#include "include/common/hardware_config.hpp"
#include "include/storage/schema.hpp"
#include "include/storage/file_format.hpp"
#include "include/execution/pipeline.hpp"
#include "include/output/tsv_emitter.hpp"

#include <iostream>
#include <string_view>
#include <vector>
#include <charconv>
#include <chrono>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <path_to_data.bin> [options]\n"
              << "Options:\n"
              << "  --min-int <val>        Lower bound filter for int column\n"
              << "  --max-int <val>        Upper bound filter for int column\n"
              << "  --char <c>             Exact match filter for char column\n"
              << "  --str-prefix <str>     Prefix filter for string column\n"
              << "  --group-by <type>      Grouping mode: 'char' (default), 'int', 'str', 'all'\n"
              << "  --order-by <target>    Sort target: 'result' (default) or 'key'\n"
              << "  --asc                  Sort ascending (default is descending)\n"
              << "  --benchmark            Print elapsed nanoseconds and throughput to stderr\n";
}

template <typename T>
bool parse_int_arg(std::string_view arg, T& out_val) {
    auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), out_val);
    return ec == std::errc{} && ptr == arg.data() + arg.size();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return engine::to_underlying(engine::EngineExitCode::ERR_CLI_MISSING_FILE);
    }

    const char* file_path = argv[1];
    engine::ExecutionPlan plan{};
    bool benchmark_mode = false;

    // CLI Flag Parsing with individual fault detection
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--min-int") {
            if (i + 1 >= argc || !parse_int_arg(argv[++i], plan.predicate.int_min)) {
                std::cerr << "[CLI ERROR] Malformed or missing integer argument for --min-int.\n";
                return engine::to_underlying(engine::EngineExitCode::ERR_CLI_INTEGER_PARSE_FAIL);
            }
        } else if (arg == "--max-int") {
            if (i + 1 >= argc || !parse_int_arg(argv[++i], plan.predicate.int_max)) {
                std::cerr << "[CLI ERROR] Malformed or missing integer argument for --max-int.\n";
                return engine::to_underlying(engine::EngineExitCode::ERR_CLI_INTEGER_PARSE_FAIL);
            }
        } else if (arg == "--char") {
            if (i + 1 >= argc) {
                std::cerr << "[CLI ERROR] Missing character argument for --char.\n";
                return engine::to_underlying(engine::EngineExitCode::ERR_CLI_INVALID_ARGS);
            }
            plan.predicate.has_char_filter = true;
            plan.predicate.char_match = argv[++i][0];
        } else if (arg == "--str-prefix") {
            if (i + 1 >= argc) {
                std::cerr << "[CLI ERROR] Missing string argument for --str-prefix.\n";
                return engine::to_underlying(engine::EngineExitCode::ERR_CLI_INVALID_ARGS);
            }
            plan.predicate.str_prefix = argv[++i];
        } else if (arg == "--group-by") {
            if (i + 1 >= argc) {
                std::cerr << "[CLI ERROR] Missing value for --group-by.\n";
                return engine::to_underlying(engine::EngineExitCode::ERR_CLI_INVALID_ARGS);
            }
            std::string_view mode = argv[++i];
            if (mode == "char") {
                plan.group_strategy = engine::GroupingStrategy::DIRECT_CHAR;
                plan.group_by_char = true;
                plan.group_by_int = false;
                plan.group_by_str = false;
            } else if (mode == "int") {
                plan.group_strategy = engine::GroupingStrategy::HASH_GENERAL;
                plan.group_by_int = true;
                plan.group_by_char = false;
                plan.group_by_str = false;
            } else if (mode == "str") {
                plan.group_strategy = engine::GroupingStrategy::HASH_GENERAL;
                plan.group_by_str = true;
                plan.group_by_char = false;
                plan.group_by_int = false;
            } else if (mode == "all") {
                plan.group_strategy = engine::GroupingStrategy::HASH_GENERAL;
                plan.group_by_int = true;
                plan.group_by_char = true;
                plan.group_by_str = true;
            } else {
                std::cerr << "[CLI ERROR] Unknown grouping mode: '" << mode << "'.\n";
                return engine::to_underlying(engine::EngineExitCode::ERR_CLI_INVALID_QUERY);
            }
        } else if (arg == "--order-by") {
            if (i + 1 >= argc) {
                std::cerr << "[CLI ERROR] Missing value for --order-by.\n";
                return engine::to_underlying(engine::EngineExitCode::ERR_CLI_INVALID_ARGS);
            }
            std::string_view target = argv[++i];
            if (target == "key") {
                plan.order_by.target = engine::OrderTarget::KEY_HASH;
            } else if (target == "result") {
                plan.order_by.target = engine::OrderTarget::AGG_RESULT;
            } else {
                std::cerr << "[CLI ERROR] Unknown sort target: '" << target << "'.\n";
                return engine::to_underlying(engine::EngineExitCode::ERR_CLI_INVALID_ARGS);
            }
        } else if (arg == "--asc") {
            plan.order_by.direction = engine::SortDirection::ASCENDING;
        } else if (arg == "--benchmark") {
            benchmark_mode = true;
        } else {
            std::cerr << "[CLI ERROR] Unrecognized flag: '" << arg << "'.\n";
            print_usage(argv[0]);
            return engine::to_underlying(engine::EngineExitCode::ERR_CLI_UNRECOGNIZED_FLAG);
        }
    }

    // Zero-Copy Mmap Open
    engine::MappedColumnarFile mapped_file;
    const engine::EngineExitCode open_status = mapped_file.open_read(file_path);
    if (open_status != engine::EngineExitCode::SUCCESS) {
        std::cerr << "[FATAL] Failed to map data file. Exit reason: " 
                  << engine::exit_code_to_string(open_status) << "\n";
        return engine::to_underlying(open_status);
    }

    std::vector<engine::OutputRow> results;
    results.reserve(1024uz);

    const auto start_ts = std::chrono::steady_clock::now();

    // Run Analytical Pipeline
    const engine::EngineExitCode exec_status = engine::ExecutionPipeline::execute(plan, results);
    if (exec_status != engine::EngineExitCode::SUCCESS) {
        std::cerr << "[FATAL] Query pipeline failed during execution. Exit reason: " 
                  << engine::exit_code_to_string(exec_status) << "\n";
        return engine::to_underlying(exec_status);
    }

    const auto end_ts = std::chrono::steady_clock::now();

    // Stream Output
    engine::TsvEmitter emitter;
    const engine::EngineExitCode emit_status = emitter.emit(results);
    if (emit_status != engine::EngineExitCode::SUCCESS) {
        std::cerr << "[FATAL] TSV streaming failed. Exit reason: " 
                  << engine::exit_code_to_string(emit_status) << "\n";
        return engine::to_underlying(emit_status);
    }

    if (benchmark_mode) {
        const auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_ts - start_ts).count();
        const double duration_sec = static_cast<double>(duration_ns) / 1e9;
        const uint64_t total_rows = engine::g_state.row_count;
        const double throughput = (duration_sec > 0.0) ? (static_cast<double>(total_rows) / duration_sec / 1e6) : 0.0;

        std::cerr << "[BENCHMARK]\n"
                  << "  Rows processed: " << total_rows << "\n"
                  << "  Latency:        " << (duration_ns / 1000) << " us (" << duration_ns << " ns)\n"
                  << "  Throughput:     " << throughput << " M rows/sec\n";
    }

    return engine::to_underlying(engine::EngineExitCode::SUCCESS);
}
