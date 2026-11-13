/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/Hash.h>
#include <Protocol/ServiceBinding.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

typedef struct HASH_CHILD HASH_CHILD;
struct HASH_CHILD
{
  EFI_HASH_PROTOCOL Protocol;
  EFI_HANDLE Handle;
  HASH_CHILD *Next;
  BOOLEAN Destroying;
};

STATIC HASH_CHILD *mChildren;
STATIC EFI_HANDLE mBindingHandle;

STATIC UINT32 ReadBig32 (CONST UINT8 *Bytes)
{
  return ((UINT32)Bytes[0] << 24) | ((UINT32)Bytes[1] << 16) | ((UINT32)Bytes[2] << 8) | Bytes[3];
}

STATIC UINT32 Rotate (UINT32 Value, UINTN Count)
{
  return (Value >> Count) | (Value << (32 - Count));
}

STATIC VOID Compress (UINT32 State[8], CONST UINT8 *Block)
{
  STATIC CONST UINT32 K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  UINT32 W[64], A, B, C, D, E, F, G, H, T1, T2, S0, S1;
  UINTN I;

  for (I = 0; I < 16; ++I)
    W[I] = ReadBig32 (Block + I * 4);
  for (I = 16; I < 64; ++I)
  {
    S0 = Rotate (W[I - 15], 7) ^ Rotate (W[I - 15], 18) ^ (W[I - 15] >> 3);
    S1 = Rotate (W[I - 2], 17) ^ Rotate (W[I - 2], 19) ^ (W[I - 2] >> 10);
    W[I] = W[I - 16] + S0 + W[I - 7] + S1;
  }
  A = State[0];
  B = State[1];
  C = State[2];
  D = State[3];
  E = State[4];
  F = State[5];
  G = State[6];
  H = State[7];
  for (I = 0; I < 64; ++I)
  {
    S1 = Rotate (E, 6) ^ Rotate (E, 11) ^ Rotate (E, 25);
    T1 = H + S1 + ((E & F) ^ (~E & G)) + K[I] + W[I];
    S0 = Rotate (A, 2) ^ Rotate (A, 13) ^ Rotate (A, 22);
    T2 = S0 + ((A & B) ^ (A & C) ^ (B & C));
    H = G;
    G = F;
    F = E;
    E = D + T1;
    D = C;
    C = B;
    B = A;
    A = T1 + T2;
  }
  State[0] += A;
  State[1] += B;
  State[2] += C;
  State[3] += D;
  State[4] += E;
  State[5] += F;
  State[6] += G;
  State[7] += H;
}

STATIC UINT32 RotateLeft (UINT32 Value, UINTN Count)
{
  return (Value << Count) | (Value >> (32 - Count));
}

// FIPS 180-4 section 6.1.2 / 6.1.3. The caller owns padding: every 64-byte
// block here is a complete SHA-1 message block.
STATIC VOID CompressSha1 (UINT32 State[5], CONST UINT8 *Block)
{
  UINT32 W[80], A, B, C, D, E, F, K, T;
  UINTN I;

  for (I = 0; I < 16; ++I)
    W[I] = ReadBig32 (Block + I * 4);
  for (I = 16; I < 80; ++I)
  {
    W[I] = RotateLeft (W[I - 3] ^ W[I - 8] ^ W[I - 14] ^ W[I - 16], 1);
  }
  A = State[0];
  B = State[1];
  C = State[2];
  D = State[3];
  E = State[4];
  for (I = 0; I < 80; ++I)
  {
    if (I < 20)
    {
      F = (B & C) | (~B & D);
      K = 0x5a827999;
    }
    else if (I < 40)
    {
      F = B ^ C ^ D;
      K = 0x6ed9eba1;
    }
    else if (I < 60)
    {
      F = (B & C) | (B & D) | (C & D);
      K = 0x8f1bbcdc;
    }
    else
    {
      F = B ^ C ^ D;
      K = 0xca62c1d6;
    }
    T = RotateLeft (A, 5) + F + E + K + W[I];
    E = D;
    D = C;
    C = RotateLeft (B, 30);
    B = A;
    A = T;
  }
  State[0] += A;
  State[1] += B;
  State[2] += C;
  State[3] += D;
  State[4] += E;
}

STATIC EFI_STATUS EFIAPI GetHashSize (CONST EFI_HASH_PROTOCOL *This, CONST EFI_GUID *Algorithm,
                                      UINTN *Size)
{
  if (This == NULL || Algorithm == NULL || Size == NULL)
    return EFI_INVALID_PARAMETER;
  if (CompareGuid (Algorithm, &gEfiHashAlgorithmSha256NoPadGuid))
  {
    *Size = sizeof (EFI_SHA256_HASH);
    return EFI_SUCCESS;
  }
  if (CompareGuid (Algorithm, &gEfiHashAlgorithmSha1Guid))
  {
    *Size = sizeof (EFI_SHA1_HASH);
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI HashBlocks (CONST EFI_HASH_PROTOCOL *This, CONST EFI_GUID *Algorithm,
                                     BOOLEAN Extend, CONST UINT8 *Message, UINT64 Size,
                                     EFI_HASH_OUTPUT *Hash)
{
  UINT32 State[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  UINT8 *Digest;
  UINTN I, Offset, DigestSize, Words;
  if (This == NULL || Algorithm == NULL || Message == NULL || Hash == NULL ||
      Hash->Sha256Hash == NULL || Size == 0 || (Size % 64) != 0 || Size > MAX_UINTN ||
      Size > MAX_UINTN - (UINTN)Message)
    return EFI_INVALID_PARAMETER;
  // The union members share the caller's pointer; an unsupported algorithm
  // must still return EFI_UNSUPPORTED and leave it untouched.
  if (CompareGuid (Algorithm, &gEfiHashAlgorithmSha256NoPadGuid))
  {
    DigestSize = sizeof (EFI_SHA256_HASH);
    Words = 8;
  }
  else if (CompareGuid (Algorithm, &gEfiHashAlgorithmSha1Guid))
  {
    State[0] = 0x67452301;
    State[1] = 0xefcdab89;
    State[2] = 0x98badcfe;
    State[3] = 0x10325476;
    State[4] = 0xc3d2e1f0;
    DigestSize = sizeof (EFI_SHA1_HASH);
    Words = 5;
  }
  else
  {
    return EFI_UNSUPPORTED;
  }
  Digest = (UINT8 *)Hash->Sha256Hash;
  if (Extend)
  {
    for (I = 0; I < Words; ++I)
      State[I] = ReadBig32 (Digest + I * 4);
  }
  for (Offset = 0; Offset < (UINTN)Size; Offset += 64)
  {
    if (Words == 8)
    {
      Compress (State, Message + Offset);
    }
    else
    {
      CompressSha1 (State, Message + Offset);
    }
  }
  for (I = 0; I < DigestSize; ++I)
  {
    Digest[I] = (UINT8)(State[I / 4] >> (24 - (I % 4) * 8));
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI CreateChild (EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE *Handle);
STATIC EFI_STATUS EFIAPI DestroyChild (EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE Handle);
STATIC EFI_SERVICE_BINDING_PROTOCOL mBinding = {CreateChild, DestroyChild};

STATIC EFI_STATUS EFIAPI CreateChild (EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE *Handle)
{
  HASH_CHILD *Child;
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  if (This != &mBinding || Handle == NULL)
    return EFI_INVALID_PARAMETER;
  Child = AllocateZeroPool (sizeof (*Child));
  if (Child == NULL)
    return EFI_OUT_OF_RESOURCES;
  Child->Protocol.GetHashSize = GetHashSize;
  Child->Protocol.Hash = HashBlocks;
  // Install dispatches protocol notifications when it restores TPL. Keep the
  // instance private until its handle and ownership list are both complete.
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = gBS->InstallMultipleProtocolInterfaces (Handle, &gEfiHashProtocolGuid, &Child->Protocol,
                                                   NULL);
  if (EFI_ERROR (Status))
  {
    FreePool (Child);
    gBS->RestoreTPL (OldTpl);
    return Status;
  }
  Child->Handle = *Handle;
  Child->Next = mChildren;
  mChildren = Child;

  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI DestroyChild (EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE Handle)
{
  HASH_CHILD *Child, **Link;
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  if (This != &mBinding || Handle == NULL)
    return EFI_INVALID_PARAMETER;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  for (Child = mChildren; Child != NULL; Child = Child->Next)
  {
    if (Child->Handle == Handle)
      break;
  }
  if (Child == NULL)
  {
    gBS->RestoreTPL (OldTpl);
    return EFI_UNSUPPORTED;
  }
  if (Child->Destroying)
  {
    gBS->RestoreTPL (OldTpl);
    return EFI_ACCESS_DENIED;
  }
  Child->Destroying = TRUE;
  // The multiple-interface wrapper changes every failure to INVALID_PARAMETER.
  // Preserve ACCESS_DENIED when a consumer refuses to release this protocol.
  Status = gBS->UninstallProtocolInterface (Handle, &gEfiHashProtocolGuid, &Child->Protocol);
  if (EFI_ERROR (Status))
  {
    Child->Destroying = FALSE;
    gBS->RestoreTPL (OldTpl);
    return Status;
  }

  for (Link = &mChildren; *Link != Child; Link = &(*Link)->Next)
  {
  }
  *Link = Child->Next;
  FreePool (Child);
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI HashEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  (VOID) ImageHandle;
  (VOID) SystemTable;
  return gBS->InstallMultipleProtocolInterfaces (
      &mBindingHandle, &gEfiHashServiceBindingProtocolGuid, &mBinding, NULL);
}
