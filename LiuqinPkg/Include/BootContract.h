/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_BOOT_CONTRACT_H
#define LIUQIN_BOOT_CONTRACT_H

/* No libc or UEFI dependency: shared by SEC, BootShim and native checks. */
typedef unsigned long long LIUQIN_U64;
typedef unsigned int LIUQIN_U32;
typedef unsigned long LIUQIN_SIZE;
#define LIUQIN_DTB_MAX 0x200000UL
#define LIUQIN_FD_BASE 0xAC000000ULL
#define LIUQIN_FD_SIZE 0x01000000ULL
#define LIUQIN_HEAP_BASE 0xAD000000ULL
#define LIUQIN_HEAP_SIZE 0x06000000ULL
#define LIUQIN_KERNEL_BASE 0xA8000000ULL
#define LIUQIN_KERNEL_SIZE 0x10000000ULL
/* Fixed liuqin AOP reservation, common to stock and board v5.10 DT. */
#define LIUQIN_CMD_DB_BASE 0x80860000ULL
#define LIUQIN_CMD_DB_SIZE 0x00020000ULL

/* Zero is success. Nonzero always means stop before writing the destination. */
int LiuqinValidateDtb (const void *dt, LIUQIN_SIZE available);
int LiuqinCheckIdentity (const void *dt);
int LiuqinCheckRange (const void *dt, LIUQIN_U64 base, LIUQIN_U64 size);
LIUQIN_U32 LiuqinDtbSize (const void *dt);
int LiuqinRangesOverlap (LIUQIN_U64 a, LIUQIN_U64 n, LIUQIN_U64 b, LIUQIN_U64 m);
#endif
