#!/usr/bin/env python3
"""Current desktop integration entry point: settings, controls and persistence."""
import runpy
from pathlib import Path
runpy.run_path(str(Path(__file__).with_name('ui_v3_test.py')),run_name='__main__')
