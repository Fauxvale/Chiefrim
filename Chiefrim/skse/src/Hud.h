/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Hud
{
	// Skyrim's HUD while Chief plays (docs §9): Halo's HUD has the crosshair,
	// shields and health, so Skyrim's crosshair and its health, magicka and
	// stamina bars are hidden ([HUD] bHideCrosshair, bHideBars). The rest stays:
	// the compass, the sneak eye (Chief crouching is the player sneaking), the
	// activate prompt, the enemy's health bar, notifications.

	// kDataLoaded: reads Chiefrim.ini.
	void Install();

	// Each frame while linked: hidden (Skyrim's HUD shows them again whenever
	// it changes mode, so this keeps at it).
	void Update();

	// The link closed: Skyrim's own again.
	void Restore();
}
