/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <BootContract.h>

typedef unsigned char U8;
typedef struct
{
  const U8 *reg;
  LIUQIN_U32 reglen, ac, sc, parent_ac, parent_sc;
  int memory, memory_name, enabled, reserved, reserved_parent, chosen, children;
  LIUQIN_U64 initrd_start, initrd_end;
  int has_start, has_end;
  LIUQIN_U32 seen;
} NODE;

static LIUQIN_U32 be32 (const U8 *p)
{
  return (LIUQIN_U32)p[0] << 24 | (LIUQIN_U32)p[1] << 16 | (LIUQIN_U32)p[2] << 8 | p[3];
}
static LIUQIN_U64 be64 (const U8 *p)
{
  return (LIUQIN_U64)be32 (p) << 32 | be32 (p + 4);
}
static int equal (const char *a, const char *b)
{
  while (*a && *a == *b)
  {
    ++a;
    ++b;
  }
  return *a == *b;
}
static int prefix (const char *a, const char *b)
{
  while (*b)
  {
    if (*a++ != *b++)
      return 0;
  }
  return 1;
}
static int string_list_has (const U8 *data, LIUQIN_U32 len, const char *value)
{
  LIUQIN_U32 at = 0;
  if (!len || data[len - 1])
    return 0;
  while (at < len)
  {
    if (equal ((const char *)data + at, value))
      return 1;
    while (at < len && data[at])
      ++at;
    ++at;
  }
  return 0;
}
static int terminated (const U8 *p, LIUQIN_SIZE len)
{
  LIUQIN_SIZE i;
  for (i = 0; i < len; ++i)
    if (!p[i])
      return 1;
  return 0;
}
static int valid_range (LIUQIN_U64 base, LIUQIN_U64 size)
{
  return size && size <= ~base;
}
int LiuqinRangesOverlap (LIUQIN_U64 a, LIUQIN_U64 n, LIUQIN_U64 b, LIUQIN_U64 m)
{
  if (!valid_range (a, n) || !valid_range (b, m))
    return 1;
  return a < b + m && b < a + n;
}
LIUQIN_U32 LiuqinDtbSize (const void *dt)
{
  return be32 ((const U8 *)dt + 4);
}

/* Structural validation also bounds the reservation map and every string. */
int LiuqinValidateDtb (const void *dt, LIUQIN_SIZE available)
{
  const U8 *p = dt;
  LIUQIN_U32 total, off, end, strings, slen, reserve, token, len, name;
  int depth = 0, roots = 0, ended = 0;
  if (!p || available < 40 || be32 (p) != 0xd00dfeed)
    return 1;
  total = be32 (p + 4);
  if (total < 40 || total > available || total > LIUQIN_DTB_MAX || be32 (p + 20) != 17 ||
      be32 (p + 24) > 17)
    return 2;
  off = be32 (p + 8);
  strings = be32 (p + 12);
  reserve = be32 (p + 16);
  slen = be32 (p + 32);
  len = be32 (p + 36);
  if (off < 40 || off % 4 || strings < 40 || strings > total || slen > total - strings ||
      len > total || off > total - len || reserve < 40 || reserve % 8 || reserve > total - 16)
    return 3;
  end = off + len;
  if (LiuqinRangesOverlap (off, len, strings, slen))
    return 3;
  for (;;)
  {
    if (reserve > total - 16 || LiuqinRangesOverlap (reserve, 16, off, len) ||
        LiuqinRangesOverlap (reserve, 16, strings, slen))
      return 4;
    if (!be64 (p + reserve + 8))
    {
      if (be64 (p + reserve))
        return 4;
      break;
    }
    if (!valid_range (be64 (p + reserve), be64 (p + reserve + 8)))
      return 4;
    reserve += 16;
  }
  while (off < end)
  {
    if (end - off < 4)
      return 5;
    token = be32 (p + off);
    off += 4;
    if (token == 1)
    {
      if (++depth > 32 || (depth == 1 && ++roots != 1))
        return 5;
      if (!terminated (p + off, end - off))
        return 5;
      while (p[off])
        ++off;
      off = (off + 4) & ~3U;
      if (off > end)
        return 5;
    }
    else if (token == 2)
    {
      if (depth <= 0)
        return 5;
      --depth;
    }
    else if (token == 3)
    {
      if (!depth || end - off < 8)
        return 5;
      len = be32 (p + off);
      name = be32 (p + off + 4);
      off += 8;
      if (name >= slen || !terminated (p + strings + name, slen - name) || len > end - off)
        return 5;
      off += (len + 3) & ~3U;
      if (off > end)
        return 5;
    }
    else if (token == 4)
    {
      continue;
    }
    else if (token == 9)
    {
      if (depth || roots != 1)
        return 5;
      ended = 1;
      break;
    }
    else
      return 5;
  }
  return ended ? 0 : 5;
}

static int check_regs (const NODE *n, LIUQIN_U64 base, LIUQIN_U64 size, int *covered)
{
  LIUQIN_U32 at, stride = (n->parent_ac + n->parent_sc) * 4;
  LIUQIN_U64 addr, bytes;
  if (!n->reg)
    return 0;
  if (!stride || n->reglen % stride)
    return 6;
  for (at = 0; at < n->reglen; at += stride)
  {
    addr = n->parent_ac == 2 ? be64 (n->reg + at) : be32 (n->reg + at);
    bytes = n->parent_sc == 2 ? be64 (n->reg + at + n->parent_ac * 4)
                              : be32 (n->reg + at + n->parent_ac * 4);
    if (!bytes)
      continue; /* ABL must fill memory placeholders before entry. */
    if (!valid_range (addr, bytes))
      return 6;
    if (n->reserved && LiuqinRangesOverlap (base, size, addr, bytes))
      return 7;
    if (n->memory && n->enabled && base >= addr && size <= bytes && base - addr <= bytes - size)
      *covered = 1;
  }
  return 0;
}

/* Only root memory and /reserved-memory physical reg entries are accepted.
 * A nonempty ranges translation or malformed initrd description fails closed.
 */
int LiuqinCheckRange (const void *dt, LIUQIN_U64 base, LIUQIN_U64 size)
{
  const U8 *p = dt, *data;
  const char *name, *key;
  NODE nodes[32], *n;
  LIUQIN_U32 off, strings, token, len, reserve;
  int depth = -1, covered = 0, error;
  if (!valid_range (base, size) || LiuqinValidateDtb (dt, LIUQIN_DTB_MAX))
    return 1;
  reserve = be32 (p + 16);
  while (be64 (p + reserve + 8))
  {
    if (LiuqinRangesOverlap (base, size, be64 (p + reserve), be64 (p + reserve + 8)))
      return 7;
    reserve += 16;
  }
  strings = be32 (p + 12);
  off = be32 (p + 8);
  for (;;)
  {
    token = be32 (p + off);
    off += 4;
    if (token == 1)
    {
      if (depth >= 0)
        nodes[depth].children = 1;
      ++depth;
      n = &nodes[depth];
      name = (const char *)p + off;
      n->reg = 0;
      n->reglen = 0;
      n->children = 0;
      n->seen = 0;
      n->ac = 2;
      n->sc = 1;
      n->parent_ac = depth ? nodes[depth - 1].ac : 2;
      n->parent_sc = depth ? nodes[depth - 1].sc : 1;
      n->memory = depth == 1 && (equal (name, "memory") || prefix (name, "memory@"));
      n->memory_name = n->memory;
      n->enabled = 1;
      n->reserved_parent = depth == 1 && equal (name, "reserved-memory");
      n->reserved = depth > 1 && nodes[depth - 1].reserved_parent;
      n->chosen = depth == 1 && equal (name, "chosen");
      n->has_start = 0;
      n->has_end = 0;
      n->initrd_start = 0;
      n->initrd_end = 0;
      while (p[off])
        ++off;
      off = (off + 4) & ~3U;
    }
    else if (token == 3)
    {
      n = &nodes[depth];
      len = be32 (p + off);
      key = (const char *)p + strings + be32 (p + off + 4);
      off += 8;
      data = p + off;
      off += (len + 3) & ~3U;
      if (n->children)
        return 6; /* DT spec: properties precede subnodes. */
      if (equal (key, "#address-cells") || equal (key, "#size-cells"))
      {
        LIUQIN_U32 flag = equal (key, "#address-cells") ? 1U : 2U;
        if (n->seen & flag)
          return 6;
        n->seen |= flag;
        if (len != 4)
          return 6;
        /* Irrelevant peripheral buses can have 0/3 cells; only RAM buses need 1/2. */
        if (equal (key, "#address-cells"))
          n->ac = be32 (data);
        else
          n->sc = be32 (data);
        if ((depth == 0 || n->reserved_parent) && (be32 (data) < 1 || be32 (data) > 2))
          return 6;
      }
      else if (equal (key, "reg") && (n->memory || n->reserved))
      {
        if (n->reg)
          return 6;
        n->reg = data;
        n->reglen = len;
      }
      else if (equal (key, "device_type") && depth == 1)
      {
        if (n->seen & 4U)
          return 6;
        n->seen |= 4U;
        if (n->memory_name && (len != 7 || !equal ((const char *)data, "memory")))
          return 6;
        if (len == 7 && equal ((const char *)data, "memory"))
          n->memory = 1;
      }
      else if (equal (key, "status") && depth == 1)
      {
        if ((n->seen & 8U) || !len || data[len - 1] || terminated (data, len - 1))
          return 6;
        n->seen |= 8U;
        n->enabled = equal ((const char *)data, "okay") || equal ((const char *)data, "ok");
      }
      else if (equal (key, "ranges") && n->reserved_parent && len)
        return 6;
      else if (n->chosen && (equal (key, "linux,initrd-start") || equal (key, "linux,initrd-end")))
      {
        LIUQIN_U64 value;
        if (len != 4 && len != 8)
          return 6;
        value = len == 8 ? be64 (data) : be32 (data);
        if (equal (key, "linux,initrd-start"))
        {
          if (n->has_start)
            return 6;
          n->has_start = 1;
          n->initrd_start = value;
        }
        else
        {
          if (n->has_end)
            return 6;
          n->has_end = 1;
          n->initrd_end = value;
        }
      }
    }
    else if (token == 2)
    {
      n = &nodes[depth--];
      error = check_regs (n, base, size, &covered);
      if (error)
        return error;
      if (n->chosen)
      {
        if (n->has_start != n->has_end || (n->has_start && n->initrd_end < n->initrd_start))
          return 6;
        if (n->has_start && n->initrd_end > n->initrd_start &&
            LiuqinRangesOverlap (base, size, n->initrd_start, n->initrd_end - n->initrd_start))
          return 7;
      }
    }
    else if (token == 9)
      break;
  }
  return covered ? 0 : 8;
}

int LiuqinCheckIdentity (const void *dt)
{
  const U8 *p = dt, *data;
  const char *key;
  LIUQIN_U32 off, strings, token, len, at;
  int depth = -1, soc = 0, board = 0, keyboard = 0, panel = 0, conflicting_board = 0;
  if (LiuqinValidateDtb (dt, LIUQIN_DTB_MAX))
    return 1;
  strings = be32 (p + 12);
  off = be32 (p + 8);
  for (;;)
  {
    token = be32 (p + off);
    off += 4;
    if (token == 1)
    {
      ++depth;
      while (p[off])
        ++off;
      off = (off + 4) & ~3U;
    }
    else if (token == 2)
      --depth;
    else if (token == 3)
    {
      len = be32 (p + off);
      key = (const char *)p + strings + be32 (p + off + 4);
      off += 8;
      data = p + off;
      off += (len + 3) & ~3U;

      if (depth)
      {
        if (equal (key, "compatible") && string_list_has (data, len, "nanosic,803"))
          keyboard = 1;
        if (equal (key, "qcom,mdss-dsi-panel-name") && terminated (data, len) &&
            prefix ((const char *)data, "xiaomi m81 "))
          panel = 1;
        continue;
      }
      if (equal (key, "qcom,msm-id") && len && !(len % 8))
      {
        for (at = 0; at < len; at += 8)
          if (be32 (data + at) == 531)
            soc = 1;
      }
      else if (equal (key, "xiaomi,miboard-id"))
      {
        if (len == 8 && be32 (data) == 16)
          board = 1;
        else
          conflicting_board = 1;
      }
      else if (equal (key, "model") && terminated (data, len) &&
               (prefix ((const char *)data, "Liuqin") || prefix ((const char *)data, "liuqin")))
        board = 1;
    }
    else if (token == 9)
      break;
  }
  return soc && !conflicting_board && (board || (keyboard && panel)) ? 0 : 9;
}
