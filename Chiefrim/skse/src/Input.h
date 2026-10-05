/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim::Input
{
	// Chief's controls from Skyrim's own (docs §7). An input sink reads
	// Skyrim's user events, so the player's remaps in Skyrim's Controls menu
	// apply to Chief as well; only Chiefrim's two hotkeys are raw keys.

	// kDataLoaded: reads Chiefrim.ini and registers the sinks.
	void Install();

	// Once a frame while linked: Skyrim's handlers for Chief's actions off,
	// and the actions published to Halo.
	void Publish(RE::PlayerCharacter* a_player);

	// A menu opened: Chief gets no input until Publish runs again in
	// gameplay (PlayerCharacter::Update doesn't run while Skyrim is paused).
	void PublishNeutral();

	// The link opened (a new session: look totals restart) or closed
	// (Skyrim's handlers back on).
	void OnLinked();
	void OnUnlinked();
}
