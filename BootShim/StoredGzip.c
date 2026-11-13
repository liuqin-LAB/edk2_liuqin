/* SPDX-License-Identifier: MIT */
#include "StoredGzip.h"

static uint32_t Read32 (const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ShimStoredGzip (const uint8_t *input, size_t input_size, uint8_t *output, size_t expected_size)
{
  size_t in = 10, out = 0;
  uint32_t crc = 0xffffffffU;
  unsigned final = 0;
  static const uint8_t header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
  /* Four reflected CRC-32 steps per table entry (polynomial edb88320).
   * A 16-entry table keeps the uncompressed 16 MiB verification fast enough
   * for a first-boot experiment, while remaining small and deterministic.
   */
  static const uint32_t crc_nibbles[16] = {0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
                                           0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
                                           0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
                                           0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU};
  if (!input || input_size < 23 || expected_size > 0xffffffffU)
    return -1;
  for (size_t i = 0; i < sizeof (header); ++i)
    if (input[i] != header[i])
      return -2;
  do
  {
    unsigned len, complement, flags;
    if (input_size - in < 13)
      return -3;
    flags = input[in++];
    /* BTYPE=00, byte-alignment padding=0; reject all other encodings. */
    if (flags != 0 && flags != 1)
      return -4;
    final = flags;
    len = (unsigned)input[in] | ((unsigned)input[in + 1] << 8);
    complement = (unsigned)input[in + 2] | ((unsigned)input[in + 3] << 8);
    in += 4;
    if ((len ^ complement) != 0xffffU || len > input_size - in - 8 || len > expected_size - out)
      return -5;
    for (unsigned i = 0; i < len; ++i)
    {
      uint8_t byte = input[in++];
      crc ^= byte;
      crc = (crc >> 4) ^ crc_nibbles[crc & 15U];
      crc = (crc >> 4) ^ crc_nibbles[crc & 15U];
      if (output)
        output[out] = byte;
      ++out;
    }
  } while (!final);
  if (in != input_size - 8 || out != expected_size || Read32 (input + in) != (crc ^ 0xffffffffU) ||
      Read32 (input + in + 4) != expected_size)
    return -6;
  return 0;
}
