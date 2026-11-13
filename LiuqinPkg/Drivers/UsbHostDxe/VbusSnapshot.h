/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_USB_VBUS_SNAPSHOT_H
#define LIUQIN_USB_VBUS_SNAPSHOT_H

#include <SpmiRead.h>

#define LIUQIN_USB_VBUS_READS 10U
#define LIUQIN_USB_VBUS_CANDIDATE_SLAVE 3U

typedef struct
{
  LIUQIN_SPMI_READ_RESULT Reads[LIUQIN_USB_VBUS_READS];
  UINT32 Attempted;
  BOOLEAN Complete;
  BOOLEAN IdentityValid;
  BOOLEAN IdentityRejected;
} LIUQIN_USB_VBUS_SNAPSHOT;

typedef BOOLEAN (*LIUQIN_USB_VBUS_CONTINUE) (VOID *Context);

VOID LiuqinUsbCollectVbusSnapshot (CONST LIUQIN_SPMI_READ_IO *Io,
                                   CONST LIUQIN_SPMI_READ_CONFIG *Config,
                                   LIUQIN_USB_VBUS_SNAPSHOT *Snapshot);
/* Cancellation is checked between transactions. The current transaction
 * may finish within SpmiRead's bound, even if its command has not yet been
 * issued. An adapter may refuse subsequent accesses after cancellation.
 * There is no reset or retry. This does not control or turn off power. */
VOID LiuqinUsbCollectVbusSnapshotControlled (CONST LIUQIN_SPMI_READ_IO *Io,
                                             CONST LIUQIN_SPMI_READ_CONFIG *Config,
                                             LIUQIN_USB_VBUS_SNAPSHOT *Snapshot,
                                             LIUQIN_USB_VBUS_CONTINUE Continue, VOID *Context);
UINT16 LiuqinUsbVbusAddress (UINT32 Index);
UINT8 LiuqinUsbVbusLength (UINT32 Index);

#endif
