#include "Puppet.h"

#include "Link.h"

namespace chiefrim::Puppet
{
	namespace
	{
		// Further than this between where we last put the player and where
		// Skyrim has him now, Skyrim moved him itself (fast travel, a script,
		// coc): tell Halo instead of pulling him back.
		constexpr float kSkyrimMovedDistance = 256.0f;

		struct State
		{
			bool worldSent{ false };
			std::uint32_t generation{ 0 };
			RE::FormID worldId{ 0 };
			bool interior{ false };
			std::optional<RE::NiPoint3> lastPuppetPosition;
			std::uint32_t lastTick{ 0 };
			bool haveTick{ false };
		} s;

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

			auto& link = Link::Get();
			link.SendWorldContext(context);
			link.SendTeleport(position, a_player->data.angle.z);

			s.worldSent = true;
			s.worldId = a_id;
			s.interior = a_interior;
			s.lastPuppetPosition = position;
			s.haveTick = false;
			logger::info("world {:08X}{}: origin and floor at ({:.0f}, {:.0f}, {:.0f})",
				a_id, a_interior ? " (interior)" : "", position.x, position.y, position.z);
		}

		void Follow(RE::PlayerCharacter* a_player, const cr_player_state& a_state)
		{
			if (s.haveTick && a_state.tick == s.lastTick) {
				return;  // nothing new from Halo this frame
			}
			s.lastTick = a_state.tick;
			s.haveTick = true;

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
		}

		void PerFrame(RE::PlayerCharacter* a_player)
		{
			auto& link = Link::Get();
			const bool wasConnected = link.Connected();
			if (!link.Update()) {
				s.worldSent = false;
				return;
			}
			if (!wasConnected) {
				s.worldSent = false;  // a new Halo: tell it everything again
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
			const bool skyrimMoved = s.lastPuppetPosition &&
				s.lastPuppetPosition->GetDistance(position) > kSkyrimMovedDistance;
			if (worldChanged || skyrimMoved) {
				SendWorld(a_player, id, interior);
				return;
			}

			if (auto state = link.ReadPlayerState()) {
				Follow(a_player, *state);
			}
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

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdateHook::func = vtable.write_vfunc(0xAD, PlayerUpdateHook::thunk);
		logger::info("hooked PlayerCharacter::Update");
	}
}
