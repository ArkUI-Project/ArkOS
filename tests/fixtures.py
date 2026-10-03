"""Fresh project-owned disks for guest tests, generated from source fixtures."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def _copy_fixture(destination, script):
    destination = Path(destination)
    if not destination.resolve().is_relative_to((ROOT / 'build').resolve()):
        raise ValueError('Guest fixture destination must be inside the project build directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fixture-', dir=destination.parent) as directory:
        image = Path(directory) / 'disk.img'
        subprocess.run([sys.executable, str(ROOT / script), str(image)], check=True)
        shutil.copyfile(image, destination)
    return destination


def fresh_data_disk(destination):
    return _copy_fixture(destination, 'scripts/create-disk.py')


def fresh_compat_disk(destination):
    return _copy_fixture(destination, 'tests/create_compat_fixture.py')
