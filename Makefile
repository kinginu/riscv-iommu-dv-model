// Copyright (c) 2024
// SPDX-License-Identifier: Apache-2.0
//
// tb/c/test_gs_fault_cases.c

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "iommu.h"
#include "tables_api.h"
#include "test_app.h"

extern int8_t  *memory;
extern uint64_t access_viol_addr;
extern uint64_t data_corruption_addr;
extern int      test_endian;

// -------------------------------------------------------------------------- //
// Harness
// -------------------------------------------------------------------------- //
static int g_pass = 0;
static int g_fail = 0;
static int test_num = 0;  // consumed by START_TEST macro in test_app.h

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
// Constants  (confirmed from iommu_req_rsp.h / iommu_fault.h)
// -------------------------------------------------------------------------- //

// status_t enum values — UPDATE these two if the grep of status_t shows
// different names (e.g. SUCCESS / FAULT instead of the ones below).
// We use numeric literals as a safe fallback until confirmed.
#define STATUS_SUCCESS  0          // status_t: no explicit name, 0 = success
#define STATUS_FAULT    UNSUPPORTED_REQUEST  // status_t enum

// iommu_mode for enable_iommu() — compiler told us the right name:
#define IOMMU_MODE_OFF  RVI_IOMMU_Off

// Avoid macro-name collision with iohgatp_t.GSCID field:
#define MY_GSCID   1
#define MY_DID     1

// -------------------------------------------------------------------------- //
// Setup helper
// -------------------------------------------------------------------------- //
static iommu_t g_iommu;

static iohgatp_t setup(uint8_t iohgatp_mode, uint8_t gade)
{
    reset_system(1 /*mem_gb*/, 65535 /*num_vms*/);
    memset(&g_iommu, 0, sizeof(g_iommu));
    enable_cq(&g_iommu, 4);
    enable_fq(&g_iommu, 4);
    enable_iommu(&g_iommu, IOMMU_MODE_OFF);

    uint64_t gppn = add_device(
        &g_iommu,
        MY_DID, MY_GSCID,
        0, 0, 0,   // en_ats, en_pri, t2gpa
        0, 0,      // dtf, prpr
        gade, 0,   // gade, sade
        0, 0, 0,   // dpe, sbe, sxl
        iohgatp_mode, IOSATP_Bare, PDTP_Bare,
        MSIPTP_Off, 0,
        0, 0);     // msi_addr_mask, msi_addr_pattern

    iohgatp_t iohgatp = {0};
    iohgatp.MODE  = iohgatp_mode;
    iohgatp.PPN   = gppn;
    iohgatp.GSCID = MY_GSCID;
    return iohgatp;
}

// Build a valid leaf PTE at level 0
static void make_leaf(iohgatp_t iohgatp, uint64_t gpa, uint64_t ppn,
                      uint8_t R, uint8_t W, uint8_t X, uint8_t U,
                      uint8_t A, uint8_t D)
{
    gpte_t pte = {0};
    pte.V = 1; pte.R = R; pte.W = W; pte.X = X; pte.U = U;
    pte.A = A; pte.D = D; pte.PPN = ppn;
    add_g_stage_pte(&g_iommu, iohgatp, gpa, pte, 0);
}

// -------------------------------------------------------------------------- //
// Test cases
// -------------------------------------------------------------------------- //

// GS-001  Sv39x4: GPA[63:41] != 0  ->  fault (cause 21)
static int8_t test_GS001(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    (void)iohgatp;
    uint64_t bad_gpa = 0x0000040000000000ULL;  // bit 42 set
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, bad_gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 21, 0);
}

// GS-003  leaf V=0  ->  fault (cause 21)
static int8_t test_GS003(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000040000000ULL;
    gpte_t pte = {0};
    pte.V = 0; pte.R = 1; pte.W = 1; pte.U = 1; pte.A = 1; pte.D = 1;
    pte.PPN = 0xDEAD;
    add_g_stage_pte(&g_iommu, iohgatp, gpa, pte, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 21, 0);
}

// GS-004  L1 V=0 on store  ->  fault (cause 23)
static int8_t test_GS004(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000080000000ULL;
    gpte_t pte = {0};   // V=0 at level 1
    add_g_stage_pte(&g_iommu, iohgatp, gpa, pte, 1);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, WRITE, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 23, 0);
}

// GS-006  R=0, W=1 (reserved)  ->  fault (cause 21)
static int8_t test_GS006(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000041000000ULL;
    gpte_t pte = {0};
    pte.V = 1; pte.R = 0; pte.W = 1; pte.U = 1; pte.A = 1; pte.D = 1;
    pte.PPN = 0x1234;
    add_g_stage_pte(&g_iommu, iohgatp, gpa, pte, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 21, 0);
}

// GS-011  leaf U=0  ->  fault (cause 21)
static int8_t test_GS011(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000043000000ULL;
    make_leaf(iohgatp, gpa, 0x5678, 1,1,0, /*U*/0, 1,1);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 21, 0);
}

// GS-012  read with PTE.R=0  ->  fault (cause 21)
static int8_t test_GS012(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000044000000ULL;
    make_leaf(iohgatp, gpa, 0xAAAA, /*R*/0,0,/*X*/1,1, 1,1);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 21, 0);
}

// GS-013  write with PTE.W=0  ->  fault (cause 23)
static int8_t test_GS013(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000044001000ULL;
    make_leaf(iohgatp, gpa, 0xBBBB, 1,/*W*/0,0,1, 1,1);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, WRITE, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 23, 0);
}

// GS-014  exec with PTE.X=0  ->  fault (cause 20)
static int8_t test_GS014(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, 0);
    uint64_t gpa = 0x0000000044002000ULL;
    make_leaf(iohgatp, gpa, 0xCCCC, 1,1,/*X*/0,1, 1,1);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,
        /*exec_req=*/1, 0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 20, 0);
}

// GS-015  A=0, GADE=0  ->  fault (cause 21)
static int8_t test_GS015(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, /*gade=*/0);
    uint64_t gpa = 0x0000000045000000ULL;
    make_leaf(iohgatp, gpa, 0xDDDD, 1,1,0,1, /*A*/0,1);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 21, 0);
}

// GS-016  D=0, store, GADE=0  ->  fault (cause 23)
static int8_t test_GS016(void)
{
    iohgatp_t iohgatp = setup(IOHGATP_Sv39x4, /*gade=*/0);
    uint64_t gpa = 0x0000000045001000ULL;
    make_leaf(iohgatp, gpa, 0xEEEE, 1,1,0,1, 1,/*D*/0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, WRITE, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 23, 0);
}

// GS-026  iohgatp=Bare  ->  success
static int8_t test_GS026(void)
{
    setup(IOHGATP_Bare, 0);
    uint64_t gpa = 0x0000000001500000ULL;
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, gpa, 1, READ, &req, &rsp);
    return check_rsp_and_faults(&g_iommu, &req, &rsp,
        (status_t)STATUS_SUCCESS, 0, 0);
}

// -------------------------------------------------------------------------- //
// main
// -------------------------------------------------------------------------- //
int main(void)
{
    printf("=== G-stage fault case tests ===\n\n");

    RUN_TEST("GS-001 GPA[63:41]!=0",         test_GS001());
    RUN_TEST("GS-003 leaf V=0",              test_GS003());
    RUN_TEST("GS-004 L1 V=0 on store",       test_GS004());
    RUN_TEST("GS-006 R=0,W=1 reserved",      test_GS006());
    RUN_TEST("GS-011 leaf U=0",              test_GS011());
    RUN_TEST("GS-012 read, PTE.R=0",         test_GS012());
    RUN_TEST("GS-013 write, PTE.W=0",        test_GS013());
    RUN_TEST("GS-014 exec, PTE.X=0",         test_GS014());
    RUN_TEST("GS-015 A=0, GADE=0",           test_GS015());
    RUN_TEST("GS-016 D=0, store, GADE=0",    test_GS016());
    RUN_TEST("GS-026 iohgatp=Bare success",  test_GS026());

    printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail > 0) ? 1 : 0;
}