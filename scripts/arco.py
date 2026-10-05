#!/usr/bin/env python3
"""Build a .arco loadable kernel driver image.

    scripts/arco.py <source.c>... -o out.arco [--name NAME] [--version V]

A .arco file is a 256-byte ARCO1 header (include/ark_driver.h) followed by one
contiguous position-independent image linked at vaddr 0 by scripts/arco.ld.
The loader maps [0, data_off) read+execute and [data_off, image_end+bss)
read+write+NX, so the image carries no relocations and no writable text: every
reference must already be RIP-relative.

Relocations are not patched at load time, so any dynamic relocation is a hard
error here rather than a kernel surprise. The script verifies the linked image
has no relocations, that the exported entry exists, that the RW boundary is
4 KiB aligned, and that no R section shares a page with a W section before it
emits the header with the image CRC-32 and SHA-256.

The header SHA-256 covers payload integrity, not publisher identity: the kernel
trust reference is the SHA-256 of this whole file recorded in the protected
manifest blob @drv.manifest.
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINKER = os.path.join(ROOT, "scripts", "arco.ld")
HEADER_BYTES = 256
MAGIC = b"ARCO1\0\0\0"
API_VERSION = 1
MAX_IMAGE = 1024 * 1024
MAX_BSS = 256 * 1024

# Freestanding PIC kernel code: no libc, no stack protector, no PLT, general
# registers only (the kernel entry path never touches SSE state), small model.
CFLAGS = [
    "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pie",
    "-fno-asynchronous-unwind-tables", "-fno-plt", "-fPIC", "-fvisibility=hidden",
    "-m64", "-mno-red-zone", "-mgeneral-regs-only", "-mcmodel=small",
]
LDFLAGS = ["-nostdlib", "-static", "--no-dynamic-linker", "-z", "noexecstack",
           "-T", LINKER, "--fatal-warnings"]


def readelf(path, *args):
    """Run readelf with a fixed locale so output parsing never depends on the
    host's language settings."""
    env = dict(os.environ, LC_ALL="C", LANG="C")
    return subprocess.run(["readelf", *args, path], capture_output=True,
                          text=True, check=True, env=env).stdout


def read_symbols(path, names):
    out = readelf(path, "-sW")
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[7] in names:
            found[parts[7]] = int(parts[1], 16)
    return found


def section_info(path):
    out = readelf(path, "-SW")
    sections = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 7 or not parts[0].startswith("["):
            continue
        try:
            idx = int(parts[0].strip("[]"))
        except ValueError:
            continue
        name = parts[1]
        if idx == 0 or name in ("", "NULL"):
            continue
        sections.append((name, parts[2], int(parts[3], 16), int(parts[4], 16)))
    return sections


RELOC_PREFIXES = (".rela", ".rel", ".dynsym", ".dynstr", ".dynamic", ".plt",
                  ".got.plt", ".interp", ".gnu.hash")


def check_no_relocations(path):
    """A .arco image is loaded verbatim, so anything the linker left to be
    relocated or resolved at runtime would have to be patched into kernel text
    at load time. Reject it here, where the fix is obvious, instead of letting
    the loader meet it."""
    offending = [name for name, _, _, _ in section_info(path)
                 if name.startswith(RELOC_PREFIXES)]
    # Data rows of `readelf -r` are "<offset> <info> <type> <symbol+addend>".
    rows = [line for line in readelf(path, "-rW").splitlines()
            if re.match(r"^\s+[0-9a-f]{6,}\s+[0-9a-f]+\s+\S", line)]
    if offending or rows:
        raise SystemExit(
            "arco: the image is not fully linked; a .arco module is loaded "
            "verbatim with no relocation step.\n  offending sections: "
            f"{offending or 'none'}\n  relocation records: {len(rows)}")


def check_write_boundaries(path, data_off):
    """The loader maps whole pages with one permission pair, so no writable
    section may start below data_off and no read-only section may reach it."""
    bad = []
    for name, kind, addr, size in section_info(path):
        if kind == "NOBITS" or size == 0:
            continue
        writable = name.startswith((".data", ".got", ".sdata", ".sbss"))
        if writable and addr < data_off:
            bad.append(f"writable {name} at {addr:#x} sits below data_off {data_off:#x}")
        if not writable and addr + size > data_off:
            bad.append(f"read-only {name} at {addr:#x}+{size:#x} crosses "
                       f"data_off {data_off:#x}")
    if bad:
        raise SystemExit("arco: section layout violates the RX/RW split:\n  " +
                         "\n  ".join(bad))


def build(sources, name, version, output, cc, ld, keep):
    if not name or not all(c.islower() or c.isdigit() or c in "_-." for c in name):
        raise SystemExit("arco: --name must be [a-z0-9_.-], 1..31 chars")
    with tempfile.TemporaryDirectory(prefix="arco-") as tmp:
        elf = os.path.join(tmp, "driver.elf")
        raw = os.path.join(tmp, "driver.bin")
        objects = []
        for index, source in enumerate(sources):
            target = os.path.join(tmp, "driver.o" if index == 0 else f"extra{index}.o")
            subprocess.run([cc] + CFLAGS + [f"-I{os.path.join(ROOT, 'include')}",
                                            "-c", source, "-o", target], check=True)
            objects.append(target)
        subprocess.run([ld] + LDFLAGS + ["-o", elf] + objects, check=True)

        check_no_relocations(elf)
        symbols = read_symbols(elf, {"arco_entry", "__arco_data_off",
                                     "__arco_image_end", "__arco_bss",
                                     "__arco_end", "__arco_start"})
        for required in ("arco_entry", "__arco_data_off", "__arco_image_end", "__arco_bss"):
            if required not in symbols:
                raise SystemExit(f"arco: linked image is missing {required}; "
                                 "every driver must define arco_entry")

        data_off = symbols["__arco_data_off"]
        image_end = symbols["__arco_image_end"]
        bss_start = symbols["__arco_bss"]
        end = symbols["__arco_end"]
        entry_off = symbols["arco_entry"]
        if data_off % 4096 or data_off == 0:
            raise SystemExit(f"arco: data boundary {data_off:#x} is not 4 KiB aligned")
        if not 0 < entry_off < data_off:
            raise SystemExit("arco: entry point must live in the read+execute region")
        if image_end < data_off:
            raise SystemExit("arco: image ends before its data boundary")
        # The loader maps the run page by page, so .bss is rounded up to a
        # whole page; the padding is part of the same zero-filled extent.
        bss_size = (max(0, end - bss_start) + 4095) // 4096 * 4096
        if bss_size > MAX_BSS:
            raise SystemExit(f"arco: bss {bss_size} exceeds the {MAX_BSS} byte budget")
        check_write_boundaries(elf, data_off)

        subprocess.run(["objcopy", "-O", "binary", "--gap-fill", "0", elf, raw],
                       check=True)
        with open(raw, "rb") as handle:
            image = handle.read()
        # The kernel maps the run page by page; the header's image_size covers
        # the file bytes only and .bss travels as a separate zero-fill length.
        image = image[:image_end]
        if not image or len(image) > MAX_IMAGE:
            raise SystemExit(f"arco: image size {len(image)} out of range")
        version_major, version_minor, version_patch = (version + ".0.0").split(".")[:3]
        packed_version = (int(version_major) << 16) | (int(version_minor) << 8) | \
            int(version_patch)

        header = bytearray(HEADER_BYTES)
        header[0:8] = MAGIC
        header[8:8 + len(name)] = name.encode("ascii")
        # Offsets are the packed ArcoHeader layout in include/ark_driver.h:
        # magic@0 name@8 api@40 version@44 image_size@48 entry_off@56
        # data_off@64 bss_size@72 image_crc32@80 header_crc32@84
        # image_sha256@88 reserved@120.
        struct.pack_into("<I", header, 40, API_VERSION)
        struct.pack_into("<I", header, 44, packed_version)
        struct.pack_into("<Q", header, 48, len(image))
        struct.pack_into("<Q", header, 56, entry_off)
        struct.pack_into("<Q", header, 64, data_off)
        struct.pack_into("<Q", header, 72, bss_size)
        struct.pack_into("<I", header, 80, zlib.crc32(image) & 0xFFFFFFFF)
        struct.pack_into("<I", header, 84, 0)  # header_crc32 placeholder
        header[88:120] = hashlib.sha256(image).digest()
        struct.pack_into("<I", header, 84, zlib.crc32(bytes(header)) & 0xFFFFFFFF)

        with open(output, "wb") as handle:
            handle.write(bytes(header))
            handle.write(image)
        if keep:
            subprocess.run(["objcopy", "-O", "binary", elf, keep], check=True)
        print(f"{output}: {len(header) + len(image)} bytes "
              f"(image {len(image)}, data @{data_off:#x}, bss {bss_size}, "
              f"entry {entry_off:#x}, api {API_VERSION})")
        print(f"sha256 {hashlib.sha256(bytes(header) + image).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description="pack an ArkOS .arco kernel driver")
    parser.add_argument("sources", nargs="+")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--ld", default=os.environ.get("LD", "ld"))
    parser.add_argument("--keep-bin", help="also write the raw linked image here")
    args = parser.parse_args()
    build(args.sources, args.name, args.version, args.output,
          args.cc, args.ld, args.keep_bin)


if __name__ == "__main__":
    main()