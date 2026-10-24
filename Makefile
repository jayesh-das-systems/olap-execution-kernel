# ==============================================================================
# Makefile: C++23 Analytical Query Engine & Hardware Pre-flight Test Suite
# ==============================================================================

CXX          ?= g++
CXXFLAGS     := -std=c++23 -O3 -DNDEBUG -march=native \
                -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
                -fno-omit-frame-pointer \
                -Iinclude

BUILD_DIR    := build
SRC_DIR      := src
TESTS_DIR    := tests
SCRIPTS_DIR  := scripts
INCLUDE_DIR  := include

# Targets
ENGINE_BIN   := $(BUILD_DIR)/engine
HW_GATE_BIN  := $(BUILD_DIR)/test_hw_env
CORR_GATE_BIN:= $(BUILD_DIR)/test_correctness
GEN_BIN      := $(BUILD_DIR)/generate_dataset

# Headers dependency tracking
HEADERS      := $(shell find $(INCLUDE_DIR) -type f -name '*.hpp')

# Synthetic dataset generator source auto-discovery
GEN_SRC      := $(firstword $(wildcard tools/generate_dataset.cpp generator_dataset.cpp))

.PHONY: all clean run-gates check help

all: $(ENGINE_BIN) $(HW_GATE_BIN) $(CORR_GATE_BIN) $(GEN_BIN)
	@chmod +x $(SCRIPTS_DIR)/run_gates.sh $(SCRIPTS_DIR)/query.sh 2>/dev/null || true
	@echo "Build complete. All binaries generated in $(BUILD_DIR)/"

# Ensure build directory exists
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ------------------------------------------------------------------------------
# Executable Compilations
# ------------------------------------------------------------------------------

$(ENGINE_BIN): $(SRC_DIR)/main.cpp $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling CLI Engine: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

$(HW_GATE_BIN): $(TESTS_DIR)/test_hw_env.cpp $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling Tier 1 HW Gate: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

$(CORR_GATE_BIN): $(TESTS_DIR)/test_correctness.cpp $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling Tier 2 Correctness Gate: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

$(GEN_BIN): $(GEN_SRC) $(HEADERS) | $(BUILD_DIR)
	@echo "==> Compiling Synthetic Data Generator: $@"
	$(CXX) $(CXXFLAGS) $< -o $@

# ------------------------------------------------------------------------------
# Operational Targets
# ------------------------------------------------------------------------------

run-gates: $(HW_GATE_BIN) $(CORR_GATE_BIN)
	@bash $(SCRIPTS_DIR)/run_gates.sh

check: all run-gates
	@echo "All pre-flight operational gates passed successfully."

clean:
	@echo "==> Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)

help:
	@echo "Available make targets:"
	@echo "  make          - Build engine, data generator, and all admission binaries"
	@echo "  make check    - Build and execute Tier 1 & Tier 2 admission gates"
	@echo "  make run-gates- Run test_hw_env and test_correctness via run_gates.sh"
	@echo "  make clean    - Remove build/ directory and compiled binaries"
