/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_ACPI_PCIE_H
#define LIUQIN_ACPI_PCIE_H
#include <Uefi.h>
typedef struct
{
  VOID *Context;
  BOOLEAN (*Read32) (VOID *Context, UINTN Address, UINT32 *Value);
  BOOLEAN (*Write32) (VOID *Context, UINTN Address, UINT32 Value);
  VOID (*DelayUs) (VOID *Context, UINTN Microseconds);

  BOOLEAN *RetainForOs;
} LIUQIN_PCIE_IO;
typedef EFI_STATUS (*LIUQIN_PCIE_SCOPED_WORK) (CONST LIUQIN_PCIE_IO *, VOID *);
EFI_STATUS LiuqinPcieInitializeWithIo (CONST LIUQIN_PCIE_IO *Io);
EFI_STATUS InitializeLiuqinPcie (VOID);
BOOLEAN LiuqinPcieHandoffReady (VOID);
#endif
