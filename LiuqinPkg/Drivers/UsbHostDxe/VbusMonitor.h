/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_USB_VBUS_MONITOR_H
#define LIUQIN_USB_VBUS_MONITOR_H
#include "VbusSnapshot.h"

typedef enum
{
  LiuqinCcUnavailable,
  LiuqinCcUnsupported,
  LiuqinCcUnknown,
  LiuqinCcDetached,
  LiuqinCcTransition,
  LiuqinCcAttachedSink,
  LiuqinCcAttachedSource,
  LiuqinCcAccessory,
  LiuqinCcFault,
  LiuqinCcInconsistent
} LIUQIN_USB_CC_STATE;

typedef struct
{
  BOOLEAN Valid;
  LIUQIN_USB_CC_STATE State;
  UINT8 StateMachine;
  UINT8 Orientation; /* 0 unknown, 1 CC1, 2 CC2 */
  BOOLEAN Attached, Debounced, SourceMode, VbusDetected;
  BOOLEAN BoostUsb, BoostCommand, TypecEnableSource;
  UINT8 Fault, Fail, Mode, Rp, Partner;

  BOOLEAN WaterDetected;
  /* No source-grant field: this is an observation, never a write policy. */
} LIUQIN_USB_CC_OBSERVATION;

VOID LiuqinUsbDecodeCc (CONST LIUQIN_USB_VBUS_SNAPSHOT *Snapshot,
                        LIUQIN_USB_CC_OBSERVATION *Observation);
#endif
