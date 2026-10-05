/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Collision
{
	// WorldExporter (docs §5.2): streams Skyrim's Havok collision near the
	// player to Halo, as triangles in Skyrim world units per cube region
	// (CR_REGION_UNITS). Halo builds its collision BSP from them.

	// The world changed (or a new Halo): forget what was sent, tell Halo to.
	void Reset();

	// Once a frame while linked, in gameplay: harvests and sends a few
	// regions around the player, within a small time budget.
	void Update(RE::PlayerCharacter* a_player);
}
