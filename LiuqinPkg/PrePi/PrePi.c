/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiPei.h>
#include <AArch64/AArch64.h>
#include <Guid/FdtHob.h>
#include <Guid/LiuqinHandoff.h>
#include <Guid/LiuqinMemoryPlan.h>
#include <BootContract.h>
#include <LiuqinMmio.h>
#include <Library/ArmLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/PrePiLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>

/* SEC has no generated C entry wrapper to invoke its constructors. */
VOID EFIAPI ProcessLibraryConstructorList (VOID);

static VOID Stop (VOID)
{
  CpuDeadLoop ();
}

static UINT64 CellValue (CONST UINT8 *Data, INT32 Cells)
{
  UINT64 Value = 0;
  INT32 Index;
  for (Index = 0; Index < Cells * 4; ++Index)
    Value = (Value << 8) | Data[Index];
  return Value;
}

static BOOLEAN ReadU32 (CONST VOID *Dtb, INT32 Node, CONST CHAR8 *Name, UINT32 *Value)
{
  INT32 Length;
  CONST VOID *Data = FdtGetProp (Dtb, Node, Name, &Length);
  if (Data == NULL || Length != 4)
    return FALSE;
  *Value = (UINT32)CellValue (Data, 1);
  return TRUE;
}

static BOOLEAN FramebufferInfo (CONST VOID *Dtb, LIUQIN_HANDOFF *Info)
{
  INT32 Node = -1, Depth = 0, Length, Parent, Ac, Sc;
  CONST UINT8 *Reg;
  CONST CHAR8 *Compatible, *Format;
  UINT64 Pixels;
  Info->FramebufferBase = FixedPcdGet64 (PcdFramebufferBase);
  Info->FramebufferSize = FixedPcdGet32 (PcdFramebufferSize);
  Info->Width = FixedPcdGet32 (PcdFramebufferWidth);
  Info->Height = FixedPcdGet32 (PcdFramebufferHeight);
  Info->Stride = FixedPcdGet32 (PcdFramebufferStride);
  Info->FramebufferSource = LIUQIN_FB_FROM_REFERENCE;
  while ((Node = FdtNextNode (Dtb, Node, &Depth)) >= 0)
  {
    Compatible = FdtGetProp (Dtb, Node, "compatible", &Length);
    if (Compatible == NULL || Length < (INT32)sizeof ("simple-framebuffer") ||
        Compatible[Length - 1] != 0 || AsciiStrCmp (Compatible, "simple-framebuffer") != 0)
      continue;
    Parent = FdtParentOffset (Dtb, Node);
    Ac = FdtAddressCells (Dtb, Parent);
    Sc = FdtSizeCells (Dtb, Parent);
    Reg = FdtGetProp (Dtb, Node, "reg", &Length);
    if (Ac < 1 || Ac > 2 || Sc < 1 || Sc > 2 || Reg == NULL || Length != (Ac + Sc) * 4)
      return FALSE;
    Info->FramebufferBase = CellValue (Reg, Ac);
    Info->FramebufferSize = CellValue (Reg + Ac * 4, Sc);
    if (!ReadU32 (Dtb, Node, "width", &Info->Width) ||
        !ReadU32 (Dtb, Node, "height", &Info->Height) ||
        !ReadU32 (Dtb, Node, "stride", &Info->Stride))
      return FALSE;
    Format = FdtGetProp (Dtb, Node, "format", &Length);
    if (Format == NULL || Length != 9 || Format[8] != 0 ||
        (AsciiStrCmp (Format, "a8r8g8b8") && AsciiStrCmp (Format, "x8r8g8b8")))
      return FALSE;
    Info->FramebufferSource = LIUQIN_FB_FROM_DTB;
    break;
  }
  Pixels = (UINT64)Info->Stride * Info->Height;
  return Info->Width && Info->Height && Info->Width <= 4096 && Info->Height <= 4096 &&
         Info->FramebufferBase % 4 == 0 && Info->Stride % 4 == 0 &&
         (UINT64)Info->Width * 4 <= Info->Stride &&
         Info->FramebufferBase >= FixedPcdGet64 (PcdFramebufferBase) && Info->FramebufferSize &&
         Info->FramebufferSize <= FixedPcdGet32 (PcdFramebufferSize) &&
         Info->FramebufferBase - FixedPcdGet64 (PcdFramebufferBase) <=
             FixedPcdGet32 (PcdFramebufferSize) - Info->FramebufferSize &&
         Pixels <= Info->FramebufferSize;
}

VOID EFIAPI LiuqinPrePiMain (VOID *Dtb)
{
  EFI_HOB_HANDOFF_INFO_TABLE *Hob;
  EFI_STATUS Status;
  ARM_MEMORY_REGION_DESCRIPTOR *Map;
  VOID *Tables, *DtbCopy;
  UINTN TableSize, Size;
  UINT64 DtbAddress;
  UINT64 Pool = FixedPcdGet64 (PcdUefiMemoryBase);
  UINT32 PoolSize = FixedPcdGet32 (PcdUefiMemorySize);
  UINT64 Fd = FixedPcdGet64 (PcdFdBaseAddress);
  UINT32 FdSize = FixedPcdGet32 (PcdFdSize);
  UINT64 Stack = Fd + FdSize - FixedPcdGet32 (PcdPrePiStackSize);
  LIUQIN_HANDOFF Info;
  LIUQIN_MEMORY_CONTEXT MemoryContext;
  LIUQIN_MEMORY_PLAN MemoryPlan;
  LIUQIN_MMIO_WINDOW Mmio[LIUQIN_CORE_MMIO_MAX_WINDOWS + LIUQIN_MMIO_MAX_WINDOWS];
  LIUQIN_MMIO_WINDOW Shared[LIUQIN_SHARED_MEMORY_MAX_WINDOWS];
  UINTN Index, Region, IoCount, CoreCount, SharedCount;
  UINT8 AddressBits;
  EFI_RESOURCE_ATTRIBUTE_TYPE Attributes =
      EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
      EFI_RESOURCE_ATTRIBUTE_TESTED | EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |
      EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE | EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
      EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE;

  if (LiuqinValidateDtb (Dtb, LIUQIN_DTB_MAX) || LiuqinCheckIdentity (Dtb) ||
      LiuqinCheckRange (Dtb, Fd, FdSize) || LiuqinCheckRange (Dtb, Pool, PoolSize))
    Stop ();
  Size = LiuqinDtbSize (Dtb);
  if (LiuqinRangesOverlap ((UINTN)Dtb, Size, Fd, FdSize) ||
      LiuqinRangesOverlap ((UINTN)Dtb, Size, Pool, PoolSize))
    Stop ();
  ZeroMem (&Info, sizeof (Info));
  Info.Version = LIUQIN_HANDOFF_VERSION;
  Info.CurrentEl = (UINT32)(ArmReadCurrentEL () >> 2);
  if (!FramebufferInfo (Dtb, &Info))
    Stop ();
  MemoryContext.OriginalDtbBase = (UINTN)Dtb;
  MemoryContext.Framebuffer.Base = FixedPcdGet64 (PcdFramebufferBase);
  MemoryContext.Framebuffer.Size = FixedPcdGet32 (PcdFramebufferSize);
  LiuqinBuildMemoryPlan (Dtb, Size, &MemoryContext, &MemoryPlan);
  SharedCount = LiuqinGetSharedMemoryWindows (Shared, ARRAY_SIZE (Shared));
  if (!SharedCount)
    Stop ();
  for (Index = 0; Index < SharedCount; ++Index)
  {
    if (LiuqinRangesOverlap (Shared[Index].Base, Shared[Index].Size, (UINTN)Dtb, Size) ||
        LiuqinRangesOverlap (Shared[Index].Base, Shared[Index].Size, Fd, FdSize) ||
        LiuqinRangesOverlap (Shared[Index].Base, Shared[Index].Size, Pool, PoolSize) ||
        LiuqinRangesOverlap (Shared[Index].Base, Shared[Index].Size, MemoryContext.Framebuffer.Base,
                             MemoryContext.Framebuffer.Size))
      Stop ();
    if (MemoryPlan.Reason == LiuqinMemoryOk)
    {
      int Reason = LiuqinReserveMemoryRange (&MemoryPlan, Shared[Index].Base, Shared[Index].Size);
      if (Reason)
      {
        MemoryPlan.Reason = (UINT32)Reason;
        MemoryPlan.Count = 0;
        MemoryPlan.TotalBytes = 0;
      }
    }
  }
  AddressBits = ArmGetPhysicalAddressBits ();
  CoreCount = LiuqinGetCoreIoWindows (FixedPcdGet64 (PcdGicDistributorBase),
                                      FixedPcdGet64 (PcdGicRedistributorsBase), Mmio,
                                      LIUQIN_CORE_MMIO_MAX_WINDOWS);
  if (CoreCount == 0)
    Stop ();
  IoCount = LiuqinGetIoWindows (Mmio + CoreCount, LIUQIN_MMIO_MAX_WINDOWS);
  if (IoCount == 0)
    Stop ();
  if (MemoryPlan.Reason == LiuqinMemoryOk)
  {
    for (Index = 0; Index < MemoryPlan.Count; ++Index)
    {
      if (AddressBits < 64 && MemoryPlan.Ranges[Index].Base + MemoryPlan.Ranges[Index].Size >
                                  LShiftU64 (1, AddressBits))
      {
        MemoryPlan.Reason = LiuqinMemoryRamOverflow;
        MemoryPlan.Count = 0;
        MemoryPlan.TotalBytes = 0;
        break;
      }
      for (Region = 0; Region < CoreCount + IoCount; ++Region)
      {
        if (LiuqinRangesOverlap (MemoryPlan.Ranges[Index].Base, MemoryPlan.Ranges[Index].Size,
                                 Mmio[Region].Base, Mmio[Region].Size))
        {
          MemoryPlan.Reason = LiuqinMemoryMmioOverlap;
          MemoryPlan.Count = 0;
          MemoryPlan.TotalBytes = 0;
          break;
        }
      }
    }
  }

  /* Pool is still untouched and excluded from the loader/DTB. Discard stale
   * ABL cache lines by VA before writing HOBs with the data cache disabled.
   */
  InvalidateDataCacheRange ((VOID *)(UINTN)Pool, PoolSize);
  if (MemoryPlan.Reason == LiuqinMemoryOk)
  {
    // Discard only validated free RAM's stale ABL cache lines before DXE
    // allocates from it. Exclusions retain live SEC/source/DTB reservations.
    for (Index = 0; Index < MemoryPlan.Count; ++Index)
      InvalidateDataCacheRange ((VOID *)(UINTN)MemoryPlan.Ranges[Index].Base,
                                (UINTN)MemoryPlan.Ranges[Index].Size);
  }
  if (ArmReadCurrentEL () == AARCH64_EL2)
    ArmWriteHcr (ArmReadHcr () | ARM_HCR_TGE);
  Hob = HobConstructor ((VOID *)(UINTN)Pool, PoolSize, (VOID *)(UINTN)Pool,
                        (VOID *)(UINTN)(Pool + PoolSize));
  if (Hob == NULL)
    Stop ();
  PrePeiSetHobList (Hob);
  ProcessLibraryConstructorList ();
  /* Fixed pool stays available when a complete DT RAM plan fails closed. */
  BuildResourceDescriptorHob (EFI_RESOURCE_SYSTEM_MEMORY, Attributes, Pool, PoolSize);
  BuildResourceDescriptorHob (EFI_RESOURCE_SYSTEM_MEMORY, Attributes, Fd, FdSize);
  if (MemoryPlan.Reason == LiuqinMemoryOk)
  {
    for (Index = 0; Index < MemoryPlan.Count; ++Index)
      BuildResourceDescriptorHob (EFI_RESOURCE_SYSTEM_MEMORY, Attributes,
                                  MemoryPlan.Ranges[Index].Base, MemoryPlan.Ranges[Index].Size);
  }
  BuildGuidDataHob (&gLiuqinMemoryPlanGuid, &MemoryPlan, sizeof (MemoryPlan));

  for (Index = 0; Index < CoreCount; ++Index)
  {
    BuildResourceDescriptorHob (EFI_RESOURCE_MEMORY_MAPPED_IO,
                                EFI_RESOURCE_ATTRIBUTE_PRESENT |
                                    EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                                    EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE,
                                Mmio[Index].Base, Mmio[Index].Size);
  }
  BuildResourceDescriptorHob (EFI_RESOURCE_MEMORY_MAPPED_IO,
                              EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                                  EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE,
                              FixedPcdGet64 (PcdFramebufferBase),
                              FixedPcdGet32 (PcdFramebufferSize));
  for (Index = CoreCount; Index < CoreCount + IoCount; ++Index)
  {
    BuildResourceDescriptorHob (EFI_RESOURCE_MEMORY_MAPPED_IO,
                                EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE,
                                Mmio[Index].Base, Mmio[Index].Size);
  }
  for (Index = 0; Index < SharedCount; ++Index)
  {
    BuildResourceDescriptorHob (EFI_RESOURCE_MEMORY_RESERVED,
                                EFI_RESOURCE_ATTRIBUTE_PRESENT |
                                    EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE,
                                Shared[Index].Base, Shared[Index].Size);
    BuildMemoryAllocationHob (Shared[Index].Base, Shared[Index].Size, EfiReservedMemoryType);
  }
  BuildMemoryAllocationHob (Fd, Stack - Fd, EfiBootServicesData);
  BuildStackHob (Stack, FixedPcdGet32 (PcdPrePiStackSize));
  BuildCpuHob (ArmGetPhysicalAddressBits (), 16);
  SetBootMode (BOOT_WITH_FULL_CONFIGURATION);
  DtbCopy = AllocatePages (EFI_SIZE_TO_PAGES (Size));
  if (DtbCopy == NULL)
    Stop ();
  CopyMem (DtbCopy, Dtb, Size);
  DtbAddress = (UINTN)DtbCopy;
  Info.DtbBase = DtbAddress;
  Info.DtbSize = Size;
  BuildGuidDataHob (&gFdtHobGuid, &DtbAddress, sizeof (DtbAddress));
  BuildGuidDataHob (&gLiuqinHandoffGuid, &Info, sizeof (Info));
  ArmPlatformGetVirtualMemoryMap (&Map);
  if (Map == NULL)
    Stop ();
  Status = ArmConfigureMmu (Map, &Tables, &TableSize);
  if (EFI_ERROR (Status))
    Stop ();
  ArmEnableDataCache ();
  ArmEnableInstructionCache ();
  SerialPortInitialize ();
  /* First stage supports only the boot CPU in the first redistributor.
   * Never let the generic driver guess the CapeP 0x40000 frame stride.
   */
  if ((ArmReadMpidr () & 0xFF00FFFFFFULL) != 0 ||
      (MmioRead64 (FixedPcdGet64 (PcdGicRedistributorsBase) + 8) >> 32) != 0)
    Stop ();

  BuildFvHob (FixedPcdGet64 (PcdFvBaseAddress), FixedPcdGet32 (PcdFvSize));

  Status = LoadDxeCoreFromFv (NULL, SIZE_128KB);

  Stop ();
}
