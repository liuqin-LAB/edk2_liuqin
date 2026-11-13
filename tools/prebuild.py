#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate platform inputs for the standard EDK II build command."""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "Build"


def tool(name):
    """Resolve an external tool, including names with dots such as ld.lld.

    CreateProcess() only appends .exe when the name has no extension, so a
    bare "ld.lld" fails on Windows even though it is on PATH.
    """
    path = shutil.which(name)
    if path is None:
        raise FileNotFoundError(f"{name} was not found on PATH")
    return path


def stale(outputs, inputs):
    return (any(not path.is_file() for path in outputs) or
            max(path.stat().st_mtime_ns for path in inputs) >
            min(path.stat().st_mtime_ns for path in outputs))


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def bootshim():
    sources = [ROOT / "BootShim" / name for name in
               ("Entry.S", "BootShim.c", "StoredGzip.c")]
    sources.append(ROOT / "LiuqinPkg/Library/BootContractLib/BootContract.c")
    inputs = [*sources, ROOT / "BootShim/BootShim.ld", ROOT / "BootShim/Layout.h",
              ROOT / "BootShim/StoredGzip.h", ROOT / "LiuqinPkg/Include/BootContract.h",
              Path(__file__)]
    output = BUILD / "BootShim.bin"
    if not stale([output], inputs):
        return
    directory = BUILD / "BootShim"
    directory.mkdir(parents=True, exist_ok=True)
    flags = ["--target=aarch64-none-elf", "-march=armv8-a", "-mstrict-align",
             "-mgeneral-regs-only", "-O2", "-ffreestanding", "-fpie",
             "-fvisibility=hidden", "-fno-builtin", "-fno-stack-protector",
             "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
             "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "LiuqinPkg/Include")]
    objects = []
    for source in sources:
        obj = directory / (source.stem + ".o")
        run("clang", *flags, "-c", source, "-o", obj)
        objects.append(obj)
    elf = directory / "BootShim.elf"
    run(tool("ld.lld"), "-static", "--gc-sections", "-T", ROOT / "BootShim/BootShim.ld",
        *objects, "-o", elf)
    run("llvm-objcopy", "-O", "binary", elf, output)


def main():
    if any(arg in {"clean", "cleanall", "cleanlib", "run"} for arg in sys.argv[1:]):
        return
    (BUILD / "Include").mkdir(parents=True, exist_ok=True)
    acpi_sources = [ROOT / "tools/acpi.py", *sorted((ROOT / "LiuqinPkg/Acpi").glob("*"))]
    acpi_outputs = [BUILD / "Include/AcpiTables.generated.h", BUILD / "Acpi/dsdt.aml",
                    BUILD / "Acpi/ssdt-io.aml"]
    if stale(acpi_outputs, acpi_sources):
        run(sys.executable, ROOT / "tools/acpi.py")
    payload = ROOT / "LiuqinPkg/Library/KernelErrataPatcherLib"
    if stale([BUILD / "Include/KernelPatchPayload.generated.h"],
             [ROOT / "tools/kernel_payload.py", payload / "KernelPatchPayload.c",
              payload / "KernelPatchPayload.S"]):
        run(sys.executable, ROOT / "tools/kernel_payload.py")
    bootshim()


if __name__ == "__main__":
    main()
