/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Overlay
{
	// The Compositor (docs §9): Halo's layers over Skyrim's picture. Halo
	// draws Chief's arms and weapon and its HUD on transparent black, at
	// Skyrim's screen size, into the link's frame slots; at each Present this
	// tells Halo the screen's size, uploads Halo's newest frame and draws it
	// over everything, while the game is in gameplay (not in menus, loading
	// screens or the console).

	// kDataLoaded: hooks the swap chain's Present.
	void Install();
}
