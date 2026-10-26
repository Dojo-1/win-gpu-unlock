// SPDX-License-Identifier: GPL-2.0-only
// gpu-unlock-rt · control device + IOCTL contract. See PORT-MANIFOLD.md.
#ifndef GU_RT_H
#define GU_RT_H

#include "gu_env.h"

// ---- target profile P1 (Acer PO3-640 / Zotac RTX 3060 Ti) ----
#define GU_TARGET_VID    0x10DEu
#define GU_TARGET_DID    0x2489u
#define GU_TARGET_SUBSYS 0xA632174Bu

// ---- monotone phase tower — exact u610.c:1457-1479 order, plus the
//      Windows-only probe (0) and induce (3). Signature runs twice:
//      8 publishes V67, 10 restores stock. No in-attach retries (geodesic). ----
typedef enum _GU_PHASE {
    GuPhaseProbe = 0,     // read-only reachability check (E1). Non-arming.
    GuPhaseAssets,        // 1  u610:610  — blobs, ELF spans, radix plan, booter hdr
    GuPhaseClaim,         // 2  u610:801  — caps, D0, BME off, snapshot, BAR0 map
    GuPhaseInduce,        // 3  NEW       — FLR away driver-init state, ROM boot replays
    GuPhaseGfw,           // 4  u610:1028 — entry evidence (plm/ss/wpr2 cold checks)
    GuPhaseLayout,        // 5  u610:657  — FB layout from BAR0 regs
    GuPhaseDma,           // 6  u610:878  — coherent buffers, booter patch, radix tree
    GuPhaseFwsec,         // 7  u610:921  — board-ROM FWSEC-FRTS on GSP falcon
    GuPhaseSignature,     // 8  u610:1137 — publish V67 (ROP payload signature)
    GuPhaseBoot,          // 9  u610:1113 — Booter start; ROP flips FEAT_OVR_PLM
    GuPhaseSignature2,    // 10 u610:1475 — restore stock signature
    GuPhaseSelector,      // 11 u610:1189 — SS0=0x88888888, SS1=8 with readback
    GuPhaseReset,         // 12 u610:1333 — cleanup FLR + containment checks
    GuPhaseUnwind,        // 13 u610:526  — single descent geodesic, idempotent
    GuPhaseCount
} GU_PHASE;

#define GU_IOCTL_RUN \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _GU_RUN_IN {
    ULONG Size;            // sizeof(GU_RUN_IN)
    ULONG Bus, Device, Fn; // BDF of the (PnP-disabled) target
    ULONG PhaseLimit;      // run phases 0..min(PhaseLimit, GuPhaseReset), then unwind
    ULONG Flags;
#define GU_F_CONFIRM_BLACKOUT 1u // gate G2; without it PhaseLimit stops at GuPhaseProbe
} GU_RUN_IN;

typedef struct _GU_RUN_OUT {
    ULONG   Size;
    ULONG   PhaseReached;                    // watermark
    NTSTATUS PhaseStatus[GuPhaseCount];
    ULONG   Diag[8];                         // probe: id/bar0/chip/pmcsr + last ops
    ULONG   PostFlrBarChip;                  // u610:1268 readback
    ULONG   SelectorReadback[2];
} GU_RUN_OUT;
// extended output: client sends outLen >= 108+256; dump dwords live at 112..

#endif // GU_RT_H
