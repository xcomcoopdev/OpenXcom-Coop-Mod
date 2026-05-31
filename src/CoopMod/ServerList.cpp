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
#include "ServerList.h"
#include "../Engine/Logger.h"
#include "../Savegame/SavedGame.h"
#include "../Engine/Game.h"
#include "../Engine/Action.h"
#include "../Engine/Exception.h"
#include "../Engine/Options.h"
#include "../Engine/Unicode.h"
#include "../Mod/Mod.h"
#include "../Engine/LocalizedText.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Interface/TextList.h"
#include "../Interface/ToggleTextButton.h"
#include "../Interface/ArrowButton.h"

#include  "HostMenu.h"
#include "DirectConnect.h"
#include "connectionUDP/connection_rendezvous_glue.h"
#include "PasswordCheckMenu.h"


namespace OpenXcom
{

std::mutex _serverListMutex;
std::vector<OpenXcom::RendezvousClient::RoomInfo> _pendingRooms;
bool _pendingRoomsOk = false;
bool _hasPendingRooms = false;
bool _isRefreshingServers = false;

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

	_txtTitle = new Text(310, 17, 5, 7);
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

	_btnFilter->setText("Filter");
	_btnFilter->onMouseClick((ActionHandler)&ServerList::btnFilterClick);

	_btnHost->setText("Host");
	_btnHost->onMouseClick((ActionHandler)&ServerList::btnHostClick);

	_btnDirectConnect->setText("Direct Connect");
	_btnDirectConnect->onMouseClick((ActionHandler)&ServerList::btnDirectConnectClick);

	_btnAddServer->setText("Add Server");

	_btnRefresh->setText("Refresh");
	_btnRefresh->onMouseClick((ActionHandler)&ServerList::btnRefreshClick);

	_btnCancel->setText(tr("STR_CANCEL"));
	_btnCancel->onMouseClick((ActionHandler)&ServerList::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&ServerList::btnCancelClick, Options::keyCancel);

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);
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

	// check if campaign
	if (!_game->getSavedGame()->getCountries()->empty())
	{
		_game->getCoopMod()->setCoopCampaign(true);
	}
	else
	{
		_game->getCoopMod()->setCoopCampaign(false);
	}

	updateServerList();

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

}

void ServerList::btnRefreshClick(Action* action)
{
	updateServerList();
}

void ServerList::btnHostClick(Action* action)
{
	_game->pushState(new HostMenu());
}

void ServerList::btnDirectConnectClick(Action* action)
{
	_game->pushState(new DirectConnect());
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

			if (_servers[sel].isUDP == true)
			{

				if (_servers[sel].passwordRequired == "YES")
				{

					_game->pushState(new PasswordCheckMenu(&_servers[sel], _game->getCoopMod()->getHostName(), true, false));

				}
				else
				{

					_game->pushState(new CoopState(15));

					if (_servers[sel].isLanDiscovery = true)
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

void ServerList::disableSort()
{
	_sortable = false;
}

void ServerList::think()
{
	State::think();

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
			_servers.clear();

			for (const auto& room : rooms)
			{
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
											   room.lanPort}));
			}
		}

		_lstServers->clearList();
		sortList(Options::serverOrder);
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

}
