/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/UsbVbusPolicy.h>

/* Pure production VBUS/Type-C decision core. No MMIO, SPMI, boot service,
 * timer or delay call exists in this translation unit. Evidence references
 * and the fail-closed contract are documented in UsbVbusPolicy.h. */

#define LIUQIN_VBUS_POLICY_CLEANUP_RESERVE 8U

STATIC
VOID ClearBytes (VOID *Buffer, UINTN Size)
{
  UINTN Index;
  volatile UINT8 *Bytes;

  Bytes = (volatile UINT8 *)Buffer;
  for (Index = 0U; Index < Size; ++Index)
  {
    Bytes[Index] = 0U;
  }
}

STATIC
BOOLEAN
PolicyTerminal (CONST LIUQIN_VBUS_POLICY *Policy)
{
  switch (Policy->State)
  {
  case LiuqinVbusPolicyStateDisabled:
  case LiuqinVbusPolicyStateStopped:
  case LiuqinVbusPolicyStateFaultStop:
  case LiuqinVbusPolicyStateTerminal:
  case LiuqinVbusPolicyStateShutdownUnproven:
    return TRUE;
  default:
    return FALSE;
  }
}

BOOLEAN
LiuqinVbusPolicyPartnerHasRd (IN LIUQIN_VBUS_POLICY_PARTNER Partner)
{
  return (Partner == LiuqinVbusPolicyPartnerSinkRd) || (Partner == LiuqinVbusPolicyPartnerSinkRdRa);
}

STATIC
BOOLEAN
StopIsFault (IN LIUQIN_VBUS_POLICY_STOP Stop)
{
  switch (Stop)
  {
  case LiuqinVbusPolicyStopClock:
  case LiuqinVbusPolicyStopObservationInvalid:
  case LiuqinVbusPolicyStopObservationStale:
  case LiuqinVbusPolicyStopIdentity:
  case LiuqinVbusPolicyStopOwner:
  case LiuqinVbusPolicyStopLock:
  case LiuqinVbusPolicyStopManager:
  case LiuqinVbusPolicyStopInheritedSource:
  case LiuqinVbusPolicyStopFault:
  case LiuqinVbusPolicyStopOverCurrent:
  case LiuqinVbusPolicyStopRoleLost:
  case LiuqinVbusPolicyStopSourceLost:
  case LiuqinVbusPolicyStopRoleWriteFailed:
  case LiuqinVbusPolicyStopEnableFailed:
  case LiuqinVbusPolicyStopActionFailure:
  case LiuqinVbusPolicyStopActionTimeout:
  case LiuqinVbusPolicyStopShutdownUnproven:
    return TRUE;
  default:
    return FALSE;
  }
}

STATIC
BOOLEAN
EvidenceReady (CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence)
{
  if (Evidence == NULL)
  {
    return FALSE;
  }

  /* Independent shutdown is not required to enable: if the primary disable
   * and readbacks cannot prove source-off, the policy keeps the failure
   * visible (ShutdownUnproven) and forbids OS handoff. OS VBUS ownership is
   * only consulted by the EBS keep-on decision. */
  return Evidence->IdentityVerified && Evidence->OwnerVerified && Evidence->LockPolicyVerified &&
         Evidence->RouteVerified && Evidence->ExternalInterlockVerified &&
         Evidence->ProtectionReportingVerified;
}

STATIC
BOOLEAN
LockAccepted (CONST LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_PMIC *Pmic)
{
  if (!Pmic->LockValid)
  {
    return FALSE;
  }

  if (Policy->Config.LockMode == LiuqinVbusPolicyLockStrictZero)
  {
    return (Pmic->LockD1 == 0U) && (Pmic->LockD2 == 0U) && (Pmic->TypecLockD1 == 0U);
  }

  return (Pmic->LockD1 == LIUQIN_VBUS_POLICY_LOCK_STOCK_D1) &&
         (Pmic->LockD2 == LIUQIN_VBUS_POLICY_LOCK_STOCK_D2) &&
         (Pmic->TypecLockD1 == LIUQIN_VBUS_POLICY_LOCK_STOCK_TYPEC_D1);
}

STATIC
BOOLEAN
ConfigValid (CONST LIUQIN_VBUS_POLICY_CONFIG *Config)
{
  if (!Config->Enabled)
  {
    return FALSE;
  }

  if ((Config->ActionTimeoutUs == 0U) ||
      (Config->ActionTimeoutUs > LIUQIN_VBUS_POLICY_MAX_ACTION_US) ||
      (Config->MaximumObservationAgeUs == 0U) ||
      (Config->MaximumObservationAgeUs > LIUQIN_VBUS_POLICY_MAX_OBSERVATION_US) ||
      (Config->AttachDebounceSamples < LIUQIN_VBUS_POLICY_MIN_DEBOUNCE) ||
      (Config->AttachDebounceSamples > LIUQIN_VBUS_POLICY_MAX_DEBOUNCE) ||
      (Config->OcpDebounceSamples < LIUQIN_VBUS_POLICY_MIN_OCP_DEBOUNCE) ||
      (Config->OcpDebounceSamples > LIUQIN_VBUS_POLICY_MAX_OCP_DEBOUNCE) ||
      (Config->StopReadbackChecks < LIUQIN_VBUS_POLICY_MIN_STOP_CHECKS) ||
      (Config->StopReadbackChecks > LIUQIN_VBUS_POLICY_MAX_STOP_CHECKS) ||
      (Config->AttachWindowUs > LIUQIN_VBUS_POLICY_MAX_ATTACH_WINDOW_US) ||
      (Config->MaximumSourceOnUs > LIUQIN_VBUS_POLICY_MAX_SOURCE_ON_US) ||
      ((UINT32)Config->LockMode > (UINT32)LiuqinVbusPolicyLockStockRuntimeOwned) ||
      ((UINT32)Config->EbsMode > (UINT32)LiuqinVbusPolicyEbsKeepSourceForWindowsBringup))
  {
    return FALSE;
  }

  return TRUE;
}

STATIC
BOOLEAN
ClockTick (IN OUT LIUQIN_VBUS_POLICY *Policy, UINT64 Now)
{
  if (Now < Policy->LastTick)
  {
    Policy->ClockFailed = TRUE;
    LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopClock, Policy->LastTick);
    return FALSE;
  }

  Policy->LastTick = Now;
  return TRUE;
}

/* Read-only pre-write gate revalidation for Begin. Deliberately does not
 * advance debounce, counters or state beyond nothing, so a revalidation
 * cannot create authority the last Next did not have. */
STATIC
BOOLEAN
GateAllows (CONST LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
            CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation, IN LIUQIN_VBUS_POLICY_ACTION Action)
{
  if (!EvidenceReady (Evidence) || (Observation == NULL) || !Observation->Valid)
  {
    return FALSE;
  }

  /* The command boundary rejects a sensed VBUS before either forward write.
   * Apply that same prerequisite before consuming the one role/enable attempt:
   * detached CC alone does not imply that the VBUS detector has cleared.
   * Cleanup and monitoring of our already-enabled source do not use this gate. */
  if ((Observation->ExternalPower != LiuqinVbusPolicyExternalAbsent) ||
      Observation->Cc.VbusDetected)
  {
    return FALSE;
  }

  if ((Policy->Config.TrustPowerPathBit7Candidate || Policy->RequireFreshRoleObservation) &&
      ((Observation->Pmic.PowerPath & LIUQIN_VBUS_POLICY_POWER_PATH_INPUT) != 0U))
  {
    return FALSE;
  }

  if (!Observation->Pmic.IdentityValid || !Observation->Pmic.OwnerValid ||
      !Observation->Pmic.OwnerStable || !Observation->Pmic.BaselineValid ||
      Observation->Pmic.ManagerConflict)
  {
    return FALSE;
  }

  if (!LockAccepted (Policy, &Observation->Pmic))
  {
    return FALSE;
  }

  if ((Observation->Pmic.BstFault != 0U) || (Observation->Pmic.BstFail != 0U) ||
      ((Observation->Pmic.AnalogLoop & LIUQIN_VBUS_POLICY_ANALOG_LOOP_OCP) != 0U))
  {
    return FALSE;
  }

  if ((Observation->Pmic.UnknownBits != 0U) || (Observation->Cc.UnknownBits != 0U) ||
      !Observation->Cc.Valid)
  {
    return FALSE;
  }

  if (((Observation->Pmic.BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) != 0U) ||
      ((Observation->Pmic.BstCfg & LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC) != 0U) ||
      ((Observation->Pmic.BstStatus &
        (LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART | LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB)) != 0U) ||
      ((Observation->Pmic.EngCfg7 & LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF) != 0U))
  {
    return FALSE;
  }

  switch (Action)
  {
  case LiuqinVbusPolicyActionWriteRole:
    return (Policy->State == LiuqinVbusPolicyStatePreflight) &&
           (Policy->RoleAttemptsThisSession == 0U) &&
           (!Policy->RequireFreshRoleObservation ||
            (Observation->CapturedAt > Policy->RecoveryPreflightAfter)) &&
           (Observation->Cc.State == LiuqinVbusPolicyCcDetached) &&
           ((Observation->Pmic.RoleByte & LIUQIN_VBUS_POLICY_ROLE_MASK) ==
            LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY);
  case LiuqinVbusPolicyActionEnableSource:
    return (Policy->State == LiuqinVbusPolicyStateAttachDebounce) && Observation->HostPathReady &&
           (Policy->DebounceCount >= Policy->Config.AttachDebounceSamples) &&
           (Policy->EnableAttemptsThisCycle == 0U) &&
           (Observation->Cc.State == LiuqinVbusPolicyCcAttachedSource) &&
           LiuqinVbusPolicyPartnerHasRd (Observation->Cc.Partner) && Observation->Cc.Attached &&
           Observation->Cc.Debounced && Observation->Cc.SourceMode &&
           ((Observation->Pmic.RoleByte & LIUQIN_VBUS_POLICY_ROLE_MASK) ==
            LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY);
  default:
    break;
  }

  return FALSE;
}

STATIC
BOOLEAN
ShouldRearm (CONST LIUQIN_VBUS_POLICY *Policy)
{
  return (Policy->Stop == LiuqinVbusPolicyStopDetach) && Policy->Config.AllowReattach &&
         Policy->SourceOnVerified && Policy->SourceOffVerified && !Policy->EbsRequested;
}

STATIC
LIUQIN_VBUS_POLICY_ACTION
ExpectedCleanup (CONST LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence)
{
  if (Policy->SourceMayBeOn && !Policy->DisableAttempted)
  {
    return LiuqinVbusPolicyActionDisableSource;
  }

  if (Policy->SourceMayBeOn && !Policy->SourceOffVerified)
  {
    UINT32 RequiredChecks;

    RequiredChecks = Policy->Config.StopReadbackChecks;
    if (Policy->IndependentShutdownAttempted)
    {
      /* One additional readback is allowed to confirm the independent path;
       * this is a verification, not a retry of the primary PMIC write. */
      ++RequiredChecks;
    }

    if (Policy->OffChecks < RequiredChecks)
    {
      return LiuqinVbusPolicyActionVerifySourceOff;
    }

    if ((Evidence != NULL) && Evidence->IndependentShutdownVerified &&
        !Policy->IndependentShutdownAttempted)
    {
      return LiuqinVbusPolicyActionTriggerIndependentShutdown;
    }

    return LiuqinVbusPolicyActionNone;
  }

  /* RoleAdopted tracks that the Type-C role byte currently belongs to this
   * manager (the journal flag RoleMayHaveChanged is cleared when a re-arm
   * keeps the adopted role). Both must trigger the inherited-role restore. */
  if ((Policy->RoleMayHaveChanged || Policy->RoleAdopted) && !Policy->RoleRestoreAttempted &&
      !ShouldRearm (Policy))
  {
    return LiuqinVbusPolicyActionRestoreRole;
  }

  return LiuqinVbusPolicyActionNone;
}

STATIC
VOID EnterTerminal (IN OUT LIUQIN_VBUS_POLICY *Policy, IN LIUQIN_VBUS_POLICY_STATE State)
{
  Policy->Planned = FALSE;
  Policy->InFlight = FALSE;
  Policy->State = State;
}

STATIC
VOID ResetCycle (IN OUT LIUQIN_VBUS_POLICY *Policy)
{
  Policy->StopLatched = FALSE;
  Policy->Stop = LiuqinVbusPolicyStopNone;
  Policy->SourceMayBeOn = FALSE;
  Policy->SourceOnVerified = FALSE;
  Policy->SourceOffVerified = FALSE;
  Policy->DisableAttempted = FALSE;
  Policy->DisableFailed = FALSE;
  Policy->IndependentShutdownAttempted = FALSE;
  Policy->OffChecks = 0U;
  Policy->RoleMayHaveChanged = FALSE;
  Policy->DebounceCount = 0U;
  Policy->DebounceOrientation = 0U;
  Policy->EnableAttemptsThisCycle = 0U;
  Policy->OcpStreak = 0U;
  Policy->ShutdownUnproven = FALSE;
  Policy->ShutdownDeadlineMissed = FALSE;
  Policy->ActionTimedOut = FALSE;
  Policy->EbsRequested = FALSE;
  Policy->RoleAdoptedAt = Policy->LastTick;
  Policy->SourceOnAt = 0U;
  ++Policy->Counters.Cycles;
  ++Policy->Counters.ReattachArmed;
  Policy->State = LiuqinVbusPolicyStateDetached;
}

STATIC
BOOLEAN
CanWaitExternalClear (CONST LIUQIN_VBUS_POLICY *Policy)
{
  return (Policy->Stop == LiuqinVbusPolicyStopExternalPower) && Policy->Config.AllowReattach &&
         Policy->Config.AllowExternalReattach && Policy->ExternalStopConfirmed &&
         !Policy->ExternalReattachBlocked && Policy->RoleRestored && !Policy->RoleAdopted &&
         !Policy->RoleMayHaveChanged && (!Policy->SourceMayBeOn || Policy->SourceOffVerified) &&
         !Policy->DisableFailed && !Policy->RoleRestoreUnproven && !Policy->ShutdownUnproven &&
         !Policy->ShutdownDeadlineMissed && !Policy->ActionTimedOut && !Policy->ClockFailed &&
         !Policy->LastObservedFault && !Policy->LastObservedManagerChanged &&
         (Policy->Counters.ActionFailures == 0U) && (Policy->Counters.ActionTimeouts == 0U) &&
         (Policy->Counters.FaultEvents == 0U) && (Policy->Counters.UnknownBitEvents == 0U) &&
         !Policy->EbsRequested;
}

STATIC
VOID EnterExternalClearWait (IN OUT LIUQIN_VBUS_POLICY *Policy)
{
  /* Call only after completed off/restore proof, including a recovered
   * Preflight whose role action has not begun. No cleanup is repeated. */
  Policy->StopLatched = TRUE;
  Policy->Stop = LiuqinVbusPolicyStopExternalPower;
  Policy->SourceMayBeOn = FALSE;
  Policy->SourceOnVerified = FALSE;
  Policy->ExternalClearCount = 0U;
  Policy->ExternalClearAfter = Policy->LastTick;
  Policy->LastExternalClearAt = Policy->LastTick;
  Policy->Planned = FALSE;
  Policy->State = LiuqinVbusPolicyStateWaitExternalClear;
}

STATIC
BOOLEAN
FinalizeStop (IN OUT LIUQIN_VBUS_POLICY *Policy)
{
  if (Policy->SourceMayBeOn && !Policy->SourceOffVerified)
  {
    Policy->ShutdownUnproven = TRUE;
  }

  if (Policy->ShutdownUnproven || (Policy->Stop == LiuqinVbusPolicyStopShutdownUnproven))
  {
    EnterTerminal (Policy, LiuqinVbusPolicyStateShutdownUnproven);
    return FALSE;
  }

  if (ShouldRearm (Policy))
  {
    ResetCycle (Policy);
    return FALSE;
  }

  if (CanWaitExternalClear (Policy))
  {
    /* The role is back to sink-only and any possible source has a separate
     * off proof. Retire live source flags without discarding that proof or
     * historical counters. Subsequent frames validate the quiescent state. */
    EnterExternalClearWait (Policy);
    return FALSE;
  }

  if (Policy->RoleRestoreUnproven)
  {
    EnterTerminal (Policy, LiuqinVbusPolicyStateTerminal);
  }
  else if (StopIsFault (Policy->Stop))
  {
    EnterTerminal (Policy, LiuqinVbusPolicyStateFaultStop);
  }
  else if ((Policy->Stop == LiuqinVbusPolicyStopDisabled) ||
           (Policy->Stop == LiuqinVbusPolicyStopInvalidConfig))
  {
    EnterTerminal (Policy, LiuqinVbusPolicyStateDisabled);
  }
  else
  {
    EnterTerminal (Policy, LiuqinVbusPolicyStateStopped);
  }

  return FALSE;
}

STATIC
BOOLEAN
NextCleanup (IN OUT LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence)
{
  LIUQIN_VBUS_POLICY_ACTION Action;

  Action = ExpectedCleanup (Policy, Evidence);
  if (Action != LiuqinVbusPolicyActionNone)
  {
    return TRUE;
  }

  (VOID) FinalizeStop (Policy);
  return FALSE;
}

STATIC
VOID AdvanceDebounce (IN OUT LIUQIN_VBUS_POLICY *Policy,
                      CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation)
{
  if (Policy->HasLastObservation && (Policy->LastDebounceAt == Observation->CapturedAt))
  {
    return;
  }

  Policy->LastDebounceAt = Observation->CapturedAt;

  if ((Policy->DebounceCount != 0U) && (Policy->DebounceOrientation != Observation->Cc.Orientation))
  {
    /* An orientation change is a new attach event; never sum across it. */
    Policy->DebounceOrientation = Observation->Cc.Orientation;
    Policy->DebounceCount = 1U;
    return;
  }

  if (Policy->DebounceCount == 0U)
  {
    Policy->DebounceOrientation = Observation->Cc.Orientation;
    Policy->DebounceCount = 1U;
  }
  else if (Policy->DebounceCount < Policy->Config.AttachDebounceSamples)
  {
    ++Policy->DebounceCount;
  }
}

STATIC
VOID DropDebounce (IN OUT LIUQIN_VBUS_POLICY *Policy)
{
  Policy->DebounceCount = 0U;
  Policy->DebounceOrientation = 0U;
}

STATIC
LIUQIN_VBUS_POLICY_ACTION
NextNormal (IN OUT LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
            CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation)
{
  switch (Policy->State)
  {
  case LiuqinVbusPolicyStatePreflight:
    if (GateAllows (Policy, Evidence, Observation, LiuqinVbusPolicyActionWriteRole))
    {
      return LiuqinVbusPolicyActionWriteRole;
    }

    return LiuqinVbusPolicyActionNone;

  case LiuqinVbusPolicyStateRoleAdopted:
    /* One transitional step after the role readback: either start the
     * attach debounce or fall back to the armed waiting state. */
    if (Observation->Cc.Valid && (Observation->Cc.State == LiuqinVbusPolicyCcAttachedSource) &&
        LiuqinVbusPolicyPartnerHasRd (Observation->Cc.Partner) && Observation->Cc.Attached &&
        Observation->Cc.Debounced)
    {
      DropDebounce (Policy);
      AdvanceDebounce (Policy, Observation);
      Policy->State = LiuqinVbusPolicyStateAttachDebounce;
    }
    else
    {
      Policy->State = LiuqinVbusPolicyStateDetached;
    }

    return LiuqinVbusPolicyActionNone;

  case LiuqinVbusPolicyStateDetached:
    if (!Observation->Cc.Valid)
    {
      return LiuqinVbusPolicyActionNone;
    }

    if ((Observation->Cc.State == LiuqinVbusPolicyCcAttachedSource) &&
        (Observation->Cc.Partner == LiuqinVbusPolicyPartnerSinkRa))
    {
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopPartnerUnsupported,
                                   Policy->LastTick);
      return LiuqinVbusPolicyActionNone;
    }

    if ((Observation->Cc.State == LiuqinVbusPolicyCcAttachedSource) &&
        LiuqinVbusPolicyPartnerHasRd (Observation->Cc.Partner) && Observation->Cc.Attached &&
        Observation->Cc.Debounced)
    {
      DropDebounce (Policy);
      AdvanceDebounce (Policy, Observation);
      Policy->State = LiuqinVbusPolicyStateAttachDebounce;
    }

    return LiuqinVbusPolicyActionNone;

  case LiuqinVbusPolicyStateAttachDebounce:
    if (!Observation->Cc.Valid || (Observation->Cc.State != LiuqinVbusPolicyCcAttachedSource))
    {
      DropDebounce (Policy);
      Policy->State = LiuqinVbusPolicyStateDetached;
      return LiuqinVbusPolicyActionNone;
    }

    if (Observation->Cc.Partner == LiuqinVbusPolicyPartnerSinkRa)
    {
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopPartnerUnsupported,
                                   Policy->LastTick);
      return LiuqinVbusPolicyActionNone;
    }

    if (!LiuqinVbusPolicyPartnerHasRd (Observation->Cc.Partner) || !Observation->Cc.Attached ||
        !Observation->Cc.Debounced || !Observation->Cc.SourceMode)
    {
      DropDebounce (Policy);
      Policy->State = LiuqinVbusPolicyStateDetached;
      return LiuqinVbusPolicyActionNone;
    }

    AdvanceDebounce (Policy, Observation);
    if (GateAllows (Policy, Evidence, Observation, LiuqinVbusPolicyActionEnableSource))
    {
      return LiuqinVbusPolicyActionEnableSource;
    }

    return LiuqinVbusPolicyActionNone;

  case LiuqinVbusPolicyStateMonitored:
    /* Healthy operation performs no write; observation validation already
     * requested a stop when necessary. */
    return LiuqinVbusPolicyActionNone;

  default:
    return LiuqinVbusPolicyActionNone;
  }
}

STATIC
BOOLEAN
FreshEnough (CONST LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation,
             UINT64 Now);

/* Observation validation. Returns the stop reason or StopNone. Counters and
 * last-observation bookkeeping advance at most once per CapturedAt. */
STATIC
LIUQIN_VBUS_POLICY_STOP
ValidateObservation (IN OUT LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                     CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation, UINT64 Now,
                     BOOLEAN CleanupSafetyOnly)
{
  BOOLEAN NewObservation;
  UINT64 Delta;
  UINT32 Age;
  UINT8 Reserved;
  BOOLEAN FreshRoleProof;
  LIUQIN_VBUS_POLICY_STOP ProofFailure;

  if (!EvidenceReady (Evidence))
  {
    return LiuqinVbusPolicyStopEvidence;
  }

  if ((Observation == NULL) || !Observation->Valid)
  {
    ++Policy->Counters.FramesRejected;
    return LiuqinVbusPolicyStopObservationInvalid;
  }

  if (Observation->CapturedAt > Now)
  {
    ++Policy->Counters.FramesRejected;
    return LiuqinVbusPolicyStopObservationInvalid;
  }

  if (Policy->Config.AllowExternalReattach && Policy->HasLastObservation &&
      ((Policy->Stop == LiuqinVbusPolicyStopExternalPower) ||
       Policy->RequireFreshRoleObservation) &&
      (Observation->CapturedAt < Policy->LastObservationAt))
  {
    ++Policy->Counters.FramesRejected;
    return LiuqinVbusPolicyStopObservationInvalid;
  }

  /* Counters semantics: FramesAccepted counts structurally complete
   * observations (NewObservation bookkeeping below); FramesRejected counts
   * observations rejected at the frame-validity level, including a valid
   * wrapper whose CC/PMIC sub-observation is missing. */

  NewObservation =
      !Policy->HasLastObservation || (Policy->LastObservationAt != Observation->CapturedAt);
  if (NewObservation)
  {
    Policy->HasLastObservation = TRUE;
    Policy->LastObservationAt = Observation->CapturedAt;
    ++Policy->Counters.FramesAccepted;

    Delta = Now - Observation->CapturedAt;
    Age = (Delta > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (UINT32)Delta;
    Policy->LastFrameAgeUs = Age;
    if (Age > Policy->MaximumFrameAgeUs)
    {
      Policy->MaximumFrameAgeUs = Age;
    }

    Policy->Counters.LastFrameAgeUs = Policy->LastFrameAgeUs;
    Policy->Counters.MaximumFrameAgeUs = Policy->MaximumFrameAgeUs;
    Policy->Counters.LastFaultBits = Observation->Pmic.BstFault;
    Policy->Counters.LastFailBits = Observation->Pmic.BstFail;
    Policy->Counters.LastOcpBits = Observation->Pmic.AnalogLoop;
    Policy->Counters.LastPowerPath = Observation->Pmic.PowerPath;
    if (Observation->Cc.WaterDetected)
    {
      ++Policy->Counters.WaterRecords;
    }
  }

  if (!FreshEnough (Policy, Observation, Now))
  {
    ++Policy->Counters.FramesRejected;
    return LiuqinVbusPolicyStopObservationStale;
  }

  /* PMIC validity first: a decoder that rejected the frame because a defined
   * PMIC byte carried a reserved bit must surface as Fault (with the
   * unknown-bit counter), not as a generic invalid observation. */
  if (!Observation->Pmic.Valid)
  {
    ++Policy->Counters.FramesRejected;
    return LiuqinVbusPolicyStopObservationInvalid;
  }

  /* A later observation can revoke a completed cleanup proof. Process both
   * contradictions before returning, so one failure cannot hide the other.
   * The adapter's reused pre-cleanup frame never supersedes a newer proof. */
  ProofFailure = LiuqinVbusPolicyStopNone;
  if (Policy->Config.AllowExternalReattach && CleanupSafetyOnly &&
      (Policy->State == LiuqinVbusPolicyStateStopping) &&
      (Policy->Stop == LiuqinVbusPolicyStopExternalPower))
  {
    FreshRoleProof = Policy->RoleRestored && (Observation->CapturedAt > Policy->RoleRestoredAt);
    if (FreshRoleProof && ((Observation->Pmic.RoleByte & LIUQIN_VBUS_POLICY_ROLE_MASK) !=
                           LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY))
    {
      Policy->RoleRestored = FALSE;
      Policy->RoleRestoreUnproven = TRUE;
      ProofFailure = LiuqinVbusPolicyStopRoleLost;
    }

    if ((FreshRoleProof ||
         (Policy->SourceOffVerified && (Observation->CapturedAt > Policy->SourceOffVerifiedAt))) &&
        (((Observation->Pmic.BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) != 0U) ||
         ((Observation->Pmic.BstCfg & LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC) != 0U) ||
         ((Observation->Pmic.BstStatus & (LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART |
                                          LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB)) != 0U) ||
         ((Observation->Pmic.EngCfg7 & LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF) != 0U)))
    {
      if (Policy->SourceOffVerified && (Observation->CapturedAt > Policy->SourceOffVerifiedAt))
      {
        Policy->SourceOffVerified = FALSE;
      }

      /* Preserve ownership and the consumed disable attempt. For our old
       * source only remaining bounded readbacks may prove it off again; an
       * idle role session must never issue a disable against foreign boost. */
      ProofFailure = Policy->SourceMayBeOn ? LiuqinVbusPolicyStopSourceLost
                                           : LiuqinVbusPolicyStopInheritedSource;
    }
  }

  if (!Observation->Pmic.IdentityValid)
  {
    if (NewObservation)
    {
      ++Policy->Counters.IdentityMismatchEvents;
    }

    return LiuqinVbusPolicyStopIdentity;
  }

  if (!Observation->Pmic.OwnerValid || !Observation->Pmic.OwnerStable)
  {
    if (NewObservation)
    {
      ++Policy->Counters.OwnerMismatchEvents;
    }

    return LiuqinVbusPolicyStopOwner;
  }

  if (!LockAccepted (Policy, &Observation->Pmic))
  {
    if (NewObservation)
    {
      ++Policy->Counters.LockMismatchEvents;
    }

    return LiuqinVbusPolicyStopLock;
  }

  if (!Observation->Pmic.BaselineValid || Observation->Pmic.ManagerConflict)
  {
    if (NewObservation)
    {
      Policy->LastObservedManagerChanged = TRUE;
    }

    return LiuqinVbusPolicyStopManager;
  }

  /* Reject undefined/reserved bits ourselves as well as trusting the
   * caller's UnknownBits summary: a buggy decoder must not widen a gate. */
  Reserved = (UINT8)(Observation->Pmic.BstEn & (UINT8) ~(UINT8)LIUQIN_VBUS_POLICY_BST_EN_ALLOWED);
  Reserved = (UINT8)(Reserved | (UINT8)(Observation->Pmic.BstCfg &
                                        (UINT8) ~(UINT8)LIUQIN_VBUS_POLICY_BST_CFG_ALLOWED));
  Reserved = (UINT8)(Reserved | (UINT8)(Observation->Pmic.BstStatus &
                                        (UINT8) ~(UINT8)LIUQIN_VBUS_POLICY_BST_STATUS_ALLOWED));
  Reserved = (UINT8)(Reserved | (UINT8)(Observation->Pmic.BstFault &
                                        (UINT8) ~(UINT8)LIUQIN_VBUS_POLICY_BST_FAULT_ALLOWED));
  Reserved = (UINT8)(Reserved | (UINT8)(Observation->Pmic.BstFail &
                                        (UINT8) ~(UINT8)LIUQIN_VBUS_POLICY_BST_FAIL_ALLOWED));
  if ((Reserved != 0U) || (Observation->Pmic.UnknownBits != 0U) ||
      (Observation->Cc.UnknownBits != 0U))
  {
    if (NewObservation)
    {
      ++Policy->Counters.UnknownBitEvents;
    }

    return LiuqinVbusPolicyStopFault;
  }

  if (!Observation->Cc.Valid)
  {
    ++Policy->Counters.FramesRejected;
    return LiuqinVbusPolicyStopObservationInvalid;
  }

  if ((Observation->Cc.State == LiuqinVbusPolicyCcFault) || (Observation->Cc.FaultBits != 0U))
  {
    if (NewObservation)
    {
      ++Policy->Counters.FaultEvents;
      Policy->LastObservedFault = TRUE;
    }

    return LiuqinVbusPolicyStopFault;
  }

  if ((Observation->Pmic.BstFault != 0U) || (Observation->Pmic.BstFail != 0U))
  {
    if (NewObservation)
    {
      ++Policy->Counters.FaultEvents;
      Policy->LastObservedFault = TRUE;
    }

    return LiuqinVbusPolicyStopFault;
  }

  if ((Observation->Pmic.AnalogLoop & LIUQIN_VBUS_POLICY_ANALOG_LOOP_OCP) != 0U)
  {
    if (NewObservation)
    {
      ++Policy->Counters.OcpEvents;
      if (Policy->OcpStreak < 0xFFFFFFFFU)
      {
        ++Policy->OcpStreak;
      }
    }

    if (Policy->OcpStreak >= Policy->Config.OcpDebounceSamples)
    {
      return LiuqinVbusPolicyStopOverCurrent;
    }
  }
  else
  {
    Policy->OcpStreak = 0U;
  }

  if (ProofFailure != LiuqinVbusPolicyStopNone)
  {
    return ProofFailure;
  }

  /* Cleanup deliberately changes role and source state. Still latch all
   * observation/identity/ownership/protection failures, but do not diagnose
   * our own completed disable as source loss or role restore as role loss. */
  if (CleanupSafetyOnly)
  {
    /* No policy action changes the enable-source selector: observing it set
     * during cleanup is a foreign manager change even if it later clears. */
    if (((Observation->Pmic.BstCfg & LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC) != 0U) ||
        (Policy->SourceOnVerified &&
         ((Observation->Pmic.EngCfg7 & LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF) != 0U)))
    {
      Policy->LastObservedManagerChanged = TRUE;
      return LiuqinVbusPolicyStopManager;
    }

    return LiuqinVbusPolicyStopNone;
  }

  /* External power: never enable against it. Before the source role is
   * adopted the inherited sink-only role keeps normal charging working, so
   * this is not a stop; once the source role was adopted or a boost may be
   * on, any external supply (declared or candidate bit) is a fail-safe stop. */
  if (Observation->ExternalPower == LiuqinVbusPolicyExternalPresent)
  {
    Policy->LastObservedExternal = TRUE;
    if (NewObservation)
    {
      ++Policy->Counters.ExternalEvents;
    }

    if (Policy->SourceMayBeOn || Policy->RoleAdopted)
    {
      if (!Policy->StopLatched)
      {
        Policy->ExternalStopConfirmed = TRUE;
      }
      return LiuqinVbusPolicyStopExternalPower;
    }
  }
  else
  {
    Policy->LastObservedExternal = FALSE;
  }

  if (Policy->Config.TrustPowerPathBit7Candidate &&
      ((Observation->Pmic.PowerPath & LIUQIN_VBUS_POLICY_POWER_PATH_INPUT) != 0U))
  {
    Policy->LastObservedExternal = TRUE;
    if (NewObservation)
    {
      ++Policy->Counters.ExternalEvents;
    }

    if (Policy->SourceMayBeOn || Policy->RoleAdopted)
    {
      if (!Policy->StopLatched)
      {
        Policy->ExternalStopConfirmed = TRUE;
      }
      return LiuqinVbusPolicyStopExternalPower;
    }
  }

  if ((Observation->ExternalPower == LiuqinVbusPolicyExternalUnknown) && Policy->SourceMayBeOn)
  {
    if (NewObservation)
    {
      ++Policy->Counters.ExternalEvents;
    }

    if (!Policy->StopLatched)
    {
      Policy->ExternalStopConfirmed = FALSE;
    }

    return LiuqinVbusPolicyStopExternalPower;
  }

  if (!Policy->SourceMayBeOn)
  {
    /* Pre-existing source/boost state not created by this manager: never
     * drive or disable another owner's boost. */
    if (((Observation->Pmic.BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) != 0U) ||
        ((Observation->Pmic.BstCfg & LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC) != 0U) ||
        ((Observation->Pmic.BstStatus & (LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART |
                                         LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB)) != 0U) ||
        ((Observation->Pmic.EngCfg7 & LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF) != 0U))
    {
      return LiuqinVbusPolicyStopInheritedSource;
    }
  }
  else
  {
    if ((Observation->Pmic.BstCfg & LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC) != 0U)
    {
      return LiuqinVbusPolicyStopManager;
    }

    if (Policy->SourceOnVerified &&
        ((Observation->Pmic.EngCfg7 & LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF) != 0U))
    {
      return LiuqinVbusPolicyStopManager;
    }

    /* Our verified source must still be the one reporting on. The boost
     * enable bit or the BOOST_ON_USB status dropping is a source loss, never
     * something to re-drive. */
    if (Policy->SourceOnVerified &&
        (((Observation->Pmic.BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) == 0U) ||
         ((Observation->Pmic.BstStatus & LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB) == 0U)))
    {
      return LiuqinVbusPolicyStopSourceLost;
    }

    if (!(Observation->Cc.Attached && Observation->Cc.Debounced && Observation->Cc.SourceMode &&
          (Observation->Cc.State == LiuqinVbusPolicyCcAttachedSource) &&
          LiuqinVbusPolicyPartnerHasRd (Observation->Cc.Partner)))
    {
      return LiuqinVbusPolicyStopDetach;
    }
  }

  if (Policy->RoleAdopted && !Policy->RoleRestoreAttempted &&
      ((Observation->Pmic.RoleByte & LIUQIN_VBUS_POLICY_ROLE_MASK) !=
       LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY))
  {
    return LiuqinVbusPolicyStopRoleLost;
  }

  if (!Policy->RoleAdopted && !Policy->RoleMayHaveChanged &&
      ((Observation->Pmic.RoleByte & LIUQIN_VBUS_POLICY_ROLE_MASK) !=
       LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY))
  {
    /* UEFI must not blindly overwrite a role some other manager configured. */
    return LiuqinVbusPolicyStopManager;
  }

  return LiuqinVbusPolicyStopNone;
}

STATIC
BOOLEAN
FreshEnough (CONST LIUQIN_VBUS_POLICY *Policy, CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation,
             UINT64 Now)
{
  UINT64 Delta;

  if (Observation->CapturedAt > Now)
  {
    return FALSE;
  }

  Delta = Now - Observation->CapturedAt;
  if (Delta > 0xFFFFFFFFULL)
  {
    return FALSE;
  }

  return (UINT32)Delta <= Policy->Config.MaximumObservationAgeUs;
}

STATIC
BOOLEAN
ExternalClearFrame (CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation)
{
  return (Observation->Cc.State == LiuqinVbusPolicyCcDetached) && !Observation->Cc.Attached &&
         !Observation->Cc.VbusDetected &&
         (Observation->ExternalPower == LiuqinVbusPolicyExternalAbsent) &&
         ((Observation->Pmic.PowerPath & LIUQIN_VBUS_POLICY_POWER_PATH_INPUT) == 0U) &&
         ((Observation->Pmic.AnalogLoop & LIUQIN_VBUS_POLICY_ANALOG_LOOP_OCP) == 0U);
}

STATIC
VOID WaitExternalClear (IN OUT LIUQIN_VBUS_POLICY *Policy,
                        CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation)
{
  BOOLEAN NewObservation;

  NewObservation = (Observation->CapturedAt > Policy->ExternalClearAfter) &&
                   (Observation->CapturedAt > Policy->LastExternalClearAt);
  if (NewObservation)
  {
    Policy->LastExternalClearAt = Observation->CapturedAt;
  }

  if (!ExternalClearFrame (Observation))
  {
    Policy->ExternalClearCount = 0U;
    return;
  }

  if (!NewObservation)
  {
    return;
  }

  ++Policy->ExternalClearCount;
  if (Policy->ExternalClearCount < Policy->Config.AttachDebounceSamples)
  {
    return;
  }

  /* Only this proved-clean external disconnect permits another role
   * attempt. Do not reset the journal, sequence, lifetime action budget or
   * counters. The next role action needs a later observation, even when a
   * caller repeatedly invokes Next within one timer callback. */
  Policy->StopLatched = FALSE;
  Policy->Stop = LiuqinVbusPolicyStopNone;
  Policy->RoleAttemptsThisSession = 0U;
  Policy->RoleRestoreAttempted = FALSE;
  Policy->DisableAttempted = FALSE;
  Policy->IndependentShutdownAttempted = FALSE;
  Policy->OffChecks = 0U;
  Policy->EnableAttemptsThisCycle = 0U;
  Policy->DebounceCount = 0U;
  Policy->DebounceOrientation = 0U;
  Policy->OcpStreak = 0U;
  Policy->SourceOnAt = 0U;
  Policy->RoleAdoptedAt = 0U;
  Policy->ExternalClearCount = 0U;
  Policy->RecoveryPreflightAfter = Policy->LastTick;
  Policy->RequireFreshRoleObservation = TRUE;
  Policy->State = LiuqinVbusPolicyStatePreflight;
}

STATIC
VOID Deadlines (IN OUT LIUQIN_VBUS_POLICY *Policy, UINT64 Now)
{
  UINT64 ShutdownBound;

  if (PolicyTerminal (Policy))
  {
    return;
  }

  if (Policy->InFlight && (Now >= Policy->ActionStartedAt) &&
      ((Now - Policy->ActionStartedAt) >= (UINT64)Policy->Config.ActionTimeoutUs))
  {
    if (!Policy->ActionTimedOut)
    {
      Policy->ActionTimedOut = TRUE;
      ++Policy->Counters.ActionTimeouts;
    }

    LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopActionTimeout, Now);
  }

  if (!Policy->StopLatched)
  {
    if (Policy->SourceMayBeOn && (Policy->Config.MaximumSourceOnUs != 0U) &&
        (Now >= Policy->SourceOnAt) &&
        ((Now - Policy->SourceOnAt) >= (UINT64)Policy->Config.MaximumSourceOnUs))
    {
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopSourceWindow, Now);
    }
    else if (!Policy->SourceMayBeOn && Policy->RoleAdopted &&
             (Policy->Config.AttachWindowUs != 0U) && (Policy->RoleAdoptedAt != 0U) &&
             (Now >= Policy->RoleAdoptedAt) &&
             ((Now - Policy->RoleAdoptedAt) >= (UINT64)Policy->Config.AttachWindowUs))
    {
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopAttachWindow, Now);
    }
  }
  else if (Policy->State != LiuqinVbusPolicyStateWaitExternalClear)
  {
    ShutdownBound =
        (UINT64)Policy->Config.ActionTimeoutUs * ((UINT64)Policy->Config.StopReadbackChecks + 4U);
    if ((Now >= Policy->ShutdownStartedAt) && ((Now - Policy->ShutdownStartedAt) >= ShutdownBound))
    {
      Policy->ShutdownDeadlineMissed = TRUE;
    }
  }
}

VOID LiuqinVbusPolicyInitialize (OUT LIUQIN_VBUS_POLICY *Policy,
                                 IN CONST LIUQIN_VBUS_POLICY_CONFIG *Config, IN UINT64 Now)
{
  if (Policy == NULL)
  {
    return;
  }

  ClearBytes (Policy, sizeof (*Policy));
  Policy->State = LiuqinVbusPolicyStateDisabled;
  Policy->Stop = LiuqinVbusPolicyStopDisabled;
  Policy->StartedAt = Now;
  Policy->LastTick = Now;

  if ((Config == NULL) || !Config->Enabled)
  {
    return;
  }

  Policy->Config = *Config;
  if (!ConfigValid (Config))
  {
    Policy->Stop = LiuqinVbusPolicyStopInvalidConfig;
    return;
  }

  Policy->State = LiuqinVbusPolicyStatePreflight;
  Policy->Stop = LiuqinVbusPolicyStopNone;
  Policy->NextSequence = 1U;
}

VOID LiuqinVbusPolicyRequestStop (IN OUT LIUQIN_VBUS_POLICY *Policy,
                                  IN LIUQIN_VBUS_POLICY_STOP Reason, IN UINT64 Now)
{
  if ((Policy == NULL) || (Reason == LiuqinVbusPolicyStopNone) ||
      ((UINT32)Reason > (UINT32)LiuqinVbusPolicyStopShutdownUnproven))
  {
    return;
  }

  if (Reason == LiuqinVbusPolicyStopExitBootServices)
  {
    Policy->EbsRequested = TRUE;
  }

  if (PolicyTerminal (Policy))
  {
    return;
  }

  if ((Policy->Stop == LiuqinVbusPolicyStopExternalPower) &&
      (Reason != LiuqinVbusPolicyStopExternalPower))
  {
    Policy->ExternalReattachBlocked = TRUE;
  }

  if (!Policy->StopLatched)
  {
    Policy->StopLatched = TRUE;
    Policy->Stop = Reason;
    Policy->ShutdownStartedAt = Now;
    Policy->DebounceCount = 0U;
    Policy->DebounceOrientation = 0U;
    /* A planned normal action is revoked; a planned cleanup action stays. */
    if (Policy->Planned && (Policy->Ticket.Action < LiuqinVbusPolicyActionDisableSource))
    {
      Policy->Planned = FALSE;
    }
  }
  else if (StopIsFault (Reason) && !StopIsFault (Policy->Stop))
  {
    /* Upgrade a clean reason to a fault reason; the first clean reason and
     * every fault reason remain visible through the counters. */
    Policy->Stop = Reason;
  }

  Policy->State = LiuqinVbusPolicyStateStopping;
}

VOID LiuqinVbusPolicyRequestExitBootServices (IN OUT LIUQIN_VBUS_POLICY *Policy,
                                              IN CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                                              IN UINT64 Now)
{
  if (Policy == NULL)
  {
    return;
  }

  Policy->EbsRequested = TRUE;
  /* Report-only snapshot of the adjudicated Windows bring-up contract. */
  Policy->WindowsBringupContractSeen = (Evidence != NULL) && Evidence->WindowsBringupRetainVerified;

  if (PolicyTerminal (Policy))
  {
    return;
  }

  if (Policy->State == LiuqinVbusPolicyStateWaitExternalClear)
  {
    /* Cleanup was already verified before entering this read-only wait. */
    EnterTerminal (Policy, LiuqinVbusPolicyStateStopped);
    return;
  }

  /* Keep-on is only reachable with the explicit profile, a reviewed OS VBUS
   * owner, a healthy monitored source and no recorded safety event. */
  if ((Policy->Config.EbsMode == LiuqinVbusPolicyEbsKeepOnIfOsOwned) && (Evidence != NULL) &&
      Evidence->OsVbusOwnerVerified && Policy->SourceOnVerified &&
      (Policy->State == LiuqinVbusPolicyStateMonitored) && !Policy->StopLatched &&
      !Policy->LastObservedFault && !Policy->LastObservedExternal &&
      !Policy->LastObservedManagerChanged)
  {
    Policy->EbsKeepOnLatched = TRUE;
    EnterTerminal (Policy, LiuqinVbusPolicyStateTerminal);
    return;
  }

  /* Windows/OOBE bring-up retention is a separate, explicit exception: it
   * never claims an OS owner, requires the adjudicated bring-up contract and
   * a verified healthy monitored source, and refuses to anything else. On
   * refusal the ordinary controlled-off stop path below latches the
   * ExitBootServices stop; the report names the first refusal cause. */
  if (Policy->Config.EbsMode == LiuqinVbusPolicyEbsKeepSourceForWindowsBringup)
  {
    if ((Evidence != NULL) && Evidence->WindowsBringupRetainVerified && Policy->SourceOnVerified &&
        (Policy->State == LiuqinVbusPolicyStateMonitored) && !Policy->StopLatched &&
        !Policy->LastObservedFault && !Policy->LastObservedExternal &&
        !Policy->LastObservedManagerChanged)
    {
      Policy->EbsKeepOnLatched = TRUE;
      EnterTerminal (Policy, LiuqinVbusPolicyStateTerminal);
      return;
    }

    LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopExitBootServices, Now);
    return;
  }

  LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopExitBootServices, Now);
}

BOOLEAN
LiuqinVbusPolicyNext (IN OUT LIUQIN_VBUS_POLICY *Policy,
                      IN CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                      IN CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation, IN UINT64 Now,
                      OUT LIUQIN_VBUS_POLICY_TICKET *Ticket)
{
  LIUQIN_VBUS_POLICY_ACTION Action;
  LIUQIN_VBUS_POLICY_STOP Reason;

  if (Ticket != NULL)
  {
    ClearBytes (Ticket, sizeof (*Ticket));
  }

  if ((Policy == NULL) || (Ticket == NULL))
  {
    return FALSE;
  }

  if (Evidence != NULL)
  {
    /* Report-only snapshot of the explicit Windows bring-up adjudication. */
    Policy->WindowsBringupContractSeen = Evidence->WindowsBringupRetainVerified;
  }

  if (PolicyTerminal (Policy))
  {
    return FALSE;
  }

  (VOID) ClockTick (Policy, Now);
  Deadlines (Policy, Now);
  if (PolicyTerminal (Policy) || Policy->InFlight)
  {
    return FALSE;
  }

  if (!Policy->StopLatched)
  {
    if ((Observation == NULL) || !Observation->Valid)
    {
      ++Policy->Counters.FramesRejected;
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopObservationInvalid, Now);
    }
    else
    {
      Reason = ValidateObservation (Policy, Evidence, Observation, Now, FALSE);
      if (Reason != LiuqinVbusPolicyStopNone)
      {
        LiuqinVbusPolicyRequestStop (Policy, Reason, Now);
      }
    }
  }

  if (!Policy->StopLatched && Policy->RequireFreshRoleObservation &&
      (Policy->State == LiuqinVbusPolicyStatePreflight) && !ExternalClearFrame (Observation))
  {
    EnterExternalClearWait (Policy);
    return FALSE;
  }

  if (Policy->State == LiuqinVbusPolicyStateWaitExternalClear)
  {
    /* The adapter can call Next again with the last cleanup frame. Validate
     * its safety metadata without misreading pre-disable source state as a
     * new foreign source; it can never advance the external-clear proof. */
    Reason = ValidateObservation (Policy, Evidence, Observation, Now,
                                  (Observation != NULL) &&
                                      (Observation->CapturedAt <= Policy->ExternalClearAfter));
    if (Reason != LiuqinVbusPolicyStopNone)
    {
      LiuqinVbusPolicyRequestStop (Policy, Reason, Now);
    }
    else
    {
      WaitExternalClear (Policy, Observation);
      return FALSE;
    }
  }
  else if (Policy->StopLatched && Policy->Config.AllowExternalReattach &&
           (Policy->Stop == LiuqinVbusPolicyStopExternalPower))
  {
    Reason = ValidateObservation (Policy, Evidence, Observation, Now, TRUE);
    if (Reason != LiuqinVbusPolicyStopNone)
    {
      LiuqinVbusPolicyRequestStop (Policy, Reason, Now);
    }
  }

  if (Policy->StopLatched)
  {
    if (!NextCleanup (Policy, Evidence))
    {
      return FALSE;
    }

    Action = ExpectedCleanup (Policy, Evidence);
  }
  else
  {
    Action = NextNormal (Policy, Evidence, Observation);
  }

  if (Action == LiuqinVbusPolicyActionNone)
  {
    return FALSE;
  }

  if (!Policy->StopLatched && (Policy->ActionsStarted >= (LIUQIN_VBUS_POLICY_MAX_ACTIONS -
                                                          LIUQIN_VBUS_POLICY_CLEANUP_RESERVE)))
  {
    LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopActionFailure, Now);
    return FALSE;
  }

  if (Policy->NextSequence == 0xFFFFFFFFU)
  {
    Policy->ShutdownUnproven = TRUE;
    EnterTerminal (Policy, LiuqinVbusPolicyStateShutdownUnproven);
    return FALSE;
  }

  Policy->Ticket.Sequence = Policy->NextSequence;
  Policy->Ticket.Action = Action;
  Policy->Ticket.RegisterValue = 0U;
  Policy->Ticket.RegisterMask = 0U;
  Policy->Ticket.TimeoutUs = Policy->Config.ActionTimeoutUs;
  if (Action == LiuqinVbusPolicyActionWriteRole)
  {
    Policy->Ticket.RegisterValue = LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY;
    Policy->Ticket.RegisterMask = LIUQIN_VBUS_POLICY_ROLE_MASK;
  }
  else if (Action == LiuqinVbusPolicyActionRestoreRole)
  {
    Policy->Ticket.RegisterValue = LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY;
    Policy->Ticket.RegisterMask = LIUQIN_VBUS_POLICY_ROLE_MASK;
  }

  ++Policy->NextSequence;
  Policy->Planned = TRUE;
  *Ticket = Policy->Ticket;
  return TRUE;
}

STATIC
BOOLEAN
SameTicket (CONST LIUQIN_VBUS_POLICY_TICKET *A, CONST LIUQIN_VBUS_POLICY_TICKET *B)
{
  return (A != NULL) && (B != NULL) && (A->Sequence == B->Sequence) && (A->Action == B->Action);
}

BOOLEAN
LiuqinVbusPolicyBegin (IN OUT LIUQIN_VBUS_POLICY *Policy,
                       IN CONST LIUQIN_VBUS_POLICY_TICKET *Ticket,
                       IN CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                       IN CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation, IN UINT64 Now)
{
  LIUQIN_VBUS_POLICY_STOP Reason;

  if ((Policy == NULL) || (Ticket == NULL) || PolicyTerminal (Policy) || Policy->InFlight ||
      !Policy->Planned || !SameTicket (Ticket, &Policy->Ticket))
  {
    return FALSE;
  }

  (VOID) ClockTick (Policy, Now);
  Deadlines (Policy, Now);
  if (PolicyTerminal (Policy) || !Policy->Planned || Policy->InFlight)
  {
    return FALSE;
  }

  if (!Policy->StopLatched && Policy->RequireFreshRoleObservation &&
      (Policy->State == LiuqinVbusPolicyStatePreflight))
  {
    Reason = ValidateObservation (Policy, Evidence, Observation, Now, FALSE);
    if (Reason != LiuqinVbusPolicyStopNone)
    {
      LiuqinVbusPolicyRequestStop (Policy, Reason, Now);
      Policy->Planned = FALSE;
      return FALSE;
    }

    if (!ExternalClearFrame (Observation))
    {
      EnterExternalClearWait (Policy);
      return FALSE;
    }
  }

  if (Policy->StopLatched)
  {
    if ((Ticket->Action < LiuqinVbusPolicyActionDisableSource) ||
        (ExpectedCleanup (Policy, Evidence) != Ticket->Action))
    {
      Policy->Planned = FALSE;
      return FALSE;
    }
  }
  else
  {
    if (!GateAllows (Policy, Evidence, Observation, Ticket->Action) ||
        !FreshEnough (Policy, Observation, Now))
    {
      Policy->Planned = FALSE;
      return FALSE;
    }
  }

  Policy->Planned = FALSE;
  Policy->InFlight = TRUE;
  Policy->ActionTimedOut = FALSE;
  Policy->ActionStartedAt = Now;
  ++Policy->ActionsStarted;

  switch (Ticket->Action)
  {
  case LiuqinVbusPolicyActionWriteRole:
    Policy->RoleMayHaveChanged = TRUE;
    Policy->RoleRestored = FALSE;
    if (Policy->RequireFreshRoleObservation)
    {
      ++Policy->Counters.Cycles;
      ++Policy->Counters.ReattachArmed;
    }

    Policy->RequireFreshRoleObservation = FALSE;
    Policy->ExternalStopConfirmed = FALSE;
    ++Policy->RoleAttemptsThisSession;
    ++Policy->Counters.RoleWrites;
    break;
  case LiuqinVbusPolicyActionEnableSource:
    /* Journal the possible source-on effect BEFORE the adapter call. */
    Policy->SourceMayBeOn = TRUE;
    Policy->SourceOnVerified = FALSE;
    Policy->SourceOffVerified = FALSE;
    ++Policy->EnableAttemptsThisCycle;
    ++Policy->Counters.EnableAttempts;
    break;
  case LiuqinVbusPolicyActionDisableSource:
    Policy->DisableAttempted = TRUE;
    ++Policy->Counters.StopAttempts;
    break;
  case LiuqinVbusPolicyActionVerifySourceOff:
    ++Policy->OffChecks;
    ++Policy->Counters.StopAttempts;
    break;
  case LiuqinVbusPolicyActionTriggerIndependentShutdown:
    Policy->IndependentShutdownAttempted = TRUE;
    break;
  case LiuqinVbusPolicyActionRestoreRole:
    Policy->RoleRestoreAttempted = TRUE;
    break;
  default:
    break;
  }

  return TRUE;
}

BOOLEAN
LiuqinVbusPolicyComplete (IN OUT LIUQIN_VBUS_POLICY *Policy,
                          IN CONST LIUQIN_VBUS_POLICY_TICKET *Ticket,
                          IN CONST LIUQIN_VBUS_POLICY_ACTION_RESULT *Result, IN UINT64 Now)
{
  BOOLEAN Success;

  if ((Policy == NULL) || (Ticket == NULL) || (Result == NULL) || !Policy->InFlight ||
      !SameTicket (Ticket, &Policy->Ticket))
  {
    return FALSE;
  }

  (VOID) ClockTick (Policy, Now);
  Deadlines (Policy, Now);
  Policy->InFlight = FALSE;

  if (PolicyTerminal (Policy))
  {
    return FALSE;
  }

  Success = !Policy->ActionTimedOut && (Result->Outcome == LiuqinVbusPolicyActionSucceeded) &&
            (Result->Facts == LiuqinVbusPolicyRequiredFacts (Ticket->Action));

  if (!Success)
  {
    ++Policy->Counters.ActionFailures;
    switch (Ticket->Action)
    {
    case LiuqinVbusPolicyActionWriteRole:
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopRoleWriteFailed, Now);
      break;
    case LiuqinVbusPolicyActionEnableSource:
      /* SourceMayBeOn was journaled before the adapter call: cleanup is
       * mandatory even when the adapter reports failure. */
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopEnableFailed, Now);
      break;
    case LiuqinVbusPolicyActionDisableSource:
      Policy->DisableFailed = TRUE;
      /* A failed/uncertain stop command is never re-armed as clean. */
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopActionFailure, Now);
      break;
    case LiuqinVbusPolicyActionVerifySourceOff:
      /* Any failed off-readback blocks the clean re-arm path even if a
       * later readback succeeds; the failure stays visible. */
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopActionFailure, Now);
      break;
    case LiuqinVbusPolicyActionTriggerIndependentShutdown:
      LiuqinVbusPolicyRequestStop (Policy, LiuqinVbusPolicyStopActionFailure, Now);
      break;
    case LiuqinVbusPolicyActionRestoreRole:
      Policy->RoleRestoreUnproven = TRUE;
      break;
    default:
      break;
    }

    return TRUE;
  }

  switch (Ticket->Action)
  {
  case LiuqinVbusPolicyActionWriteRole:
    Policy->RoleAdopted = TRUE;
    Policy->RoleAdoptedAt = Now;
    Policy->State = LiuqinVbusPolicyStateRoleAdopted;
    break;
  case LiuqinVbusPolicyActionEnableSource:
    Policy->SourceOnVerified = TRUE;
    ++Policy->Counters.EnableVerified;
    Policy->SourceOnAt = Now;
    Policy->State = LiuqinVbusPolicyStateMonitored;
    break;
  case LiuqinVbusPolicyActionDisableSource:
    break;
  case LiuqinVbusPolicyActionVerifySourceOff:
    Policy->SourceOffVerified = TRUE;
    Policy->SourceOffVerifiedAt = Now;
    ++Policy->Counters.StopVerified;
    break;
  case LiuqinVbusPolicyActionTriggerIndependentShutdown:
    break;
  case LiuqinVbusPolicyActionRestoreRole:
    Policy->RoleAdopted = FALSE;
    Policy->RoleRestored = TRUE;
    Policy->RoleRestoredAt = Now;
    Policy->RoleMayHaveChanged = FALSE;
    break;
  default:
    break;
  }

  return TRUE;
}

UINT32
LiuqinVbusPolicyRequiredFacts (IN LIUQIN_VBUS_POLICY_ACTION Action)
{
  switch (Action)
  {
  case LiuqinVbusPolicyActionWriteRole:
    return LIUQIN_VBUS_POLICY_FACT_ROLE_APPLIED;
  case LiuqinVbusPolicyActionEnableSource:
    return LIUQIN_VBUS_POLICY_FACT_SOURCE_ON;
  case LiuqinVbusPolicyActionVerifySourceOff:
    return LIUQIN_VBUS_POLICY_FACT_SOURCE_OFF;
  case LiuqinVbusPolicyActionTriggerIndependentShutdown:
    return LIUQIN_VBUS_POLICY_FACT_INDEPENDENT;
  case LiuqinVbusPolicyActionRestoreRole:
    return LIUQIN_VBUS_POLICY_FACT_ROLE_RESTORED;
  default:
    return 0U;
  }
}

VOID LiuqinVbusPolicyGetPermits (IN CONST LIUQIN_VBUS_POLICY *Policy,
                                 IN CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                                 IN CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation,
                                 OUT LIUQIN_VBUS_POLICY_PERMITS *Permits)
{
  LIUQIN_VBUS_POLICY_EBS_DECISION Decision;

  if (Permits == NULL)
  {
    return;
  }

  ClearBytes (Permits, sizeof (*Permits));
  if (Policy == NULL)
  {
    Permits->BlockingReason = LiuqinVbusPolicyStopDisabled;
    return;
  }

  if (!EvidenceReady (Evidence))
  {
    Permits->BlockingReason = LiuqinVbusPolicyStopEvidence;
  }
  else if (Policy->StopLatched)
  {
    Permits->BlockingReason = Policy->Stop;
  }
  else if ((Observation == NULL) || !Observation->Valid)
  {
    Permits->BlockingReason = LiuqinVbusPolicyStopObservationInvalid;
  }
  else if ((Observation->ExternalPower == LiuqinVbusPolicyExternalPresent) ||
           (Policy->Config.TrustPowerPathBit7Candidate &&
            ((Observation->Pmic.PowerPath & LIUQIN_VBUS_POLICY_POWER_PATH_INPUT) != 0U)))
  {
    Permits->BlockingReason = LiuqinVbusPolicyStopExternalPower;
  }
  else if (Observation->Pmic.BstFault != 0U)
  {
    Permits->BlockingReason = LiuqinVbusPolicyStopFault;
  }

  Permits->StopRequired = Policy->StopLatched && !PolicyTerminal (Policy) &&
                          (Policy->State != LiuqinVbusPolicyStateWaitExternalClear);

  if (!PolicyTerminal (Policy) && !Policy->StopLatched)
  {
    Permits->RoleWriteAllowed =
        GateAllows (Policy, Evidence, Observation, LiuqinVbusPolicyActionWriteRole);
    Permits->SourceEnableAllowed =
        GateAllows (Policy, Evidence, Observation, LiuqinVbusPolicyActionEnableSource);
  }

  LiuqinVbusPolicyGetEbsDecision (Policy, Evidence, &Decision);
  Permits->KeepOnAtEbsAllowed = Decision.KeepOn;
  Permits->HandoffAllowed = Decision.HandoffAllowed;
}

VOID LiuqinVbusPolicyGetEbsDecision (IN CONST LIUQIN_VBUS_POLICY *Policy,
                                     IN CONST LIUQIN_VBUS_POLICY_EVIDENCE *Evidence,
                                     OUT LIUQIN_VBUS_POLICY_EBS_DECISION *Decision)
{
  if (Decision == NULL)
  {
    return;
  }

  ClearBytes (Decision, sizeof (*Decision));
  Decision->RequiresStop = TRUE;
  Decision->Rationale = LiuqinVbusPolicyEbsControlledOffDefault;

  if (Policy == NULL)
  {
    return;
  }

  if (Policy->State == LiuqinVbusPolicyStateDisabled)
  {
    Decision->RequiresStop = FALSE;
    Decision->HandoffAllowed = TRUE;
    Decision->Rationale = LiuqinVbusPolicyEbsControlledOffNotEnabled;
    return;
  }

  if (Policy->ShutdownUnproven || (Policy->State == LiuqinVbusPolicyStateShutdownUnproven))
  {
    Decision->HandoffAllowed = FALSE;
    Decision->Rationale = LiuqinVbusPolicyEbsControlledOffShutdownUnproven;
    return;
  }

  if (Policy->EbsKeepOnLatched)
  {
    Decision->KeepOn = TRUE;
    Decision->RequiresStop = FALSE;
    Decision->HandoffAllowed = TRUE;
    if (Policy->Config.EbsMode == LiuqinVbusPolicyEbsKeepSourceForWindowsBringup)
    {
      /* Deliberate bring-up exception, never an OS-ownership claim. */
      Decision->WindowsBringupRetain = TRUE;
      Decision->Rationale = LiuqinVbusPolicyEbsKeepOnWindowsBringup;
    }
    else
    {
      Decision->Rationale = LiuqinVbusPolicyEbsKeepOnOsOwnsVbus;
    }

    return;
  }

  if (!Policy->SourceMayBeOn && !Policy->SourceOnVerified)
  {
    Decision->RequiresStop =
        Policy->RoleMayHaveChanged ||
        (Policy->StopLatched && (ExpectedCleanup (Policy, Evidence) != LiuqinVbusPolicyActionNone));
    Decision->HandoffAllowed = PolicyTerminal (Policy) && !Policy->RoleRestoreUnproven;
    Decision->Rationale = LiuqinVbusPolicyEbsControlledOffNotEnabled;
    return;
  }

  if (Policy->Config.EbsMode == LiuqinVbusPolicyEbsKeepOnIfOsOwned)
  {
    if (Policy->LastObservedManagerChanged)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffManagerChanged;
    }
    else if ((Evidence == NULL) || !Evidence->OsVbusOwnerVerified)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffOsOwnerUnproven;
    }
    else if (Policy->LastObservedExternal)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffExternalPower;
    }
    else if (Policy->LastObservedFault)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffFault;
    }
    else if (!Policy->SourceOnVerified || (Policy->State != LiuqinVbusPolicyStateMonitored) ||
             Policy->StopLatched)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffSourceOnUnverified;
    }
    else
    {
      Decision->KeepOn = TRUE;
      Decision->RequiresStop = FALSE;
      Decision->HandoffAllowed = TRUE;
      Decision->Rationale = LiuqinVbusPolicyEbsKeepOnOsOwnsVbus;
      return;
    }
  }
  else if (Policy->Config.EbsMode == LiuqinVbusPolicyEbsKeepSourceForWindowsBringup)
  {
    if (!Policy->EbsRequested && !Policy->StopLatched)
    {
      /* Preview/report only: keep-on is decided at ExitBootServices, never by
       * an ordinary poll. A host-visible report can therefore never claim the
       * source was retained before the EBS decision existed. */
      Decision->RequiresStop = FALSE;
      Decision->HandoffAllowed = FALSE;
      Decision->Rationale = LiuqinVbusPolicyEbsWindowsBringupPending;
      return;
    }

    /* Keep-on was not latched: report why the bring-up retention refused and
     * stay on the controlled-off path. The distinct Refused flag and the
     * dedicated rationales make this distinguishable from both the OS-owned
     * keep-on and the ordinary controlled-off default. */
    Decision->WindowsBringupRefused = Policy->EbsRequested;
    if ((Evidence == NULL) || !Evidence->WindowsBringupRetainVerified)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffWindowsBringupUnproven;
    }
    else if (Policy->LastObservedExternal)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffExternalPower;
    }
    else if (Policy->LastObservedFault)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffFault;
    }
    else if (Policy->LastObservedManagerChanged)
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffManagerChanged;
    }
    else
    {
      Decision->Rationale = LiuqinVbusPolicyEbsControlledOffWindowsBringupUnverified;
    }
  }
  else if (Policy->LastObservedExternal)
  {
    Decision->Rationale = LiuqinVbusPolicyEbsControlledOffExternalPower;
  }
  else if (Policy->LastObservedFault)
  {
    Decision->Rationale = LiuqinVbusPolicyEbsControlledOffFault;
  }
  else if (Policy->LastObservedManagerChanged)
  {
    Decision->Rationale = LiuqinVbusPolicyEbsControlledOffManagerChanged;
  }
  else
  {
    Decision->Rationale = LiuqinVbusPolicyEbsControlledOffDefault;
  }

  /* Every controlled-off rationale (including the Windows bring-up refusal)
   * still allows the OS handoff once the source is proven off and the
   * inherited role restore is proven. */
  Decision->HandoffAllowed = Policy->SourceOffVerified &&
                             (!Policy->RoleMayHaveChanged || Policy->RoleRestored) &&
                             !Policy->RoleRestoreUnproven && !Policy->ShutdownDeadlineMissed;
}

BOOLEAN
LiuqinVbusPolicyIsTerminal (IN CONST LIUQIN_VBUS_POLICY *Policy)
{
  return (Policy == NULL) || PolicyTerminal (Policy);
}

BOOLEAN
LiuqinVbusPolicyCleanupPending (IN CONST LIUQIN_VBUS_POLICY *Policy)
{
  if ((Policy == NULL) || !Policy->StopLatched || PolicyTerminal (Policy))
  {
    return FALSE;
  }

  return ExpectedCleanup (Policy, NULL) != LiuqinVbusPolicyActionNone;
}

VOID LiuqinVbusPolicyGetReport (IN CONST LIUQIN_VBUS_POLICY *Policy,
                                OUT LIUQIN_VBUS_POLICY_REPORT *Report)
{
  LIUQIN_VBUS_POLICY_EBS_DECISION Decision;

  if (Report == NULL)
  {
    return;
  }

  ClearBytes (Report, sizeof (*Report));
  if (Policy == NULL)
  {
    Report->State = LiuqinVbusPolicyStateDisabled;
    Report->Stop = LiuqinVbusPolicyStopDisabled;
    return;
  }

  Report->State = Policy->State;
  Report->Stop = Policy->Stop;
  Report->Counters = Policy->Counters;
  Report->RoleAdopted = Policy->RoleAdopted;
  Report->RoleRestored = Policy->RoleRestored;
  Report->RoleRestoreUnproven = Policy->RoleRestoreUnproven;
  Report->SourceMayBeOn = Policy->SourceMayBeOn;
  Report->SourceOnVerified = Policy->SourceOnVerified;
  Report->SourceOffVerified = Policy->SourceOffVerified;
  Report->ShutdownUnproven = Policy->ShutdownUnproven;
  Report->EbsRequested = Policy->EbsRequested;

  if (Policy->EbsKeepOnLatched)
  {
    Report->KeepOnAtEbs = TRUE;
    Report->HandoffAllowed = TRUE;
    if (Policy->Config.EbsMode == LiuqinVbusPolicyEbsKeepSourceForWindowsBringup)
    {
      Report->WindowsBringupRetain = TRUE;
      Report->EbsRationale = LiuqinVbusPolicyEbsKeepOnWindowsBringup;
    }
    else
    {
      Report->EbsRationale = LiuqinVbusPolicyEbsKeepOnOsOwnsVbus;
    }

    return;
  }

  LiuqinVbusPolicyGetEbsDecision (Policy, NULL, &Decision);
  Report->EbsRationale = Decision.Rationale;
  Report->KeepOnAtEbs = Decision.KeepOn;
  Report->WindowsBringupRetain = Decision.WindowsBringupRetain;
  Report->HandoffAllowed = Decision.HandoffAllowed;
  if (Policy->Config.EbsMode == LiuqinVbusPolicyEbsKeepSourceForWindowsBringup)
  {
    /* The fixed report must not claim "contract unproven" when the real EBS
     * request saw the contract but refused keep-on for a source/fault reason.
     * Re-derive with the report-only snapshot (no read/write is involved). */
    LIUQIN_VBUS_POLICY_EVIDENCE EbsEvidence;

    ClearBytes (&EbsEvidence, sizeof (EbsEvidence));
    EbsEvidence.WindowsBringupRetainVerified = Policy->WindowsBringupContractSeen;
    LiuqinVbusPolicyGetEbsDecision (Policy, &EbsEvidence, &Decision);
    Report->EbsRationale = Decision.Rationale;
    Report->WindowsBringupRetain = Decision.WindowsBringupRetain;
    Report->HandoffAllowed = Decision.HandoffAllowed;
  }
}

CONST CHAR8 *LiuqinVbusPolicyStateText (IN LIUQIN_VBUS_POLICY_STATE State)
{
  switch (State)
  {
  case LiuqinVbusPolicyStateDisabled:
    return "disabled";
  case LiuqinVbusPolicyStatePreflight:
    return "preflight";
  case LiuqinVbusPolicyStateDetached:
    return "detached/armed";
  case LiuqinVbusPolicyStateRoleAdopted:
    return "role adopted";
  case LiuqinVbusPolicyStateAttachDebounce:
    return "attach debounce";
  case LiuqinVbusPolicyStateMonitored:
    return "source monitored";
  case LiuqinVbusPolicyStateStopping:
    return "stopping";
  case LiuqinVbusPolicyStateStopped:
    return "stopped (clean)";
  case LiuqinVbusPolicyStateFaultStop:
    return "fault stop";
  case LiuqinVbusPolicyStateTerminal:
    return "terminal";
  case LiuqinVbusPolicyStateShutdownUnproven:
    return "shutdown unproven";
  case LiuqinVbusPolicyStateWaitExternalClear:
    return "wait external clear";
  default:
    return "unknown";
  }
}

CONST CHAR8 *LiuqinVbusPolicyStopText (IN LIUQIN_VBUS_POLICY_STOP Stop)
{
  switch (Stop)
  {
  case LiuqinVbusPolicyStopNone:
    return "none";
  case LiuqinVbusPolicyStopDisabled:
    return "disabled";
  case LiuqinVbusPolicyStopInvalidConfig:
    return "invalid config";
  case LiuqinVbusPolicyStopEvidence:
    return "evidence missing";
  case LiuqinVbusPolicyStopClock:
    return "clock regression";
  case LiuqinVbusPolicyStopObservationInvalid:
    return "observation invalid";
  case LiuqinVbusPolicyStopObservationStale:
    return "observation stale";
  case LiuqinVbusPolicyStopIdentity:
    return "identity mismatch";
  case LiuqinVbusPolicyStopOwner:
    return "owner mismatch";
  case LiuqinVbusPolicyStopLock:
    return "lock tuple mismatch";
  case LiuqinVbusPolicyStopManager:
    return "manager conflict";
  case LiuqinVbusPolicyStopInheritedSource:
    return "inherited source";
  case LiuqinVbusPolicyStopFault:
    return "pmic/typec fault";
  case LiuqinVbusPolicyStopOverCurrent:
    return "ocp loop";
  case LiuqinVbusPolicyStopExternalPower:
    return "external power";
  case LiuqinVbusPolicyStopPartnerUnsupported:
    return "unsupported partner";
  case LiuqinVbusPolicyStopRoleLost:
    return "role lost";
  case LiuqinVbusPolicyStopSourceLost:
    return "source dropped";
  case LiuqinVbusPolicyStopRoleWriteFailed:
    return "role write failed";
  case LiuqinVbusPolicyStopEnableFailed:
    return "enable failed";
  case LiuqinVbusPolicyStopActionFailure:
    return "action failure";
  case LiuqinVbusPolicyStopActionTimeout:
    return "action timeout";
  case LiuqinVbusPolicyStopAttachWindow:
    return "attach window expired";
  case LiuqinVbusPolicyStopSourceWindow:
    return "source window expired";
  case LiuqinVbusPolicyStopDetach:
    return "detach";
  case LiuqinVbusPolicyStopExitBootServices:
    return "exit boot services";
  case LiuqinVbusPolicyStopShutdownUnproven:
    return "shutdown unproven";
  default:
    return "unknown";
  }
}

CONST CHAR8 *LiuqinVbusPolicyActionText (IN LIUQIN_VBUS_POLICY_ACTION Action)
{
  switch (Action)
  {
  case LiuqinVbusPolicyActionNone:
    return "none";
  case LiuqinVbusPolicyActionWriteRole:
    return "write role 02->04";
  case LiuqinVbusPolicyActionEnableSource:
    return "enable source";
  case LiuqinVbusPolicyActionDisableSource:
    return "disable source";
  case LiuqinVbusPolicyActionVerifySourceOff:
    return "verify source off";
  case LiuqinVbusPolicyActionTriggerIndependentShutdown:
    return "independent shutdown";
  case LiuqinVbusPolicyActionRestoreRole:
    return "restore role 04->02";
  default:
    return "unknown";
  }
}

CONST CHAR8 *LiuqinVbusPolicyEbsRationaleText (IN LIUQIN_VBUS_POLICY_EBS_RATIONALE Rationale)
{
  switch (Rationale)
  {
  case LiuqinVbusPolicyEbsControlledOffDefault:
    return "controlled off (default)";
  case LiuqinVbusPolicyEbsControlledOffExternalPower:
    return "controlled off (external power)";
  case LiuqinVbusPolicyEbsControlledOffFault:
    return "controlled off (fault)";
  case LiuqinVbusPolicyEbsControlledOffNotEnabled:
    return "off (never enabled)";
  case LiuqinVbusPolicyEbsControlledOffShutdownUnproven:
    return "off (shutdown unproven)";
  case LiuqinVbusPolicyEbsControlledOffOsOwnerUnproven:
    return "controlled off (OS owner unproven)";
  case LiuqinVbusPolicyEbsControlledOffSourceOnUnverified:
    return "controlled off (source on unverified)";
  case LiuqinVbusPolicyEbsControlledOffManagerChanged:
    return "controlled off (manager changed)";
  case LiuqinVbusPolicyEbsKeepOnOsOwnsVbus:
    return "keep on (OS owns VBUS)";
  case LiuqinVbusPolicyEbsKeepOnWindowsBringup:
    return "keep on (Windows bring-up, no OS owner)";
  case LiuqinVbusPolicyEbsControlledOffWindowsBringupUnproven:
    return "controlled off (Windows bring-up contract unproven)";
  case LiuqinVbusPolicyEbsControlledOffWindowsBringupUnverified:
    return "controlled off (Windows bring-up source unverified)";
  case LiuqinVbusPolicyEbsWindowsBringupPending:
    return "Windows bring-up retain pending EBS";
  default:
    return "unknown";
  }
}
