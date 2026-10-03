#!/usr/bin/env python3
"""Offline macOS adapter using the prepared, unmodified Debian GRUB 2.12 images.

ARKOS_GRUB_CACHE contains rescue/, efi/ and efi-bin/ extracted package trees.
Invoke with the Makefile's grub-mkrescue arguments. Package sources are recorded in third_party/grub/README.md.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

args = sys.argv[1:]
out = Path(args[args.index('-o')+1])
stage = Path(args[args.index('-o')+2])
cache = Path(os.environ.get('ARKOS_GRUB_CACHE', '/private/tmp/arkos-grub'))
volume = args[args.index('-volid')+1] if '-volid' in args else 'ARKOS0110'
bootstrap = Path(__file__).resolve().parents[1]/'boot/efi-bootstrap.cfg'
shutil.copytree(cache/'efi-bin/usr/lib/grub/x86_64-efi', stage/'boot/grub/x86_64-efi', dirs_exist_ok=True)
out.unlink(missing_ok=True)
efi = stage/'boot/grub/efi.img'
efi.parent.mkdir(parents=True, exist_ok=True)
def run(*command):
    subprocess.run(command, check=True)
run('mformat', '-i', str(efi), '-C', '-T', '32768', '::')
run('mmd', '-i', str(efi), '::/EFI', '::/EFI/BOOT', '::/boot', '::/boot/grub')
run('mcopy', '-i', str(efi), str(cache/'efi/usr/lib/grub/x86_64-efi/monolithic/gcdx64.efi'), '::/EFI/BOOT/BOOTX64.EFI')
run('mcopy', '-i', str(efi), str(bootstrap), '::/boot/grub/grub.cfg')
run('xorriso', '-indev', str(cache/'rescue/usr/lib/grub-rescue/grub-rescue-cdrom.iso'),
    '-outdev', str(out), '-volid', volume, '-map', str(stage), '/',
    '-boot_image', 'any', 'replay', '-boot_image', 'any', 'next',
    '-boot_image', 'any', 'efi_path=/boot/grub/efi.img',
    '-boot_image', 'any', 'platform_id=0xef', '-boot_image', 'any', 'emul_type=no_emulation',
    '-boot_image', 'any', 'efi_boot_part=--efi-boot-image', '-commit', '-end')
