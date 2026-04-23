# =============================================================================
# riscv-iommu-dv-model  —  Makefile
# =============================================================================

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

CC      := gcc
CFLAGS  := -O0 -g \
            -I$(LIBIOMMU_INC) \
            -I$(LIBTABLES_INC) \
            -I$(TEST_INC) \
            --coverage \
            -fprofile-arcs \
            -ftest-coverage \
            -Wno-unused-variable \
            -Wno-implicit-fallthrough \
            -Wno-sign-compare \
            -Wno-type-limits
LDFLAGS := --coverage -lm

LIBIOMMU_SRCS   := $(wildcard $(LIBIOMMU_SRC)/*.c)
LIBTABLES_SRCS  := $(wildcard $(LIBTABLES_SRC)/*.c)
TBAPI_SRCS      := $(REFMODEL_ROOT)/test/tbapi.c \
                   $(REFMODEL_ROOT)/test/test_utils.c
ALL_LIB_SRCS    := $(LIBIOMMU_SRCS) $(LIBTABLES_SRCS) $(TBAPI_SRCS)

TB_SRCS   := $(wildcard $(TB_DIR)/*.c)
TEST_BINS := $(patsubst $(TB_DIR)/%.c, $(BUILD_DIR)/%, $(TB_SRCS))

.PHONY: all build test test-strict test-report coverage report clean

all: build test coverage

# `build` is a phony target that just triggers binary compilation.
# Each binary rule itself ensures `build/` exists via `mkdir -p` in the
# recipe (no order-only prereq needed — avoids phony-vs-directory confusion).
build: $(TEST_BINS)

$(BUILD_DIR)/%: $(TB_DIR)/%.c $(ALL_LIB_SRCS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: build
	@echo "==> Running test cases..."
	@mkdir -p $(BUILD_DIR)/logs
	@pass=0; fail=0; \
	for bin in $(TEST_BINS); do \
	    log=$(BUILD_DIR)/logs/$$(basename $$bin).log; \
	    echo "  [RUN] $$bin"; \
	    if $$bin > $$log 2>&1; then \
	        echo "  [OK]  $$bin"; pass=$$((pass+1)); \
	    else \
	        echo "  [FAIL] $$bin  (exit $$?)"; fail=$$((fail+1)); \
	    fi; \
	    tail -n +1 $$log; \
	done; \
	echo ""; \
	echo "Results: $$pass passed, $$fail failed"

test-report: test
	@echo "==> Generating human-readable test report..."
	@python3 scripts/gen_test_report.py $(BUILD_DIR)/logs/*.log

test-strict: build
	@echo "==> Running test cases (strict mode)..."
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
	@mkdir -p coverage
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
	genhtml $(LCOV_INFO_FILT) \
	        --output-directory $(COV_DIR) \
	        --branch-coverage \
	        --title "riscv-iommu ref model fault coverage"
	@echo "Open $(COV_DIR)/index.html to view the report."

report: coverage
	lcov --summary $(LCOV_INFO_FILT) --rc lcov_branch_coverage=1

clean:
	rm -rf $(BUILD_DIR) coverage
	find . -name '*.gcda' -delete
	find . -name '*.gcno' -delete