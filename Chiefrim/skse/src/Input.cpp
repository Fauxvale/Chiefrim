/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Input.h"

#include "Handoff.h"
#include "Link.h"
#include "Settings.h"

#include <array>
#include <string>
#include <vector>

namespace chiefrim::Input
{
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
			ActionBinding{ CR_ACTION_FIRE, L"sFire", "Left Attack/Block" },    // left mouse button
			ActionBinding{ CR_ACTION_ZOOM, L"sZoom", "Right Attack/Block" },   // right mouse button
			ActionBinding{ CR_ACTION_RELOAD, L"sReload", "Ready Weapon" },
			ActionBinding{ CR_ACTION_GRENADE, L"sThrowGrenade", "Shout" },
			ActionBinding{ CR_ACTION_MELEE, L"sMelee", "Toggle POV" },
			ActionBinding{ CR_ACTION_ACTION, L"sAction", "Activate" },
			ActionBinding{ CR_ACTION_SWITCH_WEAPON, L"sSwitchWeapon", "Zoom In,Zoom Out" },
		};

		constexpr std::array kActionNames{
			"jump", "crouch", "fire", "zoom", "reload", "throw grenade", "melee",
			"action", "switch weapon", "switch grenade", "flashlight", "mark stuck"
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
			Hotkey mark{ CR_ACTION_MARK, 0x42, 0 };                             // F8
			float  lookSensitivity{ 1.0f };   // times Halo's own mouse feel
			float  stickLookSpeed{ 3.0f };    // radians per second at full right-stick
		} config;

		std::wstring IniPath() { return Settings::IniPath(); }

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
			config.mark.key = ReadUInt(path, L"iMarkStuckKey", 0x42);
			config.mark.button = ReadUInt(path, L"iMarkStuckButton", 0);
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
			bool          gamepad{ false };    // the player last played with a gamepad
			cr_msg_key_names keyNames{};       // as last sent to Halo
			bool          keyNamesSent{ false };
			ULONGLONG     nextKeyNames{ 0 };
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

		// ---- the player's keys, named, for Halo's prompts (CR_MSG_KEY_NAMES)

		std::string KeyName(RE::INPUT_DEVICE a_device, std::uint32_t a_id)
		{
			switch (a_device) {
			case RE::INPUT_DEVICE::kKeyboard:
				{
					// Skyrim's keyboard codes are DirectInput scan codes (a US layout's
					// names, as Skyrim's Controls menu shows them)
					static constexpr std::array<std::pair<std::uint32_t, const char*>, 103> names{ {
						{ 0x01, "Esc" }, { 0x02, "1" }, { 0x03, "2" }, { 0x04, "3" }, { 0x05, "4" }, { 0x06, "5" }, { 0x07, "6" },
						{ 0x08, "7" }, { 0x09, "8" }, { 0x0A, "9" }, { 0x0B, "0" }, { 0x0C, "-" }, { 0x0D, "=" }, { 0x0E, "Backspace" },
						{ 0x0F, "Tab" }, { 0x10, "Q" }, { 0x11, "W" }, { 0x12, "E" }, { 0x13, "R" }, { 0x14, "T" }, { 0x15, "Y" },
						{ 0x16, "U" }, { 0x17, "I" }, { 0x18, "O" }, { 0x19, "P" }, { 0x1A, "[" }, { 0x1B, "]" }, { 0x1C, "Enter" },
						{ 0x1D, "Left Ctrl" }, { 0x1E, "A" }, { 0x1F, "S" }, { 0x20, "D" }, { 0x21, "F" }, { 0x22, "G" }, { 0x23, "H" },
						{ 0x24, "J" }, { 0x25, "K" }, { 0x26, "L" }, { 0x27, ";" }, { 0x28, "'" }, { 0x29, "`" }, { 0x2A, "Left Shift" },
						{ 0x2B, "\\" }, { 0x2C, "Z" }, { 0x2D, "X" }, { 0x2E, "C" }, { 0x2F, "V" }, { 0x30, "B" }, { 0x31, "N" },
						{ 0x32, "M" }, { 0x33, "," }, { 0x34, "." }, { 0x35, "/" }, { 0x36, "Right Shift" }, { 0x37, "Num *" },
						{ 0x38, "Left Alt" }, { 0x39, "Space" }, { 0x3A, "Caps Lock" }, { 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" },
						{ 0x3E, "F4" }, { 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" }, { 0x42, "F8" }, { 0x43, "F9" }, { 0x44, "F10" },
						{ 0x45, "Num Lock" }, { 0x46, "Scroll Lock" }, { 0x47, "Num 7" }, { 0x48, "Num 8" }, { 0x49, "Num 9" },
						{ 0x4A, "Num -" }, { 0x4B, "Num 4" }, { 0x4C, "Num 5" }, { 0x4D, "Num 6" }, { 0x4E, "Num +" }, { 0x4F, "Num 1" },
						{ 0x50, "Num 2" }, { 0x51, "Num 3" }, { 0x52, "Num 0" }, { 0x53, "Num ." }, { 0x57, "F11" }, { 0x58, "F12" },
						{ 0x9C, "Num Enter" }, { 0x9D, "Right Ctrl" }, { 0xB5, "Num /" }, { 0xB8, "Right Alt" }, { 0xC5, "Pause" },
						{ 0xC7, "Home" }, { 0xC8, "Up" }, { 0xC9, "Page Up" }, { 0xCB, "Left" }, { 0xCD, "Right" }, { 0xCF, "End" },
						{ 0xD0, "Down" }, { 0xD1, "Page Down" }, { 0xD2, "Insert" }, { 0xD3, "Delete" }, { 0xDB, "Left Win" },
						{ 0xDC, "Right Win" }, { 0xB7, "Print Screen" } } };
					for (const auto& [code, keyName] : names) {
						if (code == a_id) {
							return keyName;
						}
					}
					// anything else: Windows' own name for the scan code
					char name[32]{};
					const LONG lparam = static_cast<LONG>(((a_id & 0x7F) << 16) | ((a_id & 0x80) ? (1 << 24) : 0));
					if (::GetKeyNameTextA(lparam, name, sizeof(name)) > 0) {
						return name;
					}
					return std::format("Key {:02X}", a_id);
				}
			case RE::INPUT_DEVICE::kMouse:
				{
					constexpr std::array names{ "Left Mouse", "Right Mouse", "Middle Mouse", "Mouse 4", "Mouse 5", "Mouse 6",
						"Mouse 7", "Mouse 8", "Mouse Wheel", "Mouse Wheel" };
					return a_id < names.size() ? names[a_id] : std::format("Mouse {}", a_id + 1);
				}
			case RE::INPUT_DEVICE::kGamepad:
				{
					// Skyrim's gamepad codes: XInput's button bits, the triggers 9 and 10
					constexpr std::array<std::pair<std::uint32_t, const char*>, 16> names{ {
						{ 0x0001, "D-Pad Up" }, { 0x0002, "D-Pad Down" }, { 0x0004, "D-Pad Left" }, { 0x0008, "D-Pad Right" },
						{ 0x0010, "Start" }, { 0x0020, "Back" }, { 0x0040, "LS" }, { 0x0080, "RS" },
						{ 0x0100, "LB" }, { 0x0200, "RB" }, { 0x1000, "A" }, { 0x2000, "B" },
						{ 0x4000, "X" }, { 0x8000, "Y" }, { 0x0009, "LT" }, { 0x000A, "RT" } } };
					for (const auto& [code, name] : names) {
						if (code == a_id) {
							return name;
						}
					}
					return std::format("Button {:X}", a_id);
				}
			default:
				return {};
			}
		}

		// What does a user event on the device the player is using (a key, or
		// for the keyboard-and-mouse player, a mouse button); empty: unbound.
		std::string EventKeyName(RE::ControlMap* a_map, std::string_view a_event)
		{
			const auto mapped = [&](RE::INPUT_DEVICE a_device) {
				return a_map->GetMappedKey(a_event, a_device, RE::UserEvents::INPUT_CONTEXT_ID::kGameplay);
			};
			if (s.gamepad) {
				const auto key = mapped(RE::INPUT_DEVICE::kGamepad);
				return key != RE::ControlMap::kInvalid ? KeyName(RE::INPUT_DEVICE::kGamepad, key) : std::string{};
			}
			for (const auto device : { RE::INPUT_DEVICE::kKeyboard, RE::INPUT_DEVICE::kMouse }) {
				if (const auto key = mapped(device); key != RE::ControlMap::kInvalid) {
					return KeyName(device, key);
				}
			}
			return {};
		}

		// Twice a second, and on linking: the names, sent when they change
		// (a rebind, or the player switching between gamepad and keyboard).
		void SendKeyNames()
		{
			const auto now = ::GetTickCount64();
			if (s.keyNamesSent && now < s.nextKeyNames) {
				return;
			}
			s.nextKeyNames = now + 500;
			auto* map = RE::ControlMap::GetSingleton();
			if (!map) {
				return;
			}
			std::array<std::string, CR_ACTION_COUNT> names;
			for (const auto& [eventName, action] : config.eventActions) {
				if (names[action].empty()) {
					names[action] = EventKeyName(map, eventName);
				}
			}
			names[CR_ACTION_CROUCH] = EventKeyName(map, "Sneak");
			for (const auto* hotkey : { &config.switchGrenade, &config.flashlight }) {
				if (s.gamepad ? hotkey->button != 0 : hotkey->key != 0) {
					names[hotkey->action] = s.gamepad ? KeyName(RE::INPUT_DEVICE::kGamepad, hotkey->button) :
					                                    KeyName(RE::INPUT_DEVICE::kKeyboard, hotkey->key);
				}
			}
			cr_msg_key_names message{};
			for (std::size_t action = 0; action < CR_ACTION_COUNT; ++action) {
				std::strncpy(message.names[action], names[action].c_str(), CR_KEY_NAME_LENGTH - 1);
			}
			if (s.keyNamesSent && std::memcmp(message.names, s.keyNames.names, sizeof(message.names)) == 0) {
				return;
			}
			if (Link::Get().PushRaw(CR_MSG_KEY_NAMES, &message, sizeof(message))) {
				s.keyNames = message;
				s.keyNamesSent = true;
				logger::info("Halo's prompts name the {} keys: action {}, reload {}, fire {}, grenade {}",
					s.gamepad ? "gamepad" : "keyboard and mouse", message.names[CR_ACTION_ACTION], message.names[CR_ACTION_RELOAD],
					message.names[CR_ACTION_FIRE], message.names[CR_ACTION_GRENADE]);
			}
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
							s.gamepad = false;
							if (IsEvent(move->QUserEvent(), events->look)) {
								Look(static_cast<float>(move->mouseInputX), static_cast<float>(move->mouseInputY));
							}
							break;
						}
					case RE::INPUT_EVENT_TYPE::kThumbstick:
						{
							const auto* stick = static_cast<RE::ThumbstickEvent*>(event);
							if (std::fabs(stick->xValue) + std::fabs(stick->yValue) > 0.3f) {
								s.gamepad = true;
							}
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
				s.gamepad = device == RE::INPUT_DEVICE::kGamepad;
				for (const auto* hotkey : { &config.switchGrenade, &config.flashlight, &config.mark }) {
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
			if (Settings::SkyrimMoves()) {
				// Skyrim's player moves itself: Chief takes only the actions that
				// are Halo's (fire and zoom, reload, grenade, melee)
				return {
					nullptr, nullptr, nullptr,
					controls->readyWeaponHandler, nullptr, nullptr,
					nullptr, controls->shoutHandler, controls->attackBlockHandler,
					nullptr, controls->togglePOVHandler
				};
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
					if (!handler->IsInputEventHandlingEnabled()) {
						static constexpr std::array kNames{ "movement", "look", "sprint", "ready weapon", "auto move",
							"toggle run", "jump", "shout", "attack/block", "run", "toggle POV" };
						logger::info("Skyrim's {} handler was already off when Chief took over", kNames[i]);
					}
					handler->SetInputEventHandlingEnabled(false);
				} else {
					// On, whatever it was when Chief took over: taken over right
					// after a load, jump and sprint were briefly off, and putting
					// that back left the player unable to jump or sprint.
					handler->SetInputEventHandlingEnabled(true);
				}
			}
			s.handlersOff = a_chiefHasThem;
			logger::info("Skyrim's {} handlers {}",
				Settings::SkyrimMoves() ? "shout, attack, ready weapon and POV" : "movement, look, jump, shout, attack, ready weapon and POV",
				a_chiefHasThem ? "off: Chief has them" : "back on");
		}

		// Skyrim's controls kept something of their state while Chief had
		// them (after unlinking, the player walked but couldn't jump or
		// sprint, until the game was paused and unpaused): play them what
		// pausing does, the journal menu opening and closing.
		void ResetPlayerControls()
		{
			auto* controls = RE::PlayerControls::GetSingleton();
			auto* ui = RE::UI::GetSingleton();
			if (!controls || !ui) {
				return;
			}
			auto* menuSink = static_cast<RE::BSTEventSink<RE::MenuOpenCloseEvent>*>(controls);
			auto* modeSink = static_cast<RE::BSTEventSink<RE::MenuModeChangeEvent>*>(controls);
			auto* menuSource = static_cast<RE::BSTEventSource<RE::MenuOpenCloseEvent>*>(ui);
			auto* modeSource = static_cast<RE::BSTEventSource<RE::MenuModeChangeEvent>*>(ui);
			for (const bool opening : { true, false }) {
				RE::MenuOpenCloseEvent menu{};
				menu.menuName = RE::JournalMenu::MENU_NAME;
				menu.opening = opening;
				menuSink->ProcessEvent(&menu, menuSource);
				RE::MenuModeChangeEvent mode{};
				mode.menu = RE::JournalMenu::MENU_NAME;
				mode.mode = opening ? RE::MenuModeChangeEvent::Mode::kDisplayed : RE::MenuModeChangeEvent::Mode::kHidden;
				modeSink->ProcessEvent(&mode, modeSource);
			}
			logger::info("Skyrim's player controls reset (as pausing does)");
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
			for (const auto* hotkey : { &config.switchGrenade, &config.flashlight, &config.mark }) {
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
		s.keyNamesSent = false;
		logger::info("Chief's controls:");
		LogBindings();
	}

	void OnUnlinked()
	{
		ReleaseAll();
		s.gameplay = false;
		SetSkyrimHandlers(false);
		ResetPlayerControls();
	}

	void Publish(RE::PlayerCharacter* a_player)
	{
		LARGE_INTEGER now{}, frequency{};
		::QueryPerformanceCounter(&now);
		::QueryPerformanceFrequency(&frequency);
		const double dt = s.lastPublish.QuadPart ?
			std::clamp(double(now.QuadPart - s.lastPublish.QuadPart) / double(frequency.QuadPart), 0.0, 0.25) : 0.0;
		s.lastPublish = now;

		// handed off (docs §11), Skyrim's controls are its own, and Chief gets none
		const bool handedOff = Handoff::Active();
		const bool gameplay = InGameplay() && !handedOff;
		if (gameplay != s.gameplay) {
			ReleaseAll();  // nothing stays held across a menu
			s.gameplay = gameplay;
		}
		SetSkyrimHandlers(!handedOff);

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
		SendKeyNames();
	}
}
