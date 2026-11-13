/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_ACPI_PCIE_ECAM_H
#define LIUQIN_ACPI_PCIE_ECAM_H
#include "AcpiPcie.h"
typedef struct
{
  UINT32 Stage, ReadMask, Values[9], Decode[4];
  UINT32 FailedAddress, FailedValue, FailedExpected;
  UINT32 BlockerDomain, CpuProbeValue, RelativeProbeValue;
  UINT32 MoveStage, MoveReadMask, MoveValues[7];
  UINT32 CfgLimit, ScanReads[2], ScanPresent[2], ScanAbsent[2], ScanCompleteMask;
  UINT32 DmaStage, DmaDecode[2], DmaValues[4], DmaReadMask, DmaAliasMask, DmaRestoreMask;
  BOOLEAN CfgLimitRead;
  BOOLEAN DmaRestored;
  BOOLEAN MoveRestored;
  BOOLEAN Restored;
  BOOLEAN Retained;
} LIUQIN_PCIE_ECAM_RESULT;
EFI_STATUS LiuqinPcieConfigureEcam (CONST LIUQIN_PCIE_IO *, LIUQIN_PCIE_ECAM_RESULT *,
                                    LIUQIN_PCIE_SCOPED_WORK, VOID *);
#endif
