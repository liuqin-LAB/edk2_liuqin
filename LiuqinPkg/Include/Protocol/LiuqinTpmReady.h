/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_TPM_READY_H
#define LIUQIN_TPM_READY_H

#include <Uefi.h>

#define LIUQIN_TPM_READY_VERSION 1

// A Windows tpm.sys control area is backed by reserved ACPI memory and, for the
// TZ/fTPM-style CRB handoff this platform would need, is a CRB window of at
// least 0x3000 bytes (3 pages) at a page-aligned physical address. The 1.0
// Microsoft TEE ACPI Profile Table 4 field layout itself is 0x30 bytes; the
// smaller layout size is NOT sufficient as a handoff contract, so the publisher
// requires this larger window and rejects anything thinner.
#define LIUQIN_TPM_CONTROL_AREA_MIN_SIZE 0x3000

typedef struct
{
  UINT32 Version;            // LIUQIN_TPM_READY_VERSION
  UINT32 TableRevision;      // 3 (Microsoft TEE profile) or 4 (ACPI 6.4)
  UINT32 StartMethod;        // TCG ACPI Table 8 value
  UINT16 PlatformClass;      // TPM2 Flags[15:0]; 0 for revision 3
  BOOLEAN Ready;             // TRUE only after a real TPM2 command succeeded
  UINT64 ControlAreaAddress; // physical control-area base, page aligned
  UINT32 ControlAreaSize;    // mapped control-area window in bytes
} LIUQIN_TPM_READY_PROTOCOL;

extern EFI_GUID gLiuqinTpmReadyProtocolGuid;

#endif
