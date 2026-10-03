#!/usr/bin/env python3
"""Real ATA persistence integration test, using independent BIOS/UEFI QEMU runs.

Requires the environment and firmware used by tests/vm.py. A unique data disk
is created under build/test-storage; no existing disk is modified. Diagnostics
and the test image are retained for inspection. Kernel storage code is used
unchanged; no host filesystem is exposed to the guest.
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import shutil
import zlib

import vm as vm_helper
from vm import VM, ROOT

BANKS = (8, 2088)
SECTOR = 512


def snapshot(path):
    """Read-only independent decoder verifies bytes actually in the raw disk."""
    with path.open("rb") as disk:
        superblock = disk.read(SECTOR)
        assert superblock[:8] == b"ARKFS1\0\0"
        assert struct.unpack_from("<I", superblock, 508)[0] == zlib.crc32(superblock[:508])
        versions = []
        for number, lba in enumerate(BANKS):
            disk.seek(lba * SECTOR)
            header = disk.read(SECTOR)
            if header[:8] != b"ARKBANK1":
                continue
            if struct.unpack_from("<I", header, 508)[0] != zlib.crc32(header[:508]):
                continue
            generation, length, crc, count, version, commit = struct.unpack_from("<Q5I", header, 8)
            if version != 1 or commit != 0x41524B31 or not generation or count > 64 or length > 1057216:
                continue
            payload = disk.read(length)
            if zlib.crc32(payload) != crc:
                continue
            offset = 0
            entries = {}
            for _ in range(count):
                kind, size = struct.unpack_from("<II", payload, offset)
                assert kind in (1, 2) and size < 16384 and (kind != 2 or size == 0)
                raw_name = payload[offset + 8:offset + 136]
                assert b"\0" in raw_name
                name = raw_name.split(b"\0", 1)[0].decode("utf-8")
                data = payload[offset + 136:offset + 136 + size]
                assert len(data) == size and name.startswith("/") and name not in entries
                entries[name] = {"kind": kind, "data": data.decode("utf-8"), "size": size}
                offset += 136 + size
            assert offset == length
            versions.append({"bank": number, "generation": generation, "payload_bytes": length,
                             "entries": entries})
    assert versions, "No valid persisted snapshot"
    return max(versions, key=lambda item: item["generation"])


def command(vm, text, expected=None):
    before = len(vm.log.read_text())
    vm.command(text)
    # Every command must be observed in new serial bytes, never an earlier log.
    vm.wait(text + "\n", after=before)
    if expected is not None:
        vm.wait(expected, after=before)
    return before


def poweroff(vm):
    before = len(vm.log.read_text())
    vm.command("shutdown")
    vm.p.wait(timeout=15)
    tail = vm.log.read_text()[before:]
    assert "Powering off..." in tail and "[power] Power off requested" in tail
    assert vm.p.returncode == 0, vm.p.returncode
    vm.close()


def main():
    parent = ROOT / "build" / "test-storage"
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    disk = work / "arkos-data.img"
    subprocess.run(["python3", str(ROOT / "scripts/create-disk.py"), str(disk)], check=True)
    # Freeze the tested artifact even if another agent rebuilds the main ISO.
    # vm.py resolves its disk-image and diagnostics paths from its module ROOT;
    # this process-local override keeps the unmodified helper on our snapshot.
    frozen_build = work / "build"
    frozen_build.mkdir()
    frozen_iso = frozen_build / "arkos-0.4.0.iso"
    shutil.copy2(ROOT / "build/arkos-0.4.0.iso", frozen_iso)
    iso_sha256 = hashlib.sha256(frozen_iso.read_bytes()).hexdigest()
    vm_helper.ROOT = work
    records = []
    first = "ATA-PIO first line\nATA-PIO second line\n"
    final = first + "UEFI can read this\n"
    prefix = "/home/ark/Documents/persist"

    def launch(label, firmware):
        print(f"Starting {label} ({firmware})", flush=True)
        vm = VM(label, disk=disk, firmware=firmware)
        assert "[storage] ArkFS mounted" in vm.log.read_text()
        assert "physical MiB=32" in vm.log.read_text()
        assert "RAM only" not in vm.log.read_text()
        vm.terminal()
        return vm

    vm = launch("01-bios-write", "bios")
    try:
        command(vm, "df", "ArkFS: persistent ATA disk")
        command(vm, "mkdir Documents/persist")
        command(vm, 'echo "ATA-PIO first line" > Documents/persist/data.txt')
        command(vm, 'echo "ATA-PIO second line" >> Documents/persist/data.txt')
        command(vm, "cp notes.txt Documents/persist/chinese.txt")
        command(vm, "cat hello.txt | grep ArkOS | wc -l > Documents/persist/count.txt")
        command(vm, "cp Documents/persist/data.txt Documents/persist/copy.txt")
        command(vm, "mv Documents/persist/copy.txt Documents/persist/renamed.txt")
        command(vm, "cat Documents/persist/data.txt", first)
        command(vm, "cat Documents/persist/chinese.txt", "我的 ArkOS 笔记")
        command(vm, "sync", "Filesystem synchronized to disk.")
        vm.screen("terminal-persisted")
        poweroff(vm)
    finally:
        vm.close()
    saved = snapshot(disk)
    entries = saved["entries"]
    assert entries[prefix + "/data.txt"]["data"] == first
    assert entries[prefix + "/renamed.txt"]["data"] == first
    assert prefix + "/copy.txt" not in entries
    assert entries[prefix + "/count.txt"]["data"] == "2\n"
    assert "我的 ArkOS 笔记" in entries[prefix + "/chinese.txt"]["data"]
    assert "/home/ark/Documents/使用说明.txt" in entries
    assert entries[prefix]["kind"] == 2
    records.append({"session": "BIOS process 1", "generation": saved["generation"],
                    "checks": "ATA write; mkdir; UTF-8 contents and paths; cp/mv; pipes; > and >>; sync; ACPI poweroff"})
    print("PASS: BIOS wrote an independently decoded, CRC-valid persistent snapshot", flush=True)

    vm = launch("02-bios-reopen", "bios")
    try:
        command(vm, "cat Documents/persist/data.txt", first)
        command(vm, "cat Documents/persist/count.txt", "\n2\n")
        command(vm, "cat Documents/persist/chinese.txt", "我的 ArkOS 笔记")
        command(vm, "ls Documents", "使用说明.txt")
        command(vm, "rm Documents/persist/renamed.txt")
        command(vm, 'echo "UEFI can read this" >> Documents/persist/data.txt')
        command(vm, "sync", "Filesystem synchronized to disk.")
        poweroff(vm)
    finally:
        vm.close()
    saved2 = snapshot(disk)
    assert saved2["generation"] > saved["generation"]
    assert saved2["entries"][prefix + "/data.txt"]["data"] == final
    assert prefix + "/renamed.txt" not in saved2["entries"]
    records.append({"session": "BIOS process 2", "generation": saved2["generation"],
                    "checks": "fresh QEMU process read persisted bytes; UTF-8 preserved; deletion and append committed"})
    print("PASS: independent BIOS boot restored data, then persisted deletion and append", flush=True)

    vm = launch("03-uefi-reopen", "uefi")
    try:
        command(vm, "cat Documents/persist/data.txt", final)
        command(vm, "cat Documents/persist/count.txt", "\n2\n")
        command(vm, "cat Documents/persist/chinese.txt", "我的 ArkOS 笔记")
        command(vm, "ls Documents", "使用说明.txt")
        command(vm, "ls Documents/persist", "chinese.txt")
        missing = command(vm, "cat Documents/persist/renamed.txt", "File not found.")
        assert "File not found." in vm.log.read_text()[missing:]
        command(vm, "sync", "Filesystem synchronized to disk.")
        vm.screen("uefi-persistent-data")
        poweroff(vm)
    finally:
        vm.close()
    saved3 = snapshot(disk)
    assert saved3["entries"] == saved2["entries"], "Read-only UEFI session changed data"
    records.append({"session": "UEFI process 3", "generation": saved3["generation"],
                    "checks": "fresh OVMF QEMU process restored BIOS-written files; UTF-8 and deletion preserved; ACPI poweroff"})
    result = {"result": "PASS", "when_utc": datetime.now(timezone.utc).isoformat(),
              "iso_sha256": iso_sha256, "frozen_iso": str(frozen_iso), "disk": str(disk), "sessions": records,
              "logical_capacity_bytes": 64 * 16383, "physical_disk_bytes": disk.stat().st_size,
              "final_entries": {name: {"kind": item["kind"], "size": item["size"]}
                                for name, item in saved3["entries"].items()}}
    (work / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print("PASS: UEFI read all persistent data and observed deletion", flush=True)
    print(f"Evidence: {work / 'result.json'}", flush=True)


if __name__ == "__main__":
    main()
