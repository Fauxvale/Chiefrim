/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim::Caches
{
	// Weapon caches (docs §8.4): Halo weapons lying in Skyrim's bandit,
	// Forsworn and military camps and its forts, by its chests (and weapon
	// racks). The first visit decides whether a site has any ([Caches]
	// fSiteChance); the first time each of its cells loads, which of its
	// chests get one and what (light weapons; forts also medium ones; a
	// fort's boss chest a heavy one). All of it goes into the co-save at once,
	// so caches stay where they are, and a taken one stays taken. Halo lays
	// the untaken ones near the player down in its world, again after each
	// new world (a door, a load, a new Halo).

	// kDataLoaded: reads Chiefrim.ini.
	void Install();

	// CoSave's callbacks: the record ('CACH') and a revert.
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void Revert();
	inline constexpr std::uint32_t kRecord = 'CACH';

	// A new world went to Halo (this generation): its loose objects are gone.
	void OnWorld(std::uint32_t a_generation);

	// Each frame while linked, in gameplay: new cells' caches chosen, the
	// ones near the player sent to Halo.
	void Update(RE::PlayerCharacter* a_player);

	// Link::Update: Chief picked one up.
	void OnTaken(const cr_msg_cache_taken& a_message);
}
