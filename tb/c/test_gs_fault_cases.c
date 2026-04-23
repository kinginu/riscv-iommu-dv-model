// Copyright (c) 2024
// SPDX-License-Identifier: Apache-2.0
//
// tb/c/test_gs_fault_cases.c
//
// G-stage fault case coverage driver.
// Uses the existing test harness API (tbapi.c / test_app.h / libtables).
//
// Build:  make  (from repo root)
// Report: coverage/html/index.html
//
// How to add a new case:
//   1. Write a test_GSxxx() function.
//   2. Call it from main().
//   3. Run `make` — red lines in the HTML = branches still not hit.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "iommu.h"
#include "tables_api.h"
#include "test_app.h"

// -------------------------------------------------------------------------- //
// Globals (defined in tbapi.c / test_utils.c)
// -------------------------------------------------------------------------- //
extern int8_t  *memory;
extern uint64_t access_viol_addr;
extern uint64_t data_corruption_addr;
extern int      test_endian;

// -------------------------------------------------------------------------- //
// Minimal harness
// -------------------------------------------------------------------------- //
static int g_pass = 0;
static int g_fail = 0;
static int test_num = 0;   // required by START_TEST macro

#define RUN_TEST(name, fn)                        \
    do {                                          \
        printf("  %-48s : ", (name));             \
        if ((fn) == 0) {                          \
            printf("\x1B[32mPASS\x1B[0m\n");     \
            g_pass++;                             \
        } else {                                  \
            printf("\x1B[31mFAIL\x1B[0m\n");     \
            g_fail++;                             \
        }                                         \
    } while (0)

// -------------------------------------------------------------------------- //
// Common setup
//
// add_device() configures a device context entry in the DDT.
// Returns the iohgatp used, which we need for add_g_stage_pte().
//
// Parameters that matter for G-stage fault cases:
//   iohgatp_mode : IOHGATP_Sv39x4 / IOHGATP_Bare
//   iosatp_mode  : IOSATP_Bare  (single G-stage tests)
//   gade         : 0 = SW manages A/D, 1 = HW updates A/D
// -------------------------------------------------------------------------- //
#define DID       1          // device ID used across all tests
#define GSCID     1
#define MEM_GB    1
#define CQ_NPPN   4
#define FQ_NPPN   4

static iommu_t  g_iommu;

static iohgatp_t setup(uint8_t iohgatp_mode, uint8_t gade)
{
    reset_system(MEM_GB, 65535);
    memset(&g_iommu, 0, sizeof(g_iommu));
    enable_cq(&g_iommu, CQ_NPPN);
    enable_fq(&g_iommu, FQ_NPPN);
    enable_iommu(&g_iommu, IOMMU_OFF);    // sets capabilities

    // add_device returns the iohgatp.PPN assigned to this device
    uint64_t gppn = add_device(
        &g_iommu,
        DID, GSCID,
        /*en_ats=*/0, /*en_pri=*/0, /*t2gpa=*/0,
        /*dtf=*/0, /*prpr=*/0,
        /*gade=*/gade, /*sade=*/0,
        /*dpe=*/0, /*sbe=*/0, /*sxl=*/0,
        iohgatp_mode, /*iosatp_mode=*/IOSATP_Bare, /*pdt_mode=*/PDTP_Bare,
        /*msiptp_mode=*/MSIPTP_Off, /*msiptp_pages=*/0,
        /*msi_addr_mask=*/0, /*msi_addr_pattern=*/0);

    iohgatp_t iohgatp = {0};
    iohgatp.MODE = iohgatp_mode;
    iohgatp.PPN  = gppn;
    iohgatp.GSCID = GSCID;
    return iohgatp;
}

// Convenience: build a full valid leaf PTE at given GPA
static void make_valid_leaf(iohgatp_t iohgatp, uint64_t gpa, uint64_t ppn,
                             uint8_t R, uint8_t W, uint8_t X, uint8_t U,
                             uint8_t A, uint8_t D)
{
    gpte_t pte = {0};
    pte.V   = 1;
    pte.R   = R; pte.W = W; pte.X = X; pte.U = U;
    pte.A   = A; pte.D = D;
    pte.PPN = ppn;
    add_g_stage_pte(&g_iommu, iohgatp, gpa, pte, /*level=*/0);
}

// Convenience: send a read translation request and return the RSP status
static int8_t do_read(uint64_t iova)
{
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID,
        /*pid_valid=*/0, /*pid=*/0, /*no_write=*/0,
        /*exec_req=*/0, /*priv_req=*/0, /*is_cxl_dev=*/0,
        ADDR_TYPE_UNTRANSLATED, iova, /*length=*/1, READ,
        &req, &rsp);
    return rsp.status;
}

static int8_t do_write(uint64_t iova)
{
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID,
        0, 0, /*no_write=*/0,
        0, 0, 0,
        ADDR_TYPE_UNTRANSLATED, iova, 1, WRITE,
        &req, &rsp);
    return rsp.status;
}

static int8_t do_exec(uint64_t iova)
{
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID,
        0, 0, 0,
        /*exec_req=*/1, 0, 0,
        ADDR_TYPE_UNTRANSLATED, iova, 1, READ,
        &req, &rsp);
    return rsp.status;
}

// -------------------------------------------------------------------------- //
// Test cases
// -------------------------------------------------------------------------- //

// GS-001  Sv39x4: GPA[63:41] != 0  ->  GUEST_PAGE_FAULT (cause 21)
static int8_t test_GS001(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, /*gade=*/0);
    (void)iohgatp;
    uint64_t bad_gpa = 0x0000040000000000ULL;   // bit 42 set
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, bad_gpa, 1, READ, &req, &rsp);
    // cause 21 = Guest page fault on load/read
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 21, 0);
}

// GS-003  Sv39x4: leaf V=0  ->  GUEST_PAGE_FAULT (cause 21)
static int8_t test_GS003(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000040000000ULL;

    gpte_t pte = {0};
    pte.V = 0;   // invalid
    pte.R = 1; pte.W = 1; pte.U = 1; pte.A = 1; pte.D = 1;
    pte.PPN = 0xDEAD;
    add_g_stage_pte(&g_iommu, iohgatp, gpa, pte, 0);

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 21, 0);
}

// GS-004  Sv39x4: L1 V=0 on store  ->  GUEST_PAGE_FAULT (cause 23)
static int8_t test_GS004(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000080000000ULL;

    // Install an invalid PTE at level 1 (directory level)
    gpte_t pte = {0};
    pte.V = 0;
    add_g_stage_pte(&g_iommu, iohgatp, gpa, pte, 1);

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, WRITE, &req, &rsp);
    // cause 23 = Guest page fault on store
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 23, 0);
}

// GS-006  R=0, W=1 (reserved encoding)  ->  GUEST_PAGE_FAULT (cause 21)
static int8_t test_GS006(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000041000000ULL;

    gpte_t pte = {0};
    pte.V = 1; pte.R = 0; pte.W = 1; pte.U = 1; pte.A = 1; pte.D = 1;
    pte.PPN = 0x1234;
    add_g_stage_pte(&g_iommu, iohgatp, gpa, pte, 0);

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 21, 0);
}

// GS-011  Leaf U=0  ->  GUEST_PAGE_FAULT (cause 21)
// G-stage always requires U=1
static int8_t test_GS011(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000043000000ULL;
    make_valid_leaf(iohgatp, gpa, 0x5678,
        /*R*/1, /*W*/1, /*X*/0, /*U*/0,   // U=0 !
        /*A*/1, /*D*/1);

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 21, 0);
}

// GS-012  Read with PTE.R=0  ->  GUEST_PAGE_FAULT (cause 21)
static int8_t test_GS012(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000044000000ULL;
    make_valid_leaf(iohgatp, gpa, 0xAAAA,
        /*R*/0, /*W*/0, /*X*/1, /*U*/1,   // execute-only
        1, 1);

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 21, 0);
}

// GS-013  Write with PTE.W=0  ->  GUEST_PAGE_FAULT (cause 23)
static int8_t test_GS013(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000044001000ULL;
    make_valid_leaf(iohgatp, gpa, 0xBBBB,
        /*R*/1, /*W*/0, /*X*/0, /*U*/1,   // read-only
        1, 1);

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, WRITE, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 23, 0);
}

// GS-014  Fetch with PTE.X=0  ->  GUEST_PAGE_FAULT (cause 20)
static int8_t test_GS014(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000044002000ULL;
    make_valid_leaf(iohgatp, gpa, 0xCCCC,
        /*R*/1, /*W*/1, /*X*/0, /*U*/1,   // no execute
        1, 1);

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,
        /*exec_req=*/1, 0, 0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    // cause 20 = Guest page fault on instruction fetch
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 20, 0);
}

// GS-015  A=0, GADE=0  ->  GUEST_PAGE_FAULT (cause 21)
static int8_t test_GS015(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, /*gade=*/0);
    uint64_t gpa = 0x0000000045000000ULL;
    make_valid_leaf(iohgatp, gpa, 0xDDDD,
        1, 1, 0, 1,
        /*A*/0, /*D*/1);   // A=0 !

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 21, 0);
}

// GS-016  D=0, store, GADE=0  ->  GUEST_PAGE_FAULT (cause 23)
static int8_t test_GS016(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, /*gade=*/0);
    uint64_t gpa = 0x0000000045001000ULL;
    make_valid_leaf(iohgatp, gpa, 0xEEEE,
        1, 1, 0, 1,
        /*A*/1, /*D*/0);   // D=0 !

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, WRITE, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_FAULT, 23, 0);
}

// GS-026  iohgatp=Bare  ->  success (PA == GPA)
static int8_t test_GS026(void)
{
    setup(IOHGATP_Bare, 0);
    uint64_t gpa = 0x0000000001500000ULL;

    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    // Bare mode: no fault, translated PA == GPA
    return check_rsp_and_faults(&g_iommu, &req, &rsp, IOMMU_RSP_SUCCESS, 0, 0);
}

// -------------------------------------------------------------------------- //
// main
// -------------------------------------------------------------------------- //
int main(void)
{
    printf("=== G-stage fault case tests ===\n\n");

    RUN_TEST("GS-001 GPA[63:41]!=0",          test_GS001());
    RUN_TEST("GS-003 leaf V=0",               test_GS003());
    RUN_TEST("GS-004 L1 V=0 on store",        test_GS004());
    RUN_TEST("GS-006 R=0,W=1 reserved",       test_GS006());
    RUN_TEST("GS-011 leaf U=0",               test_GS011());
    RUN_TEST("GS-012 read, PTE.R=0",          test_GS012());
    RUN_TEST("GS-013 write, PTE.W=0",         test_GS013());
    RUN_TEST("GS-014 exec, PTE.X=0",          test_GS014());
    RUN_TEST("GS-015 A=0, GADE=0",            test_GS015());
    RUN_TEST("GS-016 D=0, store, GADE=0",     test_GS016());
    RUN_TEST("GS-026 iohgatp=Bare success",   test_GS026());

    printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail > 0) ? 1 : 0;
}
