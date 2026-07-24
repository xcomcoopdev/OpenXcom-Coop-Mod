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
#include "SaveUpgradeSummaryState.h"
#include <sstream>
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/CrossPlatform.h"
#include "../Mod/Mod.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Interface/TextList.h"
#include "../Savegame/SavedGame.h"
#include "LoadGameState.h"

namespace OpenXcom
{

/**
 * Initializes the summary state.
 * @param origin Game section that originated the load.
 * @param filename Name of the upgraded save.
 * @param inputs The collected inputs (for the rejoin instructions).
 * @param result The execute() result (upgraded-save path + report lines).
 */
SaveUpgradeSummaryState::SaveUpgradeSummaryState(OptionsOrigin origin, const std::string& filename, const SaveUpgrade::UpgradeInputs& inputs, const SaveUpgrade::ExecuteResult& result)
	: _origin(origin), _filename(filename), _inputs(inputs), _result(result)
{
	_screen = false;

	_window = new Window(this, 320, 200, 0, 0, POPUP_BOTH);
	_txtTitle = new Text(300, 17, 10, 7);
	_txtIntro = new Text(300, 9, 10, 26);
	// Width 284 (not 292) keeps the scrollbar - drawn at x + width + 4, 13 wide -
	// inside the 320px screen and off the window border, whose colors the track
	// picks up (ScrollBar::drawTrack copies the background, offset -5 blocks).
	// Height must be a whole number of 8px rows: TextList::updateVisible rounds
	// the row count UP, so 86 showed an 11th row with its bottom 2px cut off.
	_lstReport = new TextList(284, 88, 14, 40);
	_txtNotes = new Text(300, 38, 10, 130);
	_btnOk = new TextButton(180, 16, 70, 176);

	setInterface("geoscape", true, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "saveMenus");
	add(_txtTitle, "text", "saveMenus");
	add(_txtIntro, "text", "saveMenus");
	add(_lstReport, "list", "saveMenus");
	add(_txtNotes, "text", "saveMenus");
	add(_btnOk, "button", "saveMenus");

	centerAllSurfaces();

	setWindowBackground(_window, "saveMenus");

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setText(tr("STR_SAVE_UPGRADE_SUMMARY_TITLE"));

	_txtIntro->setText(tr("STR_SAVE_UPGRADE_SUMMARY_INTRO"));

	_lstReport->setColumns(1, 276);
	_lstReport->setBackground(_window);
	_lstReport->setMargin(4);
	_lstReport->setWordWrap(true);
	for (const std::string& line : _result.reportLines)
		_lstReport->addRow(1, line.c_str());

	// Rejoin + dead-client-file notes. The upgraded filename and "original unchanged"
	// line are in the report list above (built by the runner).
	std::ostringstream notes;
	if (_inputs.skipClient || _inputs.clientName.empty())
	{
		notes << tr("STR_SAVE_UPGRADE_REJOIN_FRESH");
	}
	else
	{
		notes << tr("STR_SAVE_UPGRADE_REJOIN").arg(_inputs.clientName);
		if (!_inputs.clientSaveFileName.empty())
			notes << "\n" << tr("STR_SAVE_UPGRADE_CLIENT_FILE_NOTE");
	}
	_txtNotes->setWordWrap(true);
	_txtNotes->setText(notes.str());

	_btnOk->setText(tr("STR_SAVE_UPGRADE_LOAD_BTN"));
	_btnOk->onMouseClick((ActionHandler)&SaveUpgradeSummaryState::btnOkClick);
	_btnOk->onKeyboardPress((ActionHandler)&SaveUpgradeSummaryState::btnOkClick, Options::keyOk);

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("saveMenus");
	}
}

SaveUpgradeSummaryState::~SaveUpgradeSummaryState()
{
}

/**
 * Loads the NEW upgraded save (not the original, which is left untouched). The
 * upgraded file detects as current, so the fresh LoadGameState does not re-gate;
 * the eventual setState() on load discards this and the upgrade-UI states beneath.
 * @param action Pointer to an action.
 */
void SaveUpgradeSummaryState::btnOkClick(Action*)
{
	Game* game = _game;
	OptionsOrigin origin = _origin;
	std::string upgradedFile = CrossPlatform::baseFilename(_result.upgradedPath);
	SDL_Color* palette = _palette;
	game->popState();
	game->pushState(new LoadGameState(origin, upgradedFile, palette));
}

}
