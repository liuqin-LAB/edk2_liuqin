/* Based on https://github.com/SamuelTulach/rainbow
 * Copyright (c) 2021 Samuel Tulach
 * Copyright (c) 2022-2023 DuoWoA authors
 * SPDX-License-Identifier: MIT */
#include "KernelErrataPatcherLib.h"
#include "KernelPatchPayload.generated.h"

VOID OslInterceptMain (IN EFI_PHYSICAL_ADDRESS Caller)
{
  UINTN Size = 0;
  EFI_PHYSICAL_ADDRESS Base = LocateWinloadBase (Caller, &Size);
  LIUQIN_KEP_INJECTION Plan;
  EFI_STATUS Status;
  UINT32 CallBranch, TailBranch;

  /* A known unique transfer call and a completely unused executable page
     tail must both exist. Unknown images are left untouched. */
  if (Base == 0 || !LiuqinKepPlanInjection (Base, Size, sizeof (mLiuqinKepPayload), &Plan) ||
      !LiuqinKepBranch (Plan.Call, Plan.Cave, TRUE, &CallBranch) ||
      !LiuqinKepBranch (Plan.Cave + LIUQIN_KEP_PAYLOAD_TAIL_OFFSET, Plan.Target, FALSE,
                        &TailBranch))
  {
    return;
  }
  Status = UnprotectWinload (Base, Size);
  if (EFI_ERROR (Status))
  {
    return;
  }

  CopyMem ((VOID *)(UINTN)(Base + Plan.Cave), mLiuqinKepPayload, sizeof (mLiuqinKepPayload));
  *(UINT32 *)(UINTN)(Base + Plan.Cave + LIUQIN_KEP_PAYLOAD_TAIL_OFFSET) = TailBranch;
  LiuqinKepPatchWinloadTrapRegisters (Base, Size);
  /* Publish the call only after the complete payload has been installed. */
  *(UINT32 *)(UINTN)(Base + Plan.Call) = CallBranch;
  LiuqinKepFlushInstructionCache (Base, Size);
  /* Restoration still runs on every successful injection; its failure never
     replaced the original ExitBootServices status. */
  (VOID) ReProtectWinload (Base, Size);
}
