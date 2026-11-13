/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AcpiPcie.h"
#include "AcpiCmdDb.h"
#include "AcpiRpmh.h"
#include "AcpiPciePhy.h"
#include "AcpiWlan.h"
#include "AcpiPcieLink.h"
#include "AcpiPcieEcam.h"
#include "AcpiPcieHandoff.h"
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/TimerLib.h>
#define DBI 0x60000000U
#define PARF 0x01C00000U
#define GDSCR 0x0017B004U
#define COLLAPSE_VOTE 0x00162128U
#define CLOCK_VOTE 0x00162008U
#define NOC_VOTE 0x00162000U
#define CONFIG_CLOCKS (BIT0 | BIT1 | BIT2 | BIT5)
#define NOC_CLOCKS (BIT11 | BIT12 | BIT19)
#define AUX_CMD 0x0017B064U
#define AUX_CFG 0x0017B068U
#define AUX_CBCR 0x0017B034U
#define PIPE_MUX 0x0017B060U
#define PIPE_CBCR 0x0017B03CU
#define AUX_CFG_MASK (BIT20 | 0x3000U | 0x700U | 0x1FU)
STATIC BOOLEAN mRetainForOs;
STATIC LIUQIN_PCIE_PHY_RESULT mPhyResult;
STATIC BOOLEAN ConfigRead8 (VOID *C, UINTN A, UINT8 *V)
{
  (VOID) C;
  *V = MmioRead8 (A);
  return TRUE;
}
STATIC BOOLEAN ConfigRead16 (VOID *C, UINTN A, UINT16 *V)
{
  (VOID) C;
  *V = MmioRead16 (A);
  return TRUE;
}
STATIC BOOLEAN ConfigWrite16 (VOID *C, UINTN A, UINT16 V)
{
  (VOID) C;
  MmioWrite16 (A, V);
  return TRUE;
}
STATIC EFI_STATUS HandoffWork (CONST LIUQIN_PCIE_IO *Io, VOID *Context)
{
  (VOID) Context;
  LIUQIN_PCIE_CONFIG_IO Config = {NULL, ConfigRead8, ConfigRead16, ConfigWrite16};
  return LiuqinPcieConfigureHandoff (Io, &Config);
}
STATIC EFI_STATUS EcamWork (CONST LIUQIN_PCIE_IO *Io, VOID *Context)
{
  (VOID) Context;
  LIUQIN_PCIE_ECAM_RESULT Result;
  return LiuqinPcieConfigureEcam (Io, &Result, HandoffWork, NULL);
}
STATIC EFI_STATUS LinkWork (CONST LIUQIN_PCIE_IO *Io, VOID *Context)
{
  (VOID) Context;
  LIUQIN_PCIE_LINK_RESULT Result;
  return LiuqinPcieTrainLink (Io, &Result, EcamWork, NULL);
}
STATIC EFI_STATUS WlanWork (CONST LIUQIN_PCIE_IO *Io, VOID *Context)
{
  (VOID) Context;
  LIUQIN_WLAN_RESULT Result;
  return LiuqinWlanEnable (Io, &Result, LinkWork, NULL);
}
STATIC BOOLEAN Read (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 *V)
{
  return Io->Read32 (Io->Context, A, V) && *V != MAX_UINT32;
}
STATIC BOOLEAN Write (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 V)
{
  UINT32 Actual;
  return Io->Write32 (Io->Context, A, V) && Io->Read32 (Io->Context, A, &Actual) && Actual == V;
}
STATIC BOOLEAN UpdateVote (CONST LIUQIN_PCIE_IO *Io, UINTN Address, UINT32 Mask, UINT32 Bits)
{
  UINT32 Value;
  // Shared GCC vote words: keep all unrelated clients' bits, both on acquire
  // and release. Readback compares only the owned mask, not status fields.
  return Read (Io, Address, &Value) &&
         Io->Write32 (Io->Context, Address, (Value & ~Mask) | (Bits & Mask)) &&
         Read (Io, Address, &Value) && (Value & Mask) == (Bits & Mask);
}
typedef struct
{
  BOOLEAN PowerVoteChanged;
  UINT32 ClockVotesChanged;
  UINT32 NocVotesChanged;
} PCIE_INTERFACE_LEASE;
STATIC EFI_STATUS OpenConfigInterface (CONST LIUQIN_PCIE_IO *Io, PCIE_INTERFACE_LEASE *Lease)
{
  UINT32 Value;
  Lease->PowerVoteChanged = FALSE;
  Lease->ClockVotesChanged = 0;
  Lease->NocVotesChanged = 0;
  UINT32 CollapseVote;
  if (!Read (Io, GDSCR, &Value) || !Read (Io, COLLAPSE_VOTE, &CollapseVote))
    return EFI_DEVICE_ERROR;
  if (!(Value & BIT31))
  {
    // Do not override another owner's hardware-control or override mode.
    if (Value & (BIT1 | BIT2))
      return EFI_UNSUPPORTED;
    if (CollapseVote & BIT0)
    {
      // Mark before issuing: a failed callback may have posted the write.
      Lease->PowerVoteChanged = TRUE;
      if (!UpdateVote (Io, COLLAPSE_VOTE, BIT0, 0))
        return EFI_DEVICE_ERROR;
    }
    Io->DelayUs (Io->Context, 1);
    for (UINTN Count = 0;; ++Count)
    {
      if (!Read (Io, GDSCR, &Value))
        return EFI_DEVICE_ERROR;
      if (Value & BIT31)
        break;
      if (Count == 500)
        return EFI_TIMEOUT;
      Io->DelayUs (Io->Context, 1);
    }
    // Respect memory power-up settling before enabling interface clocks.
    Io->DelayUs (Io->Context, 2);
  }
  if (!Read (Io, CLOCK_VOTE, &Value))
    return EFI_DEVICE_ERROR;
  Lease->ClockVotesChanged = CONFIG_CLOCKS & ~Value;
  if (Lease->ClockVotesChanged != 0 &&
      !UpdateVote (Io, CLOCK_VOTE, Lease->ClockVotesChanged, Lease->ClockVotesChanged))
    return EFI_DEVICE_ERROR;
  if (!Read (Io, NOC_VOTE, &Value))
    return EFI_DEVICE_ERROR;
  Lease->NocVotesChanged = NOC_CLOCKS & ~Value;
  if (Lease->NocVotesChanged != 0 &&
      !UpdateVote (Io, NOC_VOTE, Lease->NocVotesChanged, Lease->NocVotesChanged))
    return EFI_DEVICE_ERROR;
  Io->DelayUs (Io->Context, 10);
  STATIC CONST UINT32 Branches[] = {0x0017B030U, 0x0017B020U};
  for (UINTN Index = 0; Index < ARRAY_SIZE (Branches); ++Index)
  {
    for (UINTN Count = 0;; ++Count)
    {
      if (!Read (Io, Branches[Index], &Value))
        return EFI_DEVICE_ERROR;
      if (!(Value & BIT31))
        break;
      if (Count == 500)
        return EFI_TIMEOUT;
      Io->DelayUs (Io->Context, 1);
    }
  }
  // Accessing DBI with core reset asserted is not a meaningful config probe.
  // Releasing reset requires the full cold-init backend, which is separate.
  if (!Read (Io, 0x0017B000U, &Value))
    return EFI_DEVICE_ERROR;
  return (Value & BIT0) ? EFI_NOT_READY : EFI_SUCCESS;
}
STATIC EFI_STATUS CloseConfigInterface (CONST LIUQIN_PCIE_IO *Io, CONST PCIE_INTERFACE_LEASE *Lease)
{
  EFI_STATUS Status = EFI_SUCCESS;
  if (Lease->NocVotesChanged != 0 && !UpdateVote (Io, NOC_VOTE, Lease->NocVotesChanged, 0))
    return EFI_ABORTED;
  if (Lease->ClockVotesChanged != 0)
  {
    if (!UpdateVote (Io, CLOCK_VOTE, Lease->ClockVotesChanged, 0))
      return EFI_ABORTED; // Do not collapse a domain with uncertain clocks.
    Io->DelayUs (Io->Context, 10);
  }
  if (Lease->PowerVoteChanged)
  {
    if (!UpdateVote (Io, COLLAPSE_VOTE, BIT0, BIT0))
      Status = EFI_ABORTED;
    // Stock no-status-check-on-disable: another voter may keep PWR_ON set.
    Io->DelayUs (Io->Context, 100);
  }
  return Status;
}
typedef struct
{
  UINT32 AuxCommand;
  UINT32 AuxConfig;
  UINT32 PipeMux;
  UINT32 Branches;
  BOOLEAN ForceChanged;
  BOOLEAN ConfigChanged;
  BOOLEAN MuxChanged;
} PCIE_LOCAL_CLOCK_LEASE;

STATIC EFI_STATUS WaitBits (CONST LIUQIN_PCIE_IO *Io, UINTN Address, UINT32 Mask, UINT32 Bits)
{
  UINT32 Value;
  for (UINTN Count = 0;; ++Count)
  {
    if (!Read (Io, Address, &Value))
      return EFI_DEVICE_ERROR;
    if ((Value & Mask) == Bits)
      return EFI_SUCCESS;
    if (Count == 500)
      return EFI_TIMEOUT;
    Io->DelayUs (Io->Context, 1);
  }
}
STATIC EFI_STATUS LatchAuxConfig (CONST LIUQIN_PCIE_IO *Io)
{
  UINT32 Value;
  // CMD.UPDATE is self-clearing, not a stable bit for Write() readback.
  if (!Read (Io, AUX_CMD, &Value))
    return EFI_DEVICE_ERROR;
  if (Value & BIT0)
    return EFI_NOT_READY;
  if (!Io->Write32 (Io->Context, AUX_CMD, Value | BIT0))
    return EFI_DEVICE_ERROR;
  return WaitBits (Io, AUX_CMD, BIT0, 0);
}
STATIC EFI_STATUS OpenLocalClocks (CONST LIUQIN_PCIE_IO *Io, PCIE_LOCAL_CLOCK_LEASE *Lease)
{
  UINT32 Value, AuxStatus, PipeStatus;
  EFI_STATUS Status;
  *Lease = (PCIE_LOCAL_CLOCK_LEASE){0};
  if (!Read (Io, PARF + 0x1B0U, &Value))
    return EFI_DEVICE_ERROR;
  if (Value != 0)
    return EFI_UNSUPPORTED;
  if (!Read (Io, PARF, &Value))
    return EFI_DEVICE_ERROR;
  if (Value & BIT26)
    return EFI_UNSUPPORTED;
  if (!Read (Io, AUX_CMD, &Lease->AuxCommand) || !Read (Io, AUX_CFG, &Lease->AuxConfig) ||
      !Read (Io, PIPE_MUX, &Lease->PipeMux) || !Read (Io, AUX_CBCR, &AuxStatus) ||
      !Read (Io, PIPE_CBCR, &PipeStatus) || !Read (Io, CLOCK_VOTE, &Value))
    return EFI_DEVICE_ERROR;

  if ((Value & (BIT3 | BIT4)) || !(AuxStatus & BIT31) || !(PipeStatus & BIT31) ||
      (Lease->AuxCommand & (BIT0 | BIT1)))
    return EFI_UNSUPPORTED;

  Lease->ForceChanged = TRUE;
  if (!UpdateVote (Io, AUX_CMD, BIT1, BIT1))
    return EFI_DEVICE_ERROR;
  Status = WaitBits (Io, AUX_CMD, BIT31, 0);
  if (EFI_ERROR (Status))
    return Status;
  Lease->ConfigChanged = TRUE;
  if (!UpdateVote (Io, AUX_CFG, AUX_CFG_MASK, BIT20 | 1U))
    return EFI_DEVICE_ERROR;
  Status = LatchAuxConfig (Io);
  if (EFI_ERROR (Status))
    return Status;
  Lease->Branches |= BIT3;
  if (!UpdateVote (Io, CLOCK_VOTE, BIT3, BIT3))
    return EFI_DEVICE_ERROR;
  Status = WaitBits (Io, AUX_CBCR, BIT31, 0);
  if (EFI_ERROR (Status))
    return Status;
  Lease->MuxChanged = TRUE;
  if (!UpdateVote (Io, PIPE_MUX, 3, 2))
    return EFI_DEVICE_ERROR;
  Lease->Branches |= BIT4;
  if (!UpdateVote (Io, CLOCK_VOTE, BIT4, BIT4))
    return EFI_DEVICE_ERROR;
  return WaitBits (Io, PIPE_CBCR, BIT31, 0);
}
STATIC EFI_STATUS CloseLocalClocks (CONST LIUQIN_PCIE_IO *Io, CONST PCIE_LOCAL_CLOCK_LEASE *Lease)
{
  // Stop both acquired branches before restoring their parents. Retain the
  // config interface if any source/branch restoration cannot be confirmed.
  if (Lease->Branches != 0 && !UpdateVote (Io, CLOCK_VOTE, Lease->Branches, 0))
    return EFI_ABORTED;
  if (Lease->MuxChanged && !UpdateVote (Io, PIPE_MUX, 3, Lease->PipeMux))
    return EFI_ABORTED;
  if (Lease->ConfigChanged && (!UpdateVote (Io, AUX_CFG, AUX_CFG_MASK, Lease->AuxConfig) ||
                               EFI_ERROR (LatchAuxConfig (Io))))
    return EFI_ABORTED;
  if (Lease->ForceChanged && !UpdateVote (Io, AUX_CMD, BIT1, Lease->AuxCommand))
    return EFI_ABORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ResetOwnedCore (CONST LIUQIN_PCIE_IO *Io)
{
  UINT32 V, Core, Pin, PinIo;

  if (!Read (Io, PARF + 0x1B0, &V) || V || !Read (Io, 0x01C06200, &V) || V != 1 ||
      !Read (Io, 0x01C06240, &V) || V || !Read (Io, 0x01C06244, &V) || V ||
      !Read (Io, 0x0F150004, &V) || (V & 2U) || !Read (Io, 0x0017B000, &Core) || (Core & 1U) ||
      !Read (Io, 0x0F15E000, &Pin) || (Pin & (1U << 12)) || ((Pin >> 2) & 15U) ||
      !Read (Io, 0x0F15E004, &PinIo) || (PinIo & 2U) || !Read (Io, 0x0F15E008, &V) || (V & 1U))
    return EFI_UNSUPPORTED;

  if (!UpdateVote (Io, 0x0F15E004, 2U, 0) || !Write (Io, 0x0F15E000, (Pin & ~0x3FFU) | 0x201U))
    return EFI_ABORTED;
  Io->DelayUs (Io->Context, 1000);

  if (!Write (Io, 0x0017B000, Core | 1U))
    return EFI_ABORTED;
  Io->DelayUs (Io->Context, 1000);

  if (!Write (Io, 0x0017B000, Core))
    return EFI_ABORTED;
  Io->DelayUs (Io->Context, 1000);
  if (!Write (Io, 0x0F15E000, Pin) || !UpdateVote (Io, 0x0F15E004, 2U, PinIo))
    return EFI_ABORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ConfigureRoot (CONST LIUQIN_PCIE_IO *Io, EFI_STATUS *RestoreStatus)
{
  STATIC CONST UINT32 Offsets[] = {0x1000, 0x40,  0x350, 0x354, 0x634,
                                   0x638,  0x358, 0x35C, 0,     0x174};
  UINT32 Saved[ARRAY_SIZE (Offsets)], Values[ARRAY_SIZE (Offsets)], Value;
  UINTN Changed = 0;
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  *RestoreStatus = EFI_SUCCESS;
  // Do not reconfigure an active or already blocked root port in a probe.
  if (!Read (Io, PARF + 0x1B0U, &Value))
    return EFI_DEVICE_ERROR;
  if (Value != 0)
    return EFI_UNSUPPORTED;
  for (UINTN Index = 0; Index < ARRAY_SIZE (Offsets); ++Index)
  {
    if (!Read (Io, PARF + Offsets[Index], &Saved[Index]))
      return EFI_DEVICE_ERROR;
  }
  if (Saved[8] & BIT26)
    return EFI_UNSUPPORTED;
  Values[0] = 4;
  Values[1] = Saved[1] & ~(UINT32)BIT0;
  Values[2] = 0; // pci-msm.c::msm_pcie_enable, PCIE20_PARF_DBI_BASE_ADDR
  Values[3] = 0;
  Values[4] = Saved[4];
  Values[5] = Saved[5];
  Values[6] = 0x04000000U;
  Values[7] = 0;

  Values[8] =
      (Saved[8] & ~(UINT32)BIT29) | BIT13 | BIT12 | BIT10 | BIT9 | BIT6 | BIT4 | BIT2 | BIT1;
  Values[9] = Saved[9] | BIT4;
  for (UINTN Index = 0; Index < ARRAY_SIZE (Offsets); ++Index)
  {
    // A failed callback can represent a posted write: restore this slot too.
    Changed = Index + 1;
    if (!Write (Io, PARF + Offsets[Index], Values[Index]))
      goto Restore;
  }
  Io->DelayUs (Io->Context, 10);
  UINT32 Id, Version;
  if (!Read (Io, DBI, &Id) || Id != 0x011017CBU || !Read (Io, DBI + 0x8F8, &Version) ||
      Version != 0x3533302AU)
    goto Restore;
  Status = LiuqinPcieInitializePhy (Io, &mPhyResult, WlanWork, NULL);
  if (Status == EFI_ABORTED)
  {
    *RestoreStatus = EFI_ABORTED;
    return Status;
  }
  if (Status == EFI_SUCCESS && mPhyResult.Retained && *Io->RetainForOs)
    return EFI_SUCCESS;
Restore:
  while (Changed != 0)
  {
    --Changed;
    if (!Write (Io, PARF + Offsets[Changed], Saved[Changed]))
      *RestoreStatus = EFI_ABORTED;
  }
  return EFI_ERROR (*RestoreStatus) ? EFI_ABORTED : Status;
}
EFI_STATUS LiuqinPcieInitializeWithIo (CONST LIUQIN_PCIE_IO *Io)
{
  PCIE_INTERFACE_LEASE Lease;
  PCIE_LOCAL_CLOCK_LEASE Local;
  EFI_STATUS Status, Restore;
  if (!Io || !Io->Read32 || !Io->Write32 || !Io->DelayUs || !Io->RetainForOs)
    return EFI_INVALID_PARAMETER;
  *Io->RetainForOs = FALSE;
  Status = OpenConfigInterface (Io, &Lease);
  if (!EFI_ERROR (Status))
  {
    Status = OpenLocalClocks (Io, &Local);
    Restore = EFI_SUCCESS;
    if (!EFI_ERROR (Status))
      Status = ResetOwnedCore (Io);
    if (Status == EFI_ABORTED)
      return Status;
    if (!EFI_ERROR (Status))
      Status = ConfigureRoot (Io, &Restore);
    if (EFI_ERROR (Restore) || Status == EFI_ABORTED)
      return EFI_ABORTED;
    if (Status == EFI_SUCCESS && *Io->RetainForOs)
      return EFI_SUCCESS;
    if (EFI_ERROR (CloseLocalClocks (Io, &Local)))
      return EFI_ABORTED;
  }
  if (EFI_ERROR (CloseConfigInterface (Io, &Lease)))
    return EFI_ABORTED;
  return Status;
}
STATIC BOOLEAN NativeRead (VOID *Context, UINTN Address, UINT32 *Value)
{
  (VOID) Context;
  *Value = MmioRead32 (Address);
  return TRUE;
}
STATIC BOOLEAN NativeWrite (VOID *Context, UINTN Address, UINT32 Value)
{
  (VOID) Context;
  MmioWrite32 (Address, Value);
  return TRUE;
}
STATIC VOID NativeDelay (VOID *Context, UINTN Microseconds)
{
  (VOID) Context;
  MicroSecondDelay (Microseconds);
}
STATIC int ReadCmdDbWord (VOID *Context, LIUQIN_U32 Offset, LIUQIN_U32 *Value)
{
  CONST LIUQIN_PCIE_IO *Io = Context;
  UINT32 Word;
  if ((Offset & 3U) || Offset > LIUQIN_CMD_DB_SIZE - 4U ||
      !Io->Read32 (Io->Context, (UINTN)LIUQIN_CMD_DB_BASE + Offset, &Word))
    return 0;
  *Value = Word;
  return 1;
}
EFI_STATUS InitializeLiuqinPcie (VOID)
{
  LIUQIN_PCIE_IO Io = {NULL, NativeRead, NativeWrite, NativeDelay, &mRetainForOs};
  LIUQIN_CMD_DB_REPORT Host;
  LIUQIN_WLAN_CMD_DB_REPORT Wlan;
  LIUQIN_RPMH_COMMAND HostCommands[LIUQIN_PCIE_POWER_COMMANDS];
  LIUQIN_RPMH_COMMAND WlanCommands[LIUQIN_WLAN_POWER_COMMANDS];
  LIUQIN_RPMH_RESULT Power;
  LIUQIN_WLAN_RESULT Pins;
  EFI_STATUS Status;
  int Reason;
  if (mRetainForOs)
    return EFI_SUCCESS;
  VOID *Snapshot = AllocatePool (LIUQIN_CMD_DB_SIZE);
  if (!Snapshot)
    return EFI_OUT_OF_RESOURCES;
  Reason = LiuqinCaptureCmdDb (ReadCmdDbWord, &Io, Snapshot, LIUQIN_CMD_DB_SIZE, &Host, NULL);
  if (!Reason)
    Reason = LiuqinCaptureWlanCmdDb (ReadCmdDbWord, &Io, Snapshot, LIUQIN_CMD_DB_SIZE, &Wlan, NULL);
  FreePool (Snapshot);
  if (Reason)
    return EFI_COMPROMISED_DATA;
  Status = LiuqinPciePowerPlan (&Host, HostCommands);
  if (EFI_ERROR (Status))
    return Status;
  Status = LiuqinWlanPowerPlan (&Wlan, WlanCommands);
  if (EFI_ERROR (Status))
    return Status;
  Status = LiuqinWlanPreflight (&Io, &Pins);
  if (EFI_ERROR (Status))
    return Status;
  Status = LiuqinPciePowerOn (&Io, HostCommands, &Power);
  if (EFI_ERROR (Status))
    return Status;
  Status = LiuqinWlanPowerOn (&Io, WlanCommands, &Power);
  if (EFI_ERROR (Status))
    return Status;
  return LiuqinPcieInitializeWithIo (&Io);
}
BOOLEAN LiuqinPcieHandoffReady (VOID)
{
  return mRetainForOs;
}
