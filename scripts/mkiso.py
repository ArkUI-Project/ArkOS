#!/usr/bin/env python3
"""Build the dual-boot ArkOS ISO without mtools or grub-mkrescue.

grub-mkrescue requires mtools (mformat) to populate the EFI system
partition image. When mtools is absent this script produces the same
BIOS+UEFI El Torito layout from the locally installed GRUB modules:

  BIOS: cdboot.img + core.img (grub-mkimage i386-pc) -> eltorito.img,
        all i386-pc modules staged for runtime insmod.
  UEFI: monolithic BOOTX64.EFI (grub-mkimage x86_64-efi) placed in a
        deterministic FAT16 image written directly here.
  ISO : xorrisofs with two boot catalog entries.

Stdlib only; every external tool must already be installed.
"""
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GRUB_DIRS = {"i386-pc": Path("/usr/lib/grub/i386-pc"),
             "x86_64-efi": Path("/usr/lib/grub/x86_64-efi")}

# Core image needs only enough to find the ISO and start `normal`; every
# other module is loaded at runtime from /boot/grub/i386-pc/.
# Both cores are minimal: enough to find the ISO and insmod `normal` from
# it. normal must NOT be embedded — its init populates the command.lst
# autoload table using $prefix, which is only correct after the bootstrap
# sets it. Every other module (all_video, gfxterm, multiboot2, ...) is
# resolved at runtime from /boot/grub/<platform>/ on the ISO, exactly like
# grub-mkrescue builds.
BIOS_CORE_MODULES = ["biosdisk", "iso9660", "search",
                     "search_fs_file", "search_fs_uuid", "search_label"]
EFI_CORE_MODULES = ["part_gpt", "part_msdos", "fat", "iso9660", "ntfs",
                    "search", "search_fs_file", "search_fs_uuid",
                    "search_label"]

BOOTSTRAP = b"""search --file --set=root /boot/kernel.elf
set prefix=($root)/boot/grub
insmod normal
normal
"""

EFI_BOOTSTRAP = b"""search --file --set=root /boot/kernel.elf
set prefix=($root)/boot/grub
insmod normal
normal
"""


def patch_relocator_wx(path):
    """Mark relocator.mod .text writable so GRUB can patch its boot stubs.

    GRUB 2.14 applies page permissions from ELF section flags
    (grub_dl_set_mem_attrs), so .text lands in read-only pages.  The
    relocator stubs are patched in place at `boot` time, which page-faults
    (upstream fix moves them to an "awx" section; not in 2.14-1 yet).
    Adding SHF_WRITE to .text achieves the same permissions for this
    single module without touching system GRUB files.
    """
    data = bytearray(Path(path).read_bytes())
    assert data[:4] == b"\x7fELF" and data[4] == 2, "not an ELF64 module"
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, = struct.unpack_from("<H", data, 0x3A)
    shnum, = struct.unpack_from("<H", data, 0x3C)
    shstrndx, = struct.unpack_from("<H", data, 0x3E)
    str_off, = struct.unpack_from("<Q", data, shoff + shstrndx * shentsize + 0x18)
    patched = False
    for i in range(shnum):
        base = shoff + i * shentsize
        name_off, = struct.unpack_from("<I", data, base)
        name = data[str_off + name_off:data.index(b"\0", str_off + name_off)]
        if name == b".text":
            flags, = struct.unpack_from("<Q", data, base + 8)
            struct.pack_into("<Q", data, base + 8, flags | 1)  # SHF_WRITE
            patched = True
    assert patched, ".text section not found"
    Path(path).write_bytes(bytes(data))


def run(*cmd):
    subprocess.run([str(c) for c in cmd], check=True)


DOS_DATE = (32 << 9) | (1 << 5) | 1   # 2020-01-01, keeps images reproducible


def dir_entry(name83, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = name83
    e[11] = attr
    struct.pack_into("<H", e, 14, 0)
    struct.pack_into("<H", e, 16, DOS_DATE)
    struct.pack_into("<H", e, 18, DOS_DATE)
    struct.pack_into("<H", e, 22, 0)
    struct.pack_into("<H", e, 24, DOS_DATE)
    struct.pack_into("<H", e, 26, cluster)
    struct.pack_into("<I", e, 28, size)
    return e


def short_name(name):
    """Uppercase 8.3 name. All names used here are already 8.3-safe."""
    stem, dot, ext = name.upper().partition(".")
    raw = (stem[:8].ljust(8) + (ext[:3].ljust(3) if dot else "   ")).encode("ascii")
    assert len(raw) == 11 and b" " not in raw[:8].strip(), name
    return raw


def write_fat16(path, tree):
    """Write a 16 MiB FAT16 image. tree: {name: bytes | {subtree}}."""
    total, spc, res, nfats, root_ent, spf = 32768, 1, 1, 2, 512, 128
    root_secs = root_ent * 32 // 512
    data_start = res + nfats * spf + root_secs
    clusters_avail = total - data_start
    assert 4085 <= clusters_avail < 65525

    fat = [0] * (clusters_avail + 2)
    fat[0], fat[1] = 0xFFF8, 0xFFFF
    nxt = [2]
    blocks = {}                     # first_cluster -> padded bytes

    def alloc(n):
        first = nxt[0]
        for i in range(n):
            fat[first + i] = first + i + 1 if i + 1 < n else 0xFFFF
        nxt[0] += n
        return first

    def emit(node, parent):
        is_root = parent is None
        cluster = 0 if is_root else alloc(1)
        entries = []
        if not is_root:
            entries.append(dir_entry(b".          ", 0x10, cluster, 0))
            entries.append(dir_entry(b"..         ", 0x10, parent, 0))
        for name, val in node.items():
            if isinstance(val, dict):
                sub = emit(val, cluster)
                entries.append(dir_entry(short_name(name), 0x10, sub, 0))
            else:
                ncls = max(1, (len(val) + 511) // 512)
                first = alloc(ncls)
                blocks[first] = val + b"\0" * (ncls * 512 - len(val))
                entries.append(dir_entry(short_name(name), 0x20, first, len(val)))
        block = bytearray(512)
        for i, e in enumerate(entries):
            block[i * 32:(i + 1) * 32] = e
        blocks[0 if is_root else cluster] = block
        return cluster

    emit(tree, None)
    assert nxt[0] <= clusters_avail + 2, "ESP contents exceed 16 MiB"

    img = bytearray(total * 512)
    img[0:3] = b"\xeb\x3c\x90"
    img[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", img, 11, 512)
    img[13], img[16] = spc, nfats
    struct.pack_into("<H", img, 14, res)
    struct.pack_into("<H", img, 17, root_ent)
    struct.pack_into("<H", img, 19, total)
    img[21] = 0xF8
    struct.pack_into("<H", img, 22, spf)
    struct.pack_into("<H", img, 24, 32)
    struct.pack_into("<H", img, 26, 2)
    img[36], img[38] = 0x80, 0x29
    struct.pack_into("<I", img, 39, 0x4B5200)
    img[43:54] = b"ESP        "
    img[54:62] = b"FAT16   "
    img[510:512] = b"\x55\xaa"

    fat_bytes = b"".join(struct.pack("<H", v) for v in fat)
    fat_bytes += b"\0" * (spf * 512 - len(fat_bytes))
    for i in range(nfats):
        a = (res + i * spf) * 512
        img[a:a + spf * 512] = fat_bytes

    img[(res + nfats * spf) * 512:data_start * 512] = \
        blocks.pop(0) + b"\0" * (root_secs * 512 - 512)

    for first, blob in blocks.items():
        off = data_start * 512 + (first - 2) * 512
        img[off:off + len(blob)] = blob

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(img)


def build(stage, out, volid):
    stage = Path(stage)
    out = Path(out)
    grub_dir = stage / "boot/grub"
    bios_dir = GRUB_DIRS["i386-pc"]
    efi_dir = GRUB_DIRS["x86_64-efi"]

    missing = [m for m in BIOS_CORE_MODULES
               if not (GRUB_DIRS["i386-pc"] / f"{m}.mod").is_file()]
    assert not missing, f"GRUB modules missing for i386-pc: {missing}"

    boot_cfg = grub_dir / "grub.cfg"
    assert boot_cfg.is_file(), f"stage missing {boot_cfg}"
    kernel = stage / "boot/kernel.elf"
    assert kernel.is_file(), f"stage missing {kernel}"

    # Fonts for gfxterm (auto-loaded from $prefix/fonts/unicode.pf2).
    fonts = grub_dir / "fonts"
    fonts.mkdir(parents=True, exist_ok=True)
    for pf2 in ("unicode.pf2", "ascii.pf2", "euro.pf2"):
        src = Path("/usr/share/grub") / pf2
        if src.is_file():
            shutil.copy2(src, fonts / pf2)

    # Runtime-loadable modules + dependency lists for both platforms.
    for platform, src in (("i386-pc", bios_dir), ("x86_64-efi", efi_dir)):
        mod_dir = grub_dir / platform
        mod_dir.mkdir(parents=True, exist_ok=True)
        for f in src.iterdir():
            if f.suffix in (".mod", ".lst"):
                shutil.copy2(f, mod_dir / f.name)
    mod_dir = grub_dir / "i386-pc"
    # relocator.mod self-patches its stubs; make them writable on EFI only
    # (see patch_relocator_wx). Patch the staged copy, not the system file.
    patch_relocator_wx(grub_dir / "x86_64-efi/relocator.mod")

    # BIOS core image -> eltorito.img.
    bootstrap = stage / "boot/bootstrap.cfg"
    bootstrap.write_bytes(BOOTSTRAP)
    core = stage / "core.img"
    run("grub-mkimage", "-O", "i386-pc", "-d", bios_dir, "-o", core,
        "-p", "(cd)/boot/grub", "-c", bootstrap, *BIOS_CORE_MODULES)
    eltorito = mod_dir / "eltorito.img"
    eltorito.write_bytes((bios_dir / "cdboot.img").read_bytes() + core.read_bytes())
    core.unlink()
    bootstrap.unlink()

    # EFI core image -> BOOTX64.EFI inside a FAT16 ESP image. Only a minimal
    # core is embedded; every other module (normal, gfxterm, multiboot2, ...)
    # loads at runtime from /boot/grub/x86_64-efi/ on the ISO. The staged
    # relocator.mod there carries a writable .text (patch_relocator_wx) so
    # GRUB 2.14's runtime stub patching does not fault on NX-protected pages.
    bootstrap.write_bytes(EFI_BOOTSTRAP)
    bootx64 = stage / "BOOTX64.EFI"
    run("grub-mkimage", "-O", "x86_64-efi", "-d", efi_dir, "-o", bootx64,
        "-p", "/boot/grub", "-c", bootstrap, *EFI_CORE_MODULES)
    bootstrap.unlink()
    write_fat16(grub_dir / "efi.img",
                {"EFI": {"BOOT": {"BOOTX64.EFI": bootx64.read_bytes()}}})
    bootx64.unlink()

    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    run("xorrisofs", "-o", out, "-volid", volid, "-r", "-J",
        "-b", "boot/grub/i386-pc/eltorito.img",
        "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
        "-eltorito-alt-boot",
        "-e", "boot/grub/efi.img", "-no-emul-boot",
        stage)
    print(f"{out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: mkiso.py <stage dir> <out.iso> [volid]")
    build(Path(sys.argv[1]), Path(sys.argv[2]),
          sys.argv[3] if len(sys.argv) > 3 else "ARKOS0130")
