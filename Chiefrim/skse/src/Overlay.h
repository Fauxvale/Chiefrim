/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Overlay
{
	// The Compositor (docs §9): Halo's layers over Skyrim's picture. Halo
	// draws Chief's arms and weapon and its HUD on transparent black, at
	// Skyrim's screen size, into the link's frame slots; at each Present this
	// tells Halo the screen's size, uploads Halo's newest frame and draws it
	// over Skyrim's picture, while the game is in gameplay (not in menus,
	// loading screens or the console): under Skyrim's own HUD and menus,
	// drawn as the first of them draws ([Overlay] bUnderSkyrimMenus), or
	// at Present, over everything.

	// kDataLoaded: hooks the swap chain's Present.
	void Install();

	// Skyrim's picture, as the weapon's look meters it (before Halo's layers):
	// its key (the log average of its brightness) and the colour of its light
	// (its mean, weighted to its unsaturated parts), at most a few frames old. False: not metered (yet, or for 2 s).
	bool Surroundings(float& a_key, RE::NiColor& a_mean);
}
