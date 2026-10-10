/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "CoSave.h"
#include "Caches.h"
#include "Link.h"
#include "Settings.h"

namespace chiefrim::CoSave
{
	namespace
	{
		constexpr std::uint32_t kUniqueId = 'CHRF';
		constexpr std::uint32_t kKitRecord = 'KIT ';
		constexpr std::uint32_t kKitVersion = 1;  // cr_chief_state of protocol 14

		enum class Pending
		{
			kNothing,
			kKit,              // the latest kit, to Halo
			kStartingLoadout,  // a save with none: Halo's starting loadout
		};

		std::mutex                    lock;
		std::optional<cr_chief_state> latest;  // the kit the next save keeps
		Pending                       pending = Pending::kNothing;
		std::uint32_t                 generation = 0;  // the last restore sent
		std::uint32_t                 expected = 0;    // reports from before it are stale

		// [Loadout]: what Chief starts with, without a kit of his own. Weapons by
		// tag path or its last part, with their own rounds; none: the host map's.
		cr_chief_state StartingKit()
		{
			cr_chief_state kit{};
			kit.current_weapon = 0;
			kit.current_grenade = 0;
			kit.body = kit.shield = kit.flashlight = 1.0f;
			wchar_t value[512]{};
			::GetPrivateProfileStringW(L"Loadout", L"sWeapons", L"assault rifle, pistol", value, 512, Settings::IniPath().c_str());
			std::size_t count = 0;
			std::wstringstream list(value);
			for (std::wstring name; std::getline(list, name, L',') && count < CR_CHIEF_WEAPONS;) {
				const auto first = name.find_first_not_of(L" \t\"");
				const auto last = name.find_last_not_of(L" \t\"");
				if (first == std::wstring::npos) {
					continue;
				}
				auto& weapon = kit.weapons[count++];
				for (std::size_t i = first; i <= last && i - first < CR_WEAPON_TAG_LENGTH - 1; ++i) {
					weapon.tag[i - first] = static_cast<char>(name[i] < 128 ? name[i] : '?');
				}
				weapon.rounds_total[0] = weapon.rounds_total[1] = -1;  // the weapon's own
				weapon.rounds_loaded[0] = weapon.rounds_loaded[1] = -1;
			}
			if (count == 0) {
				kit.flags = CR_CHIEF_STARTING_LOADOUT;  // the host map's
			}
			const auto grenades = [](const wchar_t* a_key, int a_default) {
				return static_cast<std::uint8_t>(std::clamp<int>(::GetPrivateProfileIntW(L"Loadout", a_key, a_default, Settings::IniPath().c_str()), 0, 99));
			};
			kit.grenades[0] = grenades(L"iFragGrenades", 4);
			kit.grenades[1] = grenades(L"iPlasmaGrenades", 0);
			if (!kit.grenades[0] && kit.grenades[1]) {
				kit.current_grenade = 1;
			}
			return kit;
		}

		void LogKit(const char* a_what, const cr_chief_state& a_kit)
		{
			std::string weapons;
			for (const auto& weapon : a_kit.weapons) {
				if (weapon.tag[0]) {
					const std::string_view tag(weapon.tag, ::strnlen(weapon.tag, CR_WEAPON_TAG_LENGTH));
					weapons += weapons.empty() ? "" : ", ";
					weapons += tag.substr(tag.find_last_of('\\') + 1);
					if (weapon.rounds_total[0] >= 0) {
						weapons += std::format(" {}/{}", weapon.rounds_loaded[0], weapon.rounds_total[0]);
					}
				}
			}
			logger::info("co-save: {}: {}; grenades {}/{}; body {:.2f}, shields {:.2f}", a_what,
				weapons.empty() ? "no weapons" : weapons, a_kit.grenades[0], a_kit.grenades[1], a_kit.body, a_kit.shield);
		}

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			Caches::Save(a_intfc);  // this plugin's one co-save: the caches' record too
			std::scoped_lock guard(lock);
			if (!latest) {
				logger::info("co-save: no kit of Chief's yet; the save has none");
				return;
			}
			if (!a_intfc->WriteRecord(kKitRecord, kKitVersion, *latest)) {
				logger::error("co-save: couldn't write Chief's kit");
				return;
			}
			LogKit("saved", *latest);
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::scoped_lock guard(lock);
			std::uint32_t type, version, length;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == Caches::kRecord) {
					Caches::Load(a_intfc, version, length);
					continue;
				}
				if (type != kKitRecord) {
					continue;
				}
				cr_chief_state kit{};
				if (version != kKitVersion || length != sizeof(kit) || a_intfc->ReadRecordData(kit) != sizeof(kit)) {
					logger::warn("co-save: Chief's kit is version {}, {} bytes; this reads version {}, {} bytes: the starting loadout",
						version, length, kKitVersion, sizeof(kit));
					continue;
				}
				latest = kit;
				pending = Pending::kKit;
				LogKit("loaded", kit);
			}
		}

		// Before a load or a new game: until a kit loads, the starting loadout.
		void OnRevert(SKSE::SerializationInterface*)
		{
			Caches::Revert();
			std::scoped_lock guard(lock);
			latest.reset();
			pending = Pending::kStartingLoadout;
		}
	}

	void Install()
	{
		auto* serialization = SKSE::GetSerializationInterface();
		serialization->SetUniqueID(kUniqueId);
		serialization->SetSaveCallback(OnSave);
		serialization->SetLoadCallback(OnLoad);
		serialization->SetRevertCallback(OnRevert);
	}

	void OnChiefState(const cr_msg_chief_state& a_message)
	{
		std::scoped_lock guard(lock);
		if (pending != Pending::kNothing || a_message.state.generation != expected) {
			return;  // Chief's from before the kit Halo is getting
		}
		latest = a_message.state;
	}

	void OnLinked()
	{
		std::scoped_lock guard(lock);
		expected = 0;  // its own Chief
		pending = latest ? Pending::kKit : Pending::kStartingLoadout;  // never the host map's own Chief
	}

	void Update()
	{
		std::scoped_lock guard(lock);
		if (pending == Pending::kNothing) {
			return;
		}
		cr_msg_chief_state message{};
		const bool starting = pending != Pending::kKit || !latest;
		message.state = starting ? StartingKit() : *latest;
		message.state.generation = generation + 1;
		if (!Link::Get().PushRaw(CR_MSG_CHIEF_RESTORE, &message, sizeof(message))) {
			return;  // the ring is full: next frame
		}
		generation = expected = message.state.generation;
		if (message.state.flags & CR_CHIEF_STARTING_LOADOUT) {
			logger::info("co-save: no kit: Chief gets the host map's starting loadout ([Loadout] sWeapons is empty)");
		} else {
			LogKit(starting ? "no kit: the starting loadout ([Loadout]) to Halo" : "to Halo", message.state);
		}
		pending = Pending::kNothing;
	}
}
