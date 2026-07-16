#pragma once
/*
 * Copyright 2010-2026 OpenXcom Developers.
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
#include <string>
#include <vector>
#include "../Engine/State.h"
#include "OptionsBaseState.h" // OptionsOrigin
#include "../Savegame/Upgrade/SaveUpgrade.h" // UpgradeInputs

namespace OpenXcom
{

class TextButton;
class Window;
class Text;

/**
 * A themed message dialog used by the save-upgrade flow. Two shapes:
 *  - INFO:    a single OK button that just pops (unknown-future / blocking-error
 *             / execute-failure messages).
 *  - CONFIRM: "Continue anyway" / "Cancel"; Continue runs the upgrade execute()
 *             for the carried file + inputs (the warnings gate).
 * Self-themes over both the Geoscape and Battlescape palettes.
 */
class SaveUpgradeMessageState : public State
{
public:
	enum Mode { INFO, CONFIRM };
private:
	OptionsOrigin _origin;
	Mode _mode;
	std::string _filename;
	SaveUpgrade::UpgradeInputs _inputs;
	Window *_window;
	Text *_txtTitle, *_txtBody;
	TextButton *_btnConfirm, *_btnCancel;

	void build(const std::string& header, const std::vector<std::string>& lines);
public:
	/// INFO dialog: single OK, just pops.
	SaveUpgradeMessageState(OptionsOrigin origin, const std::string& header, const std::vector<std::string>& lines);
	/// CONFIRM dialog: Continue runs execute() for filename + inputs.
	SaveUpgradeMessageState(OptionsOrigin origin, const std::string& filename, const SaveUpgrade::UpgradeInputs& inputs, const std::string& header, const std::vector<std::string>& lines);
	/// Cleans up the state.
	~SaveUpgradeMessageState();
	/// Confirm/OK handler.
	void btnConfirmClick(Action* action);
	/// Cancel handler.
	void btnCancelClick(Action* action);
};

}
