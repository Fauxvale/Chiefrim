/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Loot
{
	// Halo's weapons as Skyrim's loot (docs §11.2, the loot rework): the
	// leveled lists ChiefrimArsenal.esp has of them (tools/arsenal/
	// plugin.py) go into the treasure chests its form lists name, an ordinary
	// one's and a boss chest's, and merchants' (a weapon; grenades), at
	// Chiefrim.ini's chances ([Loot]). (What an
	// enemy Chief fights falls with is its proxy's kit, by Arsenal's
	// CR_RETURNED_LOOT.) No record of Skyrim's is overridden: the
	// chests' contents are added to as the game loads, so other mods' are
	// kept. A chest a save has filled already keeps what it has until it
	// resets.

	// kDataLoaded
	void Install();
}
