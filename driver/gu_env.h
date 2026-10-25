// SPDX-License-Identifier: GPL-2.0-only
// gpu-unlock-rt · environment shim for the verbatim-portable GPL modules
// (prep_610, vbios_610, layout_610, gfw_eval).
//
// Two build modes, mirroring upstream's `#ifdef __KERNEL__` discipline:
//  - GU_WINDOWS_KERNEL: compiled inside the WDM driver. The upstream include
//    blocks were redirected here; we provide fixed-width types, bool, Linux
//    errno constants (same numeric values — only compared/returned, converted
//    to NTSTATUS at the IOCTL boundary), and the WDK string functions.
//  - host mode (default): the shim degrades to plain <stdint.h>/<string.h>,
//    keeping the portable modules unit-testable exactly as upstream tests them.
// Logging (GU_NOTE/GU_ERROR) lives in portable/diagnostic.h in both modes.
#ifndef GU_ENV_H
#define GU_ENV_H

#ifdef GU_WINDOWS_KERNEL

#include <ntddk.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// Linux errno values (x86-64), used as plain negative ints inside the port.
#define EINVAL  22
#define ENOMEM  12
#define ENODEV  19
#define EIO      5
#define EAGAIN  11
#define ETIMEDOUT 110
#define ERANGE  34
#define EBUSY   16
#define EOPNOTSUPP 95
#define EBADMSG 74
#define EPROTO  71
#define EALREADY 114
#define ENOENT  2
#define EOVERFLOW 75

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#else // host mode: plain C, for tests/CI

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// The host build of the driver core (tests/host_harness) supplies its own
// errno mapping; on real hosts these errno names already exist via <errno.h>.

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#endif // GU_WINDOWS_KERNEL

#endif // GU_ENV_H
