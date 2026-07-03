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
#include  "LobbyMenu.h"
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

#include "HostMenu.h"

namespace OpenXcom
{

struct comparePlayerName
{
	bool _reverse;

	comparePlayerName(bool reverse) : _reverse(reverse) {}

	bool operator()(const playerInfo& a, const playerInfo& b) const
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

struct comparePlayerTeam
{
	bool _reverse;

	comparePlayerTeam(bool reverse) : _reverse(reverse) {}

	bool operator()(const playerInfo& a, const playerInfo& b) const
	{
		if (a.reserved == b.reserved)
		{
			return Unicode::naturalCompare(a.team, b.team);
		}
		else
		{
			return _reverse ? b.reserved : a.reserved;
		}
	}
};

struct comparePlayerLatency
{
	bool _reverse;

	comparePlayerLatency(bool reverse) : _reverse(reverse) {}

	bool operator()(const playerInfo& a, const playerInfo& b) const
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
LobbyMenu::LobbyMenu() : _sortable(true)
{

	connectionTCP::isLobbyMenuClosed = false;

	// coop chat menu
	if (!_game->getCoopMod()->getChatMenu())
	{
		Font* smallFont = _game->getMod()->getFont("FONT_SMALL");
		_game->getCoopMod()->setChatMenu(new ChatMenu(smallFont, _game));
	}

	_screen = false;

	bool isMobile = false;
#ifdef __MOBILE__
	isMobile = true;
#endif

	// Create objects
	_window = new Window(this, 320, 200, 0, 0, POPUP_BOTH);

	int y = 172;
	int h = 16;

	int wHost = 101;
	int wDirect = 101;
	int wAdd = 101;

	int x = 8;
	_btnDisconnect = new TextButton(wHost, h, x, y);
	x += wHost;
	_btnChat = new TextButton(wDirect, h, x, y);
	x += wDirect;
	_btnCancel = new TextButton(wAdd, h, x, y);

	_txtTitle = new Text(310, 17, 5, 7);
	_txtChangeTeam = new Text(310, 9, 5, 23);
	_txtName = new Text(150, 9, 16, isMobile ? 40 : 32);
	_txtTeam = new Text(110, 9, 204, isMobile ? 40 : 32);
	_txtLatency = new Text(110, 9, 263, isMobile ? 40 : 32);
	_lstPlayers = new TextList(288, isMobile ? 104 : 112, 8, isMobile ? 50 : 42);
	_txtDetails = new Text(288, 16, 16, 156);
	_sortName = new ArrowButton(ARROW_NONE, 11, 8, 16, isMobile ? 40 : 32);
	_sortTeam = new ArrowButton(ARROW_NONE, 11, 8, 204, isMobile ? 40 : 32);
	_sortLatency = new ArrowButton(ARROW_NONE, 11, 8, 263, isMobile ? 40 : 32);

	// Set palette
	setInterface("geoscape", true, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "saveMenus");
	add(_btnDisconnect, "button", "saveMenus");
	add(_btnChat, "button", "saveMenus");
	add(_btnCancel, "button", "saveMenus");
	add(_txtTitle, "text", "saveMenus");
	add(_txtChangeTeam, "text", "saveMenus");
	add(_txtName, "text", "saveMenus");
	add(_txtTeam, "text", "saveMenus");
	add(_txtLatency, "text", "saveMenus");
	add(_lstPlayers, "list", "saveMenus");
	add(_txtDetails, "text", "saveMenus");
	add(_sortName, "text", "saveMenus");
	add(_sortTeam, "text", "saveMenus");
	add(_sortLatency, "text", "saveMenus");

	// Set up objects
	setWindowBackground(_window, "saveMenus");

	_btnDisconnect->setText("DISCONNECT");
	_btnDisconnect->onMouseClick((ActionHandler)&LobbyMenu::btnDisconnectClick);

	std::string n = SDL_GetKeyName(Options::keyChat);
	if (n.size() == 1)
		n[0] = (char)std::toupper((unsigned char)n[0]);

	std::string label = std::string(tr("CHAT").c_str()) + " [" + n + "]";
	_btnChat->setText(label.c_str());
	_btnChat->onMouseClick((ActionHandler)&LobbyMenu::btnChatClick);

	_btnCancel->setText("CANCEL");
	_btnCancel->onMouseClick((ActionHandler)&LobbyMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&LobbyMenu::btnCancelClick, Options::keyCancel);

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);

	std::string lobby_title = _game->getCoopMod()->getCurrentClientServer();

	if (_game->getCoopMod()->getServerOwner() == true)
	{
		lobby_title = _game->getCoopMod()->getHostServer();
	}

	_txtTitle->setText(lobby_title);

	if (isMobile)
	{
		_txtChangeTeam->setText("Left click to switch player to the other team.");
	}
	else
	{
		_txtChangeTeam->setAlign(ALIGN_CENTER);
		_txtChangeTeam->setText("Left click to switch player to the other team.");
	}

	if (_game->getCoopMod()->getServerOwner() == true)
	{
		_txtChangeTeam->setVisible(true);
	}
	else
	{
		_txtChangeTeam->setVisible(false);
	}

	if (connectionTCP::isCoopSessionLocked == true)
	{
		_txtChangeTeam->setVisible(false);
	}

	connectionTCP::forceCloseCoopStateMenu = false;
	connectionTCP::forceClosePasswordCheckMenu = false;

	_txtName->setText("Player");
	_txtTeam->setText("Team");
	_txtLatency->setText("Latency");

	_lstPlayers->setColumns(188, 188, 60, 40);
	_lstPlayers->setSelectable(true);
	_lstPlayers->setBackground(_window);
	_lstPlayers->setMargin(8);
	_lstPlayers->onMouseOver((ActionHandler)&LobbyMenu::lstSavesMouseOver);
	_lstPlayers->onMouseOut((ActionHandler)&LobbyMenu::lstSavesMouseOut);
	_lstPlayers->onMousePress((ActionHandler)&LobbyMenu::lstSavesPress);

	Uint8 color = 239; // 239 or 255

	if (_game->getSavedGame())
	{
		if (_game->getSavedGame()->getSavedBattle())
		{
			color = 255;
		}
	}

	_txtDetails->setWordWrap(true);
	_txtDetails->setText(tr("STR_DETAILS").arg("Waiting for players on port " + std::to_string(tcp_port)));

	_sortName->setX(_sortName->getX() + _txtName->getTextWidth() + 5);
	_sortName->onMouseClick((ActionHandler)&LobbyMenu::sortNameClick);

	_sortLatency->setX(_sortLatency->getX() + _txtLatency->getTextWidth() + 5);
	_sortLatency->onMouseClick((ActionHandler)&LobbyMenu::sortLatencyClick);

	_sortTeam->setX(_sortTeam->getX() + _txtTeam->getTextWidth() + 5);
	_sortTeam->onMouseClick((ActionHandler)&LobbyMenu::sortTeamClick);

	updateArrows();

}

/**
 *
 */
LobbyMenu::~LobbyMenu()
{

}

/**
 * Refreshes the saves list.
 */
void LobbyMenu::init()
{
	State::init();

	// coop chat menu
	if (!_game->getCoopMod()->getChatMenu())
	{
		Font* smallFont = _game->getMod()->getFont("FONT_SMALL");
		_game->getCoopMod()->setChatMenu(new ChatMenu(smallFont, _game));
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

	_lstPlayers->clearList();
	sortList(Options::playerOrder);

}

void LobbyMenu::sortList(playerSort sort)
{
	switch (sort)
	{
	case SORT_NAME_LOBBY_ASC:
		std::sort(_connectedPlayers.begin(), _connectedPlayers.end(), comparePlayerName(false));
		break;
	case SORT_NAME_LOBBY_DESC:
		std::sort(_connectedPlayers.rbegin(), _connectedPlayers.rend(), comparePlayerName(true));
		break;
	case SORT_TEAM_LOBBY_ASC:
		std::sort(_connectedPlayers.begin(), _connectedPlayers.end(), comparePlayerTeam(false));
		break;
	case SORT_TEAM_LOBBY_DESC:
		std::sort(_connectedPlayers.rbegin(), _connectedPlayers.rend(), comparePlayerTeam(true));
		break;
	case SORT_LATENCY_LOBBY_ASC:
		std::sort(_connectedPlayers.begin(), _connectedPlayers.end(), comparePlayerLatency(false));
			break;
	case SORT_LATENCY_LOBBY_DESC:
		std::sort(_connectedPlayers.rbegin(), _connectedPlayers.rend(), comparePlayerLatency(true));
		break;
	}
	updateList();
}

void LobbyMenu::updateList()
{
	int row = 0;
	int color = _lstPlayers->getSecondaryColor();
	for (auto &playerInfo : _connectedPlayers)
	{
		std::string latencyText = playerInfo.latency + " ms";
		_lstPlayers->addRow(3, playerInfo.name.c_str(), playerInfo.team.c_str(), latencyText.c_str());
		if (playerInfo.reserved && _origin != OPT_BATTLESCAPE)
		{
			_lstPlayers->setRowColor(row, color);
		}
		row++;
	}
}

/**
 * Updates the sorting arrows based
 * on the current setting.
 */
void LobbyMenu::updateArrows()
{
	_sortName->setShape(ARROW_NONE);
	_sortLatency->setShape(ARROW_NONE);
	switch (Options::playerOrder)
	{
	case SORT_NAME_LOBBY_ASC:
		_sortName->setShape(ARROW_SMALL_UP);
		break;
	case SORT_NAME_LOBBY_DESC:
		_sortName->setShape(ARROW_SMALL_DOWN);
		break;
	case SORT_LATENCY_LOBBY_ASC:
		_sortLatency->setShape(ARROW_SMALL_UP);
		break;
	case SORT_LATENCY_LOBBY_DESC:
		_sortLatency->setShape(ARROW_SMALL_DOWN);
		break;
	}

}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void LobbyMenu::btnCancelClick(Action*)
{

	connectionTCP::isPlayerReady = !connectionTCP::isPlayerReady;

	if (connectionTCP::isCoopSessionLocked == false && _game->getCoopMod()->getCoopStatic() == true)
	{

		if (_game->getCoopMod()->isCoopSession() == true && _game->getCoopMod()->getServerOwner() == true)
		{

			_countdown = 30;

			if (connectionTCP::isPlayerReady == true)
			{
				_timerStarted = true;
			}
			else
			{
				_timerStarted = false;
			}
	
		}

		Json::Value root;
		root["state"] = "coop_session_locked";
		root["isPlayerReady"] = connectionTCP::isPlayerReady;

		_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

		if (connectionTCP::isPlayerReady == true && connectionTCP::isPlayersReady == true)
		{
			connectionTCP::isCoopSessionLocked = true;
		}

	}
	else if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->isCoopSession() == true)
	{
		connectionTCP::isLobbyMenuClosed = true;
		_game->popState();

		if (connectionTCP::LobbyFileStatus == 1 && _game->getCoopMod()->getCoopStatic() == true)
		{
			Json::Value root;

			root["state"] = "SEND_FILE_HOST_SAVE";

			_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

			// fix
			connectionTCP::LobbyFileStatus = -1;

		}
		else if (connectionTCP::LobbyFileStatus == 2 && _game->getCoopMod()->getCoopStatic() == true)
		{
			Json::Value root;

			root["state"] = "SEND_FILE_CLIENT_SAVE";

			_game->getCoopMod()->inventory_battle_window = false;

			_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

			_game->pushState(new CoopState(1));

			// fix
			connectionTCP::LobbyFileStatus = -1;

		}

	}
	// If the session is locked and there is no connection, exit the lobby menu and disconnect
	else
	{

		if (_game->getCoopMod()->getServerOwner() == false)
		{
			connectionTCP::isLobbyMenuClosed = true;
			_game->popState();
			_game->getCoopMod()->disconnectTCP(true);
		}

	}

}

void LobbyMenu::btnDisconnectClick(Action* action)
{

	connectionTCP::isLobbyMenuClosed = true;

	_game->popState();

	// If the host presses the disconnect button and is allowed to save player progress, disconnect and return to the Host menu
	if (Options::HostSaveProgress == true && _game->getCoopMod()->getCoopCampaign() == true && _game->getCoopMod()->getServerOwner() == true)
	{
		_game->getCoopMod()->setServerOwner(false);
		_game->getCoopMod()->disconnectTCP(true);
		_game->pushState(new HostMenu());
	}
	// If the client presses Disconnect and the host is allowed to save player progress, or there are no bases, disconnect and return to the main menu
	else if ((Options::HostSaveProgress == true || connectionTCP::no_bases == true) && _game->getCoopMod()->getCoopCampaign() == true && _game->getCoopMod()->getServerOwner() == false)
	{
		_game->getCoopMod()->setServerOwner(false);
		_game->getCoopMod()->disconnectTCP();
		_game->pushState(new ServerList());
	}
	// Otherwise, do nothing and just return to the server list
	else
	{
		_game->getCoopMod()->setServerOwner(false);
		// Prevent returning to the main menu
		_game->getCoopMod()->disconnectTCP(true);
		_game->pushState(new ServerList());
	}

}

void LobbyMenu::btnChatClick(Action* action)
{

	// coop chat menu
	if (!_game->getCoopMod()->getChatMenu())
	{
		Font* smallFont = _game->getMod()->getFont("FONT_SMALL");
		_game->getCoopMod()->setChatMenu(new ChatMenu(smallFont, _game));
	}

	if (_game->getCoopMod()->getChatMenu())
	{
		_game->getCoopMod()->getChatMenu()->setActive(!_game->getCoopMod()->getChatMenu()->isActive());
	}
	
}

/**
 * Shows the details of the currently hovered save.
 * @param action Pointer to an action.
 */
void LobbyMenu::lstSavesMouseOver(Action*)
{
	int sel = _lstPlayers->getSelectedRow() - _firstValidRow;
	std::string wstr;
	if (sel >= 0 && sel < (int)_connectedPlayers.size())
	{
		wstr = _connectedPlayers[sel].details;
	}
	if (wstr.empty())
		wstr = "Waiting for players on port " + std::to_string(tcp_port);
	_txtDetails->setText(tr("STR_DETAILS").arg(wstr));
}

/**
 * Clears the details.
 * @param action Pointer to an action.
 */
void LobbyMenu::lstSavesMouseOut(Action*)
{
	_txtDetails->setText(tr("STR_DETAILS").arg("Waiting for players on port " + std::to_string(tcp_port)));
}

/**
 * Deletes the selected save.
 * @param action Pointer to an action.
 */
void LobbyMenu::lstSavesPress(Action* action)
{

	// The host switches a player to another team.
	if (_game->getCoopMod()->getServerOwner() == true && action->getDetails()->button.button == SDL_BUTTON_LEFT && connectionTCP::isCoopSessionLocked == false)
	{
		auto connectedPlayer = _connectedPlayers;  
		int sel = _lstPlayers->getSelectedRow() - _firstValidRow;
		if (sel >= 0 && sel < (int)connectedPlayer.size())
		{

			if (connectedPlayer[sel].team == "XCOM")
			{
				connectedPlayer[sel].team = "Alien"; 
			}
			else
			{
				connectedPlayer[sel].team = "XCOM"; 
			}

		}

		bool isHostAlien = false;
		bool isClientAlien = false;

		for (const auto& player : connectedPlayer)
		{
			if (player.team == "XCOM")
			{

				if (player.id == 1)
				{
					isHostAlien = false; 
				}
				else
				{
					isClientAlien = false;
				}

			}
			else
			{

				if (player.id == 1)
				{
					isHostAlien = true;
				}
				else
				{
					isClientAlien = true;
				}

			}
		}

		// no mode = 0, PVE = 1, PVP = 2, PVP2 = 3, PVE2 = 4,
		int current_gamemode = 0;

		// PVE
		if (isHostAlien == false && isClientAlien == false)
		{
			current_gamemode = 1;
		}
		// PVE2
		else if (isHostAlien == true && isClientAlien == true)
		{
			current_gamemode = 4;
		}
		// PVP
		else if (isHostAlien == false && isClientAlien == true)
		{
			current_gamemode = 2;
		}
		// PVP2
		else if (isHostAlien == true && isClientAlien == false)
		{
			current_gamemode = 3;
		}

		connectionTCP::_coopGamemode = current_gamemode;

		Json::Value root;
		root["state"] = "change_team";
		root["gamemode"] = connectionTCP::_coopGamemode;

		_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

	}
	else if (_game->getCoopMod()->getServerOwner() == true && action->getDetails()->button.button == SDL_BUTTON_RIGHT)
	{

		int sel = _lstPlayers->getSelectedRow() - _firstValidRow;
		if (sel >= 0 && sel < (int)_connectedPlayers.size())
		{

			if (_connectedPlayers[sel].id != 1)
			{

				_game->pushState(new CoopState(12345));

			}

		}

	}

}

void LobbyMenu::disableSort()
{
	_sortable = false;
}

void LobbyMenu::think()
{

	State::think();

	if (_game->getCoopMod()->getServerOwner() == false && _game->getCoopMod()->getCoopStatic() == false)
	{
		
		_game->popState();

		_game->pushState(new ServerList());

	}

	static Uint32 lastUpdate = 0;
	static Uint32 pingSentTime = 0;
	Uint32 now = SDL_GetTicks();

	if (now - lastUpdate >= 1000)
	{

		lastUpdate = now;

		// own player
		std::string txtTeam = "XCOM";
		const int hostId = 1;

		auto itHost = std::find_if(_connectedPlayers.begin(), _connectedPlayers.end(),
							   [hostId](const playerInfo& p)
							   {
								   return p.id == hostId;
							   });

		if (itHost == _connectedPlayers.end())
		{

			_connectedPlayers.push_back(playerInfo({hostId, _game->getCoopMod()->getHostName(), "0", false, "XCOM", "Funds: 0 Bases: 0, Crafts: 0"}));
			itHost = std::prev(_connectedPlayers.end());

		}

		if (((_game->getCoopMod()->getCoopGamemode() == 2 && _game->getCoopMod()->getHost() == false) || (_game->getCoopMod()->getCoopGamemode() == 3 && _game->getCoopMod()->getHost() == true)) || _game->getCoopMod()->getCoopGamemode() == 4)
		{
			txtTeam = "Alien";
		}
	

		int base_count = 0;
		int craft_count = 0;
		if (_game->getSavedGame() && _game->getSavedGame()->getBases())
		{
			for (auto& base : *_game->getSavedGame()->getBases())
			{
				if (base->_coopBase == false)
				{

					for (auto& craft : *base->getCrafts())
					{
						craft_count++;
					}

					base_count++;
				}
			}
		}

		// status
		std::string txtStatus = " ";

		if (connectionTCP::isCoopSessionLocked == false)
		{

			std::string txtTimer = "";

			// timer
			if (_timerStarted == true && _game->getCoopMod()->getServerOwner() == true)
			{
				_countdown--;
				txtTimer = " (" + std::to_string(_countdown) + "s)";

				Json::Value root;
				root["state"] = "lobby_timer";
				root["timer"] = _countdown;

				_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

			}

			if (connectionTCP::lobby_timer != -1)
			{
				txtTimer = " (" + std::to_string(connectionTCP::lobby_timer) + "s)";
			}

			if (_countdown <= 0 && _game->getCoopMod()->getServerOwner() == true)
			{
				_timerStarted = false;

				// Do something after one minute
				Json::Value root;
				root["state"] = "lobby_ready";
				connectionTCP::isCoopSessionLocked = true;

				_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

			}

			if (connectionTCP::isPlayerReady == true)
			{
				txtStatus = " (READY)";
				_btnCancel->setText("NOT READY" + txtTimer);
			}
			else
			{
				txtStatus = " (NOT READY)";
				_btnCancel->setText("READY" + txtTimer);
			}
		}
		else
		{
			_btnCancel->setText("CANCEL");
		}

		// funds
		int64_t funds = 0;
		if (_game->getSavedGame() && _game->getSavedGame()->getFunds())
		{
			funds = _game->getSavedGame()->getFunds();
		}

		std::string txtDetails = "Funds: " + std::to_string(funds) + " Bases: " + std::to_string(base_count) + " Crafts: " + std::to_string(craft_count);

		itHost->name = _game->getCoopMod()->getHostName() + txtStatus;
		itHost->team = txtTeam;
		itHost->details = txtDetails;
		
		// other players
		if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->isCoopSession() == true)
		{
			const int playerId = 2; // fix later...

			std::string clientName = _game->getCoopMod()->getCurrentClientName();
			std::string ping = _game->getCoopMod()->getPing();

			auto itClient = std::find_if(_connectedPlayers.begin(), _connectedPlayers.end(),
										 [playerId](const playerInfo& p)
										 {
											 return p.id == playerId;
										 });

			if (itClient == _connectedPlayers.end())
			{
				_connectedPlayers.push_back(playerInfo{playerId, clientName, "-999", false, "XCOM", "Funds: 0 Bases: 0, Crafts: 0"});
				itClient = std::prev(_connectedPlayers.end());
			}

			// status
			std::string txtStatus2 = "";

			if (connectionTCP::isCoopSessionLocked == false)
			{

				if (connectionTCP::isPlayersReady == true)
				{
					txtStatus2 = " (READY)";
				}
				else
				{
					txtStatus2 = " (NOT READY)";
				}
			}

			if (_game->getCoopMod()->getCoopGamemode() == 4)
			{
				txtTeam = "Alien";
			}
			else if (_game->getCoopMod()->getCoopGamemode() == 2 || _game->getCoopMod()->getCoopGamemode() == 3)
			{
				if (txtTeam == "XCOM")
				{
					txtTeam = "Alien";
				}
				else
				{
					txtTeam = "XCOM";
				}
			}
			else
			{
				txtTeam = "XCOM";
			}
	
			txtDetails = "Funds: " + std::to_string(_game->getCoopMod()->playersFunds) + " Bases: " + std::to_string(_game->getCoopMod()->playersBases) + " Crafts: " + std::to_string(_game->getCoopMod()->playersCrafts);

			itClient->name = clientName + txtStatus2;
			itClient->team = txtTeam;
			itClient->latency = ping;
			itClient->details = txtDetails;
	
		}
		else
		{
	
			const int clientId = 2;

			auto itClient = std::find_if(_connectedPlayers.begin(), _connectedPlayers.end(),
										 [clientId](const playerInfo& p)
										 {
											 return p.id == clientId;
										 });

			if (itClient != _connectedPlayers.end())
			{
				_connectedPlayers.erase(itClient);
			}

		}

		_lstPlayers->clearList();
		sortList(Options::playerOrder);

	}

}

void LobbyMenu::sortNameClick(Action* action)
{
	if (_sortable)
	{
		if (Options::playerOrder == SORT_NAME_LOBBY_ASC)
		{
			Options::playerOrder = SORT_NAME_LOBBY_DESC;
		}
		else
		{
			Options::playerOrder = SORT_NAME_LOBBY_ASC;
		}
		updateArrows();
		_lstPlayers->clearList();
		sortList(Options::playerOrder);
	}
}

void LobbyMenu::sortLatencyClick(Action* action)
{
	if (_sortable)
	{
		if (Options::playerOrder == SORT_LATENCY_LOBBY_ASC)
		{
			Options::playerOrder = SORT_LATENCY_LOBBY_DESC;
		}
		else
		{
			Options::playerOrder = SORT_LATENCY_LOBBY_ASC;
		}
		updateArrows();
		_lstPlayers->clearList();
		sortList(Options::playerOrder);
	}
}

void LobbyMenu::sortTeamClick(Action* action)
{
	if (_sortable)
	{
		if (Options::playerOrder == SORT_TEAM_LOBBY_ASC)
		{
			Options::playerOrder = SORT_TEAM_LOBBY_DESC;
		}
		else
		{
			Options::playerOrder = SORT_TEAM_LOBBY_ASC;
		}
		updateArrows();
		_lstPlayers->clearList();
		sortList(Options::playerOrder);
	}
}

}
