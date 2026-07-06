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

#include "AddServerMenu.h"
#include "../Engine/Surface.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the New Battle window.
 * @param game Pointer to the core game.
 */
AddServerMenu::AddServerMenu() : _craft(0), _selectType(NewBattleSelectType::MISSION), _isRightClick(false)
{

	_screen = false;

	int x = 20;
	
	// Create objects
	_window = new Window(this, 260, 160, x, 20, POPUP_BOTH);

	_cbxNetworkProtocol = new ComboBox(this, 224, 18, x + 18, 50);

	_ipAddress = new TextEdit(this, 124, 18, x + 118, 72);
	_port = new TextEdit(this, 124, 18, x + 118, 92);

	_cbxCampaign = new ComboBox(this, 112, 18, x + 130, 112);
	_cbxRegions = new ComboBox(this, 112, 18, x + 18, 112);
	_serverName = new TextEdit(this, 116, 18, x + 126, 132);

	_txtInfo = new Text(180, 18, x + 18, 95);
	_btnCancel = new TextButton(112, 18, x + 130, 152);
	_btnAdd = new TextButton(112, 18, x + 18, 152);
	_txtTitle = new Text(250, 17, x + 5, 32);

	// labels
	_lblServerName = new Text(108, 18, x + 18, 132);
	_lblHostIp = new Text(100, 18, x + 18, 72);
	_lblPort = new Text(100, 18, x + 18, 92);

	int screenWidth = Options::baseXGeoscape;
	int screenHeight = Options::baseYGeoscape;

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_lblServerName, "text", "pauseMenu");
	add(_lblHostIp, "text", "pauseMenu");
	add(_lblPort, "text", "pauseMenu");
	add(_ipAddress);
	add(_port);
	add(_serverName);
	add(_txtInfo, "text", "pauseMenu");
	add(_btnCancel, "button", "pauseMenu");
	add(_btnAdd, "button", "pauseMenu");
	add(_cbxNetworkProtocol, "button", "pauseMenu");
	add(_cbxRegions, "button", "pauseMenu");
	add(_cbxCampaign, "button", "pauseMenu");
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
	_txtTitle->setText(tr("ADD SERVER"));

	_txtInfo->setVisible(false);
	_txtInfo->setAlign(ALIGN_CENTER);
	_txtInfo->setSmall();

	_networkProtocolTypes.push_back("NETWORK: TCP");
	_networkProtocolTypes.push_back("NETWORK: UDP");
	_cbxNetworkProtocol->setOptions(_networkProtocolTypes, false);
	_cbxNetworkProtocol->onChange((ActionHandler)&AddServerMenu::cbxNetworkProtocolChange);

	// ip address
	_ipAddress->setColor(color);
	_ipAddress->setBig();
	_ipAddress->setBorderColor(color);
	_ipAddress->setText("IP-ADDRESS");
	_ipAddress->setVisible(true);

	// port
	_port->setColor(color);
	_port->setBig();
	_port->setBorderColor(color);
	_port->setText("PORT");
	_port->setVisible(true);

	_serverName->setColor(color);
	_serverName->setBig();
	_serverName->setBorderColor(color);
	_serverName->setText("Server");
	_serverName->setVisible(true);

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
	_cbxRegions->onChange((ActionHandler)&AddServerMenu::cbxRegionChange);

	_campaignTypes.push_back("Campaign");
	_campaignTypes.push_back("Custom Battle");
	_cbxCampaign->setOptions(_campaignTypes, false);
	_cbxCampaign->onChange((ActionHandler)&AddServerMenu::cbxRCampaignChange);
	
	_btnCancel->setText("CANCEL");
	_btnCancel->onMouseClick((ActionHandler)&AddServerMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&AddServerMenu::btnCancelClick, Options::keyCancel);

	_btnAdd->setText("ADD");
	_btnAdd->onMouseClick((ActionHandler)&AddServerMenu::btnCAddServerClick);
	_btnAdd->onKeyboardPress((ActionHandler)&AddServerMenu::btnCAddServerClick);

	// labels
	_lblServerName->setBig();
	_lblServerName->setBorderColor(color);
	_lblServerName->setText("GAME NAME>");
	_lblServerName->setVisible(true);

	_lblHostIp->setBig();
	_lblHostIp->setBorderColor(color);
	_lblHostIp->setText("HOST IP>");
	_lblHostIp->setVisible(true);

	_lblPort->setBig();
	_lblPort->setBorderColor(color);
	_lblPort->setText("PORT>");
	_lblPort->setVisible(true);

}

/**
 *
 */
AddServerMenu::~AddServerMenu()
{
}

/**
 * Resets the menu music and savegame
 * when coming back from the battlescape.
 */
void AddServerMenu::init()
{

}

bool AddServerMenu::parseUdpPort(const std::string& text, uint16_t& outPort)
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

void AddServerMenu::cbxNetworkProtocolChange(Action* action)
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

void AddServerMenu::cbxRegionChange(Action*)
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

void AddServerMenu::cbxRCampaignChange(Action* action)
{

	int selected_region = _cbxCampaign->getSelected();

	if (selected_region == 0)
	{
		isCampaign = true;
	}
	else
	{
		isCampaign = false;
	}

}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void AddServerMenu::btnCancelClick(Action*)
{
	_game->popState();
}

void AddServerMenu::btnCAddServerClick(Action* action)
{

	// Define the file path
	std::string filename = Options::getMasterUserFolder() + "/servers.json";

	// Root JSON object
	Json::Value root;

	// Try to read existing JSON file
	{
		std::ifstream inputFile(filename);

		if (inputFile.is_open())
		{
			Json::CharReaderBuilder reader;
			JSONCPP_STRING errors;

			if (!Json::parseFromStream(reader, inputFile, &root, &errors))
			{
				std::cerr << "Failed to parse existing servers.json: " << errors << std::endl;
				root = Json::Value(Json::objectValue);
			}

			inputFile.close();
		}
	}

	// Ensure "servers" is an array
	if (!root.isMember("servers") || !root["servers"].isArray())
	{
		root["servers"] = Json::Value(Json::arrayValue);
	}

	// Create new server object
	Json::Value server;
	server["server"] = _serverName->getText();
	server["ipAddress"] = _ipAddress->getText();
	server["port"] = _port->getText();
	server["region"] = selectedRegion;
	server["campaign"] = isCampaign;
	server["isUDP"] = isUDP;

	// Add it to servers array
	root["servers"].append(server);

	// Write updated JSON back to file
	std::ofstream outputFile(filename);

	if (outputFile.is_open())
	{
		Json::StreamWriterBuilder writer;
		writer["indentation"] = "\t";

		outputFile << Json::writeString(writer, root);
		outputFile.close();

		std::cout << "Server added to " << filename << std::endl;
	}
	else
	{
		std::cerr << "Failed to open servers.json for writing." << std::endl;
	}

	_game->popState();

}

}

