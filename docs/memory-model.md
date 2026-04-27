# Memory model used by the C testbench

This document describes how memory is modelled in the C-level fault-coverage
testbench under `tb/c/`. **Short version: there is no bus model.** The
reference model and the tests share a single flat byte array; reads and
writes are `memcpy`. AXI, TileLink, ordering, bursts, QoS, ID tracking —
none of that exists at this layer.

The cocotb harness under `tb/cocotb/` is the place where a real RTL DUT
with a bus interface would eventually be driven; today it is only a stub
(see [Cocotb harness](#cocotb-harness)).

## Storage

All system-physical memory is one flat heap allocation:

```c
// iommu_ref_model/test/test_utils.c
int8_t reset_system(uint8_t mem_gb, uint16_t num_vms) {
    if (memory != NULL) free(memory);
    memory = malloc(mem_gb * 1024UL * 1024UL * 1024UL);
    ...
    next_free_page = 0;
}
```

- `memory` is a global `int8_t *` declared by each test (defined in
  `tb/c/test_*_fault_cases.c`).
- `reset_system(1, 65535)` allocates **1 GiB** and resets the page
  allocator. The 1 GiB is the only memory that exists in the simulation
  — there is no MMIO region, no device-side scratch RAM, no separate
  guest RAM. Everything (DDT, PDT, S/VS-stage PTEs, G-stage PTEs, MSI
  page tables, CQ/FQ/PQ rings, the data being "translated") lives
  inside this one buffer.
- A System Physical Address (SPA) is just a byte index into `memory[]`.
  Page Number `PPN` × `PAGESIZE` (= 4096) is the SPA of that page.
- The pages aren't explicitly memset to zero by `reset_system` itself —
  but `malloc` of a fresh 1 GiB block on Linux comes from `mmap` and is
  zero-mapped on first touch, so any unallocated page reads back as
  zero. Test code that builds non-leaf PTEs implicitly relies on this
  ("read back, V==0, allocate, write" pattern in
  `iommu_ref_model/libtables/src/build_*.c`).

## Page allocator

```c
// iommu_ref_model/test/test_utils.c
uint64_t get_free_ppn(uint64_t num_ppn) {
    // align next_free_page up to num_ppn boundary, return it,
    // advance next_free_page, memset the returned pages to 0
}
uint64_t get_free_gppn(uint64_t num_gppn, iohgatp_t iohgatp) {
    // same but tracks per-GSCID via next_free_gpage[gscid]
}
```

A bump allocator. There is no free-list, no reuse, no fragmentation.
Each test starts fresh via `reset_system`, which zeroes
`next_free_page` and the per-GSCID `next_free_gpage[]`.

The two flavours represent two different address spaces:

| Allocator | Returned value is a … | Used for |
|---|---|---|
| `get_free_ppn(n)` | System Physical Page Number (SPA / 4096) | DDT, PDT, FS PT, G-stage PT root, leaf SPA pages, CQ/FQ rings |
| `get_free_gppn(n, iohgatp)` | Guest Physical Page Number (GPA / 4096), namespaced by `iohgatp.GSCID` | iosatp root for 2-stage, leaf GPAs that the G-stage maps to SPA |

The "GSCID namespace" is a fiction — both functions ultimately address
the same `memory[]`. `get_free_gppn` exists so that tests can model a
guest's view of a page (a GPA) which the G-stage page tables then
translate to an SPA elsewhere in `memory[]`.

## Read / write API

```c
// iommu_ref_model/test/tbapi.c
uint8_t read_memory(uint64_t addr, uint8_t size, char *data,
                    uint32_t rcid, uint32_t mcid, uint32_t pma, int endian);
uint8_t write_memory(char *data, uint64_t addr, uint32_t size,
                     uint32_t rcid, uint32_t mcid, uint32_t pma, int endian);
```

The bodies are essentially:

```c
if (addr == access_viol_addr)     return ACCESS_FAULT;
if (addr == data_corruption_addr) return DATA_CORRUPTION;
memcpy(&memory[addr], data, size);   // or memcpy(data, &memory[addr], size)
// optional 4-byte / 8-byte byte-swap when endian == BIG_ENDIAN
```

What this does **not** model:

- **No bus protocol.** No AXI4/AXI4-Lite/AXI-Stream, no TileLink, no
  Wishbone. There are no AR/AW/W/R/B channels, no IDs, no responses.
- **No timing.** Every access is instantaneous. There is no notion of
  outstanding transactions, write ordering across ports, fences, or
  back-pressure.
- **No bursts.** `size` is the exact byte count to copy; `read_memory`
  doesn't fragment into beats.
- **No multi-master arbitration.** The test, the IOMMU model, and the
  table builders all touch `memory[]` directly with no contention.
- **No PMP/PMA region table.** `pma` is a parameter that's threaded
  through but never consulted — see [Faulty access](#faulty-access).
- **No cache, TLB outside the ref model itself.** The model has its
  own IOATC; the host has nothing.
- **No QoS.** `rcid` / `mcid` are passed around but only used by the
  IOMMU to populate fault records.

The two helpers `read_memory_test` / `write_memory_test` are sugar that
hard-codes `rcid=0`, `mcid=0`, `pma=PMA`, `endian=test_endian`
(`LITTLE_ENDIAN` by default).

## Endianness

```c
int test_endian = LITTLE_ENDIAN;   // default in every C test
```

Set by the test before any I/O. When the IOMMU is configured big-endian
(via `DC.tc.SBE` / `fctl.BE`), the read/write helpers byte-swap on the
fly so that the in-memory representation always matches what an actual
big-endian master would have written. The byte-swap only handles
`size == 4` and `size % 8 == 0`; anything else aborts.

## Faulty access

Two globals act as an injection mechanism for the access-fault and
data-corruption paths:

```c
uint64_t access_viol_addr     = -1ULL;   // disabled
uint64_t data_corruption_addr = -1ULL;   // disabled
```

Both `read_memory` and `write_memory` start with:

```c
if (addr == access_viol_addr)     return ACCESS_FAULT;
if (addr == data_corruption_addr) return DATA_CORRUPTION;
```

To test an access-fault path the test sets `access_viol_addr` to the
exact byte address it expects the IOMMU to touch (typically a PTE
address). When the IOMMU's walk reaches that byte, the helper returns
`ACCESS_FAULT`, which the walk maps to the spec's "access fault"
cause (`5` / `7` / `1` depending on read/write/exec). Setting it back
to `-1ULL` disarms the trap.

This is the only PMP/PMA-like behaviour in the simulator. There is no
table of allowed regions, no per-master permissions, nothing. It's a
single-address tripwire.

## How a typical test populates memory

For a single-stage (FS-only, `iohgatp.MODE = Bare`) test, the flow is:

1. `iommu_cap_init()` → `reset_system(1, 65535)` → zero `next_free_page`,
   reset the `iommu_t` state.
2. `enable_cq` / `enable_fq` allocate one bump page each for the
   command/fault rings. CQ/FQ live in the same `memory[]` as everything
   else.
3. `enable_iommu` allocates a bump page for the DDT root and writes
   `ddtp.PPN` to that page.
4. The test builds a `device_context_t` in C, sets
   `DC.fsc.iosatp.PPN = get_free_ppn(1)` (the S-stage page-table
   root), and calls `add_dev_context()` which writes the DC into the
   DDT page.
5. Per leaf PTE the test wants, `add_s_stage_pte()` walks down from
   the iosatp root, allocating non-leaf pages with `get_free_ppn` as
   needed, and writes the leaf PTE into the level-0 page.
6. `send_translation_request(...)` calls into the IOMMU model, which
   walks the DDT → DC → iosatp → leaf entirely via `read_memory`.

For a 2-stage test the same dance happens twice: G-stage PTEs are
inserted with `add_g_stage_pte()` against an `iohgatp` whose `PPN` was
allocated via `get_free_ppn(4)` (the spec requires the G-stage root to
be 16-KiB aligned, so 4 PPNs are reserved at once).

The test never directly observes "the IOMMU did X PTE reads" — it just
sets up the structures, fires a request, and inspects the response /
fault queue.

## Cocotb harness

`tb/cocotb/` contains a thin shim — `ref_model_api.c` exposes the C
reference model as a shared library callable from Python via `ctypes`,
and `test_runner.py` is a stub that loads it. There is currently no
RTL DUT wired up. When a DUT is added, AXI / TileLink driving will
live there, not in `tb/c/`.

The C testbench described in this document does **not** drive any RTL
and is independent of the cocotb side. Its goal is purely to exercise
the reference model's fault paths for code-coverage purposes.

## Implications for test authors

- **Page numbers are tiny.** The first call to `get_free_ppn` returns
  `0`, the second `1`, etc. PPN `0` is therefore a valid, allocatable
  page — see the GS-021/023/025 setup, where the G-stage root sits at
  `iohgatp.PPN = 0xC` (the next-aligned page after CQ/FQ/DDT-root) and
  the iosatp ends up at GPPN `0` for GSCID `1`.
- **There is no notion of "reserved" SPA.** If you want a test PTE to
  point at a page that the IOMMU will then load *data* from, you have
  to allocate that page via `get_free_ppn` so it isn't reused.
- **Memory contents persist across tests within one binary.** Each
  RUN_TEST invocation calls `reset_system` via its own setup helper,
  but the global `memory` pointer is freed and re-malloc'd. Anything
  you cached as a raw SPA across tests is invalid afterwards.
- **No alignment enforcement.** Nothing in `read_memory` rejects an
  unaligned access; the spec's alignment requirements are enforced
  inside the IOMMU walk routines, not at the memory layer.
- **Coverage of access-fault paths is opt-in.** Because those paths
  only fire when `access_viol_addr` is set, the existing FS-023…027
  and GS-017…020 placeholder tests do not actually exercise them; they
  document the spec causes the IOMMU model can't synthesise on its own
  and rely on the test driver to inject if/when needed.
