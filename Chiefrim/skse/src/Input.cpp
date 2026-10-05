/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Input.h"

#include "Link.h"

#include <array>
#include <string>
#include <vector>

namespace chiefrim::Input
{
	void PublishNeutral();

	namespace
	{
		// ---- configuration (Chiefrim.ini, next to the DLL)

		struct ActionBinding
		{
			std::uint32_t action;
			const wchar_t* iniKey;
			const char*    defaultEvents;  // Skyrim ControlMap user events, comma-separated
		};

		// Halo action <- Skyrim user event(s). Crouch isn't here: it follows
		// Skyrim's sneak state. Switch grenade and flashlight are hotkeys.
		constexpr std::array kBindings{
			ActionBinding{ CR_ACTION_JUMP, L"sJump", "Jump" },
			ActionBinding{ CR_ACTION_FIRE, L"sFire", "Right Attack/Block" },
			ActionBinding{ CR_ACTION_ZOOM, L"sZoom", "Left Attack/Block" },
			ActionBinding{ CR_ACTION_RELOAD, L"sReload", "Ready Weapon" },
			ActionBinding{ CR_ACTION_GRENADE, L"sThrowGrenade", "Shout" },
			ActionBinding{ CR_ACTION_MELEE, L"sMelee", "Toggle POV" },
			ActionBinding{ CR_ACTION_ACTION, L"sAction", "Activate" },
			ActionBinding{ CR_ACTION_SWITCH_WEAPON, L"sSwitchWeapon", "Zoom In,Zoom Out" },
		};

		constexpr std::array kActionNames{
			"jump", "crouch", "fire", "zoom", "reload", "throw grenade", "melee",
			"action", "switch weapon", "switch grenade", "flashlight"
		};
		static_assert(kActionNames.size() == CR_ACTION_COUNT);

		struct Hotkey
		{
			std::uint32_t action;
			std::uint32_t key;     // DirectInput scan code; 0 = none
			std::uint32_t button;  // gamepad button code; 0 = none
		};

		struct Config
		{
			std::vector<std::pair<std::string, std::uint32_t>> eventActions;  // user event -> action
			Hotkey switchGrenade{ CR_ACTION_SWITCH_GRENADE, 0x22, 0 };          // G
			Hotkey flashlight{ CR_ACTION_FLASHLIGHT, 0x2F, 0 };                 // V
			float  lookSensitivity{ 1.0f };   // times Halo's own mouse feel
			float  stickLookSpeed{ 3.0f };    // radians per second at full right-stick
		} config;

		std::wstring IniPath()
		{
			HMODULE self = nullptr;
			::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&IniPath), &self);
			wchar_t path[MAX_PATH]{};
			::GetModuleFileNameW(self, path, MAX_PATH);
			std::wstring result(path);
			const auto slash = result.find_last_of(L"\\/");
			return result.substr(0, slash + 1) + L"Chiefrim.ini";
		}

		std::string ReadString(const std::wstring& a_path, const wchar_t* a_key, const char* a_default)
		{
			wchar_t value[512]{};
			const std::wstring fallback(a_default, a_default + std::strlen(a_default));
			::GetPrivateProfileStringW(L"Controls", a_key, fallback.c_str(), value, 512, a_path.c_str());
			std::string narrow;
			for (const wchar_t* c = value; *c; ++c) {
				narrow.push_back(static_cast<char>(*c));
			}
			return narrow;
		}

		std::uint32_t ReadUInt(const std::wstring& a_path, const wchar_t* a_key, std::uint32_t a_default)
		{
			return static_cast<std::uint32_t>(::GetPrivateProfileIntW(L"Controls", a_key, static_cast<INT>(a_default), a_path.c_str()));
		}

		float ReadFloat(const std::wstring& a_path, const wchar_t* a_key, float a_default)
		{
			const auto text = ReadString(a_path, a_key, "");
			return text.empty() ? a_default : std::strtof(text.c_str(), nullptr);
		}

		std::string Trim(std::string a_text)
		{
			const auto first = a_text.find_first_not_of(" \t");
			const auto last = a_text.find_last_not_of(" \t");
			return first == std::string::npos ? std::string{} : a_text.substr(first, last - first + 1);
		}

		void LoadConfig()
		{
			const auto path = IniPath();
			config.eventActions.clear();
			for (const auto& binding : kBindings) {
				std::string list = ReadString(path, binding.iniKey, binding.defaultEvents);
				std::size_t start = 0;
				while (start <= list.size()) {
					const auto comma = list.find(',', start);
					auto name = Trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
					if (!name.empty()) {
						config.eventActions.emplace_back(std::move(name), binding.action);
					}
					if (comma == std::string::npos) {
						break;
					}
					start = comma + 1;
				}
			}
			config.switchGrenade.key = ReadUInt(path, L"iSwitchGrenadeKey", 0x22);
			config.switchGrenade.button = ReadUInt(path, L"iSwitchGrenadeButton", 0);
			config.flashlight.key = ReadUInt(path, L"iFlashlightKey", 0x2F);
			config.flashlight.button = ReadUInt(path, L"iFlashlightButton", 0);
			config.lookSensitivity = ReadFloat(path, L"fLookSensitivity", 1.0f);
			config.stickLookSpeed = ReadFloat(path, L"fStickLookSpeed", 3.0f);
			logger::info("controls: {} Skyrim user events mapped; switch grenade key 0x{:02X}, flashlight key 0x{:02X}, look x{:.2f}",
				config.eventActions.size(), config.switchGrenade.key, config.flashlight.key, config.lookSensitivity);
		}

		// ---- Skyrim's settings

		float SkyrimFloat(const char* a_name, float a_default)
		{
			if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
				if (auto* setting = prefs->GetSetting(a_name)) {
					return setting->GetFloat();
				}
			}
			if (auto* ini = RE::INISettingCollection::GetSingleton()) {
				if (auto* setting = ini->GetSetting(a_name)) {
					return setting->GetFloat();
				}
			}
			return a_default;
		}

		bool SkyrimBool(const char* a_name, bool a_default)
		{
			if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
				if (auto* setting = prefs->GetSetting(a_name)) {
					return setting->GetBool();
				}
			}
			if (auto* ini = RE::INISettingCollection::GetSingleton()) {
				if (auto* setting = ini->GetSetting(a_name)) {
					return setting->GetBool();
				}
			}
			return a_default;
		}

		// ---- state, all on the main thread (input events and PlayerCharacter::Update)

		struct State
		{
			bool          gameplay{ false };   // routing: Chief takes input
			bool          forward{}, back{}, left{}, right{};
			float         moveX{}, moveY{};    // Move stick
			float         lookX{}, lookY{};    // Look stick
			std::uint32_t held{ 0 };
			std::array<std::uint8_t, CR_ACTION_SLOTS> presses{};
			double        yawTotal{ 0.0 };
			double        pitchTotal{ 0.0 };
			std::uint32_t session{ 0 };
			std::uint32_t frame{ 0 };
			LARGE_INTEGER lastPublish{};
			bool          handlersOff{ false };
			std::array<bool, 11> savedHandlerStates{};
		} s;

		void Press(std::uint32_t a_action, bool a_down, bool a_pressed)
		{
			const std::uint32_t bit = 1u << a_action;
			if (a_down) {
				if (!(s.held & bit)) {
					++s.presses[a_action];
				}
				s.held |= bit;
			} else {
				s.held &= ~bit;
			}
			(void)a_pressed;
		}

		void ReleaseAll()
		{
			s.forward = s.back = s.left = s.right = false;
			s.moveX = s.moveY = s.lookX = s.lookY = 0.0f;
			s.held = 0;
		}

		bool IsEvent(const RE::BSFixedString& a_event, const RE::BSFixedString& a_name)
		{
			return !a_name.empty() && a_event == a_name;
		}

		// Look: mouse counts scaled like Halo's own mouse (0.0022 rad per
		// count), by Skyrim's mouse sensitivity relative to its default, and
		// fLookSensitivity. Invert-Y follows Skyrim.
		void Look(float a_dx, float a_dy)
		{
			const float skyrimSensitivity = SkyrimFloat("fMouseHeadingSensitivity:Controls", 0.0125f) / 0.0125f;
			const float scale = 0.0022f * skyrimSensitivity * config.lookSensitivity;
			const bool  invert = SkyrimBool("bInvertYValues:Controls", false);
			s.yawTotal += static_cast<double>(a_dx * scale);
			s.pitchTotal += static_cast<double>((invert ? a_dy : -a_dy) * scale);
		}

		class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* Get()
			{
				static InputSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_event || !s.gameplay || !Link::Get().Connected()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* events = RE::UserEvents::GetSingleton();
				for (auto* event = *a_event; event; event = event->next) {
					switch (event->GetEventType()) {
					case RE::INPUT_EVENT_TYPE::kButton:
						OnButton(*event->AsButtonEvent(), *events);
						break;
					case RE::INPUT_EVENT_TYPE::kMouseMove:
						{
							const auto* move = static_cast<RE::MouseMoveEvent*>(event);
							if (IsEvent(move->QUserEvent(), events->look)) {
								Look(static_cast<float>(move->mouseInputX), static_cast<float>(move->mouseInputY));
							}
							break;
						}
					case RE::INPUT_EVENT_TYPE::kThumbstick:
						{
							const auto* stick = static_cast<RE::ThumbstickEvent*>(event);
							if (IsEvent(stick->QUserEvent(), events->move)) {
								s.moveX = stick->xValue;
								s.moveY = stick->yValue;
							} else if (IsEvent(stick->QUserEvent(), events->look)) {
								s.lookX = stick->xValue;
								s.lookY = stick->yValue;
							}
							break;
						}
					default:
						break;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			static void OnButton(const RE::ButtonEvent& a_button, const RE::UserEvents& a_events)
			{
				const bool  down = a_button.IsPressed();
				const auto& name = a_button.QUserEvent();

				// Chiefrim's own hotkeys: raw keys (the only ones).
				const auto device = a_button.GetDevice();
				for (const auto* hotkey : { &config.switchGrenade, &config.flashlight }) {
					const bool match =
						(device == RE::INPUT_DEVICE::kKeyboard && hotkey->key && a_button.GetIDCode() == hotkey->key) ||
						(device == RE::INPUT_DEVICE::kGamepad && hotkey->button && a_button.GetIDCode() == hotkey->button);
					if (match) {
						Press(hotkey->action, down, a_button.IsDown());
					}
				}

				if (name.empty()) {
					return;
				}
				// Skyrim's movement, as Skyrim maps it.
				if (IsEvent(name, a_events.forward)) {
					s.forward = down;
				} else if (IsEvent(name, a_events.back)) {
					s.back = down;
				} else if (IsEvent(name, a_events.strafeLeft)) {
					s.left = down;
				} else if (IsEvent(name, a_events.strafeRight)) {
					s.right = down;
				}

				// Chief's actions, by the user event Skyrim resolved. A wheel
				// notch has no release: it's a tap.
				constexpr std::uint32_t kWheelUp = 8, kWheelDown = 9;
				const bool wheel = device == RE::INPUT_DEVICE::kMouse &&
					(a_button.GetIDCode() == kWheelUp || a_button.GetIDCode() == kWheelDown);
				const std::string_view event{ name.c_str() };
				for (const auto& [eventName, action] : config.eventActions) {
					if (event == eventName) {
						Press(action, down, a_button.IsDown());
						if (wheel) {
							Press(action, false, false);
						}
					}
				}
			}
		};

		// The Skyrim handlers Chief takes over while linked. Activate and Sneak
		// stay Skyrim's (doors and NPCs; stealth).
		std::array<RE::PlayerInputHandler*, 11> ChiefHandlers()
		{
			auto* controls = RE::PlayerControls::GetSingleton();
			if (!controls) {
				return {};
			}
			return {
				controls->movementHandler, controls->lookHandler, controls->sprintHandler,
				controls->readyWeaponHandler, controls->autoMoveHandler, controls->toggleRunHandler,
				controls->jumpHandler, controls->shoutHandler, controls->attackBlockHandler,
				controls->runHandler, controls->togglePOVHandler
			};
		}

		void SetSkyrimHandlers(bool a_chiefHasThem)
		{
			if (a_chiefHasThem == s.handlersOff) {
				return;
			}
			auto handlers = ChiefHandlers();
			for (std::size_t i = 0; i < handlers.size(); ++i) {
				auto* handler = handlers[i];
				if (!handler) {
					continue;
				}
				if (a_chiefHasThem) {
					s.savedHandlerStates[i] = handler->IsInputEventHandlingEnabled();
					handler->SetInputEventHandlingEnabled(false);
				} else {
					handler->SetInputEventHandlingEnabled(s.savedHandlerStates[i]);
				}
			}
			s.handlersOff = a_chiefHasThem;
			logger::info("Skyrim's movement, look, jump, shout, attack, ready weapon and POV handlers {}",
				a_chiefHasThem ? "off: Chief has them" : "back on");
		}

		bool InGameplay()
		{
			auto* ui = RE::UI::GetSingleton();
			auto* map = RE::ControlMap::GetSingleton();
			if (!ui || !map || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				return false;
			}
			const auto& stack = map->GetRuntimeData().contextPriorityStack;
			return !stack.empty() && stack.back() == RE::UserEvents::INPUT_CONTEXT_ID::kGameplay;
		}

		// Logs the keys Chief's actions are bound to now, and warns when a
		// Chiefrim hotkey is also a Skyrim gameplay key.
		void LogBindings()
		{
			auto* map = RE::ControlMap::GetSingleton();
			if (!map) {
				return;
			}
			const auto key = [map](std::string_view a_event, RE::INPUT_DEVICE a_device) {
				return map->GetMappedKey(a_event, a_device, RE::UserEvents::INPUT_CONTEXT_ID::kGameplay);
			};
			for (const char* move : { "Forward", "Back", "Strafe Left", "Strafe Right", "Sneak" }) {
				logger::info("  Skyrim {:<12} keyboard 0x{:02X}  gamepad 0x{:04X}", move,
					key(move, RE::INPUT_DEVICE::kKeyboard), key(move, RE::INPUT_DEVICE::kGamepad));
			}
			for (const auto& [eventName, action] : config.eventActions) {
				logger::info("  Chief {:<14} <- Skyrim \"{}\": keyboard 0x{:02X}  mouse 0x{:02X}  gamepad 0x{:04X}",
					kActionNames[action], eventName, key(eventName, RE::INPUT_DEVICE::kKeyboard),
					key(eventName, RE::INPUT_DEVICE::kMouse), key(eventName, RE::INPUT_DEVICE::kGamepad));
			}
			for (const auto* hotkey : { &config.switchGrenade, &config.flashlight }) {
				if (!hotkey->key) {
					continue;
				}
				const auto clash = map->GetUserEventName(hotkey->key, RE::INPUT_DEVICE::kKeyboard,
					RE::UserEvents::INPUT_CONTEXT_ID::kGameplay);
				if (!clash.empty()) {
					logger::warn("Chiefrim's {} key 0x{:02X} is also Skyrim's \"{}\"; change it in Chiefrim.ini",
						kActionNames[hotkey->action], hotkey->key, clash);
					const auto message = std::format("Chiefrim: the {} key is also Skyrim's {}",
						kActionNames[hotkey->action], clash);
					RE::SendHUDMessage::ShowHUDMessage(message.c_str());
				} else {
					logger::info("  Chief {:<14} <- Chiefrim hotkey 0x{:02X}", kActionNames[hotkey->action], hotkey->key);
				}
			}
		}

		// The Controls page lives in the Journal menu: re-read the bindings when it closes.
		class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuSink* Get()
			{
				static MenuSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || !Link::Get().Connected()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->opening) {
					PublishNeutral();
				} else if (a_event->menuName == RE::JournalMenu::MENU_NAME) {
					logger::info("Journal menu closed; Chief's controls now:");
					LogBindings();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		LoadConfig();
		if (auto* devices = RE::BSInputDeviceManager::GetSingleton()) {
			devices->AddEventSink(InputSink::Get());
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
		}
		logger::info("input bridge installed (Skyrim user events -> Chief)");
	}

	void PublishNeutral()
	{
		cr_input input{};
		input.frame = ++s.frame;
		input.session = s.session;
		input.routing = CR_ROUTE_SKYRIM;
		std::memcpy(input.presses, s.presses.data(), sizeof(input.presses));
		input.yaw_total = s.yawTotal;
		input.pitch_total = s.pitchTotal;
		Link::Get().SendInput(input);
	}

	void OnLinked()
	{
		ReleaseAll();
		s.yawTotal = s.pitchTotal = 0.0;
		++s.session;
		::QueryPerformanceCounter(&s.lastPublish);
		logger::info("Chief's controls:");
		LogBindings();
	}

	void OnUnlinked()
	{
		ReleaseAll();
		s.gameplay = false;
		SetSkyrimHandlers(false);
	}

	void Publish(RE::PlayerCharacter* a_player)
	{
		LARGE_INTEGER now{}, frequency{};
		::QueryPerformanceCounter(&now);
		::QueryPerformanceFrequency(&frequency);
		const double dt = s.lastPublish.QuadPart ?
			std::clamp(double(now.QuadPart - s.lastPublish.QuadPart) / double(frequency.QuadPart), 0.0, 0.25) : 0.0;
		s.lastPublish = now;

		const bool gameplay = InGameplay();
		if (gameplay != s.gameplay) {
			ReleaseAll();  // nothing stays held across a menu
			s.gameplay = gameplay;
		}
		SetSkyrimHandlers(true);

		cr_input input{};
		input.frame = ++s.frame;
		input.session = s.session;
		input.routing = gameplay ? CR_ROUTE_HALO : CR_ROUTE_SKYRIM;
		if (gameplay) {
			// Right stick: a turn rate, integrated here.
			const bool invert = SkyrimBool("bInvertYValues:Controls", false);
			s.yawTotal += s.lookX * config.stickLookSpeed * dt;
			s.pitchTotal += (invert ? -s.lookY : s.lookY) * config.stickLookSpeed * dt;

			float x = (s.right ? 1.0f : 0.0f) - (s.left ? 1.0f : 0.0f);
			float y = (s.forward ? 1.0f : 0.0f) - (s.back ? 1.0f : 0.0f);
			if (x != 0.0f && y != 0.0f) {
				x *= 0.70710678f;
				y *= 0.70710678f;
			}
			if (std::fabs(s.moveX) > std::fabs(x)) {
				x = s.moveX;
			}
			if (std::fabs(s.moveY) > std::fabs(y)) {
				y = s.moveY;
			}
			input.forward = y;
			input.strafe = x;

			input.held = s.held;
			// Crouch is Skyrim's sneak state, however Sneak is bound or toggled.
			if (a_player->IsSneaking()) {
				input.held |= 1u << CR_ACTION_CROUCH;
			}
		}
		std::memcpy(input.presses, s.presses.data(), sizeof(input.presses));
		input.yaw_total = s.yawTotal;
		input.pitch_total = s.pitchTotal;
		Link::Get().SendInput(input);
	}
}
