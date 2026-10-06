/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <string>

namespace chiefrim::Settings
{
	// Chiefrim.ini, next to Chiefrim.dll (Data/SKSE/Plugins).
	inline std::wstring IniPath()
	{
		HMODULE self = nullptr;
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&IniPath), &self);
		wchar_t path[MAX_PATH]{};
		::GetModuleFileNameW(self, path, MAX_PATH);
		std::wstring result(path);
		return result.substr(0, result.find_last_of(L"\\/") + 1) + L"Chiefrim.ini";
	}

	inline float ReadFloat(const wchar_t* a_section, const wchar_t* a_key, float a_default)
	{
		wchar_t value[64]{};
		::GetPrivateProfileStringW(a_section, a_key, L"", value, 64, IniPath().c_str());
		return value[0] ? std::wcstof(value, nullptr) : a_default;
	}

	inline bool ReadBool(const wchar_t* a_section, const wchar_t* a_key, bool a_default)
	{
		return ::GetPrivateProfileIntW(a_section, a_key, a_default ? 1 : 0, IniPath().c_str()) != 0;
	}

	// [Movement] bSkyrimMoves: Skyrim's player walks, jumps and collides with
	// Skyrim's own controller, and Halo's Chief follows it (docs §7). 0: Halo
	// moves Chief and the player follows him (the first design).
	// [Collision] iRadius: regions (1024 units) around Chief's that Halo's
	// collision covers, so his shots hit at least that far (docs §5.2). Halo
	// builds from (2r+1)^2 regions; Skyrim sends one ring more.
	inline std::uint32_t CollisionRadius()
	{
		static const std::uint32_t value = std::clamp<std::uint32_t>(
			::GetPrivateProfileIntW(L"Collision", L"iRadius", 2, IniPath().c_str()), 1, 3);
		return value;
	}

	inline bool SkyrimMoves()
	{
		static const bool value = ReadBool(L"Movement", L"bSkyrimMoves", true);
		return value;
	}
}
