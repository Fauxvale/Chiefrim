#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Mjolnir armor from the user's own Halo map and Skyrim:
# build/mjolnir/Data/ (meshes, textures, ChiefrimMjolnir.esp), to install as
# a mod. Nothing it writes goes in the repo (build/ is git-ignored), and none
# of it may be redistributed: it is the games' own assets, converted.
# Usage: tools/mjolnir/build.sh HALO_MAP [SKYRIM_DATA_DIR]
set -eu
[ $# -ge 1 ] || { echo "usage: $0 HALO_MAP [SKYRIM_DATA_DIR]"; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
map=$1
skyrim=${2:-"$HOME/.local/share/Steam/steamapps/common/Skyrim Special Edition/Data"}
python3 -I "$here/halo_model.py" "$map"
python3 "$here/fit.py" --skyrim "$skyrim"
python3 "$here/armor.py" --skyrim "$skyrim"
python3 "$here/textures.py"
python3 "$here/plugin.py" --skyrim "$skyrim"
echo "built $(cd "$here/../.." && pwd)/build/mjolnir/Data"
