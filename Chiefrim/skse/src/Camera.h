/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim::Camera
{
	// CameraDriver (docs §6): Skyrim's first-person camera sees through
	// Chief's eyes: his eye position, his look direction, Halo's field of
	// view, with Skyrim's own first-person arms hidden.

	// kDataLoaded: hooks the first-person camera and PlayerCamera::Update.
	void Install();

	// Each frame while Chief drives the player: the view to show.
	void Drive(RE::PlayerCharacter* a_player, const cr_player_state& a_state);

	// Chief's unzoomed field of view for Halo ([Camera] fFieldOfView, degrees,
	// horizontal for 4:3 as Skyrim measures it; 0 = Halo's own).
	float FieldOfView();

	// The link closed: Skyrim's own camera, FOV and arms back.
	void Release(RE::PlayerCharacter* a_player);
}
