/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Caches.h"
#include "Link.h"
#include "Settings.h"

#include <random>
#include <unordered_map>
#include <unordered_set>

namespace chiefrim::Caches
{
	namespace
	{
		constexpr std::uint32_t kVersion = 1;

		// Skyrim.esm: the kinds of site, and the boss's chest
		constexpr RE::FormID kBanditCamp = 0x000130DF;
		constexpr RE::FormID kForswornCamp = 0x000130EE;
		constexpr RE::FormID kMilitaryCamp = 0x000130E8;
		constexpr RE::FormID kMilitaryFort = 0x000130E7;
		constexpr RE::FormID kBossContainer = 0x000130F8;

		enum class Kind
		{
			kNone,
			kCamp,
			kFort,
		};

		enum class Tier
		{
			kLight,
			kMedium,
			kHeavy,
		};

		struct Config
		{
			bool                     enabled = true;     // [Caches] bEnabled
			float                    siteChance = 0.5f;  // fSiteChance: a camp or fort has any
			float                    chestChance = 0.35f; // fChestChance: each chest (and rack) of one that has
			float                    mediumChance = 0.3f; // fMediumChance: a fort's cache is a medium weapon
			float                    heavyChance = 0.6f;  // fHeavyChance: a fort's boss chest has a heavy one
			int                      maxPerSite = 3;      // iMaxPerSite
			float                    sendRadius = 2500.0f; // fSendRadius: Skyrim units; Halo's collision reaches ~2500
			std::vector<std::string> weapons[3];          // sLightWeapons, sMediumWeapons, sHeavyWeapons
		} config;

		struct Site
		{
			bool chosen = false;  // has caches
			int  count = 0;       // caches it has
		};

		struct Cache
		{
			RE::FormID  cell = 0;
			RE::FormID  site = 0;
			std::string weapon;
			bool        taken = false;
		};

		std::mutex                                 lock;
		std::uint32_t                              seed = 0;  // this playthrough's (0: none yet)
		std::unordered_map<RE::FormID, Site>       sites;     // by location
		std::unordered_set<RE::FormID>             cells;     // whose caches are chosen
		std::unordered_map<RE::FormID, Cache>      caches;    // by the chest (its anchor)
		std::uint32_t                              generation = 0;  // Halo's world
		std::unordered_map<RE::FormID, ULONGLONG>  sentAt;    // in this world
		ULONGLONG                                  next = 0;

		std::vector<std::string> ReadList(const wchar_t* a_key, const wchar_t* a_default)
		{
			wchar_t value[512]{};
			::GetPrivateProfileStringW(L"Caches", a_key, a_default, value, 512, Settings::IniPath().c_str());
			std::vector<std::string> list;
			std::wstringstream       stream(value);
			for (std::wstring name; std::getline(stream, name, L',');) {
				const auto first = name.find_first_not_of(L" \t\"");
				const auto last = name.find_last_not_of(L" \t\"");
				if (first == std::wstring::npos) {
					continue;
				}
				std::string text;
				for (auto i = first; i <= last; ++i) {
					text += static_cast<char>(name[i] < 128 ? name[i] : '?');
				}
				list.push_back(text);
			}
			return list;
		}

		bool Has(RE::BGSLocation* a_location, RE::FormID a_keyword)
		{
			auto* keyword = RE::TESForm::LookupByID<RE::BGSKeyword>(a_keyword);
			return keyword && a_location->HasKeyword(keyword);
		}

		// The camp or fort the location is (or is in)
		Kind SiteOf(RE::BGSLocation* a_location, RE::BGSLocation*& a_site)
		{
			for (auto* location = a_location; location; location = location->parentLoc) {
				if (Has(location, kMilitaryFort)) {
					a_site = location;
					return Kind::kFort;
				}
				if (Has(location, kBanditCamp) || Has(location, kForswornCamp) || Has(location, kMilitaryCamp)) {
					a_site = location;
					return Kind::kCamp;
				}
			}
			return Kind::kNone;
		}

		std::string ModelOf(RE::TESBoundObject* a_base)
		{
			auto* model = a_base ? a_base->As<RE::TESModel>() : nullptr;
			std::string path = model && model->GetModel() ? model->GetModel() : "";
			std::ranges::transform(path, path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return path;
		}

		bool IsBossChest(RE::TESObjectREFR* a_ref)
		{
			auto* extra = a_ref->extraList.GetByType<RE::ExtraLocationRefType>();
			return extra && extra->locRefType && extra->locRefType->GetFormID() == kBossContainer;
		}

		// A chest, or a weapon rack: where a weapon could have been left
		bool IsAnchor(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || (a_ref->GetFormID() >> 24) == 0xFF) {
				return false;  // (0xFF: made at run time, gone with its cell)
			}
			auto* base = a_ref->GetBaseObject();
			const auto model = ModelOf(base);
			if (model.find("weaponrack") != std::string::npos) {
				return true;
			}
			return base && base->Is(RE::FormType::Container) && model.find("chest") != std::string::npos;
		}

		std::mt19937 Rng(RE::FormID a_a, RE::FormID a_b)
		{
			std::seed_seq sequence{ seed, a_a, a_b };
			return std::mt19937(sequence);
		}

		float Roll(std::mt19937& a_rng) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(a_rng); }

		// The site's first visit: has it any caches?
		Site& Decide(RE::BGSLocation* a_site, Kind a_kind)
		{
			auto [it, added] = sites.try_emplace(a_site->GetFormID());
			if (added) {
				auto rng = Rng(a_site->GetFormID(), 0);
				it->second.chosen = Roll(rng) < config.siteChance;
				logger::info("caches: {} {:08X} ({}): {}", a_kind == Kind::kFort ? "fort" : "camp", a_site->GetFormID(), a_site->GetName(),
					it->second.chosen ? "has caches" : "none");
			}
			return it->second;
		}

		// The cell's first load in a site with caches: which chests get one, and what
		void Choose(RE::TESObjectCELL* a_cell, RE::BGSLocation* a_site, Kind a_kind, Site& a_record)
		{
			std::vector<RE::TESObjectREFR*> anchors;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (IsAnchor(a_ref)) {
					anchors.push_back(a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			cells.insert(a_cell->GetFormID());
			auto rng = Rng(a_site->GetFormID(), a_cell->GetFormID());
			std::ranges::sort(anchors, {}, [](auto* a_ref) { return a_ref->GetFormID(); });
			std::ranges::shuffle(anchors, rng);
			// a boss chest first: the best place for one
			std::ranges::stable_partition(anchors, [](auto* a_ref) { return IsBossChest(a_ref); });
			const int before = a_record.count;
			const auto bosses = std::ranges::count_if(anchors, [](auto* a_ref) { return IsBossChest(a_ref); });
			for (auto* anchor : anchors) {
				if (a_record.count >= config.maxPerSite) {
					break;
				}
				const bool boss = IsBossChest(anchor);
				Tier       tier = Tier::kLight;
				if (boss && a_kind == Kind::kFort) {
					if (Roll(rng) >= config.heavyChance) {
						continue;
					}
					tier = Tier::kHeavy;
				} else {
					// a fort keeps a place for its boss's chest (deeper in, a later cell)
					if (a_kind == Kind::kFort && a_record.count >= config.maxPerSite - 1) {
						continue;
					}
					if (Roll(rng) >= config.chestChance) {
						continue;
					}
					tier = a_kind == Kind::kFort && Roll(rng) < config.mediumChance ? Tier::kMedium : Tier::kLight;
				}
				const auto& list = config.weapons[static_cast<int>(tier)];
				if (list.empty()) {
					continue;
				}
				const auto& weapon = list[std::uniform_int_distribution<std::size_t>(0, list.size() - 1)(rng)];
				caches[anchor->GetFormID()] = { a_cell->GetFormID(), a_site->GetFormID(), weapon, false };
				++a_record.count;
				logger::info("caches: {} by {:08X}{} in {:08X} ({})", weapon, anchor->GetFormID(), boss ? " (the boss's chest)" : "",
					a_cell->GetFormID(), a_site->GetName());
			}
			logger::info("caches: cell {:08X} of {}: {} chests and racks ({} the boss's), {} caches chosen; {} of {} in the site", a_cell->GetFormID(),
				a_site->GetName(), anchors.size(), bosses, a_record.count - before, a_record.count, config.maxPerSite);
		}

		// Where the weapon lies: on a chest's lid, before a rack
		bool Place(RE::TESObjectREFR* a_anchor, cr_vec3& a_position, float& a_yaw)
		{
			auto* model = a_anchor->Get3D();
			if (!model) {
				return false;
			}
			const auto& bound = model->worldBound;
			const float angle = a_anchor->GetAngleZ();
			RE::NiPoint3 at = bound.center;
			if (ModelOf(a_anchor->GetBaseObject()).find("weaponrack") != std::string::npos) {
				at = a_anchor->GetPosition() + RE::NiPoint3{ std::sin(angle), std::cos(angle), 0.0f } * 30.0f;
				at.z = bound.center.z;  // falls to the floor before it
			} else {
				at.z = bound.center.z + bound.radius * 0.6f;  // falls onto the lid
			}
			a_position = { at.x, at.y, at.z };
			a_yaw = std::numbers::pi_v<float> / 2.0f - angle;  // Skyrim's angle is clockwise from +y
			return true;
		}

		void Send(RE::PlayerCharacter* a_player)
		{
			if (!generation) {
				return;
			}
			const auto now = ::GetTickCount64();
			const auto here = a_player->GetPosition();
			for (const auto& [id, cache] : caches) {
				if (cache.taken) {
					continue;
				}
				auto* anchor = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!anchor || anchor->GetPosition().GetDistance(here) > config.sendRadius) {
					continue;
				}
				// again now and then: Halo gives one up that found nothing to lie on
				if (auto it = sentAt.find(id); it != sentAt.end() && now - it->second < 5000) {
					continue;
				}
				cr_msg_cache_place message{};
				if (!Place(anchor, message.position, message.yaw)) {
					continue;
				}
				message.id = id;
				message.generation = generation;
				std::strncpy(message.weapon, cache.weapon.c_str(), CR_WEAPON_TAG_LENGTH - 1);
				if (!Link::Get().PushRaw(CR_MSG_CACHE_PLACE, &message, sizeof(message))) {
					return;  // the ring is full: next time
				}
				sentAt[id] = now;
			}
		}

		template <class T>
		bool Read(SKSE::SerializationInterface* a_intfc, T& a_value)
		{
			return a_intfc->ReadRecordData(a_value) == sizeof(T);
		}
	}

	void Install()
	{
		config.enabled = Settings::ReadBool(L"Caches", L"bEnabled", true);
		config.siteChance = Settings::ReadFloat(L"Caches", L"fSiteChance", 0.5f);
		config.chestChance = Settings::ReadFloat(L"Caches", L"fChestChance", 0.35f);
		config.mediumChance = Settings::ReadFloat(L"Caches", L"fMediumChance", 0.3f);
		config.heavyChance = Settings::ReadFloat(L"Caches", L"fHeavyChance", 0.6f);
		config.maxPerSite = static_cast<int>(Settings::ReadFloat(L"Caches", L"iMaxPerSite", 3.0f));
		config.sendRadius = Settings::ReadFloat(L"Caches", L"fSendRadius", 2500.0f);
		config.weapons[0] = ReadList(L"sLightWeapons", L"pistol, plasma pistol, needler, assault rifle");
		config.weapons[1] = ReadList(L"sMediumWeapons", L"shotgun, plasma rifle");
		config.weapons[2] = ReadList(L"sHeavyWeapons", L"sniper rifle, rocket launcher, flamethrower, fuel rod");
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		std::scoped_lock guard(lock);
		if (!seed) {
			return;  // nothing chosen yet
		}
		if (!a_intfc->OpenRecord(kRecord, kVersion)) {
			logger::error("co-save: couldn't write the weapon caches");
			return;
		}
		a_intfc->WriteRecordData(seed);
		a_intfc->WriteRecordData(static_cast<std::uint32_t>(sites.size()));
		for (const auto& [id, site] : sites) {
			a_intfc->WriteRecordData(id);
			a_intfc->WriteRecordData(static_cast<std::uint8_t>(site.chosen));
			a_intfc->WriteRecordData(static_cast<std::int32_t>(site.count));
		}
		a_intfc->WriteRecordData(static_cast<std::uint32_t>(cells.size()));
		for (const auto id : cells) {
			a_intfc->WriteRecordData(id);
		}
		a_intfc->WriteRecordData(static_cast<std::uint32_t>(caches.size()));
		int taken = 0;
		for (const auto& [id, cache] : caches) {
			char weapon[CR_WEAPON_TAG_LENGTH]{};
			std::strncpy(weapon, cache.weapon.c_str(), CR_WEAPON_TAG_LENGTH - 1);
			a_intfc->WriteRecordData(id);
			a_intfc->WriteRecordData(cache.cell);
			a_intfc->WriteRecordData(cache.site);
			a_intfc->WriteRecordData(weapon);
			a_intfc->WriteRecordData(static_cast<std::uint8_t>(cache.taken));
			taken += cache.taken;
		}
		logger::info("co-save: weapon caches saved: {} sites seen, {} cells, {} caches ({} taken)", sites.size(), cells.size(), caches.size(), taken);
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t)
	{
		std::scoped_lock guard(lock);
		if (a_version != kVersion) {
			logger::warn("co-save: weapon caches are version {}; this reads {}: chosen again", a_version, kVersion);
			return;
		}
		const auto resolve = [&](RE::FormID& a_id) { return a_intfc->ResolveFormID(a_id, a_id); };
		std::uint32_t count = 0;
		if (!Read(a_intfc, seed) || !Read(a_intfc, count)) {
			return;
		}
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::FormID   id = 0;
			std::uint8_t chosen = 0;
			std::int32_t caches = 0;
			if (!Read(a_intfc, id) || !Read(a_intfc, chosen) || !Read(a_intfc, caches)) {
				return;
			}
			if (resolve(id)) {
				sites[id] = { chosen != 0, caches };
			}
		}
		if (!Read(a_intfc, count)) {
			return;
		}
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::FormID id = 0;
			if (!Read(a_intfc, id)) {
				return;
			}
			if (resolve(id)) {
				cells.insert(id);
			}
		}
		if (!Read(a_intfc, count)) {
			return;
		}
		int taken = 0;
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::FormID   id = 0;
			Cache        cache;
			char         weapon[CR_WEAPON_TAG_LENGTH]{};
			std::uint8_t isTaken = 0;
			if (!Read(a_intfc, id) || !Read(a_intfc, cache.cell) || !Read(a_intfc, cache.site) || !Read(a_intfc, weapon) || !Read(a_intfc, isTaken)) {
				return;
			}
			weapon[CR_WEAPON_TAG_LENGTH - 1] = 0;
			cache.weapon = weapon;
			cache.taken = isTaken != 0;
			if (resolve(id) && resolve(cache.cell) && resolve(cache.site)) {  // a plugin gone: its caches with it
				caches[id] = cache;
				taken += cache.taken;
			}
		}
		logger::info("co-save: weapon caches loaded: {} sites seen, {} cells, {} caches ({} taken)", sites.size(), cells.size(), caches.size(), taken);
	}

	void Revert()
	{
		std::scoped_lock guard(lock);
		seed = 0;
		sites.clear();
		cells.clear();
		caches.clear();
		sentAt.clear();
	}

	void OnWorld(std::uint32_t a_generation)
	{
		std::scoped_lock guard(lock);
		generation = a_generation;
		sentAt.clear();  // Halo's loose objects are gone: send them again
	}

	void Update(RE::PlayerCharacter* a_player)
	{
		const auto now = ::GetTickCount64();
		if (!config.enabled || now < next) {
			return;
		}
		next = now + 500;
		std::scoped_lock guard(lock);
		auto* cell = a_player->GetParentCell();
		RE::BGSLocation* site = nullptr;
		const auto kind = SiteOf(a_player->GetCurrentLocation(), site);
		if (kind != Kind::kNone && cell && !cells.contains(cell->GetFormID())) {
			if (!seed) {
				seed = std::random_device{}() | 1u;  // this playthrough's
			}
			auto& record = Decide(site, kind);
			if (record.chosen) {
				Choose(cell, site, kind, record);
			} else {
				cells.insert(cell->GetFormID());
			}
		}
		Send(a_player);
	}

	void OnTaken(const cr_msg_cache_taken& a_message)
	{
		std::scoped_lock guard(lock);
		if (auto it = caches.find(a_message.id); it != caches.end() && !it->second.taken) {
			it->second.taken = true;
			logger::info("caches: Chief took the {} by {:08X}", it->second.weapon, a_message.id);
		}
	}
}
