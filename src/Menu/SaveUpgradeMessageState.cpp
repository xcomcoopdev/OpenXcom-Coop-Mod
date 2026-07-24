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
#include "SaveUpgradeMessageState.h"
#include <sstream>
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Mod/Mod.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Savegame/SavedGame.h"
#include "SaveUpgradeFlow.h"

namespace OpenXcom
{

/**
 * INFO dialog: a single OK button that pops the state.
 */
SaveUpgradeMessageState::SaveUpgradeMessageState(OptionsOrigin origin, const std::string& header, const std::vector<std::string>& lines)
	: _origin(origin), _mode(INFO), _btnConfirm(0), _btnCancel(0)
{
	build(header, lines);
}

/**
 * CONFIRM dialog: Continue runs the upgrade execute() for the carried inputs.
 */
SaveUpgradeMessageState::SaveUpgradeMessageState(OptionsOrigin origin, const std::string& filename, const SaveUpgrade::UpgradeInputs& inputs, const std::string& header, const std::vector<std::string>& lines)
	: _origin(origin), _mode(CONFIRM), _filename(filename), _inputs(inputs), _btnConfirm(0), _btnCancel(0)
{
	build(header, lines);
}

/**
 * Builds the window, text body and buttons.
 * @param header First (bold) line of the message.
 * @param lines Additional message lines (one per entry).
 */
void SaveUpgradeMessageState::build(const std::string& header, const std::vector<std::string>& lines)
{
	_screen = false;

	_window = new Window(this, 288, 160, 16, 20, POPUP_BOTH);
	_txtTitle = new Text(268, 17, 26, 28);
	_txtBody = new Text(268, 80, 26, 48);

	setInterface("saveMenus", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "confirmLoad", "saveMenus");
	add(_txtTitle, "confirmLoad", "saveMenus");
	add(_txtBody, "confirmLoad", "saveMenus");

	if (_mode == CONFIRM)
	{
		_btnConfirm = new TextButton(126, 16, 26, 138);
		_btnCancel = new TextButton(126, 16, 164, 138);
		add(_btnConfirm, "confirmLoad", "saveMenus");
		add(_btnCancel, "confirmLoad", "saveMenus");
	}
	else
	{
		_btnConfirm = new TextButton(180, 16, 70, 138);
		add(_btnConfirm, "confirmLoad", "saveMenus");
	}

	centerAllSurfaces();

	setWindowBackground(_window, "saveMenus");

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setWordWrap(true);
	_txtTitle->setText(header);

	std::ostringstream ss;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (i)
			ss << "\n";
		ss << lines[i];
	}
	_txtBody->setWordWrap(true);
	_txtBody->setText(ss.str());

	if (_mode == CONFIRM)
	{
		_btnConfirm->setText(tr("STR_SAVE_UPGRADE_CONTINUE_ANYWAY"));
		_btnConfirm->onMouseClick((ActionHandler)&SaveUpgradeMessageState::btnConfirmClick);
		_btnConfirm->onKeyboardPress((ActionHandler)&SaveUpgradeMessageState::btnConfirmClick, Options::keyOk);
		_btnCancel->setText(tr("STR_CANCEL_UC"));
		_btnCancel->onMouseClick((ActionHandler)&SaveUpgradeMessageState::btnCancelClick);
		_btnCancel->onKeyboardPress((ActionHandler)&SaveUpgradeMessageState::btnCancelClick, Options::keyCancel);
	}
	else
	{
		_btnConfirm->setText(tr("STR_OK"));
		_btnConfirm->onMouseClick((ActionHandler)&SaveUpgradeMessageState::btnConfirmClick);
		_btnConfirm->onKeyboardPress((ActionHandler)&SaveUpgradeMessageState::btnConfirmClick, Options::keyOk);
		_btnConfirm->onKeyboardPress((ActionHandler)&SaveUpgradeMessageState::btnConfirmClick, Options::keyCancel);
	}

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("saveMenus");
	}
}

SaveUpgradeMessageState::~SaveUpgradeMessageState()
{
}

/**
 * INFO: pop. CONFIRM: pop this dialog and run the upgrade execute().
 * @param action Pointer to an action.
 */
void SaveUpgradeMessageState::btnConfirmClick(Action*)
{
	if (_mode == CONFIRM)
	{
		Game* game = _game;
		OptionsOrigin origin = _origin;
		std::string filename = _filename;
		SaveUpgrade::UpgradeInputs inputs = _inputs;
		game->popState();
		SaveUpgradeUI::runExecute(game, origin, filename, inputs);
	}
	else
	{
		_game->popState();
	}
}

/**
 * Cancels the confirm without writing anything.
 * @param action Pointer to an action.
 */
void SaveUpgradeMessageState::btnCancelClick(Action*)
{
	_game->popState();
}

}
