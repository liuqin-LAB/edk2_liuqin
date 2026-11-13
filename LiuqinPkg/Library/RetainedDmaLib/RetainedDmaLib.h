/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_RETAINED_DMA_LIB_H
#define LIUQIN_RETAINED_DMA_LIB_H

#include <Uefi.h>

#define LIUQIN_RETAINED_DMA_BUDGET (32U * 1024U * 1024U)
#define LIUQIN_RETAINED_DMA_MAX_RECORDS (LIUQIN_RETAINED_DMA_BUDGET / EFI_PAGE_SIZE)

typedef struct
{
  EFI_PHYSICAL_ADDRESS ArenaBase;
  UINTN ReservedBytes;
  UINTN ConsumedBytes;
  UINTN AllocationRecords;
  UINTN MappingRecords;
  UINTN ActiveMappings;
  UINTN LiveCommonPages;
  UINTN RetiredCommonPages;
  UINTN RetiredDataMappings;
  UINTN FailedRequests;
  EFI_STATUS ArenaStatus;
  BOOLEAN Sealed;
} LIUQIN_RETAINED_DMA_STATISTICS;

VOID EFIAPI LiuqinRetainedDmaGetStatistics (OUT LIUQIN_RETAINED_DMA_STATISTICS *Statistics);

/* Metadata-only: the controller owner must positively prove halt before EBS.
   This prevents subsequent DMA API calls; it does not stop any hardware. */
VOID EFIAPI LiuqinRetainedDmaSeal (VOID);

#endif
