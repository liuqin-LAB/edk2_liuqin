/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/AmlLib/AmlLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include "AcpiNamespace.h"
#include "AcpiTables.generated.h"
#include "AcpiPcie.h"

BOOLEAN mLiuqinPcieReady;

EFI_STATUS
CopyLiuqinDsdt (OUT EFI_ACPI_DESCRIPTION_HEADER **Table)
{
  EFI_STATUS Status;
  if (Table == NULL)
  {
    return EFI_INVALID_PARAMETER;
  }
  *Table = NULL;
  // The compiled platform description is independent of the installed OS DTB.
  // Source DT/resource comparisons run offline, never as a publication gate.
  {
    CONST EFI_ACPI_DESCRIPTION_HEADER *Header =
        (CONST EFI_ACPI_DESCRIPTION_HEADER *)(CONST VOID *)mLiuqinDsdt;
    if (Header->Signature != SIGNATURE_32 ('D', 'S', 'D', 'T') ||
        Header->Length != sizeof (mLiuqinDsdt) || Header->Revision != 2 ||
        CompareMem (Header->OemId, "LIUQIN", 6) != 0 ||
        CalculateSum8 (mLiuqinDsdt, sizeof (mLiuqinDsdt)) != 0)
      return EFI_COMPROMISED_DATA;
  }
  *Table = AllocateCopyPool (sizeof (mLiuqinDsdt), mLiuqinDsdt);
  if (*Table != NULL && mLiuqinPcieReady)
  {
    AML_ROOT_NODE_HANDLE Root = NULL;
    EFI_ACPI_DESCRIPTION_HEADER *Result = NULL;
    AML_NODE_HANDLE Sta = NULL;
    Status = AmlParseDefinitionBlock (*Table, &Root);
    if (!EFI_ERROR (Status))
      Status = AmlFindNode (Root, "\\_SB.PCI0._STA", &Sta);
    if (!EFI_ERROR (Status))
      Status = AmlNameOpUpdateInteger (Sta, 0x0F);
    if (!EFI_ERROR (Status))
      Status = AmlSerializeDefinitionBlock (Root, &Result);
    if (Root != NULL)
      AmlDeleteTree (Root);
    FreePool (*Table);
    *Table = NULL;
    if (EFI_ERROR (Status))
    {
      if (Result != NULL)
        FreePool (Result);
      return Status;
    }
    *Table = Result;
  }
  return (*Table == NULL) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}

STATIC EFI_STATUS PatchSta (IN AML_ROOT_NODE_HANDLE Root, IN CONST CHAR8 *Path, IN BOOLEAN Ready)
{
  AML_NODE_HANDLE Node;
  EFI_STATUS Status;
  Node = NULL;
  Status = AmlFindNode (Root, Path, &Node);
  if (!EFI_ERROR (Status))
  {
    Status = AmlNameOpUpdateInteger (Node, Ready ? 0x0F : 0);
  }
  return Status;
}

EFI_STATUS
BuildLiuqinIoTable (IN BOOLEAN UfsReady, IN BOOLEAN UsbReady,
                    OUT EFI_ACPI_DESCRIPTION_HEADER **Table)
{
  AML_ROOT_NODE_HANDLE Root;
  EFI_ACPI_DESCRIPTION_HEADER *Result;
  EFI_STATUS Status;
  if (Table == NULL)
  {
    return EFI_INVALID_PARAMETER;
  }
  *Table = NULL;
  Root = NULL;
  Result = NULL;
  // The immutable templates own all device identities, topology and resources.
  // Only these two named readiness integers may be changed at runtime.
  Status = AmlParseDefinitionBlock (
      (CONST EFI_ACPI_DESCRIPTION_HEADER *)(CONST VOID *)(mLiuqinSsdtIo), &Root);
  if (!EFI_ERROR (Status))
  {
    Status = PatchSta (Root, "\\_SB.UFS0._STA", UfsReady);
  }
  if (!EFI_ERROR (Status))
  {
    Status = PatchSta (Root, "\\_SB.USB0._STA", UsbReady);
  }
  if (!EFI_ERROR (Status))
  {
    Status = AmlSerializeDefinitionBlock (Root, &Result);
  }
  if (Root != NULL)
  {
    AmlDeleteTree (Root);
  }
  if (EFI_ERROR (Status))
  {
    if (Result != NULL)
    {
      FreePool (Result);
    }
    return Status;
  }
  *Table = Result;
  return EFI_SUCCESS;
}
