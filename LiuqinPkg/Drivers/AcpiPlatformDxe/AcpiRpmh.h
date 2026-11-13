/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_ACPI_RPMH_H
#define LIUQIN_ACPI_RPMH_H
#include "AcpiPcie.h"
#include "AcpiCmdDb.h"

#define LIUQIN_PCIE_POWER_COMMANDS 9U
#define LIUQIN_WLAN_POWER_COMMANDS 12U
#define LIUQIN_RPMH_MAX_COMMANDS 16U
typedef struct
{
  UINT32 Address, Data;
} LIUQIN_RPMH_COMMAND;
typedef struct
{
  UINT32 Stage, IrqStatus, TcsStatus, CompletedMask, CommandReadMask, GicEnable;
  UINT32 IrqBeforeClear, ClearPolls;
  UINT32 CommandStatus[LIUQIN_RPMH_MAX_COMMANDS];
  UINT32 Response[LIUQIN_RPMH_MAX_COMMANDS];
  BOOLEAN VoteMayHaveApplied, ConfigurationRestored;
} LIUQIN_RPMH_RESULT;

EFI_STATUS LiuqinPciePowerPlan (CONST LIUQIN_CMD_DB_REPORT *Db, LIUQIN_RPMH_COMMAND *Commands);

EFI_STATUS LiuqinPciePowerOn (CONST LIUQIN_PCIE_IO *Io, CONST LIUQIN_RPMH_COMMAND *Commands,
                              LIUQIN_RPMH_RESULT *Result);
/* Fixed stock CNSS voltage+enable requests only. Existing mode is retained.
 * The same bounded TCS transport restores configuration, not prior AP votes. */
EFI_STATUS LiuqinWlanPowerPlan (CONST LIUQIN_WLAN_CMD_DB_REPORT *, LIUQIN_RPMH_COMMAND *);
EFI_STATUS LiuqinWlanPowerOn (CONST LIUQIN_PCIE_IO *, CONST LIUQIN_RPMH_COMMAND *,
                              LIUQIN_RPMH_RESULT *);
#endif
