/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Liuqin platform contributors. */
#include <SmmuLayout.h>

typedef LIUQIN_SMMU_U32 U32;
typedef LIUQIN_SMMU_U64 U64;

_Static_assert (__CHAR_BIT__ == 8 && sizeof (U32) == 4 && sizeof (U64) == 8,
                "SMMU decoder requires 8/32/64-bit integer widths");

static void Clear (LIUQIN_SMMU_LAYOUT *Output)
{
  unsigned char *At = (unsigned char *)Output;
  __SIZE_TYPE__ Index;
  if (Output)
    for (Index = 0; Index < sizeof (*Output); ++Index)
      At[Index] = 0;
}

static int Within (U64 Span, U64 Offset, U64 Length)
{
  return Length != 0 && Offset < Span && Length <= Span - Offset;
}

int LiuqinSmmuDecodeLayout (U64 Base, U64 AvailableSpan, U32 Id0, U32 Id1, U32 SelectedBank,
                            LIUQIN_SMMU_LAYOUT *Output)
{
  U32 Groups, Shift, PageSize, GlobalPages, Banks, S2Banks;
  U64 EncodedSpan, ContextOffset, AllBanksEnd, CbarOffset, Cba2rOffset, CbfrsynraOffset;
  Clear (Output);
  if (!Output)
    return LiuqinSmmuLayoutInvalidArgument;
  /* ID0's other capabilities are opaque; only the relevant subset is required. */
  if (Id0 == 0 || Id0 == 0xFFFFFFFFU || !(Id0 & (1U << 27)) || !(Id0 & ((1U << 30) | (1U << 29))))
    return LiuqinSmmuLayoutCapability;
  Groups = Id0 & 0xFFU;
  if (Groups == 0 || Groups > LIUQIN_SMMU_MAX_STREAM_GROUPS)
    return LiuqinSmmuLayoutStreamCount;
  /* Fail closed on unnamed ID1 bits: this is a project diagnostic policy. */
  if (Id1 == 0 || Id1 == 0xFFFFFFFFU || (Id1 & ~0xF0FF00FFU) != 0)
    return LiuqinSmmuLayoutId1;
  Shift = (Id1 & 0x80000000U) ? 16U : 12U;
  PageSize = 1U << Shift;
  GlobalPages = 1U << (((Id1 >> 28) & 7U) + 1U);
  Banks = Id1 & 0xFFU;
  S2Banks = (Id1 >> 16) & 0xFFU;
  if (Banks == 0 || S2Banks > Banks || Banks > GlobalPages)
    return LiuqinSmmuLayoutContextCount;
  if (SelectedBank >= Banks)
    return LiuqinSmmuLayoutContextIndex;
  EncodedSpan = (U64)GlobalPages * 2U * PageSize;
  if (AvailableSpan != EncodedSpan)
    return LiuqinSmmuLayoutWindowMismatch;
  if (Base == 0 || (Base & (PageSize - 1U)) != 0 || AvailableSpan > ~(U64)0 - Base)
    return LiuqinSmmuLayoutAddressRange;

  ContextOffset = ((U64)GlobalPages + SelectedBank) * PageSize;
  AllBanksEnd = ((U64)GlobalPages + Banks) * PageSize;
  CbarOffset = (U64)PageSize + (U64)SelectedBank * 4U;
  Cba2rOffset = CbarOffset + 0x800U;
  CbfrsynraOffset = CbarOffset + 0x400U;
  /* Check both full global pages, every advertised CB, both stream tables,
   * and the selected GR1 attributes, before producing any addresses. */
  if (!Within (AvailableSpan, 0, (U64)PageSize * 2U) ||
      !Within (AvailableSpan, (U64)GlobalPages * PageSize, (U64)Banks * PageSize) ||
      AllBanksEnd > EncodedSpan || !Within (AvailableSpan, ContextOffset, PageSize) ||
      !Within (PageSize, LiuqinSmmuGr0Smr, (U64)Groups * 4U) ||
      !Within (PageSize, LiuqinSmmuGr0S2cr, (U64)Groups * 4U) ||
      !Within (PageSize, (U64)Banks * 4U - 4U + 0x800U, 4U) ||
      !Within (AvailableSpan, CbarOffset, 4U) || !Within (AvailableSpan, Cba2rOffset, 4U) ||
      !Within (AvailableSpan, CbfrsynraOffset, 4U))
    return LiuqinSmmuLayoutAddressRange;

  Output->Base = Base;
  Output->AvailableSpan = AvailableSpan;
  Output->EncodedSpan = EncodedSpan;
  Output->Global1 = Base + PageSize;
  Output->Context = Base + ContextOffset;
  Output->Cbar = Base + CbarOffset;
  Output->Cba2r = Base + Cba2rOffset;
  Output->Cbfrsynra = Base + CbfrsynraOffset;
  Output->PageShift = Shift;
  Output->PageSize = PageSize;
  Output->GlobalPages = GlobalPages;
  Output->StreamGroups = Groups;
  Output->ContextBanks = Banks;
  Output->Stage2OnlyBanks = S2Banks;
  Output->SelectedBank = SelectedBank;
  return LiuqinSmmuLayoutOk;
}

const char *LiuqinSmmuLayoutReasonName (int Reason)
{
  switch (Reason)
  {
  case LiuqinSmmuLayoutOk:
    return "bounded candidate register layout";
  case LiuqinSmmuLayoutInvalidArgument:
    return "invalid output argument";
  case LiuqinSmmuLayoutCapability:
    return "unsupported stream-matching capability";
  case LiuqinSmmuLayoutStreamCount:
    return "unsupported stream-group count";
  case LiuqinSmmuLayoutId1:
    return "unknown ID1 register layout";
  case LiuqinSmmuLayoutContextCount:
    return "invalid context-bank counts";
  case LiuqinSmmuLayoutContextIndex:
    return "context-bank index out of range";
  case LiuqinSmmuLayoutWindowMismatch:
    return "encoded and verified MMIO windows differ";
  case LiuqinSmmuLayoutAddressRange:
    return "register address outside bounded window";
  default:
    return "unknown SMMU layout failure";
  }
}
