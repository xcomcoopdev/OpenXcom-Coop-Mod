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
#include "SaveGameState.h"
#include <sstream>
#include "../Engine/Logger.h"
#include "../Engine/Game.h"
#include "../Engine/Exception.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Engine/CrossPlatform.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Unicode.h"
#include "../Interface/Text.h"
#include "ErrorMessageState.h"
#include "MainMenuState.h"
#include "../Savegame/SavedGame.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../CoopMod/CoopState.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the Save Game screen.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 * @param filename Name of the save file without extension.
 * @param palette Parent state palette.
 */
SaveGameState::SaveGameState(OptionsOrigin origin, const std::string &filename, SDL_Color *palette) : _firstRun(0), _origin(origin), _filename(filename), _type(SAVE_DEFAULT)
{
	buildUi(palette);
}

/**
 * Initializes all the elements in the Save Game screen.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 * @param type Type of auto-save being used.
 * @param palette Parent state palette.
 */
SaveGameState::SaveGameState(OptionsOrigin origin, SaveType type, SDL_Color *palette, int currentTurn) : _firstRun(0), _origin(origin), _type(type)
{
	switch (type)
	{
	case SAVE_QUICK:
		_filename = SavedGame::QUICKSAVE;
		break;
	case SAVE_INSTA:
		_filename = "Instasave_" + CrossPlatform::sanitizeFilename(CrossPlatform::now()) + ".sav";
		break;
	case SAVE_AUTO_GEOSCAPE:
		if (Options::oxceGeoAutosaveFrequency > 0 && Options::oxceGeoAutosaveSlots >= 2 && Options::oxceGeoAutosaveSlots <= 10 && currentTurn > 0)
		{
			// multi-slot autosave
			int slotIndex = (currentTurn / Options::oxceGeoAutosaveFrequency) % Options::oxceGeoAutosaveSlots;
			_filename = "_" + std::to_string(slotIndex) + SavedGame::AUTOSAVE_GEOSCAPE;
		}
		else
		{
			// classic autosave
			_filename = SavedGame::AUTOSAVE_GEOSCAPE;
		}
		break;
	case SAVE_AUTO_BATTLESCAPE:
		if (currentTurn > 0 && Options::autosaveSlots >= 2 && Options::autosaveSlots <= 10)
		{
			// multi-slot autosave
			int slotIndex = (currentTurn / Options::autosaveFrequency) % Options::autosaveSlots;
			_filename = "_" + std::to_string(slotIndex) + SavedGame::AUTOSAVE_BATTLESCAPE;
		}
		else
		{
			// classic autosave
			_filename = SavedGame::AUTOSAVE_BATTLESCAPE;
		}
		break;
	case SAVE_IRONMAN:
	case SAVE_IRONMAN_END:
		_filename = CrossPlatform::sanitizeFilename(_game->getSavedGame()->getName()) + ".sav";
		break;
	default:
		break;
	}

	buildUi(palette);
}

/**
 *
 */
SaveGameState::~SaveGameState()
{

}

/**
 * Builds the interface.
 * @param palette Parent state palette.
 */
void SaveGameState::buildUi(SDL_Color *palette)
{
	_screen = false;

	// Create objects
	_txtStatus = new Text(320, 17, 0, 92);

	// Set palette
	setStatePalette(palette);

	if (_origin == OPT_BATTLESCAPE)
	{
		add(_txtStatus, "textLoad", "battlescape");
		_txtStatus->setHighContrast(true);
	}
	else
	{
		add(_txtStatus, "textLoad", "geoscape");
	}

	centerAllSurfaces();

	// Set up objects
	_txtStatus->setBig();
	_txtStatus->setAlign(ALIGN_CENTER);
	_txtStatus->setText(tr("STR_SAVING_GAME"));

}

/**
 * Saves the current save.
 */
void SaveGameState::think()
{
	State::think();
	// Make sure it gets drawn properly
	if (_firstRun < 10)
	{
		_firstRun++;
	}
	else
	{
		_game->popState();

		switch (_type)
		{
		case SAVE_DEFAULT:
			// manual save, close the save screen
			_game->popState();
			if (!_game->getSavedGame()->isIronman())
			{
				// and pause screen too
				_game->popState();
			}

			// coop
			_game->getCoopMod()->setPauseOff();

			break;
		case SAVE_INSTA:
			// timestamp is visible already, no need to repeat it
			_game->getSavedGame()->setName(tr("STR_INSTA_SAVE"));
			break;
		case SAVE_QUICK:
		case SAVE_AUTO_GEOSCAPE:
		case SAVE_AUTO_BATTLESCAPE:
			// automatic save, give it a default name
			_game->getSavedGame()->setName(_filename);
		default:
			break;
		}

		// Host-save authority: a coop client never writes save data to disk -
		// the host save (with the embedded client world) is the single
		// authority. Swallow the save silently; UI states were popped above.
		if (!_game->getCoopMod()->localSavesAllowed())
		{
			if (_type == SAVE_IRONMAN_END)
			{
				Screen::updateScale(Options::geoscapeScale, Options::baseXGeoscape, Options::baseYGeoscape, true);
				_game->getScreen()->resetDisplay(false);

				_game->setState(new MainMenuState);
				_game->setSavedGame(0);
			}
			// PRD-J02: a JOINT replica must never write to disk. Autosaves stay
			// silent (they fire automatically), but a user-initiated save gets an
			// explicit refusal popup (same UX as the PauseState "cannot save"
			// dialog) so the player understands nothing was written.
			else if (_game->getCoopMod()->isJointReplica()
				&& _type != SAVE_AUTO_GEOSCAPE && _type != SAVE_AUTO_BATTLESCAPE)
			{
				_game->pushState(new CoopState(123));
			}
			return;
		}

		// Save the game
		try
		{

			// host-save authority: a host geoscape save pulls fresh client progress
			// before writing, so the embedded client world is current. Ironman-end
			// is excluded (it needs the immediate write + MainMenu transition below).
			bool deferHostSave = false;
			if ((_game->getCoopMod()->isCoopSession() == true && _game->getCoopMod()->getServerOwner() == true && _game->getSavedGame() && !_game->getSavedGame()->getSavedBattle()) && _game->getCoopMod()->coopMissionEnd == false && _type != SAVE_IRONMAN_END)
			{

				if (_type != SAVE_AUTO_GEOSCAPE && _type != SAVE_AUTO_BATTLESCAPE)
				{
					connectionTCP::saveID = _game->getCoopMod()->getDateTimeCoop();
				}

				// PRD-06/E1: arm the deferral and SKIP the immediate write. The
				// single write happens once the client's fresh world blob arrives
				// (MAP_RESULT_SAVE_PROGRESS handler -> writePendingHostSave), so
				// the .sav is serialized+written once, not twice, and always
				// embeds the freshest client world instead of a round-trip-stale one.
				connectionTCP::session.armDeferredSave(_filename);

				CoopState* coopWindow = new CoopState(COOP_DLG_HOST_SAVE_WAIT);
				_game->pushState(coopWindow);

				deferHostSave = true;
			}

			if (deferHostSave)
			{
				// deferred: the handler writes it. Clear the impatient-user input
				// queue and return without an immediate emit.
				SDL_Event e;
				while (SDL_PollEvent(&e))
				{
					// do nothing
				}
				return;
			}

			std::string backup = _filename + ".bak";
			_game->getSavedGame()->save(backup, _game->getMod());
			std::string fullPath = Options::getMasterUserFolder() + _filename;
			std::string bakPath = Options::getMasterUserFolder() + backup;

			// coop
			if (!CrossPlatform::moveFile(bakPath, fullPath) && _game->getCoopMod()->getCoopStatic() == false)
			{
				throw Exception("Save backed up in " + backup);
			}

			if (_type == SAVE_IRONMAN_END)
			{
				Screen::updateScale(Options::geoscapeScale, Options::baseXGeoscape, Options::baseYGeoscape, true);
				_game->getScreen()->resetDisplay(false);

				_game->setState(new MainMenuState);
				_game->setSavedGame(0);
			}

			// Clear the SDL event queue (i.e. ignore input from impatient users)
			SDL_Event e;
			while (SDL_PollEvent(&e))
			{
				// do nothing
			}
		}
		catch (Exception &e)
		{
			error(e.what());
		}
		catch (YAML::Exception &e)
		{
			error(e.what());
		}
	}
}

/**
 * Pops up a window with an error message.
 * @param msg Error message.
 */
void SaveGameState::error(const std::string &msg)
{
	Log(LOG_ERROR) << msg;
	std::ostringstream error;
	error << tr("STR_SAVE_UNSUCCESSFUL") << Unicode::TOK_NL_SMALL << msg;
	if (_origin != OPT_BATTLESCAPE)
		_game->pushState(new ErrorMessageState(error.str(), _palette, _game->getMod()->getInterface("errorMessages")->getElement("geoscapeColor")->color, "BACK01.SCR", _game->getMod()->getInterface("errorMessages")->getElement("geoscapePalette")->color));
	else
		_game->pushState(new ErrorMessageState(error.str(), _palette, _game->getMod()->getInterface("errorMessages")->getElement("battlescapeColor")->color, "TAC00.SCR", _game->getMod()->getInterface("errorMessages")->getElement("battlescapePalette")->color));
}

}
