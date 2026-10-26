// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpu-unlock-rt · start-IRP lower-filter driver (GuFltUnlock).
 *
 * The in-stack serialization hook: attaches below nvlddmkm on the 3060 Ti.
 * On IRP_MN_START_DEVICE it passes the IRP down (pci.sys brings the card D0
 * and assigns resources), then HOLDS the IRP pending while the full unlock
 * transaction runs in a worker thread - the same serialization Linux gets
 * from initramfs load order. nvlddmkm's start (and its GSP boot) waits until
 * the tower completes, then binds onto an unlocked card. Fail-open by design:
 * any refusal (wrong identity, WPR2 up, EALREADY unlocked, registry gate off)
 * completes the start untouched and Windows boots normally.
 *
 * Enabled only via Parameters\Enable = 1 (REG_DWORD); absent/0 = pure
 * pass-through filter. Results land in Parameters\LastRun* values.
 */
#include <wdm.h>
#include "gu_rt.h"
#include "gu_transaction.h"
#include "gu_fw_blobs.h"

typedef struct _GU_FLT_EXT {
    PDEVICE_OBJECT Next;          // top of stack below us
    PDEVICE_OBJECT Self;
    struct _IO_WORKITEM *WorkItem; // opaque fwd: IO_WORKITEM typedef not visible here
    PIRP           HeldIrp;       // start IRP held during the transaction
    ULONG          Bus, Dev, Fn;
} GU_FLT_EXT;

extern void (*GuRetainHook)(void);
static void GuFltRetain(void) { /* nothing to pin: filter lifetime = stack lifetime */ }

// ---- result breadcrumb (safe literal path, no formatting) -----------------
static void GuFltMark(const wchar_t *name, ULONG value)
{
    UNICODE_STRING key, val;
    HANDLE h;
    OBJECT_ATTRIBUTES oa;
    RtlInitUnicodeString(&key,
        L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\GuFltUnlock\\Parameters");
    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&h, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL)))
        return;
    RtlInitUnicodeString(&val, name);
    ZwSetValueKey(h, &val, 0, REG_DWORD, &value, sizeof(ULONG));
    ZwClose(h);
}

static ULONG GuFltEnableRead(void)
{
    UNICODE_STRING key, val;
    HANDLE h;
    OBJECT_ATTRIBUTES oa;
    ULONG enable = 0, result = 0;
    ULONG len = sizeof(ULONG);
    KEY_VALUE_PARTIAL_INFORMATION *kv;
    RtlInitUnicodeString(&key,
        L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\GuFltUnlock\\Parameters");
    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_QUERY_VALUE, &oa)))
        return 0;
    RtlInitUnicodeString(&val, L"Enable");
    kv = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG), 'uGgF');
    if (kv) {
        if (NT_SUCCESS(ZwQueryValueKey(h, &val, KeyValuePartialInformation, kv,
                                       sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG), &len)) &&
            kv->Type == REG_DWORD && kv->DataLength == sizeof(ULONG))
            result = *(ULONG *)kv->Data;
        ExFreePool(kv);
    }
    ZwClose(h);
    (void)enable;
    return result;
}

// ---- the serialized transaction --------------------------------------------
static void GuFltRunTransaction(GU_FLT_EXT *ext)
{
    GU_RUN_OUT out;
    GU_WORK w;
    NTSTATUS st;

    RtlZeroMemory(&out, sizeof(out));
    RtlZeroMemory(&w, sizeof(w));
    w.Bus = ext->Bus; w.Device = ext->Dev; w.Fn = ext->Fn;
    w.PciCapOff = -1; w.PmCapOff = -1;
    w.Out = &out;
    w.OutLen = sizeof(out);

    st = GuRunTower(&w, 12);
    GuFltMark(L"LastRunState", 2);
    GuFltMark(L"LastRunReached", out.PhaseReached);
    GuFltMark(L"LastRunVerdict", (ULONG)st);
    GuFltMark(L"LastRunPhase2", (ULONG)out.PhaseStatus[2]);
    GuFltMark(L"LastRunPhase7", (ULONG)out.PhaseStatus[7]);
    GuFltMark(L"LastRunPhase12", (ULONG)out.PhaseStatus[12]);
}

static VOID GuFltStartWorker(PVOID ctx)
{
    GU_FLT_EXT *ext = (GU_FLT_EXT *)ctx;
    PIRP irp = ext->HeldIrp;
    ext->HeldIrp = NULL;

    __try {
        GuFltRunTransaction(ext);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GuFltMark(L"LastRunState", 4); // worker guarded: never fail the boot
    }
    // release nvlddmkm's start regardless of outcome (fail-open)
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static NTSTATUS GuFltStartCompleted(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    GU_FLT_EXT *ext = (GU_FLT_EXT *)ctx;
    UNREFERENCED_PARAMETER(dev);
    if (NT_SUCCESS(irp->IoStatus.Status) && ext->WorkItem && GuFltEnableRead()) {
        IoMarkIrpPending(irp);
        ext->HeldIrp = irp;
        IoQueueWorkItem(ext->WorkItem, GuFltStartWorker, DelayedWorkQueue, ext);
        return STATUS_MORE_PROCESSING_REQUIRED; // worker completes the IRP
    }
    return STATUS_CONTINUE_COMPLETION;          // pass-through (gate off/fail)
}

// ---- PnP ---------------------------------------------------------------------
static NTSTATUS GuFltPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    GU_FLT_EXT *ext = (GU_FLT_EXT *)dev->DeviceExtension;
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);

    switch (io->MinorFunction) {
    case IRP_MN_START_DEVICE:
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, GuFltStartCompleted, ext, TRUE, TRUE, TRUE);
        return IoCallDriver(ext->Next, irp);
    default:
        IoSkipCurrentIrpStackLocation(irp);
        return IoCallDriver(ext->Next, irp);
    }
}

static NTSTATUS GuFltPass(PDEVICE_OBJECT dev, PIRP irp)
{
    GU_FLT_EXT *ext = (GU_FLT_EXT *)dev->DeviceExtension;
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(ext->Next, irp);
}

static NTSTATUS GuFltAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    GU_FLT_EXT *ext;
    PDEVICE_OBJECT self;
    NTSTATUS st;
    ULONG bus = 0, addr = 0, len;

    st = IoCreateDevice(drv, sizeof(GU_FLT_EXT), NULL, FILE_DEVICE_UNKNOWN,
                        FILE_DEVICE_SECURE_OPEN, FALSE, &self);
    if (!NT_SUCCESS(st))
        return st;
    ext = (GU_FLT_EXT *)self->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->Self = self;
    ext->Next = IoAttachDeviceToDeviceStack(self, pdo);
    if (!ext->Next) {
        IoDeleteDevice(self);
        return STATUS_UNSUCCESSFUL;
    }
    ext->WorkItem = IoAllocateWorkItem(self);
    // BDF from the PDO properties (DevicePropertyAddress: dev<<16|fn)
    len = sizeof(ULONG);
    IoGetDeviceProperty(pdo, DevicePropertyBusNumber, sizeof(ULONG), &bus, &len);
    len = sizeof(ULONG);
    IoGetDeviceProperty(pdo, DevicePropertyAddress, sizeof(ULONG), &addr, &len);
    ext->Bus = bus;
    ext->Dev = (addr >> 16) & 0xFFFF;
    ext->Fn = addr & 0xFFFF;
    GuFltMark(L"Attached", 1);
    GuFltMark(L"Bus", ext->Bus);

    self->Flags |= ext->Next->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE | DO_POWER_INRUSH);
    self->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static void GuFltUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    ULONG i;
    UNREFERENCED_PARAMETER(reg);
    GuRetainHook = GuFltRetain;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        drv->MajorFunction[i] = GuFltPass;
    drv->MajorFunction[IRP_MJ_PNP] = GuFltPnp;
    drv->MajorFunction[IRP_MJ_POWER] = GuFltPass; // PO calls use the pass path
    drv->DriverExtension->AddDevice = GuFltAddDevice;
    drv->DriverUnload = GuFltUnload;
    return STATUS_SUCCESS;
}
