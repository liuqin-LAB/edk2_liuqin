#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile the fixed liuqin ACPI namespace and architecture tables."""
from __future__ import annotations

import hashlib
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PLATFORM = REPO
OUT_DIR = REPO / "Build/Acpi"
FIRMWARE_HEADER = REPO / "Build/Include/AcpiTables.generated.h"
ASL_DIR = PLATFORM / "LiuqinPkg/Acpi"
ASL_TABLES = {"dsdt": ("Dsdt.asl", "DSDT", "mLiuqinDsdt"),
              "ssdt-io": ("SsdtIo.asl", "SSDT", "mLiuqinSsdtIo")}
NOT_IMPLEMENTED = 0xFFFFFFFFFFFFFFFF
NUM_CPUS = 8
GICD_BASE = 0x17100000
GICR_BASE = 0x17180000
GICR_STRIDE = 0x40000
ITS_BASE = 0x17140000
EFFICIENCY = (0, 0, 0, 0, 1, 1, 1, 2)
TIMER_GSIV = (29, 30, 27, 26)


def header(sig: str, revision: int, table_id: str = "LIUQIN") -> bytearray:
    return bytearray(struct.pack("<4sIBB6s8sI4sI", sig.encode(), 0, revision, 0,
                                 b"LIUQIN", table_id.encode().ljust(8), 2,
                                 b"LIUQ", 0x20261009))


def finish(table: bytearray) -> bytes:
    struct.pack_into("<I", table, 4, len(table))
    table[9] = 0
    table[9] = -sum(table) & 0xFF
    return bytes(table)


def madt() -> bytes:
    table = header("APIC", 5)
    table += struct.pack("<II", 0, 0)
    for cpu in range(NUM_CPUS):
        # GICC, 80 bytes (ACPI 6.4), PMU PPI7, VGIC PPI9, no SPE.
        table += struct.pack("<BBHIIIIIQQQQIQQBBH", 0x0B, 80, 0, cpu, cpu, 1,
                             0, 23, 0, 0, 0, 0, 25, GICR_BASE + cpu * GICR_STRIDE,
                             cpu * 0x100, EFFICIENCY[cpu], 0, 0)
    table += struct.pack("<BBHIQIB3s", 0x0C, 24, 0, 0, GICD_BASE, 0, 3, b"\0\0\0")
    # Per-CPU GICC bases preserve DT's explicit stride without advertising
    # the unverified always-on/discovery contract of a Type E range.
    table += struct.pack("<BBHIQI", 0x0F, 20, 0, 0, ITS_BASE, 0)
    return finish(table)


def gtdt() -> bytes:
    table = header("GTDT", 2)
    table += struct.pack("<QI", NOT_IMPLEMENTED, 0)
    # ACPI 6.4 5.2.24: level/high with the always-on capability bit.
    for gsiv in TIMER_GSIV:
        table += struct.pack("<II", gsiv, 4)
    table += struct.pack("<QII", NOT_IMPLEMENTED, 1, 96)
    # frame0 only; other DT frames are disabled. Level/high SPI8/SPI6.
    table += struct.pack("<BHBQII B3s QQIIIII", 0, 60, 0, 0x17420000, 1, 20,
                         0, b"\0\0\0", 0x17421000, 0x17422000, 40, 0, 38, 0, 2)
    return finish(table)


def fadt() -> bytes:
    table = header("FACP", 6)
    table += bytearray(276 - 36)
    table[45] = 2  # Mobile (Laptop platform policy, matching SMBIOS chassis type 9)
    struct.pack_into("<I", table, 112, 1 << 20)  # HW_REDUCED; low power idle not implemented
    struct.pack_into("<H", table, 129, 1)  # PSCI compliant, SMC from DT
    table[131] = 3  # ACPI 6.3, whose FADT also has 276 bytes
    # AcpiTableDxe fills DSDT/X_DSDT and recomputes checksum when installed.
    return finish(table)


def bgrt() -> bytes:
    table = header("BGRT", 1)
    table += struct.pack("<HBBQII", 1, 0, 0, 0, 0, 0)  # No logo image supplied
    return finish(table)


def pptt() -> bytes:
    table = header("PPTT", 2)
    # No sizes/line lengths/associativity were provided by DT. Validity=0.
    # One shared L3, six L2s: pair 0/1, pair 2/3, private 4/5/6/7.
    cache_offsets = []
    for index in range(7):
        cache_offsets.append(len(table))
        table += struct.pack("<BBHIIII BBHI", 1, 28, 0, 0,
                             0 if index == 0 else cache_offsets[0], 0, 0, 0, 0, 0, 0)
    package_offset = len(table)
    table += struct.pack("<BBHIIII I", 0, 24, 0, 1, 0, 0, 1, cache_offsets[0])
    clusters = []
    for _ in range(3):
        clusters.append(len(table))
        table += struct.pack("<BBHIIII", 0, 20, 0, 0, package_offset, 0, 0)
    for cpu in range(8):
        cluster = 0 if cpu < 4 else (1 if cpu < 7 else 2)
        cache = (1, 1, 2, 2, 3, 4, 5, 6)[cpu]
        table += struct.pack("<BBHIIII I", 0, 24, 0, 2 | 8, clusters[cluster],
                             cpu, 1, cache_offsets[cache])
    return finish(table)


def tables() -> dict[str, bytes]:
    return {"madt": madt(), "gtdt": gtdt(), "fadt": fadt(), "bgrt": bgrt(), "pptt": pptt(),
            "mcfg": mcfg(), "iort": iort()}


def mcfg() -> bytes:
    # Only PCIe0, segment 0, buses 0..1. Firmware enables CFG_SHIFT and proves
    # present/absent config reads before publishing this allocation.
    table = header("MCFG", 1)
    table += struct.pack("<QQHBBI", 0, 0x60000000, 0, 0, 1, 0)
    return finish(table)


def iort() -> bytes:
    # DEN0049 E.d layouts; Cape DT supplies all SID/ITS and IRQ associations.
    # The live NS ID1=0x6000004B exposes 75 banks, while the Android DT lists
    # 96 possible interrupts and a secure-inclusive 78-bank override. The
    # firmware gate requires this exact NS layout; do not publish 96 NS banks.
    context_irqs = list(range(129, 151)) + list(range(213, 225)) + list(range(347, 378)) + list(range(427, 437))
    assert len(context_irqs) == 75
    def node(kind, revision, ident, payload, mappings=()):
        start = 16 + len(payload)
        return (struct.pack("<BHBIII", kind, start + 20 * len(mappings), revision,
                            ident, len(mappings), start if mappings else 0) + payload +
                b"".join(struct.pack("<IIIII", *mapping) for mapping in mappings))
    its_offset = 48
    its = node(0, 1, 0, struct.pack("<II", 1, 0))
    smmu_offset = its_offset + len(its)
    smmu_payload = struct.pack("<QQIIIIIIIII", 0x15000000, 0x100000, 1, 2,
                               60, 75, 76, 0, 0, 97, 0)
    smmu_payload += struct.pack("<II", 0, 0)
    smmu_payload += b"".join(struct.pack("<II", irq, 0) for irq in context_irqs)
    smmu = node(3, 3, 1, smmu_payload,
                [(0x1C00, 0, 0x5980, its_offset, 0), (0x1C01, 0, 0x5981, its_offset, 0)])
    root = node(2, 4, 2, struct.pack("<IBHB IIBHBI", 1, 0, 0, 1, 0, 0, 32, 0, 0, 0),
                [(0, 0, 0x1C00, smmu_offset, 0), (0x100, 0, 0x1C01, smmu_offset, 0)])
    nodes = [its, smmu, root]
    # Shared SMMU ownership includes the already published DMA masters.
    # Non-PCI masters use single-ID mappings; no guessed RID mask/ranges.
    for ident, (name, sid) in enumerate((('UFS0', 0xE0), ('USB0', 0),
                                       ('QUP0', 0x5A3), ('QUP1', 0x43), ('QUP2', 0x483),
                                       ('GPI0', 0x5B6), ('GPI1', 0x56), ('GPI2', 0x496)), 3):
        # IORT requires full ACPI paths with four-character NameSeg padding.
        payload = struct.pack("<IIBHBB", 0, 0, 0, 0, 0, 32) + ('\\_SB_.' + name + '\0').encode()
        payload += bytes((-16 - len(payload)) % 4)
        nodes.append(node(1, 4, ident, payload, [(0, 0, sid, smmu_offset, 1)]))
    table = header("IORT", 5)
    table += struct.pack("<III", len(nodes), 48, 0) + b"".join(nodes)
    return finish(table)


def compile_asl() -> tuple[dict[str, bytes], str]:
    """Compile the handwritten sources and local includes outside the source tree."""
    compiler = shutil.which("iasl")
    if compiler is None:
        raise ValueError("iasl is required to compile the production ASL")
    digest = hashlib.sha256()
    for source in sorted(path for path in ASL_DIR.rglob("*") if path.is_file()):
        relative = source.relative_to(ASL_DIR).as_posix().encode()
        contents = source.read_bytes()
        digest.update(len(relative).to_bytes(4, "big") + relative)
        digest.update(len(contents).to_bytes(8, "big") + contents)
    compiled = {}
    with tempfile.TemporaryDirectory(prefix="liuqin-asl-") as temporary:
        working = Path(temporary) / "Acpi"
        shutil.copytree(ASL_DIR, working)
        for name, (filename, signature, _) in ASL_TABLES.items():
            if not (working / filename).is_file():
                raise ValueError(f"missing production ASL: {filename}")
            result = subprocess.run([compiler, "-p", str(Path(temporary) / name),
                                     "-tc", "-we", "-ve", filename], cwd=working,
                                    text=True, capture_output=True, check=False)
            if result.returncode:
                raise ValueError(f"ASL compilation failed for {filename}:\n"
                                 + result.stdout + result.stderr)
            data = (Path(temporary) / f"{name}.aml").read_bytes()
            if (len(data) < 36 or data[:4] != signature.encode() or
                    struct.unpack_from("<I", data, 4)[0] != len(data) or sum(data) & 255):
                raise ValueError(f"invalid compiler output for {filename}")
            compiled[name] = data
    return compiled, digest.hexdigest()


def firmware_header(aml: dict[str, bytes] | None = None, source_digest: str | None = None) -> str:
    if aml is None or source_digest is None:
        aml, source_digest = compile_asl()
    lines = ["/* SPDX-License-Identifier: BSD-2-Clause-Patent */",
             "/* Generated by acpi.py; do not edit.",
             " */",
             f"/* ASL source/include SHA256: {source_digest} */",
             "#ifndef LIUQIN_ACPI_TABLES_GENERATED_H", "#define LIUQIN_ACPI_TABLES_GENERATED_H", ""]
    all_tables = tables()
    arrays = [("m" + "".join(word.capitalize() for word in name.split("_")), data)
              for name, data in all_tables.items()]
    arrays += [(ASL_TABLES[name][2], data) for name, data in aml.items()]
    for symbol, data in arrays:
        lines.append(f"STATIC CONST UINT8 {symbol}[] = {{")
        lines += ["  " + ", ".join(f"0x{b:02x}" for b in data[pos:pos + 12]) + ","
                  for pos in range(0, len(data), 12)]
        lines += ["};", ""]
    lines += ["#endif", ""]
    return "\n".join(lines)


def main():
    aml, digest = compile_asl()
    FIRMWARE_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, data in (tables() | aml).items():
        (OUT_DIR / f"{name}.aml").write_bytes(data)
    FIRMWARE_HEADER.write_text(firmware_header(aml, digest))


if __name__ == "__main__":
    main()
