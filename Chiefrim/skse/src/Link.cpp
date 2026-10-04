/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Link.h"

namespace chiefrim
{
	Link& Link::Get()
	{
		static Link link;
		return link;
	}

	bool Link::TryOpen(ULONGLONG a_now)
	{
		file_ = ::CreateFileW(CR_SHM_WINE_PATH, GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file_ == INVALID_HANDLE_VALUE) {
			if (!loggedWaiting_) {
				logger::info("waiting for Halo ({} not there yet)", CR_SHM_LINUX_PATH);
				loggedWaiting_ = true;
			}
			return false;
		}

		LARGE_INTEGER size{};
		if (!::GetFileSizeEx(file_, &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(cr_shared))) {
			Close(nullptr);  // Halo is still creating it
			return false;
		}

		mapping_ = ::CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, sizeof(cr_shared), nullptr);
		if (mapping_) {
			shm_ = static_cast<cr_shared*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cr_shared)));
		}
		if (!shm_) {
			logger::error("could not map {} (error {})", CR_SHM_LINUX_PATH, ::GetLastError());
			Close(nullptr);
			return false;
		}

		if (CR_LOAD_ACQ(&shm_->magic) != CR_MAGIC) {
			Close(nullptr);  // not initialised yet
			return false;
		}
		if (shm_->version != CR_PROTOCOL_VERSION || shm_->total_size != sizeof(cr_shared)) {
			logger::error("Halo speaks protocol {} ({} bytes); this plugin speaks {} ({} bytes)",
				shm_->version, shm_->total_size, CR_PROTOCOL_VERSION, sizeof(cr_shared));
			Close(nullptr);
			nextOpenAttempt_ = ::GetTickCount64() + 10000;
			return false;
		}

		shm_->skyrim_pid = ::GetCurrentProcessId();
		CR_STORE_REL(&shm_->skyrim_state, CR_SIDE_READY);
		lastHaloHeartbeat_ = CR_LOAD_ACQ(&shm_->halo_heartbeat);
		lastHaloHeartbeatChange_ = a_now;  // the caller's clock: no unsigned wrap below
		loggedWaiting_ = false;

		heartbeat_ = std::jthread([shm = shm_](std::stop_token a_stop) {
			while (!a_stop.stop_requested()) {
				CR_STORE_REL(&shm->skyrim_heartbeat, static_cast<std::uint32_t>(::GetTickCount64()));
				std::this_thread::sleep_for(200ms);
			}
		});

		cr_msg_hello hello{};
		hello.protocol_version = CR_PROTOCOL_VERSION;
		hello.pid = ::GetCurrentProcessId();
		std::snprintf(hello.build, sizeof(hello.build), "Chiefrim SKSE %s", "0.0.1");
		Push(CR_MSG_HELLO, &hello, sizeof(hello));

		logger::info("linked to Halo (pid {})", shm_->halo_pid);
		RE::SendHUDMessage::ShowHUDMessage("Chiefrim: linked to Halo");
		return true;
	}

	void Link::Close(const char* a_reason)
	{
		if (heartbeat_.joinable()) {
			heartbeat_.request_stop();
			heartbeat_.join();
		}
		if (shm_) {
			CR_STORE_REL(&shm_->skyrim_state, CR_SIDE_CLOSING);
			::UnmapViewOfFile(shm_);
			shm_ = nullptr;
		}
		if (mapping_) {
			::CloseHandle(mapping_);
			mapping_ = nullptr;
		}
		if (file_ != INVALID_HANDLE_VALUE) {
			::CloseHandle(file_);
			file_ = INVALID_HANDLE_VALUE;
		}
		if (a_reason) {
			logger::info("link closed: {}", a_reason);
			RE::SendHUDMessage::ShowHUDMessage("Chiefrim: Halo is gone");
		}
	}

	bool Link::Update()
	{
		const auto now = ::GetTickCount64();

		if (!shm_) {
			if (now < nextOpenAttempt_) {
				return false;
			}
			nextOpenAttempt_ = now + 1000;
			if (!TryOpen(now)) {
				return false;
			}
		}

		if (CR_LOAD_ACQ(&shm_->halo_state) == CR_SIDE_CLOSING) {
			Close("Halo closed");
			return false;
		}
		const auto heartbeat = CR_LOAD_ACQ(&shm_->halo_heartbeat);
		if (heartbeat != lastHaloHeartbeat_) {
			lastHaloHeartbeat_ = heartbeat;
			lastHaloHeartbeatChange_ = now;
		} else if (now - lastHaloHeartbeatChange_ > CR_HEARTBEAT_TIMEOUT_MS) {
			Close("Halo stopped responding");
			return false;
		}

		alignas(8) unsigned char buffer[256];
		int type;
		while ((type = cr_ring_pop(&shm_->to_skyrim, buffer, sizeof(buffer))) >= 0) {
			switch (type) {
			case CR_MSG_HELLO:
				{
					const auto* hello = reinterpret_cast<const cr_msg_hello*>(buffer);
					logger::info("Halo says hello: protocol {}, pid {}, {}", hello->protocol_version, hello->pid, hello->build);
					break;
				}
			case CR_MSG_LOG:
				logger::info("halo: {}", reinterpret_cast<const cr_msg_log*>(buffer)->text);
				break;
			default:
				break;
			}
		}
		return true;
	}

	void Link::Push(std::uint16_t a_type, const void* a_message, std::uint32_t a_size)
	{
		if (shm_ && !cr_ring_push(&shm_->to_halo, a_type, a_message, a_size)) {
			logger::warn("ring to Halo is full; dropped message {}", a_type);
		}
	}

	void Link::SendWorldContext(const cr_world_context& a_context)
	{
		if (shm_) {
			CR_SLOT_WRITE(&shm_->world_context, a_context);
		}
	}

	void Link::SendTeleport(const RE::NiPoint3& a_position, float a_heading)
	{
		cr_msg_teleport teleport{};
		teleport.position = { a_position.x, a_position.y, a_position.z };
		teleport.yaw = a_heading;
		Push(CR_MSG_TELEPORT, &teleport, sizeof(teleport));
	}

	std::optional<cr_player_state> Link::ReadPlayerState()
	{
		if (!shm_) {
			return std::nullopt;
		}
		cr_player_state state{};
		if (!CR_SLOT_READ(&shm_->player_state, &state)) {
			return std::nullopt;
		}
		return state;
	}
}
