#!/usr/bin/env bash
set -euo pipefail

FUZZ_GEN="build/generate_fuzz_case"
ORACLE="build/naive_oracle"
ENGINE="build/engine"
TMP_DIR=$(mktemp -d /tmp/olap_fuzz_XXXXXX)

cleanup() {
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

DATASET_BIN="${TMP_DIR}/fuzz_dataset.bin"
ORACLE_RAW="${TMP_DIR}/oracle_raw.tsv"
ENGINE_RAW="${TMP_DIR}/engine_raw.tsv"
ORACLE_NORM="${TMP_DIR}/oracle_norm.tsv"
ENGINE_NORM="${TMP_DIR}/engine_norm.tsv"

ROW_COUNTS=(1 2 7 8 9 15 16 17 63 64 65 127 128 129 1023 1024 1025 2048 4096 8192)
PATTERNS=(0 1 2 3 4)
GROUP_MODES=("char" "int" "str")

echo "=========================================================="
echo " [FUZZ] Starting Multi-Mode Differential Correctness Sweep"
echo "=========================================================="

TOTAL_TESTS=0
FAILED_TESTS=0

for N in "${ROW_COUNTS[@]}"; do
    for PATTERN in "${PATTERNS[@]}"; do
        for GMODE in "${GROUP_MODES[@]}"; do
            TOTAL_TESTS=$((TOTAL_TESTS + 1))

            # 1. Generate test dataset
            "${FUZZ_GEN}" "${DATASET_BIN}" "${N}" "${PATTERN}" "$((1337 + TOTAL_TESTS))" > /dev/null 2>&1

            # 2. Pick predicate configuration
            MIN_INT=-100000
            MAX_INT=100000
            PREFIX="APAC"

            if (( TOTAL_TESTS % 3 == 0 )); then
                PREFIX="EMEA"
                MIN_INT=-500000
                MAX_INT=500000
            elif (( TOTAL_TESTS % 3 == 1 )); then
                PREFIX=""
                MIN_INT=-250000
                MAX_INT=250000
            fi

            # 3. Run Oracle
            if [[ -n "${PREFIX}" ]]; then
                "${ORACLE}" "${DATASET_BIN}" \
                    --min-int "${MIN_INT}" \
                    --max-int "${MAX_INT}" \
                    --str-prefix "${PREFIX}" \
                    --group-by "${GMODE}" > "${ORACLE_RAW}"
            else
                "${ORACLE}" "${DATASET_BIN}" \
                    --min-int "${MIN_INT}" \
                    --max-int "${MAX_INT}" \
                    --group-by "${GMODE}" > "${ORACLE_RAW}"
            fi

            # 4. Run Engine
            if [[ -n "${PREFIX}" ]]; then
                "${ENGINE}" "${DATASET_BIN}" \
                    --min-int "${MIN_INT}" \
                    --max-int "${MAX_INT}" \
                    --str-prefix "${PREFIX}" \
                    --group-by "${GMODE}" > "${ENGINE_RAW}"
            else
                "${ENGINE}" "${DATASET_BIN}" \
                    --min-int "${MIN_INT}" \
                    --max-int "${MAX_INT}" \
                    --group-by "${GMODE}" > "${ENGINE_RAW}"
            fi

            # 5. Normalize by sorting numerically on key_hash
            sort -k1,1n "${ORACLE_RAW}" > "${ORACLE_NORM}"
            sort -k1,1n "${ENGINE_RAW}" > "${ENGINE_NORM}"

            # 6. Verify differential identity
            if ! diff -u "${ORACLE_NORM}" "${ENGINE_NORM}" > "${TMP_DIR}/diff.patch"; then
                echo ""
                echo "[-] ASSERTION FAILED!"
                echo "    Dataset:     Rows=${N}, MaskPattern=${PATTERN}, GroupMode=${GMODE}"
                echo "    Predicates:  Int=[${MIN_INT}, ${MAX_INT}], Prefix='${PREFIX}'"
                echo "----------------------------------------------------------"
                cat "${TMP_DIR}/diff.patch"
                echo "----------------------------------------------------------"
                FAILED_TESTS=$((FAILED_TESTS + 1))
                exit 1
            fi

            printf "."
        done
    done
done

echo ""
echo "=========================================================="
echo " [PASS] Differential Sweep Succeeded"
echo " Scenarios Tested: ${TOTAL_TESTS}"
echo " Mismatches:       ${FAILED_TESTS}"
echo " Status:           All grouping modes (char, int, str) verified"
echo "=========================================================="
