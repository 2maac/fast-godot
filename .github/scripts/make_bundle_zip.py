#!/usr/bin/env python3
"""Zips a bundle directory deterministically: sorted entries, fixed timestamps.

A toolchain is a fixed artifact for the editor version it ships with, so rebuilding
it must produce the same bytes. Plain zip records mtimes, which made every rebuild a
different file with a different digest for identical content.
"""

import os
import sys
import time
import zipfile

FIXED_TIME = time.struct_time((2020, 1, 1, 0, 0, 0, 0, 0, 0))


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: make_bundle_zip.py <directory> <out.zip>\n")
        return 2
    root = os.path.abspath(argv[1])
    out = os.path.abspath(argv[2])
    entries = []
    for directory, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(directory, name)
            entries.append((os.path.relpath(full, os.path.dirname(root)), full))
    entries.sort()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
        for arcname, full in entries:
            info = zipfile.ZipInfo(arcname.replace(os.sep, "/"), date_time=FIXED_TIME[:6])
            info.external_attr = (0o755 if os.access(full, os.X_OK) else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(full, "rb") as handle:
                archive.writestr(info, handle.read())
    print("%s: %d entries, %.0f MB" % (out, len(entries), os.path.getsize(out) / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
