#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the CHIEFRIM hooks in halo/.work back to halo/patches. Only changes to
# the game's own files go in the patch: source/chiefrim/ is ours (halo/src),
# and the files halo/overrides replaces whole stay out of it, so the patch
# never carries their original text (docs/LICENSING.md: xiso.c).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
excludes=":(exclude)source/chiefrim"
for file in $(cd "$root/halo/overrides" && find . -type f | sed 's|^\./||'); do
	excludes="$excludes :(exclude)$file"
done
# shellcheck disable=SC2086
git -C "$root/halo/.work" diff -- . $excludes > "$root/halo/patches/0001-chiefrim-hooks.patch"
echo "wrote halo/patches/0001-chiefrim-hooks.patch ($(wc -l < "$root/halo/patches/0001-chiefrim-hooks.patch") lines)"
