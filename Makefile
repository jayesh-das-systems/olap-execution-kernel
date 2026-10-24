# ==============================================================================
# Makefile: C++23 Analytical Query Engine & Verification Suite
# ==============================================================================

CXX           ?= clang++-18
SANITIZE      ?= none

# Baseline flags shared across all build profiles
COMMON_FLAGS  := -std=c++23 -march=native \
                 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
                 -fno-omit-frame-pointer \
                 -Iinclude

# Profile configurations
RELEASE_FLAGS := -O3 -DNDEBUG
ASAN_FLAGS    := -O1 -g -fsanitize=address -fno-optimize-sibling-calls
UBSAN_FLAGS   := -O1 -g -fsanitize=undefined -fno-sanitize-recover=all

# Resolve CXXFLAGS based on the SANITIZE variable
ifeq ($(SANITIZE),address)
    CXXFLAGS := $(COMMON_FLAGS) $(ASAN_FLAGS)
else ifeq ($(SANITIZE),undefined)
    CXXFLAGS := $(COMMON_FLAGS) $(UBSAN_FLAGS)
else ifeq ($(SANITIZE),all)
    CXXFLAGS := $(COMMON_FLAGS) $(ASAN_FLAGS) $(UBSAN_FLAGS)
else
    CXXFLAGS := $(COMMON_FLAGS) $(RELEASE_FLAGS)
endif

# Append libc++ automatically when invoking Clang
ifneq (,$(findstring clang++,$(CXX)))
    CXXFLAGS += -stdlib=libc++
endif

BUILD_DIR     := build
SRC_DIR       := src
TESTS_DIR     := tests
TOOLS_DIR     := tools
SCRIPTS_DIR   := scripts
INCLUDE_DIR   := include

# Core Engine & Gate Binaries
ENGINE_BIN    := $(BUILD_DIR)/engine
HW_GATE_BIN   := $(BUILD_DIR)/test_hw_env
CORR_GATE_BIN := $(BUILD_DIR)/test_correctness
MUT_TEST_BIN  := $(BUILD_DIR)/test_mutations

# Tooling, Oracles & Fuzz Generators
GEN_BIN       := $(BUILD_DIR)/generate_dataset
ORACLE_BIN    := $(BUILD_DIR)/naive_oracle
FUZZ_GEN_BIN  := $(BUILD_DIR)/generate_fuzz_case

HEADERS       := $(shell find $(INCLUDE_DIR) -type f -name '*.hpp')

ALL_BINARIES  := $(ENGINE_BIN) $(HW_GATE_BIN) $(CORR_GATE_BIN) $(MUT_TEST_BIN) \
                 $(GEN_BIN) $(ORACLE_BIN) $(FUZZ_GEN_BIN)

.PHONY: all clean run-gates check fuzz asan ubsan san dev-check help

# Default release target: compiles everything at -O3 without sanitizer overhead
all: $(ALL_BINARIES)
	@chmod +x $(SCRIPTS_DIR)/run_gates.sh $(SCRIPTS_DIR)/query.sh $(SCRIPTS_DIR)/fuzz_test.sh 2>/dev/null || true
	@echo "Build complete [CXX=$(CXX), SANITIZE=$(SANITIZE)]. All binaries generated in $(BUILD_DIR)/"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ------------------------------------------------------------------------------
# Target Compilations: Engine & Test Gates
# ------------------------------------------------------------------------------

$(ENGINE_BIN): $(SRC_DIR)/main.cpp $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling CLI Engine: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

$(HW_GATE_BIN): $(TESTS_DIR)/test_hw_env.cpp $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling Tier 1 Hardware Gate: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

$(CORR_GATE_BIN): $(TESTS_DIR)/test_correctness.cpp $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling Tier 2 Correctness Gate: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

$(MUT_TEST_BIN): $(TESTS_DIR)/test_mutations.cpp $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling Mutation & Invariant Gate: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

# ------------------------------------------------------------------------------
# Target Compilations: Verification Tooling & Oracles
# ------------------------------------------------------------------------------

$(GEN_BIN): $(TOOLS_DIR)/generate_dataset.cpp $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling Synthetic Data Generator: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

$(ORACLE_BIN): $(TOOLS_DIR)/naive_oracle.cpp | $(BUILD_DIR)
	@echo "==> Compiling Scalar Reference Oracle: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

$(FUZZ_GEN_BIN): $(TOOLS_DIR)/generate_fuzz_case.cpp | $(BUILD_DIR)
	@echo "==> Compiling Edge-Case Fuzz Generator: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

# ------------------------------------------------------------------------------
# Verification & Operational Gates
# ------------------------------------------------------------------------------

run-gates: $(HW_GATE_BIN) $(CORR_GATE_BIN) $(MUT_TEST_BIN)
	@bash $(SCRIPTS_DIR)/run_gates.sh

check: all run-gates
	@echo "All pre-flight operational gates passed successfully."

# Differential fuzzing matrix across row remainder edges & bitmask permutations
fuzz: $(ENGINE_BIN) $(ORACLE_BIN) $(FUZZ_GEN_BIN)
	@chmod +x $(SCRIPTS_DIR)/fuzz_test.sh
	@bash $(SCRIPTS_DIR)/fuzz_test.sh

# ------------------------------------------------------------------------------
# Developer Sanitizer Profiles (Opt-in)
# ------------------------------------------------------------------------------

asan:
	@$(MAKE) clean
	@$(MAKE) all SANITIZE=address
	@echo "[ASAN BUILD] Clean build with AddressSanitizer complete."

ubsan:
	@$(MAKE) clean
	@$(MAKE) all SANITIZE=undefined
	@echo "[UBSAN BUILD] Clean build with UndefinedBehaviorSanitizer complete."

san:
	@$(MAKE) clean
	@$(MAKE) all SANITIZE=all
	@echo "[COMBO BUILD] Clean build with ASan + UBSan complete."

dev-check: asan
	@echo "==> Executing pre-flight gates under AddressSanitizer..."
	./$(HW_GATE_BIN)
	./$(CORR_GATE_BIN)
	./$(MUT_TEST_BIN)
	@echo "[ASAN PASS] Pre-flight admission verified under AddressSanitizer."

clean:
	@echo "==> Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)

help:
	@echo "Available make targets:"
	@echo "  make                 - Production build (-O3 -DNDEBUG, zero runtime checks)"
	@echo "  make fuzz            - Run differential matrix against scalar reference oracle"
	@echo "  make check           - Run admission gates against the current build"
	@echo "  make asan            - Recompile all targets with AddressSanitizer"
	@echo "  make ubsan           - Recompile all targets with UndefinedBehaviorSanitizer"
	@echo "  make san             - Recompile all targets with both ASan and UBSan"
	@echo "  make dev-check       - Recompile with ASan and run hardware & correctness gates"
	@echo "  make clean           - Wipe the build/ directory"
