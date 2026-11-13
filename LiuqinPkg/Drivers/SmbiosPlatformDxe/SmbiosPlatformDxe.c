/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <BootContract.h>
#include <Guid/LiuqinHandoff.h>
#include <IndustryStandard/SmBios.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Smbios.h>

STATIC EFI_SMBIOS_PROTOCOL *mSmbios;

STATIC EFI_STATUS AddRecord (VOID *Data, UINTN Size, UINT8 Type, CONST CHAR8 * CONST * Strings,
                             UINTN Count, EFI_SMBIOS_HANDLE *Handle)
{
  UINTN Index, Bytes = Size + 1, Offset;
  SMBIOS_STRUCTURE *Record;
  EFI_STATUS Status;
  for (Index = 0; Index < Count; ++Index)
    Bytes += AsciiStrSize (Strings[Index]);
  if (Count == 0)
    ++Bytes;
  Record = AllocateZeroPool (Bytes);
  if (Record == NULL)
    return EFI_OUT_OF_RESOURCES;
  CopyMem (Record, Data, Size);
  Record->Type = Type;
  Record->Length = (UINT8)Size;
  Record->Handle = SMBIOS_HANDLE_PI_RESERVED;
  Offset = Size;
  for (Index = 0; Index < Count; ++Index)
  {
    CopyMem ((UINT8 *)Record + Offset, Strings[Index], AsciiStrSize (Strings[Index]));
    Offset += AsciiStrSize (Strings[Index]);
  }
  *Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status = mSmbios->Add (mSmbios, NULL, Handle, (EFI_SMBIOS_TABLE_HEADER *)Record);
  FreePool (Record);
  return Status;
}

STATIC UINT64 ReadCells (CONST UINT8 *Data, INT32 Cells)
{
  UINT64 Value = 0;
  INT32 Index;
  for (Index = 0; Index < Cells * 4; ++Index)
    Value = (Value << 8) | Data[Index];
  return Value;
}

STATIC UINT64 PhysicalRam (CONST VOID *Dtb)
{
  INT32 Node, Length, Ac = FdtAddressCells (Dtb, 0), Sc = FdtSizeCells (Dtb, 0), At;
  CONST CHAR8 *Type;
  CONST UINT8 *Reg;
  UINT64 Total = 0, Bytes, Base;
  if (Ac < 1 || Ac > 2 || Sc < 1 || Sc > 2)
    return 0;
  for (Node = FdtFirstSubnode (Dtb, 0); Node >= 0; Node = FdtNextSubnode (Dtb, Node))
  {
    Type = FdtGetProp (Dtb, Node, "device_type", &Length);
    if (Type == NULL || Length != 7 || CompareMem (Type, "memory", 7) != 0)
      continue;
    Reg = FdtGetProp (Dtb, Node, "reg", &Length);
    if (Reg == NULL || Length <= 0 || Length % ((Ac + Sc) * 4))
      return 0;
    for (At = 0; At < Length; At += (Ac + Sc) * 4)
    {
      Base = ReadCells (Reg + At, Ac);
      Bytes = ReadCells (Reg + At + Ac * 4, Sc);
      if (Bytes > MAX_UINT64 - Base || Bytes > MAX_UINT64 - Total)
        return 0;
      Total += Bytes;
    }
  }
  return Total;
}

STATIC UINT8 CpuCount (CONST VOID *Dtb)
{
  INT32 Parent = FdtPathOffset (Dtb, "/cpus"), Node, Length;
  CONST CHAR8 *Type;
  UINT8 Count = 0;
  if (Parent < 0)
    return 0;
  for (Node = FdtFirstSubnode (Dtb, Parent); Node >= 0; Node = FdtNextSubnode (Dtb, Node))
  {
    Type = FdtGetProp (Dtb, Node, "device_type", &Length);
    if (Type != NULL && Length == 4 && CompareMem (Type, "cpu", 4) == 0)
    {
      if (Count == MAX_UINT8)
        return 0;
      ++Count;
    }
  }
  return Count;
}

EFI_STATUS EFIAPI LiuqinSmbiosEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  VOID *Hob = GetFirstGuidHob (&gLiuqinHandoffGuid);
  CONST LIUQIN_HANDOFF *Info;
  CONST VOID *Dtb;
  UINT64 Ram, Midr;
  UINTN Index;
  UINT8 Cores;
  EFI_STATUS Status;
  EFI_SMBIOS_HANDLE Handle, ArrayHandle;
  SMBIOS_TABLE_TYPE0 Bios = {0};
  SMBIOS_TABLE_TYPE1 System = {0};
  /* Zero contained elements: end at offset 0x15, then emit the SKU index.
   * sizeof(SMBIOS_TABLE_TYPE3) includes a placeholder contained element that
   * would corrupt the variable-length layout when the count is zero. */
  SMBIOS_TABLE_TYPE3 Chassis = {0};
  UINT8 ChassisBytes[OFFSET_OF (SMBIOS_TABLE_TYPE3, ContainedElements) + 1] = {0};
  SMBIOS_TABLE_TYPE4 Cpu = {0};
  SMBIOS_TABLE_TYPE16 Array = {0};
  SMBIOS_TABLE_TYPE17 Memory = {0};
  SMBIOS_TABLE_TYPE32 Boot = {0};
  CONST CHAR16 *FirmwareVersion = FixedPcdGetPtr (PcdFirmwareVersionString);
  CHAR8 BiosVersion[FixedPcdGetSize (PcdFirmwareVersionString) / sizeof (CHAR16)];
  CONST CHAR8 *BiosStrings[] = {"Liuqin UEFI", BiosVersion};
  CONST CHAR8 *SystemStrings[] = {"Xiaomi", "Xiaomi Pad 6 Pro", "liuqin"};
  CONST CHAR8 *ChassisStrings[] = {"Xiaomi"};
  CONST CHAR8 *CpuStrings[] = {"SoC", "Qualcomm", "Snapdragon (TM) 8+ Gen 1 @ 3.20 GHz"};
  CONST CHAR8 *MemoryStrings[] = {"ABL DT aggregate; package layout unknown"};
  (VOID) ImageHandle;
  (VOID) SystemTable;
  if (Hob == NULL || GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (*Info))
    return EFI_NOT_FOUND;
  Info = GET_GUID_HOB_DATA (Hob);
  if (Info->Version != LIUQIN_HANDOFF_VERSION || Info->DtbSize > LIUQIN_DTB_MAX)
    return EFI_COMPROMISED_DATA;
  Dtb = (CONST VOID *)(UINTN)Info->DtbBase;
  if (LiuqinValidateDtb (Dtb, Info->DtbSize) || LiuqinCheckIdentity (Dtb))
    return EFI_COMPROMISED_DATA;
  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&mSmbios);
  if (EFI_ERROR (Status))
    return Status;
  /* SMBIOS strings are ASCII; use the platform's version without silently
   * truncating an invalid or unrepresentable Unicode PCD. */
  for (Index = 0; Index < ARRAY_SIZE (BiosVersion); ++Index)
  {
    if (FirmwareVersion[Index] > 0x7F)
      return EFI_UNSUPPORTED;
    BiosVersion[Index] = (CHAR8)FirmwareVersion[Index];
    if (BiosVersion[Index] == '\0')
      break;
  }
  if (Index == 0 || Index == ARRAY_SIZE (BiosVersion))
    return EFI_COMPROMISED_DATA;
  Ram = PhysicalRam (Dtb);
  Cores = CpuCount (Dtb);
  Bios.Vendor = 1;
  Bios.BiosVersion = 2;
  Bios.BiosSize = 0xFF;
  Bios.ExtendedBiosSize.Size = 16;
  Bios.BiosCharacteristics.BiosCharacteristicsNotSupported = 1;
  Bios.BIOSCharacteristicsExtensionBytes[1] = BIT3; /* UEFI specification supported. */
  /* PcdFirmwareRevision is vendor-defined, not a packed SMBIOS major/minor.
   * The release date is unspecified by the platform (string index zero). */
  Bios.SystemBiosMajorRelease = 0xFF;
  Bios.SystemBiosMinorRelease = 0xFF;
  Bios.EmbeddedControllerFirmwareMajorRelease = 0xFF;
  Bios.EmbeddedControllerFirmwareMinorRelease = 0xFF;
  Status = AddRecord (&Bios, sizeof (Bios), 0, BiosStrings, 2, &Handle);
  if (EFI_ERROR (Status))
    return Status;
  System.Manufacturer = 1;
  System.ProductName = 2;
  System.Version = 3;
  System.WakeUpType = SystemWakeupTypeUnknown;
  Status = AddRecord (&System, sizeof (System), 1, SystemStrings, 3, &Handle);
  if (EFI_ERROR (Status))
    return Status;
  Chassis.Manufacturer = 1;
  /* Explicit Windows bring-up presentation requested by the user. This is
   * platform identity, not evidence of ACPI sleep or battery support. */
  Chassis.Type = MiscChassisTypeLapTop;
  Chassis.BootupState = ChassisStateUnknown;
  Chassis.PowerSupplyState = ChassisStateUnknown;
  Chassis.ThermalState = ChassisStateUnknown;
  Chassis.SecurityStatus = ChassisSecurityStatusUnknown;
  CopyMem (ChassisBytes, &Chassis, OFFSET_OF (SMBIOS_TABLE_TYPE3, ContainedElements));
  Status = AddRecord (ChassisBytes, sizeof (ChassisBytes), 3, ChassisStrings, 1, &Handle);
  if (EFI_ERROR (Status))
    return Status;
  Cpu.Socket = 1;
  Cpu.ProcessorManufacturer = 2;
  Cpu.ProcessorVersion = 3;
  Cpu.ProcessorType = CentralProcessor;
  Cpu.ProcessorFamily = 0xFE;
  Cpu.ProcessorFamily2 = ProcessorFamilyARM;
  Midr = ArmReadMidr ();
  CopyMem (&Cpu.ProcessorId, &Midr, sizeof (Midr));
  Cpu.Status = 0x41;
  Cpu.ProcessorUpgrade = ProcessorUpgradeNone;
  Cpu.L1CacheHandle = 0xFFFF;
  Cpu.L2CacheHandle = 0xFFFF;
  Cpu.L3CacheHandle = 0xFFFF;
  Cpu.CoreCount = Cores;
  Cpu.ThreadCount = Cores;
  Cpu.EnabledCoreCount = Cores;
  Cpu.CoreCount2 = Cores;
  Cpu.EnabledCoreCount2 = Cores;
  Cpu.ThreadCount2 = Cores;
  Cpu.ThreadEnabled = Cores;
  Cpu.ProcessorCharacteristics = BIT2; /* 64-bit capable. */
  /* Xiaomi Pad 6 Pro official specs: peak CPU speed up to 3.2 GHz.
   * https://www.mi.com/xiaomi-pad-6-pro/specs (published JS text).
   * SMBIOS MHz peak is nominal, not a reading of this boot's DVFS state. */
  Cpu.MaxSpeed = 3200;
  Cpu.CurrentSpeed = 0; /* Unknown: no current-frequency measurement backend. */
  Status = AddRecord (&Cpu, sizeof (Cpu), 4, CpuStrings, 3, &Handle);
  if (EFI_ERROR (Status))
    return Status;
  Array.Location = MemoryArrayLocationSystemBoard;
  Array.Use = MemoryArrayUseSystemMemory;
  Array.MemoryErrorCorrection = MemoryErrorCorrectionUnknown;
  Array.MemoryErrorInformationHandle = 0xFFFE;
  Array.NumberOfMemoryDevices = 1;
  if (Ram / 1024 < 0x80000000ULL)
    Array.MaximumCapacity = (UINT32)(Ram / 1024);
  else
  {
    Array.MaximumCapacity = 0x80000000;
    Array.ExtendedMaximumCapacity = Ram;
  }
  Status = AddRecord (&Array, sizeof (Array), 16, NULL, 0, &ArrayHandle);
  if (EFI_ERROR (Status))
    return Status;
  Memory.MemoryArrayHandle = ArrayHandle;
  Memory.MemoryErrorInformationHandle = 0xFFFE;
  Memory.TotalWidth = 0xFFFF;
  Memory.DataWidth = 0xFFFF;
  Memory.DeviceLocator = 1;
  Memory.FormFactor = MemoryFormFactorUnknown;
  Memory.MemoryType = MemoryTypeUnknown;
  if (Ram == 0)
    Memory.Size = 0xFFFF;
  else if ((Ram >> 20) < 0x7FFF)
    Memory.Size = (UINT16)(Ram >> 20);
  else
  {
    Memory.Size = 0x7FFF;
    Memory.ExtendedSize = (UINT32)(Ram >> 20);
  }
  Memory.VolatileSize = Ram;
  Status = AddRecord (&Memory, sizeof (Memory), 17, MemoryStrings, 1, &Handle);
  if (EFI_ERROR (Status))
    return Status;
  Status = AddRecord (&Boot, sizeof (Boot), 32, NULL, 0, &Handle);

  return Status;
}
