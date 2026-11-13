/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Guid/LiuqinHandoff.h>
#include <Guid/MemoryAllocationHob.h>
#include <Guid/LiuqinMemoryPlan.h>
#include <IndustryStandard/UfsHci.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <BootContract.h>
#include <Library/DevicePathLib.h>
#include <Library/DmaLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/LiuqinIoReady.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <UfsHostPolicy.h>
#include <SmmuLayout.h>
#include <UfsSmmuProbe.h>
#include "UfsRuntimeDtb.h"
#include "../../Library/RetainedDmaLib/RetainedDmaLib.h"

#define UFS_BASE 0x01D84000U
#define UFS_SIZE 0x00003000U
#define SMMU_BASE 0x15000000U
#define UFS_SID 0xE0U
/* ExitBootServices DMA-stop policy for the inherited controller.
   LiuqinUfsQuiesce() itself stops both lists and reads them back for up to
   1000 x 100 us (100 ms), so three attempts plus two 1 ms gaps bound the
   recovery attempts at <= 302 ms. There is no unbounded retry: after the
   last failed attempt fail-stop preserves ownership of DMA-visible pages. */
#define LIUQIN_UFS_EBS_QUIESCE_ATTEMPTS 3U
#define LIUQIN_UFS_EBS_RETRY_DELAY_US 1000U

typedef struct
{
  LIST_ENTRY Link;
  VOID *Inner;
  UINT64 Address;
  UINTN Size;
  EDKII_UFS_HOST_CONTROLLER_OPERATION Operation;
} UFS_MAPPING;

typedef struct
{
  LIST_ENTRY Link;
  EFI_HANDLE Handle;
  UINT64 Lun;
  EFI_BLOCK_IO_PROTOCOL *Io;
  EFI_BLOCK_READ OriginalRead;
  EFI_BLOCK_RESET OriginalReset;
  EFI_BLOCK_IO2_PROTOCOL *Io2;
  EFI_BLOCK_READ_EX OriginalRead2;
  EFI_BLOCK_RESET_EX OriginalReset2;
} UFS_VOLUME;

typedef struct
{
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} UFS_PATH;

STATIC LIST_ENTRY mMappings = INITIALIZE_LIST_HEAD_VARIABLE (mMappings);
STATIC LIST_ENTRY mVolumes = INITIALIZE_LIST_HEAD_VARIABLE (mVolumes);
STATIC EFI_HANDLE mController;
STATIC VOID *mBlockRegistration, *mBlock2Registration, *mPassRegistration;
STATIC EFI_EXT_SCSI_PASS_THRU_PROTOCOL *mPass;
STATIC EFI_EXT_SCSI_PASS_THRU_PASSTHRU mOriginalPass;
STATIC BOOLEAN mStopping;
STATIC BOOLEAN mBusy;
STATIC UINT64 mStackBase, mStackSize;

STATIC CONST LIUQIN_MEMORY_PLAN *mMemoryPlan;
STATIC UINT32 mClocks[6];
STATIC LIUQIN_IO_READY_PROTOCOL mReady = {
    LIUQIN_IO_READY_VERSION, UFS_BASE, UFS_SIZE, 297, TRUE, FALSE};
STATIC UFS_PATH mPath = {
    {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof (VENDOR_DEVICE_PATH), 0}},
     {0xf18d33dc, 0x4aca, 0x48a7, {0xac, 0x1c, 0xb6, 0xaf, 0x6d, 0xc3, 0x07, 0x56}}},
    {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}}};

STATIC UINT32 ReadReg (UINTN Offset)
{
  return MmioRead32 (UFS_BASE + Offset);
}
STATIC VOID WriteReg (UINTN Offset, UINT32 Value)
{
  MmioWrite32 (UFS_BASE + Offset, Value);
}
STATIC BOOLEAN BindingsCurrent (VOID);
STATIC EFI_STATUS EFIAPI ReadOnlyPass (EFI_EXT_SCSI_PASS_THRU_PROTOCOL *This, UINT8 *Target,
                                       UINT64 Lun,
                                       EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet,
                                       EFI_EVENT Event);

STATIC int S1Read (VOID *Context, LIUQIN_SMMU_U32 Offset, LIUQIN_SMMU_U32 *Value);
STATIC BOOLEAN AllowedBuffer (UINT64 Address, UINT64 Bytes, BOOLEAN Common)
{
  return LiuqinUfsRange (FixedPcdGet64 (PcdUefiMemoryBase), FixedPcdGet32 (PcdUefiMemorySize),
                         Address, Bytes) ||
         LiuqinUfsRamRange (mMemoryPlan, Address, Bytes) ||
         (!Common && LiuqinUfsRange (mStackBase, mStackSize, Address, Bytes));
}
STATIC BOOLEAN Idle (VOID)
{
  return ReadReg (UFS_HC_UTRLDBR_OFFSET) == 0 && ReadReg (UFS_HC_UTMRLDBR_OFFSET) == 0;
}

STATIC UINT32 PolicyRead (VOID *Context, UINTN Address)
{
  (VOID) Context;
  return MmioRead32 (Address);
}
STATIC VOID PolicyWrite (VOID *Context, UINTN Address, UINT32 Value)
{
  (VOID) Context;
  MemoryFence ();
  MmioWrite32 (Address, Value);
  MemoryFence ();
}
STATIC VOID PolicyDelay (VOID *Context, UINTN Microseconds)
{
  (VOID) Context;
  MicroSecondDelay (Microseconds);
}
STATIC VOID InvalidateBindings (VOID)
{
  LIST_ENTRY *Link;
  UFS_VOLUME *Volume;
  mStopping = TRUE;
  mReady.ControllerReady = FALSE;
  mReady.DmaReady = FALSE;
  mPass = NULL;
  mOriginalPass = NULL;
  /* Protocol uninstall frees the upstream objects. Never dereference them,
     restore callbacks into them, or permit a later allocation to rebind.
     No BootServices allocation/free is needed, including at EBS. */
  for (Link = mVolumes.ForwardLink; Link != &mVolumes; Link = Link->ForwardLink)
  {
    Volume = BASE_CR (Link, UFS_VOLUME, Link);
    Volume->Io = NULL;
    Volume->Io2 = NULL;
    Volume->OriginalRead = NULL;
    Volume->OriginalReset = NULL;
    Volume->OriginalRead2 = NULL;
    Volume->OriginalReset2 = NULL;
  }
}
STATIC VOID StopSafely (VOID)
{
  InvalidateBindings ();
  /* BootServices allocations become OS-owned after EBS. Merely refusing
     Free/Unmap cannot preserve their lifetime or the caller's input buffer.
     Do not return to the caller or complete EBS with a live DMA queue.
     This is the runtime/ReadyToBoot failure path, not the EBS callback: keep
     fail-stop, but record the reason (console + best-effort RAM log) first so
     a device without a serial port shows why it stopped. */
  if (!LiuqinUfsQuiesce (PolicyRead, PolicyWrite, PolicyDelay, NULL))
  {

    CpuDeadLoop ();
  }
}
STATIC VOID ReleaseFailed (VOID)
{

  StopSafely ();

  for (;;)
    CpuDeadLoop ();
}

STATIC BOOLEAN mCallbacksInstalled;
STATIC EFI_STATUS InitializationResult (EFI_STATUS Status)
{
  // Registered callbacks require the image to remain resident on failure.
  return mCallbacksInstalled ? EFI_SUCCESS : Status;
}
STATIC VOID EFIAPI CheckUfsBinding (EFI_EVENT Event, VOID *Context)
{
  (VOID) Event;
  (VOID) Context;
  if (mController != NULL && (!BindingsCurrent () || !Idle () || mBusy))
    StopSafely ();
}

STATIC UFS_MAPPING *Mapped (UINT64 Address, UINT64 Size, BOOLEAN Common)
{
  LIST_ENTRY *Link;
  UFS_MAPPING *Map;
  for (Link = mMappings.ForwardLink; Link != &mMappings; Link = Link->ForwardLink)
  {
    Map = BASE_CR (Link, UFS_MAPPING, Link);
    if ((!Common || Map->Operation == EdkiiUfsHcOperationBusMasterCommonBuffer) &&
        LiuqinUfsRange (Map->Address, Map->Size, Address, Size))
    {
      return Map;
    }
  }
  return NULL;
}

STATIC BOOLEAN QueueBuffer (UINT64 Address, UINT64 Bytes)
{
  UINT64 Transfer =
      ((UINT64)ReadReg (UFS_HC_UTRLBAU_OFFSET) << 32) | ReadReg (UFS_HC_UTRLBA_OFFSET);
  UINT64 Task = ((UINT64)ReadReg (UFS_HC_UTMRLBAU_OFFSET) << 32) | ReadReg (UFS_HC_UTMRLBA_OFFSET);
  return LiuqinUfsRange (Address, Bytes, Transfer, 1) || LiuqinUfsRange (Address, Bytes, Task, 1);
}

STATIC EFI_STATUS EFIAPI GetBar (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, UINTN *Bar)
{
  if (This == NULL || Bar == NULL)
    return EFI_INVALID_PARAMETER;
  *Bar = UFS_BASE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Allocate (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
                                   EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **Address,
                                   UINT64 Attributes)
{
  EFI_STATUS Status;
  if (This == NULL || Address == NULL || Pages == 0 || Pages > MAX_UINTN / EFI_PAGE_SIZE)
    return EFI_INVALID_PARAMETER;
  if (mStopping || Attributes != 0)
    return EFI_UNSUPPORTED;
  Status = DmaAllocateBuffer (MemoryType, Pages, Address);
  if (!EFI_ERROR (Status) && !AllowedBuffer ((UINTN)*Address, EFI_PAGES_TO_SIZE (Pages), TRUE))
  {
    if (EFI_ERROR (DmaFreeBuffer (Pages, *Address)))
      ReleaseFailed ();
    *Address = NULL;
    return EFI_UNSUPPORTED;
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI Free (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, UINTN Pages, VOID *Address)
{
  EFI_STATUS Status;
  if (This == NULL || Address == NULL || Pages == 0 || Pages > MAX_UINTN / EFI_PAGE_SIZE)
    return EFI_INVALID_PARAMETER;
  /* DBR=0 alone does not prove that a running list can release its storage.
     Releasing either queue requires positive RSR+DBR stop readback first. */
  if (mStopping || !Idle () || QueueBuffer ((UINTN)Address, EFI_PAGES_TO_SIZE (Pages)))
    StopSafely ();
  Status = DmaFreeBuffer (Pages, Address);
  if (EFI_ERROR (Status))
    ReleaseFailed ();
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI MapBuffer (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
                                    EDKII_UFS_HOST_CONTROLLER_OPERATION Operation, VOID *Address,
                                    UINTN *Bytes, EFI_PHYSICAL_ADDRESS *DeviceAddress,
                                    VOID **Mapping)
{
  EFI_STATUS Status;
  UFS_MAPPING *Map;
  UINTN Requested, MappedBytes;
  EFI_PHYSICAL_ADDRESS MappedAddress;
  if (This == NULL || Address == NULL || Bytes == NULL || DeviceAddress == NULL ||
      Mapping == NULL || Operation >= EdkiiUfsHcOperationMaximum)
    return EFI_INVALID_PARAMETER;
  Requested = *Bytes;
  *DeviceAddress = 0;
  *Mapping = NULL;
  if (!AllowedBuffer ((UINTN)Address, *Bytes,
                      Operation == EdkiiUfsHcOperationBusMasterCommonBuffer))
    return EFI_INVALID_PARAMETER;
  if (mStopping)
    return EFI_NOT_READY;
  Map = AllocateZeroPool (sizeof (*Map));
  if (Map == NULL)
    return EFI_OUT_OF_RESOURCES;
  MappedBytes = Requested;
  MappedAddress = 0;
  Status =
      DmaMap ((DMA_MAP_OPERATION)Operation, Address, &MappedBytes, &MappedAddress, &Map->Inner);
  if (EFI_ERROR (Status) || MappedBytes != Requested || Map->Inner == NULL)
  {
    if (Map->Inner != NULL && EFI_ERROR (DmaUnmap (Map->Inner)))
      ReleaseFailed ();
    *Bytes = Requested;
    *DeviceAddress = 0;
    FreePool (Map);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }
  /* NonCoherentDmaLib may map an unaligned inbound buffer via an identity
     addressed bounce allocation; only common buffers must equal the host VA. */
  if ((Operation == EdkiiUfsHcOperationBusMasterCommonBuffer && MappedAddress != (UINTN)Address) ||
      !AllowedBuffer (MappedAddress, MappedBytes,
                      Operation == EdkiiUfsHcOperationBusMasterCommonBuffer))
  {
    if (EFI_ERROR (DmaUnmap (Map->Inner)))
      ReleaseFailed ();
    *DeviceAddress = 0;
    FreePool (Map);
    return EFI_UNSUPPORTED;
  }
  Map->Address = MappedAddress;
  Map->Size = MappedBytes;
  Map->Operation = Operation;
  InsertTailList (&mMappings, &Map->Link);
  *DeviceAddress = MappedAddress;
  *Mapping = Map;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI UnmapBuffer (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, VOID *Mapping)
{
  LIST_ENTRY *Link;
  UFS_MAPPING *Map;
  EFI_STATUS Status;
  if (This == NULL || Mapping == NULL)
    return EFI_INVALID_PARAMETER;
  for (Link = mMappings.ForwardLink; Link != &mMappings; Link = Link->ForwardLink)
  {
    Map = BASE_CR (Link, UFS_MAPPING, Link);
    if (Map == Mapping)
    {
      if (mStopping || !Idle () ||
          (Map->Operation == EdkiiUfsHcOperationBusMasterCommonBuffer &&
           QueueBuffer (Map->Address, Map->Size)))
        StopSafely ();
      Status = DmaUnmap (Map->Inner);
      if (EFI_ERROR (Status))
        ReleaseFailed ();
      RemoveEntryList (Link);
      FreePool (Map);
      return EFI_SUCCESS;
    }
  }
  return EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS EFIAPI Flush (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This)
{
  if (This == NULL)
    return EFI_INVALID_PARAMETER;
  MemoryFence ();
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ValidateSlot (UINT64 ListAddress, UINTN Slot)
{
  UTP_TRD *Trd;
  UTP_TR_PRD *Prd;
  UFS_MAPPING *Map;
  UINT64 Command, DataAddress, DataSize, Total;
  UINTN Index;
  UINT8 *Upiu;
  UINT32 Expected;
  EDKII_UFS_HOST_CONTROLLER_OPERATION Direction;
  Map = Mapped (ListAddress + Slot * sizeof (UTP_TRD), sizeof (UTP_TRD), TRUE);
  if (Map == NULL)
    return EFI_SECURITY_VIOLATION;
  Trd = (UTP_TRD *)(UINTN)(ListAddress + Slot * sizeof (UTP_TRD));
  if (Trd->Ct != UFS_STORAGE_COMMAND_TYPE || Trd->Ocs != UFS_HC_TRD_OCS_INIT_VALUE ||
      Trd->Rsvd1 != 0 || Trd->Rsvd2 != 0 || Trd->Rsvd3 != 0 || Trd->Rsvd4 != 0 || Trd->Rsvd5 != 0 ||
      Trd->Rsvd6 != 0)
    return EFI_SECURITY_VIOLATION;
  Command = ((UINT64)Trd->UcdBaU << 32) | ((UINT64)Trd->UcdBa << 7);
  if (Mapped (Command, 32, TRUE) == NULL || Trd->RuO < 8 || Trd->RuL < 8 ||
      Mapped (Command + (UINT64)Trd->RuO * 4, (UINT64)Trd->RuL * 4, TRUE) == NULL)
    return EFI_SECURITY_VIOLATION;
  Upiu = (UINT8 *)(UINTN)Command;
  if (!LiuqinUfsReadOnlyUpiu (Upiu, 32, Trd->Dd))
    return EFI_WRITE_PROTECTED;
  if (Upiu[0] != 1)
    return Trd->PrdtL == 0 ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
  Expected =
      ((UINT32)Upiu[12] << 24) | ((UINT32)Upiu[13] << 16) | ((UINT32)Upiu[14] << 8) | Upiu[15];
  if (Trd->PrdtL == 0)
    return Expected == 0 ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
  /* Only data-in transfers may carry PRDTs in this read-only driver. */
  Direction =
      (Trd->Dd == UfsDataIn) ? EdkiiUfsHcOperationBusMasterWrite : EdkiiUfsHcOperationMaximum;
  if (Direction >= EdkiiUfsHcOperationMaximum || Trd->PrdtO < Trd->RuO + Trd->RuL ||
      Mapped (Command + (UINT64)Trd->PrdtO * 4, (UINT64)Trd->PrdtL * sizeof (*Prd), TRUE) == NULL)
    return EFI_SECURITY_VIOLATION;
  Prd = (UTP_TR_PRD *)(UINTN)(Command + (UINT64)Trd->PrdtO * 4);
  Total = 0;
  for (Index = 0; Index < Trd->PrdtL; Index++)
  {
    DataAddress = ((UINT64)Prd[Index].DbAddrU << 32) | ((UINT64)Prd[Index].DbAddr << 2);
    DataSize = (UINT64)Prd[Index].DbCount + 1;
    Map = Mapped (DataAddress, DataSize, FALSE);
    if (Map == NULL || Map->Operation != Direction || Prd[Index].Rsvd1 != 0 ||
        Prd[Index].Rsvd2 != 0 || Prd[Index].Rsvd3 != 0 || (DataSize & 3) != 0)
      return EFI_SECURITY_VIOLATION;
    Total += DataSize;
  }
  /* UFS PRDT byte counts are DWORD sized. The locked transport uses a
     separate padded auxiliary buffer for e.g. REQUEST SENSE(18). Accept only
     exact padding; every byte was independently checked against a live map.
     The UPIU expected length remains the original caller's length. */
  return Total == (((UINT64)Expected + 3) & ~3ULL) ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}

STATIC EFI_STATUS EFIAPI ReadMmio (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
                                   EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset,
                                   UINTN Count, VOID *Buffer)
{
  UINTN Index;
  if (This == NULL || Buffer == NULL || Width != EfiUfsHcWidthUint32 || Count == 0 ||
      (Offset & 3) != 0 || Offset >= 0xA0 || Count > (0xA0 - Offset) / 4)
    return EFI_INVALID_PARAMETER;
  for (Index = 0; Index < Count; Index++)
  {
    ((UINT32 *)Buffer)[Index] = ReadReg ((UINTN)Offset + Index * 4);
  }
  MemoryFence ();
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI WriteMmio (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
                                    EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset,
                                    UINTN Count, VOID *Buffer)
{
  UINT32 Value;
  UINT64 ListAddress;
  UINTN Slot;
  EFI_STATUS Status;
  if (This == NULL || Buffer == NULL || Width != EfiUfsHcWidthUint32 || Count != 1 ||
      (Offset & 3) != 0)
    return EFI_INVALID_PARAMETER;
  Value = *(UINT32 *)Buffer;
  if (mStopping)
  {
    if ((Offset == UFS_HC_UTRLRSR_OFFSET || Offset == UFS_HC_UTMRLRSR_OFFSET) && Value == 0)
    {
      StopSafely ();
      return EFI_SUCCESS;
    }
    return EFI_NOT_READY;
  }
  switch (Offset)
  {
  case UFS_HC_UTRLDBR_OFFSET:
    ListAddress = ((UINT64)ReadReg (UFS_HC_UTRLBAU_OFFSET) << 32) | ReadReg (UFS_HC_UTRLBA_OFFSET);
    for (Slot = 0; Slot < 32; Slot++)
    {
      if ((Value & (1U << Slot)) != 0)
      {
        if (Slot > (ReadReg (UFS_HC_CAP_OFFSET) & UFS_HC_CAP_NUTRS))
          return EFI_INVALID_PARAMETER;
        Status = ValidateSlot (ListAddress, Slot);
        if (EFI_ERROR (Status))
        {

          return Status;
        }
      }
    }
    break;
  case UFS_HC_UTRLBA_OFFSET:
  case UFS_HC_UTRLBAU_OFFSET:
  case UFS_HC_UTMRLBA_OFFSET:
  case UFS_HC_UTMRLBAU_OFFSET:
    /* An empty doorbell does not stop descriptor fetching. Replacing a
       running list base can expose a torn BA/BAU pair to the controller;
       require the corresponding list's positive stop readback first. */
    if (!Idle () || ReadReg (Offset == UFS_HC_UTRLBA_OFFSET || Offset == UFS_HC_UTRLBAU_OFFSET
                                 ? UFS_HC_UTRLRSR_OFFSET
                                 : UFS_HC_UTMRLRSR_OFFSET) != 0)
      return EFI_NOT_READY;
    break;
  case UFS_HC_UTRLRSR_OFFSET:
  case UFS_HC_UTMRLRSR_OFFSET:
    if (Value == 0)
    {
      /* Generic Stop next attempts HCE=0, which this inherited profile
         rejects. Prove both lists stopped before upstream can release
         its queue objects; teardown permanently invalidates bindings. */
      StopSafely ();
      return EFI_SUCCESS;
    }
    if (Value > 1 || !Idle ())
      return EFI_NOT_READY;
    if (Value == 1)
    {
      UINTN BaseOffset =
          Offset == UFS_HC_UTRLRSR_OFFSET ? UFS_HC_UTRLBA_OFFSET : UFS_HC_UTMRLBA_OFFSET;
      UINTN Length =
          Offset == UFS_HC_UTRLRSR_OFFSET
              ? ((ReadReg (UFS_HC_CAP_OFFSET) & UFS_HC_CAP_NUTRS) + 1) * sizeof (UTP_TRD)
              : (((ReadReg (UFS_HC_CAP_OFFSET) & UFS_HC_CAP_NUTMRS) >> 16) + 1) * sizeof (UTP_TMRD);
      ListAddress = ((UINT64)ReadReg (BaseOffset + 4) << 32) | ReadReg (BaseOffset);
      if ((ListAddress & 0x3FF) != 0 || Mapped (ListAddress, Length, TRUE) == NULL)
        return EFI_SECURITY_VIOLATION;
    }
    /* Starting a list does not issue commands; every transfer doorbell is
       validated, and all task-management doorbells are forbidden. */
    break;
  case UFS_HC_IE_OFFSET:
    if (Value != 0)
      return EFI_UNSUPPORTED;
    break;
  case UFS_HC_IS_OFFSET:
    break;
  case UFS_HC_UTRLCLR_OFFSET:
    /* An abort/timeout must stop before upstream's Unmap/Free cleanup,
       including Query callers outside the public SCSI wrapper. */
    if (!Idle ())
    {
      StopSafely ();
      return EFI_SUCCESS;
    }
    break;
  default:
    return EFI_WRITE_PROTECTED; /* HCE, UIC, task management and vendor MMIO. */
  }
  MemoryFence ();
  WriteReg ((UINTN)Offset, Value);
  MemoryFence ();
  return EFI_SUCCESS;
}

STATIC EDKII_UFS_HOST_CONTROLLER_PROTOCOL mHost = {GetBar,      Allocate, Free,     MapBuffer,
                                                   UnmapBuffer, Flush,    ReadMmio, WriteMmio};

STATIC EFI_STATUS EFIAPI PlatformCallback (EFI_HANDLE Controller,
                                           EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase, VOID *Data)
{
  if (Controller != mController || Data == NULL || Phase > EdkiiUfsHcPostLinkStartup)
    return EFI_INVALID_PARAMETER;
  return ((ReadReg (UFS_HC_ENABLE_OFFSET) & UFS_HC_HCE_EN) != 0 &&
          (ReadReg (UFS_HC_STATUS_OFFSET) & UFS_HC_HCS_DP) != 0)
             ? EFI_SUCCESS
             : EFI_NOT_READY;
}

STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL mPlatform = {
    EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION, NULL, PlatformCallback,
    EdkiiUfsCardRefClkFreqObsolete,         TRUE, TRUE};

STATIC BOOLEAN UfsDevice (EFI_HANDLE Handle, UINT64 *Lun)
{
  EFI_DEVICE_PATH_PROTOCOL *Path;
  UFS_DEVICE_PATH *Ufs;
  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&Path)))
    return FALSE;
  /* Require our controller prefix, not an unrelated PCI UFS controller. */
  if (GetDevicePathSize (Path) < sizeof (mPath) ||
      CompareMem (Path, &mPath.Vendor, sizeof (mPath.Vendor)) != 0)
    return FALSE;
  for (; !IsDevicePathEnd (Path); Path = NextDevicePathNode (Path))
  {
    if (DevicePathType (Path) == MESSAGING_DEVICE_PATH && DevicePathSubType (Path) == MSG_UFS_DP)
    {
      if (Path->Length[0] != sizeof (UFS_DEVICE_PATH) || Path->Length[1] != 0)
        return FALSE;
      Ufs = (VOID *)Path;
      if (Ufs->Pun != 0 || Ufs->Lun > 7)
        return FALSE;
      /* Locked UfsPassThruBuildDevicePath/GetTargetLun use an all-zero
         target and encode the ordinary UFS LUN in byte 1 of UINT64. */
      *Lun = (UINT64)Ufs->Lun << 8;
      return TRUE;
    }
  }
  return FALSE;
}

STATIC UFS_VOLUME *FindVolume (VOID *This, BOOLEAN IsIo2);

STATIC EFI_STATUS EFIAPI ProtectedWrite (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
                                         UINTN Size, VOID *Buffer)
{
  (VOID) This;
  (VOID) MediaId;
  (VOID) Lba;
  (VOID) Size;
  (VOID) Buffer;
  return mStopping ? EFI_NOT_READY : EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI ProtectedWrite2 (EFI_BLOCK_IO2_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
                                          EFI_BLOCK_IO2_TOKEN *Token, UINTN Size, VOID *Buffer)
{
  EFI_STATUS Status = mStopping ? EFI_NOT_READY : EFI_WRITE_PROTECTED;
  (VOID) This;
  (VOID) MediaId;
  (VOID) Lba;
  (VOID) Size;
  (VOID) Buffer;
  if (Token != NULL)
    Token->TransactionStatus = Status;
  return Status;
}
STATIC UFS_VOLUME *FindVolume (VOID *This, BOOLEAN IsIo2)
{
  LIST_ENTRY *Link;
  UFS_VOLUME *Volume;
  for (Link = mVolumes.ForwardLink; Link != &mVolumes; Link = Link->ForwardLink)
  {
    Volume = BASE_CR (Link, UFS_VOLUME, Link);
    if (IsIo2 ? ((VOID *)Volume->Io2 == This) : ((VOID *)Volume->Io == This))
      return Volume;
  }
  return NULL;
}
STATIC VOID UpdateReadOnly (UFS_VOLUME *Volume)
{
  if (Volume->Io != NULL)
    Volume->Io->Media->ReadOnly = TRUE;
  if (Volume->Io2 != NULL)
    Volume->Io2->Media->ReadOnly = TRUE;
}

STATIC EFI_STATUS EFIAPI ProtectedReset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN ExtendedVerification)
{
  UFS_VOLUME *Volume = FindVolume (This, FALSE);
  EFI_STATUS Status;
  if (mStopping)
    return EFI_NOT_READY;
  if (Volume == NULL)
    return EFI_INVALID_PARAMETER;
  Status = Volume->OriginalReset (This, ExtendedVerification);
  UpdateReadOnly (Volume);
  return Status;
}
STATIC EFI_STATUS EFIAPI ProtectedReset2 (EFI_BLOCK_IO2_PROTOCOL *This,
                                          BOOLEAN ExtendedVerification)
{
  UFS_VOLUME *Volume = FindVolume (This, TRUE);
  EFI_STATUS Status;
  if (mStopping)
    return EFI_NOT_READY;
  if (Volume == NULL)
    return EFI_INVALID_PARAMETER;
  Status = Volume->OriginalReset2 (This, ExtendedVerification);
  UpdateReadOnly (Volume);
  return Status;
}
STATIC EFI_STATUS FlushVolume (UFS_VOLUME *Volume)
{
  if (mStopping || mBusy)
    return EFI_NOT_READY;
  if (Volume == NULL)
    return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS; /* Read-only volumes never have dirty writes. */
}
STATIC EFI_STATUS EFIAPI ProtectedFlush (EFI_BLOCK_IO_PROTOCOL *This)
{
  return FlushVolume (FindVolume (This, FALSE));
}
STATIC EFI_STATUS EFIAPI ProtectedFlush2 (EFI_BLOCK_IO2_PROTOCOL *This, EFI_BLOCK_IO2_TOKEN *Token)
{
  EFI_STATUS Status = FlushVolume (FindVolume (This, TRUE));
  if (Token != NULL)
  {
    Token->TransactionStatus = Status;
    if (!EFI_ERROR (Status) && Token->Event != NULL)
      gBS->SignalEvent (Token->Event);
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI ProtectedRead2 (EFI_BLOCK_IO2_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
                                         EFI_BLOCK_IO2_TOKEN *Token, UINTN Size, VOID *Buffer)
{
  UFS_VOLUME *Volume = FindVolume (This, TRUE);
  EFI_STATUS Status;
  if (mStopping)
    return EFI_NOT_READY;
  if (Volume == NULL)
    return EFI_INVALID_PARAMETER;
  Status = Volume->OriginalRead2 (This, MediaId, Lba, Token, Size, Buffer);
  UpdateReadOnly (Volume);
  return Status;
}
STATIC EFI_STATUS EFIAPI ProtectedRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
                                        UINTN Size, VOID *Buffer)
{
  LIST_ENTRY *Link;
  UFS_VOLUME *Volume;
  EFI_STATUS Status;
  if (mStopping)
    return EFI_NOT_READY;
  for (Link = mVolumes.ForwardLink; Link != &mVolumes; Link = Link->ForwardLink)
  {
    Volume = BASE_CR (Link, UFS_VOLUME, Link);
    if (Volume->Io == This)
    {
      Status = Volume->OriginalRead (This, MediaId, Lba, Size, Buffer);
      UpdateReadOnly (Volume);
      return Status;
    }
  }
  return EFI_INVALID_PARAMETER;
}

STATIC BOOLEAN BindingsCurrent (VOID)
{
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL *Pass;
  EFI_BLOCK_IO_PROTOCOL *Io;
  EFI_BLOCK_IO2_PROTOCOL *Io2;
  LIST_ENTRY *Link;
  UFS_VOLUME *Volume;
  if (mStopping)
    return FALSE;
  if (mPass != NULL && (EFI_ERROR (gBS->HandleProtocol (
                            mController, &gEfiExtScsiPassThruProtocolGuid, (VOID **)&Pass)) ||
                        Pass != mPass || Pass->PassThru != ReadOnlyPass))
    return FALSE;
  for (Link = mVolumes.ForwardLink; Link != &mVolumes; Link = Link->ForwardLink)
  {
    Volume = BASE_CR (Link, UFS_VOLUME, Link);
    /* Only dereference a pointer freshly returned for its original handle. */
    if (Volume->Io != NULL &&
        (EFI_ERROR (gBS->HandleProtocol (Volume->Handle, &gEfiBlockIoProtocolGuid, (VOID **)&Io)) ||
         Io != Volume->Io || Io->ReadBlocks != ProtectedRead || Io->Reset != ProtectedReset))
      return FALSE;
    if (Volume->Io2 != NULL && (EFI_ERROR (gBS->HandleProtocol (
                                    Volume->Handle, &gEfiBlockIo2ProtocolGuid, (VOID **)&Io2)) ||
                                Io2 != Volume->Io2 || Io2->ReadBlocksEx != ProtectedRead2 ||
                                Io2->Reset != ProtectedReset2))
      return FALSE;
  }
  return TRUE;
}

STATIC VOID PublishUfsReady (VOID);

STATIC VOID ProtectVolume (EFI_HANDLE Handle)
{
  EFI_BLOCK_IO_PROTOCOL *Io;
  EFI_BLOCK_IO2_PROTOCOL *Io2;
  UFS_VOLUME *Volume;
  LIST_ENTRY *Link;
  UINT64 Lun;
  if (mStopping || !UfsDevice (Handle, &Lun))
    return;
  if (!BindingsCurrent ())
  {
    StopSafely ();
    return;
  }
  Io = NULL;
  Io2 = NULL;
  Volume = NULL;
  gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&Io);
  gBS->HandleProtocol (Handle, &gEfiBlockIo2ProtocolGuid, (VOID **)&Io2);
  if (Io != NULL)
  {
    for (Link = mVolumes.ForwardLink; Link != &mVolumes; Link = Link->ForwardLink)
    {
      Volume = BASE_CR (Link, UFS_VOLUME, Link);
      if (Volume->Io == Io)
      {
        /* A new protocol object may reuse an old allocation's address.
           Never preserve its obsolete originals or wrap our own wrapper. */
        if (Volume->Handle != Handle || Volume->Lun != Lun || Io->ReadBlocks != ProtectedRead ||
            Io->Reset != ProtectedReset ||
            (Volume->Io2 != NULL && (Io2 != Volume->Io2 || Io2->ReadBlocksEx != ProtectedRead2 ||
                                     Io2->Reset != ProtectedReset2)))
        {
          StopSafely ();
          return;
        }
        break;
      }
      Volume = NULL;
    }
    if (Volume == NULL)
    {
      Volume = AllocateZeroPool (sizeof (*Volume));
      if (Volume != NULL)
      {
        if (Io->ReadBlocks == ProtectedRead || Io->Reset == ProtectedReset)
        {
          FreePool (Volume);
          StopSafely ();
          return;
        }
        Volume->Handle = Handle;
        Volume->Lun = Lun;
        Volume->Io = Io;
        Volume->OriginalRead = Io->ReadBlocks;
        Volume->OriginalReset = Io->Reset;
        InsertTailList (&mVolumes, &Volume->Link);
        Io->ReadBlocks = ProtectedRead;
        Io->Reset = ProtectedReset;
      }
      else
      {
        StopSafely ();
        return;
      }
    }
    Io->WriteBlocks = ProtectedWrite;
    Io->FlushBlocks = ProtectedFlush;
  }
  if (Io2 != NULL)
  {
    if (Volume != NULL && Volume->Io2 == NULL)
    {
      Volume->Io2 = Io2;
      Volume->OriginalRead2 = Io2->ReadBlocksEx;
      Volume->OriginalReset2 = Io2->Reset;
      Io2->ReadBlocksEx = ProtectedRead2;
      Io2->Reset = ProtectedReset2;
    }
    Io2->WriteBlocksEx = ProtectedWrite2;
    Io2->FlushBlocksEx = ProtectedFlush2;
  }
  if (Volume != NULL)
    UpdateReadOnly (Volume);
  if (Io != NULL && Volume != NULL && Io->Media->MediaPresent && Io->Media->BlockSize == 4096 &&
      !Io->Media->LogicalPartition)
  {
    PublishUfsReady ();
  }
}

/* Publish the ACPI/IoReady handoff once a 4Kn non-partition BlockIo exists.
   Media writes remain blocked. */
STATIC VOID PublishUfsReady (VOID)
{
  if (mReady.ControllerReady || mStopping)
    return;
  mReady.ControllerReady = TRUE;
  if (EFI_ERROR (gBS->InstallProtocolInterface (&mController, &gLiuqinUfsReadyProtocolGuid,
                                                EFI_NATIVE_INTERFACE, &mReady)))
  {
    mReady.ControllerReady = FALSE;
    return;
  }
}

STATIC VOID EFIAPI BlockNotify (EFI_EVENT Event, VOID *Context)
{
  EFI_HANDLE Handle;
  UINTN Size;
  VOID *Registration = (Context == (VOID *)1) ? mBlock2Registration : mBlockRegistration;
  for (;;)
  {
    Size = sizeof (Handle);
    if (EFI_ERROR (gBS->LocateHandle (ByRegisterNotify, NULL, Registration, &Size, &Handle)))
      break;
    ProtectVolume (Handle);
  }
}

STATIC EFI_STATUS EFIAPI ReadOnlyPass (EFI_EXT_SCSI_PASS_THRU_PROTOCOL *This, UINT8 *Target,
                                       UINT64 Lun,
                                       EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet,
                                       EFI_EVENT Event)
{
  UINT8 Upiu[32];
  UINT8 Direction;
  UINT32 TransferLength;
  EFI_STATUS Status;
  if (mStopping)
    return EFI_NOT_READY;
  if (This != mPass || Packet == NULL || Packet->Cdb == NULL || Packet->CdbLength == 0 ||
      Packet->CdbLength > 16)
    return EFI_INVALID_PARAMETER;
  /* Upstream uses the SCSI peripheral-addressing representation: the UFS
     common-LUN number is byte 1 of the UINT64, not its low byte. */
  if (Lun > 0x700 || (Lun & 0xFF) != 0)
    return EFI_WRITE_PROTECTED;
  if (Packet->DataDirection != EFI_EXT_SCSI_DATA_DIRECTION_READ || Packet->OutTransferLength != 0)
    return EFI_WRITE_PROTECTED;
  Direction = Packet->InTransferLength != 0 ? UfsDataIn : UfsNoData;
  TransferLength = Packet->InTransferLength;
  ZeroMem (Upiu, sizeof (Upiu));
  Upiu[0] = 1;
  Upiu[1] = Direction == UfsDataIn ? 0x41 : 1;
  Upiu[2] = (UINT8)(Lun >> 8);
  Upiu[12] = (UINT8)(TransferLength >> 24);
  Upiu[13] = (UINT8)(TransferLength >> 16);
  Upiu[14] = (UINT8)(TransferLength >> 8);
  Upiu[15] = (UINT8)TransferLength;
  CopyMem (Upiu + 16, Packet->Cdb, Packet->CdbLength);
  if (!LiuqinUfsReadOnlyUpiu (Upiu, sizeof (Upiu), Direction))
    return EFI_WRITE_PROTECTED;
  if (mStopping || mBusy)
    return EFI_NOT_READY;
  /* This inherited-controller profile deliberately serializes requests.
     ScsiBus supports a synchronous transport beneath its event-based API. */
  mBusy = TRUE;
  Status = mOriginalPass (This, Target, Lun, Packet, NULL);
  mBusy = FALSE;
  if (Status == EFI_TIMEOUT || !Idle ())
    StopSafely ();
  if (!EFI_ERROR (Status) && Event != NULL)
    gBS->SignalEvent (Event);
  return Status;
}

STATIC VOID EFIAPI PassNotify (EFI_EVENT Event, VOID *Context)
{
  EFI_EXT_SCSI_PASS_THRU_PROTOCOL *Pass;
  if (mStopping || mController == NULL)
    return;
  if (!EFI_ERROR (
          gBS->HandleProtocol (mController, &gEfiExtScsiPassThruProtocolGuid, (VOID **)&Pass)))
  {
    if (mPass != NULL)
    {
      if (mPass != Pass || Pass->PassThru != ReadOnlyPass)
        StopSafely ();
      return;
    }
    mPass = Pass;
    mOriginalPass = Pass->PassThru;
    Pass->PassThru = ReadOnlyPass;
    Pass->Mode->Attributes &= ~EFI_EXT_SCSI_PASS_THRU_ATTRIBUTES_NONBLOCKIO;
  }
  else if (mPass != NULL)
    StopSafely ();
}

STATIC VOID EFIAPI StopDma (EFI_EVENT Event, VOID *Context)
{
  UINTN Attempt;
  (VOID) Event;
  (VOID) Context;
  InvalidateBindings ();
  /* A notify callback cannot cancel EBS. Do not allocate memory or invoke
     console protocols after the memory map is terminated. Bound the proof at
     LIUQIN_UFS_EBS_QUIESCE_ATTEMPTS full quiesce attempts (each already waits
     <=100 ms) with one fixed inter-attempt gap. Recovery attempts are bounded;
     an unproven DMA stop must never return memory ownership to Windows. */
  for (Attempt = 0; Attempt < LIUQIN_UFS_EBS_QUIESCE_ATTEMPTS; ++Attempt)
  {
    if (LiuqinUfsQuiesce (PolicyRead, PolicyWrite, PolicyDelay, NULL))
    {
      /* Preserve PHY, clocks and HCE; memory may now be reclaimed safely. */
      return;
    }
    if (Attempt + 1U < LIUQIN_UFS_EBS_QUIESCE_ATTEMPTS)
    {
      MicroSecondDelay (LIUQIN_UFS_EBS_RETRY_DELAY_US);
    }
  }
  /* BootServices queue pages can be reclaimed by the OS after this callback.
     Retaining their EFI handles cannot preserve those pages. No console or
     BootServices dependency is safe here; stop before unsafe OS handoff. */
  CpuDeadLoop ();
}

STATIC EFI_STATUS ProbeSmmu (VOID)
{
  UINT32 Smr[LIUQIN_UFS_SMMU_MAX_GROUPS], S2cr[LIUQIN_UFS_SMMU_MAX_GROUPS], Cr0, Id0;
  UINTN Count, Index;
  BOOLEAN Unknown;
  Cr0 = MmioRead32 (SMMU_BASE);
  Id0 = MmioRead32 (SMMU_BASE + 0x20);
  Count = LiuqinUfsStreamCount (Cr0, Id0);
  if (Count == 0)
    return EFI_UNSUPPORTED;
  Unknown = FALSE;
  /* Only the hardware-reported groups exist. Scan every actual group so a
     final-slot translation or second masked match cannot be missed. */
  for (Index = 0; Index < Count; Index++)
  {
    Smr[Index] = MmioRead32 (SMMU_BASE + 0x800 + Index * 4);
    S2cr[Index] = MmioRead32 (SMMU_BASE + 0xC00 + Index * 4);
    if (Smr[Index] == MAX_UINT32 || S2cr[Index] == MAX_UINT32)
    {
      Unknown = TRUE;
    }
  }
  if (Unknown)
    return EFI_UNSUPPORTED;
  if (LiuqinUfsStreamBypass (Cr0, Id0, Smr, S2cr, Count, UFS_SID))
  {
    return EFI_SUCCESS;
  }

  {
    LIUQIN_UFS_SMMU_PROBE_IO Io = {NULL, S1Read};
    LIUQIN_UFS_SMMU_PROBE_SNAPSHOT Snapshot;
    LIUQIN_UFS_SMMU_PROBE_CANDIDATE Candidate;
    int Reason;

    Reason = LiuqinUfsSmmuTakeSnapshot (&Io, SMMU_BASE, 0x100000U, &Snapshot);
    if (Reason == 0)
    {
      Reason = LiuqinUfsSmmuClassify (&Snapshot, &Candidate);
    }

    if (Reason != LiuqinUfsSmmuEligibleForIsolatedProbe)
    {
      return EFI_UNSUPPORTED;
    }

    if ((Candidate.HistoricalContextFaults != 0) || (Candidate.HistoricalGlobalFaults != 0))
    {
      return EFI_UNSUPPORTED;
    }
  }

  return EFI_SUCCESS;
}

STATIC int S1Read (VOID *Context, LIUQIN_SMMU_U32 Offset, LIUQIN_SMMU_U32 *Value)
{
  (VOID) Context;
  if (Value == NULL || (Offset & 3U) != 0 || Offset > 0x100000U - 4U)
    return 0;
  *Value = MmioRead32 (SMMU_BASE + Offset);
  return 1;
}
/* ------------------------------------------------------------------ */
/* Device-side wake on the Device W-LUN.                              */
/*                                                                    */

/* (Palima DXE BSP `ufs_bsp_core.c` forces core_skip = 1 on silicon)   */
/* and its open path still sends TEST UNIT READY and then START STOP   */
/* UNIT with power condition ACTIVE (CDB 1B 00 00 00 10 00) to the     */
/* Device W-LUN 0xD0 before any other request (UFS.c `ufs_open`;       */
/* ufs_api.c:298,336; ufs_memory.c:691-796). That driver also powers   */
/* the device down at ExitBootServices (UFS.c EBS callback, SSU        */
/* power condition 3), so an inherited device can be in a non-active   */
/* power mode where queries return 0xFF General Failure and media      */

/* completes. The locked upstream never sends SSU, so this firmware    */
/* reproduces the vendor's wake before handing the controller over.    */
/* Only these fixed, data-less requests are allowed; no medium access, */
/* no SLEEP/POWERDOWN, no UIC, no PHY/clock/ICE change.                */

#define LIUQIN_UFS_WAKE_UPIU_OFFSET 256U
#define LIUQIN_UFS_WAKE_RESP_OFFSET 32U
#define LIUQIN_UFS_WAKE_TASK_TAG 0xA6U
#define LIUQIN_UFS_WAKE_POLLS 2000U
#define LIUQIN_UFS_WAKE_POLL_US 100U
#define LIUQIN_UFS_WAKE_STEPS 6U

/* bCurrentPowerMode values (IDN 02h, read-only). */
#define LIUQIN_UFS_POWER_ACTIVE 0x11U
#define LIUQIN_UFS_POWER_SLEEP 0x22U
#define LIUQIN_UFS_POWER_DOWN 0x33U

typedef struct
{
  UINT32 Requests;
  UINT32 Ocs[LIUQIN_UFS_WAKE_STEPS];
  UINT8 Resp[LIUQIN_UFS_WAKE_STEPS][8];
  UINT32 Value[LIUQIN_UFS_WAKE_STEPS];
  BOOLEAN Completed[LIUQIN_UFS_WAKE_STEPS];
  BOOLEAN Good[LIUQIN_UFS_WAKE_STEPS];
  UINT32 TurPostAttempts;
  UINT32 FailStep;
  UINT32 FailStage;
  EFI_STATUS FailStatus;
  UINT64 HostAddress;
  UINT64 DeviceAddress;
  BOOLEAN ModeBeforeValid;
  BOOLEAN ModeAfterValid;
  UINT32 ModeBefore;
  UINT32 ModeAfter;
  BOOLEAN Ready;
} LIUQIN_UFS_WAKE_REPORT;

STATIC LIUQIN_UFS_WAKE_REPORT mWake;
STATIC VOID *mWakeHost;
STATIC UINT64 mWakeDevice;

/* Why the wake ladder stopped. Only NONE permits any request. */
#define LIUQIN_UFS_WAKE_FAIL_NONE 0U
#define LIUQIN_UFS_WAKE_FAIL_ALLOC 1U
#define LIUQIN_UFS_WAKE_FAIL_ALLOWED 2U
#define LIUQIN_UFS_WAKE_FAIL_MAP 3U
#define LIUQIN_UFS_WAKE_FAIL_QUIESCE1 4U
#define LIUQIN_UFS_WAKE_FAIL_LBA 5U
#define LIUQIN_UFS_WAKE_FAIL_LBAU 6U
#define LIUQIN_UFS_WAKE_FAIL_RSR 7U
#define LIUQIN_UFS_WAKE_FAIL_DBR 8U
#define LIUQIN_UFS_WAKE_FAIL_UCD 9U
#define LIUQIN_UFS_WAKE_FAIL_QUIESCE2 10U

/* Step indices. */
#define LIUQIN_UFS_WAKE_STEP_MODE_PRE 0U
#define LIUQIN_UFS_WAKE_STEP_TUR_PRE 1U
#define LIUQIN_UFS_WAKE_STEP_SSU 2U
#define LIUQIN_UFS_WAKE_STEP_MODE_POST 3U
#define LIUQIN_UFS_WAKE_STEP_TUR_POST 4U
#define LIUQIN_UFS_WAKE_STEP_FLAG 5U

STATIC VOID WakeCopyResponse (UINT8 Out[8], UINT32 *Ocs)
{
  UINT8 *Ucd = (UINT8 *)(UINTN)mWakeHost + LIUQIN_UFS_WAKE_UPIU_OFFSET;

  ZeroMem (Out, 8);
  CopyMem (Out, Ucd + LIUQIN_UFS_WAKE_RESP_OFFSET, 8);
  *Ocs = ((UTP_TRD *)(UINTN)mWakeHost)->Ocs;
}

/* One fixed no-data request through the same write gates as every other
   transfer: UTRLBA/UTRLRSR/UTRLDBR go through the protocol, so the slot
   validator re-checks CT/OCS/UPIU/RuO/RuL and the read-only allowlist. */
STATIC BOOLEAN WakeRequest (UINT32 Step, CONST UINT8 *Upiu, UINT8 Response[8], UINT32 *Ocs)
{
  UTP_TRD *Trd = (UTP_TRD *)(UINTN)mWakeHost;
  UINT8 *Ucd = (UINT8 *)(UINTN)mWakeHost + LIUQIN_UFS_WAKE_UPIU_OFFSET;
  UINT32 Value;
  UINT32 Poll;
  EFI_STATUS Status;
  BOOLEAN Completed = FALSE;

  if (mWakeHost == NULL || mWakeDevice == 0)
  {
    return FALSE;
  }

  mWake.FailStep = Step;
  ZeroMem (mWakeHost, EFI_PAGE_SIZE);
  ZeroMem (Trd, sizeof (*Trd));
  CopyMem (Ucd, Upiu, 32);
  Trd->Int = UFS_INTERRUPT_COMMAND;
  Trd->Ct = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs = UFS_HC_TRD_OCS_INIT_VALUE;
  /* The UTRD's UCD base address must point at the command UPIU itself, not at
     the page start: the write gate reads the UPIU from that address, and the
     controller fetches the command from there. LIUQIN_UFS_WAKE_UPIU_OFFSET is
     128-byte aligned, which the 25-bit UCD field (address >> 7) requires. */
  Trd->UcdBa = (UINT32)((mWakeDevice + LIUQIN_UFS_WAKE_UPIU_OFFSET) >> 7);
  Trd->UcdBaU = (UINT32)((mWakeDevice + LIUQIN_UFS_WAKE_UPIU_OFFSET) >> 32);
  Trd->RuO = LIUQIN_UFS_WAKE_RESP_OFFSET / sizeof (UINT32);
  Trd->RuL = 8;
  Trd->PrdtO = 16;
  MemoryFence ();
  /* Fail loudly instead of submitting a descriptor whose UCD base does not
     name this request: that is exactly how the first wake attempt was
     rejected by the write gate with EFI_WRITE_PROTECTED. */
  if (CompareMem ((UINT8 *)(UINTN)(((UINT64)Trd->UcdBaU << 32) | ((UINT64)Trd->UcdBa << 7)), Upiu,
                  32) != 0)
  {
    mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_UCD;
    mWake.FailStatus = EFI_DEVICE_ERROR;
    return FALSE;
  }

  if (!LiuqinUfsQuiesce (PolicyRead, PolicyWrite, PolicyDelay, NULL))
  {
    mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_QUIESCE1;
    mWake.FailStatus = EFI_DEVICE_ERROR;
    return FALSE;
  }

  Value = (UINT32)mWakeDevice;
  Status = mHost.Write (&mHost, EfiUfsHcWidthUint32, UFS_HC_UTRLBA_OFFSET, 1, &Value);
  if (EFI_ERROR (Status))
  {
    mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_LBA;
    mWake.FailStatus = Status;
    return FALSE;
  }

  Value = (UINT32)(mWakeDevice >> 32);
  Status = mHost.Write (&mHost, EfiUfsHcWidthUint32, UFS_HC_UTRLBAU_OFFSET, 1, &Value);
  if (EFI_ERROR (Status))
  {
    mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_LBAU;
    mWake.FailStatus = Status;
    return FALSE;
  }

  Value = UFS_HC_UTRLRSR;
  Status = mHost.Write (&mHost, EfiUfsHcWidthUint32, UFS_HC_UTRLRSR_OFFSET, 1, &Value);
  if (EFI_ERROR (Status))
  {
    mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_RSR;
    mWake.FailStatus = Status;
    return FALSE;
  }

  Value = 1;
  Status = mHost.Write (&mHost, EfiUfsHcWidthUint32, UFS_HC_UTRLDBR_OFFSET, 1, &Value);
  if (EFI_ERROR (Status))
  {
    mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_DBR;
    mWake.FailStatus = Status;
    return FALSE;
  }

  ++mWake.Requests;
  for (Poll = 0; Poll < LIUQIN_UFS_WAKE_POLLS; ++Poll)
  {
    Value = ReadReg (UFS_HC_UTRLDBR_OFFSET);
    if (Value == 0)
    {
      Completed = TRUE;
      break;
    }

    if (Value != 1)
    {
      break;
    }

    if (Poll + 1 < LIUQIN_UFS_WAKE_POLLS)
    {
      MicroSecondDelay (LIUQIN_UFS_WAKE_POLL_US);
    }
  }

  if (!LiuqinUfsQuiesce (PolicyRead, PolicyWrite, PolicyDelay, NULL))
  {
    mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_QUIESCE2;
    mWake.FailStatus = EFI_DEVICE_ERROR;
    StopSafely ();
    return FALSE;
  }

  if (!Completed)
  {
    ZeroMem (Response, 8);
    *Ocs = MAX_UINT32;
    return TRUE; /* Bounded timeout: report, never retry. */
  }

  MemoryFence ();
  WakeCopyResponse (Response, Ocs);
  return TRUE;
}

STATIC VOID WakeRunStep (UINT32 Step, CONST UINT8 *Upiu, BOOLEAN Query)
{
  UINT8 Value[4];

  if (!WakeRequest (Step, Upiu, mWake.Resp[Step], &mWake.Ocs[Step]))
  {
    return;
  }

  mWake.Completed[Step] = TRUE;
  if (Query)
  {
    ZeroMem (Value, sizeof (Value));
    CopyMem (Value,
             (UINT8 *)(UINTN)mWakeHost + LIUQIN_UFS_WAKE_UPIU_OFFSET + LIUQIN_UFS_WAKE_RESP_OFFSET +
                 20,
             sizeof (Value));
    mWake.Value[Step] =
        ((UINT32)Value[0] << 24) | ((UINT32)Value[1] << 16) | ((UINT32)Value[2] << 8) | Value[3];
    mWake.Good[Step] = (BOOLEAN)(mWake.Resp[Step][0] == 0x36 && mWake.Resp[Step][6] == 0x00);
  }
  else
  {
    mWake.Good[Step] = (BOOLEAN)(mWake.Resp[Step][0] == 0x21 && mWake.Resp[Step][6] == 0x00 &&
                                 mWake.Resp[Step][7] == 0x00);
  }
}

STATIC VOID UfsWakeDevice (VOID)
{
  UINT8 Upiu[32];
  UINT32 Attempt;

  ZeroMem (&mWake, sizeof (mWake));
  if (mWakeHost == NULL || mWakeDevice == 0)
  {
    return;
  }

  /* 1. READ ATTRIBUTE bCurrentPowerMode (IDN 02h, index/selector 0): the only
        read that can name the inherited power mode. 0x11 Active, 0x22 Sleep,
        0x33 PowerDown. */
  ZeroMem (Upiu, sizeof (Upiu));
  Upiu[0] = 0x16;
  Upiu[3] = LIUQIN_UFS_WAKE_TASK_TAG;
  Upiu[5] = 0x01;
  Upiu[12] = 0x03;
  Upiu[13] = 0x02;
  WakeRunStep (LIUQIN_UFS_WAKE_STEP_MODE_PRE, Upiu, TRUE);
  if (mWake.Completed[LIUQIN_UFS_WAKE_STEP_MODE_PRE])
  {
    mWake.ModeBeforeValid = mWake.Good[LIUQIN_UFS_WAKE_STEP_MODE_PRE];
    mWake.ModeBefore = mWake.Value[LIUQIN_UFS_WAKE_STEP_MODE_PRE];
  }

  /* 2. TEST UNIT READY on the Device W-LUN (vendor open path). */
  ZeroMem (Upiu, sizeof (Upiu));
  Upiu[0] = 0x01;
  Upiu[1] = 0x01;
  Upiu[2] = 0xD0;
  Upiu[3] = LIUQIN_UFS_WAKE_TASK_TAG + 1U;
  WakeRunStep (LIUQIN_UFS_WAKE_STEP_TUR_PRE, Upiu, FALSE);

  /* 3. START STOP UNIT, power condition ACTIVE (1 << 4). This is the only
        device power-mode change; SLEEP (0x20) and POWERDOWN (0x30) are not
        reachable through this allowlist. */
  Upiu[3] = LIUQIN_UFS_WAKE_TASK_TAG + 5U;
  Upiu[16] = 0x1B;
  Upiu[20] = 0x10;
  WakeRunStep (LIUQIN_UFS_WAKE_STEP_SSU, Upiu, FALSE);

  /* 4. Re-read bCurrentPowerMode to prove the transition. */
  ZeroMem (Upiu, sizeof (Upiu));
  Upiu[0] = 0x16;
  Upiu[3] = LIUQIN_UFS_WAKE_TASK_TAG + 2U;
  Upiu[5] = 0x01;
  Upiu[12] = 0x03;
  Upiu[13] = 0x02;
  WakeRunStep (LIUQIN_UFS_WAKE_STEP_MODE_POST, Upiu, TRUE);
  if (mWake.Completed[LIUQIN_UFS_WAKE_STEP_MODE_POST])
  {
    mWake.ModeAfterValid = mWake.Good[LIUQIN_UFS_WAKE_STEP_MODE_POST];
    mWake.ModeAfter = mWake.Value[LIUQIN_UFS_WAKE_STEP_MODE_POST];
  }

  /* 5. TEST UNIT READY again: the wake is only credible if the device now
        answers with a GOOD status. A Pre-Active transition can take a few
        milliseconds, so this is a bounded poll (three attempts, 20 ms apart),
        never an unbounded retry. */
  MicroSecondDelay (20000);
  for (Attempt = 0; Attempt < 3; ++Attempt)
  {
    ZeroMem (Upiu, sizeof (Upiu));
    Upiu[0] = 0x01;
    Upiu[1] = 0x01;
    Upiu[2] = 0xD0;
    Upiu[3] = LIUQIN_UFS_WAKE_TASK_TAG + 3U;
    WakeRunStep (LIUQIN_UFS_WAKE_STEP_TUR_POST, Upiu, FALSE);
    ++mWake.TurPostAttempts;
    if (mWake.Good[LIUQIN_UFS_WAKE_STEP_TUR_POST])
    {
      break;
    }

    if (Attempt + 1 < 3)
    {
      MicroSecondDelay (20000);
    }
  }

  /* 6. READ FLAG fDeviceInit: 0 or 1 proves the device UTP layer answers. */
  ZeroMem (Upiu, sizeof (Upiu));
  Upiu[0] = 0x16;
  Upiu[3] = LIUQIN_UFS_WAKE_TASK_TAG + 4U;
  Upiu[5] = 0x01;
  Upiu[12] = 0x05;
  Upiu[13] = 0x01;
  WakeRunStep (LIUQIN_UFS_WAKE_STEP_FLAG, Upiu, TRUE);

  mWake.Ready =
      (BOOLEAN)(mWake.Good[LIUQIN_UFS_WAKE_STEP_TUR_POST] || mWake.Good[LIUQIN_UFS_WAKE_STEP_FLAG]);
}

EFI_STATUS EFIAPI UfsHostDxeEntryPoint (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  VOID *Hob;
  LIUQIN_HANDOFF *Handoff;
  EFI_EVENT ExitEvent;
  EFI_PEI_HOB_POINTERS Hobs;
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, CheckUfsBinding, NULL,
                               &gEfiEventReadyToBootGuid, &ExitEvent);
  if (EFI_ERROR (Status))
    return Status;
  mCallbacksInstalled = TRUE;
  Hob = GetFirstGuidHob (&gLiuqinHandoffGuid);
  if (Hob == NULL || GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (*Handoff))
    return InitializationResult (EFI_NOT_FOUND);
  Handoff = GET_GUID_HOB_DATA (Hob);
  if (Handoff->Version != LIUQIN_HANDOFF_VERSION || Handoff->DtbSize < 40 ||
      Handoff->DtbSize > LIUQIN_DTB_MAX ||
      !LiuqinUfsRange (FixedPcdGet64 (PcdUefiMemoryBase), FixedPcdGet32 (PcdUefiMemorySize),
                       Handoff->DtbBase, Handoff->DtbSize) ||
      LiuqinValidateDtb ((VOID *)(UINTN)Handoff->DtbBase, (LIUQIN_SIZE)Handoff->DtbSize) != 0 ||
      LiuqinCheckIdentity ((VOID *)(UINTN)Handoff->DtbBase) != 0 ||
      !LiuqinUfsRuntimeDescription ((VOID *)(UINTN)Handoff->DtbBase))
  {
    return InitializationResult (EFI_UNSUPPORTED);
  }
  Hob = GetFirstGuidHob (&gLiuqinMemoryPlanGuid);
  if (Hob != NULL && GET_GUID_HOB_DATA_SIZE (Hob) == sizeof (*mMemoryPlan))
  {
    mMemoryPlan = GET_GUID_HOB_DATA (Hob);
  }
  /* ScsiDisk MODE SENSE and INQUIRY may use stack buffers. Accept only the
     exact SEC stack allocation already covered by BootContract's FD proof. */
  Hobs.Raw = GetHobList ();
  while (!END_OF_HOB_LIST (Hobs))
  {
    if (GET_HOB_TYPE (Hobs) == EFI_HOB_TYPE_MEMORY_ALLOCATION &&
        GET_HOB_LENGTH (Hobs) >= sizeof (EFI_HOB_MEMORY_ALLOCATION_STACK) &&
        CompareGuid (&Hobs.MemoryAllocationStack->AllocDescriptor.Name,
                     &gEfiHobMemoryAllocStackGuid) &&
        Hobs.MemoryAllocationStack->AllocDescriptor.MemoryLength ==
            FixedPcdGet32 (PcdPrePiStackSize) &&
        Hobs.MemoryAllocationStack->AllocDescriptor.MemoryBaseAddress ==
            FixedPcdGet64 (PcdUefiMemoryBase) - FixedPcdGet32 (PcdPrePiStackSize))
    {
      mStackBase = Hobs.MemoryAllocationStack->AllocDescriptor.MemoryBaseAddress;
      mStackSize = Hobs.MemoryAllocationStack->AllocDescriptor.MemoryLength;
      break;
    }
    Hobs.Raw = GET_NEXT_HOB (Hobs);
  }
  /* Read the verified GCC page first; never probe an unpowered or
     gated UFS HCI. This does not program/enable any power rail or clock. */
  if (!LiuqinUfsClocksLive (PolicyRead, NULL, mClocks))
  {
    return InitializationResult (EFI_NOT_READY);
  }
  if (ReadReg (UFS_HC_CAP_OFFSET) == MAX_UINT32 || ReadReg (UFS_HC_VER_OFFSET) == 0 ||
      (ReadReg (UFS_HC_ENABLE_OFFSET) & UFS_HC_HCE_EN) == 0 ||
      (ReadReg (UFS_HC_STATUS_OFFSET) & (UFS_HC_HCS_DP | BIT1)) != (UFS_HC_HCS_DP | BIT1) ||
      !Idle ())
  {
    return InitializationResult (EFI_NOT_READY);
  }
  Status = ProbeSmmu ();

  if (EFI_ERROR (Status))
    return InitializationResult (Status);
  /* Quiesce inherited queues after all read-only gates pass. Do not change
     HCE, PHY/clock/ICE, SMMU or persistent UFS configuration. */
  if (!LiuqinUfsQuiesce (PolicyRead, PolicyWrite, PolicyDelay, NULL))
  {
    StopSafely ();
    return InitializationResult (EFI_DEVICE_ERROR);
  }
  /* Wake the Device W-LUN exactly like the stock firmware's inherited path
     before the upstream driver binds. Bounded: four fixed no-data requests,
     no retry, no medium access. A failed wake is reported, not hidden. */
  {
    UINTN WakeBytes = EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS WakeDevice = 0;
    VOID *WakeMapping = NULL;
    Status = DmaAllocateBuffer (EfiBootServicesData, 1, &mWakeHost);
    if (EFI_ERROR (Status))
    {
      mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_ALLOC;
      mWake.FailStatus = Status;
    }
    else
    {
      mWake.HostAddress = (UINT64)(UINTN)mWakeHost;
      if (!AllowedBuffer ((UINTN)mWakeHost, EFI_PAGE_SIZE, TRUE))
      {
        mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_ALLOWED;
        mWake.FailStatus = EFI_SECURITY_VIOLATION;
      }
      else
      {
        Status = mHost.Map (&mHost, EdkiiUfsHcOperationBusMasterCommonBuffer, mWakeHost, &WakeBytes,
                            &WakeDevice, &WakeMapping);
        if (EFI_ERROR (Status))
        {
          mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_MAP;
          mWake.FailStatus = Status;
        }
      }
    }

    if ((mWake.FailStage == LIUQIN_UFS_WAKE_FAIL_NONE) && (WakeDevice != 0))
    {
      mWakeDevice = WakeDevice;
      mWake.DeviceAddress = WakeDevice;
      UfsWakeDevice ();
    }
    else
    {
      if (mWake.FailStage == LIUQIN_UFS_WAKE_FAIL_NONE)
      {
        mWake.FailStage = LIUQIN_UFS_WAKE_FAIL_MAP;
        mWake.FailStatus = EFI_DEVICE_ERROR;
      }

      mWakeHost = NULL;
      mWakeDevice = 0;
    }
  }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, StopDma, NULL,
                               &gEfiEventExitBootServicesGuid, &ExitEvent);
  if (EFI_ERROR (Status))
    return InitializationResult (Status);
  EfiCreateProtocolNotifyEvent (&gEfiBlockIoProtocolGuid, TPL_CALLBACK, BlockNotify, NULL,
                                &mBlockRegistration);
  EfiCreateProtocolNotifyEvent (&gEfiBlockIo2ProtocolGuid, TPL_CALLBACK, BlockNotify, (VOID *)1,
                                &mBlock2Registration);
  EfiCreateProtocolNotifyEvent (&gEfiExtScsiPassThruProtocolGuid, TPL_CALLBACK, PassNotify, NULL,
                                &mPassRegistration);
  Status = gBS->InstallMultipleProtocolInterfaces (
      &mController, &gEfiDevicePathProtocolGuid, &mPath, &gEdkiiUfsHostControllerProtocolGuid,
      &mHost, &gEdkiiUfsHcPlatformProtocolGuid, &mPlatform, NULL);
  if (EFI_ERROR (Status))
    return InitializationResult (Status);
  return InitializationResult (EFI_SUCCESS);
}
