#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the Mjolnir armor's plugin, build/mjolnir/Data/ChiefrimMjolnir.esp:
an armor (ARMO) and its armor addon (ARMA) per piece, made from the user's
own Skyrim.esm's Daedric armor's (its races, keywords, sounds, stats: the
heaviest armor Skyrim has), with Chief's names, armor.py's NIFs and the
body parts they cover.

Each armor covers the biped slots of its Daedric one; the cuirass covers
slot 40 too, so a tail (Argonian, Khajiit) isn't drawn through it. Each
addon covers the slots its NIF draws (the cuirass's and gauntlets' and
boots' include the forearms or calves they carry, as Skyrim's do). The
helmet's NIF is one, not one per end of the weight slider.

The worn NIFs are the armor's world models too, for now: dropped, a piece
has no collision (nor does it lie flat).

Usage: tools/mjolnir/plugin.py [--skyrim DATA_DIR] [--dir build/mjolnir]
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armor import MESHES  # noqa: E402
from esp import Plugin, Record  # noqa: E402
from fit import DEFAULT_SKYRIM  # noqa: E402

NAME = "ChiefrimMjolnir.esp"
FIRST_ID = 0x800
# biped slots, as BOD2's and BODT's bits (slot 30 is bit 0)
SLOT = {slot: 1 << (slot - 30) for slot in range(30, 62)}
# per piece: its Daedric armor, its name, its NIFs (by sex; the first
# person's for the cuirass), its addon's slots, and slots its armor adds
PIECES = [
	("helmet", "ArmorDaedricHelmet", "Mjolnir Helmet", "helmet", (30,), ()),
	("cuirass", "ArmorDaedricCuirass", "Mjolnir Armor", "cuirass_1", (32, 34, 38), (40,)),
	("gauntlets", "ArmorDaedricGauntlets", "Mjolnir Gauntlets", "gauntlets_1", (33, 34), ()),
	("boots", "ArmorDaedricBoots", "Mjolnir Boots", "boots_1", (37, 38), ()),
]
DESCRIPTION = "MJOLNIR Mark V powered assault armor."
# the templates' models' texture hashes (optional; they'd be Daedric's)
TEXTURE_HASHES = ("MO2T", "MO3T", "MO4T", "MO5T")


def zstring(text):
	return text.encode("latin-1") + b"\0"


def model(folder, name):
	# Skyrim's model paths are from meshes\
	return zstring(f"{MESHES.split(chr(92), 1)[1]}\\{folder}\\{name}.nif")


def slots(numbers):
	return sum(SLOT[n] for n in numbers)


def build(skyrim):
	plugin = Plugin(Path(skyrim) / "Skyrim.esm", ["ARMO", "ARMA"])
	armors, addons = [], []
	next_id = FIRST_ID
	for piece, template, name, nif, addon_slots, extra_slots in PIECES:
		armor_template = plugin.find("ARMO", template)
		addon_id, = struct.unpack("<I", armor_template.get("MODL"))
		addon_template = plugin.by_id("ARMA", addon_id)
		# this plugin's own forms are its masters' count (1) in the top byte
		armor_id, new_addon_id = 0x01000000 | next_id, 0x01000000 | next_id + 1
		next_id += 2

		addon = Record("ARMA", new_addon_id, subrecords=[(t, d) for t, d in addon_template.subrecords if t not in TEXTURE_HASHES])
		addon.set("EDID", zstring(f"ChiefrimMjolnir{piece.title()}AA"))
		# Skyrim.esm's addons are form version 39 or 40, with BODT (slots,
		# flags, armor type); at form version 44 they're BOD2 (slots, armor
		# type), as the Creation Kit saves them. The flags are 0 (none that
		# BOD2 drops).
		body = addon.get("BODT")
		if body[4] != 0:
			raise SystemExit(f"{addon_template.editor_id}: BODT flags {body[4]:#x}")
		index = next(i for i, (t, _) in enumerate(addon.subrecords) if t == "BODT")
		addon.subrecords[index] = ("BOD2", struct.pack("<I", slots(addon_slots)) + body[8:12])
		data = bytearray(addon.get("DNAM"))
		if nif == "helmet":
			data[2:4] = b"\0\0"  # one NIF, no weight slider
		addon.set("DNAM", bytes(data))
		addon.set("MOD2", model("male", nif))
		addon.set("MOD3", model("f" if piece == "cuirass" else "male", nif))
		if addon.get("MOD4") is not None:
			addon.set("MOD4", model("male", f"1stperson{nif}"))
			addon.set("MOD5", model("f", f"1stperson{nif}"))
		addons.append(addon)

		armor = Record("ARMO", armor_id, subrecords=[(t, d) for t, d in armor_template.subrecords if t not in TEXTURE_HASHES])
		armor.set("EDID", zstring(f"ChiefrimMjolnir{piece.title()}"))
		# Skyrim.esm's are localized (string IDs); this plugin's aren't
		armor.set("FULL", zstring(name))
		armor.set("DESC", zstring(DESCRIPTION))
		armor.set("MOD2", model("male", nif))
		body = bytearray(armor.get("BOD2"))
		current, = struct.unpack_from("<I", body)
		body[0:4] = struct.pack("<I", current | slots(extra_slots))
		armor.set("BOD2", bytes(body))
		armor.set("MODL", struct.pack("<I", new_addon_id))
		armors.append(armor)

	esp = Plugin()
	esp.header = Record("TES4", 0, subrecords=[
		("HEDR", struct.pack("<fiI", 1.71, len(armors) + len(addons), next_id)),
		("CNAM", zstring("Chiefrim")),
		("SNAM", zstring("Master Chief's Mjolnir armor, ported from Halo CE by Chiefrim's tools.")),
		("MAST", zstring("Skyrim.esm")),
		("DATA", struct.pack("<Q", 0)),
	])
	esp.groups = {"ARMO": armors, "ARMA": addons}
	return esp


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("--skyrim", default=str(DEFAULT_SKYRIM), help="Skyrim's Data directory")
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "mjolnir"))
	args = parser.parse_args()
	path = Path(args.dir) / "Data" / NAME
	build(args.skyrim).write(path)
	check = Plugin(path)
	for armor in check.groups["ARMO"]:
		addon = check.by_id("ARMA", struct.unpack("<I", armor.get("MODL"))[0])
		print(f"{armor.form_id & 0xFFFFFF:06X} {armor.editor_id}: {armor.get('FULL')[:-1].decode()}, "
			f"{addon.editor_id} -> {addon.get('MOD2')[:-1].decode()}")
	print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
	sys.exit(main())
