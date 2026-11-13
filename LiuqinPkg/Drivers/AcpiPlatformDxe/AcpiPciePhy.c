/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AcpiPciePhy.h"
#define PHY 0x01C06000U
#define PARF 0x01C00000U
#define VOTE 0x00162000U
#define CLOCK 0x00162008U
#define RCG 0x0017B048U
#define CFG 0x0017B04CU
#define PIPE 0x0017B060U
#define CLKREF 0x0019C004U
#define CORE_BCR 0x0017B000U
#define PHY_BCR 0x0017C01CU
#define RCHNG (1U << 22)
#define MASK ((1U << 20) | 0x3000U | 0x700U | 31U)
STATIC CONST struct
{
  UINT32 Offset, Value;
} mPhy[] = {
    {0x0240, 0x03}, {0x00C0, 0x01}, {0x00CC, 0x31}, {0x00D0, 0x01}, {0x0060, 0xFF}, {0x0064, 0x06},
    {0x0000, 0x4C}, {0x0004, 0x06}, {0x00E0, 0x90}, {0x00E4, 0x82}, {0x00F4, 0x07}, {0x0070, 0x02},
    {0x0010, 0x02}, {0x0074, 0x16}, {0x0014, 0x16}, {0x0078, 0x36}, {0x0018, 0x36}, {0x0110, 0x08},
    {0x00BC, 0x0E}, {0x0120, 0x42}, {0x0080, 0x0A}, {0x0084, 0x1A}, {0x0020, 0x14}, {0x0024, 0x34},
    {0x0088, 0x82}, {0x0028, 0x68}, {0x0090, 0xAB}, {0x0094, 0xEA}, {0x0098, 0x02}, {0x0030, 0xAB},
    {0x0034, 0xAA}, {0x0038, 0x02}, {0x0140, 0x14}, {0x0164, 0x34}, {0x003C, 0x01}, {0x001C, 0x04},
    {0x0174, 0x16}, {0x01BC, 0x0F}, {0x0170, 0xA0}, {0x11A4, 0x38}, {0x10DC, 0x11}, {0x1160, 0xBF},
    {0x1164, 0xBF}, {0x1168, 0xB7}, {0x116C, 0xEA}, {0x115C, 0x3F}, {0x1174, 0x5C}, {0x1178, 0x9C},
    {0x117C, 0x1A}, {0x1180, 0x89}, {0x1170, 0xDC}, {0x1188, 0x94}, {0x118C, 0x5B}, {0x1190, 0x1A},
    {0x1194, 0x89}, {0x10CC, 0xF0}, {0x1008, 0x09}, {0x1014, 0x05}, {0x104C, 0x08}, {0x1050, 0x08},
    {0x10D8, 0x0F}, {0x1118, 0x1C}, {0x10F8, 0x07}, {0x11F8, 0x08}, {0x0E84, 0x15}, {0x0E90, 0x3F},
    {0x0EE4, 0x02}, {0x0E40, 0x06}, {0x0E3C, 0x18}, {0x02DC, 0x05}, {0x0388, 0x77}, {0x0398, 0x0B},
    {0x03E0, 0x0F}, {0x060C, 0x1D}, {0x0614, 0x07}, {0x0620, 0xC1}, {0x0694, 0x00}, {0x03D0, 0x8C},
    {0x1424, 0x01}, {0x1428, 0x01}, {0x0200, 0x00}, {0x0244, 0x03},
};
STATIC BOOLEAN Read (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 *V)
{
  return Io->Read32 (Io->Context, A, V) && *V != MAX_UINT32;
}
STATIC BOOLEAN Update (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 Mask, UINT32 Bits)
{
  UINT32 V;
  return Read (Io, A, &V) && Io->Write32 (Io->Context, A, (V & ~Mask) | (Bits & Mask)) &&
         Read (Io, A, &V) && (V & Mask) == (Bits & Mask);
}
STATIC EFI_STATUS Wait (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 Mask, UINT32 Bits)
{
  UINT32 V;
  for (UINTN I = 0; I < 10000; ++I)
  {
    if (!Read (Io, A, &V))
      return EFI_DEVICE_ERROR;
    if ((V & Mask) == Bits)
      return EFI_SUCCESS;
    Io->DelayUs (Io->Context, 1);
  }
  return EFI_TIMEOUT;
}
STATIC BOOLEAN Latch (CONST LIUQIN_PCIE_IO *Io)
{
  UINT32 V;
  return Read (Io, RCG, &V) && !(V & 1U) && Io->Write32 (Io->Context, RCG, V | 1U) &&
         !EFI_ERROR (Wait (Io, RCG, 1, 0));
}
STATIC UINTN Pin (UINTN I)
{
  return 0x0F15E000U + I * 0x1000U;
}

EFI_STATUS LiuqinPcieInitializePhy (CONST LIUQIN_PCIE_IO *Io, LIUQIN_PCIE_PHY_RESULT *Result,
                                    LIUQIN_PCIE_SCOPED_WORK Work, VOID *Context)
{
  LIUQIN_PCIE_PHY_RESULT R = {0};
  UINT32 Pins[3][2], Core, Reset, Rcg, Config, Votes, Ref, Pipe, V;
  BOOLEAN Dirty = FALSE, PhyDirty = FALSE, Clean = TRUE;
  BOOLEAN ClockDirty = FALSE, RcgDirty = FALSE, RefDirty = FALSE, ResetDirty = FALSE;
  UINTN PinCount = 0;
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  if (!Io || !Io->Read32 || !Io->Write32 || !Io->DelayUs || !Result)
    return EFI_INVALID_PARAMETER;
  R.Stage = 1;
  if (!Read (Io, PARF + 0x1B0, &V))
    goto End;
  if (V != 0)
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Read (Io, PARF, &V))
    goto End;
  if ((V & (1U << 26)) || (V & (1U << 29)))
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Read (Io, PHY + 0x240, &V))
    goto End;
  if (V != 0)
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Read (Io, PHY + 0x200, &V))
    goto End;
  if (V != 1)
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Read (Io, PHY + 0x244, &V))
    goto End;
  if (V != 0)
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  if (!Read (Io, CORE_BCR, &Core) || !Read (Io, PHY_BCR, &Reset) || !Read (Io, RCG, &Rcg) ||
      !Read (Io, CFG, &Config) || !Read (Io, VOTE, &Votes) || !Read (Io, CLKREF, &Ref) ||
      !Read (Io, PIPE, &Pipe))
    goto End;
  // An inherited RCHNG vote is retained only with the observed XO source;
  // never reprogram an inherited running root or revoke its branch vote.
  if ((Core & 1U) || (Reset & 1U) || (Rcg & 1U) ||
      ((Votes & RCHNG) ? ((Config & MASK & ~1U) != (1U << 20)) : (Rcg & 2U)) || (Pipe & 3U) != 2 ||
      (!(Ref & 1U) && !(Ref & (1U << 31))))
  {
    Status = EFI_UNSUPPORTED;
    goto End;
  }
  for (UINTN I = 0; I < 3; ++I)
  {
    if (!Read (Io, Pin (I), &Pins[I][0]) || !Read (Io, Pin (I) + 4, &Pins[I][1]) ||
        !Read (Io, Pin (I) + 8, &V))
      goto End;
    R.PinBefore[I] = Pins[I][0];
    // No active pin interrupt, external-GPIO owner or unknown function.
    if ((V & 1U) || (Pins[I][0] & (1U << 12)) || ((Pins[I][0] >> 2) & 15U) > (I == 1 ? 1U : 0U) ||
        (I != 1 && (Pins[I][0] & (1U << 9)) && (Pins[I][1] & 2U)))
    {
      Status = EFI_UNSUPPORTED;
      goto End;
    }
  }
  R.Stage = 2;
  Dirty = TRUE;
  for (UINTN I = 0; I < 3; ++I)
  {
    PinCount = I + 1; // failed write can have posted
    // PERST: preload low before enabling GPIO output. Other pins are inputs.
    if (I == 0 && !Update (Io, Pin (I) + 4, 2, 0))
      goto Restore;
    UINT32 Bits = I == 0 ? 0x201U : I == 1 ? 7U : 3U;
    if (!Update (Io, Pin (I), 0x3FFU, Bits) || !Read (Io, Pin (I), &R.PinDuring[I]))
      goto Restore;
  }
  Io->DelayUs (Io->Context, 1000);
  R.Stage = 3;
  RefDirty = !(Ref & 1U);
  if (!Update (Io, CLKREF, 1, 1))
    goto Restore;
  Status = Wait (Io, CLKREF, 1U << 31, 0);
  if (EFI_ERROR (Status))
    goto Restore;
  Status = EFI_DEVICE_ERROR;
  if (!(Votes & RCHNG))
  {
    RcgDirty = TRUE;
    if (!Update (Io, RCG, 2, 2) || EFI_ERROR (Wait (Io, RCG, 1U << 31, 0)) ||
        !Update (Io, CFG, MASK, (1U << 20) | 1U) || !Latch (Io))
      goto Restore;
    ClockDirty = TRUE;
    if (!Update (Io, VOTE, RCHNG, RCHNG))
      goto Restore;
  }
  Status = Wait (Io, 0x0017B044U, 1U << 31, 0);
  if (EFI_ERROR (Status))
    goto Restore;
  Status = EFI_DEVICE_ERROR;
  R.Stage = 4;
  // Core is already out of reset and its DBI identity was verified by the
  // caller. This PHY-only step pulses PHY BCR for >=1ms; full cold-host
  // initialization will reset/reconfigure the core before training LTSSM.
  ResetDirty = TRUE;
  if (!Update (Io, PHY_BCR, 1, 1))
    goto Restore;
  Io->DelayUs (Io->Context, 1000);
  if (!Update (Io, PHY_BCR, 1, 0))
    goto Restore;
  R.Stage = 5;
  PhyDirty = TRUE;
  for (UINTN I = 0; I < ARRAY_SIZE (mPhy); ++I)
  {
    ++R.Writes;
    // PHY sequence values are register data, not masks. Do not demand
    // equality from self-clearing start bits; hardware readiness is polled.
    if (!Io->Write32 (Io->Context, PHY + mPhy[I].Offset, mPhy[I].Value))
      goto Restore;
  }
  Io->DelayUs (Io->Context, 1000);
  R.Stage = 6;
  // Stop AP PIPE vote before switching away from XO, then enable PHY PIPE.
  if (!Update (Io, CLOCK, 1U << 4, 0) || !Update (Io, PIPE, 3, 0) ||
      !Update (Io, CLOCK, 1U << 4, 1U << 4))
    goto Restore;
  Status = Wait (Io, PHY + 0x214, 1U << 6, 0);
  R.Stage = 7;
  if (!Read (Io, PHY + 0x214, &R.Status) || !Read (Io, PHY + 0x240, &R.Power) ||
      !Read (Io, PHY + 0x200, &R.Reset) || !Read (Io, PHY + 0x244, &R.Start) ||
      !Read (Io, PIPE, &R.Pipe) || !Read (Io, 0x0017B044U, &R.Clock))
    Status = EFI_DEVICE_ERROR;
  if (!EFI_ERROR (Status) && Work)
  {
    Status = Work (Io, Context);
    if (Status == EFI_ABORTED)
      goto End; // nested traffic may remain; keep PHY on
  }
  if (Status == EFI_SUCCESS && Work && Io->RetainForOs && *Io->RetainForOs)
  {
    R.Retained = TRUE;
    goto End;
  }
Restore:
  // Quiesce the newly initialized PHY before disabling its clocks. Tuning
  // registers remain programmed; they are not active after reset/powerdown.
  if (PhyDirty)
  {
    if (!Io->Write32 (Io->Context, PHY + 0x244, 0) || EFI_ERROR (Wait (Io, PHY + 0x244, 3, 0)) ||
        !Update (Io, PHY + 0x200, 1, 1) || !Update (Io, PHY + 0x240, 3, 0))
    {
      Status = EFI_ABORTED;
      goto End;
    }
    if (!Update (Io, CLOCK, 1U << 4, 0) || !Update (Io, PIPE, 3, Pipe) ||
        !Update (Io, CLOCK, 1U << 4, 1U << 4))
    {
      Status = EFI_ABORTED;
      goto End;
    }
  }
  if (ResetDirty && !Update (Io, PHY_BCR, 1, Reset))
    Clean = FALSE;
  if (ClockDirty && !Update (Io, VOTE, RCHNG, Votes))
    Clean = FALSE;
  if (RcgDirty && (!Update (Io, CFG, MASK, Config) || !Latch (Io) || !Update (Io, RCG, 2, Rcg)))
    Clean = FALSE;
  if (RefDirty &&
      (!Update (Io, CLKREF, 1, Ref) || EFI_ERROR (Wait (Io, CLKREF, 1U << 31, 1U << 31))))
    Clean = FALSE;
  // Restore pin configuration only after no PHY clock/reset remains uncertain.
  if (!Clean)
  {
    Status = EFI_ABORTED;
    goto End;
  }
  while (PinCount != 0)
  {
    --PinCount;
    if (!Update (Io, Pin (PinCount), 0x3FFU, Pins[PinCount][0]) ||
        (PinCount == 0 && !Update (Io, Pin (0) + 4, 2, Pins[0][1])))
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
