/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#include <PiDxe.h>
#include <BootContract.h>
#include <Guid/EventGroup.h>
#include <Guid/LiuqinHandoff.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LiuqinIoReady.h>
#include <Protocol/Usb2HostController.h>
#include "UsbHostPolicy.h"
#include "VbusSnapshot.h"
#include "VbusProductionAdapter.h"
#include <LiuqinMmio.h>

STATIC EFI_HANDLE mControllerHandle;
STATIC LIUQIN_USB_HOST_OBSERVATION mObservation;
STATIC LIUQIN_IO_READY_PROTOCOL mReady = {LIUQIN_IO_READY_VERSION,
                                          LIUQIN_USB_MMIO_BASE,
                                          LIUQIN_USB_XHCI_SIZE,
                                          LIUQIN_USB_GSIV,
                                          FALSE,
                                          FALSE};
STATIC EFI_STATUS mRegistrationStatus = EFI_NOT_READY;
STATIC VOID *mUsbNotification;
STATIC EFI_EVENT mUsbEvent;
STATIC EFI_EVENT mReadyToBootEvent;
STATIC EFI_EVENT mExitBootServicesEvent;
STATIC BOOLEAN mStopping;
STATIC CONST VOID *mRuntimeFdt;
STATIC LIUQIN_USB_VBUS_SNAPSHOT mVbusSnapshot;
STATIC LIUQIN_SPMI_READ_CONFIG mVbusConfig;
STATIC BOOLEAN mVbusCollected;
STATIC LIUQIN_SPMI_WRITE_CONFIG mVbusWriteConfig;
/* Production USB VBUS/Type-C state machine. */
STATIC LIUQIN_VBUS_PRODUCTION mVbusProduction;
STATIC LIUQIN_VBUS_PRODUCTION_IO mVbusProductionIo;
STATIC EFI_EVENT mVbusProductionTimer;
STATIC UINT64 mVbusProductionFrequency;
STATIC BOOLEAN mVbusProductionStarted;
STATIC BOOLEAN mVbusProductionBusy;
STATIC BOOLEAN mVbusProductionArmRefused;

#define LIUQIN_VBUS_POLL_PERIOD_100NS 2500000U
#define LIUQIN_VBUS_WARMUP_TICKS 12U
#define LIUQIN_VBUS_WARMUP_GAP_US 100000U

STATIC VOID VbusProductionArmIfReady (VOID);
STATIC BOOLEAN VbusProductionEbsStop (VOID);

STATIC
BOOLEAN
NodeCompatible (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Wanted)
{
  CONST CHAR8 *Compatible;
  INT32 Length;

  Compatible = FdtGetProp (Fdt, Node, "compatible", &Length);
  return Compatible != NULL && Length > 0 && Compatible[Length - 1] == 0 &&
         FdtStringListContains (Compatible, Length, Wanted);
}

STATIC
BOOLEAN
NodeEnabled (CONST VOID *Fdt, INT32 Node)
{
  CONST CHAR8 *Status;
  INT32 Length;

  Status = FdtGetProp (Fdt, Node, "status", &Length);
  return Status == NULL || (Length == 5 && CompareMem (Status, "okay", 5) == 0) ||
         (Length == 3 && CompareMem (Status, "ok", 3) == 0);
}

STATIC
BOOLEAN
NodeReg (CONST VOID *Fdt, INT32 Node, UINT32 Base, UINT32 Size)
{
  INT32 Parent;
  INT32 Length;
  CONST UINT32 *Reg;

  Parent = FdtParentOffset (Fdt, Node);
  if (Parent < 0 || FdtAddressCells (Fdt, Parent) != 1 || FdtSizeCells (Fdt, Parent) != 1)
  {
    return FALSE;
  }

  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  return Reg != NULL && Length >= 8 && (Length % 8) == 0 && Fdt32ToCpu (Reg[0]) == Base &&
         Fdt32ToCpu (Reg[1]) == Size;
}

STATIC
BOOLEAN
RuntimeDescription (VOID)
{
  VOID *Hob;
  LIUQIN_HANDOFF *Handoff;
  CONST VOID *Fdt;
  CONST UINT32 *Value;
  INT32 Wrapper;
  INT32 Core;
  INT32 Smmu;
  INT32 Gcc;
  INT32 Gdsc;
  INT32 Length;
  UINTN Index;
  STATIC CONST UINT32 ClockIds[5] = {169U, 20U, 12U, 171U, 174U};

  Hob = GetFirstGuidHob (&gLiuqinHandoffGuid);
  if (Hob == NULL || GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (*Handoff))
  {
    return FALSE;
  }

  Handoff = GET_GUID_HOB_DATA (Hob);
  if (Handoff->Version != LIUQIN_HANDOFF_VERSION || Handoff->DtbBase == 0 ||
      Handoff->DtbSize < 40 || Handoff->DtbSize > LIUQIN_DTB_MAX)
  {
    return FALSE;
  }

  Fdt = (CONST VOID *)(UINTN)Handoff->DtbBase;
  if (LiuqinValidateDtb (Fdt, (LIUQIN_SIZE)Handoff->DtbSize) != 0 || LiuqinCheckIdentity (Fdt) != 0)
  {
    return FALSE;
  }

  Wrapper = FdtPathOffset (Fdt, "/soc/ssusb@a600000");
  Core = FdtPathOffset (Fdt, "/soc/ssusb@a600000/dwc3@a600000");
  Smmu = FdtPathOffset (Fdt, "/soc/apps-smmu@15000000");
  Gcc = FdtPathOffset (Fdt, "/soc/clock-controller@100000");
  Gdsc = FdtPathOffset (Fdt, "/soc/qcom,gdsc@149004");
  if (Wrapper < 0 || Core < 0 || Smmu < 0 || Gcc < 0 || Gdsc < 0 ||
      !NodeCompatible (Fdt, Wrapper, "qcom,dwc-usb3-msm") ||
      !NodeCompatible (Fdt, Core, "snps,dwc3") || !NodeCompatible (Fdt, Smmu, "qcom,qsmmu-v500") ||
      (!NodeCompatible (Fdt, Gcc, "qcom,cape-gcc") &&
       !NodeCompatible (Fdt, Gcc, "qcom,waipio-gcc")) ||
      !NodeCompatible (Fdt, Gdsc, "qcom,gdsc") || !NodeEnabled (Fdt, Wrapper) ||
      !NodeEnabled (Fdt, Core) || !NodeEnabled (Fdt, Smmu) || !NodeEnabled (Fdt, Gcc) ||
      !NodeEnabled (Fdt, Gdsc) || !NodeReg (Fdt, Wrapper, LIUQIN_USB_MMIO_BASE, 0x100000U) ||
      !NodeReg (Fdt, Core, LIUQIN_USB_MMIO_BASE, 0xD93CU) ||
      !NodeReg (Fdt, Smmu, LIUQIN_USB_SMMU_BASE, 0x100000U) ||
      !NodeReg (Fdt, Gcc, LIUQIN_USB_GCC_BASE, 0x1F4200U) ||
      !NodeReg (Fdt, Gdsc, LIUQIN_USB_GCC_BASE + LIUQIN_USB_GDSC, 4U))
  {
    return FALSE;
  }

  Value = FdtGetProp (Fdt, Wrapper, "ranges", &Length);
  if (Value == NULL || Length != 0)
  {
    return FALSE;
  }

  Value = FdtGetProp (Fdt, Core, "interrupts", &Length);
  if (Value == NULL || Length != 12 || Fdt32ToCpu (Value[0]) != 0 ||
      Fdt32ToCpu (Value[1]) != 133U || Fdt32ToCpu (Value[2]) != 4U)
  {
    return FALSE;
  }

  Value = FdtGetProp (Fdt, Core, "iommus", &Length);
  if (Value == NULL || Length != 12 ||
      FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Value[0])) != Smmu ||
      Fdt32ToCpu (Value[1]) != LIUQIN_USB_STREAM_ID || Fdt32ToCpu (Value[2]) != 0U)
  {
    return FALSE;
  }

  Value = FdtGetProp (Fdt, Wrapper, "USB3_GDSC-supply", &Length);
  if (Value == NULL || Length != 4 || FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Value[0])) != Gdsc)
  {
    return FALSE;
  }

  Value = FdtGetProp (Fdt, Wrapper, "clocks", &Length);
  if (Value == NULL || Length != 40)
  {
    return FALSE;
  }

  for (Index = 0; Index < 5; ++Index)
  {
    if (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Value[2 * Index])) != Gcc ||
        Fdt32ToCpu (Value[2 * Index + 1]) != ClockIds[Index])
    {
      return FALSE;
    }
  }

  mRuntimeFdt = Fdt;
  return TRUE;
}

STATIC
UINT32
ReadUsbRegister (VOID *Context, UINTN Address)
{
  (VOID) Context;
  return MmioRead32 (Address);
}

STATIC BOOLEAN BoardU32 (INT32 Node, CONST CHAR8 *Name, UINT32 *Output)
{
  INT32 Length;
  CONST UINT32 *Value = FdtGetProp (mRuntimeFdt, Node, Name, &Length);
  if (Value == NULL || Length != 4)
    return FALSE;
  *Output = Fdt32ToCpu (*Value);
  return TRUE;
}

STATIC VOID BoardDescription (LIUQIN_SPMI_READ_CONFIG *Gates)
{
  INT32 Spmi, Length;
  CONST UINT32 *Values;
  CONST VOID *Names;
  UINTN Index;
  STATIC CONST UINT32 SpmiReg[] = {0x0C42D000, 0x4000,     0x0C400000, 0x3000,     0x0C500000,
                                   0x400000,   0x0C440000, 0x80000,    0x0C4C0000, 0x10000};
  STATIC CONST CHAR8 RegNames[] = "cnfg\0core\0chnls\0obsrvr\0intr";
  ZeroMem (Gates, sizeof (*Gates));
  if (mRuntimeFdt == NULL)
    return;
  Spmi = FdtPathOffset (mRuntimeFdt, "/soc/qcom,spmi@c42d000");
  if (Spmi >= 0 && NodeCompatible (mRuntimeFdt, Spmi, "qcom,spmi-pmic-arb") &&
      NodeEnabled (mRuntimeFdt, Spmi) && NodeReg (mRuntimeFdt, Spmi, 0x0C42D000, 0x4000))
  {
    Values = FdtGetProp (mRuntimeFdt, Spmi, "reg", &Length);
    Gates->DtValid = Values != NULL && Length == sizeof (SpmiReg);
    for (Index = 0; Gates->DtValid && Index < ARRAY_SIZE (SpmiReg); ++Index)
      Gates->DtValid = Fdt32ToCpu (Values[Index]) == SpmiReg[Index];
    Names = FdtGetProp (mRuntimeFdt, Spmi, "reg-names", &Length);
    Gates->DtValid = Gates->DtValid && Names != NULL && Length == sizeof (RegNames) &&
                     CompareMem (Names, RegNames, sizeof (RegNames)) == 0 &&
                     BoardU32 (Spmi, "qcom,bus-id", &Gates->Bus) &&
                     BoardU32 (Spmi, "qcom,ee", &Gates->Ee);
  }
}

STATIC BOOLEAN ReadBoardRegister (VOID *Context, UINTN Address, UINT32 *Value)
{
  LIUQIN_MMIO_WINDOW Windows[LIUQIN_MMIO_MAX_WINDOWS];
  UINTN Count, Index;
  (VOID) Context;
  if (Value == NULL || (Address & 3U) != 0)
    return FALSE;
  Count = LiuqinGetIoWindows (Windows, ARRAY_SIZE (Windows));
  for (Index = 0; Index < Count; ++Index)
  {
    if (Address >= Windows[Index].Base && Address - Windows[Index].Base <= Windows[Index].Size - 4)
    {
      *Value = MmioRead32 (Address);
      return TRUE;
    }
  }
  return FALSE;
}

STATIC BOOLEAN WriteVbusReadCommand (VOID *Context, UINTN Address, UINT32 Value)
{
  (VOID) Context;
  /* Only EXT_READL may use the observer command word. PMIC writes have a
   * separate channel publisher with its own owner and register checks. */
  if (!mVbusConfig.DtValid || !mVbusConfig.MmioValid || Address < LIUQIN_SPMI_OBSERVER_BASE ||
      Address - LIUQIN_SPMI_OBSERVER_BASE > LIUQIN_SPMI_OBSERVER_SIZE - 4U ||
      ((Address - LIUQIN_SPMI_OBSERVER_BASE) & 0x1FU) != 0 || (Value & ~0x08000FF7U) != 0 ||
      (Value & 0x08000000U) == 0)
    return FALSE;
  MemoryFence ();
  MmioWrite32 (Address, Value);
  MemoryFence ();
  return TRUE;
}

STATIC VOID VbusDelay (VOID *Context, UINT32 Microseconds)
{
  (VOID) Context;

  MicroSecondDelay (Microseconds);
}

STATIC VOID CollectVbusSnapshot (VOID)
{
  LIUQIN_SPMI_READ_CONFIG Gates;
  INT32 Pmic, Bus, Length;
  CONST UINT32 *Reg;
  LIUQIN_SPMI_READ_IO Io = {NULL, ReadBoardRegister, WriteVbusReadCommand, VbusDelay, NULL};
  if (mVbusCollected)
    return;
  mVbusCollected = TRUE;
  BoardDescription (&Gates);
  ZeroMem (&mVbusConfig, sizeof (mVbusConfig));
  Bus = FdtPathOffset (mRuntimeFdt, "/soc/qcom,spmi@c42d000");
  Pmic = FdtPathOffset (mRuntimeFdt, "/soc/qcom,spmi@c42d000/qcom,pm8350b@3");

  mVbusConfig.DtValid =
      Gates.DtValid && Pmic >= 0 && Bus >= 0 && FdtParentOffset (mRuntimeFdt, Pmic) == Bus &&
      FdtAddressCells (mRuntimeFdt, Bus) == 2 && FdtSizeCells (mRuntimeFdt, Bus) == 0 &&
      NodeCompatible (mRuntimeFdt, Pmic, "qcom,spmi-pmic") && NodeEnabled (mRuntimeFdt, Pmic);
  if (mVbusConfig.DtValid)
  {
    Reg = FdtGetProp (mRuntimeFdt, Pmic, "reg", &Length);
    mVbusConfig.DtValid = Reg != NULL && Length == 8 &&
                          Fdt32ToCpu (Reg[0]) == LIUQIN_USB_VBUS_CANDIDATE_SLAVE &&
                          Fdt32ToCpu (Reg[1]) == 0;
  }
  /* The same inventory defines SEC RAM conflicts/resource HOBs and MMU
   * mappings (LiuqinMmio.h), including the production observer window. */
  mVbusConfig.MmioValid = TRUE;
  mVbusConfig.Bus = Gates.Bus;
  mVbusConfig.Ee = Gates.Ee;
  mVbusConfig.CoreBase = LIUQIN_SPMI_CORE_BASE;
  mVbusConfig.CoreSize = LIUQIN_SPMI_CORE_SIZE;
  mVbusConfig.CnfgBase = LIUQIN_SPMI_CNFG_BASE;
  mVbusConfig.CnfgSize = LIUQIN_SPMI_CNFG_SIZE;
  mVbusConfig.ObserverBase = LIUQIN_SPMI_OBSERVER_BASE;
  mVbusConfig.ObserverSize = LIUQIN_SPMI_OBSERVER_SIZE;
  /* The production adapter independently validates each write channel. */
  ZeroMem (&mVbusWriteConfig, sizeof (mVbusWriteConfig));
  mVbusWriteConfig.DtValid = mVbusConfig.DtValid;
  mVbusWriteConfig.MmioValid = mVbusConfig.MmioValid;
  mVbusWriteConfig.Bus = mVbusConfig.Bus;
  mVbusWriteConfig.Ee = mVbusConfig.Ee;
  mVbusWriteConfig.CoreBase = LIUQIN_SPMI_CORE_BASE;
  mVbusWriteConfig.CoreSize = LIUQIN_SPMI_CORE_SIZE;
  mVbusWriteConfig.CnfgBase = LIUQIN_SPMI_CNFG_BASE;
  mVbusWriteConfig.CnfgSize = LIUQIN_SPMI_CNFG_SIZE;
  mVbusWriteConfig.ChannelBase = LIUQIN_SPMI_WRITE_CHANNEL_BASE;
  mVbusWriteConfig.ChannelSize = LIUQIN_SPMI_WRITE_CHANNEL_SIZE;
  LiuqinUsbCollectVbusSnapshot (&Io, &mVbusConfig, &mVbusSnapshot);
}

STATIC UINT64 VbusProductionNowUs (VOID *Context)
{
  (VOID) Context;
  if (mVbusProductionFrequency == 0U)
  {
    mVbusProductionFrequency = GetPerformanceCounterProperties (NULL, NULL);
    if (mVbusProductionFrequency == 0U)
      return 0U;
  }
  return GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000U;
}

STATIC BOOLEAN VbusProductionMmioWrite (VOID *Context, UINTN Address, UINT32 Value)
{
  UINT32 Apid, DcdcApid, RoleApid;
  UINTN Offset;
  (VOID) Context;
  if (mStopping || !mVbusConfig.DtValid || !mVbusConfig.MmioValid ||
      Address < LIUQIN_SPMI_WRITE_CHANNEL_BASE ||
      Address - LIUQIN_SPMI_WRITE_CHANNEL_BASE > LIUQIN_SPMI_WRITE_CHANNEL_SIZE - 4U ||
      ((Address - LIUQIN_SPMI_WRITE_CHANNEL_BASE) & 3U) != 0U)
    return FALSE;
  Offset = (Address - LIUQIN_SPMI_WRITE_CHANNEL_BASE) & 0xFFFU;
  if (Offset != 0x00U && Offset != 0x10U)
    return FALSE;
  if (!VbusProductionAdapterChannels (&mVbusProduction, &DcdcApid, &RoleApid))
    return FALSE;
  Apid = (UINT32)((Address - LIUQIN_SPMI_WRITE_CHANNEL_BASE) / 0x1000U);
  if (Apid != DcdcApid && Apid != RoleApid)
    return FALSE;
  MemoryFence ();
  MmioWrite32 (Address, Value);
  MemoryFence ();
  return TRUE;
}

STATIC VOID VbusProductionDelay (VOID *Context, UINT32 Microseconds)
{
  (VOID) Context;
  if (mStopping)
    return;
  MicroSecondDelay (Microseconds);
}

STATIC VOID EFIAPI VbusProductionTimerTick (EFI_EVENT Event, VOID *Context)
{
  UINT64 Now;
  (VOID) Event;
  (VOID) Context;
  if (!mVbusProductionStarted || mStopping || mVbusProductionBusy)
    return;
  mVbusProductionBusy = TRUE;
  Now = VbusProductionNowUs (NULL);
  (VOID) VbusProductionAdapterTick (&mVbusProduction, Now);
  mVbusProductionBusy = FALSE;
}

/* Earliest start path (driver entry, after the inherited host path is proven
 * eligible and the xHCI controller is registered): arm the reviewed periodic
 * timer first, then run the bounded synchronous warmup so a cold-plugged UFP
 * is powered before BDS scans boot options. The fallback ArmIfReady path keeps
 * the previous behavior exactly: no warmup, same 250 ms cadence.
 *
 * Ordering is fail-closed on purpose: the timer event and its expiry must exist
 * before the warmup may issue any PMIC write. If either cannot be created or
 * armed, nothing has been written yet and the driver keeps the historical
 * "no PMIC write" behavior instead of leaving a possible source unowned. The
 * reviewed ExitBootServices stop/keep-on path is unchanged. */
STATIC VOID VbusProductionStart (IN BOOLEAN Early)
{
  EFI_STATUS Status;

  if (mVbusProductionStarted || mStopping)
    return;
  if (!mVbusConfig.DtValid || !mVbusConfig.MmioValid || !mVbusWriteConfig.DtValid ||
      !mVbusWriteConfig.MmioValid)
  {
    if (!mVbusProductionArmRefused)
    {
      mVbusProductionArmRefused = TRUE;
    }
    return;
  }
  ZeroMem (&mVbusProductionIo, sizeof (mVbusProductionIo));
  mVbusProductionIo.Context = NULL;
  mVbusProductionIo.NowUs = VbusProductionNowUs;
  mVbusProductionIo.Read32 = ReadBoardRegister;
  mVbusProductionIo.ObserverWrite32 = WriteVbusReadCommand;
  mVbusProductionIo.Write32 = VbusProductionMmioWrite;
  mVbusProductionIo.DelayUs = VbusProductionDelay;
  VbusProductionAdapterInitialize (&mVbusProduction, &mVbusProductionIo, &mVbusConfig,
                                   &mVbusWriteConfig, TRUE, TRUE, VbusProductionNowUs (NULL));
  /* Eligibility already proved the USB core/PHY is configured in host mode
   * (requirement R9); the xHCI driver binding is not required for VBUS. */
  VbusProductionAdapterSetHostReady (&mVbusProduction, TRUE);

  /* EVT_TIMER alone has no notification: DXE discards NotifyFunction for it.
   * Signal notification is required for expiry to drive the policy. The timer
   * is armed BEFORE any tick so monitoring exists for the whole lifetime of a
   * possible source. */
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, VbusProductionTimerTick,
                             NULL, &mVbusProductionTimer);
  if (EFI_ERROR (Status))
  {

    return;
  }
  Status = gBS->SetTimer (mVbusProductionTimer, TimerPeriodic, LIUQIN_VBUS_POLL_PERIOD_100NS);
  if (EFI_ERROR (Status))
  {

    gBS->CloseEvent (mVbusProductionTimer);
    mVbusProductionTimer = NULL;
    return;
  }
  mVbusProductionStarted = TRUE;

  if (Early)
  {

    mVbusProductionBusy = TRUE;
    (VOID) VbusProductionAdapterWarmup (&mVbusProduction, LIUQIN_VBUS_WARMUP_TICKS,
                                        LIUQIN_VBUS_WARMUP_GAP_US);
    mVbusProductionBusy = FALSE;
  }
}

STATIC VOID VbusProductionArmIfReady (VOID)
{
  if (mVbusProductionStarted || mStopping)
    return;
  VbusProductionStart (FALSE);
}

STATIC BOOLEAN VbusProductionEbsStop (VOID)
{
  UINT64 Now;
  if (!mVbusProductionStarted)
    return TRUE;
  if (mVbusProductionTimer != NULL)
  {
    (VOID) gBS->SetTimer (mVbusProductionTimer, TimerCancel, 0);
  }
  if (mVbusProductionBusy)
    return FALSE;
  Now = VbusProductionNowUs (NULL);
  return VbusProductionAdapterEbs (&mVbusProduction, Now);
}

STATIC
VOID WriteUsbRegister (VOID *Context, UINTN Address, UINT32 Value)
{
  (VOID) Context;
  MemoryFence ();
  MmioWrite32 (Address, Value);
  MemoryFence ();
}

STATIC
VOID DelayUsbHalt (VOID *Context, UINTN Microseconds)
{
  (VOID) Context;
  MicroSecondDelay (Microseconds);
}

STATIC
VOID EFIAPI StopUsbDma (EFI_EVENT Event, VOID *Context)
{
  (VOID) Event;
  (VOID) Context;

  /* Production VBUS: stop the BS timer first, run the bounded controlled-off
   * (or the explicit keep-on decision). This runs before mStopping so the
   * adapter's publishers are still authorized; the xHCI halt follows.
   * Preserve the existing bring-up policy of continuing when source-off or
   * inherited role restore is unproven. */
  if (mVbusProductionStarted)
  {
    (VOID) VbusProductionEbsStop ();
  }
  mStopping = TRUE;
  mReady.ControllerReady = FALSE;
  mReady.DmaReady = FALSE;
  if (mControllerHandle == NULL)
  {
    return;
  }

  if (!LiuqinUsbQuiesceHost (ReadUsbRegister, WriteUsbRegister, DelayUsbHalt, NULL, &mObservation))
  {
    MemoryFence ();
    for (;;)
    {
      CpuDeadLoop ();
    }
  }
  MemoryFence ();
}

STATIC
VOID UpdateReadyState (VOID)
{
  EFI_USB2_HC_PROTOCOL *UsbHc;
  EFI_USB_HC_STATE HcState;
  EFI_STATUS Status;

  mReady.ControllerReady = FALSE;
  mReady.DmaReady = FALSE;
  if (mControllerHandle == NULL || mStopping)
  {
    return;
  }

  if (LiuqinUsbProbeInheritedHost (ReadUsbRegister, NULL, &mObservation) != LiuqinUsbHostEligible)
  {
    return;
  }

  Status = gBS->HandleProtocol (mControllerHandle, &gEfiUsb2HcProtocolGuid, (VOID **)&UsbHc);
  if (EFI_ERROR (Status))
  {
    mObservation.Result = LiuqinUsbHostStackNotBound;
    return;
  }

  Status = UsbHc->GetState (UsbHc, &HcState);
  if (EFI_ERROR (Status) || (HcState != EfiUsbHcStateOperational))
  {
    mObservation.Result = LiuqinUsbHostStackNotRunning;
    return;
  }

  mReady.DmaReady = TRUE;
  mReady.ControllerReady = TRUE;
  VbusProductionArmIfReady ();
}

STATIC
VOID EFIAPI UsbHostConnected (EFI_EVENT Event, VOID *Context)
{
  (VOID) Event;
  (VOID) Context;
  UpdateReadyState ();
}

STATIC
VOID EFIAPI ReportUsbHandoff (EFI_EVENT Event, VOID *Context)
{
  (VOID) Event;
  (VOID) Context;
  UpdateReadyState ();
  if (!mReady.ControllerReady || !mReady.DmaReady)
  {
  }
}

EFI_STATUS
EFIAPI
UsbHostDxeEntryPoint (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE ReadyHandle;
  EFI_STATUS Status;

  (VOID) ImageHandle;
  (VOID) SystemTable;
  ReadyHandle = NULL;
  LiuqinUsbInitializeObservation (&mObservation);
  mObservation.Ports = 0;
  ZeroMem (mObservation.PortSc, sizeof (mObservation.PortSc));
  Status = gBS->InstallMultipleProtocolInterfaces (&ReadyHandle, &gLiuqinUsbReadyProtocolGuid,
                                                   &mReady, NULL);
  if (EFI_ERROR (Status))
  {
    return Status;
  }

  if (!RuntimeDescription ())
  {
    mObservation.Result = LiuqinUsbHostRuntimeDescriptionMismatch;
  }
  else
  {
    BOOLEAN Eligible;
    /* Validate and observe the inherited SPMI path before USB host writes. */
    CollectVbusSnapshot ();
    Eligible =
        LiuqinUsbProbeInheritedHost (ReadUsbRegister, NULL, &mObservation) == LiuqinUsbHostEligible;
    if (!Eligible)
    {
      /* The inherited core is still in the bootloader's device/OTG role.
         Apply the vendor's fixed host bring-up, then re-probe; every write is
         read back and the ordinary read-only gate decides eligibility. */
      Eligible = LiuqinUsbEnableHost (ReadUsbRegister, WriteUsbRegister, DelayUsbHalt, NULL,
                                      &mObservation);
    }

    if (Eligible)
    {
      {
        Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, StopUsbDma, NULL,
                                     &gEfiEventExitBootServicesGuid, &mExitBootServicesEvent);
        if (EFI_ERROR (Status))
        {
          mObservation.Result = LiuqinUsbHostExitHookUnavailable;
        }
        else
        {
          mRegistrationStatus = RegisterNonDiscoverableMmioDevice (
              NonDiscoverableDeviceTypeXhci, NonDiscoverableDeviceDmaTypeNonCoherent, NULL,
              &mControllerHandle, 1, (UINTN)LIUQIN_USB_MMIO_BASE, (UINTN)LIUQIN_USB_XHCI_SIZE);
          if (EFI_ERROR (mRegistrationStatus))
          {
            mControllerHandle = NULL;
            mObservation.Result = LiuqinUsbHostRegistrationFailed;
          }
          else
          {
            mObservation.Result = LiuqinUsbHostStackNotBound;
            /* Production VBUS starts here, at driver entry: the inherited host
             * path is proven eligible (USB core/PHY configured in host mode,
             * SMMU identity path) and the xHCI controller is registered, so
             * requirement R9 is met without waiting for the xHCI driver to
             * bind. The warmup powers a cold-plugged UFP before BDS scans
             * boot options; every reviewed gate, the PMIC write set and the
             * ExitBootServices stop path are unchanged. */
            VbusProductionStart (TRUE);
            Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, UsbHostConnected, NULL,
                                       &mUsbEvent);
            if (!EFI_ERROR (Status))
            {
              Status = gBS->RegisterProtocolNotify (&gEfiUsb2HcProtocolGuid, mUsbEvent,
                                                    &mUsbNotification);
              if (EFI_ERROR (Status))
              {
                gBS->CloseEvent (mUsbEvent);
              }
            }
          }
        }
      }
    }
  }

  /* Capture the port status once, after the role/bring-up decisions and before
     any upstream driver owns the controller: CCS tells "the port never saw a
     device" (VBUS/CC/Type-C role) apart from "it saw one but could not
     enumerate" (PHY/eUSB2 repeater). Read-only, no port write, no reset. */
  if ((mObservation.Capability != LIUQIN_USB_UNREAD) && (mObservation.Capability != 0xFFFFFFFFU) &&
      ((mObservation.HcsParams1 >> 24) != 0) && ((mObservation.HcsParams1 >> 24) <= 8))
  {
    UINT32 OpBase = LIUQIN_USB_MMIO_BASE + (mObservation.Capability & 0xFFU);
    UINT32 Port;
    mObservation.Ports = mObservation.HcsParams1 >> 24;
    for (Port = 0; Port < mObservation.Ports; ++Port)
    {
      mObservation.PortSc[Port] = ReadUsbRegister (NULL, OpBase + LIUQIN_USB_PORTSC_OFFSET +
                                                             LIUQIN_USB_PORTSC_STRIDE * Port);
    }
  }

  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ReportUsbHandoff, NULL,
                               &gEfiEventReadyToBootGuid, &mReadyToBootEvent);

  /* Keep the readiness protocol valid even when the controller is unavailable. */
  return EFI_SUCCESS;
}
