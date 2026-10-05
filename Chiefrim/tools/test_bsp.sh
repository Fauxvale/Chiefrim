#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds halo/test/bsp_harness (chiefrim_bsp.c alone, with the game's own
# compiler flags) and runs it on synthetic Skyrim-like collision of growing
# size: bumpy ground, rotated boxes like rocks, a ramp, a cliff and a wall.
# Needs a built halo/.work (tools/setup_halo.py). SELF_TEST=1 also runs Halo's
# own ray and sphere queries on every surface.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/halo/.work"
out="$root/build/test"
mkdir -p "$out"
cp "$root/halo/src/chiefrim_bsp.c" "$root/halo/src/chiefrim_bsp.h" "$work/source/chiefrim/"
cmd=$(cd "$work" && ninja -t commands build/linux/obj/source/chiefrim/chiefrim_bsp.o | tail -1)
flags=$(echo "$cmd" | sed -e 's/^clang //' -e 's/ -MMD -MF [^ ]*//' -e 's/ -c source.*$//')
cd "$work"
eval clang $flags -O2 -w -c "$root/halo/test/bsp_harness.c" -o "$out/bsp_harness.o"
eval clang $flags -O2 -w -c source/chiefrim/chiefrim_bsp.c -o "$out/chiefrim_bsp.o"
clang -m32 -O2 -c "$root/halo/test/bsp_stubs.c" -o "$out/bsp_stubs.o"
# Halo's own collision queries too, for SELF_TEST=1 (their debug-render
# neighbours in collision_bsp.o stay unresolved: never called here)
obj="$work/build/linux/obj/source"
clang --target=i686-linux-gnu -m32 -no-pie "$out/bsp_harness.o" "$out/chiefrim_bsp.o" "$out/bsp_stubs.o" \
	"$obj/physics/collision_bsp.o" "$obj/physics/bsp2d.o" "$obj/physics/bsp3d.o" "$obj/math/real_math.o" \
	-Wl,--unresolved-symbols=ignore-all -lm -o "$out/bsp_harness"
for size in "512 0" "1024 50" "2048 200" "3072 400" "3072 1500"; do
	timeout 120 "$out/bsp_harness" $size
done
