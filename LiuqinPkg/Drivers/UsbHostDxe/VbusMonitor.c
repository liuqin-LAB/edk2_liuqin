/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "VbusMonitor.h"

STATIC VOID ClearBytes (VOID *Buffer, UINTN Size)
{
  UINTN Index;
  volatile UINT8 *Bytes = Buffer;
  for (Index = 0; Index < Size; ++Index)
    Bytes[Index] = 0;
}

VOID LiuqinUsbDecodeCc (CONST LIUQIN_USB_VBUS_SNAPSHOT *Snapshot,
                        LIUQIN_USB_CC_OBSERVATION *Observation)
{
  UINT32 Index;
  UINT8 Orientation, Ccout;
  CONST UINT8 *Typec, *Dcdc;
  if (Observation == NULL)
    return;
  ClearBytes (Observation, sizeof (*Observation));
  Observation->State = LiuqinCcUnavailable;
  if (Snapshot == NULL || !Snapshot->Complete || !Snapshot->IdentityValid ||
      Snapshot->IdentityRejected || Snapshot->Attempted != LIUQIN_USB_VBUS_READS)
    return;
  for (Index = 0; Index < LIUQIN_USB_VBUS_READS; ++Index)
    if (!Snapshot->Reads[Index].Valid || Snapshot->Reads[Index].Status != LiuqinSpmiReadComplete)
      return;
  Observation->State = LiuqinCcUnsupported;
  /* Decoder is deliberately v2-only. V1 remains a raw V1 snapshot. */
  if (Snapshot->Reads[0].Data[1] != 2U || Snapshot->Reads[0].Data[2] != 0x51U ||
      Snapshot->Reads[0].Data[3] != 0x32U || Snapshot->Reads[1].Data[0] != 2U ||
      Snapshot->Reads[1].Data[1] != 0x80U || Snapshot->Reads[2].Data[0] != 2U ||
      Snapshot->Reads[2].Data[1] != 0x81U || Snapshot->Reads[3].Data[0] != 2U ||
      Snapshot->Reads[3].Data[1] != 0x85U)
    return;
  Typec = Snapshot->Reads[9].Data;
  Dcdc = Snapshot->Reads[5].Data;
  Observation->State = LiuqinCcUnknown;
  /* Undefined bits are reported as unknown, never normalized away. */
  if ((Typec[0] & 0x80U) != 0U || (Typec[2] & 0xC0U) != 0U || (Dcdc[2] & 0x80U) != 0U ||
      (Dcdc[3] & 0xE0U) != 0U || (Dcdc[4] & 0xE0U) != 0U ||
      (Snapshot->Reads[6].Data[0] & ~0x1BU) != 0U || (Snapshot->Reads[7].Data[0] & ~0x73U) != 0U)
    return;
  Observation->Valid = TRUE;
  Observation->StateMachine = Typec[3] & 0x1FU;
  Observation->Attached = (Typec[3] & 0x20U) != 0U;
  Observation->Debounced = (Typec[5] & 8U) != 0U;
  Observation->SourceMode = (Typec[5] & 0x40U) != 0U;
  Observation->VbusDetected = (Typec[5] & 0x20U) != 0U;
  Observation->WaterDetected = (Typec[5] & 0x80U) != 0U;
  Observation->BoostUsb = (Dcdc[2] & 0x10U) != 0U;
  Observation->BoostCommand = (Snapshot->Reads[6].Data[0] & 1U) != 0U;
  Observation->TypecEnableSource = (Snapshot->Reads[7].Data[0] & 1U) != 0U;
  Observation->Fault = Dcdc[3];
  Observation->Fail = Dcdc[4];
  Observation->Mode = Snapshot->Reads[8].Data[0];
  Observation->Rp = Typec[0];
  Observation->Partner = Typec[2];
  Orientation = (Typec[3] >> 6) & 3U;
  Ccout = (Typec[5] & 1U) != 0U ? (((Typec[5] >> 1) & 1U) + 1U) : 0U;
  Observation->Orientation = Orientation == 1U ? 1U : (Orientation == 2U ? 2U : 0U);

  if ((Typec[5] & 0x14U) != 0U || Observation->Fault != 0U || Observation->Fail != 0U)
  {
    Observation->State = LiuqinCcFault;
  }
  else if (Orientation == 3U ||
           (Ccout != 0U && Observation->Orientation != 0U && Ccout != Observation->Orientation) ||
           (Observation->Mode & 6U) == 6U ||
           (Observation->Attached && (Observation->Mode & 1U) != 0U) ||
           (Observation->StateMachine == 3U && (Observation->Mode & 4U) != 0U) ||
           (Observation->StateMachine == 6U && (Observation->Mode & 2U) != 0U))
  {
    Observation->State = LiuqinCcInconsistent;
  }
  else if (Observation->StateMachine == 12U || Observation->StateMachine == 13U ||
           Observation->StateMachine == 15U)
  {
    Observation->State = LiuqinCcAccessory;
  }
  else if (Observation->StateMachine == 1U || Observation->StateMachine == 4U ||
           Observation->StateMachine == 0U)
  {
    Observation->State = Observation->Attached ? LiuqinCcInconsistent : LiuqinCcDetached;
  }
  else if (Observation->StateMachine == 3U || Observation->StateMachine == 6U)
  {
    if (!Observation->Attached || !Observation->Debounced || Observation->Orientation == 0U ||
        Ccout == 0U || Observation->SourceMode != (Observation->StateMachine == 6U))
    {
      Observation->State = LiuqinCcInconsistent;
    }
    else if (Observation->StateMachine == 3U)
    {
      /* Ordinary Rp is one of STD/1.5/3.0, not SHORT or debug-accessory Rp.
       * This is an advertisement, not an available-current measurement. */
      Observation->State =
          (Typec[0] == 8U || Typec[0] == 4U || Typec[0] == 2U) && Typec[1] == 0U && Typec[2] == 0U
              ? LiuqinCcAttachedSink
              : LiuqinCcInconsistent;
    }
    else
    {
      Observation->State =
          Typec[0] == 0U && Typec[1] == 0U && ((Typec[2] & 0x1FU) == 8U || (Typec[2] & 0x1FU) == 4U)
              ? LiuqinCcAttachedSource
              : LiuqinCcAccessory;
    }
  }
  else if ((Observation->StateMachine >= 2U && Observation->StateMachine <= 11U) ||
           (Observation->StateMachine >= 16U && Observation->StateMachine <= 18U))
  {
    Observation->State = LiuqinCcTransition;
  }
}
