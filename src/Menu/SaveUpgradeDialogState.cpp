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
#include "SaveUpgradeDialogState.h"
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
#include "SaveUpgradeClientState.h"
#include "SaveUpgradeFlow.h"

namespace OpenXcom
{

/**
 * Initializes the upgrade entry dialog.
 * @param origin Game section that originated the load.
 * @param filename Name of the save file being loaded.
 * @param detected The classification of that save.
 */
SaveUpgradeDialogState::SaveUpgradeDialogState(OptionsOrigin origin, const std::string& filename, const SaveUpgrade::DetectedSchema& detected)
	: _origin(origin), _filename(filename), _detected(detected)
{
	_screen = false;

	_window = new Window(this, 288, 160, 16, 20, POPUP_BOTH);
	_txtTitle = new Text(268, 17, 26, 28);
	_txtText = new Text(268, 66, 26, 48);
	_btnUpgrade = new TextButton(248, 16, 36, 118);
	_btnCancel = new TextButton(248, 16, 36, 138);

	setInterface("saveMenus", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "confirmLoad", "saveMenus");
	add(_txtTitle, "confirmLoad", "saveMenus");
	add(_txtText, "confirmLoad", "saveMenus");
	add(_btnUpgrade, "confirmLoad", "saveMenus");
	add(_btnCancel, "confirmLoad", "saveMenus");

	centerAllSurfaces();

	setWindowBackground(_window, "saveMenus");

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setText(tr("STR_SAVE_UPGRADE_TITLE"));

	_txtText->setWordWrap(true);
	_txtText->setText(tr("STR_SAVE_UPGRADE_PROMPT").arg(displayName()).arg(_detected.schema));

	_btnUpgrade->setText(tr("STR_SAVE_UPGRADE_CONFIRM_BTN"));
	_btnUpgrade->onMouseClick((ActionHandler)&SaveUpgradeDialogState::btnUpgradeClick);
	_btnUpgrade->onKeyboardPress((ActionHandler)&SaveUpgradeDialogState::btnUpgradeClick, Options::keyOk);

	_btnCancel->setText(tr("STR_CANCEL_UC"));
	_btnCancel->onMouseClick((ActionHandler)&SaveUpgradeDialogState::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&SaveUpgradeDialogState::btnCancelClick, Options::keyCancel);

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("saveMenus");
	}
}

SaveUpgradeDialogState::~SaveUpgradeDialogState()
{
}

/**
 * Reads the header "name" of the save so the prompt can name it; falls back to
 * the filename stem. Cheap header-only read, mirroring SavedGame::getSaveInfo.
 */
std::string SaveUpgradeDialogState::displayName() const
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
 * Confirms the upgrade. The dialog pops itself first, so whatever comes next
 * (input collection, a warnings confirm, or the summary) sits directly over the
 * screen that invoked the load - a cancel there returns to that screen.
 * @param action Pointer to an action.
 */
void SaveUpgradeDialogState::btnUpgradeClick(Action*)
{
	Game* game = _game;
	OptionsOrigin origin = _origin;
	std::string filename = _filename;

	SaveUpgrade::UpgradeRunner runner(filename);
	std::vector<SaveUpgrade::InputRequest> reqs = runner.requiredInputs();

	game->popState();

	if (reqs.empty())
	{
		// Embed / sidecar: the framework auto-ingests the client world; no input.
		SaveUpgrade::UpgradeInputs inputs;
		SaveUpgradeUI::beginUpgrade(game, origin, filename, inputs);
	}
	else
	{
		// Dual: collect the client save + names first.
		game->pushState(new SaveUpgradeClientState(origin, filename));
	}
}

/**
 * Cancels the upgrade; nothing is written and the original save is untouched.
 * @param action Pointer to an action.
 */
void SaveUpgradeDialogState::btnCancelClick(Action*)
{
	_game->popState();
}

}
