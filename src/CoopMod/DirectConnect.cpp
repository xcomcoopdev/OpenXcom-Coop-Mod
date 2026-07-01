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
#include "../Engine/Surface.h"
#include "connectionUDP/connection_rendezvous_glue.h"
#include "PasswordCheckMenu.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the New Battle window.
 * @param game Pointer to the core game.
 */
DirectConnect::DirectConnect() : _craft(0), _selectType(NewBattleSelectType::MISSION), _isRightClick(false)
{

	_screen = false;

	int x = 20;
	
	// Create objects
	_window = new Window(this, 260, 160, x, 20, POPUP_BOTH);

	_cbxNetworkProtocol = new ComboBox(this, 224, 18, x + 18, 50);

	_lblHostIp = new Text(100, 18, x + 18, 72);
	_lblPort = new Text(100, 18, x + 18, 92);

	_ipAddress = new TextEdit(this, 124, 18, x + 118, 72);
	_port = new TextEdit(this, 124, 18, x + 118, 92);

	_tcpButtonJoin = new TextButton(224, 18, x + 18, 112);

	_txtInfo = new Text(224, 18, x + 18, 95);
	_btnCancel = new TextButton(224, 18, x + 18, 132);
	_txtData = new Text(250, 17, x + 5, 50);
	_txtTitle = new Text(250, 17, x + 5, 32);

	int screenWidth = Options::baseXGeoscape;
	int screenHeight = Options::baseYGeoscape;

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_lblHostIp, "text", "pauseMenu");
	add(_lblPort, "text", "pauseMenu");
	add(_ipAddress, "text", "pauseMenu");
	add(_port, "text", "pauseMenu");
	add(_tcpButtonJoin, "button", "pauseMenu");
	add(_cbxNetworkProtocol, "button", "pauseMenu");
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

	_networkProtocolTypes.push_back("NETWORK: TCP");
	_networkProtocolTypes.push_back("NETWORK: UDP");
	_cbxNetworkProtocol->setOptions(_networkProtocolTypes, false);
	_cbxNetworkProtocol->onChange((ActionHandler)&DirectConnect::cbxNetworkProtocolChange);

	// labels
	_lblHostIp->setBig();
	_lblHostIp->setBorderColor(color);
	_lblHostIp->setText("HOST IP>");
	_lblHostIp->setVisible(false);

	_lblPort->setBig();
	_lblPort->setBorderColor(color);
	_lblPort->setText("PORT>");
	_lblPort->setVisible(false);

	// ip address
	_ipAddress->setColor(color);
	_ipAddress->setBig();
	_ipAddress->setBorderColor(color);
	_ipAddress->setAllowOverflow(true);
	_ipAddress->setText("127.0.0.1");
	_ipAddress->setVisible(false);

	// port
	_port->setColor(color);
	_port->setBig();
	_port->setBorderColor(color);
	_port->setConstraint(TEC_NUMERIC_POSITIVE);
	_port->setAllowOverflow(true);
	_port->setText("61008");
	_port->setVisible(false);
	
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
		_lblHostIp->setVisible(false);
		_ipAddress->setVisible(false);
		_lblPort->setVisible(false);
		_port->setVisible(false);
		_tcpButtonJoin->setVisible(false);
		_txtInfo->setVisible(false);
	
	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_lblHostIp->setVisible(true);
		_ipAddress->setVisible(true);
		_lblPort->setVisible(true);
		_port->setVisible(true);
		_tcpButtonJoin->setVisible(true);
		_txtInfo->setVisible(false);

	}

	// READ CLIENT ADDRESS

	std::string ipAddress;
	std::string port;

	{
		std::string filepath = Options::getMasterUserFolder() + "client_address.json";

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
				}
			}
		}
	}

	if (ipAddress.empty())
		ipAddress = "127.0.0.1";
	if (port.empty())
		port = "61008";

	_ipAddress->setText(ipAddress);
	_port->setText(port);

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
		_lblHostIp->setVisible(false);
		_ipAddress->setVisible(false);
		_lblPort->setVisible(false);
		_port->setVisible(false);
		_tcpButtonJoin->setVisible(false);
		_txtInfo->setVisible(false);

		_game->popState();

	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_lblHostIp->setVisible(true);
		_ipAddress->setVisible(true);
		_lblPort->setVisible(true);
		_port->setVisible(true);
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

bool DirectConnect::parseUdpPort(const std::string& text, uint16_t& outPort)
{
	if (text.empty())
		return false;

	char* end = nullptr;
	long value = std::strtol(text.c_str(), &end, 10);

	if (*end != '\0')
		return false;

	if (value < 1 || value > 65535)
		return false;

	outPort = static_cast<uint16_t>(value);
	return true;
}

void DirectConnect::cbxNetworkProtocolChange(Action* action)
{

	int selected_gamemode = _cbxNetworkProtocol->getSelected();

	// TCP
	if (selected_gamemode == 0)
	{
		isUDP = false;
	}
	// UDP
	else if (selected_gamemode == 1)
	{
		isUDP = true;
	}

}

void DirectConnect::joinTCPGame(Action* action)
{

	// Save client settings to JSON
	{
		std::string filepath = Options::getMasterUserFolder() + "client_address.json";
		Json::Value root;
		root["ip"] = _ipAddress->getText();
		root["port"] = _port->getText();

		std::ofstream file(filepath);
		if (file.is_open())
		{
			Json::StreamWriterBuilder writer;
			writer["indentation"] = "\t";
			file << Json::writeString(writer, root);
			file.close();
		}
	}

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

	// TCP
	if (isUDP == false)
	{
		_game->getCoopMod()->connectTCPServer(_ipAddress->getText(), _port->getText());
	}
	// UDP
	else
	{

		uint16_t hostUdpPort = 0;

		_game->getCoopMod()->_waitBC = false;
		_game->getCoopMod()->_waitBH = false;
		_game->getCoopMod()->_battleWindow = false;
		_game->getCoopMod()->_battleInit = false;
		_game->getCoopMod()->coopInventory = false;
		_game->getCoopMod()->coopMissionEnd = false;
		_game->getCoopMod()->inventory_battle_window = true;

		// Ensure the coop state menu does not close immediately.
		connectionTCP::forceCloseCoopStateMenu = false;

		// Ensure the password check menu does not close immediately.
		connectionTCP::forceClosePasswordCheckMenu = false;

		if (!parseUdpPort(_port->getText(), hostUdpPort))
		{
			DebugLog(("Direct LAN: invalid host UDP port: " + _port->getText() + "\n").c_str());

			return;
		}
		
		OpenXcom::joinLanRoomByAddressViaRendezvousAsync(
			_ipAddress->getText(),					// Host LAN IP from Direct Connect menu.
			"",										// Must match host password. Empty is allowed.
			_game->getCoopMod()->getHostName(),		// Client player name.
			hostUdpPort,							// client local UDP port, 0 = automatic
			[this, hostUdpPort](bool ok)
			{

				if (ok)
				{
					// Direct LAN connection started.
					return;
				}

				connectionTCP::forceCloseCoopStateMenu = true;

				// Direct LAN failed. Most likely wrong password, host not found,
				// firewall, or wrong port.
				// If this room/server requires a password, open passwordCheck menu.
				_game->pushState(new PasswordCheckMenu(_ipAddress->getText(), _game->getCoopMod()->getHostName(), hostUdpPort, true, true));
			}
		);
		
	}

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

