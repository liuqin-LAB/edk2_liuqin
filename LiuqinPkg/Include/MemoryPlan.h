/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_MEMORY_PLAN_H
#define LIUQIN_MEMORY_PLAN_H

#include <BootContract.h>

#define LIUQIN_MEMORY_MAX_RANGES 256U
#define LIUQIN_MEMORY_PAGE_SIZE 0x1000ULL

typedef struct
{
  LIUQIN_U64 Base;
  LIUQIN_U64 Size;
} LIUQIN_MEMORY_RANGE;

typedef struct
{
  /* Physical address of the original x0 blob, even when dt is a copy. Its
   * size comes from the validated DT header, never from Available. */
  LIUQIN_U64 OriginalDtbBase;
  /* Zero Size means no framebuffer. Pass the entire known aperture when
   * scanout geometry is not an independently validated reservation. */
  LIUQIN_MEMORY_RANGE Framebuffer;
} LIUQIN_MEMORY_CONTEXT;

typedef enum
{
  LiuqinMemoryOk = 0,
  LiuqinMemoryInvalidArgument,
  LiuqinMemoryInvalidDtb,
  LiuqinMemoryInvalidCells,
  LiuqinMemoryInvalidProperty,
  LiuqinMemoryInvalidReg,
  LiuqinMemoryRamOverflow,
  LiuqinMemoryReservationOverflow,
  LiuqinMemoryAlignmentOverflow,
  LiuqinMemoryCapacity,
  LiuqinMemoryUnknownReservation,
  LiuqinMemoryUnplacedReservation,
  LiuqinMemoryNoRam,
  LiuqinMemoryFdNotRam,
  LiuqinMemoryPoolNotRam,
  LiuqinMemoryFdReserved,
  LiuqinMemoryPoolReserved,
  LiuqinMemoryEmptyPlan,
  /* Set by platform integration when DT RAM overlaps its fixed MMIO map. */
  LiuqinMemoryMmioOverlap
} LIUQIN_MEMORY_REASON;

typedef struct
{
  LIUQIN_U32 Reason;
  /* FDT structure-block byte offset, or zero for context/fixed-region errors. */
  LIUQIN_U32 NodeOffset;
  LIUQIN_U32 Count;
  LIUQIN_U32 Reserved;
  LIUQIN_U64 TotalBytes;
  LIUQIN_MEMORY_RANGE Ranges[LIUQIN_MEMORY_MAX_RANGES];
} LIUQIN_MEMORY_PLAN;

/* Read-only, allocation-free, no libc/UEFI/MMIO. Validate first, derive only
 * from root RAM, round RAM inward and exclusions outward to 4 KiB. All output
 * ranges are sorted, disjoint, nonadjacent. On error Count/TotalBytes are zero;
 * callers must keep their existing fixed pool instead of publishing a partial
 * plan. Successful planning alone does not quiesce DMA or configure the MMU. */
int LiuqinBuildMemoryPlan (const void *Dt, LIUQIN_SIZE Available,
                           const LIUQIN_MEMORY_CONTEXT *Context, LIUQIN_MEMORY_PLAN *Plan);
/* Subtract a fixed platform reservation, rounded outward to pages. Validates
 * the input plan; on any failure the entire input remains unchanged. */
int LiuqinReserveMemoryRange (LIUQIN_MEMORY_PLAN *Plan, LIUQIN_U64 Base, LIUQIN_U64 Size);

#endif
