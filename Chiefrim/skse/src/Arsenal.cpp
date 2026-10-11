/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Arsenal.h"
#include "Link.h"

#undef GetObject  // wingdi.h's

namespace chiefrim::Arsenal
{
	namespace
	{
		constexpr std::string_view kPlugin = "ChiefrimMjolnir.esp";

		struct Weapon
		{
			RE::FormID  localId;  // in kPlugin (tools/mjolnir/weapons.py's WEAPONS has the same)
			const char* tag;      // Halo's weap
			bool        energy;   // a battery, not rounds
		};
		constexpr Weapon kWeapons[]{
			{ 0x900, "weapons\\needler\\needler", false },
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
			if (a_item.weapon->energy) {
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
			const auto open = a_name.rfind(" (");
			if (open == std::string_view::npos || !a_name.ends_with(')')) {
				return weapon;
			}
			const std::string inside(a_name.substr(open + 2, a_name.size() - open - 3));
			int first = 0, second = 0;
			char end = 0;
			if (a_item.weapon->energy) {
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

		// After the equip event, on the main thread: the equipped one handed
		// to Chief and out of the inventory; or, while he can't take it,
		// unequipped and kept.
		void HandOver(RE::FormID a_id)
		{
			SKSE::GetTaskInterface()->AddTask([a_id] {
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
				if (!link.Enabled() || !link.Connected() || !link.PushRaw(CR_MSG_GIVE_WEAPON, &give, sizeof(give))) {
					logger::info("arsenal: {} equipped while Chief can't take it (Chiefrim {}); unequipped", name, link.Enabled() ? "not linked" : "off");
					RE::SendHUDMessage::ShowHUDMessage(fmt::format("Master Chief can't take the {} now.", item.form->GetName()).c_str());
					auto* objects = RE::BGSDefaultObjectManager::GetSingleton();
					const bool left = worn->HasType(RE::ExtraDataType::kWornLeft);
					auto* slot = objects->GetObject<RE::BGSEquipSlot>(left ? RE::DEFAULT_OBJECT::kLeftHandEquip : RE::DEFAULT_OBJECT::kRightHandEquip);
					RE::ActorEquipManager::GetSingleton()->UnequipObject(player, item.form, worn, 1, slot);
					return;
				}
				logger::info("arsenal: {} equipped: to Chief ({}, {}/{} rounds, battery {:.2f})", name, give.weapon.tag,
					give.weapon.rounds_loaded[0], give.weapon.rounds_total[0], 1.0f - give.weapon.age);
				RE::SendHUDMessage::ShowHUDMessage(fmt::format("Handed the {} to Master Chief.", item.form->GetName()).c_str());
				player->RemoveItem(item.form, 1, RE::ITEM_REMOVE_REASON::kRemove, worn, nullptr);
			});
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
		Link::Get().PushRaw(CR_MSG_ARSENAL, &arsenal, sizeof(arsenal));
		logger::info("arsenal: told Halo of {} weapons with items", arsenal.count);
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
		// placed by the player, named for its rounds, and picked up: the name
		// goes into the inventory with it
		auto placed = player->PlaceObjectAtMe(item->form, false);
		if (!placed) {
			logger::warn("arsenal: couldn't place a {} to return it", item->form->GetName());
			return;
		}
		const bool fresh = weapon.rounds_total[0] < 0 && weapon.rounds_loaded[0] < 0 && weapon.age == 0.0f;
		const std::string name = fresh ? std::string(item->form->GetName()) : ItemName(*item, weapon);
		if (!fresh) {
			placed->extraList.SetOverrideName(name.c_str());
		}
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
