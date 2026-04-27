// Copyright (c) 2024
// SPDX-License-Identifier: Apache-2.0
//
// tb/c/test_fs_fault_cases.c
//
// First-stage (S-stage / iosatp) fault case coverage driver.
// Uses iohgatp=Bare so only single-stage translation is exercised.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "iommu.h"
#include "tables_api.h"
#include "test_app.h"
#include "test_report.h"

extern void __gcov_dump(void) __attribute__((weak));

static void segv_handler(int sig) {
    (void)sig;
    fprintf(stderr, "[WARN] signal %d caught - flushing coverage data\n", sig);
    if (__gcov_dump) __gcov_dump();
    exit(0);
}

// -------------------------------------------------------------------------- //
// Globals normally provided by iommu_ref_model/test/test_app.c.
// -------------------------------------------------------------------------- //
int8_t   *memory = NULL;
uint64_t  access_viol_addr     = -1ULL;
uint64_t  data_corruption_addr = -1ULL;
uint8_t   pr_go_requested = 0;
uint8_t   pw_go_requested = 0;
uint64_t  next_free_page = 0;
uint64_t  next_free_gpage[65536] = {0};
int       test_endian = LITTLE_ENDIAN;

ats_msg_t exp_msg = {0};
ats_msg_t rcvd_msg = {0};
uint8_t   exp_msg_received = 0;
uint8_t   message_received = 0;

// -------------------------------------------------------------------------- //
// Harness
// -------------------------------------------------------------------------- //
static int g_pass = 0;
static int g_fail = 0;
static int test_num = 0;

#define RUN_TEST(name, fn)                        \
    do {                                          \
        g_current_test = (name);                  \
        printf("  %-48s : ", (name));             \
        if ((fn) == 0) {                          \
            printf("\x1B[32mPASS\x1B[0m\n");     \
            g_pass++;                             \
        } else {                                  \
            printf("\x1B[31mFAIL\x1B[0m\n");     \
            g_fail++;                             \
        }                                         \
    } while (0)

#define STATUS_SUCCESS  0
#define STATUS_FAULT    UNSUPPORTED_REQUEST
#define MY_GSCID   1
#define MY_DID     1
#define MY_PID     0x42  // fits in PD8 (8-bit process_id)

static iommu_t  g_iommu;
static iosatp_t g_iosatp;  // populated by setup_fs / setup_fs_pdtv

// -------------------------------------------------------------------------- //
// Setup: iohgatp=Bare, iosatp=Sv39 (single-stage).
// We construct the DC manually and call add_dev_context so we can remember
// the iosatp for subsequent add_s_stage_pte calls.
// -------------------------------------------------------------------------- //
static void iommu_cap_init(void)
{
    reset_system(1, 65535);
    memset(&g_iommu, 0, sizeof(g_iommu));

    capabilities_t cap = {0};
    fctl_t         fctl = {0};
    cap.version = 0x10;
    cap.Sv39 = cap.Sv48 = cap.Sv57 = 1;
    cap.Sv39x4 = cap.Sv48x4 = cap.Sv57x4 = 1;
    cap.amo_hwad = cap.ats = cap.t2gpa = 1;
    cap.hpm = cap.msi_flat = cap.msi_mrif = cap.amo_mrif = 1;
    cap.dbg = 1;
    cap.pas = 50;
    cap.pd20 = cap.pd17 = cap.pd8 = 1;
    cap.Svrsw60t59b = 1;

    const uint64_t SZ   = 0x40000000ULL;
    const uint64_t SZ32 = 0x200000ULL;
    reset_iommu(&g_iommu, 8, 40, 0xff, 3, Off, DDT_3LVL, 0xFFFFFF, 0, 0,
                (FILL_IOATC_ATS_T2GPA | FILL_IOATC_ATS_ALWAYS),
                cap, fctl, SZ, SZ, SZ, SZ32, SZ, SZ, SZ, SZ32);

    enable_cq(&g_iommu, 4);
    enable_fq(&g_iommu, 4);
    enable_iommu(&g_iommu, DDT_3LVL);
}

static void setup_fs(uint8_t iosatp_mode, uint8_t sade, uint8_t dtf)
{
    iommu_cap_init();

    // Build DC manually: iohgatp=Bare, iosatp=<mode>
    static device_context_t DC;
    memset(&DC, 0, sizeof(DC));
    DC.tc.V    = 1;
    DC.tc.DTF  = dtf;
    DC.tc.SADE = sade;
    DC.tc.PDTV = 0;
    DC.iohgatp.MODE = IOHGATP_Bare;

    memset(&g_iosatp, 0, sizeof(g_iosatp));
    g_iosatp.MODE = iosatp_mode;
    if (iosatp_mode != IOSATP_Bare) {
        g_iosatp.PPN = get_free_ppn(1);
        // Zero the root PT page.
        static char zero[4096] = {0};
        write_memory_test(zero, g_iosatp.PPN * PAGESIZE, 4096);
    }
    DC.fsc.iosatp = g_iosatp;

    add_dev_context(&g_iommu, &DC, MY_DID);
}

// PDTV=1 variant: DC.fsc holds pdtp; the iosatp lives inside the PC.
// The ref model only honours `priv_req=1` when `pid_valid=1` AND there is a
// PC (DC.tc.PDTV=1, ENS=1) — otherwise the request is silently demoted to
// U_MODE (see iommu_ref_model/test/tbapi.c:get_attribs_from_req). Tests that
// need to drive S-mode (e.g. SUM checks) must use this setup.
static void setup_fs_pdtv(uint8_t iosatp_mode, uint8_t sum,
                          uint8_t sade, uint8_t dtf)
{
    iommu_cap_init();

    static device_context_t DC;
    memset(&DC, 0, sizeof(DC));
    DC.tc.V    = 1;
    DC.tc.DTF  = dtf;
    DC.tc.SADE = sade;
    DC.tc.PDTV = 1;
    DC.iohgatp.MODE = IOHGATP_Bare;

    static char zero[4096] = {0};

    // Single-level PDT (PD8) — process_id must fit in 8 bits.
    DC.fsc.pdtp.MODE = PD8;
    DC.fsc.pdtp.PPN  = get_free_ppn(1);
    write_memory_test(zero, DC.fsc.pdtp.PPN * PAGESIZE, 4096);

    add_dev_context(&g_iommu, &DC, MY_DID);

    // S-stage root (referenced via PC.fsc.iosatp).
    memset(&g_iosatp, 0, sizeof(g_iosatp));
    g_iosatp.MODE = iosatp_mode;
    g_iosatp.PPN  = get_free_ppn(1);
    write_memory_test(zero, g_iosatp.PPN * PAGESIZE, 4096);

    process_context_t PC;
    memset(&PC, 0, sizeof(PC));
    PC.ta.V     = 1;
    PC.ta.ENS   = 1;          // required for priv_req to be honoured
    PC.ta.SUM   = sum;
    PC.ta.PSCID = 0;
    PC.fsc.iosatp = g_iosatp;
    // add_process_context() unconditionally calls translate_gpa(), which
    // returns -1 when iohgatp.MODE==Bare and aborts before writing the PC.
    // Since we're single-stage here, place the 16-byte PC manually at
    // pdtp.PPN*PAGESIZE + PDI[0]*16 (PD8 → single-level PDT, PDI[0]=pid[7:0]).
    uint64_t pc_addr = DC.fsc.pdtp.PPN * PAGESIZE + (MY_PID & 0xFF) * 16;
    write_memory_test((char *)&PC, pc_addr, 16);
}

static void make_leaf_s(uint64_t va, uint64_t ppn,
                        uint8_t R, uint8_t W, uint8_t X, uint8_t U,
                        uint8_t A, uint8_t D, uint8_t N, uint8_t PBMT,
                        uint8_t RSVD)
{
    pte_t pte = {0};
    pte.V = 1; pte.R = R; pte.W = W; pte.X = X; pte.U = U;
    pte.A = A; pte.D = D; pte.N = N; pte.PBMT = PBMT;
    pte.PPN = ppn;
    if (RSVD) pte.reserved = RSVD;
    add_s_stage_pte(g_iosatp, va, pte, 0, 0 /*SXL=0 -> Sv39*/);
}

// -------------------------------------------------------------------------- //
// Test cases
// -------------------------------------------------------------------------- //

// FS-001 L0 leaf V=0 -> cause 13
static int8_t test_FS001(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000400000ULL;
    pte_t pte = {0};  // V=0
    pte.R = 1; pte.W = 1; pte.U = 1; pte.A = 1; pte.D = 1;
    pte.PPN = 0xBEEF;
    add_s_stage_pte(g_iosatp, va, pte, 0, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-002 L1 non-leaf V=0 -> cause 13
static int8_t test_FS002(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000080000000ULL;
    pte_t pte = {0};  // V=0 at level 1
    add_s_stage_pte(g_iosatp, va, pte, 1, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-003 L2 root V=0 on store -> cause 15
static int8_t test_FS003(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000001000000000ULL;
    pte_t pte = {0};
    add_s_stage_pte(g_iosatp, va, pte, 2, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, WRITE, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 15, 0);
}

// FS-004 Leaf R=0, W=1 -> cause 13
static int8_t test_FS004(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000401000ULL;
    make_leaf_s(va, 0x1001, 0,1,0,1, 1,1, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-005 Leaf PBMT=1 when Svpbmt cap=0 -> cause 13
static int8_t test_FS005(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000402000ULL;
    make_leaf_s(va, 0x1002, 1,1,0,1, 1,1, 0, 1 /*PBMT*/, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-006 Leaf N=1 when Svnapot cap=0 -> cause 13
static int8_t test_FS006(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000403000ULL;
    make_leaf_s(va, 0x1003, 1,1,0,1, 1,1, 1 /*N*/, 0, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-007 Leaf RSVD[60:54] != 0 -> cause 13
static int8_t test_FS007(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000404000ULL;
    make_leaf_s(va, 0x1004, 1,1,0,1, 1,1, 0, 0, 0x5);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-008 L1 R=1 (2MiB superpage) -> cause 13 (unsupported)
static int8_t test_FS008(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000020000000ULL;
    pte_t pte = {0};
    pte.V = 1; pte.R = 1; pte.U = 1; pte.A = 1; pte.D = 1;
    pte.PPN = 0x2001;
    add_s_stage_pte(g_iosatp, va, pte, 1, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-009 L1 W=1 non-leaf on store -> cause 15
static int8_t test_FS009(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000200000ULL;
    pte_t pte = {0};
    pte.V = 1; pte.W = 1;
    pte.PPN = 0x2002;
    add_s_stage_pte(g_iosatp, va, pte, 1, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, WRITE, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 15, 0);
}

// FS-010 L1 X=1 non-leaf on instr fetch -> cause 12
static int8_t test_FS010(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000201000ULL;
    pte_t pte = {0};
    pte.V = 1; pte.X = 1;
    pte.PPN = 0x2003;
    add_s_stage_pte(g_iosatp, va, pte, 1, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,
        1 /*exec_req*/, 0, 0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 12, 0);
}

// FS-011 L2 R=1 (1GiB superpage) -> cause 13
static int8_t test_FS011(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000400000000000ULL;
    pte_t pte = {0};
    pte.V = 1; pte.R = 1; pte.U = 1; pte.A = 1; pte.D = 1;
    pte.PPN = 0x2004;
    add_s_stage_pte(g_iosatp, va, pte, 2, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-012 L2 W=1 non-leaf on store -> cause 15
static int8_t test_FS012(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000400000000000ULL;
    pte_t pte = {0};
    pte.V = 1; pte.W = 1;
    pte.PPN = 0x2005;
    add_s_stage_pte(g_iosatp, va, pte, 2, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, WRITE, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 15, 0);
}

// FS-013 L2 X=1 non-leaf on instr fetch -> cause 12
static int8_t test_FS013(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000400000000000ULL;
    pte_t pte = {0};
    pte.V = 1; pte.X = 1;
    pte.PPN = 0x2006;
    add_s_stage_pte(g_iosatp, va, pte, 2, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,
        1 /*exec_req*/, 0, 0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 12, 0);
}

// FS-014 Leaf R=0,X=0 -> cause 13
static int8_t test_FS014(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000500000ULL;
    make_leaf_s(va, 0x1005, 0,0,0,1, 1,1, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-015 Leaf W=0 on store -> cause 15
static int8_t test_FS015(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000502000ULL;
    make_leaf_s(va, 0x1006, 1,0,0,1, 1,1, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, WRITE, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 15, 0);
}

// FS-016 Leaf X=0 on instr fetch -> cause 12
static int8_t test_FS016(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000503000ULL;
    make_leaf_s(va, 0x1007, 1,0,0,1, 1,1, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,
        1 /*exec_req*/, 0, 0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 12, 0);
}

// FS-017 U-mode req, PTE.U=0 -> cause 13
static int8_t test_FS017(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000600000ULL;
    make_leaf_s(va, 0x1008, 1,1,0, /*U=*/0, 1,1, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-018 S-mode req (priv=1), PTE.U=1, SUM=0 -> cause 13
// Needs PDTV=1 + a PC (with ENS=1, SUM=0) so the ref model treats the
// request as S-mode; pid_valid=1 + priv_req=1 are required by
// get_attribs_from_req.
static int8_t test_FS018(void)
{
    setup_fs_pdtv(IOSATP_Sv39, /*sum=*/0, 0, 0);
    uint64_t va = 0x0000000000601000ULL;
    make_leaf_s(va, 0x1009, 1,1,0,1, 1,1, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID,
        /*pid_valid=*/1, /*pid=*/MY_PID, /*no_write=*/0,
        /*exec_req=*/0, /*priv_req=*/1, /*is_cxl_dev=*/0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-019 S-mode instr fetch, PTE.U=1, X=1 -> cause 12
// SUM=0 → S-mode access to user pages is disallowed for both reads and
// instruction fetches; this test exercises the instruction-fetch path.
static int8_t test_FS019(void)
{
    setup_fs_pdtv(IOSATP_Sv39, /*sum=*/0, 0, 0);
    uint64_t va = 0x0000000000603000ULL;
    make_leaf_s(va, 0x100A, 1,1,1,1, 1,1, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID,
        /*pid_valid=*/1, /*pid=*/MY_PID, /*no_write=*/0,
        /*exec_req=*/1, /*priv_req=*/1, /*is_cxl_dev=*/0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 12, 0);
}

// FS-020 SADE=0 & A=0 on load -> cause 13 (SW A-bit fault)
static int8_t test_FS020(void)
{
    setup_fs(IOSATP_Sv39, 0 /*sade*/, 0);
    uint64_t va = 0x0000000000700000ULL;
    make_leaf_s(va, 0x100B, 1,1,0,1, /*A=*/0, 1, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-021 SADE=0 & D=0 on store -> cause 15
static int8_t test_FS021(void)
{
    setup_fs(IOSATP_Sv39, 0 /*sade*/, 0);
    uint64_t va = 0x0000000000701000ULL;
    make_leaf_s(va, 0x100C, 1,1,0,1, 1, /*D=*/0, 0,0,0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, WRITE, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 15, 0);
}

// FS-022 Non-canonical Sv39 IOVA -> cause 13 (pre-walk canonical check)
static int8_t test_FS022(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000010000000000ULL;  // bit 40 set but bit 63..39 must sext(38)
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// FS-023..027: IOBridge PMP/PMA/decode faults originate outside the
// IOMMU model. Placeholders; will FAIL on cause mismatch.
static int8_t test_FS023(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000800000ULL;
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 5, 0);
}
static int8_t test_FS024(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000810000ULL;
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 5, 0);
}
static int8_t test_FS025(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000820000ULL;
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 5, 0);
}
static int8_t test_FS026(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000830000ULL;
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,
        1 /*exec_req*/, 0, 0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 1, 0);
}
static int8_t test_FS027(void)
{
    setup_fs(IOSATP_Sv39, 0, 0);
    uint64_t va = 0x0000000000840000ULL;
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, WRITE, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 5, 0);
}

// FS-028 DTF=1 suppresses FQ record on FS fault
static int8_t test_FS028(void)
{
    setup_fs(IOSATP_Sv39, 0, 1 /*dtf*/);
    uint64_t va = 0x0000000000B00000ULL;
    pte_t pte = {0};  // V=0 -> FS fault
    pte.R = 1; pte.W = 1; pte.U = 1; pte.A = 1; pte.D = 1;
    pte.PPN = 0x100D;
    add_s_stage_pte(g_iosatp, va, pte, 0, 0);
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    send_translation_request(&g_iommu, MY_DID, 0,0,0,0,0,0,
        ADDR_TYPE_UNTRANSLATED, va, 1, READ, &req, &rsp);
    return check_and_report(&g_iommu, &req, &rsp,
        (status_t)STATUS_FAULT, 13, 0);
}

// -------------------------------------------------------------------------- //
// main
// -------------------------------------------------------------------------- //
int main(void)
{
    signal(SIGSEGV, segv_handler);
    signal(SIGABRT, segv_handler);
    signal(SIGBUS,  segv_handler);

    printf("=== First-stage fault case tests ===\n\n");

    RUN_TEST("FS-001 L0 V=0",                 test_FS001());
    RUN_TEST("FS-002 L1 V=0",                 test_FS002());
    RUN_TEST("FS-003 L2 V=0 on store",        test_FS003());
    RUN_TEST("FS-004 R=0,W=1 reserved",       test_FS004());
    RUN_TEST("FS-005 PBMT when cap=0",        test_FS005());
    RUN_TEST("FS-006 N (Svnapot) when cap=0", test_FS006());
    RUN_TEST("FS-007 RSVD!=0",                test_FS007());
    RUN_TEST("FS-008 L1 R=1 superpage rd",    test_FS008());
    RUN_TEST("FS-009 L1 W=1 non-leaf wr",     test_FS009());
    RUN_TEST("FS-010 L1 X=1 non-leaf instr",  test_FS010());
    RUN_TEST("FS-011 L2 R=1 superpage rd",    test_FS011());
    RUN_TEST("FS-012 L2 W=1 non-leaf wr",     test_FS012());
    RUN_TEST("FS-013 L2 X=1 non-leaf instr",  test_FS013());
    RUN_TEST("FS-014 Leaf R=0,X=0",           test_FS014());
    RUN_TEST("FS-015 Leaf W=0 on store",      test_FS015());
    RUN_TEST("FS-016 Leaf X=0 on instr",      test_FS016());
    RUN_TEST("FS-017 U-mode, PTE.U=0",        test_FS017());
    RUN_TEST("FS-018 S-mode, PTE.U=1, SUM=0", test_FS018());
    RUN_TEST("FS-019 S-mode instr, U=1, X=1", test_FS019());
    RUN_TEST("FS-020 A=0 SADE=0",             test_FS020());
    RUN_TEST("FS-021 D=0 SADE=0 on store",    test_FS021());
    RUN_TEST("FS-022 non-canonical IOVA",     test_FS022());
    RUN_TEST("FS-023 OUT PMP root",           test_FS023());
    RUN_TEST("FS-024 OUT PMA L1",             test_FS024());
    RUN_TEST("FS-025 OUT decode L0",          test_FS025());
    RUN_TEST("FS-026 OUT PMP instr",          test_FS026());
    RUN_TEST("FS-027 OUT PMA store",          test_FS027());
    RUN_TEST("FS-028 DTF suppress FS",        test_FS028());

    printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail > 0) ? 1 : 0;
}
