/*
 * Copyright 2010-2016 OpenXcom Developers.
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
#include "PauseState.h"
#include "../Engine/Game.h"
#include "../Mod/Mod.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "AbandonGameState.h"
#include "ListLoadState.h"
#include "ListSaveState.h"
#include "../Engine/Options.h"
#include "OptionsVideoState.h"
#include "OptionsGeoscapeState.h"
#include "OptionsBattlescapeState.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../version.h"

#include "../CoopMod/ServerList.h"
#include "../CoopMod/HostMenu.h"

namespace OpenXcom
{

GeoscapeState *_geostate;

/**
 * Initializes all the elements in the Pause window.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 */
PauseState::PauseState(OptionsOrigin origin) : _origin(origin)
{
	_screen = false;

	int x;
	if (_origin == OPT_GEOSCAPE)
	{
		x = 20;
	}
	else
	{
		x = 52;
	}

	// Create objects
	_window = new Window(this, 216, 160, x, 20, POPUP_BOTH);
	_btnLoad = new TextButton(180, 18, x + 18, 52);
	_btnSave = new TextButton(180, 18, x + 18, 72);
	_btnAbandon = new TextButton(180, 18, x + 18, 92);
	_btnOptions = new TextButton(180, 18, x + 18, 112);
	_btnCoop = new TextButton(180, 18, x + 18, 132);
	_btnCancel = new TextButton(180, 18, x + 18, 152);
	_txtTitle = new Text(206, 17, x + 5, 32);

	_txtVersion = new Text(216, 9, x, 11);

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_btnLoad, "button", "pauseMenu");
	add(_btnSave, "button", "pauseMenu");
	add(_btnAbandon, "button", "pauseMenu");
	add(_btnOptions, "button", "pauseMenu");
	add(_btnCoop, "button", "pauseMenu");
	add(_btnCancel, "button", "pauseMenu");
	add(_txtTitle, "text", "pauseMenu");
	add(_txtVersion, "text", "pauseMenu");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "pauseMenu");

	_btnLoad->setText(tr("STR_LOAD_GAME"));
	_btnLoad->onMouseClick((ActionHandler)&PauseState::btnLoadClick);

	_btnSave->setText(tr("STR_SAVE_GAME"));
	_btnSave->onMouseClick((ActionHandler)&PauseState::btnSaveClick);

	_btnAbandon->setText(tr("STR_ABANDON_GAME"));
	_btnAbandon->onMouseClick((ActionHandler)&PauseState::btnAbandonClick);

	_btnOptions->setText(tr("STR_GAME_OPTIONS"));
	_btnOptions->onMouseClick((ActionHandler)&PauseState::btnOptionsClick);

	// COOP
	_btnCoop->setText("CO-OP");
	_btnCoop->onMouseClick((ActionHandler)&PauseState::btnCoopClick);

	_btnCancel->setText(tr("STR_CANCEL_UC"));
	_btnCancel->onMouseClick((ActionHandler)&PauseState::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&PauseState::btnCancelClick, Options::keyCancel);
	if (origin == OPT_GEOSCAPE)
	{
		_btnCancel->onKeyboardPress((ActionHandler)&PauseState::btnCancelClick, Options::keyGeoOptions);
	}
	else if (origin == OPT_BATTLESCAPE)
	{
		_btnCancel->onKeyboardPress((ActionHandler)&PauseState::btnCancelClick, Options::keyBattleOptions);
		if (!_game->getSavedGame()->getSavedBattle()->getBattleGame()->getStates().empty())
		{
			_btnOptions->setVisible(false);
		}
	}

	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setBig();
	_txtTitle->setText(tr("STR_OPTIONS_UC"));

	std::ostringstream title;
	title << "OpenXcom " << OPENXCOM_VERSION_SHORT;
	_txtVersion->setText(title.str());
	_txtVersion->setAlign(ALIGN_CENTER);

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("pauseMenu");
	}

	if (_game->getSavedGame()->isIronman())
	{
		_btnLoad->setVisible(false);
		_btnSave->setVisible(false);
		_btnAbandon->setText(tr("STR_SAVE_AND_ABANDON_GAME"));
	}

	// coop: PRD-08 - hide Load whenever local loads are forbidden right now (any
	// live coop session, host included). Outside a live session the button follows
	// the normal/ironman rules above.
	if (!_game->getCoopMod()->localLoadsAllowed())
	{
		_btnLoad->setVisible(false);
	}

	if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->getServerOwner() == false)
	{

		_btnLoad->setVisible(false);
		_btnSave->setVisible(false);

	}

	// ENOUGH! No save corruption when trying to save/exit mid-action (e.g. during alien turn)
	if (origin == OPT_BATTLESCAPE)
	{
		bool playerTurn = _game->getSavedGame()->getSavedBattle()->getSide() == FACTION_PLAYER;
		bool debugMode = _game->getSavedGame()->getSavedBattle()->getDebugMode();
		bool busy = !_game->getSavedGame()->getSavedBattle()->getBattleGame()->getStates().empty();

		if ((!playerTurn && !debugMode) || busy)
		{
			_btnSave->setVisible(false); // non-ironman + ironman

			if (_game->getSavedGame()->isIronman())
			{
				_btnAbandon->setVisible(false); // ironman only
			}
		}
	}

	// coop: re-show Load/Save only when this machine is truly solo. The old
	// condition (!isCoopSession && !host) also fired for a coop-static client
	// during the brief window where isCoopSession reads false, re-exposing local
	// load/save to a client. Gating on localSavesAllowed() (which is false for a
	// client) plus !host keeps the host's Load hidden (its policy is PRD-08) and
	// preserves solo behavior exactly.
	if (_game->getCoopMod()->localSavesAllowed() && _game->getCoopMod()->getServerOwner() == false)
	{
		_btnLoad->setVisible(true);
		_btnSave->setVisible(true);
	}


	// check if campaign
	if (!_game->getSavedGame()->getCountries()->empty())
	{
		_game->getCoopMod()->setCoopCampaign(true);
	}
	else
	{
		_game->getCoopMod()->setCoopCampaign(false);
	}

}

/**
 *
 */
PauseState::~PauseState()
{

}

/**
 * Opens the Load Game screen.
 * @param action Pointer to an action.
 */
void PauseState::btnLoadClick(Action *)
{
	_game->pushState(new ListLoadState(_origin));
}

/**
 * Opens the Save Game screen.
 * @param action Pointer to an action.
 */
void PauseState::btnSaveClick(Action *)
{

	// A machine that may not touch local saves (coop client) gets an error
	// popup instead of the save list. Same authority as every other gate.
	if (!_game->getCoopMod()->localSavesAllowed())
	{
		_game->pushState(new CoopState(123));
	}
	else
	{
		_game->pushState(new ListSaveState(_origin));
	}

}

// Opens COOP view
void PauseState::btnCoopClick(Action *)
{

	// Open the host menu if the host saves the players' campaign progress, the client joins the game through the main menu (New Battle)
	if (_game->getCoopMod()->getCoopCampaign() == true && _game->getCoopMod()->getServerOwner() == false && _game->getCoopMod()->getCoopStatic() == false)
	{

		_game->pushState(new HostMenu());
	}
	else
	{
		_game->pushState(new ServerList());
	}

	if (Options::logPacketMessages == true)
	{
		_game->pushState(new CoopState(942));
	}

}

void PauseState::init()
{

	// coop: PRD-08 - hide Load whenever local loads are forbidden right now (any
	// live coop session, host included).
	if (!_game->getCoopMod()->localLoadsAllowed())
	{
		_btnLoad->setVisible(false);
	}

	if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->getServerOwner() == false)
	{
		_btnLoad->setVisible(false);
		_btnSave->setVisible(false);
	}

	// coop: re-show Load/Save only when truly solo (see init() note above);
	// never re-expose a coop client to local load/save.
	if (_game->getCoopMod()->localSavesAllowed() && _game->getCoopMod()->getServerOwner() == false)
	{
		_btnLoad->setVisible(true);
		_btnSave->setVisible(true);
	}

}

/**
 * Opens the Game Options screen.
 * @param action Pointer to an action.
 */
void PauseState::btnOptionsClick(Action *)
{
	Options::backupDisplay();
	if (_origin == OPT_GEOSCAPE)
	{
		_game->pushState(new OptionsGeoscapeState(_origin));
	}
	else if (_origin == OPT_BATTLESCAPE)
	{
		_game->pushState(new OptionsBattlescapeState(_origin));
	}
	else
	{
		_game->pushState(new OptionsVideoState(_origin));
	}
}

/**
 * Opens the Abandon Game window.
 * @param action Pointer to an action.
 */
void PauseState::btnAbandonClick(Action *)
{
	_game->pushState(new AbandonGameState(_origin));
}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void PauseState::btnCancelClick(Action *)
{


	_game->popState();
}

}
