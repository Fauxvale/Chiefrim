/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Handoff.h"
#include "Settings.h"

namespace chiefrim::Handoff
{
	namespace
	{
		// Nothing held the player this long: Chief has him again.
		constexpr ULONGLONG kReleaseMs = 300;

		bool              enabled = true;  // [Handoff] bEnabled
		std::atomic<bool> active{ false };
		const char*       reason = nullptr;  // what holds the player, for the log
		ULONGLONG         heldAt = 0;

		// What of Skyrim's holds the player now; nullptr: nothing.
		const char* Holder(RE::PlayerCharacter* a_player)
		{
			if (const auto* state = a_player->AsActorState()) {
				using Sit = RE::SIT_SLEEP_STATE;
				switch (state->GetSitSleepState()) {
				case Sit::kNormal:
					break;
				case Sit::kWantToSleep:
				case Sit::kWaitingForSleepAnim:
				case Sit::kIsSleeping:
				case Sit::kWantToWake:
					return "a bed";
				default:
					return a_player->IsOnMount() ? "a mount" : "furniture";
				}
				if (state->IsSwimming()) {
					return "swimming";
				}
			}
			if (a_player->IsOnMount()) {
				return "a mount";
			}
			// werewolf and vampire lord forms are races no one can pick
			if (const auto* race = a_player->GetRace(); race && race->data.flags.none(RE::RACE_DATA::Flag::kPlayable)) {
				return "a beast form";
			}
			if (a_player->IsInKillMove()) {
				return "a kill move";
			}
			const auto& flags = a_player->GetPlayerRuntimeData().playerFlags;
			if (flags.aiControlledToPos || flags.aiControlledFromPos || flags.aiControlledPackage) {
				return "a script (AI-driven)";
			}
			if (const auto* map = RE::ControlMap::GetSingleton(); map && !map->IsMovementControlsEnabled()) {
				return "a script (movement off)";
			}
			return nullptr;
		}
	}

	void Install()
	{
		enabled = Settings::ReadBool(L"Handoff", L"bEnabled", true);
		logger::info("hand-off to Skyrim's animations, scenes and swimming {}", enabled ? "on" : "off");
	}

	bool Update(RE::PlayerCharacter* a_player)
	{
		if (!enabled) {
			return false;
		}
		const auto  now = ::GetTickCount64();
		const char* holder = Holder(a_player);
		if (holder) {
			heldAt = now;
			if (!active.exchange(true)) {
				logger::info("hand-off: Skyrim has the player ({})", holder);
			} else if (holder != reason) {
				logger::info("hand-off: now {}", holder);
			}
			reason = holder;
		} else if (active && now - heldAt >= kReleaseMs) {
			active = false;
			reason = nullptr;
			logger::info("hand-off: Chief has the player again");
		}
		return active;
	}

	bool Active()
	{
		return active;
	}

	void Reset()
	{
		if (active.exchange(false)) {
			logger::info("hand-off: ended with the link");
		}
		reason = nullptr;
	}
}
