// SPDX-License-Identifier: GPL-2.0-only
// gpu-unlock-rt · transaction state + phase API. Faithful port of
// Zagitta/gpu-unlock kernel/gpu_unlock/unlock_610.c (GPL-2.0-only);
// source line anchors given as `u610.c:NNN`.
#ifndef GU_TRANSACTION_H
#define GU_TRANSACTION_H

#include "gu_env.h"
#include "portable/diagnostic.h"
#include "portable/prep_610.h"
#include "portable/vbios_610.h"
#include "portable/layout_610.h"
#include "portable/gfw_eval.h"

// engine register blocks (u610.c:14-16, :923-925, :1115-1117)
struct GU_FALCON_ENG { u32 base, fbif, falcon2; };
extern const struct GU_FALCON_ENG GuGspEng; // 0x00110000 / 0x00110600 / 0x00111000
extern const struct GU_FALCON_ENG GuSec2Eng; // 0x00840000 / 0x00840600 / 0x00841000

// PCI config snapshot + live access trace (mirrors u610.c:41-98 work struct)
typedef struct _GU_WORK {
    // target identity
    ULONG  Bus, Device, Fn;
    u32    Identity;            // (did<<16)|vid, from config (u610.c:145)
    // config space capability cache (set at claim)
    LONG   PciCapOff;           // PCIe cap block offset, -1 unknown
    LONG   PmCapOff;            // PM cap block offset, -1 none
    // BAR0 window
    BOOLEAN BarMapped;
    volatile u32 *Bar;          // full VA window (non-cached)
    u64    BarPa;
    SIZE_T BarLen;
    // PCI state snapshot (+/− pair: GuSaveConfig / GuRestoreConfig)
    BOOLEAN CmdValid;
    u16    OriginalCmd;         // u610.c original_cmd
    u32    SavedHdr[16];        // dwords 0x00..0x3F
    u32    SavedBars[6];
    BOOLEAN SavedDevCtlValid; u16 SavedDevCtl;
    BOOLEAN SavedPmCsrValid; u16 SavedPmCsr;
    // L2/L3 state (u610.c:60-98)
    struct gpu_unlock_span ImageSpan, SigSpan;
    struct gpu_unlock_layout_in LayoutIn;
    struct gpu_unlock_layout_out Layout;
    struct gpu_unlock_boot_desc BootDesc;
    struct gpu_unlock_radix_plan Radix;
    struct GU_PAGE { void *Cpu; PHYSICAL_ADDRESS Dma; } *Pages;
    u32 PageCount, PagesAllocated;
    void *BootCpu;  PHYSICAL_ADDRESS BootDma;
    void *BooterCpu; PHYSICAL_ADDRESS BooterDma;
    u32 BooterImageSize;
    struct gpu_unlock_booter_hdr BooterHdr;
    void *V67Cpu;   PHYSICAL_ADDRESS V67Dma;
    void *StockCpu; PHYSICAL_ADDRESS StockDma;
    u64 StockSize;
    void *MetaCpu;  PHYSICAL_ADDRESS MetaDma;
    unsigned SignatureCalls;
    BOOLEAN Published;
    BOOLEAN FirmwareStarted;
    BOOLEAN Quiesced;
    BOOLEAN Retained;
    u32 PlmEntry;
    void *FwsecCpu; PHYSICAL_ADDRESS FwsecDma;
    u32 FwsecSize;
    // last-op traces (u610.c:94-98)
    u32 LastOffset, LastValue; BOOLEAN LastValid;
    u32 LastPciOffset, LastPciValue; int LastPciError; BOOLEAN LastPciCap;
    // reporting
    struct _GU_RUN_OUT *Out;
} GU_WORK;

// Phase tower in exact u610.c:1457-1479 order. Probe is the Windows-only,
// non-arming addition. Signature runs twice (publish V67, restore stock).
typedef NTSTATUS (*GU_PHASE_FN)(GU_WORK *);

NTSTATUS GuTxProbe(GU_WORK *w);
NTSTATUS GuTxAssets(GU_WORK *w);
NTSTATUS GuTxClaim(GU_WORK *w);
NTSTATUS GuTxInduce(GU_WORK *w);  // Windows-only: FLR induction of the boot path
NTSTATUS GuTxGfw(GU_WORK *w);
NTSTATUS GuTxLayout(GU_WORK *w);
NTSTATUS GuTxDma(GU_WORK *w);
NTSTATUS GuTxFwsec(GU_WORK *w);
NTSTATUS GuTxSignature(GU_WORK *w);   // call-twice protocol enforced by caller
NTSTATUS GuTxBoot(GU_WORK *w);
NTSTATUS GuTxSelector(GU_WORK *w);
NTSTATUS GuTxReset(GU_WORK *w);
NTSTATUS GuTxUnwind(GU_WORK *w);

// errno -> NTSTATUS boundary map (gu_rt.c uses this)
NTSTATUS GuErrToNt(int err);

#endif // GU_TRANSACTION_H
