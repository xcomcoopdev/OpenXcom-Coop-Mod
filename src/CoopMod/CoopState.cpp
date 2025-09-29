/*
 * Copyright 2010-2016 OpenXcom Developers.
 * Copyright 2023-2026 XComCoopTeam (https://www.moddb.com/mods/openxcom-coop-mod)
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

#include "../Battlescape/BattlescapeGame.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Interface/Text.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Mod/Mod.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Menu/AbandonGameState.h"
#include "../Menu/ListLoadState.h"
#include "../Menu/ListSaveState.h"
#include "../Menu/OptionsBattlescapeState.h"
#include "../Menu/OptionsGeoscapeState.h"
#include "../Menu/OptionsVideoState.h"
#include "../Menu/PauseState.h"
#include "../Geoscape/GeoscapeState.h"
#include "CoopState.h"

#include "../Basescape/BasescapeState.h"

#include "../Savegame/Soldier.h"
#include "../Savegame/Vehicle.h"

#include "../Menu/SaveGameState.h"
#include "../Menu/LoadGameState.h"

#include "../Menu/MainMenuState.h"

namespace OpenXcom
{

int global_state = 0;
int state_counter = 0;
Globe *currentGlobe = 0;

/**
 * Initializes all the elements in the Pause window.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 */
CoopState::CoopState(int state)
{
	_screen = false;

	global_state = state;

	int x = 20;

	_window = new Window(this, 216, 160, x, 20, POPUP_BOTH);

	_txtTitle = new Text(206, 17, x + 5, 100);
	_btnMessage = new TextButton(100, 17, x + 55, 100);
	_btnBack = new TextButton(100, 17, x + 55, 150);
	_btnYes = new TextButton(80, 20, 40, 150);

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_txtTitle, "text", "pauseMenu");
	add(_btnMessage, "button", "pauseMenu");
	add(_btnBack, "button", "pauseMenu");
	add(_btnYes, "button", "pauseMenu");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "pauseMenu");

	if (_game->getSavedGame())
	{
		if (_game->getSavedGame()->getSavedBattle())
		{
			
			applyBattlescapeTheme("pauseMenu");
			_origin = OPT_BATTLESCAPE;
			
		}
	}

	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setBig();
	_txtTitle->setText("WAIT");

	_btnMessage->setVisible(false);
	_btnBack->setVisible(false);
	_btnYes->setVisible(false);

	_btnMessage->setText(tr("OK"));
	_btnMessage->onMouseClick((ActionHandler)&CoopState::loadCoop);

	_btnBack->setText(tr("OK"));
	_btnBack->onMouseClick((ActionHandler)&CoopState::previous);

	_btnYes->setText(tr("STR_YES"));
	_btnYes->onMouseClick((ActionHandler)&CoopState::btnYesClick);
	_btnYes->onKeyboardPress((ActionHandler)&CoopState::btnYesClick, Options::keyOk);

	// Main campaign base defense
	if (state == 77)
	{

		std::string result = "Waiting for " + _game->getCoopMod()->getCurrentClientName();
		_txtTitle->setText(result);

		_btnBack->setText("Disconnect");
		_btnBack->setVisible(true);

		Json::Value obj;
		obj["state"] = "sendCraft";
		_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());

	}

	if (state == 88)
	{
		std::string result = "Waiting for " + _game->getCoopMod()->getCurrentClientName();
		_txtTitle->setText(result);

		_btnBack->setText("Disconnect");
		_btnBack->setVisible(true);


		// coop new  battle
		if (_game->getCoopMod()->getCoopCampaign() == false && _game->getCoopMod()->getCoopStatic() == true)
		{

			Base* selected_base = _game->getSavedGame()->getSelectedBase();

			for (auto* soldier : *selected_base->getSoldiers())
			{

				// new coop soldier
				if (soldier->getCraft())
				{

					soldier->setCoopBase(selected_base->_coop_base_id);
					soldier->setCoopCraft(soldier->getCraft()->getId());
					soldier->setCoopCraftType(soldier->getCraft()->getType());
				}
				else
				{

					soldier->setCoopBase(-1);
				}
			}
		}

		Json::Value obj;
		obj["state"] = "sendCraft";
		_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());

	}

	if (state == 0)
	{
	}

	if (state == 99)
	{
		_txtTitle->setSmall();
		_txtTitle->setText("Completed Research by\n " + _game->getCoopMod()->getCurrentClientName() + ".");
		_btnBack->setVisible(true);
	}

	if (state == 150)
	{
		_txtTitle->setSmall();
		_txtTitle->setText("Items received from " + _game->getCoopMod()->getCurrentClientName() + ".\nTransferred to " + _game->getCoopMod()->current_base_name + " base.");
		_btnBack->setVisible(true);
	}

	// base orig
	if (state == 66)
	{
		_txtTitle->setSmall();
		_txtTitle->setText("Go to Geoscape to begin the co-op mission.");
		_btnBack->setVisible(true);
	}

	if (state == 979)
	{
		_txtTitle->setSmall();
		_txtTitle->setText("Cannot connect to server.\nClick OK to return to the main menu.");
		_game->getCoopMod()->disconnectTCP();
		_btnBack->setVisible(true);
	}

	// DOWNLOAD MAP
	if (state == 1)
	{
		_txtTitle->setText("Downloading map...");
		_btnBack->setText("Disconnect");
		_btnBack->setVisible(true);

	}

	// CLIENT WAITING
	if (state == 3)
	{
		std::string result = "Waiting for " + _game->getCoopMod()->getCurrentClientName();
		_txtTitle->setText(result);

		_btnBack->setText("Disconnect");
		_btnBack->setVisible(true);

	}

	// CLIENT DATA
	if (state == 4)
	{

		// coop new  battle
		if (_game->getCoopMod()->getCoopCampaign() == false && _game->getCoopMod()->getCoopStatic() == true)
		{

			Base* selected_base = _game->getSavedGame()->getSelectedBase();

			for (auto* soldier : *selected_base->getSoldiers())
			{

				// new coop soldier
				if (soldier->getCraft())
				{

					soldier->setCoopBase(selected_base->_coop_base_id);
					soldier->setCoopName(soldier->getName());
					soldier->setCoopCraft(soldier->getCraft()->getId());
					soldier->setCoopCraftType(soldier->getCraft()->getType());
				}
				else
				{

					soldier->setCoopBase(-1);
					soldier->setCoopName("");
					soldier->setCoopCraft(-1);
					soldier->setCoopCraftType("");
				}

			}

		}

		_txtTitle->setText("Please wait...");

		_btnBack->setText("Disconnect");
		_btnBack->setVisible(true);

	}



	if (state == 100)
	{
		_txtTitle->setVisible(true);
		_btnMessage->setVisible(false);
		_txtTitle->setText("PLayer Connected!");
	}

	// TCP
	if (state == 15)
	{
		_txtTitle->setText("Connecting...");
		_btnBack->setVisible(true);
		_btnBack->setText(tr("STR_CANCEL_UC"));

	}

	// SERVER ERROR
	if (state == 440)
	{

		_txtTitle->setSmall();
		_txtTitle->setText("Server error.\nConnection closed.");
		_btnBack->setVisible(true);
		_game->getCoopMod()->disconnectTCP();
		_game->popState();

	}

	// SERVER FULL
	if (state == 444)
	{

		_txtTitle->setSmall();
		_txtTitle->setText("The server is full.\nMaximum allowed is 2 players.");
		_btnBack->setVisible(true);
		_game->getCoopMod()->disconnectTCP();
		_game->popState();

	}

	if (state == 16)
	{
		_txtTitle->setText("Cannot connect to server");
		_btnBack->setVisible(true);
		_game->getCoopMod()->disconnectTCP();
		_game->popState();
	}

	if (state == 20)
	{
		_txtTitle->setText(_game->getCoopMod()->getCurrentClientName() + " has left the server");
		_btnBack->setVisible(true);

		// disconnect
		connectionTCP::_coopGamemode = 0;
		_game->getCoopMod()->disconnectTCP();
	}

	if (state == 21)
	{
		connectionTCP::_coopGamemode = 0;
		_txtTitle->setText("Server connection lost");
		_btnBack->setVisible(true);
		_game->getCoopMod()->disconnectTCP();
	}

	if (state == 50)
	{
		_txtTitle->setText("Synchronizing bases...");
		_btnBack->setText("Disconnect");
		_btnBack->setVisible(true);

		if (_game->getCoopMod()->getHost() == true)
		{
			Json::Value obj;
			obj["state"] = "SEND_FILE_HOST_BASE";
			OutputDebugStringA(obj.toStyledString().c_str());
			_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());
		}
		else
		{

			Json::Value obj;
			obj["state"] = "SEND_FILE_CLIENT_BASE";
			OutputDebugStringA(obj.toStyledString().c_str());
			_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());
		}

	}

	// out of sync
	if (state == 999)
	{

		_txtTitle->setText("ERROR: Out Of Sync");
		_btnBack->setVisible(true);

		// disconnect
		connectionTCP::_coopGamemode = 0;
		_game->getCoopMod()->disconnectTCP();

	}

	// JSON ERROR
	if (state == 250)
	{

		_txtTitle->setText("ERROR: Invalid or corrupted packet data");
		_btnBack->setVisible(true);

		// disconnect
		connectionTCP::_coopGamemode = 0;
		_game->getCoopMod()->disconnectTCP();
	}

	// Mod Compatibility
	if (state == 1000)
	{
		_txtTitle->setSmall();
		_txtTitle->setText("Mod Compatibility Issue.\nEnsure both have the same mods installed.");
		_btnBack->setVisible(true);

		// disconnect
		connectionTCP::_coopGamemode = 0;
		_game->getCoopMod()->disconnectTCP();

		_game->popState();

	}

	// campaign
	if (state == 2000)
	{
		_txtTitle->setSmall();
		_txtTitle->setText("Please load a Campaign save\n to join the session.");
		_btnBack->setVisible(true);

		// disconnect
		connectionTCP::_coopGamemode = 0;
		_game->getCoopMod()->disconnectTCP();

		_game->popState();

	}

	// new  battle
	if (state == 3000)
	{
		_txtTitle->setSmall();
		_txtTitle->setText("Please click the 'New Battle' button\n in the main menu to join the session.");
		_btnBack->setVisible(true);

		// disconnect
		connectionTCP::_coopGamemode = 0;
		_game->getCoopMod()->disconnectTCP();

		_game->popState();
	}

	// Client-side error
	if (state == 123)
	{

		std::string message =
			"Saving this file is not recommended\n"
			"due to detected desync problems.\n\n"
			"Are you sure you want to proceed?";

		_txtTitle->setSmall();
		_txtTitle->setText(message);
		_btnBack->setVisible(true);

		_btnBack->setText(tr("STR_NO"));

		_btnBack->setX(136);
		_btnBack->setY(150);
		_btnBack->setWidth(80);
		_btnBack->setHeight(20);

		_txtTitle->setHeight(40);
		_txtTitle->setY(80);

		_btnYes->setVisible(true);

	}

}

/**
 *
 */
CoopState::~CoopState()
{
}

void CoopState::think()
{

	State::think();

	static Uint32 lastUpdate = 0;
	Uint32 now = SDL_GetTicks();

	if (now - lastUpdate >= 500)
	{

		if (global_state == 1)
		{

			if (state_counter == 0)
			{
				_txtTitle->setText("Downloading map.");
				state_counter = 1;
			}
			else if (state_counter == 1)
			{
				_txtTitle->setText("Downloading map..");
				state_counter = 2;
			}
			else if (state_counter == 2)
			{
				_txtTitle->setText("Downloading map...");
				state_counter = 0;
			}

		}
		else if (global_state == 4)
		{

			if (state_counter == 0)
			{
				_txtTitle->setText("Please wait.");
				state_counter = 1;
			}
			else if (state_counter == 1)
			{
				_txtTitle->setText("Please wait..");
				state_counter = 2;
			}
			else if (state_counter == 2)
			{
				_txtTitle->setText("Please wait...");
				state_counter = 0;
			}

		}
		else if (global_state == 15)
		{

			if (state_counter == 0)
			{
				_txtTitle->setText("Connecting.");
				state_counter = 1;
			}
			else if (state_counter == 1)
			{
				_txtTitle->setText("Connecting..");
				state_counter = 2;
			}
			else if (state_counter == 2)
			{
				_txtTitle->setText("Connecting...");
				state_counter = 0;
			}

		}
		else if (global_state == 50)
		{

			if (state_counter == 0)
			{
				_txtTitle->setText("Synchronizing bases.");
				state_counter = 1;
			}
			else if (state_counter == 1)
			{
				_txtTitle->setText("Synchronizing bases..");
				state_counter = 2;
			}
			else if (state_counter == 2)
			{
				_txtTitle->setText("Synchronizing bases...");
				state_counter = 0;
			}

		}


		lastUpdate = now;

	}

	//  coop fix
	if (_game->getCoopMod()->isCoopSession() == true && _game->getCoopMod()->getCoopStatic() == false && _btnBack->getVisible() == false)
	{
		_btnBack->setVisible(true);
		_txtTitle->setText("Connection cannot be established.");
		_txtTitle->setSmall();

		_game->getCoopMod()->disconnectTCP();

	}

}

void CoopState::previous(Action *)
{

	// disconnect
	if (global_state == 50 || global_state == 1 || global_state == 88 || global_state == 3 || global_state == 4 || global_state == 15)
	{

		if (global_state == 15)
		{
			_game->getCoopMod()->cancel_connect = true;
		}

		_game->getCoopMod()->disconnectTCP();
		_game->popState();

	}
	// main menu
	else if (global_state == 979)
	{

		_game->setState(new MainMenuState);
	}
	else
	{
		_game->popState();
	}


}

void CoopState::btnYesClick(Action *)
{

	if (global_state == 123)
	{

		_game->pushState(new ListSaveState(_origin));

	}

}

void CoopState::loadWorld()
{

	// load coop mission
	if (global_state == 765)
	{

		if (_game->getCoopMod()->getServerOwner() == true)
		{
			_game->pushState(new LoadGameState(_origin, "host/battlehost.data", _palette));
		}
		else
		{
			_game->pushState(new LoadGameState(_origin, "client/battlehost.data", _palette));
		}

	}
	// set client soldier
	else if (global_state == 111)
	{

		// own path
		std::string filename = "host/battleclient.data";

		if (_game->getCoopMod()->getServerOwner() == false)
		{
			filename = "client/battleclient.data";
		}

		std::string filepath = Options::getMasterUserFolder() + filename;

		if (OpenXcom::CrossPlatform::fileExists(filepath))
		{

			Base* selected_base = 0;

			// fix
			if (_game->getSavedGame()->getSelectedBase())
			{

				selected_base = _game->getSavedGame()->getSelectedBase();
			}
			else
			{

				selected_base = _game->getCoopMod()->getSelectedCraft()->getBase();
			}

			// RECEIVE CLIENT DATA
			SavedGame* client_save = new SavedGame();

			client_save->load(filename, _game->getMod(), _game->getLanguage());

			Base* hostBase = selected_base;

			Craft* selected_craft = _game->getCoopMod()->getSelectedCraft();

			int space_available = 0;

			if (selected_craft)
			{
				space_available = _game->getCoopMod()->getSelectedCraft()->getNumTotalSoldiers() + _game->getCoopMod()->getSelectedCraft()->getSpaceAvailable();
			}

			// HOST SOLDIERS
			for (auto& host_soldier : *hostBase->getSoldiers())
			{

				if (host_soldier->getCraft())
				{

					// if same craft
					if (host_soldier->getCraft() == selected_craft)
					{

						host_soldier->setCoop(0);
						host_soldier->setCoopBase(-1);

					}

				}
				// base defense
				else if (_game->getCoopMod()->_isMainCampaignBaseDefense == true)
				{

					host_soldier->setCoop(0);
					host_soldier->setCoopBase(-1);

				}

			}


			// CLIENT SOLDIERS
			for (auto& client_base : *client_save->getBases())
			{

				// ITEROIDAAN SOTILAAT
				for (auto& soldier : *client_base->getSoldiers())
				{

					// check if match
					if ((soldier->getCoopBase() == hostBase->_coop_base_id) || _game->getCoopMod()->getCoopCampaign() == false)
					{

						if (soldier->getCoopCraft() != -1 || _game->getCoopMod()->_isMainCampaignBaseDefense == true)
						{

							// if the same craft
							std::vector<Soldier*>* soldiers = hostBase->getSoldiers();

							Soldier* lastSoldier = soldiers->back(); // Points to the last soldier
							int lastId = lastSoldier->getId();       // Assuming Soldier class has getId()

							// Check if one with the same name already exists
							auto it = std::find_if(soldiers->begin(), soldiers->end(), [&](Soldier* s)
												   { return s->getName() == soldier->getName(); });

							// If found, remove it
							if (it != soldiers->end())
							{
								delete *it; // Remove the old soldier if needed
								soldiers->erase(it);
							}


							if (selected_craft)
							{

								// If there is space, add a new one
								if ((space_available > 0 && selected_craft->getId() == soldier->getCoopCraft() && selected_craft->getRules()->getType() == soldier->getCoopCraftType()))
								{

									int newId = lastId + 1;

									soldier->setId(newId);
									soldier->setCoop(1);
									soldier->calcStatString(_game->getMod()->getStatStrings(), false);
									soldiers->push_back(soldier);

									soldier->setCraftAndMoveEquipment(selected_craft, hostBase, _game->getSavedGame()->getMonthsPassed() == -1);

									space_available--;
								}


							}
							else if (_game->getCoopMod()->_isMainCampaignBaseDefense == true)
							{

								int newId = lastId + 1;

								soldier->setId(newId);
								soldier->setCoop(1);
								soldier->calcStatString(_game->getMod()->getStatStrings(), false);
								soldiers->push_back(soldier);

								soldier->setCraftAndMoveEquipment(selected_craft, hostBase, _game->getSavedGame()->getMonthsPassed() == -1);

							}


						}
					}
				}

				if (selected_craft)
				{


					// HOST VEHICLES
					for (auto& host_vehicle : *_game->getCoopMod()->getSelectedCraft()->getVehicles())
					{

						host_vehicle->setCoop(0);
						host_vehicle->setCoopBase(-1);
					}

					// CLIENT VEHICLES
					for (auto& client_craft : *client_base->getCrafts())
					{

						for (auto& vehicle : *client_craft->getVehicles())
						{

							if (selected_craft->getId() == vehicle->getCoopCraft() && selected_craft->getType() == vehicle->getCoopCraftType() && (vehicle->getCoopBase() == hostBase->_coop_base_id) || _game->getCoopMod()->getCoopCampaign() == false)
							{

								selected_craft->makeCoopVehicle(vehicle);
							}
						}
					}


				}



			}
		}
	}
	else if (global_state == 888)
	{
		if (_game->getCoopMod()->getServerOwner() == true)
		{
			_game->pushState(new LoadGameState(_origin, "host/battleclient.data", _palette));
		}
		else
		{
			_game->pushState(new LoadGameState(_origin, "client/battleclient.data", _palette));
		}
	}
	else if (global_state == 777)
	{
		if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->save("host/basehost.data", _game->getMod());
		}
		else if (_game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->save("client/basehost.data", _game->getMod());
		}
	}
	else if (global_state == 666)
	{
		if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->save("host/battlehost.data", _game->getMod());
		}
		else if (_game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->save("client/battlehost.data", _game->getMod());
		}

	}
	else if (global_state == 55)
	{

			_game->popState();
		
			SavedGame *oldsave = _game->getSavedGame();

			SavedGame* newsave = new SavedGame();

			std::string filename = "";

			if (_game->getCoopMod()->getServerOwner() == true)
			{
				// write
				_game->getCoopMod()->coopFunds = oldsave->getFunds();
				oldsave->save("host/basehost.data", _game->getMod());

				filename = "host/baseclient.data"; 

			}
			else
			{
				// write
				_game->getCoopMod()->coopFunds = oldsave->getFunds();
				oldsave->save("client/basehost.data", _game->getMod());

				filename = "client/baseclient.data"; 

			}

			std::vector<Soldier*> current_soldiers;

			std::string filepath = Options::getMasterUserFolder() + filename;

			if (OpenXcom::CrossPlatform::fileExists(filepath))
			{

				newsave->load(filename, _game->getMod(), _game->getLanguage());


				for (auto& newbase : *newsave->getBases())
				{

					newbase->isCoopBase(true);

					// clear all vehicles and soldiers from the base
					newbase->getSoldiers()->clear();

					newbase->getVehicles()->clear();

					for (auto* unit_base : *oldsave->getBases())
					{

						// vehicles
						for (auto& old_craft : *unit_base->getCrafts())
						{

							for (auto& old_vehicle : *old_craft->getVehicles())
							{

								for (auto& new_craft : *newbase->getCrafts())
								{

									// find the old co-op vehicle that matches the new co-op vehicle
									if (old_vehicle->getCoopBase() == newbase->_coop_base_id && new_craft->getType() == old_vehicle->getCoopCraftType() && new_craft->getId() == old_vehicle->getCoopCraft())
									{

										new_craft->getVehicles()->push_back(old_vehicle->clone());

										break;

									}

								}

							}

						}


					
						// soldiers
						for (auto& soldier : *unit_base->getSoldiers())
						{

							// if a co-op soldier is found in the co-op base
							if (soldier->getCoopBase() == newbase->_coop_base_id)
							{

								Soldier* deep_copied_soldier = soldier->deepCopy(_game->getMod(), _game->getSavedGame());

								newbase->getSoldiers()->push_back(deep_copied_soldier);

							}
						}


					}

				

				}
		



			}



			_game->setSavedGame(newsave);

			// select the clicked base
			Base* selected_base = newsave->getBases()->front();

			for (auto* base : *newsave->getBases())
			{
				if (base->getName() == _game->getCoopMod()->current_base_name)
				{
					selected_base = base;
				}
			}

			_game->pushState(new BasescapeState(selected_base, currentGlobe));


	}
	else
	{

		// battlescape
		if (_game->getCoopMod()->getHost() == true)
		{

			if (_game->getCoopMod()->getServerOwner() == true)
			{
				_game->pushState(new LoadGameState(_origin, "host/battleclient.data", _palette));
			}
			else
			{
				_game->pushState(new LoadGameState(_origin, "client/battleclient.data", _palette));
			}

		}
		else
		{

			if (_game->getCoopMod()->getServerOwner() == true)
			{
				
				// save the co-op mission so it can be continued later
				SavedGame *oldsave = new SavedGame(*_game->getSavedGame());
				oldsave->setName("coop_mission");
				oldsave->save("coop_mission.sav", _game->getMod());

				// load battle
				_game->pushState(new LoadGameState(_origin, "host/battleclient.data", _palette));

			}
			else
			{

				// save the co-op mission so it can be continued later
				SavedGame *oldsave = new SavedGame(*_game->getSavedGame());
				oldsave->setName("coop_mission");
				oldsave->save("coop_mission.sav", _game->getMod());

				// load the battle
				_game->pushState(new LoadGameState(_origin, "client/battleclient.data", _palette));

			}

		}
	}

}

void CoopState::setGlobe(Globe *globe)
{
	currentGlobe = globe;
}

void CoopState::loadCoop(Action *)
{

	loadWorld();

}

}
