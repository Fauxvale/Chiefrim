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
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tools="$root/.tools"
case "$root" in
*[][*?]*)
	# CMake's file(GLOB) reads these as a pattern: it then finds neither its
	# own compiler-detection files nor the sources.
	echo "The path $root contains [ ] * or ?, which CMake can't build in. Move or rename the folder."
	exit 1 ;;
esac
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

cmake="$tools/venv/bin/cmake"
if [ ! -x "$cmake" ]; then
	python3 -m venv "$tools/venv"
	"$tools/venv/bin/pip" install -q cmake
fi

git -C "$root/.." submodule update --init Chiefrim/skse/extern/CommonLibSSE-NG

"$cmake" -S "$root/skse" -B "$root/build/skse" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$root/skse/cmake/clang-cl-xwin.cmake" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo
"$cmake" --build "$root/build/skse" --target Chiefrim
ls -l "$root/build/skse/Chiefrim.dll"
