/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Puppet.h"
#include "Settings.h"

#include "Camera.h"
#include "Collision.h"
#include "Input.h"
#include "Link.h"

namespace chiefrim::Puppet
{
	namespace
	{
		// Skyrim moved the player itself (fast travel, a door, a script, coc)
		// when a loading screen closes, the world changes, or he is further
		// than this, horizontally, from where we last put him: then Halo is
		// told instead of pulling him back. Vertical pops don't count: until
		// Halo collides with Skyrim's real ground (Phase 1), Skyrim lifts a
		// player we put under a slope back onto it.
		constexpr float kSkyrimMovedDistance = 1024.0f;

		struct State
		{
			bool worldSent{ false };
			std::uint32_t generation{ 0 };
			RE::FormID worldId{ 0 };
			bool interior{ false };
			std::optional<RE::NiPoint3> lastPuppetPosition;
			std::uint32_t lastTick{ 0 };
			bool haveTick{ false };
			ULONGLONG lastTickChange{ 0 };
			bool loggedStale{ false };
			bool loadingScreenClosed{ false };
			// After telling Halo to move Chief: don't follow (or detect Skyrim
			// moves) until Halo's state shows him there, or a moment passed.
			std::optional<RE::NiPoint3> awaitingTeleport;
			ULONGLONG awaitingSince{ 0 };
			// The character controller as Chief found it (to put back on unlink)
			bool haveSnapshot{ false };
			std::uint32_t snapshotFlags{ 0 };
			std::uint32_t snapshotState{ 0 };
			ULONGLONG watchUntil{ 0 };
			ULONGLONG nextWatch{ 0 };
		} s;

		using Flags = RE::CHARACTER_FLAGS;

		// What Skyrim's jump and sprint depend on, for the log.
		void LogMovementState(RE::PlayerCharacter* a_player, const char* a_when)
		{
			const auto* controller = a_player->GetCharController();
			bool inJump = false, sprinting = false, animationDriven = false;
			a_player->GetGraphVariableBool("bInJumpState", inJump);
			a_player->GetGraphVariableBool("IsSprinting", sprinting);
			a_player->GetGraphVariableBool("bAnimationDriven", animationDriven);
			const auto* map = RE::ControlMap::GetSingleton();
			const auto* controls = RE::PlayerControls::GetSingleton();
			const bool jumpHandler = controls && controls->jumpHandler && controls->jumpHandler->IsInputEventHandlingEnabled();
			const bool sprintHandler = controls && controls->sprintHandler && controls->sprintHandler->IsInputEventHandlingEnabled();
			logger::info("player {}: z {:.1f}, midair {}, controls 0x{:08X} (jumping {}), handlers jump {} sprint {}",
				a_when, a_player->GetPosition().z, a_player->IsInMidair(),
				map ? map->GetRuntimeData().enabledControls.underlying() : 0u,
				map && map->IsJumpingControlsEnabled(), jumpHandler, sprintHandler);
			logger::info("player {}: controller state {}, flags 0x{:08X} (can jump {}, jumping {}, support {}); graph: in jump {}, sprinting {}, animation driven {}",
				a_when,
				controller ? static_cast<std::uint32_t>(controller->context.currentState) : 99u,
				controller ? controller->flags.underlying() : 0u,
				controller && controller->flags.all(Flags::kCanJump),
				controller && controller->flags.all(Flags::kJumping),
				controller && controller->flags.all(Flags::kSupport),
				inJump, sprinting, animationDriven);
		}

		void SnapshotController(RE::PlayerCharacter* a_player)
		{
			LogMovementState(a_player, "as Chief takes over");
			if (const auto* controller = a_player->GetCharController()) {
				s.snapshotFlags = controller->flags.underlying();
				s.snapshotState = static_cast<std::uint32_t>(controller->context.currentState);
				s.haveSnapshot = true;
			}
		}

		// Following Chief teleports the player every frame with no velocity:
		// Skyrim's controller can be left mid-jump or in the air, and then it
		// refuses jump and sprint. Back to how Chief found it.
		void RestoreController(RE::PlayerCharacter* a_player)
		{
			LogMovementState(a_player, "as Chief lets go");
			auto* controller = a_player->GetCharController();
			if (!controller || !s.haveSnapshot) {
				return;
			}
			controller->flags.reset(Flags::kJumping);
			if ((s.snapshotFlags & static_cast<std::uint32_t>(Flags::kCanJump)) != 0) {
				controller->flags.set(Flags::kCanJump);
			}
			controller->context.currentState = static_cast<RE::hkpCharacterStateType>(s.snapshotState);
			controller->wantState = static_cast<RE::hkpCharacterStateType>(s.snapshotState);
			bool inJump = false;
			if (a_player->GetGraphVariableBool("bInJumpState", inJump) && inJump) {
				a_player->NotifyAnimationGraph("JumpLand");
			}
			s.haveSnapshot = false;
			// Halo's floor can sit a hair under Skyrim's: a little up, so
			// Skyrim's physics lands the player and finds its footing again.
			auto position = a_player->GetPosition();
			position.z += 5.0f;
			a_player->SetPosition(position, true);
			LogMovementState(a_player, "after putting it back");
			s.watchUntil = ::GetTickCount64() + 10000;
			s.nextWatch = ::GetTickCount64() + 2000;
		}

		// For a while after Chief lets go: is Skyrim's player itself again?
		void WatchAfterUnlink(RE::PlayerCharacter* a_player)
		{
			const auto now = ::GetTickCount64();
			if (!s.watchUntil || now < s.nextWatch) {
				return;
			}
			LogMovementState(a_player, "after Chief let go");
			s.nextWatch = now + 2000;
			if (now >= s.watchUntil) {
				s.watchUntil = 0;
			}
		}

		bool GameplayIsRunning()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		// The world the player is in: the worldspace, or the interior cell.
		bool CurrentWorld(RE::PlayerCharacter* a_player, RE::FormID& a_id, bool& a_interior)
		{
			auto* cell = a_player->GetParentCell();
			if (!cell) {
				return false;
			}
			a_interior = cell->IsInteriorCell();
			if (a_interior) {
				a_id = cell->GetFormID();
			} else if (auto* worldspace = a_player->GetWorldspace()) {
				a_id = worldspace->GetFormID();
			} else {
				return false;
			}
			return true;
		}

		// Chief's standing height for Halo, in Skyrim units ([Chief] fHeight):
		// -1 matches the player (race and scale), 0 keeps Halo's own (~150).
		float ChiefHeight(RE::PlayerCharacter* a_player)
		{
			static const float setting = Settings::ReadFloat(L"Chief", L"fHeight", -1.0f);
			if (setting >= 0.0f) {
				return setting;
			}
			const float height = a_player->GetHeight();
			if (!(height >= 60.0f && height <= 250.0f)) {
				logger::warn("the player's height reads {:.0f}; Chief is 128 units tall", height);
				return 128.0f;
			}
			return height;
		}

		// Chief's collision radius for Halo, in Skyrim units ([Chief] fRadius):
		// -1 matches the player's character controller, 0 keeps Halo's own (~43).
		float ChiefRadius(RE::PlayerCharacter* a_player)
		{
			static const float setting = Settings::ReadFloat(L"Chief", L"fRadius", -1.0f);
			if (setting >= 0.0f) {
				return setting;
			}
			const auto* controller = a_player->GetCharController();
			if (!controller) {
				return 0.0f;
			}
			// the controller's capsule is in Havok units (1/69.99 of Skyrim's)
			float radius = controller->radius;
			if (radius > 0.0f && radius < 5.0f) {
				radius *= 69.99f;
			}
			logger::info("the player's character controller: radius {:.2f}, height {:.2f}, scale {:.2f} -> Chief's radius {:.0f}",
				controller->radius, controller->height, controller->scale, radius);
			return radius >= 10.0f && radius <= 60.0f ? radius : 0.0f;
		}

		// Phase 0: the origin and the flat floor are where the player stands
		// when the world (re)starts (docs §4, §5.1).
		void SendWorld(RE::PlayerCharacter* a_player, RE::FormID a_id, bool a_interior)
		{
			const auto position = a_player->GetPosition();
			cr_world_context context{};
			context.world_id = a_id;
			context.is_interior = a_interior ? 1u : 0u;
			context.origin = { position.x, position.y, position.z };
			context.floor_z = position.z;
			context.generation = ++s.generation;
			context.field_of_view = Camera::FieldOfView();
			context.chief_height = ChiefHeight(a_player);
			context.chief_radius = ChiefRadius(a_player);

			auto& link = Link::Get();
			link.SendWorldContext(context);
			link.SendTeleport(position, a_player->data.angle.z);
			Collision::Reset(context.generation);  // Halo forgets the old world's collision

			s.worldSent = true;
			s.worldId = a_id;
			s.interior = a_interior;
			s.lastPuppetPosition = position;
			s.haveTick = false;
			s.awaitingTeleport = position;
			s.awaitingSince = ::GetTickCount64();
			logger::info("world {:08X}{}: origin and floor at ({:.0f}, {:.0f}, {:.0f}); Chief {:.0f} units tall",
				a_id, a_interior ? " (interior)" : "", position.x, position.y, position.z, context.chief_height);
			logger::info("Chief's radius {:.0f} units", context.chief_radius);
		}

		// Applies Halo's latest state every frame, new or not: between Halo's
		// frames nothing on Skyrim's side may move the player away from Chief.
		void Follow(RE::PlayerCharacter* a_player, const cr_player_state& a_state)
		{
			const auto now = ::GetTickCount64();
			if (!s.haveTick || a_state.tick != s.lastTick) {
				if (s.loggedStale) {
					logger::info("Halo's player state is updating again");
					s.loggedStale = false;
				}
				s.lastTick = a_state.tick;
				s.haveTick = true;
				s.lastTickChange = now;
			} else if (!s.loggedStale && now - s.lastTickChange > 500) {
				logger::info("Halo's player state hasn't changed for {} ms (tick {})", now - s.lastTickChange, a_state.tick);
				s.loggedStale = true;
			}

			const RE::NiPoint3 position{ a_state.position.x, a_state.position.y, a_state.position.z };
			a_player->SetPosition(position, true);
			a_player->data.angle.z = a_state.yaw;
			a_player->data.angle.x = a_state.pitch;
			if (auto* controller = a_player->GetCharController()) {
				// Halo moves the player: no momentum or fall damage of Skyrim's own.
				controller->SetLinearVelocityImpl(RE::hkVector4(0.0f, 0.0f, 0.0f, 0.0f));
				controller->fallStartHeight = position.z;
			}
			s.lastPuppetPosition = position;
			Camera::Drive(a_player, a_state);
		}

		void PerFrame(RE::PlayerCharacter* a_player)
		{
			auto& link = Link::Get();
			const bool wasConnected = link.Connected();
			if (!link.Update()) {
				s.worldSent = false;
				if (wasConnected) {
					Input::OnUnlinked();  // Skyrim's own controls back
					Camera::Release(a_player);
					RestoreController(a_player);
				}
				WatchAfterUnlink(a_player);
				return;
			}
			if (!wasConnected) {
				s.worldSent = false;  // a new Halo: tell it everything again
				Input::OnLinked();
				SnapshotController(a_player);
			}
			Input::Publish(a_player);
			if (!GameplayIsRunning()) {
				return;
			}

			RE::FormID id{};
			bool interior{};
			if (!CurrentWorld(a_player, id, interior)) {
				return;
			}

			const auto position = a_player->GetPosition();
			const bool worldChanged = !s.worldSent || id != s.worldId || interior != s.interior;

			// Halo hasn't moved Chief yet: Skyrim keeps the player where it put him.
			if (s.awaitingTeleport && !worldChanged) {
				auto state = link.ReadPlayerState();
				const bool arrived = state &&
					RE::NiPoint3{ state->position.x, state->position.y, state->position.z }.GetDistance(*s.awaitingTeleport) < 256.0f;
				if (!arrived && ::GetTickCount64() - s.awaitingSince < 2000) {
					Collision::Update(a_player);
					return;
				}
				if (!arrived) {
					logger::warn("Halo didn't move Chief to the teleport in 2 s; following him anyway");
				}
				s.awaitingTeleport.reset();
				s.lastPuppetPosition = position;
			}
			const auto horizontal = [](const RE::NiPoint3& a_a, const RE::NiPoint3& a_b) {
				return std::hypot(a_a.x - a_b.x, a_a.y - a_b.y);
			};
			const bool loaded = std::exchange(s.loadingScreenClosed, false);
			const bool skyrimMoved = loaded ||
				(s.lastPuppetPosition && horizontal(*s.lastPuppetPosition, position) > kSkyrimMovedDistance);
			if (skyrimMoved && !worldChanged && s.lastPuppetPosition) {
				const auto& from = *s.lastPuppetPosition;
				logger::info("Skyrim moved the player{} {:.0f} units by itself, from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f})",
					loaded ? " (loading screen)" : "", from.GetDistance(position),
					from.x, from.y, from.z, position.x, position.y, position.z);
			}
			if (worldChanged || skyrimMoved) {
				SendWorld(a_player, id, interior);
				return;
			}

			if (auto state = link.ReadPlayerState()) {
				Follow(a_player, *state);
			}
			Collision::Update(a_player);
		}

		struct PlayerUpdateHook
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				try {
					PerFrame(a_this);
				} catch (const std::exception& e) {
					logger::error("per-frame update: {}", e.what());
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace
	{
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
					s.loadingScreenClosed = true;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(LoadingSink::Get());
		}
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdateHook::func = vtable.write_vfunc(0xAD, PlayerUpdateHook::thunk);
		logger::info("hooked PlayerCharacter::Update");
	}
}
