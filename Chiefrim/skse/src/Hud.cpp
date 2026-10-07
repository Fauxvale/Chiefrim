/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Hud.h"
#include "Settings.h"

namespace chiefrim::Hud
{
	namespace
	{
		// hudmenu.swf's names (Skyrim - Interface.bsa)
		constexpr std::array kCrosshair{ "_root.HUDMovieBaseInstance.CrosshairInstance._visible" };
		constexpr std::array kBars{ "_root.HUDMovieBaseInstance.Health._visible", "_root.HUDMovieBaseInstance.Magica._visible",
			"_root.HUDMovieBaseInstance.Stamina._visible" };

		bool hideCrosshair = true;  // [HUD] bHideCrosshair
		bool hideBars = true;       // [HUD] bHideBars
		bool hidden = false;        // by us, to put back
		bool reported = false;

		// Each element's visibility, on Skyrim's HUD movie; false: no HUD (yet).
		bool Set(bool a_visible)
		{
			auto* ui = RE::UI::GetSingleton();
			auto  movie = ui ? ui->GetMovieView(RE::HUDMenu::MENU_NAME) : nullptr;
			if (!movie) {
				return false;
			}
			const RE::GFxValue value{ a_visible };
			int                missing = 0;
			const auto         apply = [&](const auto& a_paths) {
				for (const char* path : a_paths) {
					if (!movie->SetVariable(path, value, RE::GFxMovie::SetVarType::kNormal)) {
						++missing;
					}
				}
			};
			if (hideCrosshair) {
				apply(kCrosshair);
			}
			if (hideBars) {
				apply(kBars);
			}
			if (!std::exchange(reported, true)) {
				if (missing) {
					logger::warn("hud: {} of Skyrim's HUD elements not found (a HUD mod's own layout?); those stay", missing);
				} else {
					logger::info("hud: Skyrim's{}{} hidden while Chief plays", hideCrosshair ? " crosshair" : "",
						hideBars ? (hideCrosshair ? " and bars" : " bars") : "");
				}
			}
			return true;
		}
	}

	void Install()
	{
		hideCrosshair = Settings::ReadFloat(L"HUD", L"bHideCrosshair", 1.0f) != 0.0f;
		hideBars = Settings::ReadFloat(L"HUD", L"bHideBars", 1.0f) != 0.0f;
	}

	void Update()
	{
		if ((hideCrosshair || hideBars) && Set(false)) {
			hidden = true;
		}
	}

	void Restore()
	{
		if (std::exchange(hidden, false)) {
			Set(true);
		}
	}
}
