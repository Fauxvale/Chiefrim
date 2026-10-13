#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the Arsenal's plugin, build/arsenal/Data/ChiefrimArsenal.esp:
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

Halo's weapons as loot (docs §11.2): leveled lists (LVLI) of them by
weight, light, medium and heavy (as the caches had them), and grenades;
those a chest draws on: an ordinary one's (light, medium or grenades) and
a boss chest's (heavy or medium, now and then light or grenades); and a
merchant's (a weapon; grenades). And form lists (FLST) of what they
go in, from the user's own Skyrim.esm: its treasure chests (a container
Treas*, with the Thieves Guild's loot as a treasure chest has: not a
merchant's or a quest's), boss chests (with LItemSpecialLoot100) apart.
The plugin overrides none of Skyrim's records: skse/src/Loot.cpp adds the
lists to those chests, and to merchants' (a blacksmith's, a general
store's: with Skyrim's weapons for sale), as the game loads, at the chances
Chiefrim.ini gives them. (An enemy Chief fights falls with its Halo proxy's gun and grenades,
skse/src/Arsenal.cpp.)

Usage: tools/arsenal/plugin.py [--skyrim DATA_DIR] [--dir build/arsenal]
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

NAME = "ChiefrimArsenal.esp"
FIRST_ID = 0x800
# biped slots, as BOD2's and BODT's bits (slot 30 is bit 0)
SLOT = {slot: 1 << (slot - 30) for slot in range(30, 62)}
# per piece: its Daedric armor, its name, its NIFs (by sex; the first
# person's for the cuirass), its addon's slots, slots its armor adds, and its
# description (Halo: Combat Evolved's Mark V, not Halo 2's Mark VI)
PIECES = [
	("helmet", "ArmorDaedricHelmet", "MJOLNIR Mark V Helmet", "helmet", (30,), (),
		"The sealed helmet of MJOLNIR powered assault armor. Behind its gold-mirrored visor, the suit's "
		"heads-up display tracks shield strength, motion and ammunition."),
	("cuirass", "ArmorDaedricCuirass", "MJOLNIR Mark V Armor", "cuirass_1", (32, 34, 38), (40,),
		"MJOLNIR Mark V powered assault armor, made for the SPARTAN-IIs. A titanium-alloy shell over a "
		"layer of hydrostatic gel, guarded by a recharging energy shield."),
	("gauntlets", "ArmorDaedricGauntlets", "MJOLNIR Mark V Gauntlets", "gauntlets_1", (33, 34), (),
		"The armored gauntlets of MJOLNIR powered assault armor. The armor's powered undersuit lends "
		"the wearer's grip and blows many times a man's strength."),
	("boots", "ArmorDaedricBoots", "MJOLNIR Mark V Boots", "boots_1", (37, 38), (),
		"The armored boots of MJOLNIR powered assault armor. Half a ton of suit, and it moves as "
		"quickly as the SPARTAN inside it."),
]
WEAPON_TEMPLATE = "IronDagger"
# per weapon (weapons.WEAPONS's key): its name in Skyrim, its designation
# as Halo: Combat Evolved has it before its common name (not a later game's:
# the M6D, not the M6G; the M19 SSM, not the M41; the M90, not the M90A),
# and its description
WEAPON_NAMES = {
	"needler": ("Type-33 Needler",
		"Covenant Type-33 Guided Munitions Launcher. Fires crystalline shards that home in on their "
		"target, lodge in it and detonate together."),
	"assault_rifle": ("MA5B Assault Rifle",
		"MA5B Individual Combat Weapon System, by Misriah Armory. Fully automatic, sixty 7.62mm "
		"rounds to a magazine, with a round counter and compass on its receiver."),
	"pistol": ("M6D Pistol",
		"M6D Personal Defense Weapon System. A semi-automatic 12.7mm sidearm with a 2x smart-linked "
		"scope, firing semi-armor-piercing high-explosive rounds."),
	"shotgun": ("M90 Shotgun",
		"M90 Close Assault Weapon System. A pump-action shotgun firing 8-gauge magnum shells; at close "
		"quarters few weapons in the UNSC arsenal hit harder."),
	"sniper_rifle": ("SRS99C-S2 AM Sniper Rifle",
		"Sniper Rifle System 99C, Series 2, Anti-Materiel. Four 14.5mm armor-piercing rounds to a "
		"magazine, through a 2x and 10x scope with night vision."),
	"rocket_launcher": ("M19 SSM Rocket Launcher",
		"M19 surface-to-surface missile launcher. Two tubes of 102mm high-explosive rockets, made to "
		"stop armored vehicles."),
	"plasma_pistol": ("Type-25 Plasma Pistol",
		"Covenant Type-25 Directed Energy Pistol. Fires bolts of superheated plasma, or holds a charge "
		"for one overcharged bolt that tracks its target and strips energy shields."),
	"plasma_rifle": ("Type-25 Plasma Rifle",
		"Covenant Type-25 Directed Energy Rifle, the Elites' weapon. Fires rapid bolts of superheated "
		"plasma until it overheats and must vent."),
	"frag_grenade": ("M9 Frag Grenade",
		"M9 High-Explosive, Dual-Purpose fragmentation grenade. It bounces before it bursts, so it "
		"can be thrown around corners."),
	"plasma_grenade": ("Type-1 Plasma Grenade",
		"Covenant Type-1 Antipersonnel Grenade. A ball of plasma that clings to whatever it strikes, "
		"flesh or armor, and detonates a moment later."),
}
VENDOR_ITEM_WEAPON = 0x0008F958
# the dagger's that a weapon Chief holds has no use for: its texture
# hashes, its first-person model, its keywords (replaced)
WEAPON_LEFT_OUT = ("MODT", "KSIZ", "KWDA")
# the dagger's first-person model (a STAT)
FIRST_PERSON_TEMPLATE = "1stPersonIronDagger"
# the templates' models' texture hashes (optional; they'd be Daedric's)
TEXTURE_HASHES = ("MO2T", "MO3T", "MO4T", "MO5T")
# Halo's loot (skse/src/Loot.cpp has these form IDs): per leveled list, its
# form ID, editor ID, chance none (Chiefrim.ini's chances replace a
# container's), its flags (LVLF: 1, from all levels up to the player's; 2,
# each of a count drawn apart; a grenades' count is of one kind, so not)
# and its entries, a weapon's key or a list's form ID, or either with a
# count, as often as each is to come; each from level 1, one of them
EACH = 0x3
LOOT = [
	(0xA00, "ChiefrimLootLight", 0, EACH, ["needler", "assault_rifle", "pistol", "plasma_pistol"]),
	(0xA01, "ChiefrimLootMedium", 0, EACH, ["shotgun", "plasma_rifle"]),
	(0xA02, "ChiefrimLootHeavy", 0, EACH, ["sniper_rifle", "rocket_launcher"]),
	(0xA05, "ChiefrimLootGrenades", 0, 0x1, [("frag_grenade", 1), ("frag_grenade", 2), ("plasma_grenade", 1), ("plasma_grenade", 2)]),
	# a third each light, medium, grenades
	(0xA03, "ChiefrimLootChest", 88, EACH, [0xA00, 0xA00, 0xA01, 0xA01, 0xA05, 0xA05]),
	# a third heavy, a third medium, a sixth light, a sixth grenades
	(0xA04, "ChiefrimLootBossChest", 60, EACH, [0xA02, 0xA02, 0xA01, 0xA01, 0xA00, 0xA05]),
	# a merchant's (a blacksmith's, a general store's), as its stock comes
	# in: a weapon (light two times in three, else medium), and grenades
	(0xA06, "ChiefrimLootVendorWeapon", 75, EACH, [0xA00, 0xA00, 0xA01]),
	(0xA07, "ChiefrimLootVendorGrenades", 65, 0x1, [("frag_grenade", 2), ("frag_grenade", 4), ("plasma_grenade", 2), ("plasma_grenade", 4)]),
]
# the form lists: of ordinary treasure chests, boss chests, merchants'
# chests (a blacksmith's or a general store's: with its weapons)
CHESTS, BOSS_CHESTS, VENDORS = 0xA10, 0xA11, 0xA12
TREASURE_MARK = "TGLootProwlersProfit"
VENDOR_MARKS = ("LItemBlacksmithWeapon75", "LItemMiscVendorWeapon75")
BOSS_MARK = "LItemSpecialLoot100"


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
		full_name, description = WEAPON_NAMES[key]
		nif = Path(data_dir) / Path(weapons.MESHES.replace("\\", "/")) / f"{key}.nif"
		points = [v["position"] for b in Nif(nif).blocks if b["type"] == "BSTriShape" for v in b["vertices"]]
		low = [int(min(p[i] for p in points) // 1) for i in range(3)]
		high = [-int(-max(p[i] for p in points) // 1) for i in range(3)]
		weapon = Record("WEAP", 0x01000000 | form_id, subrecords=[(t, d) for t, d in template.subrecords if t not in WEAPON_LEFT_OUT])
		weapon.set("EDID", zstring(f"Chiefrim{weapons.editor_name(name)}"))
		weapon.set("OBND", struct.pack("<6h", *low, *high))
		weapon.set("FULL", zstring(full_name))
		model_path = zstring(f"{weapons.MESHES.split(chr(92), 1)[1]}\\{key}.nif")
		weapon.set("MODL", model_path)
		static = Record("STAT", 0x01000000 | form_id + weapons.INVENTORY_ID,
			subrecords=[(t, d) for t, d in static_template.subrecords if t != "MODT"])
		static.set("EDID", zstring(f"1stPersonChiefrim{weapons.editor_name(name)}"))
		static.set("OBND", struct.pack("<6h", *low, *high))
		static.set("MODL", model_path)
		statics.append(static)
		weapon.set("WNAM", struct.pack("<I", static.form_id))
		# the keywords go after the equip type and its impacts, as the dagger's did
		index = next(i for i, (t, _) in enumerate(weapon.subrecords) if t == "DESC")
		weapon.subrecords[index:index] = [("KSIZ", struct.pack("<I", 1)), ("KWDA", struct.pack("<I", VENDOR_ITEM_WEAPON))]
		weapon.set("DESC", zstring(description))
		weapon.set("DATA", struct.pack("<IfH", value, weight, 0))
		records.append(weapon)
	return records, statics


def loot_records(plugin):
	"""The loot's leveled lists, and the form lists of where it goes."""
	ours = {key: 0x01000000 | form_id for key, _, _, form_id, *_ in weapons.WEAPONS}
	lists = []
	for form_id, editor_id, chance_none, flags, entries in LOOT:
		record = Record("LVLI", 0x01000000 | form_id)
		record.set("EDID", zstring(editor_id))
		record.set("OBND", bytes(12))
		record.set("LVLD", bytes([chance_none]))
		record.set("LVLF", bytes([flags]))
		record.set("LLCT", bytes([len(entries)]))
		for entry in entries:
			entry, count = entry if isinstance(entry, tuple) else (entry, 1)
			target = ours[entry] if isinstance(entry, str) else 0x01000000 | entry
			record.subrecords.append(("LVLO", struct.pack("<HHIHH", 1, 0, target, count, 0)))
		lists.append(record)
	marks = {name: plugin.find("LVLI", name).form_id for name in (TREASURE_MARK, BOSS_MARK, *VENDOR_MARKS)}
	vendor_marks = {marks[name] for name in VENDOR_MARKS}
	chests, bosses, vendors = [], [], []
	for container in plugin.groups["CONT"]:
		held = {struct.unpack("<I", data[:4])[0] for kind, data in container.subrecords if kind == "CNTO"}
		editor_id = container.editor_id or ""
		if editor_id.startswith("Treas") and marks[TREASURE_MARK] in held:
			(bosses if marks[BOSS_MARK] in held else chests).append(container.form_id)
		elif "Merchant" in editor_id and held & vendor_marks:
			vendors.append(container.form_id)
	forms = []
	for form_id, editor_id, members in ((CHESTS, "ChiefrimLootChests", chests), (BOSS_CHESTS, "ChiefrimLootBossChests", bosses),
			(VENDORS, "ChiefrimLootVendors", vendors)):
		record = Record("FLST", 0x01000000 | form_id)
		record.set("EDID", zstring(editor_id))
		record.subrecords += [("LNAM", struct.pack("<I", member)) for member in members]
		forms.append(record)
	return lists, forms


def build(skyrim, data_dir):
	plugin = Plugin(Path(skyrim) / "Skyrim.esm", ["ARMO", "ARMA", "WEAP", "STAT", "LVLI", "CONT"])
	armors, addons = [], []
	next_id = FIRST_ID
	for piece, template, name, nif, addon_slots, extra_slots, description in PIECES:
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
		armor.set("DESC", zstring(description))
		armor.set("MOD2", model(None, f"{piece}_go"))
		body = bytearray(armor.get("BOD2"))
		current, = struct.unpack_from("<I", body)
		body[0:4] = struct.pack("<I", current | slots(extra_slots))
		armor.set("BOD2", bytes(body))
		armor.set("MODL", struct.pack("<I", new_addon_id))
		armors.append(armor)

	weapon_list, statics = weapon_records(plugin, data_dir)
	loot, form_lists = loot_records(plugin)
	ours = weapon_list + statics + loot + form_lists
	next_id = max([next_id] + [(r.form_id & 0xFFFFFF) + 1 for r in ours])
	esp = Plugin()
	esp.header = Record("TES4", 0, subrecords=[
		("HEDR", struct.pack("<fiI", 1.71, len(armors) + len(addons) + len(ours), next_id)),
		("CNAM", zstring("Chiefrim")),
		("SNAM", zstring("Chiefrim's Arsenal: Master Chief's Mjolnir armor and Halo's weapons, ported from Halo CE.")),
		("MAST", zstring("Skyrim.esm")),
		("DATA", struct.pack("<Q", 0)),
	])
	# in Skyrim's order of top groups
	esp.groups = {"ARMO": armors, "STAT": statics, "WEAP": weapon_list, "LVLI": loot, "FLST": form_lists, "ARMA": addons}
	return esp


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	parser.add_argument("--skyrim", default=str(DEFAULT_SKYRIM), help="Skyrim's Data directory")
	parser.add_argument("--dir", default=str(Path(__file__).resolve().parents[2] / "build" / "arsenal"))
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
	for loot in check.groups["LVLI"]:
		entries = [struct.unpack("<HHIHH", data) for kind, data in loot.subrecords if kind == "LVLO"]
		print(f"{loot.form_id & 0xFFFFFF:06X} {loot.editor_id}: {loot.get('LVLD')[0]}% none, "
			+ ", ".join(f"{e[2] & 0xFFFFFF:03X}" + (f" x{e[3]}" if e[3] > 1 else "") for e in entries))
	for form_list in check.groups["FLST"]:
		print(f"{form_list.form_id & 0xFFFFFF:06X} {form_list.editor_id}: {len(form_list.all('LNAM'))} forms")
	print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
	sys.exit(main())
