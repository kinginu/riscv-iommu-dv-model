# riscv-iommu-ref-cov

**G-stage fault coverage harness for the RISC-V IOMMU reference model.**

Fork of [riscv-non-isa/riscv-iommu](https://github.com/riscv-non-isa/riscv-iommu) that adds:

| What | Where |
|------|-------|
| devcontainer (gcc + lcov + cocotb + iverilog) | `.devcontainer/` |
| Makefile with `--coverage` build | `Makefile` |
| C testbench skeleton for GS-xxx fault cases | `tb/c/` |
| cocotb testbench that cross-checks DUT vs ref model | `tb/cocotb/` |
| GitHub Actions workflow (runs on every new test file) | `.github/workflows/ref-model-coverage.yml` |

---

## Quick start (devcontainer)

```bash
# 1. Clone with submodule
git clone --recurse-submodules https://github.com/<your-org>/riscv-iommu-ref-cov.git
cd riscv-iommu-ref-cov

# 2. Open in VS Code → "Reopen in Container"
#    (or run `.devcontainer/setup.sh` manually on Ubuntu 22.04)

# 3. Build ref model with gcov instrumentation + run C tests
make

# 4. Open the coverage report
#    coverage/html/index.html
```

---

## Running the suite locally (without a container)

The CI workflow (`.github/workflows/ref-model-coverage.yml`) is just a
thin wrapper around the Makefile, so you can reproduce every CI step
locally with the same `make` targets.

### Dependencies

```bash
sudo apt-get install -y gcc lcov python3-pip iverilog
pip3 install cocotb pytest coverage lcov_cobertura
```

`iverilog` and `cocotb` are only required for the cocotb cross-check
flow — skip them if you only run the C testbench.

### Make targets

| Target | What it does | Output |
|---|---|---|
| `make build` | Builds each `tb/c/*.c` against the ref model with `--coverage`. | `build/<test>` |
| `make test` | Runs every test binary, captures stdout. | `build/logs/<test>.log` |
| `make test-report` | Aggregates the per-case `REPORT` lines. | `coverage/test_report.{txt,md}` |
| `make coverage` | Runs `lcov` over the `.gcda` files and renders HTML. | `coverage/html/index.html`, `coverage/lcov_filtered.info` |
| `make report` | Prints the lcov summary to stdout. | — |
| `make all` | `build + test + coverage` (no `test-report`). | as above |
| `make clean` | Removes `build/`, `coverage/`, all `.gcda` / `.gcno`. | — |

### One-shot reproduction of the CI pipeline

```bash
make build
make test          # `continue-on-error: true` in CI; fine if some cases FAIL
make test-report
make coverage
# Optional: Cobertura XML for PR annotations (CI uploads this too)
lcov_cobertura coverage/lcov_filtered.info \
    --output coverage/cobertura.xml --demangle
```

Open `coverage/html/index.html` for the line/branch coverage report and
`coverage/test_report.md` for the per-case pass/fail summary.

---

## How coverage verification works

```
┌─────────────────────────────────────────────────────┐
│  tb/c/test_gs_fault_cases.c    ← your GS-xxx inputs │
│         │                                            │
│         ▼                                            │
│  libiommu (built with --coverage / gcov)             │
│         │                                            │
│         ▼                                            │
│  .gcda files  ──► lcov ──► HTML report               │
│                                                      │
│  Red lines in report = fault branches NOT yet hit    │
└─────────────────────────────────────────────────────┘
```

Every `return GST_PAGE_FAULT;` / `return GST_ACCESS_FAULT;` line in
`iommu_second_stage_trans.c` (and the other ref-model sources) is tracked
as a **branch**.  When a line stays red after all test cases are run, you
know you need an additional input case to cover it.

---

## Adding a new GS-xxx test case

### Option A — C testbench
1. Add a `test_GS0xx_*()` function to `tb/c/test_gs_fault_cases.c`.
2. Call it from `main()`.
3. Run `make` — the HTML report will update automatically.

### Option B — cocotb (RTL cross-check)
1. Export the fault-case spreadsheet as TSV →
   `tb/cocotb/gs_fault_cases.csv`.
2. Run cocotb against your RTL simulator:
   ```bash
   cd tb/cocotb
   make SIM=icarus TOPLEVEL=iommu_top MODULE=test_gs_faults
   ```
3. The cocotb run exercises the **same ref-model shared library**
   (built with `--coverage`), so the lcov data is updated automatically.

---

## CI (GitHub Actions)

The workflow at `.github/workflows/ref-model-coverage.yml` triggers
whenever a file under `tb/` or `upstream/` changes.

```
Push / PR with new tb/** file
          │
          ▼
  1. Checkout (+ submodule)
  2. apt-get: gcc, lcov, iverilog
  3. pip:     cocotb, lcov_cobertura
  4. make build
  5. make test       ← fails if any test case returns wrong code
  6. make coverage   ← fails if line < 80% or branch < 70%
  7. Upload HTML artefact + Cobertura XML
```

The **coverage thresholds** (line 80%, branch 70%) are set in the
workflow file — raise them gradually as you add more cases.

---

## Directory layout

```
riscv-iommu-ref-cov/
├── .devcontainer/
│   ├── devcontainer.json
│   └── setup.sh
├── .github/
│   └── workflows/
│       └── ref-model-coverage.yml
├── upstream/                  ← git submodule: riscv-non-isa/riscv-iommu
├── tb/
│   ├── c/
│   │   └── test_gs_fault_cases.c
│   └── cocotb/
│       ├── test_gs_faults.py
│       └── gs_fault_cases.csv   ← export from your spreadsheet
├── coverage/                  ← generated (git-ignored)
│   ├── lcov.info
│   └── html/
├── Makefile
└── README.md
```

---

## iohgatp.PPN alignment note

Per the IOMMU spec, if `iohgatp.PPN[1:0] != 0` (misaligned 16 KiB root),
the resulting behaviour is **UNSPECIFIED** — not a page fault.  
RTL implementations that silently mask the lower bits to 0 are
**conformant**.  This case is therefore **excluded** from the fault-case
spreadsheet and is not tested as an expected-fault path in the ref model.
