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
#include "SaveUpgradeAmbiguousState.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Language.h"
#include "../Engine/CrossPlatform.h"
#include "../Engine/Yaml.h"
#include "../Mod/Mod.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Savegame/SavedGame.h"
#include "LoadGameState.h"
#include "SaveUpgradeClientState.h"

namespace OpenXcom
{

/**
 * Initializes the ambiguous-build choice dialog.
 * @param origin Game section that originated the load.
 * @param filename Name of the save file being loaded.
 * @param detected The classification of that save.
 */
SaveUpgradeAmbiguousState::SaveUpgradeAmbiguousState(OptionsOrigin origin, const std::string& filename, const SaveUpgrade::DetectedSchema& detected)
	: _origin(origin), _filename(filename), _detected(detected)
{
	_screen = false;

	_window = new Window(this, 288, 160, 16, 20, POPUP_BOTH);
	_txtTitle = new Text(268, 17, 26, 28);
	_txtText = new Text(268, 52, 26, 48);
	// Full-width stacked buttons so every (long) caption fits on one line.
	_btnSolo = new TextButton(248, 16, 36, 106);
	_btnCoop = new TextButton(248, 16, 36, 126);
	_btnCancel = new TextButton(248, 16, 36, 146);

	setInterface("saveMenus", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "confirmLoad", "saveMenus");
	add(_txtTitle, "confirmLoad", "saveMenus");
	add(_txtText, "confirmLoad", "saveMenus");
	add(_btnSolo, "confirmLoad", "saveMenus");
	add(_btnCoop, "confirmLoad", "saveMenus");
	add(_btnCancel, "confirmLoad", "saveMenus");

	centerAllSurfaces();

	setWindowBackground(_window, "saveMenus");

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setText(tr("STR_SAVE_UPGRADE_AMBIGUOUS_TITLE"));

	_txtText->setWordWrap(true);
	_txtText->setText(tr("STR_SAVE_UPGRADE_AMBIGUOUS_PROMPT").arg(displayName()));

	_btnSolo->setText(tr("STR_SAVE_UPGRADE_AMBIGUOUS_SOLO_BTN"));
	_btnSolo->onMouseClick((ActionHandler)&SaveUpgradeAmbiguousState::btnSoloClick);

	_btnCoop->setText(tr("STR_SAVE_UPGRADE_AMBIGUOUS_COOP_BTN"));
	_btnCoop->onMouseClick((ActionHandler)&SaveUpgradeAmbiguousState::btnCoopClick);

	_btnCancel->setText(tr("STR_CANCEL_UC"));
	_btnCancel->onMouseClick((ActionHandler)&SaveUpgradeAmbiguousState::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&SaveUpgradeAmbiguousState::btnCancelClick, Options::keyCancel);

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("saveMenus");
	}
}

SaveUpgradeAmbiguousState::~SaveUpgradeAmbiguousState()
{
}

/**
 * Reads the header "name" of the save so the prompt can name it; falls back to
 * the filename stem. Cheap header-only read, mirroring SavedGame::getSaveInfo.
 */
std::string SaveUpgradeAmbiguousState::displayName() const
{
	if (_filename == SavedGame::QUICKSAVE)
		return _game->getLanguage()->getString("STR_QUICK_SAVE_SLOT");
	if (_filename == SavedGame::AUTOSAVE_GEOSCAPE || _filename.find(SavedGame::AUTOSAVE_GEOSCAPE) != std::string::npos)
		return _game->getLanguage()->getString("STR_AUTO_SAVE_GEOSCAPE_SLOT");
	if (_filename == SavedGame::AUTOSAVE_BATTLESCAPE || _filename.find(SavedGame::AUTOSAVE_BATTLESCAPE) != std::string::npos)
		return _game->getLanguage()->getString("STR_AUTO_SAVE_BATTLESCAPE_SLOT");
	try
	{
		YAML::YamlRootNodeReader reader(Options::getMasterUserFolder() + _filename, true);
		std::string name;
		if (reader.tryRead("name", name) && !name.empty())
			return name;
	}
	catch (...)
	{
		// fall through to the filename stem
	}
	return CrossPlatform::noExt(_filename);
}

/**
 * "Load as solo campaign": pop this dialog and start a normal load that bypasses
 * the schema gate (otherwise it would re-detect AmbiguousBuild and loop back
 * here). The loaded save re-stamps itself schema-current on its next save; until
 * then it re-asks on each load, which is accepted.
 * @param action Pointer to an action.
 */
void SaveUpgradeAmbiguousState::btnSoloClick(Action*)
{
	Game* game = _game;
	OptionsOrigin origin = _origin;
	std::string filename = _filename;
	SDL_Color* palette = _palette;
	game->popState();
	game->pushState(new LoadGameState(origin, filename, palette, "", false, true));
}

/**
 * "Upgrade as co-op save": pop this dialog and hand off to the legacy dual
 * upgrade route - the client-save picker, then the runner - by confirming this
 * was a co-op campaign (treatAmbiguousAsCoop). Reuses the exact dual flow.
 * @param action Pointer to an action.
 */
void SaveUpgradeAmbiguousState::btnCoopClick(Action*)
{
	Game* game = _game;
	OptionsOrigin origin = _origin;
	std::string filename = _filename;
	game->popState();
	game->pushState(new SaveUpgradeClientState(origin, filename, true));
}

/**
 * Cancels; nothing is written and the original save is untouched.
 * @param action Pointer to an action.
 */
void SaveUpgradeAmbiguousState::btnCancelClick(Action*)
{
	_game->popState();
}

}
