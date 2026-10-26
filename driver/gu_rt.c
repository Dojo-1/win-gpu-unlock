// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpu-unlock-rt · WDM control-device driver (L4).
 * Control device only — never binds the GPU PDO. Raw access goes through HAL
 * bus data + MmMapIoSpace on the config-reported BAR0, legal for an unowned
 * (PnP-disabled) function. The stock driver must be displaced by the L5
 * orchestrator before any arming phase runs; the in-kernel gate below refuses
 * arming phases without GU_F_CONFIRM_BLACKOUT (policy gate G2, fail closed).
 *
 * Port of the orchestration in Zagitta/gpu-unlock kernel/gpu_unlock/
 * unlock_610.c:1443-1495 (tower order, republish-on-failure, retention).
 */
#include "gu_rt.h"
#include "gu_transaction.h"
#include <wdm.h>

static PDEVICE_OBJECT gControlDev;
static PDRIVER_OBJECT gDrvObj;
extern void (*GuRetainHook)(void);
static void GuRetainSelf(void) { if (gDrvObj) ObReferenceObject(gDrvObj); }

// ============ rebind-race watch (ported Linux-boot-window insight) ============
// The Linux module wins its window by load order; on Windows the same state
// exists inside PnP rebind (D3cold -> D0 -> ... -> nvlddmkm GSP boot). This
// watch thread polls for config/MMIO validity and fires the full tower the
// instant the card is D0, racing nvlddmkm's GSP init. Losing the race is a
// clean refusal: the tower's WPR2-up check (-EBUSY) leaves the card alone.
// padded mailbox: the tower writes GU_RUN_OUT fields (PostFlrBarChip,
// SelectorReadback) at offsets 96..107 - beyond GU_WATCH_RESULT's 96 bytes
static struct { GU_WATCH_RESULT r; ULONG pad[8]; } gWatchBox;
#define gWatch (gWatchBox.r)
static LONG gWatchArmed = 0;
static ULONG gWatchBus, gWatchDev, gWatchFn;

static void GuWatchThread(PVOID ctx)
{
    GU_WORK w;
    LARGE_INTEGER t;
    u32 id = 0;
    int i;

    UNREFERENCED_PARAMETER(ctx);
    gWatch.State = 1;
    // poll up to 30 s for config validity (vendor dword readable, right ID)
    for (i = 0; i < 3000; i++) {
        PCI_SLOT_NUMBER slot; slot.u.AsULONG = 0;
        slot.u.bits.DeviceNumber = gWatchDev; slot.u.bits.FunctionNumber = gWatchFn;
        ULONG got = HalGetBusDataByOffset(PCIConfiguration, gWatchBus, slot.u.AsULONG, &id, 0, 4);
        if (got == 4 && id == 0x248910DEu)
            break;
        t.QuadPart = -10 * 10000; // 10 ms
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    if (i >= 3000 || id != 0x248910DEu) {
        gWatch.State = 3; // timeout: card never returned
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }
    // card is back: brief settle for power/decode, then fire the full tower
    t.QuadPart = -50 * 10000; // 50 ms
    KeDelayExecutionThread(KernelMode, FALSE, &t);
    RtlZeroMemory(&w, sizeof(w));
    w.Bus = gWatchBus; w.Device = gWatchDev; w.Fn = gWatchFn;
    w.PciCapOff = -1; w.PmCapOff = -1;
    w.Out = (struct _GU_RUN_OUT *)&gWatch; // phase/diag writes land in the mailbox
    (void)GuRunTower(&w, 12);
    gWatch.State = 2;
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ================= L4 · dispatcher (geodesic enforcement) ======================

// u610:1443-1495: monotone ascent; on failure republish the stock signature if
// V67 is still published (u610:1480-1482); unwind always runs; retention pins
// the driver (u610:1486-1491, __module_get analog = leaked driver reference).
// tower dispatcher now lives in gu_transaction.c (shared with the filter driver)

static NTSTATUS GuDispatchIoctl(PIRP Irp, PIO_STACK_LOCATION io)
{
    ULONG inLen  = io->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = io->Parameters.DeviceIoControl.OutputBufferLength;
    // breadcrumbs: stamp markers so early deaths are diagnosable off-hardware
    if (outLen >= 4) ((ULONG *)Irp->AssociatedIrp.SystemBuffer)[0] = 0xDEAD0000u | (inLen & 0xFF);
    if (inLen < sizeof(GU_RUN_IN) || outLen < sizeof(GU_RUN_OUT))
        return STATUS_BUFFER_TOO_SMALL;

    GU_RUN_IN req = *(GU_RUN_IN *)Irp->AssociatedIrp.SystemBuffer;
    GU_RUN_OUT *out = (GU_RUN_OUT *)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(out, sizeof(*out));
    out->Size = sizeof(*out);
    out->PhaseReached = (ULONG)-1;

    // In-kernel policy gate G2: without explicit blackout confirmation only the
    // non-arming probe may run. Fail closed (PORT-MANIFOLD §4).
    if (!(req.Flags & GU_F_CONFIRM_BLACKOUT) && req.PhaseLimit > GuPhaseProbe)
        return STATUS_ACCESS_DENIED;

    GU_WORK *w = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(GU_WORK), 0x55474755u);
    if (!w)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(w, sizeof(*w));
    w->Bus = req.Bus; w->Device = req.Device; w->Fn = req.Fn;
    w->PciCapOff = -1; w->PmCapOff = -1;
    w->Out = out;
    w->OutLen = outLen;
    w->Flags = req.Flags;

    NTSTATUS st = GuRunTower(w, req.PhaseLimit);
    out->Diag[7] = (ULONG)st; // verdict travels in-band: copyback happens on error status
    if (!w->Retained) // retained work stays leaked-by-design until power off
        ExFreePoolWithTag(w, 0x55474755u);
    Irp->IoStatus.Information = sizeof(*out);
    return STATUS_SUCCESS; // client reads PhaseReached/PhaseStatus/Diag[7]
}

// ================= L4 · boilerplate ============================================

static NTSTATUS GuCreateClose(PDEVICE_OBJECT d, PIRP i)
{
    UNREFERENCED_PARAMETER(d);
    i->IoStatus.Status = STATUS_SUCCESS;
    i->IoStatus.Information = 0;
    IoCompleteRequest(i, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS GuDeviceControl(PDEVICE_OBJECT d, PIRP i)
{
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(i);
    NTSTATUS st;

    UNREFERENCED_PARAMETER(d);
    switch (io->Parameters.DeviceIoControl.IoControlCode) {
    case GU_IOCTL_RUN:
        st = GuDispatchIoctl(i, io);
        break;
    case GU_IOCTL_WATCH_ARM: {
        GU_RUN_IN req = {0};
        HANDLE th;
        if (io->Parameters.DeviceIoControl.InputBufferLength < sizeof(GU_RUN_IN)) {
            st = STATUS_BUFFER_TOO_SMALL; break;
        }
        req = *(GU_RUN_IN *)i->AssociatedIrp.SystemBuffer;
        if (!(req.Flags & GU_F_CONFIRM_BLACKOUT)) { st = STATUS_ACCESS_DENIED; break; }
        if (InterlockedCompareExchange(&gWatchArmed, 1, 0) != 0) { st = STATUS_DEVICE_BUSY; break; }
        RtlZeroMemory(&gWatch, sizeof(gWatch));
        gWatchBus = req.Bus; gWatchDev = req.Device; gWatchFn = req.Fn;
        if (!NT_SUCCESS(PsCreateSystemThread(&th, 0, NULL, NULL, NULL, GuWatchThread, NULL))) {
            gWatchArmed = 0; st = STATUS_INSUFFICIENT_RESOURCES; break;
        }
        ZwClose(th);
        i->IoStatus.Information = 0;
        IoCompleteRequest(i, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    case GU_IOCTL_WATCH_POLL: {
        ULONG outLen = io->Parameters.DeviceIoControl.OutputBufferLength;
        if (outLen < sizeof(GU_WATCH_RESULT)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        RtlCopyMemory(i->AssociatedIrp.SystemBuffer, &gWatch, sizeof(GU_WATCH_RESULT));
        i->IoStatus.Information = sizeof(GU_WATCH_RESULT);
        IoCompleteRequest(i, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    // complete on EVERY path: an uncompleted IRP wedges the file object (and
    // with it driver unload) forever. No path in this driver pends.
    if (i->IoStatus.Status == 0)
        i->IoStatus.Status = st;
    IoCompleteRequest(i, IO_NO_INCREMENT);
    return st;
}

static void GuUnload(PDRIVER_OBJECT drv)
{
    UNICODE_STRING link;
    UNREFERENCED_PARAMETER(drv);
    // If a retained transaction pinned the driver object, the extra reference
    // prevents this unload from ever being reached — by design.
    RtlInitUnicodeString(&link, L"\\DosDevices\\GuRtUnlock");
    IoDeleteSymbolicLink(&link);
    IoDeleteDevice(gControlDev);
}

// DriverEntry progress breadcrumb under the service Parameters key (readable
// without elevation after any load attempt).
static void GuMarkStage(PDRIVER_OBJECT drv, PCWSTR stage)
{
    UNICODE_STRING key, val;
    HANDLE h;
    OBJECT_ATTRIBUTES oa;
    UNREFERENCED_PARAMETER(drv);
    // constant path - no formatting. The swprintf variant here was the 0x7E
    // root cause: the kernel CRT bound the size argument as the format
    // pointer, faulting on every load (identical RVA offset in both dumps).
    RtlInitUnicodeString(&key,
        L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\GuRtUnlock\\Parameters");
    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&h, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL)))
        return;
    RtlInitUnicodeString(&val, L"EntryStage");
    ZwSetValueKey(h, &val, 0, REG_SZ, (PVOID)stage, (ULONG)((wcslen(stage) + 1) * sizeof(WCHAR)));
    ZwClose(h);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNREFERENCED_PARAMETER(reg);
    UNICODE_STRING name;
    PDEVICE_OBJECT dev;
    NTSTATUS st;

    GuRetainHook = GuRetainSelf;
    GuMarkStage(drv, L"entry");
    gDrvObj = drv;
    RtlInitUnicodeString(&name, L"\\Device\\GuRtUnlock");
    st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN,
                        FILE_DEVICE_SECURE_OPEN, TRUE, &dev);
    if (!NT_SUCCESS(st)) {
        GuMarkStage(drv, L"dev-failed");
        return st;
    }
    GuMarkStage(drv, L"dev-ok");
    UNICODE_STRING link;
    RtlInitUnicodeString(&link, L"\\DosDevices\\GuRtUnlock");
    st = IoCreateSymbolicLink(&link, &name);
    GuMarkStage(drv, NT_SUCCESS(st) ? L"link-ok" : L"link-failed");
    if (!NT_SUCCESS(st))
        return st;

    drv->MajorFunction[IRP_MJ_CREATE]         = GuCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = GuCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = GuDeviceControl;
    drv->DriverUnload = GuUnload;
    gControlDev = dev;
    return STATUS_SUCCESS;
}
