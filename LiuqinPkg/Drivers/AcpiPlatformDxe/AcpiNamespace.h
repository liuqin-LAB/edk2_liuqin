/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_ACPI_NAMESPACE_H
#define LIUQIN_ACPI_NAMESPACE_H
#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
extern BOOLEAN mLiuqinPcieReady;

// Caller owns the returned allocation. Output remains NULL on failure.
EFI_STATUS CopyLiuqinDsdt (OUT EFI_ACPI_DESCRIPTION_HEADER **Table);
EFI_STATUS BuildLiuqinIoTable (IN BOOLEAN UfsReady, IN BOOLEAN UsbReady,
                               OUT EFI_ACPI_DESCRIPTION_HEADER **Table);
#endif
