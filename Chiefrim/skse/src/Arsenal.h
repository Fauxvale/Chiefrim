/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Arsenal
{
	// Halo's weapons as Skyrim items (docs §8.4, the loot rework): weapons
	// (WEAP) in ChiefrimMjolnir.esp (tools/mjolnir/weapons.py and plugin.py),
	// found, carried, stored and sold as Skyrim's are. Equipping one hands it
	// to Chief: Halo gives him the weapon (CR_MSG_GIVE_WEAPON, by its tag) and
	// the item leaves the player's inventory. While Chiefrim is off (unlinked)
	// it is unequipped again and kept.

	// kDataLoaded: finds the plugin's weapons and watches what the player
	// equips.
	void Install();
}
