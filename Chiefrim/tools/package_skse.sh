#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Packs the built SKSE plugin as a mod archive for a mod manager (Amethyst, MO2, Vortex):
# build/dist/Chiefrim-<version>.zip with SKSE/Plugins/Chiefrim.dll, its
# license (GPL-3.0-or-later) and the third-party notices.
# Code only: no game files or assets go in it. A public release must also
# point to the corresponding source (docs/LICENSING.md).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
dll="$root/build/skse/Chiefrim.dll"
[ -f "$dll" ] || { echo "build the plugin first: tools/setup_skse.sh"; exit 1; }
version=$(sed -n 's/^project(Chiefrim VERSION \([0-9.]*\).*/\1/p' "$root/skse/CMakeLists.txt")
stage="$root/build/dist/stage"
mkdir -p "$stage/SKSE/Plugins"
python3 "$root/tools/check_licenses.py"
cp "$dll" "$stage/SKSE/Plugins/"
cp "$root/skse/Chiefrim.ini" "$stage/SKSE/Plugins/"
cp "$root/../LICENSE" "$stage/SKSE/Plugins/Chiefrim-LICENSE.txt"
cp "$root/../THIRD-PARTY-NOTICES.md" "$stage/SKSE/Plugins/Chiefrim-THIRD-PARTY-NOTICES.md"
zip="$root/build/dist/Chiefrim-$version.zip"
(cd "$stage" && python3 -c "
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for name in ('Chiefrim.dll', 'Chiefrim.ini', 'Chiefrim-LICENSE.txt', 'Chiefrim-THIRD-PARTY-NOTICES.md'):
        z.write('SKSE/Plugins/' + name)
" "$zip")
echo "wrote $zip"
