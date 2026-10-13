/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Arsenal.h"
#include "Link.h"
#include "Settings.h"

#undef GetObject  // wingdi.h's

namespace chiefrim::Arsenal
{
	namespace
	{
		constexpr std::string_view kPlugin = "ChiefrimMjolnir.esp";

		enum class Kind
		{
			kRounds,   // magazines of rounds
			kEnergy,   // a battery
			kGrenade,  // a grenade type's equipment: given and returned by count
		};

		struct Weapon
		{
			RE::FormID  localId;  // in kPlugin (tools/mjolnir/weapons.py's WEAPONS has the same)
			const char* tag;      // Halo's weap (a grenade's: its eqip)
			Kind        kind;
		};
		constexpr Weapon kWeapons[]{
			{ 0x900, "weapons\\needler\\needler", Kind::kRounds },
			{ 0x901, "weapons\\assault rifle\\assault rifle", Kind::kRounds },
			{ 0x902, "weapons\\pistol\\pistol", Kind::kRounds },
			{ 0x903, "weapons\\shotgun\\shotgun", Kind::kRounds },
			{ 0x904, "weapons\\sniper rifle\\sniper rifle", Kind::kRounds },
			{ 0x905, "weapons\\rocket launcher\\rocket launcher", Kind::kRounds },
			{ 0x906, "weapons\\plasma pistol\\plasma pistol", Kind::kEnergy },
			{ 0x907, "weapons\\plasma rifle\\plasma rifle", Kind::kEnergy },
			{ 0x908, "weapons\\frag grenade\\frag grenade", Kind::kGrenade },
			{ 0x909, "weapons\\plasma grenade\\plasma grenade", Kind::kGrenade },
		};

		struct Item
		{
			const Weapon*          weapon;
			RE::TESObjectWEAP*     form;
		};
		std::unordered_map<RE::FormID, Item> items;  // by the item's (loaded) form ID

		const Item* ByTag(const char* a_tag)
		{
			for (const auto& [id, item] : items) {
				if (::_stricmp(item.weapon->tag, a_tag) == 0) {
					return &item;
				}
			}
			return nullptr;
		}

		// An item's rounds are in its name, as Halo's HUD shows them: "Needler
		// (20/60)" (loaded, spare), "Plasma Rifle (40%)" (battery). Skyrim
		// keeps a name with the item wherever it goes (dropped, in a chest, a
		// merchant's, a save). One without: a new one's rounds.
		std::string ItemName(const Item& a_item, const cr_chief_weapon& a_weapon)
		{
			if (a_item.weapon->kind == Kind::kGrenade) {
				return a_item.form->GetName();
			}
			if (a_item.weapon->kind == Kind::kEnergy) {
				return fmt::format("{} ({}%)", a_item.form->GetName(), std::lround(100.0f * std::clamp(1.0f - a_weapon.age, 0.0f, 1.0f)));
			}
			return fmt::format("{} ({}/{})", a_item.form->GetName(), std::max<int>(a_weapon.rounds_loaded[0], 0), std::max<int>(a_weapon.rounds_total[0], 0));
		}

		cr_chief_weapon ItemRounds(const Item& a_item, std::string_view a_name)
		{
			cr_chief_weapon weapon{};
			std::strncpy(weapon.tag, a_item.weapon->tag, CR_WEAPON_TAG_LENGTH - 1);
			weapon.rounds_total[0] = weapon.rounds_total[1] = -1;  // a new one's
			weapon.rounds_loaded[0] = weapon.rounds_loaded[1] = -1;
			if (a_item.weapon->kind == Kind::kGrenade) {
				return weapon;
			}
			const auto open = a_name.rfind(" (");
			if (open == std::string_view::npos || !a_name.ends_with(')')) {
				return weapon;
			}
			const std::string inside(a_name.substr(open + 2, a_name.size() - open - 3));
			int first = 0, second = 0;
			char end = 0;
			if (a_item.weapon->kind == Kind::kEnergy) {
				if (std::sscanf(inside.c_str(), "%d%c", &first, &end) == 2 && end == '%') {
					weapon.age = 1.0f - std::clamp(first, 0, 100) / 100.0f;
				}
			} else if (std::sscanf(inside.c_str(), "%d/%d%c", &first, &second, &end) == 2) {
				weapon.rounds_loaded[0] = static_cast<std::int16_t>(std::clamp(first, 0, 32767));
				weapon.rounds_total[0] = static_cast<std::int16_t>(std::clamp(second, 0, 32767));
			}
			return weapon;
		}

		// The player's instance of it that's equipped (either hand)
		RE::ExtraDataList* Worn(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_form)
		{
			auto inventory = a_player->GetInventory([a_form](RE::TESBoundObject& a_object) { return &a_object == a_form; });
			for (auto& [object, data] : inventory) {
				if (!data.second || !data.second->extraLists) {
					continue;
				}
				for (auto* list : *data.second->extraLists) {
					if (list && (list->HasType(RE::ExtraDataType::kWorn) || list->HasType(RE::ExtraDataType::kWornLeft))) {
						return list;
					}
				}
			}
			return nullptr;
		}

		// A menu listing the inventory's entries reads them as it redraws: one
		// equipped there and taken out from under it (the worn one removed,
		// next frame) was read freed (a crash in
		// InventoryEntryData::GetWornExtraDataList). So what's equipped in one
		// is handed over as the last of them closes.
		bool ItemMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && (ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) || ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) ||
							 ui->IsMenuOpen(RE::BarterMenu::MENU_NAME) || ui->IsMenuOpen(RE::GiftMenu::MENU_NAME) ||
							 ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME));
		}

		std::vector<RE::FormID> waiting;  // equipped in an item menu (main thread only)

		// On the main thread, no item menu open: the equipped one handed to
		// Chief and out of the inventory; or, while he can't take it,
		// unequipped and kept.
		void HandOverNow(RE::FormID a_id)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			const auto found = items.find(a_id);
			if (!player || found == items.end()) {
				return;
			}
			const auto& item = found->second;
			auto* worn = Worn(player, item.form);
			if (!worn) {
				return;  // unequipped again already
			}
			const std::string name = worn->GetDisplayName(item.form);
			auto& link = Link::Get();
			cr_msg_give_weapon give{};
			give.index = -1;
			give.flags = CR_GIVE_ITEM;
			give.item = item.form->GetFormID();
			give.weapon = ItemRounds(item, name);
			const bool grenades = item.weapon->kind == Kind::kGrenade;
			if (grenades) {
				// all the player has: Chief takes what he has room for
				const auto counts = player->GetInventoryCounts([&item](RE::TESBoundObject& a_object) { return &a_object == item.form; });
				const auto found = counts.find(item.form);
				give.flags |= CR_GIVE_GRENADES;
				give.count = static_cast<std::uint32_t>(std::max(found != counts.end() ? found->second : 1, 1));
			}
			if (!link.Enabled() || !link.Connected() || !link.PushRaw(CR_MSG_GIVE_WEAPON, &give, sizeof(give))) {
				logger::info("arsenal: {} equipped while Chief can't take it (Chiefrim {}); unequipped", name, link.Enabled() ? "not linked" : "off");
				RE::SendHUDMessage::ShowHUDMessage(fmt::format("Master Chief can't take the {} now.", item.form->GetName()).c_str());
				auto* objects = RE::BGSDefaultObjectManager::GetSingleton();
				const bool left = worn->HasType(RE::ExtraDataType::kWornLeft);
				auto* slot = objects->GetObject<RE::BGSEquipSlot>(left ? RE::DEFAULT_OBJECT::kLeftHandEquip : RE::DEFAULT_OBJECT::kRightHandEquip);
				RE::ActorEquipManager::GetSingleton()->UnequipObject(player, item.form, worn, 1, slot);
				return;
			}
			if (grenades) {
				// the one equipped, then the rest; how many he took comes back
				logger::info("arsenal: {} equipped: {} to Chief", name, give.count);
				player->RemoveItem(item.form, 1, RE::ITEM_REMOVE_REASON::kRemove, worn, nullptr);
				if (give.count > 1) {
					player->RemoveItem(item.form, static_cast<std::int32_t>(give.count - 1), RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
				}
				return;
			}
			logger::info("arsenal: {} equipped: to Chief ({}, {}/{} rounds, battery {:.2f})", name, give.weapon.tag,
				give.weapon.rounds_loaded[0], give.weapon.rounds_total[0], 1.0f - give.weapon.age);
			RE::SendHUDMessage::ShowHUDMessage(fmt::format("Handed the {} to Master Chief.", item.form->GetName()).c_str());
			player->RemoveItem(item.form, 1, RE::ITEM_REMOVE_REASON::kRemove, worn, nullptr);
		}

		// After the equip event: now, or as the item menus close
		void HandOver(RE::FormID a_id)
		{
			SKSE::GetTaskInterface()->AddTask([a_id] {
				if (ItemMenuOpen()) {
					if (std::find(waiting.begin(), waiting.end(), a_id) == waiting.end()) {
						waiting.push_back(a_id);
					}
					return;
				}
				HandOverNow(a_id);
			});
		}

		class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuSink* Get()
			{
				static MenuSink sink;
				return &sink;
			}

			// a menu closed: next frame (it's off the stack then), the waiting
			// ones if that was the last item menu
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && !a_event->opening) {
					SKSE::GetTaskInterface()->AddTask([] {
						if (waiting.empty() || ItemMenuOpen()) {
							return;
						}
						const auto ids = std::move(waiting);
						waiting.clear();
						for (const auto id : ids) {
							HandOverNow(id);
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// A placed weapon named for its rounds (a new one's: unnamed); its name
		std::string Named(RE::TESObjectREFR& a_placed, const Item& a_item, const cr_chief_weapon& a_weapon)
		{
			const bool fresh = a_item.weapon->kind == Kind::kGrenade ||
				(a_weapon.rounds_total[0] < 0 && a_weapon.rounds_loaded[0] < 0 && a_weapon.age == 0.0f);
			const std::string name = fresh ? std::string(a_item.form->GetName()) : ItemName(a_item, a_weapon);
			if (!fresh) {
				a_placed.extraList.SetOverrideName(name.c_str());
			}
			return name;
		}

		// A dying proxy's kit (CR_RETURNED_LOOT): it falls by its actor, as a
		// Skyrim actor's own weapon does, a little above and around it; an
		// actor not here (unloaded): into its inventory, else lost
		void DropLoot(const Item& a_item, const cr_chief_weapon& a_weapon, RE::FormID a_actor, std::int32_t a_count)
		{
			auto* actor = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_actor);
			if (!actor) {
				logger::warn("arsenal: {:08X} died with a {}, but isn't here; it's lost", a_actor, a_item.form->GetName());
				return;
			}
			if (!actor->Is3DLoaded()) {
				actor->AddObjectToContainer(a_item.form, nullptr, a_count, nullptr);
				logger::info("arsenal: {} ({:08X}) died with {} {}: in its inventory", actor->GetName(), a_actor, a_count, a_item.form->GetName());
				return;
			}
			static std::mt19937                   random{ std::random_device{}() };
			std::uniform_real_distribution<float> around(-25.0f, 25.0f);
			std::string                           name;
			for (std::int32_t index = 0; index < a_count; ++index) {
				auto placed = actor->PlaceObjectAtMe(a_item.form, false);
				if (!placed) {
					break;
				}
				name = Named(*placed, a_item, a_weapon);
				const auto at = actor->GetPosition();
				placed->SetPosition(RE::NiPoint3(at.x + around(random), at.y + around(random), at.z + 50.0f + 10.0f * index));
			}
			logger::info("arsenal: {} ({:08X}) died with {} {}: dropped by it", actor->GetName(), a_actor, a_count, name);
		}

		class EquipSink final : public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			static EquipSink* Get()
			{
				static EquipSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (a_event && a_event->equipped && player && a_event->actor.get() == player && items.contains(a_event->baseObject)) {
					HandOver(a_event->baseObject);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data || !data->LookupModByName(kPlugin)) {
			logger::info("arsenal: {} isn't loaded: no Halo weapons as items", kPlugin);
			return;
		}
		for (const auto& weapon : kWeapons) {
			if (auto* form = data->LookupForm<RE::TESObjectWEAP>(weapon.localId, kPlugin)) {
				items[form->GetFormID()] = { &weapon, form };
				logger::info("arsenal: {} ({:08X}) is Halo's {}", form->GetName(), form->GetFormID(), weapon.tag);
			} else {
				logger::warn("arsenal: no weapon {:03X} in {} (built before its weapons?)", weapon.localId, kPlugin);
			}
		}
		if (!items.empty()) {
			RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESEquipEvent>(EquipSink::Get());
			if (auto* ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
			}
		}
	}

	void OnLinked()
	{
		cr_msg_arsenal arsenal{};
		for (const auto& [id, item] : items) {
			if (arsenal.count < CR_ARSENAL_MAX) {
				std::strncpy(arsenal.tags[arsenal.count++], item.weapon->tag, CR_WEAPON_TAG_LENGTH - 1);
			}
		}
		// what an enemy Chief fights drops as it dies (its proxy's kit)
		arsenal.weapon_chance = std::clamp(Settings::ReadFloat(L"Loot", L"fEnemyWeaponChance", 0.33f), 0.0f, 1.0f);
		arsenal.grenade_chance[0] = std::clamp(Settings::ReadFloat(L"Loot", L"fEnemyOneGrenadeChance", 0.2f), 0.0f, 1.0f);
		arsenal.grenade_chance[1] = std::clamp(Settings::ReadFloat(L"Loot", L"fEnemyTwoGrenadesChance", 0.1f), 0.0f, 1.0f);
		if (Settings::ReadBool(L"Loot", L"bCreaturesDrop", false)) {
			arsenal.flags |= CR_ARSENAL_CREATURES_DROP;
		}
		Link::Get().PushRaw(CR_MSG_ARSENAL, &arsenal, sizeof(arsenal));
		logger::info("arsenal: told Halo of {} weapons with items; an enemy drops a gun by {:.2f}, 1 grenade by {:.2f}, 2 by {:.2f}; creatures {}",
			arsenal.count, arsenal.weapon_chance, arsenal.grenade_chance[0], arsenal.grenade_chance[1],
			arsenal.flags & CR_ARSENAL_CREATURES_DROP ? "1 or 2 grenades" : "nothing");
	}

	void OnReturned(const cr_msg_weapon_returned& a_message)
	{
		cr_chief_weapon weapon = a_message.weapon;
		weapon.tag[CR_WEAPON_TAG_LENGTH - 1] = 0;
		const auto given = items.find(a_message.item);
		const char* givenName = given != items.end() ? given->second.form->GetName() : "weapon";
		if (a_message.reason == CR_RETURNED_LEFTOVER && !weapon.tag[0]) {
			logger::info("arsenal: Chief's {} took all {} rounds of the one handed over", givenName, a_message.taken);
			RE::SendHUDMessage::ShowHUDMessage(fmt::format("Master Chief took {} rounds from the {}.", a_message.taken, givenName).c_str());
			return;
		}
		const Item* item = ByTag(weapon.tag);
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!item || !player) {
			logger::warn("arsenal: Halo returned {}, which has no item here; it's lost", weapon.tag);
			return;
		}
		const auto count = static_cast<std::int32_t>(std::clamp<std::uint32_t>(a_message.count, 0, 255));
		if (a_message.reason == CR_RETURNED_LOOT) {
			DropLoot(*item, weapon, a_message.item, count);
			return;
		}
		if (item->weapon->kind == Kind::kGrenade) {
			// grenades back by count, unnamed: they stack
			if (count > 0) {
				player->AddObjectToContainer(item->form, nullptr, count, nullptr);
			}
			const std::string name = item->form->GetName();
			logger::info("arsenal: Chief took {} {}; {} back in the inventory", a_message.taken, name, count);
			RE::SendHUDMessage::ShowHUDMessage((a_message.taken > 0 ?
				fmt::format("Master Chief took {} {}{}.", a_message.taken, name, a_message.taken == 1 ? "" : "s") :
				a_message.reason == CR_RETURNED_REFUSED ? fmt::format("Master Chief can't take {}s now.", name) :
				fmt::format("Master Chief can't carry more {}s.", name)).c_str());
			return;
		}
		// placed by the player, named for its rounds, and picked up: the name
		// goes into the inventory with it
		auto placed = player->PlaceObjectAtMe(item->form, false);
		if (!placed) {
			logger::warn("arsenal: couldn't place a {} to return it", item->form->GetName());
			return;
		}
		const std::string name = Named(*placed, *item, weapon);
		player->PickUpObject(placed.get(), 1, false, false);
		switch (a_message.reason) {
		case CR_RETURNED_DROPPED:
			logger::info("arsenal: Chief let go of the {}: back in the inventory", name);
			RE::SendHUDMessage::ShowHUDMessage(fmt::format("Master Chief handed back the {}.", name).c_str());
			break;
		case CR_RETURNED_LEFTOVER:
			logger::info("arsenal: Chief's {} took {} rounds; the {} is back in the inventory", givenName, a_message.taken, name);
			RE::SendHUDMessage::ShowHUDMessage((a_message.taken > 0 ?
				fmt::format("Master Chief took {} rounds; the {} is back in your pack.", a_message.taken, name) :
				fmt::format("Master Chief's {} is full.", item->form->GetName())).c_str());
			break;
		default:
			logger::info("arsenal: Chief couldn't take the {}: back in the inventory", name);
			RE::SendHUDMessage::ShowHUDMessage(fmt::format("Master Chief can't take the {} now.", item->form->GetName()).c_str());
			break;
		}
	}
}
