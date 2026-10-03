#!/usr/bin/env python3
"""Prepare a local development copy of the pinned Homebrew arm64 QEMU.

Source-equivalent change: include/ui/console.h default GUI interval 30 -> 4 ms.
Only this known 11.1.2 bottle is accepted. The installed executable is read-only.
The generated emulator belongs in build/, outside source/release archives.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

SOURCE_SHA256 = 'a1b97665fb9f341a65acf9b0e7dc4c7235972aae61182545ed1aacf5ffca87fa'


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, default=shutil.which('qemu-system-x86_64'))
    parser.add_argument('--output', type=Path, default=root/'build/tools/qemu-system-x86_64-high-refresh')
    args = parser.parse_args()
    if args.qemu is None:
        parser.error('Install QEMU 11.1.2 through Homebrew first.')
    data = bytearray(args.qemu.read_bytes())
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        parser.error('This script supports the verified Homebrew arm64 11.1.2 bottle only; the source-equivalent change is include/ui/console.h default GUI interval 30 -> 4 ms.')
    output = args.output.resolve()
    if output.exists():
        parser.error('Output already exists: '+str(output))
    # _gui_update + 0x4c: MOV W9,#30. The complete input hash pins both the
    # function and its file offset; replace its 16-bit immediate with 4.
    assert struct.unpack_from('<I', data, 0x384788)[0] == 0x528003c9
    struct.pack_into('<I', data, 0x384788, 0x52800089)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    output.chmod(0o755)
    subprocess.run(['codesign', '--force', '--sign', '-', str(output)], check=True)
    version = subprocess.check_output([str(output), '--version'], text=True).splitlines()[0]
    metadata = {'source': str(args.qemu.resolve()), 'source_sha256': SOURCE_SHA256,
                'output_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                'version': version, 'gui_refresh_interval_ms': 4,
                'source_equivalent_change': 'include/ui/console.h: default GUI interval 30 -> 4 ms'}
    output.with_suffix('.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(str(output))


if __name__ == '__main__':
    main()
