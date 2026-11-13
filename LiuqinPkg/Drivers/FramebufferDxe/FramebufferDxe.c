/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#include <PiDxe.h>

#include <Guid/LiuqinHandoff.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

STATIC EFI_STATUS EFIAPI QueryMode (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber,
                                    UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
STATIC EFI_STATUS EFIAPI SetMode (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber);
STATIC EFI_STATUS EFIAPI Blt (EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                              EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
                              EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation, UINTN SourceX,
                              UINTN SourceY, UINTN DestinationX, UINTN DestinationY, UINTN Width,
                              UINTN Height, UINTN Delta);

typedef struct
{
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} LIUQIN_FRAMEBUFFER_DEVICE_PATH;

STATIC CONST LIUQIN_FRAMEBUFFER_DEVICE_PATH mDevicePath = {
    {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof (VENDOR_DEVICE_PATH), 0}},
     {0x39da113f, 0x1799, 0x433b, {0xbd, 0x35, 0x72, 0x5d, 0xd0, 0x6a, 0x79, 0x6f}}},
    {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}}};

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mMode;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mGraphicsOutput = {QueryMode, SetMode, Blt, &mMode};
STATIC FRAME_BUFFER_CONFIGURE *mBltConfigure;
STATIC EFI_HANDLE mHandle;
STATIC
EFI_STATUS
EFIAPI
QueryMode (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber, UINTN *SizeOfInfo,
           EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  if ((This != &mGraphicsOutput) || (ModeNumber != 0) || (SizeOfInfo == NULL) || (Info == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocateCopyPool (sizeof (mInfo), &mInfo);
  if (*Info == NULL)
  {
    *SizeOfInfo = 0;
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (mInfo);
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
VideoRectangleFits (UINTN X, UINTN Y, UINTN Width, UINTN Height)
{
  // Subtract first: caller-supplied coordinates must not wrap on addition.
  return (X <= mInfo.HorizontalResolution) && (Y <= mInfo.VerticalResolution) &&
         (Width <= mInfo.HorizontalResolution - X) && (Height <= mInfo.VerticalResolution - Y);
}

STATIC
BOOLEAN
BufferRectangleFits (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer, UINTN X, UINTN Y, UINTN Width,
                     UINTN Height, UINTN Delta)
{
  UINTN RowBytes;
  UINTN LastRow;
  UINTN EndOffset;

  if ((X > MAX_UINTN - Width) || (X + Width > MAX_UINTN / sizeof (*Buffer)))
  {
    return FALSE;
  }

  RowBytes = (X + Width) * sizeof (*Buffer);
  if (Delta == 0)
  {
    Delta = Width * sizeof (*Buffer);
  }

  if ((RowBytes > Delta) || (Y > MAX_UINTN - (Height - 1)))
  {
    return FALSE;
  }

  LastRow = Y + Height - 1;
  if (LastRow > (MAX_UINTN - RowBytes) / Delta)
  {
    return FALSE;
  }

  EndOffset = LastRow * Delta + RowBytes;
  return (UINTN)Buffer <= MAX_UINTN - EndOffset;
}

STATIC
EFI_STATUS
EFIAPI
Blt (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
     EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation, UINTN SourceX, UINTN SourceY, UINTN DestinationX,
     UINTN DestinationY, UINTN Width, UINTN Height, UINTN Delta)
{
  EFI_TPL OldTpl;
  RETURN_STATUS Status;

  if ((This != &mGraphicsOutput) || (mBltConfigure == NULL) || (Width == 0) || (Height == 0) ||
      (Operation >= EfiGraphicsOutputBltOperationMax))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((Operation != EfiBltVideoToVideo) && (BltBuffer == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (((Operation == EfiBltVideoToBltBuffer) || (Operation == EfiBltVideoToVideo)) &&
      !VideoRectangleFits (SourceX, SourceY, Width, Height))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((Operation != EfiBltVideoToBltBuffer) &&
      !VideoRectangleFits (DestinationX, DestinationY, Width, Height))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((Operation == EfiBltVideoToBltBuffer) &&
      !BufferRectangleFits (BltBuffer, DestinationX, DestinationY, Width, Height, Delta))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((Operation == EfiBltBufferToVideo) &&
      !BufferRectangleFits (BltBuffer, SourceX, SourceY, Width, Height, Delta))
  {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = FrameBufferBlt (mBltConfigure, BltBuffer, Operation, SourceX, SourceY, DestinationX,
                           DestinationY, Width, Height, Delta);
  gBS->RestoreTPL (OldTpl);
  return (EFI_STATUS)Status;
}

STATIC
EFI_STATUS
EFIAPI
SetMode (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black;

  if (This != &mGraphicsOutput)
  {
    return EFI_INVALID_PARAMETER;
  }

  if (ModeNumber != 0)
  {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Black, sizeof (Black));
  // Only clear the visible framebuffer. Scanout timing remains inherited.
  return Blt (This, &Black, EfiBltVideoFill, 0, 0, 0, 0, mInfo.HorizontalResolution,
              mInfo.VerticalResolution, 0);
}

EFI_STATUS
EFIAPI
FramebufferDxeEntryPoint (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  RETURN_STATUS BltStatus;
  VOID *GuidHob;
  LIUQIN_HANDOFF *Handoff;
  UINT64 ApertureBase;
  UINT64 ApertureSize;
  UINT64 VisibleSize;
  UINTN ConfigureSize;

  (VOID) ImageHandle;
  (VOID) SystemTable;
  if (mHandle != NULL)
  {
    return EFI_ALREADY_STARTED;
  }

  GuidHob = GetFirstGuidHob (&gLiuqinHandoffGuid);
  if ((GuidHob == NULL) || (GET_GUID_HOB_DATA_SIZE (GuidHob) < sizeof (*Handoff)))
  {
    return EFI_NOT_FOUND;
  }

  Handoff = GET_GUID_HOB_DATA (GuidHob);
  if ((Handoff->Version != LIUQIN_HANDOFF_VERSION) ||
      ((Handoff->FramebufferSource != LIUQIN_FB_FROM_DTB) &&
       (Handoff->FramebufferSource != LIUQIN_FB_FROM_REFERENCE)))
  {
    return EFI_NOT_FOUND;
  }

  ApertureBase = FixedPcdGet64 (PcdFramebufferBase);
  ApertureSize = FixedPcdGet32 (PcdFramebufferSize);
  VisibleSize = (UINT64)Handoff->Stride * Handoff->Height;
  if ((Handoff->Width == 0) || (Handoff->Height == 0) ||
      (Handoff->Width > MAX_UINT32 / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) ||
      (Handoff->Stride % sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL) != 0) ||
      (Handoff->Stride / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL) < Handoff->Width) ||
      (Handoff->FramebufferBase % sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL) != 0) ||
      (Handoff->FramebufferBase < ApertureBase) || (Handoff->FramebufferSize == 0) ||
      (Handoff->FramebufferSize > ApertureSize) ||
      (Handoff->FramebufferBase - ApertureBase > ApertureSize - Handoff->FramebufferSize) ||
      (VisibleSize > Handoff->FramebufferSize) ||
      (Handoff->FramebufferBase > MAX_UINTN - Handoff->FramebufferSize))
  {

    return EFI_UNSUPPORTED;
  }

  ZeroMem (&mInfo, sizeof (mInfo));
  mInfo.HorizontalResolution = Handoff->Width;
  mInfo.VerticalResolution = Handoff->Height;
  mInfo.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  mInfo.PixelsPerScanLine = Handoff->Stride / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  ConfigureSize = 0;
  BltStatus = FrameBufferBltConfigure ((VOID *)(UINTN)Handoff->FramebufferBase, &mInfo, NULL,
                                       &ConfigureSize);
  if (BltStatus != RETURN_BUFFER_TOO_SMALL)
  {
    return EFI_UNSUPPORTED;
  }

  mBltConfigure = AllocatePool (ConfigureSize);
  if (mBltConfigure == NULL)
  {
    return EFI_OUT_OF_RESOURCES;
  }

  BltStatus = FrameBufferBltConfigure ((VOID *)(UINTN)Handoff->FramebufferBase, &mInfo,
                                       mBltConfigure, &ConfigureSize);
  if (RETURN_ERROR (BltStatus))
  {
    FreePool (mBltConfigure);
    mBltConfigure = NULL;
    return (EFI_STATUS)BltStatus;
  }

  mMode.MaxMode = 1;
  mMode.Mode = 0;
  mMode.Info = &mInfo;
  mMode.SizeOfInfo = sizeof (mInfo);
  mMode.FrameBufferBase = Handoff->FramebufferBase;
  mMode.FrameBufferSize = (UINTN)Handoff->FramebufferSize;
  Status = gBS->InstallMultipleProtocolInterfaces (
      &mHandle, &gEfiDevicePathProtocolGuid, (VOID *)&mDevicePath, &gEfiGraphicsOutputProtocolGuid,
      &mGraphicsOutput, NULL);
  if (EFI_ERROR (Status))
  {
    if (mBltConfigure != NULL)
    {
      FreePool (mBltConfigure);
    }
    mBltConfigure = NULL;
    mHandle = NULL;
  }

  return Status;
}
