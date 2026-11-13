/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AcpiWlan.h"
STATIC CONST UINTN Pins[] = {80, 81, 82, 204, 94};
STATIC UINTN Pin (UINTN I)
{
  return 0x0F100000U + Pins[I] * 0x1000U;
}
STATIC BOOLEAN Rd (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 *V)
{
  return Io->Read32 (Io->Context, A, V) && *V != MAX_UINT32;
}
STATIC BOOLEAN Up (CONST LIUQIN_PCIE_IO *Io, UINTN A, UINT32 Mask, UINT32 Bits)
{
  UINT32 V;
  return Rd (Io, A, &V) && Io->Write32 (Io->Context, A, (V & ~Mask) | (Bits & Mask)) &&
         Rd (Io, A, &V) && (V & Mask) == (Bits & Mask);
}
STATIC EFI_STATUS Check (CONST LIUQIN_PCIE_IO *Io, LIUQIN_WLAN_RESULT *R, UINT32 Saved[5][2])
{
  UINT32 V;
  R->Stage = 1;
  for (UINTN I = 0; I < 5; ++I)
  {
    if (!Rd (Io, Pin (I), &Saved[I][0]) || !Rd (Io, Pin (I) + 4, &Saved[I][1]) ||
        !Rd (Io, Pin (I) + 8, &V))
      return EFI_DEVICE_ERROR;
    R->Before[I] = Saved[I][0];
    R->InputBefore[I] = Saved[I][1];
    R->ReadMask |= 1U << I;
    if ((V & 1U) || (Saved[I][0] & (1U << 12)) || (Saved[I][0] & 0x3CU))
      return EFI_UNSUPPORTED;
    // Reject a pre-enabled WLAN, externally driven XO, or released EP.
    if ((I == 0 || I == 3 || I == 4) &&
        ((Saved[I][0] & (1U << 9)) ? (Saved[I][1] & 2U) : (Saved[I][1] & 1U)))
      return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}
EFI_STATUS LiuqinWlanPreflight (CONST LIUQIN_PCIE_IO *Io, LIUQIN_WLAN_RESULT *Result)
{
  UINT32 Saved[5][2];
  LIUQIN_WLAN_RESULT R = {0};
  EFI_STATUS Status;
  if (!Io || !Io->Read32 || !Result)
    return EFI_INVALID_PARAMETER;
  Status = Check (Io, &R, Saved);
  R.Quiesced = TRUE;
  *Result = R;
  return Status;
}
EFI_STATUS LiuqinWlanEnable (CONST LIUQIN_PCIE_IO *Io, LIUQIN_WLAN_RESULT *Result,
                             LIUQIN_PCIE_SCOPED_WORK Work, VOID *Context)
{
  LIUQIN_WLAN_RESULT R = {0};
  UINT32 Saved[5][2], V, Bt;
  BOOLEAN Dirty = FALSE, Clean = TRUE;
  EFI_STATUS Status;
  if (!Io || !Io->Read32 || !Io->Write32 || !Io->DelayUs || !Result)
    return EFI_INVALID_PARAMETER;
  Status = Check (Io, &R, Saved);
  if (EFI_ERROR (Status))
    goto End;
  Status = EFI_DEVICE_ERROR;
  R.Stage = 2;
  Dirty = TRUE;
  // PERST low before any endpoint enable; do not change BT or SW_CTRL.
  if (!Up (Io, Pin (4) + 4, 2, 0) || !Up (Io, Pin (4), 0x3FF, 0x201) ||
      !Up (Io, Pin (0) + 4, 2, 0) || !Up (Io, Pin (0), 0x3FF, 0x3C3))
    goto Restore;
  // CNSS requires >=100ms after BT_EN falls before WLAN_EN rises.
  Bt = Saved[1][1] & 1U;
  Io->DelayUs (Io->Context, 100000);
  if (!Rd (Io, Pin (1) + 4, &V))
    goto Restore;
  if ((V & 1U) != Bt)
  {
    Status = EFI_NOT_READY;
    goto Restore;
  }
  R.Stage = 3;
  // gpio_direction_output preserves GPIO204 pull/drive fields.
  if (!Up (Io, Pin (3) + 4, 2, 2) || !Up (Io, Pin (3), 1U << 9, 1U << 9))
    goto Restore;
  Io->DelayUs (Io->Context, 100);
  R.EnableMayHaveApplied = TRUE;
  if (!Up (Io, Pin (0) + 4, 2, 2))
    goto Restore;
  Io->DelayUs (Io->Context, 1000);
  // Stock keeps XO asserted another 2..5ms for the hardware latch.
  Io->DelayUs (Io->Context, 2000);
  if (!Up (Io, Pin (3) + 4, 2, 0))
    goto Restore;
  Io->DelayUs (Io->Context, 5000);
  R.Stage = 4;
  for (UINTN I = 0; I < 5; ++I)
  {
    if (!Rd (Io, Pin (I), &R.During[I]) || !Rd (Io, Pin (I) + 4, &R.InputDuring[I]))
      goto Restore;
  }
  // Status is control-sequence success only, not voltage or SW_CTRL readiness.
  Status = EFI_SUCCESS;
  if (Work)
  {
    Status = Work (Io, Context);
    if (Status == EFI_ABORTED)
      goto End; // nested link cleanup failed; keep power
  }
  if (Status == EFI_SUCCESS && Work && Io->RetainForOs && *Io->RetainForOs)
  {
    R.Retained = TRUE;
    goto End;
  }
Restore:
  // A failed callback may have posted. First force WLAN/XO/PERST low;
  // if uncertain, retain pin configuration and request reset instead.
  // Even an enable write that reported failure may have reached hardware.
  if (R.EnableMayHaveApplied)
    Io->DelayUs (Io->Context, 3000);
  if (!Up (Io, Pin (0) + 4, 2, 0) || !Up (Io, Pin (3) + 4, 2, 0) || !Up (Io, Pin (4) + 4, 2, 0))
  {
    Status = EFI_ABORTED;
    goto End;
  }
  Io->DelayUs (Io->Context, 5000);
  for (UINTN J = 0; J < 3; ++J)
  {
    UINTN I = J == 0 ? 3U : J == 1 ? 0U : 4U;
    if (!Up (Io, Pin (I), 0x3FF, Saved[I][0]) || !Up (Io, Pin (I) + 4, 2, Saved[I][1]))
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
