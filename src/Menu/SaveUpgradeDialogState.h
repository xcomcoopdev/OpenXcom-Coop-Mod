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
#include "../Savegame/Upgrade/SaveUpgrade.h" // DetectedSchema

namespace OpenXcom
{

class TextButton;
class Window;
class Text;

/**
 * The load-gate entry dialog: an older-format save was picked, so this offers
 * "Save backup and upgrade save" / "Cancel" instead of loading it. For the dual
 * variant it opens the input-collection state; otherwise it drives the upgrade
 * directly. Pushed by LoadGameState in place of the load. Self-themes over both
 * the Geoscape and Battlescape palettes.
 */
class SaveUpgradeDialogState : public State
{
private:
	OptionsOrigin _origin;
	std::string _filename;
	SaveUpgrade::DetectedSchema _detected;
	Window *_window;
	Text *_txtTitle, *_txtText;
	TextButton *_btnUpgrade, *_btnCancel;

	std::string displayName() const;
public:
	/// Creates the upgrade entry dialog.
	SaveUpgradeDialogState(OptionsOrigin origin, const std::string& filename, const SaveUpgrade::DetectedSchema& detected);
	/// Cleans up the state.
	~SaveUpgradeDialogState();
	/// Confirms and begins the upgrade (or opens input collection).
	void btnUpgradeClick(Action* action);
	/// Cancels back to the previous screen.
	void btnCancelClick(Action* action);
};

}
