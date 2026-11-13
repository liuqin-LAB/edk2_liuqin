/* Based on https://github.com/SamuelTulach/rainbow
 * Copyright (c) 2021 Samuel Tulach
 * Copyright (c) 2022-2023 DuoWoA authors
 * SPDX-License-Identifier: MIT */
#include "KernelErrataPatcherLib.h"

EFI_EVENT mReadyToBootEvent;

VOID EFIAPI ReadyToBootHandler (IN EFI_EVENT Event, IN VOID *Context)
{
  PERF_CALLBACK_BEGIN (&gEfiEventReadyToBootGuid);

  InstallEFIBSIntercepter ();
  gBS->CloseEvent (mReadyToBootEvent);

  PERF_CALLBACK_END (&gEfiEventReadyToBootGuid);
}

EFI_STATUS
EFIAPI
KernelErrataPatcherLibConstructor (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ReadyToBootHandler, NULL,
                               &gEfiEventReadyToBootGuid, &mReadyToBootEvent);
  if (EFI_ERROR (Status))
  {

    ASSERT (FALSE);
  }

  return InitMemoryAttributeProtocol ();
}