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
#include <utility>
#include <vector>
#include "../Engine/State.h"
#include "OptionsBaseState.h" // OptionsOrigin

namespace OpenXcom
{

class TextButton;
class Window;
class Text;
class TextList;
class TextEdit;

/**
 * Dual-variant input collection (PRD 6.3): pick the other player's standalone
 * .sav from the saves folder, enter that player's connect name (required) and
 * optionally the host name. Also offers Refresh (re-scan) and an explicit
 * "Skip - client starts fresh" escape hatch. Self-themes over both palettes.
 */
class SaveUpgradeClientState : public State
{
private:
	OptionsOrigin _origin;
	std::string _filename;
	std::vector<std::pair<std::string, std::string> > _candidates; // (fileName, displayName)
	std::string _selectedFile;
	int _selectedRow;

	Window *_window;
	Text *_txtTitle, *_txtInstructions, *_txtClientLbl, *_txtClientWarn, *_txtKeyEcho, *_txtHostLbl, *_txtHostHint;
	TextList *_lstSaves;
	TextEdit *_edtClientName, *_edtHostName;
	TextButton *_btnContinue, *_btnRefresh, *_btnSkip, *_btnCancel;

	/// Re-scans the saves folder and repopulates the list.
	void refreshList();
	/// Updates the rejoin-key echo line from the current name field.
	void updateKeyEcho();
public:
	/// Creates the client input state.
	SaveUpgradeClientState(OptionsOrigin origin, const std::string& filename);
	/// Cleans up the state.
	~SaveUpgradeClientState();
	/// Self-heals: pops itself if the save no longer needs upgrading.
	void init() override;
	/// Selects a candidate save.
	void lstSavesClick(Action* action);
	/// Reacts to name edits (updates the echo).
	void edtClientNameChange(Action* action);
	/// Continues with the selected file + names.
	void btnContinueClick(Action* action);
	/// Re-scans the saves folder.
	void btnRefreshClick(Action* action);
	/// Proceeds with no client world (explicit confirm downstream).
	void btnSkipClick(Action* action);
	/// Cancels back to the previous screen.
	void btnCancelClick(Action* action);
};

}
