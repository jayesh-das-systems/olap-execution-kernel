#!/usr/bin/env bash
set -euo pipefail

# Work in a dedicated scratch directory
FUZZ_DIR="/tmp/olap_fuzz_$$"
mkdir -p "${FUZZ_DIR}"

trap 'rm -rf "${FUZZ_DIR}"' EXIT

BUILD_DIR="./build"
ORACLE="${BUILD_DIR}/naive_oracle"
GEN="${BUILD_DIR}/generate_fuzz_case"
ENGINE_RUNNER="./scripts/query.sh"

echo "=========================================================="
echo " [FUZZ] Building Test Drivers & Execution Kernel"
echo "=========================================================="
make -j"$(nproc)" "${ORACLE}" "${GEN}"

if [[ ! -x "${ENGINE_RUNNER}" ]]; then
    echo "Error: Query runner script not found or not executable: ${ENGINE_RUNNER}"
    exit 1
fi

# 1. Critical row counts:
#    - Sub-byte & sub-chunk boundaries (1, 7, 8, 9, 15, 16, 17)
#    - Exact 1024-chunk multiples and +/-1 boundary edges (1023, 1024, 1025)
#    - Multi-chunk remainders (2047, 2048, 2049, 3317, 10007)
ROW_COUNTS=(1 7 8 9 15 16 17 1023 1024 1025 2047 2048 2049 3317 10007)

# 2. Mask patterns:
#    0: All Valid (0xFF)
#    1: All Deleted (0x00)
#    2: Alternating (0xAA)
#    3: Pseudo-random bits
#    4: Sparse edges
PATTERNS=(0 1 2 3 4)

# 3. Predicate combinations:
#    - Broad int range + empty prefix
#    - Tight int range + matching prefix
#    - Negative int range
#    - Out-of-bounds predicate (selectivity = 0%)
declare -a TEST_CONFIGS=(
    "-500000:500000:"
    "10000:80000:APAC"
    "-800000:-200000:EMEA"
    "2000000:3000000:NOMATCH"
)

TOTAL_RUNS=0
FAILED_RUNS=0

echo "=========================================================="
echo " [FUZZ] Starting Differential Correctness Sweep"
echo "=========================================================="

for N in "${ROW_COUNTS[@]}"; do
    for P in "${PATTERNS[@]}"; do
        TEST_FILE="${FUZZ_DIR}/dataset_${N}_p${P}.bin"
        
        # Generate targeted edge-case binary dataset
        "${GEN}" "${TEST_FILE}" "${N}" "${P}" 1337 > /dev/null

        for CONFIG in "${TEST_CONFIGS[@]}"; do
            IFS=":" read -r MIN_INT MAX_INT PFX <<< "${CONFIG}"
            TOTAL_RUNS=$((TOTAL_RUNS + 1))

            PFX_ARGS=()
            if [[ -n "${PFX}" ]]; then
                PFX_ARGS=("--str-prefix" "${PFX}")
            fi

            ORACLE_RAW="${FUZZ_DIR}/oracle_raw.tsv"
            ENGINE_RAW="${FUZZ_DIR}/engine_raw.tsv"
            ORACLE_NORM="${FUZZ_DIR}/oracle_norm.tsv"
            ENGINE_NORM="${FUZZ_DIR}/engine_norm.tsv"

            # 1. Run naive reference oracle
            "${ORACLE}" "${TEST_FILE}" \
                --min-int "${MIN_INT}" \
                --max-int "${MAX_INT}" \
                "${PFX_ARGS[@]}" > "${ORACLE_RAW}"

            # 2. Run vectorized production execution kernel
            #    Pipes through --no-telemetry/--format tsv to isolate tabular reduction
            "${ENGINE_RUNNER}" "${TEST_FILE}" \
                --min-int "${MIN_INT}" \
                --max-int "${MAX_INT}" \
                "${PFX_ARGS[@]}" \
                --group-by char \
                --format tsv \
                --no-telemetry > "${ENGINE_RAW}"

            # 3. Normalize outputs: preserve header, sort data lines by group key
            head -n 1 "${ORACLE_RAW}" > "${ORACLE_NORM}"
            tail -n +2 "${ORACLE_RAW}" | sort -k1,1 >> "${ORACLE_NORM}"

            head -n 1 "${ENGINE_RAW}" > "${ENGINE_NORM}"
            tail -n +2 "${ENGINE_RAW}" | sort -k1,1 >> "${ENGINE_NORM}"

            # 4. Strict byte-for-byte diff
            if ! diff -u "${ORACLE_NORM}" "${ENGINE_NORM}" > "${FUZZ_DIR}/diff.patch"; then
                echo ""
                echo "[-] ASSERTION FAILED!"
                echo "    Dataset:     Rows=${N}, MaskPattern=${P}"
                echo "    Predicates:  Int=[${MIN_INT}, ${MAX_INT}], Prefix='${PFX}'"
                echo "----------------------------------------------------------"
                cat "${FUZZ_DIR}/diff.patch"
                echo "----------------------------------------------------------"
                FAILED_RUNS=$((FAILED_RUNS + 1))
                exit 1
            fi
        done

        rm -f "${TEST_FILE}"
        printf "."
    done
done

echo ""
echo "=========================================================="
echo " [PASS] Differential Sweep Succeeded"
echo " Scenarios Tested: ${TOTAL_RUNS}"
echo " Mismatches:       0"
echo " Status:           All vectorized kernel outputs match scalar ground truth"
echo "=========================================================="
