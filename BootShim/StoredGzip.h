/* SPDX-License-Identifier: MIT */
#ifndef LIUQIN_STORED_GZIP_H
#define LIUQIN_STORED_GZIP_H
#include <stddef.h>
#include <stdint.h>

/* Only deterministic gzip with RFC1951 STORED blocks is accepted.
 * NULL output verifies every block, CRC and ISIZE without a destination write.
 */
int ShimStoredGzip (const uint8_t *input, size_t input_size, uint8_t *output, size_t expected_size);
#endif
