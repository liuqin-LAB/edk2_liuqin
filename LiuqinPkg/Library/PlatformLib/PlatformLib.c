/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiPei.h>
#include <Library/ArmPlatformLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/HobLib.h>
#include <Guid/LiuqinMemoryPlan.h>
#include <LiuqinMmio.h>

EFI_BOOT_MODE ArmPlatformGetBootMode (VOID)
{
  return BOOT_WITH_FULL_CONFIGURATION;
}
VOID ArmPlatformPeiBootAction (VOID)
{
}
RETURN_STATUS ArmPlatformInitialize (UINTN MpId)
{
  return RETURN_SUCCESS;
}
VOID ArmPlatformGetPlatformPpiList (UINTN *Size, EFI_PEI_PPI_DESCRIPTOR **List)
{
  *Size = 0;
  *List = NULL; /* Single boot CPU; no fabricated SMP mailbox. */
}
VOID ArmPlatformGetVirtualMemoryMap (ARM_MEMORY_REGION_DESCRIPTOR **Map)
{
  VOID *Hob = GetFirstGuidHob (&gLiuqinMemoryPlanGuid);
  CONST LIUQIN_MEMORY_PLAN *Plan = NULL;
  UINTN Additional = 0;
  ARM_MEMORY_REGION_DESCRIPTOR *Regions;
  UINTN Index, IoCount, CoreCount, Prefix, SharedCount;
  LIUQIN_MMIO_WINDOW Shared[LIUQIN_SHARED_MEMORY_MAX_WINDOWS];
  LIUQIN_MMIO_WINDOW Io[LIUQIN_MMIO_MAX_WINDOWS];
  LIUQIN_MMIO_WINDOW Core[LIUQIN_CORE_MMIO_MAX_WINDOWS];
  if (Hob != NULL && GET_GUID_HOB_DATA_SIZE (Hob) == sizeof (*Plan))
  {
    Plan = GET_GUID_HOB_DATA (Hob);
    if (Plan->Reason == LiuqinMemoryOk && Plan->Count <= LIUQIN_MEMORY_MAX_RANGES)
      Additional = Plan->Count;
  }

  IoCount = LiuqinGetIoWindows (Io, ARRAY_SIZE (Io));
  CoreCount =
      LiuqinGetCoreIoWindows (FixedPcdGet64 (PcdGicDistributorBase),
                              FixedPcdGet64 (PcdGicRedistributorsBase), Core, ARRAY_SIZE (Core));
  SharedCount = LiuqinGetSharedMemoryWindows (Shared, ARRAY_SIZE (Shared));
  if (IoCount == 0 || CoreCount == 0 || SharedCount == 0)
  {
    *Map = NULL;
    return;
  }
  Prefix = 3 + CoreCount + SharedCount;
  Regions = AllocateZeroPool ((Prefix + IoCount + Additional + 1) * sizeof (*Regions));
  if (Regions == NULL)
  {
    *Map = NULL;
    return;
  }
  Regions[0].PhysicalBase = FixedPcdGet64 (PcdFdBaseAddress);
  Regions[0].Length = FixedPcdGet32 (PcdFdSize);
  Regions[0].Attributes = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  Regions[1].PhysicalBase = FixedPcdGet64 (PcdUefiMemoryBase);
  Regions[1].Length = FixedPcdGet32 (PcdUefiMemorySize);
  Regions[1].Attributes = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  for (Index = 0; Index < CoreCount; ++Index)
  {
    Regions[2 + Index].PhysicalBase = Core[Index].Base;
    Regions[2 + Index].Length = Core[Index].Size;
    Regions[2 + Index].Attributes = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  }
  Regions[2 + CoreCount].PhysicalBase = FixedPcdGet64 (PcdFramebufferBase);
  Regions[2 + CoreCount].Length = FixedPcdGet32 (PcdFramebufferSize);
  Regions[2 + CoreCount].Attributes = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;

  for (Index = 0; Index < SharedCount; ++Index)
  {
    Regions[3 + CoreCount + Index].PhysicalBase = Shared[Index].Base;
    Regions[3 + CoreCount + Index].Length = Shared[Index].Size;
    Regions[3 + CoreCount + Index].Attributes = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
  }
  for (Index = 0; Index < IoCount; ++Index)
  {
    Regions[Prefix + Index].PhysicalBase = Io[Index].Base;
    Regions[Prefix + Index].Length = Io[Index].Size;
    Regions[Prefix + Index].Attributes = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  }
  for (Index = 0; Index < Additional; ++Index)
  {
    UINTN Slot = Prefix + IoCount + Index;
    Regions[Slot].PhysicalBase = Plan->Ranges[Index].Base;
    Regions[Slot].Length = Plan->Ranges[Index].Size;
    Regions[Slot].Attributes = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  }

  for (Index = 0; Index < Prefix + IoCount + Additional; ++Index)
    Regions[Index].VirtualBase = Regions[Index].PhysicalBase;
  *Map = Regions;
}
