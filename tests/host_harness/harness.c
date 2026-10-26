// SPDX-License-Identifier: GPL-2.0-only
// gpu-unlock-rt host harness: compiles+links the ENTIRE kernel-mode branch of
// the port against mock WDK APIs, then runs pure-computation smoke checks.
// Translation-unit verification only — no hardware semantics are mocked.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gu_rt.h"
#include "gu_transaction.h"
#include "gu_fw_blobs.h"

// ---- mock implementations -----------------------------------------------------
static unsigned long long g_now_100ns = 0;

int DbgPrintEx(int comp, int level, const char *fmt, ...)
{
    (void)comp; (void)level; (void)fmt;
    return 0;
}

void *ExAllocatePool2(unsigned long long flags, SIZE_T size, unsigned long tag)
{ (void)flags; (void)tag; return calloc(1, size ? size : 1); }

void ExFreePoolWithTag(void *p, unsigned long tag) { (void)tag; free(p); }

static void *g_mapio_last;
void *MmMapIoSpace(PHYSICAL_ADDRESS pa, SIZE_T n, int cache)
{ (void)pa; (void)cache; g_mapio_last = malloc(n); return g_mapio_last; }
void MmUnmapIoSpace(void *va, SIZE_T n) { (void)n; if (va == g_mapio_last) g_mapio_last = NULL; free(va); }

PHYSICAL_ADDRESS MmGetPhysicalAddress(void *va)
{ PHYSICAL_ADDRESS p; p.QuadPart = 0x10000000ull + (unsigned long long)(uintptr_t)va; return p; }
void *MmAllocateContiguousMemorySpecifyCache(SIZE_T n, PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi, PHYSICAL_ADDRESS boundary, int cache)
{ (void)lo; (void)hi; (void)boundary; (void)cache; return calloc(1, n); }
void MmFreeContiguousMemorySpecifyCache(void *va, SIZE_T n, int cache) { (void)n; (void)cache; free(va); }

unsigned long long KeQueryInterruptTime(void) { return g_now_100ns; }
int KeDelayExecutionThread(int mode, BOOLEAN alertable, LARGE_INTEGER *interval)
{ (void)mode; (void)alertable; (void)interval; g_now_100ns += 100000; return 0; }
void KeMemoryBarrier(void) {}

/* filter pass-through surface stubs (gu_flt.c) - all inert */
void IoSkipCurrentIrpStackLocation(PIRP irp) { (void)irp; }
void IoCopyCurrentIrpStackLocationToNext(PIRP irp) { (void)irp; }
void IoSetCompletionRoutine(PIRP irp, void *r, PVOID c, int a, int b, int d) { (void)irp; (void)r; (void)c; (void)a; (void)b; (void)d; }
NTSTATUS IoCallDriver(PDEVICE_OBJECT dev, PIRP irp) { (void)dev; (void)irp; return STATUS_UNSUCCESSFUL; }
void KeInitializeEvent(PKEVENT e, int t, int s) { (void)e; (void)t; (void)s; }
void KeSetEvent(PKEVENT e, int b, int w) { (void)e; (void)b; (void)w; }
int KeWaitForSingleObject(PKEVENT e, int m, int m2, int a, void *t) { (void)e; (void)m; (void)m2; (void)a; (void)t; return 0; }
PDEVICE_OBJECT IoAttachDeviceToDeviceStack(PDEVICE_OBJECT s, PDEVICE_OBJECT p) { (void)s; (void)p; return (PDEVICE_OBJECT)1; }
void IoDetachDevice(PDEVICE_OBJECT d) { (void)d; }
NTSTATUS IoGetDeviceProperty(PDEVICE_OBJECT dev, DEVICE_REGISTRY_PROPERTY p, ULONG l, void *b, ULONG *r) { (void)dev; (void)p; (void)l; (void)b; if (r) *r = 0; return STATUS_UNSUCCESSFUL; }
NTSTATUS ZwQueryValueKey(void *h, void *v, int c, void *i, ULONG l, ULONG *rl) { (void)h; (void)v; (void)c; (void)i; (void)l; if (rl) *rl = 0; return STATUS_UNSUCCESSFUL; }
NTSTATUS ZwFlushKey(void *h) { (void)h; return STATUS_UNSUCCESSFUL; }

/* Zw surface stubs: all fail, so GuGspOpen refuses (fail-closed smoke path) */
NTSTATUS ZwCreateFile(void **h, ULONG a, void *oa, void *iosb, void *al, ULONG at, ULONG sh, ULONG d, ULONG o, void *e, ULONG el)
{ (void)h;(void)a;(void)oa;(void)iosb;(void)al;(void)at;(void)sh;(void)d;(void)o;(void)e;(void)el; return STATUS_UNSUCCESSFUL; }
NTSTATUS ZwReadFile(void *h, void *ev, void *a1, void *a2, void *iosb, void *buf, ULONG len, LARGE_INTEGER *off, void *k)
{ (void)h;(void)ev;(void)a1;(void)a2;(void)iosb;(void)buf;(void)len;(void)off;(void)k; return STATUS_UNSUCCESSFUL; }
NTSTATUS ZwQueryInformationFile(void *h, void *iosb, void *info, ULONG len, ULONG cls)
{ (void)h;(void)iosb;(void)info;(void)len;(void)cls; return STATUS_UNSUCCESSFUL; }
void ZwClose(void *h) { (void)h; }

ULONG HalGetBusDataByOffset(BUS_DATA_TYPE t, ULONG bus, ULONG slot, void *buf, ULONG off, ULONG len)
{ (void)t; (void)bus; (void)slot; (void)off; memset(buf, 0xff, len); return len; }
ULONG HalSetBusDataByOffset(BUS_DATA_TYPE t, ULONG bus, ULONG slot, void *buf, ULONG off, ULONG len)
{ (void)t; (void)bus; (void)slot; (void)buf; (void)off; return len; }

IO_STACK_LOCATION g_io_loc;
PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp) { (void)irp; return &g_io_loc; }
void IoCompleteRequest(PIRP irp, char boost) { (void)irp; (void)boost; }

NTSTATUS IoCreateDevice(PDRIVER_OBJECT drv, ULONG ext, PUNICODE_STRING name, ULONG type, ULONG chars, BOOLEAN exclusive, PDEVICE_OBJECT *out)
{ (void)drv; (void)ext; (void)name; (void)type; (void)chars; (void)exclusive; *out = (PDEVICE_OBJECT)1; return STATUS_SUCCESS; }
void IoDeleteDevice(PDEVICE_OBJECT dev) { (void)dev; }
NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING l, PUNICODE_STRING t) { (void)l; (void)t; return STATUS_SUCCESS; }
void IoDeleteSymbolicLink(PUNICODE_STRING l) { (void)l; }
void ObReferenceObject(void *o) { (void)o; }
void RtlInitUnicodeString(PUNICODE_STRING s, PCWSTR b) { (void)s; (void)b; }

// ---- smoke checks -------------------------------------------------------------

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// synthesized minimal asset set: sizes/geometry the assets phase validates.
// The real pinned blobs come from upstream firmware/*.py at build time; the
// harness exercises the parsers with structurally valid stand-ins.
unsigned char GuFwBooter[72 + 60416 + 768];
unsigned char GuFwV67[64000];
unsigned char GuFwGsp[65536];
unsigned char GuFwBoot[24576];
unsigned char GuFwBootDesc[256];

static void put32(unsigned char *p, unsigned long v)
{ p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }

static void build_booter(void)
{
    put32(GuFwBooter + 0, 0x314c5442); // BOOTER_MAGIC 'BTL1'
    put32(GuFwBooter + 4, 1);          // version
    unsigned o = 8;                    // gpu_unlock_booter_hdr
    put32(GuFwBooter + o + 0, 0);      put32(GuFwBooter + o + 4, 256);    // os code
    put32(GuFwBooter + o + 8, 35328);  put32(GuFwBooter + o + 12, 25088); // os data
    put32(GuFwBooter + o + 16, 1);     // num_apps
    put32(GuFwBooter + o + 20, 256);   put32(GuFwBooter + o + 24, 35072); // app code
    put32(GuFwBooter + o + 28, 256);   put32(GuFwBooter + o + 32, 0);     // app data
    put32(GuFwBooter + o + 36, 35344); // patch_loc
    put32(GuFwBooter + o + 40, 3);     // ucode_id
    put32(GuFwBooter + o + 44, 1);     // engine_id
    put32(GuFwBooter + o + 48, 2);     // num_sigs
    put32(GuFwBooter + o + 52, 768);   // sig_bytes
    o += 56;
    put32(GuFwBooter + o, GU_BOOTER_IMAGE_SIZE);  // image_size
    put32(GuFwBooter + o + 4, GU_BOOTER_SIG_TOTAL); // sig_bytes tail
}

int main(void)
{
    // errno mapping sanity
    CHECK(GuErrToNt(0) == STATUS_SUCCESS);
    CHECK(GuErrToNt(-ENOMEM) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(GuErrToNt(-EALREADY) == STATUS_ALREADY_COMPLETE);

    // gfw_eval: the two pure assessors carried verbatim from upstream
    struct gpu_unlock_gfw_sample s = {0};
    CHECK(gpu_unlock_gfw_assess(&s) == -ENODEV);          // no config
    s.config_ok = TRUE;
    CHECK(gpu_unlock_gfw_assess(&s) == -EAGAIN);          // not halted/PL0/completed
    s.cpuctl = 0x10; s.plm = 1; s.boot = 0xff;
    CHECK(gpu_unlock_gfw_assess(&s) == 0);                // complete
    s.plm = ~0u;
    CHECK(gpu_unlock_gfw_assess(&s) == -ENODEV);          // all-ones guard
    CHECK(gpu_unlock_command_restore(0x6, 0x1) == 0x7);   // live non-mask bit 0x1 preserved, MEM|BME restored

    // radix plan + page: pure computation
    struct gpu_unlock_radix_plan plan;
    CHECK(gpu_unlock_radix_plan(72ull << 20, &plan) == 0);
    CHECK(plan.pages[3] > 0);

    // assets phase over synthesized blobs: exercises booter geometry checks
    build_booter();
    GU_RUN_OUT out; memset(&out, 0, sizeof(out));
    GU_WORK w; memset(&w, 0, sizeof(w));
    w.Out = &out; w.PciCapOff = -1; w.PmCapOff = -1;
    w.Bus = 1; w.Device = 0; w.Fn = 0;
    NTSTATUS st = GuTxAssets(&w);
    // elf section extraction will fail on the stand-in GSP (no .fwimage) — that
    // is the expected refusal; the check proves the parser fail-closed path.
    printf("assets on stand-in GSP: 0x%lx (expected refusal)\n", (unsigned long)st);
    CHECK(st == STATUS_DATA_ERROR || st == STATUS_UNSUCCESSFUL || st == STATUS_INVALID_DEVICE_STATE);

    printf(failures ? "HARNESS FAILURES: %d\n" : "harness: all checks passed\n", failures);
    return failures ? 1 : 0;
}
