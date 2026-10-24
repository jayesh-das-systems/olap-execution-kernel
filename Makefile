# ==============================================================================
# Makefile: C++23 Analytical Query Engine & Pre-flight Admission Gate Suite
# ==============================================================================

CXX          ?= g++
SANITIZE     ?= none

# Baseline flags shared across all build profiles
COMMON_FLAGS := -std=c++23 -march=native \
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

BUILD_DIR    := build
SRC_DIR      := src
TESTS_DIR    := tests
SCRIPTS_DIR  := scripts
INCLUDE_DIR  := include

# Output binaries
ENGINE_BIN   := $(BUILD_DIR)/engine
HW_GATE_BIN  := $(BUILD_DIR)/test_hw_env
CORR_GATE_BIN:= $(BUILD_DIR)/test_correctness
GEN_BIN      := $(BUILD_DIR)/generate_dataset

HEADERS      := $(shell find $(INCLUDE_DIR) -type f -name '*.hpp')
GEN_SRC      := $(firstword $(wildcard tools/generate_dataset.cpp generator_dataset.cpp))

.PHONY: all clean run-gates check asan ubsan san dev-check help

# Default release target: compiles everything at -O3 without sanitizer overhead
all: $(ENGINE_BIN) $(HW_GATE_BIN) $(CORR_GATE_BIN) $(GEN_BIN)
	@chmod +x $(SCRIPTS_DIR)/run_gates.sh $(SCRIPTS_DIR)/query.sh 2>/dev/null || true
	@echo "Build complete [SANITIZE=$(SANITIZE)]. All binaries generated in $(BUILD_DIR)/"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ------------------------------------------------------------------------------
# Target Compilations
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

$(GEN_BIN): $(GEN_SRC) $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling Synthetic Data Generator: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

# ------------------------------------------------------------------------------
# Standard Gate Targets
# ------------------------------------------------------------------------------

run-gates: $(HW_GATE_BIN) $(CORR_GATE_BIN)
	@bash $(SCRIPTS_DIR)/run_gates.sh

check: all run-gates
	@echo "All pre-flight operational gates passed successfully."

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
	@echo "==> Executing Tier 1 & 2 gates under AddressSanitizer..."
	./build/test_hw_env
	./build/test_correctness
	@echo "[ASAN PASS] Pre-flight admission verified under AddressSanitizer."

clean:
	@echo "==> Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)

help:
	@echo "Available make targets:"
	@echo "  make                      - Production build (-O3 -DNDEBUG, zero runtime checks)"
	@echo "  make asan                 - Recompile all targets with AddressSanitizer (-fsanitize=address -g -O1)"
	@echo "  make ubsan                - Recompile all targets with UBSan (-fsanitize=undefined -g -O1)"
	@echo "  make san                  - Recompile all targets with both ASan and UBSan"
	@echo "  make dev-check            - Recompile with ASan and immediately execute hardware & correctness gates"
	@echo "  make check                - Run admission gates against the current build"
	@echo "  make clean                - Wipe the build/ directory"