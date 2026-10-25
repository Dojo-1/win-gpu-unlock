// SPDX-License-Identifier: GPL-2.0-only
// Host-harness stand-in for the generated driver/gu_fw_blobs.h. Small arrays
// match the validated geometry; GSP access is span-based like the real header.
// Storage is defined in harness.c. Not the real pinned assets.
#ifndef GU_FW_BLOBS_H
#define GU_FW_BLOBS_H

extern unsigned char GuFwBooter[61256];
extern unsigned char GuFwV67[64000];
extern unsigned char GuFwBoot[24576];
extern unsigned char GuFwBootDesc[256];

#define GuFwBooterSize   ((unsigned long)61256)
#define GuFwV67Size      ((unsigned long)64000)
#define GuFwBootSize     ((unsigned long)24576)
#define GuFwBootDescSize ((unsigned long)256)

#define GuGspBinPath   L"C:\stand-in\GuRtGsp.bin"
#define GuGspFileSize  ((unsigned long long)84277400)
#define GuGspImageOffset ((unsigned long long)0x1000)
#define GuGspImageSize   ((unsigned long long)72ull<<20)
#define GuGspSigOffset   ((unsigned long long)0x200000)
#define GuGspSigSize     ((unsigned long long)4096)

#endif
