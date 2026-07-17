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
#include "LoadGameState.h"
#include <sstream>
#include "../Engine/Logger.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Engine/Game.h"
#include "../Engine/Exception.h"
#include "../Engine/Options.h"
#include "../Engine/CrossPlatform.h"
#include "../Engine/Screen.h"
#include "../Engine/LocalizedText.h"
#include "../Interface/Text.h"
#include "../Geoscape/GeoscapeState.h"
#include "ErrorMessageState.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Mod/Mod.h"
#include "../Engine/Sound.h"
#include "../Engine/Unicode.h"
#include "../Mod/RuleInterface.h"
#include "StatisticsState.h"
#include "../CoopMod/HostMenu.h"
#include "../CoopMod/CoopState.h"
#include "../CoopMod/JointEcon.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the Load Game screen.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 * @param filename Name of the save file without extension.
 * @param palette Parent state palette.
 */
LoadGameState::LoadGameState(OptionsOrigin origin, const std::string& filename, SDL_Color* palette, const std::string& coopKey, bool loadCoopProgress) : _firstRun(0), _origin(origin), _filename(filename), _coopKey(coopKey), _loadCoopProgress(loadCoopProgress)
{

	// coop
	connectionTCP::_coopGamemode = 0;

	buildUi(palette);
}

/**
 * Initializes all the elements in the Load Game screen.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 * @param type Type of auto-load being used.
 * @param palette Parent state palette.
 */
LoadGameState::LoadGameState(OptionsOrigin origin, SaveType type, SDL_Color *palette) : _firstRun(0), _origin(origin), _loadCoopProgress(false)
{

	// coop
	connectionTCP::_coopGamemode = 0;

	switch (type)
	{
	case SAVE_QUICK:
		_filename = SavedGame::QUICKSAVE;
		break;
	case SAVE_AUTO_GEOSCAPE:
		_filename = SavedGame::AUTOSAVE_GEOSCAPE;
		break;
	case SAVE_AUTO_BATTLESCAPE:
		_filename = SavedGame::AUTOSAVE_BATTLESCAPE;
		break;
	default:
		// can't auto-load ironman games
		break;
	}

	buildUi(palette);
}

/**
 *
 */
LoadGameState::~LoadGameState()
{

}

/**
 * Builds the interface.
 * @param palette Parent state palette.
 */
void LoadGameState::buildUi(SDL_Color *palette)
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
		// coop fix
		if (_game->getSavedGame()->getSavedBattle())
		{
			if (_game->getSavedGame()->getSavedBattle()->getAmbientSound() != Mod::NO_SOUND)
			{
				_game->getMod()->getSoundByDepth(0, _game->getSavedGame()->getSavedBattle()->getAmbientSound())->stopLoop();
			}
		}
	}
	else
	{
		add(_txtStatus, "textLoad", "geoscape");
	}

	centerAllSurfaces();

	// Set up objects
	_txtStatus->setBig();
	_txtStatus->setAlign(ALIGN_CENTER);
	_txtStatus->setText(tr("STR_LOADING_GAME"));

}

/**
 * Ignore quick loads without a save available.
 */
void LoadGameState::init()
{
	State::init();

	// Chokepoint gate: a machine that may not touch local saves (coop client)
	// can only reach a plain local load through the pause menu / list / confirm
	// states or a directly-pushed LoadGameState. The coop-orchestrated flows -
	// loading a host-provided blob (_coopKey set) or a resume/rejoin
	// (_loadCoopProgress) - are NOT local loads and must still run. Refuse only
	// the plain local load.
	if (!_game->getCoopMod()->localLoadsAllowed() && _coopKey.empty() && !_loadCoopProgress)
	{
		Log(LOG_INFO) << "[coop] local load refused: no local loads during a live coop session (PRD-08)";
		_game->popState();
		return;
	}

	if (_filename == SavedGame::QUICKSAVE && !CrossPlatform::fileExists(Options::getMasterUserFolder() + _filename))
	{
		_game->popState();
		return;
	}
}

/**
 * Loads the specified save.
 */
void LoadGameState::think()
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

		// Remember for later (palette reset)
		BattlescapeState *origBattleState = 0;
		if (_game->getSavedGame() != 0 && _game->getSavedGame()->getSavedBattle() != 0)
		{
			origBattleState = _game->getSavedGame()->getSavedBattle()->getBattleState();
		}

		// Reset touch flags
		_game->resetTouchButtonFlags();

		// Load the game
		SavedGame *s = new SavedGame();
		try
		{

			// coop
			if (_coopKey.empty())
			{
				s->load(_filename, _game->getMod(), _game->getLanguage());
				// the loaded save is now the authority - drop stale
				// in-memory transfer session state
				_game->getCoopMod()->resetGiftSessionState();
			}
			else
			{
				s->loadCoopSaveFromMemory(_filename, _game->getMod(), _game->getLanguage(), _coopKey);
			}

			// PRD-J02: a JOINT save carries the host's single authoritative world
			// and cannot be opened as a plain single-player game. On a build
			// without JOINT support the type key is unknown and the whole feature
			// is absent; on this build we refuse the one reachable downgrade - a
			// JOINT-typed save that is not co-op-marked (e.g. hand-edited).
			if (s->getCampaignType() == CoopCampaignType::Joint && !s->isCoopSave())
			{
				throw Exception("This is a JOINT co-op campaign save and cannot be opened as a single-player game.");
			}

			_game->setSavedGame(s);

			// PRD-J02: a JOINT client just adopted the host's world as its replica.
			// The streamed save carries the HOST's coop_save_owner_player_id (0);
			// re-assert this machine's own seat so localSeat() reflects the client,
			// not the host. (2-player transport: client seat = 1.)
			if (!_coopKey.empty()
				&& s->getCampaignType() == CoopCampaignType::Joint
				&& _game->getCoopMod()->getServerOwner() == false)
			{
				connectionTCP::coop_save_owner_player_id = 1;

				// PRD-J10: a fresh authoritative world landed - this is also how a
				// desync repair completes. Clear the resync in-flight guard so a
				// later drift can be repaired again (and so the "give up" latch is
				// re-armed for the next window).
				JointEcon::notifyWorldAdopted();
			}
			if (_game->getSavedGame()->getEnding() != END_NONE)
			{
				Options::baseXResolution = Screen::ORIGINAL_WIDTH;
				Options::baseYResolution = Screen::ORIGINAL_HEIGHT;
				_game->getScreen()->resetDisplay(false);
				_game->setState(new StatisticsState);
			}
			else
			{
				Options::baseXResolution = Options::baseXGeoscape;
				Options::baseYResolution = Options::baseYGeoscape;
				_game->getScreen()->resetDisplay(false);
				if (origBattleState != 0)
				{
					// We need to reset palettes here already, can't wait for the destructor
					origBattleState->resetPalettes();
				}
				_game->setState(new GeoscapeState);

				// coop fix
				if (_loadCoopProgress == true)
				{
					_game->getSavedGame()->setBattleGame(0);

					// resume/rejoin: report the loaded world to the host and
					// hold with frozen time until it resumes (F3/F4/D5)
					if (connectionTCP::session.lobbyMode != 0 && _game->getCoopMod()->getServerOwner() == false)
					{
						Json::Value root;
						root["state"] = "resume_ack";
						_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

						// (in a battle resume the follow-up battle stream
						// replaces the whole state stack, dialog included)
						_game->pushState(new CoopState(COOP_DLG_CLIENT_RESUME_HOLD));
					}
				}

				if (_game->getSavedGame()->getSavedBattle() != 0)
				{
					_game->getSavedGame()->getSavedBattle()->loadMapResources(_game->getMod());
					Options::baseXResolution = Options::baseXBattlescape;
					Options::baseYResolution = Options::baseYBattlescape;
					_game->getScreen()->resetDisplay(false);
					BattlescapeState *bs = new BattlescapeState;
	
					// COOP
					if ((_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->inventory_battle_window == true))
					{
		
						Base *selected_base = _game->getSavedGame()->getSelectedBase();

						if (!selected_base)
						{
							selected_base = _game->getSavedGame()->getBases()->front();
						}
						BriefingState *bri = new BriefingState(0, selected_base);

						bri->loadCoop();

						_game->pushState(bri);
			
					}
					else
					{

						// coop
						// if pvp gamemode
						if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->getHost() == false)
						{

							if (connectionTCP::getCoopGamemode() == 2 || connectionTCP::getCoopGamemode() == 3)
							{

								for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
								{

									if (unit->getCoop() == 1)
									{
										unit->convertToFaction(FACTION_PLAYER);
										unit->setOriginalFaction(FACTION_PLAYER);
									}
									else if (unit->getFaction() != FACTION_NEUTRAL)
									{
										unit->convertToFaction(FACTION_HOSTILE);
										unit->setOriginalFaction(FACTION_HOSTILE);

										std::string alienName = "MALE_CIVILIAN";

										if (unit->getGeoscapeSoldier())
										{

											if (unit->getGeoscapeSoldier()->getGender() == GENDER_FEMALE)
											{
												alienName = "FEMALE_CIVILIAN";
											}
										}

										Unit* rule = _game->getMod()->getUnit(alienName, true);
										unit->setUnitRulesCoop(rule);
									}
								}
							}
						}
						// HOST PVP2
						else if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->getHost() == true && connectionTCP::getCoopGamemode() == 3)
						{

							for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
							{

								if (unit->getCoop() == 1)
								{
									unit->convertToFaction(FACTION_HOSTILE);
									unit->setOriginalFaction(FACTION_HOSTILE);

									std::string alienName = "MALE_CIVILIAN";

									if (unit->getGeoscapeSoldier())
									{

										if (unit->getGeoscapeSoldier()->getGender() == GENDER_FEMALE)
										{
											alienName = "FEMALE_CIVILIAN";
										}
									}

									Unit* rule = _game->getMod()->getUnit(alienName, true);
									unit->setUnitRulesCoop(rule);
								}
								else if (unit->getCoop() == 0)
								{
									unit->convertToFaction(FACTION_PLAYER);
									unit->setOriginalFaction(FACTION_PLAYER);
								}
							}
						}
						// CLIENT PVP2
						else if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->getHost() == false && connectionTCP::getCoopGamemode() == 3)
						{

							for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
							{

								if (unit->getCoop() == 1)
								{

									unit->convertToFaction(FACTION_PLAYER);
									unit->setOriginalFaction(FACTION_PLAYER);
								}
								else if (unit->getCoop() == 0)
								{

									unit->convertToFaction(FACTION_HOSTILE);
									unit->setOriginalFaction(FACTION_HOSTILE);

									std::string alienName = "MALE_CIVILIAN";

									if (unit->getGeoscapeSoldier())
									{

										if (unit->getGeoscapeSoldier()->getGender() == GENDER_FEMALE)
										{
											alienName = "FEMALE_CIVILIAN";
										}
									}

									Unit* rule = _game->getMod()->getUnit(alienName, true);
									unit->setUnitRulesCoop(rule);
								}
							}
						}

						// coop
						// reset tiles (PVP)
						if (_game->getCoopMod()->getCoopStatic() == true)
						{

							if ((_game->getCoopMod()->getCoopGamemode() == 3 && _game->getCoopMod()->getHost() == true) || (_game->getCoopMod()->getCoopGamemode() == 2 && _game->getCoopMod()->getHost() == false))
							{

								_game->getSavedGame()->getSavedBattle()->resetCoopTiles();
							}
						}

						_game->pushState(bs);

					}

					_game->getSavedGame()->getSavedBattle()->setBattleState(bs);
					// Try to reactivate the touch buttons
					bs->toggleTouchButtons(false, true);

					// battle-save resume / mid-battle rejoin, phase two
					// complete: report the loaded battle to the host (F3/F4)
					if (connectionTCP::session.lobbyMode != 0 && _game->getCoopMod()->getServerOwner() == false && _coopKey == "battleclient")
					{
						Json::Value root;
						root["state"] = "resume_ack";
						_game->getCoopMod()->sendTCPPacketData(root.toStyledString());
					}
				}

				// flow-redesign F3: a co-op campaign save loaded from the
				// menu opens a lobby-gated session - host window on top of
				// the paused world; RESUME serves every client its world.
				if (_coopKey.empty()
					&& _game->getSavedGame()->isCoopSave()
					&& _game->getCoopMod()->getCoopStatic() == false)
				{
					connectionTCP::session.adoptResumeSave();
					// the host identity is locked to the save (D4): adopt it,
					// so the UI flow can never host under a different name.
					// A migrated legacy save leaves the host slot blank (the
					// old format carried no host name) - keep the local name
					// and let the host claim the slot at re-host time.
					if (!_game->getSavedGame()->getCoopPlayers().empty()
						&& !_game->getSavedGame()->getCoopPlayers()[0].empty())
					{
						_game->getCoopMod()->setHostName(_game->getSavedGame()->getCoopPlayers()[0]);
					}
					_game->pushState(new HostMenu());
				}
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
			error(e.what(), s);
		}
		catch (YAML::Exception &e)
		{
			error(e.what(), s);
		}
		CrossPlatform::flashWindow();
	}
}

/**
 * Pops up a window with an error message
 * and cleans up afterwards.
 * @param msg Error message.
 * @param save Pending save.
 */
void LoadGameState::error(const std::string &msg, SavedGame *save)
{

	Log(LOG_ERROR) << msg;
	std::ostringstream error;
	error << tr("STR_LOAD_UNSUCCESSFUL") << Unicode::TOK_NL_SMALL << msg;
	if (_origin != OPT_BATTLESCAPE)
		_game->pushState(new ErrorMessageState(error.str(), _palette, _game->getMod()->getInterface("errorMessages")->getElement("geoscapeColor")->color, "BACK01.SCR", _game->getMod()->getInterface("errorMessages")->getElement("geoscapePalette")->color));
	else
		_game->pushState(new ErrorMessageState(error.str(), _palette, _game->getMod()->getInterface("errorMessages")->getElement("battlescapeColor")->color, "TAC00.SCR", _game->getMod()->getInterface("errorMessages")->getElement("battlescapePalette")->color));

	if (_game->getSavedGame() == save)
		_game->setSavedGame(0);
	else
		delete save;
}

}
