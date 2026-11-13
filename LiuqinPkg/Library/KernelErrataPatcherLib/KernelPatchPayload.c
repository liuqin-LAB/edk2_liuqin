/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Freestanding position-independent code run in winload's application mappings.
 * No firmware pointers, boot services, allocation, or external calls are valid.
 * LPB/KLDR offsets match the Windows ARM64 loader ABI used by this port.
 */
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned long long UP;
typedef long long IP;

extern __attribute__ ((visibility ("hidden"))) const U8 KepSgiThunkStart[], KepSgiThunkEnd[];
extern __attribute__ ((visibility ("hidden"))) const U8 KepSgiThunkOperand[], KepSgiThunkReturn[];

#define PAGE 4096U
#define MAX_SECTIONS 96U
#define MAX_MODULES 1024U
#define MAX_IMAGE 0x08000000U
#define EXECUTE 0x20000000U

static U16 r16 (UP p)
{
  return *(const U16 *)p;
}
static U32 r32 (UP p)
{
  return *(const U32 *)p;
}
static UP rptr (UP p)
{
  return *(const UP *)p;
}

typedef struct
{
  UP base;
  U32 size;
  U16 count;
  const U8 *sections;
} IMAGE;

/* Reject the whole image before modifying anything. Section page tails may
 * hold a trampoline only when no other live section intersects those pages. */
static int image (UP base, U32 size, IMAGE *out)
{
  U32 pe, optional, table, n, i;
  if (!base || (base & (PAGE - 1)) || size < PAGE || size > MAX_IMAGE || (size & (PAGE - 1)) ||
      base + size < base || r16 (base) != 0x5a4d)
    return 0;
  pe = r32 (base + 0x3c);
  if ((pe & 3) || pe > size - 24 || r32 (base + pe) != 0x4550 || r16 (base + pe + 4) != 0xaa64)
    return 0;
  n = r16 (base + pe + 6);
  optional = r16 (base + pe + 20);
  table = pe + 24;
  if (!n || n > MAX_SECTIONS || optional < 0xf0 || optional > size - table ||
      r16 (base + table) != 0x20b || r32 (base + table + 32) != PAGE ||
      r32 (base + table + 56) != size)
    return 0;
  table += optional;
  if ((table & 3) || n > (size - table) / 40 || r32 (base + pe + 24 + 60) < table + n * 40 ||
      r32 (base + pe + 24 + 60) > size)
    return 0;
  for (i = 0; i < n; ++i)
  {
    UP s = base + table + i * 40;
    U32 start = r32 (s + 12), length = r32 (s + 8), raw = r32 (s + 16), j;
    if (raw > length)
      length = raw;
    if (!length)
      continue;
    if (start > size || length > size - start || (start & (PAGE - 1)) ||
        start < r32 (base + pe + 24 + 60))
      return 0;
    length = (length + PAGE - 1) & ~(PAGE - 1);
    if (length > size - start)
      return 0;
    for (j = 0; j < i; ++j)
    {
      UP other = base + table + j * 40;
      U32 os = r32 (other + 12), oz = r32 (other + 8), rawOther = r32 (other + 16);
      if (rawOther > oz)
        oz = rawOther;
      if (oz && start < os + ((oz + PAGE - 1) & ~(PAGE - 1)) && os < start + length)
        return 0;
    }
  }
  out->base = base;
  out->size = size;
  out->count = (U16)n;
  out->sections = (const U8 *)(base + table);
  return 1;
}

static int branch (UP from, UP to, U32 *result)
{
  IP delta = (IP)to - (IP)from;
  if ((delta & 3) || delta < -0x08000000LL || delta >= 0x08000000LL)
    return 0;
  *result = 0x14000000U | ((U32)(delta >> 2) & 0x03ffffffU);
  return 1;
}

/* Allocate only zero padding after both VirtualSize and RawSize, inside this executable
 * section's final page, and within SizeOfImage. Never overwrite live code. */
static UP cave (const IMAGE *im, U32 length)
{
  U32 i, j;
  for (i = 0; i < im->count; ++i)
  {
    UP s = (UP)im->sections + i * 40;
    U32 start = r32 (s + 12), size = r32 (s + 8), raw = r32 (s + 16), end, limit, offset;
    if (!(r32 (s + 36) & EXECUTE) || !size)
      continue;
    if (raw > size)
      size = raw;
    if (size > im->size - start)
      continue;
    end = start + size;
    limit = (end + PAGE - 1) & ~(PAGE - 1);
    if (limit > im->size)
      continue;
    for (j = 0; j < im->count; ++j)
    {
      UP other = (UP)im->sections + j * 40;
      U32 os = r32 (other + 12), oz = r32 (other + 8), oraw = r32 (other + 16);
      if (oraw > oz)
        oz = oraw;
      if (j != i && oz && end < os + oz && os < limit)
        break;
    }
    if (j != im->count)
      continue;
    for (offset = (end + 15) & ~15U; offset <= limit && length <= limit - offset; offset += 16)
    {
      U32 k;
      for (k = 0; k < length; ++k)
        if (*(const U8 *)(im->base + offset + k))
          break;
      if (k == length)
        return im->base + offset;
    }
  }
  return 0;
}

static void sgi (const IMAGE *im, UP site, U32 value)
{
  U32 length = (U32)(KepSgiThunkEnd - KepSgiThunkStart);
  U32 exit = (U32)(KepSgiThunkReturn - KepSgiThunkStart);
  U32 operand = (U32)(KepSgiThunkOperand - KepSgiThunkStart);
  U32 entryBranch, returnBranch, i;
  UP slot = cave (im, length);
  if (!slot || !branch (site, slot, &entryBranch) || !branch (slot + exit, site + 4, &returnBranch))
    return;
  for (i = 0; i < length; ++i)
    *(U8 *)(slot + i) = KepSgiThunkStart[i];
  /* MOV x0, x8/x10; source MSR encoding's low five bits hold Rt. */
  *(U32 *)(slot + operand) = 0xaa0003e0U | ((value & 31) << 16);
  *(U32 *)(slot + exit) = returnBranch;
  *(U32 *)site = entryBranch;
}

static void synchronize (UP first, UP end)
{
  U64 ctr;
  UP p, line;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
  line = 4ULL << ((ctr >> 16) & 15);
  for (p = first & ~(line - 1); p < end; p += line)
    __asm__ volatile ("dc cvau, %0" ::"r"(p) : "memory");
  __asm__ volatile ("dsb ish" ::: "memory");
  line = 4ULL << (ctr & 15);
  for (p = first & ~(line - 1); p < end; p += line)
    __asm__ volatile ("ic ivau, %0" ::"r"(p) : "memory");
  __asm__ volatile ("dsb ish\n\tisb" ::: "memory");
}

__attribute__ ((visibility ("hidden"))) void KepPatchLoadedModules (UP loaderBlock)
{
  UP head = loaderBlock + 0x10, entry;
  U32 remaining = MAX_MODULES;
  if (!loaderBlock || (loaderBlock & 7))
    return;
  entry = rptr (head);
  while (entry != head && entry && !(entry & 7) && remaining--)
  {
    IMAGE im;
    U32 i, changed = 0;
    if (image (rptr (entry + 0x30), r32 (entry + 0x40), &im))
    {
      for (i = 0; i < im.count; ++i)
      {
        UP s = (UP)im.sections + i * 40;
        UP p = im.base + r32 (s + 12), end = p + (r32 (s + 8) & ~3U);
        if (!(r32 (s + 36) & EXECUTE))
          continue;
        for (; p < end; p += 4)
        {
          U32 word = r32 (p), op = word & 0xffffffe0U, replacement = word;
          if (op == 0xd5381020U || op == 0xd53bd2a0U || op == 0xd53bd2c0U)
            replacement = 0xd2800000U | (word & 31); /* MOVZ Xt,#0 */
          else if (op == 0xd5181020U || op == 0xd51bd2a0U || op == 0xd51bd2c0U)
            replacement = 0xd503201fU; /* NOP */
          else if (word == 0xd518cba8U || word == 0xd518cbaaU)
          {
            sgi (&im, p, word);
            changed |= r32 (p) != word;
          }
          if (replacement != word)
          {
            *(U32 *)p = replacement;
            changed = 1;
          }
        }
      }
      if (changed)
      {
        for (i = 0; i < im.count; ++i)
        {
          UP s = (UP)im.sections + i * 40;
          UP start = im.base + r32 (s + 12);
          U32 length = r32 (s + 8), raw = r32 (s + 16);
          UP end;
          if (raw > length)
            length = raw;
          end = (start + length + PAGE - 1) & ~(UP)(PAGE - 1);
          if (r32 (s + 36) & EXECUTE)
            synchronize (start, end);
        }
      }
    }
    entry = rptr (entry);
  }
}
