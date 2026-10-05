/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <RE/S/SendHUDMessage.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stop_token>
#include <thread>

#include <Windows.h>

namespace logger = SKSE::log;
using namespace std::literals;
