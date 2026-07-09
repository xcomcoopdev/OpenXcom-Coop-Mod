#pragma once
/*
 * Copyright 2010-2016 OpenXcom Developers.
 * Copyright 2023-2026 XComCoopTeam (https://www.moddb.com/mods/openxcom-coop-mod)
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "../Engine/State.h"
#include <string>
#include <utility>
#include <vector>

namespace OpenXcom
{

class Window;
class Text;
class TextButton;
class Soldier;

/**
 * Co-op dialog to permanently transfer ownership of a soldier to another
 * player. Shows one button per other player plus Cancel. Opened with the
 * "Give Unit to Teammate" keybind (Options::giveUnit) from the base soldier
 * lists, the soldier stat screen, or the battlescape.
 */
class TransferSoldierMenu : public State
{
private:
	Window *_window;
	Text *_txtTitle;
	std::vector<TextButton*> _btnTargets;
	TextButton *_btnCancel;
	Soldier *_soldier;
	// target player ids matching _btnTargets by index
	std::vector<int> _targetIds;

public:
	/// Creates the dialog. currentOwnerId: 0 = host, 1 = client.
	TransferSoldierMenu(Soldier *soldier, int currentOwnerId);
	/// Resolves who currently owns a soldier (0 = host, 1 = client) from its
	/// persistent owner id, falling back to the co-op control flag.
	static int resolveOwnerId(Soldier *soldier);
	void btnTransferClick(Action *action);
	void btnCancelClick(Action *action);
};

}
