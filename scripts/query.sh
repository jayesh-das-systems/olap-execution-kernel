#!/usr/bin/env bash
# ==============================================================================
# scripts/query.sh — Engine Execution Supervisor & Procfs Telemetry Monitor
# ==============================================================================
set -euo pipefail

ENGINE_BIN="./build/engine"
GATE_SCRIPT="./scripts/run_gates.sh"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${BLUE}[SUPERVISOR INFO]${NC} $*" >&2; }
log_warn()  { echo -e "${YELLOW}[SUPERVISOR WARN]${NC} $*" >&2; }
log_error() { echo -e "${RED}[SUPERVISOR ERROR]${NC} $*" >&2; }
log_pass()  { echo -e "${GREEN}[SUPERVISOR PASS]${NC} $*" >&2; }

# ------------------------------------------------------------------------------
# 1. Parameter Validation & Pre-Flight Execution
# ------------------------------------------------------------------------------
if [[ $# -lt 1 ]]; then
    log_error "Missing data file argument."
    echo "Usage: $0 <path_to_data.bin> [engine_flags...]" >&2
    exit 11
fi

DATA_FILE="$1"
shift 1
ENGINE_ARGS=("$@")

if [[ ! -x "$ENGINE_BIN" ]]; then
    log_error "Engine executable not found at: $ENGINE_BIN"
    log_error "Run 'make' first."
    exit 20
fi

if [[ ! -f "$DATA_FILE" ]]; then
    log_error "Target dataset does not exist: $DATA_FILE"
    exit 20
fi

if [[ -f "$GATE_SCRIPT" ]]; then
    log_info "Executing pre-flight hardware & ABI admission gates..."
    if ! bash "$GATE_SCRIPT"; then
        GATE_STATUS=$?
        log_error "Pre-flight admission gates failed with code: ${GATE_STATUS}"
        exit "$GATE_STATUS"
    fi
    log_pass "Pre-flight admission cleared."
else
    log_warn "Gate script ($GATE_SCRIPT) not found. Skipping admission gates."
fi

# ------------------------------------------------------------------------------
# 2. Spawn Process & High-Frequency Telemetry Loop
# ------------------------------------------------------------------------------
log_info "Spawning engine instance for target: $DATA_FILE"

"$ENGINE_BIN" "$DATA_FILE" "${ENGINE_ARGS[@]}" &
ENGINE_PID=$!

PEAK_RSS_KB=0
POLL_INTERVAL=0.002 # 2ms high-resolution sampling

while kill -0 "$ENGINE_PID" 2>/dev/null; do
    if [[ -r "/proc/$ENGINE_PID/status" ]]; then
        CURRENT_RSS=$(awk '/VmRSS:/ {print $2}' "/proc/$ENGINE_PID/status" 2>/dev/null || true)
        if [[ -n "$CURRENT_RSS" && "$CURRENT_RSS" =~ ^[0-9]+$ ]]; then
            if (( CURRENT_RSS > PEAK_RSS_KB )); then
                PEAK_RSS_KB=$CURRENT_RSS
            fi
        fi
    fi
    sleep "$POLL_INTERVAL"
done

# Collect exit code
wait "$ENGINE_PID" && EXIT_CODE=0 || EXIT_CODE=$?

# Fallback reading of VmHWM from /proc if recorded during termination
if [[ -r "/proc/$ENGINE_PID/status" ]]; then
    HWM=$(awk '/VmHWM:/ {print $2}' "/proc/$ENGINE_PID/status" 2>/dev/null || true)
    if [[ -n "$HWM" && "$HWM" =~ ^[0-9]+$ && HWM -gt PEAK_RSS_KB ]]; then
        PEAK_RSS_KB=$HWM
    fi
fi

# ------------------------------------------------------------------------------
# 3. Post-Mortem Diagnostics & Output
# ------------------------------------------------------------------------------
PEAK_RSS_MB=$(awk "BEGIN {printf \"%.2f\", $PEAK_RSS_KB / 1024}")
FILE_SIZE_BYTES=$(stat -c%s "$DATA_FILE" 2>/dev/null || stat -f%z "$DATA_FILE" 2>/dev/null || echo 0)
FILE_SIZE_MB=$(awk "BEGIN {printf \"%.2f\", $FILE_SIZE_BYTES / 1048576}")

echo "------------------------------------------------------------" >&2
log_info "Execution Telemetry Summary:"
echo "  Target File Size:  ${FILE_SIZE_MB} MiB (${FILE_SIZE_BYTES} bytes)" >&2
echo "  Peak Process RSS:  ${PEAK_RSS_MB} MiB (${PEAK_RSS_KB} kB)" >&2
echo "  Exit Status Code:  ${EXIT_CODE}" >&2
echo "------------------------------------------------------------" >&2

# ------------------------------------------------------------------------------
# 4. Expanded Deterministic Error Partition Handling (10..99)
# ------------------------------------------------------------------------------
case "$EXIT_CODE" in
    0)
        log_pass "Query executed successfully."
        ;;
    10)
        log_error "CLI Argument Fault (Code 10): Malformed CLI arguments or flags."
        ;;
    11)
        log_error "CLI Argument Fault (Code 11): Target dataset path missing."
        ;;
    12)
        log_error "CLI Argument Fault (Code 12): Invalid query configuration or unknown group mode."
        ;;
    13)
        log_error "CLI Argument Fault (Code 13): Unrecognized CLI flag passed."
        ;;
    14)
        log_error "CLI Argument Fault (Code 14): Integer string parse failed via std::from_chars."
        ;;
    20)
        log_error "Storage I/O Fault (Code 20): Target binary file not found (ENOENT)."
        ;;
    21)
        log_error "Storage I/O Fault (Code 21): File read or stat operation failed."
        ;;
    22)
        log_error "Storage Integrity Fault (Code 22): File is truncated or smaller than 128-byte header."
        ;;
    23)
        log_error "Schema Fault (Code 23): Magic bytes mismatch (expected 'NETH' 0x4854454E)."
        ;;
    24)
        log_error "Schema Fault (Code 24): Unsupported file layout version (expected v3)."
        ;;
    25)
        log_error "Hardware Alignment Fault (Code 25): Memory mapping violates 64-byte boundary."
        ;;
    26)
        log_error "Memory Bounds Fault (Code 26): Column offset points outside mapped file bounds."
        ;;
    27)
        log_error "String Arena Fault (Code 27): Pool size exceeds 4 GiB 32-bit offset limits."
        ;;
    28)
        log_error "Empty Dataset Fault (Code 28): File size is 0 bytes."
        ;;
    29)
        log_error "Schema Fault (Code 29): Column offsets overlap with 128-byte header."
        ;;
    40)
        log_error "Kernel VM Fault (Code 40): POSIX mmap allocation rejected by host kernel."
        ;;
    41)
        log_error "Platform Invariant Fault (Code 41): Address space is not 64-bit."
        ;;
    42)
        log_error "Memory Exhaustion (Code 42): std::bad_alloc triggered (OOM)."
        ;;
    43)
        log_error "Kernel Subsystem Fault (Code 43): posix_madvise rejected by host kernel."
        ;;
    44)
        log_error "Kernel Subsystem Fault (Code 44): munmap call failed on active mapping."
        ;;
    45)
        log_error "Page Size Fault (Code 45): Host virtual memory page size is not 4096 bytes."
        ;;
    60)
        log_error "Execution Fault (Code 60): Aggregation hash table load factor limit tripped."
        ;;
    61)
        log_error "Arithmetic Fault (Code 61): Integer overflow encountered during aggregation."
        ;;
    62)
        log_error "Execution State Fault (Code 62): Internal vector chunk or bitmask corruption."
        ;;
    63)
        log_error "Arithmetic Fault (Code 63): Division by zero encountered."
        ;;
    64)
        log_error "Execution Fault (Code 64): Selection vector empty during mandatory evaluation."
        ;;
    65)
        log_error "Hash Table Fault (Code 65): Key collision saturation reached."
        ;;
    80)
        log_error "TSV Streamer Fault (Code 80): POSIX write failed to flush records to stdout."
        ;;
    81)
        log_error "TSV Streamer Fault (Code 81): Static output buffer overrun detected."
        ;;
    99)
        log_error "Fatal Internal Panic (Code 99): Unhandled non-standard exception trapped."
        ;;
    139)
        log_error "Fatal Signal: Segmentation Fault (SIGSEGV) — illegal memory access."
        ;;
    *)
        log_error "Unexpected failure with exit code: $EXIT_CODE"
        ;;
esac

exit "$EXIT_CODE"
