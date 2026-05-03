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
	_txtName = new Text(150, 9, 16, isMobile ? 40 : 32);
	_txtLatency = new Text(110, 9, 204, isMobile ? 40 : 32);
	_lstPlayers = new TextList(288, isMobile ? 104 : 112, 8, isMobile ? 50 : 42);
	_txtDetails = new Text(288, 16, 16, 156);
	_sortName = new ArrowButton(ARROW_NONE, 11, 8, 16, isMobile ? 40 : 32);
	_sortLatency = new ArrowButton(ARROW_NONE, 11, 8, 204, isMobile ? 40 : 32);

	// Set palette
	setInterface("geoscape", true, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "saveMenus");
	add(_btnDisconnect, "button", "saveMenus");
	add(_btnChat, "button", "saveMenus");
	add(_btnCancel, "button", "saveMenus");
	add(_txtTitle, "text", "saveMenus");
	add(_txtName, "text", "saveMenus");
	add(_txtLatency, "text", "saveMenus");
	add(_lstPlayers, "list", "saveMenus");
	add(_txtDetails, "text", "saveMenus");
	add(_sortName, "text", "saveMenus");
	add(_sortLatency, "text", "saveMenus");

	// Set up objects
	setWindowBackground(_window, "saveMenus");

	_btnDisconnect->setText("DISCONNECT");
	_btnDisconnect->onMouseClick((ActionHandler)&LobbyMenu::btnDisconnectClick);
	_btnDisconnect->onKeyboardPress((ActionHandler)&LobbyMenu::btnDisconnectClick, Options::keyCancel);

	std::string n = SDL_GetKeyName(Options::keyChat);
	if (n.size() == 1)
		n[0] = (char)std::toupper((unsigned char)n[0]);

	std::string label = std::string(tr("CHAT").c_str()) + " [" + n + "]";
	_btnChat->setText(label.c_str());

	_btnChat->onMouseClick((ActionHandler)&LobbyMenu::btnChatClick);
	_btnChat->onKeyboardPress((ActionHandler)&LobbyMenu::btnChatClick, Options::keyCancel);

	_btnCancel->setText(tr("STR_CANCEL"));
	_btnCancel->onMouseClick((ActionHandler)&LobbyMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&LobbyMenu::btnCancelClick, Options::keyCancel);

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);

	std::string lobby_title = "Welcome to " + _game->getCoopMod()->getCurrentClientName() + "'s Server";

	if (_game->getCoopMod()->getServerOwner() == true)
	{
		lobby_title = "Welcome to " + _game->getCoopMod()->getHostName() + "'s Server";
	}

	_txtTitle->setText(lobby_title);

	_txtName->setText("Player");

	_txtLatency->setText("Latency");

	_lstPlayers->setColumns(3, 188, 60, 40);
	_lstPlayers->setSelectable(true);
	_lstPlayers->setBackground(_window);
	_lstPlayers->setMargin(8);
	_lstPlayers->onMouseOver((ActionHandler)&LobbyMenu::lstSavesMouseOver);
	_lstPlayers->onMouseOut((ActionHandler)&LobbyMenu::lstSavesMouseOut);
	_lstPlayers->onMousePress((ActionHandler)&LobbyMenu::lstSavesPress);

	_txtDetails->setWordWrap(true);
	_txtDetails->setText(tr("STR_DETAILS").arg(""));

	_sortName->setX(_sortName->getX() + _txtName->getTextWidth() + 5);
	_sortName->onMouseClick((ActionHandler)&LobbyMenu::sortNameClick);

	_sortLatency->setX(_sortLatency->getX() + _txtLatency->getTextWidth() + 5);
	_sortLatency->onMouseClick((ActionHandler)&LobbyMenu::sortLatencyClick);

	updateArrows();

	_connectedPlayers.push_back(playerInfo({1, _game->getCoopMod()->getHostName(), "0", false, "Funds: 1000000, Bases: 3, Crafts: 6"}));

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
	for (const auto& playerInfo : _connectedPlayers)
	{

		std::string playername = playerInfo.name;
		std::string latency = playerInfo.latency + " ms";
		_lstPlayers->addRow(2, playername, latency);
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
	_game->popState();
}

void LobbyMenu::btnDisconnectClick(Action* action)
{

	_game->popState();

	// If the player has created a server or joined another player's game, close the ServerList and create the LobbyMenu
	if (Options::HostSaveProgress == true && _game->getCoopMod()->getCoopCampaign() == true && _game->getCoopMod()->getServerOwner() == true)
	{
		_game->getCoopMod()->disconnectTCP();
		_game->pushState(new HostMenu());
	}
	else
	{
		_game->getCoopMod()->disconnectTCP();
		_game->pushState(new ServerList());
	}

}

void LobbyMenu::btnChatClick(Action* action)
{

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
	_txtDetails->setText(tr("STR_DETAILS").arg(wstr));
}

/**
 * Clears the details.
 * @param action Pointer to an action.
 */
void LobbyMenu::lstSavesMouseOut(Action*)
{
	_txtDetails->setText(tr("STR_DETAILS").arg(""));
}

/**
 * Deletes the selected save.
 * @param action Pointer to an action.
 */
void LobbyMenu::lstSavesPress(Action* action)
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
	static Uint32 lastUpdateGamemode = 0;
	static Uint32 pingSentTime = 0;
	Uint32 now = SDL_GetTicks();

	if ((_game->getCoopMod()->isCoopSession() == true || _game->getCoopMod()->getServerOwner() == true) && now - lastUpdateGamemode >= 1000)
	{

		std::string str_disconnect2 = "";

		if (_game->getCoopMod()->getCoopGamemode() == 0 || _game->getCoopMod()->getCoopGamemode() == 1)
		{
			str_disconnect2 = "DISCONNECT (PVE)";
		}
		else if (_game->getCoopMod()->getCoopGamemode() == 2)
		{
			str_disconnect2 = "DISCONNECT (PVP)";
		}
		else if (_game->getCoopMod()->getCoopGamemode() == 3)
		{
			str_disconnect2 = "DISCONNECT (PVP2)";
		}
		else if (_game->getCoopMod()->getCoopGamemode() == 4)
		{
			str_disconnect2 = "DISCONNECT (PVE2)";
		}
		_btnDisconnect->setText(str_disconnect2);

		lastUpdateGamemode = now;
	}

	if (_game->getCoopMod()->isCoopSession() && _game->getCoopMod()->getCoopStatic() && now - lastUpdate >= 1000 && _game->getCoopMod()->getChatMenu())
	{
		const int playerId = 2; // fix later...

		std::string name = _game->getCoopMod()->getCurrentClientName();
		std::string ping = _game->getCoopMod()->getPing();

		auto it = std::find_if(_connectedPlayers.begin(), _connectedPlayers.end(),
							   [playerId](const playerInfo& p)
							   {
								   return p.id == playerId;
							   });

		if (it == _connectedPlayers.end())
		{
			_connectedPlayers.push_back(playerInfo{playerId, name, ping, false, "Funds: 1000, Bases: 3"});
			it = std::prev(_connectedPlayers.end());

			_lstPlayers->clearList();
			sortList(Options::playerOrder);
		}

		it->name = name;
		it->latency = ping;

		lastUpdate = now;
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

}
