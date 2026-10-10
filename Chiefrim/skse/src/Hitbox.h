/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim::Hitbox
{
	// An actor's hit shapes for its proxy in Halo (docs §8.1, protocol 18):
	// the bodies of its skeleton that Skyrim's own arrows hit (capsules,
	// spheres; a box or hull as the capsule along its longest side), as
	// they stand now, in Skyrim world units. Without any (no ragdoll in the
	// world), a capsule from its bounds and height, and a person's head.
	// Writes at most a_max; returns how many.
	std::uint32_t Collect(RE::Actor* a_actor, cr_hitbox* a_out, std::uint32_t a_max);

	// After a frame's actors: forget those no longer listed.
	void EndFrame();
}
