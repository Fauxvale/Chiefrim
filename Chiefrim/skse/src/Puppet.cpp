/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Puppet.h"
#include "Settings.h"

#include "Camera.h"
#include "Collision.h"
#include "CoSave.h"
#include "Combat.h"
#include "Console.h"
#include "Handoff.h"
#include "Hud.h"
#include "Lighting.h"
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
			// Movement as Skyrim shows it, summed up every 30 s (docs: hitches)
			struct
			{
				LARGE_INTEGER last{};
				LARGE_INTEGER since{};
				RE::NiPoint3 lastPosition;
				bool havePosition{ false };
				double speedAverage{ 0.0 };
				std::uint32_t frames{ 0 }, slowFrames{ 0 }, sameState{ 0 }, stalls{ 0 }, lurches{ 0 }, inAir{ 0 };
				std::uint32_t lastTick{ 0 };
			} motion;
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
			Combat::Release(a_player);  // a load (after a death too): Halo brings Chief back whole
			cr_world_context context{};
			context.world_id = a_id;
			context.is_interior = a_interior ? 1u : 0u;
			context.origin = { position.x, position.y, position.z };
			context.floor_z = position.z;
			context.generation = ++s.generation;
			context.field_of_view = Camera::FieldOfView();
			context.chief_height = ChiefHeight(a_player);
			context.chief_radius = ChiefRadius(a_player);
			context.collision_radius = Settings::CollisionRadius();

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

		// Skyrim moves the player: where it is and looks, for Halo's Chief to
		// follow (aimed along the camera).
		void PublishPlayer(RE::PlayerCharacter* a_player, bool a_drives = true)
		{
			static std::uint32_t frame = 0;
			static RE::NiPoint3 lastPosition;
			static LARGE_INTEGER lastTime{};
			cr_skyrim_player player{};
			const auto position = a_player->GetPosition();
			player.frame = ++frame;
			player.flags = a_drives ? CR_SKYRIM_DRIVES : 0u;
			player.position = { position.x, position.y, position.z };
			player.yaw = a_player->data.angle.z;
			player.pitch = a_player->data.angle.x;
			// the camera: its root's position, and its forward column
			// (Skyrim's camera-root columns are right, forward, up)
			player.eye = player.position;
			player.forward = { std::sin(player.yaw) * std::cos(player.pitch), std::cos(player.yaw) * std::cos(player.pitch), -std::sin(player.pitch) };
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
				const auto& world = camera->cameraRoot->world;
				player.eye = { world.translate.x, world.translate.y, world.translate.z };
				player.forward = { world.rotate.entry[0][1], world.rotate.entry[1][1], world.rotate.entry[2][1] };
			}
			LARGE_INTEGER now{}, frequency{};
			::QueryPerformanceCounter(&now);
			::QueryPerformanceFrequency(&frequency);
			if (lastTime.QuadPart) {
				const float dt = float(double(now.QuadPart - lastTime.QuadPart) / double(frequency.QuadPart));
				if (dt > 0.0f && dt < 0.25f) {
					player.velocity = { (position.x - lastPosition.x) / dt, (position.y - lastPosition.y) / dt, (position.z - lastPosition.z) / dt };
				}
			}
			lastTime = now;
			lastPosition = position;
			if (const auto* controller = a_player->GetCharController();
				controller && controller->context.currentState == RE::hkpCharacterStateType::kOnGround) {
				player.flags |= CR_SKYRIM_ON_GROUND;
			}
			if (a_player->IsSneaking()) {
				player.flags |= CR_SKYRIM_SNEAKING;
			}
			Link::Get().SendSkyrimPlayer(player);
		}

		// Halo publishes at its frame rate, Skyrim draws at its own (uneven
		// against Halo's): taking the latest state each frame, the player moved
		// in uneven steps, stalling and lurching. States are kept with Halo's
		// clock; the player is drawn where Chief was kDelayUs ago, between the
		// two states about that moment. A jump between them (a teleport) cuts.
		cr_player_state Interpolated(const cr_player_state& a_latest)
		{
			constexpr std::int64_t kDelayUs = 25000;
			constexpr float kCutUnits = 150.0f;
			struct Entry { std::uint32_t timeUs; cr_vec3 position; cr_vec3 eye; };
			static std::array<Entry, 16> history{};
			static std::size_t count = 0, head = 0;
			static std::int64_t offsetUs = 0;  // our clock minus Halo's
			static ULONGLONG offsetSince = 0;
			static std::uint32_t lastTick = 0;

			LARGE_INTEGER now{}, frequency{};
			::QueryPerformanceCounter(&now);
			::QueryPerformanceFrequency(&frequency);
			const std::int64_t localUs = std::int64_t(double(now.QuadPart) * 1e6 / double(frequency.QuadPart));

			if (a_latest.time_us == 0) {
				return a_latest;  // a Halo that doesn't stamp its states
			}
			if (count == 0 || a_latest.tick != lastTick) {
				head = (head + 1) % history.size();
				history[head] = { a_latest.time_us, a_latest.position, a_latest.eye };
				count = std::min(count + 1, history.size());
				lastTick = a_latest.tick;
				// the clocks' offset: the smallest seen lately (least delivery delay)
				const std::int64_t offset = localUs - std::int64_t(a_latest.time_us);
				const auto tickNow = ::GetTickCount64();
				if (count == 1 || offset < offsetUs || tickNow - offsetSince > 2000) {
					offsetUs = offset;
					offsetSince = tickNow;
				}
			}
			if (count < 2) {
				return a_latest;
			}
			// Halo's time to draw, as an age before the newest state (wrap-safe)
			const std::uint32_t target = std::uint32_t(localUs - offsetUs - kDelayUs);
			const auto ageOf = [&](std::uint32_t a_time) { return std::int32_t(history[head].timeUs - a_time); };
			const std::int32_t targetAge = ageOf(target);
			if (targetAge <= 0) {
				return a_latest;  // nothing newer to draw toward
			}
			for (std::size_t k = 0; k + 1 < count; ++k) {
				const auto& newer = history[(head + history.size() - k) % history.size()];
				const auto& older = history[(head + history.size() - k - 1) % history.size()];
				const std::int32_t newerAge = ageOf(newer.timeUs), olderAge = ageOf(older.timeUs);
				if (olderAge >= targetAge && newerAge <= targetAge) {
					const float span = float(olderAge - newerAge);
					const float t = span > 0.0f ? float(olderAge - targetAge) / span : 1.0f;
					const float dx = newer.position.x - older.position.x, dy = newer.position.y - older.position.y,
								dz = newer.position.z - older.position.z;
					if (dx * dx + dy * dy + dz * dz > kCutUnits * kCutUnits) {
						return a_latest;
					}
					cr_player_state result = a_latest;
					const auto lerp = [t](const cr_vec3& a, const cr_vec3& b) {
						return cr_vec3{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
					};
					result.position = lerp(older.position, newer.position);
					result.eye = lerp(older.eye, newer.eye);
					return result;
				}
			}
			return a_latest;  // older than we keep: as it stands
		}

		// Per frame: did the player move evenly? Frames given no new state,
		// frames that stalled or lurched against the recent speed, slow
		// frames, and Skyrim's controller thinking it's in the air.
		void Measure(RE::PlayerCharacter* a_player, const cr_player_state& a_state, const RE::NiPoint3& a_position)
		{
			auto& m = s.motion;
			LARGE_INTEGER now{}, frequency{};
			::QueryPerformanceCounter(&now);
			::QueryPerformanceFrequency(&frequency);
			if (!m.since.QuadPart) {
				m.since = now;
			}
			if (m.last.QuadPart && m.havePosition) {
				const double dt = double(now.QuadPart - m.last.QuadPart) / double(frequency.QuadPart);
				const double moved = m.lastPosition.GetDistance(a_position);
				m.frames++;
				m.slowFrames += dt > 0.040;
				m.sameState += a_state.tick == m.lastTick;
				if (dt > 0.0 && dt < 0.25) {
					const double speed = moved / dt;
					if (m.speedAverage > 150.0) {  // walking (Skyrim units a second)
						m.stalls += speed < m.speedAverage * 0.35;
						m.lurches += speed > m.speedAverage * 2.0 && moved < 200.0;
					}
					m.speedAverage = m.speedAverage * 0.9 + speed * 0.1;
				}
				if (const auto* controller = a_player->GetCharController()) {
					m.inAir += controller->context.currentState != RE::hkpCharacterStateType::kOnGround;
				}
			}
			m.last = now;
			m.lastPosition = a_position;
			m.havePosition = true;
			m.lastTick = a_state.tick;
			if (double(now.QuadPart - m.since.QuadPart) / double(frequency.QuadPart) >= 30.0) {
				if (m.frames) {
					logger::info("last 30 s as Skyrim showed it: {} frames ({} over 40 ms), {} with no new state from Halo, "
								 "{} stalls and {} lurches while walking, {} with Skyrim's controller in the air",
						m.frames, m.slowFrames, m.sameState, m.stalls, m.lurches, m.inAir);
				}
				const auto keep = m.speedAverage;
				m = {};
				m.since = now;
				m.last = now;
				m.lastPosition = a_position;
				m.havePosition = true;
				m.lastTick = a_state.tick;
				m.speedAverage = keep;
			}
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

			const cr_player_state shown = Interpolated(a_state);
			const RE::NiPoint3 position{ shown.position.x, shown.position.y, shown.position.z };
			Measure(a_player, shown, position);
			a_player->SetPosition(position, true);
			a_player->data.angle.z = a_state.yaw;
			a_player->data.angle.x = a_state.pitch;
			if (auto* controller = a_player->GetCharController()) {
				// Halo moves the player: no momentum or fall damage of Skyrim's own.
				controller->SetLinearVelocityImpl(RE::hkVector4(0.0f, 0.0f, 0.0f, 0.0f));
				controller->fallStartHeight = position.z;
				// on the ground when Chief is: placed every frame, Skyrim's
				// controller otherwise counts itself in the air all along
				if (shown.on_ground) {
					controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
				}
			}
			s.lastPuppetPosition = position;
			Camera::Drive(a_player, shown, true);
		}

		void PerFrame(RE::PlayerCharacter* a_player, float a_delta)
		{
			auto& link = Link::Get();
			const bool wasConnected = link.Connected();
			if (!link.Update()) {
				s.worldSent = false;
				if (wasConnected) {
					Handoff::Reset();
					Input::OnUnlinked();  // Skyrim's own controls back
					Hud::Restore();
					Camera::Release(a_player, true);
					Combat::Release(a_player);
					Lighting::RemoveFlashlight();
					RestoreController(a_player);
				}
				WatchAfterUnlink(a_player);
				return;
			}
			if (!wasConnected) {
				s.worldSent = false;  // a new Halo: tell it everything again
				Input::OnLinked();
				CoSave::OnLinked();
				Console::OnLinked();
				Lighting::Reset();
				SnapshotController(a_player);
			}
			// Skyrim's animations, scenes and swimming (docs §11): kept as it
			// was while paused (a crafting station's menu is open, say).
			const bool wasHandedOff = Handoff::Active();
			const bool handedOff = GameplayIsRunning() ? Handoff::Update(a_player) : wasHandedOff;
			Input::Publish(a_player);
			if (handedOff) {
				Hud::Restore();
				Camera::Release(a_player);
			} else {
				Hud::Update();
			}
			if (wasHandedOff && !handedOff && !Settings::SkyrimMoves()) {
				PublishPlayer(a_player, false);  // Halo moves Chief again, from here
			}
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
			if (Settings::SkyrimMoves()) {
				s.awaitingTeleport.reset();  // Skyrim's player is where it is; Chief follows
			}
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

			CoSave::Update();  // after the world: a load's kit comes after Halo makes Chief whole
			Combat::PerFrame(a_player, a_delta);
			Lighting::Update(a_player);
			Lighting::UpdateFlashlight(a_player);
			if (Settings::SkyrimMoves() || handedOff) {
				// handed off, Skyrim moves the player whatever the mode, and Chief follows
				PublishPlayer(a_player);
				s.lastPuppetPosition = position;  // a jump further than a frame's walk is a teleport
				if (auto state = link.ReadPlayerState(); state && !handedOff) {
					Camera::Drive(a_player, *state, false);  // Halo's field of view (zoom)
				}
			} else if (auto state = link.ReadPlayerState()) {
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
					PerFrame(a_this, a_delta);
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
