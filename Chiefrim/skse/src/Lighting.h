/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Lighting
{
	// Skyrim's light around the player, for Halo's objects (docs §9): Chief's
	// arms and weapon, and the world layer's objects, lit like Skyrim lights
	// the player rather than by the host map's lightmap.

	// kDataLoaded: reads Chiefrim.ini ([Lighting]).
	void Install();

	// Each frame while linked: ~10 times a second, CR_MSG_LIGHTING.
	void Update(RE::PlayerCharacter* a_player);

	// A new link: send at once.
	void Reset();
}
