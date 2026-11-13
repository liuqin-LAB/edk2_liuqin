/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_PCIE_HANDOFF_H
#define LIUQIN_PCIE_HANDOFF_H
#include "AcpiPcie.h"
typedef struct
{
  VOID *Context;
  BOOLEAN (*Read8) (VOID *, UINTN, UINT8 *);
  BOOLEAN (*Read16) (VOID *, UINTN, UINT16 *);
  BOOLEAN (*Write16) (VOID *, UINTN, UINT16);
} LIUQIN_PCIE_CONFIG_IO;
typedef struct
{
  UINT32 PcieCap[2], MsiControl[2], MsiXControl[2], MemoryAtu[7];
  UINT32 SmmuId[2], SidBefore, SidAfter, SidRoot, SidEndpoint;
  UINT32 Pll[4], SpeedBefore, SpeedAfter, RateBefore, RateAfter;
  UINT32 PllUser, PllState, PllStatus, PllVote, PllFsm;
} LIUQIN_PCIE_HANDOFF_STATE;
EFI_STATUS LiuqinPcieConfigureHandoff (CONST LIUQIN_PCIE_IO *, CONST LIUQIN_PCIE_CONFIG_IO *);
#endif
