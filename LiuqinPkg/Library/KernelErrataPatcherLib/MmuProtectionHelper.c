/* Based on https://github.com/SamuelTulach/rainbow
 * Copyright (c) 2021 Samuel Tulach
 * Copyright (c) 2022-2023 DuoWoA authors
 * SPDX-License-Identifier: MIT */
#include "KernelErrataPatcherLib.h"
#include <IndustryStandard/PeImage.h>

#define WINLOAD_MAX_SECTIONS 96U

typedef struct
{
  EFI_PHYSICAL_ADDRESS Base;
  UINTN Length;
  UINT64 Attributes;
  BOOLEAN RestoreRo;
} WINLOAD_EXECUTABLE_RANGE;

STATIC WINLOAD_EXECUTABLE_RANGE mExecutableRanges[WINLOAD_MAX_SECTIONS];
STATIC UINTN mExecutableRangeCount;
STATIC EFI_PHYSICAL_ADDRESS mWinloadBase;
STATIC UINTN mWinloadLength;
STATIC EFI_MEMORY_ATTRIBUTE_PROTOCOL *mMemoryAttributeProtocol = NULL;

/* Keep failed restores recorded so a later cleanup attempt can retry them. */
STATIC EFI_STATUS RestoreExecutableRanges (VOID)
{
  EFI_STATUS Status = EFI_SUCCESS;
  UINTN Index;

  for (Index = mExecutableRangeCount; Index > 0; --Index)
  {
    WINLOAD_EXECUTABLE_RANGE *Range = &mExecutableRanges[Index - 1];
    EFI_STATUS RestoreStatus;

    if (!Range->RestoreRo)
    {
      continue;
    }
    RestoreStatus = mMemoryAttributeProtocol->SetMemoryAttributes (
        mMemoryAttributeProtocol, Range->Base, Range->Length, EFI_MEMORY_RO);
    if (EFI_ERROR (RestoreStatus))
    {
      if (!EFI_ERROR (Status))
      {
        Status = RestoreStatus;
      }
    }
    else
    {
      Range->RestoreRo = FALSE;
    }
  }
  if (!EFI_ERROR (Status))
  {
    mExecutableRangeCount = 0;
    mWinloadBase = 0;
    mWinloadLength = 0;
  }
  return Status;
}

/* Validate the complete table before changing any attributes. A section's
   trailing page padding belongs to that executable section; never include a
   neighboring section or an image header page in a protection operation. */
STATIC EFI_STATUS CollectExecutableRanges (EFI_PHYSICAL_ADDRESS Base, UINTN Length)
{
  UINT8 *Image;
  EFI_IMAGE_FILE_HEADER *FileHeader;
  EFI_IMAGE_OPTIONAL_HEADER64 OptionalStorage;
  EFI_IMAGE_OPTIONAL_HEADER64 *OptionalHeader;
  EFI_IMAGE_SECTION_HEADER *Sections;
  UINT32 PeOffset;
  UINTN TableOffset;
  UINTN ImageSize;
  UINTN Index;
  UINTN Other;

  if (Base == 0 || Base > MAX_UINTN - Length || (Base & EFI_PAGE_MASK) != 0 ||
      Length < sizeof (EFI_IMAGE_DOS_HEADER))
  {
    return EFI_INVALID_PARAMETER;
  }
  Image = (UINT8 *)(UINTN)Base;
  if (ReadUnaligned16 ((UINT16 *)Image) != EFI_IMAGE_DOS_SIGNATURE)
  {
    return EFI_COMPROMISED_DATA;
  }
  PeOffset = ReadUnaligned32 ((UINT32 *)(Image + OFFSET_OF (EFI_IMAGE_DOS_HEADER, e_lfanew)));
  if (PeOffset > Length - sizeof (UINT32) - sizeof (*FileHeader) || (PeOffset & 3U) != 0 ||
      ReadUnaligned32 ((UINT32 *)(Image + PeOffset)) != EFI_IMAGE_NT_SIGNATURE)
  {
    return EFI_COMPROMISED_DATA;
  }
  FileHeader = (EFI_IMAGE_FILE_HEADER *)(Image + PeOffset + sizeof (UINT32));
  TableOffset = PeOffset + sizeof (UINT32) + sizeof (*FileHeader);
  if (FileHeader->NumberOfSections == 0 || FileHeader->NumberOfSections > WINLOAD_MAX_SECTIONS ||
      FileHeader->SizeOfOptionalHeader < sizeof (*OptionalHeader) ||
      FileHeader->SizeOfOptionalHeader > Length - TableOffset)
  {
    return EFI_COMPROMISED_DATA;
  }
  CopyMem (&OptionalStorage, Image + TableOffset, sizeof (OptionalStorage));
  OptionalHeader = &OptionalStorage;
  ImageSize = OptionalHeader->SizeOfImage;
  if (OptionalHeader->Magic != EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC || ImageSize != Length ||
      (ImageSize & EFI_PAGE_MASK) != 0)
  {
    return EFI_COMPROMISED_DATA;
  }
  TableOffset += FileHeader->SizeOfOptionalHeader;
  if ((TableOffset & 3U) != 0 ||
      FileHeader->NumberOfSections > (Length - TableOffset) / sizeof (*Sections))
  {
    return EFI_COMPROMISED_DATA;
  }
  Sections = (EFI_IMAGE_SECTION_HEADER *)(Image + TableOffset);
  TableOffset += FileHeader->NumberOfSections * sizeof (*Sections);
  if (OptionalHeader->SizeOfHeaders < TableOffset || OptionalHeader->SizeOfHeaders > ImageSize)
  {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index < FileHeader->NumberOfSections; ++Index)
  {
    UINTN Start = Sections[Index].VirtualAddress;
    UINTN Size = MAX (Sections[Index].Misc.VirtualSize, Sections[Index].SizeOfRawData);
    WINLOAD_EXECUTABLE_RANGE *Range;

    if (Size == 0)
    {
      continue;
    }
    if (Start > ImageSize || Size > ImageSize - Start)
    {
      return EFI_COMPROMISED_DATA;
    }
    if ((Sections[Index].Characteristics & EFI_IMAGE_SCN_MEM_EXECUTE) == 0)
    {
      continue;
    }
    if ((Start & EFI_PAGE_MASK) != 0 || Start < OptionalHeader->SizeOfHeaders ||
        Size > MAX_UINTN - EFI_PAGE_MASK)
    {
      return EFI_COMPROMISED_DATA;
    }
    Size = (Size + EFI_PAGE_MASK) & ~(UINTN)EFI_PAGE_MASK;
    if (Size > ImageSize - Start)
    {
      return EFI_COMPROMISED_DATA;
    }
    /* A page shared with any other live section cannot be changed safely. */
    for (Other = 0; Other < FileHeader->NumberOfSections; ++Other)
    {
      UINTN OtherStart = Sections[Other].VirtualAddress;
      UINTN OtherSize = MAX (Sections[Other].Misc.VirtualSize, Sections[Other].SizeOfRawData);

      if (Other == Index || OtherSize == 0)
      {
        continue;
      }
      if (OtherStart > ImageSize || OtherSize > ImageSize - OtherStart ||
          (Start < OtherStart + OtherSize && OtherStart < Start + Size))
      {
        return EFI_COMPROMISED_DATA;
      }
    }
    Range = &mExecutableRanges[mExecutableRangeCount++];
    Range->Base = Base + Start;
    Range->Length = Size;
    Range->Attributes = 0;
    Range->RestoreRo = FALSE;
  }
  return mExecutableRangeCount != 0 ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
UnprotectWinload (EFI_PHYSICAL_ADDRESS WinloadBase, UINTN WinloadLength)
{
  EFI_STATUS Status;
  UINTN Index;

  //
  // No Memory Attribute protocol is installed, winload is thus not protected
  //
  if (mMemoryAttributeProtocol == NULL)
  {
    return EFI_SUCCESS;
  }
  if (mExecutableRangeCount != 0)
  {
    return EFI_ALREADY_STARTED;
  }
  Status = CollectExecutableRanges (WinloadBase, WinloadLength);
  if (EFI_ERROR (Status))
  {
    mExecutableRangeCount = 0;
    return Status;
  }
  mWinloadBase = WinloadBase;
  mWinloadLength = WinloadLength;
  for (Index = 0; Index < mExecutableRangeCount; ++Index)
  {
    WINLOAD_EXECUTABLE_RANGE *Range = &mExecutableRanges[Index];

    Status = mMemoryAttributeProtocol->GetMemoryAttributes (mMemoryAttributeProtocol, Range->Base,
                                                            Range->Length, &Range->Attributes);
    if (EFI_ERROR (Status))
    {
      break;
    }
    if ((Range->Attributes & EFI_MEMORY_RO) == 0)
    {
      continue;
    }
    /* ClearMemoryAttributes may have changed some pages even if it fails. */
    Range->RestoreRo = TRUE;
    Status = mMemoryAttributeProtocol->ClearMemoryAttributes (mMemoryAttributeProtocol, Range->Base,
                                                              Range->Length, EFI_MEMORY_RO);
    if (EFI_ERROR (Status))
    {
      break;
    }
  }
  if (EFI_ERROR (Status))
  {
    EFI_STATUS RestoreStatus = RestoreExecutableRanges ();
    if (EFI_ERROR (RestoreStatus))
    {
      return RestoreStatus;
    }
  }
  return Status;
}

EFI_STATUS
EFIAPI
ReProtectWinload (EFI_PHYSICAL_ADDRESS WinloadBase, UINTN WinloadLength)
{
  //
  // No Memory Attribute protocol is installed, winload is thus not protected
  //
  if (mMemoryAttributeProtocol == NULL)
  {
    return EFI_SUCCESS;
  }
  if (mExecutableRangeCount == 0)
  {
    return EFI_SUCCESS;
  }
  if (WinloadBase != mWinloadBase || WinloadLength != mWinloadLength)
  {
    return EFI_INVALID_PARAMETER;
  }
  return RestoreExecutableRanges ();
}

EFI_STATUS
EFIAPI
InitMemoryAttributeProtocol ()
{
  EFI_STATUS Status = EFI_SUCCESS;

  Status = gBS->LocateProtocol (&gEfiMemoryAttributeProtocolGuid, NULL,
                                (VOID **)&mMemoryAttributeProtocol);

  if (EFI_ERROR (Status))
  {

    Status = EFI_SUCCESS;
  }

  return Status;
}
