/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include "UfsRuntimeDtb.h"

STATIC UINT32 Cell (CONST VOID *Data)
{
  CONST UINT8 *P = Data;
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) | ((UINT32)P[2] << 8) | P[3];
}

STATIC BOOLEAN U32Is (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 Expected)
{
  INT32 Length;
  CONST VOID *Data = FdtGetProp (Fdt, Node, Name, &Length);
  return Data != NULL && Length == 4 && Cell (Data) == Expected;
}

STATIC BOOLEAN Enabled (CONST VOID *Fdt, INT32 Node)
{
  INT32 Length;
  CONST CHAR8 *Status;
  for (; Node >= 0; Node = FdtParentOffset (Fdt, Node))
  {
    Status = FdtGetProp (Fdt, Node, "status", &Length);
    if (Status == NULL)
    {
      if (Length != -FDT_ERR_NOTFOUND)
        return FALSE;
    }
    else if (!((Length == 5 && CompareMem (Status, "okay", 5) == 0) ||
               (Length == 3 && CompareMem (Status, "ok", 3) == 0)))
      return FALSE;
  }
  return Node == -FDT_ERR_NOTFOUND;
}

STATIC BOOLEAN IsCompatible (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name)
{
  INT32 Length;
  CONST CHAR8 *Data = FdtGetProp (Fdt, Node, "compatible", &Length);
  return Data != NULL && Length > 0 && Data[Length - 1] == 0 &&
         FdtStringListContains (Data, Length, Name) == 1;
}

STATIC BOOLEAN UniqueHandle (CONST VOID *Fdt, INT32 Node)
{
  INT32 Other;
  UINT32 Handle = FdtGetPhandle (Fdt, Node);
  if (Handle == 0 || Handle == MAX_UINT32)
    return FALSE;
  for (Other = FdtNextNode (Fdt, -1, NULL); Other >= 0; Other = FdtNextNode (Fdt, Other, NULL))
  {
    if (Other != Node && FdtGetPhandle (Fdt, Other) == Handle)
      return FALSE;
  }
  return Other == -FDT_ERR_NOTFOUND;
}

STATIC BOOLEAN RefIs (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, INT32 Provider,
                      CONST UINT32 *Args, UINTN Count)
{
  INT32 Length;
  UINTN Index;
  CONST UINT8 *Data = FdtGetProp (Fdt, Node, Name, &Length);
  if (Data == NULL || Length != (INT32)((Count + 1) * 4) ||
      Cell (Data) != FdtGetPhandle (Fdt, Provider))
    return FALSE;
  for (Index = 0; Index < Count; ++Index)
  {
    if (Cell (Data + 4 + Index * 4) != Args[Index])
      return FALSE;
  }
  return TRUE;
}

STATIC BOOLEAN Region (CONST VOID *Fdt, INT32 Node, INT32 Soc, UINT32 Base, UINT32 Size)
{
  INT32 Length, Offset;
  CONST UINT8 *Data;
  UINT64 Child, Parent, Span, Delta;
  BOOLEAN Found = FALSE;
  if (FdtParentOffset (Fdt, Node) != Soc)
    return FALSE;
  Data = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Data == NULL || Length < 8 || Length % 8 != 0 || Cell (Data) != Base ||
      Cell (Data + 4) != Size)
    return FALSE;
  Data = FdtGetProp (Fdt, Soc, "ranges", &Length);
  if (Data == NULL || Length < 0 || Length % 16 != 0)
    return FALSE;
  if (Length == 0)
    return TRUE;
  for (Offset = 0; Offset < Length; Offset += 16)
  {
    Child = Cell (Data + Offset);
    Parent = ((UINT64)Cell (Data + Offset + 4) << 32) | Cell (Data + Offset + 8);
    Span = Cell (Data + Offset + 12);
    if (Span == 0 || Span - 1 > MAX_UINT32 - Child || Span - 1 > MAX_UINT64 - Parent)
      return FALSE;
    if (Base < Child)
    {
      if (Child - Base < Size)
        return FALSE; /* Partially overlapping mapping. */
      continue;
    }
    if (Base - Child >= Span)
      continue;
    Delta = Base - Child;
    if (Size > Span - Delta || Found || Parent + Delta != Base)
      return FALSE;
    Found = TRUE;
  }
  return Found;
}

STATIC BOOLEAN InterruptIsGic (CONST VOID *Fdt, INT32 Node, INT32 Soc)
{
  INT32 At, Length, Provider;
  UINT32 Handle = 0;
  CONST UINT8 *Data;
  if (FdtGetProp (Fdt, Node, "interrupts-extended", &Length) != NULL || Length != -FDT_ERR_NOTFOUND)
    return FALSE;
  for (At = Node; At >= 0; At = FdtParentOffset (Fdt, At))
  {
    if (FdtGetProp (Fdt, At, "interrupt-map", NULL) != NULL ||
        FdtGetProp (Fdt, At, "interrupt-map-mask", NULL) != NULL ||
        FdtGetProp (Fdt, At, "interrupt-map-pass-thru", NULL) != NULL ||
        FdtGetProp (Fdt, At, "interrupt-controller", NULL) != NULL ||
        FdtGetProp (Fdt, At, "#interrupt-cells", NULL) != NULL)
      return FALSE;
    Data = FdtGetProp (Fdt, At, "interrupt-parent", &Length);
    if (Data != NULL)
    {
      if (Length != 4)
        return FALSE;
      Handle = Cell (Data);
      break;
    }
    if (Length != -FDT_ERR_NOTFOUND)
      return FALSE;
  }
  Provider = FdtNodeOffsetByPhandle (Fdt, Handle);
  if (Handle == 0 || Provider < 0 || !Enabled (Fdt, Provider) || !UniqueHandle (Fdt, Provider) ||
      !IsCompatible (Fdt, Provider, "arm,gic-v3") ||
      !U32Is (Fdt, Provider, "#interrupt-cells", 3) ||
      !Region (Fdt, Provider, Soc, 0x17100000, 0x10000))
    return FALSE;
  Data = FdtGetProp (Fdt, Provider, "interrupt-controller", &Length);
  if (Data == NULL || Length != 0)
    return FALSE;
  if (FdtGetProp (Fdt, Provider, "interrupt-map", NULL) != NULL ||
      FdtGetProp (Fdt, Provider, "interrupt-map-mask", NULL) != NULL ||
      FdtGetProp (Fdt, Provider, "interrupt-map-pass-thru", NULL) != NULL)
    return FALSE;
  Data = FdtGetProp (Fdt, Node, "interrupts", &Length);
  return Data != NULL && Length == 12 && Cell (Data) == 0 && Cell (Data + 4) == 265 &&
         Cell (Data + 8) == 4;
}

BOOLEAN LiuqinUfsRuntimeDescription (CONST VOID *Fdt)
{
  INT32 Soc, Ufs, Smmu, Gcc, Gdsc, Len;
  CONST UINT8 *Data;
  STATIC CONST UINT32 Reset[] = {25};
  STATIC CONST UINT32 Iommu[] = {0xE0, 0};
  STATIC CONST UINT32 ClockIds[] = {151, 10, 150, 166, 154};
  UINTN Index;
  if (Fdt == NULL)
    return FALSE;
  Soc = FdtPathOffset (Fdt, "/soc");
  Ufs = FdtPathOffset (Fdt, "/soc/ufshc@1d84000");
  Smmu = FdtPathOffset (Fdt, "/soc/apps-smmu@15000000");
  Gcc = FdtPathOffset (Fdt, "/soc/clock-controller@100000");
  Gdsc = FdtPathOffset (Fdt, "/soc/qcom,gdsc@187004");
  if (Soc < 0 || Ufs < 0 || Smmu < 0 || Gcc < 0 || Gdsc < 0 || FdtParentOffset (Fdt, Soc) != 0 ||
      !U32Is (Fdt, Soc, "#address-cells", 1) || !U32Is (Fdt, Soc, "#size-cells", 1) ||
      !U32Is (Fdt, 0, "#address-cells", 2) || !U32Is (Fdt, 0, "#size-cells", 2) ||
      !Enabled (Fdt, Ufs) || !Enabled (Fdt, Smmu) || !Enabled (Fdt, Gcc) || !Enabled (Fdt, Gdsc) ||
      !UniqueHandle (Fdt, Smmu) || !UniqueHandle (Fdt, Gcc) || !UniqueHandle (Fdt, Gdsc) ||
      !IsCompatible (Fdt, Ufs, "qcom,ufshc") || !IsCompatible (Fdt, Smmu, "qcom,qsmmu-v500") ||
      !(IsCompatible (Fdt, Gcc, "qcom,cape-gcc") || IsCompatible (Fdt, Gcc, "qcom,waipio-gcc")) ||
      !IsCompatible (Fdt, Gdsc, "qcom,gdsc") || !U32Is (Fdt, Gcc, "#clock-cells", 1) ||
      !U32Is (Fdt, Gcc, "#reset-cells", 1) || !U32Is (Fdt, Smmu, "#iommu-cells", 2) ||
      !Region (Fdt, Ufs, Soc, 0x01D84000, 0x3000) ||
      !Region (Fdt, Gcc, Soc, 0x00100000, 0x1F4200) || !Region (Fdt, Gdsc, Soc, 0x00187004, 4) ||
      !Region (Fdt, Smmu, Soc, 0x15000000, 0x100000) || !InterruptIsGic (Fdt, Ufs, Soc) ||
      !RefIs (Fdt, Ufs, "vdd-hba-supply", Gdsc, NULL, 0) ||
      !RefIs (Fdt, Ufs, "resets", Gcc, Reset, ARRAY_SIZE (Reset)) ||
      !RefIs (Fdt, Ufs, "iommus", Smmu, Iommu, ARRAY_SIZE (Iommu)))
    return FALSE;
  Data = FdtGetProp (Fdt, Ufs, "clocks", &Len);
  if (Data == NULL || Len < 40 || Len % 8 != 0)
    return FALSE;
  for (Index = 0; Index < ARRAY_SIZE (ClockIds); ++Index)
  {
    if (Cell (Data + Index * 8) != FdtGetPhandle (Fdt, Gcc) ||
        Cell (Data + Index * 8 + 4) != ClockIds[Index])
      return FALSE;
  }
  Data = FdtGetProp (Fdt, Ufs, "dma-coherent", &Len);
  return Data != NULL && Len == 0;
}
