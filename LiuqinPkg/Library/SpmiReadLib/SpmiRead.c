/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <SpmiRead.h>

#define SPMI_STATUS_DONE 0x01U
#define SPMI_STATUS_FAILURE 0x02U
#define SPMI_STATUS_DENIED 0x04U
#define SPMI_STATUS_DROPPED 0x08U
#define SPMI_READ_LONG 0x08000000U
#define SPMI_STATUS_OFFSET 0x08U
#define SPMI_RDATA0_OFFSET 0x18U
#define SPMI_RDATA1_OFFSET 0x1CU

static VOID ClearResult (LIUQIN_SPMI_READ_RESULT *Result)
{
  UINTN Index;
  UINT8 *Bytes = (UINT8 *)Result;
  for (Index = 0U; Index < sizeof (*Result); ++Index)
  {
    Bytes[Index] = 0U;
  }
}

static BOOLEAN RegisterAddress (UINTN Base, UINTN Size, UINTN Offset, UINTN *Address,
                                LIUQIN_SPMI_READ_RESULT *Result)
{
  if ((Offset & 3U) != 0U || Size < sizeof (UINT32) || Offset > Size - sizeof (UINT32) ||
      Base > MAX_UINTN - Offset)
  {
    Result->Status = LiuqinSpmiReadResource;
    return FALSE;
  }
  *Address = Base + Offset;
  return TRUE;
}

static BOOLEAN ReadRegister (CONST LIUQIN_SPMI_READ_IO *Io, UINTN Address,
                             LIUQIN_SPMI_READ_STAGE Stage, BOOLEAN RejectAllOnes, UINT32 *Value,
                             LIUQIN_SPMI_READ_RESULT *Result)
{
  UINT32 Raw = 0U;
  if (Result->ReadCount >= LIUQIN_SPMI_READ_MAX_READS)
  {
    Result->Status = LiuqinSpmiReadBudget;
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
    Result->Status = LiuqinSpmiReadUnreadable;
    return FALSE;
  }
  *Value = Raw;
  if (RejectAllOnes && Raw == MAX_UINT32)
  {
    Result->Status = LiuqinSpmiReadUnreadable;
    return FALSE;
  }
  return TRUE;
}

static BOOLEAN ReadWindow (CONST LIUQIN_SPMI_READ_IO *Io, UINTN Base, UINTN Size, UINTN Offset,
                           LIUQIN_SPMI_READ_STAGE Stage, UINT32 *Value,
                           LIUQIN_SPMI_READ_RESULT *Result)
{
  UINTN Address;
  return RegisterAddress (Base, Size, Offset, &Address, Result) &&
         ReadRegister (Io, Address, Stage, TRUE, Value, Result);
}

static BOOLEAN ChannelErrors (LIUQIN_SPMI_READ_RESULT *Result)
{

  if ((Result->ChannelStatus & ~0x0FU) != 0U)
  {
    Result->Status = LiuqinSpmiReadUnsupported;
  }
  else if ((Result->ChannelStatus & SPMI_STATUS_DENIED) != 0U)
  {
    Result->Status = LiuqinSpmiReadDenied;
  }
  else if ((Result->ChannelStatus & SPMI_STATUS_FAILURE) != 0U)
  {
    Result->Status = LiuqinSpmiReadFailure;
  }
  else if ((Result->ChannelStatus & SPMI_STATUS_DROPPED) != 0U)
  {
    Result->Status = LiuqinSpmiReadDropped;
  }
  else
  {
    return FALSE;
  }
  return TRUE;
}

VOID LiuqinSpmiRead (CONST LIUQIN_SPMI_READ_IO *Io, CONST LIUQIN_SPMI_READ_CONFIG *Config,
                     UINT8 Slave, UINT16 Address, UINT8 Length, LIUQIN_SPMI_READ_RESULT *Result)
{
  UINT32 Major, Maximum, MapOffset, OwnerOffset, Map, Owner, Index, Ppid;
  UINT32 FirstApid = 0U, FirstMap = 0U, FirstOwner = 0U;
  UINT32 Word0, Word1 = 0U;
  UINTN ObserverOffset, CommandAddress, StatusAddress, DataAddress;
  UINT8 Bytes[LIUQIN_SPMI_READ_MAX_BYTES];
  BOOLEAN CacheValidated;
  LIUQIN_SPMI_READ_CACHE_ENTRY *CacheEntry;

  if (Result == NULL)
  {
    return;
  }
  ClearResult (Result);
  Result->Status = LiuqinSpmiReadInvalidParameter;
  if (Config != NULL)
  {
    Result->MappingPolicy = Config->MappingPolicy;
  }
  if (Io == NULL || Config == NULL || Io->Read32 == NULL || Io->Write32 == NULL ||
      Io->DelayUs == NULL || Slave > 0x0FU || Length == 0U || Length > LIUQIN_SPMI_READ_MAX_BYTES ||
      (UINT32)(Address & 0xFFU) + (UINT32)Length > 0x100U ||
      (Config->MappingPolicy != LIUQIN_SPMI_READ_SELECT_UNIQUE &&
       Config->MappingPolicy != LIUQIN_SPMI_READ_SELECT_HIGHEST_EE_V7))
  {
    return;
  }
  Result->Status = LiuqinSpmiReadPrerequisite;
  if (!Config->DtValid || !Config->MmioValid || Config->Bus != 0U || Config->Ee != 0U)
  {
    return;
  }
  Result->Status = LiuqinSpmiReadResource;
  if (Config->CoreBase != LIUQIN_SPMI_CORE_BASE || Config->CoreSize == 0U ||
      Config->CoreSize > LIUQIN_SPMI_CORE_SIZE || Config->CnfgBase != LIUQIN_SPMI_CNFG_BASE ||
      Config->CnfgSize == 0U || Config->CnfgSize > LIUQIN_SPMI_CNFG_SIZE ||
      Config->ObserverBase != LIUQIN_SPMI_OBSERVER_BASE || Config->ObserverSize == 0U ||
      Config->ObserverSize > LIUQIN_SPMI_OBSERVER_SIZE)
  {
    return;
  }
  if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize, 0U, LiuqinSpmiReadMetadata,
                   &Result->Version, Result))
  {
    return;
  }
  Major = Result->Version >> 28;
  if (Config->MappingPolicy == LIUQIN_SPMI_READ_SELECT_HIGHEST_EE_V7 && Major != 7U)
  {
    Result->Status = LiuqinSpmiReadUnsupported;
    return;
  }
  if (Major == 5U || Major == 6U)
  {
    Maximum = 512U;
    MapOffset = 0x900U;
    OwnerOffset = 0x700U;
  }
  else if (Major == 7U)
  {
    Maximum = 1024U;
    MapOffset = 0x2000U;
    OwnerOffset = 0U;
  }
  else
  {
    Result->Status = LiuqinSpmiReadUnsupported;
    return;
  }
  if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize, 4U, LiuqinSpmiReadMetadata,
                   &Result->Features, Result))
  {
    return;
  }
  Result->ApidCount = Result->Features & 0x7FFU;
  if (Result->ApidCount == 0U || Result->ApidCount > Maximum)
  {
    Result->Status = LiuqinSpmiReadUnsupported;
    return;
  }
  Ppid = ((UINT32)Slave << 8) | ((UINT32)Address >> 8);
  /* Opt-in only: Config.Cache == NULL keeps the exact full-scan behaviour. A
   * cached resolution skips only the repeated table scan. The selected APID's
   * current map/owner words are re-read and must equal the cached pair, the
   * metadata words above were re-read and the slot's mapping policy must match
   * this request. On any mismatch the slot is invalidated and the existing
   * full scan runs. */
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
      else
      {
        if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize,
                         (UINTN)MapOffset + 4U * (UINTN)CacheEntry->Apid, LiuqinSpmiReadMapping,
                         &Map, Result) ||
            !ReadWindow (Io, Config->CnfgBase, Config->CnfgSize,
                         (UINTN)OwnerOffset + 4U * (UINTN)CacheEntry->Apid, LiuqinSpmiReadMapping,
                         &Owner, Result))
        {
          return;
        }
        if (Map == CacheEntry->Map && Owner == CacheEntry->Owner)
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
          CacheEntry->Valid = FALSE;
        }
      }
    }
  }
  if (!CacheValidated)
  {
    for (Index = 0U; Index < Result->ApidCount; ++Index)
    {
      if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize, (UINTN)MapOffset + 4U * (UINTN)Index,
                       LiuqinSpmiReadMapping, &Map, Result))
      {
        return;
      }
      if (Map == 0U || ((Map >> 8) & 0xFFFU) != Ppid)
      {
        continue;
      }
      ++Result->Matches;
      if (!ReadWindow (Io, Config->CnfgBase, Config->CnfgSize,
                       (UINTN)OwnerOffset + 4U * (UINTN)Index, LiuqinSpmiReadMapping, &Owner,
                       Result))
      {
        return;
      }
      if (Result->CapturedMatches < LIUQIN_SPMI_READ_MAX_CANDIDATES)
      {
        Result->CandidateApid[Result->CapturedMatches] = Index;
        Result->CandidateMap[Result->CapturedMatches] = Map;
        Result->CandidateOwner[Result->CapturedMatches] = Owner;
        ++Result->CapturedMatches;
      }
      if (Result->Matches == 1U)
      {
        FirstApid = Index;
        FirstMap = Map;
        FirstOwner = Owner;
      }
      if ((Owner & ((Major == 7U) ? 0x0FU : 7U)) != Config->Ee)
      {
        ++Result->OtherMatches;
        if (Result->OtherMatches == 1U)
        {
          Result->OtherApid = Index;
          Result->OtherMap = Map;
          Result->OtherOwner = Owner;
        }
        continue;
      }
      ++Result->EeMatches;

      if (Result->EeMatches == 1U || Config->MappingPolicy == LIUQIN_SPMI_READ_SELECT_HIGHEST_EE_V7)
      {
        Result->Apid = Index;
        Result->Map = Map;
        Result->Owner = Owner;
      }
    }
    if (Config->MappingPolicy == LIUQIN_SPMI_READ_SELECT_HIGHEST_EE_V7 &&
        Result->Matches > LIUQIN_SPMI_READ_MAX_CANDIDATES)
    {
      Result->Status = LiuqinSpmiReadAmbiguous;
      return;
    }
    if (Result->Matches == 0U)
    {
      Result->Status = LiuqinSpmiReadNoMapping;
      return;
    }
    if (Result->EeMatches == 0U)
    {
      if (Result->Matches != 1U)
      {
        Result->Status = LiuqinSpmiReadOwner;
        return;
      }

      Result->Apid = FirstApid;
      Result->Map = FirstMap;
      Result->Owner = FirstOwner;
    }
    else if (Result->EeMatches != 1U && Config->MappingPolicy == LIUQIN_SPMI_READ_SELECT_UNIQUE)
    {
      Result->Status = LiuqinSpmiReadAmbiguous;
      return;
    }
    Result->ApidSelected = TRUE;
  }

  if (!ReadWindow (Io, Config->CoreBase, Config->CoreSize,
                   (UINTN)MapOffset + 4U * (UINTN)Result->Apid, LiuqinSpmiReadMapping, &Map,
                   Result) ||
      !ReadWindow (Io, Config->CnfgBase, Config->CnfgSize,
                   (UINTN)OwnerOffset + 4U * (UINTN)Result->Apid, LiuqinSpmiReadMapping, &Owner,
                   Result))
  {
    return;
  }
  if (Map != Result->Map || Owner != Result->Owner)
  {
    Result->Status = LiuqinSpmiReadChanged;
    return;
  }
  /* Store only a freshly rechecked full-scan selection. The two words just
   * read above are the same ones a later hit will validate against. */
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
  /* EE=0 is independently gated above. No RW-channel address is formed. */
  ObserverOffset = (UINTN)Result->Apid * ((Major == 7U) ? 0x20U : 0x80U);
  if (!RegisterAddress (Config->ObserverBase, Config->ObserverSize, ObserverOffset, &CommandAddress,
                        Result) ||
      !RegisterAddress (Config->ObserverBase, Config->ObserverSize,
                        ObserverOffset + SPMI_STATUS_OFFSET, &StatusAddress, Result) ||
      !RegisterAddress (Config->ObserverBase, Config->ObserverSize,
                        ObserverOffset + ((Length > 4U) ? SPMI_RDATA1_OFFSET : SPMI_RDATA0_OFFSET),
                        &DataAddress, Result))
  {
    return;
  }
  if (!ReadRegister (Io, StatusAddress, LiuqinSpmiReadPreflight, TRUE, &Result->ChannelStatus,
                     Result) ||
      ChannelErrors (Result))
  {
    return;
  }
  if ((Result->ChannelStatus & SPMI_STATUS_DONE) == 0U)
  {
    Result->Status = LiuqinSpmiReadBusy;
    return;
  }
  Result->Command = SPMI_READ_LONG | ((UINT32)(Address & 0xFFU) << 4) | ((UINT32)Length - 1U);
  Result->LastAddress = CommandAddress;
  if (Io->Progress != NULL)
  {
    Io->Progress (Io->Context, LiuqinSpmiReadCommand, CommandAddress);
  }
  ++Result->CommandWrites;
  if (!Io->Write32 (Io->Context, CommandAddress, Result->Command))
  {
    Result->Status = LiuqinSpmiReadUnreadable;
    return;
  }
  for (Index = 0U; Index < LIUQIN_SPMI_READ_MAX_POLLS; ++Index)
  {
    ++Result->PollCount;
    if (!ReadRegister (Io, StatusAddress, LiuqinSpmiReadPoll, TRUE, &Result->ChannelStatus,
                       Result) ||
        ChannelErrors (Result))
    {
      return;
    }
    if ((Result->ChannelStatus & SPMI_STATUS_DONE) != 0U)
    {
      break;
    }
    Io->DelayUs (Io->Context, 1U);
  }
  if (Index == LIUQIN_SPMI_READ_MAX_POLLS)
  {
    Result->Status = LiuqinSpmiReadTimeout;
    return;
  }
  if (!RegisterAddress (Config->ObserverBase, Config->ObserverSize,
                        ObserverOffset + SPMI_RDATA0_OFFSET, &DataAddress, Result) ||
      !ReadRegister (Io, DataAddress, LiuqinSpmiReadData, FALSE, &Word0, Result))
  {
    return;
  }
  if (Length > 4U &&
      (!RegisterAddress (Config->ObserverBase, Config->ObserverSize,
                         ObserverOffset + SPMI_RDATA1_OFFSET, &DataAddress, Result) ||
       !ReadRegister (Io, DataAddress, LiuqinSpmiReadData, FALSE, &Word1, Result)))
  {
    return;
  }
  for (Index = 0U; Index < (UINT32)Length; ++Index)
  {
    Bytes[Index] = (UINT8)(((Index < 4U) ? Word0 : Word1) >> (8U * (Index & 3U)));
  }
  for (Index = 0U; Index < (UINT32)Length; ++Index)
  {
    Result->Data[Index] = Bytes[Index];
  }
  Result->Valid = TRUE;
  Result->Status = LiuqinSpmiReadComplete;
}

CONST CHAR8 *LiuqinSpmiReadStatusText (LIUQIN_SPMI_READ_STATUS Status)
{
  switch (Status)
  {
  case LiuqinSpmiReadNotRun:
    return "not run";
  case LiuqinSpmiReadComplete:
    return "complete";
  case LiuqinSpmiReadInvalidParameter:
    return "invalid parameter";
  case LiuqinSpmiReadPrerequisite:
    return "prerequisite";
  case LiuqinSpmiReadUnsupported:
    return "unsupported layout";
  case LiuqinSpmiReadResource:
    return "resource bounds";
  case LiuqinSpmiReadUnreadable:
    return "access failed";
  case LiuqinSpmiReadNoMapping:
    return "no PPID mapping";
  case LiuqinSpmiReadOwner:
    return "no unique observer APID";
  case LiuqinSpmiReadAmbiguous:
    return "multiple EE-owned APIDs";
  case LiuqinSpmiReadChanged:
    return "mapping changed";
  case LiuqinSpmiReadBusy:
    return "previous command incomplete";
  case LiuqinSpmiReadDenied:
    return "transaction denied";
  case LiuqinSpmiReadFailure:
    return "transaction failed";
  case LiuqinSpmiReadDropped:
    return "transaction dropped";
  case LiuqinSpmiReadTimeout:
    return "transaction timeout";
  case LiuqinSpmiReadBudget:
    return "read budget";
  default:
    return "unknown";
  }
}
