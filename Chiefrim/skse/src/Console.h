/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "chiefrim_protocol.h"

namespace chiefrim::Console
{
	// Skyrim's console gets a "chiefrim" command (docs §11), in place of one
	// of the game's unused developer commands:
	//   chiefrim restart | on | off | toggle   as the F11 and F10 keys
	//   chiefrim give [name | number]          a weapon by name (or the next, as F7)
	//   chiefrim weapons                        the weapons Chief may have
	//   chiefrim shapes [on | off]              draw the proxies' hit shapes
	// kDataLoaded.
	void Install();

	// A new Halo: the debug drawing it should do.
	void OnLinked();

	// Halo's answer to a command (CR_MSG_CONSOLE).
	void OnHaloLine(const cr_msg_log& a_line);
}
