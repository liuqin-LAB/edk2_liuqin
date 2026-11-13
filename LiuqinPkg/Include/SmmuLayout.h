/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Liuqin platform contributors. */
#ifndef LIUQIN_SMMU_LAYOUT_H
#define LIUQIN_SMMU_LAYOUT_H

typedef unsigned int LIUQIN_SMMU_U32;
typedef unsigned long long LIUQIN_SMMU_U64;

#define LIUQIN_SMMU_MAX_STREAM_GROUPS 120U /* Project DT ceiling, not architecture limit. */

enum
{
  LiuqinSmmuLayoutOk = 0,
  LiuqinSmmuLayoutInvalidArgument,
  LiuqinSmmuLayoutCapability,
  LiuqinSmmuLayoutStreamCount,
  LiuqinSmmuLayoutId1,
  LiuqinSmmuLayoutContextCount,
  LiuqinSmmuLayoutContextIndex,
  LiuqinSmmuLayoutWindowMismatch,
  LiuqinSmmuLayoutAddressRange
};

/* Offsets for a future bounded diagnostic snapshot. These constants do not
 * authorize MMIO access, and the named registers are not intrinsically read-only.
 * No translation request, fault clear/resume or TLB maintenance is provided.
 */
enum
{
  LiuqinSmmuGr0Cr0 = 0x00,
  LiuqinSmmuGr0Id0 = 0x20,
  LiuqinSmmuGr0Id1 = 0x24,
  LiuqinSmmuGr0Id2 = 0x28,
  LiuqinSmmuGr0Id7 = 0x3C,
  LiuqinSmmuGr0Smr = 0x800,
  LiuqinSmmuGr0S2cr = 0xC00,
  LiuqinSmmuStreamStride = 4,
  LiuqinSmmuCbSctlr = 0x00,
  LiuqinSmmuCbActlr = 0x04,
  LiuqinSmmuCbTcr2 = 0x10,
  LiuqinSmmuCbTtbr0 = 0x20, /* 64-bit raw register, never dereferenced here. */
  LiuqinSmmuCbTtbr1 = 0x28,
  LiuqinSmmuCbTcr = 0x30,
  LiuqinSmmuCbContextIdr = 0x34,
  LiuqinSmmuCbMair0 = 0x38,
  LiuqinSmmuCbMair1 = 0x3C,
  LiuqinSmmuCbFsr = 0x58,
  LiuqinSmmuCbFar = 0x60, /* 64-bit raw register. */
  LiuqinSmmuCbFsynr0 = 0x68,
  LiuqinSmmuCbFsynr1 = 0x6C
};

typedef struct
{
  LIUQIN_SMMU_U64 Base;
  LIUQIN_SMMU_U64 AvailableSpan;
  LIUQIN_SMMU_U64 EncodedSpan;
  LIUQIN_SMMU_U64 Global1;
  LIUQIN_SMMU_U64 Context;
  LIUQIN_SMMU_U64 Cbar;
  LIUQIN_SMMU_U64 Cba2r;
  LIUQIN_SMMU_U64 Cbfrsynra;
  LIUQIN_SMMU_U32 PageShift;
  LIUQIN_SMMU_U32 PageSize;
  LIUQIN_SMMU_U32 GlobalPages;
  LIUQIN_SMMU_U32 StreamGroups;
  LIUQIN_SMMU_U32 ContextBanks;
  LIUQIN_SMMU_U32 Stage2OnlyBanks;
  LIUQIN_SMMU_U32 SelectedBank;
} LIUQIN_SMMU_LAYOUT;

/* Base/AvailableSpan must describe a separately proved, stable runtime DT MMIO
 * window; Id0/Id1 are already obtained raw values. This function performs no
 * reads. It accepts the project's stream-matching SMMUv2 subset, zero unnamed
 * ID1 bits, a nonzero bank count and an exact encoded-aperture/window match.
 * Every advertised bank and both complete global pages must fit that window.
 * Base must be nonzero and aligned to the decoded register page size, with a
 * representable exclusive end. Output must be writable; a non-NULL output is
 * wholly cleared on every failure. Success supplies candidate addresses only:
 * it proves neither accessibility/ownership, translation stage/format, an
 * identity mapping, cache coherency nor DMA readiness. No firmware gate may be
 * relaxed solely because this decoder succeeds.
 */
int LiuqinSmmuDecodeLayout (LIUQIN_SMMU_U64 Base, LIUQIN_SMMU_U64 AvailableSpan,
                            LIUQIN_SMMU_U32 Id0, LIUQIN_SMMU_U32 Id1, LIUQIN_SMMU_U32 SelectedBank,
                            LIUQIN_SMMU_LAYOUT *Output);
const char *LiuqinSmmuLayoutReasonName (int Reason);
#endif
