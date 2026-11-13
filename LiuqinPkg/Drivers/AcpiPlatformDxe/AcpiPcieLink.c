/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AcpiPcieLink.h"
#define PARF 0x01C00000U
#define DBI 0x60000000U
#define ATU 0x60001000U
#define CFG0 0x60100000U
#define PERST 0x0F15E004U
STATIC CONST struct
{
  UINTN A;
  UINT32 Mask, Bits, WriteMask;
} Controls[] = {
    {PARF + 0x22C, MAX_UINT32, 0, MAX_UINT32}, // poll only, no PCIe global IRQ
    {PARF + 0x508, MAX_UINT32, 0, MAX_UINT32},
    {PARF + 0x2C4, 1U << 8, 1U << 8, MAX_UINT32},
    {DBI + 0x714, 1U << 5, 1U << 5, MAX_UINT32},
    {DBI + 0x80C, 0x1F00, 0x100, MAX_UINT32},
    {DBI + 0xA0, 15, 2, 0xFFFF}, // Gen2, as CNSS QCA6490 first enumeration
    {DBI + 0x80, 3, 0, 0xFFFF},  // ASPM disabled during link initialization
    {DBI + 0x18, 0xFFFFFF, 0x010100, MAX_UINT32},
    {DBI + 0xB40, MAX_UINT32, 20, MAX_UINT32} // stock 19.2MHz AUX indication
};
STATIC BOOLEAN Rd (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 *V)
{
  return Io->Read32 (Io->Context, A, V) && *V != MAX_UINT32;
}
STATIC BOOLEAN Fail (LIUQIN_PCIE_LINK_RESULT *R, UINTN A, UINT32 V, UINT32 Mask, UINT32 Bits,
                     UINT32 Reason)
{
  if (!R->FailedReason)
  {
    R->FailedAddress = (UINT32)A;
    R->FailedValue = V;
    R->FailedMask = Mask;
    R->FailedBits = Bits & Mask;
    R->FailedReason = Reason;
  }
  return FALSE;
}
STATIC BOOLEAN Update (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 Mask, UINT32 Bits,
                       UINT32 WriteMask, LIUQIN_PCIE_LINK_RESULT *R, UINTN WaitUs, UINT32 *Last,
                       UINT32 *Polls)
{
  UINT32 V = MAX_UINT32;
  if (!Rd (Io, A, &V))
    return Fail (R, A, V, Mask, Bits, 1);
  if (!Io->Write32 (Io->Context, A, ((V & ~Mask) | (Bits & Mask)) & WriteMask))
    return Fail (R, A, V, Mask, Bits, 2);

  for (UINTN I = 0;; ++I)
  {
    V = MAX_UINT32;
    if (!Rd (Io, A, &V))
      return Fail (R, A, V, Mask, Bits, 1);
    *Last = V;
    ++*Polls;
    if ((V & Mask) == (Bits & Mask))
      return TRUE;
    if (I == WaitUs)
      return Fail (R, A, V, Mask, Bits, 3);
    Io->DelayUs (Io->Context, 1);
  }
}
STATIC BOOLEAN Up (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 Mask, UINT32 Bits, UINT32 WriteMask,
                   LIUQIN_PCIE_LINK_RESULT *R)
{
  UINT32 Last = 0, Polls = 0;
  return Update (Io, A, Mask, Bits, WriteMask, R, 0, &Last, &Polls);
}
STATIC BOOLEAN Wr (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 V, LIUQIN_PCIE_LINK_RESULT *R)
{
  return Up (Io, A, MAX_UINT32, V, MAX_UINT32, R);
}
EFI_STATUS LiuqinPcieTrainLink (CONST LIUQIN_PCIE_IO *Io, LIUQIN_PCIE_LINK_RESULT *Result,
                                LIUQIN_PCIE_SCOPED_WORK Work, VOID *Context)
{
  LIUQIN_PCIE_LINK_RESULT R = {0};
  UINT32 Saved[ARRAY_SIZE (Controls)], Atu[7], V;
  UINTN Changed = 0;
  BOOLEAN Dirty = FALSE, AtuDirty = FALSE, Clean = TRUE;
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  if (!Io || !Io->Read32 || !Io->Write32 || !Io->DelayUs || !Result)
    return EFI_INVALID_PARAMETER;
  R.Stage = 1;
  if (!Rd (Io, PARF + 0x1B0, &V))
    goto End;
  if (V & (1U << 8))
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Rd (Io, PARF, &V))
    goto End;
  if (V & ((1U << 26) | (1U << 29)))
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Rd (Io, DBI, &V))
    goto End;
  if (V != 0x011017CB)
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Rd (Io, DBI + 0x7C, &V))
    goto End;
  if ((V & 15U) < 2 || (V & 15U) > 4)
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Rd (Io, DBI + 0x80, &V))
    goto End;
  if (V & (1U << 29))
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Rd (Io, 0x01C06214, &R.Phy))
    goto End;
  if (R.Phy & (1U << 6))
  {
    Status = EFI_NOT_READY;
    goto End;
  }
  if (!Rd (Io, 0x01C06240, &V) || V != 3 || !Rd (Io, 0x01C06200, &V) || V != 0 ||
      !Rd (Io, 0x01C06244, &V) || V != 3 || !Rd (Io, 0x0017B060, &V) || (V & 3U) ||
      !Rd (Io, 0x0019C004, &V) || !(V & 1U))
    goto End;
  if (!Rd (Io, 0x0F15E000, &V) || (V & 0x3FFU) != 0x201 || !Rd (Io, PERST, &V) || (V & 2U) ||
      !Rd (Io, 0x0F150004, &V) || (V & 3U) != 3 || !Rd (Io, 0x0F152004, &V) || !(V & 1U))
  {
    Status = EFI_NOT_READY;
    goto End;
  }
  for (UINTN I = 0; I < ARRAY_SIZE (Controls); ++I)
    if (!Rd (Io, Controls[I].A, &Saved[I]))
      goto End;
  for (UINTN I = 0; I < 7; ++I)
    if (!Rd (Io, ATU + I * 4, &Atu[I]))
      goto End;
  // Never borrow a memory/DMA region as configuration space.
  if ((Atu[1] & (1U << 31)) && (Atu[0] != 4 || Atu[2] != CFG0 || Atu[3] || Atu[4] != CFG0 + 0xFFF ||
                                Atu[5] != 0x01000000 || Atu[6]))
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  R.Stage = 2;
  Dirty = TRUE;
  for (UINTN I = 0; I < ARRAY_SIZE (Controls); ++I)
  {
    Changed = I + 1;
    if (!Up (Io, Controls[I].A, Controls[I].Mask, Controls[I].Bits, Controls[I].WriteMask, &R))
      goto Restore;
  }
  // Allow endpoint power/refclock to stabilize before releasing reset.
  Io->DelayUs (Io->Context, 100000);
  R.Stage = 3;
  if (!Up (Io, PERST, 2, 2, MAX_UINT32, &R))
    goto Restore;
  Io->DelayUs (Io->Context, 1000);
  if (!Update (Io, PARF + 0x1B0, 1U << 8, 1U << 8, MAX_UINT32, &R, 1000, &R.EnableReadback,
               &R.EnablePolls))
  {
    if (R.FailedReason == 3)
      Status = EFI_TIMEOUT;
    goto Restore;
  }
  Status = EFI_TIMEOUT;
  R.Stage = 4;
  for (UINTN I = 0; I < 20; ++I)
  {
    Io->DelayUs (Io->Context, 5000);
    ++R.Polls;
    if (!Rd (Io, DBI + 0xF28, &R.Elbi) || !Rd (Io, DBI + 0x80, &R.Link) ||
        !Rd (Io, PARF + 0x1B0, &R.Ltssm))
    {
      Status = EFI_DEVICE_ERROR;
      goto Restore;
    }
    if ((R.Elbi & (1U << 10)) && (R.Link & (1U << 29)))
    {
      Status = EFI_SUCCESS;
      break;
    }
  }
  if (EFI_ERROR (Status))
    goto Restore;

  if (((R.Link >> 16) & 15U) > 2 || ((R.Link >> 20) & 63U) != 1 || !((R.Link >> 16) & 15U))
  {
    Status = EFI_UNSUPPORTED;
    goto Restore;
  }
  R.Stage = 5;
  AtuDirty = TRUE;
  Status = EFI_DEVICE_ERROR;
  if (!Wr (Io, ATU + 4, 0, &R) || !Wr (Io, ATU, 4, &R) || !Wr (Io, ATU + 8, CFG0, &R) ||
      !Wr (Io, ATU + 12, 0, &R) || !Wr (Io, ATU + 16, CFG0 + 0xFFF, &R) ||
      !Wr (Io, ATU + 20, 0x01000000, &R) || !Wr (Io, ATU + 24, 0, &R) ||
      !Wr (Io, ATU + 4, 1U << 31, &R))
    goto Restore;
  R.Stage = 6;
  Status = EFI_TIMEOUT;
  for (UINTN I = 0; I < 1000; ++I)
  {
    if (!Io->Read32 (Io->Context, CFG0, &R.EndpointId))
    {
      Status = EFI_DEVICE_ERROR;
      goto Restore;
    }
    ++R.EndpointReads;
    if ((R.EndpointId & 0xFFFFU) != 0xFFFFU && (R.EndpointId & 0xFFFFU) != 0)
    {
      Status = EFI_SUCCESS;
      break;
    }
    Io->DelayUs (Io->Context, 1000);
  }
  if (EFI_ERROR (Status))
    goto Restore;
  if (!Rd (Io, CFG0 + 4, &R.EndpointCommand) || !Rd (Io, CFG0 + 8, &R.EndpointClass) ||
      !Rd (Io, CFG0 + 12, &R.EndpointHeader) || !Rd (Io, CFG0, &V) || V != R.EndpointId)
  {
    Status = EFI_DEVICE_ERROR;
    goto Restore;
  }
  R.Stage = 7;
  if (R.EndpointId != 0x110317CB || (R.EndpointCommand & 4U))
    Status = EFI_UNSUPPORTED;
  if (!EFI_ERROR (Status) && Work)
  {
    Status = Work (Io, Context);
    if (Status == EFI_ABORTED)
      goto End;
  }
  if (Status == EFI_SUCCESS && Work && Io->RetainForOs && *Io->RetainForOs)
  {
    R.Retained = TRUE;
    goto End;
  }
Restore:
  // Stop traffic before altering iATU or removing any parent power/clock.
  if (!Up (Io, PERST, 2, 0, MAX_UINT32, &R))
  {
    Status = EFI_ABORTED;
    goto End;
  }
  Io->DelayUs (Io->Context, 1000);
  if (!Update (Io, PARF + 0x1B0, 1U << 8, 0, MAX_UINT32, &R, 1000, &R.DisableReadback,
               &R.DisablePolls))
  {
    Status = EFI_ABORTED;
    goto End;
  }
  Clean = FALSE;
  for (UINTN I = 0; I < 100; ++I)
  {
    if (!Rd (Io, DBI + 0x80, &V))
      break;
    if (!(V & (1U << 29)))
    {
      Clean = TRUE;
      break;
    }
    Io->DelayUs (Io->Context, 100);
  }
  if (!Clean)
  {
    Status = EFI_ABORTED;
    goto End;
  }
  if (AtuDirty)
  {
    if (!Wr (Io, ATU + 4, 0, &R))
    {
      Status = EFI_ABORTED;
      goto End;
    }
    for (UINTN I = 0; I < 7; ++I)
      if (I != 1 && !Wr (Io, ATU + I * 4, Atu[I], &R))
        Clean = FALSE;
    if (!Clean || !Wr (Io, ATU + 4, Atu[1], &R))
    {
      Status = EFI_ABORTED;
      goto End;
    }
  }
  while (Changed)
  {
    --Changed;
    if (!Up (Io, Controls[Changed].A, Controls[Changed].Mask, Saved[Changed],
             Controls[Changed].WriteMask, &R))
      Clean = FALSE;
  }
  R.Quiesced = Clean;
  if (!Clean)
    Status = EFI_ABORTED;
End:
  if (!Dirty)
    R.Quiesced = TRUE;
  *Result = R;
  return Status;
}
