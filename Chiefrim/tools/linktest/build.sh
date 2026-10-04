#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the link test: native_peer (Linux i386, like Halo) and wine_peer.exe
# (Windows x64, like the SKSE plugin). The Windows side needs no SDK: it links
# against an import library made from kernel32.def.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$root/build/linktest"
mkdir -p "$out"

clang -m32 -std=c11 -O2 -Wall -Wextra -Werror -I"$root/protocol" \
	"$here/native_peer.c" -o "$out/native_peer"

llvm-dlltool -m i386:x86-64 -d "$here/kernel32.def" -l "$out/kernel32.lib"
clang --target=x86_64-pc-windows-msvc -std=c11 -O2 -Wall -Werror \
	-ffreestanding -fno-stack-protector -fno-builtin -mno-stack-arg-probe \
	-I"$root/protocol" -c "$here/wine_peer.c" -o "$out/wine_peer.obj"
lld-link /nologo /nodefaultlib /subsystem:console /entry:mainCRTStartup \
	"$out/wine_peer.obj" "$out/kernel32.lib" /out:"$out/wine_peer.exe"

echo "built $out/native_peer and $out/wine_peer.exe"
