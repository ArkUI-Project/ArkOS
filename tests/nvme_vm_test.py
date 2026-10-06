#!/usr/bin/env python3
"""Guest smoke for the NVMe .arco driver and MSI-X delivery (Tester 13700H /
QEMU q35 gates).

  1. Boot log shows the inbox nvme driver loading, finding the PCIe controller
     by class 01/08, and binding an MSI-X vector through host->msi_attach.
  2. `dev drivers` reports a bound msix vector (not "irq:none"); a real sector
     read and write raise the kernel's ISR counter n=, so the driver is
     interrupt-driven rather than poll-only.
  3. Unload then reinstall: the kernel logs the vector release and the driver
     re-binds the same (recycled) vector, with no leftover or duplicate.

    ARKOS_ISO=... python3 tests/nvme_vm_test.py
"""
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vm import VM, ROOT  # noqa: E402

os.environ["ARKOS_MACHINE"] = "q35"
os.environ["ARKOS_SMP"] = "4"
ADMIN = "NvmeGate42!"
NVME_BYTES = 16 * 1024 * 1024
out = ROOT / "build" / "test-nvme-vm"
out.mkdir(parents=True, exist_ok=True)


def seed_source_blob(disk, blob_path, name):
    """Park an .arco image as an ordinary admin-owned blob so the guest can
    reinstall it with `dev install blob:NAME` after removal. Mirrors
    tests/ioapic_vm_test.py; no manifest is written, so boot still loads the
    inbox copy."""
    import importlib.util
    import zlib
    spec = importlib.util.spec_from_file_location("seed", ROOT / "scripts" / "seed-drivers.py")
    seed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(seed)
    arco = blob_path.read_bytes()
    assert arco[:8] == b"ARCO1\0\0\0"
    offset = seed.find_arkfs_offset(disk)
    with disk.open("r+b") as handle:
        lba = seed.ARENA_LBA + 64
        seed.write_blob(handle, offset, lba, arco)
        root = seed.bank_root([(seed.KERNEL_UID, lba, len(arco),
                                zlib.crc32(arco) & 0xFFFFFFFF, name)])
        handle.seek((offset + seed.INDEX_LBA + seed.BANK_STRIDE) * 512)
        handle.write(root)
        handle.write(bytes(seed.BANK_STRIDE * 512 - seed.ROOT_BYTES))
        handle.seek((offset + seed.INDEX_LBA) * 512)
        handle.write(bytes(seed.BANK_STRIDE * 512))


def fresh_disk():
    disk = out / "data.img"
    disk.unlink(missing_ok=True)
    subprocess.run([sys.executable, "scripts/create-disk.py", str(disk)],
                   check=True, cwd=ROOT)
    return disk


def fresh_nvme():
    disk = out / "nvme.img"
    with disk.open("wb") as handle:
        handle.truncate(NVME_BYTES)
    return disk


def sign_in(vm):
    vm.wait("[session] setup ready")
    vm.type(ADMIN)
    vm.key("tab")
    vm.type(ADMIN)
    vm.key("ret")
    vm.wait("[session] desktop unlocked", 60)


def command(vm, text, expect=None, timeout=30):
    start = len(vm.log.read_text())
    vm.command(text)
    if expect:
        vm.wait(expect, timeout, after=start)


def driver_n(vm):
    """Run `dev drivers` and return (vector, n) for the nvme line."""
    before = len(vm.log.read_text())
    vm.command("dev drivers")
    vm.wait("nvme", 30, after=before)
    time.sleep(1.0)
    tail = vm.log.read_text()[before:]
    assert "irq:none" not in tail, "nvme reported poll-only: " + tail
    m = re.search(r"msix(\d+) n=(\d+)", tail)
    assert m, "missing msix vector counter in drivers detail: " + tail
    return int(m.group(1)), int(m.group(2))


disk = fresh_disk()
nvme_disk = fresh_nvme()
seed_source_blob(disk, ROOT / "build" / "nvme.arco", "nvmesrc")
vm = VM("nvme-smoke", disk=str(disk), firmware="bios", device=None,
        extra_args=["-drive", f"file={nvme_disk},format=raw,if=none,id=nvmedrive",
                    "-device", "nvme,serial=arkosnvme,drive=nvmedrive"])
try:
    # ---- gate 1: driver loads and binds an MSI-X vector --------------------
    log = vm.log.read_text()
    assert "[module] loaded nvme" in log, log[-3000:]
    m = re.search(r"\[irq\] nvme msi_attach\(bdf=(\d+)\) vector=(\d+)", log)
    assert m, "nvme did not bind an MSI-X vector: " + log[-3000:]
    vector0 = int(m.group(2))
    assert "[exception]" not in log and "[panic]" not in log, log[-2000:]
    print(f"PASS gate1: nvme loaded, MSI-X vector={vector0} bound", flush=True)

    # ---- gate 2: real read + write raise the ISR count ---------------------
    sign_in(vm)
    vm.terminal()
    vec, n0 = driver_n(vm)
    assert vec == vector0, f"vector changed between bind and report: {vec} vs {vector0}"

    command(vm, "dev blk 1 read 0 1", "Read 1 sectors.")
    time.sleep(1.0)
    _, n_read = driver_n(vm)
    assert n_read > n0, f"read did not raise the ISR count: {n0} -> {n_read}"

    command(vm, "dev blk 1 write 0 1", "Wrote 1 sectors.")
    time.sleep(1.0)
    _, n_write = driver_n(vm)
    assert n_write > n_read, f"write did not raise the ISR count: {n_read} -> {n_write}"
    print(f"PASS gate2: MSI-X ISR count n={n0} -> {n_read} -> {n_write} across read+write",
          flush=True)

    # ---- gate 3: unload releases the vector, reinstall recycles it ---------
    before = len(vm.log.read_text())
    command(vm, "dev remove nvme", "Removed nvme")
    log = vm.log.read_text()[before:]
    assert "[irq] msi vectors released n=1" in log, log
    command(vm, "dev install blob:nvmesrc", "Installed nvme", timeout=60)
    log = vm.log.read_text()[before:]
    m2 = re.search(r"\[irq\] nvme msi_attach\(bdf=\d+\) vector=(\d+)", log)
    assert m2, "reinstalled nvme did not re-bind a vector: " + log
    assert int(m2.group(1)) == vector0, \
        f"vector was not recycled after release: {m2.group(1)} vs {vector0}"
    command(vm, "dev blk 1 read 0 1", "Read 1 sectors.")
    time.sleep(1.0)
    vec2, n_again = driver_n(vm)
    assert vec2 == vector0 and n_again > 0, \
        f"reinstalled driver is not interrupt-driven: vec={vec2} n={n_again}"
    print(f"PASS gate3: vector released then recycled (vector={vector0}), I/O live again",
          flush=True)
    print("PASS NVMe + MSI-X smoke gates (q35)", flush=True)
finally:
    vm.close()
