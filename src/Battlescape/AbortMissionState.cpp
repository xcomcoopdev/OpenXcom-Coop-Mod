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
#include "AbortMissionState.h"
#include <vector>
#include "../Engine/Game.h"
#include "../Mod/Mod.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Engine/Action.h"
#include "../Savegame/SavedBattleGame.h"
#include "BattlescapeGame.h"
#include "BattlescapeState.h"
#include "../Engine/Options.h"
#include "../Mod/AlienDeployment.h"
#include "../Mod/MapScript.h"
#include "../Mod/RuleCraft.h"
#include "../Savegame/Craft.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the Abort Mission window.
 * @param game Pointer to the core game.
 * @param battleGame Pointer to the saved game.
 * @param state Pointer to the Battlescape state.
 */
AbortMissionState::AbortMissionState(SavedBattleGame *battleGame, BattlescapeState *state) : _battleGame(battleGame), _state(state), _inEntrance(0), _inExit(0), _outside(0)
{
	// Create objects
	_screen = false;
	_window = new Window(this, 320, 144, 0, 0);
	_txtInEntrance = new Text(304, 17, 16, 20);
	_txtInExit = new Text(304, 17, 16, 40);
	_txtOutside = new Text(304, 17, 16, 60);
	_txtAbort = new Text(320, 17, 0, 80);
	_btnOk = new TextButton(120, 16, 16, 110);
	_btnCancel = new TextButton(120, 16, 184, 110);

	// Set palette
	_battleGame->setPaletteByDepth(this);

	add(_window, "messageWindowBorder", "battlescape");
	add(_txtInEntrance, "messageWindows", "battlescape");
	add(_txtInExit, "messageWindows", "battlescape");
	add(_txtOutside, "messageWindows", "battlescape");
	add(_txtAbort, "messageWindows", "battlescape");
	add(_btnOk, "messageWindowButtons", "battlescape");
	add(_btnCancel, "messageWindowButtons", "battlescape");

	// Check available areas (maybe should be cached somewhere)
	bool exit = false, craft = true;
	AlienDeployment *deployment = _game->getMod()->getDeployment(_battleGame->getMissionType());
	if (deployment != 0)
	{
		exit = !deployment->getNextStage().empty() || deployment->getEscapeType() == ESCAPE_EXIT || deployment->getEscapeType() == ESCAPE_EITHER;
		std::string lastUsedMapScript = _battleGame->getLastUsedMapScript();
		if (lastUsedMapScript.empty())
		{
			lastUsedMapScript = deployment->getRandomMapScript(); // don't crash on old saves
		}
		const std::vector<MapScript*> *mapScriptEntries = _game->getMod()->getMapScript(lastUsedMapScript);
		if (mapScriptEntries != 0)
		{
			craft = false;
			for (const auto* mapScriptEntry : *mapScriptEntries)
			{
				if (mapScriptEntry->getType() == MSC_ADDCRAFT)
				{
					craft = true;
					break;
				}
			}
		}
	}
	if (exit)
	{
		exit = false;
		for (int i = 0; i < _battleGame->getMapSizeXYZ(); ++i)
		{
			Tile *tile = _battleGame->getTile(i);
			if (tile && tile->getFloorSpecialTileType() == END_POINT)
			{
				exit = true;
				break;
			}
		}
	}

	// Calculate values
	BattlescapeTally tally = _battleGame->isPreview() ? _battleGame->tallyUnitsForPreview() : _battleGame->getBattleGame()->tallyUnits();
	_inEntrance = tally.inEntrance;
	_inExit = tally.inExit;
	_outside = tally.inField;

	if (!exit && _inExit > 0)
	{
		// FIXME: better would be to correctly decide already at the top (how??), but for now this will do...
		exit = true;
	}

	// Set up objects
	_window->setHighContrast(true);
	_window->setBackground(_game->getMod()->getSurface("TAC00.SCR"));

	_txtInEntrance->setBig();
	_txtInEntrance->setHighContrast(true);
	if (craft)
	{
		_txtInEntrance->setText(tr("STR_UNITS_IN_CRAFT", _inEntrance + tally.vipInEntrance));
	}
	else
	{
		_txtInEntrance->setText(tr("STR_UNITS_IN_ENTRANCE", _inEntrance + tally.vipInEntrance));
	}

	_txtInExit->setBig();
	_txtInExit->setHighContrast(true);
	_txtInExit->setText(tr("STR_UNITS_IN_EXIT", _inExit + tally.vipInExit));

	_txtOutside->setBig();
	_txtOutside->setHighContrast(true);
	_txtOutside->setText(tr("STR_UNITS_OUTSIDE", _outside + tally.vipInField));


	if (_battleGame->getMissionType() == "STR_BASE_DEFENSE")
	{
		_txtInEntrance->setVisible(false);
		_txtInExit->setVisible(false);
		_txtOutside->setVisible(false);
	}
	else if (!exit || _battleGame->isPreview())
	{
		_txtInEntrance->setY(26);
		_txtOutside->setY(54);
		_txtInExit->setVisible(false);
	}

	_txtAbort->setBig();
	_txtAbort->setAlign(ALIGN_CENTER);
	_txtAbort->setHighContrast(true);
	_txtAbort->setText(tr("STR_ABORT_MISSION_QUESTION"));
	if (_battleGame->isPreview())
	{
		_txtAbort->setText(tr("STR_CRAFT_DEPLOYMENT_QUESTION"));
	}


	_btnOk->setText(tr("STR_OK"));
	_btnOk->setHighContrast(true);
	_btnOk->onMouseClick((ActionHandler)&AbortMissionState::btnOkClick);
	_btnOk->onKeyboardPress((ActionHandler)&AbortMissionState::btnOkClick, Options::keyOk);
	if (_battleGame->isPreview() && (_outside > 0 || _inEntrance <= 0))
	{
		_btnOk->setVisible(false);
	}


	_btnCancel->setText(tr("STR_CANCEL_UC"));
	_btnCancel->setHighContrast(true);
	_btnCancel->onMouseClick((ActionHandler)&AbortMissionState::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&AbortMissionState::btnCancelClick, Options::keyCancel);
	_btnCancel->onKeyboardPress((ActionHandler)&AbortMissionState::btnCancelClick, Options::keyBattleAbort);

	centerAllSurfaces();
}

/**
 *
 */
AbortMissionState::~AbortMissionState()
{

}

/**
 * Confirms mission abort.
 * @param action Pointer to an action.
 */
void AbortMissionState::btnOkClick(Action *)
{
	if (_battleGame->isPreview())
	{
		if (_battleGame->getCraftForPreview()->getId() == RuleCraft::DUMMY_CRAFT_ID)
		{
			// dummy craft, generic deployment schema
			_battleGame->saveDummyCraftDeployment();
		}
		else
		{
			// real craft, real unit deployment
			_battleGame->saveCustomCraftDeployment();
		}

		_game->popState();
		return;
	}

	_game->popState();
	_battleGame->setAborted(true);
	_state->finishBattle(true, _inExit);
}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void AbortMissionState::btnCancelClick(Action *)
{
	// coop
	_game->getCoopMod()->setPauseOff();

	_game->popState();
}


}
