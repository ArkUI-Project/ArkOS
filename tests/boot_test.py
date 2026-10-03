#!/usr/bin/env python3
"""Compatibility entry point: ArkOS 0.3 storage/reboot test replaces 0.1 RAM test."""
import runpy
from pathlib import Path
runpy.run_path(str(Path(__file__).with_name('storage_vm_test.py')),run_name='__main__')
