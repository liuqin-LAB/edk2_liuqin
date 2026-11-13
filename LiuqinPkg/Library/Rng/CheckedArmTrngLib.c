/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Base.h>
#include <IndustryStandard/ArmStdSmc.h>
#include <Library/ArmMonitorLib.h>
#include <Library/ArmTrngLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>

#define LIUQIN_TRNG_MAX_BITS 192

STATIC BOOLEAN mAvailable;
STATIC BOOLEAN mUuidAvailable;
STATIC UINT16 mMajor;
STATIC UINT16 mMinor;

// All reserved input registers are zero for every monitor call.
STATIC VOID TrngCall (UINT32 Function, UINTN Argument, ARM_MONITOR_ARGS *Args)
{
  ZeroMem (Args, sizeof (*Args));
  Args->Arg0 = Function;
  Args->Arg1 = Argument;
  ArmMonitorCall (Args);
}

RETURN_STATUS EFIAPI LiuqinArmTrngConstructor (VOID)
{
  ARM_MONITOR_ARGS Args;
  UINT32 Revision;

  mAvailable = FALSE;
  mUuidAvailable = FALSE;
  mMajor = 0;
  mMinor = 0;
  TrngCall (SMCCC_VERSION, 0, &Args);
  Revision = (UINT32)Args.Arg0;
  // DEN0028 SMCCC 1.1 is the DEN0098 caller prerequisite. An unknown
  // major ABI or an error is not treated as a compatible implementation.
  if ((Revision >> 16) != 1 || (Revision & 0xFFFF) < 1)
  {
    goto Unavailable;
  }
  TrngCall (ARM_SMC_ID_TRNG_VERSION, 0, &Args);
  Revision = (UINT32)Args.Arg0;
  if ((Revision >> 16) != 1)
  {
    goto Unavailable;
  }
  mMajor = (UINT16)(Revision >> 16);
  mMinor = (UINT16)Revision;
  TrngCall (ARM_SMC_ID_TRNG_FEATURES, ARM_SMC_ID_TRNG_RND_AARCH64, &Args);
  if ((INT32)Args.Arg0 < 0)
  {
    goto Unavailable;
  }
  // UUID is optional and is never used as an authentication decision.
  TrngCall (ARM_SMC_ID_TRNG_FEATURES, ARM_SMC_ID_TRNG_GET_UUID, &Args);
  mUuidAvailable = (BOOLEAN)((INT32)Args.Arg0 >= 0);
  mAvailable = TRUE;

  return RETURN_SUCCESS;

Unavailable:

  // A missing TRNG does not prevent the CPU RNDR provider from loading.
  return RETURN_SUCCESS;
}

RETURN_STATUS EFIAPI GetArmTrngVersion (OUT UINT16 *MajorRevision, OUT UINT16 *MinorRevision)
{
  if (MajorRevision == NULL || MinorRevision == NULL)
  {
    return RETURN_INVALID_PARAMETER;
  }
  if (!mAvailable)
  {
    return RETURN_UNSUPPORTED;
  }
  *MajorRevision = mMajor;
  *MinorRevision = mMinor;
  return RETURN_SUCCESS;
}

RETURN_STATUS EFIAPI GetArmTrngUuid (OUT GUID *Guid)
{
  ARM_MONITOR_ARGS Args;

  if (Guid == NULL)
  {
    return RETURN_INVALID_PARAMETER;
  }
  ZeroMem (Guid, sizeof (*Guid));
  if (!mAvailable || !mUuidAvailable)
  {
    return RETURN_UNSUPPORTED;
  }
  TrngCall (ARM_SMC_ID_TRNG_GET_UUID, 0, &Args);
  if ((INT32)Args.Arg0 == TRNG_STATUS_NOT_SUPPORTED)
  {
    return RETURN_UNSUPPORTED;
  }
  Guid->Data1 = (UINT32)Args.Arg0;
  Guid->Data2 = (UINT16)Args.Arg1;
  Guid->Data3 = (UINT16)(Args.Arg1 >> 16);
  Guid->Data4[0] = (UINT8)Args.Arg2;
  Guid->Data4[1] = (UINT8)(Args.Arg2 >> 8);
  Guid->Data4[2] = (UINT8)(Args.Arg2 >> 16);
  Guid->Data4[3] = (UINT8)(Args.Arg2 >> 24);
  Guid->Data4[4] = (UINT8)Args.Arg3;
  Guid->Data4[5] = (UINT8)(Args.Arg3 >> 8);
  Guid->Data4[6] = (UINT8)(Args.Arg3 >> 16);
  Guid->Data4[7] = (UINT8)(Args.Arg3 >> 24);
  return RETURN_SUCCESS;
}

UINTN EFIAPI GetArmTrngMaxSupportedEntropyBits (VOID)
{
  return LIUQIN_TRNG_MAX_BITS;
}

RETURN_STATUS EFIAPI GetArmTrngEntropy (IN UINTN EntropyBits, IN UINTN BufferSize,
                                        OUT UINT8 *Buffer)
{
  ARM_MONITOR_ARGS Args;
  UINTN Data[3];
  UINTN Bytes;
  RETURN_STATUS Status;

  if (Buffer == NULL || EntropyBits == 0 || EntropyBits > LIUQIN_TRNG_MAX_BITS)
  {
    return RETURN_INVALID_PARAMETER;
  }
  Bytes = (EntropyBits + 7) / 8;
  if (BufferSize < Bytes)
  {
    return RETURN_BAD_BUFFER_SIZE;
  }
  ZeroMem (Buffer, BufferSize);
  if (!mAvailable)
  {
    return RETURN_UNSUPPORTED;
  }
  TrngCall (ARM_SMC_ID_TRNG_RND_AARCH64, EntropyBits, &Args);
  // RND64 success is exactly X0=0, not just a zero low word.
  if (Args.Arg0 == 0)
  {
    Data[0] = Args.Arg3;
    Data[1] = Args.Arg2;
    Data[2] = Args.Arg1;
    CopyMem (Buffer, Data, Bytes);
    if ((EntropyBits & 7) != 0)
    {
      Buffer[Bytes - 1] &= (UINT8)(0xFF >> (8 - (EntropyBits & 7)));
    }
    ZeroMem (Data, sizeof (Data));
    Status = RETURN_SUCCESS;
  }
  else if ((INTN)Args.Arg0 == TRNG_STATUS_NO_ENTROPY)
  {
    Status = RETURN_NOT_READY;
  }
  else if ((INTN)Args.Arg0 == TRNG_STATUS_INVALID_PARAMETER)
  {
    Status = RETURN_INVALID_PARAMETER;
  }
  else if ((INTN)Args.Arg0 == TRNG_STATUS_NOT_SUPPORTED)
  {
    Status = RETURN_UNSUPPORTED;
  }
  else
  {
    Status = RETURN_DEVICE_ERROR;
  }
  ZeroMem (&Args, sizeof (Args));
  return Status;
}
