/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AcpiPcieEcam.h"
#define PARF 0x01C00000U
#define DBI 0x60000000U
#define ATU 0x60001000U
#define MOVED_ATU 0x60200000U
#define MOVED_DECODE 0x00200000U
#define DMA 0x60002000U
#define MOVED_DMA 0x60202000U
#define MOVED_DMA_DECODE 0x00202000U
#define CFG 0x60100000U
#define CFG_DEVICE_SIZE 0x8000U
#define BLOCK (1U << 26)
#define BLOCKER_CPU 1U
STATIC CONST UINT32 Offsets[] = {0x380, 0x384, 0x360, 0x364, 0x368,
                                 0x36C, 0x370, 0x374, 0x378, 0x37C};
STATIC CONST UINT32 BlockValues[] = {DBI,          0, DBI + 0x1000,  0, DBI + 0xFFFFF, 0,
                                     DBI + 0x1000, 0, DBI + 0xFFFFF, 0};

STATIC CONST UINT32 AtuValues[] = {4, 0x90000000U, CFG, 0, CFG + CFG_DEVICE_SIZE - 1, 0, 0};
STATIC CONST UINT32 Samples[] = {DBI,          CFG,     DBI + 0x1000, DBI + 0x8000, CFG + 0x1000,
                                 CFG + 0x8000, CFG + 4, DBI,          CFG};
STATIC CONST UINT32 Expected[] = {0x011017CB, 0x110317CB, MAX_UINT32, MAX_UINT32, MAX_UINT32,
                                  MAX_UINT32, 0,          0x011017CB, 0x110317CB};

STATIC CONST UINT32 DmaOffsets[] = {0, 8, 0xC, 0x2C};
STATIC BOOLEAN Read (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 *V)
{
  return Io->Read32 (Io->Context, A, V) && *V != MAX_UINT32;
}
STATIC BOOLEAN Failed (LIUQIN_PCIE_ECAM_RESULT *R, UINTN A, UINT32 V, UINT32 Want)
{
  if (!R->FailedAddress)
  {
    R->FailedAddress = (UINT32)A;
    R->FailedValue = V;
    R->FailedExpected = Want;
  }
  return FALSE;
}
STATIC BOOLEAN Write (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 V, UINT32 Mask,
                      LIUQIN_PCIE_ECAM_RESULT *R, UINTN Wait)
{
  UINT32 Actual = MAX_UINT32;
  if (!Io->Write32 (Io->Context, A, V))
    return Failed (R, A, Actual, V);
  for (UINTN I = 0;; ++I)
  {
    if (!Io->Read32 (Io->Context, A, &Actual))
      return Failed (R, A, MAX_UINT32, V);
    if ((Actual & Mask) == (V & Mask))
      return TRUE;
    if (I == Wait)
      return Failed (R, A, Actual, V);
    Io->DelayUs (Io->Context, 1);
  }
}
STATIC BOOLEAN Blocker (CONST LIUQIN_PCIE_IO *Io, UINT32 Bits, LIUQIN_PCIE_ECAM_RESULT *R)
{
  UINT32 V;
  if (!Read (Io, PARF, &V))
    return Failed (R, PARF, MAX_UINT32, Bits);
  if (Bits & BLOCK)
  {
    CONST UINT32 *Values = BlockValues;
    for (UINTN I = 2; I < ARRAY_SIZE (Offsets); ++I)
      if (!Write (Io, PARF + Offsets[I], Values[I], MAX_UINT32, R, 0))
        return FALSE;
  }
  return Write (Io, PARF, (V & ~BLOCK) | (Bits & BLOCK), BLOCK, R, 1000);
}
STATIC BOOLEAN ReadSamples (CONST LIUQIN_PCIE_IO *Io, LIUQIN_PCIE_ECAM_RESULT *R)
{
  for (UINTN I = 0; I < ARRAY_SIZE (Samples); ++I)
  {
    if (!Io->Read32 (Io->Context, Samples[I], &R->Values[I]))
    {
      Failed (R, Samples[I], MAX_UINT32, Expected[I]);
      return FALSE;
    }
    R->ReadMask |= 1U << I;
    if (I == 6 ? (R->Values[I] == MAX_UINT32 || (R->Values[I] & 4U)) : R->Values[I] != Expected[I])
    {
      Failed (R, Samples[I], R->Values[I], Expected[I]);
      return FALSE;
    }
  }
  return TRUE;
}
STATIC BOOLEAN CheckAtu (CONST LIUQIN_PCIE_IO *Io, UINTN Base, CONST UINT32 *Saved,
                         LIUQIN_PCIE_ECAM_RESULT *R, BOOLEAN Capture)
{
  for (UINTN I = 0; I < 7; ++I)
  {
    UINT32 V;
    if (!Io->Read32 (Io->Context, Base + I * 4, &V))
      return Failed (R, Base + I * 4, MAX_UINT32, Saved[I]);
    if (Capture)
    {
      R->MoveValues[I] = V;
      R->MoveReadMask |= 1U << I;
    }
    if (V != Saved[I])
      return Failed (R, Base + I * 4, V, Saved[I]);
  }
  return TRUE;
}
STATIC BOOLEAN CheckDma (CONST LIUQIN_PCIE_IO *Io, UINTN Base, LIUQIN_PCIE_ECAM_RESULT *R,
                         BOOLEAN Capture)
{
  for (UINTN I = 0; I < ARRAY_SIZE (DmaOffsets); ++I)
  {
    UINTN A = Base + DmaOffsets[I];
    UINT32 V;
    if (!Io->Read32 (Io->Context, A, &V))
      return Failed (R, A, MAX_UINT32, R->DmaValues[I]);
    if (Capture)
    {
      R->DmaValues[I] = V;
      R->DmaReadMask |= 1U << I;
    }
    else
    {
      if (Base == MOVED_DMA)
        R->DmaAliasMask |= 1U << I;
      else
        R->DmaRestoreMask |= 1U << I;
      if (V != R->DmaValues[I])
        return Failed (R, A, V, R->DmaValues[I]);
    }
    // Require the observed arbitration signature and both engines fully off.
    // No attempt to stop an inherited engine or modify DMA control registers.
    if ((I == 0 && V != 0x688) || (I == 1 && V == MAX_UINT32) || (I >= 2 && V))
      return Failed (R, A, V, I == 0 ? 0x688U : 0);
  }
  return TRUE;
}
EFI_STATUS LiuqinPcieConfigureEcam (CONST LIUQIN_PCIE_IO *Io, LIUQIN_PCIE_ECAM_RESULT *Result,
                                    LIUQIN_PCIE_SCOPED_WORK Work, VOID *Context)
{
  LIUQIN_PCIE_ECAM_RESULT R = {0};
  UINT32 V, Parf[ARRAY_SIZE (Offsets)], Atu[7];
  UINTN Changed = 0;
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  BOOLEAN Dirty = FALSE, MoveDirty = FALSE, DmaDirty = FALSE, BlockDirty = FALSE, Clean = TRUE;
  if (!Io || !Io->Read32 || !Io->Write32 || !Io->DelayUs || !Result)
    return EFI_INVALID_PARAMETER;
  R.Stage = 1;
  if (!Read (Io, 0x0017B004, &V) || !(V & (1U << 31)) || !Read (Io, 0x0017B030, &V) ||
      (V & (1U << 31)) || !Read (Io, 0x0017B020, &V) || (V & (1U << 31)) ||
      !Read (Io, PARF + 0x1000, &V) || V != 4 || !Read (Io, PARF + 0x1B0, &V) || !(V & (1U << 8)))
  {
    Status = EFI_NOT_READY;
    goto End;
  }
  if (!Read (Io, PARF, &V) || (V & BLOCK) || !Read (Io, PARF + 0x358, &V) || V != 0x04000000 ||
      !Read (Io, PARF + 0x35C, &V) || V)
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  for (UINTN I = 0; I < 4; ++I)
  {
    UINT32 O = I < 2 ? 0x350U + (UINT32)I * 4U : 0x634U + ((UINT32)I - 2U) * 4U;
    if (!Read (Io, PARF + O, &R.Decode[I]))
      goto End;
  }

  if (R.Decode[0] || R.Decode[1] || R.Decode[2] != 0x1000 || R.Decode[3])
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Read (Io, DBI, &V) || V != 0x011017CB || !Read (Io, DBI + 0x8F8, &V) || V != 0x3533302A ||
      !Read (Io, DBI + 0x80, &V) || !(V & (1U << 29)) || !Read (Io, DBI + 0x18, &V) ||
      (V & 0xFFFFFFU) != 0x010100 || !Read (Io, CFG, &V) || V != 0x110317CB ||
      !Read (Io, CFG + 4, &V) || (V & 4U))
  {
    Status = EFI_NOT_READY;
    goto End;
  }
  R.Stage = 2;
  for (UINTN I = 0; I < 7; ++I)
    if (!Io->Read32 (Io->Context, ATU + I * 4, &Atu[I]))
      goto End;
  if (Atu[0] != 4 || Atu[1] != 0x80000000U || Atu[2] != CFG || Atu[3] || Atu[4] != CFG + 0xFFF ||
      Atu[5] != 0x01000000 || Atu[6])
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  for (UINTN I = 0; I < ARRAY_SIZE (Offsets); ++I)
    if (!Io->Read32 (Io->Context, PARF + Offsets[I], &Parf[I]))
      goto End;

  R.DmaStage = 1;
  for (UINTN I = 0; I < 2; ++I)
    if (!Read (Io, PARF + 0x64C + I * 4, &R.DmaDecode[I]))
      goto End;
  if (R.DmaDecode[0] != 0x2000 || R.DmaDecode[1])
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  R.DmaStage = 2;
  if (!CheckDma (Io, DMA, &R, TRUE))
    goto End;
  // Separate from relocated iATU and outside bus0..1. No eDMA CSR writes.
  R.DmaStage = 3;
  DmaDirty = TRUE;
  if (!Write (Io, PARF + 0x64C, MOVED_DMA_DECODE, MAX_UINT32, &R, 1000) ||
      !CheckDma (Io, MOVED_DMA, &R, FALSE))
    goto RestoreDma;
  R.DmaStage = 4;

  R.MoveStage = 1;
  MoveDirty = TRUE;
  if (!Write (Io, PARF + 0x634, MOVED_DECODE, MAX_UINT32, &R, 1000))
    goto RestoreDecode;
  R.MoveStage = 2;
  // Read-only alias proof: do not program the relocated window unless every
  // original register matches, including the live legacy CFG0 mapping.
  if (!CheckAtu (Io, MOVED_ATU, Atu, &R, TRUE))
    goto RestoreDecode;
  R.MoveStage = 3;
  R.Stage = 3;
  Dirty = TRUE;
  if (!Write (Io, MOVED_ATU + 4, 0, MAX_UINT32, &R, 1000))
    goto Restore;
  for (UINTN I = 0; I < 7; ++I)
    if (I != 1 && !Write (Io, MOVED_ATU + I * 4, AtuValues[I], MAX_UINT32, &R, 0))
      goto Restore;
  if (!Io->Read32 (Io->Context, MOVED_ATU + 16, &R.CfgLimit))
  {
    Failed (&R, MOVED_ATU + 16, MAX_UINT32, AtuValues[4]);
    goto Restore;
  }
  R.CfgLimitRead = TRUE;
  if (R.CfgLimit != AtuValues[4])
  {
    Failed (&R, MOVED_ATU + 16, R.CfgLimit, AtuValues[4]);
    goto Restore;
  }
  for (UINTN I = 0; I < ARRAY_SIZE (Offsets); ++I)
  {
    Changed = I + 1;
    if (!Write (Io, PARF + Offsets[I], BlockValues[I], MAX_UINT32, &R, 0))
      goto Restore;
  }
  if (!Write (Io, MOVED_ATU + 4, AtuValues[1], MAX_UINT32, &R, 1000))
    goto Restore;
  // bus0/function1 must now be an absent BDF, not the internal iATU window.
  R.Stage = 4;
  BlockDirty = TRUE;
  R.BlockerDomain = BLOCKER_CPU;
  if (!Blocker (Io, BLOCK, &R))
    goto Restore;
  R.Stage = 5;
  if (!ReadSamples (Io, &R))
    goto Restore;
  R.Stage = 7;
  Status = EFI_SUCCESS;
  if (Work)
  {
    Status = Work (Io, Context);
    if (Status == EFI_ABORTED)
      goto End;

    if (Status == EFI_SUCCESS && Io->RetainForOs && *Io->RetainForOs)
    {
      R.Retained = TRUE;
      goto End;
    }
    if (!EFI_ERROR (Status))
    {
      // Recheck the actual link/identity/BME after the larger validation suite.
      if (!ReadSamples (Io, &R))
        Status = EFI_DEVICE_ERROR;
      if (!EFI_ERROR (Status) && (!Read (Io, DBI + 0x80, &V) || !(V & (1U << 29))))
        Status = EFI_NOT_READY;
    }
  }
  if (Io->RetainForOs)
    *Io->RetainForOs = FALSE;
Restore:
  if (BlockDirty && !Blocker (Io, 0, &R))
  {
    Status = EFI_ABORTED;
    goto End;
  }
  if (!Write (Io, MOVED_ATU + 4, 0, MAX_UINT32, &R, 1000))
  {
    Status = EFI_ABORTED;
    goto End;
  }
  for (UINTN I = 0; I < 7; ++I)
    if (I != 1 && !Write (Io, MOVED_ATU + I * 4, Atu[I], MAX_UINT32, &R, 0))
      Clean = FALSE;
  while (Changed)
  {
    --Changed;
    if (!Write (Io, PARF + Offsets[Changed], Parf[Changed], MAX_UINT32, &R, 0))
      Clean = FALSE;
  }
  if (!Clean || !Write (Io, MOVED_ATU + 4, Atu[1], MAX_UINT32, &R, 1000))
  {
    Status = EFI_ABORTED;
    goto End;
  }
RestoreDecode:
  // PARF remains reachable even if the attempted alias was not implemented.
  // Never touch the old ATU address until its decode is confirmed restored.
  if (!Write (Io, PARF + 0x634, R.Decode[2], MAX_UINT32, &R, 1000) ||
      !CheckAtu (Io, ATU, Atu, &R, FALSE))
  {
    Status = EFI_ABORTED;
    goto End;
  }
  R.MoveRestored = TRUE;
RestoreDma:
  if (!Write (Io, PARF + 0x64C, R.DmaDecode[0], MAX_UINT32, &R, 1000) ||
      !CheckDma (Io, DMA, &R, FALSE))
  {
    Status = EFI_ABORTED;
    goto End;
  }
  R.DmaRestored = TRUE;
  R.Restored = TRUE;
End:
  if (!MoveDirty && !Dirty)
    R.MoveRestored = TRUE;
  if (!DmaDirty)
    R.DmaRestored = TRUE;
  if (R.MoveRestored && R.DmaRestored)
    R.Restored = TRUE;
  *Result = R;
  return Status;
}
