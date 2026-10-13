/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim::Arsenal
{
	// Halo's weapons as Skyrim items (docs §11.1, the loot rework): weapons
	// (WEAP) in ChiefrimArsenal.esp (tools/arsenal/weapons.py and plugin.py),
	// found, carried, stored and sold as Skyrim's are, each with its rounds in
	// its name ("Needler (20/60)": loaded, spare; "Plasma Rifle (40%)"; none:
	// a new one's). Equipping one hands it to Chief with them
	// (CR_MSG_GIVE_WEAPON, CR_GIVE_ITEM) and it leaves the inventory; while
	// Chiefrim is off (unlinked) it is unequipped again and kept. What Chief
	// lets go of, or can't take, comes back as an item (CR_MSG_WEAPON_RETURNED).

	// kDataLoaded: finds the plugin's weapons and watches what the player
	// equips.
	void Install();

	// A new Halo: the weapons there are items for (CR_MSG_ARSENAL), which
	// alone it returns.
	void OnLinked();

	// Link::Update: a weapon back from Chief.
	void OnReturned(const cr_msg_weapon_returned& a_message);
}
