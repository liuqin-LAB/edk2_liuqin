/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_MMIO_H
#define LIUQIN_MMIO_H
#include <Base.h>
#include <BootContract.h>

typedef struct
{
  UINT64 Base;
  UINT64 Size;
} LIUQIN_MMIO_WINDOW;
#define LIUQIN_MMIO_MAX_WINDOWS 25U
#define LIUQIN_CORE_MMIO_MAX_WINDOWS 3U
#define LIUQIN_SHARED_MEMORY_MAX_WINDOWS 1U

/* Normal noncached shared memory, never Device or allocatable RAM. The
 * reader does not write it. Do not infer a hardware write permission here. */
STATIC inline UINTN LiuqinGetSharedMemoryWindows (LIUQIN_MMIO_WINDOW *Windows, UINTN Capacity)
{
  if (Windows == NULL || Capacity < LIUQIN_SHARED_MEMORY_MAX_WINDOWS)
    return 0;
  Windows[0].Base = LIUQIN_CMD_DB_BASE;
  Windows[0].Size = LIUQIN_CMD_DB_SIZE;
  return 1;
}

STATIC inline UINTN LiuqinGetCoreIoWindows (UINT64 Distributor, UINT64 Redistributor,
                                            LIUQIN_MMIO_WINDOW *Windows, UINTN Capacity)
{
  STATIC CONST LIUQIN_MMIO_WINDOW Gic[] = {
      {0x17100000, 0x10000}, {0x17140000, 0x20000}, {0x17180000, 0x200000}};
  UINTN Index;
  if (Windows == NULL || Capacity < ARRAY_SIZE (Gic) || Distributor != 0x17100000U ||
      Redistributor != 0x17180000U)
    return 0;
  for (Index = 0; Index < ARRAY_SIZE (Gic); ++Index)
    Windows[Index] = Gic[Index];
  return ARRAY_SIZE (Gic);
}

STATIC inline UINTN LiuqinGetIoWindows (LIUQIN_MMIO_WINDOW *Windows, UINTN Capacity)
{
  STATIC CONST LIUQIN_MMIO_WINDOW Io[] = {
      {0x01D84000, 0x3000},
      {0x0A600000, 0xE000},
      {0x00149000, 0x1000},
      {0x15000000, 0x100000},
      {0x00187000, 0x1000},
      {0x0C400000, 0x3000},
      {0x0C42D000, 0x4000},
      {0x0C440000, 0x80000},
      {0x0C500000, 0x400000},
      // PCIe0 AP power/clock votes and core/PHY BCR status (Cape GCC).
      {0x00162000, 0x1000},
      {0x0017C000, 0x1000},
      // PCIe0 clock readiness, PARF and ECAM. No broad SoC mapping.
      {0x0017B000, 0x1000},
      {0x01C00000, 0x3000},
      // Cape PCIe0 PHY status/control and its GCC reference-clock branch.
      {0x01C06000, 0x2000},
      {0x0019C000, 0x1000},
      // Cape HLOS RPMh DRV2 only; bounded ACTIVE TCS transport, not DRV0/1/3.
      {0x17A20000, 0x10000},
      // Only PCIe PERST94 / CLKREQ95 / WAKE96 TLMM pages for PHY bring-up.
      {0x0F15E000, 0x1000},
      {0x0F15F000, 0x1000},
      {0x0F160000, 0x1000},
      // CNSS: WLAN80, BT81/SW_CTRL82 observation and XO204 only.
      {0x0F150000, 0x3000},
      {0x0F1CC000, 0x1000},
      {0x60000000, 0x200000},
      // Scoped iATU relocation into the first page of Cape's unused I/O aperture.
      // This page is not part of the root bridge's allocatable MEM resource.
      {0x60200000, 0x1000},
      // Relocated eDMA control registers.
      {0x60202000, 0x1000},
      // Shared GPLL0 status.
      {0x00100000, 0x1000}};
  UINTN Index;
  if (Windows == NULL || Capacity < ARRAY_SIZE (Io))
    return 0;
  for (Index = 0; Index < ARRAY_SIZE (Io); ++Index)
    Windows[Index] = Io[Index];
  return ARRAY_SIZE (Io);
}
#endif
