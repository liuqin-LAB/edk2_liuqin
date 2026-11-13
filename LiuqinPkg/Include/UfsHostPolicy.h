/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_UFS_HOST_POLICY_H
#define LIUQIN_UFS_HOST_POLICY_H
#include <Base.h>
#include <MemoryPlan.h>

typedef UINT32 (*LIUQIN_UFS_READ32) (VOID *Context, UINTN Address);
typedef VOID (*LIUQIN_UFS_WRITE32) (VOID *Context, UINTN Address, UINT32 Value);
typedef VOID (*LIUQIN_UFS_DELAY) (VOID *Context, UINTN Microseconds);

/* Local DT qcom,num-smr-override is a ceiling, not an exact hardware count. */
#define LIUQIN_UFS_SMMU_MAX_GROUPS 120U

/* UFS controller ownership and DMA policy. */
BOOLEAN LiuqinUfsReadOnlyUpiu (CONST UINT8 *Upiu, UINTN Size, UINT8 Direction);

BOOLEAN LiuqinUfsRange (UINT64 Base, UINT64 Size, UINT64 Address, UINT64 Length);
BOOLEAN LiuqinUfsRamRange (CONST LIUQIN_MEMORY_PLAN *Plan, UINT64 Address, UINT64 Length);
BOOLEAN LiuqinUfsStreamMatches (UINT32 Smr, UINT32 S2cr, UINT16 Sid, BOOLEAN Extended);
/* ID0.NUMSMRG is the actual number of groups; zero means unproven layout/state. */
UINTN LiuqinUfsStreamCount (UINT32 Cr0, UINT32 Id0);
BOOLEAN LiuqinUfsStreamBypass (UINT32 Cr0, UINT32 Id0, CONST UINT32 *Smr, CONST UINT32 *S2cr,
                               UINTN Count, UINT16 Sid);
/* Snapshot = GDSC, BCR, AXI, aggre AXI, AHB, UniPro. Reads only GCC. */
BOOLEAN LiuqinUfsClocksLive (LIUQIN_UFS_READ32 Read32, VOID *Context, UINT32 Snapshot[6]);
/* Stop host lists, then prove readback and both doorbells idle within 100ms. */
BOOLEAN LiuqinUfsQuiesce (LIUQIN_UFS_READ32 Read32, LIUQIN_UFS_WRITE32 Write32,
                          LIUQIN_UFS_DELAY Delay, VOID *Context);

#define LIUQIN_UFS_RECOVERY_MAX_POLLS 1000U

#define LIUQIN_UFS_RECOVERY_OK 0U
#define LIUQIN_UFS_RECOVERY_BAD_ARGUMENTS 1U
#define LIUQIN_UFS_RECOVERY_NOT_INHERITED 2U
#define LIUQIN_UFS_RECOVERY_STOP_FAILED 3U
#define LIUQIN_UFS_RECOVERY_HCE_STOP_FAILED 4U
#define LIUQIN_UFS_RECOVERY_HCE_START_FAILED 5U
#define LIUQIN_UFS_RECOVERY_UIC_READY_FAILED 6U
#define LIUQIN_UFS_RECOVERY_LINK_STARTUP_FAILED 7U
#define LIUQIN_UFS_RECOVERY_DEVICE_ABSENT 8U
#define LIUQIN_UFS_RECOVERY_EXIT_STATE_FAILED 9U

typedef struct
{
  UINT32 Reason;
  UINT32 HceBefore;
  UINT32 HcsBefore;
  UINT32 IsBefore;
  UINT32 IeBefore;
  UINT32 UtrlRsrBefore;
  UINT32 UtmrlRsrBefore;
  UINT32 UtrlDbrBefore;
  UINT32 UtmrlDbrBefore;
  UINT32 HceStopPolls;
  UINT32 HceStartPolls;
  UINT32 StopPolls;
  UINT32 LinkStartupAttempts;
  UINT32 LinkStartupResult; /* UIC ARG2 of the last attempt */
  UINT32 UicArg3;
  UINT32 DevicePresentPolls;
  UINT32 Uecpa; /* diagnostic read after the last link startup */
  UINT32 HceAfter;
  UINT32 HcsAfter;
  UINT32 IsAfter;
  UINT32 IeAfter;
  UINT32 UtrlRsrAfter;
  UINT32 UtmrlRsrAfter;
  UINT32 UtrlDbrAfter;
  UINT32 UtmrlDbrAfter;
  UINT32 Reads;
  UINT32 Writes;
  UINT32 Delays;
  BOOLEAN ListsStopped;
  BOOLEAN ControllerEnabled;
  BOOLEAN LinkStarted;
  BOOLEAN DevicePresent;
  BOOLEAN QuiescentAtExit;
} LIUQIN_UFS_RECOVERY_REPORT;

BOOLEAN LiuqinUfsRecoverInherited (LIUQIN_UFS_READ32 Read32, LIUQIN_UFS_WRITE32 Write32,
                                   LIUQIN_UFS_DELAY Delay, VOID *Context,
                                   LIUQIN_UFS_RECOVERY_REPORT *Report);
CONST char *LiuqinUfsRecoveryReasonName (UINT32 Reason);
#endif
