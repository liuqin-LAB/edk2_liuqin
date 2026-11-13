/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_VBUS_PRODUCTION_ADAPTER_H
#define LIUQIN_VBUS_PRODUCTION_ADAPTER_H

#include <Base.h>
#include <Library/UsbVbusPolicy.h>
#include <SpmiRead.h>
#include <SpmiWrite.h>

#define LIUQIN_VBUS_PRODUCTION_SLAVE 3U
#define LIUQIN_VBUS_PRODUCTION_PERIOD_US 250000U

/* Extra read-only bytes not part of the VbusSnapshot tuple. */
#define LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS 0x27C6U
#define LIUQIN_VBUS_PRODUCTION_LOCK_D1_ADDRESS 0x27D1U
#define LIUQIN_VBUS_PRODUCTION_LOCK_D2_ADDRESS 0x27D2U
#define LIUQIN_VBUS_PRODUCTION_TYPEC_LOCK_D1 0x2BD1U

/* Reviewed register addresses reused from VbusPmicWriter.h values. */
#define LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS 0x2750U
#define LIUQIN_VBUS_PRODUCTION_BST_CFG_ADDRESS 0x2756U
#define LIUQIN_VBUS_PRODUCTION_BST_STS_ADDRESS 0x270DU
#define LIUQIN_VBUS_PRODUCTION_BST_FAULT_ADDRESS 0x270EU
#define LIUQIN_VBUS_PRODUCTION_BST_FAIL_ADDRESS 0x270FU
#define LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS 0x2B44U
#define LIUQIN_VBUS_PRODUCTION_SOFTSTART_POLLS 20U
#define LIUQIN_VBUS_PRODUCTION_SOFTSTART_DELAY_US 10000U
#define LIUQIN_VBUS_PRODUCTION_MAX_ACTION_READS 40U
#define LIUQIN_VBUS_PRODUCTION_MAX_ACTION_WRITES 8U
#define LIUQIN_VBUS_PRODUCTION_MAX_ACTIONS_PER_TICK 8U
#define LIUQIN_VBUS_PRODUCTION_MAX_EBS_ACTIONS 16U

/* Early-boot warmup bounds. After the source role is adopted, this many
 * consecutive observations without a complete debounced Rd/Rd-Ra attach prove
 * that nothing is plugged in; the periodic timer keeps watching for hotplug.
 * The wall-clock limit bounds the whole synchronous burst because one tick can
 * run the reviewed softstart poll. Scheduling only: no config, evidence,
 * register or write-set change. */
#define LIUQIN_VBUS_PRODUCTION_WARMUP_IDLE_TICKS 3U
#define LIUQIN_VBUS_PRODUCTION_WARMUP_LIMIT_US 1500000U

/* Injected transport. Read32/Write32 address complete 32-bit USHORT accesses;
 * the production driver bounds them to the mapped SPMI windows and refuses
 * every write once EBS cleanup has begun. DelayUs must
 * sleep at most the requested bounded time. NowUs must be monotonic
 * microseconds. All callbacks are required. */
typedef struct
{
  VOID *Context;
  UINT64 (*NowUs) (VOID *Context);
  BOOLEAN (*Read32) (VOID *Context, UINTN Address, UINT32 *Value);

  BOOLEAN (*ObserverWrite32) (VOID *Context, UINTN Address, UINT32 Value);
  BOOLEAN (*Write32) (VOID *Context, UINTN Address, UINT32 Value);
  VOID (*DelayUs) (VOID *Context, UINT32 Microseconds);
} LIUQIN_VBUS_PRODUCTION_IO;

/* Diagnostic-only first failure. Values never participate in a permit. */
typedef enum
{
  LiuqinVbusFailureNone,
  LiuqinVbusFailureRoleContext,
  LiuqinVbusFailureRoleRead,
  LiuqinVbusFailureRoleExpected,
  LiuqinVbusFailureRoleBaseline,
  LiuqinVbusFailureReadbackRead,
  LiuqinVbusFailureReadbackMismatch,
  LiuqinVbusFailureGuardStartTime,
  LiuqinVbusFailureGuardRead,
  LiuqinVbusFailureGuardCcChanged,
  LiuqinVbusFailureGuardPower,
  LiuqinVbusFailureGuardLock,
  LiuqinVbusFailureGuardBaseline,
  LiuqinVbusFailureGuardExternal,
  LiuqinVbusFailureGuardTarget,
  LiuqinVbusFailureGuardOwner,
  LiuqinVbusFailureGuardEndTime,
  LiuqinVbusFailureTransport,
  LiuqinVbusFailurePublisher,
  LiuqinVbusFailureActionTimeout,
  LiuqinVbusFailureAction,
  LiuqinVbusFailureRoleOwner,
  LiuqinVbusFailureRoleLock,
  LiuqinVbusFailureActionClock,
  LiuqinVbusFailureDataReissue
} LIUQIN_VBUS_FAILURE_STAGE;

/* DataValid bits identify already-collected tuples; zero is not a read value
 * unless the corresponding bit is set. No diagnostic adds a bus transaction.
 * CommandAttempted means the adapter command callback was reached;
 * CommandSubmitted means the underlying publisher returned TRUE. A FALSE
 * publisher return does not prove that hardware saw no command. */
#define LIUQIN_VBUS_DIAG_SNAPSHOT 0x01U
#define LIUQIN_VBUS_DIAG_EXTRA 0x02U
#define LIUQIN_VBUS_DIAG_POWER 0x04U
#define LIUQIN_VBUS_DIAG_TYPEC 0x08U
#define LIUQIN_VBUS_DIAG_CURRENT 0x10U
#define LIUQIN_VBUS_DIAG_READBACK 0x20U
#define LIUQIN_VBUS_DIAG_LOCK 0x40U
typedef struct
{
  BOOLEAN Valid;
  LIUQIN_VBUS_POLICY_ACTION Action;
  LIUQIN_VBUS_FAILURE_STAGE Stage;
  UINT64 ActionElapsedUs;
  UINT64 GuardElapsedUs;
  UINT64 GuardStartedAt;
  BOOLEAN GuardStarted;
  UINT16 Address;
  UINT8 Expected;
  UINT8 Value;
  UINT8 Current;
  UINT8 Readback;
  UINT8 DataValid;
  BOOLEAN CommandAttempted;
  BOOLEAN CommandPublished;
  BOOLEAN CommandSubmitted;
  UINT32 StagedData;
  BOOLEAN DataReissued;
  BOOLEAN DataReissueAccepted;
  BOOLEAN WriteResultValid;
  LIUQIN_SPMI_WRITE_STATUS WriteStatus;
  UINT32 ChannelStatus;
  BOOLEAN ReadFailed;
  UINT16 ReadAddress;
  LIUQIN_SPMI_READ_STATUS ReadStatus;
  UINT8 Typec[6];
  UINT8 TypecRepeat[6];
  UINT8 Power[5];
  UINT8 PowerRepeat[5];
  UINT8 BstEn;
  UINT8 BstCfg;
  UINT8 Role;
  UINT8 Eng;
  UINT8 Lock[3];
} LIUQIN_VBUS_PRODUCTION_FAILURE;

/* Filled by Initialize. The caller owns the storage. */
typedef struct
{
  LIUQIN_VBUS_POLICY Policy;
  LIUQIN_VBUS_POLICY_CONFIG Config;
  LIUQIN_VBUS_POLICY_EVIDENCE Evidence;
  LIUQIN_VBUS_POLICY_OBSERVATION Observation;

  UINT8 CcRaw[6];
  LIUQIN_SPMI_READ_IO ReadIo;
  LIUQIN_SPMI_WRITE_IO WriteIo;
  CONST LIUQIN_SPMI_READ_CONFIG *ReadConfig;
  CONST LIUQIN_SPMI_WRITE_CONFIG *WriteConfig;
  LIUQIN_VBUS_PRODUCTION_IO Io;

  /* Resolved write ownership (metadata only; never a permission). */
  BOOLEAN OwnerCaptured;
  UINT32 DcdcApid;
  UINT32 DcdcMap;
  UINT32 DcdcOwner;
  UINT32 RoleApid;
  UINT32 RoleMap;
  UINT32 RoleOwner;
  BOOLEAN OwnerValid;
  BOOLEAN OwnerStable;

  /* Boot baseline for non-target bits (manager-conflict detection). */
  BOOLEAN BaselineCaptured;
  UINT8 BaselineBstEn;
  UINT8 BaselineBstCfg;
  UINT8 BaselineEngCfg7;
  UINT8 BaselineRole;

  BOOLEAN Initialized;
  BOOLEAN HostReady;
  BOOLEAN EbsDone;
  /* Explicit Windows/OOBE bring-up retention policy (enabled together with
   * the production ACK). Only this flag may select
   * LiuqinVbusPolicyEbsKeepSourceForWindowsBringup; it is captured once at
   * Initialize and never changes at runtime. It is a reviewed bring-up
   * exception, not an OS-side VBUS ownership claim. */
  BOOLEAN WindowsBringupEnabled;
  /* Synchronous WriteByte context. Observer reads use a separate channel;
   * forward commands are revalidated after the RW FIFO has been staged. */
  BOOLEAN WritePending;
  BOOLEAN WriteCleanup;
  UINT16 WriteAddress;
  UINT8 WriteExpected;
  UINT8 WriteValue;
  BOOLEAN WriteDataStaged;
  UINTN WriteDataAddress;
  UINT32 WriteDataValue;
  UINT32 Ticks;
  UINT32 ObservationFailures;
  UINT32 ActionCalls;
  UINT32 FailedActions;
  UINT32 Writes;
  UINT32 Reads;
  LIUQIN_VBUS_POLICY_ACTION LastAction;
  LIUQIN_VBUS_POLICY_OUTCOME LastOutcome;
  LIUQIN_VBUS_PRODUCTION_FAILURE ActionDiagnostic;
  LIUQIN_VBUS_PRODUCTION_FAILURE FirstFailure;
} LIUQIN_VBUS_PRODUCTION;

/* Compact report used by the driver's fixed display lines. */
typedef struct
{
  LIUQIN_VBUS_POLICY_STATE State;
  LIUQIN_VBUS_POLICY_STOP Stop;
  LIUQIN_VBUS_POLICY_EBS_RATIONALE EbsRationale;
  BOOLEAN RoleAdopted;
  BOOLEAN RoleRestored;
  BOOLEAN RoleRestoreUnproven;
  BOOLEAN SourceOnVerified;
  BOOLEAN SourceOffVerified;
  BOOLEAN ShutdownUnproven;
  BOOLEAN EbsRequested;
  BOOLEAN HandoffAllowed;
  BOOLEAN KeepOnAtEbs;
  /* KeepOnAtEbs was selected by the explicit Windows bring-up mode; no OS
   * VBUS owner is claimed and the source retains the adopted role. */
  BOOLEAN WindowsBringupRetain;
  UINT32 RoleWrites;
  UINT32 EnableAttempts;
  UINT32 EnableVerified;
  UINT32 StopVerified;
  UINT32 FaultEvents;
  UINT32 ExternalEvents;
  UINT32 UnknownBitEvents;
  UINT32 ActionFailures;
  UINT32 Ticks;
  UINT32 ObservationFailures;
  UINT32 Writes;
  UINT32 Reads;

  BOOLEAN ObservationValid;
  BOOLEAN LockValid;
  BOOLEAN OwnerValid;
  BOOLEAN OwnerStable;
  BOOLEAN BaselineValid;
  BOOLEAN HostPathReady;
  UINT8 CcState;
  BOOLEAN VbusDetected;
  UINT8 CcPartner;
  UINT8 CcRaw[6];
  UINT8 ExternalPower;
  UINT8 PmicRole;
  UINT8 PowerPath;
  UINT8 BstEn;
  UINT8 BstCfg;
  UINT8 BstStatus;
  UINT8 BstFault;
  UINT8 BstFail;
  UINT8 AnalogLoop;
  UINT8 LockD1;
  UINT8 LockD2;
  UINT8 TypecLockD1;
  UINT8 EngCfg7;
  LIUQIN_VBUS_PRODUCTION_FAILURE FirstFailure;
} LIUQIN_VBUS_PRODUCTION_REPORT;

CONST CHAR8 *VbusProductionFailureText (LIUQIN_VBUS_FAILURE_STAGE Stage);

/* Requires ProductionAck, the production PCD pair already validated by the
 * caller, and a provider-configuration the caller proved from the runtime DT
 * and the mapped windows. With Ack FALSE the policy stays permanently
 * disabled and Tick/Ebs perform no register access.
 * The liuqin platform selects WindowsBringup = TRUE:
 * TRUE selects the reviewed Windows/OOBE keep-source-across-EBS mode
 * (retain a verified source, do not restore the sink role). FALSE keeps the
 * fail-closed LiuqinVbusPolicyEbsControlledOff default unchanged. The mode is
 * still gated by ProductionAck and by the policy's own verified-source and
 * observation-health conditions at EBS. */
VOID VbusProductionAdapterInitialize (OUT LIUQIN_VBUS_PRODUCTION *Adapter,
                                      IN CONST LIUQIN_VBUS_PRODUCTION_IO *Io,
                                      IN CONST LIUQIN_SPMI_READ_CONFIG *ReadConfig,
                                      IN CONST LIUQIN_SPMI_WRITE_CONFIG *WriteConfig,
                                      IN BOOLEAN ProductionAck, IN BOOLEAN WindowsBringup,
                                      IN UINT64 Now);

/* The host path must be proven before VBUS may be enabled: the USB core/PHY
 * must be configured in host mode (requirement R9). The inherited host probe
 * (DWC3 host-role/tuning readback + SMMU identity path) proves that, so the
 * production policy can start at UsbHostDxe entry. The xHCI driver binding is
 * intentionally not a prerequisite: a cold-plugged UFP must be powered before
 * BDS scans boot options. */
VOID VbusProductionAdapterSetHostReady (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter,
                                        IN BOOLEAN HostReady);

/* One periodic observation -> policy -> at most one normal action (cleanup
 * actions may continue in the same tick). Returns TRUE if a hardware action
 * was attempted, FALSE otherwise. */
BOOLEAN
VbusProductionAdapterTick (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT64 Now);

/* Bounded early-boot warmup: runs up to MaxTicks normal ticks with GapUs
 * between them (using the injected NowUs/DelayUs primitives only) and returns
 * the ticks actually executed. UsbHostDxe calls it at driver entry so an
 * already attached UFP/Rd is powered before BDS scans boot options, instead of
 * waiting for the first periodic tick after the xHCI stack binds.
 *
 * This is scheduling only: Tick keeps every reviewed gate, and config,
 * evidence and the write set are unchanged. It stops early once the
 * synchronous outcome is settled:
 *  - source on verified, a stop latched, or a terminal state;
 *  - the source role was adopted but no debounced Rd attach was seen for
 *    WARMUP_IDLE_TICKS consecutive ticks (nothing is plugged in; the periodic
 *    timer keeps watching);
 *  - a declared external supply blocks the pre-role gate (only a later
 *    observation can clear it);
 *  - MaxTicks or the WARMUP_LIMIT_US wall-clock bound is reached.
 * Call only before VbusProductionAdapterEbs. */
UINT32
VbusProductionAdapterWarmup (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT32 MaxTicks,
                             IN UINT32 GapUs);

/* EBS controlled-off/decision before xHCI halt. No-op when the policy is
 * disabled. Returns TRUE only when OS handoff is allowed (source off verified
 * or nothing was ever enabled and the manager reached a terminal state).
 * In the explicit Windows bring-up mode a verified healthy monitored source is
 * retained without a stop (source stays on, adopted role is not restored) and
 * TRUE is returned; any doubt falls back to the bounded controlled-off path. */
BOOLEAN
VbusProductionAdapterEbs (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT64 Now);

VOID VbusProductionAdapterReport (IN CONST LIUQIN_VBUS_PRODUCTION *Adapter,
                                  OUT LIUQIN_VBUS_PRODUCTION_REPORT *Report);

/* Resolved channel APIDs for the driver's defense-in-depth MMIO publisher.
 * FALSE means no write channel is currently authorized (all writes refused). */
BOOLEAN
VbusProductionAdapterChannels (IN CONST LIUQIN_VBUS_PRODUCTION *Adapter, OUT UINT32 *DcdcApid,
                               OUT UINT32 *RoleApid);

#endif
