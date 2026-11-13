/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Liuqin platform contributors. */
#include <UfsSmmuProbe.h>

typedef LIUQIN_SMMU_U32 U32;
typedef LIUQIN_SMMU_U64 U64;
_Static_assert (__CHAR_BIT__ == 8 && sizeof (U32) == 4 && sizeof (U64) == 8,
                "UFS SMMU probe requires 8/32/64-bit widths");

static const U32 HeaderOffsets[11] = {0,    0x20, 0x24, 0x28, 0x3C, 0x40,
                                      0x44, 0x48, 0x50, 0x54, 0x58};
/* No TTBR, ATS, TLBI, RESUME, FSRRESTORE or secure alias offset appears. */
static const U32 BankOffsets[12] = {0,    4,    0x10, 0x30, 0x34, 0x38,
                                    0x3C, 0x58, 0x60, 0x64, 0x68, 0x6C};

static void Clear (void *Output, __SIZE_TYPE__ Length)
{
  unsigned char *At = (unsigned char *)Output;
  __SIZE_TYPE__ Index;
  if (Output)
    for (Index = 0; Index < Length; ++Index)
      At[Index] = 0;
}
static void Copy (void *Output, const void *Input, __SIZE_TYPE__ Length)
{
  unsigned char *To = (unsigned char *)Output;
  const unsigned char *From = (const unsigned char *)Input;
  __SIZE_TYPE__ Index;
  for (Index = 0; Index < Length; ++Index)
    To[Index] = From[Index];
}
static int Same (const U32 *A, const U32 *B, U32 Count)
{
  U32 Index;
  for (Index = 0; Index < Count; ++Index)
    if (A[Index] != B[Index])
      return 0;
  return 1;
}
static int HeaderVariant (const U32 *H)
{
  if (H[LiuqinUfsSmmuId0] != 0x4C017E71U || H[LiuqinUfsSmmuId1] != 0x6000004BU ||
      H[LiuqinUfsSmmuId2] != 0x00005111U || H[LiuqinUfsSmmuId7] != 0x00000024U)
    return LiuqinUfsSmmuUnsupportedIdVariant;
  /* IHI0062D.c p9-197: global MemAttr is inactive with MTCFG=0.
   * Accept only these two stored values; never normalize raw snapshot words.
   * Full unique TYPE0/S1 M0/S2 bypass and stability proofs remain mandatory.
   */
  if (H[LiuqinUfsSmmuCr0] != 0x00200406U && H[LiuqinUfsSmmuCr0] != 0x00240406U)
    return LiuqinUfsSmmuUnsupportedControl;
  return 0;
}
static int Select (const LIUQIN_UFS_SMMU_PROBE_PASS *P, U32 *Selected, U32 *Bank, U32 *References)
{
  U32 Index, Matches = 0, Ref = 0, Pick = 0, Cb = 0;
  for (Index = 0; Index < 113U; ++Index)
  {
    U32 Smr = P->Smr[Index], S2cr = P->S2cr[Index];
    U32 Mask = (Smr >> 16) & 0x7FFFU;
    if (Smr == 0xFFFFFFFFU || S2cr == 0xFFFFFFFFU)
      return LiuqinUfsSmmuAllOnes;
    if ((Smr & 0x80000000U) && (((Smr ^ 0xE0U) & 0x7FFFU & ~Mask) == 0))
    {
      ++Matches;
      Pick = Index;
      Cb = S2cr & 0xFFU;
    }
  }
  if (Matches == 0)
    return LiuqinUfsSmmuNoStream;
  if (Matches != 1)
    return LiuqinUfsSmmuAmbiguousStream;
  if (P->Smr[Pick] != 0x800000E0U || P->S2cr[Pick] != Cb)
    return LiuqinUfsSmmuUnsupportedTargetStream;
  if (Cb >= 75U)
    return LiuqinUfsSmmuInvalidBank;
  for (Index = 0; Index < 113U; ++Index)
    if ((P->Smr[Index] & 0x80000000U) && ((P->S2cr[Index] >> 16) & 3U) == 0 &&
        (P->S2cr[Index] & 0xFFU) == Cb)
      ++Ref;
  if (Ref != 1)
    return LiuqinUfsSmmuSharedNsBank;
  *Selected = Pick;
  *Bank = Cb;
  *References = Ref;
  return 0;
}
static int ContextVariant (const U32 *B, U32 *Cache)
{
  if (B[LiuqinUfsSmmuCbar] != 0x0001F000U || B[LiuqinUfsSmmuCba2r] != 1U ||
      B[LiuqinUfsSmmuActlr] != 3U || B[LiuqinUfsSmmuTcr] != 0 || B[LiuqinUfsSmmuTcr2] != 0x60U ||
      B[LiuqinUfsSmmuMair0] != 0 || B[LiuqinUfsSmmuMair1] != 0)
    return LiuqinUfsSmmuUnsupportedContextVariant;
  if (B[LiuqinUfsSmmuSctlr] == 0x000000E0U)
    *Cache = LiuqinUfsSmmuCacheNonCca;
  else if (B[LiuqinUfsSmmuSctlr] == 0x0A9F00E0U)
    *Cache = LiuqinUfsSmmuCacheCca;
  else
    return LiuqinUfsSmmuUnsupportedContextVariant;
  return 0;
}
static int FaultVariant (const U32 *H, const U32 *B)
{
  U32 Fsr = B[LiuqinUfsSmmuFsr], Gfsr = H[LiuqinUfsSmmuGfsr];
  /* IHI0062D.c §16.5.9: [10:9] Format, [0] and [29:11] reserved.
   * NS GFSR bit7 and [30:9] reserved (§9.6.15). Preserve every raw bit.
   */
  if ((Fsr & ~0xC00007FEU) != 0 || (Fsr & 0x600U) != 0x400U || (Gfsr & ~0x8000017FU) != 0)
    return LiuqinUfsSmmuUnsupportedFaultVariant;
  if ((Fsr & 0xC0000000U) != 0 || (Gfsr & 0x80000000U) != 0)
    return LiuqinUfsSmmuActiveFault;
  return 0;
}

int LiuqinUfsSmmuClassify (const LIUQIN_UFS_SMMU_PROBE_SNAPSHOT *S,
                           LIUQIN_UFS_SMMU_PROBE_CANDIDATE *C)
{
  LIUQIN_SMMU_LAYOUT Layout;
  U32 Pass, Index, Pick = 0, Bank = 0, Ref = 0, Cache = 0;
  int Reason;
  Clear (C, sizeof (*C));
  if (!S || !C)
    return LiuqinUfsSmmuInvalidArgument;
  if (S->CaptureReason != 0)
    return S->CaptureReason;
  if (S->VerifiedBase != 0x15000000ULL || S->VerifiedSpan != 0x100000ULL ||
      S->CompletedPasses != 2 || S->ReadAttempts != LIUQIN_UFS_SMMU_PROBE_MAX_READS)
    return LiuqinUfsSmmuIncompleteSnapshot;
  for (Pass = 0; Pass < 2; ++Pass)
  {
    const LIUQIN_UFS_SMMU_PROBE_PASS *P = &S->Pass[Pass];
    if (P->HeaderWordsRead != 11 || P->StreamWordsRead != 226 || P->BankWordsRead != 15 ||
        P->GuardWordsRead != 5)
      return LiuqinUfsSmmuIncompleteSnapshot;
    for (Index = 0; Index < 11; ++Index)
      if (P->Header[Index] == 0xFFFFFFFFU)
        return LiuqinUfsSmmuAllOnes;
    for (Index = 0; Index < 15; ++Index)
      if (P->Bank[Index] == 0xFFFFFFFFU)
        return LiuqinUfsSmmuAllOnes;
    Reason = HeaderVariant (P->Header);
    if (Reason)
      return Reason;
    if (!Same (P->Header, P->Guard, 5))
      return LiuqinUfsSmmuUnstableSnapshot;
    Reason = Select (P, &Pick, &Bank, &Ref);
    if (Reason)
      return Reason;
    Reason = LiuqinSmmuDecodeLayout (S->VerifiedBase, S->VerifiedSpan, P->Header[LiuqinUfsSmmuId0],
                                     P->Header[LiuqinUfsSmmuId1], Bank, &Layout);
    if (Reason)
      return LiuqinUfsSmmuLayoutRejected;
    Reason = ContextVariant (P->Bank, &Cache);
    if (Reason)
      return Reason;
    Reason = FaultVariant (P->Header, P->Bank);
    if (Reason)
      return Reason;
  }
  if (!Same (S->Pass[0].Header, S->Pass[1].Header, 11) ||
      !Same (S->Pass[0].Smr, S->Pass[1].Smr, 113) ||
      !Same (S->Pass[0].S2cr, S->Pass[1].S2cr, 113) || !Same (S->Pass[0].Bank, S->Pass[1].Bank, 15))
    return LiuqinUfsSmmuUnstableSnapshot;
  C->EligibleForIsolatedProbe = 1;
  C->StreamIndex = Pick;
  C->SelectedBank = Bank;
  C->NsReferences = Ref;
  C->CacheVariant = Cache;
  Copy (&C->CandidateLayout, &Layout, sizeof (Layout));
  C->HistoricalContextFaults = S->Pass[0].Bank[LiuqinUfsSmmuFsr] & 0x1FEU;
  C->HistoricalGlobalFaults = S->Pass[0].Header[LiuqinUfsSmmuGfsr] & 0x17FU;
  return 0;
}

static int Read (const LIUQIN_UFS_SMMU_PROBE_IO *Io, LIUQIN_UFS_SMMU_PROBE_SNAPSHOT *S, U32 Pass,
                 U32 Offset, U32 *Value, U32 *Counter)
{
  int Reason = 0;
  if ((Offset & 3U) != 0 || Offset > 0x100000U - 4U ||
      S->ReadAttempts >= LIUQIN_UFS_SMMU_PROBE_MAX_READS)
    Reason = LiuqinUfsSmmuLayoutRejected;
  else
  {
    ++S->ReadAttempts;
    if (Io->Read32 (Io->Context, Offset, Value) != 1)
      Reason = LiuqinUfsSmmuReadFailed;
    else
    {
      ++*Counter;
      if (*Value == 0xFFFFFFFFU)
        Reason = LiuqinUfsSmmuAllOnes;
    }
  }
  if (Reason)
  {
    S->FailedPass = Pass;
    S->FailedOffset = Offset;
  }
  return Reason;
}

int LiuqinUfsSmmuTakeSnapshot (const LIUQIN_UFS_SMMU_PROBE_IO *Io, U64 VerifiedBase,
                               U64 VerifiedSpan, LIUQIN_UFS_SMMU_PROBE_SNAPSHOT *S)
{
  LIUQIN_SMMU_LAYOUT Layout;
  LIUQIN_UFS_SMMU_PROBE_CANDIDATE Candidate;
  U32 Pass, Index, Pick, Bank, Ref;
  int Reason;
  Clear (S, sizeof (*S));
  if (!S)
    return LiuqinUfsSmmuInvalidArgument;
  S->VerifiedBase = VerifiedBase;
  S->VerifiedSpan = VerifiedSpan;
  S->FailedPass = 0xFFFFFFFFU;
  S->FailedOffset = 0xFFFFFFFFU;
  Reason = LiuqinUfsSmmuInvalidArgument;
  if (!Io || !Io->Read32 || VerifiedBase != 0x15000000ULL || VerifiedSpan != 0x100000ULL)
    goto Done;
  for (Pass = 0; Pass < 2; ++Pass)
  {
    LIUQIN_UFS_SMMU_PROBE_PASS *P = &S->Pass[Pass];
    for (Index = 0; Index < 5; ++Index)
    {
      Reason = Read (Io, S, Pass, HeaderOffsets[Index], &P->Header[Index], &P->HeaderWordsRead);
      if (Reason)
        goto Done;
    }
    Reason = HeaderVariant (P->Header);
    if (Reason)
      goto Done;
    for (; Index < 11; ++Index)
    {
      Reason = Read (Io, S, Pass, HeaderOffsets[Index], &P->Header[Index], &P->HeaderWordsRead);
      if (Reason)
        goto Done;
    }
    for (Index = 0; Index < 113; ++Index)
    {
      Reason = Read (Io, S, Pass, 0x800U + Index * 4U, &P->Smr[Index], &P->StreamWordsRead);
      if (Reason)
        goto Done;
      Reason = Read (Io, S, Pass, 0xC00U + Index * 4U, &P->S2cr[Index], &P->StreamWordsRead);
      if (Reason)
        goto Done;
    }
    /* Never follow a second-pass CBIDX that differs from the first full scan.
     * Stable NS table state is still not a secure/remote ownership proof.
     */
    if (Pass != 0 && (!Same (S->Pass[0].Header, P->Header, 11) ||
                      !Same (S->Pass[0].Smr, P->Smr, 113) || !Same (S->Pass[0].S2cr, P->S2cr, 113)))
    {
      Reason = LiuqinUfsSmmuUnstableSnapshot;
      goto Done;
    }
    Reason = Select (P, &Pick, &Bank, &Ref);
    if (Reason)
      goto Done;
    S->LayoutReason =
        LiuqinSmmuDecodeLayout (VerifiedBase, VerifiedSpan, P->Header[LiuqinUfsSmmuId0],
                                P->Header[LiuqinUfsSmmuId1], Bank, &Layout);
    if (S->LayoutReason)
    {
      Reason = LiuqinUfsSmmuLayoutRejected;
      goto Done;
    }
    {
      U32 Attributes[3] = {(U32)(Layout.Cbar - VerifiedBase), (U32)(Layout.Cba2r - VerifiedBase),
                           (U32)(Layout.Cbfrsynra - VerifiedBase)};
      for (Index = 0; Index < 3; ++Index)
      {
        Reason = Read (Io, S, Pass, Attributes[Index], &P->Bank[Index], &P->BankWordsRead);
        if (Reason)
          goto Done;
      }
    }
    for (Index = 0; Index < 12; ++Index)
    {
      Reason = Read (Io, S, Pass, (U32)(Layout.Context - VerifiedBase) + BankOffsets[Index],
                     &P->Bank[3 + Index], &P->BankWordsRead);
      if (Reason)
        goto Done;
    }
    for (Index = 0; Index < 5; ++Index)
    {
      Reason = Read (Io, S, Pass, HeaderOffsets[Index], &P->Guard[Index], &P->GuardWordsRead);
      if (Reason)
        goto Done;
    }
    ++S->CompletedPasses;
  }
  S->CaptureReason = 0;
  Reason = LiuqinUfsSmmuClassify (S, &Candidate);
Done:
  S->CaptureReason = Reason;
  return Reason;
}

const char *LiuqinUfsSmmuProbeReasonName (int Reason)
{
  switch (Reason)
  {
  case LiuqinUfsSmmuEligibleForIsolatedProbe:
    return "eligible-for-isolated-probe";
  case LiuqinUfsSmmuInvalidArgument:
    return "invalid or unauthorized snapshot arguments";
  case LiuqinUfsSmmuIncompleteSnapshot:
    return "incomplete bounded two-pass snapshot";
  case LiuqinUfsSmmuReadFailed:
    return "register read did not complete";
  case LiuqinUfsSmmuAllOnes:
    return "all-ones register read";
  case LiuqinUfsSmmuLayoutRejected:
    return "register layout rejected";
  case LiuqinUfsSmmuUnsupportedIdVariant:
    return "unsupported SMMU ID register variant";
  case LiuqinUfsSmmuUnsupportedControl:
    return "unsupported inherited CR0 variant";
  case LiuqinUfsSmmuNoStream:
    return "no valid NS stream for SID e0";
  case LiuqinUfsSmmuAmbiguousStream:
    return "multiple or overlapping SID e0 streams";
  case LiuqinUfsSmmuUnsupportedTargetStream:
    return "SID e0 is not exact TYPE0 without overrides";
  case LiuqinUfsSmmuSharedNsBank:
    return "selected bank has multiple NS TYPE0 references";
  case LiuqinUfsSmmuInvalidBank:
    return "selected bank index is out of range";
  case LiuqinUfsSmmuUnsupportedContextVariant:
    return "unsupported inherited bypass context variant";
  case LiuqinUfsSmmuUnsupportedFaultVariant:
    return "unsupported fault register bits or format";
  case LiuqinUfsSmmuActiveFault:
    return "stalled or multiple fault evidence";
  case LiuqinUfsSmmuUnstableSnapshot:
    return "stream, context or fault snapshot changed";
  default:
    return "unknown UFS SMMU probe failure";
  }
}
