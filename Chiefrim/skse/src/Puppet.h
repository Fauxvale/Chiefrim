/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace chiefrim::Puppet
{
	// Hooks PlayerCharacter::Update. Each frame: keeps the link, tells Halo
	// where the world is, and moves the Skyrim player to Chief (docs §6).
	void Install();
}
