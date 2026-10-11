/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Loot.h"
#include "Settings.h"

namespace chiefrim::Loot
{
	namespace
	{
		constexpr std::string_view kPlugin = "ChiefrimMjolnir.esp";
		// tools/mjolnir/plugin.py's LOOT lists and the form lists of where
		// each goes, its chance Chiefrim.ini's ([Loot])
		struct Placement
		{
			RE::FormID     list;
			RE::FormID     containers;
			const wchar_t* key;
			float          chance;  // the ini's default
			const char*    what;    // for the log
		};
		constexpr Placement kPlacements[]{
			{ 0xA03, 0xA10, L"fChestChance", 0.12f, "treasure chest" },
			{ 0xA04, 0xA11, L"fBossChestChance", 0.4f, "boss chest" },
			{ 0xA06, 0xA12, L"fVendorWeaponChance", 0.25f, "merchant (a weapon)" },
			{ 0xA07, 0xA12, L"fVendorGrenadeChance", 0.35f, "merchant (grenades)" },
		};

		std::int8_t ChanceNone(float a_chance)
		{
			return static_cast<std::int8_t>(100 - std::lround(100.0f * std::clamp(a_chance, 0.0f, 1.0f)));
		}

		// The list into each of the form list's containers; how many
		std::size_t AddTo(RE::BGSListForm* a_containers, RE::TESLevItem* a_loot)
		{
			std::size_t count = 0;
			for (auto* form : a_containers->forms) {
				if (auto* container = form ? form->As<RE::TESObjectCONT>() : nullptr) {
					container->AddObjectToContainer(a_loot, 1, nullptr);
					++count;
				}
			}
			return count;
		}
	}

	void Install()
	{
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data || !data->LookupModByName(kPlugin)) {
			logger::info("loot: {} isn't loaded: no Halo weapons in Skyrim's loot", kPlugin);
			return;
		}
		if (!Settings::ReadBool(L"Loot", L"bEnabled", true)) {
			logger::info("loot: off (Chiefrim.ini [Loot] bEnabled=0)");
			return;
		}
		for (const auto& placement : kPlacements) {
			auto* loot = data->LookupForm<RE::TESLevItem>(placement.list, kPlugin);
			auto* containers = data->LookupForm<RE::BGSListForm>(placement.containers, kPlugin);
			if (!loot || !containers) {
				logger::warn("loot: {} has no list {:03X} or {:03X} (built before them?)", kPlugin, placement.list, placement.containers);
				continue;
			}
			const float chance = std::clamp(Settings::ReadFloat(L"Loot", placement.key, placement.chance), 0.0f, 1.0f);
			loot->chanceNone = ChanceNone(chance);
			const auto count = chance > 0.0f ? AddTo(containers, loot) : 0;
			logger::info("loot: Halo's in {} kinds of {} ({:.0f}%)", count, placement.what, 100.0f * chance);
		}
	}
}
