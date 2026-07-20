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
#include "../Geoscape/GeoscapeState.h"
#include "../Geoscape/Globe.h"
#include "../Geoscape/BaseNameState.h"
#include "../Geoscape/BuildNewBaseState.h"
#include "../Basescape/PlaceLiftState.h"
#include "../Menu/NewGameState.h"
#include "../Savegame/Base.h"

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

	// Playtest B7: detect the "opened mid-game" case BEFORE markLobbyOpen flips the
	// lobby flag. Two conditions must BOTH hold, or the pre-game campaign lobby (which
	// also has a paused GeoscapeState underneath, created by NewGameState before the
	// lobby) would wrongly show RESUME GAME:
	//   1. the campaign has actually started (sessionLocked) - false in the pre-game
	//      lobby, true once START/RESUME/campaign_start locked the session; and
	//   2. a running-game geoscape sits on the stack underneath to return to.
	if (connectionTCP::session.sessionLocked)
	{
		for (auto* s : _game->getStates())
		{
			if (dynamic_cast<GeoscapeState*>(s)) { _resumeToGame = true; break; }
		}
	}

	connectionTCP::session.markLobbyOpen();

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

	// Campaign lobbies are host-driven: clients have no ready/start button
	// at all (flow-redesign D4)
	if (connectionTCP::session.lobbyMode != 0)
	{
		if (_game->getCoopMod()->getServerOwner() == true)
		{
			_btnCancel->setText(connectionTCP::session.lobbyMode == 1 ? "START CAMPAIGN" : "RESUME CAMPAIGN");
			_btnCancel->setVisible(false); // shown once eligible (think())
		}
		else
		{
			_btnCancel->setVisible(false);
		}
	}

	// Playtest B7: opened mid-game -> RESUME GAME for every player (host and client),
	// overriding the lobby gating above. think() re-asserts this each tick.
	if (_resumeToGame)
	{
		_btnCancel->setText("RESUME GAME");
		_btnCancel->setVisible(true);
	}

	_txtTitle->setBig();
	_txtTitle->setAlign(ALIGN_CENTER);

	std::string lobby_title = _game->getCoopMod()->getCurrentClientServer();

	if (_game->getCoopMod()->getServerOwner() == true)
	{
		lobby_title = _game->getCoopMod()->getHostServer();
	}

	// PRD-J01: read-only campaign economy model so a client sees what it is
	// joining before the roster locks. Host reads its own save; a client (no
	// save yet) uses the type carried in the join handshake.
	if (connectionTCP::session.lobbyMode != 0)
	{
		int ct = connectionTCP::_lobbyCampaignType;
		if (_game->getCoopMod()->getServerOwner() == true && _game->getSavedGame())
		{
			ct = static_cast<int>(_game->getSavedGame()->getCampaignType());
		}
		lobby_title += (ct == static_cast<int>(CoopCampaignType::Joint)) ? "  [JOINT]" : "  [SEPARATE]";
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

	if (connectionTCP::session.sessionLocked == true)
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
/**
 * Confirmation dialog for START CAMPAIGN: the player set and teams are
 * locked once the campaign begins (flow-redesign D3). File-local.
 */
class ConfirmStartCampaignState : public State
{
private:
	LobbyMenu *_lobby;
	Window *_window;
	Text *_txtMessage;
	TextButton *_btnOk, *_btnCancel;
public:
	ConfirmStartCampaignState(LobbyMenu *lobby) : _lobby(lobby)
	{
		_screen = false;

		_window = new Window(this, 256, 100, 32, 50, POPUP_BOTH);
		_txtMessage = new Text(240, 48, 40, 60);
		_btnOk = new TextButton(100, 16, 44, 124);
		_btnCancel = new TextButton(100, 16, 176, 124);

		setInterface("geoscape", true, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

		add(_window, "window", "saveMenus");
		add(_txtMessage, "text", "saveMenus");
		add(_btnOk, "button", "saveMenus");
		add(_btnCancel, "button", "saveMenus");

		centerAllSurfaces();

		setWindowBackground(_window, "saveMenus");

		_txtMessage->setAlign(ALIGN_CENTER);
		_txtMessage->setWordWrap(true);
		_txtMessage->setText("Once the campaign starts, the number and names of the players cannot be changed. Are you sure you want to start?");

		_btnOk->setText("OK");
		_btnOk->onMouseClick((ActionHandler)&ConfirmStartCampaignState::btnOkClick);
		_btnCancel->setText("CANCEL");
		_btnCancel->onMouseClick((ActionHandler)&ConfirmStartCampaignState::btnCancelClick);
		_btnCancel->onKeyboardPress((ActionHandler)&ConfirmStartCampaignState::btnCancelClick, Options::keyCancel);
	}

	void btnOkClick(Action *)
	{
		LobbyMenu *lobby = _lobby;
		_game->popState(); // this dialog
		// PRD-10 C4: re-check at the commit point - a client may have dropped
		// while this dialog was open. If so, do NOT start; the lobby underneath
		// already shows the not-eligible state (START button hidden, the ctor's
		// "Waiting for players on port X" details). startCampaign() also guards
		// defensively, but bail here so no side effect is even attempted.
		if (!lobby->startEligible())
		{
			Log(LOG_INFO) << "[coop] START CAMPAIGN aborted: lobby no longer eligible at confirm time";
			return;
		}
		lobby->startCampaign();
	}

	void btnCancelClick(Action *)
	{
		_game->popState(); // back to the lobby
	}

	// PRD-10 C4: nothing else pops this dialog when a client drops (only the top
	// state's think() runs, so the lobby underneath is dormant). Watch
	// eligibility here and dismiss ourselves the moment the lone client leaves,
	// so the host never confirms a start into a departed roster. btnOkClick
	// re-checks too, covering the click-before-this-fires race.
	void think() override
	{
		State::think();
		if (!_lobby->startEligible())
		{
			Log(LOG_INFO) << "[coop] START CAMPAIGN confirm dialog dismissed: client left the lobby";
			_game->popState();
		}
	}
};

bool LobbyMenu::startEligible() const
{
	// >= 1 non-host client attached (D3); N=1 transport today. NOT
	// getCoopStatic(): onConnect==1 merely means the host is listening.
	return connectionTCP::session.clientInLobby;
}

std::string LobbyMenu::actionButtonText() const
{
	return _btnCancel->getText();
}

bool LobbyMenu::actionButtonVisible() const
{
	return _btnCancel->getVisible();
}

std::string LobbyMenu::detailsText() const
{
	return _txtDetails->getText();
}

std::vector<std::string> LobbyMenu::rosterNames() const
{
	std::vector<std::string> names;
	for (const auto &p : _connectedPlayers)
	{
		names.push_back(p.name);
	}
	return names;
}

std::vector<std::string> LobbyMenu::missingPlayers() const
{
	// registered players (minus the host) not currently connected
	std::vector<std::string> missing;
	if (!_game->getSavedGame())
	{
		return missing;
	}
	std::string hostName = _game->getCoopMod()->getHostName();
	std::string connectedClient = connectionTCP::session.clientInLobby
									  ? _game->getCoopMod()->getCurrentClientName()
									  : "";
	for (const auto &p : _game->getSavedGame()->getCoopPlayers())
	{
		if (p != hostName && p != connectedClient)
		{
			missing.push_back(p);
		}
	}
	return missing;
}

void LobbyMenu::resumeCampaign()
{

	connectionTCP::session.campaignStarted();
	// battle save: after the geoscape world ack, stream the battle (2c)
	connectionTCP::session.armResumeHandshake(_game->getSavedGame()->getSavedBattle() != nullptr);

	// Serve the connected client its world: stored blob if we have one,
	// otherwise a fresh world + base building (registered-no-blob, D6).
	// PRD-J02: JOINT has no per-client stored blob - there is a single
	// authoritative world. Always take the campaign_resume path: the client's
	// request_load_progress makes the host serialize the CURRENT world fresh and
	// stream it (streamJointWorldToClient). Never build a fresh client world.
	std::string clientName = _game->getCoopMod()->getCurrentClientName();
	if (_game->getCoopMod()->isJointCampaign()
		|| connectionTCP::hasCoopFile(connectionTCP::hostBlobKey(clientName)))
	{
		Json::Value root;
		root["state"] = "campaign_resume";
		_game->getCoopMod()->sendTCPPacketData(root.toStyledString());
	}
	else
	{
		Json::Value root = connectionTCP::buildCampaignStartPacket(_game->getSavedGame());
		_game->getCoopMod()->sendTCPPacketData(root.toStyledString());
	}

	// close the lobby (popping anything stacked above it) and hold until
	// every player reports its world loaded
	closeLobby();

	_game->pushState(new CoopState(COOP_DLG_RESUME_ACK_WAIT));

}

void LobbyMenu::openStartConfirmDialog()
{
	_game->pushState(new ConfirmStartCampaignState(this));
}

/**
 * Playtest B7: return from the mid-game coop menu to the running game. The lobby was
 * opened over a live campaign geoscape; the connection and shared world stay up, so
 * just pop the coop-menu states (this lobby + the pause menu it came from) back down
 * to that geoscape. Restores the lobby-closed flag that this menu's ctor cleared.
 */
void LobbyMenu::returnToRunningGame()
{
	connectionTCP::session.markLobbyClosed();
	// Pop every state above the running geoscape (bounded by the stack depth).
	int guard = 0;
	while (guard++ < 32 && _game->getStates().size() > 1
		&& !dynamic_cast<GeoscapeState*>(_game->getStates().back()))
	{
		_game->popState();
	}
}

bool LobbyMenu::clickStartConfirmOk()
{
	if (_game->getStates().empty())
	{
		return false;
	}
	ConfirmStartCampaignState *dlg = dynamic_cast<ConfirmStartCampaignState *>(_game->getStates().back());
	if (!dlg)
	{
		return false;
	}
	dlg->btnOkClick(nullptr); // the real click path: pop + re-check + maybe start
	return true;
}

void LobbyMenu::startCampaign()
{

	// PRD-10 C4: never start with a departed roster. Every UI path re-checks
	// before reaching here, but guard defensively for any direct caller (the
	// harness lobby_start_campaign command calls this). No side effects on refusal.
	if (!startEligible())
	{
		Log(LOG_INFO) << "[coop] startCampaign refused: lobby no longer start-eligible (client left)";
		return;
	}

	// lock players and teams (change_team refuses while locked)
	connectionTCP::session.campaignStarted();

	// A NEW campaign always mints a fresh saveID and starts with no world blobs
	// (fixes C2: a second campaign in the same process must not reuse the first
	// campaign's ID or serve its stale client world). resumeCampaign keeps the
	// loaded ID. The campaign_start packet built below carries this fresh ID and
	// the client adopts it (connectionTCP campaign_start handler); the join-time
	// mint (connectionTCP ~7149) remains only as a handshake fallback.
	connectionTCP::saveID = _game->getCoopMod()->getDateTimeCoop();
	{
		std::lock_guard<std::mutex> lock(connectionTCP::coopFilesMutex);
		connectionTCP::coopFilesHost.clear();
		connectionTCP::coopFilesClient.clear();
	}

	// the locked player list, host first (D4/D6)
	std::vector<std::string> players;
	players.push_back(_game->getCoopMod()->getHostName());
	players.push_back(_game->getCoopMod()->getCurrentClientName());
	_game->getSavedGame()->setCoopPlayers(players);
	_game->getSavedGame()->setCoopSave(true);

	// initial save right at START so a crash during base building resumes
	// into base placement (D6). Direct write: the funnel's client-progress
	// pull has nothing to pull yet.
	try
	{
		_game->getSavedGame()->save("_autogeo_.asav", _game->getMod());
	}
	catch (const std::exception &e)
	{
		Log(LOG_ERROR) << "[coop] initial campaign save failed: " << e.what();
	}

	// clients: create their worlds and start base building
	Json::Value root = connectionTCP::buildCampaignStartPacket(_game->getSavedGame());
	_game->getCoopMod()->sendTCPPacketData(root.toStyledString());

	// host: close the lobby (popping whatever sits above it, e.g. the
	// Profile pushed by the join handshake) and place the first base
	closeLobby();

	GeoscapeState *gs = nullptr;
	for (auto *s : _game->getStates())
	{
		if (auto *g = dynamic_cast<GeoscapeState *>(s))
		{
			gs = g;
		}
	}

	beginInitialBasePlacement(_game, gs, _game->getSavedGame()->getBases()->back());

}

void LobbyMenu::btnCancelClick(Action*)
{

	// Playtest B7: opened mid-game -> the button is RESUME GAME. The shared world is
	// already live on every machine, so just return to it (no re-handshake), for the
	// host and the client alike. Handled before any lobby-start logic.
	if (_resumeToGame)
	{
		returnToRunningGame();
		return;
	}

	// campaign lobby: the button is START/RESUME CAMPAIGN, host only (D3/D4)
	if (connectionTCP::session.lobbyMode != 0)
	{
		if (connectionTCP::session.lobbyMode == 1
			&& _game->getCoopMod()->getServerOwner() == true
			&& connectionTCP::session.sessionLocked == false
			&& startEligible())
		{
			_game->pushState(new ConfirmStartCampaignState(this));
		}
		else if (connectionTCP::session.lobbyMode == 2
			&& _game->getCoopMod()->getServerOwner() == true
			&& missingPlayers().empty()
			&& startEligible())
		{
			resumeCampaign();
		}
		return;
	}

	connectionTCP::isPlayerReady = !connectionTCP::isPlayerReady;

	if (connectionTCP::session.sessionLocked == false && _game->getCoopMod()->getCoopStatic() == true)
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
			connectionTCP::session.campaignStarted();
		}

	}
	else if (_game->getCoopMod()->getCoopStatic() == true && _game->getCoopMod()->isCoopSession() == true)
	{
		connectionTCP::session.markLobbyClosed();
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
			connectionTCP::session.markLobbyClosed();
			_game->popState();
			_game->getCoopMod()->disconnectTCP(true);
		}

	}

}

void LobbyMenu::pushServerListUnlessPresent()
{
	// ServerList's ctor dereferences the SavedGame (getCountries()); a fresh
	// client that never received a world (F2 lobby, mid-join rejoin) has none,
	// so pushing the browser would crash - land back on the state underneath.
	if (!_game->getSavedGame())
	{
		return;
	}
	for (auto* s : _game->getStates())
	{
		if (dynamic_cast<ServerList*>(s) != nullptr)
		{
			return; // the browser we came from is already underneath
		}
	}
	_game->pushState(new ServerList());
}

void LobbyMenu::closeLobby()
{
	// pop everything above the lobby, then the lobby itself; the world/geoscape
	// beneath survives and the session records that the lobby is gone.
	connectionTCP::session.markLobbyClosed();
	while (!_game->getStates().empty())
	{
		bool isLobby = dynamic_cast<LobbyMenu*>(_game->getStates().back()) != nullptr;
		_game->popState();
		if (isLobby)
		{
			break;
		}
	}
}

void LobbyMenu::btnDisconnectClick(Action* action)
{

	connectionTCP::session.markLobbyClosed();

	_game->popState();

	// Disconnect BEFORE dropping the role: the teardown classifies the
	// machine from session.role, and falsifying it first made the cleanup run
	// the client path on a host (the disconnect->cancel bug family).
	// If the host presses the disconnect button, disconnect and return to the Host menu
	if (_game->getCoopMod()->getCoopCampaign() == true && _game->getCoopMod()->getServerOwner() == true)
	{
		_game->getCoopMod()->disconnectTCP(true);
		_game->getCoopMod()->setServerOwner(false);
		_game->pushState(new HostMenu());
	}
	// If the client presses Disconnect, disconnect and return to the main menu
	else if (_game->getCoopMod()->getCoopCampaign() == true && _game->getCoopMod()->getServerOwner() == false)
	{
		_game->getCoopMod()->disconnectTCP();
		_game->getCoopMod()->setServerOwner(false);
		pushServerListUnlessPresent();
	}
	// Otherwise, do nothing and just return to the server list
	else
	{
		// Prevent returning to the main menu
		_game->getCoopMod()->disconnectTCP(true);
		_game->getCoopMod()->setServerOwner(false);
		pushServerListUnlessPresent();
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
	if (_game->getCoopMod()->getServerOwner() == true && action->getDetails()->button.button == SDL_BUTTON_LEFT && connectionTCP::session.sessionLocked == false)
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

		// Connection gone: leave the lobby ONCE, and reuse a server browser
		// already underneath instead of stacking a fresh one on top of it
		// (repeated pushes made CANCEL act on a stale browser).
		if (!_redirected && !_game->getStates().empty() && _game->getStates().back() == this)
		{
			_redirected = true;

			_game->popState();

			pushServerListUnlessPresent();
		}

		return;

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

		if (connectionTCP::session.lobbyMode != 0)
		{
			// Campaign lobby: host-driven, no ready dance. The host's button
			// appears once starting is possible (D3/D4).
			if (_game->getCoopMod()->getServerOwner() == true && connectionTCP::session.sessionLocked == false)
			{
				if (connectionTCP::session.lobbyMode == 1)
				{
					_btnCancel->setText("START CAMPAIGN");
					_btnCancel->setVisible(startEligible());
				}
				else if (connectionTCP::session.lobbyMode == 2)
				{
					// resume gate: all registered players must be present
					std::vector<std::string> missing = missingPlayers();
					if (missing.empty())
					{
						_btnCancel->setText("RESUME CAMPAIGN");
						_btnCancel->setVisible(startEligible());
						_txtDetails->setText(tr("STR_DETAILS").arg("All players connected"));
					}
					else
					{
						_btnCancel->setVisible(false);
						// merged form: names AND port (the ctor's generic
						// "Waiting for players on port X" covers the no-names case)
						std::string wait = "Waiting for ";
						for (size_t i = 0; i < missing.size(); ++i)
						{
							if (i > 0)
							{
								wait += ", ";
							}
							wait += missing[i];
						}
						wait += " on port " + std::to_string(tcp_port);
						_txtDetails->setText(tr("STR_DETAILS").arg(wait));
					}
				}
			}
		}
		else if (connectionTCP::session.sessionLocked == false)
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
				connectionTCP::session.campaignStarted();

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

		// Playtest B7: mid-game coop menu -> keep RESUME GAME asserted over whatever
		// the lobby button logic above chose.
		if (_resumeToGame)
		{
			_btnCancel->setText("RESUME GAME");
			_btnCancel->setVisible(true);
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
		
		// other players. Machine-aware presence: the HOST shows a peer only
		// once it passed every join gate (clientInLobby - a refused wrong-name
		// attempt must never flash into the roster); the CLIENT shows its
		// host as soon as it is attached (coopStatic).
		bool peerPresent = _game->getCoopMod()->getServerOwner()
							   ? connectionTCP::session.clientInLobby
							   : _game->getCoopMod()->getCoopStatic() == true;
		if (peerPresent
			&& (connectionTCP::session.lobbyMode != 0
				|| _game->getCoopMod()->isCoopSession() == true))
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

			if (connectionTCP::session.lobbyMode == 0 && connectionTCP::session.sessionLocked == false)
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
