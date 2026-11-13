/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_HANDOFF_H
#define LIUQIN_HANDOFF_H
#include <Uefi.h>
#define LIUQIN_HANDOFF_GUID                                                                        \
  {0x9d65870b, 0x5f41, 0x4a40, {0x92, 0xa7, 0xd3, 0x28, 0x4e, 0x73, 0x9c, 0x11}}
#define LIUQIN_HANDOFF_VERSION 1
#define LIUQIN_FB_FROM_DTB 1
#define LIUQIN_FB_FROM_REFERENCE 2
typedef struct
{
  UINT32 Version;
  UINT32 CurrentEl;
  UINT64 DtbBase;
  UINT64 DtbSize;
  UINT64 FramebufferBase;
  UINT64 FramebufferSize;
  UINT32 Width;
  UINT32 Height;
  UINT32 Stride;
  UINT32 FramebufferSource;
} LIUQIN_HANDOFF;
extern EFI_GUID gLiuqinHandoffGuid;
#endif
