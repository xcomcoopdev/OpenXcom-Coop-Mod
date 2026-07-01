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
	_window = new Window(this, 260, 160, x, 20, POPUP_BOTH);
	_lstSaves = new TextList(224, 18, x + 18, 60);

	_lblServerName = new Text(108, 18, x + 18, 72);
	_lblPort = new Text(108, 18, x + 18, 92);
	_lblPassword = new Text(108, 18, x + 18, 132);

	_port = new TextEdit(this, 116, 18, x + 126, 92);
	_serverName = new TextEdit(this, 116, 18, x + 126, 72);
	_tcpButtonHost = new TextButton(112, 18, x + 18, 152);
	_btnStartHotseat = new TextButton(224, 18, x + 18, 112);
	_btnReactionFire = new TextButton(224, 18, x + 18, 132);
	_cbxVisibility = new ComboBox(this, 224, 18, x + 18, 50);
	_cbxRegions = new ComboBox(this, 112, 18, x + 18, 112); 
	_cbxMaxPlayers = new ComboBox(this, 112, 18, x + 130, 112); 
	_password = new TextEdit(this, 116, 18, x + 126, 132);
	_btnCancel = new TextButton(112, 18, x + 130, 152);
	_txtTitle = new Text(250, 17, x + 5, 32);

	int screenWidth = Options::baseXGeoscape;
	int screenHeight = Options::baseYGeoscape;


	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);


	add(_window, "window", "pauseMenu");
	add(_lblServerName, "text", "pauseMenu");
	add(_lblPort, "text", "pauseMenu");
	add(_lblPassword, "text", "pauseMenu");
	add(_port, "text", "pauseMenu");
	add(_serverName, "text", "pauseMenu");
	add(_password, "text", "pauseMenu");
	add(_tcpButtonHost, "button", "pauseMenu");
	add(_btnStartHotseat, "button", "pauseMenu");
	add(_btnReactionFire, "button", "pauseMenu");
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

	// labels
	_lblServerName->setBig();
	_lblServerName->setBorderColor(color);
	_lblServerName->setText("GAME NAME>");
	_lblServerName->setVisible(false);

	_lblPort->setBig();
	_lblPort->setBorderColor(color);
	_lblPort->setText("PORT>");
	_lblPort->setVisible(false);

	_lblPassword->setBig();
	_lblPassword->setBorderColor(color);
	_lblPassword->setText("PASSWORD>");
	_lblPassword->setVisible(false);

	// port
	_port->setColor(color);
	_port->setBig();
	_port->setBorderColor(color);
	_port->setConstraint(TEC_NUMERIC_POSITIVE);
	_port->setAllowOverflow(true);
	_port->setText("61008");
	_port->setVisible(false);

	_serverName->setColor(color);
	_serverName->setBig();
	_serverName->setBorderColor(color);
	_serverName->setAllowOverflow(true);
	_serverName->setText("Vigilo Confido");
	_serverName->setVisible(false);
	
	_tcpButtonHost->setText("START HOST");
	_tcpButtonHost->onMouseClick((ActionHandler)&HostMenu::hostTCPGame);
	_tcpButtonHost->setVisible(true);

	_btnStartHotseat->setText("ENABLE HOTSEAT");
	_btnStartHotseat->onMouseClick((ActionHandler)&HostMenu::startHotseat);
	_btnStartHotseat->setVisible(false);

	if (connectionTCP::_isHotseatReactionFireEnabled == true)
	{
		_btnReactionFire->setText("DISABLE REACTION FIRE");
	}
	else
	{
		_btnReactionFire->setText("ENABLE REACTION FIRE");
	}

	_btnReactionFire->onMouseClick((ActionHandler)&HostMenu::btnReactionFireClick);
	_btnReactionFire->setVisible(false);

	_btnCancel->setText(tr("CANCEL"));
	_btnCancel->onMouseClick((ActionHandler)&HostMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&HostMenu::btnCancelClick, Options::keyCancel);

	_visibilityTypes.push_back("VISIBILITY: PRIVATE (TCP)");
	_visibilityTypes.push_back("VISIBILITY: PRIVATE (UDP)");
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
	_password->setAllowOverflow(true);
	_password->setBorderColor(color);
	_password->setText("");
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
		_lblPort->setVisible(false);
		_serverName->setVisible(false);
		_lblServerName->setVisible(false);
		_cbxVisibility->setVisible(false);
		_password->setVisible(false);
		_lblPassword->setVisible(false);
		_cbxMaxPlayers->setVisible(false);
		_cbxRegions->setVisible(false);
		_btnReactionFire->setVisible(false);

		// show
		_btnStartHotseat->setVisible(true);
		_btnStartHotseat->setText("DISABLE HOTSEAT");

	}
	else if ((_game->getCoopMod()->isConnected() == 1) || _game->getCoopMod()->getServerOwner() == true)
	{

		_port->setVisible(false);
		_lblPort->setVisible(false);
		_serverName->setVisible(false);
		_lblServerName->setVisible(false);
		_tcpButtonHost->setVisible(false);
		_cbxVisibility->setVisible(false);
		_password->setVisible(false);
		_lblPassword->setVisible(false);
		_cbxMaxPlayers->setVisible(false);
		_cbxRegions->setVisible(false);

	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_port->setVisible(true);
		_lblPort->setVisible(true);
		_serverName->setVisible(true);
		_lblServerName->setVisible(true);
		_tcpButtonHost->setVisible(true);
		_cbxVisibility->setVisible(true);
		_password->setVisible(true);
		_lblPassword->setVisible(true);
		_cbxMaxPlayers->setVisible(true);
		_cbxRegions->setVisible(true);

	}

	// READ HOST ADDRESS

	// Name of the JSON file
	std::string filename = "host_address.json";
	std::string filepath = Options::getMasterUserFolder() + filename;

	std::string port;
	std::string serverName;
	std::string password;

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
				port = root.get("port", "").asString();
				serverName = root.get("server", "").asString();
				password = root.get("password", "").asString();

				
				if (port == "")
				{
					// port is empty
					port = "3000";
				}


				if (serverName == "")
				{
					// name is empty
					serverName = "Vigilo Confido";
				}

				_port->setText(port);
				_serverName->setText(serverName);
				if (password != "")
				{
					_password->setText(password);
				}
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
		_lblPort->setVisible(false);
		_serverName->setVisible(false);
		_lblServerName->setVisible(false);
		_cbxVisibility->setVisible(false);
		_password->setVisible(false);
		_lblPassword->setVisible(false);
		_cbxMaxPlayers->setVisible(false);
		_cbxRegions->setVisible(false);

		if (connectionTCP::_isHotseatReactionFireEnabled == true)
		{
			_btnReactionFire->setText("DISABLE REACTION FIRE");
		}
		else
		{
			_btnReactionFire->setText("ENABLE REACTION FIRE");
		}

		// show
		_btnStartHotseat->setVisible(true);
		_btnReactionFire->setVisible(false);
		_btnStartHotseat->setText("DISABLE HOTSEAT");

	}
	else if ((_game->getCoopMod()->isConnected() == 1) || _game->getCoopMod()->getServerOwner() == true)
	{

		_port->setVisible(false);
		_lblPort->setVisible(false);
		_serverName->setVisible(false);
		_lblServerName->setVisible(false);
		_tcpButtonHost->setVisible(false);
		_cbxVisibility->setVisible(false);
		_password->setVisible(false);
		_lblPassword->setVisible(false);
		_cbxMaxPlayers->setVisible(false);
		_cbxRegions->setVisible(false);

	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_port->setVisible(true);
		_lblPort->setVisible(true);
		_serverName->setVisible(true);
		_lblServerName->setVisible(true);
		_tcpButtonHost->setVisible(true);
		_cbxVisibility->setVisible(true);
		_password->setVisible(true);
		_lblPassword->setVisible(true);
		_cbxMaxPlayers->setVisible(true);
		_cbxRegions->setVisible(true);

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
	_lblPort->setVisible(true);
	_serverName->setVisible(true);
	_lblServerName->setVisible(true);
	_password->setVisible(true);
	_lblPassword->setVisible(true);
	_cbxMaxPlayers->setVisible(true);
	_cbxRegions->setVisible(true);

	// hide
	_btnStartHotseat->setVisible(false);
	_btnReactionFire->setVisible(false);

	// HOTSEAT
	if (selected_gamemode == 3)
	{

		// hide
		_tcpButtonHost->setVisible(false);
		_port->setVisible(false);
		_lblPort->setVisible(false);
		_serverName->setVisible(false);
		_lblServerName->setVisible(false);
		_password->setVisible(false);
		_lblPassword->setVisible(false);
		_cbxMaxPlayers->setVisible(false);
		_cbxRegions->setVisible(false);

		// show
		_btnStartHotseat->setVisible(true);
		_btnReactionFire->setVisible(true);
	}
	// UDP (private)
	else if (selected_gamemode == 1)
	{
		_isUDPconnection = true;
		isListed = false;
	}
	// UDP (public)
	else if (selected_gamemode == 2)
	{
		_isUDPconnection = true;
		isListed = true;
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

	// Save host settings to JSON
	{
		std::string filepath = Options::getMasterUserFolder() + "host_address.json";
		Json::Value root;
		root["server"] = _serverName->getText();
		root["port"] = _port->getText();
		root["password"] = _password->getText();

		std::ofstream file(filepath);
		if (file.is_open())
		{
			Json::StreamWriterBuilder writer;
			writer["indentation"] = "\t";
			file << Json::writeString(writer, root);
			file.close();
		}
	}

	connectionTCP::password = "";
	connectionTCP::isPasswordRequired = false;

	// password
	if (_password->getText() != "")
	{

		connectionTCP::password = _password->getText();
		connectionTCP::isPasswordRequired = true;

	}

	connectionTCP::_coopGamemode = 1;

	_game->getCoopMod()->setCoopSession(false);

	_port->setVisible(false);
	_lblPort->setVisible(false);
	_serverName->setVisible(false);
	_lblServerName->setVisible(false);
	_tcpButtonHost->setVisible(false);
	_cbxVisibility->setVisible(false);
	_password->setVisible(false);
	_lblPassword->setVisible(false);
	_cbxMaxPlayers->setVisible(false);
	_cbxRegions->setVisible(false);

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
		if (_password->getText() != "")
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

		// mod compatibility
		std::vector<std::string> mod_names = _game->getMod()->getCoopModList();
		std::string str_hash;

		for (const auto& mod_name : mod_names)
		{
			str_hash += mod_name + ";";
		}

		OpenXcom::hostListedViaRendezvousAsync(
			_game->getCoopMod()->getHostServer(),  // Public room name shown in the server list.
			udpPassword,						   // Room password. Empty string means public room without password.
			_game->getCoopMod()->getHostName(),	   // Host player name shown to the other player / lobby.
			selectedRegion,                        // Region selected by host, for example "EU", "US", "Asia".
			str_hash,                              // Mod hash. Use this to check matching mod setup.
			isListed,							   // Listed room flag. true = visible in server list, false = private/unlisted.
			_game->getCoopMod()->getCoopCampaign(),// Is it Campaign or Custom Battle.
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
	_lblPort->setVisible(false);
	_tcpButtonHost->setVisible(false);
	_serverName->setVisible(false);
	_lblServerName->setVisible(false);
	_cbxVisibility->setVisible(false);
	_password->setVisible(false);
	_lblPassword->setVisible(false);
	_cbxMaxPlayers->setVisible(false);
	_cbxRegions->setVisible(false);

	// show
	_btnStartHotseat->setVisible(true);

	// fix
	_cbxVisibility->setSelected(2);

	if (_game->getCoopMod()->_isHotseatActive)
	{
		_btnStartHotseat->setText("DISABLE HOTSEAT");
		_btnReactionFire->setVisible(false);
	}
	else
	{
		_btnStartHotseat->setText("ENABLE HOTSEAT");

		_cbxVisibility->setVisible(true);
		_btnReactionFire->setVisible(true);

	}

}

void HostMenu::btnReactionFireClick(Action* action)
{

	connectionTCP::_isHotseatReactionFireEnabled = !connectionTCP::_isHotseatReactionFireEnabled;

	if (connectionTCP::_isHotseatReactionFireEnabled == true)
	{
		_btnReactionFire->setText("DISABLE REACTION FIRE");
	}
	else
	{
		_btnReactionFire->setText("ENABLE REACTION FIRE");
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

