/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim::Combat
{
	// Chief against Skyrim's people (docs §8). Each frame while linked: the
	// actors near the player go to Halo (which keeps a hittable stand-in for
	// each), and the player's lost health is refunded and sent to Halo, where
	// it hurts Chief, shields first. Halo's hits on the stand-ins come back
	// and go through Skyrim's own hit processing. Restore-health potions and
	// food heal Chief.

	// kDataLoaded: hit events, Skyrim's hit pipeline, the debug weapon key.
	void Install();

	// Each frame while linked, in gameplay.
	void PerFrame(RE::PlayerCharacter* a_player, float a_delta);

	// Link::Update, for Halo's messages.
	void OnHitActor(const cr_msg_hit_actor& a_hit);
	void OnChiefDied();
	void OnExplosion(const cr_msg_explosion& a_explosion);  // Skyrim's loose objects fly

	// The link closed or a new world: the player is Skyrim's own again.
	void Release(RE::PlayerCharacter* a_player);
}
