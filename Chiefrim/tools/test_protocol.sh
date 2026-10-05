#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds and runs protocol/test_protocol.c as i386 and x86-64, and checks the
# header's layout pins against both Windows targets.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/test"
mkdir -p "$out"

for bits in 32 64; do
	clang -m$bits -std=c11 -O2 -Wall -Wextra -Werror -I"$root/protocol" \
		"$root/protocol/test_protocol.c" -lm -o "$out/test_protocol_$bits"
	"$out/test_protocol_$bits"
done

# Halo itself builds with -malign-double (8-byte doubles in i386 structs).
printf '#include "chiefrim_protocol.h"\n' |
	clang -m32 -malign-double -std=c11 -Wall -Werror -fsyntax-only -I"$root/protocol" -x c -
echo "ok: layout pins hold for i386 with -malign-double (Halo's build)"

for target in x86_64-pc-windows-msvc i686-pc-windows-msvc; do
	printf '#include "chiefrim_protocol.h"\n' |
		clang++ --target=$target -ffreestanding -std=c++23 -Wall -Werror \
			-fsyntax-only -I"$root/protocol" -x c++ -
	echo "ok: layout pins hold for $target"
done
