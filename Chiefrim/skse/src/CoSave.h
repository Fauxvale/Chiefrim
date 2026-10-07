/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim::CoSave
{
	// Chief's kit in the SKSE co-save (docs §11): his weapons, ammo,
	// grenades, vitality and flashlight. Halo reports it when it changes, and
	// the latest goes into each save, linked or not. Loading a save sends its
	// kit back to Halo, or the starting loadout for a save without one (a new
	// game, an older save); a new Halo (F10, F11, a crash) gets the latest.

	// SKSEPluginLoad: the serialization callbacks.
	void Install();

	// Link::Update, for Halo's reports.
	void OnChiefState(const cr_msg_chief_state& a_message);

	// A new Halo: it has its own Chief, until the latest kit reaches it.
	void OnLinked();

	// Each frame while linked, once the world has gone to Halo: a waiting
	// restore goes out.
	void Update();
}
