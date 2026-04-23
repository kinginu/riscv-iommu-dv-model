"""
tb/cocotb/test_runner.py
========================
Generic parametrized cocotb runner.

Reads test cases from YAML, drives the DUT, and optionally cross-checks
the outcome against the C reference model (loaded as a shared library).

Why this shape?
---------------
- One runner = zero per-case Python boilerplate. Adding a test = appending
  YAML.
- Separation of concerns:
    * YAML    — describes WHAT to test
    * runner  — knows HOW to drive the DUT
    * ref_model wrapper — knows HOW to call the golden model
- Parametrized by pytest, so each case shows up as its own test in CI
  (e.g. `pytest tb/cocotb/test_runner.py::test_case[GS-006]`).

Running it
----------
    # DUT-only mode (no golden comparison)
    make SIM=verilator MODULE=test_runner \
         TOPLEVEL=iommu_top \
         CASES=tb/cases/gs_faults.yaml

    # With golden comparison against the C ref model
    REF_MODEL_LIB=build/libiommu_ref.so \
        make SIM=verilator MODULE=test_runner CASES=...
"""

from __future__ import annotations

import os
import yaml
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer


# --------------------------------------------------------------------------- #
# Case model
# --------------------------------------------------------------------------- #
@dataclass
class PTEntry:
    level:   int
    kind:    str          # "leaf" | "non_leaf"
    valid:   int = 1
    ppn:     int = 0
    r:       int = 0
    w:       int = 0
    x:       int = 0
    u:       int = 1
    a:       int = 1
    d:       int = 1
    n:       int = 0      # NAPOT
    pbmt:    int = 0


@dataclass
class Expect:
    status:      str          # "success" | "fault"
    cause:       int | None = None
    fault_stage: str | None = None  # "G" | "VS" | None
    pa:          int | None = None


@dataclass
class Case:
    id:          str
    description: str
    # config (falls back to defaults)
    iohgatp_mode: str = "Sv39x4"
    iosatp_mode:  str = "Bare"
    gade:         int = 0
    sade:         int = 0
    device_id:    int = 1
    gscid:        int = 1
    privilege:    str = "U"
    # stimulus
    iova:         int = 0
    access:       str = "read"
    # state
    g_stage_ptes:  list[PTEntry] = field(default_factory=list)
    vs_stage_ptes: list[PTEntry] = field(default_factory=list)
    # expectations
    expect: Expect = field(default_factory=lambda: Expect(status="success"))


# --------------------------------------------------------------------------- #
# Loader: YAML -> list[Case]
# --------------------------------------------------------------------------- #
def _interp(value: Any, ctx: dict) -> Any:
    """Interpolate {placeholders} and evaluate simple expressions."""
    if isinstance(value, str):
        # Full-string expression: "{level != 2}" -> bool
        m = re.fullmatch(r"\{(.+?)\}", value)
        if m:
            try:
                return eval(m.group(1), {}, ctx)
            except Exception:
                pass
        # Substring interpolation: "level {level} read"
        return value.format(**ctx)
    if isinstance(value, dict):
        return {k: _interp(v, ctx) for k, v in value.items()}
    if isinstance(value, list):
        return [_interp(v, ctx) for v in value]
    return value


def load_cases(path: str | Path) -> list[Case]:
    with open(path) as f:
        doc = yaml.safe_load(f)

    defaults = doc.get("defaults", {})
    cases: list[Case] = []

    for entry in doc.get("cases", []):
        # Matrix expansion
        matrix = entry.get("matrix") or [{}]
        for params in matrix:
            ctx = {**defaults, **params}
            resolved = _interp({k: v for k, v in entry.items()
                                if k not in ("matrix",)}, ctx)

            cfg     = {**defaults, **resolved.get("config", {})}
            stim    = resolved.get("stimulus", {})
            pt      = resolved.get("pagetables", {}) or {}
            exp_raw = resolved.get("expect", {})

            case = Case(
                id          = resolved["id"],
                description = resolved.get("description", ""),
                iohgatp_mode = cfg.get("iohgatp_mode", "Sv39x4"),
                iosatp_mode  = cfg.get("iosatp_mode",  "Bare"),
                gade         = int(cfg.get("gade", 0)),
                sade         = int(cfg.get("sade", 0)),
                device_id    = int(cfg.get("device_id", 1)),
                gscid        = int(cfg.get("gscid", 1)),
                iova         = int(stim.get("iova", 0)),
                access       = stim.get("access", "read"),
                g_stage_ptes = [PTEntry(**p) for p in pt.get("g_stage", [])],
                vs_stage_ptes= [PTEntry(**p) for p in pt.get("vs_stage", [])],
                expect = Expect(
                    status      = exp_raw.get("status", "success"),
                    cause       = int(exp_raw["cause"]) if "cause" in exp_raw else None,
                    fault_stage = exp_raw.get("fault_stage"),
                    pa          = int(exp_raw["pa"]) if "pa" in exp_raw else None,
                ),
            )
            cases.append(case)

    return cases


# --------------------------------------------------------------------------- #
# DUT driver  (adapt signal names to your RTL)
# --------------------------------------------------------------------------- #
class DUTDriver:
    """Thin wrapper around the DUT BFM — all signal-name knowledge lives here."""

    def __init__(self, dut):
        self.dut = dut
        self.ram = dut.ram  # assume a simple memory model hanging off the DUT

    async def reset(self):
        self.dut.rst_n.value = 0
        self.dut.en_1S_i.value = 0
        self.dut.en_2S_i.value = 0
        for _ in range(5):
            await RisingEdge(self.dut.clk)
        self.dut.rst_n.value = 1
        await RisingEdge(self.dut.clk)

    def install_pte(self, base_ppn: int, vpn: int, pte_word: int):
        addr = (base_ppn << 12) | (vpn * 8)
        self.ram.write(addr, pte_word)

    def configure(self, case: Case, iohgatp_root_ppn: int, iosatp_root_ppn: int):
        self.dut.iohgatp_ppn_i.value = iohgatp_root_ppn
        self.dut.iosatp_ppn_i.value  = iosatp_root_ppn
        self.dut.en_1S_i.value = 1 if case.iosatp_mode  != "Bare" else 0
        self.dut.en_2S_i.value = 1 if case.iohgatp_mode != "Bare" else 0
        self.dut.is_store_i.value = 1 if case.access == "write" else 0
        self.dut.is_fetch_i.value = 1 if case.access == "exec"  else 0

    async def run_translation(self, iova: int, timeout_cycles: int = 1000):
        self.dut.req_iova_i.value = iova
        self.dut.req_valid_i.value = 1
        await RisingEdge(self.dut.clk)
        self.dut.req_valid_i.value = 0

        for _ in range(timeout_cycles):
            await RisingEdge(self.dut.clk)
            if int(self.dut.ptw_error_o.value):
                return {
                    "status": "fault",
                    "cause":  int(self.dut.cause_o.value),
                    "pa":     None,
                }
            if int(self.dut.trans_valid_o.value):
                return {
                    "status": "success",
                    "cause":  0,
                    "pa":     int(self.dut.trans_pa_o.value),
                }
        return {"status": "timeout", "cause": None, "pa": None}


# --------------------------------------------------------------------------- #
# Reference model wrapper (ctypes shim — filled in when libiommu_ref.so exists)
# --------------------------------------------------------------------------- #
class RefModel:
    """
    Thin wrapper around the C reference model compiled as libiommu_ref.so.

    If the shared library is not available, `available` is False and all
    comparisons are skipped (DUT-only mode).
    """
    def __init__(self, lib_path: str | None = None):
        self.available = False
        self.lib = None
        path = lib_path or os.environ.get("REF_MODEL_LIB")
        if path and os.path.exists(path):
            import ctypes
            self.lib = ctypes.CDLL(path)
            self.available = True

    def simulate(self, case: Case) -> dict:
        """Run `case` through the golden model and return {status, cause, pa}."""
        if not self.available:
            return {}
        # TODO: call a thin C entry point that sets up iommu_t, installs
        # PTEs, and returns the outcome. See tb/cocotb/ref_model_api.c for
        # the proposed wrapper signature.
        return {}


# --------------------------------------------------------------------------- #
# The single parametrized test
# --------------------------------------------------------------------------- #
_CASES_FILE = os.environ.get(
    "CASES",
    str(Path(__file__).parent.parent / "cases" / "gs_faults.yaml"),
)
_CASES: list[Case] = load_cases(_CASES_FILE)


def _expand_factory(cases: list[Case]):
    """Register one cocotb test per case so each shows up individually in CI."""

    for case in cases:

        @cocotb.test(name=f"test_case[{case.id}]")
        async def _test(dut, _case=case):   # capture by default arg
            await _run_case(dut, _case)


async def _run_case(dut, case: Case):
    dut._log.info(f"=== {case.id}: {case.description} ===")

    # Clock + reset
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    drv = DUTDriver(dut)
    await drv.reset()

    # Allocate root PPNs for the page tables (deterministic, case-id based)
    iohgatp_root = 0x1000 + hash(case.id) % 0x1000
    iosatp_root  = 0x2000 + hash(case.id) % 0x1000

    # Install G-stage page tables
    # NOTE: the exact PTE word encoding lives in ptw_helpers.PteFactory in
    # your current codebase — reuse that here.
    from ptw_helpers import PteFactory, format_sv39_iova

    iova = case.iova
    vpn2 = (iova >> 30) & 0x1FF
    vpn1 = (iova >> 21) & 0x1FF
    vpn0 = (iova >> 12) & 0x1FF

    # Allocate per-level PPNs (simple fixed scheme for reproducibility)
    lvl1_ppn = iohgatp_root + 1
    lvl0_ppn = iohgatp_root + 2
    for pte in case.g_stage_ptes:
        if pte.level == 2:
            drv.install_pte(iohgatp_root, vpn2,
                PteFactory.s1_non_leaf(ppn=lvl1_ppn, v=pte.valid))
        elif pte.level == 1:
            drv.install_pte(lvl1_ppn, vpn1,
                PteFactory.s1_non_leaf(ppn=lvl0_ppn, v=pte.valid))
        elif pte.level == 0:
            drv.install_pte(lvl0_ppn, vpn0,
                PteFactory.s1_leaf(
                    ppn=pte.ppn, v=pte.valid,
                    r=pte.r, w=pte.w, x=pte.x, u=pte.u,
                    a=pte.a, d=pte.d))

    drv.configure(case, iohgatp_root, iosatp_root)

    # ------------------------------------------------------------------ #
    # Drive DUT
    # ------------------------------------------------------------------ #
    dut_result = await drv.run_translation(iova)
    dut._log.info(f"DUT result: {dut_result}")

    # ------------------------------------------------------------------ #
    # Compare against expected
    # ------------------------------------------------------------------ #
    assert dut_result["status"] == case.expect.status, (
        f"[{case.id}] status mismatch: DUT={dut_result['status']} "
        f"expected={case.expect.status}"
    )
    if case.expect.cause is not None:
        assert dut_result["cause"] == case.expect.cause, (
            f"[{case.id}] cause mismatch: DUT=0x{dut_result['cause']:x} "
            f"expected=0x{case.expect.cause:x}"
        )
    if case.expect.pa is not None:
        assert dut_result["pa"] == case.expect.pa, (
            f"[{case.id}] PA mismatch: DUT=0x{dut_result['pa']:x} "
            f"expected=0x{case.expect.pa:x}"
        )

    # ------------------------------------------------------------------ #
    # Golden cross-check (optional)
    # ------------------------------------------------------------------ #
    ref = RefModel()
    if ref.available:
        ref_result = ref.simulate(case)
        assert ref_result["status"] == dut_result["status"], (
            f"[{case.id}] DUT/ref status mismatch: "
            f"DUT={dut_result} ref={ref_result}"
        )
        if ref_result.get("cause") is not None:
            assert ref_result["cause"] == dut_result["cause"], (
                f"[{case.id}] DUT/ref cause mismatch: "
                f"DUT=0x{dut_result['cause']:x} ref=0x{ref_result['cause']:x}"
            )

    dut._log.info(f"  ★ {case.id} PASS")


# Register all cases as individual cocotb tests
_expand_factory(_CASES)
