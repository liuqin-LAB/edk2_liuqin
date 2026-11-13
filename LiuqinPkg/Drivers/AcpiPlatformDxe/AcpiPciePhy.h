/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_ACPI_PCIE_PHY_H
#define LIUQIN_ACPI_PCIE_PHY_H
#include "AcpiPcie.h"
typedef struct
{
  UINT32 Stage, Writes, Status, Power, Reset, Start, Pipe, Clock;
  UINT32 PinBefore[3], PinDuring[3];
  BOOLEAN Quiesced, Retained;
} LIUQIN_PCIE_PHY_RESULT;

EFI_STATUS LiuqinPcieInitializePhy (CONST LIUQIN_PCIE_IO *, LIUQIN_PCIE_PHY_RESULT *,
                                    LIUQIN_PCIE_SCOPED_WORK, VOID *);
#endif
