/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LIUQIN_USB_HOST_POLICY_H
#define LIUQIN_USB_HOST_POLICY_H

#include <Base.h>

#define LIUQIN_USB_MMIO_BASE 0x0A600000U
#define LIUQIN_USB_XHCI_SIZE 0x00008000U
#define LIUQIN_USB_GSIV 165U
#define LIUQIN_USB_GCC_BASE 0x00100000U
#define LIUQIN_USB_SMMU_BASE 0x15000000U
/* The runtime DTB check requires exactly this SMMU window. */
#define LIUQIN_USB_SMMU_WINDOW 0x100000U
#define LIUQIN_USB_STREAM_ID 0U
#define LIUQIN_USB_SMMU_MAX_GROUPS 120U
#define LIUQIN_USB_UNREAD 0xFFFFFFFEU
#define LIUQIN_USB_CLOCK_COUNT 5U
#define LIUQIN_USB_HALT_WAIT_US 1000U
#define LIUQIN_USB_HALT_MAX_POLLS 100U

#define LIUQIN_USB_GDSC 0x49004U
#define LIUQIN_USB_BCR 0x49000U
#define LIUQIN_USB_GCTL 0xC110U
#define LIUQIN_USB_GSNPSID 0xC120U
#define LIUQIN_USB_GHWPARAMS0 0xC140U
#define LIUQIN_USB_GUSB2PHYCFG 0xC200U
#define LIUQIN_USB_GUSB3PIPECTL 0xC2C0U
#define LIUQIN_USB_SMMU_CR0 0x000U
#define LIUQIN_USB_SMMU_ID0 0x020U
#define LIUQIN_USB_SMMU_ID1 0x024U
#define LIUQIN_USB_SMMU_SMR 0x800U
#define LIUQIN_USB_SMMU_S2CR 0xC00U

#define LIUQIN_USB_GCTL_PRTCAPDIR_MASK 0x00003000U
#define LIUQIN_USB_GCTL_PRTCAPDIR_HOST 0x00001000U
#define LIUQIN_USB_GCTL_PRTCAPDIR_DEVICE 0x00002000U
#define LIUQIN_USB_GCTL_CORESOFTRESET 0x00000800U
#define LIUQIN_USB_GCTL_U2EXIT_LFPS 0x00000004U
#define LIUQIN_USB_GCTL_DSBLCLKGTNG 0x00000001U
#define LIUQIN_USB_HCCPARAMS1_OFFSET 0x00000010U
#define LIUQIN_USB_HCCPARAMS1_PPC 0x00000008U
#define LIUQIN_USB_PORTSC_OFFSET 0x00000400U
#define LIUQIN_USB_PORTSC_STRIDE 0x00000010U
#define LIUQIN_USB_PORTSC_WCE 0x02000000U
#define LIUQIN_USB_PORTSC_PP 0x00000200U
#define LIUQIN_USB_GRXTHRCFG 0x0000C10CU
#define LIUQIN_USB_GSBUSCFG1 0x0000C104U
#define LIUQIN_USB_GUCTL1 0x0000C11CU
#define LIUQIN_USB_GUCTL3 0x0000C60CU
#define LIUQIN_USB_GRXTHRCFG_HOST 0x04660000U
#define LIUQIN_USB_GSBUSCFG1_HOST 0x00001E00U
#define LIUQIN_USB_GSBUSCFG1_EN1KPAGE 0x00001000U
#define LIUQIN_USB_GUCTL3_HOST 0x00010000U
#define LIUQIN_USB_GUCTL1_HOST 0x00600000U
#define LIUQIN_USB_HOST_WAIT_US 100U

typedef UINT32 (*LIUQIN_USB_READ32) (VOID *Context, UINTN Address);
typedef VOID (*LIUQIN_USB_WRITE32) (VOID *Context, UINTN Address, UINT32 Value);
typedef VOID (*LIUQIN_USB_DELAY) (VOID *Context, UINTN Microseconds);

typedef enum
{
  LiuqinUsbHostEligible,
  LiuqinUsbHostNotProbed,
  LiuqinUsbHostPowerOff,
  LiuqinUsbHostClockOff,
  LiuqinUsbHostCoreReset,
  LiuqinUsbHostBadCoreId,
  LiuqinUsbHostUnsupportedCore,
  LiuqinUsbHostBoardPrepareRequired,
  LiuqinUsbHostPhyReset,
  LiuqinUsbHostSmmuUnknown,
  LiuqinUsbHostSmmuDisabled,
  LiuqinUsbHostSmmuAmbiguous,
  LiuqinUsbHostSmmuTranslation,
  LiuqinUsbHostSmmuFault,
  LiuqinUsbHostBadCapabilities,
  LiuqinUsbHostStackNotBound,
  LiuqinUsbHostStackNotRunning,
  LiuqinUsbHostRegistrationFailed,
  LiuqinUsbHostEmulatorSkipped,
  LiuqinUsbHostRuntimeDescriptionMismatch,
  LiuqinUsbHostExitHookUnavailable,
  LiuqinUsbHostHaltFailed,
  LiuqinUsbHostHalted,
  LiuqinUsbHostLifecycleUnproven,
  LiuqinUsbHostBringupFailed
} LIUQIN_USB_HOST_RESULT;

typedef struct
{
  UINT32 Gdsc;
  UINT32 Bcr;
  UINT32 Clocks[LIUQIN_USB_CLOCK_COUNT];
  UINT32 GsnpsId;
  UINT32 Gctl;
  UINT32 HwParams0;
  UINT32 Usb2Phy;
  UINT32 Usb3Phy;
  UINT32 SmmuId0;
  UINT32 SmmuCr0;
  UINT32 SmmuMatches;
  UINT32 MatchingSmr;
  UINT32 MatchingS2cr;
  UINT32 Ports;
  UINT32 PortSc[8];
  UINT32 SmmuBank;
  UINT32 SmmuSctlr;
  /* 0 unread, 1 S2CR_TYPE_BYPASS, 2 TYPE0 into a disabled context bank with a
     non-CCA variant, 3 TYPE0 into a disabled context bank with the CCA
     variant. Only 1..3 permit DMA. */
  UINT32 SmmuVariant;
  UINT32 Capability;
  UINT32 HcsParams1;
  UINT32 DoorbellOffset;
  UINT32 RuntimeOffset;
  UINT32 UsbCommand;
  UINT32 UsbStatus;
  BOOLEAN DmaReady;
  LIUQIN_USB_HOST_RESULT Result;
} LIUQIN_USB_HOST_OBSERVATION;

/* Read-only policy. No board registers or SMMU state are modified. */
VOID LiuqinUsbInitializeObservation (LIUQIN_USB_HOST_OBSERVATION *Observation);

LIUQIN_USB_HOST_RESULT
LiuqinUsbProbeInheritedHost (LIUQIN_USB_READ32 Read32, VOID *Context,
                             LIUQIN_USB_HOST_OBSERVATION *Observation);

/* The independent KDNET-EEM profile inherits ABL's working peripheral PHY and
 * repeater. It never converts a host into a peripheral, nor changes power or
 * SMMU state. The same power/clock/PHY/SID0 identity-DMA gate is required, but
 * xHCI capabilities are irrelevant in peripheral mode. */
LIUQIN_USB_HOST_RESULT
LiuqinUsbProbeInheritedDevice (LIUQIN_USB_READ32 Read32, VOID *Context,
                               LIUQIN_USB_HOST_OBSERVATION *Observation);

/* DWC3 core.h / Palima HalusbHWIO.h: DCTL RUN_STOP[31], CSFTRST[30],
 * DSTS DEVCTRLHLT[22]. Only DCTL.RUN_STOP and DEVTEN may be written here.
 * A bounded, read-back halt retires inherited ABL device DMA before KD owns
 * the core; never reset/restart it at ExitBootServices after KD may bind. */
#define LIUQIN_USB_DCTL 0xC704U
#define LIUQIN_USB_DEVTEN 0xC708U
#define LIUQIN_USB_DSTS 0xC70CU
#define LIUQIN_USB_DCTL_RUN_STOP 0x80000000U
#define LIUQIN_USB_DCTL_CSFTRST 0x40000000U
#define LIUQIN_USB_DSTS_HALTED 0x00400000U
BOOLEAN
LiuqinUsbQuiesceDevice (LIUQIN_USB_READ32 Read32, LIUQIN_USB_WRITE32 Write32,
                        LIUQIN_USB_DELAY Delay, VOID *Context,
                        LIUQIN_USB_HOST_OBSERVATION *Observation);

/* EBS only: revalidate the inherited host, clear operational USBCMD R/S and
 * INTE, then prove HCHalted with readback. Never reset the core, clear DMA
 * pointers, free queues, change roles or write clocks/PHY/SMMU registers.
 * Failure must prevent the caller from handing BootServices RAM to the OS. */
BOOLEAN
LiuqinUsbQuiesceHost (LIUQIN_USB_READ32 Read32, LIUQIN_USB_WRITE32 Write32, LIUQIN_USB_DELAY Delay,
                      VOID *Context, LIUQIN_USB_HOST_OBSERVATION *Observation);

/* Board host bring-up: switch the inherited DWC3 wrapper from the
 * bootloader's device/OTG role to xHCI host and apply the vendor's fixed
 * host-mode tuning. Every write is read back; the final normal read-only gate
 * must accept the result. No PHY, clock, repeater, VBUS or SMMU register is
 * touched. Fail-closed: any unreadable register, missing role readback or
 * missing tuning bit returns FALSE and the controller is never published. */
BOOLEAN
LiuqinUsbEnableHost (LIUQIN_USB_READ32 Read32, LIUQIN_USB_WRITE32 Write32, LIUQIN_USB_DELAY Delay,
                     VOID *Context, LIUQIN_USB_HOST_OBSERVATION *Observation);

const char *LiuqinUsbHostResultText (LIUQIN_USB_HOST_RESULT Result);

#endif
