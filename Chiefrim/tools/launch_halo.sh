#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs Halo in Chiefrim mode for a session with Skyrim (docs/DESIGN.md §11),
# and keeps it running: when Halo exits or crashes it's started again (a few
# seconds later each time it keeps crashing), and Skyrim can restart, stop and
# start it through /dev/shm/chiefrim_control (the SKSE plugin's hotkeys, and
# its watchdog when Halo stops responding).
#
# Usage:
#   tools/launch_halo.sh [--bot]
#       Start it before or after Skyrim; the plugin links when both are up.
#       Ctrl+C stops it (and Halo).
#   tools/launch_halo.sh --steam %command%
#       As Skyrim's launch options in Steam (with the script's full path):
#       Halo starts with Skyrim and stops when Skyrim exits.
#   --bot  a scripted player walks, turns and jumps by itself (HALO_TEST_INPUT)
#
# Halo's data root is build/halo-data, a link to the user's maps/ (HALO_MAPS),
# so the user's own Halo install and saves are never touched. Its log is
# build/halo-data/debug.txt, its terminal output build/halo-data/halo.out
# (the last run's; the one before is halo.out.1). CHIEFRIM_HALO_WRAPPER runs
# Halo through a command (tests: "gamescope --backend headless --").
#
# CHIEFRIM_HALO_GPU picks the GPU Halo renders on, in a laptop with two:
#   igpu (the default): the integrated one, the system's own choice; Skyrim
#       keeps the discrete GPU to itself.
#   dgpu: NVIDIA's discrete GPU, through PRIME render offload. Halo is 32-bit
#       and NVIDIA's 32-bit EGL can't open a Wayland display, so Halo runs
#       through XWayland's GLX (needs lib32-nvidia-utils). halo.out's
#       "OpenGL ..." line names the GPU it got.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
maps=${HALO_MAPS:-"$root/../HaloProjects/Halo-CE-Universal/maps"}
data="$root/build/halo-data"
halo="$root/halo/.work/build/linux/halo"
control=/dev/shm/chiefrim_control

[ -x "$halo" ] || { echo "build Halo first: tools/setup_halo.py"; exit 1; }
[ -d "$maps" ] || { echo "no maps at $maps (set HALO_MAPS)"; exit 1; }
mkdir -p "$data" "$root/build/halo-saves" "$root/build/collision-dumps"
ln -sfn "$(cd "$maps" && pwd)" "$data/maps"
printf 'map_name levels\\b30\\b30\n' > "$data/init.txt"

bot=""
steam=""
case "${1:-}" in
--bot) bot="bot:1"; shift ;;
--steam) steam=1; shift ;;
esac

say() { echo "chiefrim: $*" >&2; }

# Halo in the background; its output to halo.out (and this terminal)
gpu_env=""
case "${CHIEFRIM_HALO_GPU:-igpu}" in
igpu) ;;
dgpu) gpu_env="SDL_VIDEO_DRIVER=x11 __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia" ;;
*) echo "CHIEFRIM_HALO_GPU: igpu or dgpu, not ${CHIEFRIM_HALO_GPU}"; exit 1 ;;
esac

halo_pid=""
start_halo() {
	[ -f "$data/halo.out" ] && mv -f "$data/halo.out" "$data/halo.out.1"
	env CHIEFRIM=1 CHIEFRIM_DUMP_DIR="$root/build/collision-dumps" \
		HALO_DATA_ROOT="$data" HALO_SAVE_ROOT="$root/build/halo-saves" \
		HALO_UPDATE_AUTO=false HALO_NET_ONLINE=false HALO_FULLSCREEN=0 \
		HALO_TEST_INPUT="$bot" $gpu_env \
		${CHIEFRIM_HALO_WRAPPER:-} "$halo" > "$data/halo.out" 2>&1 &
	halo_pid=$!
	if [ -z "$steam" ]; then
		tail -f --pid="$halo_pid" "$data/halo.out" 2>/dev/null &
	fi
	say "Halo started (pid $halo_pid, ${CHIEFRIM_HALO_GPU:-igpu}); log $data/debug.txt"
}
stop_halo() {
	[ -n "$halo_pid" ] || return 0
	kill "$halo_pid" 2>/dev/null || true
	i=0
	while kill -0 "$halo_pid" 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
	kill -9 "$halo_pid" 2>/dev/null || true
	wait "$halo_pid" 2>/dev/null || true
	halo_pid=""
}

game_pid=""
cleanup() {
	trap - INT TERM EXIT
	stop_halo
	rm -f "$control"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

if [ -n "$steam" ]; then
	# Skyrim itself (Steam's %command%), alongside
	"$@" &
	game_pid=$!
fi

: > "$control"
last_command=""
wanted=run
crashes=0          # in a row, each soon after its start
started_at=0
while :; do
	if [ -n "$game_pid" ] && ! kill -0 "$game_pid" 2>/dev/null; then
		say "Skyrim exited; stopping Halo"
		wait "$game_pid" 2>/dev/null || true
		exit 0
	fi

	# Skyrim's requests: "<count> restart|stop|start"
	command=$(cat "$control" 2>/dev/null || true)
	if [ -n "$command" ] && [ "$command" != "$last_command" ]; then
		last_command=$command
		case "${command#* }" in
		restart) say "Skyrim asks for a restart"; stop_halo; wanted=run; crashes=0 ;;
		stop) say "Skyrim turned Chiefrim off"; stop_halo; wanted=stop ;;
		start) say "Skyrim turned Chiefrim on"; wanted=run; crashes=0 ;;
		esac
	fi

	if [ "$wanted" = run ] && { [ -z "$halo_pid" ] || ! kill -0 "$halo_pid" 2>/dev/null; }; then
		if [ -n "$halo_pid" ]; then
			status=0
			wait "$halo_pid" 2>/dev/null || status=$?
			halo_pid=""
			now=$(date +%s)
			if [ $((now - started_at)) -lt 30 ]; then crashes=$((crashes + 1)); else crashes=1; fi
			delay=$((crashes * crashes))
			[ $delay -gt 30 ] && delay=30
			say "Halo exited (status $status); starting it again in $delay s"
			grep -E "EXCEPTION|halt" "$data/debug.txt" 2>/dev/null | tail -1 >&2 || true
			sleep "$delay"
		fi
		start_halo
		started_at=$(date +%s)
	fi
	sleep 0.5
done
