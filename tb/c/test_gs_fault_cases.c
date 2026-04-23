// tb/c/test_gs_fault_cases.c
//
// Example C-level test driver for second_stage_address_translation().
// Each test case corresponds to one row in the GS-xxx spreadsheet.
//
// How to add a new case:
//   1. Fill in an iommu_t + iohgatp_t fixture that sets up the desired
//      PTE state (use the helper macros below).
//   2. Call run_test() with the expected return code.
//   3. Re-run `make` — the coverage report will show which branches in
//      iommu_second_stage_trans.c are now newly hit.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "iommu.h"

// -------------------------------------------------------------------------- //
// Minimal test harness
// -------------------------------------------------------------------------- //
static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(name, got, expected)                                     \
    do {                                                                   \
        if ((got) == (expected)) {                                         \
            printf("  [PASS] %-40s  got=0x%02x\n", (name), (got));        \
            g_pass++;                                                      \
        } else {                                                           \
            printf("  [FAIL] %-40s  got=0x%02x  expected=0x%02x\n",       \
                   (name), (got), (expected));                             \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

// -------------------------------------------------------------------------- //
// Shared IOMMU fixture — call before each test
// -------------------------------------------------------------------------- //
static void init_iommu_sv39x4(iommu_t *iommu)
{
    memset(iommu, 0, sizeof(*iommu));

    // Sv39x4 capable, 56-bit PA, Svpbmt disabled, Svrsw60t59b disabled
    iommu->reg_file.capabilities.Sv39x4    = 1;
    iommu->reg_file.capabilities.pas       = 56;
    iommu->reg_file.capabilities.Svpbmt    = 0;
    iommu->reg_file.capabilities.Svrsw60t59b = 0;
    iommu->reg_file.fctl.gxl               = 0;   // 64-bit guest
    iommu->reg_file.fctl.be                = 0;   // little-endian

    iommu->sv39x4_bare_pg_sz = 2UL * 1024 * 1024; // 2 MiB
}

// -------------------------------------------------------------------------- //
// GS-001  GPA size check — Sv39x4: bits [63:41] must be 0
// -------------------------------------------------------------------------- //
static void test_GS001_gpa_size(void)
{
    iommu_t   iommu;
    iohgatp_t iohgatp = { .MODE = IOHGATP_Sv39x4, .PPN = 0x1000 };
    uint64_t  pa, gsz;
    gpte_t    gpte;

    init_iommu_sv39x4(&iommu);

    // GPA with bit 41 set — upper bits != 0
    uint64_t bad_gpa = 0x0000040000000000ULL; // bit 42 set

    uint8_t ret = second_stage_address_translation(
        &iommu,
        bad_gpa, /*check_access_perms=*/1, /*DID=*/0,
        /*is_read=*/1, /*is_write=*/0, /*is_exec=*/0, /*is_implicit=*/0,
        /*PV=*/0, /*PID=*/0, /*PSCV=*/0, /*PSCID=*/0,
        /*GV=*/1, /*GSCID=*/0, iohgatp, /*GADE=*/0, /*SADE=*/0,
        /*SXL=*/0, &pa, &gsz, &gpte,
        /*rcid=*/0, /*mcid=*/0);

    EXPECT_EQ("GS-001: GPA[63:41]!=0 -> GST_PAGE_FAULT", ret, GST_PAGE_FAULT);
}

// -------------------------------------------------------------------------- //
// GS-003  Leaf V=0 -> GST_PAGE_FAULT
// -------------------------------------------------------------------------- //
static void test_GS003_leaf_v0(void)
{
    iommu_t   iommu;
    iohgatp_t iohgatp = { .MODE = IOHGATP_Sv39x4, .PPN = 0x1000 };
    uint64_t  pa, gsz;
    gpte_t    gpte;

    init_iommu_sv39x4(&iommu);

    // Set up page table in simulated memory such that the leaf PTE has V=0.
    // (In a real integration this is done via the memory model stub;
    //  replace with your team's memory-mock API.)
    sim_memory_write_pte(
        (iohgatp.PPN * PAGESIZE) | (vpn2_of(0x0000000040000000ULL) * 8),
        make_pte(.V=1, .R=0, .W=0, .X=0)); // non-leaf L2

    sim_memory_write_pte(
        L1_base_from_above | (vpn1_of(0x0000000040000000ULL) * 8),
        make_pte(.V=1, .R=0, .W=0, .X=0)); // non-leaf L1

    sim_memory_write_pte(
        L0_base_from_above | (vpn0_of(0x0000000040000000ULL) * 8),
        make_pte(.V=0)); // leaf — V=0 → page fault

    uint8_t ret = second_stage_address_translation(
        &iommu,
        0x0000000040000000ULL, 1, 0,
        1, 0, 0, 0,
        0, 0, 0, 0,
        1, 0, iohgatp, 0, 0,
        0, &pa, &gsz, &gpte, 0, 0);

    EXPECT_EQ("GS-003: leaf V=0 -> GST_PAGE_FAULT", ret, GST_PAGE_FAULT);
}

// -------------------------------------------------------------------------- //
// GS-006  R=0, W=1 (reserved encoding) -> GST_PAGE_FAULT
// -------------------------------------------------------------------------- //
static void test_GS006_r0w1(void)
{
    // Similar fixture with a leaf PTE where R=0, W=1
    // This exercises the `gpte->R == 0 && gpte->W == 1` branch in step 3.
    printf("  [TODO] GS-006: add memory-model fixture\n");
}

// -------------------------------------------------------------------------- //
// GS-015  A=0, GADE=0 -> GST_PAGE_FAULT
// -------------------------------------------------------------------------- //
static void test_GS015_a0_gade0(void)
{
    // Exercises the A/D bit path in step 7.
    // Valid leaf PTE but A=0, GADE=0 → fault.
    printf("  [TODO] GS-015: add memory-model fixture\n");
}

// -------------------------------------------------------------------------- //
// main
// -------------------------------------------------------------------------- //
int main(void)
{
    printf("=== G-stage fault case tests ===\n\n");

    test_GS001_gpa_size();
    test_GS003_leaf_v0();
    test_GS006_r0w1();
    test_GS015_a0_gade0();

    printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail > 0) ? 1 : 0;
}
