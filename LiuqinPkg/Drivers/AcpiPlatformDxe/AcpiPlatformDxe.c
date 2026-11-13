/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/EventGroup.h>
#include <IndustryStandard/Acpi64.h>
#include <Library/AmlLib/AmlLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/LiuqinIoReady.h>
#include <Protocol/LiuqinTpmReady.h>

#include "AcpiTables.generated.h"
#include "AcpiNamespace.h"
#include "AcpiPcie.h"

STATIC EFI_EVENT mPcieEvent;
STATIC BOOLEAN mPcieAttempted;
STATIC UINTN mBaseDsdtKey;
STATIC BOOLEAN mPcieTablesPublished;
STATIC EFI_STATUS PublishPcieHandoff (VOID);

STATIC VOID EFIAPI InitializePcie (EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  (VOID) Event;
  (VOID) Context;
  if (mPcieAttempted)
    return;
  mPcieAttempted = TRUE;
  Status = InitializeLiuqinPcie ();
  if (Status == EFI_ABORTED)
    CpuDeadLoop ();
  if (!EFI_ERROR (Status) && LiuqinPcieHandoffReady ())
  {
    Status = PublishPcieHandoff ();
  }
}

#define LIUQIN_UFS_DECLARED_BASE 0x01D84000
#define LIUQIN_UFS_DECLARED_SIZE 0x1C000
#define LIUQIN_UFS_READY_BASE 0x01D84000
#define LIUQIN_UFS_READY_SIZE 0x3000
#define LIUQIN_USB_DECLARED_BASE 0x0A600000
#define LIUQIN_USB_DECLARED_SIZE 0x8000
#define LIUQIN_USB_READY_BASE 0x0A600000
#define LIUQIN_USB_READY_SIZE 0x8000
#define LIUQIN_IO_NO_CHILD 0xFFFFFFFFU

STATIC_ASSERT ((LIUQIN_UFS_READY_BASE == LIUQIN_UFS_DECLARED_BASE) &&
                   (LIUQIN_UFS_READY_SIZE <= LIUQIN_UFS_DECLARED_SIZE),
               "UFS ready window must stay inside the declared _CRS window");

STATIC_ASSERT ((LIUQIN_USB_READY_BASE == LIUQIN_USB_DECLARED_BASE) &&
                   (LIUQIN_USB_READY_SIZE == LIUQIN_USB_DECLARED_SIZE),
               "USB host ready window must match the declared _CRS window");

//
// TPM2 ACPI table, ACPI 6.4 section 5.2.33 / Microsoft TEE ACPI Profile Table 3.
// Revision 3 defines the dword at 0x24 as reserved-zero and is 52 bytes: it has
// no start-method-specific parameter field. Revision 4 defines Flags[15:0] as
// PlatformClass and appends 12 start-method-specific parameter bytes (64 bytes).
// A future backend must say which layout it owns; nothing publishes today.
//
#pragma pack(1)
typedef struct
{
  EFI_ACPI_DESCRIPTION_HEADER Header;
  UINT32 Flags;
  UINT64 AddressOfControlArea;
  UINT32 StartMethod;
} LIUQIN_TPM2_ACPI_TABLE_REV3;

typedef struct
{
  EFI_ACPI_DESCRIPTION_HEADER Header;
  UINT32 Flags;
  UINT64 AddressOfControlArea;
  UINT32 StartMethod;
  UINT8 PlatformSpecificParameters[12];
} LIUQIN_TPM2_ACPI_TABLE_REV4;
#pragma pack()

STATIC_ASSERT (sizeof (LIUQIN_TPM2_ACPI_TABLE_REV3) == 52,
               "TPM2 revision-3 layout must be 52 bytes");
STATIC_ASSERT (sizeof (LIUQIN_TPM2_ACPI_TABLE_REV4) == 64,
               "TPM2 revision-4 layout must be 64 bytes");

STATIC EFI_ACPI_TABLE_PROTOCOL *mAcpi;
STATIC UINTN mIoTableKey;
STATIC BOOLEAN mIoInstalled;
STATIC BOOLEAN mUpdatingIo;
STATIC BOOLEAN mPublishedUfs;
STATIC BOOLEAN mPublishedUsb;
STATIC UINTN mPendingIoTableKey;
STATIC BOOLEAN mPendingIoInstalled;
STATIC EFI_EVENT mReadyEvent;
STATIC EFI_EVENT mUfsEvent;
STATIC EFI_EVENT mUsbEvent;
STATIC EFI_EVENT mTpmEvent;
STATIC VOID *mUfsRegistration;
STATIC VOID *mUsbRegistration;
STATIC VOID *mTpmRegistration;
STATIC BOOLEAN mTpmInstalled;
STATIC UINTN mTpmTableKey;

STATIC
BOOLEAN
ValidPublishedHeader (IN CONST EFI_ACPI_DESCRIPTION_HEADER *Table)
{
  return (BOOLEAN)((Table != NULL) && (Table->Length >= sizeof (*Table)) &&
                   ((Table->Length <= 0x10000) ||
                    ((Table->Signature == SIGNATURE_32 ('D', 'S', 'D', 'T')) &&
                     (Table->Length >= sizeof (mLiuqinDsdt)) &&
                     (Table->Length <= sizeof (mLiuqinDsdt) + 16))) &&
                   (CompareMem (Table->OemId, "LIUQIN", 6) == 0) &&
                   (CalculateSum8 ((CONST UINT8 *)Table, Table->Length) == 0));
}

/**
  A TPM2 table is only well-formed for the revision it claims: revision 3 is the
  52-byte fixed table, revision 4 adds 12 start-method-specific parameter bytes.
**/
STATIC
BOOLEAN
Tpm2TableLengthValid (IN CONST EFI_ACPI_DESCRIPTION_HEADER *Table)
{
  if (Table->Revision == 3)
  {
    return (BOOLEAN)(Table->Length == sizeof (LIUQIN_TPM2_ACPI_TABLE_REV3));
  }

  if (Table->Revision == 4)
  {
    return (BOOLEAN)(Table->Length == sizeof (LIUQIN_TPM2_ACPI_TABLE_REV4));
  }

  return FALSE;
}

STATIC
EFI_STATUS
ValidatePublication (VOID)
{
  EFI_ACPI_6_4_ROOT_SYSTEM_DESCRIPTION_POINTER *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER *Xsdt;
  EFI_ACPI_DESCRIPTION_HEADER *Table;
  EFI_ACPI_DESCRIPTION_HEADER *Dsdt;
  EFI_ACPI_6_4_FIXED_ACPI_DESCRIPTION_TABLE Fadt;
  UINT64 Address;
  UINTN Index;
  UINT32 Seen;
  UINT32 Bit;
  UINT32 Expected;

  Rsdp = NULL;
  for (Index = 0; Index < gST->NumberOfTableEntries; ++Index)
  {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gEfiAcpiTableGuid))
    {
      Rsdp = gST->ConfigurationTable[Index].VendorTable;
      break;
    }
  }

  if ((Rsdp == NULL) || (CompareMem (Rsdp, "RSD PTR ", 8) != 0) || (Rsdp->Revision != 2) ||
      (Rsdp->Length != sizeof (*Rsdp)) || (CompareMem (Rsdp->OemId, "LIUQIN", 6) != 0) ||
      (CalculateSum8 ((CONST UINT8 *)Rsdp, 20) != 0) ||
      (CalculateSum8 ((CONST UINT8 *)Rsdp, sizeof (*Rsdp)) != 0) || (Rsdp->XsdtAddress == 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  if (!ValidPublishedHeader (Xsdt) || (Xsdt->Signature != SIGNATURE_32 ('X', 'S', 'D', 'T')) ||
      (((Xsdt->Length - sizeof (*Xsdt)) % sizeof (UINT64)) != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Seen = 0;
  for (Index = sizeof (*Xsdt); Index < Xsdt->Length; Index += sizeof (UINT64))
  {
    CopyMem (&Address, (UINT8 *)Xsdt + Index, sizeof (Address));
    Table = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Address;
    if (!ValidPublishedHeader (Table))
    {
      return EFI_COMPROMISED_DATA;
    }

    switch (Table->Signature)
    {
    case SIGNATURE_32 ('F', 'A', 'C', 'P'):
      if (Table->Length != sizeof (Fadt))
      {
        return EFI_COMPROMISED_DATA;
      }

      CopyMem (&Fadt, Table, sizeof (Fadt));
      if ((Fadt.XDsdt == 0) || ((Fadt.Dsdt != 0) && (Fadt.Dsdt != Fadt.XDsdt)))
      {
        return EFI_COMPROMISED_DATA;
      }

      Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Fadt.XDsdt;
      if (!ValidPublishedHeader (Dsdt) || (Dsdt->Signature != SIGNATURE_32 ('D', 'S', 'D', 'T')))
      {
        return EFI_COMPROMISED_DATA;
      }

      Bit = BIT0;
      break;
    case SIGNATURE_32 ('A', 'P', 'I', 'C'):
      Bit = BIT1;
      break;
    case SIGNATURE_32 ('G', 'T', 'D', 'T'):
      Bit = BIT2;
      break;
    case SIGNATURE_32 ('B', 'G', 'R', 'T'):
      Bit = BIT3;
      break;
    case SIGNATURE_32 ('P', 'P', 'T', 'T'):
      Bit = BIT4;
      break;
    case SIGNATURE_32 ('S', 'S', 'D', 'T'):
      Bit = BIT5;
      break;
    case SIGNATURE_32 ('T', 'P', 'M', '2'):
      if (!Tpm2TableLengthValid (Table))
      {
        return EFI_COMPROMISED_DATA;
      }

      Bit = BIT6;
      break;
    case SIGNATURE_32 ('M', 'C', 'F', 'G'):
      if (!mLiuqinPcieReady || Table->Length != sizeof (mMcfg) ||
          CompareMem (Table, mMcfg, sizeof (mMcfg)) != 0)
        return EFI_COMPROMISED_DATA;
      Bit = BIT8;
      break;
    case SIGNATURE_32 ('I', 'O', 'R', 'T'):
      if (!mLiuqinPcieReady || Table->Length != sizeof (mIort) ||
          CompareMem (Table, mIort, sizeof (mIort)) != 0)
        return EFI_COMPROMISED_DATA;
      Bit = BIT9;
      break;
    default:
      return EFI_COMPROMISED_DATA;
    }

    if ((Seen & Bit) != 0)
    {
      return EFI_COMPROMISED_DATA;
    }

    Seen |= Bit;
  }

  Expected = BIT0 | BIT1 | BIT2 | BIT3;
  if (mLiuqinPcieReady)
    Expected |= BIT8 | BIT9;
  {
    Expected |= BIT4;
    if (mIoInstalled)
    {
      Expected |= BIT5;
    }
  }

  if (mTpmInstalled)
  {
    Expected |= BIT6;
  }

  if (Seen != Expected)
  {
    return EFI_COMPROMISED_DATA;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InstallTable (IN CONST VOID *Data, IN UINTN Size, OUT UINTN *Key)
{
  EFI_ACPI_DESCRIPTION_HEADER Header;

  if (Size < sizeof (Header))
  {
    return EFI_COMPROMISED_DATA;
  }

  CopyMem (&Header, Data, sizeof (Header));
  if ((Header.Length != Size) || (CalculateSum8 (Data, Size) != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  return mAcpi->InstallAcpiTable (mAcpi, (VOID *)Data, Size, Key);
}

// Replace the single DSDT before any loader runs. ACPI Table DXE owns copies;
// keep a baseline copy until the complete XSDT/FADT/MCFG/IORT chain validates.
STATIC EFI_STATUS PublishPcieHandoff (VOID)
{
  EFI_ACPI_DESCRIPTION_HEADER *Old = NULL, *New = NULL;
  UINTN McfgKey = 0, IortKey = 0, NewKey = 0;
  BOOLEAN Mcfg = FALSE, Iort = FALSE, Removed = FALSE, Installed = FALSE, Clean = TRUE;
  EFI_STATUS Status;
  if (mPcieTablesPublished)
    return ValidatePublication ();
  if (!mAcpi || !LiuqinPcieHandoffReady ())
    return EFI_NOT_READY;
  Status = CopyLiuqinDsdt (&Old);
  if (EFI_ERROR (Status))
    return Status;
  mLiuqinPcieReady = TRUE;
  Status = CopyLiuqinDsdt (&New);
  mLiuqinPcieReady = FALSE;
  if (EFI_ERROR (Status))
    goto End;
  Status = InstallTable (mMcfg, sizeof (mMcfg), &McfgKey);
  if (EFI_ERROR (Status))
    goto Rollback;
  Mcfg = TRUE;
  Status = InstallTable (mIort, sizeof (mIort), &IortKey);
  if (EFI_ERROR (Status))
    goto Rollback;
  Iort = TRUE;
  Status = mAcpi->UninstallAcpiTable (mAcpi, mBaseDsdtKey);
  if (EFI_ERROR (Status))
    goto Rollback;
  Removed = TRUE;
  Status = InstallTable (New, New->Length, &NewKey);
  if (EFI_ERROR (Status))
    goto Rollback;
  Installed = TRUE;
  mLiuqinPcieReady = TRUE;
  Status = ValidatePublication ();
  if (!EFI_ERROR (Status))
  {
    mBaseDsdtKey = NewKey;
    mPcieTablesPublished = TRUE;
    goto End;
  }
Rollback:
  mLiuqinPcieReady = FALSE;
  if (Installed && EFI_ERROR (mAcpi->UninstallAcpiTable (mAcpi, NewKey)))
    Clean = FALSE;
  if (Clean && Removed && EFI_ERROR (InstallTable (Old, Old->Length, &mBaseDsdtKey)))
    Clean = FALSE;
  if (Iort && EFI_ERROR (mAcpi->UninstallAcpiTable (mAcpi, IortKey)))
    Clean = FALSE;
  if (Mcfg && EFI_ERROR (mAcpi->UninstallAcpiTable (mAcpi, McfgKey)))
    Clean = FALSE;
  if (!Clean || EFI_ERROR (ValidatePublication ()))
  {

    CpuDeadLoop ();
  }
End:
  if (New)
    FreePool (New);
  FreePool (Old);
  return Status;
}

STATIC
BOOLEAN
IoReady (IN EFI_GUID *Guid, IN UINT64 Base, IN UINT64 Size, IN UINT32 Gsiv)
{
  LIUQIN_IO_READY_PROTOCOL *Ready;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (Guid, NULL, (VOID **)&Ready);
  return (BOOLEAN)(!EFI_ERROR (Status) && (Ready != NULL) &&
                   (Ready->Version == LIUQIN_IO_READY_VERSION) && (Ready->MmioBase == Base) &&
                   (Ready->MmioSize == Size) && (Ready->Gsiv == Gsiv) && Ready->DmaReady &&
                   Ready->ControllerReady);
}

// Public TCG ACPI specification Table 8 start methods only. 9-10 are reserved
// for vendor-specific legacy use and are NOT accepted: the local Qualcomm
// reference encodes 9 but has no published vendor contract, and the backend
// gate (a real TPM2 command/response) is what prevents an unbacked table.
STATIC CONST UINT32 mTpmStartMethods[] = {2, 6, 7, 8, 11, 15};

STATIC
BOOLEAN
TpmControlAreaValid (IN CONST LIUQIN_TPM_READY_PROTOCOL *Ready)
{
  if (Ready->StartMethod == 6)
  {
    // TIS/FIFO has no control area.
    return (BOOLEAN)((Ready->ControlAreaAddress == 0) && (Ready->ControlAreaSize == 0));
  }

  return (BOOLEAN)((Ready->ControlAreaAddress != 0) &&
                   ((Ready->ControlAreaAddress & (SIZE_4KB - 1)) == 0) &&
                   (Ready->ControlAreaSize >= LIUQIN_TPM_CONTROL_AREA_MIN_SIZE) &&
                   (Ready->ControlAreaSize <= SIZE_1MB));
}

STATIC
BOOLEAN
TpmReadyFieldsValid (IN CONST LIUQIN_TPM_READY_PROTOCOL *Ready)
{
  UINTN Index;

  if ((Ready->Version != LIUQIN_TPM_READY_VERSION) || !Ready->Ready)
  {
    return FALSE;
  }

  if ((Ready->TableRevision != 3) && (Ready->TableRevision != 4))
  {
    return FALSE;
  }

  // Revision 3 reserves the whole Flags dword as zero (Microsoft TEE ACPI
  // Profile 4.4); only revision 4 defines PlatformClass in Flags[15:0].
  if ((Ready->TableRevision == 3) && (Ready->PlatformClass != 0))
  {
    return FALSE;
  }

  for (Index = 0; Index < ARRAY_SIZE (mTpmStartMethods); ++Index)
  {
    if (Ready->StartMethod == mTpmStartMethods[Index])
    {
      break;
    }
  }

  if (Index == ARRAY_SIZE (mTpmStartMethods))
  {
    return FALSE;
  }

  return TpmControlAreaValid (Ready);
}

/**
  Publish the TPM2 ACPI table only when a proven backend installed the TPM
  ready protocol and every field validates. A missing/invalid backend publishes
  nothing: an empty or fabricated TPM2 table would misrepresent the platform.
  A publication/verification failure is logged and never fails the ACPI
  baseline, which must still describe CPUs, timer and storage.
**/
STATIC
EFI_STATUS
PublishTpmTable (VOID)
{
  LIUQIN_TPM_READY_PROTOCOL *Ready;
  LIUQIN_TPM2_ACPI_TABLE_REV4 Table;
  UINT32 Length;
  EFI_STATUS Status;
  UINTN Key;

  if (mTpmInstalled)
  {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (&gLiuqinTpmReadyProtocolGuid, NULL, (VOID **)&Ready);
  if (EFI_ERROR (Status))
  {
    return EFI_SUCCESS;
  }

  if (!TpmReadyFieldsValid (Ready))
  {

    return EFI_SUCCESS;
  }

  Length = (Ready->TableRevision == 3) ? (UINT32)sizeof (LIUQIN_TPM2_ACPI_TABLE_REV3)
                                       : (UINT32)sizeof (Table);
  ZeroMem (&Table, sizeof (Table));
  Table.Header.Signature = SIGNATURE_32 ('T', 'P', 'M', '2');
  Table.Header.Length = Length;
  Table.Header.Revision = (UINT8)Ready->TableRevision;
  CopyMem (Table.Header.OemId, "LIUQIN", 6);
  CopyMem (&Table.Header.OemTableId, "LIUQTPM ", 8);
  Table.Header.OemRevision = 2;
  CopyMem (&Table.Header.CreatorId, "LIUQ", 4);
  Table.Header.CreatorRevision = 0x20261027;
  Table.Flags = (Ready->TableRevision == 4) ? (UINT32)Ready->PlatformClass : 0;
  Table.AddressOfControlArea = Ready->ControlAreaAddress;
  Table.StartMethod = Ready->StartMethod;
  Table.Header.Checksum = (UINT8)(0U - CalculateSum8 ((CONST UINT8 *)&Table, Length));

  Status = InstallTable (&Table, Length, &Key);
  if (EFI_ERROR (Status))
  {

    return EFI_SUCCESS;
  }

  mTpmInstalled = TRUE;
  mTpmTableKey = Key;
  Status = ValidatePublication ();
  if (EFI_ERROR (Status))
  {
    mAcpi->UninstallAcpiTable (mAcpi, mTpmTableKey);
    mTpmInstalled = FALSE;
    mTpmTableKey = 0;

    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
RefreshIoTables (VOID)
{
  EFI_STATUS Status;
  EFI_ACPI_DESCRIPTION_HEADER *Table;
  BOOLEAN Ufs;
  BOOLEAN Usb;
  UINTN Key;
  EFI_STATUS CleanupStatus;

  if (mUpdatingIo)
  {
    return EFI_SUCCESS;
  }

  mUpdatingIo = TRUE;
  Table = NULL;
  // Keep the controller-only ready window separate from the retained broader
  // reference-DSDT _CRS window; the latter is not an exact Liuqin DT resource.
  Ufs = IoReady (&gLiuqinUfsReadyProtocolGuid, LIUQIN_UFS_READY_BASE, LIUQIN_UFS_READY_SIZE, 297);
  Usb = IoReady (&gLiuqinUsbReadyProtocolGuid, LIUQIN_USB_READY_BASE, LIUQIN_USB_READY_SIZE, 165);

  // A failed rollback must keep its key until the extra table is removed.
  // Do not accumulate another SSDT on the next notification.
  if (mPendingIoInstalled)
  {
    Status = mAcpi->UninstallAcpiTable (mAcpi, mPendingIoTableKey);
    if (EFI_ERROR (Status))
    {
      goto Done;
    }

    mPendingIoInstalled = FALSE;
  }

  if (mIoInstalled && (Ufs == mPublishedUfs) && (Usb == mPublishedUsb))
  {
    Status = ValidatePublication ();
    goto Done;
  }

  Status = BuildLiuqinIoTable (Ufs, Usb, &Table);

  if (!EFI_ERROR (Status))
  {
    // Stage the replacement before removing the last valid table. An
    // allocation/install failure must not erase the published I/O namespace.
    Status = InstallTable (Table, Table->Length, &Key);
    if (!EFI_ERROR (Status) && mIoInstalled)
    {
      Status = mAcpi->UninstallAcpiTable (mAcpi, mIoTableKey);
      if (EFI_ERROR (Status))
      {
        CleanupStatus = mAcpi->UninstallAcpiTable (mAcpi, Key);
        if (EFI_ERROR (CleanupStatus))
        {
          mPendingIoTableKey = Key;
          mPendingIoInstalled = TRUE;
        }
      }
    }

    if (!EFI_ERROR (Status))
    {
      mIoTableKey = Key;
      mIoInstalled = TRUE;
      mPublishedUfs = Ufs;
      mPublishedUsb = Usb;
    }
  }

Done:
  if (Table != NULL)
  {
    FreePool (Table);
  }

  if (!EFI_ERROR (Status))
  {
    Status = ValidatePublication ();
  }

  mUpdatingIo = FALSE;
  return Status;
}

STATIC
VOID EFIAPI UpdateIoTables (IN EFI_EVENT Event, IN VOID *Context)
{
  EFI_STATUS Status;

  (VOID) Event;
  (VOID) Context;
  // TPM2 publication is independent of I/O readiness and is re-evaluated on
  // every observer/ReadyToBoot notification; it stays absent without a backend.
  PublishTpmTable ();
  Status = RefreshIoTables ();
  if (EFI_ERROR (Status))
  {
  }
}

STATIC
EFI_STATUS
CreateIoObserver (IN EFI_GUID *Guid, OUT EFI_EVENT *Event, OUT VOID **Registration)
{
  EFI_STATUS Status;

  // EfiCreateProtocolNotifyEvent only ASSERTs on service errors and cannot
  // propagate them in RELEASE. Use the services directly and own cleanup.
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, UpdateIoTables, NULL, Event);
  if (!EFI_ERROR (Status))
  {
    Status = gBS->RegisterProtocolNotify (Guid, *Event, Registration);
    if (EFI_ERROR (Status))
    {
      gBS->CloseEvent (*Event);
      *Event = NULL;
    }
  }

  return Status;
}

STATIC
VOID CloseIoObservers (VOID)
{
  if (mPcieEvent != NULL)
  {
    gBS->CloseEvent (mPcieEvent);
    mPcieEvent = NULL;
  }
  if (mReadyEvent != NULL)
  {
    gBS->CloseEvent (mReadyEvent);
    mReadyEvent = NULL;
  }

  if (mUfsEvent != NULL)
  {
    gBS->CloseEvent (mUfsEvent);
    mUfsEvent = NULL;
  }

  if (mUsbEvent != NULL)
  {
    gBS->CloseEvent (mUsbEvent);
    mUsbEvent = NULL;
  }

  if (mTpmEvent != NULL)
  {
    gBS->CloseEvent (mTpmEvent);
    mTpmEvent = NULL;
  }
}

EFI_STATUS
EFIAPI
LiuqinAcpiEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_ACPI_DESCRIPTION_HEADER *Dsdt;
  CONST UINT8 *Madt;
  CONST UINT8 *Gtdt;
  UINTN MadtSize;
  UINTN GtdtSize;
  UINTN Keys[8];
  UINTN Count;
  EFI_STATUS CleanupStatus;

  (VOID) ImageHandle;
  (VOID) SystemTable;

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&mAcpi);
  if (EFI_ERROR (Status))
  {
    return Status;
  }

  Dsdt = NULL;
  Count = 0;
  mLiuqinPcieReady = FALSE;
  Status = CopyLiuqinDsdt (&Dsdt);
  if (EFI_ERROR (Status))
  {
    return Status;
  }

  Madt = mMadt;
  Gtdt = mGtdt;
  MadtSize = sizeof (mMadt);
  GtdtSize = sizeof (mGtdt);

  // DSDT precedes FADT: upstream AcpiTableDxe maintains DSDT/X_DSDT links.
  Status = InstallTable (Dsdt, Dsdt->Length, &Keys[Count]);
  if (!EFI_ERROR (Status))
  {
    ++Count;
    Status = InstallTable (mFadt, sizeof (mFadt), &Keys[Count]);
  }

  if (!EFI_ERROR (Status))
  {
    ++Count;
    Status = InstallTable (Madt, MadtSize, &Keys[Count]);
  }

  if (!EFI_ERROR (Status))
  {
    ++Count;
    Status = InstallTable (Gtdt, GtdtSize, &Keys[Count]);
  }

  if (!EFI_ERROR (Status))
  {
    ++Count;
    Status = InstallTable (mBgrt, sizeof (mBgrt), &Keys[Count]);
  }

  if (!EFI_ERROR (Status))
  {
    ++Count;
    {
      Status = InstallTable (mPptt, sizeof (mPptt), &Keys[Count]);
      if (!EFI_ERROR (Status))
      {
        ++Count;
      }
    }
  }

  if (!EFI_ERROR (Status) && mLiuqinPcieReady)
  {
    Status = InstallTable (mMcfg, sizeof (mMcfg), &Keys[Count]);
    if (!EFI_ERROR (Status))
    {
      ++Count;
      Status = InstallTable (mIort, sizeof (mIort), &Keys[Count]);
      if (!EFI_ERROR (Status))
        ++Count;
    }
  }
  FreePool (Dsdt);
  if (!EFI_ERROR (Status))
  {
    Status = ValidatePublication ();
  }

  if (!EFI_ERROR (Status))
  {
    // Observe delayed binding as well as the final BDS handoff to a loader.
    Status = CreateIoObserver (&gLiuqinUfsReadyProtocolGuid, &mUfsEvent, &mUfsRegistration);
    if (!EFI_ERROR (Status))
    {
      Status = CreateIoObserver (&gLiuqinUsbReadyProtocolGuid, &mUsbEvent, &mUsbRegistration);
    }

    if (!EFI_ERROR (Status))
    {
      Status = CreateIoObserver (&gLiuqinTpmReadyProtocolGuid, &mTpmEvent, &mTpmRegistration);
    }

    if (!EFI_ERROR (Status))
    {
      Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, UpdateIoTables, NULL,
                                   &gEfiEventReadyToBootGuid, &mReadyEvent);
    }

    if (!EFI_ERROR (Status))
    {
      // Read the current protocols after all observers are registered. A TPM
      // backend installed before this driver is handled by the same path; a
      // late one is caught by the notify event or ReadyToBoot.
      Status = RefreshIoTables ();
    }
  }

  if (!EFI_ERROR (Status))
  {
    PublishTpmTable ();
  }

  if (EFI_ERROR (Status))
  {
    // No callback may retain an entry point into an unsuccessfully loaded
    // driver. ACPI owns copies of the tables; remove every retained key too.
    CloseIoObservers ();
    if (mPendingIoInstalled)
    {
      CleanupStatus = mAcpi->UninstallAcpiTable (mAcpi, mPendingIoTableKey);
      if (!EFI_ERROR (CleanupStatus))
      {
        mPendingIoInstalled = FALSE;
      }
    }

    if (mIoInstalled)
    {
      CleanupStatus = mAcpi->UninstallAcpiTable (mAcpi, mIoTableKey);
      if (!EFI_ERROR (CleanupStatus))
      {
        mIoInstalled = FALSE;
      }
    }

    if (mTpmInstalled)
    {
      CleanupStatus = mAcpi->UninstallAcpiTable (mAcpi, mTpmTableKey);
      if (!EFI_ERROR (CleanupStatus))
      {
        mTpmInstalled = FALSE;
      }
    }

    while (Count > 0)
    {
      mAcpi->UninstallAcpiTable (mAcpi, Keys[--Count]);
    }

    return Status;
  }

  mBaseDsdtKey = Keys[0];
  mPcieTablesPublished = mLiuqinPcieReady;

  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, InitializePcie, NULL,
                               &gEfiEndOfDxeEventGroupGuid, &mPcieEvent);
  if (EFI_ERROR (Status))
    InitializePcie (NULL, NULL);
  return EFI_SUCCESS;
}
