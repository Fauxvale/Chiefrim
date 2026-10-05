/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Input.h"
#include "Puppet.h"

namespace
{
	void SetupLog()
	{
		auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		auto path = *dir / "Chiefrim.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			chiefrim::Input::Install();
			chiefrim::Puppet::Install();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	logger::info("Chiefrim {} loading (runtime {})", "0.0.1", a_skse->RuntimeVersion().string());
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
