/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "UsbHostPolicy.h"

static const UINT32 mUsbClockOffsets[LIUQIN_USB_CLOCK_COUNT] = {0x49018U, 0x49084U, 0x49088U,
                                                                0x49024U, 0x49020U};

static LIUQIN_USB_HOST_RESULT ProbeDisabledBank (LIUQIN_USB_READ32 Read32, VOID *Context,
                                                 LIUQIN_USB_HOST_OBSERVATION *Observation,
                                                 UINT32 Id1, UINT32 S2cr, UINT32 Groups)
{
  UINT32 Shift;
  UINT32 PageSize;
  UINT32 GlobalPages;
  UINT32 Banks;
  UINT32 Bank;
  UINT32 Index;
  UINT32 References;
  UINT32 Smr;
  UINT32 Other;
  UINT32 Value;
  UINTN CbBase;

  if ((Id1 == 0U) || (Id1 == 0xFFFFFFFFU) || ((Id1 & ~0xF0FF00FFU) != 0U))
  {
    return LiuqinUsbHostSmmuUnknown;
  }

  Shift = ((Id1 & 0x80000000U) != 0U) ? 16U : 12U;
  PageSize = 1U << Shift;
  GlobalPages = 1U << (((Id1 >> 28) & 7U) + 1U);
  Banks = Id1 & 0xFFU;
  Bank = S2cr & 0xFFU;
  if ((Banks == 0U) || (Banks > GlobalPages) || (Bank >= Banks) ||
      (((UINTN)(GlobalPages + Bank) * PageSize) > LIUQIN_USB_SMMU_WINDOW))
  {
    return LiuqinUsbHostSmmuUnknown;
  }

  /* Only the bank index may be encoded; any other bit changes the meaning. */
  if (S2cr != Bank)
  {
    return LiuqinUsbHostSmmuTranslation;
  }

  /* Exactly one valid NS TYPE0 reference to this bank. */
  References = 0U;
  for (Index = 0U; Index < Groups; ++Index)
  {
    Other = Read32 (Context, LIUQIN_USB_SMMU_BASE + LIUQIN_USB_SMMU_S2CR + 4U * Index);
    Smr = Read32 (Context, LIUQIN_USB_SMMU_BASE + LIUQIN_USB_SMMU_SMR + 4U * Index);
    if ((Other == 0xFFFFFFFFU) || (Smr == 0xFFFFFFFFU))
    {
      return LiuqinUsbHostSmmuUnknown;
    }

    if ((((Other >> 16) & 3U) == 0U) && ((Other & 0xFFU) == Bank) && ((Smr & (1U << 31)) != 0U))
    {
      ++References;
    }
  }

  if (References != 1U)
  {
    return LiuqinUsbHostSmmuAmbiguous;
  }

  CbBase = (UINTN)LIUQIN_USB_SMMU_BASE + (UINTN)(GlobalPages + Bank) * PageSize;
  Observation->SmmuBank = Bank;
  Observation->SmmuSctlr = Value = Read32 (Context, CbBase);
  if ((Value == 0xFFFFFFFFU) || ((Value & 1U) != 0U))
  {
    return LiuqinUsbHostSmmuTranslation;
  }

  if (Value == 0x000000E0U)
  {
    Observation->SmmuVariant = 2U;
  }
  else if (Value == 0x0A9F00E0U)
  {
    Observation->SmmuVariant = 3U;
  }
  else
  {
    return LiuqinUsbHostSmmuUnknown;
  }

  return LiuqinUsbHostEligible;
}

static LIUQIN_USB_HOST_RESULT ProbeDma (LIUQIN_USB_READ32 Read32, VOID *Context,
                                        LIUQIN_USB_HOST_OBSERVATION *Observation)
{
  UINT32 Id0;
  UINT32 Cr0;
  UINT32 Groups;
  UINT32 Index;
  UINT32 Smr;
  UINT32 S2cr;
  UINT32 Mask;
  UINT32 Matches;
  LIUQIN_USB_HOST_RESULT Result;
  BOOLEAN Extended;
  BOOLEAN Valid;

  Observation->SmmuId0 = Id0 = Read32 (Context, LIUQIN_USB_SMMU_BASE + LIUQIN_USB_SMMU_ID0);
  Observation->SmmuCr0 = Cr0 = Read32 (Context, LIUQIN_USB_SMMU_BASE + LIUQIN_USB_SMMU_CR0);
  Groups = Id0 & 0xFFU;
  if ((Id0 == 0U) || (Id0 == 0xFFFFFFFFU) || (Cr0 == 0xFFFFFFFFU) || ((Id0 & (1U << 27)) == 0U) ||
      ((Id0 & ((1U << 30) | (1U << 29))) == 0U) || (Groups == 0U) ||
      (Groups > LIUQIN_USB_SMMU_MAX_GROUPS))
  {
    return LiuqinUsbHostSmmuUnknown;
  }

  /* CLIENTPD has no validated handoff contract; do not treat it as bypass. */
  if ((Cr0 & 1U) != 0U)
  {
    return LiuqinUsbHostSmmuDisabled;
  }

  Extended = (BOOLEAN)((Cr0 & (1U << 3)) != 0U);
  if (Extended && ((Id0 & (1U << 8)) == 0U))
  {
    return LiuqinUsbHostSmmuUnknown;
  }

  Matches = 0U;
  for (Index = 0U; Index < Groups; ++Index)
  {
    Smr = Read32 (Context, LIUQIN_USB_SMMU_BASE + LIUQIN_USB_SMMU_SMR + 4U * Index);
    S2cr = Read32 (Context, LIUQIN_USB_SMMU_BASE + LIUQIN_USB_SMMU_S2CR + 4U * Index);
    if ((Smr == 0xFFFFFFFFU) || (S2cr == 0xFFFFFFFFU))
    {
      return LiuqinUsbHostSmmuUnknown;
    }

    Valid = Extended ? (BOOLEAN)((S2cr & (1U << 10)) != 0U) : (BOOLEAN)((Smr & (1U << 31)) != 0U);
    if (!Valid)
    {
      continue;
    }

    /* Without EXIDENABLE, bit 31 is VALID rather than a mask bit. */
    Mask = (Smr >> 16) & (Extended ? 0xFFFFU : 0x7FFFU);
    if ((((Smr & 0xFFFFU) ^ LIUQIN_USB_STREAM_ID) & ~Mask) != 0U)
    {
      continue;
    }

    ++Matches;
    Observation->MatchingSmr = Smr;
    Observation->MatchingS2cr = S2cr;
  }

  Observation->SmmuMatches = Matches;
  if (Matches > 1U)
  {
    return LiuqinUsbHostSmmuAmbiguous;
  }

  if (Matches == 0U)
  {
    if ((Cr0 & (1U << 10)) != 0U)
    {
      return LiuqinUsbHostSmmuFault;
    }
  }
  else
  {
    switch ((Observation->MatchingS2cr >> 16) & 3U)
    {
    case 1U: /* S2CR_TYPE_BYPASS */
      Observation->SmmuVariant = 1U;
      break;
    case 0U: /* S2CR_TYPE_TRANS: only a disabled bank is an identity path. */
      Result = ProbeDisabledBank (Read32, Context, Observation,
                                  Read32 (Context, LIUQIN_USB_SMMU_BASE + LIUQIN_USB_SMMU_ID1),
                                  Observation->MatchingS2cr, Groups);
      if (Result != LiuqinUsbHostEligible)
      {
        return Result;
      }

      break;
    case 2U: /* S2CR_TYPE_FAULT */
      return LiuqinUsbHostSmmuFault;
    default:
      return LiuqinUsbHostSmmuUnknown;
    }
  }

  Observation->DmaReady = TRUE;
  return LiuqinUsbHostEligible;
}

VOID LiuqinUsbInitializeObservation (LIUQIN_USB_HOST_OBSERVATION *Observation)
{
  UINT32 Index;

  Observation->Gdsc = Observation->Bcr = LIUQIN_USB_UNREAD;
  Observation->DmaReady = FALSE;
  Observation->SmmuMatches = 0U;
  Observation->MatchingSmr = Observation->MatchingS2cr = LIUQIN_USB_UNREAD;
  Observation->GsnpsId = Observation->Gctl = Observation->HwParams0 = LIUQIN_USB_UNREAD;
  Observation->Usb2Phy = Observation->Usb3Phy = LIUQIN_USB_UNREAD;
  Observation->SmmuId0 = Observation->SmmuCr0 = LIUQIN_USB_UNREAD;
  Observation->Capability = Observation->HcsParams1 = LIUQIN_USB_UNREAD;
  Observation->DoorbellOffset = Observation->RuntimeOffset = LIUQIN_USB_UNREAD;
  Observation->UsbCommand = Observation->UsbStatus = LIUQIN_USB_UNREAD;
  for (Index = 0U; Index < LIUQIN_USB_CLOCK_COUNT; ++Index)
  {
    Observation->Clocks[Index] = LIUQIN_USB_UNREAD;
  }

  Observation->Result = LiuqinUsbHostNotProbed;
}

static LIUQIN_USB_HOST_RESULT ProbeHost (LIUQIN_USB_READ32 Read32, VOID *Context,
                                         LIUQIN_USB_HOST_OBSERVATION *Observation,
                                         BOOLEAN AllowDeviceRole, BOOLEAN DeviceOnly)
{
  UINT32 Index;
  UINT32 Ip;
  UINT32 CapLength;
  UINT32 Version;
  UINT32 Ports;
  UINT32 Slots;
  LIUQIN_USB_HOST_RESULT DmaResult;
  LIUQIN_USB_HOST_RESULT Result;

  LiuqinUsbInitializeObservation (Observation);

  /* Probe GCC first; no access to an unpowered/reset/gated DWC3 domain. */
  Observation->Gdsc = Read32 (Context, LIUQIN_USB_GCC_BASE + LIUQIN_USB_GDSC);
  Observation->Bcr = Read32 (Context, LIUQIN_USB_GCC_BASE + LIUQIN_USB_BCR);
  Result = LiuqinUsbHostPowerOff;
  if ((Observation->Gdsc == 0xFFFFFFFFU) || ((Observation->Gdsc & (1U << 31)) == 0U))
  {
    goto Done;
  }

  Result = LiuqinUsbHostCoreReset;
  if ((Observation->Bcr & 1U) != 0U)
  {
    goto Done;
  }

  for (Index = 0U; Index < LIUQIN_USB_CLOCK_COUNT; ++Index)
  {
    Observation->Clocks[Index] = Read32 (Context, LIUQIN_USB_GCC_BASE + mUsbClockOffsets[Index]);
  }

  Result = LiuqinUsbHostClockOff;
  for (Index = 0U; Index < LIUQIN_USB_CLOCK_COUNT; ++Index)
  {
    if ((Observation->Clocks[Index] & (1U << 31)) != 0U)
    {
      goto Done;
    }
  }

  Observation->GsnpsId = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_GSNPSID);
  Ip = Observation->GsnpsId >> 16;
  Result = LiuqinUsbHostBadCoreId;
  if ((Ip != 0x5533U) && (Ip != 0x3331U) && (Ip != 0x3332U))
  {
    goto Done;
  }

  Observation->Gctl = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_GCTL);
  Observation->HwParams0 = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_GHWPARAMS0);
  Observation->Usb2Phy = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_GUSB2PHYCFG);
  Observation->Usb3Phy = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_GUSB3PIPECTL);
  DmaResult = ProbeDma (Read32, Context, Observation);

  Result = LiuqinUsbHostCoreReset;
  if ((Observation->Gctl == 0xFFFFFFFFU) || ((Observation->Gctl & (1U << 11)) != 0U))
  {
    goto Done;
  }

  Result = LiuqinUsbHostUnsupportedCore;
  if (((Observation->HwParams0 & 3U) != (DeviceOnly ? 0U : 1U)) &&
      ((Observation->HwParams0 & 3U) != 2U))
  {
    goto Done;
  }

  /* Switching a device-mode core requires the board's repeater/VBUS recipe. */
  Result = LiuqinUsbHostBoardPrepareRequired;
  if (!AllowDeviceRole && ((Observation->Gctl >> 12) & 3U) != 1U)
  {
    goto Done;
  }

  if (DeviceOnly &&
      ((Observation->Gctl & LIUQIN_USB_GCTL_PRTCAPDIR_MASK) != LIUQIN_USB_GCTL_PRTCAPDIR_DEVICE))
  {
    goto Done;
  }

  Result = LiuqinUsbHostPhyReset;
  if (((Observation->Usb2Phy | Observation->Usb3Phy) & (1U << 31)) != 0U)
  {
    goto Done;
  }

  Result = DmaResult;
  if (Result != LiuqinUsbHostEligible)
  {
    goto Done;
  }

  if (DeviceOnly)
  {
    goto Done;
  }

  Observation->Capability = Read32 (Context, LIUQIN_USB_MMIO_BASE);
  Observation->HcsParams1 = Read32 (Context, LIUQIN_USB_MMIO_BASE + 4U);
  Observation->DoorbellOffset = Read32 (Context, LIUQIN_USB_MMIO_BASE + 0x14U);
  Observation->RuntimeOffset = Read32 (Context, LIUQIN_USB_MMIO_BASE + 0x18U);
  CapLength = Observation->Capability & 0xFFU;
  Version = Observation->Capability >> 16;
  Ports = Observation->HcsParams1 >> 24;
  Slots = Observation->HcsParams1 & 0xFFU;
  Result = LiuqinUsbHostBadCapabilities;
  /* Locked XhciReg.h XHC_HCSPARAMS1 reserves bits [23:19]. A failed
   * all-ones read otherwise looks like nonzero ports/slots/interrupters. */
  if ((Observation->Capability == 0xFFFFFFFFU) || (Observation->HcsParams1 == 0xFFFFFFFFU) ||
      ((Observation->HcsParams1 & 0x00F80000U) != 0U) || (CapLength < 0x20U) ||
      ((CapLength & 3U) != 0U) || (Version < 0x100U) || (Version > 0x120U) || (Ports == 0U) ||
      (Slots == 0U) || (((Observation->HcsParams1 >> 8) & 0x7FFU) == 0U) ||
      ((Observation->DoorbellOffset & 3U) != 0U) || ((Observation->RuntimeOffset & 0x1FU) != 0U) ||
      (Observation->DoorbellOffset < CapLength) ||
      (Observation->DoorbellOffset > LIUQIN_USB_XHCI_SIZE - 4U * (Slots + 1U)) ||
      (Observation->RuntimeOffset < CapLength) ||
      (Observation->RuntimeOffset > LIUQIN_USB_XHCI_SIZE - 0x40U) ||
      (CapLength + 0x400U + 0x10U * Ports > LIUQIN_USB_XHCI_SIZE))
  {
    goto Done;
  }

  Result = LiuqinUsbHostEligible;
Done:
  Observation->Result = Result;
  return Result;
}

LIUQIN_USB_HOST_RESULT
LiuqinUsbProbeInheritedHost (LIUQIN_USB_READ32 Read32, VOID *Context,
                             LIUQIN_USB_HOST_OBSERVATION *Observation)
{
  return ProbeHost (Read32, Context, Observation, FALSE, FALSE);
}

LIUQIN_USB_HOST_RESULT
LiuqinUsbProbeInheritedDevice (LIUQIN_USB_READ32 Read32, VOID *Context,
                               LIUQIN_USB_HOST_OBSERVATION *Observation)
{
  return ProbeHost (Read32, Context, Observation, TRUE, TRUE);
}

BOOLEAN
LiuqinUsbQuiesceDevice (LIUQIN_USB_READ32 Read32, LIUQIN_USB_WRITE32 Write32,
                        LIUQIN_USB_DELAY Delay, VOID *Context,
                        LIUQIN_USB_HOST_OBSERVATION *Observation)
{
  UINT32 Dctl, Dsts, Index;
  if (Read32 == NULL || Write32 == NULL || Delay == NULL || Observation == NULL ||
      LiuqinUsbProbeInheritedDevice (Read32, Context, Observation) != LiuqinUsbHostEligible)
  {
    return FALSE;
  }
  Observation->Result = LiuqinUsbHostHaltFailed;
  Dctl = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_DCTL);
  if (Dctl == 0xFFFFFFFFU || (Dctl & LIUQIN_USB_DCTL_CSFTRST) != 0U)
    return FALSE;
  Write32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_DCTL, Dctl & ~LIUQIN_USB_DCTL_RUN_STOP);
  for (Index = 0; Index < LIUQIN_USB_HALT_MAX_POLLS; ++Index)
  {
    Dsts = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_DSTS);
    if (Dsts == 0xFFFFFFFFU)
      return FALSE;
    if ((Dsts & LIUQIN_USB_DSTS_HALTED) != 0U)
      break;
    Delay (Context, LIUQIN_USB_HALT_WAIT_US);
  }
  if (Index == LIUQIN_USB_HALT_MAX_POLLS ||
      Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_DCTL) !=
          (Dctl & ~LIUQIN_USB_DCTL_RUN_STOP))
    return FALSE;
  Write32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_DEVTEN, 0);
  if (Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_DEVTEN) != 0)
    return FALSE;
  Observation->Result = LiuqinUsbHostHalted;
  return TRUE;
}

static BOOLEAN HostOrBits (LIUQIN_USB_READ32 Read32, LIUQIN_USB_WRITE32 Write32, VOID *Context,
                           UINT32 Offset, UINT32 Bits)
{
  UINT32 Value;

  Value = Read32 (Context, LIUQIN_USB_MMIO_BASE + Offset);
  if (Value == 0xFFFFFFFFU)
  {
    return FALSE;
  }

  Write32 (Context, LIUQIN_USB_MMIO_BASE + Offset, Value | Bits);
  Value = Read32 (Context, LIUQIN_USB_MMIO_BASE + Offset);
  return (Value != 0xFFFFFFFFU) && ((Value & Bits) == Bits);
}

BOOLEAN
LiuqinUsbEnableHost (LIUQIN_USB_READ32 Read32, LIUQIN_USB_WRITE32 Write32, LIUQIN_USB_DELAY Delay,
                     VOID *Context, LIUQIN_USB_HOST_OBSERVATION *Observation)
{
  UINT32 Gctl;
  UINT32 Hcc;
  UINT32 OpBase;
  UINT32 Ports;
  UINT32 Index;
  UINT32 Value;
  UINTN Address;

  if (Read32 == NULL || Write32 == NULL || Delay == NULL || Observation == NULL)
  {
    return FALSE;
  }

  /* Everything except the role must already be proven before any write. */
  if (ProbeHost (Read32, Context, Observation, TRUE, FALSE) != LiuqinUsbHostEligible)
  {
    return FALSE;
  }

  if (((Observation->Gctl >> 12) & 3U) != 1U)
  {
    /* Vendor sequence: clear PRTCAPDIR, then set host plus U2EXIT_LFPS. */
    Gctl = Observation->Gctl & ~LIUQIN_USB_GCTL_PRTCAPDIR_MASK;
    Write32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_GCTL, Gctl);
    Write32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_GCTL,
             Gctl | LIUQIN_USB_GCTL_U2EXIT_LFPS | LIUQIN_USB_GCTL_PRTCAPDIR_HOST |
                 LIUQIN_USB_GCTL_DSBLCLKGTNG);
    Delay (Context, LIUQIN_USB_HOST_WAIT_US);
    Gctl = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_GCTL);
    Observation->Gctl = Gctl;
    if ((Gctl == 0xFFFFFFFFU) || (((Gctl >> 12) & 3U) != 1U) ||
        ((Gctl & LIUQIN_USB_GCTL_CORESOFTRESET) != 0))
    {
      goto Fail;
    }
  }

  /* Fixed host-mode tuning from SynopsysInitHostConfig. */
  if (!HostOrBits (Read32, Write32, Context, LIUQIN_USB_GRXTHRCFG, LIUQIN_USB_GRXTHRCFG_HOST) ||
      !HostOrBits (Read32, Write32, Context, LIUQIN_USB_GSBUSCFG1,
                   LIUQIN_USB_GSBUSCFG1_HOST | LIUQIN_USB_GSBUSCFG1_EN1KPAGE) ||
      !HostOrBits (Read32, Write32, Context, LIUQIN_USB_GUCTL3, LIUQIN_USB_GUCTL3_HOST) ||
      !HostOrBits (Read32, Write32, Context, LIUQIN_USB_GUCTL1, LIUQIN_USB_GUCTL1_HOST))
  {
    goto Fail;
  }

  /* Wake-on-connect and port power for every port the controller reports. */
  Hcc = Read32 (Context, LIUQIN_USB_MMIO_BASE + LIUQIN_USB_HCCPARAMS1_OFFSET);
  if (Hcc == 0xFFFFFFFFU)
  {
    goto Fail;
  }

  OpBase = LIUQIN_USB_MMIO_BASE + (Observation->Capability & 0xFFU);
  Ports = Observation->HcsParams1 >> 24;
  for (Index = 0; Index < Ports; ++Index)
  {
    Address = OpBase + LIUQIN_USB_PORTSC_OFFSET + LIUQIN_USB_PORTSC_STRIDE * Index;
    Value = Read32 (Context, Address);
    if (Value == 0xFFFFFFFFU)
    {
      goto Fail;
    }

    Value |= LIUQIN_USB_PORTSC_WCE;
    if ((Hcc & LIUQIN_USB_HCCPARAMS1_PPC) != 0)
    {
      Value |= LIUQIN_USB_PORTSC_PP;
    }

    Write32 (Context, Address, Value);
    Value = Read32 (Context, Address);
    if ((Value == 0xFFFFFFFFU) || ((Value & LIUQIN_USB_PORTSC_WCE) == 0))
    {
      goto Fail;
    }
  }

  /* The normal read-only gate must now accept the host role end to end. */
  return LiuqinUsbProbeInheritedHost (Read32, Context, Observation) == LiuqinUsbHostEligible;

Fail:
  Observation->DmaReady = FALSE;
  Observation->Result = LiuqinUsbHostBringupFailed;
  return FALSE;
}

BOOLEAN
LiuqinUsbQuiesceHost (LIUQIN_USB_READ32 Read32, LIUQIN_USB_WRITE32 Write32, LIUQIN_USB_DELAY Delay,
                      VOID *Context, LIUQIN_USB_HOST_OBSERVATION *Observation)
{
  UINTN Operational;
  UINT32 Command;
  UINT32 Status;
  UINT32 Poll;

  if (Read32 == NULL || Write32 == NULL || Delay == NULL || Observation == NULL)
  {
    return FALSE;
  }

  if (LiuqinUsbProbeInheritedHost (Read32, Context, Observation) != LiuqinUsbHostEligible)
  {
    Observation->DmaReady = FALSE;
    return FALSE;
  }

  /* Locked XhciReg.h: operational USBCMD+0 / USBSTS+4; R/S bit0,
   * HCRST bit1, INTE bit2; USBSTS HCHalted bit0 / CNR bit11. */
  Operational = LIUQIN_USB_MMIO_BASE + (Observation->Capability & 0xFFU);
  Observation->UsbCommand = Command = Read32 (Context, Operational);
  Observation->DmaReady = FALSE;
  Observation->Result = LiuqinUsbHostHaltFailed;
  if (Command == 0xFFFFFFFFU || (Command & (1U << 1)) != 0U)
  {
    return FALSE; // Never replay an in-progress host reset.
  }

  if ((Command & 5U) != 0U)
  {
    Write32 (Context, Operational, Command & ~5U);
  }

  /* HCHalted is the host contract that command/event/transfer processing has
   * stopped. Disable interrupts and require RUN to read back clear as well;
   * a lone all-ones or transitional status cannot prove DMA has stopped. */
  for (Poll = 0U; Poll <= LIUQIN_USB_HALT_MAX_POLLS; ++Poll)
  {
    Observation->UsbCommand = Command = Read32 (Context, Operational);
    Observation->UsbStatus = Status = Read32 (Context, Operational + 4U);
    if (Command != 0xFFFFFFFFU && Status != 0xFFFFFFFFU && (Command & 7U) == 0U &&
        (Status & 1U) != 0U && (Status & ((1U << 8) | (1U << 9) | (1U << 11))) == 0U)
    {
      Observation->Result = LiuqinUsbHostHalted;
      return TRUE;
    }
    if (Poll != LIUQIN_USB_HALT_MAX_POLLS)
    {
      Delay (Context, LIUQIN_USB_HALT_WAIT_US);
    }
  }

  return FALSE;
}

const char *LiuqinUsbHostResultText (LIUQIN_USB_HOST_RESULT Result)
{
  switch (Result)
  {
  case LiuqinUsbHostEligible:
    return "inherited host accepted";
  case LiuqinUsbHostNotProbed:
    return "not probed";
  case LiuqinUsbHostPowerOff:
    return "USB power domain off/unreadable";
  case LiuqinUsbHostClockOff:
    return "USB clock stopped/unreadable";
  case LiuqinUsbHostCoreReset:
    return "USB core in reset";
  case LiuqinUsbHostBadCoreId:
    return "DWC3 identification unavailable";
  case LiuqinUsbHostUnsupportedCore:
    return "host capability unavailable";
  case LiuqinUsbHostBoardPrepareRequired:
    return "device/OTG mode: board host initialization required";
  case LiuqinUsbHostPhyReset:
    return "PHY in reset/unreadable";
  case LiuqinUsbHostSmmuUnknown:
    return "SMMU stream state unproven";
  case LiuqinUsbHostSmmuDisabled:
    return "SMMU CLIENTPD handoff unsupported";
  case LiuqinUsbHostSmmuAmbiguous:
    return "multiple SMMU stream matches";
  case LiuqinUsbHostSmmuTranslation:
    return "SMMU translation requires DMA mapping";
  case LiuqinUsbHostSmmuFault:
    return "SMMU stream blocked";
  case LiuqinUsbHostBadCapabilities:
    return "invalid xHCI capability window";
  case LiuqinUsbHostStackNotBound:
    return "standard xHCI stack not bound";
  case LiuqinUsbHostStackNotRunning:
    return "standard xHCI stack not running";
  case LiuqinUsbHostRegistrationFailed:
    return "xHCI registration failed";
  case LiuqinUsbHostEmulatorSkipped:
    return "liuqin USB probe skipped in QEMU";
  case LiuqinUsbHostRuntimeDescriptionMismatch:
    return "runtime USB description missing/mismatched";
  case LiuqinUsbHostExitHookUnavailable:
    return "USB DMA exit hook unavailable";
  case LiuqinUsbHostHaltFailed:
    return "USB DMA halt/readback failed; reboot required";
  case LiuqinUsbHostHalted:
    return "USB host halted; DMA ownership released";
  case LiuqinUsbHostLifecycleUnproven:
    return "USB DMA release lifecycle unproven; host not registered";
  case LiuqinUsbHostBringupFailed:
    return "board host bring-up readback failed";
  default:
    return "unknown USB status";
  }
}
