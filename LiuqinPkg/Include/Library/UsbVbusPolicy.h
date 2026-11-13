/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_USB_VBUS_POLICY_H
#define LIUQIN_USB_VBUS_POLICY_H

#include <Base.h>

/* Fixed PM8350B v2 facts used by the policy. Changing these requires new
 * device evidence, not a configuration change. */
#define LIUQIN_VBUS_POLICY_PMIC_SLAVE 3U
#define LIUQIN_VBUS_POLICY_TYPEC_ROLE_ADDRESS 0x2B44U
#define LIUQIN_VBUS_POLICY_ROLE_MASK 0x07U
#define LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY 0x02U
#define LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY 0x04U
#define LIUQIN_VBUS_POLICY_LOCK_D1_ADDRESS 0x27D1U
#define LIUQIN_VBUS_POLICY_LOCK_D2_ADDRESS 0x27D2U
#define LIUQIN_VBUS_POLICY_TYPEC_LOCK_D1_ADDRESS 0x2BD1U
#define LIUQIN_VBUS_POLICY_LOCK_STOCK_D1 0x03U
#define LIUQIN_VBUS_POLICY_LOCK_STOCK_D2 0x00U
#define LIUQIN_VBUS_POLICY_LOCK_STOCK_TYPEC_D1 0x03U

#define LIUQIN_VBUS_POLICY_BST_EN_OTG_EN 0x01U
#define LIUQIN_VBUS_POLICY_BST_EN_ALLOWED 0x1BU
#define LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC 0x01U
#define LIUQIN_VBUS_POLICY_BST_CFG_ALLOWED 0x73U
#define LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART 0x08U
#define LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB 0x10U
#define LIUQIN_VBUS_POLICY_BST_STATUS_ALLOWED 0x7FU
#define LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF 0x01U
#define LIUQIN_VBUS_POLICY_BST_FAULT_ALLOWED 0x1FU
#define LIUQIN_VBUS_POLICY_BST_FAIL_ALLOWED 0x1FU

#define LIUQIN_VBUS_POLICY_TYPEC_MISC_FAULT 0x14U
#define LIUQIN_VBUS_POLICY_TYPEC_MISC_WATER 0x80U

#define LIUQIN_VBUS_POLICY_POWER_PATH_INPUT 0x80U

/* ANALOG_LOOP_STATUS bit7 OCP_LOOP. It is an analog-loop status bit; the
 * board has no measured OCP threshold, trip timing or latch semantics. */
#define LIUQIN_VBUS_POLICY_ANALOG_LOOP_OCP 0x80U

#define LIUQIN_VBUS_POLICY_PARTNER_MASK 0x1FU
#define LIUQIN_VBUS_POLICY_PARTNER_RD 0x08U
#define LIUQIN_VBUS_POLICY_PARTNER_RD_RA 0x04U
#define LIUQIN_VBUS_POLICY_PARTNER_RA 0x02U

/* Bounds. These are software safety limits, not PMIC/electrical guarantees.
 * A value of zero for AttachWindowUs/MaximumSourceOnUs means "until EBS". */
#define LIUQIN_VBUS_POLICY_MAX_ACTION_US 10000000U
#define LIUQIN_VBUS_POLICY_MAX_OBSERVATION_US 5000000U
#define LIUQIN_VBUS_POLICY_MIN_DEBOUNCE 2U
#define LIUQIN_VBUS_POLICY_MAX_DEBOUNCE 16U
#define LIUQIN_VBUS_POLICY_MIN_OCP_DEBOUNCE 1U
#define LIUQIN_VBUS_POLICY_MAX_OCP_DEBOUNCE 8U
#define LIUQIN_VBUS_POLICY_MIN_STOP_CHECKS 1U
#define LIUQIN_VBUS_POLICY_MAX_STOP_CHECKS 4U
#define LIUQIN_VBUS_POLICY_MAX_ATTACH_WINDOW_US 600000000U
#define LIUQIN_VBUS_POLICY_MAX_SOURCE_ON_US 3600000000U
#define LIUQIN_VBUS_POLICY_MAX_ACTIONS 4096U

typedef enum
{
  LiuqinVbusPolicyStateDisabled = 0, /* terminal: zero-init or invalid config */
  LiuqinVbusPolicyStatePreflight,    /* gates only, no writes yet */
  LiuqinVbusPolicyStateDetached,     /* safe baseline seen; adopt role */
  LiuqinVbusPolicyStateRoleAdopted,  /* role readback 0x04; waiting attach */
  LiuqinVbusPolicyStateAttachDebounce,
  LiuqinVbusPolicyStateMonitored, /* source enabled and verified on */
  LiuqinVbusPolicyStateStopping,  /* latched stop cleanup plan */
  LiuqinVbusPolicyStateStopped,   /* terminal after clean bounded run */
  LiuqinVbusPolicyStateFaultStop, /* terminal after fault/uncertainty */
  LiuqinVbusPolicyStateTerminal,
  LiuqinVbusPolicyStateShutdownUnproven,
  LiuqinVbusPolicyStateWaitExternalClear /* clean sink role; read-only wait */
} LIUQIN_VBUS_POLICY_STATE;

typedef enum
{
  LiuqinVbusPolicyCcInvalid = 0, /* unavailable/unsupported/read failure */
  LiuqinVbusPolicyCcDetached,
  LiuqinVbusPolicyCcAttachedSink,   /* local sink attached to Rp source */
  LiuqinVbusPolicyCcAttachedSource, /* local source attached to UFP/Rd or Ra */
  LiuqinVbusPolicyCcTransition,
  LiuqinVbusPolicyCcAccessory,
  LiuqinVbusPolicyCcFault,
  LiuqinVbusPolicyCcInconsistent,
  LiuqinVbusPolicyCcUnknown
} LIUQIN_VBUS_POLICY_CC_STATE;

typedef enum
{
  LiuqinVbusPolicyPartnerUnknown = 0,
  LiuqinVbusPolicyPartnerNone = 1,
  LiuqinVbusPolicyPartnerSourceRp = 0x02,
  LiuqinVbusPolicyPartnerSinkRa = LIUQIN_VBUS_POLICY_PARTNER_RA,
  LiuqinVbusPolicyPartnerSinkRdRa = LIUQIN_VBUS_POLICY_PARTNER_RD_RA,
  LiuqinVbusPolicyPartnerSinkRd = LIUQIN_VBUS_POLICY_PARTNER_RD
} LIUQIN_VBUS_POLICY_PARTNER;

typedef enum
{
  LiuqinVbusPolicyExternalUnknown = 0,
  LiuqinVbusPolicyExternalAbsent,
  LiuqinVbusPolicyExternalPresent
} LIUQIN_VBUS_POLICY_EXTERNAL;

typedef struct
{
  BOOLEAN Valid;
  LIUQIN_VBUS_POLICY_CC_STATE State;
  LIUQIN_VBUS_POLICY_PARTNER Partner;
  UINT8 StateMachine;
  UINT8 Orientation;
  BOOLEAN Attached;
  BOOLEAN Debounced;
  BOOLEAN SourceMode;
  BOOLEAN VbusDetected;
  BOOLEAN WaterDetected; /* record only, never a fault */
  UINT8 FaultBits;       /* hard bits actually observed */
  UINT8 UnknownBits;
} LIUQIN_VBUS_POLICY_CC;

/* PMIC read results. These are raw bytes as read by the caller's reviewed
 * read-only path; this module never performs the reads itself. */
typedef struct
{
  BOOLEAN Valid;
  BOOLEAN IdentityValid;   /* 0x0102 model/rev + 0x2704 + 0x2B04 identities */
  BOOLEAN OwnerValid;      /* unique EE0-owned APID for PPID 0x327 */
  BOOLEAN OwnerStable;     /* map/owner re-read unchanged */
  BOOLEAN LockValid;       /* all three lock bytes read completely */
  UINT8 LockD1;            /* 0x27D1 */
  UINT8 LockD2;            /* 0x27D2 */
  UINT8 TypecLockD1;       /* 0x2BD1 */
  BOOLEAN BaselineValid;   /* saved non-enable baseline still matches */
  UINT8 BstEn;             /* 0x2750 */
  UINT8 BstCfg;            /* 0x2756 */
  UINT8 BstStatus;         /* 0x270D */
  UINT8 EngCfg7;           /* 0x27C6 */
  UINT8 PowerPath;         /* 0x270B */
  UINT8 AnalogLoop;        /* 0x270C */
  UINT8 BstFault;          /* 0x270E */
  UINT8 BstFail;           /* 0x270F */
  UINT8 RoleByte;          /* 0x2B44 readback */
  UINT8 UnknownBits;       /* reserved bits observed in defined bytes */
  BOOLEAN ManagerConflict; /* non-target bits changed vs saved baseline */
} LIUQIN_VBUS_POLICY_PMIC;

/* One consistent observation. CapturedAt must come from the same monotonic
 * microsecond clock used for every policy call. */
typedef struct
{
  BOOLEAN Valid;
  UINT64 CapturedAt;
  LIUQIN_VBUS_POLICY_CC Cc;
  LIUQIN_VBUS_POLICY_PMIC Pmic;

  LIUQIN_VBUS_POLICY_EXTERNAL ExternalPower;

  BOOLEAN HostPathReady;
  /* Optional independent port-voltage observation. The production profile
   * has no proved measurement path yet, so this stays FALSE and the policy
   * never claims safe-zero. */
  BOOLEAN PortVoltageValid;
  BOOLEAN PortSafeZero;
} LIUQIN_VBUS_POLICY_OBSERVATION;

/* Reviewed evidence adjudications. Zero-initialized = nothing proved. */
typedef struct
{
  BOOLEAN IdentityVerified;            /* PM8350B v2 identity proven */
  BOOLEAN OwnerVerified;               /* exclusive EE0 write ownership proven */
  BOOLEAN LockPolicyVerified;          /* selected lock mode proven for this boot */
  BOOLEAN RouteVerified;               /* board boost/VBUS route proven */
  BOOLEAN ExternalInterlockVerified;   /* external-supply signal semantics proven */
  BOOLEAN ProtectionReportingVerified; /* fault/fail bits proven to report */
  BOOLEAN IndependentShutdownVerified; /* independent shutdown path proven */
  BOOLEAN OsVbusOwnerVerified;         /* OS/ACPI side owns VBUS after EBS */
  /* Adjudicated explicit Windows/OOBE bring-up retention: the caller proved
   * that this image uses the Windows bring-up policy, all production gates
   * hold, and retaining a verified source is the reviewed boot policy. This does NOT prove an
   * OS-side VBUS owner; keep-on under this flag is a bounded bring-up exception and must never be
   * reported as an OS handoff. */
  BOOLEAN WindowsBringupRetainVerified;
} LIUQIN_VBUS_POLICY_EVIDENCE;

typedef enum
{
  LiuqinVbusPolicyLockStrictZero = 0,       /* historical default: 00/00/00 gate */
  LiuqinVbusPolicyLockStockRuntimeOwned = 1 /* exact inherited 03/00/03 */
} LIUQIN_VBUS_POLICY_LOCK_MODE;

typedef enum
{
  LiuqinVbusPolicyEbsControlledOff = 0, /* fail-closed default */
  LiuqinVbusPolicyEbsKeepOnIfOsOwned = 1,
  /* Windows/OOBE bring-up policy: there is no
   * Windows-side PMIC/UCSI/VBUS owner yet, so a verified self-powered dock
   * source is retained across ExitBootServices and the inherited sink role is
   * NOT restored. This is a deliberate, reviewed bring-up exception, not an
   * OS handoff contract: it is reachable only with
   * EVIDENCE.WindowsBringupRetainVerified (adjudicated by the caller from the
   * fixed boot policy) plus a healthy verified source. Any doubt at EBS
   * refuses keep-on and falls back to the controlled-off path. */
  LiuqinVbusPolicyEbsKeepSourceForWindowsBringup = 2
} LIUQIN_VBUS_POLICY_EBS_MODE;

typedef struct
{
  BOOLEAN Enabled;
  BOOLEAN AllowReattach;
  UINT32 ActionTimeoutUs;
  UINT32 MaximumObservationAgeUs;
  UINT32 AttachDebounceSamples;
  UINT32 AttachWindowUs;    /* 0 = until EBS */
  UINT32 MaximumSourceOnUs; /* 0 = continuous */
  UINT32 OcpDebounceSamples;
  UINT32 StopReadbackChecks;
  LIUQIN_VBUS_POLICY_LOCK_MODE LockMode;
  LIUQIN_VBUS_POLICY_EBS_MODE EbsMode;
  BOOLEAN TrustPowerPathBit7Candidate;
  BOOLEAN AllowExternalReattach; /* explicit opt-in; default FALSE */
} LIUQIN_VBUS_POLICY_CONFIG;

typedef enum
{
  LiuqinVbusPolicyActionNone = 0,
  LiuqinVbusPolicyActionWriteRole,     /* 0x2B44 mask 0x07 0x02 -> 0x04 */
  LiuqinVbusPolicyActionEnableSource,  /* ENG -> BST_EN -> softstart -> ENG clear */
  LiuqinVbusPolicyActionDisableSource, /* clear our source-enable masks + readback */
  LiuqinVbusPolicyActionVerifySourceOff,
  LiuqinVbusPolicyActionTriggerIndependentShutdown,
  LiuqinVbusPolicyActionRestoreRole /* 0x2B44 mask 0x07 -> 0x02 */
} LIUQIN_VBUS_POLICY_ACTION;

typedef enum
{
  LiuqinVbusPolicyActionSucceeded,
  LiuqinVbusPolicyActionFailed,
  LiuqinVbusPolicyActionIndeterminate
} LIUQIN_VBUS_POLICY_OUTCOME;

/* Action result contract: Success requires Facts to be EXACTLY
 * LiuqinVbusPolicyRequiredFacts(Action). DisableSource requires zero facts:
 * the command acknowledgement alone never proves source-off; the separate
 * VerifySourceOff action must return FACT_SOURCE_OFF. */
#define LIUQIN_VBUS_POLICY_FACT_ROLE_APPLIED 0x001U
#define LIUQIN_VBUS_POLICY_FACT_SOURCE_ON 0x002U
#define LIUQIN_VBUS_POLICY_FACT_SOURCE_OFF 0x004U
#define LIUQIN_VBUS_POLICY_FACT_ROLE_RESTORED 0x008U
#define LIUQIN_VBUS_POLICY_FACT_INDEPENDENT 0x010U

typedef struct
{
  UINT32 Sequence;
  LIUQIN_VBUS_POLICY_ACTION Action;
  UINT8 RegisterValue;
  UINT8 RegisterMask;
  UINT32 TimeoutUs;
} LIUQIN_VBUS_POLICY_TICKET;

typedef struct
{
  LIUQIN_VBUS_POLICY_OUTCOME Outcome;
  UINT32 Facts;
} LIUQIN_VBUS_POLICY_ACTION_RESULT;

typedef enum
{
  LiuqinVbusPolicyStopNone = 0,
  LiuqinVbusPolicyStopDisabled,
  LiuqinVbusPolicyStopInvalidConfig,
  LiuqinVbusPolicyStopEvidence,
  LiuqinVbusPolicyStopClock,
  LiuqinVbusPolicyStopObservationInvalid,
  LiuqinVbusPolicyStopObservationStale,
  LiuqinVbusPolicyStopIdentity,
  LiuqinVbusPolicyStopOwner,
  LiuqinVbusPolicyStopLock,
  LiuqinVbusPolicyStopManager,
  LiuqinVbusPolicyStopInheritedSource,
  LiuqinVbusPolicyStopFault,
  LiuqinVbusPolicyStopOverCurrent,
  LiuqinVbusPolicyStopExternalPower,
  LiuqinVbusPolicyStopPartnerUnsupported,
  LiuqinVbusPolicyStopRoleLost,
  LiuqinVbusPolicyStopSourceLost,
  LiuqinVbusPolicyStopRoleWriteFailed,
  LiuqinVbusPolicyStopEnableFailed,
  LiuqinVbusPolicyStopActionFailure,
  LiuqinVbusPolicyStopActionTimeout,
  LiuqinVbusPolicyStopAttachWindow,
  LiuqinVbusPolicyStopSourceWindow,
  LiuqinVbusPolicyStopDetach,
  LiuqinVbusPolicyStopExitBootServices,
  LiuqinVbusPolicyStopShutdownUnproven
} LIUQIN_VBUS_POLICY_STOP;

typedef enum
{
  LiuqinVbusPolicyEbsControlledOffDefault = 0,
  LiuqinVbusPolicyEbsControlledOffExternalPower,
  LiuqinVbusPolicyEbsControlledOffFault,
  LiuqinVbusPolicyEbsControlledOffNotEnabled,
  LiuqinVbusPolicyEbsControlledOffShutdownUnproven,
  LiuqinVbusPolicyEbsControlledOffOsOwnerUnproven,
  LiuqinVbusPolicyEbsControlledOffSourceOnUnverified,
  LiuqinVbusPolicyEbsControlledOffManagerChanged,
  LiuqinVbusPolicyEbsKeepOnOsOwnsVbus,
  /* Windows/OOBE bring-up mode: retain a verified source without an OS owner.
   * Distinct from LiuqinVbusPolicyEbsKeepOnOsOwnsVbus so a report can never
   * claim OS ownership for the bring-up exception. */
  LiuqinVbusPolicyEbsKeepOnWindowsBringup,
  /* Windows/OOBE bring-up mode requested but the profile-contract evidence is
   * absent/not adjudicated: fail closed with a controlled off. */
  LiuqinVbusPolicyEbsControlledOffWindowsBringupUnproven,
  /* Windows/OOBE bring-up mode with the contract present, but the source
   * state was unverified/not monitored/latched at EBS: refuse keep-on. */
  LiuqinVbusPolicyEbsControlledOffWindowsBringupUnverified,
  /* Report-only preview before the EBS request: the explicit Windows bring-up
   * mode is armed with a possible source on, but keep-on is only decided when
   * ExitBootServices actually requests the decision. Never keep-on. */
  LiuqinVbusPolicyEbsWindowsBringupPending
} LIUQIN_VBUS_POLICY_EBS_RATIONALE;

typedef struct
{
  BOOLEAN KeepOn;
  BOOLEAN RequiresStop;
  BOOLEAN HandoffAllowed;
  /* Windows/OOBE bring-up keep-on actually selected (distinct from KeepOn,
   * which can also mean an OS-owned keep-on). */
  BOOLEAN WindowsBringupRetain;
  /* Windows/OOBE bring-up keep-on requested by the mode but refused at EBS;
   * the controlled-off rationale names the first cause. */
  BOOLEAN WindowsBringupRefused;
  LIUQIN_VBUS_POLICY_EBS_RATIONALE Rationale;
} LIUQIN_VBUS_POLICY_EBS_DECISION;

typedef struct
{
  BOOLEAN RoleWriteAllowed;
  BOOLEAN SourceEnableAllowed;
  BOOLEAN KeepOnAtEbsAllowed;
  BOOLEAN HandoffAllowed;
  BOOLEAN StopRequired;
  LIUQIN_VBUS_POLICY_STOP BlockingReason;
} LIUQIN_VBUS_POLICY_PERMITS;

typedef struct
{
  UINT32 RoleWrites;
  UINT32 EnableAttempts;
  UINT32 EnableVerified;
  UINT32 StopAttempts;
  UINT32 StopVerified;
  UINT32 Cycles; /* clean detach rearms / external role sessions */
  UINT32 ReattachArmed;
  UINT32 FramesAccepted;
  UINT32 FramesRejected;
  UINT32 FaultEvents;
  UINT32 ExternalEvents;
  UINT32 DetachEvents;
  UINT32 LockMismatchEvents;
  UINT32 OwnerMismatchEvents;
  UINT32 IdentityMismatchEvents;
  UINT32 UnknownBitEvents;
  UINT32 OcpEvents;
  UINT32 WaterRecords;
  UINT32 ActionFailures;
  UINT32 ActionTimeouts;
  UINT32 LastFaultBits; /* 0x270E raw value of the last observed frame */
  UINT32 LastFailBits;  /* 0x270F raw value of the last observed frame */
  UINT32 LastOcpBits;   /* 0x270C raw OCP indicator */
  UINT32 LastPowerPath; /* 0x270B raw value */
  UINT32 MaximumFrameAgeUs;
  UINT32 LastFrameAgeUs;
} LIUQIN_VBUS_POLICY_COUNTERS;

typedef struct
{
  LIUQIN_VBUS_POLICY_STATE State;
  LIUQIN_VBUS_POLICY_STOP Stop;
  LIUQIN_VBUS_POLICY_COUNTERS Counters;
  BOOLEAN RoleAdopted;
  BOOLEAN RoleRestored;
  BOOLEAN RoleRestoreUnproven;
  BOOLEAN SourceMayBeOn;
  BOOLEAN SourceOnVerified;
  BOOLEAN SourceOffVerified;
  BOOLEAN ShutdownUnproven;
  BOOLEAN EbsRequested;
  BOOLEAN HandoffAllowed;
  BOOLEAN KeepOnAtEbs;
  /* KeepOnAtEbs was selected by the explicit Windows bring-up mode (no OS
   * VBUS owner is claimed). */
  BOOLEAN WindowsBringupRetain;
  LIUQIN_VBUS_POLICY_EBS_RATIONALE EbsRationale;
} LIUQIN_VBUS_POLICY_REPORT;

typedef struct
{
  LIUQIN_VBUS_POLICY_CONFIG Config;
  LIUQIN_VBUS_POLICY_STATE State;
  LIUQIN_VBUS_POLICY_STOP Stop;
  UINT64 StartedAt;
  UINT64 LastTick;
  UINT64 RoleAdoptedAt;
  UINT64 SourceOnAt;
  UINT64 ShutdownStartedAt;
  UINT64 ActionStartedAt;
  /* The caller may call Next/Begin/Complete several times for the same
   * observation (e.g. a retried plan revalidation); counters and debounce
   * advance at most once per distinct CapturedAt value. */
  UINT64 LastObservationAt;
  UINT64 LastDebounceAt;
  BOOLEAN HasLastObservation;
  BOOLEAN LastObservedFault;
  BOOLEAN LastObservedExternal;
  BOOLEAN LastObservedManagerChanged;
  BOOLEAN LastObservedSourceOn; /* verified this boot */
  UINT32 NextSequence;
  UINT32 ActionsStarted;
  UINT32 DebounceCount;
  UINT8 DebounceOrientation;
  UINT32 OcpStreak;
  UINT32 EnableAttemptsThisCycle;
  UINT32 RoleAttemptsThisSession; /* lifetime count is Counters.RoleWrites */
  UINT32 ExternalClearCount;
  UINT64 ExternalClearAfter;
  UINT64 LastExternalClearAt;
  UINT64 RecoveryPreflightAfter;
  UINT64 SourceOffVerifiedAt;
  UINT64 RoleRestoredAt;
  BOOLEAN RequireFreshRoleObservation;
  BOOLEAN ExternalStopConfirmed;
  BOOLEAN ExternalReattachBlocked;
  BOOLEAN Planned;
  BOOLEAN InFlight;
  BOOLEAN StopLatched;
  BOOLEAN EbsRequested;
  BOOLEAN EbsKeepOnLatched;
  /* Snapshot of Evidence.WindowsBringupRetainVerified seen by the EBS request.
   * Report-only: lets the fixed report name the real refusal cause without
   * claiming evidence the policy cannot re-derive. */
  BOOLEAN WindowsBringupContractSeen;
  BOOLEAN ClockFailed;
  BOOLEAN ActionTimedOut;
  BOOLEAN ShutdownDeadlineMissed;
  BOOLEAN RoleMayHaveChanged;
  BOOLEAN RoleAdopted;
  BOOLEAN RoleRestored;
  BOOLEAN RoleRestoreAttempted;
  BOOLEAN RoleRestoreUnproven;
  BOOLEAN SourceMayBeOn;
  BOOLEAN SourceOnVerified;
  BOOLEAN SourceOffVerified;
  BOOLEAN DisableAttempted;
  BOOLEAN DisableFailed;
  BOOLEAN IndependentShutdownAttempted;
  BOOLEAN RestoreAttempted;
  BOOLEAN ShutdownUnproven;
  BOOLEAN ReattachArmed;
  UINT32 OffChecks;
  UINT32 MaximumFrameAgeUs;
  UINT32 LastFrameAgeUs;
  LIUQIN_VBUS_POLICY_TICKET Ticket;
  LIUQIN_VBUS_POLICY_COUNTERS Counters;
} LIUQIN_VBUS_POLICY;

VOID LiuqinVbusPolicyInitialize (LIUQIN_VBUS_POLICY *Policy,
                                 CONST LIUQIN_VBUS_POLICY_CONFIG *Config, UINT64 Now);

/* Plans the next action, if any. Never performs I/O. Returns FALSE with
 * *Ticket.Action == None when the caller should just observe again or the
 * policy reached a terminal state. */
BOOLEAN
LiuqinVbusPolicyNext (LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                      CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation, UINT64 Now,
                      LIUQIN_VBUS_POLICY_TICKET *Ticket);

/* Revalidates the SAME ticket immediately before the adapter call and
 * journals possible effects first. Returns FALSE if the caller must not call
 * the adapter (changed input, stop latched, expired deadline). */
BOOLEAN
LiuqinVbusPolicyBegin (LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_TICKET *Ticket,
                       CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                       CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation, UINT64 Now);

/* Consumes the adapter result for the in-flight ticket. A failed or
 * indeterminate normal action latches a stop; a failed cleanup action keeps
 * the bounded cleanup plan going and can end in ShutdownUnproven. */
BOOLEAN
LiuqinVbusPolicyComplete (LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_TICKET *Ticket,
                          CONST LIUQIN_VBUS_POLICY_ACTION_RESULT *Result, UINT64 Now);

/* Latches an irreversible stop for the current cycle. The first reason wins;
 * repeated calls are idempotent and never re-enable. ExitBootServices is a
 * stop reason and also sets EbsRequested. */
VOID LiuqinVbusPolicyRequestStop (LIUQIN_VBUS_POLICY *Policy, LIUQIN_VBUS_POLICY_STOP Reason,
                                  UINT64 Now);

/* Requests the EBS handoff decision. With the fail-closed default the policy
 * latches an ExitBootServices stop and the caller drives the bounded cleanup
 * loop (Next/Begin/Complete) before handoff. With EbsMode keep-on + reviewed
 * OS-owner evidence + a healthy monitored source, the policy freezes without
 * stopping (EbsKeepOnLatched) and reports KeepOn through the EBS decision.
 * With EbsMode WindowsBringup the policy freezes without stopping only when
 * EVIDENCE.WindowsBringupRetainVerified AND a verified monitored source with
 * no fault/external/manager event exists; otherwise it takes the ordinary
 * controlled-off stop path (never a silent keep-on). */
VOID LiuqinVbusPolicyRequestExitBootServices (IN OUT LIUQIN_VBUS_POLICY *Policy,
                                              IN CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                                              IN UINT64 Now);

/* Pure gate snapshot for the current policy state and the last inputs. Does
 * not change state and does not perform I/O. */
VOID LiuqinVbusPolicyGetPermits (CONST LIUQIN_VBUS_POLICY *Policy,
                                 CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                                 CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation,
                                 LIUQIN_VBUS_POLICY_PERMITS *Permits);

/* EBS/OS ownership decision. Never "keep on" unless the config asks for it,
 * a reviewed OS VBUS owner is declared, the source is verified on, the last
 * observation shows no fault/external supply, and the manager conflict flags
 * are clear. The Windows bring-up mode is a separate, explicit exception:
 * it requires EVIDENCE.WindowsBringupRetainVerified, never claims an OS
 * owner, reports LiuqinVbusPolicyEbsKeepOnWindowsBringup, and refuses back to
 * controlled-off on any doubt. Controlled-off remains available until
 * source-off is verified. */
VOID LiuqinVbusPolicyGetEbsDecision (CONST LIUQIN_VBUS_POLICY *Policy,
                                     CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                                     LIUQIN_VBUS_POLICY_EBS_DECISION *Decision);

VOID LiuqinVbusPolicyGetReport (CONST LIUQIN_VBUS_POLICY *Policy,
                                LIUQIN_VBUS_POLICY_REPORT *Report);

/* True for terminal states (no further normal or cleanup action). */
BOOLEAN
LiuqinVbusPolicyIsTerminal (CONST LIUQIN_VBUS_POLICY *Policy);

/* True when a latched stop still has bounded cleanup work to plan. */
BOOLEAN
LiuqinVbusPolicyCleanupPending (CONST LIUQIN_VBUS_POLICY *Policy);

CONST CHAR8 *LiuqinVbusPolicyStateText (IN LIUQIN_VBUS_POLICY_STATE State);

CONST CHAR8 *LiuqinVbusPolicyStopText (IN LIUQIN_VBUS_POLICY_STOP Stop);

CONST CHAR8 *LiuqinVbusPolicyActionText (IN LIUQIN_VBUS_POLICY_ACTION Action);

CONST CHAR8 *LiuqinVbusPolicyEbsRationaleText (IN LIUQIN_VBUS_POLICY_EBS_RATIONALE Rationale);

UINT32
LiuqinVbusPolicyRequiredFacts (IN LIUQIN_VBUS_POLICY_ACTION Action);

/* Exact, decoded source-partner test shared by policy and command guards.
 * The caller must also require AttachedSource and all ordinary CC gates. */
BOOLEAN
LiuqinVbusPolicyPartnerHasRd (IN LIUQIN_VBUS_POLICY_PARTNER Partner);

#endif
