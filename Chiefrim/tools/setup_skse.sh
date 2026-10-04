#!/bin/sh
# Builds the SKSE plugin on Linux (docs/DESIGN.md §2): clang-cl and lld-link
# against Microsoft's CRT and Windows SDK, downloaded by xwin.
#
# The first run fetches the tools into .tools/ (git-ignored): xwin, and CMake
# in a Python venv. Fetching the CRT and SDK means accepting Microsoft's
# license terms for them; this script only does that with --accept-license.
#
# Usage: tools/setup_skse.sh [--accept-license]
# Result: build/skse/Chiefrim.dll
#
# CMake's file(GLOB) reads [ and ] as a pattern, and the project lives under
# "Master Chief In Skyrim Project [ Chiefrim ]". So CMake runs from a link
# without brackets (~/.cache/chiefrim/root -> Chiefrim/), and CMake itself is
# installed under ~/.cache/chiefrim/venv for the same reason.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tools="$root/.tools"
cache="${XDG_CACHE_HOME:-$HOME/.cache}/chiefrim"
mkdir -p "$cache"
ln -sfn "$root" "$cache/root"
xwin_version=0.10.0
xwin_name="xwin-$xwin_version-x86_64-unknown-linux-musl"
mkdir -p "$tools"

if [ ! -d "$tools/xwin/crt" ]; then
	if [ "${1:-}" != "--accept-license" ]; then
		echo "The MSVC CRT and Windows SDK are missing. Fetching them means accepting"
		echo "Microsoft's license terms; rerun with --accept-license to do so."
		exit 1
	fi
	if [ ! -x "$tools/$xwin_name/xwin" ]; then
		url="https://github.com/Jake-Shadle/xwin/releases/download/$xwin_version/$xwin_name.tar.gz"
		curl -sSLo "$tools/$xwin_name.tar.gz" "$url"
		curl -sSLo "$tools/$xwin_name.tar.gz.sha256" "$url.sha256"
		expected=$(cut -c1-64 "$tools/$xwin_name.tar.gz.sha256")
		actual=$(sha256sum "$tools/$xwin_name.tar.gz" | cut -c1-64)
		[ "$expected" = "$actual" ] || { echo "xwin checksum mismatch"; exit 1; }
		tar -C "$tools" -xzf "$tools/$xwin_name.tar.gz"
	fi
	"$tools/$xwin_name/xwin" --accept-license --arch x86_64 --cache-dir "$tools/xwin-cache" \
		splat --output "$tools/xwin"
fi

cmake="$cache/venv/bin/cmake"
if [ ! -x "$cmake" ]; then
	python3 -m venv "$cache/venv"
	"$cache/venv/bin/pip" install -q cmake
fi

git -C "$root/.." submodule update --init Chiefrim/skse/extern/CommonLibSSE-NG

link="$cache/root"
"$cmake" -S "$link/skse" -B "$link/build/skse" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$link/skse/cmake/clang-cl-xwin.cmake" \
	-DXWIN_DIR="$link/.tools/xwin" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo
"$cmake" --build "$link/build/skse" --target Chiefrim
ls -l "$root/build/skse/Chiefrim.dll"
