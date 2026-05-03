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


namespace OpenXcom
{

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

struct compareServerLatency
{
	bool _reverse;

	compareServerLatency(bool reverse) : _reverse(reverse) {}

	bool operator()(const ServerInfo& a, const ServerInfo& b) const
	{
		if (a.reserved == b.reserved)
		{
			return a.latency < b.latency;
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
	_txtName = new Text(150, 9, 16, isMobile ? 40 : 32);
	_txtPlayers = new Text(110, 9, 204, isMobile ? 40 : 32);
	_txtLatency = new Text(110, 9, 263, isMobile ? 40 : 32);
	_lstServers = new TextList(288, isMobile ? 96 : 104, 8, isMobile ? 50 : 42);
	_txtDetails = new Text(288, 16, 16, 148);
	_sortName = new ArrowButton(ARROW_NONE, 11, 8, 16, isMobile ? 40 : 32);
	_sortPlayers = new ArrowButton(ARROW_NONE, 11, 8, 204, isMobile ? 40 : 32);
	_sortLatency = new ArrowButton(ARROW_NONE, 11, 8, 263, isMobile ? 40 : 32);
	_btnJoin = new ToggleTextButton(288, 16, 16, 23);

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
	add(_txtLatency, "text", "saveMenus");
	add(_lstServers, "list", "saveMenus");
	add(_txtDetails, "text", "saveMenus");
	add(_sortName, "text", "saveMenus");
	add(_sortPlayers, "text", "saveMenus");
	add(_sortLatency, "text", "saveMenus");
	add(_btnJoin, "button", "saveMenus");

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
		_txtJoin->setVisible(false);
		_btnJoin->setText("Left click to join.");
	}
	else
	{
		_btnJoin->setVisible(false);
		_txtJoin->setAlign(ALIGN_CENTER);
		_txtJoin->setText("Left click to join.");
	}

	_txtName->setText("Server");

	_txtPlayers->setText("Players");

	_txtLatency->setText("Latency");

	_lstServers->setColumns(3, 188, 60, 40);
	_lstServers->setSelectable(true);
	_lstServers->setBackground(_window);
	_lstServers->setMargin(8);
	_lstServers->onMouseOver((ActionHandler)&ServerList::lstSavesMouseOver);
	_lstServers->onMouseOut((ActionHandler)&ServerList::lstSavesMouseOut);
	_lstServers->onMousePress((ActionHandler)&ServerList::lstSavesPress);

	_txtDetails->setWordWrap(true);
	_txtDetails->setText(tr("STR_DETAILS").arg(""));

	_sortName->setX(_sortName->getX() + _txtName->getTextWidth() + 5);
	_sortName->onMouseClick((ActionHandler)&ServerList::sortNameClick);

	_sortPlayers->setX(_sortPlayers->getX() + _txtPlayers->getTextWidth() + 5);
	_sortPlayers->onMouseClick((ActionHandler)&ServerList::sortPlayersClick);

	_sortLatency->setX(_sortLatency->getX() + _txtLatency->getTextWidth() + 5);
	_sortLatency->onMouseClick((ActionHandler)&ServerList::sortLatencyClick);

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

	try
	{
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, true, 2, 1, 0, false, "Gamemode: PVE, Mods: NO, Password: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, true, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, true, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));
		_servers.push_back(ServerInfo({"test", "player1", "127.0.0.1", 3000, false, 2, 1, 0, false, "Description: test, Gamemode: PVE, Mods: NO"}));

		_lstServers->clearList();
		sortList(Options::serverOrder);
	}
	catch (Exception &e)
	{
		Log(LOG_ERROR) << e.what();
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
	_sortLatency->setShape(ARROW_NONE);
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
	case SORT_LATENCY_ASC:
		_sortLatency->setShape(ARROW_SMALL_UP);
		break;
	case SORT_LATENCY_DESC:
		_sortLatency->setShape(ARROW_SMALL_DOWN);
		break;
	}

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
	case SORT_LATENCY_ASC:
		std::sort(_servers.begin(), _servers.end(), compareServerLatency(false));
		break;
	case SORT_LATENCY_DESC:
		std::sort(_servers.rbegin(), _servers.rend(), compareServerLatency(true));
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

		std::string isOnline = "Offline";
		if (serverInfo.online)
		{
			isOnline = "Online";
		}

		std::string server = serverInfo.name + " [" + isOnline + "]"; 
		std::string players = std::to_string(serverInfo.currentPlayers) + "/" + std::to_string(serverInfo.maxPlayers);
		std::string latency = std::to_string(serverInfo.latency) + " ms";
		_lstServers->addRow(3, server, players, latency);
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
 * Shows the details of the currently hovered save.
 * @param action Pointer to an action.
 */
void ServerList::lstSavesMouseOver(Action*)
{
	int sel = _lstServers->getSelectedRow() - _firstValidRow;
	std::string wstr;
	if (sel >= 0 && sel < (int)_servers.size())
	{
		wstr = _servers[sel].details;
	}
	_txtDetails->setText(tr("STR_DETAILS").arg(wstr));
}

/**
 * Clears the details.
 * @param action Pointer to an action.
 */
void ServerList::lstSavesMouseOut(Action*)
{
	_txtDetails->setText(tr("STR_DETAILS").arg(""));
}

/**
 * Deletes the selected save.
 * @param action Pointer to an action.
 */
void ServerList::lstSavesPress(Action* action)
{

	/*
	if ((action->getDetails()->button.button == SDL_BUTTON_RIGHT || _btnDelete->getPressed()) && _lstSaves->getSelectedRow() >= _firstValidRow)
	{
		_game->pushState(new DeleteGameState(_origin, _saves[_lstSaves->getSelectedRow() - _firstValidRow].fileName));
	}
	*/

	/*
	if (action->getDetails()->button.button == SDL_BUTTON_LEFT && !_btnDelete->getPressed())
	{
		loadSave(_lstSaves->getSelectedRow());
	}
	*/

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

void ServerList::sortLatencyClick(Action* action)
{
	if (_sortable)
	{
		if (Options::serverOrder == SORT_LATENCY_ASC)
		{
			Options::serverOrder = SORT_LATENCY_DESC;
		}
		else
		{
			Options::serverOrder = SORT_LATENCY_ASC;
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

}
