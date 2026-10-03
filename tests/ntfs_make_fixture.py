#!/usr/bin/env python3
"""Create disposable real NTFS fixtures with host-only mkntfs/ntfscp."""
import argparse, os, pathlib, shutil, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--demo', action='store_true')
parser.add_argument('--output', type=pathlib.Path)
options = parser.parse_args()
demo = options.demo
folder = (options.output.parent / (options.output.stem + '-files') if options.output
          else root / 'build' / ('ntfs-demo' if demo else 'ntfs-test'))
folder.mkdir(parents=True, exist_ok=True)
toolroot = pathlib.Path(os.environ.get('ARK_TOOLROOT', str(root.parent / 'toolroot')))
def tool(name):
    for p in [toolroot / 'sbin' / name, toolroot / 'bin' / name]:
        if p.exists(): return str(p)
    return shutil.which(name) or name
env = dict(os.environ, LD_LIBRARY_PATH=str(toolroot / 'lib/x86_64-linux-gnu'), LC_ALL='C.UTF-8')
def run(*args):
    p = subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode: raise RuntimeError(p.stdout)
image = options.output or (root / 'build' / 'ntfs-demo.img' if demo else folder / 'fixture.img')
with image.open('xb') as f: f.truncate((32 if demo else 64) * 1024 * 1024)
run(tool('mkntfs'), '-F', '-Q', '-L', 'ARK-NTFS-TEST', str(image))
if demo:
    files = {
        'README.txt': 'ArkOS native NTFS read-only demo\n\nThis file is stored on a real NTFS volume.\nTry: cat /mnt/ntfs/README.txt\nCopy to home or FAT32 to edit; NTFS writes are unsupported.\n',
        '中文说明.txt': 'ArkOS 原生 NTFS 只读驱动\n\n这不是内存模拟文件，也不依赖 Linux 内核。\n支持目录、中文文件名，以及普通驻留和非驻留文件。\n本卷只读，可复制到个人目录或 FAT32 卷后编辑。\n',
        'large.txt': 'This larger NTFS file demonstrates the 16383-byte desktop text-view limit.\n' * 1500,
    }
    for name, text in files.items():
        source = folder / name; source.write_text(text, encoding='utf-8')
        run(tool('ntfscp'), str(image), str(source), '/' + name)
    print(image)
    sys.exit(0)
small = folder / 'resident.txt'; small.write_text('Hello from native NTFS!\n你好，ArkOS！\n', encoding='utf-8')
big = folder / 'large.bin'; big.write_bytes(bytes((i * 29 + 7) % 251 for i in range(100000)))
run(tool('ntfscp'), str(image), str(small), '/resident.txt')
run(tool('ntfscp'), str(image), str(big), '/large.bin')
run(tool('ntfscp'), str(image), str(small), '/中文🚀.txt')
run(tool('ntfscp'), str(image), str(small), '/$Extend/nested.txt')
# Force a genuine INDEX_ALLOCATION tree, rather than testing only a tiny root.
for i in range(96):
    run(tool('ntfscp'), str(image), str(small), f'/entry-{i:03}.txt')
print(image)
