#!/usr/bin/env python3
"""Create a NEW ArkOS ArkFS data disk. Never reformats or overwrites a path.

The format intentionally matches kernel/storage.c; integers are little endian.
Only generation 1 in bank 0 is initialized, representing an empty namespace.
The first boot creates default directories and documents through the kernel.
"""
import argparse
import os
from pathlib import Path
import struct
import sys
import zlib

SECTOR = 512
BANK_SECTORS = 2080
BANK0 = 8
BANK1 = BANK0 + BANK_SECTORS
MAGIC = b"ARKFS1\x00\x00"
COMMIT_TAG = 0x41524B31


def make_superblock(sectors: int) -> bytes:
    block = bytearray(SECTOR)
    block[:8] = MAGIC
    struct.pack_into("<9I", block, 8, 1, SECTOR, sectors, BANK_SECTORS,
                     BANK0, BANK1, 64, 128, 16384)
    struct.pack_into("<I", block, 508, zlib.crc32(block[:508]))
    return bytes(block)


def make_empty_bank() -> bytes:
    block = bytearray(SECTOR)
    block[:8] = b"ARKBANK1"
    struct.pack_into("<Q5I", block, 8, 1, 0, 0, 0, 1, COMMIT_TAG)
    struct.pack_into("<I", block, 508, zlib.crc32(block[:508]))
    return bytes(block)


def create(path: Path, size_mib: int = 32) -> None:
    if not 4 <= size_mib <= 128 * 1024:
        raise ValueError("size must be 4 through 131072 MiB (LBA28 limit)")
    size = size_mib * 1024 * 1024
    # Exclusive creation also refuses existing symlinks and device paths.
    # Keep a partially created file on I/O failure for inspection, never unlink
    # an arbitrary path that may have changed after opening it.
    with path.open("xb") as disk:
        disk.truncate(size)
        disk.seek(0)
        disk.write(make_superblock(size // SECTOR))
        disk.seek(BANK0 * SECTOR)
        disk.write(make_empty_bank())
        disk.flush()
        os.fsync(disk.fileno())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path, default=Path("arkos-data.img"))
    parser.add_argument("--size-mib", type=int, default=32)
    args = parser.parse_args()
    try:
        create(args.path, args.size_mib)
    except FileExistsError:
        print(f"Refusing to overwrite existing path: {args.path}", file=sys.stderr)
        return 1
    except (OSError, ValueError) as error:
        print(f"Cannot create ArkFS image: {error}", file=sys.stderr)
        return 1
    print(f"Created {args.path}: {args.size_mib} MiB ArkFS data disk")
    print("Logical capacity: 64 file/directory entries; 16383 text bytes per file.")
    print("Attach as primary IDE master; keep this image between boots.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
