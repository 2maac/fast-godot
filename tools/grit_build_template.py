#!/usr/bin/env python3
"""Compatibility shim: the build script is now tools/grit_build.py.

Kept so an older editor (which looks this file up by name) can still build exports
from a newer checkout.
"""

import os
import runpy
import sys

if __name__ == "__main__":
    runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "grit_build.py"), run_name="__main__")
    sys.exit(0)
