/* SPDX-License-Identifier: GPL-3.0-or-later */
// Adapted in part from SkyCraft's Combat.cpp (MIT, Copyright (c) 2026 chasmlol;
// THIRD-PARTY-NOTICES.md): the actor table, hits through Skyrim's own hit
// processing, the player's damage refunded and sent on, the player killed.
#include "Combat.h"
#include "Hitbox.h"
#include "Input.h"
#include "Link.h"
#include "Settings.h"

#include <numbers>
#include <unordered_map>
#include <vector>

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
			float         blastForce = 10.0f;         // [Combat] fBlastForce
			float         burnSeconds = 5.0f;         // [Combat] fBurnSeconds
			float         burnDamage = 0.15f;         // [Combat] fBurnDamage
			float         propSpeed = 5.0f;           // [Combat] fPropLaunchSpeed: m/s at an explosion's centre
			float         propMinRadius = 64.0f;      // [Combat] fPropMinRadius: smaller splashes (a plasma bolt's) push nothing
			bool          shootThrough = true;        // [Combat] bShootThroughDestructibles
			float         objectDamage = 10.0f;       // [Combat] fObjectDamage: a shot's, to a destructible object
			float         blastObjectDamage = 50.0f;  // [Combat] fBlastObjectDamage: an explosion's, at its centre
			bool          hitObjects = true;          // [Combat] bShotsHitObjects: scripted objects get OnHit (traps)
			bool          ignite = true;              // [Combat] bShotsIgnite: as by a flame (oil, gas)
		} config;

		// Skyrim's torch: the traps that burn (TrapExplosiveGas, and TrapOilPool
		// after it) take a hit with it as a flame's (akWeapon == torch01)
		constexpr RE::FormID kTorch = 0x0001D4EC;
		// Firebolt's effect (FireDamageFFAimed: MagicDamageFire, and in
		// TrapGasOnMagicEffectApply): an oil pool's script (TrapOilPool) lights
		// from a fire effect applied to it, as from Flames
		constexpr RE::FormID kFireEffect = 0x00012F03;

		// The object has a script of its own: something may listen for its hits
		bool Scripted(RE::TESObjectREFR* a_ref)
		{
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!policy) {
				return false;
			}
			const auto handle = policy->GetHandleForObject(RE::FormType::Reference, a_ref);
			if (handle == policy->EmptyHandle()) {
				return false;
			}
			RE::BSSpinLockGuard lock(vm->attachedScriptsLock);
			return vm->attachedScripts.find(handle) != vm->attachedScripts.end();
		}

		// The player's hit on a scripted object, as Skyrim's own weapons
		// raise it (TESHitEvent: Papyrus's OnHit): hanging oil lamps fall,
		// tripwires and rigged beams go off; with bShotsIgnite, by a flame
		// (Skyrim's torch), so oil pools and gas burn
		void HitObject(RE::TESObjectREFR* a_ref)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* events = RE::ScriptEventSourceHolder::GetSingleton();
			if (!config.hitObjects || !player || !events || !a_ref || a_ref->As<RE::Actor>() || a_ref->IsDisabled() || !Scripted(a_ref)) {
				return;
			}
			RE::TESHitEvent hit{ a_ref, player, config.ignite ? kTorch : 0u, 0u, RE::TESHitEvent::Flag::kNone };
			events->SendEvent(&hit);
			if (config.ignite) {
				// and a fire effect on it: oil pools light from that (the hit by the
				// torch alone didn't light one in game)
				RE::TESMagicEffectApplyEvent burn{};
				burn.target.reset(a_ref);
				burn.caster.reset(player);
				burn.magicEffect = kFireEffect;
				events->SendEvent(&burn);
			}
			static int logged = 0;
			if (logged++ < 20) {
				auto* base = a_ref->GetBaseObject();
				logger::info("combat: Chief's hit on scripted {:08X} (base {:08X}){}", a_ref->GetFormID(), base ? base->GetFormID() : 0u,
					config.ignite ? ", as a flame's, and a fire effect" : "");
			}
		}

		// A destructible object's health now (its base's until first hurt)
		float ObjectHealth(RE::TESObjectREFR* a_ref)
		{
			if (auto* extra = a_ref->extraList.GetByType<RE::ExtraObjectHealth>()) {
				return extra->health;
			}
			auto* base = a_ref->GetBaseObject();
			auto* destructible = base ? skyrim_cast<RE::BGSDestructibleObjectForm*>(base) : nullptr;
			return destructible && destructible->data ? static_cast<float>(destructible->data->health) : 0.0f;
		}

		// Skyrim's own damage to a destructible object, as ObjectReference.
		// DamageObject does (its stages, their effects, destroyed at no
		// health). Not through Papyrus: a web has no script, so the VM had no
		// object of it to call the method on, and nothing happened (in game)
		void DamageObject(RE::TESObjectREFR* a_ref, float a_damage)
		{
			if (a_damage <= 0.0f) {
				return;
			}
			const float before = ObjectHealth(a_ref);
			a_ref->DamageObject(a_damage, false);
			static int logged = 0;
			if (logged++ < 20) {
				logger::info("combat: Chief's {:.0f} damage to destructible {:08X}: health {:.0f} -> {:.0f}{}", a_damage, a_ref->GetFormID(), before,
					ObjectHealth(a_ref), a_ref->IsDisabled() ? " (gone)" : "");
			}
		}

		// The people an explosion set alight (docs §8.2): burning for a while,
		// hurt a little each frame.
		struct Burning
		{
			RE::FormID form{ 0 };
			float      left{ 0.0f };      // seconds
			float      perSecond{ 0.0f }; // damage
		};
		std::vector<Burning> burning;

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

		// The head's centre above the feet, as the actor stands now (crouching,
		// sitting, a child, a Khajiit): Halo puts its proxy's head there. 0: no
		// head node (a creature), and Halo goes by the height.
		float HeadHeight(RE::Actor* a_actor, const RE::NiPoint3& a_feet)
		{
			auto* root = a_actor->Get3D(false);
			auto* head = root ? root->GetObjectByName("NPC Head [Head]") : nullptr;
			if (!head) {
				return 0.0f;
			}
			const float height = head->world.translate.z - a_feet.z;
			return height > 10.0f && height < 2000.0f ? height : 0.0f;
		}

		// ---- explosions throw Skyrim's loose objects --------------------------

		// Wakes a body and its island (hkpEntity::activate; protected in CommonLib)
		void Activate(RE::hkpEntity* a_entity)
		{
			using func_t = void (*)(RE::hkpEntity*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(60096, 60849) };
			func(a_entity);
		}

		// The dynamic rigid bodies in an explosion's reach (clutter, baskets,
		// skulls, loose weapons), pushed away from its centre and lifted, by
		// up to fPropLaunchSpeed (m/s) at its centre, fading to nothing at its
		// edge: they hop and tumble, they don't fly across the room. People
		// are thrown by Explode (with their hit).
		void Launch(const cr_msg_explosion& a_explosion)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* tes = RE::TES::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			// (no push in Halo: its camera shake's wider area, not the blast)
			if (!tes || !world || config.propSpeed <= 0.0f || a_explosion.acceleration <= 0.0f || a_explosion.radius < config.propMinRadius) {
				return;
			}
			const RE::NiPoint3 center{ a_explosion.center.x, a_explosion.center.y, a_explosion.center.z };
			const float radius = std::min(a_explosion.radius, 2048.0f);
			const float scale = RE::bhkWorld::GetWorldScale();  // Skyrim units to Havok's
			const float reach = center.GetDistance(player->GetPosition()) + radius;
			int pushed = 0;
			RE::BSWriteLockGuard guard(world->worldLock);
			tes->ForEachReferenceInRange(player, reach, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || a_ref->As<RE::Actor>() || !a_ref->Get3D()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				RE::BSVisit::TraverseScenegraphCollision(a_ref->Get3D(), [&](RE::bhkNiCollisionObject* a_collision) {
					auto* body = a_collision->body ? a_collision->body->AsBhkRigidBody() : nullptr;
					auto* rigid = body ? static_cast<RE::hkpRigidBody*>(body->referencedObject.get()) : nullptr;
					using Motion = RE::hkpMotion::MotionType;
					if (!rigid || !rigid->motion.type.any(Motion::kDynamic, Motion::kSphereInertia, Motion::kBoxInertia, Motion::kThinBoxInertia)) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					alignas(16) float at[4];
					_mm_store_ps(at, rigid->motion.motionState.transform.translation.quad);
					const RE::NiPoint3 position{ at[0] / scale, at[1] / scale, at[2] / scale };
					RE::NiPoint3 away = position - center;
					const float distance = away.Length();
					if (distance > radius) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					away = distance > 1.0f ? away / distance : RE::NiPoint3{ 0.0f, 0.0f, 1.0f };
					away.z += 0.6f;  // up and away
					away /= away.Length();
					// an impulse of mass times this speed: the velocity itself, woken
					// (CommonLib's ApplyLinearImpulse calls Havok it doesn't link)
					const float speed = config.propSpeed * (1.0f - distance / radius);
					Activate(rigid);
					rigid->motion.linearVelocity.quad = _mm_add_ps(rigid->motion.linearVelocity.quad,
						_mm_setr_ps(away.x * speed, away.y * speed, away.z * speed, 0.0f));
					++pushed;
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				return RE::BSContainer::ForEachResult::kContinue;
			});
			static int logged = 0;
			if (pushed && logged++ < 5) {
				logger::info("combat: an explosion (radius {:.0f}) at ({:.0f}, {:.0f}, {:.0f}) threw {} loose objects",
					radius, center.x, center.y, center.z, pushed);
			}
		}

		// ---- the player's healing --------------------------------------------

		// Health a potion or food restores: its restore-health effects, all of
		// their duration (food heals over time).
		float HealthRestored(const RE::AlchemyItem* a_item)
		{
			float total = 0.0f;
			for (const auto* effect : a_item->effects) {
				const auto* setting = effect ? effect->baseEffect : nullptr;
				if (!setting || setting->IsDetrimental() || setting->IsHostile() ||
					setting->GetArchetype() != RE::EffectSetting::Archetype::kValueModifier ||
					setting->data.primaryAV != RE::ActorValue::kHealth) {
					continue;
				}
				total += effect->effectItem.magnitude * std::max(static_cast<float>(effect->effectItem.duration), 1.0f);
			}
			return total;
		}

		// Drinking a potion or eating is equipping it. Skyrim's player stays at
		// full health while Chief has him (his lost health is refunded), so the
		// healing goes to Chief, on the scale of Skyrim's damage to him.
		class HealSink final : public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			static HealSink* Get()
			{
				static HealSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!a_event || !a_event->equipped || !player || a_event->actor.get() != player || !Link::Get().Connected() || s.chiefDead) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* item = RE::TESForm::LookupByID<RE::AlchemyItem>(a_event->baseObject);
				const float health = item ? HealthRestored(item) : 0.0f;
				if (health <= 0.0f) {
					return RE::BSEventNotifyControl::kContinue;
				}
				cr_msg_chief_heal heal{};
				heal.amount = health / std::max(config.incomingReference, 1.0f);
				heal.item = item->GetFormID();
				Link::Get().PushRaw(CR_MSG_CHIEF_HEAL, &heal, sizeof(heal));
				logger::info("combat: {} restores {:.0f} health: Chief heals {:.0f}% of his vitality", item->GetName(), health, heal.amount * 100.0f);
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

		// Swinging, drawing a bow or casting: Halo's motion tracker shows it,
		// as it shows a unit firing (protocol 20).
		bool Attacking(RE::Actor* a_actor)
		{
			const auto* state = a_actor->AsActorState();
			return (state && state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) || a_actor->WhoIsCasting() != 0;
		}

		// Halo's proxies: the living, and for a moment the newly dead (listed
		// alive last frame), flagged so: a proxy drops its weapon and grenades.
		// With each, where it's hit (Hitbox, protocol 18)
		void WriteActors(RE::PlayerCharacter* a_player)
		{
			static std::vector<std::pair<float, RE::Actor*>> nearby;
			static std::unordered_set<RE::FormID>             alive, wasAlive;
			static std::unordered_map<RE::FormID, ULONGLONG>  diedAt;
			nearby.clear();
			std::swap(alive, wasAlive);
			alive.clear();
			const auto now = ::GetTickCount64();
			std::erase_if(diedAt, [now](const auto& a_entry) { return now - a_entry.second > 3000; });
			auto* lists = RE::ProcessLists::GetSingleton();
			const auto playerPos = a_player->GetPosition();
			if (lists) {
				for (auto& handle : lists->highActorHandles) {
					auto  actorPtr = handle.get();
					auto* actor = actorPtr.get();
					if (!actor || actor == a_player || actor->IsDisabled() || !actor->Is3DLoaded() || actor->IsGhost()) {
						continue;
					}
					if (actor->IsDead()) {
						const auto id = actor->GetFormID();
						if (wasAlive.contains(id)) {
							diedAt.emplace(id, now);
						}
						if (!diedAt.contains(id)) {
							continue;
						}
					} else {
						alive.insert(actor->GetFormID());
					}
					const float distance = actor->GetPosition().GetDistance(playerPos);
					if (distance <= kActorRange) {
						nearby.emplace_back(distance, actor);
					}
				}
			}
			std::sort(nearby.begin(), nearby.end(), [](const auto& a_a, const auto& a_b) { return a_a.first < a_b.first; });

			static cr_actors actors;  // (34 KB: not on the stack)
			std::memset(&actors, 0, sizeof(actors));
			actors.frame = ++s.actorFrame;
			for (const auto& [distance, actor] : nearby) {
				if (actors.count >= CR_ACTORS_MAX) {
					break;
				}
				auto&      out = actors.actors[actors.count++];
				const auto position = actor->GetPosition();
				out.form_id = actor->GetFormID();
				out.flags = (actor->IsHostileToActor(a_player) ? CR_ACTOR_HOSTILE : 0u) | (actor->IsEssential() ? CR_ACTOR_ESSENTIAL : 0u) |
				            (actor->IsDead() ? CR_ACTOR_DEAD : 0u) | (Attacking(actor) ? CR_ACTOR_ATTACKING : 0u) |
				            (actor->IsSneaking() ? CR_ACTOR_SNEAKING : 0u);
				out.position = { position.x, position.y, position.z };
				out.heading = actor->GetAngleZ();
				out.height = std::clamp(actor->GetHeight(), 20.0f, 2000.0f);
				out.head = HeadHeight(actor, position);
				if (!actor->IsDead()) {
					const auto count = Hitbox::Collect(actor, actors.hitboxes + actors.hitbox_count,
						std::min(CR_HITBOXES_PER_ACTOR, CR_HITBOXES_MAX - actors.hitbox_count));
					out.hitbox_first = static_cast<std::uint16_t>(actors.hitbox_count);
					out.hitbox_count = static_cast<std::uint16_t>(count);
					actors.hitbox_count += count;
				}
			}
			Hitbox::EndFrame();
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
			if (!Settings::ReadFloat(L"Combat", L"bSkyrimHitProcessing", 1.0f)) {
				logger::info("combat: bSkyrimHitProcessing=0; Chief's hits are done piece by piece");
				return;
			}
			if (!REL::Module::IsAE()) {
				logger::info("combat: not AE; Chief's hits are plain damage");
				return;
			}
			const auto  caller = REL::ID(38627).address();
			const auto  target = REL::ID(38586).address();
			const auto* code = reinterpret_cast<const std::uint8_t*>(caller);
			const auto  use = [&](const char* a_how) {
				processHit = reinterpret_cast<ProcessHitFn*>(target);
				hitDataCtor = reinterpret_cast<HitDataCtorFn*>(REL::ID(43995).address());
				logger::info("combat: Chief's hits go through Skyrim's hit processing ({})", a_how);
			};
			for (std::size_t i = 0; i + 5 <= 0x1000; ++i) {
				std::int32_t rel;
				std::memcpy(&rel, code + i + 1, 4);
				if (code[i] == 0xE8 && caller + i + 5 + static_cast<std::intptr_t>(rel) == target) {
					use("the melee handler calls it");
					return;
				}
			}
			// Not called directly: another plugin may have hooked the call (it
			// then goes to that plugin, which calls the hit processing itself).
			// The melee handler's call is at +0x4A8 (SkyCraft's finding); if it
			// leads out of Skyrim's code, the address is right and still Skyrim's.
			const auto site = caller + 0x4A8;
			const auto* at = reinterpret_cast<const std::uint8_t*>(site);
			const auto& module = REL::Module::get();
			const auto  inSkyrim = [&](std::uintptr_t a_address) {
				return a_address >= module.base() && a_address < module.base() + module.segment(REL::Segment::textx).offset() +
				                                                     module.segment(REL::Segment::textx).size();
			};
			const auto owner = [](std::uintptr_t a_address) {
				HMODULE handle = nullptr;
				char    name[MAX_PATH]{};
				if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCSTR>(a_address), &handle) && ::GetModuleFileNameA(handle, name, MAX_PATH)) {
					const std::string_view path{ name };
					return std::string{ path.substr(path.find_last_of("\\/") + 1) };
				}
				return std::string{ "no module (a trampoline)" };
			};
			if (at[0] == 0xE8) {
				std::int32_t rel;
				std::memcpy(&rel, at + 1, 4);
				std::uintptr_t to = site + 5 + static_cast<std::intptr_t>(rel);
				std::string    path = owner(to);
				// a trampoline's jump: FF 25 [rip+0] then the address
				for (int hop = 0; hop < 4 && !inSkyrim(to); ++hop) {
					const auto* jump = reinterpret_cast<const std::uint8_t*>(to);
					if (jump[0] == 0xFF && jump[1] == 0x25) {
						std::int32_t disp;
						std::memcpy(&disp, jump + 2, 4);
						std::memcpy(&to, jump + 6 + disp, sizeof(to));
						path += " -> " + owner(to);
					} else {
						break;
					}
				}
				if (!inSkyrim(to)) {
					logger::info("combat: the melee handler's call to the hit processing is hooked ({}); it's still Skyrim's", path);
					use("its call is another plugin's hook");
					return;
				}
				logger::warn("combat: the melee handler's call at +0x4A8 goes to Skyrim's +0x{:X}, not the hit processing (+0x{:X})",
					to - module.base(), target - module.base());
			} else {
				logger::warn("combat: no call at the melee handler's +0x4A8 (bytes {:02X} {:02X} {:02X} {:02X} {:02X})", at[0], at[1], at[2],
					at[3], at[4]);
			}
			logger::warn("combat: Skyrim's hit processing isn't where expected; Chief's hits are done piece by piece");
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

		// A hit's reaction where Skyrim's hit processing isn't found: a big hit
		// staggers, away from Chief; any other flinches (Skyrim's recoil, which
		// also interrupts an attack), at most once a second each, so automatic
		// fire doesn't hold anyone in place.
		std::unordered_map<RE::FormID, ULONGLONG> lastFlinch;

		void React(RE::Actor* a_actor, const RE::NiPoint3& a_dir, float a_stagger)
		{
			if (a_stagger > 0.0f) {
				float direction = (std::atan2(a_dir.x, a_dir.y) - a_actor->GetAngleZ()) / (2.0f * std::numbers::pi_v<float>) + 0.5f;
				direction -= std::floor(direction);
				a_actor->SetGraphVariableFloat("staggerDirection", direction);
				a_actor->SetGraphVariableFloat("staggerMagnitude", a_stagger);
				a_actor->NotifyAnimationGraph("staggerStart");
				return;
			}
			const ULONGLONG now = ::GetTickCount64();
			auto&           last = lastFlinch[a_actor->GetFormID()];
			if (now - last >= 1000) {
				last = now;
				a_actor->NotifyAnimationGraph("recoilStart");
			}
			if (lastFlinch.size() > 256) {
				std::erase_if(lastFlinch, [now](const auto& a_entry) { return now - a_entry.second > 5000; });
			}
		}

		// The toughness of an NPC against the player (docs §13): an NPC at
		// four times the player's level takes twice the hits (exponent 0.5).
		float Toughness(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			const float ratio = float(std::max<std::uint16_t>(a_actor->GetLevel(), 1)) / float(std::max<std::uint16_t>(a_player->GetLevel(), 1));
			return std::clamp(std::pow(ratio, config.levelExponent), 0.5f, 3.0f);
		}

		// An explosion's hit: thrown away from its centre (Skyrim's own
		// knock-down, as its explosions do: a ragdoll), harder the more it took,
		// and set alight. Before the damage, so one it kills flies too.
		void Explode(RE::PlayerCharacter* a_player, RE::Actor* a_actor, const RE::NiPoint3& a_blast, float a_fraction, float a_toughness)
		{
			const float force = config.blastForce * std::clamp(a_fraction * 2.0f, 0.5f, 1.5f);
			if (auto* process = a_actor->GetActorRuntimeData().currentProcess; process && force > 0.0f) {
				// from just below the centre, so they go up as well as away
				process->KnockExplosion(a_actor, a_blast - RE::NiPoint3{ 0.0f, 0.0f, 32.0f }, force);
			}
			if (config.burnSeconds <= 0.0f) {
				return;
			}
			if (auto* fire = RE::TESForm::LookupByID<RE::TESEffectShader>(0x0001B212)) {  // FireFXShader (what Skyrim's fire spells burn with)
				a_actor->ApplyEffectShader(fire, config.burnSeconds);
			}
			const float resist = std::clamp(a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kResistFire), 0.0f, 85.0f) / 100.0f;
			const float total = config.burnDamage * a_actor->GetActorValueMax(RE::ActorValue::kHealth) * config.damageMult / a_toughness * (1.0f - resist);
			const auto  it = std::ranges::find(burning, a_actor->GetFormID(), &Burning::form);
			Burning&    burn = it != burning.end() ? *it : burning.emplace_back(Burning{ a_actor->GetFormID() });
			burn.left = config.burnSeconds;
			burn.perSecond = total / config.burnSeconds;
			static int logged = 0;
			if (logged++ < 5) {
				logger::info("combat: an explosion threw {} ({:08X}) with force {:.1f} and set them alight ({:.0f} damage over {:.0f} s)",
					a_actor->GetDisplayFullName(), a_actor->GetFormID(), force, total, config.burnSeconds);
			}
		}

		void Burn(RE::PlayerCharacter* a_player, float a_delta)
		{
			std::erase_if(burning, [&](Burning& a_burn) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_burn.form);
				a_burn.left -= a_delta;
				if (!actor || actor->IsDead() || a_burn.left <= 0.0f) {
					return true;
				}
				actor->DoDamage(a_burn.perSecond * a_delta, a_player, true);
				return false;
			});
		}

		void ApplyHit(RE::PlayerCharacter* a_player, RE::Actor* a_actor, float a_fraction, const cr_msg_hit_actor& a_hit)
		{
			const bool explosion = (a_hit.flags & CR_HIT_EXPLOSION) != 0;
			const float maxHealth = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
			const float toughness = Toughness(a_actor, a_player);
			// a headshot kills, as it kills a marine, whatever the actor's level
			const bool headshot = (a_hit.flags & CR_HIT_HEADSHOT) != 0;
			const float damage = headshot ? a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) + maxHealth :
				a_fraction * maxHealth * config.damageMult / toughness;
			if (!(damage > 0.0f)) {
				return;
			}
			// big hits (a rocket, a grenade's middle) stagger; bullets don't
			// (an explosion throws them instead)
			const float stagger = !explosion && a_fraction >= 0.5f ? std::clamp(a_fraction, 0.5f, 1.0f) : 0.0f;
			auto*       weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(0x00013985);  // Hunting Bow: a ranged hit's impacts and sounds
			auto*       node = HitNode(a_actor);
			RE::NiPoint3 hitPos = node ? node->world.translate : Chest(a_actor);
			RE::NiPoint3 dir = hitPos - a_player->GetPosition();
			dir = dir.Length() > 1e-3f ? dir / dir.Length() : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
			if (explosion) {
				Explode(a_player, a_actor, RE::NiPoint3{ a_hit.blast.x, a_hit.blast.y, a_hit.blast.z }, a_fraction, toughness);
			}

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
				if (!a_actor->IsDead() && !explosion && stagger <= 0.0f) {
					React(a_actor, dir, 0.0f);  // the flinch (a bullet's stagger is 0)
				}
			} else {
				// Without it (1.6.1170: docs §8.2), what it would do, piece by
				// piece: the damage, a stagger or a flinch, and the hit event.
				a_actor->DoDamage(damage, a_player, true);
				if (!a_actor->IsDead() && !explosion) {
					React(a_actor, dir, stagger);
				}
				RE::TESHitEvent event(a_actor, a_player, weapon ? weapon->GetFormID() : 0, 0, RE::TESHitEvent::Flag::kNone);
				RE::ScriptEventSourceHolder::GetSingleton()->SendEvent(&event);
			}
			// blood and the sound of the hit
			// (a blade's impacts: the bow's own are its bash)
			auto* blade = RE::TESForm::LookupByID<RE::TESObjectWEAP>(0x0001397E);  // Iron Dagger
			if (auto* impacts = RE::BGSImpactManager::GetSingleton(); impacts && blade && blade->impactDataSet && node) {
				impacts->PlayImpactEffect(a_actor, blade->impactDataSet, node->name.c_str(), dir, 128.0f, false, false);
			}
			if (!a_actor->IsDead() && !a_actor->IsPlayerTeammate() && !a_actor->IsInCombat()) {
				a_actor->StartCombat(a_player);
			}
			++s.hitsOnActors;
			s.damageTotal += damage;
			if (s.hitsOnActors <= 5) {
				logger::info("combat: Chief hit {} ({:08X}, level {}, health {:.0f}) for {:.2f} of a proxy: {:.1f} damage (toughness {:.2f}){}{}",
					a_actor->GetDisplayFullName(), a_actor->GetFormID(), a_actor->GetLevel(), maxHealth, a_fraction, damage, toughness,
					headshot ? ", a headshot" : "", a_actor->IsDead() ? ", dead" : "");
			}
		}
	}

	void Install()
	{
		const auto path = Settings::IniPath();
		config.damageMult = Settings::ReadFloat(L"Combat", L"fDamageMult", 1.0f);
		config.levelExponent = Settings::ReadFloat(L"Combat", L"fLevelExponent", 0.5f);
		config.incomingReference = Settings::ReadFloat(L"Combat", L"fIncomingReference", 250.0f);
		config.blastForce = Settings::ReadFloat(L"Combat", L"fBlastForce", 10.0f);
		config.burnSeconds = Settings::ReadFloat(L"Combat", L"fBurnSeconds", 5.0f);
		config.burnDamage = Settings::ReadFloat(L"Combat", L"fBurnDamage", 0.15f);
		config.propSpeed = Settings::ReadFloat(L"Combat", L"fPropLaunchSpeed", 5.0f);
		config.propMinRadius = Settings::ReadFloat(L"Combat", L"fPropMinRadius", 64.0f);
		config.shootThrough = Settings::ReadBool(L"Combat", L"bShootThroughDestructibles", true);
		config.objectDamage = Settings::ReadFloat(L"Combat", L"fObjectDamage", 10.0f);
		config.blastObjectDamage = Settings::ReadFloat(L"Combat", L"fBlastObjectDamage", 50.0f);
		config.hitObjects = Settings::ReadBool(L"Combat", L"bShotsHitObjects", true);
		config.ignite = Settings::ReadBool(L"Combat", L"bShotsIgnite", true);
		config.giveWeaponKey = ::GetPrivateProfileIntW(L"Controls", L"iGiveWeaponKey", 0x41, path.c_str());
		config.toggleKey = ::GetPrivateProfileIntW(L"Controls", L"iToggleChiefrimKey", 0x44, path.c_str());
		config.restartKey = ::GetPrivateProfileIntW(L"Controls", L"iRestartHaloKey", 0x57, path.c_str());
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESHitEvent>(HitSink::Get());
			events->AddEventSink<RE::TESEquipEvent>(HealSink::Get());
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
		// dead (Chief, or Skyrim killed its player): nothing to refund or hit
		// until the reload's new world (Release)
		if (s.chiefDead || a_player->IsDead()) {
			return;
		}
		if (!std::exchange(s.keyChecked, true)) {
			CheckKeyClash(true);
		}
		SetEssential(a_player, true);  // Chief's death decides; Skyrim's mustn't come first
		WriteActors(a_player);
		BridgePlayerDamage(a_player, a_delta);
		Burn(a_player, a_delta);
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

	bool ShootThrough(RE::TESObjectREFR* a_ref)
	{
		if (!config.shootThrough || !a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || a_ref->As<RE::Actor>()) {
			return false;
		}
		auto* base = a_ref->GetBaseObject();
		auto* destructible = base ? skyrim_cast<RE::BGSDestructibleObjectForm*>(base) : nullptr;
		return destructible && destructible->data && destructible->data->health > 0;
	}

	void OnExplosion(const cr_msg_explosion& a_explosion)
	{
		Launch(a_explosion);
		// the destructible objects in its reach, by how near
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* tes = RE::TES::GetSingleton();
		if (!player || !tes || a_explosion.radius <= 1.0f) {
			return;
		}
		const RE::NiPoint3 center{ a_explosion.center.x, a_explosion.center.y, a_explosion.center.z };
		const float radius = std::min(a_explosion.radius, 2048.0f);
		std::vector<std::pair<RE::TESObjectREFR*, float>> hurt;
		tes->ForEachReferenceInRange(player, center.GetDistance(player->GetPosition()) + radius, [&](RE::TESObjectREFR* a_ref) {
			auto* model = a_ref && !a_ref->As<RE::Actor>() ? a_ref->Get3D() : nullptr;
			if (model) {
				// to its nearest side, near enough: its centre less its size
				const float distance = std::max(model->worldBound.center.GetDistance(center) - model->worldBound.radius * 0.5f, 0.0f);
				if (distance < radius) {
					hurt.emplace_back(a_ref, 1.0f - distance / radius);
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		// (out of the loop: a hit's scripts and a destruction may change the cell's references)
		for (const auto& [ref, closeness] : hurt) {
			if (ShootThrough(ref) && !Scripted(ref)) {  // a scripted one destroys itself (an oil pool as it lights)
				DamageObject(ref, config.blastObjectDamage * closeness);
			}
			HitObject(ref);  // a lamp falls, oil and gas burn
		}
	}

	void OnShot(const cr_msg_shot& a_shot)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) {
			return;
		}
		// the first thing on the way, as Skyrim's own arrows would find it
		const float scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pick{};
		pick.rayInput.from = RE::hkVector4(a_shot.from.x * scale, a_shot.from.y * scale, a_shot.from.z * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(a_shot.to.x * scale, a_shot.to.y * scale, a_shot.to.z * scale, 0.0f);
		RE::CFilter filter{};
		player->GetCollisionFilterInfo(filter);  // its group: the player's own capsule isn't hit
		filter.SetCollisionLayer(RE::COL_LAYER::kProjectile);
		pick.rayInput.filterInfo = filter;
		{
			RE::BSReadLockGuard lock(world->worldLock);
			world->PickObject(pick);
		}
		const auto* hit = pick.rayOutput.HasHit() ? pick.rayOutput.rootCollidable : nullptr;
		auto*       ref = hit ? RE::TESHavokUtilities::FindCollidableRef(*hit) : nullptr;
		if (!ref || ref->As<RE::Actor>()) {
			return;  // people are hit through their proxies
		}
		if (ShootThrough(ref) && !Scripted(ref)) {  // a scripted one destroys itself (an oil pool as it lights)
			DamageObject(ref, config.objectDamage);
		}
		HitObject(ref);  // a lamp falls, oil and gas burn, a tripwire goes off
	}

	void OnHitActor(const cr_msg_hit_actor& a_hit)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_hit.form_id);
		if (player && actor && !actor->IsDead()) {
			ApplyHit(player, actor, a_hit.fraction, a_hit);
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
		burning.clear();
	}
}
