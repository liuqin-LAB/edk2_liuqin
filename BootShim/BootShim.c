/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include <BootContract.h>
#include "Layout.h"
#include "StoredGzip.h"

extern const uint8_t _payload[] __attribute__ ((visibility ("hidden")));
extern void ShimInvalidateRange (uint64_t base, uint64_t size);
extern void ShimEnterFirmware (const void *dt, uint64_t source, uint64_t entry)
    __attribute__ ((noreturn));

static int Overlap (uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
  return an > UINT64_MAX - a || bn > UINT64_MAX - b || (a < b + bn && b < a + an);
}

void ShimMain (const void *dt, uint64_t source)
{
  uint64_t dt_address = (uint64_t)(uintptr_t)dt;
  uint64_t image_size, payload_offset;
  uint32_t dt_size;
  const uint8_t *image = (const uint8_t *)(uintptr_t)source;
  /* The Linux boot ABI supplies a readable physical DTB.
   * Reject NULL/misalignment before libfdt reads its fixed header.
   */
  if (!dt || (dt_address & 7) || dt_address > UINT64_MAX - SHIM_MAX_DTB)
    return;
  dt_size = LiuqinDtbSize (dt);
  if (dt_size < 40 || dt_size > SHIM_MAX_DTB || LiuqinValidateDtb (dt, dt_size) != 0 ||
      LiuqinCheckIdentity (dt) != 0)
    return;
  image_size = *(const uint64_t *)(const void *)(image + 16);
  payload_offset = (uint64_t)(uintptr_t)_payload - source;
  if (payload_offset < 64 || payload_offset > SHIM_MAX_SIZE || image_size < payload_offset + 23 ||
      image_size > SHIM_FD_SIZE + SHIM_MAX_SIZE || source < SHIM_KERNEL_BASE ||
      source >= SHIM_KERNEL_END || image_size > SHIM_KERNEL_END - source ||
      Overlap (source, image_size, SHIM_FD_BASE, SHIM_FD_SIZE) ||
      Overlap (source, image_size, SHIM_HEAP_BASE, SHIM_HEAP_SIZE) ||
      Overlap (source, image_size, dt_address, dt_size) ||
      Overlap (dt_address, dt_size, SHIM_FD_BASE, SHIM_FD_SIZE) ||
      Overlap (dt_address, dt_size, SHIM_HEAP_BASE, SHIM_HEAP_SIZE))
    return;
  /* Validate both allocations before any firmware destination is written. */
  if (LiuqinCheckRange (dt, SHIM_FD_BASE, SHIM_FD_SIZE) != 0 ||
      LiuqinCheckRange (dt, SHIM_HEAP_BASE, SHIM_HEAP_SIZE) != 0 ||
      ShimStoredGzip (_payload, image_size - payload_offset, NULL, SHIM_FD_SIZE) != 0)
    return;
  /* ABL may leave cache lines for this now-verified, disjoint destination.
   * Discard them before the uncached copy; do not touch the live shim stack.
   */
  ShimInvalidateRange (SHIM_FD_BASE, SHIM_FD_SIZE);
  if (ShimStoredGzip (_payload, image_size - payload_offset, (uint8_t *)(uintptr_t)SHIM_FD_BASE,
                      SHIM_FD_SIZE) != 0)
    return;
  ShimEnterFirmware (dt, source, SHIM_FD_BASE);
}
