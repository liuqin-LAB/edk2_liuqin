/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "VbusSnapshot.h"

STATIC BOOLEAN IdentityMatches (UINT32 Index, CONST LIUQIN_SPMI_READ_RESULT *Read)
{
  STATIC CONST UINT8 Subtypes[] = {0x80, 0x81, 0x85};
  if (Index == 0U)
  {
    return (Read->Data[1] == 1U || Read->Data[1] == 2U) && Read->Data[2] == 0x51U &&
           Read->Data[3] == 0x32U;
  }
  return Index <= ARRAY_SIZE (Subtypes) && Read->Data[0] == 0x02U &&
         Read->Data[1] == Subtypes[Index - 1U];
}

UINT16 LiuqinUsbVbusAddress (UINT32 Index)
{
  STATIC CONST UINT16 Addresses[LIUQIN_USB_VBUS_READS] = {0x0102, 0x2604, 0x2704, 0x2B04, 0x2606,
                                                          0x270B, 0x2750, 0x2756, 0x2B44, 0x2B06};
  return Index < LIUQIN_USB_VBUS_READS ? Addresses[Index] : 0;
}

UINT8 LiuqinUsbVbusLength (UINT32 Index)
{
  STATIC CONST UINT8 Lengths[LIUQIN_USB_VBUS_READS] = {4, 2, 2, 2, 2, 5, 1, 1, 1, 6};
  return Index < LIUQIN_USB_VBUS_READS ? Lengths[Index] : 0;
}

VOID LiuqinUsbCollectVbusSnapshotControlled (CONST LIUQIN_SPMI_READ_IO *Io,
                                             CONST LIUQIN_SPMI_READ_CONFIG *Config,
                                             LIUQIN_USB_VBUS_SNAPSHOT *Snapshot,
                                             LIUQIN_USB_VBUS_CONTINUE Continue, VOID *Context)
{
  UINTN Byte;
  UINT32 Index;
  volatile UINT8 *Bytes;
  if (Snapshot == NULL)
    return;
  Bytes = (volatile UINT8 *)Snapshot;
  for (Byte = 0; Byte < sizeof (*Snapshot); ++Byte)
    Bytes[Byte] = 0;
  for (Index = 0; Index < LIUQIN_USB_VBUS_READS; ++Index)
  {
    if (Continue != NULL && !Continue (Context))
      return;
    ++Snapshot->Attempted;
    LiuqinSpmiRead (Io, Config, LIUQIN_USB_VBUS_CANDIDATE_SLAVE, LiuqinUsbVbusAddress (Index),
                    LiuqinUsbVbusLength (Index), &Snapshot->Reads[Index]);
    if (!Snapshot->Reads[Index].Valid)
      return;
    if (Index < 4U && !IdentityMatches (Index, &Snapshot->Reads[Index]))
    {
      Snapshot->IdentityRejected = TRUE;
      return;
    }
    if (Index == 3U)
      Snapshot->IdentityValid = TRUE;
  }
  Snapshot->Complete = TRUE;
}

VOID LiuqinUsbCollectVbusSnapshot (CONST LIUQIN_SPMI_READ_IO *Io,
                                   CONST LIUQIN_SPMI_READ_CONFIG *Config,
                                   LIUQIN_USB_VBUS_SNAPSHOT *Snapshot)
{
  LiuqinUsbCollectVbusSnapshotControlled (Io, Config, Snapshot, NULL, NULL);
}
