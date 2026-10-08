#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lists the tags of Halo cache maps (docs/DESIGN.md §5.3): which weapons
(and, with --class, bipeds, vehicles or any other tag class) each map holds,
to choose the host map. Reads the user's own maps; nothing is written.

Xbox maps (version 5): a 2 KiB header, then the rest of the file deflated.
The tag data's own header gives the tag instances (32 bytes each: three
class codes, the tag's id, and its name and data addresses in the loaded
map, at the tag data's base address).

Usage: tools/list_map_tags.py [--class weap] [--matrix] MAP.map...
  --matrix  one row per tag, one column per map: which maps hold it
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path


def tags(path):
    raw = Path(path).read_bytes()
    if raw[:4] != b"daeh":
        raise ValueError(f"{path}: not a Halo cache map")
    version, size, _, tag_offset, tag_size = struct.unpack_from("<5I", raw, 4)
    data = raw[:0x800] + zlib.decompress(raw[0x800:]) if version == 5 else raw
    if len(data) < tag_offset + tag_size:
        raise ValueError(f"{path}: {len(data)} bytes, the tag data ends at {tag_offset + tag_size}")
    instances_address, _, _, count = struct.unpack_from("<4I", data, tag_offset)
    base = instances_address - 0x24  # the instances follow the tag data's 36-byte header

    def string(address):
        start = tag_offset + address - base
        return data[start:data.index(b"\0", start)].decode("latin-1")

    result = []
    for index in range(count):
        cls, _, _, _, name_address = struct.unpack_from("<4sII I I", data, instances_address - base + tag_offset + 32 * index)
        result.append((cls[::-1].decode("latin-1"), string(name_address)))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("maps", nargs="+")
    parser.add_argument("--class", dest="cls", default="weap", help="tag class (weap, bipd, vehi, eqip, ...)")
    parser.add_argument("--matrix", action="store_true")
    options = parser.parse_args()

    per_map = {}
    for path in options.maps:
        try:
            per_map[Path(path).stem] = sorted({name for cls, name in tags(path) if cls == options.cls})
        except (ValueError, zlib.error) as error:
            print(error, file=sys.stderr)
    if options.matrix:
        names = sorted({name for found in per_map.values() for name in found})
        width = max(map(len, names), default=0)
        print(" " * width + "  " + " ".join(f"{m[:11]:>11}" for m in per_map))
        for name in names:
            print(f"{name:<{width}}  " + " ".join(f"{'x' if name in found else '.':>11}" for found in per_map.values()))
    else:
        for map_name, found in per_map.items():
            print(f"{map_name}: {len(found)} {options.cls}")
            for name in found:
                print(f"  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
