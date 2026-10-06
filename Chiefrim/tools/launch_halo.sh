#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts Halo in Chiefrim mode for a session with Skyrim (docs/DESIGN.md §11).
# Start it before or after Skyrim; the SKSE plugin links when both are up.
#
# Usage: tools/launch_halo.sh [--bot]
#   --bot  a scripted player walks, turns and jumps by itself (HALO_TEST_INPUT),
#          so Skyrim can keep the focus while you watch it follow
#
# Phase 0: Halo shows its own window. Click into it to drive Chief (WASD,
# mouse, space); the Skyrim player follows. Halo's data root is
# build/halo-data, a link to the user's maps/ (HALO_MAPS), so the user's own
# Halo install and saves are never touched.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
maps=${HALO_MAPS:-"$root/../HaloProjects/Halo-CE-Universal/maps"}
data="$root/build/halo-data"
halo="$root/halo/.work/build/linux/halo"

[ -x "$halo" ] || { echo "build Halo first: tools/setup_halo.py"; exit 1; }
[ -d "$maps" ] || { echo "no maps at $maps (set HALO_MAPS)"; exit 1; }
mkdir -p "$data" "$root/build/halo-saves" "$root/build/collision-dumps"
ln -sfn "$(cd "$maps" && pwd)" "$data/maps"
printf 'map_name levels\\b30\\b30\n' > "$data/init.txt"

bot=""
[ "${1:-}" = "--bot" ] && bot="bot:1"

echo "Halo log: $data/debug.txt; its terminal output also goes to $data/halo.out"
env CHIEFRIM=1 CHIEFRIM_DUMP_DIR="$root/build/collision-dumps" \
	HALO_DATA_ROOT="$data" HALO_SAVE_ROOT="$root/build/halo-saves" \
	HALO_UPDATE_AUTO=false HALO_NET_ONLINE=false HALO_FULLSCREEN=0 \
	HALO_TEST_INPUT="$bot" \
	"$halo" 2>&1 | tee "$data/halo.out"
