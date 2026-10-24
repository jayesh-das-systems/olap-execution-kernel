#pragma once

#if !defined(__cplusplus) || __cplusplus < 202302L
#error "FATAL: Strict ISO C++23 required. Refusing to compile under older standards."
#endif

#include <cstdint>
#include <string_view>

namespace engine {

// Deterministic error code boundaries strictly partitioned in the 10..99 range
// to avoid collisions with standard POSIX shell reserved exit codes (126, 127, 128+N).
enum class EngineExitCode : uint8_t {
    SUCCESS                           = 0,

    // CLI & Query Plan Invariants (10..19)
    ERR_CLI_INVALID_ARGS              = 10,
    ERR_CLI_MISSING_FILE              = 11,
    ERR_CLI_INVALID_QUERY             = 12,
    ERR_CLI_UNRECOGNIZED_FLAG         = 13,
    ERR_CLI_INTEGER_PARSE_FAIL        = 14,

    // Storage, Alignment & File Validation (20..39)
    ERR_IO_NOT_FOUND                  = 20,
    ERR_IO_READ_FAILED                = 21,
    ERR_FILE_TRUNCATED                = 22,
    ERR_MAGIC_MISMATCH                = 23,
    ERR_VERSION_UNSUPPORTED           = 24,
    ERR_ALIGNMENT_FAULT               = 25,
    ERR_OFFSET_OUT_OF_BOUNDS          = 26,
    ERR_STRING_POOL_OVERFLOW          = 27,
    ERR_FILE_EMPTY                    = 28,
    ERR_SCHEMA_CORRUPT_HEADER         = 29,

    // Virtual Memory & POSIX Subsystems (40..59)
    ERR_MMAP_FAILED                   = 40,
    ERR_SYS_HARDWARE_INVARIANT        = 41,
    ERR_OOM                           = 42,
    ERR_MADVISE_FAILED                = 43,
    ERR_MUNMAP_FAILED                 = 44,
    ERR_PAGE_SIZE_MISMATCH            = 45,

    // Query Execution, Grouping & Math (60..79)
    ERR_EXEC_LOAD_FACTOR_EXCEEDED     = 60,
    ERR_EXEC_OVERFLOW                 = 61,
    ERR_EXEC_CORRUPT_STATE            = 62,
    ERR_EXEC_DIVISION_BY_ZERO         = 63,
    ERR_EXEC_EMPTY_SELECTION          = 64,
    ERR_EXEC_TABLE_COLLISION_SATURATE = 65,

    // Post-Flight & Output Serialization (80..89)
    ERR_OUTPUT_WRITE_FAILED           = 80,
    ERR_OUTPUT_BUFFER_OVERRUN         = 81,

    // Unknown Fatal Abort (99)
    ERR_INTERNAL_PANIC                = 99
};

[[nodiscard]] inline constexpr uint8_t to_underlying(EngineExitCode code) noexcept {
    return static_cast<uint8_t>(code);
}

[[nodiscard]] inline constexpr std::string_view exit_code_to_string(EngineExitCode code) noexcept {
    switch (code) {
        case EngineExitCode::SUCCESS:                           return "SUCCESS";
        case EngineExitCode::ERR_CLI_INVALID_ARGS:              return "ERR_CLI_INVALID_ARGS (10)";
        case EngineExitCode::ERR_CLI_MISSING_FILE:              return "ERR_CLI_MISSING_FILE (11)";
        case EngineExitCode::ERR_CLI_INVALID_QUERY:             return "ERR_CLI_INVALID_QUERY (12)";
        case EngineExitCode::ERR_CLI_UNRECOGNIZED_FLAG:         return "ERR_CLI_UNRECOGNIZED_FLAG (13)";
        case EngineExitCode::ERR_CLI_INTEGER_PARSE_FAIL:        return "ERR_CLI_INTEGER_PARSE_FAIL (14)";

        case EngineExitCode::ERR_IO_NOT_FOUND:                  return "ERR_IO_NOT_FOUND (20)";
        case EngineExitCode::ERR_IO_READ_FAILED:                return "ERR_IO_READ_FAILED (21)";
        case EngineExitCode::ERR_FILE_TRUNCATED:                return "ERR_FILE_TRUNCATED (22)";
        case EngineExitCode::ERR_MAGIC_MISMATCH:                return "ERR_MAGIC_MISMATCH (23)";
        case EngineExitCode::ERR_VERSION_UNSUPPORTED:           return "ERR_VERSION_UNSUPPORTED (24)";
        case EngineExitCode::ERR_ALIGNMENT_FAULT:               return "ERR_ALIGNMENT_FAULT (25)";
        case EngineExitCode::ERR_OFFSET_OUT_OF_BOUNDS:          return "ERR_OFFSET_OUT_OF_BOUNDS (26)";
        case EngineExitCode::ERR_STRING_POOL_OVERFLOW:          return "ERR_STRING_POOL_OVERFLOW (27)";
        case EngineExitCode::ERR_FILE_EMPTY:                    return "ERR_FILE_EMPTY (28)";
        case EngineExitCode::ERR_SCHEMA_CORRUPT_HEADER:         return "ERR_SCHEMA_CORRUPT_HEADER (29)";

        case EngineExitCode::ERR_MMAP_FAILED:                   return "ERR_MMAP_FAILED (40)";
        case EngineExitCode::ERR_SYS_HARDWARE_INVARIANT:        return "ERR_SYS_HARDWARE_INVARIANT (41)";
        case EngineExitCode::ERR_OOM:                           return "ERR_OOM (42)";
        case EngineExitCode::ERR_MADVISE_FAILED:                return "ERR_MADVISE_FAILED (43)";
        case EngineExitCode::ERR_MUNMAP_FAILED:                 return "ERR_MUNMAP_FAILED (44)";
        case EngineExitCode::ERR_PAGE_SIZE_MISMATCH:            return "ERR_PAGE_SIZE_MISMATCH (45)";

        case EngineExitCode::ERR_EXEC_LOAD_FACTOR_EXCEEDED:     return "ERR_EXEC_LOAD_FACTOR_EXCEEDED (60)";
        case EngineExitCode::ERR_EXEC_OVERFLOW:                 return "ERR_EXEC_OVERFLOW (61)";
        case EngineExitCode::ERR_EXEC_CORRUPT_STATE:            return "ERR_EXEC_CORRUPT_STATE (62)";
        case EngineExitCode::ERR_EXEC_DIVISION_BY_ZERO:         return "ERR_EXEC_DIVISION_BY_ZERO (63)";
        case EngineExitCode::ERR_EXEC_EMPTY_SELECTION:          return "ERR_EXEC_EMPTY_SELECTION (64)";
        case EngineExitCode::ERR_EXEC_TABLE_COLLISION_SATURATE: return "ERR_EXEC_TABLE_COLLISION_SATURATE (65)";

        case EngineExitCode::ERR_OUTPUT_WRITE_FAILED:           return "ERR_OUTPUT_WRITE_FAILED (80)";
        case EngineExitCode::ERR_OUTPUT_BUFFER_OVERRUN:         return "ERR_OUTPUT_BUFFER_OVERRUN (81)";

        case EngineExitCode::ERR_INTERNAL_PANIC:                return "ERR_INTERNAL_PANIC (99)";
    }
    return "UNKNOWN_FATAL_ERROR";
}

} // namespace engine
