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

#include "DirectConnect.h"
#include "../Menu/SaveGameState.h"
#include "../Menu/LoadGameState.h"
#include "CoopState.h"
#include "../Mod/ExtraSprites.h"
#include "../Engine/Surface.h"
#include "Profile.h"
#include "ChatMenu.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the New Battle window.
 * @param game Pointer to the core game.
 */
DirectConnect::DirectConnect() : _craft(0), _selectType(NewBattleSelectType::MISSION), _isRightClick(false)
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

	_ipAddress = new TextEdit(this, 180, 18, x + 18, 72);
	_port = new TextEdit(this, 180, 18, x + 18, 92);
	_playerName = new TextEdit(this, 180, 18, x + 18, 112);

	_tcpButtonJoin = new TextButton(180, 18, x + 18, 132);

	_txtInfo = new Text(180, 18, x + 18, 95);
	_btnCancel = new TextButton(180, 18, x + 18, 152);
	_txtData = new Text(206, 17, x + 5, 50);
	_txtTitle = new Text(206, 17, x + 5, 32);

	int screenWidth = Options::baseXGeoscape;
	int screenHeight = Options::baseYGeoscape;

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_ipAddress);
	add(_port);
	add(_playerName);
	add(_tcpButtonJoin, "button", "pauseMenu");
	add(_txtInfo, "text", "pauseMenu");
	add(_btnCancel, "button", "pauseMenu");
	add(_txtData, "text", "pauseMenu");
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
	_txtTitle->setText(tr("DIRECT CONNECT"));

	_txtData->setAlign(ALIGN_CENTER);
	_txtData->setBig();
	_txtData->setText("HELLO");
	_txtData->setVisible(false);


	_txtInfo->setVisible(false);
	_txtInfo->setAlign(ALIGN_CENTER);
	_txtInfo->setSmall();

	// ip address
	_ipAddress->setColor(color);
	_ipAddress->setBig();
	_ipAddress->setBorderColor(color);
	_ipAddress->setText("IP-ADDRESS");
	_ipAddress->setVisible(false);

	// port
	_port->setColor(color);
	_port->setBig();
	_port->setBorderColor(color);
	_port->setText("PORT");
	_port->setVisible(false);

	_playerName->setColor(color);
	_playerName->setBig();
	_playerName->setBorderColor(color);
	_playerName->setText("Player");
	_playerName->setVisible(false);
	
	_tcpButtonJoin->setText("JOIN");
	_tcpButtonJoin->onMouseClick((ActionHandler)&DirectConnect::joinTCPGame);
	_tcpButtonJoin->onKeyboardPress((ActionHandler)&DirectConnect::joinTCPGame, Options::keyOk);
	_tcpButtonJoin->setVisible(true);

	_btnCancel->setText("CANCEL");
	_btnCancel->onMouseClick((ActionHandler)&DirectConnect::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&DirectConnect::btnCancelClick, Options::keyCancel);

	// check if campaign mission
	if (!_game->getSavedGame()->getCountries()->empty())
	{
		_game->getCoopMod()->setCoopCampaign(true);
	}
	else
	{

		_game->getCoopMod()->setCoopCampaign(false);
	}

	if (_game->getCoopMod()->isConnected() == 1)
	{
		_ipAddress->setVisible(false);
		_port->setVisible(false);
		_playerName->setVisible(false);
		_tcpButtonJoin->setVisible(false);
		_txtInfo->setVisible(false);
	
	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_ipAddress->setVisible(true);
		_port->setVisible(true);
		_playerName->setVisible(true);
		_tcpButtonJoin->setVisible(true);
		_txtInfo->setVisible(false);

	}

	// READ IP ADDRESS

	// Name of the JSON file
	std::string filename = "ip_address.json";
	std::string filepath = Options::getMasterUserFolder() + filename;

	std::string ipAddress;
	std::string port;
	std::string playerName;

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
				playerName = root.get("name", "").asString();

				
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


				if (playerName == "")
				{
					// name is empty
					playerName = "Player";
				}

				_ipAddress->setText(ipAddress);
				_port->setText(port);
				_playerName->setText(playerName); 
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
DirectConnect::~DirectConnect()
{
}

/**
 * Resets the menu music and savegame
 * when coming back from the battlescape.
 */
void DirectConnect::init()
{

	if (_game->getCoopMod()->isConnected() == 1)
	{
		_ipAddress->setVisible(false);
		_port->setVisible(false);
		_playerName->setVisible(false);
		_tcpButtonJoin->setVisible(false);
		_txtInfo->setVisible(false);

		_game->popState();

	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_ipAddress->setVisible(true);
		_port->setVisible(true);
		_playerName->setVisible(true);
		_tcpButtonJoin->setVisible(true);
		_txtInfo->setVisible(false);

	}

}

void DirectConnect::convertUnits()
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

void DirectConnect::btnChatClick(Action* action)
{

	if (_game->getCoopMod()->getChatMenu())
	{

		_game->getCoopMod()->getChatMenu()->setActive(!_game->getCoopMod()->getChatMenu()->isActive());

	}

}

void DirectConnect::joinTCPGame(Action* action)
{

	// JOIN GAME
	_game->getCoopMod()->setCoopSession(false);

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

	if (convert == true)
	{
		convertUnits();
	}

	_game->pushState(new CoopState(15));

	_game->getCoopMod()->connectTCPServer(_playerName->getText(), _ipAddress->getText(), _port->getText());
}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void DirectConnect::btnCancelClick(Action*)
{
	_game->popState();

}

}

