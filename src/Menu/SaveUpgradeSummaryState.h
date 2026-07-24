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
#include "../Engine/State.h"
#include "OptionsBaseState.h" // OptionsOrigin
#include "../Savegame/Upgrade/SaveUpgrade.h" // UpgradeInputs, ExecuteResult

namespace OpenXcom
{

class TextButton;
class Window;
class Text;
class TextList;

/**
 * Success summary shown after an upgrade: the transforms applied (scrollable
 * report), the upgraded-save filename, and rejoin instructions. OK loads the upgraded
 * save (which now detects as current, so it is not re-gated). Self-themes over
 * both palettes.
 */
class SaveUpgradeSummaryState : public State
{
private:
	OptionsOrigin _origin;
	std::string _filename;
	SaveUpgrade::UpgradeInputs _inputs;
	SaveUpgrade::ExecuteResult _result;

	Window *_window;
	Text *_txtTitle, *_txtIntro, *_txtNotes;
	TextList *_lstReport;
	TextButton *_btnOk;
public:
	/// Creates the summary state.
	SaveUpgradeSummaryState(OptionsOrigin origin, const std::string& filename, const SaveUpgrade::UpgradeInputs& inputs, const SaveUpgrade::ExecuteResult& result);
	/// Cleans up the state.
	~SaveUpgradeSummaryState();
	/// Loads the upgraded save.
	void btnOkClick(Action* action);
};

}
