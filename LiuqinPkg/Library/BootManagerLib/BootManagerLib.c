/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#include <PiDxe.h>

#include <Guid/EventGroup.h>
#include <Guid/GlobalVariable.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LiuqinIoReady.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/SortLib.h>
#include <BootPolicyLib.h>

STATIC CONST EFI_GUID mShellFileGuid = {
    0x7c04a583, 0x9e3e, 0x4f1c, {0xad, 0x65, 0xe0, 0x52, 0x68, 0xd0, 0xb4, 0xd1}};
STATIC BOOLEAN mShellAttempted;
STATIC CONST CHAR16 mShellOptions[] = L"-nostartup";

#define LIUQIN_USB_BOOT_WAIT_ROUNDS 20U
#define LIUQIN_USB_BOOT_WAIT_STEP_US 250000U

STATIC VOID SelectFullConsole (VOID)
{
  INT32 Mode, Best = -1;
  UINTN Columns, Rows;
  UINT64 Area = 0;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out = gST->ConOut;
  if (Out == NULL || Out->Mode == NULL)
    return;
  for (Mode = 0; Mode < Out->Mode->MaxMode && Mode < 64; ++Mode)
  {
    if (!EFI_ERROR (Out->QueryMode (Out, (UINTN)Mode, &Columns, &Rows)) && Columns >= 80 &&
        Rows >= 25 && Columns <= 4096 && Rows <= 4096 && (UINT64)Columns * Rows > Area)
    {
      Best = Mode;
      Area = (UINT64)Columns * Rows;
    }
  }
  if (Best >= 0 && !EFI_ERROR (Out->SetMode (Out, (UINTN)Best)))
  {
  }
}

STATIC BOOLEAN FileExistsOnHandle (EFI_HANDLE Handle, CONST CHAR16 *FilePath)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
  EFI_STATUS Status;
  if (gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs) !=
          EFI_SUCCESS ||
      Fs == NULL || Fs->OpenVolume == NULL)
  {
    return FALSE;
  }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status) || Root == NULL)
  {
    return FALSE;
  }
  Status = Root->Open (Root, &File, (CHAR16 *)FilePath, EFI_FILE_MODE_READ, 0);

  if (!EFI_ERROR (Status) && File != NULL)
    File->Close (File);
  Root->Close (Root);
  return (BOOLEAN)(!EFI_ERROR (Status));
}

/* TRUE when some filesystem already carries one of the two standard ESP boot
 * targets this platform ranks (Windows or removable media). Only used to decide
 * whether a USB wait window is worth entering; it does not rank or reorder any
 * boot option. */
STATIC BOOLEAN KnownBootTargetPresent (VOID)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Index;
  BOOLEAN Found = FALSE;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                         NULL, &Count, &Handles)))
    return FALSE;
  for (Index = 0; Index < Count; ++Index)
  {
    if (FileExistsOnHandle (Handles[Index], LIUQIN_BOOT_WINDOWS_PATH) ||
        FileExistsOnHandle (Handles[Index], LIUQIN_BOOT_REMOVABLE_PATH))
    {
      Found = TRUE;
      break;
    }
  }
  if (Handles != NULL)
    FreePool (Handles);
  return Found;
}

/* TRUE when the USB host stack can plausibly enumerate a boot medium. The
 * platform handoff protocol must exist (the Liuqin USB DXE ran) and either both
 * readiness flags are set, or an xHCI controller is currently bound. The flags
 * can lag a completed binding, so a bound controller permits the scan even
 * when the last notify did not refresh them. No PMIC write is authorized here. */
STATIC BOOLEAN UsbHostReadyForBoot (VOID)
{
  LIUQIN_IO_READY_PROTOCOL *Ready = NULL;
  EFI_HANDLE *Handles = NULL;
  EFI_STATUS Status;
  UINTN Count = 0;

  if (EFI_ERROR (gBS->LocateProtocol (&gLiuqinUsbReadyProtocolGuid, NULL, (VOID **)&Ready)) ||
      Ready == NULL || Ready->Version != LIUQIN_IO_READY_VERSION)
    return FALSE;
  if (Ready->ControllerReady && Ready->DmaReady)
    return TRUE;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsb2HcProtocolGuid, NULL, &Count, &Handles);
  if (Handles != NULL)
    FreePool (Handles);
  return (BOOLEAN)(!EFI_ERROR (Status) && Count != 0);
}

/* Allow Type-C/VBUS and USB hub polling to finish before selecting a fallback.
 * Reconnect children after each interval; an existing loader skips the wait.
 * Twenty 250 ms intervals bound the wait to 5 s plus connection/scan time. */
STATIC VOID WaitForUsbBootTarget (VOID)
{
  UINTN Round;

  for (Round = 0; Round < LIUQIN_USB_BOOT_WAIT_ROUNDS; ++Round)
  {
    if (KnownBootTargetPresent () || !UsbHostReadyForBoot ())
      break;
    if (EFI_ERROR (gBS->Stall (LIUQIN_USB_BOOT_WAIT_STEP_US)))
      break;
    EfiBootManagerConnectAll ();
  }
}

/* Class and comparable bytes of one boot option. A named file is classified by
 * its path; a media option without a file node stays "media"; an FV image is
 * the embedded Shell only when its FFS GUID matches. */
STATIC VOID BootTargetOf (CONST EFI_BOOT_MANAGER_LOAD_OPTION *Option, LIUQIN_BOOT_CLASS *Class,
                          CONST UINT8 **Path, UINTN *Size)
{
  EFI_DEVICE_PATH_PROTOCOL *Node;
  *Class = LiuqinBootClassMedia;
  *Path = (CONST UINT8 *)Option->FilePath;
  *Size = Option->FilePath != NULL ? GetDevicePathSize (Option->FilePath) : 0;
  if (Option->FilePath == NULL)
  {
    *Class = LiuqinBootClassNone;
    *Size = 0;
    return;
  }
  for (Node = Option->FilePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node))
  {
    if (DevicePathType (Node) != MEDIA_DEVICE_PATH)
      continue;
    if (DevicePathSubType (Node) == MEDIA_FILEPATH_DP)
    {
      CHAR16 *Name = ((FILEPATH_DEVICE_PATH *)Node)->PathName;
      LIUQIN_BOOT_CLASS Found = LiuqinBootClassifyPath (Name);
      *Class = Found == LiuqinBootClassNone ? LiuqinBootClassMedia : Found;
      *Path = (CONST UINT8 *)Name;
      *Size = StrSize (Name);
      return;
    }
    if (DevicePathSubType (Node) == MEDIA_PIWG_FW_FILE_DP)
    {
      MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *Fv = (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Node;
      if (CompareGuid (&Fv->FvFileName, &mShellFileGuid))
      {
        *Class = LiuqinBootClassShell;
        *Path = (CONST UINT8 *)&Fv->FvFileName;
        *Size = sizeof (Fv->FvFileName);
      }
      return;
    }
  }
}

STATIC INTN EFIAPI BootTargetCompare (CONST VOID *A, CONST VOID *B)
{
  LIUQIN_BOOT_CLASS ClassA, ClassB;
  CONST UINT8 *PathA, *PathB;
  UINTN SizeA, SizeB;
  BootTargetOf ((CONST EFI_BOOT_MANAGER_LOAD_OPTION *)A, &ClassA, &PathA, &SizeA);
  BootTargetOf ((CONST EFI_BOOT_MANAGER_LOAD_OPTION *)B, &ClassB, &PathB, &SizeB);
  return LiuqinBootCompare (ClassA, PathA, SizeA, ClassB, PathB, SizeB);
}

STATIC BOOLEAN BootTargetPresent (CONST EFI_BOOT_MANAGER_LOAD_OPTION *Options, UINTN Count,
                                  CONST EFI_DEVICE_PATH_PROTOCOL *Path)
{
  UINTN Index, Size;
  if (Options == NULL || Path == NULL)
    return FALSE;
  Size = GetDevicePathSize (Path);
  for (Index = 0; Index < Count; ++Index)
  {
    if (Options[Index].FilePath == NULL)
      continue;
    if (GetDevicePathSize (Options[Index].FilePath) == Size &&
        CompareMem (Options[Index].FilePath, Path, Size) == 0)
      return TRUE;
  }
  return FALSE;
}

/* Publish one option per medium that actually carries FilePath. A missing file
 * is not an error: it only means this board has no such boot target. */
STATIC VOID AddFileBootOption (CONST CHAR16 *FilePath, CHAR16 *Description)
{
  EFI_HANDLE *Handles = NULL;
  EFI_BOOT_MANAGER_LOAD_OPTION *Options, Option;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_STATUS Status;
  UINTN Count = 0, HandleCount = 0, Index;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL,
                                          &HandleCount, &Handles)))
    HandleCount = 0;
  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  for (Index = 0; Index < HandleCount; ++Index)
  {
    if (!FileExistsOnHandle (Handles[Index], FilePath))
      continue;
    Path = FileDevicePath (Handles[Index], FilePath);
    if (Path == NULL)
      continue;
    if (BootTargetPresent (Options, Count, Path))
    {
      FreePool (Path);
      continue;
    }
    Status =
        EfiBootManagerInitializeLoadOption (&Option, LoadOptionNumberUnassigned, LoadOptionTypeBoot,
                                            LOAD_OPTION_ACTIVE, Description, Path, NULL, 0);
    FreePool (Path);
    if (EFI_ERROR (Status))
    {
      continue;
    }
    Status = EfiBootManagerAddLoadOptionVariable (&Option, 0);
    EfiBootManagerFreeLoadOption (&Option);
  }
  if (Options != NULL)
    EfiBootManagerFreeLoadOptions (Options, Count);
  if (Handles != NULL)
    FreePool (Handles);
}

STATIC VOID AddShellBootOption (VOID)
{
  EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
  EFI_BOOT_MANAGER_LOAD_OPTION *Options, Option;
  EFI_STATUS Status;
  UINTN Count = 0;
  Status = GetFileDevicePathFromAnyFv (&mShellFileGuid, EFI_SECTION_PE32, 0, &Path);
  if (EFI_ERROR (Status) || Path == NULL)
  {
    return;
  }
  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  if (!BootTargetPresent (Options, Count, Path))
  {
    Status = EfiBootManagerInitializeLoadOption (
        &Option, LoadOptionNumberUnassigned, LoadOptionTypeBoot, LOAD_OPTION_ACTIVE, L"UEFI Shell",
        Path, (VOID *)mShellOptions, sizeof (mShellOptions));
    if (!EFI_ERROR (Status))
    {
      Status = EfiBootManagerAddLoadOptionVariable (&Option, 0);
      EfiBootManagerFreeLoadOption (&Option);
    }
  }
  if (Options != NULL)
    EfiBootManagerFreeLoadOptions (Options, Count);
  FreePool (Path);
}

STATIC VOID ConfigureBootTargets (VOID)
{
  EfiBootManagerRefreshAllBootOption ();
  AddFileBootOption (LIUQIN_BOOT_WINDOWS_PATH, L"Windows Boot Manager");
  AddFileBootOption (LIUQIN_BOOT_REMOVABLE_PATH, L"EFI Removable Media");
  AddShellBootOption ();
  EfiBootManagerSortLoadOptionVariable (LoadOptionTypeBoot, BootTargetCompare);
}

/* A RAM bootstrap must never silently select an installed UFS loader when
 * its own material is incomplete. BDS loads the complete WinPE chain from
 * this one RAM carrier, before Windows can initialize the KD USB transport. */

STATIC EFI_STATUS ConfigureUsbKeyboardConsole (VOID)
{
  STATIC CONST struct
  {
    USB_CLASS_DEVICE_PATH Usb;
    EFI_DEVICE_PATH_PROTOCOL End;
  } Keyboard = {
      {{MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP, {sizeof (USB_CLASS_DEVICE_PATH), 0}},
       0xFFFF,
       0xFFFF,
       3,
       1,
       1},
      {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {END_DEVICE_PATH_LENGTH, 0}}};

  if (EfiGetCurrentTpl () != TPL_APPLICATION)
    return EFI_UNSUPPORTED;
  return EfiBootManagerUpdateConsoleVariable (ConIn, (EFI_DEVICE_PATH_PROTOCOL *)(UINTN)&Keyboard,
                                              NULL);
}

STATIC VOID ConnectInputConsoles (VOID)
{
  EFI_HANDLE *Handles = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  UINTN Count, Index;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextInProtocolGuid, NULL, &Count,
                                          &Handles)))
    return;
  for (Index = 0; Index < Count; ++Index)
  {
    Path = DevicePathFromHandle (Handles[Index]);
    if (Path != NULL)
      EfiBootManagerUpdateConsoleVariable (ConIn, Path, NULL);
  }
  FreePool (Handles);
}

STATIC
VOID LaunchEmbeddedShell (VOID)
{
  EFI_STATUS Status;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  EFI_BOOT_MANAGER_LOAD_OPTION Option;
  if (mShellAttempted)
  {
    return;
  }

  mShellAttempted = TRUE;
  DevicePath = NULL;
  Status = GetFileDevicePathFromAnyFv (&mShellFileGuid, EFI_SECTION_PE32, 0, &DevicePath);
  if (EFI_ERROR (Status))
  {

    return;
  }

  Status = EfiBootManagerInitializeLoadOption (
      &Option, LoadOptionNumberUnassigned, LoadOptionTypeBoot, LOAD_OPTION_ACTIVE, L"UEFI Shell",
      DevicePath, (VOID *)mShellOptions, sizeof (mShellOptions));
  FreePool (DevicePath);
  if (EFI_ERROR (Status))
  {

    return;
  }

  EfiBootManagerBoot (&Option);
  Status = Option.Status;
  EfiBootManagerFreeLoadOption (&Option);
}

VOID EFIAPI PlatformBootManagerBeforeConsole (VOID)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;

  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  Status = ConfigureUsbKeyboardConsole ();

  EfiBootManagerConnectAll ();
  ConnectInputConsoles ();

  Handles = NULL;
  Status =
      gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status))
  {

    return;
  }

  for (Index = 0; Index < Count; ++Index)
  {
    DevicePath = DevicePathFromHandle (Handles[Index]);
    if (DevicePath == NULL)
    {
      continue;
    }

    Status = EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);

    Status = EfiBootManagerUpdateConsoleVariable (ErrOut, DevicePath, NULL);

    Status = gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }

  FreePool (Handles);
}

VOID EFIAPI PlatformBootManagerAfterConsole (VOID)
{
  EfiBootManagerConnectAllDefaultConsoles ();
  SelectFullConsole ();
  if (gST->ConOut != NULL)
    gST->ConOut->ClearScreen (gST->ConOut);
  /* A late USB drive only matters when nothing known is bootable yet; the wait
   * keeps the normal path (Shell fallback) when no medium appears. */
  WaitForUsbBootTarget ();
  ConnectInputConsoles ();
  EfiBootManagerConnectAllDefaultConsoles ();

  // BdsDxe evaluates the explicit RAM carrier or the normal ranked policy.
  ConfigureBootTargets ();
  EfiEventGroupSignal (&gEfiEventReadyToBootGuid);
}

VOID EFIAPI PlatformBootManagerWaitCallback (UINT16 TimeoutRemain)
{
  (VOID) TimeoutRemain;
}

VOID EFIAPI PlatformBootManagerUnableToBoot (VOID)
{
  LaunchEmbeddedShell ();
}
