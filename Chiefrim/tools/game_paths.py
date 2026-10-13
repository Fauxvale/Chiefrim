#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the user's own game files are (docs/INSTALLATION-LINUX.md): Halo's maps/
and Skyrim's Data/. The build and launch scripts all ask this, so none of
them holds one machine's paths.

Halo's maps/, the first of these that exists:
1. HALO_MAPS;
2. halo/.work/build/linux/maps, where the install guide has the user put
   them once the upstream OpenCE release has extracted them from the Xbox
   disc image (tools/setup_halo.py keeps build/);
3. HaloProjects/Halo-CE-Universal/maps beside the repo (an OpenCE of one's
   own, from before Chiefrim built it).
None: the second, where they'd be extracted. The host map in it is
CHIEFRIM_MAP's, d20 (Keyes) by default (docs/DESIGN.md §6.1).

Skyrim's Data/, the first of these that exists:
1. SKYRIM_DATA;
2. Skyrim Special Edition (Steam app 489830) in any of Steam's libraries,
   as libraryfolders.vdf lists them (native Steam's or the Flatpak's);
3. Steam's default library.
None: the last.

Reads nothing but Steam's library lists; needs only Python's own modules.

Usage: tools/game_paths.py maps|map|skyrim
  prints the maps/ folder, the host map's file, or Skyrim's Data/
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKYRIM_APP = "489830"
STEAM_ROOTS = (
	Path.home() / ".local/share/Steam",
	Path.home() / ".steam/steam",
	Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
)


def halo_maps():
	candidates = [ROOT / "halo/.work/build/linux/maps", ROOT.parent / "HaloProjects/Halo-CE-Universal/maps"]
	if os.environ.get("HALO_MAPS"):
		candidates.insert(0, Path(os.environ["HALO_MAPS"]).expanduser())
	return next((path for path in candidates if path.is_dir()), candidates[0])


def host_map():
	return halo_maps() / f"{os.environ.get('CHIEFRIM_MAP') or 'd20'}.map"


def steam_libraries():
	"""Each of Steam's libraries, the default one first."""
	libraries = []
	for root in STEAM_ROOTS:
		listing = root / "steamapps/libraryfolders.vdf"
		paths = [root]
		if listing.is_file():
			text = listing.read_text(errors="replace")
			paths += [Path(found.replace("\\\\", "\\")) for found in re.findall(r'"path"\s+"([^"]+)"', text)]
		for path in paths:
			if path.is_dir() and path.resolve() not in [library.resolve() for library in libraries]:
				libraries.append(path)
	return libraries


def skyrim_data():
	if os.environ.get("SKYRIM_DATA"):
		return Path(os.environ["SKYRIM_DATA"]).expanduser()
	for library in steam_libraries():
		manifest = library / f"steamapps/appmanifest_{SKYRIM_APP}.acf"
		if manifest.is_file():
			found = re.search(r'"installdir"\s+"([^"]+)"', manifest.read_text(errors="replace"))
			data = library / "steamapps/common" / (found.group(1) if found else "Skyrim Special Edition") / "Data"
			if data.is_dir():
				return data
	return STEAM_ROOTS[0] / "steamapps/common/Skyrim Special Edition/Data"


def main():
	what = {"maps": halo_maps, "map": host_map, "skyrim": skyrim_data}
	if len(sys.argv) != 2 or sys.argv[1] not in what:
		print(__doc__.strip().rsplit("\n\n", 1)[-1], file=sys.stderr)
		return 2
	print(what[sys.argv[1]]())
	return 0


if __name__ == "__main__":
	sys.exit(main())
