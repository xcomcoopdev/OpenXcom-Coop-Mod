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
#include "SaveUpgradeClientState.h"
#include <tuple>
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Language.h"
#include "../Engine/CrossPlatform.h"
#include "../Engine/Yaml.h"
#include "../Mod/Mod.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Interface/TextList.h"
#include "../Interface/TextEdit.h"
#include "../Savegame/SavedGame.h"
#include "SaveUpgradeFlow.h"

namespace OpenXcom
{

/**
 * Initializes the client input state.
 * @param origin Game section that originated the load.
 * @param filename Name of the host save being upgraded.
 */
SaveUpgradeClientState::SaveUpgradeClientState(OptionsOrigin origin, const std::string& filename, bool treatAmbiguousAsCoop)
	: _origin(origin), _filename(filename), _treatAmbiguousAsCoop(treatAmbiguousAsCoop), _selectedRow(-1)
{
	_screen = false;

	_window = new Window(this, 320, 200, 0, 0, POPUP_BOTH);
	_txtTitle = new Text(300, 17, 10, 7);
	_txtInstructions = new Text(300, 25, 10, 25);
	// Width 284 (not 292) keeps the scrollbar - drawn at x + width + 4, 13 wide -
	// inside the 320px screen and off the window border, whose colors the track
	// picks up (ScrollBar::drawTrack copies the background, offset -5 blocks).
	_lstSaves = new TextList(284, 57, 14, 52);
	_txtClientLbl = new Text(150, 9, 14, 112);
	_edtClientName = new TextEdit(this, 132, 9, 135, 112);
	_txtClientWarn = new Text(300, 9, 10, 122);
	_txtKeyEcho = new Text(300, 9, 10, 131);
	_txtHostLbl = new Text(150, 9, 14, 141);
	_edtHostName = new TextEdit(this, 132, 9, 135, 141);
	_txtHostHint = new Text(300, 9, 10, 151);
	// Uneven widths so every caption fits on one line: "Skip - fresh start" needs
	// far more room than "Refresh"/"Cancel" (74px each clipped/wrapped it). The row
	// still spans 8..316 with even 4px gaps.
	_btnContinue = new TextButton(60, 16, 8, 176);
	_btnRefresh = new TextButton(56, 16, 72, 176);
	_btnSkip = new TextButton(120, 16, 132, 176);
	_btnCancel = new TextButton(60, 16, 256, 176);

	setInterface("geoscape", true, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "saveMenus");
	add(_txtTitle, "text", "saveMenus");
	add(_txtInstructions, "text", "saveMenus");
	add(_lstSaves, "list", "saveMenus");
	add(_txtClientLbl, "text", "saveMenus");
	add(_edtClientName);
	add(_txtClientWarn, "text", "saveMenus");
	add(_txtKeyEcho, "text", "saveMenus");
	add(_txtHostLbl, "text", "saveMenus");
	add(_edtHostName);
	add(_txtHostHint, "text", "saveMenus");
	add(_btnContinue, "button", "saveMenus");
	add(_btnRefresh, "button", "saveMenus");
	add(_btnSkip, "button", "saveMenus");
	add(_btnCancel, "button", "saveMenus");

	centerAllSurfaces();

	setWindowBackground(_window, "saveMenus");

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setText(tr("STR_SAVE_UPGRADE_INPUT_TITLE"));

	_txtInstructions->setWordWrap(true);
	_txtInstructions->setText(tr("STR_SAVE_UPGRADE_PICK_INSTRUCTIONS"));

	_lstSaves->setColumns(1, 276);
	_lstSaves->setSelectable(true);
	_lstSaves->setBackground(_window);
	_lstSaves->setMargin(4);
	_lstSaves->onMouseClick((ActionHandler)&SaveUpgradeClientState::lstSavesClick);

	Uint8 editColor = _lstSaves->getSecondaryColor();

	_txtClientLbl->setText(tr("STR_SAVE_UPGRADE_CLIENT_NAME"));

	_edtClientName->setColor(editColor);
	_edtClientName->setText("");
	_edtClientName->onChange((ActionHandler)&SaveUpgradeClientState::edtClientNameChange);

	_txtClientWarn->setText(tr("STR_SAVE_UPGRADE_CLIENT_NAME_WARNING"));

	_txtKeyEcho->setText("");

	_txtHostLbl->setText(tr("STR_SAVE_UPGRADE_HOST_NAME"));

	_edtHostName->setColor(editColor);
	_edtHostName->setText("");

	_txtHostHint->setText(tr("STR_SAVE_UPGRADE_HOST_NAME_HINT"));

	_btnContinue->setText(tr("STR_SAVE_UPGRADE_CONTINUE"));
	_btnContinue->onMouseClick((ActionHandler)&SaveUpgradeClientState::btnContinueClick);

	_btnRefresh->setText(tr("STR_SAVE_UPGRADE_REFRESH"));
	_btnRefresh->onMouseClick((ActionHandler)&SaveUpgradeClientState::btnRefreshClick);

	_btnSkip->setText(tr("STR_SAVE_UPGRADE_SKIP"));
	_btnSkip->onMouseClick((ActionHandler)&SaveUpgradeClientState::btnSkipClick);

	_btnCancel->setText(tr("STR_CANCEL_UC"));
	_btnCancel->onMouseClick((ActionHandler)&SaveUpgradeClientState::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&SaveUpgradeClientState::btnCancelClick, Options::keyCancel);

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("saveMenus");
	}

	refreshList();
}

SaveUpgradeClientState::~SaveUpgradeClientState()
{
}

/**
 * This state stays beneath the preflight/warning/summary dialogs so a cancel
 * or error there returns here with the inputs intact. If it ever resurfaces
 * AFTER the upgrade already succeeded (e.g. the follow-up load of the upgraded
 * file failed and the error dialog popped back to us), the file no longer
 * needs upgrading and this state is stale - pop straight back to the invoker.
 */
void SaveUpgradeClientState::init()
{
	State::init();
	SaveUpgrade::DetectedSchema detected = SaveUpgrade::SchemaDetector::detect(_filename);
	// A genuine dual save detects as Legacy; an ambiguous save the player confirmed
	// as co-op detects as AmbiguousBuild. Either is still upgradeable here. Anything
	// else (Current after a successful upgrade, Solo, ...) means we are stale -> pop.
	if (!detected.needsUpgrade() && !detected.isAmbiguous())
	{
		_game->popState();
	}
}

/**
 * Prefills the name fields. Only the debug/visual-QA hook calls this - an empty
 * TextEdit draws nothing, so a screenshot needs the fields populated to show them.
 * @param clientName Other player's name.
 * @param hostName Host name (may be empty).
 */
void SaveUpgradeClientState::prefillNames(const std::string& clientName, const std::string& hostName)
{
	_edtClientName->setText(clientName);
	_edtHostName->setText(hostName);
	updateKeyEcho();
}

/**
 * Scans the saves folder for candidate client .sav files, excluding the file
 * being upgraded and any _bak_v backups, and fills the list. Reads each file's
 * header "name" for display, falling back to the filename stem.
 */
void SaveUpgradeClientState::refreshList()
{
	_candidates.clear();
	_selectedFile.clear();
	_selectedRow = -1;
	_lstSaves->clearList();

	const std::string folder = Options::getMasterUserFolder();
	for (const auto& item : CrossPlatform::getFolderContents(folder, "sav"))
	{
		const std::string& name = std::get<0>(item);
		if (name == _filename)
			continue;
		if (name.find("_bak_v") != std::string::npos)
			continue;

		std::string display = CrossPlatform::noExt(name);
		try
		{
			YAML::YamlRootNodeReader reader(folder + name, true);
			std::string headerName;
			if (reader.tryRead("name", headerName) && !headerName.empty())
				display = headerName;
		}
		catch (...)
		{
			// keep the filename stem
		}
		_candidates.push_back(std::make_pair(name, display));
	}

	if (_candidates.empty())
	{
		_lstSaves->addRow(1, tr("STR_SAVE_UPGRADE_NO_CANDIDATES").c_str());
		_lstSaves->setRowColor(0, _lstSaves->getSecondaryColor());
	}
	else
	{
		for (const auto& c : _candidates)
			_lstSaves->addRow(1, c.second.c_str());
	}
}

/**
 * Refreshes the rejoin-key echo from the current name field.
 */
void SaveUpgradeClientState::updateKeyEcho()
{
	std::string name = _edtClientName->getText();
	if (name.empty())
		_txtKeyEcho->setText("");
	else
		_txtKeyEcho->setText(tr("STR_SAVE_UPGRADE_KEY_ECHO").arg(name));
}

/**
 * Selects the clicked candidate save and pre-fills the name field.
 * @param action Pointer to an action.
 */
void SaveUpgradeClientState::lstSavesClick(Action*)
{
	if (_candidates.empty())
		return;
	int row = (int)_lstSaves->getSelectedRow();
	if (row < 0 || row >= (int)_candidates.size())
		return;

	if (_selectedRow >= 0 && _selectedRow < (int)_candidates.size())
		_lstSaves->setRowColor(_selectedRow, _lstSaves->getColor());
	_selectedRow = row;
	_selectedFile = _candidates[row].first;
	_lstSaves->setRowColor(row, _lstSaves->getSecondaryColor());

	if (_edtClientName->getText().empty())
	{
		_edtClientName->setText(_candidates[row].second);
		updateKeyEcho();
	}
}

/**
 * Updates the echo as the name is typed.
 * @param action Pointer to an action.
 */
void SaveUpgradeClientState::edtClientNameChange(Action*)
{
	updateKeyEcho();
}

/**
 * Continues with the selected client save and names.
 * @param action Pointer to an action.
 */
void SaveUpgradeClientState::btnContinueClick(Action*)
{
	if (_selectedFile.empty())
	{
		std::vector<std::string> lines;
		lines.push_back(tr("STR_SAVE_UPGRADE_NEED_FILE"));
		SaveUpgradeUI::pushMessage(_game, _origin, tr("STR_SAVE_UPGRADE_INPUT_TITLE"), lines);
		return;
	}
	if (_edtClientName->getText().empty())
	{
		std::vector<std::string> lines;
		lines.push_back(tr("STR_SAVE_UPGRADE_NEED_NAME"));
		SaveUpgradeUI::pushMessage(_game, _origin, tr("STR_SAVE_UPGRADE_INPUT_TITLE"), lines);
		return;
	}

	SaveUpgrade::UpgradeInputs inputs;
	inputs.clientSaveFileName = _selectedFile;
	inputs.clientName = _edtClientName->getText();
	inputs.hostName = _edtHostName->getText();
	inputs.skipClient = false;
	inputs.treatAmbiguousAsCoop = _treatAmbiguousAsCoop;

	SaveUpgradeUI::beginUpgrade(_game, _origin, _filename, inputs);
}

/**
 * Re-scans the saves folder.
 * @param action Pointer to an action.
 */
void SaveUpgradeClientState::btnRefreshClick(Action*)
{
	refreshList();
}

/**
 * Proceeds with no client world. The downstream warnings confirm makes the
 * consequence explicit (that player restarts fresh on rejoin).
 * @param action Pointer to an action.
 */
void SaveUpgradeClientState::btnSkipClick(Action*)
{
	SaveUpgrade::UpgradeInputs inputs;
	inputs.hostName = _edtHostName->getText();
	inputs.skipClient = true;
	inputs.treatAmbiguousAsCoop = _treatAmbiguousAsCoop;

	SaveUpgradeUI::beginUpgrade(_game, _origin, _filename, inputs);
}

/**
 * Cancels; nothing is written and the original save is untouched.
 * @param action Pointer to an action.
 */
void SaveUpgradeClientState::btnCancelClick(Action*)
{
	_game->popState();
}

}
