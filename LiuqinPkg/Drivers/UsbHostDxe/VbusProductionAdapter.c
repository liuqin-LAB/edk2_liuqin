/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "VbusProductionAdapter.h"
#include "VbusSnapshot.h"
#include "VbusMonitor.h"
#include <Library/BaseMemoryLib.h>

/* See VbusProductionAdapter.h for the evidence boundary. This file contains
 * no MMIO/SPMI/BootServices call: the reviewed BASE libraries and the injected
 * LIUQIN_VBUS_PRODUCTION_IO primitives are the only transports. */

#define LIUQIN_VBUS_PRODUCTION_DCDC_PPID_ADDRESS 0x2750U
#define LIUQIN_VBUS_PRODUCTION_ROLE_PPID_ADDRESS 0x2B44U

/* Baseline-stability masks: every non-target bit that must not change between
 * the boot baseline and a later write. Bits the enable sequence itself may
 * toggle (OTG_EN, EN_SRC_CFG, ENG bit0) are excluded. */
#define LIUQIN_VBUS_PRODUCTION_BST_EN_STABLE 0x1AU
#define LIUQIN_VBUS_PRODUCTION_BST_CFG_STABLE 0x72U
#define LIUQIN_VBUS_PRODUCTION_ENG_STABLE 0xFEU
#define LIUQIN_VBUS_PRODUCTION_ROLE_STABLE 0xF8U

/* Hard TypeC fault bits (VBUS_ERROR | VCONN_OVERCURRENT). */
#define LIUQIN_VBUS_PRODUCTION_TYPEC_FAULT 0x14U
/* POWER_PATH_STATUS bit7 candidate external-supply / reverse-feed indicator. */
#define LIUQIN_VBUS_PRODUCTION_POWER_PATH_EXT 0x80U
#define LIUQIN_VBUS_PRODUCTION_TYPEC_MISC_INDEX 5U

STATIC BOOLEAN ForwardCommandAllowed (LIUQIN_VBUS_PRODUCTION *Adapter, UINT64 *GuardStartedAt);
STATIC UINT64 ProductionNow (LIUQIN_VBUS_PRODUCTION *Adapter);
STATIC BOOLEAN GuardFailure (LIUQIN_VBUS_PRODUCTION *Adapter, LIUQIN_VBUS_FAILURE_STAGE Stage);

STATIC BOOLEAN ActionFailure (LIUQIN_VBUS_PRODUCTION *Adapter, LIUQIN_VBUS_FAILURE_STAGE Stage)
{
  if (Adapter->ActionDiagnostic.Stage == LiuqinVbusFailureNone)
  {
    Adapter->ActionDiagnostic.Stage = Stage;
  }
  return FALSE;
}

STATIC
BOOLEAN
ProductionRead32 (VOID *Context, UINTN Address, UINT32 *Value)
{
  LIUQIN_VBUS_PRODUCTION *Adapter = Context;

  if ((Adapter == NULL) || !Adapter->Initialized)
  {
    return FALSE;
  }

  return Adapter->Io.Read32 (Adapter->Io.Context, Address, Value);
}

STATIC
BOOLEAN
ProductionObserverWrite32 (VOID *Context, UINTN Address, UINT32 Value)
{
  LIUQIN_VBUS_PRODUCTION *Adapter = Context;

  if ((Adapter == NULL) || !Adapter->Initialized || (Adapter->Io.ObserverWrite32 == NULL))
  {
    return FALSE;
  }

  return Adapter->Io.ObserverWrite32 (Adapter->Io.Context, Address, Value);
}

STATIC
BOOLEAN
ProductionWrite32 (VOID *Context, UINTN Address, UINT32 Value)
{
  LIUQIN_VBUS_PRODUCTION *Adapter = Context;
  UINTN Offset;
  UINT32 ExpectedApid;
  UINT64 Now;
  UINT64 GuardStartedAt;

  if ((Adapter == NULL) || !Adapter->Initialized)
  {
    return FALSE;
  }

  if (!Adapter->WritePending || (Address < Adapter->WriteConfig->ChannelBase))
  {
    return FALSE;
  }

  Offset = (Address - Adapter->WriteConfig->ChannelBase) & 0xFFFU;
  ExpectedApid = ((Adapter->WriteAddress >> 8) == 0x2BU) ? Adapter->RoleApid : Adapter->DcdcApid;
  if ((Address - Adapter->WriteConfig->ChannelBase) / 0x1000U != ExpectedApid ||
      (Offset != 0U && Offset != 0x10U))
  {
    return ActionFailure (Adapter, LiuqinVbusFailurePublisher);
  }

  if (Offset == 0x10U)
  {
    if (Adapter->WriteDataStaged || Value != (UINT32)Adapter->WriteValue)
    {
      return ActionFailure (Adapter, LiuqinVbusFailurePublisher);
    }
    Adapter->WriteDataAddress = Address;
    Adapter->WriteDataValue = Value;
    Adapter->ActionDiagnostic.StagedData = Value;
  }
  else
  {
    Adapter->ActionDiagnostic.CommandAttempted = TRUE;
    if (Value != (((UINT32)Adapter->WriteAddress & 0xFFU) << 4) || !Adapter->WriteDataStaged ||
        Adapter->WriteDataAddress != Address + 0x10U ||
        Adapter->WriteDataValue != (UINT32)Adapter->WriteValue)
    {
      return ActionFailure (Adapter, LiuqinVbusFailurePublisher);
    }

    if (!Adapter->WriteCleanup && !ForwardCommandAllowed (Adapter, &GuardStartedAt))
    {
      return FALSE;
    }

    if (!Adapter->WriteCleanup)
    {

      Adapter->ActionDiagnostic.DataReissued = TRUE;
      ++Adapter->Writes;
      if (!Adapter->Io.Write32 (Adapter->Io.Context, Adapter->WriteDataAddress,
                                Adapter->WriteDataValue))
      {
        return ActionFailure (Adapter, LiuqinVbusFailureDataReissue);
      }
      Adapter->ActionDiagnostic.DataReissueAccepted = TRUE;
      Now = ProductionNow (Adapter);
      if (!Adapter->Policy.InFlight || Adapter->Policy.StopLatched || Now < GuardStartedAt ||
          Now < Adapter->Policy.ActionStartedAt ||
          Now - GuardStartedAt > Adapter->Config.MaximumObservationAgeUs ||
          Now - Adapter->Policy.ActionStartedAt >= Adapter->Config.ActionTimeoutUs)
      {
        return GuardFailure (Adapter, LiuqinVbusFailureGuardEndTime);
      }
    }
  }

  ++Adapter->Writes;
  if (Offset == 0U)
  {
    Adapter->ActionDiagnostic.CommandPublished = TRUE;
  }
  if (!Adapter->Io.Write32 (Adapter->Io.Context, Address, Value))
  {
    return ActionFailure (Adapter, LiuqinVbusFailurePublisher);
  }
  if (Offset == 0U)
  {
    Adapter->ActionDiagnostic.CommandSubmitted = TRUE;
  }
  else
  {
    Adapter->WriteDataStaged = TRUE;
  }
  return TRUE;
}

STATIC
VOID ProductionDelay (VOID *Context, UINT32 Microseconds)
{
  LIUQIN_VBUS_PRODUCTION *Adapter = Context;

  if ((Adapter == NULL) || (Adapter->Io.DelayUs == NULL))
  {
    return;
  }

  Adapter->Io.DelayUs (Adapter->Io.Context, Microseconds);
}

STATIC
UINT64
ProductionNow (LIUQIN_VBUS_PRODUCTION *Adapter)
{
  if ((Adapter == NULL) || (Adapter->Io.NowUs == NULL))
  {
    return 0U;
  }

  return Adapter->Io.NowUs (Adapter->Io.Context);
}

STATIC
BOOLEAN
ReadBytes (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT16 Address, IN UINT8 Length,
           OUT UINT8 *Buffer)
{
  LIUQIN_SPMI_READ_RESULT Result;

  if (!Adapter->Initialized || (Buffer == NULL) || (Length == 0U))
  {
    return FALSE;
  }

  ++Adapter->Reads;
  LiuqinSpmiRead (&Adapter->ReadIo, Adapter->ReadConfig, LIUQIN_VBUS_PRODUCTION_SLAVE, Address,
                  Length, &Result);
  if (!Result.Valid || (Result.Status != LiuqinSpmiReadComplete))
  {
    if (!Adapter->ActionDiagnostic.ReadFailed &&
        Adapter->ActionDiagnostic.Stage == LiuqinVbusFailureNone)
    {
      Adapter->ActionDiagnostic.ReadFailed = TRUE;
      Adapter->ActionDiagnostic.ReadAddress = Address;
      Adapter->ActionDiagnostic.ReadStatus = Result.Status;
    }
    return FALSE;
  }

  CopyMem (Buffer, Result.Data, Length);
  return TRUE;
}

STATIC
BOOLEAN
WriteByte (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT16 Address, IN UINT8 Value,
           IN UINT8 Expected, IN BOOLEAN Cleanup)
{
  LIUQIN_SPMI_WRITE_RESULT Result;
  UINT8 Buffer[1];
  LIUQIN_VBUS_PRODUCTION_FAILURE SavedDiagnostic;
  BOOLEAN PreserveDiagnostic;

  if (!Adapter->Initialized || Adapter->WritePending)
  {
    return FALSE;
  }

  PreserveDiagnostic = Adapter->ActionDiagnostic.Stage != LiuqinVbusFailureNone ||
                       Adapter->ActionDiagnostic.ReadFailed;
  CopyMem (&SavedDiagnostic, &Adapter->ActionDiagnostic, sizeof (SavedDiagnostic));

  Adapter->WritePending = TRUE;
  Adapter->WriteCleanup = Cleanup;
  Adapter->WriteAddress = Address;
  Adapter->WriteExpected = Expected;
  Adapter->WriteValue = Value;
  Adapter->WriteDataStaged = FALSE;
  Adapter->WriteDataAddress = 0U;
  Adapter->WriteDataValue = 0U;
  Adapter->ActionDiagnostic.StagedData = 0U;
  Adapter->ActionDiagnostic.DataReissued = FALSE;
  Adapter->ActionDiagnostic.DataReissueAccepted = FALSE;
  Adapter->ActionDiagnostic.Address = Address;
  Adapter->ActionDiagnostic.Expected = Expected;
  Adapter->ActionDiagnostic.Value = Value;
  Adapter->ActionDiagnostic.Current = Expected;
  Adapter->ActionDiagnostic.DataValid = LIUQIN_VBUS_DIAG_CURRENT;
  Adapter->ActionDiagnostic.CommandAttempted = FALSE;
  Adapter->ActionDiagnostic.CommandPublished = FALSE;
  Adapter->ActionDiagnostic.CommandSubmitted = FALSE;
  Adapter->ActionDiagnostic.GuardStarted = FALSE;
  Adapter->ActionDiagnostic.GuardElapsedUs = 0U;
  Buffer[0] = Value;
  LiuqinSpmiWrite (&Adapter->WriteIo, Adapter->WriteConfig, LIUQIN_VBUS_PRODUCTION_SLAVE, Address,
                   Buffer, 1U, &Result);
  Adapter->WritePending = FALSE;
  Adapter->ActionDiagnostic.WriteResultValid = TRUE;
  Adapter->ActionDiagnostic.WriteStatus = Result.Status;
  Adapter->ActionDiagnostic.ChannelStatus = Result.ChannelStatus;
  if (PreserveDiagnostic)
  {
    CopyMem (&Adapter->ActionDiagnostic, &SavedDiagnostic, sizeof (SavedDiagnostic));
  }
  return Result.Status == LiuqinSpmiWriteComplete
             ? TRUE
             : ActionFailure (Adapter, LiuqinVbusFailureTransport);
}

/* One masked byte update with owner/lock re-checks, baseline preservation and
 * a readback. The caller supplies the byte baseline captured at boot. */
STATIC
BOOLEAN
MaskedWrite (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT16 Address, IN UINT8 Mask,
             IN UINT8 Value, IN UINT8 Baseline, IN BOOLEAN Cleanup, OUT UINT8 *NewValue)
{
  UINT8 Current;
  UINT8 Updated;
  UINT8 Readback;
  LIUQIN_SPMI_WRITE_RESULT Resolve;
  UINT8 Lock[3];

  if (!ReadBytes (Adapter, Address, 1U, &Current))
  {
    return FALSE;
  }

  if ((Current & (UINT8)(~Mask)) != (Baseline & (UINT8)(~Mask)))
  {
    /* A baseline conflict refuses every new effect. The only exception is the
     * source-off cleanup write, which owns only its own mask and must still
     * remove a source this manager may have enabled (same reviewed rule as the
     * one-shot writer); the conflict is recorded elsewhere and never re-armed. */
    if (!Cleanup)
    {
      return FALSE;
    }
  }

  /* Re-resolve ownership and re-read the lock tuple immediately before the
   * write. Both are read-only metadata reads; a change fails closed. */
  LiuqinSpmiWriteResolve (&Adapter->WriteIo, Adapter->WriteConfig, LIUQIN_VBUS_PRODUCTION_SLAVE,
                          Address, &Resolve);
  if ((Resolve.Status != LiuqinSpmiWriteComplete) || !Resolve.ApidSelected)
  {
    return FALSE;
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_LOCK_D1_ADDRESS, 1U, &Lock[0]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_LOCK_D2_ADDRESS, 1U, &Lock[1]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_TYPEC_LOCK_D1, 1U, &Lock[2]))
  {
    return FALSE;
  }

  if ((Lock[0] != LIUQIN_VBUS_POLICY_LOCK_STOCK_D1) ||
      (Lock[1] != LIUQIN_VBUS_POLICY_LOCK_STOCK_D2) ||
      (Lock[2] != LIUQIN_VBUS_POLICY_LOCK_STOCK_TYPEC_D1))
  {
    return FALSE;
  }

  Updated = (UINT8)((Current & (UINT8)(~Mask)) | (Value & Mask));
  if (NewValue != NULL)
  {
    *NewValue = Updated;
  }

  if (Updated == Current)
  {
    return TRUE;
  }

  if (!WriteByte (Adapter, Address, Updated, Current, Cleanup))
  {
    return FALSE;
  }

  if (!ReadBytes (Adapter, Address, 1U, &Readback))
  {
    return FALSE;
  }

  return Readback == Updated;
}

STATIC
VOID RefreshOwnership (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter)
{
  LIUQIN_SPMI_WRITE_RESULT Dcdc;
  LIUQIN_SPMI_WRITE_RESULT Role;

  LiuqinSpmiWriteResolve (&Adapter->WriteIo, Adapter->WriteConfig, LIUQIN_VBUS_PRODUCTION_SLAVE,
                          LIUQIN_VBUS_PRODUCTION_DCDC_PPID_ADDRESS, &Dcdc);
  LiuqinSpmiWriteResolve (&Adapter->WriteIo, Adapter->WriteConfig, LIUQIN_VBUS_PRODUCTION_SLAVE,
                          LIUQIN_VBUS_PRODUCTION_ROLE_PPID_ADDRESS, &Role);

  Adapter->OwnerValid = (Dcdc.Status == LiuqinSpmiWriteComplete) && Dcdc.ApidSelected &&
                        (Role.Status == LiuqinSpmiWriteComplete) && Role.ApidSelected;
  if (!Adapter->OwnerValid)
  {
    Adapter->OwnerStable = FALSE;
    return;
  }

  if (!Adapter->OwnerCaptured)
  {
    Adapter->OwnerCaptured = TRUE;
    Adapter->DcdcApid = Dcdc.Apid;
    Adapter->DcdcMap = Dcdc.Map;
    Adapter->DcdcOwner = Dcdc.Owner;
    Adapter->RoleApid = Role.Apid;
    Adapter->RoleMap = Role.Map;
    Adapter->RoleOwner = Role.Owner;
    Adapter->OwnerStable = TRUE;
    return;
  }

  Adapter->OwnerStable = (Adapter->DcdcApid == Dcdc.Apid) && (Adapter->DcdcMap == Dcdc.Map) &&
                         (Adapter->DcdcOwner == Dcdc.Owner) && (Adapter->RoleApid == Role.Apid) &&
                         (Adapter->RoleMap == Role.Map) && (Adapter->RoleOwner == Role.Owner);
}

STATIC
BOOLEAN
CaptureBaseline (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter)
{
  UINT8 Value;

  if (Adapter->BaselineCaptured)
  {
    return TRUE;
  }

  /* Captured on the first complete observation. Each byte is re-read here and
   * the non-target bits are masked; the masked value is the stable baseline. */
  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS, 1U, &Adapter->BaselineBstEn) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_CFG_ADDRESS, 1U, &Adapter->BaselineBstCfg) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS, 1U,
                  &Adapter->BaselineEngCfg7) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, 1U, &Adapter->BaselineRole))
  {
    return FALSE;
  }

  Value = (UINT8)(Adapter->BaselineBstEn & LIUQIN_VBUS_PRODUCTION_BST_EN_STABLE);
  Adapter->BaselineBstEn = Value;
  Value = (UINT8)(Adapter->BaselineBstCfg & LIUQIN_VBUS_PRODUCTION_BST_CFG_STABLE);
  Adapter->BaselineBstCfg = Value;
  Value = (UINT8)(Adapter->BaselineEngCfg7 & LIUQIN_VBUS_PRODUCTION_ENG_STABLE);
  Adapter->BaselineEngCfg7 = Value;
  Value = (UINT8)(Adapter->BaselineRole & LIUQIN_VBUS_PRODUCTION_ROLE_STABLE);
  Adapter->BaselineRole = Value;
  Adapter->BaselineCaptured = TRUE;
  return TRUE;
}

STATIC
BOOLEAN
BaselineStable (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter)
{
  UINT8 Value;

  if (!Adapter->BaselineCaptured)
  {
    return FALSE;
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS, 1U, &Value) ||
      ((Value & LIUQIN_VBUS_PRODUCTION_BST_EN_STABLE) != Adapter->BaselineBstEn))
  {
    return FALSE;
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_CFG_ADDRESS, 1U, &Value) ||
      ((Value & LIUQIN_VBUS_PRODUCTION_BST_CFG_STABLE) != Adapter->BaselineBstCfg))
  {
    return FALSE;
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS, 1U, &Value) ||
      ((Value & LIUQIN_VBUS_PRODUCTION_ENG_STABLE) != Adapter->BaselineEngCfg7))
  {
    return FALSE;
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, 1U, &Value) ||
      ((Value & LIUQIN_VBUS_PRODUCTION_ROLE_STABLE) != Adapter->BaselineRole))
  {
    return FALSE;
  }

  return TRUE;
}

/* Fills the policy CC input from the reviewed decoder output. */
STATIC
VOID FillCc (OUT LIUQIN_VBUS_POLICY_CC *Cc, IN CONST LIUQIN_USB_CC_OBSERVATION *Decoded,
             IN CONST UINT8 *Typec)
{
  ZeroMem (Cc, sizeof (*Cc));
  Cc->Valid = Decoded->Valid;
  Cc->StateMachine = Decoded->StateMachine;
  Cc->Orientation = Decoded->Orientation;
  Cc->Attached = Decoded->Attached;
  Cc->Debounced = Decoded->Debounced;
  Cc->SourceMode = Decoded->SourceMode;
  Cc->VbusDetected = Decoded->VbusDetected;
  Cc->WaterDetected = Decoded->WaterDetected;
  Cc->FaultBits =
      (UINT8)(Typec[LIUQIN_VBUS_PRODUCTION_TYPEC_MISC_INDEX] & LIUQIN_VBUS_PRODUCTION_TYPEC_FAULT);
  Cc->UnknownBits = 0U;
  Cc->Partner = LiuqinVbusPolicyPartnerUnknown;

  switch (Decoded->State)
  {
  case LiuqinCcDetached:
    Cc->State = LiuqinVbusPolicyCcDetached;
    Cc->Partner = LiuqinVbusPolicyPartnerNone;
    break;
  case LiuqinCcAttachedSink:
    Cc->State = LiuqinVbusPolicyCcAttachedSink;
    Cc->Partner = (Typec[0] == 8U || Typec[0] == 4U || Typec[0] == 2U)
                      ? LiuqinVbusPolicyPartnerSourceRp
                      : LiuqinVbusPolicyPartnerUnknown;
    break;
  case LiuqinCcAttachedSource:
    Cc->State = LiuqinVbusPolicyCcAttachedSource;
    if ((Typec[2] & LIUQIN_VBUS_POLICY_PARTNER_MASK) == LIUQIN_VBUS_POLICY_PARTNER_RD)
    {
      Cc->Partner = LiuqinVbusPolicyPartnerSinkRd;
    }
    else if ((Typec[2] & LIUQIN_VBUS_POLICY_PARTNER_MASK) == LIUQIN_VBUS_POLICY_PARTNER_RD_RA)
    {
      Cc->Partner = LiuqinVbusPolicyPartnerSinkRdRa;
    }
    else if ((Typec[2] & LIUQIN_VBUS_POLICY_PARTNER_MASK) == LIUQIN_VBUS_POLICY_PARTNER_RA)
    {
      Cc->Partner = LiuqinVbusPolicyPartnerSinkRa;
    }

    break;
  case LiuqinCcTransition:
    Cc->State = LiuqinVbusPolicyCcTransition;
    break;
  case LiuqinCcAccessory:
    Cc->State = LiuqinVbusPolicyCcAccessory;
    break;
  case LiuqinCcFault:
    Cc->State = LiuqinVbusPolicyCcFault;
    break;
  case LiuqinCcInconsistent:
    Cc->State = LiuqinVbusPolicyCcInconsistent;
    break;
  case LiuqinCcUnknown:
    Cc->State = LiuqinVbusPolicyCcUnknown;
    break;
  default:
    Cc->State = LiuqinVbusPolicyCcInvalid;
    break;
  }
}

/* Conservative external-supply declaration. No measured external detector
 * exists on this board yet; this never enables against a local-sink partner and
 * treats the POWER_PATH bit7 candidate as Present (fail-safe direction). */
STATIC
LIUQIN_VBUS_POLICY_EXTERNAL
ClassifyExternal (IN CONST LIUQIN_VBUS_POLICY_CC *Cc, IN CONST UINT8 *SnapshotPowerPath)
{
  if ((*SnapshotPowerPath & LIUQIN_VBUS_PRODUCTION_POWER_PATH_EXT) != 0U)
  {
    return LiuqinVbusPolicyExternalPresent;
  }

  if (Cc->State == LiuqinVbusPolicyCcAttachedSink)
  {
    return LiuqinVbusPolicyExternalPresent;
  }

  if ((Cc->State == LiuqinVbusPolicyCcDetached) ||
      ((Cc->State == LiuqinVbusPolicyCcAttachedSource) &&
       LiuqinVbusPolicyPartnerHasRd (Cc->Partner)))
  {
    return LiuqinVbusPolicyExternalAbsent;
  }

  return LiuqinVbusPolicyExternalUnknown;
}

STATIC
BOOLEAN
Observe (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT64 Now)
{
  LIUQIN_USB_VBUS_SNAPSHOT Snapshot;
  LIUQIN_USB_CC_OBSERVATION Decoded;
  LIUQIN_VBUS_POLICY_OBSERVATION *Obs;
  UINT8 Extra[4];
  UINT8 IdentityOk;
  UINT8 Reserved;

  Obs = &Adapter->Observation;
  ZeroMem (Obs, sizeof (*Obs));
  Obs->CapturedAt = Now;

  LiuqinUsbCollectVbusSnapshot (&Adapter->ReadIo, Adapter->ReadConfig, &Snapshot);
  LiuqinUsbDecodeCc (&Snapshot, &Decoded);

  IdentityOk = Snapshot.Complete && Snapshot.IdentityValid && !Snapshot.IdentityRejected &&
                       (Snapshot.Attempted == LIUQIN_USB_VBUS_READS) &&
                       (Decoded.State != LiuqinCcUnsupported)
                   ? 1U
                   : 0U;

  /* Fixed extra tuple: ENG_CFG7, lock D1/D2 and TypeC lock D1. */
  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS, 1U, &Extra[0]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_LOCK_D1_ADDRESS, 1U, &Extra[1]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_LOCK_D2_ADDRESS, 1U, &Extra[2]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_TYPEC_LOCK_D1, 1U, &Extra[3]))
  {
    ++Adapter->ObservationFailures;
    Obs->Valid = FALSE;
    return FALSE;
  }

  if (IdentityOk == 0U)
  {
    ++Adapter->ObservationFailures;
    Obs->Valid = FALSE;
    return FALSE;
  }

  if (!CaptureBaseline (Adapter))
  {
    ++Adapter->ObservationFailures;
    Obs->Valid = FALSE;
    return FALSE;
  }

  RefreshOwnership (Adapter);

  FillCc (&Obs->Cc, &Decoded, Snapshot.Reads[9].Data);
  CopyMem (Adapter->CcRaw, Snapshot.Reads[9].Data, sizeof (Adapter->CcRaw));

  Obs->Cc.Valid = Decoded.Valid;
  Obs->Valid = TRUE;
  Obs->Pmic.Valid = TRUE;
  Obs->Pmic.IdentityValid = TRUE;
  Obs->Pmic.OwnerValid = Adapter->OwnerValid;
  Obs->Pmic.OwnerStable = Adapter->OwnerStable;
  Obs->Pmic.LockValid = TRUE;
  Obs->Pmic.LockD1 = Extra[1];
  Obs->Pmic.LockD2 = Extra[2];
  Obs->Pmic.TypecLockD1 = Extra[3];
  Obs->Pmic.LockValid = (Extra[1] == LIUQIN_VBUS_POLICY_LOCK_STOCK_D1) &&
                                (Extra[2] == LIUQIN_VBUS_POLICY_LOCK_STOCK_D2) &&
                                (Extra[3] == LIUQIN_VBUS_POLICY_LOCK_STOCK_TYPEC_D1)
                            ? TRUE
                            : FALSE;
  Obs->Pmic.BstEn = Snapshot.Reads[6].Data[0];
  Obs->Pmic.BstCfg = Snapshot.Reads[7].Data[0];
  Obs->Pmic.BstStatus = Snapshot.Reads[5].Data[2];
  Obs->Pmic.EngCfg7 = Extra[0];
  Obs->Pmic.PowerPath = Snapshot.Reads[5].Data[0];
  Obs->Pmic.AnalogLoop = Snapshot.Reads[5].Data[1];
  Obs->Pmic.BstFault = Snapshot.Reads[5].Data[3];
  Obs->Pmic.BstFail = Snapshot.Reads[5].Data[4];
  Obs->Pmic.RoleByte = Snapshot.Reads[8].Data[0];
  Obs->Pmic.UnknownBits = 0U;
  Obs->Pmic.ManagerConflict = FALSE;
  Obs->Pmic.BaselineValid = BaselineStable (Adapter);

  Reserved = (UINT8)(Obs->Pmic.BstEn & (UINT8)~LIUQIN_VBUS_POLICY_BST_EN_ALLOWED);
  Reserved = (UINT8)(Reserved | (Obs->Pmic.BstCfg & (UINT8)~LIUQIN_VBUS_POLICY_BST_CFG_ALLOWED));
  Reserved =
      (UINT8)(Reserved | (Obs->Pmic.BstStatus & (UINT8)~LIUQIN_VBUS_POLICY_BST_STATUS_ALLOWED));
  Reserved =
      (UINT8)(Reserved | (Obs->Pmic.BstFault & (UINT8)~LIUQIN_VBUS_POLICY_BST_FAULT_ALLOWED));
  Reserved = (UINT8)(Reserved | (Obs->Pmic.BstFail & (UINT8)~LIUQIN_VBUS_POLICY_BST_FAIL_ALLOWED));
  Obs->Pmic.UnknownBits = Reserved;

  Obs->ExternalPower = ClassifyExternal (&Obs->Cc, &Obs->Pmic.PowerPath);
  Obs->HostPathReady = Adapter->HostReady;
  Obs->PortVoltageValid = FALSE;
  Obs->PortSafeZero = FALSE;
  return TRUE;
}

STATIC BOOLEAN GuardFailure (LIUQIN_VBUS_PRODUCTION *Adapter, LIUQIN_VBUS_FAILURE_STAGE Stage)
{
  UINT64 End = ProductionNow (Adapter);
  Adapter->ActionDiagnostic.GuardElapsedUs = End >= Adapter->ActionDiagnostic.GuardStartedAt
                                                 ? End - Adapter->ActionDiagnostic.GuardStartedAt
                                                 : ~(UINT64)0;
  return ActionFailure (Adapter, Stage);
}

/* Called at the actual RW command boundary, AFTER SpmiWriteLib's potentially
 * slow full-table resolution and FIFO staging. Reads use the independent
 * observer address window; they issue no PMIC write. The staged RW data is
 * reasserted after this guard, since its retention across observer commands
 * is not a proven hardware contract.
 * Keep the production 500 ms frame / 1 s action limits, without borrowing the
 * isolated source session's APID cache or renewing an action's deadline. */
STATIC
BOOLEAN
ForwardCommandAllowed (LIUQIN_VBUS_PRODUCTION *Adapter, UINT64 *GuardStartedAt)
{
  LIUQIN_USB_VBUS_SNAPSHOT Snapshot;
  LIUQIN_USB_CC_OBSERVATION Decoded;
  LIUQIN_VBUS_POLICY_CC Cc;
  STATIC CONST UINT8 Compared[] = {0U, 1U, 2U, 3U, 5U};
  UINT8 Typec[6];
  UINT8 Power[5];
  UINT8 Lock[3];
  UINT8 Eng;
  UINT8 Current;
  UINTN Index;
  UINT64 Begin;
  UINT64 End;
  LIUQIN_VBUS_PRODUCTION_FAILURE *Diag = &Adapter->ActionDiagnostic;

  Begin = ProductionNow (Adapter);
  *GuardStartedAt = Begin;
  Diag->GuardStarted = TRUE;
  Diag->GuardStartedAt = Begin;
  if (!Adapter->Policy.InFlight || Adapter->Policy.StopLatched ||
      (Begin < Adapter->Policy.ActionStartedAt) ||
      (Begin - Adapter->Policy.ActionStartedAt >= Adapter->Config.ActionTimeoutUs))
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardStartTime);
  }

  LiuqinUsbCollectVbusSnapshot (&Adapter->ReadIo, Adapter->ReadConfig, &Snapshot);
  LiuqinUsbDecodeCc (&Snapshot, &Decoded);
  CopyMem (Diag->Typec, Snapshot.Reads[9].Data, sizeof (Diag->Typec));
  CopyMem (Diag->Power, Snapshot.Reads[5].Data, sizeof (Diag->Power));
  Diag->BstEn = Snapshot.Reads[6].Data[0];
  Diag->BstCfg = Snapshot.Reads[7].Data[0];
  Diag->Role = Snapshot.Reads[8].Data[0];
  if (Snapshot.Complete)
    Diag->DataValid |= LIUQIN_VBUS_DIAG_SNAPSHOT;
  if (!Decoded.Valid)
  {
    for (Index = 0U; Index < Snapshot.Attempted; ++Index)
    {
      if (!Snapshot.Reads[Index].Valid || Snapshot.Reads[Index].Status != LiuqinSpmiReadComplete)
      {
        Diag->ReadFailed = TRUE;
        Diag->ReadAddress = LiuqinUsbVbusAddress ((UINT32)Index);
        Diag->ReadStatus = Snapshot.Reads[Index].Status;
        break;
      }
    }
    return GuardFailure (Adapter, LiuqinVbusFailureGuardRead);
  }
  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS, 1U, &Eng) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_LOCK_D1_ADDRESS, 1U, &Lock[0]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_LOCK_D2_ADDRESS, 1U, &Lock[1]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_TYPEC_LOCK_D1, 1U, &Lock[2]))
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardRead);
  }
  Diag->Eng = Eng;
  CopyMem (Diag->Lock, Lock, sizeof (Lock));
  Diag->DataValid |= LIUQIN_VBUS_DIAG_EXTRA | LIUQIN_VBUS_DIAG_LOCK;
  if (!ReadBytes (Adapter, LiuqinUsbVbusAddress (5U), 5U, Power))
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardRead);
  }
  CopyMem (Diag->PowerRepeat, Power, sizeof (Power));
  Diag->DataValid |= LIUQIN_VBUS_DIAG_POWER;
  if (!ReadBytes (Adapter, LiuqinUsbVbusAddress (9U), 6U, Typec))
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardRead);
  }
  CopyMem (Diag->TypecRepeat, Typec, sizeof (Typec));
  Diag->DataValid |= LIUQIN_VBUS_DIAG_TYPEC;

  for (Index = 0U; Index < ARRAY_SIZE (Compared); ++Index)
  {
    UINT8 Mask = Compared[Index] == 5U ? 0x7FU : 0xFFU;
    if (((Snapshot.Reads[9].Data[Compared[Index]] ^ Typec[Compared[Index]]) & Mask) != 0U)
    {
      return GuardFailure (Adapter, LiuqinVbusFailureGuardCcChanged);
    }
  }

  if (((Power[0] | Snapshot.Reads[5].Data[0]) & LIUQIN_VBUS_PRODUCTION_POWER_PATH_EXT) != 0U ||
      ((Power[1] | Snapshot.Reads[5].Data[1]) & LIUQIN_VBUS_POLICY_ANALOG_LOOP_OCP) != 0U ||
      ((Power[2] | Snapshot.Reads[5].Data[2]) &
       (UINT8)(~LIUQIN_VBUS_POLICY_BST_STATUS_ALLOWED | LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART |
               LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB)) != 0U ||
      Power[3] != 0U || Power[4] != 0U || Decoded.Fault != 0U || Decoded.Fail != 0U ||
      (Typec[5] & LIUQIN_VBUS_PRODUCTION_TYPEC_FAULT) != 0U ||
      (Snapshot.Reads[6].Data[0] & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) != 0U ||
      (Snapshot.Reads[7].Data[0] & LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC) != 0U)
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardPower);
  }
  if ((Lock[0] != LIUQIN_VBUS_POLICY_LOCK_STOCK_D1) ||
      (Lock[1] != LIUQIN_VBUS_POLICY_LOCK_STOCK_D2) ||
      (Lock[2] != LIUQIN_VBUS_POLICY_LOCK_STOCK_TYPEC_D1))
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardLock);
  }
  if (((Snapshot.Reads[6].Data[0] & LIUQIN_VBUS_PRODUCTION_BST_EN_STABLE) !=
       Adapter->BaselineBstEn) ||
      ((Snapshot.Reads[7].Data[0] & LIUQIN_VBUS_PRODUCTION_BST_CFG_STABLE) !=
       Adapter->BaselineBstCfg) ||
      ((Snapshot.Reads[8].Data[0] & LIUQIN_VBUS_PRODUCTION_ROLE_STABLE) != Adapter->BaselineRole) ||
      ((Eng & LIUQIN_VBUS_PRODUCTION_ENG_STABLE) != Adapter->BaselineEngCfg7))
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardBaseline);
  }

  FillCc (&Cc, &Decoded, Typec);
  if (Cc.VbusDetected || ClassifyExternal (&Cc, Power) != LiuqinVbusPolicyExternalAbsent)
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardExternal);
  }

  if (Adapter->LastAction == LiuqinVbusPolicyActionWriteRole)
  {
    if ((Cc.State != LiuqinVbusPolicyCcDetached) ||
        (Adapter->WriteAddress != LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS) ||
        ((Adapter->WriteValue & LIUQIN_VBUS_POLICY_ROLE_MASK) !=
         LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY) ||
        ((Snapshot.Reads[8].Data[0] & LIUQIN_VBUS_POLICY_ROLE_MASK) !=
         LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY))
    {
      return GuardFailure (Adapter, LiuqinVbusFailureGuardTarget);
    }
    Current = Snapshot.Reads[8].Data[0];
  }
  else if (Adapter->LastAction == LiuqinVbusPolicyActionEnableSource)
  {
    if (!Adapter->HostReady || Cc.State != LiuqinVbusPolicyCcAttachedSource ||
        !LiuqinVbusPolicyPartnerHasRd (Cc.Partner) || !Cc.Attached || !Cc.Debounced ||
        !Cc.SourceMode || Cc.Orientation != Adapter->Observation.Cc.Orientation ||
        ((Snapshot.Reads[8].Data[0] & LIUQIN_VBUS_POLICY_ROLE_MASK) !=
         LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY))
    {
      return GuardFailure (Adapter, LiuqinVbusFailureGuardTarget);
    }
    switch (Adapter->WriteAddress)
    {
    case LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS:
      Current = Eng;
      break;
    case LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS:
      if ((Eng & LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF) == 0U)
      {
        return GuardFailure (Adapter, LiuqinVbusFailureGuardTarget);
      }
      Current = Snapshot.Reads[6].Data[0];
      break;
    case LIUQIN_VBUS_PRODUCTION_BST_CFG_ADDRESS:
      Current = Snapshot.Reads[7].Data[0];
      break;
    default:
      return GuardFailure (Adapter, LiuqinVbusFailureGuardTarget);
    }
  }
  else
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardTarget);
  }

  if (Current != Adapter->WriteExpected)
  {
    return GuardFailure (Adapter, LiuqinVbusFailureGuardTarget);
  }

  /* Observer transactions consumed time after the writer's owner check.
   * Re-resolve both channels and compare with this boot's captured owners;
   * never adopt a new mapping here. This remains metadata-only. */
  RefreshOwnership (Adapter);
  End = ProductionNow (Adapter);
  Diag->GuardElapsedUs = End >= Begin ? End - Begin : ~(UINT64)0;
  if (!Adapter->OwnerValid || !Adapter->OwnerStable)
  {
    return ActionFailure (Adapter, LiuqinVbusFailureGuardOwner);
  }
  if (End < Begin || End < Adapter->Policy.ActionStartedAt ||
      End - Begin > Adapter->Config.MaximumObservationAgeUs ||
      End - Adapter->Policy.ActionStartedAt >= Adapter->Config.ActionTimeoutUs)
  {
    return ActionFailure (Adapter, LiuqinVbusFailureGuardEndTime);
  }
  return TRUE;
}

STATIC
VOID BuildEvidence (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter)
{
  LIUQIN_VBUS_POLICY_EVIDENCE *Evidence = &Adapter->Evidence;

  ZeroMem (Evidence, sizeof (*Evidence));
  /* Build-profile adjudications accepted by the PCD/ACK pair. The dynamic
   * identity/owner/lock/CC results are carried in the observation, so the
   * policy can report the *specific* gate that failed instead of a generic
   * "evidence missing"; every one of them still fails closed. */
  Evidence->IdentityVerified = TRUE;
  Evidence->OwnerVerified = TRUE;
  Evidence->LockPolicyVerified = TRUE;
  Evidence->RouteVerified = TRUE;
  Evidence->ExternalInterlockVerified = TRUE;
  Evidence->ProtectionReportingVerified = TRUE;
  /* No independently proved shutdown route and no proved OS VBUS owner on
   * this firmware; these stay FALSE so ShutdownUnproven cannot be bypassed
   * and an OS-owned keep-on can never be selected. The Windows/OOBE bring-up
   * profile is a distinct, explicit exception: it adjudicates source retention
   * without claiming OS ownership, and is captured once at Initialize from the
   * single liuqin Windows bring-up configuration. */
  Evidence->IndependentShutdownVerified = FALSE;
  Evidence->OsVbusOwnerVerified = FALSE;
  Evidence->WindowsBringupRetainVerified = Adapter->WindowsBringupEnabled;
}

STATIC
UINT32
ExecuteEnable (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT64 Deadline)
{
  UINT8 BstEn;
  UINT8 BstCfg;
  UINT8 BstStatus;
  UINT8 EngCfg7;
  UINT8 Role;
  UINT8 Fault;
  UINT8 Fail;
  UINT8 Value;
  UINT32 Polls;
  UINT64 Now;

  /* Quiescent, identity and role preconditions are re-read from the device;
   * the observation that authorized this action is not trusted as a write
   * permit. */
  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS, 1U, &BstEn) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_CFG_ADDRESS, 1U, &BstCfg) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_STS_ADDRESS, 1U, &BstStatus) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS, 1U, &EngCfg7) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, 1U, &Role) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_FAULT_ADDRESS, 1U, &Fault) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_FAIL_ADDRESS, 1U, &Fail))
  {
    return 0U;
  }

  if ((Adapter->Observation.Pmic.UnknownBits != 0U) ||
      ((Role & LIUQIN_VBUS_POLICY_ROLE_MASK) != LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY) ||
      ((BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) != 0U) ||
      ((BstCfg & LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC) != 0U) ||
      ((BstStatus &
        (LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART | LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB)) != 0U) ||
      ((EngCfg7 & LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF) != 0U) || (Fault != 0U) || (Fail != 0U))
  {
    return 0U;
  }

  if (!MaskedWrite (Adapter, LIUQIN_VBUS_PRODUCTION_BST_CFG_ADDRESS,
                    LIUQIN_VBUS_POLICY_BST_CFG_EN_SRC, 0U, Adapter->BaselineBstCfg, FALSE, &Value))
  {
    return 0U;
  }

  if (!MaskedWrite (Adapter, LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS,
                    LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF, LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF,
                    Adapter->BaselineEngCfg7, FALSE, &Value))
  {
    return 0U;
  }

  if (!MaskedWrite (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS,
                    LIUQIN_VBUS_POLICY_BST_EN_OTG_EN, LIUQIN_VBUS_POLICY_BST_EN_OTG_EN,
                    Adapter->BaselineBstEn, FALSE, &Value))
  {
    return 0U;
  }

  /* Softstart poll. Sleep-first as in the reference sequence, bounded by the
   * action deadline and the fixed poll count. */
  for (Polls = 0U; Polls < LIUQIN_VBUS_PRODUCTION_SOFTSTART_POLLS; ++Polls)
  {
    Now = ProductionNow (Adapter);
    if (Now >= Deadline)
    {
      break;
    }

    ProductionDelay (Adapter, LIUQIN_VBUS_PRODUCTION_SOFTSTART_DELAY_US);
    if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_STS_ADDRESS, 1U, &BstStatus))
    {
      break;
    }

    if ((BstStatus & LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART) != 0U)
    {
      break;
    }
  }

  /* Always clear our engineering bit, even if softstart was not observed. */
  if (!MaskedWrite (Adapter, LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS,
                    LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF, 0U, Adapter->BaselineEngCfg7, TRUE,
                    &Value))
  {
    return 0U;
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS, 1U, &BstEn) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_STS_ADDRESS, 1U, &BstStatus) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_FAULT_ADDRESS, 1U, &Fault) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_FAIL_ADDRESS, 1U, &Fail) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, 1U, &Role))
  {
    return 0U;
  }

  if (((BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) == 0U) ||
      ((BstStatus & LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB) == 0U) ||
      ((BstStatus & LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART) == 0U) || (Fault != 0U) ||
      (Fail != 0U) ||
      ((Role & LIUQIN_VBUS_POLICY_ROLE_MASK) != LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY))
  {
    return 0U;
  }

  return LIUQIN_VBUS_POLICY_FACT_SOURCE_ON;
}

STATIC
BOOLEAN
ExecuteDisable (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter)
{
  BOOLEAN Ok;
  UINT8 BstEn;
  UINT8 Value;

  Ok = TRUE;
  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS, 1U, &BstEn))
  {
    return FALSE;
  }

  if ((BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) != 0U)
  {
    if (!MaskedWrite (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS,
                      LIUQIN_VBUS_POLICY_BST_EN_OTG_EN, 0U, Adapter->BaselineBstEn, TRUE, &Value))
    {
      Ok = FALSE;
      (VOID) ActionFailure (Adapter, LiuqinVbusFailureAction);
    }
  }

  /* Our engineering bit is cleared as cleanup; never the enable-source mode. */
  if (!MaskedWrite (Adapter, LIUQIN_VBUS_PRODUCTION_ENG_CFG7_ADDRESS,
                    LIUQIN_VBUS_POLICY_ENG_CFG7_OTG_FPF, 0U, Adapter->BaselineEngCfg7, TRUE,
                    &Value))
  {
    Ok = FALSE;
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS, 1U, &BstEn))
  {
    return FALSE;
  }

  return Ok && ((BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) == 0U);
}

STATIC
UINT32
ExecuteVerifyOff (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter)
{
  UINT8 BstEn;
  UINT8 Status;

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_EN_ADDRESS, 1U, &BstEn) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_BST_STS_ADDRESS, 1U, &Status))
  {
    return 0U;
  }

  if (((BstEn & LIUQIN_VBUS_POLICY_BST_EN_OTG_EN) == 0U) &&
      ((Status &
        (LIUQIN_VBUS_POLICY_BST_STATUS_SOFTSTART | LIUQIN_VBUS_POLICY_BST_STATUS_ON_USB)) == 0U))
  {
    return LIUQIN_VBUS_POLICY_FACT_SOURCE_OFF;
  }

  return 0U;
}

STATIC
BOOLEAN
RecheckRoleWriteContext (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter)
{
  LIUQIN_SPMI_WRITE_RESULT Resolve;
  UINT8 Lock[3];

  RefreshOwnership (Adapter);
  if (!Adapter->OwnerValid || !Adapter->OwnerStable)
  {
    return ActionFailure (Adapter, LiuqinVbusFailureRoleOwner);
  }

  LiuqinSpmiWriteResolve (&Adapter->WriteIo, Adapter->WriteConfig, LIUQIN_VBUS_PRODUCTION_SLAVE,
                          LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, &Resolve);
  if ((Resolve.Status != LiuqinSpmiWriteComplete) || !Resolve.ApidSelected ||
      (Resolve.Apid != Adapter->RoleApid))
  {
    return ActionFailure (Adapter, LiuqinVbusFailureRoleOwner);
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_LOCK_D1_ADDRESS, 1U, &Lock[0]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_LOCK_D2_ADDRESS, 1U, &Lock[1]) ||
      !ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_TYPEC_LOCK_D1, 1U, &Lock[2]))
  {
    return FALSE;
  }

  CopyMem (Adapter->ActionDiagnostic.Lock, Lock, sizeof (Lock));
  Adapter->ActionDiagnostic.DataValid |= LIUQIN_VBUS_DIAG_LOCK;
  return (Lock[0] == LIUQIN_VBUS_POLICY_LOCK_STOCK_D1) &&
                 (Lock[1] == LIUQIN_VBUS_POLICY_LOCK_STOCK_D2) &&
                 (Lock[2] == LIUQIN_VBUS_POLICY_LOCK_STOCK_TYPEC_D1)
             ? TRUE
             : ActionFailure (Adapter, LiuqinVbusFailureRoleLock);
}

STATIC
BOOLEAN
WriteRole (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT8 ExpectedMaskValue,
           IN UINT8 NewMaskValue)
{
  UINT8 Current;
  UINT8 Updated;
  UINT8 Readback;
  LIUQIN_VBUS_PRODUCTION_FAILURE *Diag = &Adapter->ActionDiagnostic;

  Diag->Address = LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS;
  Diag->Expected = ExpectedMaskValue;
  Diag->Value = NewMaskValue;

  if (!RecheckRoleWriteContext (Adapter))
  {
    return ActionFailure (Adapter, LiuqinVbusFailureRoleContext);
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, 1U, &Current))
  {
    return ActionFailure (Adapter, LiuqinVbusFailureRoleRead);
  }
  Diag->Current = Current;
  Diag->DataValid |= LIUQIN_VBUS_DIAG_CURRENT;

  if ((Current & (UINT8)(~LIUQIN_VBUS_POLICY_ROLE_MASK)) !=
      (Adapter->BaselineRole & (UINT8)(~LIUQIN_VBUS_POLICY_ROLE_MASK)))
  {
    return ActionFailure (Adapter, LiuqinVbusFailureRoleBaseline);
  }

  /* A refused forward command may leave the inherited sink byte untouched.
   * Cleanup may prove that existing state without another write. This is
   * deliberately limited to restoring sink, after owner/lock/baseline checks,
   * and requires a second complete byte read; it never retries a forward
   * effect or accepts an unknown/currently different role. */
  if ((NewMaskValue == LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY) &&
      ((Current & LIUQIN_VBUS_POLICY_ROLE_MASK) == NewMaskValue))
  {
    if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, 1U, &Readback))
    {
      return ActionFailure (Adapter, LiuqinVbusFailureReadbackRead);
    }
    Diag->Readback = Readback;
    Diag->DataValid |= LIUQIN_VBUS_DIAG_READBACK;
    return Readback == Current ? TRUE : ActionFailure (Adapter, LiuqinVbusFailureReadbackMismatch);
  }

  if ((Current & LIUQIN_VBUS_POLICY_ROLE_MASK) != ExpectedMaskValue)
  {
    return ActionFailure (Adapter, LiuqinVbusFailureRoleExpected);
  }

  Updated = (UINT8)((Current & (UINT8)(~LIUQIN_VBUS_POLICY_ROLE_MASK)) | NewMaskValue);
  if (!WriteByte (Adapter, LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, Updated, Current,
                  NewMaskValue == LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY))
  {
    return FALSE;
  }

  if (!ReadBytes (Adapter, LIUQIN_VBUS_PRODUCTION_ROLE_ADDRESS, 1U, &Readback))
  {
    return ActionFailure (Adapter, LiuqinVbusFailureReadbackRead);
  }

  Diag->Readback = Readback;
  Diag->DataValid |= LIUQIN_VBUS_DIAG_READBACK;
  return Readback == Updated ? TRUE : ActionFailure (Adapter, LiuqinVbusFailureReadbackMismatch);
}

STATIC
VOID Execute (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN CONST LIUQIN_VBUS_POLICY_TICKET *Ticket,
              OUT LIUQIN_VBUS_POLICY_ACTION_RESULT *Result)
{
  UINT32 Facts;

  ZeroMem (Result, sizeof (*Result));
  Result->Outcome = LiuqinVbusPolicyActionFailed;
  Adapter->LastAction = Ticket->Action;
  ZeroMem (&Adapter->ActionDiagnostic, sizeof (Adapter->ActionDiagnostic));
  Adapter->ActionDiagnostic.Action = Ticket->Action;

  switch (Ticket->Action)
  {
  case LiuqinVbusPolicyActionWriteRole:
    if (WriteRole (Adapter, LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY, LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY))
    {
      Result->Outcome = LiuqinVbusPolicyActionSucceeded;
      Result->Facts = LIUQIN_VBUS_POLICY_FACT_ROLE_APPLIED;
    }

    break;
  case LiuqinVbusPolicyActionRestoreRole:
    if (WriteRole (Adapter, LIUQIN_VBUS_POLICY_ROLE_SOURCE_ONLY, LIUQIN_VBUS_POLICY_ROLE_SINK_ONLY))
    {
      Result->Outcome = LiuqinVbusPolicyActionSucceeded;
      Result->Facts = LIUQIN_VBUS_POLICY_FACT_ROLE_RESTORED;
    }

    break;
  case LiuqinVbusPolicyActionEnableSource:
    Facts = ExecuteEnable (Adapter, Adapter->Policy.ActionStartedAt + (UINT64)Ticket->TimeoutUs);
    if (Facts == LIUQIN_VBUS_POLICY_FACT_SOURCE_ON)
    {
      Result->Outcome = LiuqinVbusPolicyActionSucceeded;
      Result->Facts = Facts;
    }
    else
    {
      /* A partial sequence may have changed the enable path: report an
       * indeterminate outcome so the policy's journaled stop path runs. */
      Result->Outcome = LiuqinVbusPolicyActionIndeterminate;
    }

    break;
  case LiuqinVbusPolicyActionDisableSource:
    if (ExecuteDisable (Adapter))
    {
      Result->Outcome = LiuqinVbusPolicyActionSucceeded;
      Result->Facts = 0U;
    }

    break;
  case LiuqinVbusPolicyActionVerifySourceOff:
    Facts = ExecuteVerifyOff (Adapter);
    if (Facts == LIUQIN_VBUS_POLICY_FACT_SOURCE_OFF)
    {
      Result->Outcome = LiuqinVbusPolicyActionSucceeded;
      Result->Facts = Facts;
    }

    break;
  case LiuqinVbusPolicyActionTriggerIndependentShutdown:
    /* No independently proved shutdown route exists for this board. */
    Result->Outcome = LiuqinVbusPolicyActionFailed;
    Result->Facts = 0U;
    break;
  default:
    break;
  }

  Adapter->LastOutcome = Result->Outcome;
}

/* Freeze after policy completion: an adapter success can still miss the
 * action deadline. Cleanup and later observations must not replace this
 * first cause with a secondary restore/disable failure. */
STATIC VOID CaptureActionFailure (LIUQIN_VBUS_PRODUCTION *Adapter,
                                  CONST LIUQIN_VBUS_POLICY_TICKET *Ticket,
                                  CONST LIUQIN_VBUS_POLICY_ACTION_RESULT *Result, UINT64 Now)
{
  if (Adapter->FirstFailure.Valid ||
      (!Adapter->Policy.ActionTimedOut && !Adapter->Policy.ClockFailed &&
       Result->Outcome == LiuqinVbusPolicyActionSucceeded &&
       Result->Facts == LiuqinVbusPolicyRequiredFacts (Ticket->Action)))
    return;

  if (Adapter->ActionDiagnostic.Stage == LiuqinVbusFailureNone)
  {
    Adapter->ActionDiagnostic.Stage =
        Adapter->Policy.ClockFailed
            ? LiuqinVbusFailureActionClock
            : (Adapter->Policy.ActionTimedOut ? LiuqinVbusFailureActionTimeout
                                              : LiuqinVbusFailureAction);
  }
  Adapter->ActionDiagnostic.ActionElapsedUs =
      Now >= Adapter->Policy.ActionStartedAt ? Now - Adapter->Policy.ActionStartedAt : ~(UINT64)0;
  Adapter->ActionDiagnostic.Valid = TRUE;
  CopyMem (&Adapter->FirstFailure, &Adapter->ActionDiagnostic, sizeof (Adapter->FirstFailure));
}

static inline BOOLEAN PolicyEnabled (CONST LIUQIN_VBUS_PRODUCTION *Adapter)
{
  return (Adapter != NULL) && Adapter->Initialized && Adapter->Config.Enabled && !Adapter->EbsDone;
}

VOID VbusProductionAdapterInitialize (OUT LIUQIN_VBUS_PRODUCTION *Adapter,
                                      IN CONST LIUQIN_VBUS_PRODUCTION_IO *Io,
                                      IN CONST LIUQIN_SPMI_READ_CONFIG *ReadConfig,
                                      IN CONST LIUQIN_SPMI_WRITE_CONFIG *WriteConfig,
                                      IN BOOLEAN ProductionAck, IN BOOLEAN WindowsBringup,
                                      IN UINT64 Now)
{
  if (Adapter == NULL || Io == NULL || ReadConfig == NULL || WriteConfig == NULL ||
      Io->Read32 == NULL || Io->ObserverWrite32 == NULL || Io->Write32 == NULL ||
      Io->NowUs == NULL || Io->DelayUs == NULL)
  {
    return;
  }

  ZeroMem (Adapter, sizeof (*Adapter));
  Adapter->Io = *Io;
  Adapter->ReadConfig = ReadConfig;
  Adapter->WriteConfig = WriteConfig;
  Adapter->ReadIo.Context = Adapter;
  Adapter->ReadIo.Read32 = ProductionRead32;
  Adapter->ReadIo.Write32 = ProductionObserverWrite32;
  Adapter->ReadIo.DelayUs = ProductionDelay;
  Adapter->ReadIo.Progress = NULL;
  Adapter->WriteIo.Context = Adapter;
  Adapter->WriteIo.Read32 = ProductionRead32;
  Adapter->WriteIo.Write32 = ProductionWrite32;
  Adapter->WriteIo.DelayUs = ProductionDelay;
  Adapter->WriteIo.Progress = NULL;

  Adapter->Config.Enabled = ProductionAck;
  Adapter->Config.AllowReattach = TRUE;
  Adapter->Config.AllowExternalReattach = TRUE;
  Adapter->Config.ActionTimeoutUs = 1000000U;
  Adapter->Config.MaximumObservationAgeUs = 500000U;
  Adapter->Config.AttachDebounceSamples = 3U;
  Adapter->Config.AttachWindowUs = 0U;
  Adapter->Config.MaximumSourceOnUs = 0U;
  Adapter->Config.OcpDebounceSamples = 2U;
  Adapter->Config.StopReadbackChecks = 2U;
  Adapter->Config.LockMode = LiuqinVbusPolicyLockStockRuntimeOwned;
  /* The fail-closed controlled-off default is unchanged for every build that
   * does not explicitly select the Windows/OOBE bring-up profile. The
   * explicit profile is the only source of the keep-source mode, and it is
   * still gated by the production ACK and by the policy's EBS health checks. */
  Adapter->WindowsBringupEnabled = ProductionAck && WindowsBringup;
  Adapter->Config.EbsMode = Adapter->WindowsBringupEnabled
                                ? LiuqinVbusPolicyEbsKeepSourceForWindowsBringup
                                : LiuqinVbusPolicyEbsControlledOff;
  Adapter->Config.TrustPowerPathBit7Candidate = FALSE;
  Adapter->Initialized = TRUE;
  LiuqinVbusPolicyInitialize (&Adapter->Policy, &Adapter->Config, Now);
}

VOID VbusProductionAdapterSetHostReady (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter,
                                        IN BOOLEAN HostReady)
{
  if (Adapter != NULL)
  {
    Adapter->HostReady = HostReady;
  }
}

BOOLEAN
VbusProductionAdapterTick (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT64 Now)
{
  LIUQIN_VBUS_POLICY_TICKET Ticket;
  LIUQIN_VBUS_POLICY_ACTION_RESULT Result;
  BOOLEAN Acted;
  UINT32 Actions;

  if (!PolicyEnabled (Adapter))
  {
    return FALSE;
  }

  ++Adapter->Ticks;
  (VOID) Observe (Adapter, Now);
  BuildEvidence (Adapter);

  Acted = FALSE;
  for (Actions = 0U; Actions < LIUQIN_VBUS_PRODUCTION_MAX_ACTIONS_PER_TICK; ++Actions)
  {
    Now = ProductionNow (Adapter);
    if (!LiuqinVbusPolicyNext (&Adapter->Policy, &Adapter->Evidence, &Adapter->Observation, Now,
                               &Ticket))
    {
      break;
    }

    Now = ProductionNow (Adapter);
    if (!LiuqinVbusPolicyBegin (&Adapter->Policy, &Ticket, &Adapter->Evidence,
                                &Adapter->Observation, Now))
    {
      break;
    }

    ++Adapter->ActionCalls;
    Execute (Adapter, &Ticket, &Result);
    if ((Result.Outcome != LiuqinVbusPolicyActionSucceeded) ||
        ((Result.Facts != 0U) && (Result.Facts != LiuqinVbusPolicyRequiredFacts (Ticket.Action))))
    {
      ++Adapter->FailedActions;
    }

    Now = ProductionNow (Adapter);
    (VOID) LiuqinVbusPolicyComplete (&Adapter->Policy, &Ticket, &Result, Now);
    CaptureActionFailure (Adapter, &Ticket, &Result, Now);
    Acted = TRUE;

    /* Normal boot performs at most one action per tick: the next decision
     * needs a fresh observation. Latched cleanup does not trust observations
     * and may run its bounded action list now. */
    if (!Adapter->Policy.StopLatched)
    {
      break;
    }
  }

  return Acted;
}

BOOLEAN
VbusProductionAdapterEbs (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT64 Now)
{
  LIUQIN_VBUS_POLICY_TICKET Ticket;
  LIUQIN_VBUS_POLICY_ACTION_RESULT Result;
  LIUQIN_VBUS_POLICY_EBS_DECISION Decision;
  UINT32 Actions;

  if ((Adapter == NULL) || !Adapter->Initialized)
  {
    return TRUE;
  }

  BuildEvidence (Adapter);
  LiuqinVbusPolicyRequestExitBootServices (&Adapter->Policy, &Adapter->Evidence, Now);

  for (Actions = 0U; Actions < LIUQIN_VBUS_PRODUCTION_MAX_EBS_ACTIONS; ++Actions)
  {
    if (LiuqinVbusPolicyIsTerminal (&Adapter->Policy))
    {
      break;
    }

    Now = ProductionNow (Adapter);
    if (!LiuqinVbusPolicyNext (&Adapter->Policy, &Adapter->Evidence, &Adapter->Observation, Now,
                               &Ticket))
    {
      break;
    }

    Now = ProductionNow (Adapter);
    if (!LiuqinVbusPolicyBegin (&Adapter->Policy, &Ticket, &Adapter->Evidence,
                                &Adapter->Observation, Now))
    {
      break;
    }

    ++Adapter->ActionCalls;
    Execute (Adapter, &Ticket, &Result);
    if ((Result.Outcome != LiuqinVbusPolicyActionSucceeded) ||
        ((Result.Facts != 0U) && (Result.Facts != LiuqinVbusPolicyRequiredFacts (Ticket.Action))))
    {
      ++Adapter->FailedActions;
    }

    Now = ProductionNow (Adapter);
    (VOID) LiuqinVbusPolicyComplete (&Adapter->Policy, &Ticket, &Result, Now);
    CaptureActionFailure (Adapter, &Ticket, &Result, Now);
  }

  LiuqinVbusPolicyGetEbsDecision (&Adapter->Policy, &Adapter->Evidence, &Decision);
  Adapter->EbsDone = TRUE;
  return Decision.HandoffAllowed;
}

VOID VbusProductionAdapterReport (IN CONST LIUQIN_VBUS_PRODUCTION *Adapter,
                                  OUT LIUQIN_VBUS_PRODUCTION_REPORT *Report)
{
  LIUQIN_VBUS_POLICY_REPORT PolicyReport;
  UINT32 Index;

  if (Report == NULL)
  {
    return;
  }

  ZeroMem (Report, sizeof (*Report));
  if (Adapter == NULL)
  {
    return;
  }

  LiuqinVbusPolicyGetReport (&Adapter->Policy, &PolicyReport);
  Report->State = PolicyReport.State;
  Report->Stop = PolicyReport.Stop;
  Report->EbsRationale = PolicyReport.EbsRationale;
  Report->RoleAdopted = PolicyReport.RoleAdopted;
  Report->RoleRestored = PolicyReport.RoleRestored;
  Report->RoleRestoreUnproven = PolicyReport.RoleRestoreUnproven;
  Report->SourceOnVerified = PolicyReport.SourceOnVerified;
  Report->SourceOffVerified = PolicyReport.SourceOffVerified;
  Report->ShutdownUnproven = PolicyReport.ShutdownUnproven;
  Report->EbsRequested = PolicyReport.EbsRequested;
  Report->HandoffAllowed = PolicyReport.HandoffAllowed;
  Report->KeepOnAtEbs = PolicyReport.KeepOnAtEbs;
  Report->WindowsBringupRetain = PolicyReport.WindowsBringupRetain;
  Report->RoleWrites = PolicyReport.Counters.RoleWrites;
  Report->EnableAttempts = PolicyReport.Counters.EnableAttempts;
  Report->EnableVerified = PolicyReport.Counters.EnableVerified;
  Report->StopVerified = PolicyReport.Counters.StopVerified;
  Report->FaultEvents = PolicyReport.Counters.FaultEvents;
  Report->ExternalEvents = PolicyReport.Counters.ExternalEvents;
  Report->UnknownBitEvents = PolicyReport.Counters.UnknownBitEvents;
  Report->ActionFailures = PolicyReport.Counters.ActionFailures + Adapter->FailedActions;
  Report->Ticks = Adapter->Ticks;
  Report->ObservationFailures = Adapter->ObservationFailures;
  Report->Writes = Adapter->Writes;
  Report->Reads = Adapter->Reads;
  Report->ObservationValid = Adapter->Observation.Valid;
  Report->LockValid = Adapter->Observation.Pmic.LockValid;
  Report->OwnerValid = Adapter->Observation.Pmic.OwnerValid;
  Report->OwnerStable = Adapter->Observation.Pmic.OwnerStable;
  Report->BaselineValid = Adapter->Observation.Pmic.BaselineValid;
  Report->HostPathReady = Adapter->Observation.HostPathReady;
  Report->CcState = (UINT8)Adapter->Observation.Cc.State;
  Report->VbusDetected = Adapter->Observation.Cc.VbusDetected;
  Report->CcPartner = (UINT8)Adapter->Observation.Cc.Partner;
  for (Index = 0U; Index < 6U; ++Index)
  {
    Report->CcRaw[Index] = Adapter->CcRaw[Index];
  }
  Report->ExternalPower = (UINT8)Adapter->Observation.ExternalPower;
  Report->PmicRole = Adapter->Observation.Pmic.RoleByte;
  Report->PowerPath = Adapter->Observation.Pmic.PowerPath;
  Report->BstEn = Adapter->Observation.Pmic.BstEn;
  Report->BstCfg = Adapter->Observation.Pmic.BstCfg;
  Report->BstStatus = Adapter->Observation.Pmic.BstStatus;
  Report->BstFault = Adapter->Observation.Pmic.BstFault;
  Report->BstFail = Adapter->Observation.Pmic.BstFail;
  Report->AnalogLoop = Adapter->Observation.Pmic.AnalogLoop;
  Report->LockD1 = Adapter->Observation.Pmic.LockD1;
  Report->LockD2 = Adapter->Observation.Pmic.LockD2;
  Report->TypecLockD1 = Adapter->Observation.Pmic.TypecLockD1;
  Report->EngCfg7 = Adapter->Observation.Pmic.EngCfg7;
  CopyMem (&Report->FirstFailure, &Adapter->FirstFailure, sizeof (Report->FirstFailure));
}

CONST CHAR8 *VbusProductionFailureText (LIUQIN_VBUS_FAILURE_STAGE Stage)
{
  STATIC CONST CHAR8 *CONST Names[] = {
      "none",          "role-context",      "role-read",        "role-expected",  "role-baseline",
      "readback-read", "readback-mismatch", "guard-start-time", "guard-read",     "guard-cc-change",
      "guard-power",   "guard-lock",        "guard-baseline",   "guard-external", "guard-target",
      "guard-owner",   "guard-end-time",    "transport",        "publisher",      "action-timeout",
      "action",        "role-owner",        "role-lock",        "action-clock",   "data-reissue"};
  return (UINT32)Stage < ARRAY_SIZE (Names) ? Names[Stage] : "unknown";
}

BOOLEAN
VbusProductionAdapterChannels (IN CONST LIUQIN_VBUS_PRODUCTION *Adapter, OUT UINT32 *DcdcApid,
                               OUT UINT32 *RoleApid)
{
  if ((Adapter == NULL) || !Adapter->Initialized || !Adapter->OwnerValid || !Adapter->OwnerStable)
  {
    return FALSE;
  }

  if (DcdcApid != NULL)
  {
    *DcdcApid = Adapter->DcdcApid;
  }

  if (RoleApid != NULL)
  {
    *RoleApid = Adapter->RoleApid;
  }

  return TRUE;
}

STATIC
BOOLEAN
WarmupTerminalState (IN LIUQIN_VBUS_POLICY_STATE State)
{
  return (State == LiuqinVbusPolicyStateStopped) || (State == LiuqinVbusPolicyStateFaultStop) ||
         (State == LiuqinVbusPolicyStateTerminal) || (State == LiuqinVbusPolicyStateDisabled) ||
         (State == LiuqinVbusPolicyStateShutdownUnproven);
}

/* True only for a complete debounced Rd/Rd-Ra attach: the same condition the
 * reviewed policy requires before it may plan EnableSource. Read-only. */
STATIC
BOOLEAN
WarmupAttachSeen (IN CONST LIUQIN_VBUS_POLICY_OBSERVATION *Observation)
{
  return Observation->Valid && Observation->Cc.Valid &&
         (Observation->Cc.State == LiuqinVbusPolicyCcAttachedSource) &&
         LiuqinVbusPolicyPartnerHasRd (Observation->Cc.Partner) && Observation->Cc.Attached &&
         Observation->Cc.Debounced;
}

/* Stop synchronous warmup when another tick cannot advance initialization. */
STATIC
BOOLEAN
WarmupSettled (IN CONST LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT32 IdleTicks)
{
  if (Adapter->Policy.SourceOnVerified || Adapter->Policy.StopLatched ||
      WarmupTerminalState (Adapter->Policy.State))
  {
    return TRUE;
  }

  /* A declared external supply blocks both forward gates before the role
   * write, and only a later observation can clear it: nothing is left for the
   * synchronous phase to do. The periodic timer keeps watching. */
  if ((Adapter->Policy.State == LiuqinVbusPolicyStatePreflight) && Adapter->Observation.Valid &&
      (Adapter->Observation.ExternalPower != LiuqinVbusPolicyExternalAbsent))
  {
    return TRUE;
  }

  /* Source role adopted but nothing plugged in: the synchronous phase cannot
   * enable anything, and the periodic timer keeps watching for hotplug. */
  return Adapter->Policy.RoleAdopted && (IdleTicks >= LIUQIN_VBUS_PRODUCTION_WARMUP_IDLE_TICKS) &&
         !WarmupAttachSeen (&Adapter->Observation);
}

UINT32
VbusProductionAdapterWarmup (IN OUT LIUQIN_VBUS_PRODUCTION *Adapter, IN UINT32 MaxTicks,
                             IN UINT32 GapUs)
{
  UINT32 Ticks;
  UINT32 IdleTicks;
  UINT64 StartedAt;

  if (!PolicyEnabled (Adapter) || (MaxTicks == 0U))
  {
    return 0U;
  }

  StartedAt = ProductionNow (Adapter);
  IdleTicks = 0U;
  for (Ticks = 0U; Ticks < MaxTicks; ++Ticks)
  {
    if (WarmupSettled (Adapter, IdleTicks))
    {
      break;
    }

    if ((Ticks != 0U) && (GapUs != 0U))
    {
      ProductionDelay (Adapter, GapUs);
    }

    (VOID) VbusProductionAdapterTick (Adapter, ProductionNow (Adapter));

    if (WarmupAttachSeen (&Adapter->Observation))
    {
      IdleTicks = 0U;
    }
    else if (Adapter->Policy.RoleAdopted)
    {
      ++IdleTicks;
    }
    else
    {
      IdleTicks = 0U;
    }

    /* A tick can run the reviewed softstart poll, so bound the whole burst by
     * wall clock as well as by MaxTicks. */
    if ((ProductionNow (Adapter) - StartedAt) >= (UINT64)LIUQIN_VBUS_PRODUCTION_WARMUP_LIMIT_US)
    {
      ++Ticks;
      break;
    }
  }

  return Ticks;
}
