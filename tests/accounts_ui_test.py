#!/usr/bin/env python3
"""Native ring-3 account UI -> syscall -> PBKDF2 -> ATA persistence.

Creates an exclusive fresh disk and freezes the ISO; never changes release data.
Two independent QEMU processes boot BIOS then UEFI. No network test port used.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time

from PIL import Image
import vm as vm_helper
from vm import ROOT, VM
from storage_vm_test import snapshot, command, poweroff

ADMIN_PASSWORD = "Native53!"
GUEST_PASSWORD = "Guest579!"


def profiles(disk):
    data = snapshot(disk)["entries"].get("/.system/accounts", {}).get("data", "")
    if not data:
        return {}, data
    lines = data.splitlines()
    assert lines[0].startswith("ARKACCT1|")
    result = {}
    for line in lines[1:]:
        uid, flags, user, display, salt, verifier, iterations = line.split("|")
        result[user] = dict(uid=int(uid), flags=int(flags), display=bytes.fromhex(display).decode(),
                            salt=bytes.fromhex(salt), verifier=bytes.fromhex(verifier),
                            iterations=int(iterations))
    return result, data


def wait_disk(disk, predicate):
    end = time.monotonic() + 30
    while time.monotonic() < end:
        users, data = profiles(disk)
        if predicate(users):
            return users, data
        time.sleep(.1)
    raise AssertionError("Account mutation was not committed to the ATA image")


def capture(vm, name):
    vm.screen(name)
    image = Image.open(vm.out / (name + ".ppm"))
    assert image.size == (1280, 800), image.size
    image.save(vm.out / (name + ".png"))


def login(vm, password, guest=False):
    if guest:
        vm.key("down")
    before = len(vm.log.read_text())
    vm.type(password)
    start = time.monotonic()
    vm.key("ret")
    vm.wait("[session] desktop unlocked", timeout=45, after=before)
    return round(time.monotonic() - start, 3)


def logout_manager(vm):
    before = len(vm.log.read_text())
    vm.tap(510, 568)  # manager x320/y135, logout button
    vm.wait("[session] login ready", after=before)


def enter_manager(vm):
    vm.key("ctrl-u")
    time.sleep(.35)


def no_password_logs(vm):
    text = vm.log.read_text()
    for value in (ADMIN_PASSWORD, GUEST_PASSWORD, "Wrong579!"):
        assert value not in text, "A password was exposed in the serial log"


def main():
    parent = ROOT / "build" / "test-accounts-ui"
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    frozen = work / "arkos-accounts-frozen.iso"
    shutil.copy2(Path(os.environ.get("ARKOS_ISO", ROOT / "build/arkos-0.5.0.iso")), frozen)
    os.environ["ARKOS_ISO"] = str(frozen)
    disk = work / "accounts-data.img"
    subprocess.run(["python3", str(ROOT / "scripts/create-disk.py"), str(disk)], check=True)
    vm_helper.ROOT = work
    report = {"iso_sha256": hashlib.sha256(frozen.read_bytes()).hexdigest(), "work": str(work), "sessions": []}
    print("Frozen ISO", report["iso_sha256"], flush=True)
    print("Diagnostics", work, flush=True)

    vm = VM("01-accounts-bios", disk=disk, firmware="bios")
    try:
        vm.wait("[session] setup ready")
        capture(vm, "01-first-boot")
        before = len(vm.log.read_text())
        vm.type(ADMIN_PASSWORD)
        vm.key("tab")
        vm.type(ADMIN_PASSWORD)
        start = time.monotonic()
        vm.key("ret")
        vm.wait("[session] desktop unlocked", timeout=45, after=before)
        enroll_seconds = round(time.monotonic() - start, 3)
        vm.terminal()
        command(vm, 'echo "ARK-PRIVATE-PERSIST-57" > private.txt')
        command(vm, "sync", "Mounted filesystems synchronized.")
        before = len(vm.log.read_text())
        vm.key("ctrl-k")
        vm.wait("[session] locked", after=before)
        capture(vm, "02-locked")
        before = len(vm.log.read_text())
        vm.type("Wrong579!")
        vm.key("ret")
        time.sleep(2)
        vm.key("f1")
        capture(vm, "03-wrong-password")
        assert "[session] desktop unlocked" not in vm.log.read_text()[before:]
        assert "[ui] open Terminal" not in vm.log.read_text()[before:]
        unlock_seconds = login(vm, ADMIN_PASSWORD)
        enter_manager(vm)
        capture(vm, "04-admin-controls")
        vm.tap(552, 520)  # new user
        for index, value in enumerate(("guest", "Guest", GUEST_PASSWORD, GUEST_PASSWORD)):
            if index:
                vm.key("tab")
            vm.type(value)
        vm.key("ret")
        users, _ = wait_disk(disk, lambda users: "guest" in users)
        assert users["guest"]["uid"] == 1001 and users["guest"]["flags"] == 0
        capture(vm, "05-created-guest")

        # Exercise both administrator controls through their real native UI.
        vm.tap(446, 271)  # second row
        vm.tap(876, 520)
        wait_disk(disk, lambda users: users.get("guest", {}).get("flags") == 2)
        vm.tap(876, 520)
        wait_disk(disk, lambda users: users.get("guest", {}).get("flags") == 0)
        logout_manager(vm)
        # Selection remains on guest after the management operation.
        guest_login_seconds = login(vm, GUEST_PASSWORD)
        vm.terminal()
        command(vm, "pwd", "\n/home/guest\n")
        command(vm, "cd /etc")
        command(vm, "cd")
        command(vm, "pwd", "\n/home/guest\n")
        command(vm, 'echo "GUEST-PERSIST-93" > guest.txt')
        before = command(vm, "cat /home/ark/private.txt", "File not found.")
        assert "\nARK-PRIVATE-PERSIST-57\n" not in vm.log.read_text()[before:]
        command(vm, "cat /.system/accounts", "File not found.")
        capture(vm, "06-guest-home-and-scope")

        enter_manager(vm)
        users_before, db_before = profiles(disk)
        vm.tap(552, 520)  # hidden create control must not open a form
        vm.type("denied-user")
        vm.key("ret")
        vm.tap(446, 240)  # select Ark then attempt disabled control
        vm.tap(876, 520)
        capture(vm, "07-standard-user-controls")
        _, db_after = profiles(disk)
        assert db_before == db_after and users_before["ark"]["flags"] == 1
        vm.key("esc")
        vm.terminal()
        command(vm, "sync", "Mounted filesystems synchronized.")
        no_password_logs(vm)
        poweroff(vm)
        report["sessions"].append({"firmware": "BIOS", "enroll_to_desktop_seconds": enroll_seconds,
                                   "unlock_to_desktop_seconds": unlock_seconds, "guest_login_seconds": guest_login_seconds,
                                   "checks": ["first enrollment", "wrong unlock stays locked", "administrator creates/enables/disables user",
                                              "guest home and default cd", "private home and account file denied", "standard-user controls denied", "ACPI shutdown"]})
    finally:
        vm.close()

    saved = snapshot(disk)
    assert saved["entries"]["/home/guest/guest.txt"]["data"] == "GUEST-PERSIST-93\n"
    assert saved["entries"]["/home/ark/private.txt"]["data"] == "ARK-PRIVATE-PERSIST-57\n"
    users, database = profiles(disk)
    assert set(users) == {"ark", "guest"}
    for user, password in (("ark", ADMIN_PASSWORD), ("guest", GUEST_PASSWORD)):
        profile = users[user]
        assert password not in database
        assert hashlib.pbkdf2_hmac("sha256", password.encode(), profile["salt"], profile["iterations"]) == profile["verifier"]
    assert users["ark"]["salt"] != users["guest"]["salt"]
    report["host_checks"] = ["independent ArkFS CRC/generation decoder", "two private home files", "independent hashlib PBKDF2 comparison", "no plaintext passwords", "unique salts"]
    print("PASS BIOS enrollment, lock, account management, scoped guest files, real ATA snapshot", flush=True)

    vm = VM("02-accounts-uefi", disk=disk, firmware="uefi")
    try:
        vm.wait("[session] login ready")
        assert "[session] setup ready" not in vm.log.read_text()
        capture(vm, "08-persisted-login")
        login(vm, GUEST_PASSWORD, guest=True)
        vm.terminal()
        command(vm, "pwd", "\n/home/guest\n")
        command(vm, "cat guest.txt", "\nGUEST-PERSIST-93\n")
        command(vm, "cat /home/ark/private.txt", "File not found.")
        capture(vm, "09-uefi-guest-persisted")
        enter_manager(vm)
        logout_manager(vm)
        # Manager selected current guest; arrow up selects Ark on login screen.
        vm.key("up")
        login(vm, ADMIN_PASSWORD)
        vm.terminal()
        command(vm, "pwd", "\n/home/ark\n")
        command(vm, "cat private.txt", "\nARK-PRIVATE-PERSIST-57\n")
        command(vm, "cat /home/guest/guest.txt", "File not found.")
        capture(vm, "10-uefi-admin-persisted")
        no_password_logs(vm)
        poweroff(vm)
        report["sessions"].append({"firmware": "UEFI", "checks": ["login required after reboot", "guest password and files persist", "admin password and files persist", "bidirectional home isolation", "ACPI shutdown"]})
    finally:
        vm.close()
    _, final_database = profiles(disk)
    assert final_database == database, "Read-only account sessions changed account records"
    report["final_generation"] = snapshot(disk)["generation"]
    report["result"] = "PASS"
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS independent UEFI process restored both accounts and private files", flush=True)
    print("REPORT", work / "report.json", flush=True)


if __name__ == "__main__":
    main()
