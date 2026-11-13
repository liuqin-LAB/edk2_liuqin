/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <MemoryPlan.h>

typedef unsigned char U8;
typedef struct
{
  const U8 *Reg;
  LIUQIN_U32 RegLength, Ac, Sc, ParentAc, ParentSc, Offset, Seen, Phandle, LinuxPhandle;
  LIUQIN_U64 DynamicSize, InitrdStart, InitrdEnd;
  int Memory, ReservedParent, Reservation, Chosen, Children;
  int SharedPool, NoMap, Reusable, Enabled;
  int ExactMemory, EmptyEbuff, SeenEbuff;
  int UnknownProperty, MetadataInvalid, StatusKnown;
} NODE;
typedef struct
{
  LIUQIN_U32 Count;
  LIUQIN_MEMORY_RANGE Ranges[LIUQIN_MEMORY_MAX_RANGES];
} RANGES;

#define SEEN_AC (1U << 0)
#define SEEN_SC (1U << 1)
#define SEEN_REG (1U << 2)
#define SEEN_SIZE (1U << 3)
#define SEEN_COMPATIBLE (1U << 4)
#define SEEN_NO_MAP (1U << 5)
#define SEEN_REUSABLE (1U << 6)
#define SEEN_TYPE (1U << 7)
#define SEEN_RANGES (1U << 8)
#define SEEN_START (1U << 9)
#define SEEN_END (1U << 10)
#define SEEN_STATUS (1U << 11)
#define SEEN_PHANDLE (1U << 12)
#define SEEN_LPHANDLE (1U << 13)
#define SEEN_ALLOC (1U << 14)
#define SEEN_ALIGNMENT (1U << 15)
#define SEEN_MEM_TYPE (1U << 16)
#define PAGE_MASK (LIUQIN_MEMORY_PAGE_SIZE - 1ULL)

static LIUQIN_U32 Be32 (const U8 *P)
{
  return (LIUQIN_U32)P[0] << 24 | (LIUQIN_U32)P[1] << 16 | (LIUQIN_U32)P[2] << 8 | P[3];
}
static LIUQIN_U64 Be64 (const U8 *P)
{
  return (LIUQIN_U64)Be32 (P) << 32 | Be32 (P + 4);
}
static int Equal (const char *A, const char *B)
{
  while (*A && *A == *B)
  {
    ++A;
    ++B;
  }
  return *A == *B;
}
static int Prefix (const char *A, const char *B)
{
  while (*B)
  {
    if (*A++ != *B++)
      return 0;
  }
  return 1;
}
static int StringListHas (const U8 *Data, LIUQIN_U32 Length, const char *Value)
{
  LIUQIN_U32 At = 0;
  if (!Length || Data[Length - 1])
    return -1;
  while (At < Length)
  {
    if (Equal ((const char *)Data + At, Value))
      return 1;
    while (At < Length && Data[At])
      ++At;
    ++At;
  }
  return 0;
}
static int Once (NODE *Node, LIUQIN_U32 Bit)
{
  if (Node->Seen & Bit)
    return 0;
  Node->Seen |= Bit;
  return 1;
}
static int SingleString (const U8 *Data, LIUQIN_U32 Length)
{
  LIUQIN_U32 Index;
  if (!Length || Data[Length - 1])
    return 0;
  for (Index = 0; Index + 1 < Length; ++Index)
    if (!Data[Index])
      return 0;
  return 1;
}
static int Fail (LIUQIN_MEMORY_PLAN *Plan, int Reason, LIUQIN_U32 Offset)
{
  Plan->Reason = (LIUQIN_U32)Reason;
  Plan->NodeOffset = Offset;
  return Reason;
}

/* Insert into an already sorted union; duplicates/overlap/adjacency consume
 * no extra capacity. A 257th distinct interval is rejected before any write. */
static int Insert (RANGES *Set, LIUQIN_U64 Base, LIUQIN_U64 End)
{
  LIUQIN_U32 First = 0, Last, Index, Removed;
  while (First < Set->Count && Set->Ranges[First].Base + Set->Ranges[First].Size < Base)
    ++First;
  Last = First;
  while (Last < Set->Count && Set->Ranges[Last].Base <= End)
  {
    if (Set->Ranges[Last].Base < Base)
      Base = Set->Ranges[Last].Base;
    if (Set->Ranges[Last].Base + Set->Ranges[Last].Size > End)
      End = Set->Ranges[Last].Base + Set->Ranges[Last].Size;
    ++Last;
  }
  Removed = Last - First;
  if (!Removed && Set->Count == LIUQIN_MEMORY_MAX_RANGES)
    return LiuqinMemoryCapacity;
  if (!Removed)
  {
    for (Index = Set->Count; Index > First; --Index)
      Set->Ranges[Index] = Set->Ranges[Index - 1];
    ++Set->Count;
  }
  else
  {
    for (Index = Last; Index < Set->Count; ++Index)
      Set->Ranges[Index - Removed + 1] = Set->Ranges[Index];
    Set->Count -= Removed - 1;
  }
  Set->Ranges[First].Base = Base;
  Set->Ranges[First].Size = End - Base;
  return LiuqinMemoryOk;
}

static int Add (RANGES *Set, LIUQIN_U64 Base, LIUQIN_U64 Size, int Exclude)
{
  LIUQIN_U64 End;
  if (!Size)
    return LiuqinMemoryOk;
  if (Size > ~Base)
    return Exclude ? LiuqinMemoryReservationOverflow : LiuqinMemoryRamOverflow;
  End = Base + Size;
  if (Exclude)
  {
    Base &= ~PAGE_MASK;
    if (End & PAGE_MASK)
    {
      if (End > ~PAGE_MASK)
        return LiuqinMemoryAlignmentOverflow;
      End = (End + PAGE_MASK) & ~PAGE_MASK;
    }
  }
  else
  {
    if (Base & PAGE_MASK)
    {
      if (Base > ~PAGE_MASK)
        return LiuqinMemoryAlignmentOverflow;
      Base = (Base + PAGE_MASK) & ~PAGE_MASK;
    }
    End &= ~PAGE_MASK;
  }
  return Base < End ? Insert (Set, Base, End) : LiuqinMemoryOk;
}

static int Covered (const RANGES *Ram, LIUQIN_U64 Base, LIUQIN_U64 Size)
{
  LIUQIN_U32 Index;
  for (Index = 0; Index < Ram->Count; ++Index)
  {
    if (Base >= Ram->Ranges[Index].Base && Size <= Ram->Ranges[Index].Size &&
        Base - Ram->Ranges[Index].Base <= Ram->Ranges[Index].Size - Size)
      return 1;
  }
  return 0;
}
static int Intersects (const RANGES *Set, LIUQIN_U64 Base, LIUQIN_U64 Size)
{
  LIUQIN_U32 Index;
  for (Index = 0; Index < Set->Count; ++Index)
    if (Base < Set->Ranges[Index].Base + Set->Ranges[Index].Size &&
        Set->Ranges[Index].Base < Base + Size)
      return 1;
  return 0;
}

/* Only used by resource-free nodes. The DT was
 * structurally validated; this pass uses only a bounded node-offset stack.
 * A target must identify one node; matching legacy aliases in that node are OK. */
static int UniqueUnconfiguredPhandle (const U8 *Blob, LIUQIN_U32 Phandle)
{
  LIUQIN_U32 Off = Be32 (Blob + 8), Strings = Be32 (Blob + 12);
  LIUQIN_U32 Token, Length, Value, MatchNode = 0xFFFFFFFFU, Nodes[32];
  int Depth = -1;
  const char *Key;
  for (;;)
  {
    Token = Be32 (Blob + Off);
    Off += 4;
    if (Token == 1)
    {
      Nodes[++Depth] = Off - 4;
      while (Blob[Off])
        ++Off;
      Off = (Off + 4) & ~3U;
    }
    else if (Token == 3)
    {
      Length = Be32 (Blob + Off);
      Key = (const char *)Blob + Strings + Be32 (Blob + Off + 4);
      Off += 8;
      if (Equal (Key, "phandle") || Equal (Key, "linux,phandle"))
      {
        if (Length != 4)
          return 0;
        Value = Be32 (Blob + Off);
        if (!Value || Value == 0xFFFFFFFFU)
          return 0;
        if (Value == Phandle)
        {
          if (MatchNode != 0xFFFFFFFFU && MatchNode != Nodes[Depth])
            return 0;
          MatchNode = Nodes[Depth];
        }
      }
      Off += (Length + 3) & ~3U;
    }
    else if (Token == 2)
      --Depth;
    else if (Token == 9)
      return MatchNode != 0xFFFFFFFFU;
  }
}

static int FinishNode (const NODE *Node, RANGES *Ram, RANGES *Excluded, const U8 *Blob,
                       LIUQIN_MEMORY_RANGE *Initrd)
{
  LIUQIN_U32 At, Stride;
  LIUQIN_U64 Base, Size;
  int Error, Physical = 0;
  if (Node->Memory || Node->Reservation)
  {
    if (Node->ParentAc < 1 || Node->ParentAc > 2 || Node->ParentSc < 1 || Node->ParentSc > 2)
      return LiuqinMemoryInvalidCells;

    if (Node->Children && !(Node->Memory && Node->ExactMemory && Node->SeenEbuff))
      return LiuqinMemoryInvalidProperty;
    if (Node->Seen & SEEN_REG)
    {
      Stride = (Node->ParentAc + Node->ParentSc) * 4;
      if (!Node->RegLength || Node->RegLength % Stride)
        return LiuqinMemoryInvalidReg;
      for (At = 0; At < Node->RegLength; At += Stride)
      {
        Base = Node->ParentAc == 2 ? Be64 (Node->Reg + At) : Be32 (Node->Reg + At);
        Size = Node->ParentSc == 2 ? Be64 (Node->Reg + At + Node->ParentAc * 4)
                                   : Be32 (Node->Reg + At + Node->ParentAc * 4);
        if (Size)
          Physical = 1;
        Error = Node->Memory && !Node->Enabled
                    ? LiuqinMemoryOk
                    : Add (Node->Memory ? Ram : Excluded, Base, Size, Node->Reservation);
        if (Error)
          return Error;
      }
    }
    else if (Node->Memory)
      return LiuqinMemoryInvalidReg;
    if (Node->Reservation && !Physical)
    {
      /* reg has precedence over size. An explicitly empty physical range
       * cannot be silently reinterpreted as a not-yet-allocated CMA pool. */
      if (Node->Seen & SEEN_REG)
        return LiuqinMemoryUnknownReservation;
      /* Linux's reserved-memory scanner creates neither a physical resource
       * nor a dynamic allocation when both reg and size are absent. Classify
       * by resource attributes, never by name/unit address. Unknown vendor
       * attributes may encode an independent contract and still fail closed.
       * Present zero-size/invalid reg and present zero size do not qualify. */
      if (!(Node->Seen & (SEEN_REG | SEEN_SIZE)))
      {
        if (Node->UnknownProperty)
          return LiuqinMemoryUnknownReservation;
        if (Node->MetadataInvalid || !Node->StatusKnown || (Node->NoMap && Node->Reusable) ||
            ((Node->Seen & SEEN_PHANDLE) && (Node->Seen & SEEN_LPHANDLE) &&
             Node->Phandle != Node->LinuxPhandle))
          return LiuqinMemoryInvalidProperty;
        if ((Node->Seen & (SEEN_PHANDLE | SEEN_LPHANDLE)) &&
            !UniqueUnconfiguredPhandle (Blob, (Node->Seen & SEEN_PHANDLE) ? Node->Phandle
                                                                          : Node->LinuxPhandle))
          return LiuqinMemoryInvalidProperty;
        return LiuqinMemoryOk;
      }
      if (!(Node->Seen & SEEN_SIZE) || !Node->DynamicSize)
        return LiuqinMemoryUnknownReservation;
      /* Linux allocates these unplaced reusable pools later. No fixed physical
       * range exists to export/subtract yet. Every other unplaced node blocks
       * broad RAM publication, including no-map and disabled nodes. */
      if (!Node->SharedPool || !Node->Reusable || Node->NoMap)
        return LiuqinMemoryUnplacedReservation;
    }
  }
  if (Node->Chosen)
  {
    if (!!(Node->Seen & SEEN_START) != !!(Node->Seen & SEEN_END) ||
        ((Node->Seen & SEEN_START) && Node->InitrdEnd < Node->InitrdStart))
      return LiuqinMemoryInvalidProperty;
    if (Node->Seen & SEEN_START)
    {
      if (Initrd != 0)
      {
        Initrd->Base = Node->InitrdStart;
        Initrd->Size = Node->InitrdEnd - Node->InitrdStart;
      }
      else
      {
        Error = Add (Excluded, Node->InitrdStart, Node->InitrdEnd - Node->InitrdStart, 1);
        if (Error)
          return Error;
      }
    }
  }
  return LiuqinMemoryOk;
}

static int BuildMemoryPlan (const void *Dt, LIUQIN_SIZE Available,
                            const LIUQIN_MEMORY_CONTEXT *Context, LIUQIN_MEMORY_PLAN *Plan,
                            LIUQIN_MEMORY_RANGE *Initrd)
{
  const U8 *Data, *Blob = Dt;
  const char *Name, *Key;
  NODE Nodes[32], *Node;
  RANGES Ram, Excluded, Free;
  LIUQIN_U32 Off, Strings, Token, Length, Reserve, Index, ExIndex, NodeOffset;
  LIUQIN_U64 Cursor, End, ExEnd, Total = 0;
  int Depth = -1, Error, FoundReserved = 0, FoundChosen = 0, FoundEbuff = 0;
  LIUQIN_U32 UnconfiguredCount = 0;
  int Has;
  if (!Plan)
    return LiuqinMemoryInvalidArgument;
  Plan->Reason = LiuqinMemoryOk;
  Plan->NodeOffset = 0;
  Plan->Count = 0;
  Plan->Reserved = 0;
  Plan->TotalBytes = 0;
  for (Index = 0; Index < LIUQIN_MEMORY_MAX_RANGES; ++Index)
  {
    Plan->Ranges[Index].Base = 0;
    Plan->Ranges[Index].Size = 0;
  }
  if (!Context)
    return Fail (Plan, LiuqinMemoryInvalidArgument, 0);
  if (LiuqinValidateDtb (Dt, Available))
    return Fail (Plan, LiuqinMemoryInvalidDtb, 0);
  Ram.Count = 0;
  Excluded.Count = 0;
  Free.Count = 0;
  Reserve = Be32 (Blob + 16);
  while (Be64 (Blob + Reserve + 8))
  {
    Error = Add (&Excluded, Be64 (Blob + Reserve), Be64 (Blob + Reserve + 8), 1);
    if (Error)
      return Fail (Plan, Error, 0);
    Reserve += 16;
  }
  Strings = Be32 (Blob + 12);
  Off = Be32 (Blob + 8);
  for (;;)
  {
    NodeOffset = Off - Be32 (Blob + 8);
    Token = Be32 (Blob + Off);
    Off += 4;
    if (Token == 1)
    {
      Name = (const char *)Blob + Off;
      Has = 0;
      if (Depth >= 0)
      {
        Node = &Nodes[Depth];
        if (Node->EmptyEbuff)
          return Fail (Plan, LiuqinMemoryInvalidProperty, Node->Offset);
        Node->Children = 1;
        if (Node->Memory)
        {
          if (!Node->ExactMemory || !Equal (Name, "ebuff") || Node->SeenEbuff || FoundEbuff)
            return Fail (Plan, LiuqinMemoryInvalidProperty, Node->Offset);
          Node->SeenEbuff = 1;
          FoundEbuff = 1;
          Has = 1;
        }
      }
      ++Depth;
      Node = &Nodes[Depth];
      Node->Reg = 0;
      Node->RegLength = 0;
      Node->Seen = 0;
      Node->Phandle = 0;
      Node->LinuxPhandle = 0;
      Node->UnknownProperty = 0;
      Node->MetadataInvalid = 0;
      Node->StatusKnown = 1;
      Node->Offset = NodeOffset;
      Node->Ac = 2;
      Node->Sc = 1;
      Node->ParentAc = Depth ? Nodes[Depth - 1].Ac : 2;
      Node->ParentSc = Depth ? Nodes[Depth - 1].Sc : 1;
      Node->DynamicSize = 0;
      Node->InitrdStart = 0;
      Node->InitrdEnd = 0;
      Node->Children = 0;
      Node->SharedPool = 0;
      Node->NoMap = 0;
      Node->Reusable = 0;
      Node->Enabled = 1;
      Node->Memory = Depth == 1 && (Equal (Name, "memory") || Prefix (Name, "memory@"));
      Node->ExactMemory = Depth == 1 && Equal (Name, "memory");
      Node->EmptyEbuff = Has;
      Node->SeenEbuff = 0;
      Node->ReservedParent = Depth == 1 && Equal (Name, "reserved-memory");
      Node->Reservation = Depth > 1 && Nodes[Depth - 1].ReservedParent;
      Node->Chosen = Depth == 1 && Equal (Name, "chosen");
      if ((Node->ReservedParent && FoundReserved++) || (Node->Chosen && FoundChosen++))
        return Fail (Plan, LiuqinMemoryInvalidProperty, NodeOffset);
      while (Blob[Off])
        ++Off;
      Off = (Off + 4) & ~3U;
    }
    else if (Token == 3)
    {
      Node = &Nodes[Depth];
      Length = Be32 (Blob + Off);
      Key = (const char *)Blob + Strings + Be32 (Blob + Off + 4);
      Off += 8;
      Data = Blob + Off;
      Off += (Length + 3) & ~3U;
      if (Node->EmptyEbuff || Node->Children)
        return Fail (Plan, LiuqinMemoryInvalidProperty, Node->Offset);
      Error = LiuqinMemoryOk;
      if (Equal (Key, "#address-cells") || Equal (Key, "#size-cells"))
      {
        if (Node->Reservation)
          Node->UnknownProperty = 1;
        Has = Equal (Key, "#address-cells");
        if (Length != 4 || !Once (Node, Has ? SEEN_AC : SEEN_SC))
          Error = LiuqinMemoryInvalidCells;
        else
        {
          if (Has)
            Node->Ac = Be32 (Data);
          else
            Node->Sc = Be32 (Data);
          if ((Depth == 0 || Node->ReservedParent) && (Be32 (Data) < 1 || Be32 (Data) > 2))
            Error = LiuqinMemoryInvalidCells;
        }
      }
      else if ((Equal (Key, "phandle") || Equal (Key, "linux,phandle")) && Node->Reservation)
      {
        Has = Equal (Key, "phandle");
        if (!Once (Node, Has ? SEEN_PHANDLE : SEEN_LPHANDLE) || Length != 4 || !Be32 (Data) ||
            Be32 (Data) == 0xFFFFFFFFU)
          Error = LiuqinMemoryInvalidProperty;
        else if (Has)
          Node->Phandle = Be32 (Data);
        else
          Node->LinuxPhandle = Be32 (Data);
      }
      else if (Equal (Key, "reg") && Depth > 0 && (Depth == 1 || Node->Reservation))
      {
        if (!Once (Node, SEEN_REG))
          Error = LiuqinMemoryInvalidReg;
        else
        {
          Node->Reg = Data;
          Node->RegLength = Length;
        }
      }
      else if (Equal (Key, "device_type") && Depth == 1)
      {
        if (!Once (Node, SEEN_TYPE) || !SingleString (Data, Length))
          Error = LiuqinMemoryInvalidProperty;
        else if (Length == 7 && Equal ((const char *)Data, "memory"))
          Node->Memory = 1;
        else if (Node->Memory)
          Error = LiuqinMemoryInvalidProperty;
      }
      else if (Equal (Key, "status") && (Depth == 1 || Node->Reservation))
      {
        if (!Once (Node, SEEN_STATUS) || !SingleString (Data, Length))
          Error = LiuqinMemoryInvalidProperty;
        else
        {
          Node->Enabled = Equal ((const char *)Data, "okay") || Equal ((const char *)Data, "ok");
          Node->StatusKnown = Node->Enabled || Equal ((const char *)Data, "disabled");
        }
      }
      else if (Equal (Key, "ranges") && Node->ReservedParent)
      {
        if (!Once (Node, SEEN_RANGES) || Length)
          Error = LiuqinMemoryInvalidProperty;
      }
      else if (Equal (Key, "size") && Node->Reservation)
      {
        if (!Once (Node, SEEN_SIZE) || (Node->ParentSc != 1 && Node->ParentSc != 2) ||
            Length != Node->ParentSc * 4)
          Error = LiuqinMemoryInvalidProperty;
        else
          Node->DynamicSize = Length == 8 ? Be64 (Data) : Be32 (Data);
      }
      else if (Equal (Key, "compatible") && Node->Reservation)
      {
        Has = StringListHas (Data, Length, "shared-dma-pool");
        if (!Once (Node, SEEN_COMPATIBLE) || Has < 0)
          Error = LiuqinMemoryInvalidProperty;
        else
          Node->SharedPool = Has;
      }
      else if ((Equal (Key, "no-map") || Equal (Key, "reusable")) && Node->Reservation)
      {
        Has = Equal (Key, "no-map");
        if (!Once (Node, Has ? SEEN_NO_MAP : SEEN_REUSABLE) || Length)
          Error = LiuqinMemoryInvalidProperty;
        else if (Has)
          Node->NoMap = 1;
        else
          Node->Reusable = 1;
      }
      else if (Equal (Key, "alloc-ranges") && Node->Reservation)
      {
        LIUQIN_U32 At, Stride = (Node->ParentAc + Node->ParentSc) * 4;
        LIUQIN_U64 Base, Bytes;
        if (!Once (Node, SEEN_ALLOC) || Node->ParentAc < 1 || Node->ParentAc > 2 ||
            Node->ParentSc < 1 || Node->ParentSc > 2 || !Length || Length % Stride)
          Node->MetadataInvalid = 1;
        else
          for (At = 0; At < Length; At += Stride)
          {
            Base = Node->ParentAc == 2 ? Be64 (Data + At) : Be32 (Data + At);
            Bytes = Node->ParentSc == 2 ? Be64 (Data + At + Node->ParentAc * 4)
                                        : Be32 (Data + At + Node->ParentAc * 4);
            if (!Bytes || Base > ~0ULL - Bytes)
              Node->MetadataInvalid = 1;
          }
      }
      else if (Equal (Key, "alignment") && Node->Reservation)
      {
        LIUQIN_U64 Alignment;
        if (!Once (Node, SEEN_ALIGNMENT) || (Node->ParentSc != 1 && Node->ParentSc != 2) ||
            Length != Node->ParentSc * 4)
          Node->MetadataInvalid = 1;
        else
        {
          Alignment = Length == 8 ? Be64 (Data) : Be32 (Data);
          if (!Alignment || (Alignment & (Alignment - 1)))
            Node->MetadataInvalid = 1;
        }
      }
      else if (Equal (Key, "mem-type") && Node->Reservation)
      {
        if (!Once (Node, SEEN_MEM_TYPE) || Length != 4 || Be32 (Data) > 2)
          Node->MetadataInvalid = 1;
      }
      else if (Node->Chosen &&
               (Equal (Key, "linux,initrd-start") || Equal (Key, "linux,initrd-end")))
      {
        Has = Equal (Key, "linux,initrd-start");
        if (!Once (Node, Has ? SEEN_START : SEEN_END) || (Length != 4 && Length != 8))
          Error = LiuqinMemoryInvalidProperty;
        else if (Has)
          Node->InitrdStart = Length == 8 ? Be64 (Data) : Be32 (Data);
        else
          Node->InitrdEnd = Length == 8 ? Be64 (Data) : Be32 (Data);
      }
      else if (Node->Reservation)
        Node->UnknownProperty = 1;
      if (Error)
        return Fail (Plan, Error, Node->Offset);
    }
    else if (Token == 2)
    {
      Node = &Nodes[Depth--];
      /* Bound repeated full-tree phandle validation even for empty markers
       * that do not consume a physical interval in the range union. */
      if (Node->Reservation && !(Node->Seen & (SEEN_REG | SEEN_SIZE)) &&
          ++UnconfiguredCount > LIUQIN_MEMORY_MAX_RANGES)
        return Fail (Plan, LiuqinMemoryCapacity, Node->Offset);
      Error = FinishNode (Node, &Ram, &Excluded, Blob, Initrd);
      if (Error)
        return Fail (Plan, Error, Node->Offset);
    }
    else if (Token == 9)
      break;
    /* Token 4 (NOP) has no semantic effect; structural validation already
     * proved all offsets, property strings, nesting and the END token. */
  }
  if (!Ram.Count)
    return Fail (Plan, LiuqinMemoryNoRam, 0);
  Error = Add (&Excluded, Context->OriginalDtbBase, LiuqinDtbSize (Dt), 1);
  if (!Error)
    Error = Add (&Excluded, Context->Framebuffer.Base, Context->Framebuffer.Size, 1);
  if (Error)
    return Fail (Plan, Error, 0);
  if (!Covered (&Ram, LIUQIN_FD_BASE, LIUQIN_FD_SIZE))
    return Fail (Plan, LiuqinMemoryFdNotRam, 0);
  if (!Covered (&Ram, LIUQIN_HEAP_BASE, LIUQIN_HEAP_SIZE))
    return Fail (Plan, LiuqinMemoryPoolNotRam, 0);
  if (Intersects (&Excluded, LIUQIN_FD_BASE, LIUQIN_FD_SIZE))
    return Fail (Plan, LiuqinMemoryFdReserved, 0);
  if (Intersects (&Excluded, LIUQIN_HEAP_BASE, LIUQIN_HEAP_SIZE))
    return Fail (Plan, LiuqinMemoryPoolReserved, 0);
  /* The entire Android kernel load window retains BootShim/source ownership.
   * FD and pool lie inside it by design and are separately published by SEC. */
  Error = Add (&Excluded, LIUQIN_KERNEL_BASE, LIUQIN_KERNEL_SIZE, 1);
  if (!Error)
    Error = Add (&Excluded, LIUQIN_FD_BASE, LIUQIN_FD_SIZE, 1);
  if (!Error)
    Error = Add (&Excluded, LIUQIN_HEAP_BASE, LIUQIN_HEAP_SIZE, 1);
  if (Error)
    return Fail (Plan, Error, 0);
  for (Index = 0; Index < Ram.Count; ++Index)
  {
    Cursor = Ram.Ranges[Index].Base;
    End = Cursor + Ram.Ranges[Index].Size;
    for (ExIndex = 0; ExIndex < Excluded.Count && Cursor < End; ++ExIndex)
    {
      ExEnd = Excluded.Ranges[ExIndex].Base + Excluded.Ranges[ExIndex].Size;
      if (ExEnd <= Cursor)
        continue;
      if (Excluded.Ranges[ExIndex].Base >= End)
        break;
      if (Excluded.Ranges[ExIndex].Base > Cursor)
      {
        Error = Insert (&Free, Cursor, Excluded.Ranges[ExIndex].Base);
        if (Error)
          return Fail (Plan, Error, 0);
      }
      if (ExEnd > Cursor)
        Cursor = ExEnd;
    }
    if (Cursor < End)
    {
      Error = Insert (&Free, Cursor, End);
      if (Error)
        return Fail (Plan, Error, 0);
    }
  }
  if (!Free.Count)
    return Fail (Plan, LiuqinMemoryEmptyPlan, 0);
  for (Index = 0; Index < Free.Count; ++Index)
  {
    Total += Free.Ranges[Index].Size;
    Plan->Ranges[Index] = Free.Ranges[Index];
  }
  Plan->TotalBytes = Total;
  Plan->Count = Free.Count;
  return LiuqinMemoryOk;
}

int LiuqinBuildMemoryPlan (const void *Dt, LIUQIN_SIZE Available,
                           const LIUQIN_MEMORY_CONTEXT *Context, LIUQIN_MEMORY_PLAN *Plan)
{
  return BuildMemoryPlan (Dt, Available, Context, Plan, 0);
}

int LiuqinReserveMemoryRange (LIUQIN_MEMORY_PLAN *Plan, LIUQIN_U64 Base, LIUQIN_U64 Size)
{
  RANGES Free = {0};
  LIUQIN_U64 End, Start, Limit, Previous = 0, Total = 0, Remaining = 0;
  LIUQIN_U32 Index;
  int Error;
  if (!Plan || Plan->Reason != LiuqinMemoryOk || !Plan->Count ||
      Plan->Count > LIUQIN_MEMORY_MAX_RANGES || !Size)
    return LiuqinMemoryInvalidArgument;
  if (Base > ~0ULL - Size)
    return LiuqinMemoryReservationOverflow;
  End = Base + Size;
  if (End > ~PAGE_MASK)
    return LiuqinMemoryAlignmentOverflow;
  Base &= ~PAGE_MASK;
  End = (End + PAGE_MASK) & ~PAGE_MASK;
  for (Index = 0; Index < Plan->Count; ++Index)
  {
    Start = Plan->Ranges[Index].Base;
    Size = Plan->Ranges[Index].Size;
    if (!Size || ((Start | Size) & PAGE_MASK) || Start > ~0ULL - Size ||
        (Index && Start <= Previous) || Total > ~0ULL - Size)
      return LiuqinMemoryInvalidArgument;
    Limit = Start + Size;
    Previous = Limit;
    Total += Size;
    if (Limit <= Base || Start >= End)
      Error = Insert (&Free, Start, Limit);
    else
    {
      Error = Start < Base ? Insert (&Free, Start, Base) : 0;
      if (!Error && Limit > End)
        Error = Insert (&Free, End, Limit);
    }
    if (Error)
      return Error;
  }
  if (Total != Plan->TotalBytes)
    return LiuqinMemoryInvalidArgument;
  if (!Free.Count)
    return LiuqinMemoryEmptyPlan;
  for (Index = 0; Index < Free.Count; ++Index)
    Remaining += Free.Ranges[Index].Size;
  for (Index = 0; Index < LIUQIN_MEMORY_MAX_RANGES; ++Index)
  {
    Plan->Ranges[Index].Base = 0;
    Plan->Ranges[Index].Size = 0;
    if (Index < Free.Count)
      Plan->Ranges[Index] = Free.Ranges[Index];
  }
  Plan->Count = Free.Count;
  Plan->TotalBytes = Remaining;
  return LiuqinMemoryOk;
}
