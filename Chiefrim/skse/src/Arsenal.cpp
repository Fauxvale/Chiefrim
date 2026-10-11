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
		};
		constexpr Weapon kWeapons[]{
			{ 0x900, "weapons\\needler\\needler" },
		};

		std::unordered_map<RE::FormID, const char*> tags;  // by the item's (loaded) form ID

		// The item out of the player's hands (either), on the main thread
		// after the equip event; and out of the inventory, when Chief took it.
		void Unequip(RE::FormID a_id, bool a_remove)
		{
			SKSE::GetTaskInterface()->AddTask([a_id, a_remove] {
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_id);
				if (!player || !weapon) {
					return;
				}
				auto* objects = RE::BGSDefaultObjectManager::GetSingleton();
				auto* equipment = RE::ActorEquipManager::GetSingleton();
				for (const bool left : { false, true }) {
					if (player->GetEquippedObject(left) == weapon) {
						auto* slot = objects->GetObject<RE::BGSEquipSlot>(left ? RE::DEFAULT_OBJECT::kLeftHandEquip : RE::DEFAULT_OBJECT::kRightHandEquip);
						equipment->UnequipObject(player, weapon, nullptr, 1, slot);
					}
				}
				if (a_remove) {
					player->RemoveItem(weapon, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
				}
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
				if (!a_event || !a_event->equipped || !player || a_event->actor.get() != player) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto found = tags.find(a_event->baseObject);
				if (found == tags.end()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* weapon = RE::TESForm::LookupByID(a_event->baseObject);
				const char* name = weapon ? weapon->GetName() : found->second;
				auto& link = Link::Get();
				cr_msg_give_weapon give{};
				give.index = -1;
				std::strncpy(give.name, found->second, CR_WEAPON_NAME_LENGTH - 1);
				if (!link.Enabled() || !link.Connected() || !link.PushRaw(CR_MSG_GIVE_WEAPON, &give, sizeof(give))) {
					logger::info("arsenal: {} equipped while Chief can't take it (Chiefrim {}); unequipped", name,
						link.Enabled() ? "not linked" : "off");
					RE::SendHUDMessage::ShowHUDMessage(fmt::format("Master Chief can't take the {} now.", name).c_str());
					Unequip(a_event->baseObject, false);
					return RE::BSEventNotifyControl::kContinue;
				}
				logger::info("arsenal: {} equipped: to Chief ({})", name, found->second);
				RE::SendHUDMessage::ShowHUDMessage(fmt::format("Master Chief took the {}.", name).c_str());
				Unequip(a_event->baseObject, true);
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
				tags[form->GetFormID()] = weapon.tag;
				logger::info("arsenal: {} ({:08X}) is Halo's {}", form->GetName(), form->GetFormID(), weapon.tag);
			} else {
				logger::warn("arsenal: no weapon {:03X} in {} (built before its weapons?)", weapon.localId, kPlugin);
			}
		}
		if (!tags.empty()) {
			RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESEquipEvent>(EquipSink::Get());
		}
	}
}
