#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prunes Halo's collision build dumps (build/collision-dumps, docs/DESIGN.md
§5.2): every dump of a build Chief fell through or got stuck in is kept (it
carries his position: the cases to replay), and of the builds dumped for
being slow only the newest --keep-slow. Slow builds come in runs of three a
Halo start, many of the same place, and tools/test_bsp.sh replays them all.

Usage: tools/prune_dumps.py [--keep-slow N] [--dry-run] [DIR]
"""

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def has_chief(path):
    """CRDUMP2: the header, the count, the triangles, then a flag (1: Chief's
    position follows) and his position, 16 bytes in all; None: not a dump"""
    with path.open("rb") as dump:
        if dump.read(8) != b"CRDUMP2\0":
            return None
        dump.seek(-16, 2)
        return struct.unpack("<i", dump.read(4))[0] == 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--keep-slow", type=int, default=20)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("dir", nargs="?", default=ROOT / "build" / "collision-dumps", type=Path)
    options = parser.parse_args()
    if not options.dir.is_dir():
        return 0

    slow = []
    kept_falls = 0
    for path in options.dir.glob("build-*.bin"):
        try:
            chief = has_chief(path)
        except OSError:
            continue
        if chief is None:
            continue
        if chief:
            kept_falls += 1
        else:
            slow.append(path)
    slow.sort(key=lambda path: path.stat().st_mtime, reverse=True)
    removed = slow[max(options.keep_slow, 0):]
    freed = sum(path.stat().st_size for path in removed)
    if not options.dry_run:
        for path in removed:
            path.unlink()
    if removed:
        print(f"prune_dumps: {'would remove' if options.dry_run else 'removed'} {len(removed)} slow-build dumps"
              f" ({freed / 1e6:.0f} MB); kept {len(slow) - len(removed)} and {kept_falls} of Chief falling or stuck")
    return 0


if __name__ == "__main__":
    sys.exit(main())
