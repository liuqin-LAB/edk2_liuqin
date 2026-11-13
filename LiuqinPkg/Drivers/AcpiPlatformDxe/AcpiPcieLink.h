/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_ACPI_PCIE_LINK_H
#define LIUQIN_ACPI_PCIE_LINK_H
#include "AcpiPcie.h"
typedef struct
{
  UINT32 Stage, Polls, Elbi, Link, Ltssm, Phy, EndpointId;
  UINT32 EndpointCommand, EndpointClass, EndpointHeader, EndpointReads;
  UINT32 EnableReadback, DisableReadback, EnablePolls, DisablePolls;
  UINT32 FailedAddress, FailedValue, FailedMask, FailedBits, FailedReason;
  BOOLEAN Quiesced, Retained;
} LIUQIN_PCIE_LINK_RESULT;

EFI_STATUS LiuqinPcieTrainLink (CONST LIUQIN_PCIE_IO *, LIUQIN_PCIE_LINK_RESULT *,
                                LIUQIN_PCIE_SCOPED_WORK, VOID *);
#endif
