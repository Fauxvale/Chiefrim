/* SPDX-License-Identifier: GPL-3.0-or-later */
// Adapted in part from SkyCraft's Combat.cpp (MIT, Copyright (c) 2026 chasmlol;
// THIRD-PARTY-NOTICES.md): the actor table, hits through Skyrim's own hit
// processing, the player's damage refunded and sent on, the player killed.
#include "Combat.h"
#include "Input.h"
#include "Link.h"
#include "Settings.h"

namespace chiefrim::Combat
{
	namespace
	{
		constexpr float kActorRange = 25.0f * CR_SKY_UNITS_PER_WU;  // ~5300 units, ~75 m (docs §8.1)
		constexpr float kHitMemorySeconds = 1.0f;  // a hit event explains a health drop this soon after it
		constexpr float kDotFlushSeconds = 0.5f;   // damage over time goes to Halo in sums this often

		struct Config
		{
			float         damageMult = 1.0f;          // [Combat] fDamageMult
			float         levelExponent = 0.5f;       // [Combat] fLevelExponent
			float         incomingReference = 250.0f; // [Combat] fIncomingReference
			std::uint32_t giveWeaponKey = 0x41;       // [Controls] iGiveWeaponKey (F7: free in Skyrim; F9 is Quickload)
			std::uint32_t toggleKey = 0x44;           // [Controls] iToggleChiefrimKey (F10)
			std::uint32_t restartKey = 0x57;          // [Controls] iRestartHaloKey (F11)
		} config;

		struct RecentHit
		{
			RE::FormID    attacker{ 0 };
			std::uint32_t kind{ CR_HURT_OTHER };
			float         age{ 99.0f };
		};

		struct
		{
			RecentHit         lastHit;
			float             dotDamage = 0.0f;  // damage without a hit event: summed
			float             dotAge = 0.0f;
			bool              essentialSet = false;
			bool              healthPrimed = false;
			bool              chiefDead = false;
			std::atomic<int>  givePresses{ 0 };
			std::uint32_t     actorFrame = 0;
			std::uint32_t     hitsOnActors = 0, hurts = 0;
			float             hurtTotal = 0.0f, damageTotal = 0.0f;
			ULONGLONG         nextReport = 0;
			bool              keyChecked = false;  // in game, with a corner message
		} s;

		// ---- the player's hits from Skyrim -----------------------------------

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			static HitSink* Get()
			{
				static HitSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!a_event || !player || a_event->target.get() != player) {
					return RE::BSEventNotifyControl::kContinue;
				}
				RecentHit hit;
				hit.attacker = a_event->cause ? a_event->cause->GetFormID() : 0;
				hit.age = 0.0f;
				hit.kind = CR_HURT_MELEE;
				if (a_event->projectile != 0) {
					hit.kind = CR_HURT_PROJECTILE;
				} else if (auto* source = RE::TESForm::LookupByID(a_event->source)) {
					switch (source->GetFormType()) {
					case RE::FormType::Spell:
					case RE::FormType::Enchantment:
					case RE::FormType::Scroll:
					case RE::FormType::Ingredient:
					case RE::FormType::AlchemyItem:
						hit.kind = CR_HURT_MAGIC;
						break;
					default:
						break;
					}
				}
				s.lastHit = hit;
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Chiefrim's own keys: give Chief the host map's next weapon (debug,
		// docs §8.4), Chiefrim off and on, Halo restarted (docs §11).
		class HotkeySink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static HotkeySink* Get()
			{
				static HotkeySink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				for (auto* event = a_events ? *a_events : nullptr; event; event = event->next) {
					auto* button = event->AsButtonEvent();
					if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) {
						continue;
					}
					const auto key = button->GetIDCode();
					if (config.giveWeaponKey && key == config.giveWeaponKey) {
						++s.givePresses;
					} else if (config.toggleKey && key == config.toggleKey) {
						Link::Get().RequestToggle();
					} else if (config.restartKey && key == config.restartKey) {
						Link::Get().RequestRestart();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// These keys must not be Skyrim's own (as Input.cpp checks Chiefrim's
		// other hotkeys): they would do both.
		void CheckKeyClash(bool a_show)
		{
			auto* map = RE::ControlMap::GetSingleton();
			if (!map) {
				return;
			}
			const std::pair<std::uint32_t, const char*> keys[]{ { config.giveWeaponKey, "give-weapon (iGiveWeaponKey)" },
				{ config.toggleKey, "Chiefrim on/off (iToggleChiefrimKey)" }, { config.restartKey, "restart Halo (iRestartHaloKey)" } };
			for (const auto& [key, what] : keys) {
				if (!key) {
					continue;
				}
				const auto clash = map->GetUserEventName(key, RE::INPUT_DEVICE::kKeyboard, RE::UserEvents::INPUT_CONTEXT_ID::kGameplay);
				if (clash.empty()) {
					continue;
				}
				logger::warn("Chiefrim's {} key 0x{:02X} is also Skyrim's \"{}\"; change it in Chiefrim.ini", what, key, clash);
				if (a_show) {
					const auto message = std::format("Chiefrim: the {} key is also Skyrim's {}", what, clash);
					RE::SendHUDMessage::ShowHUDMessage(message.c_str());
				}
			}
		}

		void SetEssential(RE::PlayerCharacter* a_player, bool a_on)
		{
			if (a_on == s.essentialSet) {
				return;
			}
			auto& flags = a_player->GetActorRuntimeData().boolFlags;
			if (a_on) {
				flags.set(RE::Actor::BOOL_FLAGS::kEssential);
			} else {
				flags.reset(RE::Actor::BOOL_FLAGS::kEssential);
			}
			s.essentialSet = a_on;
		}

		RE::NiPoint3 Chest(RE::Actor* a_actor)
		{
			return a_actor->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, a_actor->GetHeight() * 0.6f };
		}

		void SendHurt(std::uint32_t a_kind, float a_damage, RE::FormID a_attacker)
		{
			if (a_damage <= 0.01f || s.chiefDead) {
				return;
			}
			cr_msg_player_hurt hurt{};
			hurt.amount = a_damage / std::max(config.incomingReference, 1.0f);
			hurt.kind = a_kind;
			hurt.attacker = a_attacker;
			if (auto* attacker = a_attacker ? RE::TESForm::LookupByID<RE::Actor>(a_attacker) : nullptr) {
				const auto from = Chest(attacker);
				hurt.from = { from.x, from.y, from.z };
			}
			Link::Get().PushRaw(CR_MSG_PLAYER_HURT, &hurt, sizeof(hurt));
			++s.hurts;
			s.hurtTotal += a_damage;
			if (s.hurts <= 5) {
				logger::info("combat: the player took {:.1f} damage ({}) from {:08X}: {:.2f} of Chief's vitality", a_damage,
					a_kind == CR_HURT_MELEE ? "melee" : a_kind == CR_HURT_PROJECTILE ? "projectile" : a_kind == CR_HURT_MAGIC ? "magic" : "other",
					a_attacker, hurt.amount);
			}
		}

		// Halo owns Chief's health (docs §8.3): Skyrim's damage to the player
		// is refunded here and sent to Halo. (The player's health stays full in
		// Skyrim rather than following Chief's body: lowered, Skyrim's essential
		// player kneels in bleedout at the next hit.)
		void BridgePlayerDamage(RE::PlayerCharacter* a_player, float a_delta)
		{
			auto*       av = a_player->AsActorValueOwner();
			const float max = a_player->GetActorValueMax(RE::ActorValue::kHealth);
			const float deficit = max - av->GetActorValue(RE::ActorValue::kHealth);
			s.lastHit.age += a_delta;
			if (!s.healthPrimed) {
				// whatever the save had lost before Chief took over isn't a new hit
				s.healthPrimed = true;
				if (deficit > 0.0f) {
					av->RestoreActorValue(RE::ActorValue::kHealth, deficit);
				}
				return;
			}
			if (deficit > 0.01f) {
				av->RestoreActorValue(RE::ActorValue::kHealth, deficit);
				if (s.lastHit.age < kHitMemorySeconds) {
					SendHurt(s.lastHit.kind, deficit, s.lastHit.attacker);
					s.lastHit.age = 99.0f;  // one hit event explains one health drop
				} else {
					s.dotDamage += deficit;  // spells and poison over time, falls, traps
				}
			}
			s.dotAge += a_delta;
			if (s.dotAge >= kDotFlushSeconds) {
				SendHurt(CR_HURT_MAGIC, s.dotDamage, 0);
				s.dotDamage = 0.0f;
				s.dotAge = 0.0f;
			}
		}

		// ---- the actors, to Halo ---------------------------------------------

		void WriteActors(RE::PlayerCharacter* a_player)
		{
			static std::vector<std::pair<float, RE::Actor*>> nearby;
			nearby.clear();
			auto* lists = RE::ProcessLists::GetSingleton();
			const auto playerPos = a_player->GetPosition();
			if (lists) {
				for (auto& handle : lists->highActorHandles) {
					auto  actorPtr = handle.get();
					auto* actor = actorPtr.get();
					if (!actor || actor == a_player || actor->IsDisabled() || !actor->Is3DLoaded() || actor->IsGhost() || actor->IsDead()) {
						continue;
					}
					const float distance = actor->GetPosition().GetDistance(playerPos);
					if (distance <= kActorRange) {
						nearby.emplace_back(distance, actor);
					}
				}
			}
			std::sort(nearby.begin(), nearby.end(), [](const auto& a_a, const auto& a_b) { return a_a.first < a_b.first; });

			cr_actors actors{};
			actors.frame = ++s.actorFrame;
			for (const auto& [distance, actor] : nearby) {
				if (actors.count >= CR_ACTORS_MAX) {
					break;
				}
				auto&      out = actors.actors[actors.count++];
				const auto position = actor->GetPosition();
				out.form_id = actor->GetFormID();
				out.flags = (actor->IsHostileToActor(a_player) ? CR_ACTOR_HOSTILE : 0u) | (actor->IsEssential() ? CR_ACTOR_ESSENTIAL : 0u);
				out.position = { position.x, position.y, position.z };
				out.heading = actor->GetAngleZ();
				out.height = std::clamp(actor->GetHeight(), 20.0f, 2000.0f);
				out.radius = std::clamp(actor->GetBoundRadius(), 5.0f, 500.0f);
			}
			Link::Get().SendActors(actors);
		}

		// ---- Chief's hits, into Skyrim ---------------------------------------

		// Skyrim's own hit processing (as its melee code calls it): damage,
		// hit reactions and stagger, the pain voice, the hit event (crime,
		// quests, the enemy health bar), kill credit. Found by the call the
		// game's melee handler makes; plain damage if it isn't there.
		using ProcessHitFn = void(RE::Actor*, RE::HitData&);
		using HitDataCtorFn = RE::HitData*(RE::HitData*);
		ProcessHitFn*  processHit = nullptr;
		HitDataCtorFn* hitDataCtor = nullptr;

		void ResolveHitPipeline()
		{
			if (!REL::Module::IsAE()) {
				logger::info("combat: not AE; Chief's hits are plain damage");
				return;
			}
			const auto  caller = REL::ID(38627).address();
			const auto  target = REL::ID(38586).address();
			const auto* code = reinterpret_cast<const std::uint8_t*>(caller);
			for (std::size_t i = 0; i + 5 <= 0x1000; ++i) {
				std::int32_t rel;
				std::memcpy(&rel, code + i + 1, 4);
				if (code[i] == 0xE8 && caller + i + 5 + static_cast<std::intptr_t>(rel) == target) {
					processHit = reinterpret_cast<ProcessHitFn*>(target);
					hitDataCtor = reinterpret_cast<HitDataCtorFn*>(REL::ID(43995).address());
					logger::info("combat: Chief's hits go through Skyrim's hit processing");
					return;
				}
			}
			logger::warn("combat: Skyrim's hit processing isn't where expected; Chief's hits are plain damage");
		}

		RE::NiAVObject* HitNode(RE::Actor* a_actor)
		{
			auto* root = a_actor->Get3D();
			if (!root) {
				return nullptr;
			}
			for (const char* name : { "NPC Spine2 [Spn2]", "NPC Spine1 [Spn1]", "NPC Spine [Spn0]", "NPC Pelvis [Pelv]" }) {
				if (auto* node = root->GetObjectByName(name)) {
					return node;
				}
			}
			return root;
		}

		// The toughness of an NPC against the player (docs §13): an NPC at
		// four times the player's level takes twice the hits (exponent 0.5).
		float Toughness(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			const float ratio = float(std::max<std::uint16_t>(a_actor->GetLevel(), 1)) / float(std::max<std::uint16_t>(a_player->GetLevel(), 1));
			return std::clamp(std::pow(ratio, config.levelExponent), 0.5f, 3.0f);
		}

		void ApplyHit(RE::PlayerCharacter* a_player, RE::Actor* a_actor, float a_fraction)
		{
			const float maxHealth = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
			const float toughness = Toughness(a_actor, a_player);
			const float damage = a_fraction * maxHealth * config.damageMult / toughness;
			if (!(damage > 0.0f)) {
				return;
			}
			// big hits (a rocket, a grenade's middle) stagger; bullets don't
			const float stagger = a_fraction >= 0.5f ? std::clamp(a_fraction, 0.5f, 1.0f) : 0.0f;
			auto*       weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(0x00013985);  // Hunting Bow: a ranged hit's impacts and sounds
			auto*       node = HitNode(a_actor);
			RE::NiPoint3 hitPos = node ? node->world.translate : Chest(a_actor);
			RE::NiPoint3 dir = hitPos - a_player->GetPosition();
			dir = dir.Length() > 1e-3f ? dir / dir.Length() : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };

			if (processHit && hitDataCtor) {
				alignas(16) std::array<std::byte, sizeof(RE::HitData)> storage{};
				auto* hit = reinterpret_cast<RE::HitData*>(storage.data());
				hitDataCtor(hit);
				hit->Populate(a_player, a_actor, nullptr);
				hit->weapon = weapon;
				hit->hitPosition = hitPos;
				hit->hitDirection = dir;
				hit->totalDamage = damage;
				hit->physicalDamage = damage;
				hit->resistedPhysicalDamage = 0.0f;
				hit->resistedTypedDamage = 0.0f;
				hit->sneakAttackBonus = 1.0f;
				hit->bonusHealthDamageMult = 1.0f;
				hit->stagger = stagger;
				hit->pushBack = 0.0f;
				hit->skill = RE::ActorValue::kNone;
				hit->flags.reset(RE::HitData::Flag::kPowerAttack, RE::HitData::Flag::kCritical, RE::HitData::Flag::kSneakAttack,
					RE::HitData::Flag::kMeleeAttack);
				processHit(a_actor, *hit);
			} else {
				a_actor->DoDamage(damage, a_player, true);
			}
			if (!a_actor->IsDead() && !a_actor->IsPlayerTeammate() && !a_actor->IsInCombat()) {
				a_actor->StartCombat(a_player);
			}
			++s.hitsOnActors;
			s.damageTotal += damage;
			if (s.hitsOnActors <= 5) {
				logger::info("combat: Chief hit {} ({:08X}, level {}, health {:.0f}) for {:.2f} of a proxy: {:.1f} damage (toughness {:.2f}){}",
					a_actor->GetDisplayFullName(), a_actor->GetFormID(), a_actor->GetLevel(), maxHealth, a_fraction, damage, toughness,
					a_actor->IsDead() ? ", dead" : "");
			}
		}
	}

	void Install()
	{
		const auto path = Settings::IniPath();
		config.damageMult = Settings::ReadFloat(L"Combat", L"fDamageMult", 1.0f);
		config.levelExponent = Settings::ReadFloat(L"Combat", L"fLevelExponent", 0.5f);
		config.incomingReference = Settings::ReadFloat(L"Combat", L"fIncomingReference", 250.0f);
		config.giveWeaponKey = ::GetPrivateProfileIntW(L"Controls", L"iGiveWeaponKey", 0x41, path.c_str());
		config.toggleKey = ::GetPrivateProfileIntW(L"Controls", L"iToggleChiefrimKey", 0x44, path.c_str());
		config.restartKey = ::GetPrivateProfileIntW(L"Controls", L"iRestartHaloKey", 0x57, path.c_str());
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESHitEvent>(HitSink::Get());
		}
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink(HotkeySink::Get());
		}
		ResolveHitPipeline();
		CheckKeyClash(false);
		logger::info("combat: damage x{:.2f}, level exponent {:.2f}, {:.0f} Skyrim damage kills Chief; keys: give weapon 0x{:02X},"
					 " Chiefrim on/off 0x{:02X}, restart Halo 0x{:02X}",
			config.damageMult, config.levelExponent, config.incomingReference, config.giveWeaponKey, config.toggleKey, config.restartKey);
	}

	void PerFrame(RE::PlayerCharacter* a_player, float a_delta)
	{
		if (s.chiefDead) {
			return;
		}
		if (!std::exchange(s.keyChecked, true)) {
			CheckKeyClash(true);
		}
		SetEssential(a_player, true);  // Chief's death decides; Skyrim's mustn't come first
		WriteActors(a_player);
		BridgePlayerDamage(a_player, a_delta);
		for (int presses = s.givePresses.exchange(0); presses > 0; --presses) {
			cr_msg_give_weapon give{};
			give.index = -1;
			Link::Get().PushRaw(CR_MSG_GIVE_WEAPON, &give, sizeof(give));
			logger::info("combat: give Chief the next weapon (debug key)");
		}
		const auto now = ::GetTickCount64();
		if (now >= s.nextReport) {
			if (s.nextReport && (s.hitsOnActors || s.hurts)) {
				logger::info("combat: last 30 s, Chief hit Skyrim's people {} times ({:.0f} damage); the player took {:.0f} damage in {} hits",
					s.hitsOnActors, s.damageTotal, s.hurtTotal, s.hurts);
			}
			s.hitsOnActors = s.hurts = 0;
			s.damageTotal = s.hurtTotal = 0.0f;
			s.nextReport = now + 30000;
		}
	}

	void OnHitActor(const cr_msg_hit_actor& a_hit)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_hit.form_id);
		if (player && actor && !actor->IsDead()) {
			ApplyHit(player, actor, a_hit.fraction);
		}
	}

	void OnChiefDied()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || player->IsDead() || s.chiefDead) {
			return;
		}
		s.chiefDead = true;
		auto* killer = s.lastHit.attacker ? RE::TESForm::LookupByID<RE::Actor>(s.lastHit.attacker) : nullptr;
		logger::info("combat: Chief is dead (last hit by {:08X}); so is Skyrim's player", s.lastHit.attacker);
		SetEssential(player, false);
		const float health = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
		player->KillImpl(killer, health + 1.0f, true, false);
		if (!player->IsDead()) {
			player->KillImmediate();
		}
	}

	void Release(RE::PlayerCharacter* a_player)
	{
		if (a_player) {
			SetEssential(a_player, false);
		}
		s.healthPrimed = false;
		s.chiefDead = false;
		s.lastHit = RecentHit{};
		s.dotDamage = 0.0f;
	}
}
