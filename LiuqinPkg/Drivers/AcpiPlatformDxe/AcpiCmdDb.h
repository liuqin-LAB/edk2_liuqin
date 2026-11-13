/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_ACPI_CMD_DB_H
#define LIUQIN_ACPI_CMD_DB_H
#include <BootContract.h>

#define LIUQIN_CMD_DB_RESOURCE_COUNT 5U
#define LIUQIN_CMD_DB_AUX_MAX 64U
#define LIUQIN_WLAN_RESOURCE_COUNT 6U
typedef struct
{
  LIUQIN_U32 Address; /* RPMh protocol address, never a CPU MMIO address. */
  unsigned short Slave, Version, AuxLength;
  unsigned char Aux[LIUQIN_CMD_DB_AUX_MAX];
} LIUQIN_CMD_DB_RESOURCE;
typedef struct
{
  /* Opaque observed metadata; no checksum/version algorithm is asserted. */
  LIUQIN_U32 Version, Checksum, Flags, EntryCount;
  LIUQIN_CMD_DB_RESOURCE Resources[LIUQIN_CMD_DB_RESOURCE_COUNT];
} LIUQIN_CMD_DB_REPORT;
typedef struct
{
  LIUQIN_U32 Version, Checksum, Flags, EntryCount;
  /* Fixed stock order: smpe2, smpb11, smpb10, smpc1, smpb12, ldoe7. */
  LIUQIN_CMD_DB_RESOURCE Resources[LIUQIN_WLAN_RESOURCE_COUNT];
} LIUQIN_WLAN_CMD_DB_REPORT;
typedef struct
{
  LIUQIN_U32 Slave, HeaderOffset, DataOffset, Count, Version;
} LIUQIN_CMD_DB_GROUP;
typedef struct
{
  /* Diagnostic-only: populated even on a parse failure, never a resource
   * report or permission to issue power requests. Offsets are blob-relative. */
  LIUQIN_U32 HeaderDecoded, Version, Checksum, Flags;
  LIUQIN_U32 AtGroup, AtEntry, AtTarget, AtOffset;
  LIUQIN_CMD_DB_GROUP Groups[8];
} LIUQIN_CMD_DB_DIAGNOSTIC;
enum
{
  LiuqinCmdDbOk,
  LiuqinCmdDbArgument,
  LiuqinCmdDbMagic,
  LiuqinCmdDbBounds,
  LiuqinCmdDbCapacity,
  LiuqinCmdDbDuplicate,
  LiuqinCmdDbMissing,
  LiuqinCmdDbSlave,
  LiuqinCmdDbAux,
  LiuqinCmdDbRead,
  LiuqinCmdDbChanged
};
/* Layout derived from this board's v5.10 drivers/soc/qcom/cmd-db.c.
 * Parses a bounded caller-owned snapshot. All output is committed together
 * on success, unchanged on error. Resource order: ldob5, ldoc10, cx.lvl,
 * mx.lvl, xo.lvl. A successful parse does not authorize any power operation. */
int LiuqinParseCmdDb (const void *Snapshot, LIUQIN_SIZE Size, LIUQIN_CMD_DB_REPORT *Report,
                      LIUQIN_CMD_DB_DIAGNOSTIC *Diagnostic);
/* Callback must perform ordered aligned 32-bit reads from the fixed shared
 * reservation. Offset is relative to it, never an RPMh resource address.
 * Two complete passes must agree before parsing. Snapshot may be partial on
 * failure; Report remains unchanged. No allocation or writes to the device. */
int LiuqinCaptureCmdDb (int (*Read) (void *, LIUQIN_U32, LIUQIN_U32 *), void *Context,
                        void *Snapshot, LIUQIN_SIZE Size, LIUQIN_CMD_DB_REPORT *Report,
                        LIUQIN_CMD_DB_DIAGNOSTIC *Diagnostic);
int LiuqinParseWlanCmdDb (const void *, LIUQIN_SIZE, LIUQIN_WLAN_CMD_DB_REPORT *,
                          LIUQIN_CMD_DB_DIAGNOSTIC *);
int LiuqinCaptureWlanCmdDb (int (*Read) (void *, LIUQIN_U32, LIUQIN_U32 *), void *, void *,
                            LIUQIN_SIZE, LIUQIN_WLAN_CMD_DB_REPORT *, LIUQIN_CMD_DB_DIAGNOSTIC *);
#endif
