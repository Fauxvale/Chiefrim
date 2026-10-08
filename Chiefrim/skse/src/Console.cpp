/* SPDX-License-Identifier: GPL-3.0-or-later */
// The "chiefrim" console command. SKSE has no way to add a console command
// in Skyrim SE/AE, so, as other plugins do, one of the game's developer
// commands that does nothing for players is renamed and given this handler.
// The typed line is read whole (Script::GetCommand), so a weapon's name may
// have spaces.
#include "Console.h"

#include "Link.h"

#include <sstream>
#include <vector>

namespace chiefrim::Console
{
	namespace
	{
		constexpr const char* kName = "chiefrim";
		constexpr const char* kHelp = "Chiefrim: chiefrim restart | on | off | toggle | give [name] | weapons | shapes [on|off]";

		// Developer commands no player needs, the first found taken (another
		// mod may have taken one already: then it isn't found by this name)
		constexpr const char* kCandidates[] = { "TestSeenData", "TestLocalMap", "ShowRenderPasses", "DumpNiUpdates" };

		std::uint32_t debugFlags = 0;  // CR_DEBUG_*

		void Print(const std::string& a_text)
		{
			if (auto* console = RE::ConsoleLog::GetSingleton()) {
				console->Print("%s", a_text.c_str());
			}
		}

		std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return a_text;
		}

		bool Linked()
		{
			if (Link::Get().Connected()) {
				return true;
			}
			Print(Link::Get().Enabled() ? "Chiefrim: Halo isn't linked yet" : "Chiefrim is off; chiefrim on starts it");
			return false;
		}

		void SendDebug()
		{
			cr_msg_debug debug{};
			debug.flags = debugFlags;
			if (Link::Get().Connected()) {
				Link::Get().PushRaw(CR_MSG_DEBUG, &debug, sizeof(debug));
			}
		}

		void Run(const std::vector<std::string>& a_words)
		{
			const auto verb = a_words.size() > 1 ? Lower(a_words[1]) : std::string();
			std::string rest;
			for (std::size_t i = 2; i < a_words.size(); ++i) {
				rest += (rest.empty() ? "" : " ") + a_words[i];
			}
			logger::info("console: {} {}", verb, rest);

			if (verb == "restart") {
				if (!Link::Get().Enabled()) {
					Print("Chiefrim is off; chiefrim on starts it");
					return;
				}
				Link::Get().RequestRestart();
				Print("Chiefrim: restarting Halo");
			} else if (verb == "on" || verb == "off" || verb == "toggle") {
				const bool want = verb == "toggle" ? !Link::Get().Enabled() : verb == "on";
				if (want == Link::Get().Enabled()) {
					Print(want ? "Chiefrim is already on" : "Chiefrim is already off");
					return;
				}
				Link::Get().RequestToggle();
				Print(want ? "Chiefrim: on, starting Halo" : "Chiefrim: off");
			} else if (verb == "give") {
				if (!Linked()) {
					return;
				}
				cr_msg_give_weapon give{};
				give.index = -1;
				const bool number = !rest.empty() && std::all_of(rest.begin(), rest.end(), [](unsigned char a_c) { return std::isdigit(a_c); });
				if (number) {
					give.index = std::atoi(rest.c_str());
				} else if (rest.size() >= CR_WEAPON_NAME_LENGTH) {
					Print("Chiefrim: that name is too long");
					return;
				} else {
					std::memcpy(give.name, rest.data(), rest.size());
				}
				Link::Get().PushRaw(CR_MSG_GIVE_WEAPON, &give, sizeof(give));
			} else if (verb == "weapons") {
				if (!Linked()) {
					return;
				}
				cr_msg_give_weapon list{};
				list.index = -1;
				list.flags = CR_GIVE_LIST;
				Link::Get().PushRaw(CR_MSG_GIVE_WEAPON, &list, sizeof(list));
			} else if (verb == "shapes") {
				const auto how = Lower(rest);
				const bool show = how == "on" ? true : how == "off" ? false : !(debugFlags & CR_DEBUG_HITBOXES);
				debugFlags = show ? (debugFlags | CR_DEBUG_HITBOXES) : (debugFlags & ~CR_DEBUG_HITBOXES);
				SendDebug();
				Print(show ? "Chiefrim: hit shapes shown (yellow; a person's head red; from bounds, cyan; no shapes: white)" :
				             "Chiefrim: hit shapes hidden");
			} else {
				Print(kHelp);
			}
		}

		bool Execute(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*, RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script* a_script,
			RE::ScriptLocals*, double&, std::uint32_t&)
		{
			if (!a_script) {
				return true;
			}
			std::istringstream       line(a_script->GetCommand());
			std::vector<std::string> words;
			for (std::string word; line >> word;) {
				words.push_back(word);
			}
			Run(words);
			return true;
		}

		// Up to four words after the command, for the compiler; Execute reads the line itself
		RE::SCRIPT_PARAMETER params[] = {
			{ "what", RE::SCRIPT_PARAM_TYPE::kChar, true },
			{ "word", RE::SCRIPT_PARAM_TYPE::kChar, true },
			{ "word", RE::SCRIPT_PARAM_TYPE::kChar, true },
			{ "word", RE::SCRIPT_PARAM_TYPE::kChar, true },
		};
	}

	void Install()
	{
		for (const auto* candidate : kCandidates) {
			auto* command = RE::SCRIPT_FUNCTION::LocateConsoleCommand(candidate);
			if (!command) {
				continue;
			}
			command->functionName = kName;
			command->shortName = "";
			command->helpString = kHelp;
			command->referenceFunction = false;
			command->SetParameters(params);
			command->executeFunction = &Execute;
			command->conditionFunction = nullptr;
			logger::info("console: \"chiefrim\" in place of the developer command {}", candidate);
			return;
		}
		logger::warn("console: none of the developer commands Chiefrim takes is there (another mod took them?): no \"chiefrim\" command");
	}

	void OnLinked()
	{
		SendDebug();
	}

	void OnHaloLine(const cr_msg_log& a_line)
	{
		std::string text(a_line.text, strnlen(a_line.text, sizeof(a_line.text)));
		logger::info("console: halo: {}", text);
		Print(text);
	}
}
