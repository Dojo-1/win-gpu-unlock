// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpu-unlock-rt · faithful WDM port of Zagitta/gpu-unlock
 * kernel/gpu_unlock/unlock_610.c (GPL-2.0-only). Anchors: `u610:NNN`.
 *
 * Differences from the Linux original, all documented in PORT-MANIFOLD.md §5:
 *  - Config space via HAL bus data (device is PnP-disabled; no PDO binding).
 *  - Firmware comes from build-time generated gu_fw_blobs.h, not request_firmware.
 *  - NEW phase `induce` (after claim): an FLR-class reset that discards the
 *    driver-initialized state of this power cycle (WPR2 up, GSP-RM run) and
 *    replays the ROM boot path — the window Linux gets for free by running
 *    pre-driver. The source proves this replay exists: its own reset phase
 *    FLRs and then waits for ROM GFW completion with WPR2 down (u610:1391-1425).
 *  - The resume/suspend path is not ported (cold-induction path only).
 */
#include "gu_transaction.h"
#include "gu_rt.h"          // GU_RUN_OUT definition
#include "gu_fw_blobs.h"
#include <wdm.h>

#define GU_TAG 0x55474755u

const struct GU_FALCON_ENG GuGspEng  = { .base = 0x00110000, .fbif = 0x00110600, .falcon2 = 0x00111000 }; // u610:923
const struct GU_FALCON_ENG GuSec2Eng = { .base = 0x00840000, .fbif = 0x00840600, .falcon2 = 0x00841000 }; // u610:1115

#define GU_SS0_UNLOCK 0x88888888u // u610:24
#define GU_SS1_UNLOCK 0x8u
#define BOOTER_MAGIC  0x314c5442u // u610:26 'BTL1'
#define GU_GFW_TIMEOUT_US 2050000u

// ================= time / alloc (L1) ==========================================

static u64 GuNowUs(void)
{
    return (u64)KeQueryInterruptTime() / 10; // interrupt time is 100ns units
}

static void GuUsleep(LONG us)
{
    LARGE_INTEGER t; t.QuadPart = -(LONGLONG)us * 10;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static void *GuAllocZero(SIZE_T size)
{
    // ExAllocatePool2 zero-initializes unless POOL_FLAG_UNINITIALIZED
    void *p = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, GU_TAG);
    if (!p && size) GU_ERROR("alloc failed bytes=%llu\n", (unsigned long long)size);
    return p;
}

static void GuFree(void *p) { if (p) ExFreePoolWithTag(p, GU_TAG); }

// dma_alloc_coherent + dma_set_mask(32) analog: physically-contiguous, cached,
// below 4 GiB. The falcon DMAs physical addresses (u610:150).
static void *GuDmaAlloc(SIZE_T size, PHYSICAL_ADDRESS *dma)
{
    PHYSICAL_ADDRESS maxAddr; maxAddr.QuadPart = 0xFFFFFFFFull; // DMA_BIT_MASK(32) u610:892
    void *cpu = MmAllocateContiguousMemorySpecifyCache(size, MmGetPhysicalAddress((void *)(ULONG_PTR)0), maxAddr, MmCached);
    if (cpu) dma->QuadPart = MmGetPhysicalAddress(cpu).QuadPart;
    else GU_ERROR("dma alloc failed bytes=%llu\n", (unsigned long long)size);
    return cpu;
}

static void GuDmaFree(void *cpu, SIZE_T size)
{
    if (cpu) MmFreeContiguousMemorySpecifyCache(cpu, size, MmCached);
}

// ================= PCI config (L1) ============================================
// diag_pci_word/dword u610:118-142; capability reads via cached cap offsets.

static int GuCfgRead(GU_WORK *w, int offset, void *value, ULONG len, BOOLEAN cap)
{
    PCI_SLOT_NUMBER slot; ULONG got;
    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = w->Device;
    slot.u.bits.FunctionNumber = w->Fn;
    RtlZeroMemory(value, len);
    if (cap && w->PciCapOff < 0) { w->LastPciCap = cap; w->LastPciOffset = offset; w->LastPciValue = ~0u; w->LastPciError = -ENODEV; return -ENODEV; }
    got = HalGetBusDataByOffset(PCIConfiguration, w->Bus, slot.u.AsULONG, value,
                                cap ? (ULONG)(w->PciCapOff + offset) : (ULONG)offset, len);
    w->LastPciCap = cap;
    w->LastPciOffset = cap ? (ULONG)(w->PciCapOff + offset) : (ULONG)offset;
    if (len == 2) w->LastPciValue = *(u16 *)value;
    else if (len == 4) w->LastPciValue = *(u32 *)value;
    w->LastPciError = got == len ? 0 : -ENODEV;
    return w->LastPciError;
}

static int GuCfgWrite(GU_WORK *w, int offset, void *value, ULONG len, BOOLEAN cap)
{
    PCI_SLOT_NUMBER slot; ULONG put;
    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = w->Device;
    slot.u.bits.FunctionNumber = w->Fn;
    if (cap && w->PciCapOff < 0) return -ENODEV;
    put = HalSetBusDataByOffset(PCIConfiguration, w->Bus, slot.u.AsULONG, value,
                                cap ? (ULONG)(w->PciCapOff + offset) : (ULONG)offset, len);
    return put == len ? 0 : -EIO;
}

static int GuCfgWordR(GU_WORK *w, int off, u16 *v, BOOLEAN cap) { return GuCfgRead(w, off, v, 2, cap); }
static int GuCfgDwordR(GU_WORK *w, int off, u32 *v, BOOLEAN cap) { return GuCfgRead(w, off, v, 4, cap); }
static int GuCfgWordW(GU_WORK *w, int off, u16 v, BOOLEAN cap) { return GuCfgWrite(w, off, &v, 2, cap); }
static int GuCfgDwordW(GU_WORK *w, int off, u32 v, BOOLEAN cap) { return GuCfgWrite(w, off, &v, 4, cap); }

#define PCI_VENDOR_ID      0x00
#define PCI_COMMAND        0x04
#define PCI_BASE_ADDRESS_0 0x10
#define PCI_PM_CTRL        4       // within PM cap; state mask
#define PCI_PM_CTRL_STATE_MASK 3
#define PCI_EXP_DEVCAP     0x04    // within PCIe cap
#define PCI_EXP_DEVCTL     0x08
#define PCI_EXP_DEVSTA     0x0A
#define PCI_EXP_DEVCAP_FLR     (1u << 28)
#define PCI_EXP_DEVCTL_FLR     (1u << 15)
#define PCI_EXP_DEVSTA_TRPND   (1u << 5)
#define GU_PCI_COMMAND_IO     0x1u
#define GU_PCI_COMMAND_MEMORY 0x2u
#define GU_PCI_COMMAND_MASTER 0x4u

// ================= BAR0 MMIO (L1) =============================================

static int read_bar(GU_WORK *w, u32 offset, u32 *value) // u610:203
{
    w->LastOffset = offset;
    w->LastValid = FALSE;
    if (!w->BarMapped || (u64)offset > (u64)w->BarLen || (u64)w->BarLen - offset < 4)
        return -EINVAL;
    *value = READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)w->Bar + offset));
    w->LastValue = *value;
    w->LastValid = TRUE;
    return 0;
}

static void write_bar(GU_WORK *w, u32 offset, u32 value) // u610:215
{
    WRITE_REGISTER_ULONG((volatile ULONG *)((PUCHAR)w->Bar + offset), value);
}

static bool inaccessible(u32 value) // u610:257
{
    return value == ~0u || (value & 0xffff0000u) == 0xbadf0000u;
}

static int poll_us(GU_WORK *w, u32 offset, u32 mask, u32 expect, // u610:220
                   unsigned budget_us, bool timeout_is_failure)
{
    u64 deadline = GuNowUs() + budget_us;
    u32 value = 0;
    for (;;) {
        if (read_bar(w, offset, &value) || value == ~0u ||
            (value & 0xffff0000u) == 0xbadf0000u) {
            GU_ERROR("poll inaccessible: reg=%#x last=%#x mask=%#x expect=%#x budget=%u\n",
                     offset, value, mask, expect, budget_us);
            return -EIO;
        }
        if ((value & mask) == expect)
            return 0;
        if (GuNowUs() >= deadline) {
            if (timeout_is_failure)
                GU_ERROR("poll timeout: reg=%#x last=%#x mask=%#x expect=%#x budget=%u\n",
                         offset, value, mask, expect, budget_us);
            else
                GU_NOTE("poll timeout tolerated: reg=%#x last=%#x\n", offset, value);
            return -ETIMEDOUT;
        }
        GuUsleep(50); // usleep_range(50,100)
    }
}

// ================= falcon primitives (L2) =====================================

struct falcon_eng_alias { u32 base, fbif, falcon2; };

static int falcon_reset(GU_WORK *w, const struct GU_FALCON_ENG *eng) // u610:262
{
    u32 engine, hwcfg, bcr, chip;
    unsigned i;
    int ret;

    // 610 kflcnPreResetWait_GA102 permits this timeout (HW bug 3419321).
    ret = poll_us(w, eng->base + 0xf4, 1u << 31, 1u << 31, 1000, false);
    if (ret && ret != -ETIMEDOUT)
        return ret;
    if (ret)
        GU_NOTE("falcon pre-reset timeout tolerated per HW bug 3419321\n");
    if (read_bar(w, eng->base + 0x3c0, &engine) || inaccessible(engine))
        return -ENODEV;
    write_bar(w, eng->base + 0x3c0, engine | 1);
    for (i = 0; i < 10; i++)
        (void)READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)w->Bar + eng->base + 0x3c0));
    write_bar(w, eng->base + 0x3c0, engine & ~1u);
    for (i = 0; i < 10; i++)
        (void)READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)w->Bar + eng->base + 0x3c0));
    ret = poll_us(w, eng->base + 0xf4, 1u << 12, 0, 1000000, true);
    if (ret)
        return ret;
    if (read_bar(w, eng->base + 0xf4, &hwcfg) || inaccessible(hwcfg))
        return -EIO;
    if (hwcfg & (1u << 10)) {
        if (read_bar(w, eng->falcon2 + 0x668, &bcr) || inaccessible(bcr))
            return -ENODEV;
        if (bcr & (1u << 4)) {
            write_bar(w, eng->falcon2 + 0x668, 0);
            ret = poll_us(w, eng->falcon2 + 0x668, 0x11, 1, 1000000, true);
            if (ret)
                return ret;
        }
    }
    if (read_bar(w, 0, &chip) || inaccessible(chip))
        return -ENODEV;
    write_bar(w, eng->base + 0x84, chip);
    GU_NOTE("falcon reset done: base=%#x hwcfg=%#x chip=%#x\n", eng->base, hwcfg, chip);
    return 0;
}

// DMATRFMOFFS is 24 bits, FBOFFS 32 bits, BASE 49 bits (u610:305-314).
static bool falcon_dma_valid(u32 dest, u32 mem_off, u64 src, u32 size)
{
    return size && !((size | dest | mem_off | src) & 255) &&
        (u64)dest + size <= (1ull << 24) &&
        (u64)mem_off + size <= (1ull << 32) &&
        src < (1ull << 49) &&
        (u64)mem_off + size <= (1ull << 49) - src;
}

static int falcon_dma(GU_WORK *w, u32 base, u32 dest, u32 mem_off, // u610:316
                      u64 src, u32 size, u32 cmd)
{
    u32 done = 0;
    int ret;

    if (!falcon_dma_valid(dest, mem_off, src, size))
        return -EINVAL;
    ret = poll_us(w, base + 0x118, 1, 0, 1000000, true);
    if (ret)
        return ret;
    write_bar(w, base + 0x110, (u32)((src >> 8) & 0xffffffffu));
    write_bar(w, base + 0x128, (u32)((src >> 40) & 0x1ffu));
    while (done < size) {
        ret = poll_us(w, base + 0x118, 1, 0, 1000000, true);
        if (ret)
            return ret;
        write_bar(w, base + 0x114, dest);
        write_bar(w, base + 0x11c, mem_off);
        write_bar(w, base + 0x118, cmd);
        done += 256; dest += 256; mem_off += 256;
    }
    return poll_us(w, base + 0x118, 2, 2, 1000000, true);
}

static int falcon_hs(GU_WORK *w, const struct GU_FALCON_ENG *eng, // u610:347
                     u32 code_off, u32 code_size, u32 code_va, u32 code_pa,
                     u32 data_off, u32 data_size, u32 data_pa,
                     u32 sig_dmem, u32 engine_mask, u32 ucode_id,
                     u64 image, u32 mailbox0, u32 mailbox1, bool wait_plm)
{
    u32 trans, cpuctl;
    u64 code_src;
    int ret;

    if (image > ~0ull - code_off || image + code_off < code_va ||
        image > ~0ull - data_off)
        return -ERANGE;
    code_src = image + code_off - code_va;
    if (!falcon_dma_valid(code_pa, code_va, code_src, code_size) ||
        (data_size && !falcon_dma_valid(data_pa, 0, image + data_off, data_size)))
        return -EINVAL;
    ret = falcon_reset(w, eng);
    if (ret)
        return ret;
    if (read_bar(w, eng->fbif + 0x24, &trans) || inaccessible(trans))
        return -EIO;
    trans |= 1u << 7;
    write_bar(w, eng->fbif + 0x24, trans);
    write_bar(w, eng->base + 0x10c, 0);
    if (read_bar(w, eng->fbif, &trans) || inaccessible(trans))
        return -ENODEV;
    trans = (trans & ~7u) | 5u;
    write_bar(w, eng->fbif, trans);
    KeMemoryBarrier(); // dma_wmb()
    ret = falcon_dma(w, eng->base, code_pa, code_va, code_src,
                     code_size, gpu_unlock_dma_cmd(true));
    if (ret)
        return ret;
    if (data_size) {
        ret = falcon_dma(w, eng->base, data_pa, 0, image + data_off, data_size,
                         gpu_unlock_dma_cmd(false));
        if (ret)
            return ret;
    }
    write_bar(w, eng->falcon2 + 0x210, sig_dmem);
    write_bar(w, eng->falcon2 + 0x19c, engine_mask);
    write_bar(w, eng->falcon2 + 0x198, ucode_id & 0xff);
    write_bar(w, eng->falcon2 + 0x180, 1);
    write_bar(w, eng->base + 0x104, code_va);
    write_bar(w, eng->base + 0x40, mailbox0);
    write_bar(w, eng->base + 0x44, mailbox1);
    if (read_bar(w, eng->base + 0x100, &cpuctl) || inaccessible(cpuctl))
        return -EIO;
    GU_NOTE("falcon start: base=%#x cpuctl=%#x code_va=%#x sig_dmem=%#x eng=%#x ucode=%u mb=%#x:%#x\n",
            eng->base, cpuctl, code_va, sig_dmem, engine_mask, ucode_id, mailbox1, mailbox0);
    if (cpuctl & (1u << 6))
        write_bar(w, eng->base + 0x130, 1u << 1);
    else
        write_bar(w, eng->base + 0x100, 1u << 1);
    if (wait_plm) {
        u64 deadline = GuNowUs() + 5000000;
        for (;;) {
            u32 plm, engine;
            u32 identity;
            if (GuCfgDwordR(w, PCI_VENDOR_ID, &identity, FALSE) ||
                identity != w->Identity ||
                read_bar(w, eng->base + 0x100, &engine) || inaccessible(engine) ||
                read_bar(w, GU_FEAT_PLM_OFFSET, &plm))
                return -ENODEV;
            // Cold entry was closed: this transition proves payload execution (u610:415)
            if (plm == ~0u && w->PlmEntry != ~0u)
                return 0;
            if ((engine & (1u << 4)) || GuNowUs() >= deadline)
                GU_ERROR("early PLM failed: pci=%#x cpuctl=%#x plm=%#x entry=%#x\n",
                         identity, engine, plm, w->PlmEntry);
            if (engine & (1u << 4))
                return -EIO;
            if (GuNowUs() >= deadline)
                return -ETIMEDOUT;
            GuUsleep(50);
        }
    }
    return poll_us(w, eng->base + 0x100, 1u << 4, 1u << 4, 5000000, true);
}

// ================= helpers ====================================================

static void put_le64(u8 *dest, u64 value) // u610:1129
{
    for (unsigned i = 0; i < 8; i++)
        dest[i] = (u8)(value >> (8 * i));
}

static void GuSaveConfig(GU_WORK *w) // pci_save_state analog: header + BARs + DEVCTL
{
    for (int i = 0; i < 16; i++)
        (void)GuCfgDwordR(w, i * 4, &w->SavedHdr[i], FALSE);
    for (int i = 0; i < 6; i++)
        w->SavedBars[i] = w->SavedHdr[4 + i];
    w->SavedDevCtlValid = GuCfgWordR(w, PCI_EXP_DEVCTL, &w->SavedDevCtl, TRUE) == 0;
    w->SavedPmCsrValid = w->PmCapOff >= 0 &&
        GuCfgWordR(w, PCI_PM_CTRL, &w->SavedPmCsr, TRUE) == 0;
}

static int GuRestoreConfig(GU_WORK *w) // pci_restore_state analog (u610:1377)
{
    u32 v;
    // BARs first, then command (restores decode without transient garbage)
    for (int i = 0; i < 6; i++)
        if (GuCfgDwordW(w, PCI_BASE_ADDRESS_0 + i * 4, w->SavedBars[i], FALSE))
            return -EIO;
    if (GuCfgDwordW(w, PCI_COMMAND, w->SavedHdr[1] & ~(GU_PCI_COMMAND_MASTER | GU_PCI_COMMAND_IO), FALSE))
        return -EIO;
    if (w->SavedDevCtlValid &&
        GuCfgWordW(w, PCI_EXP_DEVCTL, w->SavedDevCtl & (u16)~PCI_EXP_DEVCTL_FLR, TRUE))
        return -EIO;
    (void)GuCfgDwordR(w, PCI_VENDOR_ID, &v, FALSE);
    return v == w->Identity ? 0 : -ENODEV;
}

// PCIe FLR, port of pcie_flr(): wait TRPND clear, assert FLR, wait completion.
static int GuPcieFlr(GU_WORK *w)
{
    u16 devsta;
    u64 deadline;

    deadline = GuNowUs() + 1000000;
    for (;;) {
        if (GuCfgWordR(w, PCI_EXP_DEVSTA, &devsta, TRUE))
            return -EIO;
        if (!(devsta & PCI_EXP_DEVSTA_TRPND))
            break;
        if (GuNowUs() >= deadline)
            return -EBUSY;
        GuUsleep(1000);
    }
    if (GuCfgWordW(w, PCI_EXP_DEVCTL, (u16)(w->SavedDevCtl | PCI_EXP_DEVCTL_FLR), TRUE))
        return -EIO;
    deadline = GuNowUs() + 1000000;
    for (;;) {
        if (GuCfgWordR(w, PCI_EXP_DEVSTA, &devsta, TRUE))
            return -EIO;
        if (!(devsta & PCI_EXP_DEVSTA_TRPND))
            return 0;
        if (GuNowUs() >= deadline)
            return -ETIMEDOUT;
        GuUsleep(1000);
    }
}

static int wait_post_flr_bar(GU_WORK *w, u32 chip) // u610:1268
{
    u64 deadline = GuNowUs() + 1000000;
    u32 value = 0;
    for (;;) {
        if (read_bar(w, 0, &value))
            return -EIO;
        if (value == chip)
            return 0;
        if (GuNowUs() >= deadline) {
            GU_ERROR("post-FLR BAR timeout: chip=%#x expected=%#x\n", value, chip);
            return -ETIMEDOUT;
        }
        GuUsleep(1000);
    }
}

static int wait_post_flr_gfw(GU_WORK *w) // u610:1292
{
    u64 deadline = GuNowUs() + GU_GFW_TIMEOUT_US;
    struct gpu_unlock_gfw_sample sample;
    u32 identity = 0;
    int ret;

    for (;;) {
        RtlZeroMemory(&sample, sizeof(sample));
        ret = GuCfgDwordR(w, PCI_VENDOR_ID, &identity, FALSE);
        sample.config_ok = !ret && identity == w->Identity;
        if (!sample.config_ok) { ret = -ENODEV; break; }
        if (read_bar(w, GU_GFW_CPUCTL_OFFSET, &sample.cpuctl) ||
            read_bar(w, GU_GFW_PLM_OFFSET, &sample.plm)) { ret = -EIO; break; }
        if (!inaccessible(sample.plm) && (sample.plm & GU_GFW_PLM_LEVEL0) &&
            read_bar(w, GU_GFW_BOOT_OFFSET, &sample.boot)) { ret = -EIO; break; }
        ret = inaccessible(sample.plm) || inaccessible(sample.boot) ||
              inaccessible(sample.cpuctl) ? -EAGAIN : gpu_unlock_gfw_assess(&sample);
        if (!ret)
            break;
        if (GuNowUs() >= deadline) { ret = -ETIMEDOUT; break; }
        GuUsleep(1000);
    }
    GU_NOTE("post-FLR GFW: error=%d pci=%#x plm=%#x boot=%#x cpuctl=%#x\n",
            ret, identity, sample.plm, sample.boot, sample.cpuctl);
    return ret;
}

// hold_sec2_reset u610:1202: reset SEC2, leave halted with reset released.
static int hold_sec2_reset(GU_WORK *w)
{
    unsigned j;
    u32 engine, hwcfg;
    int ret;

    if (read_bar(w, GuSec2Eng.base + 0x3c0, &engine) || inaccessible(engine))
        return -ENODEV;
    write_bar(w, GuSec2Eng.base + 0x3c0, engine | 1);
    for (j = 0; j < 10; j++) { // 10 register cycles per ksec2ResetHw_TU102
        if (read_bar(w, GuSec2Eng.base + 0x3c0, &engine) || inaccessible(engine))
            return -EIO;
    }
    ret = poll_us(w, GuSec2Eng.base + 0x3c0, 1, 1, 1000, true);
    if (ret)
        return ret;
    if (read_bar(w, GuSec2Eng.base + 0x3c0, &engine) || inaccessible(engine))
        return -EIO;
    write_bar(w, GuSec2Eng.base + 0x3c0, engine & ~1u);
    for (j = 0; j < 10; j++) {
        if (read_bar(w, GuSec2Eng.base + 0x3c0, &engine) || inaccessible(engine))
            return -EIO;
    }
    ret = poll_us(w, GuSec2Eng.base + 0x3c0, 1, 0, 1000, true);
    if (ret)
        return ret;
    ret = poll_us(w, GuSec2Eng.base + 0xf4, 1u << 12, 0, 1000000, true);
    if (ret)
        return ret;
    if (read_bar(w, GuSec2Eng.base + 0xf4, &hwcfg) || inaccessible(hwcfg))
        return -EIO;
    GU_NOTE("SEC2 reset released: engine=%#x hwcfg2=%#x\n", engine, hwcfg);
    return 0;
}

static void free_coherent(GU_WORK *w) // u610:489
{
    for (u32 i = 0; i < w->PagesAllocated; i++)
        GuDmaFree(w->Pages[i].Cpu, GU_RADIX_PAGE);
    GuFree(w->Pages);
    w->Pages = NULL;
    w->PagesAllocated = 0;
    if (w->BootCpu)   GuDmaFree(w->BootCpu, w->BootDesc.image_size);
    if (w->FwsecCpu)  GuDmaFree(w->FwsecCpu, w->FwsecSize);
    if (w->BooterCpu) GuDmaFree(w->BooterCpu, GU_BOOTER_IMAGE_SIZE);
    if (w->V67Cpu)    GuDmaFree(w->V67Cpu, GU_V67_SIZE);
    if (w->StockCpu)  GuDmaFree(w->StockCpu, (SIZE_T)w->StockSize);
    if (w->MetaCpu)   GuDmaFree(w->MetaCpu, GU_WPR_META_SIZE);
    w->BootCpu = w->FwsecCpu = w->BooterCpu = w->V67Cpu = w->StockCpu = w->MetaCpu = NULL;
}

// ================= phases =====================================================

// Phase 0 · probe — Windows-only, non-arming. Answers unknown E1.
NTSTATUS GuTxProbe(GU_WORK *w)
{
    u32 id, subsys, bar0lo, bar0hi;
    u32 chip = 0;

    if (GuCfgDwordR(w, PCI_VENDOR_ID, &id, FALSE))
        return STATUS_DEVICE_NOT_CONNECTED;
    if ((id & 0xFFFF) != 0x10DEu || ((id >> 16) & 0xFFFF) != 0x2489u)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    w->Identity = id;
    if (GuCfgDwordR(w, 0x2C, &subsys, FALSE) || subsys != 0xA632174Bu)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (GuCfgDwordR(w, PCI_BASE_ADDRESS_0, &bar0lo, FALSE))
        return STATUS_DEVICE_NOT_CONNECTED;
    if (GuCfgDwordR(w, PCI_BASE_ADDRESS_0 + 4, &bar0hi, FALSE))
        bar0hi = 0;
    w->Out->Diag[0] = id;
    w->Out->Diag[1] = bar0lo;
    if (!bar0lo || (bar0lo & 1))
        return STATUS_UNSUCCESSFUL; // unmapped or I/O BAR
    // 64-bit BAR0 (bits 2:1 == 2) not expected on GA10x; refuse to guess.
    if (((bar0lo >> 1) & 3) == 2)
        return STATUS_NOT_SUPPORTED;

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = ((u64)bar0hi << 32) | (bar0lo & ~0xFull);
    w->BarPa = pa.QuadPart;
    w->BarLen = 0x1000000; // GA104 BAR0 window 16 MiB; re-verified against layout needs
    void *bar = MmMapIoSpace(pa, (SIZE_T)w->BarLen, MmNonCached);
    if (!bar)
        return STATUS_INSUFFICIENT_RESOURCES;
    w->Bar = (volatile u32 *)bar;
    w->BarMapped = TRUE;
    (void)read_bar(w, 0, &chip); // diagnostic only: D3hot reads ~0 here; claim induces D0
    w->Out->Diag[2] = chip;
    MmUnmapIoSpace(bar, (SIZE_T)w->BarLen);
    w->Bar = NULL;
    w->BarMapped = FALSE;
    // PM state report (never written here — probe is zero-energy). Claim performs
    // the D0 induction and fails closed if config/BAR0 cannot come up.
    u16 pmcsr = 0xFFFF;
    PCI_SLOT_NUMBER slot; slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = w->Device; slot.u.bits.FunctionNumber = w->Fn;
    (void)HalGetBusDataByOffset(PCIConfiguration, w->Bus, slot.u.AsULONG, &pmcsr, 0x54, 2);
    w->Out->Diag[3] = pmcsr;
    GU_NOTE("probe: id=%#x subsys=%#x bar0=%#llx chip=%#x pm=%u\n",
            id, subsys, (unsigned long long)pa.QuadPart, chip, pmcsr);
    return STATUS_SUCCESS;
}

// Phase 1 · assets — u610:610-655 + load_booter u610:434-470, from gu_fw_blobs.h.
NTSTATUS GuTxAssets(GU_WORK *w)
{
    const u8 *d;
    u32 magic, version, image_size, sig_bytes;
    struct gpu_unlock_booter_hdr hdr;
    int ret;

    if (GuFwV67Size != GU_V67_SIZE)
        return GuErrToNt(-EBADMSG);
    // spans hash-bound at build time (make_fw_blobs.py parses the ELF once);
    // same bounds the runtime parser enforced (u610:625-635)
    w->ImageSpan.offset = GuGspImageOffset;
    w->ImageSpan.size = GuGspImageSize;
    w->SigSpan.offset = GuGspSigOffset;
    w->SigSpan.size = GuGspSigSize;
    ret = GuGspOpen(w);
    if (ret)
        return GuErrToNt(ret);
    if (!w->ImageSpan.size || w->ImageSpan.size > 128ull << 20 || w->SigSpan.size != 4096)
        return GuErrToNt(-EINVAL);
    ret = gpu_unlock_radix_plan(w->ImageSpan.size, &w->Radix);
    if (ret)
        return GuErrToNt(ret);
    if (GuFwBootSize != 24576)
        return GuErrToNt(-EINVAL);
    ret = gpu_unlock_boot_desc_parse(GuFwBootDesc, GuFwBootDescSize,
                                     GuFwBootSize, &w->BootDesc);
    if (ret)
        return GuErrToNt(ret);
    // load_booter
    d = GuFwBooter;
    if (GuFwBooterSize < 72)
        return GuErrToNt(-EINVAL);
    RtlCopyMemory(&magic, d, 4);
    RtlCopyMemory(&version, d + 4, 4);
    if (magic != BOOTER_MAGIC || version != 1)
        return GuErrToNt(-EINVAL);
    RtlCopyMemory(&hdr, d + 8, sizeof(hdr));
    RtlCopyMemory(&image_size, d + 8 + sizeof(hdr), 4);
    RtlCopyMemory(&sig_bytes, d + 12 + sizeof(hdr), 4);
    if (!hdr.ucode_id || hdr.num_apps != 1 || sig_bytes != hdr.sig_bytes ||
        image_size != GU_BOOTER_IMAGE_SIZE ||
        GuFwBooterSize != 72u + image_size + sig_bytes)
        return GuErrToNt(-EINVAL);
    if (hdr.ucode_id != 3 || hdr.engine_id != 1 || hdr.num_sigs != 2 ||
        sig_bytes != GU_BOOTER_SIG_TOTAL || hdr.os_code_off != 0 ||
        hdr.os_code_size != 256 || hdr.os_data_off != 35328 ||
        hdr.os_data_size != 25088 || hdr.app_code_off != 256 ||
        hdr.app_code_size != 35072 || hdr.app_data_off != 256 ||
        hdr.app_data_size != 0 || hdr.patch_loc != 35344)
        return GuErrToNt(-EINVAL);
    w->BooterHdr = hdr;
    w->BooterImageSize = image_size;
    GU_NOTE("assets ok: image=%llu sig=%llu boot=%u\n",
            (unsigned long long)w->ImageSpan.size, (unsigned long long)w->SigSpan.size,
            w->BootDesc.image_size);
    return STATUS_SUCCESS;
}

// Phase 2 · claim — u610:801-876 + Windows specifics: cap walk, D0 induction.
NTSTATUS GuTxClaim(GU_WORK *w)
{
    u16 command, pmcsr;
    u32 cap;
    int ret;
    ULONG off, hdr;

    // capability walk (Linux pci_find_capability)
    w->PciCapOff = -1; w->PmCapOff = -1;
    {
        u16 capPtr = 0;
        if (GuCfgWordR(w, 0x34, &capPtr, FALSE))
            return GuErrToNt(-ENODEV);
        off = capPtr & 0xFC;
    }
    for (ULONG i = 0; i < 48 && off >= 0x40; i++) {
        if (GuCfgDwordR(w, off, &hdr, FALSE))
            return GuErrToNt(-ENODEV);
        if ((hdr & 0xFF) == 0x10 && w->PciCapOff < 0)
            w->PciCapOff = (LONG)off;
        if ((hdr & 0xFF) == 0x01 && w->PmCapOff < 0)
            w->PmCapOff = (LONG)off;
        ULONG next = (hdr >> 8) & 0xFC;
        if (!next)
            break;
        off = next;
    }
    if (w->PciCapOff < 0)
        return GuErrToNt(-EOPNOTSUPP);

    // D0 induction: Windows PnP disable may leave D3hot. Linux runs pre-driver
    // in D0 and refuses otherwise; on Windows we must ask for D0 ourselves.
    if (w->PmCapOff >= 0) {
        ret = GuCfgWordR(w, PCI_PM_CTRL, &pmcsr, TRUE);
        if (ret)
            return GuErrToNt(ret);
        if ((pmcsr & PCI_PM_CTRL_STATE_MASK) != 0) {
            GU_NOTE("claim: PM state D%u -> inducing D0\n", pmcsr & PCI_PM_CTRL_STATE_MASK);
            if (GuCfgWordW(w, PCI_PM_CTRL, pmcsr & ~PCI_PM_CTRL_STATE_MASK, TRUE))
                return GuErrToNt(-EIO);
            LARGE_INTEGER t; t.QuadPart = -2 * 10000; // 2 ms
            KeDelayExecutionThread(KernelMode, FALSE, &t);
            if (GuCfgWordR(w, PCI_PM_CTRL, &pmcsr, TRUE) ||
                (pmcsr & PCI_PM_CTRL_STATE_MASK) != 0)
                return GuErrToNt(-EOPNOTSUPP); // slot keeps it powered down: fail closed
        }
    }

    // FLR capability gate (u610:818-822)
    ret = GuCfgDwordR(w, PCI_EXP_DEVCAP, &cap, TRUE);
    if (ret || !(cap & PCI_EXP_DEVCAP_FLR))
        return GuErrToNt(-EOPNOTSUPP);

    // command snapshot + firmware BME clear (u610:823-841)
    ret = GuCfgWordR(w, PCI_COMMAND, &command, FALSE);
    if (ret || command == 0xffff)
        return GuErrToNt(-ENODEV);
    w->OriginalCmd = command;
    w->CmdValid = TRUE;
    if (command & GU_PCI_COMMAND_MASTER) {
        command &= (u16)~GU_PCI_COMMAND_MASTER;
        if (GuCfgWordW(w, PCI_COMMAND, command, FALSE) ||
            GuCfgWordR(w, PCI_COMMAND, &command, FALSE) ||
            command == 0xffff || (command & GU_PCI_COMMAND_MASTER))
            return GuErrToNt(-EBUSY);
        w->OriginalCmd = command; // firmware's BME stays off (not resume)
    }
    // pci_enable_device_mem analog (u610:854-859)
    if (!(command & GU_PCI_COMMAND_MEMORY)) {
        command |= GU_PCI_COMMAND_MEMORY;
        if (GuCfgWordW(w, PCI_COMMAND, command, FALSE) ||
            GuCfgWordR(w, PCI_COMMAND, &command, FALSE) ||
            !(command & GU_PCI_COMMAND_MEMORY))
            return GuErrToNt(-EIO);
    }

    // snapshot + BAR0 map (u610:860-875). GA10x BAR0 is 16 MiB 32-bit.
    GuSaveConfig(w);
    w->BarLen = 0x1000000;
    if (w->BarLen < 0x421000)
        return GuErrToNt(-EINVAL);
    PHYSICAL_ADDRESS barPa; barPa.QuadPart = w->BarPa;
    void *bar = MmMapIoSpace(barPa, (SIZE_T)w->BarLen, MmNonCached);
    if (!bar)
        return GuErrToNt(-ENOMEM);
    w->Bar = (volatile u32 *)bar;
    w->BarMapped = TRUE;
    GU_NOTE("claim ok: cmd=%#x devcap=%#x bar0=%#llx\n",
            command, cap, (unsigned long long)w->BarPa);
    return STATUS_SUCCESS;
}

// Phase 3 · induce — NEW. FLR away this power cycle's driver-initialized state
// (WPR2 up, GSP-RM run) so the ROM boot path replays and the tower starts from
// the cold entry the Linux module gets by running pre-driver. Mechanics ported
// from the source's own reset phase (u610:1333-1430), minus selector logic.
NTSTATUS GuTxInduce(GU_WORK *w)
{
    u32 identity, bar, chip;
    u16 command;
    int ret;

    if (read_bar(w, 0, &chip) || !chip || inaccessible(chip))
        return GuErrToNt(-ENODEV);
    if (GuCfgWordR(w, PCI_COMMAND, &command, FALSE) ||
        command == 0xffff || (command & GU_PCI_COMMAND_MASTER))
        return GuErrToNt(-EIO);
    GU_NOTE("induce: FLR begin chip=%#x\n", chip);
    ret = GuPcieFlr(w);
    GU_NOTE("induce: FLR returned %d\n", ret);
    if (ret)
        return GuErrToNt(ret);
    if (GuRestoreConfig(w))
        return GuErrToNt(-ENODEV);
    if (GuCfgDwordR(w, PCI_VENDOR_ID, &identity, FALSE) || identity != w->Identity ||
        GuCfgWordR(w, PCI_COMMAND, &command, FALSE) ||
        command == 0xffff || (command & GU_PCI_COMMAND_MASTER) ||
        !(command & GU_PCI_COMMAND_MEMORY))
        return GuErrToNt(-ENODEV);
    for (int i = 0; i < 6; i++) {
        if (GuCfgDwordR(w, PCI_BASE_ADDRESS_0 + i * 4, &bar, FALSE) || bar != w->SavedBars[i]) {
            GU_ERROR("induce: BAR%u actual=%#x expected=%#x\n", i, bar, w->SavedBars[i]);
            return GuErrToNt(-EIO);
        }
    }
    ret = wait_post_flr_bar(w, chip);
    if (ret)
        return GuErrToNt(ret);
    ret = wait_post_flr_gfw(w);
    if (ret)
        return GuErrToNt(ret);
    ret = hold_sec2_reset(w);
    if (ret)
        return GuErrToNt(ret);
    GU_NOTE("induce: card returned to cold entry (GFW complete, WPR2 down)\n");
    return STATUS_SUCCESS;
}

// Phase 4 · gfw — u610:1028-1074. Entry evidence + wait for ROM GFW.
NTSTATUS GuTxGfw(GU_WORK *w)
{
    u64 deadline = GuNowUs() + GU_GFW_TIMEOUT_US;
    int assessed = -EAGAIN;
    struct gpu_unlock_gfw_sample sample;

    for (;;) {
        u16 vendor = 0xffff;
        RtlZeroMemory(&sample, sizeof(sample));
        if (read_bar(w, GU_GFW_PLM_OFFSET, &sample.plm) ||
            read_bar(w, GU_GFW_BOOT_OFFSET, &sample.boot) ||
            read_bar(w, GU_GFW_CPUCTL_OFFSET, &sample.cpuctl))
            return GuErrToNt(-EIO);
        if (!GuCfgWordR(w, PCI_VENDOR_ID, &vendor, FALSE) && vendor != 0xffff)
            sample.config_ok = TRUE;
        assessed = gpu_unlock_gfw_assess(&sample);
        if (!assessed) {
            u32 ss0, ss1, wpr_hi;
            if (read_bar(w, GU_FEAT_PLM_OFFSET, &w->PlmEntry) ||
                read_bar(w, GU_FEAT_SS0_OFFSET, &ss0) || ss0 == ~0u ||
                read_bar(w, GU_FEAT_SS1_OFFSET, &ss1) || ss1 == ~0u ||
                read_bar(w, 0x001fa828, &wpr_hi) || inaccessible(wpr_hi))
                return GuErrToNt(-ENODEV);
            GU_NOTE("entry: plm=%#x ss0=%#x ss1=%#x wpr2_hi=%#x\n",
                    w->PlmEntry, ss0, ss1, wpr_hi);
            if (w->PlmEntry == ~0u)
                return GuErrToNt(-EALREADY); // already unlocked or not cold
            if (wpr_hi >> 4) {
                GU_ERROR("card initialised since power on (WPR2 enabled)\n");
                return GuErrToNt(-EBUSY);
            }
            return STATUS_SUCCESS;
        }
        if (assessed != -EAGAIN)
            return GuErrToNt(assessed);
        if (GuNowUs() >= deadline)
            return GuErrToNt(-ETIMEDOUT);
        GuUsleep(1000);
    }
}

// Phase 5 · layout — u610:657-747.
NTSTATUS GuTxLayout(GU_WORK *w)
{
    struct gpu_unlock_layout_in *in = &w->LayoutIn;
    u32 mb, vga, lo, hi, plm;
    int ret;

    if (read_bar(w, 0x001183a4, &mb) || !mb || mb == ~0u ||
        read_bar(w, 0x00625f04, &vga) || vga == ~0u ||
        read_bar(w, 0x001fa7c8, &plm) || plm == ~0u || !(plm & 1) ||
        read_bar(w, 0x001fa82c, &lo) || lo == ~0u ||
        read_bar(w, 0x001fa830, &hi) || hi == ~0u)
        return GuErrToNt(-ENODEV);
    GU_NOTE("layout inputs: fb_mb=%#x vga=%#x mmu=%#x:%#x:%#x image=%llu boot=%u\n",
            mb, vga, plm, lo, hi, (unsigned long long)w->ImageSpan.size,
            w->BootDesc.image_size);
    in->usable_fb = (u64)mb << 20;
    if (in->usable_fb < 256ull << 20)
        return GuErrToNt(-ERANGE);
    in->vga_base = in->usable_fb - (1ull << 20);
    if (vga & (1u << 3)) {
        in->vga_base = (u64)(vga >> 8) << 16;
        if (in->vga_base < in->usable_fb - (1ull << 20))
            in->vga_base = in->usable_fb - 0x20000;
    }
    if (in->vga_base >= in->usable_fb)
        return GuErrToNt(-ERANGE);
    in->mmu_lock_low = (u64)(lo >> 4) << 12;
    {
        u64 mmu_hi = (u64)(hi >> 4) << 12;
        u64 mmu_top = mmu_hi > ~0ull - 0xffffull ? ~0ull : (mmu_hi + 0xffffull) & ~0xffffull;
        bool top_lock;

        in->mmu_lock_valid = mmu_hi >= in->mmu_lock_low;
        top_lock = in->mmu_lock_valid && (mmu_top >> 20) == (in->usable_fb >> 20);
        if (!top_lock)
            in->mmu_lock_valid = FALSE;
    }
    in->frts_size = 1ull << 20;
    in->boot_size = w->BootDesc.image_size;
    in->image_size = w->ImageSpan.size;
    in->nonwpr_size = 1ull << 20;
    in->heap_request = 88ull << 20;
    ret = gpu_unlock_layout(in, &w->Layout);
    if (ret)
        return GuErrToNt(ret);
    if (w->Layout.nonwpr_start < in->usable_fb - (256ull << 20))
        return GuErrToNt(-ERANGE);
    GU_NOTE("layout: wpr=%#llx..%#llx gsp=%#llx boot=%#llx frts=%#llx\n",
            (unsigned long long)w->Layout.wpr_start, (unsigned long long)w->Layout.wpr_end,
            (unsigned long long)w->Layout.gsp_offset, (unsigned long long)w->Layout.boot_offset,
            (unsigned long long)w->Layout.frts_offset);
    return STATUS_SUCCESS;
}

// radix builder — u610:749-799.
static int unlock_transaction_radix(GU_WORK *w)
{
    const struct gpu_unlock_radix_plan *r = &w->Radix;
    u64 *addresses;
    u32 i, level, first = 0;
    int ret = 0;

    w->PageCount = (u32)(r->table_pages + r->pages[3]);
    w->Pages = GuAllocZero(w->PageCount * sizeof(*w->Pages));
    addresses = GuAllocZero(w->PageCount * sizeof(*addresses));
    if (!w->Pages || !addresses) {
        GuFree(addresses);
        return -ENOMEM;
    }
    for (i = 0; i < w->PageCount; i++) {
        struct GU_PAGE *page = &w->Pages[i];
        page->Cpu = GuDmaAlloc(GU_RADIX_PAGE, &page->Dma);
        if (!page->Cpu) { ret = -ENOMEM; goto out; }
        w->PagesAllocated++;
        addresses[i] = (u64)page->Dma.QuadPart;
        RtlZeroMemory(page->Cpu, GU_RADIX_PAGE);
        if (i >= r->table_pages) {
            // local 4 KiB fiber of the image, read straight into its page
            u64 offset = (i - r->table_pages) * GU_RADIX_PAGE;
            SIZE_T n = w->ImageSpan.size - offset < GU_RADIX_PAGE ?
                       (SIZE_T)(w->ImageSpan.size - offset) : GU_RADIX_PAGE;
            ret = GuGspRead(w, w->ImageSpan.offset + offset, page->Cpu, n);
            if (ret)
                goto out;
        }
    }
    for (level = 0; level < 3; level++) {
        u32 child = first + (u32)r->pages[level];
        for (i = 0; i < r->pages[level]; i++) {
            u32 at = i * (GU_RADIX_PAGE / 8);
            u32 n = r->pages[level + 1] - at < GU_RADIX_PAGE / 8 ?
                    (u32)(r->pages[level + 1] - at) : GU_RADIX_PAGE / 8;
            ret = gpu_unlock_radix_page(w->Pages[first + i].Cpu,
                                        addresses + child + at, n);
            if (ret)
                goto out;
        }
        first = child;
    }
out:
    GuFree(addresses);
    return ret;
}

// patch_booter — u610:472-487.
static int patch_booter(GU_WORK *w)
{
    u32 fuse_raw, fuse_ver;
    u32 index = w->BooterHdr.ucode_id - 1;

    if (read_bar(w, GU_FUSE_SEC2_UCODE1 + index * 4, &fuse_raw))
        return -EIO;
    fuse_ver = gpu_unlock_fuse_version(fuse_raw);
    GU_NOTE("booter sig: fuse_raw=%#x version=%u\n", fuse_raw, fuse_ver);
    return gpu_unlock_booter_patch(w->BooterCpu, w->BooterImageSize, &w->BooterHdr,
                                   GuFwBooter + 72 + w->BooterImageSize,
                                   w->BooterHdr.sig_bytes, fuse_ver);
}

// Phase 6 · dma — u610:878-919 (mask juggling is implicit in GuDmaAlloc's 4 GiB cap).
NTSTATUS GuTxDma(GU_WORK *w)
{
    w->BooterCpu = GuDmaAlloc(w->BooterImageSize, &w->BooterDma);
    if (!w->BooterCpu)
        return GuErrToNt(-ENOMEM);
    RtlCopyMemory(w->BooterCpu, GuFwBooter + 72, w->BooterImageSize);
    w->V67Cpu = GuDmaAlloc(GU_V67_SIZE, &w->V67Dma);
    w->MetaCpu = GuDmaAlloc(GU_WPR_META_SIZE, &w->MetaDma);
    if (!w->V67Cpu || !w->MetaCpu)
        return GuErrToNt(-ENOMEM);
    RtlCopyMemory(w->V67Cpu, GuFwV67, GU_V67_SIZE);
    w->StockSize = w->SigSpan.size;
    w->StockCpu = GuDmaAlloc((SIZE_T)w->StockSize, &w->StockDma);
    w->BootCpu = GuDmaAlloc(w->BootDesc.image_size, &w->BootDma);
    if (!w->StockCpu || !w->BootCpu)
        return GuErrToNt(-ENOMEM);
    {
        int r2 = GuGspRead(w, w->SigSpan.offset, w->StockCpu, (SIZE_T)w->StockSize);
        if (r2)
            return GuErrToNt(r2);
    }
    RtlCopyMemory(w->BootCpu, GuFwBoot, w->BootDesc.image_size);
    {
        int ret = patch_booter(w);
        return GuErrToNt(ret ? ret : unlock_transaction_radix(w));
    }
}

// Phase 7 · fwsec — u610:921-1026. Runs the board ROM's FWSEC-FRTS on the GSP
// falcon to provision WPR2 at the layout's FRTS offset.
NTSTATUS GuTxFwsec(GU_WORK *w)
{
    SIZE_T mapped = 0;
    u8 *rom = NULL;
    struct gpu_unlock_fwsec fw;
    u32 fuse_raw, version, scratch, wpr_lo, wpr_hi;
    u16 command;
    u64 frts = w->Layout.frts_offset;
    void *image = NULL;
    PHYSICAL_ADDRESS image_dma;
    int ret;

    GU_NOTE("FWSEC: reading PROM at 0x300000\n");
    mapped = (SIZE_T)(1024 * 1024);
    rom = GuAllocZero(mapped);
    if (!rom)
        return GuErrToNt(-ENOMEM);
    RtlCopyMemory(rom, (PUCHAR)w->Bar + 0x300000, mapped); // memcpy_fromio
    GU_NOTE("FWSEC: PROM bytes=%llu sig=%#x\n", (unsigned long long)mapped,
            (unsigned)(rom[0] | (rom[1] << 8)));
    ret = gpu_unlock_vbios_fwsec(rom, mapped, &fw);
    if (ret) {
        GU_ERROR("FWSEC-FRTS not found in board ROM (%d)\n", ret);
        goto out;
    }
    if (fw.engine_mask != 0x400) {
        GU_ERROR("FWSEC: unsupported GSP engine mask=%#x\n", fw.engine_mask);
        ret = -EOPNOTSUPP;
        goto out;
    }
    image = GuDmaAlloc(fw.image_size, &image_dma);
    if (!image) { ret = -ENOMEM; goto out; }
    w->FwsecCpu = image;
    w->FwsecDma = image_dma;
    w->FwsecSize = fw.image_size;
    RtlCopyMemory(image, rom + fw.image_off, fw.image_size);
    ret = gpu_unlock_fwsec_patch_frts(image, &fw, frts);
    if (ret)
        goto out;
    if (!fw.ucode_id ||
        read_bar(w, 0x008241c0 + (fw.ucode_id - 1) * 4, &fuse_raw)) {
        ret = -EIO;
        goto out;
    }
    version = gpu_unlock_fuse_version(fuse_raw);
    GU_NOTE("FWSEC fuse: raw=%#x version=%u\n", fuse_raw, version);
    {
        u32 sig_at = 0;
        ret = gpu_unlock_fwsec_sig_offset(fw.sig_versions, version, 384, &sig_at);
        if (ret)
            goto out;
        RtlCopyMemory((u8 *)image + fw.data_off + fw.pkc_off, rom + fw.sig_off + sig_at, 384);
    }
    // pci_set_master (u610:985)
    {
        u16 cmd;
        if (GuCfgWordR(w, PCI_COMMAND, &cmd, FALSE))
            { ret = -EIO; goto out; }
        if (GuCfgWordW(w, PCI_COMMAND, cmd | GU_PCI_COMMAND_MASTER, FALSE) ||
            GuCfgWordR(w, PCI_COMMAND, &command, FALSE) ||
            command == 0xffff || !(command & GU_PCI_COMMAND_MASTER))
            { ret = -EIO; goto out; }
    }
    w->FirmwareStarted = TRUE;
    ret = falcon_hs(w, &GuGspEng, 0, fw.imem_size, fw.imem_va, fw.imem_pa,
                    fw.data_off, fw.dmem_size, fw.dmem_pa, fw.pkc_off,
                    fw.engine_mask, fw.ucode_id,
                    (u64)image_dma.QuadPart, 0, 0, false);
    if (ret)
        goto out;
    if (read_bar(w, 0x00001400 + 0x0e * 4, &scratch)) {
        ret = -EIO;
        goto out;
    }
    GU_NOTE("FWSEC result: scratch=%#x expected_hi16=0\n", scratch);
    if (scratch >> 16) {
        ret = -EIO;
        goto out;
    }
    if (read_bar(w, 0x001fa824, &wpr_lo) || wpr_lo == ~0u ||
        read_bar(w, 0x001fa828, &wpr_hi) || wpr_hi == ~0u) {
        ret = -EIO;
        goto out;
    }
    GU_NOTE("FWSEC WPR2: low=%#x high=%#x page=%#llx end=%#llx\n",
            wpr_lo, wpr_hi, (unsigned long long)(frts >> 12),
            (unsigned long long)w->Layout.wpr_end);
    if (!(wpr_hi >> 4) || (wpr_hi >> 4) < (wpr_lo >> 4) ||
        ((u64)(wpr_hi >> 4) << 12) >= w->Layout.wpr_end ||
        (wpr_lo >> 4) != (u32)(frts >> 12))
        ret = -EIO;
out:
    GuFree(rom);
    return GuErrToNt(ret);
}

// Phase 8/10 · signature — u610:1137-1167. First call publishes V67 (the ROP
// payload's signature slot), second restores the stock signature.
NTSTATUS GuTxSignature(GU_WORK *w)
{
    u8 *meta = w->MetaCpu;

    if (!meta || !w->StockCpu || !w->PagesAllocated || w->SignatureCalls >= 2)
        return GuErrToNt(-EINVAL);
    if (w->SignatureCalls == 0) {
        struct gpu_unlock_wpr_args args;
        RtlZeroMemory(&args, sizeof(args));
        args.fb_size = w->LayoutIn.usable_fb;
        args.vga_offset = w->LayoutIn.vga_base;
        args.frts_size = w->LayoutIn.frts_size;
        args.signature_dma = (u64)w->V67Dma.QuadPart;
        args.signature_size = GU_V67_SIZE;
        args.radix_dma = (u64)w->Pages[0].Dma.QuadPart;
        args.image_size = w->ImageSpan.size;
        args.boot_dma = (u64)w->BootDma.QuadPart;
        args.boot = w->BootDesc;
        int ret = gpu_unlock_wpr_meta_pack(meta, &w->Layout, &args);
        if (ret)
            return GuErrToNt(ret);
        KeMemoryBarrier(); // dma_wmb()
        w->Published = TRUE;
        GU_NOTE("signature: V67 published\n");
    } else {
        put_le64(meta + 72, (u64)w->StockDma.QuadPart);
        put_le64(meta + 80, w->StockSize);
        KeMemoryBarrier();
        GU_NOTE("signature: stock restored\n");
    }
    w->SignatureCalls++;
    return STATUS_SUCCESS;
}

// Phase 9 · boot — u610:1113-1127. Starts the patched Booter on SEC2; the ROP
// chain executes and FEAT_OVR_PLM flips to ~0 (wait_plm loop in falcon_hs).
NTSTATUS GuTxBoot(GU_WORK *w)
{
    const struct gpu_unlock_booter_hdr *h = &w->BooterHdr;

    if (!w->Published || !w->FirmwareStarted)
        return GuErrToNt(-EINVAL);
    return GuErrToNt(falcon_hs(w, &GuSec2Eng, h->app_code_off, h->app_code_size,
                    h->app_code_off, 0, h->os_data_off, h->os_data_size, 0,
                    h->patch_loc - h->os_data_off, h->engine_id, h->ucode_id,
                    (u64)w->BooterDma.QuadPart,
                    (u32)w->MetaDma.QuadPart, (u32)((u64)w->MetaDma.QuadPart >> 32),
                    true));
}

// Phase 11 · selector — u610:1170-1194. SS0/SS1 full-rate writes with readback.
static int write_selector(GU_WORK *w, const char *name, u32 offset, u32 unlock)
{
    u32 value;

    if (read_bar(w, offset, &value) || inaccessible(value))
        return -ENODEV;
    if (value == unlock)
        return 0;
    GU_NOTE("selector write: %s=%#x\n", name, unlock);
    write_bar(w, offset, unlock);
    if (read_bar(w, offset, &value) || value == ~0u)
        return -ENODEV;
    return value == unlock ? 0 : -EIO;
}

NTSTATUS GuTxSelector(GU_WORK *w)
{
    NTSTATUS st = GuErrToNt(write_selector(w, "SS0", GU_FEAT_SS0_OFFSET, GU_SS0_UNLOCK));
    if (NT_SUCCESS(st)) {
        st = GuErrToNt(write_selector(w, "SS1", GU_FEAT_SS1_OFFSET, GU_SS1_UNLOCK));
        if (NT_SUCCESS(st)) {
            w->Out->SelectorReadback[0] = GU_SS0_UNLOCK;
            w->Out->SelectorReadback[1] = w->LastValue;
        }
    }
    return st;
}

// Phase 12 · reset — u610:1333-1430. Cleanup FLR with full containment.
NTSTATUS GuTxReset(GU_WORK *w)
{
    u32 ss0, ss1, plm, chip, cpuctl;
    u16 command, pending;
    int ret, i;

    if (w->SignatureCalls != 2)
        return GuErrToNt(-EPROTO);
    if (read_bar(w, GU_FEAT_SS0_OFFSET, &ss0) || ss0 == ~0u ||
        read_bar(w, GU_FEAT_SS1_OFFSET, &ss1) || ss1 == ~0u)
        return GuErrToNt(-ENODEV);
    if (ss0 != GU_SS0_UNLOCK || ss1 != GU_SS1_UNLOCK)
        return GuErrToNt(-EIO);
    // pci_clear_master (u610:1349)
    if (GuCfgWordR(w, PCI_COMMAND, &command, FALSE) ||
        GuCfgWordW(w, PCI_COMMAND, command & (u16)~GU_PCI_COMMAND_MASTER, FALSE) ||
        GuCfgWordR(w, PCI_COMMAND, &command, FALSE) ||
        command == 0xffff || (command & GU_PCI_COMMAND_MASTER))
        return GuErrToNt(-EIO);
    if (read_bar(w, 0, &chip) || !chip || inaccessible(chip))
        return GuErrToNt(-ENODEV);
    // No firmware framebuffer owner exists on Windows with the device disabled;
    // aperture removal is a no-op here (Linux-only concern).
    GU_NOTE("PCIe FLR begin: BME=0 DMA retained chip=%#x\n", chip);
    ret = GuPcieFlr(w);
    GU_NOTE("PCIe FLR returned %d\n", ret);
    if (ret)
        return GuErrToNt(ret);
    if (GuRestoreConfig(w))
        return GuErrToNt(-ENODEV);
    if (GuCfgWordR(w, PCI_COMMAND, &command, FALSE) ||
        command == 0xffff || (command & GU_PCI_COMMAND_MASTER) ||
        !(command & GU_PCI_COMMAND_MEMORY))
        return GuErrToNt(-ENODEV);
    for (i = 0; i < 6; i++) {
        u32 v;
        if (GuCfgDwordR(w, PCI_BASE_ADDRESS_0 + i * 4, &v, FALSE) || v != w->SavedBars[i]) {
            GU_ERROR("post-FLR BAR%u: actual=%#x expected=%#x\n", i, v, w->SavedBars[i]);
            return GuErrToNt(-EIO);
        }
    }
    ret = wait_post_flr_bar(w, chip);
    if (ret)
        return GuErrToNt(ret);
    ret = wait_post_flr_gfw(w);
    if (ret)
        return GuErrToNt(ret);
    ret = hold_sec2_reset(w);
    if (ret)
        return GuErrToNt(ret);
    if (GuCfgWordR(w, PCI_EXP_DEVSTA, &pending, TRUE) ||
        pending == 0xffff || (pending & PCI_EXP_DEVSTA_TRPND))
        return GuErrToNt(-EBUSY);
    {
        u32 lo, hi;
        if (read_bar(w, GU_GFW_CPUCTL_OFFSET, &cpuctl) || inaccessible(cpuctl) ||
            !(cpuctl & GU_GFW_CPUCTL_HALTED) ||
            read_bar(w, 0x001fa824, &lo) || inaccessible(lo) ||
            read_bar(w, 0x001fa828, &hi) || inaccessible(hi) ||
            read_bar(w, GU_FEAT_SS0_OFFSET, &ss0) || inaccessible(ss0) ||
            read_bar(w, GU_FEAT_SS1_OFFSET, &ss1) || inaccessible(ss1) ||
            read_bar(w, GU_FEAT_PLM_OFFSET, &plm) ||
            (plm & 0xffff0000u) == 0xbadf0000u)
            return GuErrToNt(-ENODEV);
        GU_NOTE("post-FLR: SEC2 reset, GSP halted, BME=0 WPR2=%08x:%08x SS0=%08x SS1=%08x\n",
                lo, hi, ss0, ss1);
        if (hi >> 4) {
            GU_ERROR("post-FLR WPR2 remains enabled; stock driver requires HI.VAL=0\n");
            return GuErrToNt(-EBUSY);
        }
        w->Out->PostFlrBarChip = chip;
    }
    w->Quiesced = TRUE;
    if (ss0 != GU_SS0_UNLOCK || ss1 != GU_SS1_UNLOCK)
        return GuErrToNt(-EIO);
    return STATUS_SUCCESS;
}

// Phase 13 · unwind — u610:526-608 + u610:1486-1492 retention rule.
NTSTATUS GuTxUnwind(GU_WORK *w)
{
    u16 now, want;
    int ret = 0;

    // A timeout never authorizes freeing firmware-visible memory.
    if (w->FirmwareStarted && !w->Quiesced) {
        w->Retained = TRUE;
        GU_ERROR("firmware did not stop; keeping DMA buffers until power off\n");
        return GuErrToNt(-EBUSY); // gu_rt.c pins the driver object (module_get analog)
    }
    if (w->BarMapped) {
        MmUnmapIoSpace((void *)w->Bar, (SIZE_T)w->BarLen);
        w->Bar = NULL;
        w->BarMapped = FALSE;
    }
    free_coherent(w);
    GuGspClose(w);
    if (!w->CmdValid)
        return GuErrToNt(ret);
    if (GuCfgWordR(w, PCI_COMMAND, &now, FALSE) || now == 0xffff)
        return GuErrToNt(-ENODEV);
    want = gpu_unlock_command_restore(w->OriginalCmd, now);
    if (want != now && GuCfgWordW(w, PCI_COMMAND, want, FALSE))
        return GuErrToNt(-EIO);
    if (GuCfgWordR(w, PCI_COMMAND, &now, FALSE))
        return GuErrToNt(-EIO);
    GU_NOTE("restore PCI command: actual=%#x original=%#x\n", now, w->OriginalCmd);
    if ((now & (GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER)) !=
        (w->OriginalCmd & (GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER)))
        return GuErrToNt(-EIO);
    return GuErrToNt(ret);
}

// ================= errno -> NTSTATUS ==========================================

NTSTATUS GuErrToNt(int err)
{
    switch (err) {
    case 0:          return STATUS_SUCCESS;
    case -ENOMEM:    return STATUS_INSUFFICIENT_RESOURCES;
    case -ENODEV:    return STATUS_DEVICE_NOT_CONNECTED;
    case -EIO:       return STATUS_IO_DEVICE_ERROR;
    case -EINVAL:    return STATUS_INVALID_DEVICE_STATE;
    case -ETIMEDOUT: return STATUS_TIMEOUT;
    case -EBUSY:     return STATUS_DEVICE_BUSY;
    case -EALREADY:  return STATUS_ALREADY_COMPLETE;   // card already unlocked
    case -EOPNOTSUPP:return STATUS_NOT_SUPPORTED;
    case -EBADMSG:   return STATUS_DATA_ERROR;
    case -EPROTO:    return STATUS_PROTOCOL_ERROR;
    case -ERANGE:    return STATUS_INVALID_VIEW_SIZE;
    default:         return STATUS_UNSUCCESSFUL;
    }
}
