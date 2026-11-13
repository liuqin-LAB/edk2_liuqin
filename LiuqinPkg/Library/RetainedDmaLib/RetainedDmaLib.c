/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DmaLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Cpu.h>
#include "RetainedDmaLib.h"

#define DMA_CACHE_MASK                                                                             \
  (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB | EFI_MEMORY_UCE)
#define DMA_PAGE_COMMON_LIVE 1U
#define DMA_PAGE_COMMON_RETIRED 2U
#define DMA_PAGE_DATA 3U

typedef struct
{
  UINTN Base;
  UINTN Bytes;
  BOOLEAN Common;
} DMA_ALLOCATION;

typedef struct
{
  VOID *Host;
  VOID *Buffer;
  UINTN Bytes;
  DMA_MAP_OPERATION Operation;
  BOOLEAN Active;
} DMA_MAPPING;

STATIC EFI_CPU_ARCH_PROTOCOL *mCpu;
STATIC LIUQIN_RETAINED_DMA_STATISTICS mStatistics;
STATIC DMA_ALLOCATION mAllocations[LIUQIN_RETAINED_DMA_MAX_RECORDS];
STATIC DMA_MAPPING mMappings[LIUQIN_RETAINED_DMA_MAX_RECORDS];
STATIC UINT16 mPageOwner[LIUQIN_RETAINED_DMA_MAX_RECORDS];
STATIC UINT8 mPageState[LIUQIN_RETAINED_DMA_MAX_RECORDS];
STATIC EFI_PHYSICAL_ADDRESS mAddressLimit;
STATIC BOOLEAN mInitialized;

STATIC BOOLEAN ValidRange (UINTN Base, UINTN Bytes)
{
  return (BOOLEAN)((Bytes != 0) && (Base <= MAX_UINTN - (Bytes - 1U)));
}

STATIC BOOLEAN RangesOverlap (UINTN First, UINTN FirstBytes, UINTN Second, UINTN SecondBytes)
{
  return (BOOLEAN)((First <= Second) ? (Second - First < FirstBytes)
                                     : (First - Second < SecondBytes));
}

/* Walk the whole range: checking only its first GCD descriptor is insufficient.
   The bound also prevents a malformed descriptor from producing an endless walk. */
STATIC EFI_STATUS CheckUncachedRange (UINTN Base, UINTN Bytes, BOOLEAN CapabilitiesOnly)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  EFI_STATUS Status;
  UINTN Cursor;
  UINTN Remaining;
  UINTN Part;
  UINTN Index;
  UINT64 DescriptorEnd;

  Cursor = Base;
  Remaining = Bytes;
  for (Index = 0; (Remaining != 0) && (Index < LIUQIN_RETAINED_DMA_MAX_RECORDS); ++Index)
  {
    Status = gDS->GetMemorySpaceDescriptor (Cursor, &Descriptor);
    if (EFI_ERROR (Status))
    {
      return Status;
    }
    if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeSystemMemory) ||
        (Descriptor.BaseAddress > Cursor) || (Descriptor.Length == 0) ||
        (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length))
    {
      return EFI_DEVICE_ERROR;
    }
    DescriptorEnd = Descriptor.BaseAddress + Descriptor.Length;
    if ((DescriptorEnd <= Cursor) || ((Descriptor.Capabilities & EFI_MEMORY_WC) == 0))
    {
      return EFI_UNSUPPORTED;
    }
    if (!CapabilitiesOnly && (((Descriptor.Capabilities & EFI_MEMORY_XP) == 0) ||
                              ((Descriptor.Attributes & DMA_CACHE_MASK) != EFI_MEMORY_WC) ||
                              ((Descriptor.Attributes & EFI_MEMORY_XP) == 0)))
    {
      return EFI_DEVICE_ERROR;
    }
    Part = Remaining;
    if ((UINT64)Part > DescriptorEnd - Cursor)
    {
      Part = (UINTN)(DescriptorEnd - Cursor);
    }
    Remaining -= Part;
    Cursor += Part;
  }
  return (Remaining == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/* The platform HOB may advertise cache types without advertising XP. Follow
   the locked NonCoherentDmaLib's capability extension, but require success. */
STATIC EFI_STATUS EnableExecuteProtection (UINTN Base, UINTN Bytes)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  EFI_STATUS Status;
  UINTN Part;
  UINTN Index;
  UINT64 End;

  for (Index = 0; (Bytes != 0) && (Index < LIUQIN_RETAINED_DMA_MAX_RECORDS); ++Index)
  {
    Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
    if (EFI_ERROR (Status))
    {
      return Status;
    }
    if ((Descriptor.BaseAddress > Base) || (Descriptor.Length == 0) ||
        (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length))
    {
      return EFI_DEVICE_ERROR;
    }
    End = Descriptor.BaseAddress + Descriptor.Length;
    if (End <= Base)
    {
      return EFI_DEVICE_ERROR;
    }
    Part = Bytes;
    if ((UINT64)Part > End - Base)
    {
      Part = (UINTN)(End - Base);
    }
    if ((Descriptor.Capabilities & EFI_MEMORY_XP) == 0)
    {
      Status =
          gDS->SetMemorySpaceCapabilities (Base, Part, Descriptor.Capabilities | EFI_MEMORY_XP);
      if (EFI_ERROR (Status))
      {
        return Status;
      }
    }
    Base += Part;
    Bytes -= Part;
  }
  return (Bytes == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS EnsureArena (VOID)
{
  EFI_PHYSICAL_ADDRESS Address;
  EFI_STATUS Status;

  if (mStatistics.ReservedBytes != 0)
  {
    return mStatistics.ArenaStatus;
  }
  Address = mAddressLimit;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiACPIMemoryNVS,
                               LIUQIN_RETAINED_DMA_MAX_RECORDS, &Address);
  if (EFI_ERROR (Status))
  {
    mStatistics.ArenaStatus = Status;
    return Status;
  }
  /* Even a malformed allocator result is retained; never fall back or free it. */
  mStatistics.ArenaBase = Address;
  mStatistics.ReservedBytes = LIUQIN_RETAINED_DMA_BUDGET;
  mStatistics.ArenaStatus = EFI_DEVICE_ERROR;
  if ((Address == 0) || ((Address & EFI_PAGE_MASK) != 0) || (Address > mAddressLimit) ||
      (LIUQIN_RETAINED_DMA_BUDGET - 1U > mAddressLimit - Address) ||
      (Address > MAX_UINTN - LIUQIN_RETAINED_DMA_BUDGET))
  {
    return mStatistics.ArenaStatus;
  }
  Status = CheckUncachedRange ((UINTN)Address, LIUQIN_RETAINED_DMA_BUDGET, TRUE);
  if (!EFI_ERROR (Status))
  {
    Status = EnableExecuteProtection ((UINTN)Address, LIUQIN_RETAINED_DMA_BUDGET);
  }
  if (!EFI_ERROR (Status))
  {
    /* Eliminate dirty aliases before changing the page attributes. */
    Status = mCpu->FlushDataCache (mCpu, Address, LIUQIN_RETAINED_DMA_BUDGET,
                                   EfiCpuFlushTypeWriteBackInvalidate);
  }
  if (!EFI_ERROR (Status))
  {
    /* Locked Arm CpuDxe maps WC to Normal Non-Cacheable; UC maps to Device
       memory and can fault on unaligned generic CopyMem/common-buffer access. */
    Status = gDS->SetMemorySpaceAttributes (Address, LIUQIN_RETAINED_DMA_BUDGET,
                                            EFI_MEMORY_WC | EFI_MEMORY_XP);
  }
  if (!EFI_ERROR (Status))
  {
    Status = CheckUncachedRange ((UINTN)Address, LIUQIN_RETAINED_DMA_BUDGET, FALSE);
  }
  mStatistics.ArenaStatus = Status;
  return Status;
}

STATIC EFI_STATUS AllocateRetained (UINTN Pages, UINTN Alignment, BOOLEAN Common, VOID **Buffer)
{
  EFI_STATUS Status;
  DMA_ALLOCATION *Allocation;
  UINTN Start;
  UINTN Address;
  UINTN Bytes;
  UINTN Page;
  UINTN FirstPage;

  if ((Pages == 0) || (Pages > LIUQIN_RETAINED_DMA_MAX_RECORDS) ||
      (mStatistics.AllocationRecords == LIUQIN_RETAINED_DMA_MAX_RECORDS))
  {
    return EFI_OUT_OF_RESOURCES;
  }
  Bytes = EFI_PAGES_TO_SIZE (Pages);
  if (Alignment < EFI_PAGE_SIZE)
  {
    Alignment = EFI_PAGE_SIZE;
  }
  if ((Alignment > LIUQIN_RETAINED_DMA_BUDGET) ||
      (Bytes > LIUQIN_RETAINED_DMA_BUDGET - mStatistics.ConsumedBytes))
  {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = EnsureArena ();
  if (EFI_ERROR (Status))
  {
    return Status;
  }
  Address = (UINTN)mStatistics.ArenaBase + mStatistics.ConsumedBytes;
  Address = (Address + Alignment - 1U) & ~(Alignment - 1U);
  Start = Address - (UINTN)mStatistics.ArenaBase;
  if ((Start > LIUQIN_RETAINED_DMA_BUDGET) || (Bytes > LIUQIN_RETAINED_DMA_BUDGET - Start))
  {
    return EFI_OUT_OF_RESOURCES;
  }
  /* Attribute changes by another driver are an error, including for data maps. */
  Status = CheckUncachedRange (Address, Bytes, FALSE);
  if (EFI_ERROR (Status))
  {
    return Status;
  }
  Allocation = &mAllocations[mStatistics.AllocationRecords];
  Allocation->Base = Address;
  Allocation->Bytes = Bytes;
  Allocation->Common = Common;
  ++mStatistics.AllocationRecords;
  FirstPage = Start / EFI_PAGE_SIZE;
  for (Page = 0; Page < Pages; ++Page)
  {
    mPageOwner[FirstPage + Page] = (UINT16)mStatistics.AllocationRecords;
    mPageState[FirstPage + Page] = Common ? DMA_PAGE_COMMON_LIVE : DMA_PAGE_DATA;
  }
  mStatistics.ConsumedBytes = Start + Bytes;
  if (Common)
  {
    mStatistics.LiveCommonPages += Pages;
  }
  ZeroMem ((VOID *)Address, Bytes);
  *Buffer = (VOID *)Address;
  return EFI_SUCCESS;
}

/* A range may be a subrange of one allocation, but it cannot include a retired
   page, cross allocations, or name arena padding/data bounce memory. */
STATIC BOOLEAN LiveCommonRange (UINTN Base, UINTN Bytes)
{
  UINTN First;
  UINTN Last;
  UINTN Page;
  UINT16 Owner;
  DMA_ALLOCATION *Allocation;

  if (!ValidRange (Base, Bytes) || (mStatistics.ReservedBytes == 0) ||
      (Base < mStatistics.ArenaBase) ||
      (Base - mStatistics.ArenaBase >= LIUQIN_RETAINED_DMA_BUDGET) ||
      (Bytes > LIUQIN_RETAINED_DMA_BUDGET - (Base - (UINTN)mStatistics.ArenaBase)))
  {
    return FALSE;
  }
  First = (Base - (UINTN)mStatistics.ArenaBase) / EFI_PAGE_SIZE;
  Last = (Base - (UINTN)mStatistics.ArenaBase + Bytes - 1U) / EFI_PAGE_SIZE;
  Owner = mPageOwner[First];
  if ((Owner == 0) || (Owner > mStatistics.AllocationRecords))
  {
    return FALSE;
  }
  Allocation = &mAllocations[Owner - 1U];
  if (!Allocation->Common || (Base < Allocation->Base) ||
      (Base - Allocation->Base >= Allocation->Bytes) ||
      (Bytes > Allocation->Bytes - (Base - Allocation->Base)))
  {
    return FALSE;
  }
  for (Page = First; Page <= Last; ++Page)
  {
    if ((mPageOwner[Page] != Owner) || (mPageState[Page] != DMA_PAGE_COMMON_LIVE))
    {
      return FALSE;
    }
  }
  return TRUE;
}

EFI_STATUS
EFIAPI
DmaMap (DMA_MAP_OPERATION Operation, VOID *HostAddress, UINTN *NumberOfBytes,
        PHYSICAL_ADDRESS *DeviceAddress, VOID **Mapping)
{
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  DMA_MAPPING *Map;
  VOID *Buffer;
  UINTN Bytes;

  if (DeviceAddress != NULL)
  {
    *DeviceAddress = 0;
  }
  if (Mapping != NULL)
  {
    *Mapping = NULL;
  }
  if (!mInitialized)
  {
    return EFI_NOT_READY;
  }
  if (mStatistics.Sealed)
  {
    return EFI_ACCESS_DENIED;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = EFI_INVALID_PARAMETER;
  if ((HostAddress == NULL) || (NumberOfBytes == NULL) || (DeviceAddress == NULL) ||
      (Mapping == NULL) || ((UINT32)Operation >= (UINT32)MapOperationMaximum))
  {
    goto Done;
  }
  Bytes = *NumberOfBytes;
  if (!ValidRange ((UINTN)HostAddress, Bytes))
  {
    goto Done;
  }
  if (mStatistics.MappingRecords == LIUQIN_RETAINED_DMA_MAX_RECORDS)
  {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }
  if (Operation == MapOperationBusMasterCommonBuffer)
  {
    Status = EFI_UNSUPPORTED;
    if (!LiveCommonRange ((UINTN)HostAddress, Bytes))
    {
      goto Done;
    }
    Status = CheckUncachedRange ((UINTN)HostAddress, Bytes, FALSE);
    if (EFI_ERROR (Status))
    {
      goto Done;
    }
    Buffer = HostAddress;
  }
  else
  {
    /* All data mappings bounce, regardless of original cacheability/alignment
       or whether the original address is below 4 GiB. No partial success. */
    if (Bytes > LIUQIN_RETAINED_DMA_BUDGET)
    {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }
    Status = AllocateRetained (EFI_SIZE_TO_PAGES (Bytes), mCpu->DmaBufferAlignment, FALSE, &Buffer);
    if (EFI_ERROR (Status))
    {
      goto Done;
    }
    /* Preserve untransferred bytes on short/error device-write completions.
       The upper layer still must prove TD retirement before calling Unmap. */
    CopyMem (Buffer, HostAddress, Bytes);
  }
  Map = &mMappings[mStatistics.MappingRecords++];
  Map->Host = HostAddress;
  Map->Buffer = Buffer;
  Map->Bytes = Bytes;
  Map->Operation = Operation;
  Map->Active = TRUE;
  ++mStatistics.ActiveMappings;
  MemoryFence ();
  *DeviceAddress = (PHYSICAL_ADDRESS)(UINTN)Buffer;
  *Mapping = Map;
  Status = EFI_SUCCESS;
Done:
  if (EFI_ERROR (Status))
  {
    ++mStatistics.FailedRequests;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
EFIAPI
DmaUnmap (VOID *Mapping)
{
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  DMA_MAPPING *Map;
  UINTN Token;
  UINTN First;

  if (!mInitialized)
  {
    return EFI_NOT_READY;
  }
  if (mStatistics.Sealed)
  {
    return EFI_ACCESS_DENIED;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = EFI_INVALID_PARAMETER;
  Token = (UINTN)Mapping;
  First = (UINTN)mMappings;
  if ((Token < First) || (Token - First >= mStatistics.MappingRecords * sizeof (DMA_MAPPING)) ||
      ((Token - First) % sizeof (DMA_MAPPING) != 0))
  {
    goto Done;
  }
  Map = (DMA_MAPPING *)Mapping;
  if (!Map->Active)
  {
    goto Done;
  }
  Status = CheckUncachedRange ((UINTN)Map->Buffer, Map->Bytes, FALSE);
  if (EFI_ERROR (Status))
  {
    goto Done;
  }
  MemoryFence ();
  if (Map->Operation == MapOperationBusMasterWrite)
  {
    CopyMem (Map->Host, Map->Buffer, Map->Bytes);
  }
  Map->Active = FALSE;
  --mStatistics.ActiveMappings;
  if (Map->Operation != MapOperationBusMasterCommonBuffer)
  {
    ++mStatistics.RetiredDataMappings;
  }
  Status = EFI_SUCCESS;
Done:
  if (EFI_ERROR (Status))
  {
    ++mStatistics.FailedRequests;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
EFIAPI
DmaAllocateAlignedBuffer (EFI_MEMORY_TYPE MemoryType, UINTN Pages, UINTN Alignment,
                          VOID **HostAddress)
{
  EFI_TPL OldTpl;
  EFI_STATUS Status;

  if (HostAddress != NULL)
  {
    *HostAddress = NULL;
  }
  if (!mInitialized)
  {
    return EFI_NOT_READY;
  }
  if (mStatistics.Sealed)
  {
    return EFI_ACCESS_DENIED;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = EFI_INVALID_PARAMETER;
  if ((HostAddress == NULL) || (Pages == 0) ||
      ((MemoryType != EfiBootServicesData) && (MemoryType != EfiRuntimeServicesData)) ||
      ((Alignment != 0) && ((Alignment & (Alignment - 1U)) != 0)))
  {
    goto Done;
  }
  Status = AllocateRetained (Pages, Alignment, TRUE, HostAddress);
Done:
  if (EFI_ERROR (Status))
  {
    ++mStatistics.FailedRequests;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
EFIAPI
DmaAllocateBuffer (EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **HostAddress)
{
  return DmaAllocateAlignedBuffer (MemoryType, Pages, EFI_PAGE_SIZE, HostAddress);
}

EFI_STATUS
EFIAPI
DmaFreeBuffer (UINTN Pages, VOID *HostAddress)
{
  EFI_TPL OldTpl;
  EFI_STATUS Status;
  UINTN Base;
  UINTN Bytes;
  UINTN Index;
  UINTN FirstPage;
  DMA_MAPPING *Map;

  if (!mInitialized)
  {
    return EFI_NOT_READY;
  }
  if (mStatistics.Sealed)
  {
    return EFI_ACCESS_DENIED;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = EFI_INVALID_PARAMETER;
  Base = (UINTN)HostAddress;
  if ((HostAddress == NULL) || (Pages == 0) || (Pages > LIUQIN_RETAINED_DMA_MAX_RECORDS) ||
      ((Base & EFI_PAGE_MASK) != 0))
  {
    goto Done;
  }
  Bytes = EFI_PAGES_TO_SIZE (Pages);
  if (!LiveCommonRange (Base, Bytes))
  {
    goto Done;
  }

  for (Index = 0; Index < mStatistics.MappingRecords; ++Index)
  {
    Map = &mMappings[Index];
    if (Map->Active && RangesOverlap ((UINTN)Map->Host, Map->Bytes, Base, Bytes))
    {
      Status = EFI_ACCESS_DENIED;
      goto Done;
    }
  }
  FirstPage = (Base - (UINTN)mStatistics.ArenaBase) / EFI_PAGE_SIZE;
  for (Index = 0; Index < Pages; ++Index)
  {
    mPageState[FirstPage + Index] = DMA_PAGE_COMMON_RETIRED;
  }
  mStatistics.LiveCommonPages -= Pages;
  mStatistics.RetiredCommonPages += Pages;
  Status = EFI_SUCCESS;
Done:
  if (EFI_ERROR (Status))
  {
    ++mStatistics.FailedRequests;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

VOID EFIAPI LiuqinRetainedDmaGetStatistics (LIUQIN_RETAINED_DMA_STATISTICS *Statistics)
{
  EFI_TPL OldTpl;

  if (Statistics == NULL)
  {
    return;
  }
  if (mStatistics.Sealed || !mInitialized)
  {
    CopyMem (Statistics, &mStatistics, sizeof (*Statistics));
    return;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  CopyMem (Statistics, &mStatistics, sizeof (*Statistics));
  gBS->RestoreTPL (OldTpl);
}

VOID EFIAPI LiuqinRetainedDmaSeal (VOID)
{
  mStatistics.Sealed = TRUE;
  MemoryFence ();
}

EFI_STATUS
EFIAPI
LiuqinRetainedDmaLibConstructor (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  (VOID) ImageHandle;
  (VOID) SystemTable;
  if (mInitialized)
  {
    return EFI_ALREADY_STARTED;
  }
  if (PcdGet64 (PcdDmaDeviceOffset) != 0)
  {
    return EFI_UNSUPPORTED;
  }
  mAddressLimit = PcdGet64 (PcdDmaDeviceLimit);
  if ((mAddressLimit > MAX_UINT32) || (mAddressLimit < LIUQIN_RETAINED_DMA_BUDGET - 1U))
  {
    return EFI_UNSUPPORTED;
  }
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  if (EFI_ERROR (Status))
  {
    return Status;
  }
  if ((mCpu == NULL) || (mCpu->DmaBufferAlignment == 0) || (mCpu->FlushDataCache == NULL) ||
      ((mCpu->DmaBufferAlignment & (mCpu->DmaBufferAlignment - 1U)) != 0) ||
      (mCpu->DmaBufferAlignment > LIUQIN_RETAINED_DMA_BUDGET))
  {
    return EFI_UNSUPPORTED;
  }
  mStatistics.ArenaStatus = EFI_NOT_READY;
  mInitialized = TRUE;
  return EFI_SUCCESS;
}
