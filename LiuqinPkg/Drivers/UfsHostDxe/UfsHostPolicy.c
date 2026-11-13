/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <UfsHostPolicy.h>
#include <IndustryStandard/UfsHci.h>

BOOLEAN
LiuqinUfsClocksLive (LIUQIN_UFS_READ32 Read32, VOID *Context, UINT32 Snapshot[6])
{
  STATIC CONST UINTN Offsets[6] = {0x87004, 0x87000, 0x87018, 0x870D4, 0x87020, 0x87064};
  UINTN Index;
  if (Read32 == NULL || Snapshot == NULL)
    return FALSE;
  for (Index = 0; Index < 6; Index++)
    Snapshot[Index] = 0xFFFFFFFE;
  Snapshot[0] = Read32 (Context, 0x00100000 + Offsets[0]);
  if (Snapshot[0] == MAX_UINT32 || (Snapshot[0] & BIT31) == 0)
    return FALSE;
  Snapshot[1] = Read32 (Context, 0x00100000 + Offsets[1]);
  if (Snapshot[1] == MAX_UINT32 || (Snapshot[1] & BIT0) != 0)
    return FALSE;
  for (Index = 2; Index < 6; Index++)
  {
    Snapshot[Index] = Read32 (Context, 0x00100000 + Offsets[Index]);
    if ((Snapshot[Index] & BIT31) != 0)
      return FALSE;
  }
  return TRUE;
}

BOOLEAN
LiuqinUfsQuiesce (LIUQIN_UFS_READ32 Read32, LIUQIN_UFS_WRITE32 Write32, LIUQIN_UFS_DELAY Delay,
                  VOID *Context)
{
  UINTN Attempt;
  if (Read32 == NULL || Write32 == NULL || Delay == NULL)
    return FALSE;
  Write32 (Context, 0x01D84000 + UFS_HC_IE_OFFSET, 0);
  Write32 (Context, 0x01D84000 + UFS_HC_UTRLRSR_OFFSET, 0);
  Write32 (Context, 0x01D84000 + UFS_HC_UTMRLRSR_OFFSET, 0);
  for (Attempt = 0; Attempt < 1000; Attempt++)
  {
    /* A posted/ignored IE write must not pass the ownership-transfer proof
       merely because the two queues are already idle. */
    if (Read32 (Context, 0x01D84000 + UFS_HC_IE_OFFSET) == 0 &&
        Read32 (Context, 0x01D84000 + UFS_HC_UTRLRSR_OFFSET) == 0 &&
        Read32 (Context, 0x01D84000 + UFS_HC_UTMRLRSR_OFFSET) == 0 &&
        Read32 (Context, 0x01D84000 + UFS_HC_UTRLDBR_OFFSET) == 0 &&
        Read32 (Context, 0x01D84000 + UFS_HC_UTMRLDBR_OFFSET) == 0)
      return TRUE;
    Delay (Context, 100);
  }
  return FALSE;
}

BOOLEAN
LiuqinUfsRange (UINT64 Base, UINT64 Size, UINT64 Address, UINT64 Length)
{
  return Length != 0 && Size != 0 && Size <= MAX_UINT64 - Base && Address >= Base &&
         Address - Base < Size && Length <= Size - (Address - Base);
}

BOOLEAN
LiuqinUfsRamRange (CONST LIUQIN_MEMORY_PLAN *Plan, UINT64 Address, UINT64 Length)
{
  UINTN Index;
  UINT64 Total = 0, End = 0;
  BOOLEAN Found = FALSE;
  if (Plan == NULL || Plan->Reason != LiuqinMemoryOk || Plan->Reserved != 0 || Plan->Count == 0 ||
      Plan->Count > LIUQIN_MEMORY_MAX_RANGES)
    return FALSE;
  for (Index = 0; Index < Plan->Count; Index++)
  {
    CONST LIUQIN_MEMORY_RANGE *Range = &Plan->Ranges[Index];
    if (Range->Size == 0 || (Range->Base & 0xFFF) != 0 || (Range->Size & 0xFFF) != 0 ||
        Range->Size > MAX_UINT64 - Range->Base || Range->Size > MAX_UINT64 - Total ||
        (Index != 0 && Range->Base <= End))
      return FALSE;
    End = Range->Base + Range->Size;
    Total += Range->Size;
    if (LiuqinUfsRange (Range->Base, Range->Size, Address, Length))
      Found = TRUE;
  }
  return Total == Plan->TotalBytes && Found;
}

STATIC BOOLEAN AllZero (CONST UINT8 *Bytes, UINTN Count)
{
  UINTN Index;

  for (Index = 0; Index < Count; ++Index)
  {
    if (Bytes[Index] != 0)
    {
      return FALSE;
    }
  }

  return TRUE;
}

BOOLEAN
LiuqinUfsReadOnlyUpiu (CONST UINT8 *P, UINTN Size, UINT8 Direction)
{
  UINT8 Opcode;
  if (P == NULL || Size < 32 || P[8] != 0 || P[10] != 0 || P[11] != 0)
  {
    return FALSE;
  }
  /* NOP OUT: no data, no LUN or extended transaction. */
  if (P[0] == 0)
  {
    return Direction == 0 && P[1] == 0 && P[2] == 0;
  }
  if (P[0] == 0x16)
  {
    if (P[1] != 0 || P[2] != 0 || P[4] != 0 || P[6] != 0 || P[7] != 0)
    {
      return FALSE;
    }
    Opcode = P[12];
    if (P[5] == 1 && (Opcode == 1 || Opcode == 3 || Opcode == 5))
    {
      /* READ DESCRIPTOR / READ ATTRIBUTE / READ FLAG, no outbound payload. */
      return Direction == 0 || Direction == 2;
    }
    /* Upstream initialization requires SET fDeviceInit. It is volatile, not
       a configuration-descriptor/attribute/medium write. No other SET is allowed. */
    return P[5] == 0x81 && Opcode == 6 && P[13] == 1 && P[14] == 0 && P[15] == 0 && P[18] == 0 &&
           P[19] == 0 && P[20] == 0 && P[21] == 0 && P[22] == 0 && P[23] == 0 &&
           (Direction == 0 || Direction == 1);
  }
  if (P[0] != 1 || P[4] != 0)
  {
    return FALSE; /* Includes task management and a nonzero command set type. */
  }
  if (P[2] == 0xD0U)
  {

    if (Direction != 0 || P[1] != 1 || P[12] != 0 || P[13] != 0 || P[14] != 0 || P[15] != 0)
    {
      return FALSE;
    }
    if (P[16] == 0x00)
    {
      return AllZero (P + 17, 15); /* TEST UNIT READY */
    }
    if (P[16] == 0x1B)
    {
      /* START STOP UNIT, POWER CONDITION = ACTIVE only (1 << 4 = 0x10). */
      return P[17] == 0 && P[18] == 0 && P[19] == 0 && P[20] == 0x10 && AllZero (P + 21, 11);
    }
    return FALSE;
  }
  if (P[2] > 7)
  {
    return FALSE; /* Includes all other well-known LUNs, RPMB and boot LUN. */
  }
  if (!((Direction == 0 && P[1] == 1 && P[12] == 0 && P[13] == 0 && P[14] == 0 && P[15] == 0) ||
        (Direction == 2 && P[1] == 0x41)))
  {
    return FALSE;
  }
  Opcode = P[16];
  if (Opcode == 0)
  {
    return Direction == 0; /* TEST UNIT READY */
  }
  if (Direction != 2)
  {
    return FALSE;
  }
  switch (Opcode)
  {
  case 0x03: /* REQUEST SENSE */
  case 0x08: /* READ(6) */
  case 0x12: /* INQUIRY */
  case 0x1A: /* MODE SENSE(6) */
  case 0x25: /* READ CAPACITY(10) */
  case 0x28: /* READ(10) */
  case 0x5A: /* MODE SENSE(10) */
  case 0x88: /* READ(16) */
  case 0xA0: /* REPORT LUNS */
    return TRUE;
  case 0x9E: /* SERVICE ACTION IN(16): READ CAPACITY only */
    return P[17] == 0x10;
  default:
    return FALSE; /* Includes writes, UNMAP, format, security and vendor CDBs. */
  }
}

BOOLEAN
LiuqinUfsStreamMatches (UINT32 Smr, UINT32 S2cr, UINT16 Sid, BOOLEAN Extended)
{
  UINT32 Mask;
  if (Extended ? ((S2cr & BIT10) == 0) : ((Smr & BIT31) == 0))
  {
    return FALSE;
  }
  Mask = (Smr >> 16) & (Extended ? 0xFFFFU : 0x7FFFU);
  return ((Sid ^ (Smr & 0xFFFFU)) & ~Mask) == 0;
}

UINTN
LiuqinUfsStreamCount (UINT32 Cr0, UINT32 Id0)
{
  UINTN Count;
  Count = Id0 & 0xFFU;

  if (Id0 == 0 || Id0 == MAX_UINT32 || Cr0 == MAX_UINT32 || (Cr0 & BIT0) != 0 ||
      (Id0 & BIT27) == 0 || (Id0 & (BIT30 | BIT29)) == 0 || Count == 0 ||
      Count > LIUQIN_UFS_SMMU_MAX_GROUPS || ((Cr0 & BIT3) != 0 && (Id0 & BIT8) == 0))
  {
    return 0;
  }
  return Count;
}

BOOLEAN
LiuqinUfsStreamBypass (UINT32 Cr0, UINT32 Id0, CONST UINT32 *Smr, CONST UINT32 *S2cr, UINTN Count,
                       UINT16 Sid)
{
  BOOLEAN Extended;
  UINTN Index, Matches;
  UINT32 Type;

  if (Count == 0 || Count != LiuqinUfsStreamCount (Cr0, Id0) || Smr == NULL || S2cr == NULL)
  {
    return FALSE;
  }
  Extended = (Cr0 & BIT3) != 0;
  if (Extended && (Id0 & BIT8) == 0)
  {
    return FALSE;
  }
  Matches = 0;
  Type = 0;
  for (Index = 0; Index < Count; Index++)
  {
    if (Smr[Index] == MAX_UINT32 || S2cr[Index] == MAX_UINT32)
    {
      return FALSE;
    }
    if (LiuqinUfsStreamMatches (Smr[Index], S2cr[Index], Sid, Extended))
    {
      Matches++;
      Type = (S2cr[Index] >> 16) & 3;
    }
  }
  if (Matches == 1)
  {
    return Type == 1; /* S2CR_TYPE_BYPASS; translation/fault rejected. */
  }
  return Matches == 0 && (Cr0 & BIT10) == 0; /* Unmatched stream bypass. */
}

/* ------------------------------------------------------------------ */
/* Bounded inherited-controller recovery. See UfsHostPolicy.h for the  */
/* Linux/Palima mechanism citations. No PHY, clock, gear, SSU or       */
/* medium access; every register is read back before it is trusted.    */

#define LIUQIN_UFS_HC_BASE 0x01D84000U
#define LIUQIN_UFS_UIC_DME_LINKSTARTUP 0x16U
#define LIUQIN_UFS_RECOVERY_POLL_US 100U

STATIC VOID RecoveryClear (LIUQIN_UFS_RECOVERY_REPORT *Report)
{
  UINT8 *Bytes = (UINT8 *)Report;
  UINTN Index;

  for (Index = 0; Index < sizeof (*Report); ++Index)
  {
    Bytes[Index] = 0;
  }
}

STATIC BOOLEAN RecoveryRead (LIUQIN_UFS_READ32 Read32, VOID *Context,
                             LIUQIN_UFS_RECOVERY_REPORT *Report, UINT32 Offset, UINT32 *Value)
{
  ++Report->Reads;
  *Value = Read32 (Context, (UINTN)(LIUQIN_UFS_HC_BASE + Offset));
  return *Value != MAX_UINT32;
}

STATIC VOID RecoveryWrite (LIUQIN_UFS_WRITE32 Write32, VOID *Context,
                           LIUQIN_UFS_RECOVERY_REPORT *Report, UINT32 Offset, UINT32 Value)
{
  ++Report->Writes;
  Write32 (Context, (UINTN)(LIUQIN_UFS_HC_BASE + Offset), Value);
}

STATIC VOID RecoveryDelay (LIUQIN_UFS_DELAY Delay, VOID *Context,
                           LIUQIN_UFS_RECOVERY_REPORT *Report)
{
  ++Report->Delays;
  Delay (Context, LIUQIN_UFS_RECOVERY_POLL_US);
}

/* Stop both lists and prove all three stop words plus both doorbells idle.
   All three writes are attempted; a stuck doorbell can never become a proof. */
STATIC BOOLEAN RecoveryStopLists (LIUQIN_UFS_READ32 Read32, LIUQIN_UFS_WRITE32 Write32,
                                  LIUQIN_UFS_DELAY Delay, VOID *Context,
                                  LIUQIN_UFS_RECOVERY_REPORT *Report)
{
  STATIC CONST UINT32 Offsets[5] = {UFS_HC_IE_OFFSET, UFS_HC_UTMRLRSR_OFFSET, UFS_HC_UTRLRSR_OFFSET,
                                    UFS_HC_UTRLDBR_OFFSET, UFS_HC_UTMRLDBR_OFFSET};
  UINT32 Poll;
  UINTN Index;

  RecoveryWrite (Write32, Context, Report, UFS_HC_IE_OFFSET, 0);
  RecoveryWrite (Write32, Context, Report, UFS_HC_UTMRLRSR_OFFSET, 0);
  RecoveryWrite (Write32, Context, Report, UFS_HC_UTRLRSR_OFFSET, 0);
  for (Poll = 0; Poll < LIUQIN_UFS_RECOVERY_MAX_POLLS; ++Poll)
  {
    BOOLEAN Idle = TRUE;
    UINT32 Value;

    ++Report->StopPolls;
    for (Index = 0; Index < ARRAY_SIZE (Offsets); ++Index)
    {
      if (!RecoveryRead (Read32, Context, Report, Offsets[Index], &Value))
      {
        return FALSE;
      }

      if (Value != 0)
      {
        Idle = FALSE;
      }
    }

    if (Idle)
    {
      return TRUE;
    }

    if (Poll + 1 < LIUQIN_UFS_RECOVERY_MAX_POLLS)
    {
      RecoveryDelay (Delay, Context, Report);
    }
  }

  return FALSE;
}

STATIC BOOLEAN RecoverySetHce (LIUQIN_UFS_READ32 Read32, LIUQIN_UFS_WRITE32 Write32,
                               LIUQIN_UFS_DELAY Delay, VOID *Context,
                               LIUQIN_UFS_RECOVERY_REPORT *Report, UINT32 Enable)
{
  UINT32 Poll;
  UINT32 Value = 0;

  RecoveryWrite (Write32, Context, Report, UFS_HC_ENABLE_OFFSET, Enable);
  for (Poll = 0; Poll < LIUQIN_UFS_RECOVERY_MAX_POLLS; ++Poll)
  {
    if (Enable == 0)
    {
      ++Report->HceStopPolls;
    }
    else
    {
      ++Report->HceStartPolls;
    }

    if (!RecoveryRead (Read32, Context, Report, UFS_HC_ENABLE_OFFSET, &Value))
    {
      return FALSE;
    }

    if ((Value & UFS_HC_HCE_EN) == Enable)
    {
      return TRUE;
    }

    if (Poll + 1 < LIUQIN_UFS_RECOVERY_MAX_POLLS)
    {
      RecoveryDelay (Delay, Context, Report);
    }
  }

  return FALSE;
}

/* One DME_LINKSTARTUP UIC command, mirroring the locked upstream sequence:
   clear a stale UCCS, program all three arguments, wait HCS.UCRDY, write the
   opcode, wait IS.UCCS, then require UIC result code zero in ARG2[7:0]. */
STATIC BOOLEAN RecoveryLinkStartup (LIUQIN_UFS_READ32 Read32, LIUQIN_UFS_WRITE32 Write32,
                                    LIUQIN_UFS_DELAY Delay, VOID *Context,
                                    LIUQIN_UFS_RECOVERY_REPORT *Report)
{
  UINT32 Poll;
  UINT32 Value = 0;
  UINT32 Arg2 = 0;
  UINT32 Arg3 = 0;

  ++Report->LinkStartupAttempts;
  if (!RecoveryRead (Read32, Context, Report, UFS_HC_IS_OFFSET, &Value))
  {
    return FALSE;
  }

  if ((Value & UFS_HC_IS_UCCS) != 0)
  {
    RecoveryWrite (Write32, Context, Report, UFS_HC_IS_OFFSET, Value);
  }

  RecoveryWrite (Write32, Context, Report, UFS_HC_UCMD_ARG1_OFFSET, 0);
  RecoveryWrite (Write32, Context, Report, UFS_HC_UCMD_ARG2_OFFSET, 0);
  RecoveryWrite (Write32, Context, Report, UFS_HC_UCMD_ARG3_OFFSET, 0);

  Value = 0;
  for (Poll = 0; Poll < LIUQIN_UFS_RECOVERY_MAX_POLLS; ++Poll)
  {
    if (!RecoveryRead (Read32, Context, Report, UFS_HC_STATUS_OFFSET, &Value))
    {
      return FALSE;
    }

    if ((Value & UFS_HC_HCS_UCRDY) != 0)
    {
      break;
    }

    if (Poll + 1 < LIUQIN_UFS_RECOVERY_MAX_POLLS)
    {
      RecoveryDelay (Delay, Context, Report);
    }
  }

  if ((Value & UFS_HC_HCS_UCRDY) == 0)
  {
    Report->Reason = LIUQIN_UFS_RECOVERY_UIC_READY_FAILED;
    return FALSE;
  }

  RecoveryWrite (Write32, Context, Report, UFS_HC_UIC_CMD_OFFSET, LIUQIN_UFS_UIC_DME_LINKSTARTUP);

  Value = 0;
  for (Poll = 0; Poll < LIUQIN_UFS_RECOVERY_MAX_POLLS; ++Poll)
  {
    if (!RecoveryRead (Read32, Context, Report, UFS_HC_IS_OFFSET, &Value))
    {
      return FALSE;
    }

    if ((Value & UFS_HC_IS_UCCS) != 0)
    {
      break;
    }

    if (Poll + 1 < LIUQIN_UFS_RECOVERY_MAX_POLLS)
    {
      RecoveryDelay (Delay, Context, Report);
    }
  }

  if ((Value & UFS_HC_IS_UCCS) == 0)
  {
    Report->Reason = LIUQIN_UFS_RECOVERY_LINK_STARTUP_FAILED;
    return FALSE;
  }

  if (!RecoveryRead (Read32, Context, Report, UFS_HC_UCMD_ARG2_OFFSET, &Arg2) ||
      !RecoveryRead (Read32, Context, Report, UFS_HC_UCMD_ARG3_OFFSET, &Arg3))
  {
    return FALSE;
  }

  Report->LinkStartupResult = Arg2 & 0xFFU;
  Report->UicArg3 = Arg3;
  if ((Arg2 & 0xFFU) != 0)
  {
    Report->Reason = LIUQIN_UFS_RECOVERY_LINK_STARTUP_FAILED;
    return FALSE;
  }

  return TRUE;
}

STATIC BOOLEAN RecoveryDevicePresent (LIUQIN_UFS_READ32 Read32, LIUQIN_UFS_DELAY Delay,
                                      VOID *Context, LIUQIN_UFS_RECOVERY_REPORT *Report)
{
  UINT32 Poll;
  UINT32 Value = 0;

  for (Poll = 0; Poll < LIUQIN_UFS_RECOVERY_MAX_POLLS; ++Poll)
  {
    ++Report->DevicePresentPolls;
    if (!RecoveryRead (Read32, Context, Report, UFS_HC_STATUS_OFFSET, &Value))
    {
      return FALSE;
    }

    if ((Value & UFS_HC_HCS_DP) != 0)
    {
      return TRUE;
    }

    if (Poll + 1 < LIUQIN_UFS_RECOVERY_MAX_POLLS)
    {
      RecoveryDelay (Delay, Context, Report);
    }
  }

  return FALSE;
}

BOOLEAN
LiuqinUfsRecoverInherited (LIUQIN_UFS_READ32 Read32, LIUQIN_UFS_WRITE32 Write32,
                           LIUQIN_UFS_DELAY Delay, VOID *Context,
                           LIUQIN_UFS_RECOVERY_REPORT *Report)
{
  UINT32 Attempt;
  UINT32 LastReason;
  UINTN Index;
  STATIC CONST UINT32 AfterOffsets[8] = {
      UFS_HC_ENABLE_OFFSET,  UFS_HC_STATUS_OFFSET,   UFS_HC_IS_OFFSET,      UFS_HC_IE_OFFSET,
      UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTMRLRSR_OFFSET, UFS_HC_UTRLDBR_OFFSET, UFS_HC_UTMRLDBR_OFFSET};
  UINT32 AfterValues[8];

  if (Report == NULL)
  {
    return FALSE;
  }

  RecoveryClear (Report);
  Report->Reason = LIUQIN_UFS_RECOVERY_BAD_ARGUMENTS;
  if (Read32 == NULL || Write32 == NULL || Delay == NULL)
  {
    return FALSE;
  }

  /* Preflight: only an inherited, idle, device-present controller is touched. */
  Report->Reason = LIUQIN_UFS_RECOVERY_NOT_INHERITED;
  if (!RecoveryRead (Read32, Context, Report, UFS_HC_ENABLE_OFFSET, &Report->HceBefore) ||
      !RecoveryRead (Read32, Context, Report, UFS_HC_STATUS_OFFSET, &Report->HcsBefore) ||
      !RecoveryRead (Read32, Context, Report, UFS_HC_IS_OFFSET, &Report->IsBefore) ||
      !RecoveryRead (Read32, Context, Report, UFS_HC_IE_OFFSET, &Report->IeBefore) ||
      !RecoveryRead (Read32, Context, Report, UFS_HC_UTRLRSR_OFFSET, &Report->UtrlRsrBefore) ||
      !RecoveryRead (Read32, Context, Report, UFS_HC_UTMRLRSR_OFFSET, &Report->UtmrlRsrBefore) ||
      !RecoveryRead (Read32, Context, Report, UFS_HC_UTRLDBR_OFFSET, &Report->UtrlDbrBefore) ||
      !RecoveryRead (Read32, Context, Report, UFS_HC_UTMRLDBR_OFFSET, &Report->UtmrlDbrBefore))
  {
    return FALSE;
  }

  if ((Report->HcsBefore & UFS_HC_HCS_DP) == 0 || Report->UtrlDbrBefore != 0 ||
      Report->UtmrlDbrBefore != 0)
  {
    return FALSE;
  }

  Report->Reason = LIUQIN_UFS_RECOVERY_STOP_FAILED;
  if (!RecoveryStopLists (Read32, Write32, Delay, Context, Report))
  {
    return FALSE;
  }

  Report->ListsStopped = TRUE;
  Report->Reason = LIUQIN_UFS_RECOVERY_HCE_STOP_FAILED;
  if (!RecoverySetHce (Read32, Write32, Delay, Context, Report, 0))
  {
    return FALSE;
  }

  Report->Reason = LIUQIN_UFS_RECOVERY_HCE_START_FAILED;
  if (!RecoverySetHce (Read32, Write32, Delay, Context, Report, UFS_HC_HCE_EN))
  {
    return FALSE;
  }

  Report->ControllerEnabled = TRUE;
  LastReason = LIUQIN_UFS_RECOVERY_LINK_STARTUP_FAILED;

  /* Linux repeats DME_LINKSTARTUP once when the device is not active; the
     second command is what moves an inherited device into active mode. */
  for (Attempt = 0; Attempt < 2; ++Attempt)
  {
    if (!RecoveryLinkStartup (Read32, Write32, Delay, Context, Report))
    {
      LastReason = Report->Reason;
      continue;
    }

    Report->LinkStarted = TRUE;
    if (RecoveryDevicePresent (Read32, Delay, Context, Report))
    {
      Report->DevicePresent = TRUE;
    }
  }

  if (!Report->LinkStarted)
  {
    /* Keep the specific UIC failure (not-ready vs. link startup) for diagnosis. */
    Report->Reason = LastReason;
    return FALSE;
  }

  if (!Report->DevicePresent)
  {
    Report->Reason = LIUQIN_UFS_RECOVERY_DEVICE_ABSENT;
    return FALSE;
  }

  if (!RecoveryRead (Read32, Context, Report, UFS_HC_UECPA_OFFSET, &Report->Uecpa))
  {
    return FALSE;
  }

  /* The upstream driver must program its own lists; leave them stopped and
     prove the exact exit state before returning ownership. */
  Report->Reason = LIUQIN_UFS_RECOVERY_EXIT_STATE_FAILED;
  for (Index = 0; Index < ARRAY_SIZE (AfterOffsets); ++Index)
  {
    if (!RecoveryRead (Read32, Context, Report, AfterOffsets[Index], &AfterValues[Index]))
    {
      return FALSE;
    }
  }

  Report->HceAfter = AfterValues[0];
  Report->HcsAfter = AfterValues[1];
  Report->IsAfter = AfterValues[2];
  Report->IeAfter = AfterValues[3];
  Report->UtrlRsrAfter = AfterValues[4];
  Report->UtmrlRsrAfter = AfterValues[5];
  Report->UtrlDbrAfter = AfterValues[6];
  Report->UtmrlDbrAfter = AfterValues[7];

  if ((Report->HceAfter & UFS_HC_HCE_EN) == 0 || (Report->HcsAfter & UFS_HC_HCS_DP) == 0 ||
      Report->IeAfter != 0 || Report->UtrlRsrAfter != 0 || Report->UtmrlRsrAfter != 0 ||
      Report->UtrlDbrAfter != 0 || Report->UtmrlDbrAfter != 0)
  {
    return FALSE;
  }

  Report->QuiescentAtExit = TRUE;
  Report->Reason = LIUQIN_UFS_RECOVERY_OK;
  return TRUE;
}

CONST char *LiuqinUfsRecoveryReasonName (UINT32 Reason)
{
  switch (Reason)
  {
  case LIUQIN_UFS_RECOVERY_OK:
    return "inherited-controller-recovered";
  case LIUQIN_UFS_RECOVERY_BAD_ARGUMENTS:
    return "bad-arguments";
  case LIUQIN_UFS_RECOVERY_NOT_INHERITED:
    return "inherited-controller-not-idle-or-absent";
  case LIUQIN_UFS_RECOVERY_STOP_FAILED:
    return "list-stop-readback-failed";
  case LIUQIN_UFS_RECOVERY_HCE_STOP_FAILED:
    return "hce-disable-readback-failed";
  case LIUQIN_UFS_RECOVERY_HCE_START_FAILED:
    return "hce-enable-readback-failed";
  case LIUQIN_UFS_RECOVERY_UIC_READY_FAILED:
    return "uic-not-ready";
  case LIUQIN_UFS_RECOVERY_LINK_STARTUP_FAILED:
    return "link-startup-failed";
  case LIUQIN_UFS_RECOVERY_DEVICE_ABSENT:
    return "device-not-present-after-link-startup";
  case LIUQIN_UFS_RECOVERY_EXIT_STATE_FAILED:
    return "exit-state-not-quiescent";
  default:
    return "unknown-recovery-status";
  }
}
