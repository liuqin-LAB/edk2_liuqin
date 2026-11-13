/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <BootPolicyLib.h>

/* Compare one character of an EFI path: case-insensitive, and '\\' == '/'. */
STATIC
CHAR16
FoldPathChar (CHAR16 Character)
{
  if ((Character >= (CHAR16)'A') && (Character <= (CHAR16)'Z'))
  {
    return (CHAR16)(Character + 32);
  }

  if (Character == (CHAR16)'/')
  {
    return (CHAR16)'\\';
  }

  return Character;
}

STATIC
BOOLEAN
PathEquals (CONST CHAR16 *Path, CONST CHAR16 *Expected)
{
  if ((Path == NULL) || (Expected == NULL))
  {
    return FALSE;
  }

  while ((*Path != 0) && (*Expected != 0))
  {
    if (FoldPathChar (*Path) != FoldPathChar (*Expected))
    {
      return FALSE;
    }

    ++Path;
    ++Expected;
  }

  return (BOOLEAN)((*Path == 0) && (*Expected == 0));
}

LIUQIN_BOOT_CLASS
LiuqinBootClassifyPath (CONST CHAR16 *Path)
{
  if ((Path == NULL) || (*Path == 0))
  {
    return LiuqinBootClassNone;
  }

  if (PathEquals (Path, LIUQIN_BOOT_WINDOWS_PATH))
  {
    return LiuqinBootClassWindows;
  }

  if (PathEquals (Path, LIUQIN_BOOT_REMOVABLE_PATH))
  {
    return LiuqinBootClassRemovable;
  }

  return LiuqinBootClassNone;
}

UINTN
LiuqinBootRank (LIUQIN_BOOT_CLASS Class)
{
  switch (Class)
  {
  case LiuqinBootClassWindows:
    return 0U;
  case LiuqinBootClassRemovable:
    return 1U;
  case LiuqinBootClassMedia:
    return 2U;
  case LiuqinBootClassShell:
    return 3U;
  default:
    return 4U;
  }
}

INTN LiuqinBootCompare (LIUQIN_BOOT_CLASS ClassA, CONST UINT8 *PathA, UINTN SizeA,
                        LIUQIN_BOOT_CLASS ClassB, CONST UINT8 *PathB, UINTN SizeB)
{
  UINTN RankA;
  UINTN RankB;
  UINTN Common;
  UINTN Index;

  RankA = LiuqinBootRank (ClassA);
  RankB = LiuqinBootRank (ClassB);
  if (RankA != RankB)
  {
    return (RankA < RankB) ? -1 : 1;
  }

  Common = (SizeA < SizeB) ? SizeA : SizeB;
  for (Index = 0U; Index < Common; ++Index)
  {
    if (PathA[Index] != PathB[Index])
    {
      return (PathA[Index] < PathB[Index]) ? -1 : 1;
    }
  }

  if (SizeA == SizeB)
  {
    return 0;
  }

  return (SizeA < SizeB) ? -1 : 1;
}
