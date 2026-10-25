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

// ================= L4 · dispatcher (geodesic enforcement) ======================

// u610:1443-1495: monotone ascent; on failure republish the stock signature if
// V67 is still published (u610:1480-1482); unwind always runs; retention pins
// the driver (u610:1486-1491, __module_get analog = leaked driver reference).
static NTSTATUS GuRunTower(GU_WORK *w, ULONG phaseLimit)
{
    GU_PHASE_FN phases[GuPhaseCount] = {
        [GuPhaseProbe]      = GuTxProbe,
        [GuPhaseAssets]     = GuTxAssets,
        [GuPhaseClaim]      = GuTxClaim,
        [GuPhaseInduce]     = GuTxInduce,
        [GuPhaseGfw]        = GuTxGfw,
        [GuPhaseLayout]     = GuTxLayout,
        [GuPhaseDma]        = GuTxDma,
        [GuPhaseFwsec]      = GuTxFwsec,
        [GuPhaseSignature]  = GuTxSignature,
        [GuPhaseBoot]       = GuTxBoot,
        [GuPhaseSignature2] = GuTxSignature,
        [GuPhaseSelector]   = GuTxSelector,
        [GuPhaseReset]      = GuTxReset,
        [GuPhaseUnwind]     = GuTxUnwind,
    };
    NTSTATUS st = STATUS_SUCCESS;
    ULONG p, last = min(phaseLimit, (ULONG)GuPhaseReset);

    for (p = 0; p <= last; p++) {
        w->Out->PhaseReached = p;
        st = phases[p](w);
        w->Out->PhaseStatus[p] = st;
        if (!NT_SUCCESS(st))
            break;
    }
    // Never leave the V67 signature published for a later stock boot (u610:1481).
    if (!NT_SUCCESS(st) && w->SignatureCalls == 1 && w->MetaCpu)
        (void)GuTxSignature(w);
    // Unwind always runs (single descent geodesic), even after success.
    w->Out->PhaseStatus[GuPhaseUnwind] = phases[GuPhaseUnwind](w);
    w->Out->PhaseReached = GuPhaseUnwind;
    if (NT_SUCCESS(st) && !NT_SUCCESS(w->Out->PhaseStatus[GuPhaseUnwind]))
        st = w->Out->PhaseStatus[GuPhaseUnwind];
    // Firmware still running: keep buffers until power off (u610:1486-1491).
    if (w->Retained && gDrvObj) {
        ObReferenceObject(gDrvObj); // pins the driver: unload now fails safely
        st = STATUS_DEVICE_POWERED_OFF; // "power off before trying again"
    }
    return st;
}

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

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNREFERENCED_PARAMETER(reg);
    UNICODE_STRING name;
    PDEVICE_OBJECT dev;
    NTSTATUS st;

    gDrvObj = drv;
    RtlInitUnicodeString(&name, L"\\Device\\GuRtUnlock");
    st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN,
                        FILE_DEVICE_SECURE_OPEN, TRUE, &dev);
    if (!NT_SUCCESS(st))
        return st;
    UNICODE_STRING link;
    RtlInitUnicodeString(&link, L"\\DosDevices\\GuRtUnlock");
    IoCreateSymbolicLink(&link, &name);

    drv->MajorFunction[IRP_MJ_CREATE]         = GuCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = GuCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = GuDeviceControl;
    drv->DriverUnload = GuUnload;
    gControlDev = dev;
    return STATUS_SUCCESS;
}
