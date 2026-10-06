/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim
{
	// The Skyrim end of the shared memory (protocol/chiefrim_protocol.h, docs §10).
	// Halo creates /dev/shm/chiefrim_v1; this opens it as Z:\dev\shm\chiefrim_v1
	// whenever Halo is up, and lets go when Halo stops.
	class Link
	{
	public:
		static Link& Get();

		// Once a frame on the main thread: connects, keeps the heartbeat and
		// reads Halo's messages. Returns whether the link is up.
		bool Update();

		bool Connected() const { return shm_ != nullptr; }

		// Chiefrim's hotkeys (any thread; applied in the next Update): Chiefrim
		// off (unlinked, Skyrim's own player again, Halo stopped) or on, and Halo
		// killed and started again. The supervisor running Halo
		// (tools/launch_halo.sh) does the stopping and starting.
		void RequestToggle() { toggleRequested_ = true; }
		void RequestRestart() { restartRequested_ = true; }
		bool Enabled() const { return enabled_; }

		void SendWorldContext(const cr_world_context& a_context);
		void SendTeleport(const RE::NiPoint3& a_position, float a_heading);
		void SendInput(const cr_input& a_input);

		// Pushes a message onto the ring to Halo; false if it's full now.
		bool PushRaw(std::uint16_t a_type, const void* a_message, std::uint32_t a_size);

		// The latest player state; nullopt until Halo publishes one.
		std::optional<cr_player_state> ReadPlayerState();
		void SendSkyrimPlayer(const cr_skyrim_player& a_player);

		// The compositor (docs §9), on the thread that runs Update: Skyrim's
		// screen to Halo, and Halo's frames; nullptr while unlinked.
		void SendDisplay(const cr_display& a_display);
		void SendCamera(const cr_camera& a_camera);
		void SendActors(const cr_actors& a_actors);
		const cr_frames* Frames() const { return shm_ ? &shm_->frames : nullptr; }

	private:
		bool TryOpen(ULONGLONG a_now);
		void Close(const char* a_reason);
		void Push(std::uint16_t a_type, const void* a_message, std::uint32_t a_size);
		// to the supervisor: "restart", "stop" or "start" (/dev/shm/chiefrim_control)
		void Control(const char* a_command);

		HANDLE file_{ INVALID_HANDLE_VALUE };
		HANDLE mapping_{ nullptr };
		cr_shared* shm_{ nullptr };
		std::uint32_t lastHaloHeartbeat_{ 0 };
		std::uint32_t haloPid_{ 0 };  // the Halo linked to: another one (restarted) means linking again
		ULONGLONG lastHaloHeartbeatChange_{ 0 };
		ULONGLONG nextOpenAttempt_{ 0 };
		bool loggedWaiting_{ false };
		bool enabled_{ true };
		std::atomic<bool> toggleRequested_{ false };
		std::atomic<bool> restartRequested_{ false };
		std::uint32_t controlCount_{ 0 };

		// Writes Skyrim's heartbeat a few times a second, also while the game
		// is paused in a menu or a loading screen, when PlayerCharacter::Update
		// doesn't run. Stopped (and joined) before the view is unmapped.
		std::jthread heartbeat_;
	};
}
