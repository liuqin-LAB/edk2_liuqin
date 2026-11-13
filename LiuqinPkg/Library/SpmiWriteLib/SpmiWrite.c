/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <SpmiRead.h>
#include <SpmiWrite.h>

#define SPMI_WRITE_STATUS_DONE 0x01U
#define SPMI_WRITE_STATUS_FAILURE 0x02U
#define SPMI_WRITE_STATUS_DENIED 0x04U
#define SPMI_WRITE_STATUS_DROPPED 0x08U
#define SPMI_WRITE_STATUS_WDT_RST 0x10U
#define SPMI_WRITE_STATUS_RESERVED 0x1FU
#define SPMI_WRITE_OPCODE 0x00000000U
#define SPMI_WRITE_OPCODE_SHIFT 27U
#define SPMI_WRITE_PRIORITY_SHIFT 26U
#define SPMI_WRITE_ADDRESS_SHIFT 4U
#define SPMI_WRITE_STATUS_OFFSET 0x08U
#define SPMI_WRITE_DATA_OFFSET 0x10U
#define SPMI_WRITE_COMMAND_OFFSET 0x00U
#define SPMI_WRITE_CHANNEL_STRIDE 0x1000U

STATIC
VOID ClearResult (LIUQIN_SPMI_WRITE_RESULT *Result)
{
  UINTN Index;
  UINT8 *Bytes = (UINT8 *)Result;
  for (Index = 0U; Index < sizeof (*Result); ++Index)
  {
    Bytes[Index] = 0U;
  }
}

STATIC
BOOLEAN
RegisterAddress (UINTN Base, UINTN Size, UINTN Offset, UINTN *Address,
                 LIUQIN_SPMI_WRITE_RESULT *Result)
{
  if ((Offset & 3U) != 0U || Size < sizeof (UINT32) || Offset > Size - sizeof (UINT32) ||
      Base > MAX_UINTN - Offset)
  {
    Result->Status = LiuqinSpmiWriteResource;
    return FALSE;
  }
  *Address = Base + Offset;
  return TRUE;
}

STATIC
BOOLEAN
ReadRegister (CONST LIUQIN_SPMI_WRITE_IO *Io, UINTN Address, LIUQIN_SPMI_WRITE_STAGE Stage,
              BOOLEAN RejectAllOnes, UINT32 *Value, LIUQIN_SPMI_WRITE_RESULT *Result)
{
  UINT32 Raw = 0U;
  if (Result->ReadCount >= LIUQIN_SPMI_WRITE_MAX_READS)
  {
    Result->Status = LiuqinSpmiWriteBudget;
    return FALSE;
  }
  Result->LastAddress = Address;
  if (Io->Progress != NULL)
  {
    Io->Progress (Io->Context, Stage, Address);
  }
  ++Result->ReadCount;
  if (!Io->Read32 (Io->Context, Address, &Raw))
  {
    Result->Status = LiuqinSpmiWriteUnreadable;
    return FALSE;
  }
  *Value = Raw;
  if (RejectAllOnes && Raw == MAX_UINT32)
  {
    Result->Status = LiuqinSpmiWriteUnreadable;
    return FALSE;
  }
  return TRUE;
}

STATIC
BOOLEAN
ReadWindow (CONST LIUQIN_SPMI_WRITE_IO *Io, UINTN Base, UINTN Size, UINTN Offset,
            LIUQIN_SPMI_WRITE_STAGE Stage, UINT32 *Value, LIUQIN_SPMI_WRITE_RESULT *Result)
{
  UINTN Address;
  return RegisterAddress (Base, Size, Offset, &Address, Result) &&
         ReadRegister (Io, Address, Stage, TRUE, Value, Result);
}

/* Resolve PPID -> policy-selected EE-owned APID. Performs no channel and no command
 * access. Steps mirror the read path's metadata scan, but a mapping owned by
 * another EE is never accepted here. */
STATIC
BOOLEAN
ResolveChannel (CONST LIUQIN_SPMI_WRITE_IO *Io, CONST LIUQIN_SPMI_WRITE_CONFIG *Config, UINT8 Slave,
                UINT16 Address, LIUQIN_SPMI_WRITE_RESULT *Result)
{
  UINT32 Major, Maximum, MapOffset, OwnerOffset, Map, Owner, Index, Ppid;
  BOOLEAN CacheValidated;
  LIUQIN_SPMI_READ_CACHE_ENTRY *CacheEntry;

  Result->MappingPolicy = Config->MappingPolicy;
  if (Config->MappingPolicy != LIUQIN_SPMI_WRITE_SELECT_UNIQUE &&
      Config->MappingPolicy != LIUQIN_SPMI_WRITE_SELECT_HIGHEST_EE_V7)
  {
    Result->Status = LiuqinSpmiWriteInvalidParameter;
    return FALSE;
  }
  if (!Config->DtValid || !Config->MmioValid || Config->Bus != 0U || Config->Ee != 0U)
  {
    Result->Status = LiuqinSpmiWritePrerequisite;
    return FALSE;
  }
  if (Config->CoreBase != 0x0C400000U || Config->CoreSize == 0U || Config->CoreSize > 0x3000U ||
      Config->CnfgBase != 0x0C42D000U || Config->CnfgSize == 0U || Config->CnfgSize > 0x4000U ||
      Config->ChannelBase != LIUQIN_SPMI_WRITE_CHANNEL_BASE || Config->ChannelSize == 0U ||
      Config->ChannelSize > LIUQIN_SPMI_WRITE_CHANNEL_SIZE)
  {
    Result->Status = LiuqinSpmiWriteResource;
    return FALSE;
  }
  if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize, 0U, LiuqinSpmiWriteMetadata,
                   &Result->Version, Result))
  {
    return FALSE;
  }
  Major = Result->Version >> 28;
  /* Only the V7 layout is implemented. The V5/V6 channel stride and owner
   * offsets differ, so no length-based guess is made here. */
  if (Major != 7U)
  {
    Result->Status = LiuqinSpmiWriteUnsupported;
    return FALSE;
  }
  Maximum = 1024U;
  MapOffset = 0x2000U;
  OwnerOffset = 0U;
  if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize, 4U, LiuqinSpmiWriteMetadata,
                   &Result->Features, Result))
  {
    return FALSE;
  }
  Result->ApidCount = Result->Features & 0x7FFU;
  if (Result->ApidCount == 0U || Result->ApidCount > Maximum)
  {
    Result->Status = LiuqinSpmiWriteUnsupported;
    return FALSE;
  }
  Ppid = ((UINT32)Slave << 8) | ((UINT32)Address >> 8);
  /* Opt-in only: Config->Cache == NULL keeps the exact full-scan behaviour.
   * A validated hit skips only the repeated table scan. The version/features
   * words were just re-read, the slot's mapping policy must match, the selected
   * APID's current map and full owner words are re-read and must equal the
   * cached pair, and the owner must still be the configured EE. Any difference
   * invalidates the slot and falls back to the existing full scan, which may
   * select a new mapping or refuse. The live recheck below still runs. */
  CacheValidated = FALSE;
  if (Config->Cache != NULL)
  {
    CacheEntry = &Config->Cache->Entry[Ppid % LIUQIN_SPMI_READ_CACHE_ENTRIES];
    if (CacheEntry->Valid && (UINT32)CacheEntry->Ppid == Ppid)
    {
      if (CacheEntry->MappingPolicy != Config->MappingPolicy ||
          CacheEntry->Version != Result->Version || CacheEntry->Features != Result->Features ||
          CacheEntry->Apid >= Result->ApidCount)
      {
        CacheEntry->Valid = FALSE;
      }
      else if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize,
                            (UINTN)MapOffset + 4U * (UINTN)CacheEntry->Apid, LiuqinSpmiWriteMapping,
                            &Map, Result) ||
               !ReadWindow (Io, Config->CnfgBase, Config->CnfgSize,
                            (UINTN)OwnerOffset + 4U * (UINTN)CacheEntry->Apid,
                            LiuqinSpmiWriteMapping, &Owner, Result))
      {
        return FALSE;
      }
      else if (Map == CacheEntry->Map && Owner == CacheEntry->Owner &&
               (Owner & 0x0FU) == Config->Ee)
      {
        Result->Apid = CacheEntry->Apid;
        Result->Map = Map;
        Result->Owner = Owner;
        Result->ApidSelected = TRUE;
        Result->CacheHit = TRUE;
        CacheValidated = TRUE;
      }
      else
      {
        /* A changed map/owner or a mapping that is no longer EE-owned must not
         * be reused; the full scan below decides again. */
        CacheEntry->Valid = FALSE;
      }
    }
  }
  if (!CacheValidated)
  {
    for (Index = 0U; Index < Result->ApidCount; ++Index)
    {
      if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize, (UINTN)MapOffset + 4U * (UINTN)Index,
                       LiuqinSpmiWriteMapping, &Map, Result))
      {
        return FALSE;
      }
      if (Map == 0U || ((Map >> 8) & 0xFFFU) != Ppid)
      {
        continue;
      }
      ++Result->Matches;
      if (!ReadWindow (Io, Config->CnfgBase, Config->CnfgSize,
                       (UINTN)OwnerOffset + 4U * (UINTN)Index, LiuqinSpmiWriteMapping, &Owner,
                       Result))
      {
        return FALSE;
      }
      if (Result->CapturedMatches < LIUQIN_SPMI_WRITE_MAX_CANDIDATES)
      {
        UINT32 Candidate = Result->CapturedMatches;
        Result->CandidateApid[Candidate] = Index;
        Result->CandidateMap[Candidate] = Map;
        Result->CandidateOwner[Candidate] = Owner;
        ++Result->CapturedMatches;
      }
      if ((Owner & 0x0FU) != Config->Ee)
      {
        ++Result->OtherMatches;
        continue;
      }
      ++Result->EeMatches;
      if (Result->EeMatches == 1U ||
          Config->MappingPolicy == LIUQIN_SPMI_WRITE_SELECT_HIGHEST_EE_V7)
      {
        Result->Apid = Index;
        Result->Map = Map;
        Result->Owner = Owner;
      }
    }
    if (Result->Matches == 0U)
    {
      Result->Status = LiuqinSpmiWriteNoMapping;
      return FALSE;
    }
    if (Config->MappingPolicy == LIUQIN_SPMI_WRITE_SELECT_HIGHEST_EE_V7 &&
        Result->Matches > LIUQIN_SPMI_WRITE_MAX_CANDIDATES)
    {
      Result->Status = LiuqinSpmiWriteAmbiguous;
      return FALSE;
    }

    if (Result->EeMatches == 0U)
    {
      Result->Status = LiuqinSpmiWriteOwner;
      return FALSE;
    }
    if (Config->MappingPolicy == LIUQIN_SPMI_WRITE_SELECT_UNIQUE && Result->EeMatches != 1U)
    {
      Result->Status = LiuqinSpmiWriteAmbiguous;
      return FALSE;
    }
    Result->ApidSelected = TRUE;
  }
  if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize,
                   (UINTN)MapOffset + 4U * (UINTN)Result->Apid, LiuqinSpmiWriteMapping, &Map,
                   Result) ||
      !ReadWindow (Io, Config->CnfgBase, Config->CnfgSize,
                   (UINTN)OwnerOffset + 4U * (UINTN)Result->Apid, LiuqinSpmiWriteMapping, &Owner,
                   Result))
  {
    return FALSE;
  }
  if (Map != Result->Map || Owner != Result->Owner)
  {
    Result->Status = LiuqinSpmiWriteChanged;
    return FALSE;
  }
  /* Store only a freshly rechecked full-scan selection. The two words read
   * above are the same ones a later hit will validate against. */
  if (!CacheValidated && Config->Cache != NULL)
  {
    CacheEntry = &Config->Cache->Entry[Ppid % LIUQIN_SPMI_READ_CACHE_ENTRIES];
    CacheEntry->Valid = TRUE;
    CacheEntry->Ppid = (UINT16)Ppid;
    CacheEntry->Version = Result->Version;
    CacheEntry->Features = Result->Features;
    CacheEntry->Apid = Result->Apid;
    CacheEntry->Map = Result->Map;
    CacheEntry->Owner = Result->Owner;
    CacheEntry->MappingPolicy = Result->MappingPolicy;
  }
  return TRUE;
}

STATIC
BOOLEAN
ChannelErrors (LIUQIN_SPMI_WRITE_RESULT *Result)
{
  /* Unknown status bits are refused rather than read as success. A watchdog
   * reset means the arbiter aborted the transaction even though DONE may be
   * set, so it is treated as a failure and never as a completed write. */
  if ((Result->ChannelStatus & ~SPMI_WRITE_STATUS_RESERVED) != 0U)
  {
    Result->Status = LiuqinSpmiWriteUnsupported;
  }
  else if ((Result->ChannelStatus & SPMI_WRITE_STATUS_DENIED) != 0U)
  {
    Result->Status = LiuqinSpmiWriteDenied;
  }
  else if ((Result->ChannelStatus & SPMI_WRITE_STATUS_FAILURE) != 0U)
  {
    Result->Status = LiuqinSpmiWriteFailure;
  }
  else if ((Result->ChannelStatus & SPMI_WRITE_STATUS_DROPPED) != 0U)
  {
    Result->Status = LiuqinSpmiWriteDropped;
  }
  else if ((Result->ChannelStatus & SPMI_WRITE_STATUS_WDT_RST) != 0U)
  {
    Result->Status = LiuqinSpmiWriteWatchdog;
  }
  else
  {
    return FALSE;
  }
  return TRUE;
}

VOID LiuqinSpmiWriteResolve (CONST LIUQIN_SPMI_WRITE_IO *Io, CONST LIUQIN_SPMI_WRITE_CONFIG *Config,
                             UINT8 Slave, UINT16 Address, LIUQIN_SPMI_WRITE_RESULT *Result)
{
  if (Result == NULL)
  {
    return;
  }
  ClearResult (Result);
  Result->Status = LiuqinSpmiWriteInvalidParameter;
  if (Io == NULL || Config == NULL || Io->Read32 == NULL || Io->Write32 == NULL ||
      Io->DelayUs == NULL || Slave > 0x0FU)
  {
    return;
  }
  if (!ResolveChannel (Io, Config, Slave, Address, Result))
  {
    return;
  }
  /* No transaction ran: Valid stays FALSE and WriteCount stays zero. */
  Result->Status = LiuqinSpmiWriteComplete;
}

VOID LiuqinSpmiWrite (CONST LIUQIN_SPMI_WRITE_IO *Io, CONST LIUQIN_SPMI_WRITE_CONFIG *Config,
                      UINT8 Slave, UINT16 Address, CONST UINT8 *Bytes, UINT8 Length,
                      LIUQIN_SPMI_WRITE_RESULT *Result)
{
  UINT32 Index;
  UINT32 Word = 0U;
  UINTN StatusAddress, DataAddress, CommandAddress, ChannelOffset;
  UINT8 *WordBytes = (UINT8 *)&Word;

  if (Result == NULL)
  {
    return;
  }
  ClearResult (Result);
  Result->Status = LiuqinSpmiWriteInvalidParameter;
  if (Io == NULL || Config == NULL || Bytes == NULL || Io->Read32 == NULL || Io->Write32 == NULL ||
      Io->DelayUs == NULL || Slave > 0x0FU || Length == 0U ||
      Length > LIUQIN_SPMI_WRITE_MAX_BYTES || (UINT32)(Address & 0xFFU) + (UINT32)Length > 0x100U)
  {
    return;
  }

  if (!ResolveChannel (Io, Config, Slave, Address, Result))
  {
    return;
  }
  ChannelOffset = (UINTN)Result->Apid * SPMI_WRITE_CHANNEL_STRIDE;
  if (!RegisterAddress (Config->ChannelBase, Config->ChannelSize,
                        ChannelOffset + SPMI_WRITE_COMMAND_OFFSET, &CommandAddress, Result) ||
      !RegisterAddress (Config->ChannelBase, Config->ChannelSize,
                        ChannelOffset + SPMI_WRITE_STATUS_OFFSET, &StatusAddress, Result) ||
      !RegisterAddress (Config->ChannelBase, Config->ChannelSize,
                        ChannelOffset + SPMI_WRITE_DATA_OFFSET, &DataAddress, Result))
  {
    return;
  }
  if (!ReadRegister (Io, StatusAddress, LiuqinSpmiWritePreflight, TRUE, &Result->ChannelStatus,
                     Result) ||
      ChannelErrors (Result))
  {
    return;
  }
  if ((Result->ChannelStatus & SPMI_WRITE_STATUS_DONE) == 0U)
  {
    Result->Status = LiuqinSpmiWriteBusy;
    return;
  }
  for (Index = 0U; Index < (UINT32)Length; ++Index)
  {
    WordBytes[Index] = Bytes[Index];
  }
  Result->DataWord = Word;
  Result->LastAddress = DataAddress;
  if (Io->Progress != NULL)
  {
    Io->Progress (Io->Context, LiuqinSpmiWriteData, DataAddress);
  }
  ++Result->WriteCount;
  if (!Io->Write32 (Io->Context, DataAddress, Word))
  {
    Result->Status = LiuqinSpmiWriteUnreadable;
    return;
  }
  Result->Command = (SPMI_WRITE_OPCODE << SPMI_WRITE_OPCODE_SHIFT) |
                    (0U << SPMI_WRITE_PRIORITY_SHIFT) |
                    (((UINT32)Address & 0xFFU) << SPMI_WRITE_ADDRESS_SHIFT) | ((UINT32)Length - 1U);
  Result->LastAddress = CommandAddress;
  if (Io->Progress != NULL)
  {
    Io->Progress (Io->Context, LiuqinSpmiWriteCommand, CommandAddress);
  }
  ++Result->WriteCount;
  ++Result->CommandWrites;
  if (!Io->Write32 (Io->Context, CommandAddress, Result->Command))
  {
    Result->Status = LiuqinSpmiWriteUnreadable;
    return;
  }
  for (Index = 0U; Index < LIUQIN_SPMI_WRITE_MAX_POLLS; ++Index)
  {
    ++Result->PollCount;
    if (!ReadRegister (Io, StatusAddress, LiuqinSpmiWritePoll, TRUE, &Result->ChannelStatus,
                       Result) ||
        ChannelErrors (Result))
    {
      return;
    }
    if ((Result->ChannelStatus & SPMI_WRITE_STATUS_DONE) != 0U)
    {
      break;
    }
    Io->DelayUs (Io->Context, 1U);
  }
  if (Index == LIUQIN_SPMI_WRITE_MAX_POLLS)
  {
    Result->Status = LiuqinSpmiWriteTimeout;
    return;
  }
  Result->Valid = TRUE;
  Result->Status = LiuqinSpmiWriteComplete;
}

CONST CHAR8 *LiuqinSpmiWriteStatusText (LIUQIN_SPMI_WRITE_STATUS Status)
{
  switch (Status)
  {
  case LiuqinSpmiWriteNotRun:
    return "not run";
  case LiuqinSpmiWriteComplete:
    return "complete";
  case LiuqinSpmiWriteInvalidParameter:
    return "invalid parameter";
  case LiuqinSpmiWritePrerequisite:
    return "prerequisite";
  case LiuqinSpmiWriteUnsupported:
    return "unsupported layout";
  case LiuqinSpmiWriteResource:
    return "resource bounds";
  case LiuqinSpmiWriteUnreadable:
    return "access failed";
  case LiuqinSpmiWriteNoMapping:
    return "no PPID mapping";
  case LiuqinSpmiWriteOwner:
    return "no EE-owned write channel";
  case LiuqinSpmiWriteAmbiguous:
    return "multiple EE-owned channels";
  case LiuqinSpmiWriteChanged:
    return "mapping changed";
  case LiuqinSpmiWriteBusy:
    return "previous command incomplete";
  case LiuqinSpmiWriteDenied:
    return "transaction denied";
  case LiuqinSpmiWriteFailure:
    return "transaction failed";
  case LiuqinSpmiWriteDropped:
    return "transaction dropped";
  case LiuqinSpmiWriteWatchdog:
    return "arbiter watchdog reset";
  case LiuqinSpmiWriteTimeout:
    return "transaction timeout";
  case LiuqinSpmiWriteBudget:
    return "read budget";
  default:
    return "unknown";
  }
}
