/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_ACPI_WLAN_H
#define LIUQIN_ACPI_WLAN_H
#include "AcpiPcie.h"
typedef struct
{
  UINT32 Stage, ReadMask;
  UINT32 Before[5], InputBefore[5], During[5], InputDuring[5];
  BOOLEAN EnableMayHaveApplied, Quiesced, Retained;
} LIUQIN_WLAN_RESULT;
/* Only after validated live RPMh WLAN voltage+enable requests completed.
 * PERST stays low. GPIO81/82 are observed, never changed. Pulse GPIO204 as
 * stock CNSS, then hold GPIO80 high long enough to observe SW_CTRL. Disable
 * WLAN again and restore the owned GPIO fields. Votes persist until reset;
 * this does not start PCIe, MHI, Wi-Fi firmware or networking. */
/* Read-only guards before any endpoint power request; repeat before enable. */
EFI_STATUS LiuqinWlanPreflight (CONST LIUQIN_PCIE_IO *, LIUQIN_WLAN_RESULT *);
// Work runs with WLAN enabled; ABORTED preserves its configuration for reset.
EFI_STATUS LiuqinWlanEnable (CONST LIUQIN_PCIE_IO *, LIUQIN_WLAN_RESULT *, LIUQIN_PCIE_SCOPED_WORK,
                             VOID *);
#endif
