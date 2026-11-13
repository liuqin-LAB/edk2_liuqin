/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_SPMI_READ_H
#define LIUQIN_SPMI_READ_H

#include <Base.h>

#define LIUQIN_SPMI_CORE_BASE 0x0C400000U
#define LIUQIN_SPMI_CORE_SIZE 0x00003000U
#define LIUQIN_SPMI_CNFG_BASE 0x0C42D000U
#define LIUQIN_SPMI_CNFG_SIZE 0x00004000U
#define LIUQIN_SPMI_OBSERVER_BASE 0x0C440000U
#define LIUQIN_SPMI_OBSERVER_SIZE 0x00080000U
#define LIUQIN_SPMI_READ_MAX_BYTES 8U
#define LIUQIN_SPMI_READ_MAX_POLLS 1000U
#define LIUQIN_SPMI_READ_MAX_READS 3055U
#define LIUQIN_SPMI_READ_MAX_CANDIDATES 8U
#define LIUQIN_SPMI_READ_SELECT_UNIQUE 0U
#define LIUQIN_SPMI_READ_SELECT_HIGHEST_EE_V7 1U

typedef enum
{
  LiuqinSpmiReadNotRun,
  LiuqinSpmiReadComplete,
  LiuqinSpmiReadInvalidParameter,
  LiuqinSpmiReadPrerequisite,
  LiuqinSpmiReadUnsupported,
  LiuqinSpmiReadResource,
  LiuqinSpmiReadUnreadable,
  LiuqinSpmiReadNoMapping,
  LiuqinSpmiReadOwner,
  LiuqinSpmiReadAmbiguous,
  LiuqinSpmiReadChanged,
  LiuqinSpmiReadBusy,
  LiuqinSpmiReadDenied,
  LiuqinSpmiReadFailure,
  LiuqinSpmiReadDropped,
  LiuqinSpmiReadTimeout,
  LiuqinSpmiReadBudget
} LIUQIN_SPMI_READ_STATUS;

typedef enum
{
  LiuqinSpmiReadMetadata,
  LiuqinSpmiReadMapping,
  LiuqinSpmiReadPreflight,
  LiuqinSpmiReadCommand,
  LiuqinSpmiReadPoll,
  LiuqinSpmiReadData
} LIUQIN_SPMI_READ_STAGE;

/* Ordered MMIO callbacks: success means that one access completed. The
 * adapter owns CPU barriers and exception handling. A false Write32 result
 * does not prove that the command was not issued; callers must not retry.
 * DelayUs is required for the bounded one-microsecond polling interval.
 * Progress is optional and is called before each access.
 * This layer only ever calls Write32 for one observer EXT_READL command.
 */
typedef struct
{
  VOID *Context;
  BOOLEAN (*Read32) (VOID *Context, UINTN Address, UINT32 *Value);
  BOOLEAN (*Write32) (VOID *Context, UINTN Address, UINT32 Value);
  VOID (*DelayUs) (VOID *Context, UINT32 Microseconds);
  VOID (*Progress) (VOID *Context, LIUQIN_SPMI_READ_STAGE Stage, UINTN Address);
} LIUQIN_SPMI_READ_IO;

#define LIUQIN_SPMI_READ_CACHE_ENTRIES 8U

typedef struct
{
  BOOLEAN Valid;
  UINT16 Ppid;
  UINT32 Version;
  UINT32 Features;
  UINT32 Apid;
  UINT32 Map;
  UINT32 Owner;
  /* Mapping policy that produced this selection; a slot only matches a
   * request under the same policy. */
  UINT32 MappingPolicy;
} LIUQIN_SPMI_READ_CACHE_ENTRY;

/* Tagged so the write library can forward-declare the same type without
 * including this header; the shared opt-in resolution cache lives here. */
typedef struct LIUQIN_SPMI_READ_CACHE
{
  LIUQIN_SPMI_READ_CACHE_ENTRY Entry[LIUQIN_SPMI_READ_CACHE_ENTRIES];
} LIUQIN_SPMI_READ_CACHE;

/* The production adapter must independently validate the runtime DT identity,
 * named bus-0 resources and EE, and prove that these MMIO ranges are mapped
 * without RAM/HOB conflicts. It must serialize observer access. Setting these
 * gates is not permission to modify PMIC registers or arbiter configuration.
 */
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
  UINTN ObserverBase;
  UINTN ObserverSize;
  /* Default zero preserves the unique-EE/sole-mapping observer policy.
   * The V7 highest-EE policy requires an explicit diagnostic opt-in. */
  UINT32 MappingPolicy;
  /* Optional isolated-session resolution cache; NULL keeps the full scan. */
  LIUQIN_SPMI_READ_CACHE *Cache;
} LIUQIN_SPMI_READ_CONFIG;

typedef struct
{
  LIUQIN_SPMI_READ_STATUS Status;
  BOOLEAN Valid;
  UINT8 Data[LIUQIN_SPMI_READ_MAX_BYTES];
  UINT32 Version;
  UINT32 Features;
  UINT32 ApidCount;
  UINT32 Matches;
  UINT32 EeMatches;
  UINT32 OtherMatches;
  /* First non-current-EE mapping observed during the scan, not selected
   * for a transaction. OtherMatches indicates whether these fields exist.
   * It is neither an ownership transfer nor an atomic metadata snapshot. */
  UINT32 OtherApid, OtherMap, OtherOwner;
  BOOLEAN ApidSelected;
  UINT32 Apid;
  UINT32 Map;
  UINT32 Owner;
  UINT32 Command;
  UINT32 ChannelStatus;
  UINT32 ReadCount;
  UINT32 CommandWrites;
  UINT32 PollCount;
  UINTN LastAddress;
  UINT32 MappingPolicy;
  /* Ascending APID order, including foreign write owners. CapturedMatches
   * counts complete map/owner pairs, capped at MAX_CANDIDATES; Matches is
   * the full scan count. These are observations, not write permissions. */
  UINT32 CapturedMatches;
  UINT32 CandidateApid[LIUQIN_SPMI_READ_MAX_CANDIDATES];
  UINT32 CandidateMap[LIUQIN_SPMI_READ_MAX_CANDIDATES];
  UINT32 CandidateOwner[LIUQIN_SPMI_READ_MAX_CANDIDATES];
  /* TRUE only when this transaction reused a validated cache resolution and
   * skipped the full APID scan. It is a cost observation, never a permission. */
  BOOLEAN CacheHit;
} LIUQIN_SPMI_READ_RESULT;

/* One read of 1..8 bytes, confined to one 256-byte PMIC peripheral. V1 accepts
 * V5/V6 major layouts and V7, bus 0 and EE 0 only. It requires exactly one
 * matching EE-owned APID, or a sole PPID mapping when none is EE-owned.
 * Explicit SELECT_HIGHEST_EE_V7 instead selects the highest matching EE-owned
 * V7 APID after the full scan. It retains the sole-mapping fallback when no
 * candidate is EE-owned, and refuses more than MAX_CANDIDATES total matches.
 * Write ownership is not observer read permission. The default policy refuses
 * duplicates with no unique EE-owned candidate, without trying other channels.
 * ApidSelected identifies when Apid/Map/Owner contain the selected mapping;
 * Owner remains the raw write-owner word, not a claim of read permission.
 * No reset, channel remapping, PMIC write, retry or cleanup transaction exists.
 * Data is committed only after the entire read succeeds; failed results have
 * Valid=FALSE and zero Data. Diagnostic raw fields remain available.
 */
VOID LiuqinSpmiRead (CONST LIUQIN_SPMI_READ_IO *Io, CONST LIUQIN_SPMI_READ_CONFIG *Config,
                     UINT8 Slave, UINT16 Address, UINT8 Length, LIUQIN_SPMI_READ_RESULT *Result);

CONST CHAR8 *LiuqinSpmiReadStatusText (LIUQIN_SPMI_READ_STATUS Status);

#endif
