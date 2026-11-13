#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack the ARM64 entry shim and firmware into an Android boot-v4 image."""
from __future__ import annotations
import argparse
from pathlib import Path
import shutil
import struct
import sys
import zlib

PAGE = 4096
FD_SIZE = 0x01000000
BOOT_LIMIT = 0x0C000000
GZIP_HEADER = bytes.fromhex("1f8b08000000000000ff")


def stored_gzip(data: bytes) -> bytes:
    result = bytearray(GZIP_HEADER)
    chunks = [data[index:index + 65535] for index in range(0, len(data), 65535)] or [b""]
    for index, chunk in enumerate(chunks):
        length = len(chunk)
        result += struct.pack("<BHH", int(index == len(chunks) - 1), length, length ^ 0xFFFF)
        result += chunk
    result += struct.pack("<II", zlib.crc32(data), len(data))
    return bytes(result)


def pack(shim: bytes, fd: bytes) -> bytes:
    if len(fd) != FD_SIZE:
        raise ValueError(f"FD 必须为16 MiB / FD must be 16 MiB: {len(fd):#x}")
    if not 64 <= len(shim) <= 0x100000 or len(shim) % PAGE:
        raise ValueError("BootShim 必须按4096对齐且≤1 MiB / Invalid aligned shim size")
    if shim[56:60] != b"ARM\x64" or struct.unpack_from("<Q", shim, 8)[0] != 0x80000:
        raise ValueError("缺少Linux ARM64 Image头 / Missing arm64 Linux Image header")
    if shim[16:56] != bytes(40) or shim[60:64] != bytes(4):
        raise ValueError("BootShim 模板头非空 / Nonzero BootShim template header")
    payload = stored_gzip(fd)
    kernel = bytearray(shim) + payload
    struct.pack_into("<Q", kernel, 16, len(kernel))
    header = bytearray(PAGE)
    header[0:8] = b"ANDROID!"
    struct.pack_into("<IIII", header, 8, len(kernel), 0, 0, 1584)
    struct.pack_into("<I", header, 40, 4)
    # Reserved, cmdline and signature_size remain zero; no DTB in v4.
    image = bytes(header) + bytes(kernel) + bytes((-len(kernel)) % PAGE)
    if len(image) > BOOT_LIMIT:
        raise ValueError("boot镜像超出192 MiB / Boot image exceeds 192 MiB")
    return image


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("-b", "--buildtarget", default="RELEASE")
    parser.add_argument("-t", "--tagname", default="CLANGPDB")
    parser.add_argument("-m", "--module")
    options, _ = parser.parse_known_args()
    if any(arg in {"clean", "cleanall", "cleanlib"} for arg in sys.argv[1:]):
        for name in ("boot-liuqin.img", "boot-liuqin.img.sha256"):
            (root / name).unlink(missing_ok=True)
        if "cleanall" in sys.argv[1:]:
            for name in ("Acpi", "Include", "BootShim"):
                shutil.rmtree(root / "Build" / name, ignore_errors=True)
            (root / "Build/BootShim.bin").unlink(missing_ok=True)
        return
    if options.module or any(arg in {"genc", "genmake", "modules", "libraries", "run"}
                             for arg in sys.argv[1:]):
        return
    firmware = root / "Build/Firmware" / (options.buildtarget + "_" + options.tagname)
    image = pack((root / "Build/BootShim.bin").read_bytes(),
                 (firmware / "FV/LIUQIN_UEFI.fd").read_bytes())
    output = root / "boot-liuqin.img"
    output.write_bytes(image)
    import hashlib
    output.with_suffix(".img.sha256").write_text(
        f"{hashlib.sha256(image).hexdigest()}  {output.name}\n")
    print(f"{output.name}: {len(image)} bytes (Android boot v4)")


if __name__ == "__main__":
    main()
