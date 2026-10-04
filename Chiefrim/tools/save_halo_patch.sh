#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the CHIEFRIM hooks in halo/.work back to halo/patches. Only changes to
# the game's own files go in the patch; source/chiefrim/ is ours and comes
# from halo/src (tools/setup_halo.py copies it).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
git -C "$root/halo/.work" diff -- . ':(exclude)source/chiefrim' \
	> "$root/halo/patches/0001-chiefrim-hooks.patch"
echo "wrote halo/patches/0001-chiefrim-hooks.patch ($(wc -l < "$root/halo/patches/0001-chiefrim-hooks.patch") lines)"
