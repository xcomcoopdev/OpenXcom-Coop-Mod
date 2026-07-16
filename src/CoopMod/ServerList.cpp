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
#include <algorithm>
#include <functional>
#include <fstream>
#include "ServerList.h"
#include "../Engine/Logger.h"
#include "../Savegame/SavedGame.h"
#include "../Engine/Game.h"
#include "../Engine/Action.h"
#include "../Engine/Exception.h"
#include "../Engine/Options.h"
#include "../Engine/Unicode.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../Engine/LocalizedText.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Interface/TextList.h"
#include "../Interface/ToggleTextButton.h"
#include "../Interface/ArrowButton.h"
#include "../Interface/DisableableComboBox.h"
#include "../Engine/CrossPlatform.h"
#include <json/json.h>

#include  "HostMenu.h"
#include "DirectConnect.h"
#include "connectionUDP/connection_rendezvous_glue.h"
#include "connectionUDP/rendezvous_config.h"
#include "PasswordCheckMenu.h"
#include "ModCheckMenu.h"
#include "AddServerMenu.h"
#include "FilterMenu.h"

namespace OpenXcom
{

std::mutex _serverListMutex;
std::vector<OpenXcom::RendezvousClient::RoomInfo> _pendingRooms;
bool _pendingRoomsOk = false;
bool _hasPendingRooms = false;
bool _isRefreshingServers = false;

// Health-probe results, filled by worker threads and drained on the UI thread.
std::mutex _probeMutex;
std::vector<OpenXcom::RendezvousProbeResult> _pendingProbes;
bool _hasPendingProbes = false;

struct compareServerName
{
	bool _reverse;

	compareServerName(bool reverse) : _reverse(reverse) {}

	bool operator()(const ServerInfo& a, const ServerInfo& b) const
	{
		if (a.reserved == b.reserved)
		{
			return Unicode::naturalCompare(a.name, b.name);
		}
		else
		{
			return _reverse ? b.reserved : a.reserved;
		}
	}
};

struct compareServerPlayers
{
	bool _reverse;

	compareServerPlayers(bool reverse) : _reverse(reverse) {}

	bool operator()(const ServerInfo& a, const ServerInfo& b) const
	{
		if (a.reserved == b.reserved)
		{
			return a.currentPlayers < b.currentPlayers;
		}
		else
		{
			return _reverse ? b.reserved : a.reserved;
		}
	}
};

struct compareServerRegion
{
	bool _reverse;

	compareServerRegion(bool reverse) : _reverse(reverse) {}

	bool operator()(const ServerInfo& a, const ServerInfo& b) const
	{
		if (a.reserved == b.reserved)
		{
			return Unicode::naturalCompare(a.region, b.region);
		}
		else
		{
			return _reverse ? b.reserved : a.reserved;
		}
	}
};

struct compareServerPassword
{
	bool _reverse;

	compareServerPassword(bool reverse) : _reverse(reverse) {}

	bool operator()(const ServerInfo& a, const ServerInfo& b) const
	{
		if (a.reserved == b.reserved)
		{
			return Unicode::naturalCompare(a.passwordRequired, b.passwordRequired);
		}
		else
		{
			return _reverse ? b.reserved : a.reserved;
		}
	}
};

/**
 * Initializes all the elements in the Saved Game screen.
 * @param game Pointer to the core game.
 * @param origin Game section that originated this state.
 * @param firstValidRow First row containing saves.
 * @param autoquick Show auto/quick saved games?
 */
ServerList::ServerList() : _sortable(true)
{
	_screen = false;

	bool isMobile = false;
#ifdef __MOBILE__
	isMobile = true;
#endif

	// Create objects
	_window = new Window(this, 320, 200, 0, 0, POPUP_BOTH);

	int y = 179; // 172
	int h = 16;

	int wHost = 50;
	int wDirect = 84;
	int wAdd = 72;
	int wRefresh = 55;
	int wCancel = 44;

	int x = 8;

	_playername = new TextEdit(this, 140, h, x + 65, y - 34);
	_lblPlayerName = new Text(65, h, x, y - 34);
	_search = new TextEdit(this, 100, h, x + 55, y - 18);
	_btnFilter = new TextButton(wHost, h, x, y - 18);
	_btnHost = new TextButton(wHost, h, x, y);
	x += wHost;
	_btnDirectConnect = new TextButton(wDirect, h, x, y);
	x += wDirect;
	_btnAddServer = new TextButton(wAdd, h, x, y);
	x += wAdd;
	_btnRefresh = new TextButton(wRefresh, h, x, y);
	x += wRefresh;
	_btnCancel = new TextButton(wCancel, h, x, y);

	//_btnCancel = new TextButton(80, 16, 120, 172);

	_txtTitle = new Text(310, 17, 8, 7);
	_txtJoin = new Text(310, 9, 5, 23);
	_txtName = new Text(115, 9, 16, isMobile ? 40 : 32);
	_txtPlayers = new Text(50, 9, 126, isMobile ? 40 : 32); 
	_txtRegion = new Text(50, 9, 238, isMobile ? 40 : 32);
	_txtPasswordRequired = new Text(50, 9, 178, isMobile ? 40 : 32); 
	_lstServers = new TextList(298, isMobile ? 96 : 104, 8, isMobile ? 50 : 42);

	_sortName = new ArrowButton(ARROW_NONE, 11, 8, 16, isMobile ? 40 : 32);
	_sortPlayers = new ArrowButton(ARROW_NONE, 11, 8, 126, isMobile ? 40 : 32);
	_sortRegion = new ArrowButton(ARROW_NONE, 11, 8, 238, isMobile ? 40 : 32);
	_sortPassword = new ArrowButton(ARROW_NONE, 11, 8, 178, isMobile ? 40 : 32);

	// Set palette
	setInterface("geoscape", true, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "saveMenus");
	add(_lblPlayerName, "text", "saveMenus");
	add(_playername);
	add(_search);
	add(_btnFilter, "button", "saveMenus");
	add(_btnHost, "button", "saveMenus");
	add(_btnDirectConnect, "button", "saveMenus");
	add(_btnAddServer, "button", "saveMenus");
	add(_btnRefresh, "button", "saveMenus");
	add(_btnCancel, "button", "saveMenus");
	add(_txtTitle, "text", "saveMenus");
	add(_txtJoin, "text", "saveMenus");
	add(_txtName, "text", "saveMenus");
	add(_txtPlayers, "text", "saveMenus");
	add(_txtPasswordRequired, "text", "saveMenus");
	add(_txtRegion, "text", "saveMenus");
	add(_lstServers, "list", "saveMenus");
	add(_sortName, "text", "saveMenus");
	add(_sortPlayers, "text", "saveMenus");
	add(_sortRegion, "text", "saveMenus");
	add(_sortPassword, "text", "saveMenus");

	// Set up objects
	setWindowBackground(_window, "saveMenus");

	Uint8 color = 239; // 239 or 255

	if (_game->getSavedGame())
	{
		if (_game->getSavedGame()->getSavedBattle())
		{
			color = 255;
		}
	}

	_search->setColor(color);
	_search->setBorderColor(color);
	_search->setText("Search servers...");
	_search->setVisible(true);
	_search->onMouseClick((ActionHandler)&ServerList::btnSearchClick);
	_search->onChange((ActionHandler)&ServerList::edtSearchChange);

	_lblPlayerName->setColor(color);
	_lblPlayerName->setBorderColor(color);
	_lblPlayerName->setText("PLAYER NAME>");
	_lblPlayerName->setVisible(true);

	_playername->setColor(500);
	_playername->setBorderColor(500);

	// Read saved player name
	{
		std::string filepath = Options::getMasterUserFolder() + "player_name.json";
		std::string savedName;

		if (OpenXcom::CrossPlatform::fileExists(filepath))
		{
			std::ifstream file(filepath, std::ifstream::binary);
			if (file.is_open())
			{
				Json::Value root;
				Json::CharReaderBuilder builder;
				std::string errs;

				if (Json::parseFromStream(builder, file, &root, &errs))
				{
					savedName = root.get("name", "").asString();
				}
			}
		}

		if (savedName.empty())
			savedName = "Jane Kelly";

		_game->getCoopMod()->setHostName(savedName);
		_playername->setText(savedName);
	}
	_playername->setVisible(true);
	_playername->onChange((ActionHandler)&ServerList::edtPlayerNameChange);

	_btnFilter->setText("Filter");
	_btnFilter->onMouseClick((ActionHandler)&ServerList::btnFilterClick);

	_btnHost->setText("Host");
	_btnHost->onMouseClick((ActionHandler)&ServerList::btnHostClick);

	_btnDirectConnect->setText("Direct Connect");
	_btnDirectConnect->onMouseClick((ActionHandler)&ServerList::btnDirectConnectClick);

	_btnAddServer->setText("Add Server");
	_btnAddServer->onMouseClick((ActionHandler)&ServerList::btnAddServerClick);

	_btnRefresh->setText("Refresh");
	_btnRefresh->onMouseClick((ActionHandler)&ServerList::btnRefreshClick);

	_btnCancel->setText(tr("STR_CANCEL"));
	_btnCancel->onMouseClick((ActionHandler)&ServerList::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&ServerList::btnCancelClick, Options::keyCancel);

	_txtTitle->setBig();
	// Left-aligned so the header clears the rendezvous-server combobox that
	// sits in the top-right corner.
	_txtTitle->setAlign(ALIGN_LEFT);
	_txtTitle->setText("SERVER BROWSER");

	if (isMobile)
	{
		_txtJoin->setText("Left click to join.");
	}
	else
	{
		_txtJoin->setAlign(ALIGN_CENTER);
		_txtJoin->setText("Left click to join.");
	}

	_txtName->setText("Server");
	_txtPlayers->setText("Players");
	_txtRegion->setText("Region");
	_txtPasswordRequired->setText("Password");

	_lstServers->setColumns(4, 110, 53, 57, 77); 
	_lstServers->setSelectable(true);
	_lstServers->setBackground(_window);
	_lstServers->setMargin(8);
	_lstServers->onMouseOver((ActionHandler)&ServerList::lstServerMouseOver);
	_lstServers->onMouseOut((ActionHandler)&ServerList::lstServerMouseOut);
	_lstServers->onMousePress((ActionHandler)&ServerList::lstServerPress);

	_sortName->setX(_sortName->getX() + _txtName->getTextWidth() + 5);
	_sortName->onMouseClick((ActionHandler)&ServerList::sortNameClick);

	_sortPlayers->setX(_sortPlayers->getX() + _txtPlayers->getTextWidth() + 5);
	_sortPlayers->onMouseClick((ActionHandler)&ServerList::sortPlayersClick);

	_sortRegion->setX(_sortRegion->getX() + _txtRegion->getTextWidth() + 5);
	_sortRegion->onMouseClick((ActionHandler)&ServerList::sortRegionClick);

	_sortPassword->setX(_sortPassword->getX() + _txtPasswordRequired->getTextWidth() + 5);
	_sortPassword->onMouseClick((ActionHandler)&ServerList::sortPasswordClick);

	updateArrows();

	// check if campaign (no loaded game = not a campaign; guard the deref so
	// a save-less path into the browser cannot crash here)
	if (_game->getSavedGame() && !_game->getSavedGame()->getCountries()->empty())
	{
		_game->getCoopMod()->setCoopCampaign(true);
	}
	else
	{
		_game->getCoopMod()->setCoopCampaign(false);
	}

	// --- Rendezvous-server selector -------------------------------------------
	{
		std::vector<std::string> names = getRendezvousServerNames();

		// Restore last-selected server (falls back to the first configured one).
		setActiveRendezvousServerByName(loadSavedServerName());

		_serverStatus.assign(names.size(), 0); // 0 = waiting for probe
		_probesStarted = false;

		// Offline warning, hidden until the active server is known unreachable.
		_txtOfflineWarning = new Text(298, 25, 8, 60);
		add(_txtOfflineWarning, "text", "saveMenus");
		_txtOfflineWarning->setColor(color);
		_txtOfflineWarning->setAlign(ALIGN_CENTER);
		_txtOfflineWarning->setWordWrap(true);
		_txtOfflineWarning->setVisible(false);

		// Palette-correct greyed color for disabled (offline) rows, matching the
		// coop mod's OptionsMultiplayer "disabledUserOption" convention.
		bool inBattle = _game->getSavedGame() && _game->getSavedGame()->getSavedBattle();
		Uint8 disabledColor = color;
		if (RuleInterface* rule = _game->getMod()->getInterface(inBattle ? "battlescape" : "advancedMenu"))
		{
			if (const Element* el = rule->getElement("disabledUserOption"))
				disabledColor = el->color;
		}
		_serverComboColor = color;
		_serverComboDisabledColor = disabledColor;

		// Added LAST so its dropdown draws above every other widget.
		_cbxServer = new DisableableComboBox(this, 104, 16, 210, 6);
		add(_cbxServer, "button", "saveMenus");
		_cbxServer->setColor(color);
		_cbxServer->setDisabledColor(disabledColor);
		rebuildServerCombo();
		_cbxServer->onChange((ActionHandler)&ServerList::cbxServerChange);
		_cbxServer->setVisible(names.size() > 1);
	}

	updateServerList();

	// Probe all configured servers so offline ones can be flagged in the combo.
	startServerProbes();

}

/**
 *
 */
ServerList::~ServerList()
{

}

/**
 * Refreshes the saves list.
 */
void ServerList::init()
{
	State::init();

	if (connectionTCP::canRemoveManuallyAddedServer == true)
	{

		connectionTCP::canRemoveManuallyAddedServer = false;

		removeManuallyAddedServerFromFile();

	}

	updateServerList();

	// If the player has created a server or joined another player's game, close the ServerList and create the LobbyMenu
	if (_game->getCoopMod()->isConnected() == 1 || _game->getCoopMod()->getServerOwner() == true)
	{

		_game->popState();

		_game->pushState(new LobbyMenu());

	}
	
	_origin = OPT_MENU;

	if (_game->getSavedGame())
	{
		if (_game->getSavedGame()->getSavedBattle())
		{
			_origin = OPT_BATTLESCAPE;
		}
	}

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("saveMenus");
	}

}

/**
 * Updates the sorting arrows based
 * on the current setting.
 */
void ServerList::updateArrows()
{
	_sortName->setShape(ARROW_NONE);
	_sortPlayers->setShape(ARROW_NONE);
	_sortRegion->setShape(ARROW_NONE);
	switch (Options::serverOrder)
	{
	case SORT_SERVER_ASC:
		_sortName->setShape(ARROW_SMALL_UP);
		break;
	case SORT_SERVER_DESC:
		_sortName->setShape(ARROW_SMALL_DOWN);
		break;
	case SORT_PLAYERS_ASC:
		_sortPlayers->setShape(ARROW_SMALL_UP);
		break;
	case SORT_PLAYERS_DESC:
		_sortPlayers->setShape(ARROW_SMALL_DOWN);
		break;
	case SORT_REGION_ASC:
		_sortRegion->setShape(ARROW_SMALL_UP);
		break;
	case SORT_REGION_DESC:
		_sortRegion->setShape(ARROW_SMALL_DOWN);
		break;
	}

}

void ServerList::updateServerList()
{

	loadFilters();

	loadServersFromJson();

	_lstServers->clearList();
	sortList(Options::serverOrder);

	if (_isRefreshingServers)
		return;

	_isRefreshingServers = true;

	OpenXcom::refreshServerListViaRendezvousAsync(
		[this](bool ok, std::vector<OpenXcom::RendezvousClient::RoomInfo> rooms)
		{
			std::lock_guard<std::mutex> lock(_serverListMutex);

			_pendingRoomsOk = ok;
			_pendingRooms = std::move(rooms);
			_hasPendingRooms = true;
		});

}

bool ServerList::parseUdpPort(const std::string& text, uint16_t& outPort)
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

void ServerList::loadServersFromJson()
{

	_servers.erase(
		std::remove_if(_servers.begin(), _servers.end(),
					   [](const ServerInfo& server)
					   {
						   return server.added;
					   }),
		_servers.end());

	std::string filename = Options::getMasterUserFolder() + "/servers.json";

	std::ifstream inputFile(filename);

	if (!inputFile.is_open())
	{
		std::cerr << "servers.json not found: " << filename << std::endl;
		return;
	}

	Json::Value root;
	Json::CharReaderBuilder reader;
	JSONCPP_STRING errors;

	if (!Json::parseFromStream(reader, inputFile, &root, &errors))
	{
		std::cerr << "Failed to parse servers.json: " << errors << std::endl;
		return;
	}

	inputFile.close();

	// Check that "servers" exists and is an array
	if (!root.isMember("servers") || !root["servers"].isArray())
	{
		std::cerr << "servers.json does not contain a valid servers array." << std::endl;
		return;
	}

	// Loop all servers
	for (const Json::Value& server : root["servers"])
	{
		std::string servername = server.get("server", "").asString();
		std::string ipAddress = server.get("ipAddress", "").asString();
		std::string port = server.get("port", "").asString();
		std::string region = server.get("region", "").asString();
		bool isCampaign = server.get("campaign", false).asBool();
		bool isUDP = server.get("isUDP", false).asBool();

		if (!isAllowedBySearch(servername))
		{
			continue;
		}

		if (!isAllowedByFilters(region, false, isUDP, "", true, isCampaign))
		{
			continue;
		}

		uint16_t hostUdpPort = 0;

		if (!parseUdpPort(port, hostUdpPort))
		{
			DebugLog(("Direct LAN: invalid host UDP port: " + port + "\n").c_str());
		}

		_servers.push_back(ServerInfo({"0",
						servername,
						_game->getCoopMod()->getHostName(),
						0,
						0,
						region,
						false,
						isUDP,
						"Unknown",
						false,
						ipAddress,
						hostUdpPort,
						"",
						true,
						isCampaign }));

	}
}

void ServerList::loadFilters()
{

	std::string filename = Options::getMasterUserFolder() + "/filters.json";

	std::ifstream inputFile(filename);

	if (!inputFile.is_open())
	{
		std::cout << "filters.json not found, using default filters." << std::endl;
		return;
	}

	Json::Value root;
	Json::CharReaderBuilder reader;
	JSONCPP_STRING errors;

	if (!Json::parseFromStream(reader, inputFile, &root, &errors))
	{
		std::cerr << "Failed to parse filters.json: " << errors << std::endl;
		return;
	}

	selectedRegion = root.get("Region", "ANY REGION").asString();
	selectedPassword = root.get("Password", "With or without password").asString();
	selectedNetworkProtocol = root.get("NetworkProtocol", "NETWORK: ANY").asString();
	selectedModCompatibility = root.get("ModCompatibility", "All mods").asString();
	selectedManualServers = root.get("ManualServers", "ANY SERVER").asString();
	selectedCampaign = root.get("Campaign", "Any game mode").asString();

}

bool ServerList::removeManuallyAddedServerFromFile()
{
	std::string filename = Options::getMasterUserFolder() + "/servers.json";

	int removeID = connectionTCP::manuallyAddedServerRemoveID;

	if (removeID < 0 || removeID >= static_cast<int>(_servers.size()))
	{
		std::cerr << "Invalid manually added server remove ID." << std::endl;
		return false;
	}

	// Server data to remove
	bool targetCampaign = _servers[removeID].isCampaign;
	std::string targetName = _servers[removeID].name;
	std::string targetRegion = _servers[removeID].region;
	bool targetIsUDP = _servers[removeID].isUDP;
	std::string targetHost = _servers[removeID].lanHost;

	Json::Value root;

	// Read servers.json
	{
		std::ifstream inputFile(filename);

		if (!inputFile.is_open())
		{
			std::cerr << "Failed to open servers.json for reading." << std::endl;
			return false;
		}

		Json::CharReaderBuilder reader;
		JSONCPP_STRING errors;

		if (!Json::parseFromStream(reader, inputFile, &root, &errors))
		{
			std::cerr << "Failed to parse servers.json: " << errors << std::endl;
			return false;
		}
	}

	if (!root.isMember("servers") || !root["servers"].isArray())
	{
		std::cerr << "servers.json does not contain a valid servers array." << std::endl;
		return false;
	}

	// Find matching server from JSON
	for (Json::ArrayIndex i = 0; i < root["servers"].size(); ++i)
	{
		Json::Value server = root["servers"][i];

		bool matches =
			server.get("campaign", false).asBool() == targetCampaign &&
			server.get("server", "").asString() == targetName &&
			server.get("region", "").asString() == targetRegion &&
			server.get("isUDP", false).asBool() == targetIsUDP &&
			server.get("ipAddress", "").asString() == targetHost;

		if (matches)
		{
			Json::Value removedServer;
			root["servers"].removeIndex(i, &removedServer);

			// Write updated JSON back to file
			std::ofstream outputFile(filename, std::ios::out | std::ios::trunc);

			if (!outputFile.is_open())
			{
				std::cerr << "Failed to open servers.json for writing." << std::endl;
				return false;
			}

			Json::StreamWriterBuilder writer;
			writer["indentation"] = "\t";

			outputFile << Json::writeString(writer, root);

			std::cout << "Manually added server removed from servers.json." << std::endl;
			return true;
		}
	}

	std::cerr << "Matching manually added server was not found in servers.json." << std::endl;
	return false;
}

void ServerList::savePlayerNameToIpAddressFile(std::string playerName)
{
	std::string filepath = Options::getMasterUserFolder() + "player_name.json";

	Json::Value root;
	root["name"] = playerName;

	std::ofstream file(filepath);
	if (file.is_open())
	{
		Json::StreamWriterBuilder writer;
		writer["indentation"] = "\t";
		file << Json::writeString(writer, root);
		file.close();
	}
}

bool ServerList::isAllowedBySearch(std::string serverName)
{
	// Get search text from the search input
	std::string searchText = _search->getText();

	// Empty search or placeholder text allows all servers
	if (searchText.empty() || searchText == "Search servers...")
	{
		return true;
	}

	// Convert both strings to lowercase for case-insensitive search
	std::transform(serverName.begin(), serverName.end(), serverName.begin(),
				   [](unsigned char c)
				   { return std::tolower(c); });

	std::transform(searchText.begin(), searchText.end(), searchText.begin(),
				   [](unsigned char c)
				   { return std::tolower(c); });

	// Allow server if the server name contains the search text
	return serverName.find(searchText) != std::string::npos;
}

bool ServerList::isAllowedByFilters(std::string region, bool passwordRequired, bool isUDP, std::string modHash, bool added, bool isCampaign)
{
	if (selectedRegion != "ANY REGION" && region != selectedRegion)
	{
		return false;
	}

	// Password filter
	if (selectedPassword == "With password" && !passwordRequired)
	{
		return false;
	}

	if (selectedPassword == "Without password" && passwordRequired)
	{
		return false;
	}

	// Network protocol filter
	if (selectedNetworkProtocol == "NETWORK: TCP ONLY" && isUDP)
	{
		return false;
	}

	if (selectedNetworkProtocol == "NETWORK: UDP ONLY" && !isUDP)
	{
		return false;
	}

	// Mod compatibility filter
	bool compatibleMods = _game->getCoopMod()->hasRequiredMods(modHash);

	if (selectedModCompatibility == "Compatible mods only" && !compatibleMods)
	{
		return false;
	}

	if (selectedModCompatibility == "Incompatible mods only" && compatibleMods)
	{
		return false;
	}

	// Manually added servers filter
	if (selectedManualServers == "MANUALLY ADDED ONLY" && !added)
	{
		return false;
	}

	if (selectedManualServers == "NOT MANUALLY ADDED" && added)
	{
		return false;
	}

	// Campaign / Custom battle filter
	if (selectedCampaign == "Campaign" && !isCampaign)
	{
		return false;
	}

	if (selectedCampaign == "Custom battle" && isCampaign)
	{
		return false;
	}

	// Passed all filters
	return true;
}

/**
 * Sorts the save game list.
 * @param sort Order to sort the games in.
 */
void ServerList::sortList(serverSort sort)
{
	switch (sort)
	{
	case SORT_SERVER_ASC:
		std::sort(_servers.begin(), _servers.end(), compareServerName(false));
		break;
	case SORT_SERVER_DESC:
		std::sort(_servers.rbegin(), _servers.rend(), compareServerName(true));
		break;
	case SORT_PLAYERS_ASC:
		std::sort(_servers.begin(), _servers.end(), compareServerPlayers(false));
		break;
	case SORT_PLAYERS_DESC:
		std::sort(_servers.rbegin(), _servers.rend(), compareServerPlayers(true));
		break;
	case SORT_REGION_ASC:
		std::sort(_servers.begin(), _servers.end(), compareServerRegion(false));
		break;
	case SORT_REGION_DESC:
		std::sort(_servers.rbegin(), _servers.rend(), compareServerRegion(true));
		break;
	case SORT_PASSWORD_ASC:
		std::sort(_servers.begin(), _servers.end(), compareServerPassword(false));
		break;
	case SORT_PASSWORD_DESC:
		std::sort(_servers.rbegin(), _servers.rend(), compareServerPassword(true));
		break;
	}
	updateList();
}

/**
 * Updates the save game list with the current list
 * of available savegames.
 */
void ServerList::updateList()
{
	int row = 0;
	int color = _lstServers->getSecondaryColor();
	for (const auto& serverInfo : _servers)
	{

		std::string server = serverInfo.name; 
		std::string players = std::to_string(serverInfo.currentPlayers) + "/" + std::to_string(serverInfo.maxPlayers);
		std::string region = serverInfo.region;
		std::string password = serverInfo.passwordRequired;
		_lstServers->addRow(4, server.c_str(), players.c_str(), password.c_str(), region.c_str());
		if (serverInfo.reserved && _origin != OPT_BATTLESCAPE)
		{
			_lstServers->setRowColor(row, color);
		}
		row++;
	}
}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void ServerList::btnCancelClick(Action*)
{
	_game->popState();
}

void ServerList::btnFilterClick(Action* action)
{
	_game->pushState(new FilterMenu);
}

void ServerList::btnSearchClick(Action* action)
{
	_search->setText("");
}

void ServerList::btnRefreshClick(Action* action)
{
	updateServerList();
}

void ServerList::btnHostClick(Action* action)
{
	_game->pushState(new HostMenu);
}

void ServerList::btnDirectConnectClick(Action* action)
{
	_game->pushState(new DirectConnect);
}

void ServerList::btnAddServerClick(Action* action)
{
	_game->pushState(new AddServerMenu);
}

/**
 * Shows the details of the currently hovered server.
 * @param action Pointer to an action.
 */
void ServerList::lstServerMouseOver(Action*)
{
}

/**
 * Clears the details.
 * @param action Pointer to an action.
 */
void ServerList::lstServerMouseOut(Action*)
{
}

/**
 * Deletes the selected save.
 * @param action Pointer to an action.
 */
void ServerList::lstServerPress(Action* action)
{
	if (action->getDetails()->button.button == SDL_BUTTON_LEFT)
	{

		int sel = _lstServers->getSelectedRow() - _firstValidRow;
		std::string wstr;
		if (sel >= 0 && sel < (int)_servers.size())
		{

			std::string udpPassword = "";

			if (!_servers[sel].added && !_game->getCoopMod()->hasRequiredMods(_servers[sel].modHash))
			{

				_game->pushState(new ModCheckMenu(_servers[sel].modHash));

				return;
			}

			if (_servers[sel].isUDP == true && _servers[sel].added == false)
			{

				if (_servers[sel].passwordRequired == "YES")
				{

					_game->pushState(new PasswordCheckMenu(&_servers[sel], _game->getCoopMod()->getHostName(), true, false));

				}
				else
				{

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

					_game->pushState(new CoopState(15));

					const bool useLanEndpoint =
						_servers[sel].isLanDiscovery &&
						!_servers[sel].lanHost.empty() &&
						_servers[sel].lanPort != 0;

					if (useLanEndpoint)
					{

						OpenXcom::joinLanRoomViaRendezvousAsync(
							_servers[sel].id,                   // Rendezvous room id.
							_servers[sel].lanHost,              // LAN IP found by UDP discovery, e.g. 192.168.1.50.
							_servers[sel].lanPort,              // Host LAN UDP port, usually 3001 if 3000 is discovery.
							"",                                 // Empty if no password.
							_game->getCoopMod()->getHostName(), // Local player name.
							0                                   // Client local UDP port. 0 = automatic.
						);
					}
					else
					{

						OpenXcom::joinListedViaRendezvousAsync(
							_servers[sel].id,                   // Room ID from the selected server-list entry. This identifies which room to join.
							"",                                 // Room password. Empty string means no password; must match host password if one is set.
							_game->getCoopMod()->getHostName(), // Local player name sent to the rendezvous server / shown to the host or lobby.
							0                                   // Local UDP port. 0 = let the OS choose a free UDP port automatically.
						);
					}

				}

			}
			else if (_servers[sel].added == true)
			{

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

				_game->pushState(new CoopState(15));

				if (_servers[sel].isUDP == true)
				{

					uint16_t port = _servers[sel].lanPort;
					std::string ipAddress = _servers[sel].lanHost;

					OpenXcom::joinLanRoomByAddressViaRendezvousAsync(
						ipAddress,                          // Host LAN IP from Direct Connect menu.
						"",                                 // Must match host password. Empty is allowed.
						_game->getCoopMod()->getHostName(), // Client player name.
						port,                               // client local UDP port, 0 = automatic
						[this, port, ipAddress](bool ok)
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
							_game->pushState(new PasswordCheckMenu(ipAddress, _game->getCoopMod()->getHostName(), port, true, true));
						});

				}
				else
				{

					_game->getCoopMod()->connectTCPServer(_servers[sel].lanHost, std::to_string(_servers[sel].lanPort));

				}

			}

		}
	}
	else if (action->getDetails()->button.button == SDL_BUTTON_RIGHT)
	{

		int sel = _lstServers->getSelectedRow() - _firstValidRow;

		if (sel >= 0 && sel < (int)_servers.size())
		{

			if (_servers[sel].added == true)
			{
				connectionTCP::manuallyAddedServerRemoveID = sel;

				_game->pushState(new CoopState(1234));
			}

		}

	}

}

/**
 * Sorts the saves by name.
 * @param action Pointer to an action.
 */
void ServerList::sortNameClick(Action*)
{
	if (_sortable)
	{
		if (Options::serverOrder == SORT_SERVER_ASC)
		{
			Options::serverOrder = SORT_SERVER_DESC;
		}
		else
		{
			Options::serverOrder = SORT_SERVER_ASC;
		}
		updateArrows();
		_lstServers->clearList();
		sortList(Options::serverOrder);
	}
}

/**
 * Sorts the saves by date.
 * @param action Pointer to an action.
 */
void ServerList::sortPlayersClick(Action*)
{
	if (_sortable)
	{
		if (Options::serverOrder == SORT_PLAYERS_ASC)
		{
			Options::serverOrder = SORT_PLAYERS_DESC;
		}
		else
		{
			Options::serverOrder = SORT_PLAYERS_ASC;
		}
		updateArrows();
		_lstServers->clearList();
		sortList(Options::serverOrder);
	}
}

void ServerList::sortRegionClick(Action* action)
{
	if (_sortable)
	{
		if (Options::serverOrder == SORT_REGION_ASC)
		{
			Options::serverOrder = SORT_REGION_DESC;
		}
		else
		{
			Options::serverOrder = SORT_REGION_ASC;
		}
		updateArrows();
		_lstServers->clearList();
		sortList(Options::serverOrder);
	}
}

void ServerList::sortPasswordClick(Action* action)
{
	if (_sortable)
	{
		if (Options::serverOrder == SORT_PASSWORD_ASC)
		{
			Options::serverOrder = SORT_PASSWORD_DESC;
		}
		else
		{
			Options::serverOrder = SORT_PASSWORD_ASC;
		}
		updateArrows();
		_lstServers->clearList();
		sortList(Options::serverOrder);
	}
}

void ServerList::edtSearchChange(Action* action)
{

	updateServerList();

}

void ServerList::edtPlayerNameChange(Action* action)
{
	_game->getCoopMod()->setHostName(_playername->getText());

	savePlayerNameToIpAddressFile(_game->getCoopMod()->getHostName());

}

void ServerList::disableSort()
{
	_sortable = false;
}

void ServerList::think()
{
	State::think();

	// Apply completed health probes on the main thread.
	{
		std::vector<OpenXcom::RendezvousProbeResult> probes;
		{
			std::lock_guard<std::mutex> lock(_probeMutex);
			if (_hasPendingProbes)
			{
				probes = std::move(_pendingProbes);
				_pendingProbes.clear();
				_hasPendingProbes = false;
			}
		}

		if (!probes.empty())
		{
			for (const auto& p : probes)
			{
				if (p.index < _serverStatus.size())
					_serverStatus[p.index] = p.online ? 1 : 2;
			}
			rebuildServerCombo();
			updateOfflineWarning();
		}
	}

	// Animate "Fetching server list . . ." while the selected server is probing.
	updateFetchingAnimation();

	// Handle completed async server-list refresh on the main thread.
	bool hasPending = false;
	bool ok = false;
	std::vector<OpenXcom::RendezvousClient::RoomInfo> rooms;

	{
		std::lock_guard<std::mutex> lock(_serverListMutex);

		if (_hasPendingRooms)
		{
			hasPending = true;
			ok = _pendingRoomsOk;
			rooms = std::move(_pendingRooms);
			_hasPendingRooms = false;
		}
	}

	if (hasPending)
	{
		_isRefreshingServers = false;

		if (ok)
		{

			_servers.erase(
				std::remove_if(_servers.begin(), _servers.end(),
							   [](const ServerInfo& server)
							   {
								   return !server.added;
							   }),
				_servers.end());

			for (const auto& room : rooms)
			{

				if (!isAllowedBySearch(room.name))
				{
					continue;
				}

				if (!isAllowedByFilters(room.region, room.passwordRequired, true, room.modHash, false, room.isCampaign))
				{
					continue;
				}

				std::string password = room.passwordRequired ? "YES" : "NO";
				
				_servers.push_back(ServerInfo({room.roomId,
											   room.name,
											   room.hostName,
											   room.maxPlayers,
											   room.players,
											   room.region,
											   false,
											   true,
											   password,
											   room.isLan,
											   room.lanHost,
											   room.lanPort,
											   room.modHash,
											   false,
											   room.isCampaign }));
			}

			_lstServers->clearList();
			sortList(Options::serverOrder);

		}

	}

	// Start a new refresh every 30 seconds.
	static Uint32 lastUpdate = 0;
	Uint32 now = SDL_GetTicks();

	if (now - lastUpdate >= 30000)
	{
		lastUpdate = now;
		updateServerList();
	}
}

/**
 * Kicks off parallel health probes for every configured rendezvous server.
 * Results are collected on worker threads and applied in think().
 */
void ServerList::startServerProbes()
{
	if (_probesStarted)
		return;
	_probesStarted = true;

	OpenXcom::probeAllRendezvousServersAsync(2500,
		[](OpenXcom::RendezvousProbeResult result)
		{
			std::lock_guard<std::mutex> lock(_probeMutex);
			_pendingProbes.push_back(std::move(result));
			_hasPendingProbes = true;
		});
}

/**
 * Rebuilds the rendezvous-server combobox labels/enabled state from the current
 * probe status. Waiting servers show " (Wait...)", offline ones " (offline)".
 */
void ServerList::rebuildServerCombo()
{
	if (!_cbxServer)
		return;

	std::vector<std::string> names = getRendezvousServerNames();
	std::vector<std::string> labels;
	std::vector<bool> enabled;
	labels.reserve(names.size());
	enabled.reserve(names.size());

	for (size_t i = 0; i < names.size(); ++i)
	{
		int st = (i < _serverStatus.size()) ? _serverStatus[i] : 0;
		std::string suffix = (st == 0) ? " (Wait...)" : (st == 2) ? " (offline)" : "";
		labels.push_back(names[i] + suffix);
		enabled.push_back(st == 1);
	}

	_cbxServer->setOptions(labels, enabled, false);
	// Keep the current selection shown even if it is disabled (offline/waiting).
	_cbxServer->forceSelect(getActiveRendezvousServer());
}

/**
 * Shows a warning and clears the list when the active server is offline;
 * hides the warning otherwise.
 */
void ServerList::updateOfflineWarning()
{
	if (!_txtOfflineWarning)
		return;

	size_t active = getActiveRendezvousServer();
	int status = (active < _serverStatus.size()) ? _serverStatus[active] : 1;
	bool offline = (status == 2);

	// Dim the closed combobox label text (only) when the current selection is
	// offline; the button face keeps its default color.
	if (_cbxServer)
		_cbxServer->setButtonTextColor(offline ? _serverComboDisabledColor : _serverComboColor);

	if (offline)
	{
		_txtOfflineWarning->setText("Selected rendezvous server is offline. Pick another from the list or direct connect with TCP.");
		_txtOfflineWarning->setVisible(true);
		_servers.erase(
			std::remove_if(_servers.begin(), _servers.end(),
						   [](const ServerInfo& server) { return !server.added; }),
			_servers.end());
		_lstServers->clearList();
		sortList(Options::serverOrder);
	}
	else if (status == 1)
	{
		// Online: nothing to warn about.
		_txtOfflineWarning->setVisible(false);
	}
	// status == 0 (still probing): leave the textbox to updateFetchingAnimation().
}

/**
 * While the selected server's probe is still pending, animate a
 * "Fetching server list . . ." message in the same textbox as the offline
 * warning, cycling one dot every 0.5s.
 */
void ServerList::updateFetchingAnimation()
{
	if (!_txtOfflineWarning)
		return;

	size_t active = getActiveRendezvousServer();
	int status = (active < _serverStatus.size()) ? _serverStatus[active] : 1;
	if (status != 0)
		return; // only while the active server is still being probed

	static const char* const kDots[4] = { "", " .", " . .", " . . ." };
	int frame = (SDL_GetTicks() / 500) % 4;

	_txtOfflineWarning->setText(std::string("Fetching server list") + kDots[frame]);
	_txtOfflineWarning->setVisible(true);
}

/**
 * @return Path of the per-user last-selected rendezvous server file.
 */
std::string ServerList::selectionFilePath()
{
	return Options::getMasterUserFolder() + "rendezvous_selection.json";
}

/**
 * @return The saved server name, or "" if none/unreadable (caller falls back to
 * the first configured server).
 */
std::string ServerList::loadSavedServerName()
{
	std::string path = selectionFilePath();
	if (!OpenXcom::CrossPlatform::fileExists(path))
		return std::string();

	std::ifstream file(path, std::ifstream::binary);
	if (!file.is_open())
		return std::string();

	Json::Value root;
	Json::CharReaderBuilder builder;
	std::string errs;
	if (!Json::parseFromStream(builder, file, &root, &errs))
		return std::string();

	return root.get("name", "").asString();
}

/**
 * Persists the last-selected rendezvous server name.
 */
void ServerList::saveSelectedServerName(const std::string& name)
{
	Json::Value root;
	root["name"] = name;

	Json::StreamWriterBuilder builder;
	std::ofstream file(selectionFilePath(), std::ofstream::binary);
	if (file.is_open())
		file << Json::writeString(builder, root);
}

/**
 * Handler for picking a different rendezvous server from the combobox.
 */
void ServerList::cbxServerChange(Action*)
{
	size_t sel = _cbxServer->getSelected();
	if (!_cbxServer->isEnabled(sel))
		return; // disabled row (offline/waiting) - ignore

	setActiveRendezvousServer(sel);

	std::vector<std::string> names = getRendezvousServerNames();
	if (sel < names.size())
		saveSelectedServerName(names[sel]);

	_txtOfflineWarning->setVisible(false);
	_lstServers->clearList();
	updateServerList();
	updateOfflineWarning();
}

}
