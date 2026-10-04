#!/bin/sh
# Packs the built SKSE plugin as a mod archive for Vortex or MO2:
# build/dist/Chiefrim-<version>.zip with SKSE/Plugins/Chiefrim.dll.
# Code only: no game files or assets go in it.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
dll="$root/build/skse/Chiefrim.dll"
[ -f "$dll" ] || { echo "build the plugin first: tools/setup_skse.sh"; exit 1; }
version=$(sed -n 's/^project(Chiefrim VERSION \([0-9.]*\).*/\1/p' "$root/skse/CMakeLists.txt")
stage="$root/build/dist/stage"
mkdir -p "$stage/SKSE/Plugins"
cp "$dll" "$stage/SKSE/Plugins/"
zip="$root/build/dist/Chiefrim-$version.zip"
(cd "$stage" && python3 -c "
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('SKSE/Plugins/Chiefrim.dll')
" "$zip")
echo "wrote $zip"
