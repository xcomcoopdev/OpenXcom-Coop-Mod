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

#include "PasswordCheckMenu.h"
#include "../Engine/Surface.h"
#include "connectionUDP/connection_rendezvous_glue.h"

namespace OpenXcom
{

void PasswordCheckMenu::initMenu()
{

	_screen = false;

	int x = 20;

	// Create objects
	_window = new Window(this, 216, 160, x, 20, POPUP_BOTH);

	_password = new TextEdit(this, 180, 18, x + 18, 72);

	_tcpButtonJoin = new TextButton(180, 18, x + 18, 132);

	_txtInfo = new Text(180, 18, x + 18, 95);
	_btnCancel = new TextButton(180, 18, x + 18, 152);
	_txtTitle = new Text(206, 17, x + 5, 32);

	int screenWidth = Options::baseXGeoscape;
	int screenHeight = Options::baseYGeoscape;

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_password);
	add(_tcpButtonJoin, "button", "pauseMenu");
	add(_txtInfo, "text", "pauseMenu");
	add(_btnCancel, "button", "pauseMenu");
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
	_txtTitle->setText(tr("ENTER PASSWORD"));

	_txtInfo->setVisible(false);
	_txtInfo->setAlign(ALIGN_CENTER);
	_txtInfo->setSmall();

	// password
	_password->setColor(color);
	_password->setBig();
	_password->setBorderColor(color);
	_password->setText("PASSWORD");
	_password->setVisible(true);

	_tcpButtonJoin->setText("JOIN");
	_tcpButtonJoin->onMouseClick((ActionHandler)&PasswordCheckMenu::joinTCPGame);
	_tcpButtonJoin->onKeyboardPress((ActionHandler)&PasswordCheckMenu::joinTCPGame, Options::keyOk);
	_tcpButtonJoin->setVisible(true);

	_btnCancel->setText("CANCEL");
	_btnCancel->onMouseClick((ActionHandler)&PasswordCheckMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&PasswordCheckMenu::btnCancelClick, Options::keyCancel);
}
/**
 * Initializes all the elements in the New Battle window.
 * @param game Pointer to the core game.
 */
PasswordCheckMenu::PasswordCheckMenu(ServerInfo* serverinfo, std::string player, bool isUDP, bool isDirect) : _serverinfo(serverinfo), _player(player), _isUDP(isUDP), _isDirect(isDirect)
{
	initMenu();
}

PasswordCheckMenu::PasswordCheckMenu(std::string ipAddress, std::string player, uint16_t port, bool isUDP, bool isDirect) : _ipAddress(ipAddress), _player(player), _port(port), _isUDP(isUDP), _isDirect(isDirect)
{
	initMenu();
}

/**
 *
 */
PasswordCheckMenu::~PasswordCheckMenu()
{
}

/**
 * Resets the menu music and savegame
 * when coming back from the battlescape.
 */
void PasswordCheckMenu::init()
{

}

void PasswordCheckMenu::joinTCPGame(Action* action)
{

	// Ensure the coop state menu does not close immediately.
	connectionTCP::forceCloseCoopStateMenu = false;

	// Ensure the password check menu does not close immediately.
	connectionTCP::forceClosePasswordCheckMenu = false;

	_game->pushState(new CoopState(15));

	_game->getCoopMod()->_waitBC = false;
	_game->getCoopMod()->_waitBH = false;
	_game->getCoopMod()->_battleWindow = false;
	_game->getCoopMod()->_battleInit = false;
	_game->getCoopMod()->coopInventory = false;
	_game->getCoopMod()->coopMissionEnd = false;
	_game->getCoopMod()->inventory_battle_window = true;

	connectionTCP::password = _password->getText();

	if (_serverinfo && _isUDP && !_isDirect)
	{

		if (_serverinfo->isLanDiscovery = true)
		{

			OpenXcom::joinLanRoomViaRendezvousAsync(
				_serverinfo->id,                     // Rendezvous room id.
				_serverinfo->lanHost,                // LAN IP found by UDP discovery, e.g. 192.168.1.50.
				_serverinfo->lanPort,                // Host LAN UDP port, usually 3001 if 3000 is discovery.
				_password->getText(),                // Empty if no password.
				_player,                             // Local player name.
				0                                    // Client local UDP port. 0 = automatic.
			);
		}
		else
		{

			OpenXcom::joinListedViaRendezvousAsync(
				_serverinfo->id,                      // Room ID from the selected server-list entry. This identifies which room to join.
				_password->getText(),                 // Room password. Empty string means no password; must match host password if one is set.
				_game->getCoopMod()->getHostName(),   // Local player name sent to the rendezvous server / shown to the host or lobby.
				0                                     // Local UDP port. 0 = let the OS choose a free UDP port automatically.
			);
		}

	}
	else if (_isUDP && _isDirect)
	{

		OpenXcom::joinLanRoomByAddressViaRendezvousAsync(
			_ipAddress,                         // Host LAN IP from Direct Connect menu.
			_password->getText(),               // Must match host password. Empty is allowed.
			_player,                            // Client player name.
			_port                               // client local UDP port, 0 = automatic
		);

	}
	// TCP
	else if (!_isUDP && _isDirect)
	{

		_game->getCoopMod()->connectTCPServer(_ipAddress, std::to_string(_port));

	}

}

void PasswordCheckMenu::think()
{
	State::think();

	static Uint32 lastUpdate = 0;
	Uint32 now = SDL_GetTicks();

	if (now - lastUpdate >= 500)
	{

		lastUpdate = now;

		// Force-close the co-op state menu
		if (connectionTCP::forceClosePasswordCheckMenu == true)
		{
			connectionTCP::forceClosePasswordCheckMenu = false;
			_game->popState();
		}

	}
	
}

/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void PasswordCheckMenu::btnCancelClick(Action*)
{
	connectionTCP::forceCloseCoopStateMenu = true;
	_game->popState();

}

}

