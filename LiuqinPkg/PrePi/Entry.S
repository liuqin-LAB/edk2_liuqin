// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>

ASM_FUNC(_ModuleEntryPoint)
  msr daifset, #0xf
  msr spsel, #1
  mov x19, x0                       // DTB handed through BootShim
  msr tpidr_el0, xzr                // no HOB exists yet
  MOV64(x2, FixedPcdGet64(PcdFdBaseAddress))
  MOV32(x3, FixedPcdGet32(PcdFdSize))
  add x2, x2, x3
  mov sp, x2                       // validated FD tail, excluded from FV
  mov x29, xzr
  bl ArmEnableVFP
  mov x0, x19
  bl LiuqinPrePiMain
1:
  wfe
  b 1b
