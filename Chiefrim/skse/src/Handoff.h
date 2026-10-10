/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Handoff
{
	// Skyrim's own animations and scenes (docs §11): while the player sits,
	// sleeps, works at a crafting station, rides, swims (docs §6, deep
	// water), is a werewolf or vampire lord, is in a kill move, or a script
	// holds him (AI-driven, or his movement turned off), Skyrim has the
	// player: its controls, camera, arms and HUD bars, and Chief gets no input
	// and his weapon and HUD aren't drawn ([Handoff] bEnabled). Halo's world
	// layer stays. So too while the player is dead (Chief died in Halo, or
	// Skyrim killed him), through the reload's loading screen, until it has
	// faded in and he has control again ([Handoff] bDeath).

	// kDataLoaded: reads Chiefrim.ini.
	void Install();

	// Once a frame while linked, in gameplay: whether Skyrim has the player
	// now. It takes him at once, and gives him back when nothing has held
	// him for a moment (getting out of the water, or up, in steps).
	bool Update(RE::PlayerCharacter* a_player);

	// Skyrim has the player (any thread; false while unlinked).
	bool Active();

	// The link closed.
	void Reset();
}
