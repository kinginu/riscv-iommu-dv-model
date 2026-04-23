/*
 * tb/cocotb/ref_model_api.c
 * =========================
 * Thin C wrapper that turns libiommu into a shared library with one
 * stable entry point callable from Python via ctypes.
 *
 * Build:
 *   gcc -shared -fPIC -O0 -g \
 *       -Iiommu_ref_model/libiommu/include \
 *       -Iiommu_ref_model/libtables/include \
 *       -Iiommu_ref_model/test \
 *       tb/cocotb/ref_model_api.c \
 *       iommu_ref_model/libiommu/src/*.c \
 *       iommu_ref_model/libtables/src/*.c \
 *       iommu_ref_model/test/tbapi.c \
 *       iommu_ref_model/test/test_utils.c \
 *       --coverage -lm \
 *       -o build/libiommu_ref.so
 *
 * Usage from Python (see test_runner.py):
 *
 *   import ctypes
 *   lib = ctypes.CDLL("build/libiommu_ref.so")
 *   lib.ref_translate.argtypes = [ctypes.POINTER(ref_input_t),
 *                                 ctypes.POINTER(ref_output_t)]
 *   lib.ref_translate.restype  = ctypes.c_int
 *   lib.ref_translate(byref(inp), byref(out))
 *
 * Adding new fields (e.g. iotval2) is just a matter of appending to the
 * input/output structs below — no Python changes needed as long as the
 * ctypes Structure on the Python side mirrors the layout.
 */

#include <stdint.h>
#include <string.h>

#include "iommu.h"
#include "tables_api.h"
#include "test_app.h"

/* --------------------------------------------------------------------- */
/* Wire-format structs (kept flat and POD for easy ctypes mirroring).    */
/* --------------------------------------------------------------------- */
typedef struct {
    /* config */
    uint8_t  iohgatp_mode;
    uint8_t  iosatp_mode;
    uint8_t  gade;
    uint8_t  sade;
    uint32_t device_id;
    uint32_t gscid;
    /* stimulus */
    uint64_t iova;
    uint8_t  is_read;
    uint8_t  is_write;
    uint8_t  is_exec;
    /* page table state (flattened — max 8 PTEs per stage) */
    uint8_t  num_g_ptes;
    struct {
        uint8_t  level;
        uint8_t  valid;
        uint8_t  r, w, x, u, a, d;
        uint64_t ppn;
    } g_ptes[8];
} ref_input_t;

typedef struct {
    uint8_t  status;      /* 0 = success, 1 = fault */
    uint16_t cause;
    uint64_t pa;
    uint64_t iotval2;
} ref_output_t;

/* --------------------------------------------------------------------- */
/* Globals required by tbapi.c                                           */
/* --------------------------------------------------------------------- */
extern int8_t  *memory;
extern int      test_endian;

static iommu_t g_iommu;

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */
int ref_translate(const ref_input_t *in, ref_output_t *out)
{
    memset(out, 0, sizeof(*out));

    /* 1. Bring the ref model to a clean state */
    reset_system(1, 65535);
    memset(&g_iommu, 0, sizeof(g_iommu));
    enable_cq(&g_iommu, 4);
    enable_fq(&g_iommu, 4);
    enable_iommu(&g_iommu, RVI_IOMMU_Off);

    /* 2. Register the device */
    uint64_t gppn = add_device(
        &g_iommu,
        in->device_id, in->gscid,
        0, 0, 0,            /* en_ats, en_pri, t2gpa */
        0, 0,               /* dtf, prpr */
        in->gade, in->sade,
        0, 0, 0,            /* dpe, sbe, sxl */
        in->iohgatp_mode, in->iosatp_mode, PDTP_Bare,
        MSIPTP_Off, 0,
        0, 0);

    iohgatp_t iohgatp = {0};
    iohgatp.MODE  = in->iohgatp_mode;
    iohgatp.PPN   = gppn;
    iohgatp.GSCID = in->gscid;

    /* 3. Install page-table entries described in the input */
    for (uint8_t i = 0; i < in->num_g_ptes; i++) {
        const typeof(in->g_ptes[0]) *p = &in->g_ptes[i];
        gpte_t pte = {0};
        pte.V   = p->valid;
        pte.R   = p->r; pte.W = p->w; pte.X = p->x; pte.U = p->u;
        pte.A   = p->a; pte.D = p->d;
        pte.PPN = p->ppn;
        add_g_stage_pte(&g_iommu, iohgatp, in->iova, pte, p->level);
    }

    /* 4. Send translation request */
    hb_to_iommu_req_t req; iommu_to_hb_rsp_t rsp;
    uint8_t rw = in->is_write ? WRITE : READ;
    send_translation_request(
        &g_iommu, in->device_id,
        0, 0, 0,
        in->is_exec, 0, 0,
        ADDR_TYPE_UNTRANSLATED,
        in->iova, 1, rw,
        &req, &rsp);

    /* 5. Translate rsp.status (status_t) to our simple wire format */
    out->status  = (rsp.status == 0) ? 0 : 1;
    out->pa      = ((uint64_t)rsp.PPN) << 12;
    /* cause and iotval2 come from the fault queue — left for follow-up */
    out->cause   = 0;
    out->iotval2 = 0;

    return 0;
}
