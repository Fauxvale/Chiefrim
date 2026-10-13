#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Arsenal, the Mjolnir armor and Halo's weapons (as Skyrim
# items), from the user's own Halo map and Skyrim:
# build/arsenal/Data/ (meshes, textures, ChiefrimArsenal.esp), and the same
# as a mod archive for a mod manager, build/dist/ChiefrimArsenal.zip.
# Nothing it writes goes in the repo (build/ is git-ignored), and none of it
# may be redistributed: it is the games' own assets, converted.
# Usage: tools/arsenal/build.sh [HALO_MAP] [SKYRIM_DATA_DIR]
# Unless given (or given as ""), both are where tools/game_paths.py finds
# them (docs/INSTALLATION.md): the host map, d20 (Keyes), in the user's
# extracted maps/ (HALO_MAPS, CHIEFRIM_MAP), and Skyrim's Data/ (SKYRIM_DATA,
# else Steam's libraries).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
map=${1:-$(python3 -I "$here/../game_paths.py" map)}
skyrim=${2:-$(python3 -I "$here/../game_paths.py" skyrim)}
[ -f "$map" ] || { echo "no Halo map at $map: extract Halo's maps (docs/INSTALLATION.md) or set HALO_MAPS"; exit 1; }
[ -f "$skyrim/Skyrim.esm" ] || { echo "no Skyrim at $skyrim: set SKYRIM_DATA to Skyrim's Data folder"; exit 1; }
build=$(cd "$here/../.." && pwd)/build
# from nothing, so no file of an earlier build ends up in the archive
rm -rf "$build/arsenal/Data"
python3 -I "$here/halo_model.py" "$map"
python3 "$here/fit.py" --skyrim "$skyrim"
python3 "$here/armor.py" --skyrim "$skyrim"
python3 "$here/ground.py" --skyrim "$skyrim"
python3 "$here/textures.py"
python3 -I "$here/weapons.py" "$map" --skyrim "$skyrim"
python3 "$here/plugin.py" --skyrim "$skyrim"
zip="$build/dist/ChiefrimArsenal.zip"
mkdir -p "$build/dist"
python3 -I - "$build/arsenal/Data" "$zip" <<'PY'
import sys, zipfile
from pathlib import Path
data, archive = Path(sys.argv[1]), Path(sys.argv[2])
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
    for path in sorted(p for p in data.rglob("*") if p.is_file()):
        z.write(path, path.relative_to(data).as_posix())
PY
echo "built $build/arsenal/Data"
echo "packed $zip (for your own game only: it holds Halo's and Skyrim's assets)"
