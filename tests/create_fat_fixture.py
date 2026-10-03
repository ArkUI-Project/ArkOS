#!/usr/bin/env python3
"""Create a new independently formatted FAT32 fixture; refuses existing paths."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def create(path):
    with path.open('xb') as image:
        image.truncate(64 * 1024 * 1024)
    env = os.environ.copy()
    tools = ROOT.parent / 'toolroot' / 'usr'
    env['PATH'] = str(tools/'sbin') + ':' + str(tools/'bin') + ':' + env['PATH']
    def run(*args):
        subprocess.run(args, env=env, check=True)
    run('mkfs.fat', '-F', '32', '-n', 'ARKSHARE', str(path))
    run('mmd', '-i', str(path), '::/Documents')
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        files = {
            'README.TXT': 'ArkOS external FAT32 volume.\nThis file was created independently by host mtools.\n',
            '中文示例.txt': '中文文件：欢迎使用 ArkOS。\n',
            'Documents/host-note.txt': 'A note from the host. Native ArkOS can update this file.\n',
            'large.txt': 'Large file for bounded text-editor checks.\n' * 1300,
        }
        for i, (name, content) in enumerate(files.items()):
            local = temp / str(i)
            local.write_text(content)
            run('mcopy', '-i', str(path), str(local), '::/' + name)
    run('fsck.fat', '-n', str(path))
if __name__ == '__main__':
    path = Path(sys.argv[1] if len(sys.argv)>1 else ROOT/'build/fat32-demo.img')
    path.parent.mkdir(parents=True, exist_ok=True)
    create(path)
    print(path)
