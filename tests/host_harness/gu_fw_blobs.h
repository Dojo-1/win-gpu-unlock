// SPDX-License-Identifier: GPL-2.0-only
// Host-harness stand-in for the generated driver/gu_fw_blobs.h. Sizes match the
// geometry the assets phase validates (72+60416+768 booter, 0xfa00 v67).
// Storage is defined in harness.c. Not the real pinned assets.
#ifndef GU_FW_BLOBS_H
#define GU_FW_BLOBS_H

extern unsigned char GuFwBooter[61256];
extern unsigned char GuFwV67[64000];
extern unsigned char GuFwGsp[65536];
extern unsigned char GuFwBoot[24576];
extern unsigned char GuFwBootDesc[256];

#define GuFwBooterSize   ((unsigned long)61256)
#define GuFwV67Size      ((unsigned long)64000)
#define GuFwGspSize      ((unsigned long)65536)
#define GuFwBootSize     ((unsigned long)24576)
#define GuFwBootDescSize ((unsigned long)256)

#endif
