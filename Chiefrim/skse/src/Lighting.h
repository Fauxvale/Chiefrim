/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

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

	// Chief's flashlight on Skyrim's world (protocol 17): Halo's light as it
	// shines now (CR_MSG_FLASHLIGHT), and each frame in gameplay a light of
	// Skyrim's where the beam along the player's view lands.
	void OnFlashlight(const cr_msg_flashlight& a_message);
	void UpdateFlashlight(RE::PlayerCharacter* a_player);
	// Unlinked (or Chiefrim off): no light, until Halo says again.
	void RemoveFlashlight();
}
