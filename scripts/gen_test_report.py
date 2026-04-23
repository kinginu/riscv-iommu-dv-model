#!/usr/bin/env python3
"""Generate a human-readable test report from test-binary stdout.

Input:  paths to log files that contain lines starting with "REPORT\t".
Output: coverage/test_report.txt (plain) and coverage/test_report.md (markdown).

The point of the report is NOT just to show pass/fail. It is to make it
obvious *why* each case failed (cause mismatch? iotval2? no fault at
all?) so that you can:
  - fix the test's expected values if the ref model behaves differently
    than the spec-matrix assumed, and
  - identify coverage holes and draft new cases.
"""

from __future__ import annotations

import os
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Iterable

REPORT_PREFIX = "REPORT\t"

# Short, human-friendly names for RISC-V IOMMU fault causes we currently
# exercise. Keep it a dict rather than an enum so unknown codes pass
# through untouched.
CAUSE_NAMES = {
    0:  "no-fault",
    1:  "instr-access-fault",
    4:  "rd-access-fault",
    5:  "rd-access-fault (IOB PMP/PMA)",
    7:  "st/amo-access-fault",
    12: "instr-page-fault",
    13: "load-page-fault",
    15: "store/amo-page-fault",
    20: "instr-guest-page-fault",
    21: "load-guest-page-fault",
    23: "store/amo-guest-page-fault",
    256: "all-inbound-disallowed",
    258: "misconfigured-DC",
    260: "tr-type-disallowed",
}

REASON_HINTS = {
    "ok":                "expected behaviour confirmed",
    "status_mismatch":   "response status != expected (translate succeeded but fault expected, or vice-versa)",
    "no_fault":          "expected a fault, ref model logged none - walk likely succeeded",
    "unexpected_fault":  "expected success, ref model logged a fault",
    "cause_mismatch":    "fault raised but with a different cause code",
    "iotval_mismatch":   "fault raised with wrong iotval (should be original IOVA)",
    "iotval2_mismatch":  "fault raised with wrong iotval2 (GPA>>2 | bits); check test expectation",
    "did_mismatch":      "fault raised against a different device id",
    "other":             "passed struct checks but ref model still reported fail",
}


@dataclass
class Case:
    test: str
    rw: str
    iova: int
    exp_status: int
    act_status: int
    exp_cause: int
    act_cause: int
    exp_iotval2: int
    act_iotval2: int
    act_iotval: int
    act_ttyp: int
    act_fault_present: int
    result: str
    reason: str
    source: str = ""       # which log file this row came from
    raw: str = field(default="", repr=False)

    @classmethod
    def parse(cls, line: str, source: str = "") -> "Case | None":
        # The RUN_TEST harness prints `"  NAME : "` before the test runs
        # and `"PASS"` / `"FAIL"` after, without flushing a newline in
        # between. That means a "REPORT\t..." line emitted by
        # check_and_report() typically appears in the middle of a line
        # like:  "  GS-001 ...  : REPORT\t...\n"
        # so we search for the prefix instead of requiring it at column 0.
        idx = line.find(REPORT_PREFIX)
        if idx < 0:
            return None
        body = line[idx + len(REPORT_PREFIX):].rstrip("\n")
        # Strip trailing ANSI escapes that sometimes follow (PASS/FAIL colour).
        body = re.sub(r"\x1b\[[0-9;]*m.*$", "", body)
        fields = {}
        for part in body.split("\t"):
            if "=" not in part:
                continue
            k, _, v = part.partition("=")
            fields[k] = v

        def as_int(k: str) -> int:
            v = fields.get(k, "0")
            try:
                return int(v, 0)
            except ValueError:
                return 0

        return cls(
            test=fields.get("test", "?"),
            rw=fields.get("rw", "?"),
            iova=as_int("iova"),
            exp_status=as_int("exp_status"),
            act_status=as_int("act_status"),
            exp_cause=as_int("exp_cause"),
            act_cause=as_int("act_cause"),
            exp_iotval2=as_int("exp_iotval2"),
            act_iotval2=as_int("act_iotval2"),
            act_iotval=as_int("act_iotval"),
            act_ttyp=as_int("act_ttyp"),
            act_fault_present=as_int("act_fault_present"),
            result=fields.get("result", "?"),
            reason=fields.get("reason", "?"),
            source=source,
            raw=line.rstrip("\n"),
        )


def cause_label(n: int) -> str:
    name = CAUSE_NAMES.get(n)
    return f"{n} ({name})" if name else str(n)


def load_cases(paths: Iterable[str]) -> list[Case]:
    cases: list[Case] = []
    for p in paths:
        name = os.path.basename(p)
        try:
            with open(p, "r", errors="replace") as f:
                for line in f:
                    c = Case.parse(line, source=name)
                    if c is not None:
                        cases.append(c)
        except FileNotFoundError:
            print(f"warn: {p} not found", file=sys.stderr)
    return cases


# ---------------------------------------------------------------------------- #
# Plain-text report
# ---------------------------------------------------------------------------- #

def render_txt(cases: list[Case]) -> str:
    out = []
    out.append("=" * 78)
    out.append("Test fault-code report")
    out.append("=" * 78)
    out.append("")

    if not cases:
        out.append("(no REPORT lines found in any input log)")
        return "\n".join(out) + "\n"

    total = len(cases)
    pass_n = sum(1 for c in cases if c.result == "PASS")
    fail_n = total - pass_n
    out.append(f"Total cases : {total}")
    out.append(f"Passed      : {pass_n}")
    out.append(f"Failed      : {fail_n}")
    out.append("")
    out.append("Failure reasons:")
    reasons = Counter(c.reason for c in cases if c.result == "FAIL")
    for reason, n in reasons.most_common():
        hint = REASON_HINTS.get(reason, "")
        out.append(f"  {n:3d}  {reason:20s}  {hint}")
    out.append("")

    # One row per case
    header = f"{'Test':40s}  {'RW':5s}  {'exp':26s}  {'actual':26s}  {'result':6s}  reason"
    out.append(header)
    out.append("-" * len(header))
    for c in cases:
        exp = f"cause={c.exp_cause}"
        act = f"cause={c.act_cause}"
        if not c.act_fault_present:
            act = "no-fault-logged"
        out.append(f"{c.test:40.40s}  {c.rw:5s}  {exp:26s}  {act:26s}  {c.result:6s}  {c.reason}")
    out.append("")

    # Coverage hole hints based on which causes we *never* saw
    seen_causes = {c.act_cause for c in cases if c.act_fault_present}
    missing = sorted(set(CAUSE_NAMES) - seen_causes - {0})
    out.append("Coverage hints")
    out.append("-" * 14)
    if missing:
        out.append("Cause codes the ref model never produced in this run:")
        for code in missing:
            out.append(f"  {code:3d}  {CAUSE_NAMES[code]}")
        out.append("These may indicate test-expectation gaps or still-missing cases.")
    else:
        out.append("All known cause codes observed at least once.")
    out.append("")

    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------- #
# Markdown report
# ---------------------------------------------------------------------------- #

def render_md(cases: list[Case]) -> str:
    out = []
    out.append("# Test fault-code report")
    out.append("")

    if not cases:
        out.append("_no REPORT lines found in any input log_")
        return "\n".join(out) + "\n"

    total = len(cases)
    pass_n = sum(1 for c in cases if c.result == "PASS")
    fail_n = total - pass_n
    out.append(f"**Total:** {total}  **Passed:** {pass_n}  **Failed:** {fail_n}")
    out.append("")

    # Failure-reason summary
    out.append("## Failure reasons")
    out.append("")
    out.append("| count | reason | meaning |")
    out.append("|------:|--------|---------|")
    reasons = Counter(c.reason for c in cases if c.result == "FAIL")
    for reason, n in reasons.most_common():
        hint = REASON_HINTS.get(reason, "")
        out.append(f"| {n} | `{reason}` | {hint} |")
    out.append("")

    # Per-driver grouping
    by_source: dict[str, list[Case]] = defaultdict(list)
    for c in cases:
        by_source[c.source or "(unknown)"].append(c)

    for source, rows in sorted(by_source.items()):
        out.append(f"## {source}")
        out.append("")
        out.append("| Test | RW | IOVA | exp cause | act cause | iotval2 exp | iotval2 act | result | reason |")
        out.append("|------|----|------|-----------|-----------|-------------|-------------|--------|--------|")
        for c in rows:
            exp_c = cause_label(c.exp_cause)
            if c.act_fault_present:
                act_c = cause_label(c.act_cause)
            else:
                act_c = "_(no fault)_"
            result_md = "✅ PASS" if c.result == "PASS" else "❌ FAIL"
            out.append(
                f"| `{c.test}` | {c.rw} | `0x{c.iova:016x}` | "
                f"{exp_c} | {act_c} | "
                f"`0x{c.exp_iotval2:016x}` | `0x{c.act_iotval2:016x}` | "
                f"{result_md} | `{c.reason}` |"
            )
        out.append("")

    # Coverage-hole hint
    seen_causes = {c.act_cause for c in cases if c.act_fault_present}
    missing = sorted(set(CAUSE_NAMES) - seen_causes - {0})
    out.append("## Coverage hints")
    out.append("")
    if missing:
        out.append("Cause codes the ref model never produced in this run:")
        out.append("")
        for code in missing:
            out.append(f"- **{code}** — {CAUSE_NAMES[code]}")
        out.append("")
        out.append(
            "These may indicate (a) the test's expected cause was wrong, "
            "(b) a setup gap that prevents the ref model reaching that fault, "
            "or (c) a still-missing test case for that scenario."
        )
    else:
        out.append("All known cause codes observed at least once.")
    out.append("")

    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------- #

def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: gen_test_report.py <log-file> [<log-file>...]", file=sys.stderr)
        return 2
    cases = load_cases(argv[1:])
    os.makedirs("coverage", exist_ok=True)
    with open("coverage/test_report.txt", "w") as f:
        f.write(render_txt(cases))
    with open("coverage/test_report.md", "w") as f:
        f.write(render_md(cases))
    print(f"Wrote coverage/test_report.txt and coverage/test_report.md "
          f"({len(cases)} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
