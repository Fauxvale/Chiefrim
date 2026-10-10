#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Phase 0 test stand (docs/DESIGN.md §12): runs the Chiefrim Halo build in
# Chiefrim mode against tools/fake_skyrim.py, then prints both sides' logs.
#
# Usage: tools/run_phase0.sh [seconds] [--visible]
#   --visible  show Halo's window (with sound) on the desktop
#   Otherwise Halo runs in a headless gamescope (no window, no sound). Not
#   the port's own hidden-window mode: with it, the lens-flare occlusion query
#   crashes the GL driver after a few seconds (upstream bug, also without
#   Chiefrim; docs/DESIGN.md §15).
#
# Extra environment passes through, for example HALO_TEST_INPUT=bot:1 (a
# scripted player that walks, turns, jumps and fires), CHIEFRIM_DEBUG=1, and
# FAKE_SKYRIM_ARGS (e.g. "--silence-at 5 --silence-for 4").
# Halo's data root is build/halo-data: a link to the user's maps/ folder and
# its own init.txt (the host map), so the user's Halo install and saves are
# never touched. Set HALO_MAPS to the folder holding the extracted maps/.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
seconds=${1:-40}
visible=${2:-}
maps=${HALO_MAPS:-"$root/../HaloProjects/Halo-CE-Universal/maps"}
data="$root/build/halo-data"
log="$root/build/phase0"
halo="$root/halo/.work/build/linux/halo"

[ -x "$halo" ] || { echo "build Halo first: tools/setup_halo.py"; exit 1; }
[ -d "$maps" ] || { echo "no maps at $maps (set HALO_MAPS)"; exit 1; }
mkdir -p "$data" "$root/build/halo-saves" "$log"
ln -sfn "$(cd "$maps" && pwd)" "$data/maps"
map=${CHIEFRIM_MAP:-d20}
printf 'map_name levels\\%s\\%s\n' "$map" "$map" > "$data/init.txt"
: > "$data/debug.txt"

if [ "$visible" = "--visible" ]; then
	audio=${SDL_AUDIO_DRIVER:-}
	wrap=""
else
	audio=dummy
	command -v gamescope > /dev/null || { echo "install gamescope, or use --visible"; exit 1; }
	wrap="gamescope --backend headless -W 1280 -H 720 --"
fi

mkdir -p "$root/build/collision-dumps"
python3 "$root/tools/prune_dumps.py" || true
(
	CHIEFRIM=1 CHIEFRIM_DUMP_DIR="$root/build/collision-dumps" \
	HALO_DATA_ROOT="$data" HALO_SAVE_ROOT="$root/build/halo-saves" \
	HALO_UPDATE_AUTO=false HALO_NET_ONLINE=false \
	HALO_HIDDEN_WINDOW=false HALO_EXIT_AFTER="$seconds" \
	HALO_SCREENSHOT_DIR="${HALO_SCREENSHOT_DIR:-}" HALO_SCREENSHOT_EVERY="${HALO_SCREENSHOT_EVERY:-0}" \
	SDL_AUDIO_DRIVER=$audio \
	timeout $((seconds + 30)) $wrap "$halo" > "$log/halo.out" 2>&1
	echo "halo exit $?" >> "$log/halo.out"
) &
sleep 1
timeout $((seconds + 20)) "$root/tools/fake_skyrim.py" --seconds $((seconds + 10)) ${FAKE_SKYRIM_ARGS:-} > "$log/fake_skyrim.out" 2>&1 || true
wait

echo "== fake_skyrim"
cat "$log/fake_skyrim.out"
echo "== halo debug.txt (chiefrim and errors)"
grep -E "chiefrim|EXCEPTION|halt|assert" "$data/debug.txt" || true
tail -1 "$log/halo.out"
