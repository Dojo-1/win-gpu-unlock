// SPDX-License-Identifier: GPL-2.0-only
// gpu-unlock-rt · GSP image access. The 84 MB gsp_ga10x.bin is never mapped
// whole: the driver opens it once and reads only the local spans a phase
// needs (signature: 4 KiB; radix pages: 4 KiB each) straight into their
// destination buffers. Spans and size come hash-bound from gu_fw_blobs.h.
#include "gu_rt.h"
#include "gu_transaction.h"
#include "gu_fw_blobs.h"

int GuGspOpen(GU_WORK *w)            // returns 0 / Linux errno
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION std;
    NTSTATUS st;

    if (w->GspHandle)
        return 0; // idempotent within one transaction
    RtlInitUnicodeString(&path, GuGspBinPath);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    st = ZwCreateFile(&w->GspHandle, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb,
                      NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) {
        GU_ERROR("gsp open failed %#x path=%S\n", st, GuGspBinPath);
        w->GspHandle = NULL;
        return -ENOENT;
    }
    st = ZwQueryInformationFile(w->GspHandle, &iosb, &std, sizeof(std),
                                FileStandardInformation);
    if (!NT_SUCCESS(st) ||
        (unsigned long long)std.EndOfFile.QuadPart != GuGspFileSize) {
        GU_ERROR("gsp size mismatch: %#llx expected %#llx\n",
                 NT_SUCCESS(st) ? (unsigned long long)std.EndOfFile.QuadPart : 0ull,
                 (unsigned long long)GuGspFileSize);
        ZwClose(w->GspHandle);
        w->GspHandle = NULL;
        return -EBADMSG; // wrong file: refuse, never guess
    }
    return 0;
}

int GuGspRead(GU_WORK *w, unsigned long long fileOffset, void *buf, SIZE_T len)
{
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;

    if (!w->GspHandle)
        return -ENOENT;
    off.QuadPart = (LONGLONG)fileOffset;
    NTSTATUS st = ZwReadFile(w->GspHandle, NULL, NULL, NULL, &iosb, buf,
                             (ULONG)len, &off, NULL);
    if (!NT_SUCCESS(st) || iosb.Information != len)
        return -EIO;
    return 0;
}

void GuGspClose(GU_WORK *w)
{
    if (w->GspHandle) {
        ZwClose(w->GspHandle);
        w->GspHandle = NULL;
    }
}
