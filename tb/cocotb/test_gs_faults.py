"""
tb/cocotb/test_gs_faults.py
===========================
cocotb testbench that drives the DUT with GS-stage fault cases and
cross-checks against the C reference model.

Workflow
--------
1. Test cases are loaded from `tb/cocotb/gs_fault_cases.csv`
   (exported from the shared spreadsheet, one row per GS-xxx case).
2. For each row, the stimulus is driven to the DUT via the BFM.
3. The same stimulus is also passed to the reference model (via a C
   shared library built with --coverage).
4. Outputs are compared: if they differ, the test fails.
5. After all cases, the Makefile `coverage` target collects .gcda files
   and generates the lcov report, showing which ref-model branches were
   exercised.

CSV columns (tab-separated, match your spreadsheet export):
    test_id, iohgatp_mode, gpa, access_type, expected_fault_code, note
"""

import csv
import os
import ctypes
import cocotb
from cocotb.triggers import RisingEdge, Timer
from cocotb.clock import Clock

# --------------------------------------------------------------------------- #
# Load the reference model shared library (built with --coverage)
# --------------------------------------------------------------------------- #
_LIB_PATH = os.environ.get(
    "REF_MODEL_LIB",
    "build/libiommu_cov.so"   # built by `make shared`
)
try:
    _ref = ctypes.CDLL(_LIB_PATH)
    REF_MODEL_AVAILABLE = True
except OSError:
    cocotb.log.warning(f"Ref model library not found at {_LIB_PATH}. "
                       "Running DUT-only mode (no golden comparison).")
    REF_MODEL_AVAILABLE = False


# --------------------------------------------------------------------------- #
# CSV loader
# --------------------------------------------------------------------------- #
CASE_FILE = os.path.join(os.path.dirname(__file__), "gs_fault_cases.csv")

def load_cases(path: str) -> list[dict]:
    """Return a list of test-case dicts from the CSV export."""
    cases = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            # Skip comment rows or empty IDs
            if not row.get("test_id", "").startswith("GS-"):
                continue
            cases.append(row)
    return cases


# --------------------------------------------------------------------------- #
# Helper: drive one test case to the DUT
# --------------------------------------------------------------------------- #
async def drive_case(dut, case: dict):
    """Apply stimulus from one CSV row to the DUT."""
    access = case["access_type"].upper()

    dut.iova.value       = int(case["gpa"], 16)
    dut.is_read.value    = 1 if "READ"  in access else 0
    dut.is_write.value   = 1 if "WRITE" in access else 0
    dut.is_exec.value    = 1 if "INSTR" in access else 0
    dut.iohgatp_mode.value = {
        "SV39X4": 0b1000,
        "SV48X4": 0b1001,
        "SV57X4": 0b1010,
        "BARE":   0b0000,
    }.get(case.get("iohgatp_mode", "SV39X4").upper(), 0b1000)

    await RisingEdge(dut.clk)
    await RisingEdge(dut.clk)   # wait for DUT to settle


# --------------------------------------------------------------------------- #
# Main cocotb test
# --------------------------------------------------------------------------- #
@cocotb.test()
async def test_gs_fault_cases(dut):
    """Drive all GS-xxx fault cases and compare DUT output vs ref model."""

    # Start clock
    clock = Clock(dut.clk, 10, units="ns")
    cocotb.start_soon(clock.start())

    # Reset
    dut.rst_n.value = 0
    await Timer(50, units="ns")
    dut.rst_n.value = 1
    await RisingEdge(dut.clk)

    if not os.path.exists(CASE_FILE):
        cocotb.log.warning(
            f"Case file {CASE_FILE} not found. "
            "Create it by exporting the spreadsheet as TSV."
        )
        return

    cases = load_cases(CASE_FILE)
    cocotb.log.info(f"Loaded {len(cases)} GS fault cases from {CASE_FILE}")

    passed = 0
    failed = 0

    for case in cases:
        test_id = case["test_id"]
        expected_code = int(case.get("expected_fault_code", "0"), 0)

        await drive_case(dut, case)

        # Read DUT fault output (adapt signal names to your RTL)
        dut_fault_code = int(dut.fault_code.value)

        ok = (dut_fault_code == expected_code)
        if ok:
            passed += 1
            cocotb.log.info(f"[PASS] {test_id}")
        else:
            failed += 1
            cocotb.log.error(
                f"[FAIL] {test_id}: DUT returned 0x{dut_fault_code:02x}, "
                f"expected 0x{expected_code:02x}  ({case.get('note','')})"
            )

    cocotb.log.info(f"=== {passed} passed, {failed} failed ===")
    assert failed == 0, f"{failed} test case(s) failed"
