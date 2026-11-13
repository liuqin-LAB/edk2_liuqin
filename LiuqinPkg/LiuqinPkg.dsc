# SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  PLATFORM_NAME           = Liuqin
  PLATFORM_GUID           = a1c2dddb-51ee-4694-a657-13363d951389
  PLATFORM_VERSION        = 1.00
  DSC_SPECIFICATION       = 0x0001001B
  OUTPUT_DIRECTORY        = Build/Firmware
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS           = RELEASE
  SKUID_IDENTIFIER        = DEFAULT
  FLASH_DEFINITION        = LiuqinPkg/LiuqinPkg.fdf
  PREBUILD                = python tools/prebuild.py
  POSTBUILD               = python tools/pack_boot.py

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerVirtCounterLib/ArmGenericTimerVirtCounterLib.inf
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  ArmPlatformLib|LiuqinPkg/Library/PlatformLib/PlatformLib.inf
  BootContractLib|LiuqinPkg/Library/BootContractLib/BootContractLib.inf
  BootPolicyLib|LiuqinPkg/Library/BootPolicyLib/BootPolicyLib.inf
  SmmuLayoutLib|LiuqinPkg/Library/SmmuLayoutLib/SmmuLayoutLib.inf
  UsbVbusPolicyLib|LiuqinPkg/Library/UsbVbusPolicyLib/UsbVbusPolicyLib.inf
  SpmiReadLib|LiuqinPkg/Library/SpmiReadLib/SpmiReadLib.inf
  SpmiWriteLib|LiuqinPkg/Library/SpmiWriteLib/SpmiWriteLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  KernelErrataPatcherLib|LiuqinPkg/Library/KernelErrataPatcherLib/KernelErrataPatcherLib.inf
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf
  SerialPortLib|MdePkg/Library/BaseSerialPortLibNull/BaseSerialPortLibNull.inf
  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  ReportStatusCodeLib|MdePkg/Library/BaseReportStatusCodeLibNull/BaseReportStatusCodeLibNull.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  RealTimeClockLib|LiuqinPkg/Library/SyntheticRtcLib/SyntheticRtcLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  PlatformBootManagerLib|LiuqinPkg/Library/BootManagerLib/BootManagerLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  FrameBufferBltLib|MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  GptLib|MdeModulePkg/Library/GptLib/GptLib.inf
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
  UfsSmmuProbeLib|LiuqinPkg/Library/UfsSmmuProbeLib/UfsSmmuProbeLib.inf
  AmlLib|DynamicTablesPkg/Library/Common/AmlLib/AmlLib.inf
  AcpiHelperLib|DynamicTablesPkg/Library/Common/AcpiHelperLib/AcpiHelperLib.inf
  BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/BaseCryptLib.inf
  OpensslLib|CryptoPkg/Library/OpensslLib/OpensslLibCrypto.inf
  IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  RngLib|MdePkg/Library/BaseRngLib/BaseRngLib.inf
  ArmTrngLib|LiuqinPkg/Library/Rng/CheckedArmTrngLib.inf

[LibraryClasses.common.SEC]
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf

[LibraryClasses.common.DXE_CORE]
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf

[BuildOptions]
  GCC:*_*_AARCH64_CC_FLAGS = -fno-jump-tables
  GCC:RELEASE_*_*_CC_FLAGS = -DMDEPKG_NDEBUG

[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER]
  GCC:*_CLANGDWARF_*_DLINK_FLAGS = -z common-page-size=0x10000
  GCC:*_CLANGPDB_*_DLINK_FLAGS = /ALIGN:65536

[PcdsFixedAtBuild]
  gArmTokenSpaceGuid.PcdFdBaseAddress|0xAC000000
  gArmTokenSpaceGuid.PcdFdSize|0x01000000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0xAC001000
  gArmTokenSpaceGuid.PcdFvSize|0x00EFF000
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0xAD000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x06000000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x00040000
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x06000000
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x17100000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x17180000
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|16
  gEmbeddedTokenSpaceGuid.PcdTimerPeriod|10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdCapsuleInRamSupport|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdVariableStoreSize|0x10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdHwErrStorageSize|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"Liuqin UEFI"
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"1.00"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareRevision| 100
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosVersion|0x0308
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosEntryPointProvideMethod|0x2
  gEmbeddedTokenSpaceGuid.PcdDmaDeviceOffset|0
  gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xFFFFFFFF
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
  gEfiMdeModulePkgTokenSpaceGuid.PcdDriverHealthConfigureForm|{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0
  gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|8192
  gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  gEfiShellPkgTokenSpaceGuid.PcdShellDefaultDelay|0
  gEfiShellPkgTokenSpaceGuid.PcdShellProfileMask|2

[PcdsPatchableInModule]
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|0

[PcdsFeatureFlag]
  gEfiMdeModulePkgTokenSpaceGuid.PcdSupportUpdateCapsuleReset|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSupportProcessCapsuleAtRuntime|FALSE
  gEfiShellPkgTokenSpaceGuid.PcdShellForceConsole|TRUE

[Components]
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }
  LiuqinPkg/PrePi/PrePi.inf {
    <LibraryClasses>
      NULL|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  }
  MdeModulePkg/Core/Dxe/DxeMain.inf
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  LiuqinPkg/Drivers/FramebufferDxe/FramebufferDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf
  LiuqinPkg/Drivers/SmbiosPlatformDxe/SmbiosPlatformDxe.inf
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  LiuqinPkg/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.inf
  LiuqinPkg/Drivers/UsbHostDxe/UsbHostDxe.inf
  LiuqinPkg/Drivers/UfsHostDxe/UfsHostDxe.inf {
    <LibraryClasses>
      DmaLib|LiuqinPkg/Library/RetainedDmaLib/RetainedDmaLib.inf
  }
  SecurityPkg/Hash2DxeCrypto/Hash2DxeCrypto.inf
  LiuqinPkg/Drivers/HashDxe/HashDxe.inf
  SecurityPkg/RandomNumberGenerator/RngDxe/RngDxe.inf
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellAcpiViewCommandLib/UefiShellAcpiViewCommandLib.inf
  }
