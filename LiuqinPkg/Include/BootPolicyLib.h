/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_BOOT_POLICY_H
#define LIUQIN_BOOT_POLICY_H

#include <Base.h>
#define LIUQIN_BOOT_WINDOWS_PATH L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"
#define LIUQIN_BOOT_REMOVABLE_PATH L"\\EFI\\BOOT\\BOOTAA64.EFI"

typedef enum
{
  LiuqinBootClassWindows = 0, /* \EFI\Microsoft\Boot\bootmgfw.efi */
  LiuqinBootClassRemovable,   /* \EFI\BOOT\BOOTAA64.EFI */
  LiuqinBootClassMedia,       /* enumerated media without a named file */
  LiuqinBootClassShell,       /* embedded diagnostic Shell, last resort */
  LiuqinBootClassNone         /* not a boot target */
} LIUQIN_BOOT_CLASS;

/* Classify one EFI file path. '\\' and '/' are equivalent, case is ignored. */
LIUQIN_BOOT_CLASS
LiuqinBootClassifyPath (CONST CHAR16 *Path);

/* Priority of a class: smaller boots earlier. */
UINTN
LiuqinBootRank (LIUQIN_BOOT_CLASS Class);

/* Total order over boot targets: class rank first, then the raw path bytes so
 * that equal ranks stay deterministic (PerformQuickSort is not stable). */
INTN LiuqinBootCompare (LIUQIN_BOOT_CLASS ClassA, CONST UINT8 *PathA, UINTN SizeA,
                        LIUQIN_BOOT_CLASS ClassB, CONST UINT8 *PathB, UINTN SizeB);

#endif
