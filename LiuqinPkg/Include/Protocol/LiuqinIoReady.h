/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_IO_READY_H
#define LIUQIN_IO_READY_H

#include <Uefi.h>

#define LIUQIN_IO_READY_VERSION 1

typedef struct
{
  UINT32 Version;
  UINT64 MmioBase;
  UINT64 MmioSize;
  UINT32 Gsiv;
  BOOLEAN DmaReady;
  BOOLEAN ControllerReady;
} LIUQIN_IO_READY_PROTOCOL;

extern EFI_GUID gLiuqinUfsReadyProtocolGuid;
extern EFI_GUID gLiuqinUsbReadyProtocolGuid;

#endif
