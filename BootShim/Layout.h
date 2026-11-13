/* SPDX-License-Identifier: MIT */
#ifndef LIUQIN_SHIM_LAYOUT_H
#define LIUQIN_SHIM_LAYOUT_H

/* [假设] Project allocation inside stock Kernel window; runtime DT must agree.
 * [取证:本地] docs/reference/liuqin-uefiplatLA.cfg: Kernel A8000000+10000000.
 * [取证:本地] MTE Reserved C0000000+20000000 excludes old CE000000 choice.
 */
#define SHIM_FD_BASE 0xAC000000ULL
#define SHIM_FD_SIZE 0x01000000ULL
#define SHIM_HEAP_BASE 0xAD000000ULL
#define SHIM_HEAP_SIZE 0x06000000ULL
#define SHIM_KERNEL_BASE 0xA8000000ULL
#define SHIM_KERNEL_END 0xB8000000ULL
#define SHIM_MAX_DTB 0x00200000ULL
#define SHIM_MAX_SIZE 0x00100000ULL

#endif
