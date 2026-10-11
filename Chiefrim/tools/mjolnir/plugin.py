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

Each armor's world model (dropped, and in the inventory) is ground.py's
<piece>_go.nif.

Halo's weapons (weapons.py's WEAPONS) are weapons (WEAP) made from Skyrim's
Iron Dagger, at the form IDs weapons.py gives them (skse/src/Arsenal.cpp
finds them by these): their names, weapons.py's NIFs as their world models,
their value and weight, no damage (an NPC never prefers one), and of the
dagger's keywords only VendorItemWeapon (a blacksmith buys one). A weapon's
first-person model (WNAM, a static) is what the inventory shows it as, so
each has one (made from the dagger's, its own NIF again); equipping one
hands it to Chief, so it is never held.

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
from nif import Nif  # noqa: E402
import weapons  # noqa: E402

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
WEAPON_TEMPLATE = "IronDagger"
WEAPON_DESCRIPTION = "A weapon of Halo's. Equip it to hand it to Master Chief."
VENDOR_ITEM_WEAPON = 0x0008F958
# the dagger's that a weapon Chief holds has no use for: its texture
# hashes, its first-person model, its keywords (replaced)
WEAPON_LEFT_OUT = ("MODT", "KSIZ", "KWDA")
# the dagger's first-person model (a STAT)
FIRST_PERSON_TEMPLATE = "1stPersonIronDagger"
# the templates' models' texture hashes (optional; they'd be Daedric's)
TEXTURE_HASHES = ("MO2T", "MO3T", "MO4T", "MO5T")


def zstring(text):
	return text.encode("latin-1") + b"\0"


def model(folder, name):
	# Skyrim's model paths are from meshes\
	return zstring("\\".join([MESHES.split(chr(92), 1)[1], *([folder] if folder else []), f"{name}.nif"]))


def slots(numbers):
	return sum(SLOT[n] for n in numbers)


def weapon_records(plugin, data_dir):
	"""The Halo weapons' WEAP records, from the dagger's, and their
	first-person (inventory) models' STATs."""
	template = plugin.find("WEAP", WEAPON_TEMPLATE)
	static_template = plugin.find("STAT", FIRST_PERSON_TEMPLATE)
	records, statics = [], []
	for key, _, name, form_id, value, weight in weapons.WEAPONS:
		nif = Path(data_dir) / Path(weapons.MESHES.replace("\\", "/")) / f"{key}.nif"
		points = [v["position"] for b in Nif(nif).blocks if b["type"] == "BSTriShape" for v in b["vertices"]]
		low = [int(min(p[i] for p in points) // 1) for i in range(3)]
		high = [-int(-max(p[i] for p in points) // 1) for i in range(3)]
		weapon = Record("WEAP", 0x01000000 | form_id, subrecords=[(t, d) for t, d in template.subrecords if t not in WEAPON_LEFT_OUT])
		weapon.set("EDID", zstring(f"Chiefrim{key.title()}"))
		weapon.set("OBND", struct.pack("<6h", *low, *high))
		weapon.set("FULL", zstring(name))
		model_path = zstring(f"{weapons.MESHES.split(chr(92), 1)[1]}\\{key}.nif")
		weapon.set("MODL", model_path)
		static = Record("STAT", 0x01000000 | form_id + weapons.INVENTORY_ID,
			subrecords=[(t, d) for t, d in static_template.subrecords if t != "MODT"])
		static.set("EDID", zstring(f"1stPersonChiefrim{key.title()}"))
		static.set("OBND", struct.pack("<6h", *low, *high))
		static.set("MODL", model_path)
		statics.append(static)
		weapon.set("WNAM", struct.pack("<I", static.form_id))
		# the keywords go after the equip type and its impacts, as the dagger's did
		index = next(i for i, (t, _) in enumerate(weapon.subrecords) if t == "DESC")
		weapon.subrecords[index:index] = [("KSIZ", struct.pack("<I", 1)), ("KWDA", struct.pack("<I", VENDOR_ITEM_WEAPON))]
		weapon.set("DESC", zstring(WEAPON_DESCRIPTION))
		weapon.set("DATA", struct.pack("<IfH", value, weight, 0))
		records.append(weapon)
	return records, statics


def build(skyrim, data_dir):
	plugin = Plugin(Path(skyrim) / "Skyrim.esm", ["ARMO", "ARMA", "WEAP", "STAT"])
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
		armor.set("MOD2", model(None, f"{piece}_go"))
		body = bytearray(armor.get("BOD2"))
		current, = struct.unpack_from("<I", body)
		body[0:4] = struct.pack("<I", current | slots(extra_slots))
		armor.set("BOD2", bytes(body))
		armor.set("MODL", struct.pack("<I", new_addon_id))
		armors.append(armor)

	weapon_list, statics = weapon_records(plugin, data_dir)
	next_id = max([next_id] + [(r.form_id & 0xFFFFFF) + 1 for r in weapon_list + statics])
	esp = Plugin()
	esp.header = Record("TES4", 0, subrecords=[
		("HEDR", struct.pack("<fiI", 1.71, len(armors) + len(addons) + len(weapon_list) + len(statics), next_id)),
		("CNAM", zstring("Chiefrim")),
		("SNAM", zstring("Master Chief's Mjolnir armor and Halo's weapons, ported from Halo CE by Chiefrim's tools.")),
		("MAST", zstring("Skyrim.esm")),
		("DATA", struct.pack("<Q", 0)),
	])
	# in Skyrim's order of top groups
	esp.groups = {"ARMO": armors, "STAT": statics, "WEAP": weapon_list, "ARMA": addons}
	return esp


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("--skyrim", default=str(DEFAULT_SKYRIM), help="Skyrim's Data directory")
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "mjolnir"))
	args = parser.parse_args()
	path = Path(args.dir) / "Data" / NAME
	build(args.skyrim, path.parent).write(path)
	check = Plugin(path)
	for armor in check.groups["ARMO"]:
		addon = check.by_id("ARMA", struct.unpack("<I", armor.get("MODL"))[0])
		print(f"{armor.form_id & 0xFFFFFF:06X} {armor.editor_id}: {armor.get('FULL')[:-1].decode()}"
			f" ({armor.get('MOD2')[:-1].decode()}), {addon.editor_id} -> {addon.get('MOD2')[:-1].decode()}")
	for weapon in check.groups["WEAP"]:
		value, weight, _ = struct.unpack("<IfH", weapon.get("DATA"))
		print(f"{weapon.form_id & 0xFFFFFF:06X} {weapon.editor_id}: {weapon.get('FULL')[:-1].decode()}"
			f" ({weapon.get('MODL')[:-1].decode()}), {value} gold, {weight:g} weight,"
			f" inventory model {struct.unpack('<I', weapon.get('WNAM'))[0] & 0xFFFFFF:06X}")
	print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
	sys.exit(main())
