# =============================================================================
# riscv-iommu-ref-cov  —  Makefile
#
# Builds the upstream libiommu reference model with gcov coverage
# instrumentation, then runs all registered test cases and generates
# an lcov HTML report.
#
# Usage:
#   make              — build + run tests + generate HTML report
#   make build        — compile with coverage flags only
#   make test         — run all test binaries
#   make coverage     — collect gcov data & generate HTML report
#   make clean        — remove build artefacts and coverage data
# =============================================================================

# --------------------------------------------------------------------------- #
# Paths (adjust REF_MODEL_DIR if the submodule lives elsewhere)
# --------------------------------------------------------------------------- #
REF_MODEL_DIR   := upstream/iommu_ref_model
LIBIOMMU_SRC    := $(REF_MODEL_DIR)/libiommu/src
LIBIOMMU_INC    := $(REF_MODEL_DIR)/libiommu/include
TB_DIR          := tb/c
BUILD_DIR       := build
COV_DIR         := coverage/html
LCOV_INFO       := coverage/lcov.info
LCOV_INFO_FILT  := coverage/lcov_filtered.info

# --------------------------------------------------------------------------- #
# Toolchain
# --------------------------------------------------------------------------- #
CC      := gcc
CFLAGS  := -O0 -g \
            -I$(LIBIOMMU_INC) \
            --coverage \
            -fprofile-arcs \
            -ftest-coverage \
            -Wall -Wextra -Wno-unused-parameter
LDFLAGS := --coverage

# --------------------------------------------------------------------------- #
# Sources
# --------------------------------------------------------------------------- #
LIBIOMMU_SRCS := $(wildcard $(LIBIOMMU_SRC)/*.c)
TB_SRCS       := $(wildcard $(TB_DIR)/*.c)

# One test binary per file in tb/c/
TEST_BINS := $(patsubst $(TB_DIR)/%.c, $(BUILD_DIR)/%, $(TB_SRCS))

# --------------------------------------------------------------------------- #
# Targets
# --------------------------------------------------------------------------- #
.PHONY: all build test coverage clean report

all: build test coverage

build: $(TEST_BINS)

# Link each test binary against all libiommu sources
$(BUILD_DIR)/%: $(TB_DIR)/%.c $(LIBIOMMU_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $@

test: build
	@echo "==> Running test cases..."
	@pass=0; fail=0; \
	for bin in $(TEST_BINS); do \
	    echo "  [RUN] $$bin"; \
	    if $$bin; then \
	        echo "  [OK]  $$bin"; pass=$$((pass+1)); \
	    else \
	        echo "  [FAIL] $$bin (exit $$?)"; fail=$$((fail+1)); \
	    fi; \
	done; \
	echo ""; \
	echo "Results: $$pass passed, $$fail failed"; \
	test $$fail -eq 0

coverage: test
	@echo "==> Collecting coverage data..."
	mkdir -p coverage
	lcov --capture \
	     --directory $(BUILD_DIR) \
	     --directory $(LIBIOMMU_SRC) \
	     --output-file $(LCOV_INFO) \
	     --rc lcov_branch_coverage=1
	# Strip system headers and upstream test harness from report
	lcov --remove $(LCOV_INFO) \
	     '/usr/*' \
	     '*/tb/c/*' \
	     --output-file $(LCOV_INFO_FILT) \
	     --rc lcov_branch_coverage=1
	@echo "==> Generating HTML report -> $(COV_DIR)"
	genhtml $(LCOV_INFO_FILT) \
	        --output-directory $(COV_DIR) \
	        --branch-coverage \
	        --title "riscv-iommu ref model fault coverage"
	@echo ""
	@echo "Open $(COV_DIR)/index.html in a browser to view the report."

# Print a short coverage summary to stdout (useful for CI)
report: coverage
	lcov --summary $(LCOV_INFO_FILT) --rc lcov_branch_coverage=1

clean:
	rm -rf $(BUILD_DIR) coverage
	find . -name '*.gcda' -delete
	find . -name '*.gcno' -delete
