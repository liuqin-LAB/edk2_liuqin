/** @file

  Wrapper around gBS->ExitBootServices() that captures the winload caller's
  return address in x30 and passes it as the third argument, so the errata
  patcher can locate winload.efi and its OslArm64TransferToKernel call site.

  Ported from WOA-Project/mu_andromeda_platforms
  Platforms/AndromedaPkg/Library/KernelErrataPatcherLib (MIT).
  Copyright (c) 2022-2023 DuoWoA authors.
  SPDX-License-Identifier: MIT

**/

/* Keep this file free of C headers: EDK2 treats .S as text-through-cpp and
   then assembles it, so only assembler syntax may reach the assembler. */
    .text
    .extern KernelErrataPatcherExitBootServices
    .global ExitBootServicesWrapper
#ifdef __ELF__
    .type ExitBootServicesWrapper, %function
#endif

// Wrapper around ExitBootServices for hooking into winload.efi.
// EFI_STATUS EFIAPI ExitBootServicesWrapper (EFI_HANDLE, UINTN);
ExitBootServicesWrapper:
        // x30 holds the return address into winload.efi.
        mov     x2, x30

        // Tail-call the C handler with (ImageHandle, MapKey, caller LR).
        b       KernelErrataPatcherExitBootServices

#ifdef __ELF__
    .size ExitBootServicesWrapper, . - ExitBootServicesWrapper
#endif
