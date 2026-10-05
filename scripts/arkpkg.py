#!/usr/bin/env python3
"""Build or inspect ArkOS's native .arkpkg v1 packages (standard library only)."""
import argparse
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path

HEADER = 256
MAX_BYTES = 8*1024*1024-32
CAPS = {"ui": 4, "files": 2, "network": 8, "activity": 32, "device": 64}
EH = struct.Struct("<16sHHIQQQIHHHHHH")
PH = struct.Struct("<IIQQQQQQ")


def compact_elf(raw, memory_limit=64*1024*1024):
    h = list(EH.unpack_from(raw))
    if h[0][:8] != b"\x7fELF\x02\x01\x01\0" or h[1:4] != [2, 62, 1]:
        raise ValueError("expected an ArkOS static x86_64 ELF")
    if h[8:10] != [EH.size, PH.size] or not 0 < h[10] <= 32 or h[5]+PH.size*h[10] > len(raw):
        raise ValueError("invalid ELF program headers")
    h[6] = 0; h[11] = h[12] = h[13] = 0
    result = bytearray(h[5]+PH.size*h[10]); result[:EH.size] = EH.pack(*h)
    loads = []; entry = False; mapped = 128*1024
    for i in range(h[10]):
        p = list(PH.unpack_from(raw, h[5]+PH.size*i))
        kind, flags, offset, address, _, size, memory, align = p
        if kind in (2, 3, 7) or kind == 0x6474e551 and flags & 1:
            raise ValueError("dynamic linkage, TLS and executable stacks are unsupported")
        if kind == 1 and memory:
            if size > memory or offset+size > len(raw) or offset % 4096 or address % 4096 or align != 4096:
                raise ValueError("invalid ELF load layout")
            if flags & ~7 or not flags & 4 or flags & 3 == 3 or address < 0x40000000 or address+memory > 0x60000000:
                raise ValueError("ELF violates ArkOS address or W^X policy")
            end = (address+memory+4095) & ~4095
            if any(address < b and a < end for a, b in loads):
                raise ValueError("overlapping ELF segments")
            loads.append((address, end)); mapped += end-address
            entry |= bool(flags & 1 and address <= h[4] < address+size)
            result.extend(bytes((-len(result)) % 4096))
            p[2] = len(result); result.extend(raw[offset:offset+size])
        elif kind == 1 and size:
            raise ValueError("nonempty load segment without memory")
        PH.pack_into(result, h[5]+i*PH.size, *p)
    if not loads or not entry or mapped > memory_limit:
        raise ValueError("ELF entry or process memory budget failed")
    return bytes(result)


def text(value, capacity):
    data = value.encode("utf-8")
    if len(data) >= capacity or any(c < 32 or c == 127 for c in data):
        raise ValueError(f"text must fit {capacity-1} UTF-8 bytes and contain no controls")
    return data.ljust(capacity, b"\0")


def validate_id(value):
    if not re.fullmatch(r"[a-z0-9][a-z0-9.-]{0,26}", value) or value.endswith("."):
        raise ValueError("ID must use 1–27 lowercase ASCII letters, digits, dots or hyphens")


def build(args):
    validate_id(args.id)
    if not args.title:
        raise ValueError("package title must not be empty")
    version = [int(v) for v in args.version.split(".")]
    if len(version) != 3 or any(not 0 <= v < 2**32 for v in version):
        raise ValueError("version must be three uint32 values: major.minor.patch")
    capabilities = 4
    for name in args.permissions.split(","):
        capabilities |= CAPS[name]
    payload = compact_elf(args.elf.read_bytes())
    if len(payload)+HEADER > MAX_BYTES:
        raise ValueError("package exceeds 8 MiB minus the installation envelope")
    header = bytearray(HEADER); header[:8] = b"ARKPKG1\0"
    struct.pack_into("<HH9I", header, 8, 1, 62, HEADER, HEADER+len(payload), 1,
                     capabilities, *version, len(payload), 0)
    header[48:80] = text(args.id, 32); header[80:144] = text(args.title, 64)
    header[144:208] = text(args.summary, 64); header[208:240] = hashlib.sha256(payload).digest()
    struct.pack_into("<I", header, 252, zlib.crc32(header[:252]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header+payload)
    print(json.dumps(inspect(args.output), ensure_ascii=False, indent=2))


def inspect(path):
    data = path.read_bytes()
    if not HEADER <= len(data) <= 32*1024*1024 or data[:8] != b"ARKPKG1\0":
        raise ValueError("invalid ArkPkg magic or size")
    values = struct.unpack_from("<HH9I", data, 8)
    fmt, arch, header, total, abi, caps, major, minor, patch, payload, flags = values
    if (fmt, arch, header, total, abi, payload) != (1, 62, HEADER, len(data), 1, len(data)-HEADER):
        raise ValueError("invalid ArkPkg layout")
    system_engine = flags in [2 | (i << 8) for i in (0,1,3,4,10,11,12,16)]
    if not system_engine and len(data)>MAX_BYTES:raise ValueError("package exceeds installation size limit")
    if flags not in (0,1) and not system_engine:
        raise ValueError("invalid system package entry")
    if (caps != 31 if system_engine else caps & ~46 or not caps & 4) or any(data[240:252]):
        raise ValueError("invalid permissions or reserved fields")
    if zlib.crc32(data[:252]) != struct.unpack_from("<I", data, 252)[0] or hashlib.sha256(data[HEADER:]).digest() != data[208:240]:
        raise ValueError("ArkPkg CRC/SHA-256 mismatch")
    compact_elf(data[HEADER:],512*1024*1024 if system_engine else 64*1024*1024)
    fields = []
    for first, last in ((48, 80), (80, 144), (144, 208)):
        raw = data[first:last]
        if b"\0" not in raw:
            raise ValueError("manifest strings must be NUL terminated")
        value = raw.split(b"\0", 1)[0].decode("utf-8")
        text(value, last-first)
        fields.append(value)
    validate_id(fields[0])
    if not fields[1]:
        raise ValueError("package title must not be empty")
    return {"format": "ArkPkg v1", "system": bool(flags), "id": fields[0],
            "title": fields[1], "version": f"{major}.{minor}.{patch}",
            "summary": fields[2], "bytes": total,
            "permissions": [name for name, bit in CAPS.items() if caps & bit],
            "sha256": hashlib.sha256(data).hexdigest(), "publisher_signature": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("build"); create.add_argument("elf", type=Path)
    create.add_argument("output", type=Path); create.add_argument("--id", required=True)
    create.add_argument("--title", required=True); create.add_argument("--version", default="1.0.0")
    create.add_argument("--summary", default=""); create.add_argument("--permissions", default="ui")
    show = commands.add_parser("inspect"); show.add_argument("file", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "build": build(args)
        else: print(json.dumps(inspect(args.file), ensure_ascii=False, indent=2))
    except (ValueError, KeyError, struct.error, OSError) as exc:
        parser.exit(2, f"arkpkg: {exc}\n")


if __name__ == "__main__": main()
