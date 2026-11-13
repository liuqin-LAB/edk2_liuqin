/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_UFS_RUNTIME_DTB_H
#define LIUQIN_UFS_RUNTIME_DTB_H
#include <Uefi.h>

/* Read-only validation of the fixed Liuqin host/clock/DMA/interrupt contract.
   The caller must first validate the complete FDT envelope and board identity.
   No MMIO, power, PHY or SMMU operation is performed here. */
BOOLEAN LiuqinUfsRuntimeDescription (CONST VOID *Fdt);
#endif
