// Copyright (c) 2024
// SPDX-License-Identifier: Apache-2.0
//
// tb/c/test_report.h
//
// Helpers to emit machine-readable REPORT lines for each test case.
// Every test case that goes through check_and_report() produces one
// "REPORT\t..." line on stdout. scripts/gen_test_report.py then parses
// those lines into coverage/test_report.{txt,md}.

#ifndef TB_C_TEST_REPORT_H
#define TB_C_TEST_REPORT_H

#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "iommu.h"
#include "tables_api.h"
#include "test_app.h"

// A snapshot of the next FQ record, taken before check_rsp_and_faults()
// pops it. have_fault==0 means the FQ was empty at the time of peek.
typedef struct {
    uint8_t  have_fault;
    uint16_t cause;
    uint32_t pid;
    uint8_t  pv;
    uint8_t  priv;
    uint8_t  ttyp;
    uint32_t did;
    uint64_t iotval;
    uint64_t iotval2;
} captured_fault_t;

// Set by the RUN_TEST macro before each test runs.
static const char *g_current_test = "unknown";

static captured_fault_t peek_fault(iommu_t *iommu) {
    captured_fault_t f = {0};
    fqh_t fqh; fqb_t fqb;
    fqh.raw = read_register(iommu, FQH_OFFSET, 4);
    uint32_t fqt = read_register(iommu, FQT_OFFSET, 4);
    if (fqh.raw >= fqt) return f;

    fqb.raw = read_register(iommu, FQB_OFFSET, 8);
    fault_rec_t rec;
    read_memory_test((fqb.ppn * PAGESIZE) | (fqh.index * FQ_ENTRY_SZ),
                     FQ_ENTRY_SZ, (char *)&rec);
    f.have_fault = 1;
    f.cause   = rec.CAUSE;
    f.pid     = rec.PID;
    f.pv      = rec.PV;
    f.priv    = rec.PRIV;
    f.ttyp    = rec.TTYP;
    f.did     = rec.DID;
    f.iotval  = rec.iotval;
    f.iotval2 = rec.iotval2;
    return f;
}

// Human-readable transaction descriptor ("read"/"write"/"exec"/"ats").
static const char *req_kind(const hb_to_iommu_req_t *req) {
    if (req->tr.at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) return "ats";
    if (req->exec_req)                                        return "exec";
    if (req->tr.read_writeAMO == WRITE)                       return "write";
    return "read";
}

// Classify the outcome in a stable, machine-readable token.
static const char *classify(int passed, const hb_to_iommu_req_t *req,
                            const iommu_to_hb_rsp_t *rsp,
                            status_t exp_status, uint16_t exp_cause,
                            uint64_t exp_iotval2, const captured_fault_t *cf) {
    if (passed == 0) return "ok";
    if (rsp->status != exp_status) return "status_mismatch";
    if (exp_cause != 0 && !cf->have_fault) return "no_fault";
    if (exp_cause == 0 &&  cf->have_fault) return "unexpected_fault";
    if (cf->have_fault) {
        if (cf->cause   != exp_cause)          return "cause_mismatch";
        if (cf->iotval2 != exp_iotval2)        return "iotval2_mismatch";
        if (cf->iotval  != req->tr.iova)       return "iotval_mismatch";
        if (cf->did     != req->device_id)     return "did_mismatch";
    }
    return "other";
}

static int8_t check_and_report(iommu_t *iommu,
                               hb_to_iommu_req_t *req,
                               iommu_to_hb_rsp_t *rsp,
                               status_t status,
                               uint16_t cause,
                               uint64_t exp_iotval2)
{
    captured_fault_t cf = peek_fault(iommu);

    // Delegate pass/fail to the ref model's existing check.
    int8_t r = check_rsp_and_faults(iommu, req, rsp, status, cause, exp_iotval2);

    // Emit one TSV line. The REPORT\t prefix is the grep anchor.
    printf("REPORT\t"
           "test=%s\t"
           "rw=%s\t"
           "iova=0x%016" PRIx64 "\t"
           "exp_status=%u\tact_status=%u\t"
           "exp_cause=%u\tact_cause=%u\t"
           "exp_iotval2=0x%016" PRIx64 "\tact_iotval2=0x%016" PRIx64 "\t"
           "act_iotval=0x%016" PRIx64 "\t"
           "act_ttyp=%u\t"
           "act_fault_present=%u\t"
           "result=%s\t"
           "reason=%s\n",
           g_current_test,
           req_kind(req),
           (uint64_t)req->tr.iova,
           (unsigned)status, (unsigned)rsp->status,
           (unsigned)cause, (unsigned)(cf.have_fault ? cf.cause : 0),
           (uint64_t)exp_iotval2, (uint64_t)(cf.have_fault ? cf.iotval2 : 0ULL),
           (uint64_t)(cf.have_fault ? cf.iotval : 0ULL),
           (unsigned)(cf.have_fault ? cf.ttyp : 0),
           (unsigned)cf.have_fault,
           r == 0 ? "PASS" : "FAIL",
           classify(r, req, rsp, status, cause, exp_iotval2, &cf));
    fflush(stdout);
    return r;
}

#endif // TB_C_TEST_REPORT_H
