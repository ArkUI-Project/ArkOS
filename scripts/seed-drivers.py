#!/usr/bin/env python3
"""Seed an installed ArkOS data disk with .arco driver records.

The kernel's blob store is a two-bank COW index at volume LBA 8192 (kernel/blob.c)
and driver bytes live in the arena above it. This tool reproduces that layout on
a disk image that ArkOS itself has already formatted, so a guest booted from it
verifies, maps and runs a real .arco module through the normal boot path
(kernel/module.c module_boot_load) instead of a synthesised in-memory image.

The manifest is assembled here independently of the C struct: if scripts/arco.py
and kernel/module.c ever disagree about the ARCO1 or ARKD1 layouts, the guest
refuses the module and the test fails.

    scripts/seed-drivers.py --disk build/test-install-bios/target.img \\
        --arco build/demo.arco
"""
import argparse
import hashlib
import struct
import zlib
from pathlib import Path

INDEX_LBA = 8192
BANK_STRIDE = 8
ARENA_LBA = 8208
RECORD_BYTES = 96
ROOT_BYTES = 4096
MANIFEST_MAGIC = b"ARKD1\0\0\0"
MANIFEST_HEADER_BYTES = 24
# ManifestEntry in kernel/module.c: name[32], sha256[32], bytes u64,
# version u32, api u32, reserved u32 -> 84 bytes padded to 88.
ENTRY_BYTES = 88
DRIVER_API = 1
KERNEL_UID = 1000


def find_arkfs_offset(disk):
    """Same discovery the kernel does: a raw ARKFS1 disk, or an MBR partition
    whose first sector is ARKFS1."""
    with disk.open("rb") as handle:
        boot = handle.read(512)
        if boot[:6] == b"ARKFS1":
            return 0
        if boot[510:512] != b"\x55\xaa":
            raise SystemExit("seed-drivers: disk has no MBR and no ArkFS superblock")
        for index in range(4):
            entry = boot[446 + index * 16:446 + (index + 1) * 16]
            if entry[4] != 0xDA:
                continue
            start, count = struct.unpack_from("<II", entry, 8)
            if start < 2048 or count < 16384:
                continue
            handle.seek(start * 512)
            if handle.read(6) == b"ARKFS1":
                return start


def manifest_bytes(name, image):
    """Assemble the ARKD1 manifest exactly as module_store() would."""
    version = image["version"]
    entry = bytearray(ENTRY_BYTES)
    entry[0:len(name)] = name.encode()
    entry[32:64] = image["sha256"]
    struct.pack_into("<QIII", entry, 64, image["bytes"], version, DRIVER_API, 0)
    root = bytearray(MANIFEST_HEADER_BYTES + ENTRY_BYTES)
    root[0:8] = MANIFEST_MAGIC
    struct.pack_into("<II", root, 8, 1, 0)  # count, crc32 placeholder
    struct.pack_into("<II", root, 16, 0, 0)
    root[MANIFEST_HEADER_BYTES:] = entry
    struct.pack_into("<I", root, 12, zlib.crc32(bytes(root)) & 0xFFFFFFFF)
    return bytes(root)


def bank_root(records):
    root = bytearray(ROOT_BYTES)
    root[0:8] = b"ARKBLOB1"
    struct.pack_into("<Q", root, 8, 1)  # generation: newer than bank 0
    for index, (uid, start, length, checksum, name) in enumerate(records):
        assert index < 16, "bank 0 holds at most 16 records"
        slot = 32 + index * RECORD_BYTES
        struct.pack_into("<IIII", root, slot, uid, start, length, checksum)
        root[slot + 24:slot + 24 + len(name)] = name.encode()
    struct.pack_into("<I", root, ROOT_BYTES - 4,
                     zlib.crc32(bytes(root[:ROOT_BYTES - 4])) & 0xFFFFFFFF)
    return bytes(root)


def write_blob(disk, volume_offset, start, payload):
    disk.seek((volume_offset + start) * 512)
    disk.write(payload)
    padding = (-len(payload)) % 512
    if padding:
        disk.write(bytes(padding))


def seed(disk_path, arco_path, name=None, tamper=False):
    published = Path(arco_path).read_bytes()
    if published[:8] != b"ARCO1\0\0\0":
        raise SystemExit("seed-drivers: not an ARCO1 image")
    header_name = published[8:40].split(b"\0")[0].decode()
    api, version = struct.unpack_from("<II", published, 40)
    if api != DRIVER_API:
        raise SystemExit(f"seed-drivers: driver api {api} is not {DRIVER_API}")
    driver = name or header_name

    # The manifest always describes the published original.
    image = {"sha256": hashlib.sha256(published).digest(), "bytes": len(published),
             "version": version}
    arco = bytearray(published)
    if tamper:
        # Model an attacker who can rewrite the volume: the stored bytes change
        # and the blob store stays internally consistent, so the only thing
        # standing between them and ring 0 is the manifest's whole-file hash.
        at = 300
        arco[at] ^= 0x40
        print(f"tampering: flipped image byte {at} "
              f"({published[at]:#04x} -> {arco[at]:#04x})")

    manifest = manifest_bytes(driver, image)

    disk = Path(disk_path)
    offset = find_arkfs_offset(disk)
    with disk.open("r+b") as handle:
        disk = handle
        disk.seek(offset * 512)
        size_sectors = struct.unpack_from("<I", disk.read(512), 16)[0]
        if size_sectors < ARENA_LBA + 128:
            raise SystemExit("seed-drivers: ArkFS volume is too small for the blob store")

        # Park the payloads above the arena start with room for the allocator.
        demo_lba = ARENA_LBA + 64
        manifest_lba = demo_lba + (len(arco) + 511) // 512 + 8
        write_blob(disk, offset, demo_lba, bytes(arco))
        write_blob(disk, offset, manifest_lba, manifest)

        records = [
            (KERNEL_UID, manifest_lba, len(manifest),
             zlib.crc32(manifest) & 0xFFFFFFFF, "@drv.manifest"),
            (KERNEL_UID, demo_lba, len(arco),
             zlib.crc32(arco) & 0xFFFFFFFF, "@drv." + driver),
        ]
        # Bank 1 is the newer generation, so the kernel adopts it as active.
        root = bank_root(records)
        disk.seek((offset + INDEX_LBA + BANK_STRIDE) * 512)
        disk.write(root)
        disk.write(bytes(BANK_STRIDE * 512 - ROOT_BYTES))
        # Retire bank 0 so it can never win the generation comparison.
        disk.seek((offset + INDEX_LBA) * 512)
        disk.write(bytes(BANK_STRIDE * 512))
    print(f"seeded {driver} v{version >> 16}.{(version >> 8) & 255}.{version & 255} "
          f"({len(arco)} bytes) into {disk_path} at ArkFS+{demo_lba} "
          f"(volume offset {offset}, {size_sectors} sectors)")
    print(f"manifest sha256 {hashlib.sha256(arco).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", required=True, help="installed ArkOS data disk image")
    parser.add_argument("--arco", required=True, help=".arco image to publish")
    parser.add_argument("--name", help="override the driver's internal name")
    parser.add_argument("--tamper-image", action="store_true",
                        help="flip one payload byte and keep the blob store's own "
                             "checksum consistent, so only the manifest can catch it")
    args = parser.parse_args()
    seed(args.disk, args.arco, args.name, args.tamper_image)


if __name__ == "__main__":
    main()