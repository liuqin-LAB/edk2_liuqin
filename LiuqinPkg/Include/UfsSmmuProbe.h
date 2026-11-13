/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Liuqin platform contributors. */
#ifndef LIUQIN_UFS_SMMU_PROBE_H
#define LIUQIN_UFS_SMMU_PROBE_H
#include <SmmuLayout.h>

#define LIUQIN_UFS_SMMU_PROBE_PASSES 2U
#define LIUQIN_UFS_SMMU_PROBE_HEADER_WORDS 11U
#define LIUQIN_UFS_SMMU_PROBE_BANK_WORDS 15U
#define LIUQIN_UFS_SMMU_PROBE_GUARD_WORDS 5U
#define LIUQIN_UFS_SMMU_PROBE_MAX_READS 514U

enum
{
  LiuqinUfsSmmuEligibleForIsolatedProbe = 0,
  LiuqinUfsSmmuInvalidArgument,
  LiuqinUfsSmmuIncompleteSnapshot,
  LiuqinUfsSmmuReadFailed,
  LiuqinUfsSmmuAllOnes,
  LiuqinUfsSmmuLayoutRejected,
  LiuqinUfsSmmuUnsupportedIdVariant,
  LiuqinUfsSmmuUnsupportedControl,
  LiuqinUfsSmmuNoStream,
  LiuqinUfsSmmuAmbiguousStream,
  LiuqinUfsSmmuUnsupportedTargetStream,
  LiuqinUfsSmmuSharedNsBank,
  LiuqinUfsSmmuInvalidBank,
  LiuqinUfsSmmuUnsupportedContextVariant,
  LiuqinUfsSmmuUnsupportedFaultVariant,
  LiuqinUfsSmmuActiveFault,
  LiuqinUfsSmmuUnstableSnapshot
};

enum
{
  LiuqinUfsSmmuCacheNonCca = 1,
  LiuqinUfsSmmuCacheCca = 2
};

/* Callback receives an aligned byte offset in the separately authorized
 * 0x15000000 + 1MiB NS SMMU window. Return exactly 1 only for a completed
 * read, and store its raw value. There is intentionally no write callback.
 * Failure cannot trap/recover from an MMIO external abort: caller must already
 * prove its DT window, mapping, clocks and NS accessibility. No TTBR is read.
 */
typedef int (*LIUQIN_UFS_SMMU_PROBE_READ32) (void *Context, LIUQIN_SMMU_U32 Offset,
                                             LIUQIN_SMMU_U32 *Value);
typedef struct
{
  void *Context;
  LIUQIN_UFS_SMMU_PROBE_READ32 Read32;
} LIUQIN_UFS_SMMU_PROBE_IO;

enum
{
  LiuqinUfsSmmuCr0,
  LiuqinUfsSmmuId0,
  LiuqinUfsSmmuId1,
  LiuqinUfsSmmuId2,
  LiuqinUfsSmmuId7,
  LiuqinUfsSmmuGfarLow,
  LiuqinUfsSmmuGfarHigh,
  LiuqinUfsSmmuGfsr,
  LiuqinUfsSmmuGfsynr0,
  LiuqinUfsSmmuGfsynr1,
  LiuqinUfsSmmuGfsynr2
};
enum
{
  LiuqinUfsSmmuCbar,
  LiuqinUfsSmmuCba2r,
  LiuqinUfsSmmuCbfrsynra,
  LiuqinUfsSmmuSctlr,
  LiuqinUfsSmmuActlr,
  LiuqinUfsSmmuTcr2,
  LiuqinUfsSmmuTcr,
  LiuqinUfsSmmuContextIdr,
  LiuqinUfsSmmuMair0,
  LiuqinUfsSmmuMair1,
  LiuqinUfsSmmuFsr,
  LiuqinUfsSmmuFarLow,
  LiuqinUfsSmmuFarHigh,
  LiuqinUfsSmmuFsynr0,
  LiuqinUfsSmmuFsynr1
};

typedef struct
{
  LIUQIN_SMMU_U32 Header[LIUQIN_UFS_SMMU_PROBE_HEADER_WORDS];
  LIUQIN_SMMU_U32 Smr[LIUQIN_SMMU_MAX_STREAM_GROUPS];
  LIUQIN_SMMU_U32 S2cr[LIUQIN_SMMU_MAX_STREAM_GROUPS];
  LIUQIN_SMMU_U32 Bank[LIUQIN_UFS_SMMU_PROBE_BANK_WORDS];
  LIUQIN_SMMU_U32 Guard[LIUQIN_UFS_SMMU_PROBE_GUARD_WORDS];
  LIUQIN_SMMU_U32 HeaderWordsRead, StreamWordsRead, BankWordsRead, GuardWordsRead;
} LIUQIN_UFS_SMMU_PROBE_PASS;

typedef struct
{
  LIUQIN_SMMU_U64 VerifiedBase, VerifiedSpan;
  LIUQIN_UFS_SMMU_PROBE_PASS Pass[LIUQIN_UFS_SMMU_PROBE_PASSES];
  LIUQIN_SMMU_U32 CompletedPasses, ReadAttempts, FailedPass, FailedOffset;
  int CaptureReason, LayoutReason;
} LIUQIN_UFS_SMMU_PROBE_SNAPSHOT;

typedef struct
{
  LIUQIN_SMMU_U32 EligibleForIsolatedProbe;
  LIUQIN_SMMU_U32 StreamIndex, SelectedBank, NsReferences, CacheVariant;
  LIUQIN_SMMU_U32 HistoricalContextFaults, HistoricalGlobalFaults;
  LIUQIN_SMMU_LAYOUT CandidateLayout;
} LIUQIN_UFS_SMMU_PROBE_CANDIDATE;

/* Bounded two-pass acquisition, stopping immediately on an unreadable/all-ones
 * register or unsupported ID/stream layout. Raw successful reads, including
 * all-ones values and partial fault records, remain in Snapshot on every
 * failure. The counters distinguish partial data from complete evidence.
 * Snapshot must be writable and disjoint from IO and its callback context.
 */
int LiuqinUfsSmmuTakeSnapshot (const LIUQIN_UFS_SMMU_PROBE_IO *Io, LIUQIN_SMMU_U64 VerifiedBase,
                               LIUQIN_SMMU_U64 VerifiedSpan,
                               LIUQIN_UFS_SMMU_PROBE_SNAPSHOT *Snapshot);

/* Pure classifier; Candidate is wholly zeroed on failure, Snapshot is never
 * modified. Requires two complete stable scans of every actual NS stream and
 * selected bank, exact SID e0, TYPE0 and one NS TYPE0 reference to that bank.
 * Only the explicitly documented observed register variant is supported.
 * Success is eligibility for one separately guarded no-data NVS NOP, never
 * DMAReady, BlockIo readiness, permission, identity mapping or EBS permission.
 * Input and output structures must not overlap.
 */
int LiuqinUfsSmmuClassify (const LIUQIN_UFS_SMMU_PROBE_SNAPSHOT *Snapshot,
                           LIUQIN_UFS_SMMU_PROBE_CANDIDATE *Candidate);
const char *LiuqinUfsSmmuProbeReasonName (int Reason);
#endif
