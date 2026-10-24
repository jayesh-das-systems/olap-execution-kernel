#!/usr/bin/env bash
# ==============================================================================
# scripts/run_gates.sh — Pre-Flight Three-Tier Admission Gate Controller
# ==============================================================================
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${BLUE}[GATE INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[GATE WARN]${NC} $*"; }
log_error() { echo -e "${RED}[GATE FAULT]${NC} $*"; }
log_pass()  { echo -e "${GREEN}[GATE PASS]${NC} $*"; }

HW_GATE_BIN="./build/test_hw_env"
CORR_GATE_BIN="./build/test_correctness"

# ------------------------------------------------------------------------------
# TIER 0: Compiler Version & C++23 Diagnostic Gate
# ------------------------------------------------------------------------------
echo "============================================================"
log_info "TIER 0: Inspecting Compiler Toolchain & Feature-Test Support"
echo "============================================================"

CXX_PATH="${CXX:-g++}"
if ! command -v "$CXX_PATH" &>/dev/null; then
    CXX_PATH="clang++"
fi

if ! command -v "$CXX_PATH" &>/dev/null; then
    log_error "No valid C++ compiler (g++ or clang++) located in PATH."
    exit 41
fi

CXX_VERSION=$("$CXX_PATH" -dumpversion 2>/dev/null || echo "0")
MAJOR_VERSION=$(echo "$CXX_VERSION" | cut -d. -f1)

log_info "Detected Compiler: $CXX_PATH (Version: $CXX_VERSION)"

# GCC requires >= 14, Clang requires >= 18 for full P2590R2 / P0847R7 support
if [[ "$CXX_PATH" == *"g++"* ]] && (( MAJOR_VERSION < 14 )); then
    log_warn "GCC version $CXX_VERSION detected. GCC 14+ is strongly advised for ISO C++23."
elif [[ "$CXX_PATH" == *"clang"* ]] && (( MAJOR_VERSION < 18 )); then
    log_warn "Clang version $CXX_VERSION detected. Clang 18+ is strongly advised for ISO C++23."
fi

# ------------------------------------------------------------------------------
# TIER 1: Hardware Invariants & POSIX Virtual Memory Admission Gate
# ------------------------------------------------------------------------------
echo "============================================================"
log_info "TIER 1: Executing Hardware & Kernel Environment Gate"
echo "============================================================"

if [[ ! -x "$HW_GATE_BIN" ]]; then
    log_error "Binary not found at: $HW_GATE_BIN"
    log_error "Execute 'make' or compile Tier 1 test harness first."
    exit 20
fi

if ! "$HW_GATE_BIN"; then
    STATUS=$?
    log_error "Tier 1 hardware invariant gate rejected environment with code: $STATUS"
    exit "$STATUS"
fi

log_pass "Tier 1 cleared: 64-bit address space, 4 KiB pages, and mmap validated."

# ------------------------------------------------------------------------------
# TIER 2: Schema ABI, Bitmask Logic & Pipeline Math Verification Gate
# ------------------------------------------------------------------------------
echo "============================================================"
log_info "TIER 2: Executing Schema ABI & Vector Correctness Gate"
echo "============================================================"

if [[ ! -x "$CORR_GATE_BIN" ]]; then
    log_error "Binary not found at: $CORR_GATE_BIN"
    log_error "Execute 'make' or compile Tier 2 test harness first."
    exit 20
fi

if ! "$CORR_GATE_BIN"; then
    STATUS=$?
    log_error "Tier 2 schema ABI or pipeline correctness gate failed with code: $STATUS"
    exit "$STATUS"
fi

log_pass "Tier 2 cleared: 128-byte cache alignment, validity bitmasks, and math verified."

echo "============================================================"
log_pass "ALL PRE-FLIGHT ADMISSION GATES CLEARED (Engine Bootup Authorized)"
echo "============================================================"
exit 0
