/* Based on https://github.com/SamuelTulach/rainbow
 * Copyright (c) 2021 Samuel Tulach
 * Copyright (c) 2022-2023 DuoWoA authors
 * SPDX-License-Identifier: MIT */
#ifndef _KERNEL_ERRATA_PATCHER_H_
#define _KERNEL_ERRATA_PATCHER_H_

#include <PiDxe.h>

#include <Library/UefiLib.h>
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PerformanceLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/MemoryAttribute.h>

/* This is a targeted Qualcomm firmware workaround, not EL1-to-EL2
   instruction translation. ABL entry at EL1 alone does not prove a trap.
   ACTLR/AMU suppression and the SGI IRM workaround follow the SM8450
   reference policy; their necessity on this SM8475 firmware needs device
   validation. PSCI_MEMPROTECT is deliberately not patched. */

typedef struct
{
  UINTN Call;
  UINTN Target;
  UINTN Cave;
} LIUQIN_KEP_INJECTION;

BOOLEAN LiuqinKepBranch (UINTN From, UINTN To, BOOLEAN Link, UINT32 *Instruction);
BOOLEAN LiuqinKepPlanInjection (EFI_PHYSICAL_ADDRESS Base, UINTN Size, UINTN PayloadSize,
                                LIUQIN_KEP_INJECTION *Plan);

EFI_STATUS
EFIAPI
KernelErrataPatcherExitBootServices (IN EFI_HANDLE ImageHandle, IN UINTN MapKey,
                                     IN EFI_PHYSICAL_ADDRESS fwpKernelSetupPhase1);

EFI_STATUS
EFIAPI
ExitBootServicesWrapper (IN EFI_HANDLE ImageHandle, IN UINTN MapKey);

EFI_PHYSICAL_ADDRESS LocateWinloadBase (EFI_PHYSICAL_ADDRESS base, UINTN *size);

EFI_STATUS
EFIAPI
UnprotectWinload (EFI_PHYSICAL_ADDRESS WinloadBase, UINTN WinloadLength);

EFI_STATUS
EFIAPI
ReProtectWinload (EFI_PHYSICAL_ADDRESS WinloadBase, UINTN WinloadLength);

EFI_STATUS
EFIAPI
InitMemoryAttributeProtocol ();

VOID InstallEFIBSIntercepter ();

VOID OslInterceptMain (IN EFI_PHYSICAL_ADDRESS fwpKernelSetupPhase1);

/* Liuqin additions: rewrite every QHEE-trapped system-register access in the
   executable sections of the loaded winload.efi image (not only the single
   ACTLR_EL1 read the upstream patcher looks for), then flush the instruction
   cache so the rewritten instructions are actually fetched. */
VOID LiuqinKepPatchWinloadTrapRegisters (IN EFI_PHYSICAL_ADDRESS WinloadBase, IN UINTN WinloadSize);

VOID LiuqinKepFlushInstructionCache (IN EFI_PHYSICAL_ADDRESS Base, IN UINTN Size);

#endif /* _KERNEL_ERRATA_PATCHER_H_ */