// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpu-unlock-rt · start-IRP lower filter, generation 2 (GuFltUnlock).
 *
 * Rebuilt after the 2026-10-26 MULTIPLE_IRP_COMPLETE_REQUESTS boot-loop
 * post-mortem. Governing principle: IRP single-completion BY CONSTRUCTION -
 * exactly one code path ever calls IoCompleteRequest, reachable on every IRP
 * fate, with no thread handoffs. The previous async-hold pattern (completion
 * routine + worker + IoMarkIrpPending) is banned.
 *
 * Safeguards (see docs/TRACK2-CHARTER.md):
 *   - crash-count self-disarm: DriverEntry's FIRST statement increments
 *     BootCount; >= 3 -> permanent pass-through until manual reset. A boot
 *     loop is structurally impossible: worst case is three crashes, then
 *     normal boots forever.
 *   - synchronous-wait start hold: the transaction runs in the dispatch
 *     thread at PASSIVE; the lower completion signals an event and returns
 *     MORE_PROCESSING unconditionally - we own completion from that instant.
 *   - hardware guard: only the 3060 Ti is ever acted on; every other display
 *     device gets pure pass-through.
 *   - registry gate Enable=1 required for any intervention; all refusals
 *     fail open into a normal boot.
 *
 * The event wait is deliberately INDEFINITE: the only party below us is
 * pci.sys completing a device start, which is as reliable as the kernel
 * itself; a timed-out wait that completed the IRP early would re-create the
 * double-completion edge this rewrite exists to remove. The transaction's own
 * internal budgets (a few tens of seconds worst case) bound the hold.
 */
#include <wdm.h>
#include "gu_rt.h"
#include "gu_transaction.h"
#include "gu_fw_blobs.h"

// SEH portability: real __try/__except under MSVC; compile-out under the
// GCC host-harness build (the guarded body still type-checks, the handler
// becomes dead code and is excluded by the linker)
#if defined(_MSC_VER)
#define GU_SEH_TRY __try
#define GU_SEH_EXCEPT(filter) __except (filter)
#else
#define GU_SEH_TRY if (1)
#define GU_SEH_EXCEPT(filter) else if (0)
#endif

typedef struct _GU_FLT_EXT {
    PDEVICE_OBJECT Next;
    PDEVICE_OBJECT Self;
    ULONG          Bus, Dev, Fn;
    BOOLEAN        IsTarget;
} GU_FLT_EXT;

static BOOLEAN gPassive = FALSE;   // crash-counter tripped: never intervene

extern void (*GuRetainHook)(void);
static void GuFltRetain(void) { /* filter lifetime = stack lifetime */ }

// ---- registry helpers (literal paths only; the swprintf class is banned) ----
static NTSTATUS GuFltOpenParams(HANDLE *h, ACCESS_MASK mask)
{
    UNICODE_STRING key;
    OBJECT_ATTRIBUTES oa;
    RtlInitUnicodeString(&key,
        L"\\REGISTRY\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\GuFltUnlock\\Parameters");
    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwCreateKey(h, mask, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
}

static void GuFltMark(const wchar_t *name, ULONG value)
{
    UNICODE_STRING val;
    HANDLE h;
    if (!NT_SUCCESS(GuFltOpenParams(&h, KEY_SET_VALUE)))
        return;
    RtlInitUnicodeString(&val, name);
    ZwSetValueKey(h, &val, 0, REG_DWORD, &value, sizeof(ULONG));
    // durability (review F3.1): the crash-counter write must survive a
    // bugcheck seconds later - lazy hive flush would lose it and re-enable
    // the boot loop this counter exists to break
    ZwFlushKey(h);
    ZwClose(h);
}

static ULONG GuFltReadDword(const wchar_t *name)
{
    ULONG result = 0, len = 0;
    HANDLE h;
    UNICODE_STRING val;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    if (!NT_SUCCESS(GuFltOpenParams(&h, KEY_QUERY_VALUE)))
        return 0;
    RtlInitUnicodeString(&val, name);
    if (NT_SUCCESS(ZwQueryValueKey(h, &val, KeyValuePartialInformation,
                                   buf, sizeof(buf), &len)) &&
        kv->Type == REG_DWORD && kv->DataLength == sizeof(ULONG))
        result = *(ULONG *)kv->Data;
    ZwClose(h);
    return result;
}

// ---- crash-count self-disarm: FIRST code the driver ever runs ---------------
static void GuBootArm(void)
{
    ULONG count = GuFltReadDword(L"BootCount");
    if (count >= 3) {
        gPassive = TRUE;             // boot-loop breaker: permanent pass-through
        GuFltMark(L"Passive", 1);
        return;
    }
    GuFltMark(L"BootCount", count + 1);
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

    // guard INSIDE this frame (review F6.1): w must stay live so the handler
    // can run the real unwind - mapping/handle/DMA cleanup and the
    // keep-if-firmware-running decision - instead of leaking and handing
    // nvlddmkm a torn GPU with a SUCCESS start
    GU_SEH_TRY {
        st = GuRunTower(&w, 12);
    }
    GU_SEH_EXCEPT (EXCEPTION_EXECUTE_HANDLER) {
        (void)GuTxUnwind(&w);
        st = STATUS_UNSUCCESSFUL;
        GuFltMark(L"LastRunState", 4);   // guarded fault: unwound, boot proceeds
        GuFltMark(L"LastRunVerdict", (ULONG)st);
        return;
    }
    GuFltMark(L"LastRunState", 2);
    GuFltMark(L"LastRunReached", out.PhaseReached);
    GuFltMark(L"LastRunVerdict", (ULONG)st);
    GuFltMark(L"LastRunPhase2", (ULONG)out.PhaseStatus[2]);
    GuFltMark(L"LastRunPhase7", (ULONG)out.PhaseStatus[7]);
    GuFltMark(L"LastRunPhase12", (ULONG)out.PhaseStatus[12]);
    if (NT_SUCCESS(st))
        GuFltMark(L"BootCount", 0);   // success resets the crash counter
}

// ---- start hold: synchronous wait, single completion site ------------------
static NTSTATUS GuFltStartEventDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;   // unconditional ownership transfer
}

static NTSTATUS GuFltStartSerialized(GU_FLT_EXT *ext, PIRP irp)
{
    KEVENT done;
    NTSTATUS st;

    KeInitializeEvent(&done, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, GuFltStartEventDone, &done, TRUE, TRUE, TRUE);
    st = IoCallDriver(ext->Next, irp);
    if (st == STATUS_PENDING)
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL); // indefinite, by design

    st = irp->IoStatus.Status;                 // lower drivers' verdict
    if (NT_SUCCESS(st))
        GuFltRunTransaction(ext);              // SEH guard lives inside, with unwind
    IoCompleteRequest(irp, IO_NO_INCREMENT);   // the ONLY completion site
    return st;
}

// ---- PnP ---------------------------------------------------------------------
static NTSTATUS GuFltPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    GU_FLT_EXT *ext = (GU_FLT_EXT *)dev->DeviceExtension;
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;

    switch (io->MinorFunction) {
    case IRP_MN_START_DEVICE: {
        // unconditional trace marks: every start through this filter is
        // visible even when the gate refuses (the armed run showed empty
        // marks with a 1s rebind - blind spot now closed)
        ULONG enableRead = GuFltReadDword(L"Enable");
        GuFltMark(L"StartSeen", 1);
        GuFltMark(L"StartTarget", ext->IsTarget ? 1 : 0);
        GuFltMark(L"StartPassive", gPassive ? 1 : 0);
        GuFltMark(L"StartEnable", enableRead);
        if (ext->IsTarget && !gPassive && enableRead == 1)
            return GuFltStartSerialized(ext, irp);
        IoSkipCurrentIrpStackLocation(irp);
        return IoCallDriver(ext->Next, irp);
    }
    case IRP_MN_REMOVE_DEVICE:
        IoSkipCurrentIrpStackLocation(irp);
        st = IoCallDriver(ext->Next, irp);
        IoDetachDevice(ext->Next);             // filter lifetime ends with the stack
        IoDeleteDevice(dev);
        return st;
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
    WCHAR ids[512];

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
    IoGetDeviceProperty(pdo, DevicePropertyBusNumber, sizeof(ULONG), &bus, &len);
    IoGetDeviceProperty(pdo, DevicePropertyAddress, sizeof(ULONG), &addr, &len);
    ext->Bus = bus;
    ext->Dev = (addr >> 16) & 0xFFFF;
    ext->Fn = addr & 0xFFFF;
    len = sizeof(ids);
    if (NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyHardwareID,
                                       sizeof(ids), ids, &len)))
        ext->IsTarget = (wcsstr(ids, L"VEN_10DE&DEV_2489") != NULL);
    if (ext->IsTarget) {
        GuFltMark(L"Attached", 1);
        GuFltMark(L"Bus", ext->Bus);
    }
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

    GuBootArm();                               // THE first statement: crash counter
    GuRetainHook = GuFltRetain;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        drv->MajorFunction[i] = GuFltPass;
    drv->MajorFunction[IRP_MJ_PNP] = GuFltPnp;
    drv->MajorFunction[IRP_MJ_POWER] = GuFltPass;
    drv->DriverExtension->AddDevice = GuFltAddDevice;
    drv->DriverUnload = GuFltUnload;
    GuFltMark(L"EntryStage", 1);
    return STATUS_SUCCESS;
}
