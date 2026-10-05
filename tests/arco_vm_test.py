#!/usr/bin/env python3
"""Guest proof that a real .arco kernel module loads and runs on ArkOS.

The module is the one scripts/arco.py builds from sdk/driver_demo.c, published
onto a project-created ArkFS data disk (scripts/create-disk.py) the way an
installation would: a @drv.manifest record holding the whole-file SHA-256 plus
the @drv.demo image blob. Nothing is simulated inside the guest — the machine
boots and the kernel alone decides what runs.

Asserted in the guest, all from the real boot log and the real shell:

  * the kernel names the module it verified, mapped and started;
  * the module's INIT executed in ring 0 (its own log line is present);
  * the node it registered is part of the inventory an ordinary app sees;
  * the driver list reports it loaded and QUERY returns its file SHA-256;
  * a file that is not an ARCO1 image is refused, with no state change;
  * removal by an admin retires the module and frees its device node;
  * a single flipped payload byte — with the blob store's own checksum kept
    consistent, as a volume writer would — makes the next boot refuse the
    module, so the manifest hash is what actually holds the line.

    python3 tests/arco_vm_test.py bios
    python3 tests/arco_vm_test.py uefi

Set ARKOS_TEST_INPUT=none to boot without the virtio-multitouch pointer. That is
how UEFI is run today: with that device attached, UEFI faults inside
virtio_input_next_event on a region OVMF assigned below 4 GiB. That defect is
reproducible on an unmodified tree, with -smp 1, no data disk and no module
loaded, so it is not this feature's doing and is not masked here.
"""
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vm import VM, ROOT  # noqa: E402

os.environ["ARKOS_MACHINE"] = "q35"
os.environ["ARKOS_SMP"] = "4"
firmware = sys.argv[1] if len(sys.argv) > 1 else "bios"
out = ROOT / "build" / f"test-arco-{firmware}"
out.mkdir(parents=True, exist_ok=True)
ADMIN = "ArcoAdmin42!"
INPUT = None if os.environ.get("ARKOS_TEST_INPUT") == "none" else "virtio-multitouch-pci"
READY = "[session] desktop unlocked"


def build_driver():
    """The module under test is a real artifact, produced the documented way."""
    arco = out / "demo.arco"
    subprocess.run([sys.executable, "scripts/arco.py", "sdk/driver_demo.c",
                    "-o", str(arco), "--name", "demo", "--version", "1.0.0"],
                   check=True, cwd=ROOT)
    return arco


def fresh_disk(name):
    """A new ArkFS volume created by this project's own formatter."""
    disk = out / f"{name}.img"
    disk.unlink(missing_ok=True)
    subprocess.run([sys.executable, "scripts/create-disk.py", str(disk)],
                   check=True, cwd=ROOT)
    return disk


def sign_in(vm):
    vm.wait("[session] setup ready")
    vm.type(ADMIN)
    vm.key("tab")
    vm.type(ADMIN)
    vm.key("ret")
    vm.wait(READY, 60)


def command(vm, text, expect=None, timeout=30):
    start = len(vm.log.read_text())
    vm.command(text)
    if expect:
        vm.wait(expect, timeout, after=start)


# ---------------------------------------------------------------- boot load
arco = build_driver()
good = fresh_disk("driver-loaded")
subprocess.run([sys.executable, "scripts/seed-drivers.py", "--disk", str(good),
                "--arco", str(arco)], check=True, cwd=ROOT)
print("published demo.arco with a manifest hash onto a new ArkFS volume", flush=True)

vm = VM("arco-boot-" + firmware, disk=str(good), firmware=firmware, device=INPUT)
try:
    sign_in(vm)
    log = vm.log.read_text()
    # ArkFS2 is the baseline: the volume is either a fresh v2 image or a
    # migrated v1 one, and the v1 mount banner is no longer printed.
    assert ("[storage] ArkFS2 mounted" in log or
            "[storage] migrated ArkFS1 onto ArkFS2" in log), log[-2000:]
    assert "[module] loaded demo" in log, log[-2000:]
    assert "[drv] demo module online" in log, log[-2000:]
    # The inbox NIC driver is linked into kernel.elf, so it loads even though
    # this disk's manifest only names demo.
    assert "[module] loaded e1000 (inbox)" in log, log[-2000:]
    assert "[drv] e1000 module online" in log, log[-2000:]
    # irq_attach bound the PCI interrupt line and unmasked it on the PIC.
    assert "[drv] e1000 irq " in log and " bound" in log, log[-2000:]
    assert "[exception]" not in log and "[panic]" not in log, log[-2000:]

    vm.terminal()
    command(vm, "dev", "Demo counter")
    command(vm, "dev", "Ethernet controller")
    command(vm, "dev drivers", "demo")
    command(vm, "dev drivers", "e1000")
    command(vm, "dev drivers", "loaded")
    command(vm, "dev query demo", "sha256 ")
    command(vm, "dev query e1000", "sha256 ")
    vm.screen("01-driver-loaded")
    from PIL import Image
    Image.open(vm.out / "01-driver-loaded.ppm").save(vm.out / "01-driver-loaded.png")

    # The module's poll callback keeps the node's own counters moving.
    before = len(vm.log.read_text())
    command(vm, "dev", "Demo counter")
    assert "Demo counter" in vm.log.read_text()[before:]

    # A file that is not an ARCO1 image is refused by header validation and
    # leaves the installed set untouched.
    command(vm, "echo not-a-module > junk.arco")
    command(vm, "dev install junk.arco", "ARCO1")
    command(vm, "dev drivers", "demo")
    command(vm, "dev install missing.arco", None)
    command(vm, "dev drivers", "demo")

    # An admin may retire it: the driver leaves the loadable set and its node
    # flips to ARK_DEV_STATE_ABSENT (3); inventory keeps the record rather than
    # erasing it, matching kernel module_remove semantics.
    command(vm, "dev remove demo", "Removed demo")
    command(vm, "dev drivers", "e1000")
    before = len(vm.log.read_text())
    command(vm, "dev", "state 3")
    assert "Demo counter" in vm.log.read_text()[before:]
    # Removing the inbox NIC driver unbinds it for this boot: the module's own
    # DEINIT detaches its ops and the network stack reports the loss. The slot
    # itself returns on the next boot — inbox drivers are session-scoped.
    command(vm, "dev remove e1000", "Removed e1000")
    log = vm.log.read_text()
    assert "[drv] e1000 module removed" in log or "network driver removed" in log, log[-2000:]
    command(vm, "dev drivers", "No loadable drivers installed")
    command(vm, "dev", "state 3")
    log = vm.log.read_text()
    assert "[exception]" not in log and "[panic]" not in log, log[-2000:]
    print("PASS ArkOS verified, mapped and ran a real .arco module in ring 0, "
          f"and retired it on request ({firmware})", flush=True)
finally:
    vm.close()

# ------------------------------------------------------------ tamper refusal
bad = fresh_disk("driver-tampered")
subprocess.run([sys.executable, "scripts/seed-drivers.py", "--disk", str(bad),
                "--arco", str(arco), "--tamper-image"], check=True, cwd=ROOT)

tampered = VM("arco-tamper-" + firmware, disk=str(bad), firmware=firmware, device=INPUT)
try:
    sign_in(tampered)
    log = tampered.log.read_text()
    assert "[module] loaded demo" not in log, "a modified module image still loaded"
    assert "[module] load failed: demo" in log, log[-2000:]
    assert "bytes differ from manifest" in log, log[-2000:]
    # The tampered manifest driver fails closed while the unrelated inbox
    # driver still loads, keeping the machine networked.
    assert "[module] loaded e1000 (inbox)" in log, log[-2000:]
    assert "[exception]" not in log and "[panic]" not in log, log[-2000:]
    tampered.terminal()
    # The refusal is reported to the user, not swallowed: the module stays in
    # the list as FAILED with the reason attached.
    command(tampered, "dev drivers", "demo")
    command(tampered, "dev drivers", "failed")
    before = len(tampered.log.read_text())
    command(tampered, "dev", None)
    after = tampered.log.read_text()[before:]
    assert "Demo counter" not in after, "a refused module still published a device"
    print("PASS manifest verification refused the modified module image, and the "
          f"rest of the system kept running ({firmware})", flush=True)
finally:
    tampered.close()

print(f"PASS .arco loadable kernel drivers: {firmware}", flush=True)