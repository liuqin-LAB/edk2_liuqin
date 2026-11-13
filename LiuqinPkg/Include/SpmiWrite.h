/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_SPMI_WRITE_H
#define LIUQIN_SPMI_WRITE_H

#include <Base.h>

#define LIUQIN_SPMI_WRITE_CHANNEL_BASE 0x0C500000U
#define LIUQIN_SPMI_WRITE_CHANNEL_SIZE 0x00400000U
#define LIUQIN_SPMI_WRITE_MAX_POLLS 1000U
#define LIUQIN_SPMI_WRITE_MAX_READS 3055U
#define LIUQIN_SPMI_WRITE_MAX_BYTES 4U
#define LIUQIN_SPMI_WRITE_MAX_CANDIDATES 8U

/* Zero preserves the original unique-EE-owned contract for existing users.
 * The explicit V7 policy follows matching K:989-1045: scan the complete bus
 * in ascending APID order, selecting each current-EE match in turn, hence
 * the highest current-EE APID. IRQ ownership (map bit24) does not select a
 * polling read/write channel. BOOT's V7 hardware definition uses all four
 * owner bits (Q/.../hoya7/hw/v7p0/SpmiCoreHwio.h:70-86); owner 8 must not be
 * treated as EE0 even though K masks three bits. This policy neither grants
 * ownership nor writes a translation/owner table. */
#define LIUQIN_SPMI_WRITE_SELECT_UNIQUE 0U
#define LIUQIN_SPMI_WRITE_SELECT_HIGHEST_EE_V7 1U

typedef enum
{
  LiuqinSpmiWriteNotRun,
  LiuqinSpmiWriteComplete,
  LiuqinSpmiWriteInvalidParameter,
  LiuqinSpmiWritePrerequisite,
  LiuqinSpmiWriteUnsupported,
  LiuqinSpmiWriteResource,
  LiuqinSpmiWriteUnreadable,
  LiuqinSpmiWriteNoMapping,
  LiuqinSpmiWriteOwner,
  LiuqinSpmiWriteAmbiguous,
  LiuqinSpmiWriteChanged,
  LiuqinSpmiWriteBusy,
  LiuqinSpmiWriteDenied,
  LiuqinSpmiWriteFailure,
  LiuqinSpmiWriteDropped,
  LiuqinSpmiWriteWatchdog,
  LiuqinSpmiWriteTimeout,
  LiuqinSpmiWriteBudget
} LIUQIN_SPMI_WRITE_STATUS;

typedef enum
{
  LiuqinSpmiWriteMetadata,
  LiuqinSpmiWriteMapping,
  LiuqinSpmiWritePreflight,
  LiuqinSpmiWriteData,
  LiuqinSpmiWriteCommand,
  LiuqinSpmiWritePoll
} LIUQIN_SPMI_WRITE_STAGE;

/* Ordered MMIO callbacks. A false Write32 result does not prove that the
 * command was not issued, so this layer never retries and never issues a
 * second command word for the same request. */
typedef struct
{
  VOID *Context;
  BOOLEAN (*Read32) (VOID *Context, UINTN Address, UINT32 *Value);
  BOOLEAN (*Write32) (VOID *Context, UINTN Address, UINT32 Value);
  VOID (*DelayUs) (VOID *Context, UINT32 Microseconds);
  VOID (*Progress) (VOID *Context, LIUQIN_SPMI_WRITE_STAGE Stage, UINTN Address);
} LIUQIN_SPMI_WRITE_IO;

/* The production adapter must independently validate the runtime DT identity,
 * the named bus-0 resources and the EE, and must prove that these MMIO ranges
 * are mapped without RAM/HOB conflicts. V1 accepts the V7 layout, bus 0 and
 * EE 0 only; a V5/V6 length-based channel stride is not implemented.
 */

struct LIUQIN_SPMI_READ_CACHE;
typedef struct
{
  BOOLEAN DtValid;
  BOOLEAN MmioValid;
  UINT32 Bus;
  UINT32 Ee;
  UINTN CoreBase;
  UINTN CoreSize;
  UINTN CnfgBase;
  UINTN CnfgSize;
  UINTN ChannelBase;
  UINTN ChannelSize;
  UINT32 MappingPolicy;

  struct LIUQIN_SPMI_READ_CACHE *Cache;
} LIUQIN_SPMI_WRITE_CONFIG;

typedef struct
{
  LIUQIN_SPMI_WRITE_STATUS Status;
  BOOLEAN Valid;
  BOOLEAN ApidSelected;
  UINT32 Apid;
  UINT32 Map;
  UINT32 Owner;
  UINT32 Matches;
  UINT32 EeMatches;
  UINT32 OtherMatches;
  UINT32 Version;
  UINT32 Features;
  UINT32 ApidCount;
  UINT32 Command;
  UINT32 ChannelStatus;
  UINT32 DataWord;
  UINT32 ReadCount;
  UINT32 WriteCount;
  UINT32 CommandWrites;
  UINT32 PollCount;
  UINTN LastAddress;
  UINT32 MappingPolicy;
  UINT32 CapturedMatches;
  UINT32 CandidateApid[LIUQIN_SPMI_WRITE_MAX_CANDIDATES];
  UINT32 CandidateMap[LIUQIN_SPMI_WRITE_MAX_CANDIDATES];
  UINT32 CandidateOwner[LIUQIN_SPMI_WRITE_MAX_CANDIDATES];
  /* TRUE only when this resolve validated and reused a cache slot and skipped
   * the full APID scan. A cost observation, never a permission. */
  BOOLEAN CacheHit;
} LIUQIN_SPMI_WRITE_RESULT;

VOID LiuqinSpmiWriteResolve (CONST LIUQIN_SPMI_WRITE_IO *Io, CONST LIUQIN_SPMI_WRITE_CONFIG *Config,
                             UINT8 Slave, UINT16 Address, LIUQIN_SPMI_WRITE_RESULT *Result);

/* One bounded write of 1..4 bytes to one PMIC register, confined to one
 * 256-byte peripheral. The default policy requires a unique PPID mapping whose
 * write owner is the configured EE (0); the explicit V7 policy selects its
 * highest EE-owned APID instead. Every call performs a fresh complete table
 * scan, then rechecks the selected raw map/full owner before channel access.
 * A missing EE-owned mapping, policy ambiguity, or a metadata change refuses
 * the request without touching the channel. A previous transaction
 * that is still incomplete refuses the request. Exactly one command word is
 * written per call and no retry, remap, reset, IRQ or CONFIG register access
 * exists. Success means the arbiter reported DONE without DENIED, FAILURE or
 * DROPPED; it is not a readback of the register contents and cannot prove the
 * electrical effect.
 */
VOID LiuqinSpmiWrite (CONST LIUQIN_SPMI_WRITE_IO *Io, CONST LIUQIN_SPMI_WRITE_CONFIG *Config,
                      UINT8 Slave, UINT16 Address, CONST UINT8 *Bytes, UINT8 Length,
                      LIUQIN_SPMI_WRITE_RESULT *Result);

CONST CHAR8 *LiuqinSpmiWriteStatusText (LIUQIN_SPMI_WRITE_STATUS Status);

#endif
