/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AcpiPcieHandoff.h"
#define ROOT 0x60000000U
#define EP 0x60100000U
#define PARF 0x01C00000U
#define MEM_ATU 0x60200200U
#define RCG 0x0017B048U
#define RATE 0x0017B04CU
#define RATE_MASK ((1U << 20) | 0x3000U | 0x700U | 31U)
typedef struct
{
  CONST LIUQIN_PCIE_IO *Io;
  CONST LIUQIN_PCIE_CONFIG_IO *Cfg;
  LIUQIN_PCIE_HANDOFF_STATE *R;
} ACCESS;
STATIC BOOLEAN Bad (ACCESS *A, UINTN Address, UINT32 Value, UINT32 Expected)
{
  (VOID) A;
  (VOID) Address;
  (VOID) Value;
  (VOID) Expected;
  return FALSE;
}
STATIC BOOLEAN R32 (ACCESS *A, UINTN P, UINT32 *V)
{
  if (!A->Io->Read32 (A->Io->Context, P, V))
    return Bad (A, P, MAX_UINT32, 0);
  return TRUE;
}
STATIC BOOLEAN R16 (ACCESS *A, UINTN P, UINT16 *V)
{

  if (!A->Cfg->Read16 (A->Cfg->Context, P, V))
    return Bad (A, P, MAX_UINT16, 0);
  return TRUE;
}
STATIC BOOLEAN R8 (ACCESS *A, UINTN P, UINT8 *V)
{

  if (!A->Cfg->Read8 (A->Cfg->Context, P, V))
    return Bad (A, P, MAX_UINT8, 0);
  return TRUE;
}
STATIC BOOLEAN W32 (ACCESS *A, UINTN P, UINT32 V)
{

  return A->Io->Write32 (A->Io->Context, P, V) || Bad (A, P, MAX_UINT32, V);
}
STATIC BOOLEAN W16 (ACCESS *A, UINTN P, UINT16 V)
{

  return A->Cfg->Write16 (A->Cfg->Context, P, V) || Bad (A, P, MAX_UINT16, V);
}
STATIC BOOLEAN Eq32 (ACCESS *A, UINTN P, UINT32 Want)
{
  UINT32 V = MAX_UINT32;
  return R32 (A, P, &V) && (V == Want || Bad (A, P, V, Want));
}
STATIC BOOLEAN Set32 (ACCESS *A, UINTN P, UINT32 Want)
{
  return W32 (A, P, Want) && Eq32 (A, P, Want);
}
STATIC BOOLEAN Wait (ACCESS *A, UINTN P, UINT32 Mask, UINT32 Want)
{
  UINT32 V = MAX_UINT32;
  for (UINTN I = 0; I < 1000; ++I)
  {
    if (!R32 (A, P, &V))
      return FALSE;
    if ((V & Mask) == Want)
      return TRUE;
    A->Io->DelayUs (A->Io->Context, 1);
  }
  return Bad (A, P, V, Want);
}
STATIC BOOLEAN Capabilities (ACCESS *A)
{
  for (UINTN Bus = 0; Bus < 2; ++Bus)
  {
    UINTN Base = ROOT + (Bus << 20);
    UINT8 P;
    UINT64 Seen = 0;
    if (!R8 (A, Base + 0x34, &P))
      return FALSE;
    for (UINTN Count = 0; P && Count < 48; ++Count)
    {
      if (P < 0x40 || (P & 3U) || (Seen & ((UINT64)1 << (P / 4))))
        return Bad (A, Base + P, P, 0);
      Seen |= (UINT64)1 << (P / 4);
      UINT16 H, C;
      if (!R16 (A, Base + P, &H) || !R16 (A, Base + P + 2, &C))
        return FALSE;
      if ((H & 255U) == 0x10)
        A->R->PcieCap[Bus] = P;
      if ((H & 255U) == 5)
      {
        A->R->MsiControl[Bus] = C;
        if (C & 1U)
          return Bad (A, Base + P + 2, C, C & ~1U);
      }
      if ((H & 255U) == 0x11)
      {
        A->R->MsiXControl[Bus] = C;
        if (C & 0x8000U)
          return Bad (A, Base + P + 2, C, C & ~0x8000U);
      }
      P = (UINT8)(H >> 8);
    }
    if (P || !A->R->PcieCap[Bus])
      return Bad (A, Base + 0x34, P, 0);
  }
  return TRUE;
}
STATIC EFI_STATUS Memory (ACCESS *A, UINT32 *RootCommand, UINT32 *Bridge, BOOLEAN *Dirty)
{
  if (!R32 (A, ROOT + 4, RootCommand) || (*RootCommand & 4U) || !R32 (A, ROOT + 0x20, Bridge))
    return EFI_UNSUPPORTED;
  for (UINTN I = 0; I < 7; ++I)
    if (!R32 (A, MEM_ATU + I * 4, &A->R->MemoryAtu[I]))
      return EFI_DEVICE_ERROR;
  if (A->R->MemoryAtu[1] & (1U << 31))
    return EFI_UNSUPPORTED;
  *Dirty = TRUE;
  STATIC CONST UINT32 Payload[] = {0, 0x80000000, 0x60300000, 0, 0x63FFFFFF, 0x60300000, 0};
  if (!Set32 (A, MEM_ATU + 4, 0))
    return EFI_DEVICE_ERROR;
  for (UINTN I = 0; I < 7; ++I)
    if (I != 1 && !Set32 (A, MEM_ATU + I * 4, Payload[I]))
      return EFI_DEVICE_ERROR;
  if (!Set32 (A, MEM_ATU + 4, Payload[1]) || !Set32 (A, ROOT + 0x20, 0x63F06030) ||
      !W16 (A, ROOT + 4, (UINT16)(*RootCommand | 2U)) || !Eq32 (A, ROOT + 4, *RootCommand | 2U))
    return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
STATIC BOOLEAN RestoreMemory (ACCESS *A, UINT32 RootCommand, UINT32 Bridge)
{
  if (!Set32 (A, MEM_ATU + 4, 0))
    return FALSE;
  BOOLEAN Clean = TRUE;
  for (UINTN I = 0; I < 7; ++I)
    if (I != 1 && !Set32 (A, MEM_ATU + I * 4, A->R->MemoryAtu[I]))
      Clean = FALSE;
  if (!Set32 (A, ROOT + 0x20, Bridge) || !W16 (A, ROOT + 4, (UINT16)RootCommand) ||
      !Eq32 (A, ROOT + 4, RootCommand))
    Clean = FALSE;
  if (!Clean || !Set32 (A, MEM_ATU + 4, A->R->MemoryAtu[1]))
    return FALSE;
  return TRUE;
}
STATIC UINT8 Hash (UINT16 Rid)
{
  UINT32 H = 0;
  for (INTN B = 15; B >= 0; --B)
  {
    UINT32 F = ((H >> 7) ^ ((UINT32)Rid >> B)) & 1U;
    H = ((H << 1) ^ (F ? 7U : 0U)) & 255U;
  }
  return (UINT8)H;
}
STATIC EFI_STATUS Sid (ACCESS *A, UINT32 Saved[256], BOOLEAN *Dirty)
{
  LIUQIN_PCIE_HANDOFF_STATE *R = A->R;
  UINT32 RootCmd;
  if (!Eq32 (A, EP + 4, 0x00100000) || !R32 (A, ROOT + 4, &RootCmd) || (RootCmd & 4U) ||
      !R32 (A, 0x15000020, &R->SmmuId[0]) || !R32 (A, 0x15000024, &R->SmmuId[1]) ||
      R->SmmuId[0] != 0x4C017E71 || R->SmmuId[1] != 0x6000004B)
    return EFI_UNSUPPORTED;
  if (!R32 (A, PARF + 0x2C00, &R->SidBefore) || R->SidBefore == MAX_UINT32)
    return EFI_DEVICE_ERROR;
  for (UINTN I = 0; I < 256; ++I)
    if (!R32 (A, PARF + 0x2000 + I * 4, &Saved[I]))
      return EFI_DEVICE_ERROR;
  *Dirty = TRUE;
  // Controller is owned, both BME bits are clear, and only two RIDs exist.
  // Translation table value is BDF[31:16]|localSID[15:8]|next[7:0].
  if (!Set32 (A, PARF + 0x2C00, R->SidBefore | 1U))
    return EFI_DEVICE_ERROR;
  for (UINTN I = 0; I < 256; ++I)
    if (!Set32 (A, PARF + 0x2000 + I * 4, I == Hash (0x100) ? 0x01000100U : 0))
      return EFI_DEVICE_ERROR;
  if (!Set32 (A, PARF + 0x2C00, R->SidBefore & ~1U))
    return EFI_DEVICE_ERROR;
  for (UINTN I = 0; I < 256; ++I)
  {
    if (!Eq32 (A, PARF + 0x2000 + I * 4, I == Hash (0x100) ? 0x01000100U : 0))
      return EFI_DEVICE_ERROR;
  }
  if (!R32 (A, PARF + 0x2C00, &R->SidAfter) ||
      !R32 (A, PARF + 0x2000 + Hash (0) * 4, &R->SidRoot) ||
      !R32 (A, PARF + 0x2000 + Hash (0x100) * 4, &R->SidEndpoint))
    return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
STATIC BOOLEAN RestoreSid (ACCESS *A, CONST UINT32 Saved[256])
{
  if (!Set32 (A, PARF + 0x2C00, A->R->SidBefore | 1U))
    return FALSE;
  BOOLEAN Clean = TRUE;
  for (UINTN I = 0; I < 256; ++I)
    if (!Set32 (A, PARF + 0x2000 + I * 4, Saved[I]))
      Clean = FALSE;
  if (!Clean || !Set32 (A, PARF + 0x2C00, A->R->SidBefore))
    return FALSE;
  return TRUE;
}
STATIC BOOLEAN LatchRate (ACCESS *A, UINT32 Config, UINT32 Command)
{
  if (!W32 (A, RCG, Command | 2U) || !Wait (A, RCG, 2, 2) || !Wait (A, RCG, 1U << 31, 0) ||
      !W32 (A, RATE, Config) || !Eq32 (A, RATE, Config) || !W32 (A, RCG, Command | 3U) ||
      !Wait (A, RCG, 1, 0) || !W32 (A, RCG, Command & ~1U) || !Wait (A, RCG, 3, Command & 2U))
    return FALSE;
  return Wait (A, 0x17B044, 1U << 31, 0);
}
STATIC BOOLEAN Train (ACCESS *A, UINT16 Target, UINT32 *Link)
{
  UINTN Cap = ROOT + A->R->PcieCap[0];
  UINT16 Control, Control2;
  if (!R16 (A, Cap + 0x30, &Control2) ||
      !W16 (A, Cap + 0x30, (UINT16)((Control2 & ~15U) | Target)) ||
      !R16 (A, Cap + 0x10, &Control) || !W16 (A, Cap + 0x10, (UINT16)(Control | 0x20U)))
    return FALSE;
  for (UINTN I = 0; I < 100; ++I)
  {
    A->Io->DelayUs (A->Io->Context, 5000);
    if (!R32 (A, Cap + 0x10, Link))
      return FALSE;
    if ((*Link & (1U << 29)) && !(*Link & (1U << 27)) && ((*Link >> 16) & 15U) == Target &&
        ((*Link >> 20) & 63U) == 1)
      return TRUE;
  }
  return Bad (A, Cap + 0x10, *Link, (UINT32)Target << 16);
}
STATIC EFI_STATUS Speed (ACCESS *A, UINT32 *Rcg, UINT16 *Target, BOOLEAN *Dirty)
{
  LIUQIN_PCIE_HANDOFF_STATE *R = A->R;
  UINT32 V, Cap;
  for (UINTN Bus = 0; Bus < 2; ++Bus)
  {
    UINTN P = ROOT + (Bus << 20) + R->PcieCap[Bus] + 0xC;
    if (!R32 (A, P, &Cap))
      return EFI_DEVICE_ERROR;
    if ((Cap & 15U) < 3)
      return Bad (A, P, Cap, 3), EFI_UNSUPPORTED;
  }
  if (!R32 (A, ROOT + R->PcieCap[0] + 0x10, &R->SpeedBefore) || !R32 (A, RCG, Rcg) ||
      !R32 (A, RATE, &R->RateBefore) || !R16 (A, ROOT + R->PcieCap[0] + 0x30, Target))
    return EFI_DEVICE_ERROR;
  if (*Rcg & 1U)
    return Bad (A, RCG, *Rcg, *Rcg & ~1U), EFI_UNSUPPORTED;
  // clk-rcg2.c calc_rate(): HID=0 bypasses division; HID=1 divides by1.
  // Accept both XO encodings and restore the exact original configuration.
  if ((R->RateBefore & RATE_MASK & ~1U) != (1U << 20))
    return Bad (A, RATE, R->RateBefore, 1U << 20), EFI_UNSUPPORTED;
  if ((*Target & 15U) < 1 || (*Target & 15U) > 2)
    return Bad (A, ROOT + R->PcieCap[0] + 0x30, *Target, 2), EFI_UNSUPPORTED;
  STATIC CONST UINT32 PllOffsets[] = {0, 4, 0x10, 0x14};
  for (UINTN I = 0; I < 4; ++I)
    if (!R32 (A, 0x100000 + PllOffsets[I], &R->Pll[I]))
      return EFI_DEVICE_ERROR;
  if (!R32 (A, 0x100018, &R->PllUser) || !R32 (A, 0x100008, &R->PllState) ||
      !R32 (A, 0x10000C, &R->PllStatus) || !R32 (A, 0x162018, &R->PllVote))
    return EFI_DEVICE_ERROR;
  // GPLL0 is shared: only consume an already locked/running 600MHz source.
  // Cape gcc_cape_fixup selects Lucid OLE. Its enable path uses USER_CTL
  // ENABLE_VOTE_RUN(bit25): in FSM mode it votes and waits for LOCK, without
  // setting OPMODE=RUN. OPMODE=0 is therefore not a standalone failure.
  // Require locked active global output in FSM mode; direct mode needs RUN.
  // No PLL votes, output controls or frequency registers are written here.
  R->PllFsm = (R->PllUser & (1U << 25)) != 0;
  V = (1U << 31) | 5U;
  if ((R->Pll[0] & V) != V)
    return Bad (A, 0x100000, R->Pll[0], V), EFI_UNSUPPORTED;
  if (R->PllFsm)
  {
    if (!(R->Pll[0] & (1U << 30)))
      return Bad (A, 0x100000, R->Pll[0], V | (1U << 30)), EFI_UNSUPPORTED;
  }
  else if (R->Pll[1] != 1)
    return Bad (A, 0x100004, R->Pll[1], 1), EFI_UNSUPPORTED;
  if ((R->Pll[2] & 0xFFFFU) != 31)
    return Bad (A, 0x100010, R->Pll[2], 31), EFI_UNSUPPORTED;
  if ((R->Pll[3] & 0xFFFFU) != 0x4000)
    return Bad (A, 0x100014, R->Pll[3], 0x4000), EFI_UNSUPPORTED;
  *Dirty = TRUE;
  V = (R->RateBefore & ~RATE_MASK) | (1U << 20) | 0x100U | 11U; // GPLL0 /6 =100MHz
  if (!LatchRate (A, V, *Rcg) || !R32 (A, RATE, &R->RateAfter) || !Train (A, 3, &R->SpeedAfter))
    return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
STATIC BOOLEAN RestoreSpeed (ACCESS *A, UINT32 Rcg, UINT16 Target)
{
  UINT32 V;
  if (!Train (A, (UINT16)(Target & 15U), &V) || !LatchRate (A, A->R->RateBefore, Rcg) ||
      !W16 (A, ROOT + A->R->PcieCap[0] + 0x30, Target))
    return FALSE;
  return TRUE;
}
EFI_STATUS LiuqinPcieConfigureHandoff (CONST LIUQIN_PCIE_IO *Io, CONST LIUQIN_PCIE_CONFIG_IO *Cfg)
{
  LIUQIN_PCIE_HANDOFF_STATE R = {0};
  ACCESS A = {Io, Cfg, &R};
  UINT32 RootCommand = 0, Bridge = 0, SidSaved[256], Rcg = 0, V;
  UINT16 Target = 0;
  BOOLEAN MemoryDirty = FALSE, SidDirty = FALSE, SpeedDirty = FALSE;
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  if (!Io || !Io->Read32 || !Io->Write32 || !Io->DelayUs || !Io->RetainForOs || !Cfg ||
      !Cfg->Read8 || !Cfg->Read16 || !Cfg->Write16)
    return EFI_INVALID_PARAMETER;
  *Io->RetainForOs = FALSE;
  if (!Eq32 (&A, EP + 4, 0x00100000) || !Capabilities (&A))
    return Status;
  Status = Memory (&A, &RootCommand, &Bridge, &MemoryDirty);
  if (EFI_ERROR (Status))
    goto Restore;
  Status = Speed (&A, &Rcg, &Target, &SpeedDirty);
  if (EFI_ERROR (Status))
    goto Restore;
  Status = Sid (&A, SidSaved, &SidDirty);
  if (EFI_ERROR (Status))
    goto Restore;
  if (!Eq32 (&A, EP, 0x110317CB) || !Eq32 (&A, EP + 4, 0x00100000) ||
      !Eq32 (&A, ROOT, 0x011017CB) || !R32 (&A, ROOT + 4, &V) || (V & 4U) ||
      !R32 (&A, ROOT + R.PcieCap[0] + 0x10, &V) || ((V >> 16) & 0x3FFU) != 0x13U ||
      !(V & (1U << 29)))
  {
    Status = EFI_DEVICE_ERROR;
    goto Restore;
  }
  // Commit only after all windows, SID routing and the link are ready.
  // Endpoint BARs and bus mastering remain under OS ownership.
  *Io->RetainForOs = TRUE;
  return EFI_SUCCESS;
Restore:
  if (SidDirty && !RestoreSid (&A, SidSaved))
    return EFI_ABORTED;
  if (SpeedDirty && !RestoreSpeed (&A, Rcg, Target))
    return EFI_ABORTED;
  if (MemoryDirty && !RestoreMemory (&A, RootCommand, Bridge))
    return EFI_ABORTED;
  return Status;
}
