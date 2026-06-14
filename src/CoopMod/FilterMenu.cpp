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

#include "FilterMenu.h"
#include "../Engine/Surface.h"

namespace OpenXcom
{

/**
 * Initializes all the elements in the New Battle window.
 * @param game Pointer to the core game.
 */
FilterMenu::FilterMenu() : _craft(0), _selectType(NewBattleSelectType::MISSION), _isRightClick(false)
{

	_screen = false;

	int x = 20;
	
	// Create objects
	_window = new Window(this, 216, 160, x, 20, POPUP_BOTH);

	_cbxNetworkProtocol = new ComboBox(this, 180, 18, x + 18, 50);
	_cbxPassword = new ComboBox(this, 180, 18, x + 18, 70);
	_cbxModCompatibility = new ComboBox(this, 180, 18, x + 18, 90);
	_cbxCampaign = new ComboBox(this, 90, 18, x + 108, 112);
	_cbxRegions = new ComboBox(this, 90, 18, x + 18, 112);
	_cbxManualServers = new ComboBox(this, 180, 18, x + 18, 132);

	_txtInfo = new Text(180, 18, x + 18, 95);
	_btnCancel = new TextButton(90, 18, x + 108, 152);
	_btnAdd = new TextButton(90, 18, x + 18, 152);
	_txtTitle = new Text(206, 17, x + 5, 32);

	int screenWidth = Options::baseXGeoscape;
	int screenHeight = Options::baseYGeoscape;

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_txtInfo, "text", "pauseMenu");
	add(_btnCancel, "button", "pauseMenu");
	add(_btnAdd, "button", "pauseMenu");
	add(_cbxManualServers, "button", "pauseMenu");
	add(_cbxRegions, "button", "pauseMenu");
	add(_cbxCampaign, "button", "pauseMenu");
	add(_cbxModCompatibility, "button", "pauseMenu");
	add(_cbxPassword, "button", "pauseMenu");
	add(_cbxNetworkProtocol, "button", "pauseMenu");
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
	_txtTitle->setText(tr("FILTER"));

	_txtInfo->setVisible(false);
	_txtInfo->setAlign(ALIGN_CENTER);
	_txtInfo->setSmall();

	_networkProtocolTypes.push_back("NETWORK: ANY");
	_networkProtocolTypes.push_back("NETWORK: TCP ONLY");
	_networkProtocolTypes.push_back("NETWORK: UDP ONLY");
	_cbxNetworkProtocol->setOptions(_networkProtocolTypes, false);
	_cbxNetworkProtocol->onChange((ActionHandler)&FilterMenu::cbxNetworkProtocolChange);

	_passwordTypes.push_back("With or without password");
	_passwordTypes.push_back("With password");
	_passwordTypes.push_back("Without password");
	_cbxPassword->setOptions(_passwordTypes, false);
	_cbxPassword->onChange((ActionHandler)&FilterMenu::cbxPasswordChange);

	_modCompatibilityTypes.push_back("All mods");
	_modCompatibilityTypes.push_back("Compatible mods only");
	_modCompatibilityTypes.push_back("Incompatible mods only");
	_cbxModCompatibility->setOptions(_modCompatibilityTypes, false);
	_cbxModCompatibility->onChange((ActionHandler)&FilterMenu::cbxModCompatibilityChange);

	_regionTypes.push_back("ANY REGION");
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
	_cbxRegions->onChange((ActionHandler)&FilterMenu::cbxRegionChange);

	_campaignTypes.push_back("Any game mode");
	_campaignTypes.push_back("Campaign");
	_campaignTypes.push_back("Custom battle");
	_cbxCampaign->setOptions(_campaignTypes, false);
	_cbxCampaign->onChange((ActionHandler)&FilterMenu::cbxRCampaignChange);

	_manualServerTypes.push_back("ANY SERVER");
	_manualServerTypes.push_back("MANUALLY ADDED ONLY");
	_manualServerTypes.push_back("NOT MANUALLY ADDED");
	_cbxManualServers->setOptions(_manualServerTypes, false);
	_cbxManualServers->onChange((ActionHandler)&FilterMenu::cbxManualServerChange);
	
	_btnCancel->setText("CANCEL");
	_btnCancel->onMouseClick((ActionHandler)&FilterMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&FilterMenu::btnCancelClick, Options::keyCancel);

	_btnAdd->setText("OK");
	_btnAdd->onMouseClick((ActionHandler)&FilterMenu::btnOKClick);
	_btnAdd->onKeyboardPress((ActionHandler)&FilterMenu::btnOKClick);

}

/**
 *
 */
FilterMenu::~FilterMenu()
{
}

/**
 * Resets the menu music and savegame
 * when coming back from the battlescape.
 */
void FilterMenu::init()
{

}

bool FilterMenu::parseUdpPort(const std::string& text, uint16_t& outPort)
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

void FilterMenu::cbxNetworkProtocolChange(Action* action)
{

	int selected_NetworkProtocol = _cbxNetworkProtocol->getSelected();

	if (_cbxNetworkProtocol->getSelected() && selected_NetworkProtocol >= 0 && selected_NetworkProtocol < _networkProtocolTypes.size())
	{
		selectedNetworkProtocol = _networkProtocolTypes[selected_NetworkProtocol];
	}
	else
	{
		selectedNetworkProtocol = "NETWORK: ANY";
	}

}

void FilterMenu::cbxPasswordChange(Action* action)
{

	int selected_password = _cbxPassword->getSelected();

	if (_cbxPassword->getSelected() && selected_password >= 0 && selected_password < _passwordTypes.size())
	{
		selectedPassword = _passwordTypes[selected_password];
	}
	else
	{
		selectedPassword = "With or without password";
	}

}

void FilterMenu::cbxModCompatibilityChange(Action* action)
{

	int selected_ModCompatibility = _cbxModCompatibility->getSelected();

	if (_cbxModCompatibility->getSelected() && selected_ModCompatibility >= 0 && selected_ModCompatibility < _modCompatibilityTypes.size())
	{
		selectedModCompatibility = _modCompatibilityTypes[selected_ModCompatibility];
	}
	else
	{
		selectedModCompatibility = "All mods";
	}

}

void FilterMenu::cbxRegionChange(Action*)
{

	int selected_region = _cbxRegions->getSelected();

	if (_cbxRegions->getSelected() && selected_region >= 0 && selected_region < _regionTypes.size())
	{
		selectedRegion = _regionTypes[selected_region];
	}
	else
	{
		selectedRegion = "ANY REGION";
	}

}

void FilterMenu::cbxRCampaignChange(Action* action)
{

	int selected_campaign = _cbxCampaign->getSelected();

	if (_cbxCampaign->getSelected() && selected_campaign >= 0 && selected_campaign < _campaignTypes.size())
	{
		selectedCampaign = _campaignTypes[selected_campaign];
	}
	else
	{
		selectedCampaign = "Any game mode";
	}
}

void FilterMenu::cbxManualServerChange(Action* action)
{

	int selected_ManualServers = _cbxManualServers->getSelected();

	if (_cbxManualServers->getSelected() && selected_ManualServers >= 0 && selected_ManualServers < _manualServerTypes.size())
	{
		selectedManualServers = _manualServerTypes[selected_ManualServers];
	}
	else
	{
		selectedManualServers = "ANY SERVER";
	}

}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void FilterMenu::btnCancelClick(Action*)
{
	_game->popState();
}

void FilterMenu::btnOKClick(Action* action)
{

	// Define the file path
	std::string filename = Options::getMasterUserFolder() + "/filters.json";

	// Root JSON object
	Json::Value root(Json::objectValue);

	// Set values directly to root
	root["NetworkProtocol"] = selectedNetworkProtocol;
	root["Region"] = selectedRegion;
	root["Campaign"] = selectedCampaign;
	root["Password"] = selectedPassword;
	root["ModCompatibility"] = selectedModCompatibility;
	root["ManualServers"] = selectedManualServers;

	// Write JSON to file, replacing everything
	std::ofstream outputFile(filename, std::ios::out | std::ios::trunc);

	if (outputFile.is_open())
	{
		Json::StreamWriterBuilder writer;
		writer["indentation"] = "\t";

		outputFile << Json::writeString(writer, root);
		outputFile.close();

		std::cout << "Filters saved to " << filename << std::endl;
	}
	else
	{
		std::cerr << "Failed to open filters.json for writing." << std::endl;
	}
	
	_game->popState();

}

}

