/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AcpiRpmh.h"
#define DRV 0x17A20000U
#define TCS (DRV + 0xD00U)
#define STRIDE 0x2A0U
#define TRIGGER 0x01000000U
#define AMC 0x00010000U
#define MSG 0x00010108U
#define DONE 0x00010100U
#define POLL_US 20000U
#define GIC_ENABLE_32_63 0x17100104U // Cape DRV2 IRQ is SPI5 = GSIV37

STATIC UINT32 Level (CONST LIUQIN_CMD_DB_RESOURCE *R, UINTN Index)
{
  return (UINT32)R->Aux[Index * 2] | (UINT32)R->Aux[Index * 2 + 1] << 8;
}
STATIC BOOLEAN MaxLevel (CONST LIUQIN_CMD_DB_RESOURCE *R, UINT32 *Index)
{
  UINTN Count;
  if (R->AuxLength < 4 || R->AuxLength > 32 || (R->AuxLength & 1U) || Level (R, 0) != 0)
    return FALSE;
  Count = R->AuxLength / 2U;
  while (Count > 1 && Level (R, Count - 1) == 0)
    --Count;
  for (UINTN I = 1; I < Count; ++I)
    if (Level (R, I) <= Level (R, I - 1) || Level (R, I) > 0x200U)
      return FALSE;
  if (Level (R, Count - 1) < 0x100U)
    return FALSE; // board minimum NOM
  *Index = (UINT32)(Count - 1);
  return TRUE;
}
EFI_STATUS LiuqinPciePowerPlan (CONST LIUQIN_CMD_DB_REPORT *Db, LIUQIN_RPMH_COMMAND *Commands)
{
  LIUQIN_RPMH_COMMAND Plan[LIUQIN_PCIE_POWER_COMMANDS];
  UINT32 Cx, Mx;
  if (Db == NULL || Commands == NULL)
    return EFI_INVALID_PARAMETER;
  for (UINTN I = 0; I < LIUQIN_CMD_DB_RESOURCE_COUNT; ++I)
  {
    CONST LIUQIN_CMD_DB_RESOURCE *R = &Db->Resources[I];
    UINT32 Slave = I < 2 ? 4U : 3U;
    if (R->Slave != Slave || ((R->Address >> 16) & 15U) != Slave || (R->Address & 15U) != 0 ||
        R->Address > 0xFFFF0U)
      return EFI_COMPROMISED_DATA;
    for (UINTN J = 0; J < I; ++J)
      if (R->Address == Db->Resources[J].Address)
        return EFI_COMPROMISED_DATA;
  }
  if (!MaxLevel (&Db->Resources[2], &Cx) || !MaxLevel (&Db->Resources[3], &Mx))
    return EFI_COMPROMISED_DATA;
  Plan[0] = (LIUQIN_RPMH_COMMAND){Db->Resources[4].Address, 3};
  Plan[1] = (LIUQIN_RPMH_COMMAND){Db->Resources[2].Address, Cx};
  Plan[2] = (LIUQIN_RPMH_COMMAND){Db->Resources[3].Address, Mx};
  for (UINTN I = 0; I < 2; ++I)
  {
    UINTN N = 3 + I * 3;
    Plan[N] = (LIUQIN_RPMH_COMMAND){Db->Resources[I].Address, I == 0 ? 880U : 1200U};
    Plan[N + 1] = (LIUQIN_RPMH_COMMAND){Db->Resources[I].Address + 8U, 7};
    Plan[N + 2] = (LIUQIN_RPMH_COMMAND){Db->Resources[I].Address + 4U, 1};
  }
  for (UINTN I = 0; I < LIUQIN_PCIE_POWER_COMMANDS; ++I)
    Commands[I] = Plan[I];
  return EFI_SUCCESS;
}

STATIC BOOLEAN Rd (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 *V)
{
  return Io->Read32 (Io->Context, A, V) && *V != MAX_UINT32;
}
STATIC BOOLEAN Wr (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 V)
{
  UINT32 Actual;
  if (!Io->Write32 (Io->Context, A, V))
    return FALSE;
  // CONTROL crosses a clock domain; the board driver also synchronizes it.
  for (UINTN I = 0; I < 1000; ++I)
  {
    if (!Rd (Io, A, &Actual))
      return FALSE;
    if (Actual == V)
      return TRUE;
    if (A != TCS + 0x14)
      return FALSE;
    Io->DelayUs (Io->Context, 1);
  }
  return FALSE;
}
STATIC BOOLEAN Quiescent (CONST LIUQIN_PCIE_IO *Io)
{
  UINT32 V;
  for (UINTN I = 0; I < 3; ++I)
  {
    UINTN A = TCS + I * STRIDE;
    if (!Rd (Io, A + 0x18, &V) || V != 1 || !Rd (Io, A + 0x14, &V) || V != 0 ||
        !Rd (Io, A + 0x10, &V) || V != 0 || !Rd (Io, A + 0x1C, &V) || V != 0)
      return FALSE;
  }
  return Rd (Io, TCS + 4, &V) && (V & 1U) == 0;
}
STATIC BOOLEAN Idle (CONST LIUQIN_PCIE_IO *Io, LIUQIN_RPMH_RESULT *R)
{
  for (UINTN I = 0; I < POLL_US; ++I)
  {
    if (!Rd (Io, TCS + 0x18, &R->TcsStatus))
      return FALSE;
    if (R->TcsStatus == 1)
      return TRUE;
    Io->DelayUs (Io->Context, 1);
  }
  return FALSE;
}
STATIC BOOLEAN Results (CONST LIUQIN_PCIE_IO *Io, LIUQIN_RPMH_RESULT *R, UINTN Count)
{
  for (UINTN I = 0; I < Count; ++I)
  {
    UINTN A = TCS + 0x30 + I * 0x14;
    if (!Rd (Io, A + 0xC, &R->CommandStatus[I]) || !Rd (Io, A + 0x10, &R->Response[I]))
      return FALSE;
    R->CommandReadMask |= 1U << I;
    if ((R->CommandStatus[I] & DONE) == DONE)
      R->CompletedMask |= 1U << I;
  }
  return TRUE;
}
STATIC BOOLEAN ClearCompletion (CONST LIUQIN_PCIE_IO *Io, LIUQIN_RPMH_RESULT *R)
{
  if (!Rd (Io, TCS + 4, &R->IrqStatus))
    return FALSE;
  R->IrqBeforeClear = R->IrqStatus;
  if ((R->IrqStatus & 1U) == 0)
    return TRUE;

  if (!R->VoteMayHaveApplied || !Io->Write32 (Io->Context, TCS + 8, 1))
    return FALSE;
  // Board Linux also clears the trigger again after IRQ_CLEAR.
  if (!Wr (Io, TCS + 0x14, 0))
    return FALSE;
  for (UINTN I = 0; I < 1000; ++I)
  {
    ++R->ClearPolls;
    if (!Rd (Io, TCS + 4, &R->IrqStatus))
      return FALSE;
    if ((R->IrqStatus & 1U) == 0)
      return TRUE;
    Io->DelayUs (Io->Context, 1);
  }
  return FALSE;
}

STATIC EFI_STATUS Send (CONST LIUQIN_PCIE_IO *Io, CONST LIUQIN_RPMH_COMMAND *Commands, UINTN Count,
                        LIUQIN_RPMH_RESULT *Result)
{
  LIUQIN_RPMH_RESULT R = {0};
  UINT32 Config, Solver, IrqEnable, Saved[LIUQIN_RPMH_MAX_COMMANDS][3];
  UINT32 Mask;
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  BOOLEAN Clean = TRUE, Dirty = FALSE;
  if (!Io || !Io->Read32 || !Io->Write32 || !Io->DelayUs || !Commands || !Result || !Count ||
      Count > LIUQIN_RPMH_MAX_COMMANDS)
    return EFI_INVALID_PARAMETER;
  Mask = (1U << Count) - 1U;
  R.Stage = 1;
  if (!Rd (Io, DRV + 0xC, &Config) || !Rd (Io, DRV + 4, &Solver))
    goto End;
  if (((Config >> 12) & 63U) != 8 || ((Config >> 27) & 31U) != 16 || (Solver & TRIGGER))
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  R.Stage = 2;
  if (!Rd (Io, GIC_ENABLE_32_63, &R.GicEnable))
    goto End;
  if (R.GicEnable & (1U << 5))
  {
    Status = EFI_ACCESS_DENIED;
    goto End;
  }
  if (!Quiescent (Io))
  {
    Status = EFI_NOT_READY;
    goto End;
  }
  if (!Rd (Io, TCS, &IrqEnable))
    goto End;
  for (UINTN I = 0; I < Count; ++I)
    for (UINTN J = 0; J < 3; ++J)
      if (!Rd (Io, TCS + 0x30 + I * 0x14 + J * 4, &Saved[I][J]))
        goto End;
  if (!Quiescent (Io))
  {
    Status = EFI_NOT_READY;
    goto End;
  }
  R.Stage = 3;
  Dirty = TRUE; // a failing callback may have posted the write

  if (!Wr (Io, TCS, IrqEnable | 1U))
    goto Restore;
  for (UINTN I = 0; I < Count; ++I)
  {
    UINTN A = TCS + 0x30 + I * 0x14;
    if (!Wr (Io, A, MSG) || !Wr (Io, A + 4, Commands[I].Address) ||
        !Wr (Io, A + 8, Commands[I].Data))
      goto Restore;
  }
  if (!Wr (Io, TCS + 0x10, Mask) || !Wr (Io, TCS + 0x1C, Mask) || !Wr (Io, TCS + 0x14, 0) ||
      !Wr (Io, TCS + 0x14, AMC))
    goto Restore;
  if (!Rd (Io, TCS + 4, &R.IrqStatus) || (R.IrqStatus & 1U))
    goto Restore;
  if (!Rd (Io, GIC_ENABLE_32_63, &R.GicEnable) || (R.GicEnable & (1U << 5)))
    goto Restore;
  R.Stage = 4;
  R.VoteMayHaveApplied = TRUE;
  // Trigger can self-clear; completion, not equality, validates this write.
  if (!Io->Write32 (Io->Context, TCS + 0x14, AMC | TRIGGER))
    goto Restore;
  Status = EFI_TIMEOUT;
  for (UINTN I = 0; I < POLL_US; ++I)
  {
    if (!Rd (Io, TCS + 4, &R.IrqStatus))
    {
      Status = EFI_DEVICE_ERROR;
      break;
    }
    if (R.IrqStatus & 1U)
    {
      Status = EFI_SUCCESS;
      break;
    }
    Io->DelayUs (Io->Context, 1);
  }
  if (EFI_ERROR (Status))
    goto Restore;
  R.Stage = 5;
  Status = EFI_DEVICE_ERROR;
  if (!Idle (Io, &R))
    goto Restore;
  if (!Results (Io, &R, Count))
    goto Restore;
  if (R.CompletedMask != Mask)
    goto Restore;
  R.Stage = 6;
  Status = EFI_SUCCESS;
Restore:
  // Never clear an active request or overwrite payload with unknown liveness.
  if (R.VoteMayHaveApplied && !Idle (Io, &R))
  {
    Status = EFI_ABORTED;
    goto End;
  }
  // Even a timeout must expose actual per-command data before cleanup.
  // Missing fresh IRQ still fails; old status bits cannot establish success.
  if (R.VoteMayHaveApplied && R.CommandReadMask == 0 && !Results (Io, &R, Count))
    Status = EFI_DEVICE_ERROR;
  if (!Wr (Io, TCS + 0x14, AMC) || !Wr (Io, TCS + 0x14, 0))
  {
    // Without confirmed trigger/mode disable, leave payload intact for reset.
    Status = EFI_ABORTED;
    goto End;
  }
  if (!Wr (Io, TCS + 0x1C, 0))
    Clean = FALSE;
  if (!Wr (Io, TCS + 0x10, 0))
    Clean = FALSE;
  for (UINTN I = 0; I < Count; ++I)
    for (UINTN J = 0; J < 3; ++J)
      if (!Wr (Io, TCS + 0x30 + I * 0x14 + J * 4, Saved[I][J]))
        Clean = FALSE;
  if (!ClearCompletion (Io, &R))
    Clean = FALSE;
  if (!Rd (Io, TCS, &Config) || !Wr (Io, TCS, (Config & ~1U) | (IrqEnable & 1U)))
    Clean = FALSE;
  if (!Quiescent (Io))
    Clean = FALSE;
  R.ConfigurationRestored = Clean;
  if (!Clean)
    Status = EFI_ABORTED;
End:
  if (!Dirty)
    R.ConfigurationRestored = TRUE;
  *Result = R;
  return Status;
}

EFI_STATUS LiuqinPciePowerOn (CONST LIUQIN_PCIE_IO *Io, CONST LIUQIN_RPMH_COMMAND *Commands,
                              LIUQIN_RPMH_RESULT *Result)
{
  if (!Commands)
    return EFI_INVALID_PARAMETER;
  // This is a board power transport, not an arbitrary RPMh write primitive.
  for (UINTN I = 0; I < LIUQIN_PCIE_POWER_COMMANDS; ++I)
  {
    UINT32 A = Commands[I].Address;
    UINT32 Slave = (A >> 16) & 15U;
    if ((A & 3U) || A > 0xFFFFCU || (Slave != 3 && Slave != 4))
      return EFI_INVALID_PARAMETER;
    for (UINTN J = 0; J < I; ++J)
      if (A == Commands[J].Address)
        return EFI_INVALID_PARAMETER;
  }
  if (Commands[0].Data != 3 || Commands[1].Data == 0 || Commands[1].Data > 15 ||
      Commands[2].Data == 0 || Commands[2].Data > 15)
    return EFI_INVALID_PARAMETER;
  for (UINTN I = 0; I < 3; ++I)
    if (((Commands[I].Address >> 16) & 15U) != 3 || (Commands[I].Address & 15U))
      return EFI_INVALID_PARAMETER;
  for (UINTN I = 0; I < 2; ++I)
  {
    UINTN N = 3 + I * 3;
    UINT32 A = Commands[N].Address;
    if (((A >> 16) & 15U) != 4 || (A & 15U) || Commands[N + 1].Address != A + 8 ||
        Commands[N + 2].Address != A + 4 || Commands[N].Data != (I == 0 ? 880U : 1200U) ||
        Commands[N + 1].Data != 7 || Commands[N + 2].Data != 1)
      return EFI_INVALID_PARAMETER;
  }
  return Send (Io, Commands, LIUQIN_PCIE_POWER_COMMANDS, Result);
}
STATIC CONST UINT32 WlanMv[] = {1012, 966, 1800, 1900, 1350, 2800};
EFI_STATUS LiuqinWlanPowerPlan (CONST LIUQIN_WLAN_CMD_DB_REPORT *Db, LIUQIN_RPMH_COMMAND *Commands)
{
  LIUQIN_RPMH_COMMAND Plan[LIUQIN_WLAN_POWER_COMMANDS];
  if (!Db || !Commands)
    return EFI_INVALID_PARAMETER;
  for (UINTN I = 0; I < LIUQIN_WLAN_RESOURCE_COUNT; ++I)
  {
    CONST LIUQIN_CMD_DB_RESOURCE *R = &Db->Resources[I];
    if (R->Slave != 4 || ((R->Address >> 16) & 15U) != 4 || (R->Address & 15U) ||
        R->Address > 0xFFFF0U || R->AuxLength > LIUQIN_CMD_DB_AUX_MAX)
      return EFI_COMPROMISED_DATA;
    for (UINTN J = 0; J < I; ++J)
      if (R->Address == Db->Resources[J].Address)
        return EFI_COMPROMISED_DATA;
    // Match CNSS set_voltage + enable. Preserve existing mode: the board's
    // SMPS nodes do not declare a PMIC mode/type for us to override.
    Plan[I * 2] = (LIUQIN_RPMH_COMMAND){R->Address, WlanMv[I]};
    Plan[I * 2 + 1] = (LIUQIN_RPMH_COMMAND){R->Address + 4, 1};
  }
  for (UINTN I = 0; I < LIUQIN_WLAN_POWER_COMMANDS; ++I)
    Commands[I] = Plan[I];
  return EFI_SUCCESS;
}
EFI_STATUS LiuqinWlanPowerOn (CONST LIUQIN_PCIE_IO *Io, CONST LIUQIN_RPMH_COMMAND *Commands,
                              LIUQIN_RPMH_RESULT *Result)
{
  if (!Commands)
    return EFI_INVALID_PARAMETER;
  for (UINTN I = 0; I < LIUQIN_WLAN_RESOURCE_COUNT; ++I)
  {
    UINT32 A = Commands[I * 2].Address;
    if (((A >> 16) & 15U) != 4 || (A & 15U) || A > 0xFFFF0U || Commands[I * 2].Data != WlanMv[I] ||
        Commands[I * 2 + 1].Address != A + 4 || Commands[I * 2 + 1].Data != 1)
      return EFI_INVALID_PARAMETER;
    for (UINTN J = 0; J < I; ++J)
      if (A == Commands[J * 2].Address)
        return EFI_INVALID_PARAMETER;
  }
  return Send (Io, Commands, LIUQIN_WLAN_POWER_COMMANDS, Result);
}
