/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Handoff.h"
#include "Settings.h"

namespace chiefrim::Handoff
{
	namespace
	{
		// Nothing held the player this long: Chief has him again.
		constexpr ULONGLONG kReleaseMs = 300;

		// After the reload's loading screen, Skyrim fades the world in: at
		// most this long it may still be Skyrim's if its fader is up.
		constexpr ULONGLONG kFaderMs = 10000;

		bool              enabled = true;      // [Handoff] bEnabled
		bool              onDeath = true;      // [Handoff] bDeath
		ULONGLONG         fadeInMs = 2000;     // [Handoff] fDeathFadeInSeconds
		std::atomic<bool> active{ false };
		const char*       reason = nullptr;  // what holds the player, for the log
		ULONGLONG         heldAt = 0;

		// The player died (Chief in Halo, which kills him, or Skyrim's own):
		// Skyrim has him through his death, the reload and its fade-in.
		bool                   dying = false;
		ULONGLONG              diedAt = 0;
		std::atomic<ULONGLONG> loadedAt{ 0 };  // a loading screen closed

		class LoadingSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static LoadingSink* Get()
			{
				static LoadingSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && !a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					loadedAt = ::GetTickCount64();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// The player's death, and the reload after it, until he's back in
		// control; nullptr: not dying.
		const char* Death(RE::PlayerCharacter* a_player, ULONGLONG a_now)
		{
			if (!onDeath) {
				return nullptr;
			}
			if (a_player->IsDead()) {
				if (!dying) {
					dying = true;
					diedAt = a_now;
				}
				return "death";
			}
			if (!dying) {
				return nullptr;
			}
			const auto loaded = loadedAt.load();
			if (loaded > diedAt) {  // the reload came: until it has faded in
				auto*       ui = RE::UI::GetSingleton();
				const bool  fader = ui && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME);
				if (a_now - loaded < fadeInMs || (fader && a_now - loaded < kFaderMs)) {
					return "death (the reload fading in)";
				}
			}
			dying = false;  // alive again, loaded or brought back
			return nullptr;
		}

		// What of Skyrim's holds the player now; nullptr: nothing.
		const char* Holder(RE::PlayerCharacter* a_player, ULONGLONG a_now)
		{
			if (const char* death = Death(a_player, a_now)) {
				return death;
			}
			if (!enabled) {
				return nullptr;
			}
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
		onDeath = Settings::ReadBool(L"Handoff", L"bDeath", true);
		fadeInMs = static_cast<ULONGLONG>(std::clamp(Settings::ReadFloat(L"Handoff", L"fDeathFadeInSeconds", 2.0f), 0.0f, 30.0f) * 1000.0f);
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(LoadingSink::Get());
		}
		logger::info("hand-off to Skyrim's animations, scenes and swimming {}; through the player's death and reload {}",
			enabled ? "on" : "off", onDeath ? std::format("on ({:.1f} s of fade-in)", fadeInMs / 1000.0) : std::string("off"));
	}

	bool Update(RE::PlayerCharacter* a_player)
	{
		if (!enabled && !onDeath) {
			return false;
		}
		const auto  now = ::GetTickCount64();
		const char* holder = Holder(a_player, now);
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
		dying = false;
	}
}
