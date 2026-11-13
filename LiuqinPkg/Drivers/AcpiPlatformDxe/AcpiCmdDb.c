/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AcpiCmdDb.h"

static unsigned short Le16 (const unsigned char *P)
{
  return (unsigned short)((unsigned int)P[0] | (unsigned int)P[1] << 8);
}
static LIUQIN_U32 Le32 (const unsigned char *P)
{
  return (LIUQIN_U32)P[0] | (LIUQIN_U32)P[1] << 8 | (LIUQIN_U32)P[2] << 16 | (LIUQIN_U32)P[3] << 24;
}
static int Fits (LIUQIN_SIZE Size, LIUQIN_U32 Offset, LIUQIN_U32 Length)
{
  return Offset <= Size && Length <= Size - Offset;
}
static void InitDiagnostic (LIUQIN_CMD_DB_DIAGNOSTIC *Diagnostic)
{
  LIUQIN_CMD_DB_DIAGNOSTIC Empty = {0};
  if (!Diagnostic)
    return;
  *Diagnostic = Empty;
  Diagnostic->AtGroup = Diagnostic->AtEntry = Diagnostic->AtTarget = ~0U;
}
#define NAMED_MAX 6U
typedef struct
{
  LIUQIN_U32 Version, Checksum, Flags, EntryCount;
  LIUQIN_CMD_DB_RESOURCE Resources[NAMED_MAX];
} NAMED_REPORT;
static int ParseNamed (const void *Snapshot, LIUQIN_SIZE Size, const unsigned char Names[][8],
                       const unsigned short *Slaves, LIUQIN_U32 Targets, NAMED_REPORT *Report,
                       LIUQIN_CMD_DB_DIAGNOSTIC *Diagnostic)
{
  const unsigned char *B = Snapshot, *G, *E;
  NAMED_REPORT Result = {0};
  LIUQIN_U32 Found = 0, Group, Index, Target, Char, Headers, Data, Count;
  LIUQIN_U32 Length, Offset, Address, Total = 0;
  unsigned short Slave;
  int End = 0, Match;
  InitDiagnostic (Diagnostic);
  if (!B || !Report || !Targets || Targets > NAMED_MAX || Size < 144 || Size > LIUQIN_CMD_DB_SIZE)
    return LiuqinCmdDbArgument;
  if (Le32 (B + 4) != 0x0C0330DBU)
    return LiuqinCmdDbMagic;
  Result.Version = Le32 (B);
  Result.Checksum = Le32 (B + 136);
  Result.Flags = Le32 (B + 140);
  if (Diagnostic)
  {
    Diagnostic->HeaderDecoded = 1;
    Diagnostic->Version = Result.Version;
    Diagnostic->Checksum = Result.Checksum;
    Diagnostic->Flags = Result.Flags;
    for (Group = 0; Group < 8; ++Group)
    {
      G = B + 8 + Group * 16;
      Diagnostic->Groups[Group].Slave = Le16 (G);
      Diagnostic->Groups[Group].HeaderOffset = 144U + Le16 (G + 2);
      Diagnostic->Groups[Group].DataOffset = 144U + Le16 (G + 4);
      Diagnostic->Groups[Group].Count = Le16 (G + 6);
      Diagnostic->Groups[Group].Version = Le16 (G + 8);
    }
  }
  for (Group = 0; Group < 8; ++Group)
  {
    G = B + 8 + Group * 16;
    Slave = Le16 (G);
    if (Diagnostic)
    {
      Diagnostic->AtGroup = Group;
      Diagnostic->AtEntry = ~0U;
      Diagnostic->AtTarget = ~0U;
      Diagnostic->AtOffset = 8U + Group * 16U;
    }
    if (!Slave)
    {
      End = 1;
      continue;
    }
    if (End || Slave > 7)
      return LiuqinCmdDbSlave;
    // Board cmd-db.c scans every group; it does not require unique slave IDs.
    // Multiple groups of the same accelerator type may hold distinct keys.
    // Target resource names must still be unique across the whole database.
    Headers = 144U + Le16 (G + 2);
    Data = 144U + Le16 (G + 4);
    Count = Le16 (G + 6);
    if (Count > 1024U - Total)
      return LiuqinCmdDbCapacity;
    Total += Count;
    if (!Fits (Size, Headers, Count * 24U) || !Fits (Size, Data, 0))
      return LiuqinCmdDbBounds;
    for (Index = 0; Index < Count; ++Index)
    {
      E = B + Headers + Index * 24U;
      if (Diagnostic)
      {
        Diagnostic->AtEntry = Index;
        Diagnostic->AtTarget = ~0U;
        Diagnostic->AtOffset = Headers + Index * 24U;
      }
      Length = Le16 (E + 20);
      Offset = Data + Le16 (E + 22);
      if (!Fits (Size, Offset, Length))
        return LiuqinCmdDbBounds;
      for (Target = 0; Target < Targets; ++Target)
      {
        Match = 1;
        for (Char = 0; Char < 8; ++Char)
          if (E[Char] != Names[Target][Char])
            Match = 0;
        if (!Match)
          continue;
        if (Diagnostic)
          Diagnostic->AtTarget = Target;
        if (Found & (1U << Target))
          return LiuqinCmdDbDuplicate;
        Address = Le32 (E + 16);
        if (Slave != Slaves[Target] || ((Address >> 16) & 7U) != Slave || (Address & 3U))
          return LiuqinCmdDbSlave;
        if (Length > LIUQIN_CMD_DB_AUX_MAX || (Slave == 3 && (!Length || (Length & 1U))))
          return LiuqinCmdDbAux;
        Result.Resources[Target].Address = Address;
        Result.Resources[Target].Slave = Slave;
        Result.Resources[Target].Version = Le16 (G + 8);
        Result.Resources[Target].AuxLength = (unsigned short)Length;
        for (Char = 0; Char < Length; ++Char)
          Result.Resources[Target].Aux[Char] = B[Offset + Char];
        Found |= 1U << Target;
      }
    }
  }
  if (Found != (1U << Targets) - 1U)
    return LiuqinCmdDbMissing;
  Result.EntryCount = Total;
  *Report = Result;
  return LiuqinCmdDbOk;
}

static const unsigned char HostNames[][8] = {"ldob5", "ldoc10", "cx.lvl", "mx.lvl", "xo.lvl"};
static const unsigned short HostSlaves[] = {4, 4, 3, 3, 3};
static const unsigned char WlanNames[][8] = {"smpe2", "smpb11", "smpb10",
                                             "smpc1", "smpb12", "ldoe7"};
static const unsigned short WlanSlaves[] = {4, 4, 4, 4, 4, 4};
int LiuqinParseCmdDb (const void *Snapshot, LIUQIN_SIZE Size, LIUQIN_CMD_DB_REPORT *Report,
                      LIUQIN_CMD_DB_DIAGNOSTIC *Diagnostic)
{
  NAMED_REPORT R;
  int Code;
  if (!Report)
  {
    InitDiagnostic (Diagnostic);
    return LiuqinCmdDbArgument;
  }
  Code = ParseNamed (Snapshot, Size, HostNames, HostSlaves, LIUQIN_CMD_DB_RESOURCE_COUNT, &R,
                     Diagnostic);
  if (Code)
    return Code;
  Report->Version = R.Version;
  Report->Checksum = R.Checksum;
  Report->Flags = R.Flags;
  Report->EntryCount = R.EntryCount;
  for (unsigned int I = 0; I < LIUQIN_CMD_DB_RESOURCE_COUNT; ++I)
    Report->Resources[I] = R.Resources[I];
  return Code;
}
int LiuqinParseWlanCmdDb (const void *Snapshot, LIUQIN_SIZE Size, LIUQIN_WLAN_CMD_DB_REPORT *Report,
                          LIUQIN_CMD_DB_DIAGNOSTIC *Diagnostic)
{
  NAMED_REPORT R;
  int Code;
  if (!Report)
  {
    InitDiagnostic (Diagnostic);
    return LiuqinCmdDbArgument;
  }
  Code = ParseNamed (Snapshot, Size, WlanNames, WlanSlaves, LIUQIN_WLAN_RESOURCE_COUNT, &R,
                     Diagnostic);
  if (Code)
    return Code;
  Report->Version = R.Version;
  Report->Checksum = R.Checksum;
  Report->Flags = R.Flags;
  Report->EntryCount = R.EntryCount;
  for (unsigned int I = 0; I < LIUQIN_WLAN_RESOURCE_COUNT; ++I)
    Report->Resources[I] = R.Resources[I];
  return Code;
}

int LiuqinCaptureCmdDb (int (*Read) (void *, LIUQIN_U32, LIUQIN_U32 *), void *Context,
                        void *Snapshot, LIUQIN_SIZE Size, LIUQIN_CMD_DB_REPORT *Report,
                        LIUQIN_CMD_DB_DIAGNOSTIC *Diagnostic)
{
  unsigned char *B = Snapshot;
  LIUQIN_U32 Offset, Value, Byte;
  InitDiagnostic (Diagnostic);
  if (!Read || !B || !Report || Size != LIUQIN_CMD_DB_SIZE)
    return LiuqinCmdDbArgument;
  for (Offset = 0; Offset < Size; Offset += 4)
  {
    if (!Read (Context, Offset, &Value))
      return LiuqinCmdDbRead;
    for (Byte = 0; Byte < 4; ++Byte)
      B[Offset + Byte] = (unsigned char)(Value >> (Byte * 8));
  }
  for (Offset = 0; Offset < Size; Offset += 4)
  {
    if (!Read (Context, Offset, &Value))
      return LiuqinCmdDbRead;
    if (Value != Le32 (B + Offset))
      return LiuqinCmdDbChanged;
  }
  return LiuqinParseCmdDb (B, Size, Report, Diagnostic);
}

int LiuqinCaptureWlanCmdDb (int (*Read) (void *, LIUQIN_U32, LIUQIN_U32 *), void *Context,
                            void *Snapshot, LIUQIN_SIZE Size, LIUQIN_WLAN_CMD_DB_REPORT *Report,
                            LIUQIN_CMD_DB_DIAGNOSTIC *Diagnostic)
{
  unsigned char *B = Snapshot;
  LIUQIN_U32 Offset, Value, Byte;
  InitDiagnostic (Diagnostic);
  if (!Read || !B || !Report || Size != LIUQIN_CMD_DB_SIZE)
    return LiuqinCmdDbArgument;
  for (Offset = 0; Offset < Size; Offset += 4)
  {
    if (!Read (Context, Offset, &Value))
      return LiuqinCmdDbRead;
    for (Byte = 0; Byte < 4; ++Byte)
      B[Offset + Byte] = (unsigned char)(Value >> (Byte * 8));
  }
  for (Offset = 0; Offset < Size; Offset += 4)
  {
    if (!Read (Context, Offset, &Value))
      return LiuqinCmdDbRead;
    if (Value != Le32 (B + Offset))
      return LiuqinCmdDbChanged;
  }
  return LiuqinParseWlanCmdDb (B, Size, Report, Diagnostic);
}
