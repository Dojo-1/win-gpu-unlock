// SPDX-License-Identifier: GPL-2.0-only
// Host-build mock of the WDK surface used by gpu-unlock-rt. Lets CI compile the
// entire kernel-mode branch (GU_WINDOWS_KERNEL) of the port — gu_rt.c and
// gu_transaction.c included — on any runner with a C compiler, without the WDK.
// Mocks are non-functional stubs: this harness verifies translation units
// (types, prototypes, syntax), not hardware behavior.
#ifndef GU_MOCK_NTDDK_H
#define GU_MOCK_NTDDK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>

typedef int NTSTATUS;
typedef unsigned int ULONG;  /* 4 bytes even on LP64 hosts */
typedef long LONG;
typedef unsigned char UCHAR, *PUCHAR;
typedef unsigned short USHORT;
typedef void *PVOID, *HANDLE;
typedef const wchar_t *PCWSTR;
typedef int BOOLEAN;
typedef UCHAR BYTE;
typedef uint64_t ULONG64, ULONGLONG;
typedef long long LONGLONG, *PLONGLONG;
typedef size_t SIZE_T;
typedef uintptr_t ULONG_PTR;

#define TRUE 1
#define FALSE 0
#define NULL64 ((void *)0)

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef union _LARGE_INTEGER { struct { ULONG LowPart; LONG HighPart; }; long long QuadPart; } LARGE_INTEGER;
typedef union _PHYSICAL_ADDRESS { struct { ULONG LowPart; LONG HighPart; }; unsigned long long QuadPart; } PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

typedef struct _UNICODE_STRING { USHORT Length; USHORT MaximumLength; wchar_t *Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;
struct _IRP;
typedef struct _IRP IRP, *PIRP;

typedef struct _IO_STATUS_BLOCK { NTSTATUS Status; ULONG_PTR Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
typedef struct _IO_STACK_LOCATION IO_STACK_LOCATION, *PIO_STACK_LOCATION;

// ---- NTSTATUS constants used by the port ----
#define STATUS_SUCCESS               ((NTSTATUS)0x00000000)
#define STATUS_UNSUCCESSFUL          ((NTSTATUS)0xC0000001)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_DEVICE_NOT_CONNECTED  ((NTSTATUS)0xC000009D)
#define STATUS_DEVICE_POWERED_OFF    ((NTSTATUS)0xC000009F)
#define STATUS_DEVICE_BUSY           ((NTSTATUS)0x00000102)
#define STATUS_BUFFER_TOO_SMALL      ((NTSTATUS)0xC0000023)
#define STATUS_ACCESS_DENIED         ((NTSTATUS)0xC0000022)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010)
#define STATUS_INVALID_DEVICE_STATE  ((NTSTATUS)0xC000009B)
#define STATUS_IO_DEVICE_ERROR       ((NTSTATUS)0xC0000185)
#define STATUS_TIMEOUT               ((NTSTATUS)0x00000102)
#define STATUS_ALREADY_COMPLETE      ((NTSTATUS)0x00000184)
#define STATUS_NOT_SUPPORTED         ((NTSTATUS)0xC00000BB)
#define STATUS_DATA_ERROR            ((NTSTATUS)0xC000003E)
#define STATUS_INVALID_VIEW_SIZE     ((NTSTATUS)0xC000001F)
#define STATUS_PENDING               ((NTSTATUS)0x00000103)
#define STATUS_NOT_FOUND             ((NTSTATUS)0xC0000225)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182)

#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

// ---- debug print ----
#define DPFLTR_DEFAULT_ID 0
#define DPFLTR_INFO_LEVEL 3
#define DPFLTR_ERROR_LEVEL 0
int DbgPrintEx(int comp, int level, const char *fmt, ...);

// ---- pool ----
#define POOL_FLAG_NON_PAGED 0x0000000000000040ULL
void *ExAllocatePool2(unsigned long long flags, SIZE_T size, unsigned long tag);
void ExFreePoolWithTag(void *p, unsigned long tag);

// ---- MM ----
void *MmMapIoSpace(PHYSICAL_ADDRESS pa, SIZE_T n, int cache);
void MmUnmapIoSpace(void *va, SIZE_T n);
PHYSICAL_ADDRESS MmGetPhysicalAddress(void *va);
#define MmNonCached 0
#define MmCached 2
void *MmAllocateContiguousMemorySpecifyCache(SIZE_T n, PHYSICAL_ADDRESS lo, PHYSICAL_ADDRESS hi, PHYSICAL_ADDRESS boundary, int cache);
void MmFreeContiguousMemorySpecifyCache(void *va, SIZE_T n, int cache);

// ---- time / sync ----
unsigned long long KeQueryInterruptTime(void);
int KeDelayExecutionThread(int mode, BOOLEAN alertable, LARGE_INTEGER *interval);
#define KernelMode 0
void KeMemoryBarrier(void);

// ---- HAL PCI bus data ----
typedef enum _BUS_DATA_TYPE { PCIConfiguration = 4 } BUS_DATA_TYPE, *PBUS_DATA_TYPE;
typedef union _PCI_SLOT_NUMBER {
    union {
        ULONG AsULONG;
        struct { ULONG bits0_4:5, DeviceNumber:5, FunctionNumber:3, bits13_31:16; } bits;
    } u;
} PCI_SLOT_NUMBER, *PPCI_SLOT_NUMBER;
ULONG HalGetBusDataByOffset(BUS_DATA_TYPE t, ULONG bus, ULONG slot, void *buf, ULONG off, ULONG len);
ULONG HalSetBusDataByOffset(BUS_DATA_TYPE t, ULONG bus, ULONG slot, void *buf, ULONG off, ULONG len);

// ---- object / file (gu_gsp.c surface) ----
struct _UNICODE_STRING;
typedef struct _OBJECT_ATTRIBUTES { ULONG Length; void *RootDirectory; struct _UNICODE_STRING *ObjectName; ULONG Attributes; void *SecurityDescriptor; void *SecurityQualityOfService; } OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef struct _FILE_STANDARD_INFORMATION { LARGE_INTEGER AllocationSize; LARGE_INTEGER EndOfFile; ULONG NumberOfLinks; BOOLEAN DeletePending; BOOLEAN Directory; } FILE_STANDARD_INFORMATION;
#define OBJ_CASE_INSENSITIVE 0x40
#define OBJ_KERNEL_HANDLE    0x200
#define FILE_READ_DATA       0x0001
#define SYNCHRONIZE          0x00100000
#define FILE_SHARE_READ      0x1
#define FILE_OPEN            0x1
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE      0x40
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FileStandardInformation 5
#define InitializeObjectAttributes(p, n, a, r, s) do { (p)->Length = sizeof(OBJECT_ATTRIBUTES); (p)->RootDirectory = (r); (p)->ObjectName = (n); (p)->Attributes = (a); (p)->SecurityDescriptor = (s); (p)->SecurityQualityOfService = 0; } while (0)
NTSTATUS ZwCreateFile(void **handle, ULONG access, void *oa, void *iosb, void *alloc, ULONG attrs, ULONG share, ULONG disp, ULONG opts, void *eabuf, ULONG ealen);
NTSTATUS ZwReadFile(void *handle, void *event, void *apcrc, void *apctx, void *iosb, void *buf, ULONG len, LARGE_INTEGER *off, void *key);
NTSTATUS ZwQueryInformationFile(void *handle, void *iosb, void *info, ULONG len, ULONG cls);
void ZwClose(void *handle);

// ---- IRP machinery ----
#define IRP_MJ_CREATE 0
#define IRP_MJ_CLOSE  2
#define IRP_MJ_DEVICE_CONTROL 14
#define IO_NO_INCREMENT 0
struct _IO_STACK_LOCATION {
    union {
        struct { PVOID Type3InputBuffer; ULONG IoControlCode; ULONG InputBufferLength; ULONG OutputBufferLength; } DeviceIoControl;
    } Parameters;
};
PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP irp);
struct _IRP { IO_STATUS_BLOCK IoStatus; union { PVOID SystemBuffer; } AssociatedIrp; };
void IoCompleteRequest(PIRP irp, char prioboost);

// ---- device / driver ----
#define FILE_DEVICE_UNKNOWN 0x00000022
#define FILE_DEVICE_SECURE_OPEN 0x00000100
NTSTATUS IoCreateDevice(PDRIVER_OBJECT drv, ULONG ext, PUNICODE_STRING name, ULONG type, ULONG chars, BOOLEAN exclusive, PDEVICE_OBJECT *out);
void IoDeleteDevice(PDEVICE_OBJECT dev);
NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING link, PUNICODE_STRING target);
void IoDeleteSymbolicLink(PUNICODE_STRING link);
void ObReferenceObject(void *obj);
void RtlInitUnicodeString(PUNICODE_STRING s, PCWSTR buf);
typedef NTSTATUS (*DRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef DRIVER_DISPATCH PDRIVER_DISPATCH;
typedef void (*DRIVER_UNLOAD)(PDRIVER_OBJECT);
typedef DRIVER_UNLOAD PDRIVER_UNLOAD;

struct _DRIVER_OBJECT {
    PDRIVER_DISPATCH MajorFunction[28];
    PDRIVER_UNLOAD DriverUnload;
};

// ---- CTL_CODE ----
#define CTL_CODE(d, f, m, a) ((d << 16) | (a << 14) | (f << 2) | m)
#define METHOD_BUFFERED 0
#define FILE_ANY_ACCESS 0

// ---- register access ----
#define READ_REGISTER_ULONG(addr)  (*(volatile ULONG *)(addr))
#define WRITE_REGISTER_ULONG(addr, val) (*(volatile ULONG *)(addr) = (val))

// ---- Rtl as macros over CRT ----
#define RtlZeroMemory(dst, len) memset((dst), 0, (len))
#define RtlCopyMemory(dst, src, len) memcpy((dst), (src), (len))
#define UNREFERENCED_PARAMETER(x) ((void)(x))

#endif // GU_MOCK_NTDDK_H
