#!/usr/bin/env bash
set -euo pipefail

FUZZ_DIR="/tmp/olap_fuzz_$$"
mkdir -p "${FUZZ_DIR}"

trap 'rm -rf "${FUZZ_DIR}"' EXIT

BUILD_DIR="./build"
ORACLE="${BUILD_DIR}/naive_oracle"
GEN="${BUILD_DIR}/generate_fuzz_case"
ENGINE_BIN="${BUILD_DIR}/engine"

echo "=========================================================="
echo " [FUZZ] Building Test Drivers & Execution Kernel"
echo "=========================================================="
make -j"$(nproc)" "${ORACLE}" "${GEN}" "${ENGINE_BIN}"

ROW_COUNTS=(1 7 8 9 15 16 17 1023 1024 1025 2047 2048 2049 3317 10007)
PATTERNS=(0 1 2 3 4)

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

            # 2. Run engine directly (discard stderr telemetry, capture TSV from stdout)
            "${ENGINE_BIN}" "${TEST_FILE}" \
                --min-int "${MIN_INT}" \
                --max-int "${MAX_INT}" \
                "${PFX_ARGS[@]}" \
                --group-by char 2>/dev/null > "${ENGINE_RAW}"

            # 3. Normalize outputs: preserve header, sort data lines by group key
            
            sort -k1,1n "${ORACLE_RAW}" > "${ORACLE_NORM}"

            
            sort -k1,1n "${ENGINE_RAW}" > "${ENGINE_NORM}"

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
