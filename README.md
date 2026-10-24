# olap-execution-kernel

[![Language](https://img.shields.io/badge/C%2B%2B-23-blue.svg?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/23)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Linux%20x86__64-orange.svg?style=flat-square&logo=linux)](https://kernel.org)
[![Compiler](https://img.shields.io/badge/Compiler-Clang%2018%20%7C%20GCC%2014%2B-purple.svg?style=flat-square)](https://llvm.org)

An ultra-high-throughput, zero-copy OLAP vectorized query execution engine engineered in modern ISO C++23.

Built from bare metal using POSIX system primitives, memory-mapped columnar layouts, cacheline-aligned data structures, and branchless scalar/vectorized processing pipelines. Capable of sustaining **60+ M rows/sec on selective filter and aggregation workloads** on commodity multi-core hardware.

---

## Key Architectural Highlights

* **Zero-Copy Memory-Mapped Storage:** Maps immutable columnar binary datasets directly via POSIX `mmap(2)` with `MAP_POPULATE` and `MADV_SEQUENTIAL`, completely eliminating runtime serialization and buffer copies.
* **Vectorized Processing Model:** Processes columnar vectors in fixed 1024-tuple chunks (`VECTOR_SIZE`), maximizing L1/L2 cache locality and instruction-level parallelism (ILP).
* **Admission Gates Architecture:** Enforces hard environment and ABI validation across 3 pre-flight tiers prior to pipeline execution:
  * **Tier 0:** C++23 feature-test verification (`<expected>`, library feature macros like `__cpp_lib_expected`, and ISO C++23 compiler compliance).
  * **Tier 1:** Linux kernel environment, 64-bit virtual memory constraints, and POSIX 4 KiB page-alignment validation.
  * **Tier 2:** ABI sanity, packed column struct validation, 128-byte cacheline alignment checks, and bitmask semantics.
* **Sliding-Window & Soft Deletion Mechanics:** Dynamic stream processing over `[head_idx, tail_idx)` physical buffer spans without reallocation or full-buffer compactions, backed by bitmask soft-deletes via `live_mask`.
* **Zero-Heap Safe Arithmetic:** Overflow-safe analytical aggregations powered by `std::expected<int64_t, ArithmeticError>` without hot-path exception throwing.
* **Specialized Grouping Operators:**
  * **Direct Character Array:** O(1) indexing for low-cardinality single-byte grouping domains.
  * **Open-Address Linear Probing Hash Table:** Power-of-two mask hashing with Murmur/Fibonacci mixing for multi-column groupings.

---

## Performance Benchmarks

Executed on standard Google Cloud virtualized hardware (`Linux x86_64`, single-threaded core, 244.38 MiB binary dataset, 10,000,000 rows):

| Query Profile | Selectivity | Latency | Sustained Throughput | Peak RSS |
| :--- | :---: | :---: | :---: | :---: |
| **Filtered Direct-Char Aggregation**<br>*(Int range + inline string prefix)* | **14.0%** | **~160 ms** | **62.23 M rows/sec** | **209.36 MiB** |
| **Zero-Match Short-Circuit**<br>*(Out-of-bounds predicate)* | **0.0%** | **~61 ms** | **163.93 M rows/sec** | **198.50 MiB** |
| **High-Cardinality Hash Grouping**<br>*(Full table multi-key aggregation)* | **100.0%** | **~395 ms** | **25.31 M rows/sec** | **245.10 MiB** |

---

## Directory Structure

```text
olap-execution-kernel/
├── include/
│   ├── common/
│   │   ├── exit_codes.hpp       # Standardized engine error codes
│   │   └── hardware_config.hpp  # Cacheline sizes, alignment, and vector limits
│   ├── execution/
│   │   ├── accumulator.hpp      # Direct-char and vectorized hash accumulators
│   │   ├── expression.hpp       # Vectorized selection vectors & scalar filters
│   │   ├── orderby.hpp          # Result sorting and top-K handling
│   │   ├── pipeline.hpp         # Execution pipeline orchestrator
│   │   └── vector_chunk.hpp     # Chunk representations and bitmask filters
│   ├── output/
│   │   └── tsv_emitter.hpp      # Fast buffered streaming result serializer
│   └── storage/
│       ├── file_format.hpp      # Binary dataset headers and column metadata
│       └── schema.hpp           # 128-byte aligned schemas & GlobalExecutionState
├── src/
│   └── main.cpp                 # Engine entry point and CLI runner
├── tests/
│   ├── test_correctness.cpp     # Tier 2 ABI and synthetic reduction verification
│   ├── test_hw_env.cpp          # Tier 1 POSIX/mmap environment verification
│   └── test_mutations.cpp       # Dynamic appends, soft deletes, and window sliding
├── tools/
│   └── generate_dataset.cpp     # Synthetic 10M-row columnar binary generator
├── scripts/
│   ├── query.sh                 # End-to-end admission gate and query execution wrapper
│   └── run_gates.sh             # Admission gate runner
├── Makefile                     # Build targets with strict warnings and optimizations
└── README.md
```

---

## Prerequisites & Toolchain

A conforming ISO C++23 toolchain with `<expected>` support is strictly required:

* **Linux:** Kernel 5.4+ (x86_64)
* **Compiler:** `clang++-18` (with `-stdlib=libc++`) or `g++-13+`
* **Build System:** GNU Make 4.0+
* **Libraries:** POSIX threads (`-lpthread`)

## Toolchain & Compiler Philosophy

This engine is built and verified primarily against **LLVM Clang 18 with `libc++`**, enforcing strict ISO C++23 compliance, rigorous diagnostic checks, and clean memory models without relying on GCC-specific header leakage or compiler extensions.

* **Primary Toolchain:** `clang++-18` with `-stdlib=libc++` (Strict conformance & optimization baseline)
* **Secondary Toolchain:** `g++-13`+ (Compatible fallback)
* **Build System:** GNU Make 4.0+
* **Target Architecture:** Linux x86_64

---

## Quickstart

### 1. Clone the Repository

```bash
git clone https://github.com/jayesh-das-systems/olap-execution-kernel.git
cd olap-execution-kernel
```

### 2. Build the Verification Suite & Engine

```bash
make clean
make all
```

### 3. Run Pre-flight Admission Tests

```bash
# Verify kernel and POSIX mmap environment (Tier 1)
./build/test_hw_env

# Verify schema ABI layouts, bitmasks, and checked math (Tier 2)
./build/test_correctness

# Verify in-flight row mutations, soft-deletions, and sliding window
./build/test_mutations
```

### 4. Generate the 10M-Row Synthetic Dataset

```bash
./build/generate_dataset benchmark_10m.bin 10000000
```

### 5. Execute an Analytical Query

Run the production wrapper to clear admission gates and benchmark pipeline execution:

```bash
./scripts/query.sh benchmark_10m.bin \
  --min-int 10000 \
  --max-int 80000 \
  --str-prefix "APAC" \
  --group-by char \
  --order-by result \
  --benchmark
```

---

## Technical Details

### Memory Layout

All tabular data files adopt an immutable columnar physical layout. Column arrays are padded to 128-byte boundaries matching modern CPU double-cacheline configurations, minimizing false sharing and ensuring aligned SIMD load eligibility.

### In-Memory Validity & Mutation Bits

Tuple validity and active visibility are maintained via compact bit vectors:

```cpp
live_byte = mask[row >> 3];
bit = (live_byte >> (row & 7)) & 1;
```

Soft-deletions toggle bits in-place without triggering physical array shifts, guaranteeing deterministic O(1) mutation overhead.

---

## License

This project is licensed under the Apache License, Version 2.0. See the [LICENSE](LICENSE) file for full license text.
