/* Based on https://github.com/SamuelTulach/rainbow
 * Copyright (c) 2021 Samuel Tulach
 * Copyright (c) 2022-2023 DuoWoA authors
 * SPDX-License-Identifier: MIT */
#include "KernelErrataPatcherLib.h"
#include <IndustryStandard/PeImage.h>

/* Querying into static storage does not allocate or change the caller's
   MapKey. Do not walk arbitrary DRAM backwards if winload cannot be found. */
STATIC UINT64 mKepMemoryMap[8192];

STATIC BOOLEAN KepReadableRange (UINTN MapSize, UINTN DescriptorSize, EFI_PHYSICAL_ADDRESS Base,
                                 UINTN Length)
{
  UINTN Offset;
  EFI_PHYSICAL_ADDRESS End, Current = Base;
  if (Length == 0 || Base > MAX_UINT64 - Length)
  {
    return FALSE;
  }
  End = Base + Length;
  while (Current < End)
  {
    EFI_PHYSICAL_ADDRESS Next = Current;
    for (Offset = 0; Offset < MapSize; Offset += DescriptorSize)
    {
      EFI_MEMORY_DESCRIPTOR D;
      UINT64 Bytes;
      CopyMem (&D, (UINT8 *)mKepMemoryMap + Offset, sizeof (D));
      if (D.Type != EfiLoaderCode && D.Type != EfiLoaderData && D.Type != EfiBootServicesCode &&
          D.Type != EfiBootServicesData)
      {
        continue;
      }
      if (D.NumberOfPages > (MAX_UINT64 - D.PhysicalStart) / EFI_PAGE_SIZE)
      {
        continue;
      }
      Bytes = D.NumberOfPages * EFI_PAGE_SIZE;
      if (Current >= D.PhysicalStart && Current - D.PhysicalStart < Bytes)
      {
        Next = MAX (Next, D.PhysicalStart + Bytes);
      }
    }
    if (Next == Current)
    {
      return FALSE;
    }
    Current = Next;
  }
  return TRUE;
}

EFI_PHYSICAL_ADDRESS LocateWinloadBase (EFI_PHYSICAL_ADDRESS Caller, UINTN *Size)
{
  EFI_PHYSICAL_ADDRESS Base = Caller & ~(EFI_PHYSICAL_ADDRESS)EFI_PAGE_MASK;
  UINTN Page, MapSize = sizeof (mKepMemoryMap), MapKey, DescriptorSize = 0;
  UINT32 Version;
  EFI_STATUS Status;
  *Size = 0;
  Status = gBS->GetMemoryMap (&MapSize, (EFI_MEMORY_DESCRIPTOR *)mKepMemoryMap, &MapKey,
                              &DescriptorSize, &Version);
  if (EFI_ERROR (Status) || MapSize > sizeof (mKepMemoryMap) ||
      DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR) || MapSize % DescriptorSize != 0)
  {
    return 0;
  }
  for (Page = 0; Page < 0x4000U; ++Page)
  {
    UINT8 *Image = (UINT8 *)(UINTN)Base;
    if (!KepReadableRange (MapSize, DescriptorSize, Base, EFI_PAGE_SIZE))
    {
      break;
    }
    if (ReadUnaligned16 ((UINT16 *)Image) == EFI_IMAGE_DOS_SIGNATURE)
    {
      UINT32 Pe = ReadUnaligned32 ((UINT32 *)(Image + 0x3c));
      if ((Pe & 3U) == 0 && Pe <= EFI_PAGE_SIZE - 0x58U &&
          ReadUnaligned32 ((UINT32 *)(Image + Pe)) == EFI_IMAGE_NT_SIGNATURE &&
          ReadUnaligned16 ((UINT16 *)(Image + Pe + 4)) == 0xaa64U &&
          ReadUnaligned16 ((UINT16 *)(Image + Pe + 20)) >= 0x40U &&
          ReadUnaligned16 ((UINT16 *)(Image + Pe + 24)) == 0x20bU)
      {
        UINT32 ImageSize = ReadUnaligned32 ((UINT32 *)(Image + Pe + 0x50));
        if (ImageSize >= EFI_PAGE_SIZE && ImageSize <= 0x4000000U &&
            (ImageSize & EFI_PAGE_MASK) == 0 && Caller - Base < ImageSize &&
            KepReadableRange (MapSize, DescriptorSize, Base, ImageSize))
        {
          *Size = ImageSize;
          return Base;
        }
      }
    }
    if (Base < EFI_PAGE_SIZE)
    {
      break;
    }
    Base -= EFI_PAGE_SIZE;
  }
  return 0;
}

/* All image consumers use the same strict loaded-PE contract. Neither a
   matching word in data nor an invalid section authorizes any code write. */
#define KEP_MAX_SECTIONS 96U

STATIC BOOLEAN KepSections (UINT8 *Image, UINTN Size, EFI_IMAGE_SECTION_HEADER **Sections,
                            UINTN *Count)
{
  UINT32 Pe;
  UINTN Table, Index, Other;
  EFI_IMAGE_FILE_HEADER *File;
  EFI_IMAGE_OPTIONAL_HEADER64 OptionalStorage;
  EFI_IMAGE_OPTIONAL_HEADER64 *Optional;

  if (Image == NULL || ((UINTN)Image & EFI_PAGE_MASK) != 0 ||
      Size < sizeof (EFI_IMAGE_DOS_HEADER) || (UINTN)Image > MAX_UINTN - Size ||
      ReadUnaligned16 ((UINT16 *)Image) != EFI_IMAGE_DOS_SIGNATURE)
  {
    return FALSE;
  }
  Pe = ReadUnaligned32 ((UINT32 *)(Image + 0x3c));
  if ((Pe & 3U) != 0 || Pe > Size - 24U ||
      ReadUnaligned32 ((UINT32 *)(Image + Pe)) != EFI_IMAGE_NT_SIGNATURE)
  {
    return FALSE;
  }
  File = (EFI_IMAGE_FILE_HEADER *)(Image + Pe + 4U);
  Table = Pe + 24U;
  if (File->Machine != 0xaa64U || File->NumberOfSections == 0 ||
      File->NumberOfSections > KEP_MAX_SECTIONS ||
      File->SizeOfOptionalHeader < sizeof (*Optional) || File->SizeOfOptionalHeader > Size - Table)
  {
    return FALSE;
  }
  CopyMem (&OptionalStorage, Image + Table, sizeof (OptionalStorage));
  Optional = &OptionalStorage;
  if (Optional->Magic != EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC || Optional->SizeOfImage != Size ||
      (Size & EFI_PAGE_MASK) != 0 || Optional->SectionAlignment != EFI_PAGE_SIZE)
  {
    return FALSE;
  }
  Table += File->SizeOfOptionalHeader;
  if ((Table & 3U) != 0 || File->NumberOfSections > (Size - Table) / sizeof (**Sections))
  {
    return FALSE;
  }
  *Sections = (EFI_IMAGE_SECTION_HEADER *)(Image + Table);
  *Count = File->NumberOfSections;
  Table += *Count * sizeof (**Sections);
  if (Optional->SizeOfHeaders < Table || Optional->SizeOfHeaders > Size)
  {
    return FALSE;
  }
  for (Index = 0; Index < *Count; ++Index)
  {
    EFI_IMAGE_SECTION_HEADER *S = &(*Sections)[Index];
    UINTN Start = S->VirtualAddress;
    UINTN Length = MAX (S->Misc.VirtualSize, S->SizeOfRawData);
    if (Length == 0)
    {
      continue;
    }
    if ((Start & EFI_PAGE_MASK) != 0 || Start < Optional->SizeOfHeaders || Start > Size ||
        Length > Size - Start)
    {
      return FALSE;
    }
    Length = ALIGN_VALUE (Length, EFI_PAGE_SIZE);
    if (Length > Size - Start)
    {
      return FALSE;
    }
    for (Other = 0; Other < Index; ++Other)
    {
      EFI_IMAGE_SECTION_HEADER *T = &(*Sections)[Other];
      UINTN TLength = MAX (T->Misc.VirtualSize, T->SizeOfRawData);
      if (TLength != 0 && Start < T->VirtualAddress + ALIGN_VALUE (TLength, EFI_PAGE_SIZE) &&
          T->VirtualAddress < Start + Length)
      {
        return FALSE;
      }
    }
  }
  return TRUE;
}

BOOLEAN
LiuqinKepBranch (UINTN From, UINTN To, BOOLEAN Link, UINT32 *Instruction)
{
  INT64 Delta;
  if ((From & 3U) != 0 || (To & 3U) != 0 || (From > To ? From - To : To - From) > 0x8000000U)
  {
    return FALSE;
  }
  Delta = (INT64)To - (INT64)From;
  if (Delta < -0x8000000LL || Delta >= 0x8000000LL)
  {
    return FALSE;
  }
  *Instruction = (Link ? 0x94000000U : 0x14000000U) | ((UINT32)(Delta / 4) & 0x03ffffffU);
  return TRUE;
}

/* Plan first, mutate later. The payload lives only beyond BOTH VirtualSize
   and SizeOfRawData, within an executable section's already mapped final
   page. The complete PE is validated, so this cannot overlap another section.
   Existing live code before the transfer routine is never used as a cave. */
BOOLEAN
LiuqinKepPlanInjection (EFI_PHYSICAL_ADDRESS Base, UINTN Size, UINTN PayloadSize,
                        LIUQIN_KEP_INJECTION *Plan)
{
  STATIC CONST struct
  {
    UINT32 Target;
    UINT32 Following;
  } Candidates[] = {{0x1a80, 0x52800015}, {0x19d0, 0x52800015}, {0x1860, 0x52800015},
                    {0x14d0, 0x52800014}, {0x1450, 0xd2800002}, {0x1080, 0xd2800002},
                    {0x1000, 0xd2800002}};
  UINT8 *Image = (UINT8 *)(UINTN)Base;
  EFI_IMAGE_SECTION_HEADER *Sections;
  UINTN Count, Index, Candidate, Current, Matches = 0;
  UINT32 Branch;
  BOOLEAN TargetExecutable;

  ZeroMem (Plan, sizeof (*Plan));
  if (PayloadSize < 4 || (PayloadSize & 3U) != 0 || !KepSections (Image, Size, &Sections, &Count))
  {
    return FALSE;
  }
  for (Candidate = 0; Candidate < ARRAY_SIZE (Candidates); ++Candidate)
  {
    UINTN Target = Candidates[Candidate].Target;
    TargetExecutable = FALSE;
    for (Index = 0; Index < Count; ++Index)
    {
      EFI_IMAGE_SECTION_HEADER *S = &Sections[Index];
      if ((S->Characteristics & EFI_IMAGE_SCN_MEM_EXECUTE) != 0 && Target >= S->VirtualAddress &&
          Target + 16 <= S->VirtualAddress + S->Misc.VirtualSize &&
          (ReadUnaligned32 ((UINT32 *)(Image + Target)) & 0x9f00001fU) == 0x90000002U &&
          (ReadUnaligned32 ((UINT32 *)(Image + Target + 4)) & 0xffc003ffU) == 0xf9400042U &&
          ReadUnaligned32 ((UINT32 *)(Image + Target + 8)) == 0x9100005fU &&
          ReadUnaligned32 ((UINT32 *)(Image + Target + 12)) == 0xaa0003f3U)
      {
        TargetExecutable = TRUE;
      }
    }
    if (!TargetExecutable)
    {
      continue;
    }
    for (Index = 0; Index < Count; ++Index)
    {
      EFI_IMAGE_SECTION_HEADER *S = &Sections[Index];
      if ((S->Characteristics & EFI_IMAGE_SCN_MEM_EXECUTE) == 0)
      {
        continue;
      }
      for (Current = S->VirtualAddress; Current + 8 <= S->VirtualAddress + S->Misc.VirtualSize;
           Current += 4)
      {
        if (LiuqinKepBranch (Current, Target, TRUE, &Branch) &&
            ReadUnaligned32 ((UINT32 *)(Image + Current)) == Branch &&
            ReadUnaligned32 ((UINT32 *)(Image + Current + 4)) == Candidates[Candidate].Following)
        {
          Plan->Call = Current;
          Plan->Target = Target;
          ++Matches;
        }
      }
    }
  }
  if (Matches != 1)
  {
    return FALSE;
  }
  for (Index = 0; Index < Count; ++Index)
  {
    EFI_IMAGE_SECTION_HEADER *S = &Sections[Index];
    UINTN Length = MAX (S->Misc.VirtualSize, S->SizeOfRawData);
    UINTN Start = S->VirtualAddress + ALIGN_VALUE (Length, 16U);
    UINTN End = S->VirtualAddress + ALIGN_VALUE (Length, EFI_PAGE_SIZE);
    BOOLEAN Empty = TRUE;
    if ((S->Characteristics & EFI_IMAGE_SCN_MEM_EXECUTE) == 0 || Start > End ||
        PayloadSize > End - Start)
    {
      continue;
    }
    for (Current = Start; Current < Start + PayloadSize; ++Current)
    {
      if (Image[Current] != 0)
      {
        Empty = FALSE;
        break;
      }
    }
    if (Empty && LiuqinKepBranch (Plan->Call, Start, TRUE, &Branch) &&
        LiuqinKepBranch (Start + PayloadSize - 4, Plan->Target, FALSE, &Branch))
    {
      Plan->Cave = Start;
      return TRUE;
    }
  }
  return FALSE;
}

VOID LiuqinKepPatchWinloadTrapRegisters (EFI_PHYSICAL_ADDRESS Base, UINTN Size)
{
  UINT8 *Image = (UINT8 *)(UINTN)Base;
  EFI_IMAGE_SECTION_HEADER *Sections;
  UINTN Count, Index, Current;
  if (!KepSections (Image, Size, &Sections, &Count))
  {
    return;
  }
  for (Index = 0; Index < Count; ++Index)
  {
    EFI_IMAGE_SECTION_HEADER *S = &Sections[Index];
    if ((S->Characteristics & EFI_IMAGE_SCN_MEM_EXECUTE) == 0)
    {
      continue;
    }
    for (Current = S->VirtualAddress; Current + 4 <= S->VirtualAddress + S->Misc.VirtualSize;
         Current += 4)
    {
      UINT32 *Word = (UINT32 *)(Image + Current);
      UINT32 Value = *Word, Op = Value & 0xffffffe0U;
      if (Op == 0xd5381020U || Op == 0xd53bd2a0U || Op == 0xd53bd2c0U)
      {
        *Word = 0xd2800000U | (Value & 31U);
      }
      else if (Op == 0xd5181020U || Op == 0xd51bd2a0U || Op == 0xd51bd2c0U)
      {
        *Word = 0xd503201fU;
      }
    }
  }
}

VOID LiuqinKepFlushInstructionCache (EFI_PHYSICAL_ADDRESS Base, UINTN Size)
{
  if (Size != 0)
  {
    WriteBackDataCacheRange ((VOID *)(UINTN)Base, Size);
    InvalidateInstructionCacheRange ((VOID *)(UINTN)Base, Size);
  }
}
