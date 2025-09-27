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

#include "CoopMenu.h"
#include "../Menu/SaveGameState.h"
#include "../Menu/LoadGameState.h"
#include "CoopState.h"

#include "../Mod/ExtraSprites.h"
#include "../Engine/Surface.h"

#include "Profile.h"

#include "ChatMenu.h"

#include "vorbis/vorbisfile.h"


namespace OpenXcom
{

int current_gamemode = 0;

OggVorbis_File vf;
void* dummy = (void*)&ov_clear;

/**
 * Initializes all the elements in the New Battle window.
 * @param game Pointer to the core game.
 */
CoopMenu::CoopMenu() : _craft(0), _selectType(NewBattleSelectType::MISSION), _isRightClick(false)
{

	// coop chat menu
	if (!_game->getCoopMod()->getChatMenu())
	{
		Font* smallFont = _game->getMod()->getFont("FONT_SMALL");
		_game->getCoopMod()->setChatMenu(new ChatMenu(smallFont, _game));
	}

	_screen = false;

	// coop
	_game->getSavedGame()->setCoop(this);

	int x = 20;

	// ping
	_hostPing = new Text(180, 18, x + 20, 60);
	_clientPing = new Text(180, 18, x + 20, 72);
	
	// Create objects
	_window = new Window(this, 216, 160, x, 20, POPUP_BOTH);
	_lstSaves = new TextList(180, 18, x + 18, 60);
	_ipAddress = new TextEdit(this, 180, 18, x + 18, 72);
	_playerName = new TextEdit(this, 180, 18, x + 18, 92);
	_tcpButtonJoin = new TextButton(180, 18, x + 18, 112);
	_tcpButtonHost = new TextButton(180, 18, x + 18, 132);

	_btnPVE = new TextButton(180, 18, x + 18, 52);
	_btnPVE2 = new TextButton(180, 18, x + 18, 52);
	_btnPVP = new TextButton(180, 18, x + 18, 52);
	_btnPVP2 = new TextButton(180, 18, x + 18, 52);

	_btnMessage = new TextButton(180, 18, x + 18, 92);

	_btnChat = new TextButton(180, 18, x + 18, 112);

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
	add(_playerName);
	add(_tcpButtonJoin, "button", "pauseMenu");
	add(_tcpButtonHost, "button", "pauseMenu");
	add(_btnMessage, "button", "pauseMenu");
	add(_btnChat, "button", "pauseMenu");
	add(_btnPVE, "button", "pauseMenu");
	add(_btnPVE2, "button", "pauseMenu");
	add(_btnPVP, "button", "pauseMenu");
	add(_btnPVP2, "button", "pauseMenu");
	add(_txtInfo, "text", "pauseMenu");
	add(_btnCancel, "button", "pauseMenu");
	add(_txtData, "text", "pauseMenu");
	add(_txtTitle, "text", "pauseMenu");
	add(_hostPing, "text", "pauseMenu");
	add(_clientPing, "text", "pauseMenu");


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
	_txtTitle->setText(tr("COOP MENU"));

	_txtData->setAlign(ALIGN_CENTER);
	_txtData->setBig();
	_txtData->setText("HELLO");
	_txtData->setVisible(false);


	_txtInfo->setVisible(false);
	_txtInfo->setAlign(ALIGN_CENTER);
	_txtInfo->setSmall();

	// ping
	// ping host
	_hostPing->setVisible(false);
	_hostPing->setAlign(ALIGN_CENTER);
	_hostPing->setSmall();
	_hostPing->setColor(color);
	_hostPing->setBorderColor(color);

	std::string name = _game->getCoopMod()->getHostName();
	std::string ping = "0";
	_hostPing->setText("Player: " + name + " | Latency: " + ping + " ms");

	// ping client
	_clientPing->setVisible(false);
	_clientPing->setAlign(ALIGN_CENTER);
	_clientPing->setSmall();
	_clientPing->setColor(color);
	_clientPing->setBorderColor(color);

	// ip address
	_ipAddress->setColor(color);
	_ipAddress->setBig();
	_ipAddress->setBorderColor(color);
	_ipAddress->setText("IP-ADDRESS");
	_ipAddress->setVisible(false);

	_playerName->setColor(color);
	_playerName->setBig();
	_playerName->setBorderColor(color);
	_playerName->setText("Player");
	_playerName->setVisible(false);

	_btnMessage->setText(tr("DISCONNECT"));
	_btnMessage->setVisible(false);
	_btnMessage->onMouseClick((ActionHandler)&CoopMenu::disconnect);

	_btnChat->setText(tr("CHAT ('C')"));
	_btnChat->setVisible(false);
	_btnChat->onMouseClick((ActionHandler)&CoopMenu::btnChatClick);
	
	_tcpButtonJoin->setText("JOIN GAME");
	_tcpButtonJoin->onMouseClick((ActionHandler)&CoopMenu::joinTCPGame);
	_tcpButtonJoin->onKeyboardPress((ActionHandler)&CoopMenu::joinTCPGame, Options::keyOk);
	_tcpButtonJoin->setVisible(true);

	_tcpButtonHost->setText("HOST GAME");
	_tcpButtonHost->onMouseClick((ActionHandler)&CoopMenu::hostTCPGame);
	_tcpButtonHost->onKeyboardPress((ActionHandler)&CoopMenu::hostTCPGame, Options::keyOk);
	_tcpButtonHost->setVisible(true);

	_btnCancel->setText("CANCEL");
	_btnCancel->onMouseClick((ActionHandler)&CoopMenu::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&CoopMenu::btnCancelClick, Options::keyCancel);

	// game modes
	_btnPVE->setText("GAMEMODE: PVE");
	_btnPVE->onMouseClick((ActionHandler)&CoopMenu::btnPVEClick);
	_btnPVE->onKeyboardPress((ActionHandler)&CoopMenu::btnPVEClick, Options::keyCancel);
	_btnPVE->setVisible(false);

	_btnPVE2->setText("GAMEMODE: PVE2");
	_btnPVE2->onMouseClick((ActionHandler)&CoopMenu::btnPVE2Click);
	_btnPVE2->onKeyboardPress((ActionHandler)&CoopMenu::btnPVE2Click, Options::keyCancel);
	_btnPVE2->setVisible(false);

	_btnPVP->setText("GAMEMODE: PVP");
	_btnPVP->onMouseClick((ActionHandler)&CoopMenu::btnPVPClick);
	_btnPVP->onKeyboardPress((ActionHandler)&CoopMenu::btnPVPClick, Options::keyCancel);
	_btnPVP->setVisible(false);

	_btnPVP2->setText("GAMEMODE: PVP2");
	_btnPVP2->onMouseClick((ActionHandler)&CoopMenu::btnPVP2Click);
	_btnPVP2->onKeyboardPress((ActionHandler)&CoopMenu::btnPVP2Click, Options::keyCancel);
	_btnPVP2->setVisible(false);

	// check if campaign mission
	if (!_game->getSavedGame()->getCountries()->empty())
	{

		_game->getCoopMod()->setCoopCampaign(true);
	}
	else
	{

		_game->getCoopMod()->setCoopCampaign(false);
	}

	if ((_game->getCoopMod()->isConnected() == 1) || _game->getCoopMod()->getServerOwner() == true)
	{

		_hostPing->setVisible(true);
		_clientPing->setVisible(true);
		_btnMessage->setVisible(true);
		_btnChat->setVisible(true);
		_ipAddress->setVisible(false);
		_playerName->setVisible(false);
		_tcpButtonJoin->setVisible(false);
		_tcpButtonHost->setVisible(false);
		_txtInfo->setVisible(false);
		_btnPVE->setVisible(false);
		_btnPVE2->setVisible(false);
		_btnPVP->setVisible(false);
		_btnPVP2->setVisible(false);
	
	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_hostPing->setVisible(false);
		_clientPing->setVisible(false);
		_btnMessage->setVisible(false);
		_btnChat->setVisible(false);
		_ipAddress->setVisible(true);
		_playerName->setVisible(true);
		_tcpButtonJoin->setVisible(true);
		_tcpButtonHost->setVisible(true);
		_txtInfo->setVisible(false);

		_btnPVP->setVisible(false);
		_btnPVP2->setVisible(false);
		_btnPVE->setVisible(false);
		_btnPVE2->setVisible(false);


		if (_game->getCoopMod()->getCoopCampaign() == false && _game->getCoopMod()->getServerOwner() == false)
		{

			if (_game->getCoopMod()->getCoopGamemode() == 4)
			{
				_btnPVE2->setVisible(true);
			}
			else if (_game->getCoopMod()->getCoopGamemode() == 1 || _game->getCoopMod()->getCoopGamemode() == 0)
			{
				_btnPVE->setVisible(true);
			}
			else if (_game->getCoopMod()->getCoopGamemode() == 2)
			{
				_btnPVP->setVisible(true);
			}
			else if (_game->getCoopMod()->getCoopGamemode() == 3)
			{
				_btnPVP2->setVisible(true);
			}

		}



	}

	// READ IP ADDRESS

	// Name of the JSON file
	std::string filename = "ip_address.json";
	std::string filepath = Options::getMasterUserFolder() + filename;

	std::string ipAddress;
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
				playerName = root.get("name", "").asString();

				
				if (ipAddress.empty())
				{
					// ip is empty
					ipAddress = "IP-ADDRESS";
				}

				if (playerName == "")
				{
					// name is empty
					playerName = "Player";
				}

				_ipAddress->setText(ipAddress);
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
CoopMenu::~CoopMenu()
{
}

/**
 * Resets the menu music and savegame
 * when coming back from the battlescape.
 */
void CoopMenu::init()
{



	if ((_game->getCoopMod()->isConnected() == 1) || _game->getCoopMod()->getServerOwner() == true)
	{

		_hostPing->setVisible(true);
		_clientPing->setVisible(true);
		_btnMessage->setVisible(true);
		_btnChat->setVisible(true);
		_ipAddress->setVisible(false);
		_playerName->setVisible(false);
		_tcpButtonJoin->setVisible(false);
		_tcpButtonHost->setVisible(false);
		_txtInfo->setVisible(false);
		_btnPVE->setVisible(false);
		_btnPVE2->setVisible(false);
		_btnPVP->setVisible(false);
		_btnPVP2->setVisible(false);
	}
	else if (_game->getCoopMod()->isConnected() == -1)
	{
		_hostPing->setVisible(false);
		_clientPing->setVisible(false);
		_btnMessage->setVisible(false);
		_btnChat->setVisible(false);
		_ipAddress->setVisible(true);
		_playerName->setVisible(true);
		_tcpButtonJoin->setVisible(true);
		_tcpButtonHost->setVisible(true);
		_txtInfo->setVisible(false);

		_btnPVP->setVisible(false);
		_btnPVP2->setVisible(false);
		_btnPVE->setVisible(false);
		_btnPVE2->setVisible(false);

		if (_game->getCoopMod()->getCoopCampaign() == false && _game->getCoopMod()->getServerOwner() == false)
		{

			if (_game->getCoopMod()->getCoopGamemode() == 4)
			{
				_btnPVE2->setVisible(true);
			}
			else if (_game->getCoopMod()->getCoopGamemode() == 1 || _game->getCoopMod()->getCoopGamemode() == 0)
			{
				_btnPVE->setVisible(true);
			}
			else if (_game->getCoopMod()->getCoopGamemode() == 2)
			{
				_btnPVP->setVisible(true);
			}
			else if (_game->getCoopMod()->getCoopGamemode() == 3)
			{
				_btnPVP2->setVisible(true);
			}
		}
	}

	if (_game->getSavedGame()->getSavedBattle())
	{

		// check if already converted units...
		for (auto unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_PLAYER && unit->getCoop() == 1)
			{

					_btnPVE->setVisible(false);
					_btnPVE2->setVisible(false);
					_btnPVP->setVisible(false);
					_btnPVP2->setVisible(false);

					break;
			}
		}
	}

}

void CoopMenu::sendFileBase(int state)
{


	if (_game->getCoopMod()->getHost() == false)
	{
		// saving is not allowed if in battle and inside another player's base!
		if (!_game->getSavedGame()->getSavedBattle() && _game->getCoopMod()->playerInsideCoopBase == false)
		{

			if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->save("host/basehost.data", _game->getMod());
				_game->pushState(new SaveGameState(OPT_MENU, "host/basehost.data", _palette));
				_game->popState();
			}
			else if (_game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->save("client/basehost.data", _game->getMod());
				_game->pushState(new SaveGameState(OPT_MENU, "client/basehost.data", _palette));
				_game->popState();
			}


		}

	}
	else
	{

		// do not allow saving if in battle and inside another player's base!
		if (!_game->getSavedGame()->getSavedBattle() && _game->getCoopMod()->playerInsideCoopBase == false)
		{

			if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->save("host/basehost.data", _game->getMod());
				_game->pushState(new SaveGameState(OPT_MENU, "host/basehost.data", _palette));
				_game->popState();
			}
			else if (_game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->save("client/basehost.data", _game->getMod());
				_game->pushState(new SaveGameState(OPT_MENU, "client/basehost.data", _palette));
				_game->popState();
			}

		}

	}


	
}

void CoopMenu::btnPVEClick(Action *action)
{

	_btnPVE->setVisible(false);
	_btnPVE2->setVisible(true);

	current_gamemode = 4;

}

void CoopMenu::btnPVE2Click(Action* action)
{

	_btnPVE2->setVisible(false);
	_btnPVP->setVisible(true);

	current_gamemode = 2;

}

void CoopMenu::btnPVP2Click(Action *action)
{

	_btnPVP2->setVisible(false);
	_btnPVE->setVisible(true);

	current_gamemode = 1;

}

void CoopMenu::btnChatClick(Action* action)
{

	if (_game->getCoopMod()->getChatMenu())
	{

		_game->getCoopMod()->getChatMenu()->setActive(!_game->getCoopMod()->getChatMenu()->isActive());

	}

}

void CoopMenu::btnPVPClick(Action *action)
{

	_btnPVP->setVisible(false);
	_btnPVP2->setVisible(true);

	current_gamemode = 3;

}

void CoopMenu::showGamemode()
{

	if ((_game->getCoopMod()->isConnected() == -1) && _game->getCoopMod()->getServerOwner() == false)
	{
		_btnPVE->setVisible(true);
	}

	_btnPVP->setVisible(false);
	_btnPVP2->setVisible(false);
	_btnPVE2->setVisible(false);

}

int CoopMenu::getGameMode()
{

	return _game->getCoopMod()->getCoopGamemode();
}

void CoopMenu::sendFile()
{

	// Client sends the file to the host
	if (_game->getCoopMod()->getHost() == false)
	{

		if ((_game->getCoopMod()->playerInsideCoopBase == true || _game->getCoopMod()->coopMissionEnd == true) && _game->getCoopMod()->getCoopCampaign() == true)
		{

			// Go to Geoscape to begin the co-op mission.
			_game->getCoopMod()->playerInsideCoopBase = false;

			_game->getCoopMod()->ready_coop_battle = true;

			_game->popState();

			CoopState *coopWindow = new CoopState(66);
			_game->pushState(coopWindow);

		}
		else
		{

			// saving files
			if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
			{

				_game->getSavedGame()->save("host/battlehost.data", _game->getMod());
			}
			else if (_game->getCoopMod()->coopMissionEnd == false)
			{

				_game->getSavedGame()->save("client/battlehost.data", _game->getMod());

			}


			Json::Value obj;
			obj["state"] = "SEND_FILE_HOST_TRUE";

			_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());



		}


	}
	// Host sends the file to the client
	else
	{


		if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->save("host/battlehost.data", _game->getMod());
		}
		else if (_game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->save("client/battlehost.data", _game->getMod());
		}

		Json::Value obj;
		obj["state"] = "SEND_FILE_CLIENT_TRUE";

		_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());


	}

	
}

void CoopMenu::joinTCPGame(Action *action)
{

	// coop chat menu
	if (!_game->getCoopMod()->getChatMenu())
	{
		Font* smallFont = _game->getMod()->getFont("FONT_SMALL");
		_game->getCoopMod()->setChatMenu(new ChatMenu(smallFont, _game));
	}

	// JOIN GAME
	_game->getCoopMod()->setCoopSession(false);

	_game->getCoopMod()->setPlayerTurn(0);

	// Convert single-player save to multiplayer save (PvE)
	if (_game->getCoopMod()->getCoopGamemode() == 0 || _game->getCoopMod()->getCoopGamemode() == 1)
	{

		_game->getCoopMod()->setPlayerTurn(3);

		// Split the soldiers in half
		if (_game->getSavedGame()->getSavedBattle())
		{

			_game->getSavedGame()->getSavedBattle()->getBattleState()->setCurrentTurn(3);

			int soldier_total_count = 0;

			// check soldiers count
			for (auto &entity : *_game->getSavedGame()->getSavedBattle()->getUnits())
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
	else if (_game->getSavedGame()->getCoop()->getGameMode() == 4 && _game->getSavedGame()->getSavedBattle())
	{

		// swapper
		_game->getCoopMod()->setPlayerTurn(3);

		for (auto &unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_HOSTILE)
			{

				unit->convertToFaction(FACTION_PLAYER);
				unit->setOriginalFaction(FACTION_PLAYER);
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

			_game->getSavedGame()->getSavedBattle()->getBattleState()->setCurrentTurn(3);

			int soldier_total_count = 0;

			// check soldiers count
			for (auto &entity : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				if (entity->getFaction() == FACTION_PLAYER)
				{
					soldier_total_count++;
				}
			}

			int soldier_used = (soldier_total_count / 2);

			// make coop soldiers
			for (auto &unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
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
	else if (_game->getSavedGame()->getCoop()->getGameMode() == 2 && _game->getSavedGame()->getSavedBattle())
	{

		_game->getCoopMod()->setPlayerTurn(3);

		for (auto &unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_HOSTILE)
			{

				unit->setCoop(1);
				unit->convertToFaction(FACTION_PLAYER);
				unit->setOriginalFaction(FACTION_PLAYER);
			}
		}
	}
	// pvp2
	else if (_game->getSavedGame()->getCoop()->getGameMode() == 3 && _game->getSavedGame()->getSavedBattle())
	{

		_game->getCoopMod()->setPlayerTurn(3);

		for (auto &unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_HOSTILE)
			{

				unit->setCoop(0);
				unit->convertToFaction(FACTION_PLAYER);
				unit->setOriginalFaction(FACTION_PLAYER);
			}
			else if (unit->getFaction() == FACTION_PLAYER)
			{

				unit->setCoop(1);
			}
		}
	}

	_game->pushState(new CoopState(15));

	_game->getCoopMod()->connectTCPServer(_playerName->getText(), _ipAddress->getText());
}

void CoopMenu::hostTCPGame(Action *action)
{

	// coop chat menu
	if (!_game->getCoopMod()->getChatMenu())
	{
		Font* smallFont = _game->getMod()->getFont("FONT_SMALL");
		_game->getCoopMod()->setChatMenu(new ChatMenu(smallFont, _game));
	}

	connectionTCP::_coopGamemode = current_gamemode;

	_game->getCoopMod()->setCoopSession(false);

	std::string name = _playerName->getText();
	std::string ping = "0";
	_hostPing->setText("Player: " + name + " | Latency: " + ping + " ms");

	_hostPing->setVisible(true);
	_btnMessage->setVisible(true);
	_btnChat->setVisible(true);
	_ipAddress->setVisible(false);
	_playerName->setVisible(false);
	_tcpButtonJoin->setVisible(false);
	_tcpButtonHost->setVisible(false);

	_btnPVP->setVisible(false);
	_btnPVP2->setVisible(false);
	_btnPVE->setVisible(false);
	_btnPVE2->setVisible(false);

	_game->getCoopMod()->setPlayerTurn(0);

	bool convert = true;

	if (_game->getSavedGame()->getSavedBattle())
	{

		// check if already converted units...
		for (auto unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_PLAYER && unit->getCoop() == 1)
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

	
	// Convert single-player save to multiplayer save (PvE)
	if (_game->getCoopMod()->getCoopGamemode() == 0 || _game->getCoopMod()->getCoopGamemode() == 1)
	{

		_game->getCoopMod()->setPlayerTurn(3);

		connectionTCP::_coopGamemode = 1;

		// Split the soldiers in half
		if (_game->getSavedGame()->getSavedBattle() && convert == true)
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
	// if pvp gamemode
	else if (_game->getSavedGame()->getCoop()->getGameMode() == 2 && _game->getSavedGame()->getSavedBattle())
	{

		_game->getCoopMod()->setPlayerTurn(3);

		if (convert == true)
		{

			for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				if (unit->getFaction() == FACTION_HOSTILE)
				{

					unit->setCoop(1);
					unit->convertToFaction(FACTION_PLAYER);
					unit->setOriginalFaction(FACTION_PLAYER);
				}
			}

		}



	}
	// pvp2
	else if (_game->getSavedGame()->getCoop()->getGameMode() == 3 && _game->getSavedGame()->getSavedBattle())
	{

		_game->getCoopMod()->setPlayerTurn(3);

		if (convert == true)
		{

			for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{

				if (unit->getFaction() == FACTION_HOSTILE)
				{

					unit->setCoop(0);
					unit->convertToFaction(FACTION_PLAYER);
					unit->setOriginalFaction(FACTION_PLAYER);
				}
				else if (unit->getFaction() == FACTION_PLAYER)
				{

					unit->setCoop(1);
				}
			}

		}


	}


	// HOST GAME
	_game->getCoopMod()->hostTCPServer(_playerName->getText(), _ipAddress->getText());
}

// DISCONNECT BUTTON
void CoopMenu::disconnect(Action *action)
{

	_hostPing->setVisible(false);
	_clientPing->setVisible(false);
	_btnMessage->setVisible(false);
	_btnChat->setVisible(false);
	_tcpButtonHost->setVisible(true);
	_tcpButtonJoin->setVisible(true);
	_ipAddress->setVisible(true);
	_playerName->setVisible(true);

	if (_game->getCoopMod()->getCoopCampaign() == false)
	{
		_btnPVE->setVisible(true);
	}

	_btnPVP->setVisible(false);
	_btnPVP2->setVisible(false);
	_btnPVE2->setVisible(false);

	_game->getCoopMod()->disconnectTCP();

	

	if (_game->getSavedGame()->getSavedBattle())
	{

		// check if already converted units...
		for (auto unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
		{

			if (unit->getFaction() == FACTION_PLAYER && unit->getCoop() == 1)
			{

				_btnPVE->setVisible(false);
				_btnPVE2->setVisible(false);
				_btnPVP->setVisible(false);
				_btnPVP2->setVisible(false);

				break;
			}
		}
	}

}


/**
 * Returns to the previous screen.
 * @param action Pointer to an action.
 */
void CoopMenu::btnCancelClick(Action *)
{

	_game->popState();


}

void CoopMenu::think()
{

	State::think();

	static Uint32 lastUpdate = 0;
	static Uint32 pingSentTime = 0;
	Uint32 now = SDL_GetTicks();

	if (_game->getCoopMod()->isCoopSession() == false || _game->getCoopMod()->getCoopStatic() == false)
	{
		_clientPing->setVisible(false);
	}

	if (_game->getCoopMod()->isCoopSession() == true && _game->getCoopMod()->getCoopStatic() == true && now - lastUpdate >= 1000 && _game->getCoopMod()->getChatMenu())
	{

		_clientPing->setVisible(true);
		
		std::string host_name = _game->getCoopMod()->getHostName();
		_hostPing->setText("Player: " + host_name + " | Latency: " + "0" + " ms");

		std::string name = _game->getCoopMod()->getCurrentClientName();
		std::string ping = _game->getCoopMod()->getPing();

		_clientPing->setText("Player: " + name + " | Latency: " + ping + " ms");

		lastUpdate = now;

	}

}

}

