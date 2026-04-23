# =============================================================================
# riscv-iommu-dv-model  —  Makefile
#
# Builds libiommu + libtables reference model with gcov coverage
# instrumentation, links against tb/c/ test drivers, then generates
# an lcov HTML coverage report.
#
# Usage:
#   make              — build + test + HTML report
#   make build        — compile only
#   make test         — run test binaries
#   make coverage     — collect gcov data, generate HTML
#   make clean        — remove build artefacts and coverage data
# =============================================================================

# --------------------------------------------------------------------------- #
# Paths  (relative to repo root)
# --------------------------------------------------------------------------- #
REFMODEL_ROOT   := iommu_ref_model
LIBIOMMU_INC    := $(REFMODEL_ROOT)/libiommu/include
LIBIOMMU_SRC    := $(REFMODEL_ROOT)/libiommu/src
LIBTABLES_INC   := $(REFMODEL_ROOT)/libtables/include
LIBTABLES_SRC   := $(REFMODEL_ROOT)/libtables/src
TEST_INC        := $(REFMODEL_ROOT)/test

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
            -I$(LIBTABLES_INC) \
            -I$(TEST_INC) \
            --coverage \
            -fprofile-arcs \
            -ftest-coverage \
            -Wall -Wextra -Wno-unused-parameter
LDFLAGS := --coverage -lm

# --------------------------------------------------------------------------- #
# Sources
# --------------------------------------------------------------------------- #
LIBIOMMU_SRCS   := $(wildcard $(LIBIOMMU_SRC)/*.c)
LIBTABLES_SRCS  := $(wildcard $(LIBTABLES_SRC)/*.c)
# tbapi.c provides the memory model stub used by ref model internals
TBAPI_SRCS      := $(REFMODEL_ROOT)/test/tbapi.c \
                   $(REFMODEL_ROOT)/test/test_utils.c

ALL_LIB_SRCS    := $(LIBIOMMU_SRCS) $(LIBTABLES_SRCS) $(TBAPI_SRCS)

# One test binary per .c file in tb/c/
TB_SRCS   := $(wildcard $(TB_DIR)/*.c)
TEST_BINS := $(patsubst $(TB_DIR)/%.c, $(BUILD_DIR)/%, $(TB_SRCS))

# --------------------------------------------------------------------------- #
# Targets
# --------------------------------------------------------------------------- #
.PHONY: all build test coverage report clean

all: build test coverage

build: $(BUILD_DIR) $(TEST_BINS)

$(BUILD_DIR):
	mkdir -p $@

# Each test binary = its own tb/c/*.c  +  all library sources
$(BUILD_DIR)/%: $(TB_DIR)/%.c $(ALL_LIB_SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: build
	@echo "==> Running test cases..."
	@pass=0; fail=0; \
	for bin in $(TEST_BINS); do \
	    echo "  [RUN] $$bin"; \
	    if $$bin; then \
	        echo "  [OK]  $$bin"; pass=$$((pass+1)); \
	    else \
	        echo "  [FAIL] $$bin  (exit $$?)"; fail=$$((fail+1)); \
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
	     --output-file $(LCOV_INFO) \
	     --rc lcov_branch_coverage=1
	lcov --remove $(LCOV_INFO) \
	     '/usr/*' \
	     '*/test/tbapi.c' \
	     '*/test/test_utils.c' \
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

report: coverage
	lcov --summary $(LCOV_INFO_FILT) --rc lcov_branch_coverage=1

clean:
	rm -rf $(BUILD_DIR) coverage
	find . -name '*.gcda' -delete
	find . -name '*.gcno' -delete