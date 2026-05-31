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

#include "HostMenu.h"
#include "../Menu/SaveGameState.h"
#include "../Menu/LoadGameState.h"
#include "CoopState.h"
#include "../Mod/ExtraSprites.h"
#include "../Engine/Surface.h"
#include "Profile.h"
#include "ChatMenu.h"
#include "connectionUDP/connection_rendezvous_glue.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the New Battle window.
 * @param game Pointer to the core game.
 */
HostMenu::HostMenu() : _craft(0), _selectType(NewBattleSelectType::MISSION), _isRightClick(false)
{

	// coop chat menu
	if (!_game->getCoopMod()->getChatMenu())
	{
		Font* smallFont = _game->getMod()->getFont("FONT_SMALL");
		_game->getCoopMod()->setChatMenu(new ChatMenu(smallFont, _game));
	}

	_screen = false;

	int x = 20;
	
	// Create objects
	_window = new Window(this, 216, 160, x, 20, POPUP_BOTH);
	_lstSaves = new TextList(180, 18, x + 18, 60);

	_port = new TextEdit(this, 180, 18, x + 18, 92);
	_serverName = new TextEdit(this, 180, 18, x + 18, 72);
	_tcpButtonHost = new TextButton(90, 18, x + 18, 152);
	_btnStartHotseat = new TextButton(180, 18, x + 18, 112);
	_cbxVisibility = new ComboBox(this, 180, 18, x + 18, 50);
	_cbxRegions = new ComboBox(this, 90, 18, x + 18, 112); 
	_cbxMaxPlayers = new ComboBox(this, 90, 18, x + 108, 112); 
	_password = new TextEdit(this, 180, 18, x + 18, 132);
	_btnCancel = new TextButton(90, 18, x + 108, 152);
	_txtTitle = new Text(206, 17, x + 5, 32);

	int screenWidth = Options::baseXGeoscape;
	int screenHeight = Options::baseYGeoscape;


	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);


	add(_window, "window", "pauseMenu");
	add(_port);
	add(_serverName);
	add(_password);
	add(_tcpButtonHost, "button", "pauseMenu");
	add(_btnStartHotseat, "button", "pauseMenu");
	add(_btnCancel, "button", "pauseMenu");
	add(_cbxVisibility, "button", "pauseMenu");
	add(_cbxRegions, "button", "pauseMenu");
	add(_cbxMaxPlayers, "button", "pauseMenu");
	add(_txtTitle, "text", "pauseMenu");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "pauseMenu");

	Uint8 color = 500; // 500 or 255

	if (_game->getSavedGame())
	{
		if (_game->getSavedGame()->getSavedBattle())
		{
			
			applyBattlescapeTheme("pauseMenu");
			color = 255;
			
		}
	}

	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setBig();
	_txtTitle->setText(tr("HOST"));

	// port
	_port->setColor(color);
	_port->setBig();
	_port->setBorderColor(color);
	_port->setText("PORT");
	_port->setVisible(false);

	_serverName->setColor(color);
	_serverName->setBig();
	_serverName->setBorderColor(color);
	_serverName->setText("Server");
	_serverName->setVisible(false);
	
	_tcpButtonHost->setText("START HOST");
	_tcpButtonHost->onMouseClick((ActionHandler)&HostMenu::hostTCPGame);
	_tcpButtonHost->setVisible(true);

	_btnStartHotseat->setText("ENABLE HOTSEAT");
	_btnStartHotseat->onMouseClick((ActionHandler)&HostMenu::startHotseat);
	_btnStartHotseat->setVisible(false);

	_btnCancel->setText(tr("CANCEL"));
	_btnCancel->onMouseClick((ActionHandler)&HostMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&HostMenu::btnCancelClick, Options::keyCancel);

	_visibilityTypes.push_back("VISIBILITY: PRIVATE (TCP)");
	_visibilityTypes.push_back("VISIBILITY: PUBLIC (UDP)");
	_visibilityTypes.push_back("HOTSEAT MODE");
	_cbxVisibility->setOptions(_visibilityTypes, false);
	_cbxVisibility->onChange((ActionHandler)&HostMenu::cbxVisibilityChange);

	_regionTypes.push_back("NORTH AMERICA");
	_regionTypes.push_back("ARCTIC");
	_regionTypes.push_back("ANTARCTICA");
	_regionTypes.push_back("SOUTH AMERICA");
	_regionTypes.push_back("EUROPE");
	_regionTypes.push_back("NORTH AFRICA");
	_regionTypes.push_back("SOUTHERN AFRICA");
	_regionTypes.push_back("CENTRAL ASIA");
	_regionTypes.push_back("SOUTH EAST ASIA");
	_regionTypes.push_back("SIBERIA");
	_regionTypes.push_back("AUSTRALASIA");
	_regionTypes.push_back("PACIFIC");
	_regionTypes.push_back("NORTH ATLANTIC");
	_regionTypes.push_back("SOUTH ATLANTIC");
	_regionTypes.push_back("INDIAN OCEAN");
	_cbxRegions->setOptions(_regionTypes, false);
	_cbxRegions->onChange((ActionHandler)&HostMenu::cbxRegionChange);

	_maxplayersTypes.push_back("MAX PLAYERS: 2");
	_cbxMaxPlayers->setOptions(_maxplayersTypes, false);
	_cbxMaxPlayers->onChange((ActionHandler)&HostMenu::cbxMaxPlayersChange);

	_password->setColor(color);
	_password->setBig();
	_password->setBorderColor(color);
	_password->setText("Password");
	_password->setVisible(false);

	// check if campaign mission
	if (!_game->getSavedGame()->getCountries()->empty())
	{
		_game->getCoopMod()->setCoopCampaign(true);
	}
	else
	{

		_game->getCoopMod()->setCoopCampaign(false);
	}

	// hotseat
	if (_game->getCoopMod()->_isHotseatActive == true)
	{

		// hide
		_tcpButtonHost->setVisible(false);
		_port->setVisible(false);
		_serverName->setVisible(false);
		_cbxVisibility->setVisible(false);
		_password->setVisible(false);

		// show
		_btnStartHotseat->setVisible(true);
		_btnStartHotseat->setText("DISABLE HOTSEAT");

	}
	else if ((_game->getCoopMod()->isConnected() == 1) || _game->getCoopMod()->getServerOwner() == true)
	{

		_port->setVisible(false);
		_serverName->setVisible(false);
		_tcpButtonHost->setVisible(false);
		_cbxVisibility->setVisible(false);
		_password->setVisible(false);

	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_port->setVisible(true);
		_serverName->setVisible(true);
		_tcpButtonHost->setVisible(true);
		_cbxVisibility->setVisible(true);
		_password->setVisible(true);

	}

	// READ IP ADDRESS

	// Name of the JSON file
	std::string filename = "ip_address.json";
	std::string filepath = Options::getMasterUserFolder() + filename;

	std::string ipAddress;
	std::string port;
	std::string serverName;

	if (OpenXcom::CrossPlatform::fileExists(filepath))
	{
		std::ifstream file(filepath, std::ifstream::binary);
		if (file.is_open())
		{
			Json::Value root;
			Json::CharReaderBuilder builder;
			std::string errs;

			bool parsingSuccessful = Json::parseFromStream(builder, file, &root, &errs);
			file.close();

			if (parsingSuccessful)
			{
				ipAddress = root.get("ip", "").asString();
				port = root.get("port", "").asString();
				serverName = root.get("server", "").asString();

				
				if (ipAddress.empty())
				{
					// ip is empty
					ipAddress = "IP-ADDRESS";
				}

				if (port == "")
				{
					// port is empty
					port = "3000";
				}


				if (serverName == "")
				{
					// name is empty
					serverName = "Server";
				}

				_port->setText(port);
				_serverName->setText(serverName); 
			}
			else
			{
				std::cerr << "Failed to parse JSON: " << errs << std::endl;
			}
		}
		else
		{
			std::cerr << "Failed to open the file." << std::endl;
		}
	}

}

/**
 *
 */
HostMenu::~HostMenu()
{
}

/**
 * Resets the menu music and savegame
 * when coming back from the battlescape.
 */
void HostMenu::init()
{

	// hotseat
	if (_game->getCoopMod()->_isHotseatActive == true)
	{

		// hide
		_tcpButtonHost->setVisible(false);
		_port->setVisible(false);
		_serverName->setVisible(false);
		_cbxVisibility->setVisible(false);
		_password->setVisible(false);

		// show
		_btnStartHotseat->setVisible(true);
		_btnStartHotseat->setText("DISABLE HOTSEAT");

	}
	else if ((_game->getCoopMod()->isConnected() == 1) || _game->getCoopMod()->getServerOwner() == true)
	{

		_port->setVisible(false);
		_serverName->setVisible(false);
		_tcpButtonHost->setVisible(false);
		_cbxVisibility->setVisible(false);
		_password->setVisible(false);

	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_port->setVisible(true);
		_serverName->setVisible(true);
		_tcpButtonHost->setVisible(true);
		_cbxVisibility->setVisible(true);
		_password->setVisible(true);

	}
}

void HostMenu::convertUnits()
{

	// Convert single-player save to multiplayer save (PvE)
	if (_game->getCoopMod()->getCoopGamemode() == 0 || _game->getCoopMod()->getCoopGamemode() == 1)
	{

		_game->getCoopMod()->setPlayerTurn(3);

		connectionTCP::_coopGamemode = 1;

		// Split the soldiers in half
		if (_game->getSavedGame()->getSavedBattle())
		{

			_game->getSavedGame()->getSavedBattle()->getBattleState()->setCurrentTurn(3);

			int soldier_total_count = 0;

			// check soldiers count
			for (auto entity : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				if (entity->getFaction() == FACTION_PLAYER)
				{
					soldier_total_count++;
				}
			}

			int soldier_used = (soldier_total_count / 2);

			// make coop soldiers
			for (auto unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				if (unit->getFaction() == FACTION_PLAYER)
				{

					unit->setCoop(1);

					if (soldier_used <= 0)
					{
						break;
					}

					soldier_used--;
				}
			}
		}
	}

	// if pve2 gamemode
	if (_game->getCoopMod()->getCoopGamemode() == 4)
	{
		// swapper
		for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_HOSTILE)
			{

				unit->convertToFaction(FACTION_PLAYER);
				unit->setOriginalFaction(FACTION_PLAYER);
				_game->getSavedGame()->getSavedBattle()->setSelectedUnit(unit);
				unit->setAIModule(0);
			}
			else if (unit->getFaction() == FACTION_PLAYER)
			{

				unit->convertToFaction(FACTION_HOSTILE);
				unit->setOriginalFaction(FACTION_HOSTILE);
			}
		}

		// Split the soldiers in half
		if (_game->getSavedGame()->getSavedBattle())
		{

			int soldier_total_count = 0;

			// check soldiers count
			for (auto& entity : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				if (entity->getFaction() == FACTION_PLAYER)
				{
					soldier_total_count++;
				}
			}

			int soldier_used = (soldier_total_count / 2);

			// make coop soldiers
			for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				if (unit->getFaction() == FACTION_PLAYER)
				{

					unit->setCoop(1);

					if (soldier_used <= 0)
					{
						break;
					}

					soldier_used--;
				}
			}
		}
	}
	// if pvp gamemode
	else if (_game->getCoopMod()->getCoopGamemode() == 2)
	{

		for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_HOSTILE)
			{
				unit->setCoop(1);
			}
			else if (unit->getFaction() == FACTION_PLAYER)
			{

				unit->setCoop(0);
			}
		}
	}
	// pvp2
	else if (_game->getCoopMod()->getCoopGamemode() == 3)
	{

		for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_HOSTILE)
			{

				unit->setCoop(0);
			}
			else if (unit->getFaction() == FACTION_PLAYER)
			{

				unit->setCoop(1);
			}
		}

	}

}

void HostMenu::btnChatClick(Action* action)
{

	if (_game->getCoopMod()->getChatMenu())
	{

		_game->getCoopMod()->getChatMenu()->setActive(!_game->getCoopMod()->getChatMenu()->isActive());

	}

}

void HostMenu::cbxVisibilityChange(Action* action)
{

	int selected_gamemode = _cbxVisibility->getSelected();

	// show
	_tcpButtonHost->setVisible(true);
	_port->setVisible(true);
	_serverName->setVisible(true);
	_password->setVisible(true);

	// hide
	_btnStartHotseat->setVisible(false);

	// HOTSEAT
	if (selected_gamemode == 2)
	{

		// hide
		_tcpButtonHost->setVisible(false);
		_port->setVisible(false);
		_serverName->setVisible(false);
		_password->setVisible(false);

		// show
		_btnStartHotseat->setVisible(true);
	}
	// UDP
	else if (selected_gamemode == 1)
	{
		_isUDPconnection = true;
	}
	// TCP
	else if (selected_gamemode == 0)
	{
		_isUDPconnection = false;
	}

}

void HostMenu::cbxMaxPlayersChange(Action* action)
{
}

void HostMenu::cbxRegionChange(Action*)
{

	int selected_region = _cbxRegions->getSelected();

	if (_cbxRegions->getSelected() && selected_region >= 0 && selected_region < _regionTypes.size())
	{
		selectedRegion = _regionTypes[selected_region];
	}
	else
	{
		selectedRegion = "NORTH AMERICA";
	}

}

void HostMenu::hostTCPGame(Action* action)
{

	// password
	if (_password->getText() != "" && _password->getText() != "Password")
	{

		connectionTCP::password = _password->getText();
		connectionTCP::isPasswordRequired = true;

	}

	connectionTCP::_coopGamemode = 1;

	_game->getCoopMod()->setCoopSession(false);

	_port->setVisible(false);
	_serverName->setVisible(false);
	_tcpButtonHost->setVisible(false);
	_cbxVisibility->setVisible(false);
	_password->setVisible(false);

	_game->getCoopMod()->setPlayerTurn(3);

	bool convert = true;

	if (_game->getSavedGame()->getSavedBattle())
	{

		// check if already converted units...
		for (auto unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getCoop() == 1)
			{

				convert = false;
				break;
			}
		}

	}
	else
	{

		convert = false;

	}

	if (_game->getCoopMod()->getCoopCampaign() == true && connectionTCP::_host_save_progress == true)
	{
		convert = false;
	}

	if (convert == true)
	{
		convertUnits();
	}

	// HOST GAME
	// TCP
	if (_isUDPconnection == false)
	{
		_game->getCoopMod()->hostTCPServer(_serverName->getText(), _port->getText());
	}
	// UDP
	else
	{

		std::string udpPassword = "";

		// password
		if (_password->getText() != "" && _password->getText() != "Password")
		{
			udpPassword = _password->getText();
		}

		_game->getCoopMod()->setHostServer(_serverName->getText());

		// init
		_game->getCoopMod()->_waitBC = false;
		_game->getCoopMod()->_waitBH = false;
		_game->getCoopMod()->_battleWindow = false;
		_game->getCoopMod()->_battleInit = false;
		_game->getCoopMod()->coopInventory = false;
		_game->getCoopMod()->coopMissionEnd = false;
		_game->getCoopMod()->inventory_battle_window = true;

		OpenXcom::hostListedViaRendezvousAsync(
			_game->getCoopMod()->getHostServer(),  // Public room name shown in the server list.
			udpPassword,						   // Room password. Empty string means public room without password.
			_game->getCoopMod()->getHostName(),	   // Host player name shown to the other player / lobby.
			selectedRegion,                        // Region selected by host, for example "EU", "US", "Asia".
			"",									   // Mod hash. Empty for now; later use this to check matching mod setup.
			true,								   // Listed room flag. true = visible in server list, false = private/unlisted.
			std::stoi(_port->getText())			   // Local UDP port. 0 = let the OS choose a free port automatically.
		);

	}

	_game->getCoopMod()->setServerOwner(true);

	// If the player has created a server or joined another player's game, close the ServerList and create the LobbyMenu
	if (Options::HostSaveProgress == true && _game->getCoopMod()->getCoopCampaign() == true)
	{
		_game->popState();

		_game->pushState(new LobbyMenu());
	}
	else
	{
		_game->popState();
	}

}

void HostMenu::startHotseat(Action* action)
{

	_game->getCoopMod()->_isHotseatActive = !_game->getCoopMod()->_isHotseatActive;

	// hide
	_port->setVisible(false);
	_tcpButtonHost->setVisible(false);
	_serverName->setVisible(false);
	_cbxVisibility->setVisible(false);
	_password->setVisible(false);

	// show
	_btnStartHotseat->setVisible(true);

	if (_game->getCoopMod()->_isHotseatActive)
	{
		_btnStartHotseat->setText("DISABLE HOTSEAT");
	}
	else
	{
		_btnStartHotseat->setText("ENABLE HOTSEAT");

		_cbxVisibility->setVisible(true);

	}

}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void HostMenu::btnCancelClick(Action*)
{
	_game->popState();
}

}

