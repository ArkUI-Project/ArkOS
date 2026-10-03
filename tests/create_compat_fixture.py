#!/usr/bin/env python3
"""Build a new disposable MBR fixture from independently formatted volumes."""
from pathlib import Path
import argparse, struct, subprocess, sys, shutil, tempfile

ROOT = Path(__file__).resolve().parents[1]


def create(out):
    if out.exists():
        raise FileExistsError(f'Refusing to overwrite {out}')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='volumes-', dir=out.parent) as directory:
        fat = Path(directory) / 'fat32.img'
        ntfs = Path(directory) / 'ntfs.img'
        subprocess.run([sys.executable, str(ROOT / 'tests/create_fat_fixture.py'), str(fat)], check=True)
        subprocess.run([sys.executable, str(ROOT / 'tests/ntfs_make_fixture.py'),
                        '--demo', '--output', str(ntfs)], check=True)
        assert fat.stat().st_size == 64 * 1024 * 1024 and ntfs.stat().st_size == 32 * 1024 * 1024
        mbr = bytearray(512)
        for i, (kind, start, count) in enumerate([(0x0c, 2048, 131072), (0x07, 133120, 65536)]):
            at = 446 + i * 16
            mbr[at + 1:at + 4] = b'\xfe\xff\xff'
            mbr[at + 4] = kind
            mbr[at + 5:at + 8] = b'\xfe\xff\xff'
            struct.pack_into('<II', mbr, at + 8, start, count)
        mbr[510:512] = b'\x55\xaa'
        with out.open('xb') as target:
            target.truncate(97 * 1024 * 1024)
            target.write(mbr)
            for source, start in [(fat, 2048), (ntfs, 133120)]:
                target.seek(start * 512)
                with source.open('rb') as volume:
                    shutil.copyfileobj(volume, target)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', nargs='?', type=Path, default=ROOT / 'build/compat-data.img')
    output = parser.parse_args().path
    create(output)
    print(output)
