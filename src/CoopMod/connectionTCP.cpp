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

#include "connectionTCP.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>
#include <unordered_set>

#include "../Engine/Game.h"
#include "../Menu/MainMenuState.h"

#include "../Basescape/CraftSoldiersState.h"
#include "../Mod/AlienDeployment.h"
#include "../Menu/CutsceneState.h"

#include "../Savegame/AlienMission.h"
#include "../Mod/UfoTrajectory.h"
#include "../Savegame/Ufo.h"
#include "../Battlescape/DebriefingState.h"
#include "../Battlescape/BattlescapeState.h"

#include "../Savegame/Country.h"
#include "../Mod/RuleCountry.h"
#include "../Mod/RuleRegion.h"
#include "../Savegame/Region.h"

#include "../Mod/RuleCraftWeapon.h"
#include "../Savegame/Craft.h"
#include "../Savegame/CraftWeapon.h"

#include "../Menu/NewGameState.h"
#include "../Menu/LoadGameState.h"
#include "../Geoscape/GeoscapeState.h"
#include "../Geoscape/Globe.h"
#include "../Geoscape/BaseNameState.h"
#include "../Geoscape/BuildNewBaseState.h"
#include "../Basescape/PlaceLiftState.h"

#include "./connectionUDP/connection_rendezvous_glue.h"

#include "PasswordCheckMenu.h"
#include "ModCheckMenu.h"
#include "GiftNoticeState.h"
#include "SharedEcon.h"
#include "connectionUDP/connection_udp_glue.h"

#include "../Savegame/BaseFacility.h"
#include "../Engine/Logger.h"
#include "../Engine/Yaml.h"
#include "../Mod/Mod.h"
#include "../Savegame/Base.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/Transfer.h"

namespace OpenXcom
{

// COOP VARIABLES
// is the session created?
bool coopSession = false;
// allow sending a file to the client
bool sendFileClient = false;
// is the file to be sent a base?
bool sendFileBase = false;
// allow sending a file to the host
bool sendFileHost = false;
// allow sending a file to the host
bool sendProgressSaveFileToHost = false;
std::string sendProgressLoadFileToClient = "";
// Snapshot of the resume blob, copied on the main thread when a
// request_load_progress arrives so the streamer thread never touches the
// shared blob maps for it.
std::string sendProgressLoadBlob = "";
// is the file to be sent a saved file?
bool sendFileSave = false;
// map data
std::string mapData = "";
// how much space does the host have in the craft?
int _hostSpace;

ConfirmLandingState* _landing;
NewBattleState* _battleState;
GeoscapeState* _geo;
Craft* _selectedCraft;
Pathfinding* _selectedPath;

// ip address
std::string ipAddress = "";

// port (default: 3000)
int tcp_port = 3000;

// is it the host?
bool onTcpHost = false;

// is the server owner the one who creates the server?

// the local server name
std::string sendTcpServerName = "Server";

// the recipient player's name
std::string tcpServerName = "Server";

// the local player's name
std::string sendTcpPlayer = "Player";

// the recipient player's name
std::string tcpPlayerName = "Player";

int onConnect = -1; // -1 = connect lost, 0 = client cant connect, -2 = disconnect, 1 = connected, -3 = server error, 2 = waiting for player

bool clearPackets = false;

// trigger the event once
bool onceTime = false;

// base markers
std::string j_markers;

// has the map data arrived?
bool isWaitMap = true;

// trading
Json::Value waitedTrades;

int connectionTCP::_coopGamemode = 0;

int connectionTCP::coop_save_owner_player_id = 0; 

// PRD-J01: economy model shown in the lobby to a joining client (0=Sep,1=Shared).
int connectionTCP::_lobbyCampaignType = 0;

// PRD-J01: set once in the ctor; lets the static seat accessors reach the roster.
Game* connectionTCP::_staticGame = nullptr;

bool connectionTCP::_isChatActiveStatic = false;

bool connectionTCP::_isActiveAISync = false;

bool connectionTCP::_isActivePlayerSync = false; 

bool connectionTCP::_enable_time_sync = true;

bool connectionTCP::_enable_reaction_shoot = true;

bool connectionTCP::_enable_other_player_footsteps = true;

bool connectionTCP::_enable_host_only_time_speed = false;

bool connectionTCP::_enable_xcom_equipment_aliens_pvp = true;

bool connectionTCP::_unbalanced_craft_soldiers_limit = false;

bool connectionTCP::_coopCampaign = false;

bool connectionTCP::_battleInit = false;

bool connectionTCP::playerInsideCoopBase = false;

bool connectionTCP::coopInventory = false;

bool connectionTCP::moveCoopItems = false;

bool connectionTCP::no_bases = false;

bool connectionTCP::isCoopBaseLoading = false;

bool connectionTCP::_isHotseatActive = false;

bool connectionTCP::_isHotseatReactionFireEnabled = false;

bool connectionTCP::show_inactive_player_inventory = false;

bool connectionTCP::pauseSound = false;

bool connectionTCP::saveError = false;

CoopSession connectionTCP::session;

std::string connectionTCP::joinRefusalReason = "";
std::string connectionTCP::sharedFailReason = "";

// --- CoopSession transitions: every lifecycle change is logged. The mirrored
// --- booleans ARE the encoding (PRD-12 S4 deleted the write-only phase enum);
// --- every multi-field / cross-file write funnels through a named method here.

void CoopSession::beginHosting()
{
	role = CoopRole::Host;
	Log(LOG_INFO) << "[coop-session] beginHosting (role=Host)";
}

void CoopSession::beginJoining()
{
	role = CoopRole::Client;
	Log(LOG_INFO) << "[coop-session] beginJoining (role=Client)";
}

void CoopSession::clientAttached()
{
	clientInLobby = true;
	Log(LOG_INFO) << "[coop-session] clientAttached (clientInLobby=1)";
}

void CoopSession::campaignStarted()
{
	sessionLocked = true;
	Log(LOG_INFO) << "[coop-session] campaignStarted (sessionLocked=1)";
}

void CoopSession::sessionLive()
{
	// waiting dialogs released; play begins/resumes. No boolean of its own -
	// the surrounding flow already set lobbyClosed/campaignBegun; this is the
	// lifecycle marker in the log.
	Log(LOG_INFO) << "[coop-session] sessionLive";
}

void CoopSession::freeze()
{
	// a registered player dropped mid-session (D5). The freeze dialog + the
	// preserved lobby/campaign booleans carry the state; this marks it in the log.
	Log(LOG_INFO) << "[coop-session] freeze (lobbyMode=" << lobbyMode
		<< " locked=" << sessionLocked << ")";
}

void CoopSession::setRole(CoopRole r)
{
	role = r;
	Log(LOG_INFO) << "[coop-session] setRole -> "
		<< (r == CoopRole::Host ? "Host" : r == CoopRole::Client ? "Client" : "None");
}

void CoopSession::adoptResumeSave()
{
	lobbyMode = 2;
	sessionLocked = false;
	resumeAck = false;
	Log(LOG_INFO) << "[coop-session] adoptResumeSave (lobbyMode=2, unlocked, ack cleared)";
}

void CoopSession::armResumeHandshake(bool hasBattle)
{
	resumeAck = false;
	resumeBattlePending = hasBattle;
	Log(LOG_INFO) << "[coop-session] armResumeHandshake (battlePending=" << hasBattle << ")";
}

void CoopSession::markLobbyOpen()
{
	lobbyClosed = false;
	Log(LOG_INFO) << "[coop-session] markLobbyOpen (lobbyClosed=0)";
}

void CoopSession::markLobbyClosed()
{
	lobbyClosed = true;
	Log(LOG_INFO) << "[coop-session] markLobbyClosed (lobbyClosed=1)";
}

void CoopSession::armDeferredSave(const std::string& name)
{
	pendingHostSaveName = name;
	Log(LOG_INFO) << "[coop-session] armDeferredSave ('" << name << "')";
}

void CoopSession::clearDeferredSave()
{
	pendingHostSaveName.clear();
	Log(LOG_INFO) << "[coop-session] clearDeferredSave";
}

void CoopSession::signalCampaignBegun()
{
	campaignBegun = true;
	Log(LOG_INFO) << "[coop-session] signalCampaignBegun (campaignBegun=1)";
}

void CoopSession::consumeCampaignBegun()
{
	campaignBegun = false;
	Log(LOG_INFO) << "[coop-session] consumeCampaignBegun (campaignBegun=0)";
}

void CoopSession::resetSession()
{
	Log(LOG_INFO) << "[coop-session] resetSession";
	role = CoopRole::None;
	lobbyMode = 0;
	clientInLobby = false;
	sessionLocked = false;
	lobbyClosed = true;
	resumeAck = false;
	resumeBattlePending = false;
	resumeBattleEligible.clear();
	campaignBegun = false;
	pendingHostSaveName.clear();

	// Full teardown returns the process to a pristine coop identity so a later
	// solo save (or a second campaign) does not inherit this session's saveID or
	// its stale world blobs (fixes C1/C2). This is the ONLY teardown path;
	// onClientDrop deliberately keeps both so the host can serve a rejoin (D5).
	connectionTCP::saveID = 0;
	{
		std::lock_guard<std::mutex> lock(connectionTCP::coopFilesMutex);
		connectionTCP::coopFilesHost.clear();
		connectionTCP::coopFilesClient.clear();
	}
}

void CoopSession::onClientDrop()
{
	Log(LOG_INFO) << "[coop-session] onClientDrop";
	clientInLobby = false;
	resumeAck = false;
	resumeBattlePending = false;
	resumeBattleEligible.clear();
	campaignBegun = false;
	pendingHostSaveName.clear();
	// role/lobbyMode/sessionLocked/lobbyClosed survive: the campaign context
	// outlives a peer drop (D5) - the freeze dialog or waiting lobby takes
	// over. The legacy new-battle lobby (mode 0) instead restarts its ready
	// dance, so its lock is released here.
	if (lobbyMode == 0)
	{
		sessionLocked = false;
	}
}

bool connectionTCP::isPasswordRequired = false;
std::string connectionTCP::password = "";

bool connectionTCP::isPlayerReady = false;

bool connectionTCP::isPlayersReady = false;

int connectionTCP::LobbyFileStatus = -1;

int connectionTCP::lobby_timer = -1;

bool connectionTCP::loadProgressBusy = false;

bool connectionTCP::forceCloseCoopStateMenu = false;

bool connectionTCP::forceClosePasswordCheckMenu = false;

int connectionTCP::manuallyAddedServerRemoveID = -1;

bool connectionTCP::canRemoveManuallyAddedServer = false;

bool connectionTCP::isInfoboxClosed = true;

// saveID is only used when the host saves each player's progress. This ensures that players load the correct save data.
long long connectionTCP::saveID = 0;

int connectionTCP::_weekday = 0;
int connectionTCP::_day = 0;
int connectionTCP::_month = 0;
int connectionTCP::_year = 0;
int connectionTCP::_hour = 0;
int connectionTCP::_minute = 0;
int connectionTCP::_second = 0;

int connectionTCP::monthsPassed = 0;
int connectionTCP::daysPassed = 0;

std::unordered_map<std::string, std::string> OpenXcom::connectionTCP::coopFilesHost{};
std::unordered_map<std::string, std::string> OpenXcom::connectionTCP::coopFilesClient{};
std::mutex OpenXcom::connectionTCP::coopFilesMutex;

std::string current_ping = "";

connectionTCP::connectionTCP(Game* game) : _game(game)
{
	// PRD-J01: publish the process-single Game for the static seat accessors.
	_staticGame = game;
	// PRD-J03: register the SHARED economy command handlers (idempotent).
	SharedEcon::init();
}

connectionTCP::~connectionTCP()
{

	 _stop = true;
	 _clientStop = true;
	 _hostStop = true;

	if (_loopThread.joinable())
		_loopThread.join();   

	if (_clientThread.joinable())
		_clientThread.join();

	if (_hostThread.joinable())
		_hostThread.join();   

}

SPSCQueue<1024> g_txQ{};
SPSCQueue<1024> g_rxQ{};

// Main-thread hold queue used by updateCoopTask(). Keep it outside the
// function so disconnect/reconnect cleanup can reset it fully between sessions.
static std::mutex g_rxHoldMutex;
static std::deque<std::string> g_rxHold;

// TX-queue drop counter (test harness diagnostic; see connectionTCP.h).
std::atomic<uint64_t> g_txDropCount{0};

// ===== Geoscape sync conflation slot =====
// One overwrite slot per snapshot channel (see CoopSnapSlot). The main thread
// (GeoscapeState::think) overwrites; the send drain reads the freshest value and
// clears the dirty flag. Written/read under g_snapMx so a mid-read frame can't be
// torn by a concurrent overwrite.
static std::mutex g_snapMx;
static std::array<std::string, SNAP_COUNT> g_snap;
static std::array<bool, SNAP_COUNT> g_snapDirty{}; // value-init -> all false

void enqueueSnapshot(CoopSnapSlot slot, std::string&& s)
{
	if (slot < 0 || slot >= SNAP_COUNT)
		return;
	std::lock_guard<std::mutex> lk(g_snapMx);
	g_snap[slot] = std::move(s); // discards only the stale prior snapshot (LWW-safe)
	g_snapDirty[slot] = true;
}

bool anySnapshotDirty()
{
	std::lock_guard<std::mutex> lk(g_snapMx);
	for (int i = 0; i < SNAP_COUNT; ++i)
		if (g_snapDirty[i])
			return true;
	return false;
}

bool popSnapshot(std::string& out)
{
	std::lock_guard<std::mutex> lk(g_snapMx);
	for (int i = 0; i < SNAP_COUNT; ++i)
	{
		if (g_snapDirty[i])
		{
			out = g_snap[i]; // raw payload; UDP sends whole messages (no framing)
			g_snapDirty[i] = false;
			return true;
		}
	}
	return false;
}

static inline void appendFramed(std::string& out, const std::string& payload); // defined below

// Append every dirty snapshot into `out` (framed) and clear its flag. Called by
// the send drains right after the g_txQ batch, so snapshots ride the same
// sendAll as the reliable batch (freshest value only, at link rate).
static void drainSnapshotsInto(std::string& out)
{
	std::lock_guard<std::mutex> lk(g_snapMx);
	for (int i = 0; i < SNAP_COUNT; ++i)
	{
		if (g_snapDirty[i])
		{
			appendFramed(out, g_snap[i]);
			g_snapDirty[i] = false;
		}
	}
}

// Reset conflation slots on session teardown (mirrors the g_rxHold clear).
static void clearSnapshotSlots()
{
	std::lock_guard<std::mutex> lk(g_snapMx);
	for (int i = 0; i < SNAP_COUNT; ++i)
	{
		g_snap[i].clear();
		g_snapDirty[i] = false;
	}
}

// ===== Time helper =====
static inline uint64_t now_ms()
{
	using namespace std::chrono;
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

// Optional sugar: serialize JSON and enqueue via sendTCPPacketStaticData (no direct socket send).
static inline void sendJSONNoLock(const Json::Value& v)
{
	Json::StreamWriterBuilder wb;
	wb["indentation"] = "";
	std::string s = Json::writeString(wb, v);
	sendTCPPacketStaticData(std::move(s));
}

bool enqueueTx(std::string&& s)
{
	if (s.empty())
		return false;

	if (!g_txQ.push(std::move(s)))
	{
		DebugLog("TX queue full, dropping packet\n");
		++g_txDropCount;
		return false;
	}

	return true;
}

void clearNetworkSessionQueues()
{
	// Reset all shared packet queues so a new session starts like a fresh game launch.
	// This clears stale packets left by the previous TCP/UDP session, including
	// packets held by updateCoopTask() while the game was not ready to consume them.
	clearPackets = false;

	std::string drop;
	while (g_txQ.pop(drop))
	{
	}

	while (g_rxQ.pop(drop))
	{
	}

	{
		std::lock_guard<std::mutex> lock(g_rxHoldMutex);
		g_rxHold.clear();
	}

	clearSnapshotSlots();
}

// HOST: emit PING once per second (independent from client)
static uint64_t h_nextPingAt = 0;
static uint64_t h_rttAvgMs = 0;
static constexpr double kHostRttEWMA = 0.2;

static inline void hostMaybeSendPing()
{
	uint64_t t = now_ms();
	if (t >= h_nextPingAt)
	{
		h_nextPingAt = t + 1000;
		Json::Value ping;
		ping["type"] = "PING";
		ping["ts"] = Json::UInt64(t);
		sendJSONNoLock(ping);
	}
}

// HOST: when receiving PONG, compute RTT and log it
static inline bool maybeHandlePongOnHost(const Json::Value& obj)
{
	if (obj.isMember("type") && obj["type"].asString() == "PONG")
	{
		uint64_t sent = obj["ts"].asUInt64();
		uint64_t rtt = now_ms() - sent;

		OpenXcom::current_ping = std::to_string((unsigned long long)rtt);

		return true; // handled
	}
	return false;
}

void sendTCPPacketStaticData(std::string data)
{
	enqueueTx(std::move(data));
}

// CLIENT: if incoming JSON is PING, reply with PONG (mirror host behavior)
static inline bool maybeHandlePingOnClient(const Json::Value& obj)
{
	if (obj.isMember("type") && obj["type"].asString() == "PING")
	{
		Json::Value pong;
		pong["type"] = "PONG";
		pong["ts"] = obj["ts"];
		sendJSONNoLock(pong);
		return true; // handled internally
	}
	return false;
}

// Log helper
void logError(const std::string& msg)
{
	std::cerr << msg << std::endl;
	DebugLog((msg + "\n").c_str());
}

bool connectionTCP::hasCoopFile(const std::string& key)
{
	std::lock_guard<std::mutex> lock(coopFilesMutex);

	const auto& coopFiles = getServerOwner()
								? coopFilesHost
								: coopFilesClient;

	return coopFiles.find(key) != coopFiles.end();
}

std::string connectionTCP::hostBlobKey(const std::string& clientName)
{
	return "host_" + std::to_string(connectionTCP::saveID) + "_" + clientName + ".data";
}

// Single authority for "may this machine touch local saves". coopSession is the
// file-scope global behind isCoopSession(); getCoopStatic()/getServerOwner() are
// static. Solo (no session, not connected) or the host may use local .sav files;
// a coop client may not (its world lives only in the host's save).
bool connectionTCP::localSavesAllowed()
{
	return (!coopSession && !getCoopStatic()) || getServerOwner();
}

bool connectionTCP::localLoadsAllowed()
{
	// Same liveness terms the save gate uses, WITHOUT the host escape: a live
	// session forbids local loads for everyone (C7). Solo / post-session: allowed.
	return !coopSession && !getCoopStatic();
}


std::string connectionTCP::clientBlobKey(const std::string& hostName)
{
	return "client_" + std::to_string(connectionTCP::saveID) + "_" + hostName + ".data";
}

const std::string* connectionTCP::findHostClientBlob(const std::string& clientName)
{
	// Keys look like host_<saveID>_<clientName>.data. Match the EXACT name field
	// (so "Bob" never matches "Super_Bob") and, among matches, keep the newest
	// saveID (datetime ids are equal-width, so lexicographic compare orders
	// them). Parsing lives here, not in SavedGame::save.
	static const std::string prefix = "host_";
	static const std::string ext = ".data";
	const std::string* best = nullptr;
	std::string bestId;
	for (const auto& kv : coopFilesHost)
	{
		const std::string& k = kv.first;
		if (kv.second.empty()
			|| k.size() < prefix.size() + ext.size()
			|| k.compare(0, prefix.size(), prefix) != 0
			|| k.compare(k.size() - ext.size(), ext.size(), ext) != 0)
			continue;
		size_t idEnd = k.find('_', prefix.size());
		if (idEnd == std::string::npos || k.size() < idEnd + 1 + ext.size())
			continue;
		std::string name = k.substr(idEnd + 1, k.size() - (idEnd + 1) - ext.size());
		if (name != clientName)
			continue;
		std::string id = k.substr(prefix.size(), idEnd - prefix.size());
		if (!best || id > bestId)
		{
			best = &kv.second;
			bestId = id;
		}
	}
	return best;
}

// One authority for the campaign_start packet (see header). Reads the player
// roster from the save (host lobby start sets it just before calling this, so
// save->getCoopPlayers() equals the freshly-built list).
Json::Value connectionTCP::buildCampaignStartPacket(const SavedGame* save)
{
	Json::Value root;
	root["state"] = "campaign_start";
	root["difficulty"] = (int)save->getDifficulty();
	root["gamemode"] = connectionTCP::_coopGamemode;
	root["saveID"] = static_cast<Json::Int64>(connectionTCP::saveID);
	// PRD-J01: propagate the campaign economy model so the client adopts it.
	root["campaignType"] = static_cast<int>(save->getCampaignType());
	int idx = 0;
	for (const auto& p : save->getCoopPlayers())
	{
		root["players"][idx++] = p;
	}
	return root;
}

bool connectionTCP::inCoopCampaignContext() const
{
	return _game->getSavedGame()
		&& !_game->getSavedGame()->getCountries()->empty()
		&& _game->getSavedGame()->isCoopSave();
}

// Drop world-blob captures that key the same player under an older saveID, so
// the maps hold one entry per player instead of growing with every saveID
// regeneration. Matches the EXACT player-name field of the key
// (<prefix><saveID>_<playerName>.data), never a suffix: the old "ends with
// _<name>.data" test also matched a DIFFERENT player whose name ended in the
// stored name (storing "Bob" erased "Super_Bob"'s world - CONFIRMED S8 data
// loss). Names are compared field-for-field so no such collision is possible.
static void eraseStaleBlobEntries(std::unordered_map<std::string, std::string>& files,
								  const std::string& prefix,
								  const std::string& playerName,
								  const std::string& keepKey)
{
	for (auto it = files.begin(); it != files.end();)
	{
		const std::string& k = it->first;
		bool stale = false;
		if (k != keepKey
			&& k.size() >= 5
			&& k.compare(0, prefix.size(), prefix) == 0
			&& k.compare(k.size() - 5, 5, ".data") == 0)
		{
			// player-name field: everything between the saveID's trailing
			// underscore and the ".data" extension.
			size_t idEnd = k.find('_', prefix.size());
			if (idEnd != std::string::npos && k.size() >= idEnd + 1 + 5)
			{
				std::string name = k.substr(idEnd + 1, k.size() - (idEnd + 1) - 5);
				stale = (name == playerName);
			}
		}
		if (stale)
			it = files.erase(it);
		else
			++it;
	}
}

namespace {
// PRD-11 C13: thrown by the streamer's ack-wait loops when the connection is
// torn down mid-transfer, so the streamer abandons the stream instead of
// parking forever on isWaitMap while still holding sendFileClient.
struct StreamAbort {};
}

// in the loop, load the map file data between host and client
void connectionTCP::loopData()
{
	// Wait for the client's map-chunk ack (isWaitMap), but bail out if the
	// connection is being torn down so a mid-transfer drop cannot park this
	// thread. The teardown signal is disconnectTCP forcing BOTH send flags false
	// (a live transfer always holds exactly one of them true); the destructor
	// sets _stop. coopSession is NOT a reliable "streaming" signal here - the
	// redesigned resume/rejoin flows stream a world without the old ready
	// handshake that sets it, so keying on it aborts legitimate streams.
	auto waitForMapAck = [&]() {
		while (!isWaitMap)
		{
			if (_stop || (!sendFileClient && !sendFileHost))
				throw StreamAbort{};
			SDL_Delay(20);
		}
	};

	while (!_stop)
	{
		try
		{
			if (sendFileClient)
			{
				int fileindex = 0;

				std::string filepath = "";

				if (sendProgressLoadFileToClient != "")
				{
					filepath = sendProgressLoadFileToClient;
				}
				else if (sendFileBase)
				{
					filepath = "basehost";
				}
				else
				{
					filepath = "battlehost";
				}
	
				std::istringstream memoryStream;
				std::istream* myfile = nullptr;

				if (sendProgressLoadFileToClient == "")
				{
					// Read from memory
					std::lock_guard<std::mutex> lock(connectionTCP::coopFilesMutex);

					const auto& coopFiles = getServerOwner()
												? connectionTCP::coopFilesHost
												: connectionTCP::coopFilesClient;

					auto it = coopFiles.find(filepath);
					if (it == coopFiles.end())
					{
						throw std::runtime_error("Failed to read from hash map with key: " + filepath);
					}

					memoryStream.str(it->second);
					myfile = &memoryStream;
				}
				else
				{
					// Resume blob, snapshotted on the main thread when the
					// request_load_progress arrived (nothing streams from disk)
					memoryStream.str(sendProgressLoadBlob);
					myfile = &memoryStream;
				}

				std::string line;
				std::string result;

				while (std::getline(*myfile, line))
				{
					waitForMapAck();

					std::cout << line << std::endl;
					if (fileindex != 0)
						line = "\n" + line;
					result += line;

					if (result.size() > 3000 && result.size() < 4000)
					{
						isWaitMap = false;
						Json::Value obj;
						obj["state"] = "map_result_data";
						obj["data"] = result;
						sendTCPPacketStaticData(obj.toStyledString());
						result = "";
					}
					else if (result.size() > 4000)
					{
						isWaitMap = true;
						for (unsigned i = 0; i < result.length(); i += 3000)
						{
							waitForMapAck();

							isWaitMap = false;

							std::string splitValue = result.substr(i, 3000);
							Json::Value obj;
							obj["state"] = "map_result_data";
							obj["data"] = splitValue;
							sendTCPPacketStaticData(obj.toStyledString());
						}
						result = "";
					}
					fileindex++;
				}

				isWaitMap = false;
				Json::Value obj;
				obj["state"] = "map_result_data";
				obj["data"] = result;
				sendTCPPacketStaticData(obj.toStyledString());

				waitForMapAck();

				std::string jsonData = sendFileBase
										   ? "{\"state\" : \"MAP_RESULT_CLIENT_BASE\"}"
										   : "{\"state\" : \"MAP_RESULT_CLIENT\"}";

				if (sendProgressLoadFileToClient != "")
				{
					jsonData = "{\"state\" : \"MAP_RESULT_LOAD_PROGRESS\"}";
				}

				sendTCPPacketStaticData(jsonData);
				sendFileBase = false;
				sendFileClient = false;
				sendFileSave = false;
				sendProgressLoadFileToClient = "";
				sendProgressLoadBlob = "";
			}
			else if (sendFileHost)
			{
				int fileindex = 0;

				std::string filepath;

				if (sendProgressSaveFileToHost)
				{
					filepath = clientBlobKey(_game->getCoopMod()->getHostName());
				}
				else if (sendFileSave)
				{
					filepath = "battlehost";
				}
				else if (sendFileBase)
				{
					filepath = "basehost";
				}
				else if (connectionTCP::_coopCampaign)
				{
					filepath = "basehost";
				}
				else
				{
					filepath = "battlehost";
				}

				std::string coopKey = filepath; // tai filename, jos mapin avain on filename

				std::string blobCopy;
				{
					std::lock_guard<std::mutex> lock(connectionTCP::coopFilesMutex);

					const auto& coopFiles = getServerOwner()
												? connectionTCP::coopFilesHost
												: connectionTCP::coopFilesClient;

					auto it = coopFiles.find(coopKey);
					if (it == coopFiles.end())
					{
						throw std::runtime_error("Failed to read from hash map with key: " + coopKey);
					}

					blobCopy = it->second;
				}

				std::istringstream myfile(blobCopy);

				std::string line;
				std::string result;

				while (getline(myfile, line))
				{
					waitForMapAck();

					std::cout << line << std::endl;
					if (fileindex != 0)
						line = "\n" + line;
					result += line;

					if (result.size() > 3000 && result.size() < 4000)
					{
						isWaitMap = false;
						Json::Value obj;
						obj["state"] = "map_result_data";
						obj["data"] = result;
						sendTCPPacketStaticData(obj.toStyledString());
						result.clear();
					}
					else if (result.size() > 4000)
					{
						isWaitMap = true;
						for (unsigned i = 0; i < result.length(); i += 3000)
						{
							waitForMapAck();

							isWaitMap = false;

							std::string splitValue = result.substr(i, 3000);
							Json::Value obj;
							obj["state"] = "map_result_data";
							obj["data"] = splitValue;
							sendTCPPacketStaticData(obj.toStyledString());
						}
						result.clear();
					}
					fileindex++;
				}

				isWaitMap = false;
				Json::Value obj;
				obj["state"] = "map_result_data";
				obj["data"] = result;
				sendTCPPacketStaticData(obj.toStyledString());

				waitForMapAck();

				std::string jsonData = sendFileBase
										   ? "{\"state\" : \"MAP_RESULT_HOST_BASE\"}"
										   : "{\"state\" : \"MAP_RESULT_HOST\"}";

				if (sendProgressSaveFileToHost)
				{
					jsonData = "{\"state\" : \"MAP_RESULT_SAVE_PROGRESS\"}";
				}

				sendTCPPacketStaticData(jsonData);
				sendFileBase = false;
				sendFileHost = false;
				sendFileSave = false;
				sendProgressSaveFileToHost = false;
			}
		}
		catch (const StreamAbort&)
		{
			// PRD-11 C13: connection torn down mid-transfer. Abandon the stream
			// and release every send flag so the streamer returns to idle instead
			// of parking with sendFileClient still set.
			Log(LOG_INFO) << "[coop] streamer: connection torn down mid-transfer, abandoning stream";
			sendFileBase = false;
			sendFileClient = false;
			sendFileHost = false;
			sendFileSave = false;
			sendProgressLoadFileToClient = "";
			sendProgressLoadBlob = "";
			sendProgressSaveFileToHost = false;
		}
		catch (const std::exception& e)
		{
			// Build one message for both logError and crash.log
			std::string msg = "Error in loopData: " + std::string(e.what());

			logError(msg);
			CRASH_LOG(msg);
		}
		catch (...)
		{
			std::string msg = "Unknown error in loopData!";

			logError(msg);
			CRASH_LOG(msg);
		}

		SDL_Delay(10); // Prevent 100% CPU usage when idle
	}
}

void connectionTCP::giftSoldier(Soldier* soldier, int newOwnerId, bool broadcast)
{

	if (!soldier || !_game->getSavedGame())
	{
		return;
	}

	// Playtest: SHARED geoscape gift is host-authoritative - route the ownership move
	// through the soldier_gift shared_cmd so BOTH machines adopt it (the SEPARATE
	// local+broadcast below never reached the SHARED replica). Battle-time gifts still
	// use the live-control path below.
	if (broadcast && isSharedCampaign() && !_game->getSavedGame()->getSavedBattle())
	{
		int baseId = 0;
		auto* bases = _game->getSavedGame()->getBases();
		for (size_t i = 0; i < bases->size(); ++i)
		{
			bool here = false;
			for (auto* s : *bases->at(i)->getSoldiers())
				if (s == soldier) { here = true; break; }
			if (here) { baseId = (int)i; break; }
		}
		Json::Value payload;
		payload["soldierId"] = soldier->getId();
		payload["newOwner"] = newOwnerId;
		SharedEcon::submitLocalCmd(_game, "soldier_gift", baseId, payload);
		return;
	}

	soldier->setOwnerPlayerId(newOwnerId);
	soldier->setCoop(newOwnerId);

	int localPlayerId = localSeat();

	Log(LOG_INFO) << "[coop-gift] giftSoldier '" << soldier->getName() << "' id=" << soldier->getId()
	              << " newOwner=" << newOwnerId << " localPlayer=" << localPlayerId
	              << " broadcast=" << (broadcast ? 1 : 0)
	              << " inBattle=" << (_game->getSavedGame()->getSavedBattle() ? 1 : 0);

	if (_game->getSavedGame()->getSavedBattle())
	{

		// Battle running: flip live control now, do the physical move only
		// after the mission ends (the BattleUnit and the debriefing still
		// reference this Soldier).
		auto* battle = _game->getSavedGame()->getSavedBattle();

		for (auto& unit : *battle->getUnits())
		{

			if (unit->getGeoscapeSoldier() == soldier)
			{

				unit->setCoop(newOwnerId);

				// The unit is no longer ours: move the selection along.
				if (battle->getSelectedUnit() == unit && newOwnerId != localPlayerId)
				{
					battle->selectNextPlayerUnit();
				}

				if (broadcast)
				{

					// Immediate control flip on the peer's battle too.
					Json::Value obj;
					obj["state"] = "giftSoldier";
					obj["soldier_id"] = soldier->getId();
					obj["owner"] = newOwnerId;
					obj["unit_id"] = unit->getId();

					sendTCPPacketData(obj.toStyledString());

				}

				break;

			}

		}

		if (broadcast && newOwnerId != localPlayerId)
		{
			_pendingSoldierGifts.push_back(std::make_pair(soldier, newOwnerId));
		}

	}
	else if (broadcast && newOwnerId != localPlayerId)
	{

		// The soldier's object lives in its owner's save (guest-soldier
		// model): hand it to the peer and drop it from our world. It keeps
		// its station base, so it stays "in" the base it is in right now.
		sendSoldierGiftPacket(soldier, newOwnerId);
		removeSoldierFromLocalBases(soldier);
		_giftedSoldiers.push_back(soldier);
		_giftedAwaySoldierIds.insert(soldier->getId());

		// keep the host-side client blob fresh (no-op on the host itself)
		pushProgressToHostSilently();

	}

}

void connectionTCP::processPendingSoldierGifts()
{

	// Replay physical gifts that arrived while our world was swapped out
	// for the peer's base view OR for a coop battle. The flags clear before
	// LoadGameState has actually restored our save, so also require that an
	// own (non-mirror) base is present, that no own-world reload is still
	// pending (LoadGameState on top of the stack), and that we are not mid
	// mission-end - the swapped peer/battle world would otherwise re-swallow
	// the soldier and mark its packet id as seen, losing it for good.
	bool ownWorldReady = false;

	State* topState = _game->getStates().empty() ? nullptr : _game->getStates().back();
	bool ownWorldLoadPending = (dynamic_cast<LoadGameState*>(topState) != nullptr);

	if (!_pendingIncomingGifts.empty()
	    && _game->getCoopMod()->playerInsideCoopBase == false
	    && _game->getCoopMod()->coopMissionEnd == false
	    && !ownWorldLoadPending
	    && _game->getSavedGame()
	    && !_game->getSavedGame()->getSavedBattle())
	{

		for (auto& base : *_game->getSavedGame()->getBases())
		{
			if (base->_coopBase == false && base->_coopIcon == false)
			{
				ownWorldReady = true;
				break;
			}
		}

	}

	if (ownWorldReady)
	{

		std::vector<Json::Value> replay;
		replay.swap(_pendingIncomingGifts);

		for (auto& obj : replay)
		{
			onTCPMessage("giftSoldier", obj);
		}

	}

	if (_game->getSavedGame() && !_game->getSavedGame()->getSavedBattle() && getCoopStatic() && getCoopCampaign() && _pendingSoldierGifts.empty())
	{

		// Targeted sweep: a stale copy of a soldier we gifted away this
		// session can resurrect when the pre-visit "basehost" snapshot is
		// restored after a gift made while viewing the peer's base. Park
		// exactly those (matched by id AND still peer-owned - a soldier
		// traded back to us has our owner id and is left alone). Deliberately
		// NOT a blanket owner check: legacy saves carry stale ownerPlayerId
		// values on unrelated soldiers.
		if (!_giftedAwaySoldierIds.empty())
		{

			int localPlayerId = localSeat();

			for (auto& base : *_game->getSavedGame()->getBases())
			{

				auto& soldiers = *base->getSoldiers();

				for (auto it = soldiers.begin(); it != soldiers.end();)
				{

					Soldier* s = *it;

					if (_giftedAwaySoldierIds.count(s->getId()) != 0 && s->getOwnerPlayerId() != 999 && s->getOwnerPlayerId() != localPlayerId)
					{
						_giftedSoldiers.push_back(s);
						it = soldiers.erase(it);
					}
					else
					{
						++it;
					}

				}

			}

		}

	}

	if (_pendingSoldierGifts.empty())
	{
		return;
	}

	if (_game->getSavedGame() && _game->getSavedGame()->getSavedBattle())
	{
		// Still in battle; try again later.
		return;
	}

	for (auto& pending : _pendingSoldierGifts)
	{

		Soldier* soldier = pending.first;

		// Died during the mission: stays in the giver's memorial. The physical
		// hand-off never happened, so undo the in-battle ownership flip that
		// giftSoldier applied - otherwise the fallen soldier would
		// sit in the giver's Hall of Honour still flagged as the peer's
		// (coop/ownerPlayerId), and the receiver never gets a memorial entry
		// (its gift was skipped). Reset to a plain own-soldier so the
		// giver's memorial records it correctly.
		if (soldier->getDeath())
		{
			soldier->setCoop(0);
			soldier->setOwnerPlayerId(999);
			continue;
		}

		// Auto-keep an in-battle-gifted soldier on the craft it was
		// deployed on, mirroring how a giver's own crew stays aboard their
		// craft after a mission. The guest lives in the receiver's world, so
		// the live Craft* pointer cannot survive the hand-off (it is detached
		// in sendSoldierGiftPacket) - translate it into the guest CoopCraft
		// mechanism, which the receiver's mission-end reload and battle merge
		// honour (CoopCraft = the host craft id, CoopCraftType = its type).
		// A wounded survivor is deliberately left unassigned so it is not flown
		// straight back out while it should be recovering.
		if (Craft* craft = soldier->getCraft())
		{
			if (!soldier->isWounded())
			{
				soldier->setCoopCraft(craft->getId());
				soldier->setCoopCraftType(craft->getType());
			}
			else
			{
				soldier->setCoopCraft(-1);
				soldier->setCoopCraftType("");
			}
		}

		sendSoldierGiftPacket(soldier, pending.second);
		removeSoldierFromLocalBases(soldier);
		_giftedSoldiers.push_back(soldier);
		_giftedAwaySoldierIds.insert(soldier->getId());

	}

	_pendingSoldierGifts.clear();

	// transfers happened while our world was busy - sync the blob now
	pushProgressToHostSilently();

}

void connectionTCP::pushProgressToHostSilently()
{

	// Client only: serialize the current world and stream it to the host so
	// the host's next save embeds an up-to-date client blob. Never while the
	// world is swapped out - a base visit, an active battle, or the mission-end
	// window before the own-world reload (GeoscapeState::init) has run. In all
	// of those the live save is the PEER's world; uploading it would overwrite
	// our own-world blob with the host's world and destroy our roster.
	if (getServerOwner() || !getCoopStatic() || connectionTCP::saveID == 0)
	{
		return;
	}
	// PRD-J02: a SHARED replica has no world of its own to push - the host's
	// single authoritative save is the whole truth. No-op.
	if (isSharedReplica())
	{
		return;
	}
	State* topStatePush = _game->getStates().empty() ? nullptr : _game->getStates().back();
	if (!_game->getSavedGame() || _game->getSavedGame()->getSavedBattle()
	    || _game->getCoopMod()->playerInsideCoopBase
	    || _game->getCoopMod()->coopMissionEnd
	    || (dynamic_cast<LoadGameState*>(topStatePush) != nullptr))
	{
		return;
	}

	std::string filename = clientBlobKey(_game->getCoopMod()->getHostName());
	_game->getSavedGame()->saveCoopToMemory(filename, _game->getMod(), filename);
	{
		std::lock_guard<std::mutex> lock(coopFilesMutex);
		eraseStaleBlobEntries(coopFilesClient, "client_", _game->getCoopMod()->getHostName(), filename);
	}

	Json::Value obj;
	obj["state"] = "SEND_FILE_HOST_TRUE_SAVE_PROGRESS";
	sendTCPPacketData(obj.toStyledString());

	Log(LOG_INFO) << "[coop-gift] pushed client progress to host (" << filename << ")";

}

void connectionTCP::syncOwnWorldGuestCraft(int coopBaseId, const std::map<std::string, std::pair<int, std::string>>& assignments)
{

	// Fix B (Bug 1). Client only. Durably mirror the client's guest->host-craft
	// assignments into the client's OWN-world blob (client_<saveID>_<host>.data),
	// which is exactly what GeoscapeState::init reloads at mission end. The real
	// mirror-base UI (SoldiersState::btnOkClick) only writes CoopCraft into the
	// "basehost" blob (the client's copy of the HOST world), so without this the
	// guest's CoopCraft reverts to its stale value and the soldier is unassigned
	// from the skyranger after every battle.
	//
	// Unlike pushProgressToHostSilently() this deliberately NEVER serializes or
	// uploads the LIVE save and does NOT send SEND_FILE_HOST_TRUE_SAVE_PROGRESS:
	// this runs while the client is inside the mirror base, where the live save
	// is the swapped-in HOST world. That packet round-trips and drives the client
	// to upload its live (host) world as the client blob - the exact corruption
	// Fix A prevents. We only edit the in-memory own-world blob, which is all the
	// mission-end reload needs.
	if (getServerOwner() || !getCoopStatic() || connectionTCP::saveID == 0)
	{
		return;
	}
	if (assignments.empty())
	{
		return;
	}

	std::string filename = clientBlobKey(_game->getCoopMod()->getHostName());
	if (!hasCoopFile(filename))
	{
		return;
	}

	SavedGame* ownWorld = new SavedGame();
	ownWorld->loadCoopSaveFromMemory(filename, _game->getMod(), _game->getLanguage(), filename);

	bool changed = false;
	for (auto* base : *ownWorld->getBases())
	{
		for (auto* s : *base->getSoldiers())
		{
			// Only the client's own guests live in this blob; matching by the
			// peer base id + coop name skips any unrelated soldiers. Soldier ids
			// differ across worlds, so coopName is the stable cross-world key.
			if (s->getCoopBase() != coopBaseId)
			{
				continue;
			}
			auto it = assignments.find(s->getCoopName());
			if (it == assignments.end())
			{
				continue;
			}
			if (s->getCoopCraft() != it->second.first || s->getCoopCraftType() != it->second.second)
			{
				s->setCoopCraft(it->second.first);
				s->setCoopCraftType(it->second.second);
				changed = true;
			}
		}
	}

	if (changed)
	{
		ownWorld->saveCoopToMemory(filename, _game->getMod(), filename);
		Log(LOG_INFO) << "[coop-gift] synced guest craft assignments into own-world blob (" << filename << ")";
	}

	delete ownWorld;

}

// Last census actually put on the wire, so an unchanged tally costs nothing.
static std::string _lastGuestCensus;

/**
 * COOP living quarters: tell the peer how many of OUR soldiers are stationed at
 * each of THEIR bases.
 *
 * A soldier transferred to a peer base keeps living in this machine's roster
 * (TransferItemsState::completeTransfer deliberately skips the erase when the
 * destination is a co-op base) and tags itself with getCoopBase() = that base's
 * id. The peer therefore has no Soldier object for it at all - its syncTrade
 * drops the incoming TRANSFER_SOLDIER outright - so without this report the
 * guest occupies nobody's living quarters. Base::getUsedQuarters() subtracts
 * these guests here and adds the peer's census as coop_guests there, so the
 * base that HOUSES a soldier is the one that pays for it.
 */
void connectionTCP::sendGuestCensus(bool force)
{
	if (!getCoopStatic() || !getCoopCampaign() || !_game->getSavedGame())
		return;
	// SHARED has one world: a transfer really moves the soldier, and
	// getTotalSoldiers() already counts it at the destination while it is in
	// transit. Nothing to report.
	if (isSharedCampaign())
		return;

	std::map<int, int> guests; // peer base coop id -> headcount
	for (auto* base : *_game->getSavedGame()->getBases())
	{
		// only OUR real bases hold our soldiers; a visited peer base is a
		// swapped-in copy and would double-count them
		if (base->_coopBase || base->_coopIcon)
			continue;
		for (auto* soldier : *base->getSoldiers())
		{
			if (soldier->getCoopBase() != -1)
				guests[soldier->getCoopBase()]++;
		}
	}

	Json::Value root;
	root["state"] = "guest_census";
	Json::Value list(Json::arrayValue);
	for (const auto& entry : guests)
	{
		Json::Value e;
		e["base_id"] = entry.first;
		e["guests"] = entry.second;
		list.append(e);
	}
	root["bases"] = list;

	std::string payload = root.toStyledString();
	if (!force && payload == _lastGuestCensus)
		return;
	_lastGuestCensus = payload;
	sendTCPPacketData(payload);
}

void connectionTCP::resetGiftSessionState()
{

	_pendingSoldierGifts.clear();
	_pendingIncomingGifts.clear();
	_seenGiftPacketIds.clear();
	_giftedAwaySoldierIds.clear();

	// PRD-06 C5: a different world is being loaded - abort any armed deferred
	// host save so a late client blob cannot rewrite the (now stale) named save.
	if (!session.pendingHostSaveName.empty())
	{
		Log(LOG_INFO) << "[coop] world switch aborts armed deferred host save (" << session.pendingHostSaveName << ")";
		session.pendingHostSaveName.clear();
	}

}

void connectionTCP::writePendingHostSave()
{

	if (session.pendingHostSaveName.empty())
	{
		return;
	}

	// A battle started (or the save vanished) since the request: the live world
	// is no longer the one the save captured - disarm without writing.
	if (!_game->getSavedGame() || _game->getSavedGame()->getSavedBattle())
	{
		session.pendingHostSaveName.clear();
		return;
	}

	try
	{
		// same atomic dance as SaveGameState: backup, then rename
		std::string backup = session.pendingHostSaveName + ".bak";
		_game->getSavedGame()->save(backup, _game->getMod());
		CrossPlatform::moveFile(Options::getMasterUserFolder() + backup,
								Options::getMasterUserFolder() + session.pendingHostSaveName);
	}
	catch (const std::exception& e)
	{
		Log(LOG_ERROR) << "[coop] deferred host save write failed: " << e.what();
	}

	session.clearDeferredSave();

}

void connectionTCP::sendSoldierGiftPacket(Soldier* soldier, int newOwnerId)
{

	// Which base is the soldier stationed at? If it is already a guest at the
	// peer's base, keep that station; otherwise it is in one of our own bases
	// - find it and use that base's coop id.
	int stationBaseId = soldier->getCoopBase();

	if (stationBaseId == -1 && _game->getSavedGame())
	{

		for (auto& base : *_game->getSavedGame()->getBases())
		{

			auto containsSoldier = [soldier](const std::vector<Soldier*>& list)
			{
				return std::find(list.begin(), list.end(), soldier) != list.end();
			};

			// The live roster may be temporarily swapped out while a soldier
			// list screen is open - check the snapshots too.
			if (containsSoldier(*base->getSoldiers()) || containsSoldier(base->base_oldsoldiers) || containsSoldier(base->base_oldsoldiers2))
			{
				stationBaseId = base->_coop_base_id;
				break;
			}

		}

	}

	// Detach from any craft so the serialized soldier does not carry a craft
	// reference that would be resolved against the receiver's save.
	soldier->setCraft(0);

	YAML::YamlRootNodeWriter writer;
	writer.setAsMap();
	soldier->save(writer["soldier"], _game->getMod()->getScriptGlobal());

	// Durable unique id: player tag + wall-clock + counter. Receipts persist
	// in saves across sessions, so a per-run counter alone would collide.
	long long xferId = (getHost() ? 1000000000000000LL : 2000000000000000LL) + (long long)time(0) * 1000LL + (++_giftSendCounter % 1000);

	std::string yaml = writer.emit().yaml;

	Json::Value obj;
	obj["state"] = "giftSoldier";
	obj["soldier_id"] = soldier->getId();
	obj["owner"] = newOwnerId;
	obj["unit_id"] = -1;
	obj["station_base_id"] = stationBaseId;
	obj["xfer_id"] = Json::Value::Int64(xferId);
	obj["soldier_yaml"] = yaml;

	std::string packet = obj.toStyledString();

	Log(LOG_INFO) << "[coop-gift] SEND soldier '" << soldier->getName() << "' id=" << soldier->getId()
	              << " newOwner=" << newOwnerId << " stationBaseId=" << stationBaseId
	              << " packetBytes=" << packet.size();

	sendTCPPacketData(packet);

}

void connectionTCP::removeSoldierFromLocalBases(Soldier* soldier)
{

	if (!_game->getSavedGame())
	{
		return;
	}

	for (auto& base : *_game->getSavedGame()->getBases())
	{

		auto eraseFrom = [soldier](std::vector<Soldier*>& list)
		{
			list.erase(std::remove(list.begin(), list.end(), soldier), list.end());
		};

		eraseFrom(*base->getSoldiers());
		// SoldiersState/CraftSoldiersState swap the roster while open and
		// restore it from these snapshots afterwards - purge them too so the
		// soldier cannot resurrect on the giver's side.
		eraseFrom(base->base_oldsoldiers);
		eraseFrom(base->base_oldsoldiers2);

	}

}

void connectionTCP::clearAllReceivedTCPPackets()
{

	clearPackets = true;

}

void connectionTCP::createLoopdataThread()
{

	_loopThread = std::thread(&connectionTCP::loopData, this);

}

// an endless loop that processes the sync-packet data: battlescape, tasks, remove targets, research, trading, disconnect, errors.
void connectionTCP::updateCoopTask()
{

	// coop: finish queued in-battle soldier transfers as soon as no battle is
	// active (fallback for the client, which may not run the host's
	// coopMissionEnd path in GeoscapeState).
	processPendingSoldierGifts();

	// COOP living quarters: re-report our guest headcount whenever it changes.
	// Driven from here rather than from each mutation site (transfer, gift,
	// sack, base loss) so no path can forget it; sendGuestCensus is a cheap
	// tally and only touches the wire when the result actually differs.
	sendGuestCensus();

	if (connectionTCP::saveError == true)
	{

		connectionTCP::saveError = false;
		_game->pushState(new CoopState(995));

	}

	// This runs the Battlescape states even when the player is in the pause menu or elsewhere, so synchronization continues in the background.
	// However, this is never executed while the infobox menu is open, to avoid breaking the panic and berserk states.
	if (_game->getCoopMod()->getCoopStatic() == true && _game->getSavedGame() && connectionTCP::isInfoboxClosed == true)
	{

		if (_game->getSavedGame()->getSavedBattle())
		{

			if (_game->getSavedGame()->getSavedBattle()->getBattleGame() && _game->getSavedGame()->getSavedBattle()->getBattleState())
			{

				if (!_game->isState(_game->getSavedGame()->getSavedBattle()->getBattleState()))
				{

					_game->getSavedGame()->getSavedBattle()->getBattleGame()->handleStateCoop();

				}

			}

		}

	}

	// time
	if (connectionTCP::getCoopStatic() == true && connectionTCP::getServerOwner() == false && connectionTCP::_enable_time_sync == true && _year != 0)
	{

		if (_game->getSavedGame())
		{

			GameTime new_time(connectionTCP::_weekday, connectionTCP::_day, connectionTCP::_month, connectionTCP::_year, connectionTCP::_hour, connectionTCP::_minute, connectionTCP::_second);

			_game->getSavedGame()->setTime(new_time);

			_game->getSavedGame()->setMonthsPassed(connectionTCP::monthsPassed);
			_game->getSavedGame()->setDaysPassed(connectionTCP::daysPassed);

		}

	}

	// coop
	// trade
	if (getCoopStatic() && !waitedTrades.empty())
	{
		Json::Value newWaitedTrades(Json::arrayValue);

		for (Json::Value::ArrayIndex i = 0; i < waitedTrades.size(); ++i)
		{

			Base* currentBase = nullptr;

			int base_id = waitedTrades[i]["base_to_id"].asInt();

			for (auto base : *_game->getSavedGame()->getBases())
			{
				// My bad, _coop_base_id should be used instead of the base name.
				if (base->_coop_base_id == base_id)
				{
					currentBase = base;
					break;
				}
			}

			if (currentBase)
			{
				current_base_name = currentBase->getName(_game->getLanguage());

				CoopState* window = new CoopState(150);
				_game->pushState(window);

				currentBase->syncTrade(waitedTrades[i].toStyledString().c_str(), _game->getSavedGame(), _game->getMod());

				// Clear or mark element as empty
				waitedTrades[i] = 0; // Set the element to null
			}

			// Check a condition for keeping items. In this example, keep non-empty elements.
			if (waitedTrades[i] != 0)
			{
				newWaitedTrades.append(waitedTrades[i]); // Add the item to be kept into the new array
			}
		}

		// Replace the original array with the new one, where unwanted elements have been removed
		waitedTrades = newWaitedTrades;
	}

	// PRD-J03: drain the SHARED economy protocol queues at the same controlled
	// main-thread point as waitedTrades (host validates+applies+broadcasts queued
	// shared_cmd; replicas apply queued shared_apply and surface shared_fail).
	SharedEcon::update(_game);

	// wrong password
	if (onConnect == -5)
	{

		// the attempt is over: retire "Connecting..." before reporting why
		closeConnectingDialog();

		// Make sure it calls disconnectTCP, otherwise it may get stuck.
		_game->pushState(new CoopState(441));
	}

	// coop
	// server error!
	if (onConnect == -3)
	{

		closeConnectingDialog();

		// Make sure it calls disconnectTCP, otherwise it may get stuck.
		_game->pushState(new CoopState(440));

	}

	// disconnect from server!
	if (onConnect == -2)
	{

		if (allow_cutscene == true)
		{
			// Make sure it calls disconnectTCP, otherwise it may get stuck.
			if (getServerOwner() == true)
			{
				// campaign flow: no "... has left the server" popup - the
				// waiting lobby / freeze dialog handles real drops and a
				// refused joiner warrants no notification at all (D5). The
				// disconnect still has to run (CoopState(20)'s constructor
				// used to do it); its cleanup pushes the freeze dialog.
				if (connectionTCP::session.lobbyMode == 0)
				{
					_game->pushState(new CoopState(20));
				}
				else
				{
					_game->getCoopMod()->disconnectTCP();
				}
			}
			else if (getServerOwner() == false)
			{
				_game->pushState(new CoopState(21));
			}
		}
		else
		{
			// disconnect
			connectionTCP::_coopGamemode = 0;
			_game->getCoopMod()->disconnectTCP();
		}

	}

	// coop
	// Pull everything currently available from the transport queue into the hold queue.
	// The hold queue is global so disconnect/reconnect cleanup can clear it completely.
	{
		std::lock_guard<std::mutex> lock(g_rxHoldMutex);
		std::string msg;
		while (g_rxQ.pop(msg))
		{
			g_rxHold.emplace_back(std::move(msg));
		}
	}

	for (;;)
	{
		size_t passCount = 0;
		{
			std::lock_guard<std::mutex> lock(g_rxHoldMutex);
			if (g_rxHold.empty())
				break;
			passCount = g_rxHold.size();
		}

		size_t consumedThisPass = 0;

		for (size_t i = 0; i < passCount; ++i)
		{
			std::string jsonStr;
			{
				std::lock_guard<std::mutex> lock(g_rxHoldMutex);
				if (g_rxHold.empty())
					break;
				jsonStr = std::move(g_rxHold.front());
				g_rxHold.pop_front();
			}

			try
			{

				Json::CharReaderBuilder rb;
				std::unique_ptr<Json::CharReader> reader(rb.newCharReader());

				Json::Value obj;
				std::string errs;

				const char* begin = jsonStr.data();
				const char* end = begin + jsonStr.size();

				if (!reader->parse(begin, end, &obj, &errs))
				{
					DebugLog(std::string("JSON parse error: ") + errs + "\n");
					continue; // drop malformed
				}

				const std::string stateString = obj.get("state", "defaultState").asString();
				const int fromId = obj.get("from", -1).asInt();

				// debug mode
				if (Options::logPacketMessages == true && Options::logInfoToFile == true)
				{			
					std::string str_debug =
						std::string("task completed: ") + (_coop_task_completed ? "true" : "false") +
						"   connection status: " + std::to_string(onConnect) + 
						"   packet name: " + stateString +
						"   packet data: " + obj.toStyledString();

					DebugLog(str_debug);
				}

				// Make operator precedence explicit:
				const bool consumeNow =
						 (_coop_task_completed || ((stateString == "abortPath" && _coopWalkInit) ||
						 (stateString == "unit_death" && _coopInitDeath) ||
						 (stateString == "after_unit_death" && _coopInitDeath)) ||
					 stateString == "close_event" || stateString == "click_close" || stateString == "minimap_data" || stateString == "AIProgress" || stateString == "update_progress" || stateString == "DebriefingState" || stateString == "endTurn" || stateString == "hit_tile" || stateString == "destroy_tile" || stateString == "set_fire_tile" || stateString == "set_smoke_tile" || stateString == "unit_fire" || stateString == "calc_explode_fov" || stateString == "hasHitUnit") &&
					!(stateString == "endPlayerTurn" && (_coopEnd == 1 || (_game->getSavedGame() && !_game->getSavedGame()->getSavedBattle())));

				if (consumeNow)
				{
					onTCPMessage(stateString, obj);
					++consumedThisPass;
				}
				else
				{
					// Rotate to the back so we can try the next message.
					{
						std::lock_guard<std::mutex> lock(g_rxHoldMutex);
						g_rxHold.emplace_back(std::move(jsonStr));
					}
				}
			}
			catch (const std::exception& e)
			{
				// Build a single message used for both DebugLog and crash log
				std::string msg = std::string("Network process exception: ") + e.what();

				// Existing debug log
				DebugLog((msg + "\n").c_str());

				// Write a crash-style log file into user/logs/crash_YYYY-MM-DD_HH-MM-SS.log
				CRASH_LOG(msg);

				// Put back to the *back* to avoid pinning the head.
				{
					std::lock_guard<std::mutex> lock(g_rxHoldMutex);
					g_rxHold.emplace_back(std::move(jsonStr));
				}
				onConnect = -3;
				break;
			}
		}

		// If nothing progressed this pass, stop to avoid busy-waiting
		if (consumedThisPass == 0)
			break;
	}

	// coop
	// UNABLE TO CONNECT TO SERVER
	if (onConnect == 0)
	{
		onConnect = -1;

		// the attempt is over either way: never leave "Connecting..." up. A
		// join to a port nobody is listening on used to sit on it forever.
		closeConnectingDialog();

		// if client cancels the action
		if (cancel_connect == false)
		{
			_game->pushState(new CoopState(16));
		}

		cancel_connect = false;
	}


}

std::vector<std::string> splitVector(std::string s, std::string delimiter)
{
	size_t pos_start = 0, pos_end, delim_len = delimiter.length();
	std::string token;
	std::vector<std::string> res;

	while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos)
	{
		token = s.substr(pos_start, pos_end - pos_start);
		pos_start = pos_end + delim_len;
		res.push_back(token);
	}

	res.push_back(s.substr(pos_start));
	return res;
}

std::vector<std::string> connectionTCP::splitVectorMod(std::string s, std::string delimiter)
{
	return splitVector(s, delimiter);
}

bool connectionTCP::hasRequiredMods(const std::string& mod_hash)
{
	// Local mods
	std::vector<std::string> local_mod_names = _game->getMod()->getCoopModList();

	// If server does not require mods, allow join
	if (mod_hash == "")
		return true;

	// Required mods from the host/server
	std::vector<std::string> required_mods =
		_game->getCoopMod()->splitVectorMod(mod_hash, ";");

	// Remove empty strings, because mod_hash may end with ";"
	required_mods.erase(
		std::remove(required_mods.begin(), required_mods.end(), ""),
		required_mods.end());

	local_mod_names.erase(
		std::remove(local_mod_names.begin(), local_mod_names.end(), ""),
		local_mod_names.end());

	// Check that mod count is the same
	if (local_mod_names.size() != required_mods.size())
		return false;

	// Check that every required mod exists locally
	for (const auto& mod : required_mods)
	{
		if (std::find(local_mod_names.begin(), local_mod_names.end(), mod) == local_mod_names.end())
			return false;
	}

	return true;
}

void connectionTCP::syncCoopInventory()
{

	// inventory
	for (int i = 0; i < _jsonInventory.size(); i++)
	{

		std::string inv_id = _jsonInventory[i]["inv_id"].asString();
		int inv_x = _jsonInventory[i]["inv_x"].asInt();
		int inv_y = _jsonInventory[i]["inv_y"].asInt();
		int unit_id = _jsonInventory[i]["unit_id"].asInt();
		int item_id = _jsonInventory[i]["item_id"].asInt();
		int move_cost = _jsonInventory[i]["move_cost"].asInt();
		int slot_x = _jsonInventory[i]["slot_x"].asInt();
		int slot_y = _jsonInventory[i]["slot_x"].asInt();

		int getHealQuantity = _jsonInventory[i]["getHealQuantity"].asInt();
		int getPainKillerQuantity = _jsonInventory[i]["getPainKillerQuantity"].asInt();
		int getStimulantQuantity = _jsonInventory[i]["getStimulantQuantity"].asInt();
		int getFuseTimer = _jsonInventory[i]["getFuseTimer"].asInt();
		int getXCOMProperty = _jsonInventory[i]["getXCOMProperty"].asBool();
		int isAmmo = _jsonInventory[i]["isAmmo"].asBool();
		int isWeaponWithAmmo = _jsonInventory[i]["isWeaponWithAmmo"].asBool();
		int isFuseEnabled = _jsonInventory[i]["isFuseEnabled"].asBool();
		int getAmmoQuantity = _jsonInventory[i]["getAmmoQuantity"].asInt();

		std::string item_name = _jsonInventory[i]["item_name"].asString();

		int tile_x = _jsonInventory[i]["tile_x"].asInt();
		int tile_y = _jsonInventory[i]["tile_y"].asInt();
		int tile_z = _jsonInventory[i]["tile_z"].asInt();

		// new!!!
		bool coopbase = _jsonInventory[i]["coopbase"].asBool();
		bool other_coop_inventory = _jsonInventory[i]["other_coop_inventory"].asBool();
		int coopbase_id = _jsonInventory[i]["coopbase_id"].asInt();
		int craft_id = _jsonInventory[i]["craft_id"].asInt();
		std::string craft_type = _jsonInventory[i]["craft_type"].asString();
		int slot_type_int = _jsonInventory[i]["slot_type"].asInt();
		std::string item_type = _jsonInventory[i]["item_type"].asString();
		int item_slot_type = _jsonInventory[i]["item_slot_type"].asInt();

		const auto& arr = _jsonInventory[i]["coopItems"];
		int coop_item_id = _jsonInventory[i]["coop_item_id"].asInt();
		const auto& ammosArr = _jsonInventory[i]["ammos"];
		bool tu = _jsonInventory[i]["tu"].asBool();

		int sel_item_id = _jsonInventory[i]["sel_item_id"].asInt();
		std::string sel_item_type = _jsonInventory[i]["sel_item_type"].asString();

		bool unload_weapon = _jsonInventory[i]["unload_weapon"].asBool();

		// battle
		if (coopInventory == true && _game->getSavedGame()->getSavedBattle()->getBattleState())
		{

			if (other_coop_inventory == true)
			{

				std::string ammos = "";

				if (!ammosArr.isNull())
				{

					ammos = ammosArr.toStyledString();
				}

				_game->getSavedGame()->getSavedBattle()->getBattleState()->moveCoopInventory(ammos, item_name, inv_id, inv_x, inv_y, unit_id, item_id, move_cost, slot_x, slot_y, getHealQuantity, getPainKillerQuantity, getStimulantQuantity, getFuseTimer, getXCOMProperty, isAmmo, isWeaponWithAmmo, isFuseEnabled, getAmmoQuantity, tile_x, tile_y, tile_z, tu, sel_item_id, sel_item_type, unload_weapon);

				_jsonInventory[i] = {};
			}
		}
		// base
		else if (other_coop_inventory == false)
		{

			if (_game->getSavedGame()->getSavedBattle())
			{
				bool found = false;

				std::string coopItems = "";

				if (!arr.isNull())
				{
					coopItems = arr.toStyledString();
				}

				found = _game->getSavedGame()->getSavedBattle()->moveBaseCoopInventory(item_type, coop_item_id, coopbase_id, craft_id, craft_type, slot_type_int, item_slot_type, arr.toStyledString());

				if (found)
				{
					_jsonInventory[i] = {};
				}
			}
		}
	}

	// added items
	for (int i = 0; i < jsonAddedCoopItems.size(); i++)
	{

		if (_game->getSavedGame())
		{

			int coopbase_id = jsonAddedCoopItems[i]["coopbase_id"].asInt();
			int craft_id = jsonAddedCoopItems[i]["craft_id"].asInt();
			std::string craft_type = jsonAddedCoopItems[i]["craft_type"].asString();

			Base* current_base = 0;
			Craft* current_craft = 0;

			for (auto& base : *_game->getSavedGame()->getBases())
			{

				if (base->_coop_base_id == coopbase_id)
				{
					current_base = base;

					for (auto& craft : *base->getCrafts())
					{

						if (craft->getId() == craft_id && craft->getRules()->getType() == craft_type)
						{
							current_craft = craft;
							break;
						}
					}

					break;
				}
			}

			if (current_base && current_craft)
			{

				auto& coopItems = current_craft->getCoopItems();

				int item_coop_id = jsonAddedCoopItems[i]["item_coop_id"].asInt();
				bool coopbase = jsonAddedCoopItems[i]["coopbase"].asInt();
				std::string item_type = jsonAddedCoopItems[i]["item_type"].asString();

				// exists?
				bool item_exists = false;
				for (const auto& ci : coopItems)
				{
					if (ci.id == item_coop_id &&
						ci.type == item_type &&
						ci.owner == !coopbase)
					{
						item_exists = true;
						break;
					}
				}

				if (!item_exists)
				{
					coopItems.push_back({item_coop_id, item_type, !coopbase});

					_jsonInventory[i] = {};
				}
			}
		}
	}


}

bool isNumber(const std::string& s)
{
	for (char c : s)
	{
		if (!std::isdigit(c))
			return false;
	}
	return !s.empty();
}

int getPortFromAddress(const std::string& address)
{
	// If the input is empty, return -1
	if (address.empty())
	{
		return -1;
	}

	// If the input is just a number, return it as a port
	if (isNumber(address))
	{
		return std::stoi(address);
	}

	// Split the input by ':'
	auto parts = splitVector(address, ":");
	if (parts.size() == 2 && isNumber(parts[1]))
	{
		// If the second part is a valid number, return it as a port
		return std::stoi(parts[1]);
	}

	// If no port is found or it's invalid, return -1
	return -1;
}

void resetCoopState(bool isHost)
{
	coopSession = false;
	isWaitMap = true;
	onceTime = false;
	sendFileClient = false;
	sendProgressSaveFileToHost = false;
	sendFileBase = false;
	sendFileHost = false;
	sendFileSave = false;

	mapData.clear();

	onTcpHost = isHost;
	connectionTCP::session.role = isHost ? CoopRole::Host : CoopRole::Client;
	onConnect = -1;
	connectionTCP::no_bases = false;
	connectionTCP::isCoopBaseLoading = false;
	connectionTCP::session.sessionLocked = false;
	connectionTCP::isPlayerReady = false;
	connectionTCP::isPlayersReady = false;
	connectionTCP::LobbyFileStatus = -1;
	connectionTCP::lobby_timer = -1;
	connectionTCP::forceCloseCoopStateMenu = false;
	connectionTCP::forceClosePasswordCheckMenu = false;

}

// SERVER SETUP


// ===== Constants =====
static constexpr uint32_t kMaxMsgLen = 4u * 1024u * 1024u; // Safety cap: 4 MB per message

// ===== TCP helpers =====

// Send all bytes reliably (loop until all data is written).
// Uses SDLNet_TCP_Send under the hood.
static inline bool sendAll(TCPsocket s, const char* data, int len)
{
	int sent = 0;
	while (sent < len)
	{
		int n = SDLNet_TCP_Send(s, data + sent, len - sent);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

// Build BE32 length-prefixed frame into a single contiguous buffer.
static inline void appendFramed(std::string& out, const std::string& payload)
{
	uint32_t len = (uint32_t)payload.size();
	uint32_t be = SDL_SwapBE32(len);
	size_t old = out.size();
	out.resize(old + 4 + payload.size());
	std::memcpy(out.data() + old, &be, 4);
	std::memcpy(out.data() + old + 4, payload.data(), payload.size());
}

// ===== RTT measurement via PING/PONG =====

// Client: emit PING once per second
static uint64_t g_nextPingAt = 0;
static uint64_t g_rttAvgMs = 0;
static constexpr double kRttEWMA = 0.2;

static inline void clientMaybeSendPing()
{
	uint64_t t = now_ms();
	if (t >= g_nextPingAt)
	{
		g_nextPingAt = t + 1000;
		Json::Value ping;
		ping["type"] = "PING";
		ping["ts"] = Json::UInt64(t);
		sendJSONNoLock(ping); // goes through TX queue
	}
}

// Host: if incoming JSON is PING, enqueue PONG via the same TX queue; do not forward to game.
static inline bool maybeHandlePingOnHost(const Json::Value& obj)
{
	if (obj.isMember("type") && obj["type"].asString() == "PING")
	{
		Json::Value pong;
		pong["type"] = "PONG";
		pong["ts"] = obj["ts"];
		sendJSONNoLock(pong); // host -> single client via TX queue
		return true;          // handled internally
	}
	return false;
}

// Client: if incoming JSON is PONG, compute RTT and log, do not forward to game.
static inline bool maybeHandlePongOnClient(const Json::Value& obj)
{
	if (obj.isMember("type") && obj["type"].asString() == "PONG")
	{
		uint64_t sent = obj["ts"].asUInt64();
		uint64_t rtt = now_ms() - sent;

		current_ping = std::to_string((unsigned long long)rtt);

		return true; // handled internally
	}
	return false;
}

// Clears all received packets (client/host):
// - recvBuffer: partially received framed bytes
// - g_rxQ: already-parsed JSON messages waiting for the game thread
// - socket: any bytes already waiting in the TCP socket are read and dropped (non-blocking)
static inline void clearAllReceivedPackets(TCPsocket sock,
											SDLNet_SocketSet socketSet,
											std::vector<char>& recvBuffer)
{
	// Drop partially received framed bytes
	recvBuffer.clear();

	// Drop already parsed messages waiting for the game thread
	std::string drop;
	while (g_rxQ.pop(drop))
	{
		// intentionally empty
	}

	// Drop bytes already waiting in the TCP socket (non-blocking)
	if (sock && socketSet)
	{
		for (;;)
		{
			int ready = SDLNet_CheckSockets(socketSet, 0);
			if (ready <= 0 || !SDLNet_SocketReady(sock))
				break;

			char buf[16 * 1024];
			int bytes = SDLNet_TCP_Recv(sock, buf, sizeof(buf));
			if (bytes <= 0)
				break; // disconnected or error (caller handles disconnect)
		}
	}
}

// ===== Client thread =====
void connectionTCP::startTCPClient()
{

	SDL_Delay(1000);
	DebugLog("startTCPClient\n");
	resetCoopState(false); // client

#ifdef _WIN32
	SDL_Delay(100); // tiny stagger; avoid long sleeps
#endif

	if (SDLNet_Init() == -1)
	{
		DebugLog("SDLNet init failed\n");
		onConnect = -3;
		return;
	}

	IPaddress ip;
	if (SDLNet_ResolveHost(&ip, ipAddress.c_str(), tcp_port) == -1)
	{
		DebugLog("Can't resolve host\n");
		SDLNet_Quit();
		onConnect = 0;
		return;
	}

	TCPsocket sock = SDLNet_TCP_Open(&ip);
	if (!sock)
	{
		DebugLog("Can't connect to server\n");
		SDLNet_Quit();
		onConnect = 0;
		return;
	}

	SDLNet_SocketSet socketSet = SDLNet_AllocSocketSet(1);
	SDLNet_TCP_AddSocket(socketSet, sock);

	std::vector<char> recvBuffer;
	recvBuffer.reserve(4096);

	bool initSent = false; // one-time handshake
	onConnect = 1;

	for (;;)
	{

		if (clearPackets == true)
		{
			clearPackets = false;
			clearAllReceivedPackets(sock, socketSet, recvBuffer);
		}

		if (onConnect == -1)
			break;

		if (_clientStop)
			break;

		// ---- Batch-send: drain up to 64 queued payloads into one write ----
		{
			std::string out;
			out.reserve(8192);
			std::string msg;
			int batched = 0;
			while (batched < 64 && g_txQ.pop(msg))
			{
				appendFramed(out, msg);
				++batched;
			}
			// Conflated geoscape snapshots ride the same framed write as the
			// reliable batch (freshest value only, at link rate -> no backlog).
			drainSnapshotsInto(out);
			if (!out.empty())
			{
				if (!sendAll(sock, out.data(), (int)out.size()))
				{
					DebugLog("DISCONNECT CLIENT: SEND\n");
					clearAllReceivedPackets(sock, socketSet, recvBuffer);
					onConnect = -2;
					onceTime = false;
					break;
				}
			}
		}

		// ---- Receive: read all available data (no artificial delays) ----
		int ready = SDLNet_CheckSockets(socketSet, 0); // 0 ms timeout
		if (ready > 0 && SDLNet_SocketReady(sock))
		{
			for (;;)
			{
				char buf[16 * 1024];
				int bytes = SDLNet_TCP_Recv(sock, buf, sizeof(buf));
				if (bytes <= 0)
				{
					DebugLog("DISCONNECT CLIENT: RECV\n");
					clearAllReceivedPackets(sock, socketSet, recvBuffer);
					onConnect = -2;
					onceTime = false;
					goto client_cleanup;
				}
				recvBuffer.insert(recvBuffer.end(), buf, buf + bytes);
				if (bytes < (int)sizeof(buf))
					break; // nothing more immediately
			}

			// ---- Parse frames ----
			while (recvBuffer.size() >= 4)
			{

				uint32_t msgLenNet = 0;
				std::memcpy(&msgLenNet, recvBuffer.data(), 4);
				uint32_t msgLen = SDL_SwapBE32(msgLenNet);

				if (msgLen == 0 || msgLen > kMaxMsgLen)
				{
					DebugLog("Client: invalid message size, disconnecting\n");
					clearAllReceivedPackets(sock, socketSet, recvBuffer);
					onConnect = -3;
					onceTime = false;
					goto client_cleanup;
				}

				const size_t need = 4ull + static_cast<size_t>(msgLen);
				if (recvBuffer.size() < need)
					break;

				std::string message(
					reinterpret_cast<const char*>(recvBuffer.data() + 4),
					static_cast<size_t>(msgLen));

				recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + need);

				if (!message.empty())
				{

					// Handle PING/PONG internally, push others to RX queue for the game thread
					Json::CharReaderBuilder rb;
					std::unique_ptr<Json::CharReader> reader(rb.newCharReader());

					Json::Value obj;
					std::string errs;

					const char* begin = message.data();
					const char* end = begin + message.size();

					if (reader->parse(begin, end, &obj, &errs))
					{
						if (maybeHandlePingOnClient(obj))
							continue;

						if (maybeHandlePongOnClient(obj))
							continue;
					}
					else
					{
						DebugLog(std::string("JSON parse failed: ") + errs + "\n");
						continue; // drop invalid JSON
					}

					if (!g_rxQ.push(std::move(message)))
					{
						DebugLog("RX queue full, dropping message\n");
					}

				}
			}
		}

		// ---- One-time handshake ----
		if (!initSent)
		{
			initSent = true;
			Json::Value hello;
			hello["state"] = "INIT_SERVER";
			hello["playername"] = sendTcpPlayer;
			hello["servername"] = sendTcpServerName;
			hello["tcp_password"] = connectionTCP::password;

			sendJSONNoLock(hello);
		}

		// ---- Client RTT ping ----
		clientMaybeSendPing();

		// ---- Gentle yield only if nothing happened ----
		if (ready == 0 && g_txQ.empty() && !anySnapshotDirty())
		{
#ifdef _WIN32
			SDL_Delay(0);
#endif
		}
	}

client_cleanup:
	SDLNet_FreeSocketSet(socketSet);
	SDLNet_TCP_Close(sock);
	SDLNet_Quit();
	return;
}

// ===== Host thread (single client) =====
// Simplified for exactly two players: host + one client.
// If another client tries to connect, close it silently (no "server_full" message),
// because we only use sendTCPPacketStaticData for outbound traffic.
void connectionTCP::startTCPHost()
{
	DebugLog("startTCPHost\n");
	resetCoopState(true); // host

	if (SDLNet_Init() == -1)
	{
		onConnect = -3;
		DebugLog("SDLNet init failed\n");
		return;
	}

	IPaddress ip;
	if (SDLNet_ResolveHost(&ip, nullptr, tcp_port) == -1)
	{
		DebugLog("Can't resolve host\n");
		SDLNet_Quit();
		onConnect = -3;
		return;
	}

	TCPsocket listening = SDLNet_TCP_Open(&ip);
	if (!listening)
	{
		DebugLog("Can't open TCP socket\n");
		SDLNet_Quit();
		onConnect = -3;
		return;
	}

	SDLNet_SocketSet socketSet = SDLNet_AllocSocketSet(2);
	SDLNet_TCP_AddSocket(socketSet, listening);

	TCPsocket clientSock = nullptr;
	std::vector<char> recvBuffer;
	recvBuffer.reserve(4096);

	onConnect = 1;
	// thread-side role mirror (pre-struct behavior kept; the main-thread
	// hosting path also sets this via setServerOwner)
	session.role = CoopRole::Host;

	for (;;)
	{

		if (clearPackets == true)
		{
			clearPackets = false;
			clearAllReceivedPackets(clientSock, socketSet, recvBuffer);
		}

		if (onConnect == -1)
		{
			clearAllReceivedPackets(clientSock, socketSet, recvBuffer);
			break;
		}
	

		if (_hostStop)
		{
			clearAllReceivedPackets(clientSock, socketSet, recvBuffer);
			break;
		}

		// ---- Accept new client if we don't have one ----
		if (TCPsocket newClient = SDLNet_TCP_Accept(listening))
		{
			if (!clientSock)
			{
				clientSock = newClient;
				SDLNet_TCP_AddSocket(socketSet, clientSock);
				DebugLog("Host: client connected\n");
				onConnect = 1;
			}
			else
			{
				// Only 1 client supported -> close extra connection silently
				SDLNet_TCP_Close(newClient);
			}
		}

		// ---- Batch-send outbound messages to the single client ----
		if (clientSock)
		{
			std::string out;
			out.reserve(8192);
			std::string msg;
			int batched = 0;
			while (batched < 64 && g_txQ.pop(msg))
			{
				appendFramed(out, msg);
				++batched;
			}
			// Conflated geoscape snapshots ride the same framed write as the
			// reliable batch (freshest value only, at link rate -> no backlog).
			drainSnapshotsInto(out);
			if (!out.empty())
			{
				if (!sendAll(clientSock, out.data(), (int)out.size()))
				{
					DebugLog("Host: send failed, drop client\n");
					onConnect = -3;
					SDLNet_TCP_DelSocket(socketSet, clientSock);
					SDLNet_TCP_Close(clientSock);
					clientSock = nullptr;
					recvBuffer.clear();
				}
			}
		}

		// ---- Receive from client (drain all available bytes) ----
		int ready = SDLNet_CheckSockets(socketSet, 0); // 0 ms timeout
		if (ready > 0 && clientSock && SDLNet_SocketReady(clientSock))
		{
			for (;;)
			{
				char buf[16 * 1024];
				int bytes = SDLNet_TCP_Recv(clientSock, buf, sizeof(buf));
				if (bytes <= 0)
				{
					DebugLog("Host: client disconnected\n");
					onConnect = -2;
					SDLNet_TCP_DelSocket(socketSet, clientSock);
					SDLNet_TCP_Close(clientSock);
					clientSock = nullptr;
					recvBuffer.clear();
					break;
				}
				recvBuffer.insert(recvBuffer.end(), buf, buf + bytes);
				if (bytes < (int)sizeof(buf))
					break;
			}

			// Parse frames
			while (clientSock && recvBuffer.size() >= 4)
			{
				uint32_t msgLenNet = 0;
				std::memcpy(&msgLenNet, recvBuffer.data(), 4);
				uint32_t msgLen = SDL_SwapBE32(msgLenNet);

				if (msgLen == 0 || msgLen > kMaxMsgLen)
				{
					DebugLog("Host: invalid message size, drop client\n");
					onConnect = -3;
					SDLNet_TCP_DelSocket(socketSet, clientSock);
					SDLNet_TCP_Close(clientSock);
					clientSock = nullptr;
					recvBuffer.clear();
					break;
				}

				const size_t need = 4ull + static_cast<size_t>(msgLen);
				if (recvBuffer.size() < need)
					break;

				std::string message(
					reinterpret_cast<const char*>(recvBuffer.data() + 4),
					static_cast<size_t>(msgLen));

				recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + need);

				if (!message.empty())
				{
					Json::CharReaderBuilder rb;
					std::unique_ptr<Json::CharReader> reader(rb.newCharReader());

					Json::Value obj;
					std::string errs;

					const char* begin = message.data();
					const char* end = begin + message.size();

					if (reader->parse(begin, end, &obj, &errs))
					{
						if (maybeHandlePingOnHost(obj))
							continue;

						if (maybeHandlePongOnHost(obj))
							continue;
					}
					else
					{
						DebugLog(std::string("Host: JSON parse failed: ") + errs + "\n");
						continue; // drop invalid JSON
					}

					if (!g_rxQ.push(std::move(message)))
						DebugLog("RX queue full, dropping message\n");
				}
			}
		}

		if (clientSock)
			hostMaybeSendPing();

		// ---- Gentle yield if nothing to do ----
		if (ready == 0 && OpenXcom::g_txQ.empty() && !OpenXcom::anySnapshotDirty())
		{
#ifdef _WIN32
			SDL_Delay(0);
#endif
		}
	}

	// ---- Cleanup ----
	if (clientSock)
	{
		SDLNet_TCP_DelSocket(socketSet, clientSock);
		SDLNet_TCP_Close(clientSock);
	}
	SDLNet_TCP_DelSocket(socketSet, listening);
	SDLNet_TCP_Close(listening);
	SDLNet_FreeSocketSet(socketSet);
	SDLNet_Quit();

	// thread-side role clear on host-thread exit (pre-struct behavior kept)
	session.role = CoopRole::None;
	onConnect = -1;
	return;
}

/**
 * Push a state while keeping an already-open "player joined" popup on top.
 *
 * The join handshake pushes Profile when the peer is announced, but the lobby
 * arrives in a LATER packet (initProfile, below), so a plain pushState buried
 * the popup under the lobby on the client - the host, whose lobby is already
 * open when the peer joins, showed it correctly. The popup is the newest thing
 * that happened, so it stays on top and the lobby renders behind it.
 */
/**
 * Retire the "Connecting..." wait dialog (CoopState 15).
 *
 * Every connect attempt ends here, success or failure, so the dialog can never
 * be left lurking under what comes next. It used to be popped only on one
 * client success path: a refused or failed join stacked its error popup ON TOP
 * of it, and dismissing that error resurfaced a dead "Connecting..." window;
 * a join that simply never completed left it up forever.
 *
 * A password join buries [Connecting, PasswordCheckMenu, Connecting] (JOIN
 * pushes a second wait dialog), so the stale password menu is popped too, not
 * just a run of Connecting dialogs (issue #46).
 */
void connectionTCP::closeConnectingDialog()
{
	while (!_game->getStates().empty())
	{
		State* top = _game->getStates().back();
		CoopState* connecting = dynamic_cast<CoopState*>(top);
		if ((connecting && connecting->getStateCode() == 15)
			|| dynamic_cast<PasswordCheckMenu*>(top) != nullptr)
		{
			_game->popState();
		}
		else
		{
			break;
		}
	}
}

void connectionTCP::pushKeepingProfileOnTop(State* state)
{
	const bool profileOnTop = !_game->getStates().empty()
		&& dynamic_cast<Profile*>(_game->getStates().back()) != nullptr;

	if (profileOnTop)
	{
		_game->popState();               // lift the popup off
		_game->pushState(state);         // the lobby lands underneath it
		_game->pushState(new Profile);   // and the popup goes back on top
	}
	else
	{
		_game->pushState(state);
	}
}

void connectionTCP::initProfile(bool clientInBattle, bool inBattle)
{
	// campaign flow: sessions are lobby-gated up front - no post-join lobby
	// re-entry (F2/F3). Only the legacy new-battle path reopens it here.
	if (_game->getCoopMod()->getServerOwner() == false && connectionTCP::session.lobbyMode == 0)
	{
		pushKeepingProfileOnTop(new LobbyMenu);
	}

	if (_game->getCoopMod()->getCoopStatic() == true)
	{

		// if the client is in battle and the host is not, send the host a file and a notification
		if (clientInBattle == true && inBattle == false)
		{

			// client only!
			if (_game->getCoopMod()->getHost() == false)
			{

				connectionTCP::LobbyFileStatus = 1;
			}
		}
		// CHECK IF THE HOST IS IN BATTLE � IF SO, ADD JOINERS; OTHERWISE DO NOTHING
		else if (inBattle == true)
		{

			// only client!
			if (_game->getCoopMod()->getHost() == false)
			{

				connectionTCP::LobbyFileStatus = 2;
			}
		}
	}
}

long long connectionTCP::getDateTimeCoop() const
{
	time_t now = time(0);
	tm* timeInfo = localtime(&now);

	return (timeInfo->tm_year + 1900) * 10000000000LL +
		   (timeInfo->tm_mon + 1) * 100000000LL +
		   timeInfo->tm_mday * 1000000LL +
		   timeInfo->tm_hour * 10000LL +
		   timeInfo->tm_min * 100LL +
		   timeInfo->tm_sec;
}

// TCP
void connectionTCP::onTCPMessage(std::string stateString, Json::Value obj)
{

	// PRD-J03: single early hook routing the shared_* economy protocol into the
	// SharedEcon dispatch table (the anti-if-chain requirement). If SharedEcon
	// consumes the message, it never falls through to the if-chain below.
	if (SharedEcon::onMessage(_game, stateString, obj))
		return;

	if (stateString == "kick_player")
	{

		disconnectTCP();

		_game->pushState(new CoopState(123456));

	}

	// refused by the campaign roster gate (flow-redesign F3)
	if (stateString == "lobby_join_refused")
	{

		connectionTCP::joinRefusalReason = obj.get("reason", "").asString();

		disconnectTCP();

		// drop the "Connecting..." dialog left from the join attempt so
		// dismissing the refusal leaves no stray dialog behind
		if (!_game->getStates().empty())
		{
			CoopState* topDialog = dynamic_cast<CoopState*>(_game->getStates().back());
			if (topDialog && topDialog->getStateCode() == 15)
			{
				_game->popState();
			}
		}
		connectionTCP::forceCloseCoopStateMenu = true;

		_game->pushState(new CoopState(63));

	}

	if (stateString == "tcp_password")
	{

		// A bounce while the prompt is already up means the password the
		// player submitted was wrong: let updateCoopTask() raise the
		// "Incorrect password" dialog (441). The FIRST bounce is only the
		// challenge; flagging it -5 too made 441 bury the prompt on the
		// next frame, before the player could type anything.
		bool promptAlreadyOpen = false;
		for (State* s : _game->getStates())
		{
			if (dynamic_cast<PasswordCheckMenu*>(s) != nullptr)
			{
				promptAlreadyOpen = true;
				break;
			}
		}

		if (promptAlreadyOpen)
		{
			onConnect = -5;
		}
		else
		{
			connectionTCP::forceCloseCoopStateMenu = true;

			// If this room/server requires a password, open passwordCheck menu.
			_game->pushState(new PasswordCheckMenu(ipAddress, _game->getCoopMod()->getHostName(), tcp_port, false, true));
		}

	}

	if (stateString == "lobby_ready")
	{

		connectionTCP::session.campaignStarted();

		// Skirmish (mode 0): the host pressed START BATTLE. Leave the lobby with
		// it - the host is already generating the battle and streams it over, so
		// the client must not be left sitting on a dead lobby it cannot dismiss
		// (it has no button). Pops the join popup stacked above the lobby too.
		if (connectionTCP::session.lobbyMode == 0 && getServerOwner() == false)
		{
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

	}

	if (stateString == "lobby_timer")
	{

		int timer = obj["timer"].asInt();
		connectionTCP::lobby_timer = timer;

	}

	if (stateString == "coop_session_locked")
	{

		bool isPlayerReady = obj["isPlayerReady"].asBool();
		connectionTCP::isPlayersReady = isPlayerReady;

		if (connectionTCP::isPlayersReady == false)
		{
			connectionTCP::lobby_timer = -1;
		}

		if (connectionTCP::isPlayerReady == true && connectionTCP::isPlayersReady == true)
		{
			connectionTCP::session.campaignStarted();
		}

	}

	if (stateString == "change_player_name")
	{

		std::string name = obj["name"].asString();
		tcpPlayerName = name;

	}

	if (stateString == "change_team")
	{

		// teams are locked once the campaign starts (flow-redesign D3)
		if (connectionTCP::session.sessionLocked == false)
		{
			int gamemode = obj["gamemode"].asInt();
			connectionTCP::_coopGamemode = gamemode;
		}

	}

	// --- campaign flow redesign ------------------------------------------
	// Host clicked START CAMPAIGN: build this player's own world with the
	// host's difficulty (D2; ironman stays host-only) and begin base
	// placement. The lobby is wiped by setState.
	if (stateString == "campaign_start" && getServerOwner() == false)
	{

		int difficulty = obj["difficulty"].asInt();
		connectionTCP::_coopGamemode = obj["gamemode"].asInt();
		connectionTCP::saveID = obj["saveID"].asInt64();
		connectionTCP::session.campaignStarted();
		connectionTCP::session.lobbyMode = 1;

		std::vector<std::string> players;
		for (const auto& p : obj["players"])
		{
			players.push_back(p.asString());
		}

		CoopCampaignType campaignType =
			static_cast<CoopCampaignType>(obj.get("campaignType", 0).asInt());

		if (campaignType == CoopCampaignType::Shared)
		{
			// PRD-J02: a SHARED client is a replica - it never builds its own
			// world. Do NOT run beginInitialBasePlacement. Hold the wait dialog
			// until the host finishes placing its first base and streams the
			// authoritative world (streamSharedWorldToClient ->
			// MAP_RESULT_LOAD_PROGRESS), which the client then adopts exactly
			// like a resume. The roster/type ride the streamed save's header.
			connectionTCP::session.markLobbyClosed();
			connectionTCP::session.resumeAck = false;
			_game->pushState(new CoopState(COOP_DLG_CLIENT_LOAD_WAIT));
		}
		else
		{
			SavedGame* save = _game->getMod()->newSave((GameDifficulty)difficulty);
			save->setDifficulty((GameDifficulty)difficulty);
			save->setCoopSave(true);
			save->setCoopPlayers(players);
			// PRD-J01: adopt the host's campaign economy model (default Separate).
			save->setCampaignType(campaignType);
			_game->setSavedGame(save);

			connectionTCP::session.markLobbyClosed();

			GeoscapeState* gs = new GeoscapeState;
			_game->setState(gs);
			gs->init();

			beginInitialBasePlacement(_game, gs, _game->getSavedGame()->getBases()->back());
		}

	}

	// Host clicked RESUME CAMPAIGN and we have a stored world: fetch it
	// (same wire flow as the classic Profile resume) (F3)
	if (stateString == "campaign_resume" && getServerOwner() == false)
	{

		_game->pushState(new CoopState(COOP_DLG_CLIENT_LOAD_WAIT));

		Json::Value root;
		root["state"] = "request_load_progress";
		sendTCPPacketData(root.toStyledString());

	}

	// PRD-11 C13: the host is busy streaming another transfer. Signal the
	// load-wait dialog to retry (it schedules a ~2s-spaced retry, bounded).
	if (stateString == "load_progress_busy" && getServerOwner() == false)
	{
		connectionTCP::loadProgressBusy = true;
	}

	// The host began/resumed the campaign: drop the waiting dialog (D5)
	if (stateString == "campaign_begun" && getServerOwner() == false)
	{

		connectionTCP::session.signalCampaignBegun();
		connectionTCP::session.sessionLive();

	}

	// A resuming player finished loading its world (F3). For a battle save
	// the geoscape ack triggers phase two: the battle stream.
	if (stateString == "resume_ack")
	{

		// PRD-11 C8: only stream the battle to a client that was actually served
		// a resume world blob. A registered-but-no-blob client is routed through
		// fresh base building and acks too (BaseNameState); streaming the old
		// battle into its freshly built world would corrupt it. Such an acker is
		// absent from resumeBattleEligible, so it falls through to the plain
		// campaign-resume ack.
		std::string acker = _game->getCoopMod()->getCurrentClientName();
		bool battleEligible = connectionTCP::session.resumeBattleEligible.count(acker) > 0;

		if (connectionTCP::session.resumeBattlePending && battleEligible)
		{
			connectionTCP::session.resumeBattlePending = false;
			connectionTCP::session.resumeBattleEligible.clear();

			Json::Value root;
			root["state"] = "campaign_resume_battle";
			sendTCPPacketData(root.toStyledString());
		}
		else
		{
			connectionTCP::session.resumeAck = true;

			// PRD-J09 / PRD-J10: an AUTOMATIC SHARED world restream just landed on
			// the client - post-battle (J09) or desync repair (J10). Adopting a
			// streamed world always parks the client in
			// COOP_DLG_CLIENT_RESUME_HOLD (LoadGameState) until the host
			// "resumes" - at bootstrap/resume that release is the operator's
			// BEGIN click (campaign_begun). Neither of these restreams has a
			// click behind it, so release the hold here: the replica now holds
			// the authoritative world and the session is already live.
			if ((sharedPostBattleRestream || sharedResyncRestream)
				&& isSharedCampaign() && getServerOwner())
			{
				const char* why = sharedPostBattleRestream ? "post-battle" : "resync";
				sharedPostBattleRestream = false;
				sharedResyncRestream = false;
				connectionTCP::session.sessionLive();

				Json::Value begun;
				begun["state"] = "campaign_begun";
				sendTCPPacketData(begun.toStyledString());

				Log(LOG_INFO) << "[coop-shared] " << why << " restream adopted; released the client hold";
			}
		}

	}

	// Phase two of a battle-save resume: fetch the battle from the host
	// (same wire flow the legacy lobby used for a battle-hosting session)
	if (stateString == "campaign_resume_battle" && getServerOwner() == false)
	{

		_game->getCoopMod()->inventory_battle_window = false;

		_game->pushState(new CoopState(1));

		Json::Value root;
		root["state"] = "SEND_FILE_CLIENT_SAVE";
		sendTCPPacketData(root.toStyledString());

	}

	if (stateString == "giveUnit")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				int unit_id = obj["unit_id"].asInt();
				int coop = obj["coop"].asInt();

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getId() == unit_id)
					{

						// save
						if (getHost() == true && getCoopCampaign() == true)
						{

							if (unit->getGeoscapeSoldier())
							{

								if (unit->getGeoscapeSoldier()->getOwnerPlayerId() == 999)
								{

									unit->getGeoscapeSoldier()->setOwnerPlayerId(unit->getCoop());

								}

							}

						}

						unit->setCoop(coop);

						break;

					}

				}

			}

		}

	}

	if (stateString == "giftSoldier")
	{

		if (_game->getSavedGame())
		{

			int soldier_id = obj["soldier_id"].asInt();
			int owner = obj["owner"].asInt();
			int unit_id = obj["unit_id"].asInt();

			Log(LOG_INFO) << "[coop-gift] RECV giftSoldier id=" << soldier_id << " owner=" << owner
			              << " hasYaml=" << (obj.isMember("soldier_yaml") ? 1 : 0)
			              << " inBattle=" << (_game->getSavedGame()->getSavedBattle() ? 1 : 0);

			if (obj.isMember("soldier_yaml") && _game->getCoopMod()->playerInsideCoopBase == true)
			{

				// Our SavedGame is currently swapped out for the peer's base
				// view; applying the transfer now would land the soldier in
				// the temporary world and lose it on exit. Queue the real
				// apply for later, but ALSO drop a display copy into the
				// visited base so the new owner sees the soldier right away,
				// and show the notification now.
				Log(LOG_INFO) << "[coop-gift] RECV deferred (viewing peer base)";

				try
				{

					int stationBaseId = obj["station_base_id"].asInt();

					YAML::YamlRootNodeReader reader(YAML::YamlString{obj["soldier_yaml"].asString()}, "giftSoldier");
					auto soldierReader = reader["soldier"];
					std::string type = soldierReader["type"].readVal(_game->getMod()->getSoldiersList().front());
					std::string soldierName = soldierReader["name"].readVal(std::string());

					Base* visited = 0;

					for (auto& base : *_game->getSavedGame()->getBases())
					{
						if (base->_coop_base_id == stationBaseId)
						{
							visited = base;
							break;
						}
					}

					if (visited && _game->getMod()->getSoldier(type))
					{
						// Display-only copy: this world is discarded on exit;
						// the durable copy comes from the deferred replay.
						Soldier* copy = new Soldier(_game->getMod()->getSoldier(type), 0, 0 /*nationality*/);
						copy->load(soldierReader, _game->getMod(), _game->getSavedGame(), _game->getMod()->getScriptGlobal());
						copy->setCraft(0);
						copy->setCoopBase(stationBaseId);
						copy->setOwnerPlayerId(owner);
						copy->setCoop(owner);
						visited->getSoldiers()->push_back(copy);
					}

					if (!obj.get("notified", false).asBool())
					{
						std::string baseName = visited ? visited->getName() : "their base";
						_game->pushState(new GiftNoticeState(getCurrentClientName() + " gifted " + soldierName + " to you at base " + baseName));
						obj["notified"] = true;
					}

				}
				catch (const std::exception& e)
				{
					Log(LOG_INFO) << "[coop-gift] RECV display-copy failed: " << e.what();
				}

				_pendingIncomingGifts.push_back(obj);

			}
			else if (obj.isMember("soldier_yaml")
			         && (_game->getCoopMod()->coopMissionEnd
			             || _game->getSavedGame()->getSavedBattle()))
			{

				// Coop battle just ended (or is still tearing down): our live
				// SavedGame is still the HOST's battle world - the own-world
				// reload (GeoscapeState::init) has not run yet. Applying the
				// physical gift now would match the giver's real base in
				// that throwaway world (coopBase cleared to -1, soldier deleted
				// by the post-battle cleanup) and the follow-up client-progress
				// push would upload the host world as our own-world blob. Defer
				// and let processPendingSoldierGifts() replay it once our
				// own world is restored (host base is a mirror there, so the
				// soldier correctly stays a guest at station_base_id).
				Log(LOG_INFO) << "[coop-gift] RECV deferred (mission-end swapped world)";
				_pendingIncomingGifts.push_back(obj);

			}
			else if (obj.isMember("soldier_yaml"))
			{

				// Physical transfer: the giver removed the soldier from their
				// save; recreate it in ours, keeping its station base.
				int stationBaseId = obj["station_base_id"].asInt();

				try
				{

					YAML::YamlRootNodeReader reader(YAML::YamlString{obj["soldier_yaml"].asString()}, "giftSoldier");
					auto soldierReader = reader["soldier"];

					std::string type = soldierReader["type"].readVal(_game->getMod()->getSoldiersList().front());

					if (_game->getMod()->getSoldier(type))
					{

						// If the station base is one of OUR real bases, the
						// soldier is coming home: it lives there normally.
						// Otherwise it stays a guest at the giver's base and
						// merely lives in our save (guest-soldier model).
						Base* homeBase = 0;
						Base* firstOwnBase = 0;

						for (auto& base : *_game->getSavedGame()->getBases())
						{

							if (base->_coopBase == false && base->_coopIcon == false)
							{

								if (!firstOwnBase)
								{
									firstOwnBase = base;
								}

								if (base->_coop_base_id == stationBaseId)
								{
									homeBase = base;
									break;
								}

							}

						}

						Base* targetBase = homeBase ? homeBase : firstOwnBase;

						// No own base in the current save = our world is not
						// (yet) loaded, e.g. the transitional frames right
						// after leaving the peer's base view. Never consume
						// the packet in that window - defer and replay.
						if (!targetBase)
						{
							Log(LOG_INFO) << "[coop-gift] RECV deferred (no own base in current save)";
							_pendingIncomingGifts.push_back(obj);
						}
						else
						{

						// Ignore duplicate deliveries via the sender's unique
						// packet id (in-memory: with the host save as the single
						// authority, packets are never re-sent across sessions).
						long long xferId = obj.get("xfer_id", 0).asInt64();
						bool exists = (xferId != 0 && _seenGiftPacketIds.count(xferId) != 0);

						if (xferId != 0)
						{
							_seenGiftPacketIds.insert(xferId);
						}

						Log(LOG_INFO) << "[coop-gift] RECV type=" << type << " exists=" << (exists ? 1 : 0)
						              << " homeBase=" << (homeBase ? homeBase->getName() : "none")
						              << " targetBase=" << targetBase->getName()
						              << " stationBaseId=" << stationBaseId;

						if (!exists)
						{

							Soldier* soldier = new Soldier(_game->getMod()->getSoldier(type), 0, 0 /*nationality*/);
							soldier->load(soldierReader, _game->getMod(), _game->getSavedGame(), _game->getMod()->getScriptGlobal());

							// The peer's soldier ids and ours come from
							// separate saves: on collision give the incoming
							// soldier a fresh id so lookups stay unambiguous.
							int maxId = soldier->getId();

							bool collision = false;

							for (auto& base : *_game->getSavedGame()->getBases())
							{
								for (auto& s : *base->getSoldiers())
								{
									if (s->getId() == soldier->getId())
									{
										collision = true;
									}
									maxId = std::max(maxId, s->getId());
								}
							}

							for (auto& s : *_game->getSavedGame()->getDeadSoldiers())
							{
								if (s->getId() == soldier->getId())
								{
									collision = true;
								}
								maxId = std::max(maxId, s->getId());
							}

							if (collision)
							{
								soldier->setId(maxId + 1);
							}

							soldier->setCraft(0);
							soldier->setCoopBase(homeBase ? -1 : stationBaseId);
							soldier->setOwnerPlayerId(owner);
							soldier->setCoop(owner);

							targetBase->getSoldiers()->push_back(soldier);

							// SoldiersState/CraftSoldiersState restore the
							// roster from these snapshots when they close; if
							// one is open right now (snapshot non-empty), add
							// the soldier there too or the restore drops it.
							if (!targetBase->base_oldsoldiers.empty())
							{
								targetBase->base_oldsoldiers.push_back(soldier);
							}
							if (!targetBase->base_oldsoldiers2.empty())
							{
								targetBase->base_oldsoldiers2.push_back(soldier);
							}

							Log(LOG_INFO) << "[coop-gift] RECV added soldier '" << soldier->getName()
							              << "' id=" << soldier->getId() << " to base '" << targetBase->getName()
							              << "' coopBase=" << soldier->getCoopBase();

							// keep the host-side client blob fresh (no-op on
							// the host itself)
							pushProgressToHostSilently();

							// Tell the new owner (skip if the deferred path
							// already notified during a base visit). The
							// station base's name is the one the player
							// recognizes - for a guest that's the giver's
							// (mirror) base, not the own base holding it.
							if (!obj.get("notified", false).asBool())
							{

								std::string baseName = targetBase->getName();

								for (auto& base : *_game->getSavedGame()->getBases())
								{
									if (base->_coop_base_id == stationBaseId)
									{
										baseName = base->getName();
										break;
									}
								}

								_game->pushState(new GiftNoticeState(getCurrentClientName() + " gifted " + soldier->getName() + " to you at base " + baseName));

							}

						}

						}

					}
					else
					{
						Log(LOG_INFO) << "[coop-gift] RECV unknown soldier type " << type;
					}

				}
				catch (const std::exception& e)
				{
					Log(LOG_INFO) << "[coop-gift] RECV failed to load soldier yaml: " << e.what();
				}

			}
			else
			{

				// Control flip for a soldier deployed in the current battle.
				// The physical move arrives in a later packet once the
				// giver's mission has ended.
				if (_game->getSavedGame()->getSavedBattle())
				{

					auto* battle = _game->getSavedGame()->getSavedBattle();

					for (auto& unit : *battle->getUnits())
					{

						bool match = (unit_id != -1 && unit->getId() == unit_id);

						if (!match && unit->getGeoscapeSoldier() && unit->getGeoscapeSoldier()->getId() == soldier_id)
						{
							match = true;
						}

						if (match)
						{

							unit->setCoop(owner);

							if (unit->getGeoscapeSoldier())
							{
								unit->getGeoscapeSoldier()->setOwnerPlayerId(owner);
								unit->getGeoscapeSoldier()->setCoop(owner);
							}

							int localPlayerId = localSeat();

							if (battle->getSelectedUnit() == unit && owner != localPlayerId)
							{
								battle->selectNextPlayerUnit();
							}

							break;

						}

					}

				}

			}

		}

	}

	if (stateString == "calc_explode_fov")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				int maxRadius = obj["maxRadius"].asInt();
				bool coop_is_second_fov = obj["coop_is_second_fov"].asBool();

				int center_tile_x = obj["center_tile_x"].asInt();
				int center_tile_y = obj["center_tile_y"].asInt();
				int center_tile_z = obj["center_tile_z"].asInt();


				Position center_position = Position(center_tile_x, center_tile_y, center_tile_z);

				_game->getSavedGame()->getSavedBattle()->coopExplosionCalc(center_position, maxRadius, coop_is_second_fov);

			}

		}

	}

	if (stateString == "ufo_popup")
	{

		std::string str_type = obj["type"].asString();
		std::string str_race = obj["race"].asString();

		// Legacy single-slot fields kept for any other reader; the QUEUE is what the
		// geoscape consumes, so simultaneous detections no longer overwrite each other.
		show_coop_ufo_popup_type = str_type;
		show_coop_ufo_popup_race = str_race;

		CoopUfoAlert alert;
		alert.ufoId = obj.get("ufo_id", -1).asInt();
		alert.type = str_type;
		alert.race = str_race;
		coopUfoAlerts.push_back(alert);
		while (coopUfoAlerts.size() > kMaxCoopUfoAlerts)
			coopUfoAlerts.erase(coopUfoAlerts.begin()); // drop oldest, stay bounded

	}

	if (stateString == "mission_popup")
	{

		int mission_id = obj["mission_id"].asInt();

		// issue #78: in SHARED, materialize the site immediately if the snapshot
		// has not delivered it yet (spawn + detection in one host tick, with the
		// host's snapshot sender frozen behind its own dialog). Same create path
		// as the target_positions receiver; think() then matches by id and pops.
		if (isSharedReplica() && _game->getSavedGame() && obj.isMember("rules"))
		{
			SavedGame* sg = _game->getSavedGame();
			MissionSite* site = nullptr;
			for (auto* s : *sg->getMissionSites())
				if (s->getId() == mission_id) { site = s; break; }
			if (!site)
			{
				const RuleAlienMission* srule = _game->getMod()->getAlienMission(obj["rules"].asString(), false);
				AlienDeployment* dep = _game->getMod()->getDeployment(obj["deployment"].asString(), false);
				if (srule && dep)
				{
					site = new MissionSite(srule, dep, nullptr);
					site->setId(mission_id);
					site->setLongitude(obj["lon"].asDouble());
					site->setLatitude(obj["lat"].asDouble());
					site->setAlienRace(obj["race"].asString());
					site->setCity(obj["city"].asString());
					site->setSecondsRemaining((size_t)obj.get("time", 100000000).asUInt64());
					site->setDetected(true);
					sg->getMissionSites()->push_back(site);
					// Keep the main-thread despawn prune from eating it before the
					// next snapshot refreshes the authoritative set.
					std::lock_guard<std::mutex> lk(sharedLiveSiteIdsMutex);
					sharedLiveSiteIds.insert(mission_id);
				}
			}
		}

		show_coop_mission_popup = mission_id;

	}

	// delete_base
	if (stateString == "delete_base")
	{
		// PRD-J07: SEPARATE-only mirror machinery. In SHARED base removal rides the
		// fac_dismantle / base_destroyed shared_apply (keeps base indices in
		// lock-step); a stray delete_base would match a REAL base's random
		// _coop_base_id and desync every index-routed command.
		if (isSharedCampaign())
		{
			return;
		}

		int base_id = obj["base_id"].asInt();

		if (_game->getSavedGame())
		{

			if (auto* sg = _game->getSavedGame())
			{
				auto& bases = *sg->getBases(); // std::vector<Base*>&

				for (auto it = bases.begin(); it != bases.end();)
				{
					Base* b = *it;
					if (b && b->_coop_base_id == base_id)
					{
						delete b;             // free memory permanently
						it = bases.erase(it); // remove from the list; returns next iterator
						break;
					}
					else
					{
						++it;
					}
				}
			}
		}
	}

	// cutscene!
	if (stateString == "cutscene")
	{

		std::string cutsceneId = obj["cutsceneId"].asString();
		int monthsPassed = obj["monthsPassed"].asInt();
		int daysPassed = obj["daysPassed"].asInt();
		int ending = obj["ending"].asInt();

		if (_game->getSavedGame())
		{

			_game->getSavedGame()->setEnding((GameEnding)ending);

			_game->getSavedGame()->setMonthsPassed(monthsPassed);
			_game->getSavedGame()->setMonthsPassed(daysPassed);
		}

		allow_cutscene = false;

		_game->pushState(new CutsceneState(cutsceneId));
	}

	// server full!
	if (stateString == "server_full")
	{

		// PRD-11: server_full is a host->client refusal. onConnect = -1 is the
		// host listen thread's exit condition, so a client that spoofs this
		// message could stop the host's thread. Only act on it as a client.
		if (connectionTCP::session.role == CoopRole::Client)
		{
			onConnect = -1;
			closeConnectingDialog();
			_game->pushState(new CoopState(444));
		}
		else
		{
			Log(LOG_WARNING) << "[coop] ignoring server_full: not a client (role="
				<< (connectionTCP::session.role == CoopRole::Host ? "Host" : "None") << ")";
		}
	}

	if (stateString == "chat_message")
	{

		std::string msg_time = obj["msg_time"].asString();
		std::string msg_player = obj["msg_player"].asString();
		std::string msg_text = obj["msg_text"].asString();

		if (_chatMenu)
		{
			_chatMenu->addMessage(msg_time, msg_player, msg_text);
		}
	}

	if (stateString == "new_game")
	{

		_game->pushState(new NewGameState);

	}

	if (stateString == "request_load_progress")
	{

		if (_game->getSavedGame() && !sendFileClient && isSharedCampaign())
		{

			// PRD-J02: SHARED resume/bootstrap. There is exactly one authoritative
			// world (the host's) and no per-client stored blob - serialize the
			// CURRENT world fresh and stream it as the client's replica. The
			// client adopts it via the same MAP_RESULT_LOAD_PROGRESS path a
			// SEPARATE resume uses.
			streamSharedWorldToClient();
			if (!sendFileClient)
			{
				// serialization refused (unplaced base etc.): let the client retry
				Json::Value busy;
				busy["state"] = "load_progress_busy";
				sendTCPPacketData(busy.toStyledString());
			}
			else
			{
				// P2/F3: SHARED mid-battle resume. The geoscape-phase world is now
				// streaming; if the authoritative world carries a battle, ride the very
				// same two-phase battle stream a SEPARATE resume uses. Arm the pending
				// flag and mark this client eligible so its geoscape resume_ack fires
				// campaign_resume_battle -> SEND_FILE_CLIENT_SAVE -> battlehost (the
				// snapshot chain is mode-agnostic; the SHARED role is derived from
				// getServerOwner() at BattlescapeState:1687). The client's phase-one
				// loadCoopProgress load drops the battleGame, so the geoscape adopt
				// stays battle-free and the battle arrives fresh in phase two.
				connectionTCP::session.resumeBattlePending =
					(_game->getSavedGame()->getSavedBattle() != nullptr);
				if (connectionTCP::session.resumeBattlePending)
				{
					connectionTCP::session.resumeBattleEligible.insert(
						_game->getCoopMod()->getCurrentClientName());
				}
			}
		}
		else if (_game->getSavedGame() && !sendFileClient)
		{

			// battle live: after the geoscape world ack, stream the battle
			// (F3 battle-save resume + F4 mid-battle rejoin share this)
			connectionTCP::session.resumeBattlePending = (_game->getSavedGame()->getSavedBattle() != nullptr);

			// PRD-09 C12: F3 battle-save resume in a fresh process. Unlike the
			// live mission-start path (SEND_FILE_CLIENT_TRUE stashes
			// coop_geoscape_return before dispatching the mission), nothing
			// regenerates that snapshot on resume - so the server owner's
			// mission-end restore would keep the peer-derived battle world as its
			// campaign. Stash the loaded world's geoscape now, while
			// getServerOwner() is true (so it lands in coopFilesHost where the
			// restore reads it). Detach the battle first: coop_geoscape_return
			// must be a PURE geoscape - the restore reloads it as the continuation
			// world and re-entering it must not resurrect the just-finished battle.
			if (connectionTCP::session.resumeBattlePending && getServerOwner() == true && _game->getCoopMod()->getCoopCampaign() == true)
			{
				SavedBattleGame* keepBattle = _game->getSavedGame()->detachBattleGame();
				_game->getSavedGame()->saveCoopToMemory("coop_geoscape_return", _game->getMod(), "coop_geoscape_return");
				_game->getSavedGame()->reattachBattleGame(keepBattle);
				Log(LOG_INFO) << "[coop] F3 battle resume: stashed geoscape-only coop_geoscape_return for mission-end restore";
			}

			std::string filename = hostBlobKey(_game->getCoopMod()->getCurrentClientName());

			bool blobFound = false;
			{
				std::lock_guard<std::mutex> lock(coopFilesMutex);
				auto it = coopFilesHost.find(filename);
				if (it != coopFilesHost.end() && !it->second.empty())
				{
					// found! snapshot the blob for the streamer thread
					sendProgressLoadBlob = it->second;
					blobFound = true;
				}
			}

			if (!blobFound)
			{

				// registered player without a stored world: fresh world with
				// the campaign's difficulty + base building (D2/D6)
				Json::Value root = buildCampaignStartPacket(_game->getSavedGame());

				sendTCPPacketData(root.toStyledString());

			}
			else
			{

				// PRD-11 C8: this client is being served a real resume world;
				// mark it eligible for the follow-up battle stream. The no-blob
				// campaign_start branch above deliberately does NOT.
				connectionTCP::session.resumeBattleEligible.insert(
					_game->getCoopMod()->getCurrentClientName());

				sendFileClient = true;
				sendProgressLoadFileToClient = filename;

			}

		}
		else if (_game->getSavedGame() && sendFileClient)
		{

			// PRD-11 C13: the streamer is busy with another transfer. Never drop
			// the request silently (the client's load-wait dialog has no timeout);
			// tell it to retry.
			Json::Value busy;
			busy["state"] = "load_progress_busy";
			sendTCPPacketData(busy.toStyledString());

		}

	}

	if (stateString == "sendProgressSaveRequest")
	{

		long long saveID = obj["saveID"].asInt64();
		connectionTCP::saveID = saveID;

		sendSaveProgressFile();

	}

	if (stateString == "sendCraft")
	{

		setHost(false);

		CoopState* coopWindow = new CoopState(4);
		_game->pushState(coopWindow);

		sendMissionFile();
	}

	if (stateString == "craft_list")
	{

		size_t id = obj["selected_craft_id"].asUInt();

		_coop_selected_craft_id = id;
	}

	if (stateString == "change_unit_name")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{

				int unit_id = obj["unit_id"].asInt();
				std::string unit_name = obj["unit_name"].asString();

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getId() == unit_id)
					{

						unit->setName(unit_name);

						if (unit->getGeoscapeSoldier())
						{

							unit->getGeoscapeSoldier()->setName(unit_name);

						}

					}
				}

			}
		}

	}

	if (stateString == "motion_scan")
	{
		if (_game->getSavedGame() && _game->getSavedGame()->getSavedBattle())
		{
			int unit_id = obj["unit_id"].asInt();
			int turn = obj["turn"].asInt();

			for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
			{
				if (unit->getId() == unit_id)
				{
					unit->setScannedTurn(turn);
					break;
				}
			}
		}
	}

	if (stateString == "abortPath")
	{

		int unit_id = obj["unit_id"].asInt();

		int x = obj["x"].asInt();
		int y = obj["y"].asInt();
		int z = obj["z"].asInt();

		int setDirection = obj["setDirection"].asInt();
		int setFaceDirection = obj["setFaceDirection"].asInt();

		int setTurretDirection = obj["setTurretDirection"].asInt();
		int setTurretToDirection = obj["setTurretToDirection"].asInt();

		BattleUnit *selected_unit = 0;

		// abort path
		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{

				AbortCoopWalk = true;

				_game->getSavedGame()->getSavedBattle()->getBattleGame()->abortCoopPath(x, y, z, unit_id, setDirection, setFaceDirection);

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getId() == unit_id)
					{

						selected_unit = unit;
						unit->setDirection(setDirection);
						unit->setFaceDirection(setFaceDirection);

						unit->setDirectionTurretCoop(setTurretDirection);
						unit->setTurretToDirectionCoop(setTurretToDirection);

						_game->getSavedGame()->getSavedBattle()->getBattleGame()->teleport(x, y, z, unit);

						break;
					}
				}
		
			}
		}

		// visible units
		for (int j = 0; j < obj["visible_units"].size(); j++)
		{

			int v_unit_id = obj["visible_units"][j]["unit_id"].asInt();

			BattleUnit *visible_unit = 0;

			if (_game->getSavedGame())
			{

				if (_game->getSavedGame()->getSavedBattle())
				{

					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{

						if (unit->getId() == v_unit_id)
						{

							visible_unit = unit;
							break;

						}


					}

					if (visible_unit && selected_unit)
					{

						selected_unit->addToVisibleUnits(visible_unit);

					}


				}

			}

	
		}
		

	}

	if (stateString == "cancelCurrentAction")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{

				_game->getSavedGame()->getSavedBattle()->getBattleGame()->cancelCurrentActionCoop();
			}
		}
	}

	// CHANGE THE BASE NAME
	if (stateString == "changeBaseName")
	{
		// PRD-J07: SEPARATE-only (renames a _coopIcon mirror + the basehost memory
		// blob). SHARED renames ride the base_rename shared_apply.
		if (isSharedCampaign())
		{
			return;
		}

		std::string old_name = obj["oldName"].asString();
		std::string new_name = obj["newName"].asString();

		for (auto base : *_game->getSavedGame()->getBases())
		{

			// change the base icon name
			if (old_name == base->getName() && base->_coopIcon == true)
			{

				base->setName(new_name);

				break;
			}
		}

		std::string filename = "basehost";

		SavedGame* file_units = new SavedGame();

		bool save = false;

		file_units->loadCoopSaveFromMemory(filename, _game->getMod(), _game->getLanguage(), filename);

		for (auto& base : *file_units->getBases())
		{

			if (base->getName() == old_name)
			{

				base->setName(new_name);
				save = true;
				break;
			}
		}

		if (save)
		{
			file_units->saveCoopToMemory(filename, _game->getMod(), filename);
		}
		
	}

	// transfer
	if (stateString == "transfer_completed")
	{

		int base_to_id = obj["base_to_id"].asInt();
		int base_from_id = obj["base_from_id"].asInt();
		int total_funds = obj["total_funds"].asInt();

		Base* baseFrom = 0;
		Base* baseTo = 0;

		if (!_game->getSavedGame())
		{
			_game->pushState(new CoopState(551));
			return;
		}
		
		for (auto& base : *_game->getSavedGame()->getBases())
		{
			if (base->_coop_base_id == base_from_id)
			{
				baseFrom = base;
				break;
			}
		}

		for (auto& base : *_game->getSavedGame()->getBases())
		{
			if (base->_coop_base_id == base_to_id)
			{
				baseTo = base;
				break;
			}
		}

		if (baseFrom && baseTo)
		{
			// TransferItemsState has two senders for this same "transfer" packet:
			//   createPendingTransfers() - ACK-gated. Nothing left the source base;
			//     this ACK is what applies the funds debit, the store removal and
			//     the co-op limit decrements (via removePendingTransfers()).
			//   completeTransfer()       - immediate. All of that already happened
			//     locally when the trade was made, so re-applying it here debits
			//     everything twice; the store re-validation inside
			//     removePendingTransfers() then finds nothing left to remove and
			//     fails, leaving the trade half-applied (the peer has the goods,
			//     this base keeps them AND keeps a stale pending list that the next
			//     trade would re-send).
			// The flag rides the packet and comes back untouched in this ACK.
			bool alreadyApplied = obj.get("already_applied", false).asBool();

			if (alreadyApplied || baseFrom->removePendingTransfers(baseTo->getTransfers()))
			{

				if (!alreadyApplied)
				{
					_game->getSavedGame()->setFunds(_game->getSavedGame()->getFunds() - total_funds);

					baseTo->decreaseCoopTransferLimits();
				}

				for (Transfer* transfer : *baseTo->getTransfers())
				{
					// ~Transfer deletes the soldier/craft it carries. Neither path
					// removes a transferred SOLDIER from the source base (it stays
					// on as a guest of the peer base, see completeTransfer()), and
					// removePendingTransfers() keeps crew soldiers too - so hand the
					// object back before freeing the Transfer, or the base is left
					// holding a dangling pointer.
					if (transfer)
					{
						Soldier* keptSoldier = transfer->getSoldier();
						if (keptSoldier
							&& std::find(baseFrom->getSoldiers()->begin(), baseFrom->getSoldiers()->end(),
								keptSoldier) != baseFrom->getSoldiers()->end())
						{
							transfer->setSoldier(nullptr);
						}
						Craft* keptCraft = transfer->getCraft();
						if (keptCraft
							&& std::find(baseFrom->getCrafts()->begin(), baseFrom->getCrafts()->end(),
								keptCraft) != baseFrom->getCrafts()->end())
						{
							transfer->setCraft(nullptr);
						}
					}
					delete transfer;
				}
				baseTo->getTransfers()->clear();

			}
			else
			{
				_game->pushState(new CoopState(552));
			}

		}
		else
		{
			_game->pushState(new CoopState(553));
		}

	}

	// purchase
	if (stateString == "purchase_completed")
	{

		int total_funds = obj["total_funds"].asInt();
		_game->getCoopMod()->coopFunds = _game->getCoopMod()->coopFunds - total_funds;

	}

	if (stateString == "transfer_failed" || stateString == "purchase_failed")
	{
		_game->pushState(new CoopState(551));
	}

	// Transfer and purchase
	// COOP living quarters: the peer reports how many of ITS soldiers are
	// stationed at each of OUR bases (see sendGuestCensus). We hold no Soldier
	// object for them, so this headcount is the only way they can occupy the
	// living quarters of the base they actually live in. Absent bases are reset,
	// so the census is always a full replacement, never a delta.
	if (stateString == "guest_census")
	{
		if (_game->getSavedGame())
		{
			std::map<int, int> reported;
			const Json::Value& list = obj["bases"];
			if (list.isArray())
			{
				for (const auto& e : list)
					reported[e.get("base_id", 0).asInt()] = e.get("guests", 0).asInt();
			}
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				if (base->_coopBase || base->_coopIcon)
					continue;
				auto it = reported.find(base->_coop_base_id);
				base->coop_guests = (it != reported.end()) ? it->second : 0;
			}
		}
	}

	if (stateString == "purchase" || stateString == "transfer")
	{

		// Resolve the target base BEFORE acknowledging. updateCoopTask() only
		// applies a queued trade to a base whose _coop_base_id == base_to_id;
		// if no local base matches, the trade is silently retained forever while
		// the sender - told "*_completed" - has already removed its goods. So a
		// missing target base must be rejected here, not accepted (silent loss).
		int base_to_id = obj.get("base_to_id", 0).asInt();
		Base* targetBase = nullptr;
		if (_game->getSavedGame())
		{
			for (auto& base : *_game->getSavedGame()->getBases())
			{
				if (base->_coop_base_id == base_to_id)
				{
					targetBase = base;
					break;
				}
			}
		}

		// Check whether the transfer or purchase data is valid.
		if (targetBase &&
			obj.isMember("items") &&
			!obj["items"].empty())
		{
			// The request is valid.
			waitedTrades.append(obj);

			// Send a success response to the other player.
			if (stateString == "transfer")
			{
				Json::Value obj2;
				obj2["state"] = "transfer_completed";
				obj2["base_to_id"] = obj.get("base_to_id", 0);
				obj2["base_from_id"] = obj.get("base_from_id", 0);
				obj2["total_funds"] = obj.get("total_funds", 0);
				// echoed straight back so the sender can tell its own immediate
				// (already-applied) trades from the ACK-gated ones
				obj2["already_applied"] = obj.get("already_applied", false);
				sendTCPPacketData(obj2.toStyledString());
			}
			else
			{
				Json::Value obj3;
				obj3["state"] = "purchase_completed";
				obj3["total_funds"] = obj.get("total_funds", 0);
				sendTCPPacketData(obj3.toStyledString());
			}

		}
		else
		{
			// The request is invalid.
			// Send a failure response to the other player.
			if (stateString == "transfer")
			{
				Json::Value obj4;
				obj4["state"] = "transfer_failed";
				obj4["base_to_id"] = obj.get("base_to_id", 0);
				obj4["base_from_id"] = obj.get("base_from_id", 0);
				sendTCPPacketData(obj4.toStyledString());
			}
			else
			{
				Json::Value obj5;
				obj5["state"] = "purchase_failed";
				sendTCPPacketData(obj5.toStyledString());
			}
		}
	}

	if (stateString == "selected_unit")
	{

		int actor_id = obj["actor_id"].asInt();

		int reverse = obj["reverse"].asInt();

		bool kneel = obj["kneel"].asBool();

		if (_game->getSavedGame()->getSavedBattle())
		{
			if (_game->getSavedGame()->getSavedBattle()->getBattleState())
			{
				_game->getSavedGame()->getSavedBattle()->getBattleState()->setSelectedCoopUnit(actor_id);

				_game->getSavedGame()->getSavedBattle()->setKneelReserved(kneel);
				_game->getSavedGame()->getSavedBattle()->setTUReserved((BattleActionType)reverse);
			}
		}
	}

	if (stateString == "time")
	{

		if (getServerOwner() == false)
		{
			int weekday = obj["weekday"].asInt();
			int day = obj["day"].asInt();
			int month = obj["month"].asInt();
			int year = obj["year"].asInt();
			int hour = obj["hour"].asInt();
			int minute = obj["minute"].asInt();
			int second = obj["second"].asInt();

			connectionTCP::_weekday = weekday;
			connectionTCP::_day = day;
			connectionTCP::_month = month;
			connectionTCP::_year = year;
			connectionTCP::_hour = hour;
			connectionTCP::_minute = minute;
			connectionTCP::_second = second;

			int monthsPassed = obj["monthsPassed"].asInt();
			int daysPassed = obj["daysPassed"].asInt();

			connectionTCP::monthsPassed = monthsPassed;
			connectionTCP::daysPassed = daysPassed;

			// PRD-J04: verify the host's world checksum piggybacked on this
			// heartbeat (funds + base count + research count). Log-only detect;
			// repair is J10. No-op unless the host stamped a SHARED checksum.
			SharedEcon::verifyWorldChecksum(_game, obj);

		}

		std::string time_speed = obj["time_speed"].asString();
		other_time_speed_coop = time_speed;
		// Persistent copy for the geoscape ally-speed indicator (other_time_speed_coop
		// is cleared every timeAdvance, so it can't drive the UI on its own).
		peerTimeSpeedId = time_speed;
		// A "time" packet is emitted every geoscape think() and carries where on the
		// geoscape the sender is: -1 = normal (ally marker tracks their speed), 0 = an
		// open dogfight window (marker -> Intercept). Navigating to a sub-screen stops
		// these packets, so the last dedicated geo_focus value sticks instead.
		peerFocusScreen = obj.get("geo_focus", -1).asInt();
		// Peer heartbeat (both sides): note when we last heard from the peer on the
		// geoscape. The host's timeAdvance() freezes the shared clock when this goes
		// stale, and both sides dim the ally marker to yellow when it does.
		lastPeerTimePacketMs = SDL_GetTicks();

	}

	if (stateString == "geo_focus")
	{
		// coop: the peer navigated to a geoscape sub-screen (0..5 toolbar index). The
		// ally marker on our geoscape moves to that toolbar button; -1 (back on the
		// geoscape) is restored by the next "time" packet.
		peerFocusScreen = obj["screen"].asInt();
	}

	if (stateString == "changeHost")
	{

		setHost(false);
	}

	if (stateString == "changeHost3")
	{

		setPlayerTurn(1);
		setHost(true);
	}

	if (stateString == "changeHost4")
	{

		setHost(false);
	}

	if (stateString == "research")
	{

		waitedResearch.append(obj);

	}

	if (stateString == "add_coop_item")
	{

		if (_game->getSavedGame())
		{

			int coopbase_id = obj["coopbase_id"].asInt();
			int craft_id = obj["craft_id"].asInt();
			std::string craft_type = obj["craft_type"].asString();

			Base* current_base = 0;
			Craft* current_craft = 0;

			for (auto& base : *_game->getSavedGame()->getBases())
			{

				if (base->_coop_base_id == coopbase_id)
				{
					current_base = base;

					for (auto& craft : *base->getCrafts())
					{

						if (craft->getId() == craft_id && craft->getRules()->getType() == craft_type)
						{
							current_craft = craft;
							break;
						}
					}

					break;
				}
			}

			if (current_base && current_craft)
			{

				auto& coopItems = current_craft->getCoopItems();

				int item_coop_id = obj["item_coop_id"].asInt();
				bool coopbase = obj["coopbase"].asInt();
				std::string item_type = obj["item_type"].asString();

				// exists?
				bool item_exists = false;
				for (const auto& ci : coopItems)
				{
					if (ci.id == item_coop_id &&
						ci.type == item_type &&
						ci.owner == !coopbase)
					{
						item_exists = true;
						break;
					}
				}

				if (!item_exists)
				{
					coopItems.push_back({item_coop_id, item_type, !coopbase});
				}

			}
			else
			{
				jsonAddedCoopItems.append(obj);
			}
		}

	}

	if (stateString == "request_coop_items")
	{

		if (_game->getSavedGame())
		{

			int coopbase_id = obj["coopbase_id"].asInt();
			int craft_id = obj["craft_id"].asInt();
			std::string craft_type = obj["craft_type"].asString();

			Base* current_base = 0;
			Craft* current_craft = 0;

			for (auto& base : *_game->getSavedGame()->getBases())
			{

				if (base->_coop_base_id == coopbase_id)
				{
					current_base = base;

					for (auto& craft : *base->getCrafts())
					{

						if (craft->getId() == craft_id && craft->getRules()->getType() == craft_type)
						{
							current_craft = craft;
							break;
						}
					}

					break;
				}
			}

			if (current_base && current_craft)
			{

				auto& coopItems = current_craft->getCoopItems();

				// save
				Json::Value obj;
				obj["state"] = "save_coop_items";

				obj["coopbase_id"] = coopbase_id;
				obj["craft_id"] = craft_id;
				obj["craft_type"] = craft_type;

				int item_index = 0;
				for (auto& coopItem : coopItems)
				{

					obj["coopItems"][item_index]["id"] = coopItem.id;
					obj["coopItems"][item_index]["type"] = coopItem.type;
					obj["coopItems"][item_index]["owner"] = coopItem.owner;

					item_index++;
				}

				sendTCPPacketData(obj.toStyledString());

			}

		}



	}

	if (stateString == "save_coop_items")
	{

		if (_game->getSavedGame())
		{
		
			int coopbase_id = obj["coopbase_id"].asInt();
			int craft_id = obj["craft_id"].asInt();
			std::string craft_type = obj["craft_type"].asString();

			Base *current_base = 0;
			Craft* current_craft = 0;

			for (auto& base : *_game->getSavedGame()->getBases())
			{

				if (base->_coop_base_id == coopbase_id)
				{
					current_base = base;

					for (auto& craft : *base->getCrafts())
					{

						if (craft->getId() == craft_id && craft->getRules()->getType() == craft_type)
						{
							current_craft = craft;
							break;
						}

					}

					break;

				}

			}

			if (current_base && current_craft)
			{

				const auto& arr = obj["coopItems"];
				auto& coopItems = current_craft->getCoopItems();

				std::vector<CoopItem> newItems;

				newItems.reserve(arr.size());

				for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
				{
					const auto& it = arr[i];

					int item_id = it["id"].asInt();
					std::string item_type = it["type"].asString();
					bool item_owner = it["owner"].asBool();

					newItems.push_back({item_id, item_type, item_owner});
				}

				// overwrite
				coopItems.swap(newItems);

				connectionTCP::moveCoopItems = true;

			}

		}

	}

	if (stateString == "Inventory")
	{

		std::string inv_id = obj["inv_id"].asString();
		int inv_x = obj["inv_x"].asInt();
		int inv_y = obj["inv_y"].asInt();
		int unit_id = obj["unit_id"].asInt();
		int item_id = obj["item_id"].asInt();
		int move_cost = obj["move_cost"].asInt();
		int slot_x = obj["slot_x"].asInt();
		int slot_y = obj["slot_y"].asInt();

		int getHealQuantity = obj["getHealQuantity"].asInt();
		int getPainKillerQuantity = obj["getPainKillerQuantity"].asInt();
		int getStimulantQuantity = obj["getStimulantQuantity"].asInt();
		int getFuseTimer = obj["getFuseTimer"].asInt();
		int getXCOMProperty = obj["getXCOMProperty"].asBool();
		int isAmmo = obj["isAmmo"].asBool();
		int isWeaponWithAmmo = obj["isWeaponWithAmmo"].asBool();
		int isFuseEnabled = obj["isFuseEnabled"].asBool();
		int getAmmoQuantity = obj["getAmmoQuantity"].asInt();

		std::string item_name = obj["item_name"].asString();

		int tile_x = obj["tile_x"].asInt();
		int tile_y = obj["tile_y"].asInt();
		int tile_z = obj["tile_z"].asInt();

		// new!!!
		bool coopbase = obj["coopbase"].asBool();
		bool other_coop_inventory = obj["other_coop_inventory"].asBool();
		int coopbase_id = obj["coopbase_id"].asInt();
		int craft_id = obj["craft_id"].asInt();
		std::string craft_type = obj["craft_type"].asString();
		int slot_type_int = obj["slot_type"].asInt();
		std::string item_type = obj["item_type"].asString();
		int item_slot_type = obj["item_slot_type"].asInt();

		const auto& coopItemsArr = obj["coopItems"];
		int coop_item_id = obj["coop_item_id"].asInt();
		const auto& ammosArr = obj["ammos"];
		bool tu = obj["tu"].asBool();

		int sel_item_id = obj["sel_item_id"].asInt();
		std::string sel_item_type = obj["sel_item_type"].asString();

		bool unload_weapon = obj["unload_weapon"].asBool();

		// battle
		if (coopInventory == true && _game->getSavedGame()->getSavedBattle())
		{

			if (other_coop_inventory == true)
			{
			
				std::string ammos = "";

				if (!ammosArr.isNull())
				{

					ammos = ammosArr.toStyledString();
				}

				_game->getSavedGame()->getSavedBattle()->getBattleState()->moveCoopInventory(ammos, item_name, inv_id, inv_x, inv_y, unit_id, item_id, move_cost, slot_x, slot_y, getHealQuantity, getPainKillerQuantity, getStimulantQuantity, getFuseTimer, getXCOMProperty, isAmmo, isWeaponWithAmmo, isFuseEnabled, getAmmoQuantity, tile_x, tile_y, tile_z, tu, sel_item_id, sel_item_type, unload_weapon);

			}
	
		}
		// base
		else if (other_coop_inventory == false)
		{

			bool found = false;

			if (_game->getSavedGame()->getSavedBattle())
			{

				std::string coopItems = "";

				if (!coopItemsArr.isNull())
				{

					coopItems = coopItemsArr.toStyledString();

				}

				found = _game->getSavedGame()->getSavedBattle()->moveBaseCoopInventory(item_type, coop_item_id, coopbase_id, craft_id, craft_type, slot_type_int, item_slot_type, coopItems);

			}

			if (found == false)
			{
				// later...
				_jsonInventory.append(obj);
			}

		}
		else
		{
			// later...
			_jsonInventory.append(obj);
		}

	}

	if (stateString == "GamePausedON")
	{

		if (gamePaused == 0)
		{
			gamePaused = 2;
			setPlayerTurn(1);
		}

		if (onTcpHost == true)
		{

			_waitBC = false;
		}
		else
		{

			_waitBH = false;
		}
	}

	if (stateString == "GamePausedOFF")
	{

		if (onTcpHost == true)
		{

			_waitBC = true;
		}
		else
		{

			_waitBH = true;
		}

		setPlayerTurn(gamePaused);
		gamePaused = 0;
	}

	if (stateString == "TU_COOP")
	{
		int reverse = obj["reverse"].asInt();

		_game->getSavedGame()->getSavedBattle()->setTUReserved((BattleActionType)reverse);
	}

	if (stateString == "kneel_reserved")
	{
		bool battle_action = obj["battle_action"].asBool();

		_game->getSavedGame()->getSavedBattle()->setKneelReserved(battle_action);
	}

	if (stateString == "kneel")
	{

		int id = obj["id"].asInt();
		BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();
		battlestate->toggeCoopKneel(id);
	}

	if (stateString == "BattleScapeMove")
	{

		AbortCoopWalk = false;

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

					std::string jsonString = obj.toStyledString(); // Converts the entire JSON object

					battlestate->movePlayerTarget(jsonString);

				}
			}
		}

	}

	if (stateString == "psi_attack")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

					std::string jsonString = obj.toStyledString(); // Converts the entire JSON object

					battlestate->psi_attack(jsonString);

				}
			}
		}


	}

	if (stateString == "melee_attack")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

					std::string jsonString = obj.toStyledString(); // Converts the entire JSON object

					battlestate->melee_attack(jsonString);

				}
			}
		}

	}

	if (stateString == "afterBattlescapeUnitTurn")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

					std::string jsonString = obj.toStyledString(); // Converts the entire JSON object

					battlestate->turnPlayerTargetAfter(jsonString);
				}
			}
		}

	}

	if (stateString == "turnBattlescapeUnit")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

					std::string jsonString = obj.toStyledString(); // Converts the entire JSON object

					battlestate->turnPlayerTarget(jsonString);

				}
			}
		}

	}

	if (stateString == "place_facility")
	{
		// PRD-J07: SEPARATE-only mirror markers; SHARED builds ride fac_build.
		if (playerInsideCoopBase == true && !isSharedCampaign())
		{

			_coopFacility.append(obj);

		}

	}

	if (stateString == "dismantle_facility")
	{
		// PRD-J07: SEPARATE-only mirror markers; SHARED rides fac_dismantle.
		if (playerInsideCoopBase == true && !isSharedCampaign())
		{

			_deleteCoopFacility.append(obj);

		}

	}

	if (stateString == "psi_press")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{
					_game->getSavedGame()->getSavedBattle()->getBattleState()->coopPsiButtonAction();
				}
			}
		}
	}

	if (stateString == "ProjectileFlyBState")
	{

		_hasHitUnit = -1;

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

					std::string jsonString = obj.toStyledString(); // Converts the entire JSON object

					battlestate->shootPlayerTarget(jsonString);

				}
			}
		}

	}

	if (stateString == "active_grenade")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					int actor_id = obj["actor_id"].asInt();
					int type = obj["type"].asInt();
					std::string hand = obj["hand"].asString();

					bool fusetimer = obj["fusetimer"].asInt();

					int item_id = obj["item_id"].asInt();

					_game->getSavedGame()->getSavedBattle()->getBattleState()->coopActiveGranade(actor_id, type, hand, fusetimer, item_id);

				}
			}
		}

	}

	if (stateString == "action_click")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					int actor_id = obj["actor_id"].asInt();
					int type = obj["type"].asInt();
					std::string hand = obj["hand"].asString();

					bool fuse = obj["fuse"].asBool();
					bool fusetimer = obj["fusetimer"].asInt();

					int target_x = obj["target_x"].asInt();
					int target_y = obj["target_y"].asInt();
					int target_z = obj["target_z"].asInt();

					int time = obj["time"].asInt();

					std::string weapon_type = obj["weapon_type"].asString();
					int weapon_id = obj["weapon_id"].asInt();

					_game->getSavedGame()->getSavedBattle()->getBattleState()->coopActionClick(actor_id, hand, type, fuse, fusetimer, target_x, target_y, target_z, time, weapon_type, weapon_id);

				}
			}
		}


	}

	if (stateString == "unit_action")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
		
				int actor_id = obj["actor_id"].asInt();

				for (auto* unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getId() == actor_id)
					{
						_game->getSavedGame()->getSavedBattle()->setSelectedUnit(unit);
						break;
					}
				}
			}
		}

	}

	// medkit
	if (stateString == "medkit")
	{

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{

					int actor_id = obj["actor_id"].asInt();
					int type = obj["type"].asInt();
					int part = obj["part"].asInt();
					int time = obj["time"].asInt();
					std::string medkit_state = obj["medkit_state"].asString();
					std::string action_result = obj["action_result"].asString();

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

					battlestate->coopHealing(actor_id, type, part, medkit_state, action_result, time);

				}
			}
		}

	}

	// info box
	if (stateString == "info_box")
	{

		_hasHitUnit = -1;

		std::string msg = obj["msg"].asString();

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{ 

				_game->getSavedGame()->getSavedBattle()->getBattleGame()->infoboxCoop(msg);

			}

		}
	}


	// info box ok
	if (stateString == "info_box_ok")
	{

		_hasHitUnit = -1;

		std::string msg = obj["msg"].asString();

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				_game->getSavedGame()->getSavedBattle()->getBattleGame()->infoboxOkCoop(msg);
			}
		}
	}

	if (stateString == "convertUnit")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					int unit_id = obj["unit_id"].asInt();

					// Check if the same unit
					if (unit->getId() == unit_id)
					{

						bool respawn = obj["respawn"].asBool();
						int spawnUnitFactionInt = obj["spawn_unit_faction"].asInt();
						std::string spawnUnitType = obj["spawn_unit_type"].asString();
	

						unit->setRespawn(respawn);
						unit->setSpawnUnitFaction((UnitFaction)spawnUnitFactionInt);

						auto* spawnType = _game->getSavedGame()->getSavedBattle()->getMod()->getUnit(spawnUnitType);
						unit->setSpawnUnit(spawnType);

						_game->getSavedGame()->getSavedBattle()->convertUnit(unit);

						break;
					}
				}
			}
		}

	}

	if (stateString == "after_unit_death")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					int unit_id = obj["unit_id"].asInt();

					// Check if the same unit
					if (unit->getId() == unit_id)
					{

						int time = obj["time"].asInt();
						int health = obj["health"].asInt();
						int energy = obj["energy"].asInt();
						int morale = obj["morale"].asInt();
						int mana = obj["mana"].asInt();
						int stunlevel = obj["stunlevel"].asInt();
						int motionpoints = obj["motionpoints"].asInt();

						int setDirection = obj["setDirection"].asInt();
						int setFaceDirection = obj["setFaceDirection"].asInt();

						bool respawn = obj["respawn"].asBool();
						unit->setRespawn(respawn);

						unit->setDirection(setDirection);
						unit->setFaceDirection(setFaceDirection);

						unit->setMotionPointsCoop(motionpoints);
						unit->setTimeUnits(time);
						unit->setHealth(health);
						unit->setCoopMorale(morale);
						unit->setCoopEnergy(energy);
						unit->setCoopMana(mana);

						unit->setStunlevelCoop(stunlevel);
						
						int status_int = obj["status"].asInt();
						UnitStatus unitStatus = intToUnitstatus(status_int);
						unit->setCoopStatus(unitStatus);

						// TILE
						bool isTile = obj["isTile"].asBool();

						if (!isTile)
						{

							if (unit->getStatus() != STATUS_DEAD && unit->getStatus() != STATUS_UNCONSCIOUS)
							{
								unit->setCoopStatus(STATUS_DEAD);
							}

							unit->setTile(nullptr, _game->getSavedGame()->getSavedBattle());
						}

						if (!unit->getTile() && unit->getStatus() != STATUS_DEAD && unit->getStatus() != STATUS_UNCONSCIOUS)
						{
							unit->setCoopStatus(STATUS_DEAD);
						}

						break;

					}
				}
			}
		}

	}

	if (stateString == "selfDestruct")
	{

		
		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					int unit_id = obj["unit_id"].asInt();

					// Check if the same unit
					if (unit->getId() == unit_id)
					{

						unit->damageCoop(_game->getSavedGame()->getSavedBattle());

						break;

					}
				}

			}
		}

	}

	// hit tile
	if (stateString == "hit_tile")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				if (_game->getSavedGame()->getSavedBattle()->getBattleGame())
				{

					int center_x = obj["center_x"].asInt();
					int center_y = obj["center_y"].asInt();
					int center_z = obj["center_z"].asInt();

					int power = obj["power"].asInt();
					int damageType_int = obj["damageType"].asInt();
					bool rangeAtack = obj["rangeAtack"].asBool();
					int terrainMeleeTilePart = obj["terrainMeleeTilePart"].asInt();

					uint64_t seed = obj["seed"].asUInt64();
					int smokeRNG = obj["smokeRNG"].asInt();
					_smokeRNGs.push_back(smokeRNG);

					float ArmorEffectiveness = obj["ArmorEffectiveness"].asFloat();
					bool FireBlastCalc = obj["FireBlastCalc"].asBool();
					int FireThreshold = obj["FireThreshold"].asInt();
					int FixRadius = obj["FixRadius"].asInt();
					bool IgnoreDirection = obj["IgnoreDirection"].asBool();
					bool IgnoreNormalMoraleLose = obj["IgnoreNormalMoraleLose"].asBool();
					bool IgnoreOverKill = obj["IgnoreOverKill"].asBool();
					bool IgnorePainImmunity = obj["IgnorePainImmunity"].asBool();
					bool IgnoreSelfDestruct = obj["IgnoreSelfDestruct"].asBool();
					float RadiusEffectiveness = obj["RadiusEffectiveness"].asFloat();
					float RadiusReduction = obj["RadiusReduction"].asFloat();
					bool RandomArmor = obj["RandomArmor"].asBool();
					bool RandomArmorPre = obj["RandomArmorPre"].asBool();
					bool RandomEnergy = obj["RandomEnergy"].asBool();
					bool RandomHealth = obj["RandomHealth"].asBool();
					bool RandomItem = obj["RandomItem"].asBool();
					bool RandomMana = obj["RandomMana"].asBool();
					bool RandomMorale = obj["RandomMorale"].asBool();
					bool RandomStun = obj["RandomStun"].asBool();
					bool RandomTile = obj["RandomTile"].asBool();
					bool RandomTime = obj["RandomTime"].asBool();
					int RandomType = obj["RandomType"].asInt();
					bool RandomWound = obj["RandomWound"].asBool();
					int ResistType = obj["ResistType"].asInt();
					int SmokeThreshold = obj["SmokeThreshold"].asInt();
					int TileDamageMethod = obj["TileDamageMethod"].asInt();
					float ToArmor = obj["ToArmor"].asFloat();
					float ToArmorPre = obj["ToArmorPre"].asFloat();
					float ToEnergy = obj["ToEnergy"].asFloat();
					float ToHealth = obj["ToHealth"].asFloat();
					float ToItem = obj["ToItem"].asFloat();
					float ToMana = obj["ToMana"].asFloat();
					float ToMorale = obj["ToMorale"].asFloat();
					float ToStun = obj["ToStun"].asFloat();
					float ToTile = obj["ToTile"].asFloat();
					float ToWound = obj["ToWound"].asFloat();

					RuleDamageType* dmg = new RuleDamageType();

					dmg->ArmorEffectiveness = ArmorEffectiveness;
					dmg->FireBlastCalc = FireBlastCalc;
					dmg->FireThreshold = FireThreshold;
					dmg->FixRadius = FixRadius;
					dmg->IgnoreDirection = IgnoreDirection;
					dmg->IgnoreNormalMoraleLose = IgnoreNormalMoraleLose;
					dmg->IgnoreOverKill = IgnoreOverKill;
					dmg->IgnorePainImmunity = IgnorePainImmunity;
					dmg->IgnoreSelfDestruct = IgnoreSelfDestruct;
					dmg->RadiusEffectiveness = RadiusEffectiveness;
					dmg->RadiusReduction = RadiusReduction;
					dmg->RandomArmor = RandomArmor;
					dmg->RandomArmorPre = RandomArmorPre;
					dmg->RandomEnergy = RandomEnergy;
					dmg->RandomHealth = RandomHealth;
					dmg->RandomItem = RandomItem;
					dmg->RandomMana = RandomMana;
					dmg->RandomMorale = RandomMorale;
					dmg->RandomStun = RandomStun;
					dmg->RandomTile = RandomTile;
					dmg->RandomTime = RandomTime;
					dmg->RandomType = intToItemDamageRandomType(RandomType);
					dmg->RandomWound = RandomWound;
					dmg->ResistType = intToItemDamageType(ResistType);
					dmg->SmokeThreshold = SmokeThreshold;
					dmg->TileDamageMethod = TileDamageMethod;
					dmg->ToArmor = ToArmor;
					dmg->ToArmorPre = ToArmorPre;
					dmg->ToEnergy = ToEnergy;
					dmg->ToHealth = ToHealth;
					dmg->ToItem = ToItem;
					dmg->ToMana = ToMana;
					dmg->ToMorale = ToMorale;
					dmg->ToStun = ToStun;
					dmg->ToTile = ToTile;
					dmg->ToWound = ToWound;

					if (!_battleActions.empty())
					{
						BattleActionAttack oldest = _battleActions.front();  
				
						_game->getSavedGame()->getSavedBattle()->getBattleGame()->hitCoop(oldest, Position(center_x, center_y, center_z), power, dmg, rangeAtack, terrainMeleeTilePart, seed);

						_battleActions.erase(_battleActions.begin()); 
				
					}

				}

			}

		}

	}

	// unit_death
	if (stateString == "unit_death")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				_game->getSavedGame()->getSavedBattle()->abortPathCoop();

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{
	
					int unit_id = obj["unit_id"].asInt();


					// Check if the same unit
					if (unit->getId() == unit_id)
					{

							int time = obj["time"].asInt();
							int health = obj["health"].asInt();
							int energy = obj["energy"].asInt();
							int morale = obj["morale"].asInt();
							int mana = obj["mana"].asInt();
							int stunlevel = obj["stunlevel"].asInt();
							int motionpoints = obj["motionpoints"].asInt();

							int setDirection = obj["setDirection"].asInt();
							int setFaceDirection = obj["setFaceDirection"].asInt();

							bool respawn = obj["respawn"].asBool();
							unit->setRespawn(respawn);

							unit->setDirection(setDirection);
							unit->setFaceDirection(setFaceDirection);

							unit->setMotionPointsCoop(motionpoints);
							unit->setTimeUnits(time);
							unit->setHealth(health);
							unit->setCoopMorale(morale);
							unit->setCoopEnergy(energy);
							unit->setCoopMana(mana);

							unit->setStunlevelCoop(stunlevel);

							int pos_x = obj["pos_x"].asInt();
							int pos_y = obj["pos_y"].asInt();
							int pos_z = obj["pos_z"].asInt();

							// Check if positions do not match
							if (unit->getPosition().x != pos_x || unit->getPosition().y != pos_y || unit->getPosition().z != pos_z)
							{
								_game->getSavedGame()->getSavedBattle()->getBattleGame()->teleport(pos_x, pos_y, pos_z, unit);
							}

							int status_int = obj["status"].asInt();
							UnitStatus unitStatus = intToUnitstatus(status_int);
							unit->setCoopStatus(unitStatus);

							int damageType_int = obj["damageType"].asInt();
							bool noSound = obj["noSound"].asBool();
							const RuleDamageType* damageType = _game->getMod()->getDamageType(intToItemDamageType(damageType_int));

							_game->getSavedGame()->getSavedBattle()->getBattleGame()->coopDeath(unit, damageType, noSound);

							// TILE
							bool isTile = obj["isTile"].asBool();

							if (!isTile)
							{

								if (unit->getStatus() != STATUS_DEAD && unit->getStatus() != STATUS_UNCONSCIOUS)
								{
									unit->setCoopStatus(STATUS_DEAD);
								}

								unit->setTile(nullptr, _game->getSavedGame()->getSavedBattle());
							}

							if (!unit->getTile() && unit->getStatus() != STATUS_DEAD && unit->getStatus() != STATUS_UNCONSCIOUS)
							{
								unit->setCoopStatus(STATUS_DEAD);
							}
			
					}


				}
			}
		}

		// Make sure the Battlescape does not get stuck...
		_hasHitUnit = -1;

	}

	if (stateString == "set_smoke_tile")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				int tile_pos_x = obj["tile_pos_x"].asInt();
				int tile_pos_y = obj["tile_pos_y"].asInt();
				int tile_pos_z = obj["tile_pos_z"].asInt();

				int smoke = obj["smoke"].asInt();
				int animation_offset = obj["animation_offset"].asInt();
				int overlaps = obj["overlaps"].asInt();

				Tile* selected_tile = _game->getSavedGame()->getSavedBattle()->getTile(Position(tile_pos_x, tile_pos_y, tile_pos_z));

				selected_tile->setSmokeCoop(smoke, animation_offset, overlaps);
			}
		}
	}

	if (stateString == "set_fire_tile")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				int tile_pos_x = obj["tile_pos_x"].asInt();
				int tile_pos_y = obj["tile_pos_y"].asInt();
				int tile_pos_z = obj["tile_pos_z"].asInt();

				int fire = obj["fire"].asInt();	
				int animation_offset = obj["animation_offset"].asInt();

				Tile* selected_tile = _game->getSavedGame()->getSavedBattle()->getTile(Position(tile_pos_x, tile_pos_y, tile_pos_z));

				selected_tile->setFireCoop(fire, animation_offset);

			}
		}

	}

	// destroy tile
	if (stateString == "destroy_tile")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				int tile_pos_x = obj["tile_pos_x"].asInt();
				int tile_pos_y = obj["tile_pos_y"].asInt();
				int tile_pos_z = obj["tile_pos_z"].asInt();

				int tile_part = obj["tile_part"].asInt();
				int special_tile_type = obj["special_tile_type"].asInt();

				int explosive = obj["explosive"].asInt();
				int explosive_type = obj["explosive_type"].asInt();

				Tile *selected_tile = _game->getSavedGame()->getSavedBattle()->getTile(Position(tile_pos_x, tile_pos_y, tile_pos_z));

				selected_tile->destroyCoop((TilePart)tile_part, (SpecialTileType)special_tile_type);

				selected_tile->setExplosive(explosive, explosive_type, true);

			}

		}

	}

	if (stateString == "unit_fire")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				int unit_id = obj["unit_id"].asInt();
				int fire = obj["fire"].asInt();

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getId() == unit_id)
					{

						unit->setFireCoop(fire);
						break;
					}
				}
			}
		}

	}

	if (stateString == "hasHitUnit")
	{

		// make sure a new projectile is not created immediately when a unit is hit
		_hasHitUnit = -2;

	}

	// hit unit
	if (stateString == "hit_unit")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				int unit_id = obj["unit_id"].asInt();
				int health = obj["health"].asInt();
				int stunlevel = obj["stunlevel"].asInt();

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getId() == unit_id)
					{

						const Json::Value& fatalArray = obj["fatalWounds"];

						for (int part = 0; part < BODYPART_MAX && part < fatalArray.size(); ++part)
						{
							unit->setFatalWoundCoop(part, fatalArray[part].asInt());
						}

						unit->setHealth(health);
						unit->setStunlevelCoop(stunlevel);
						break;

					}

				}

			}
		}

	}

	if (stateString == "checkForProximityGrenades")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				if (_game->getSavedGame()->getSavedBattle()->getBattleGame())
				{

					int unit_id = obj["unit_id"].asInt();

					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{

						if (unit->getId() == unit_id)
						{
		
							_game->getSavedGame()->getSavedBattle()->getBattleGame()->checkForProximityCoop(unit);

							break;

						}

					}


				}

			}
		}

	}

	// NEXT TURN
	if (stateString == "next_turn")
	{

		_hasHitUnit = -1;

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				bool end = obj["end"].asBool();

				_game->getSavedGame()->getSavedBattle()->abortPathCoop();

				// tiles
				auto* savedGame = _game->getSavedGame();
				auto* battle = savedGame->getSavedBattle();
				int mapSize = battle->getMapSizeXYZ();

				// 1) Reset all tiles
				for (int i = 0; i < mapSize; ++i)
				{
					Tile* tile = battle->getTile(i);
					tile->setFireCoop(0, 0);
					tile->setSmokeCoop(0, 0, -1);
				}

				// 2) Apply JSON data
				const Json::Value& tiles = obj["tiles"];
				for (Json::ArrayIndex json_id = 0; json_id < tiles.size(); ++json_id)
				{
					int tile_pos_x = tiles[json_id]["tile_pos_x"].asInt();
					int tile_pos_y = tiles[json_id]["tile_pos_y"].asInt();
					int tile_pos_z = tiles[json_id]["tile_pos_z"].asInt();

					bool getDangerous = tiles[json_id]["getDangerous"].asBool();
					int getFire = tiles[json_id]["getFire"].asInt();
					int getSmoke = tiles[json_id]["getSmoke"].asInt();

					int animation_offset = tiles[json_id]["animation_offset"].asInt();
					int overlaps = tiles[json_id]["overlaps"].asInt();

					// Direct lookup by coordinates
					Tile* tile = battle->getTile(Position(tile_pos_x, tile_pos_y, tile_pos_z));

					if (!tile)
						continue;

					tile->setDangerous(getDangerous);
					tile->setFireCoop(getFire, animation_offset);
					tile->setSmokeCoop(getSmoke, animation_offset, overlaps);
				}

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					for (int i = 0; i < obj["units"].size(); i++)
					{

						int json_id = obj["units"][i]["unit_id"].asInt();

						// Check if the same unit
						if (unit->getId() == json_id)
						{

							int time = obj["units"][i]["time"].asInt();
							int health = obj["units"][i]["health"].asInt();
							int energy = obj["units"][i]["energy"].asInt();
							int morale = obj["units"][i]["morale"].asInt();
							int mana = obj["units"][i]["mana"].asInt();
							int stunlevel = obj["units"][i]["stunlevel"].asInt();

							int motionpoints = obj["units"][i]["motionpoints"].asInt();

							int setDirection = obj["units"][i]["setDirection"].asInt();
							int setFaceDirection = obj["units"][i]["setFaceDirection"].asInt();

							int setTurretDirection = obj["units"][i]["setTurretDirection"].asInt();
							int setTurretToDirection = obj["units"][i]["setTurretToDirection"].asInt();

							bool respawn = obj["units"][i]["respawn"].asBool();

							int fire = obj["units"][i]["fire"].asInt();
							unit->setFireCoop(fire);

							unit->setRespawn(respawn);

							unit->setDirection(setDirection);
							unit->setFaceDirection(setFaceDirection);

							unit->setDirectionTurretCoop(setTurretDirection);
							unit->setTurretToDirectionCoop(setTurretToDirection);

							unit->setMotionPointsCoop(motionpoints);

							unit->setCoopEnergy(energy);
							unit->setTimeUnits(time);
							
							unit->setHealth(health);
							unit->setCoopMorale(morale);
				
							unit->setCoopMana(mana);
							unit->setStunlevelCoop(stunlevel);

							const Json::Value& fatalArray = obj["units"][i]["fatalWounds"];

							for (int part = 0; part < BODYPART_MAX && part < fatalArray.size(); ++part)
							{
								unit->setFatalWoundCoop(part, fatalArray[part].asInt());
							}


							int pos_x = obj["units"][i]["pos_x"].asInt();
							int pos_y = obj["units"][i]["pos_y"].asInt();
							int pos_z = obj["units"][i]["pos_z"].asInt();

							// mind control (client)
							if (unit->_coop_mindcontrolled == true)
							{

								unit->_coop_mindcontrolled = false;

								if (unit->getCoop() == 0)
								{
		
									unit->setCoop(1);

									if (_game->getCoopMod()->getHost() == false)
									{
										unit->convertToFaction(FACTION_PLAYER);
										unit->setOriginalFaction(FACTION_PLAYER);
									}
									else
									{
										unit->convertToFaction(FACTION_HOSTILE);
										unit->setOriginalFaction(FACTION_HOSTILE);
									}

								}
								else if (unit->getCoop() == 1)
								{
									
									unit->setCoop(0);

									if (_game->getCoopMod()->getHost() == true)
									{
										unit->convertToFaction(FACTION_PLAYER);
										unit->setOriginalFaction(FACTION_PLAYER);
									}
									else
									{
										unit->convertToFaction(FACTION_HOSTILE);
										unit->setOriginalFaction(FACTION_HOSTILE);
									}

								}
							}

							// Check if positions do not match
							if (unit->getPosition().x != pos_x || unit->getPosition().y != pos_y || unit->getPosition().z != pos_z)
							{

								_game->getSavedGame()->getSavedBattle()->getBattleGame()->teleport(pos_x, pos_y, pos_z, unit);
							}

							int status_int = obj["units"][i]["status"].asInt();
							UnitStatus unitStatus = intToUnitstatus(status_int);

							unit->setCoopStatus(unitStatus);

							// TILE
							bool isTile = obj["units"][i]["isTile"].asBool();

							if (!isTile)
							{

								if (unit->getStatus() != STATUS_DEAD && unit->getStatus() != STATUS_UNCONSCIOUS)
								{
									unit->setCoopStatus(STATUS_DEAD);
								}

								unit->setTile(nullptr, _game->getSavedGame()->getSavedBattle());
							}

							if (!unit->getTile() && unit->getStatus() != STATUS_DEAD && unit->getStatus() != STATUS_UNCONSCIOUS)
							{
								unit->setCoopStatus(STATUS_DEAD);
							}

							break;

						}
					}
				}
			}
		}

	}

	// ufo damage
	if (stateString == "ufo_damage")
	{

		if (_game->getSavedGame() && playerInsideCoopBase == false)
		{

			int ufo_id = obj["ufo_id"].asInt();
			int damage = obj["damage"].asInt();

			int status_int = obj["status"].asInt();
			Ufo::UfoStatus status = intToUfostatus(status_int);
			std::string altitude = obj["altitude"].asString();
			bool detected = obj["detected"].asBool();

			int wave = obj["wave"].asInt();
			int crash_id = obj["crash_id"].asInt();
			int land_id = obj["land_id"].asInt();

			std::string craft_rule = obj["craft_rule"].asString();
			int craft_id = obj["craft_id"].asInt();

			bool end = obj["end"].asBool();
			bool survived = obj["survived"].asBool();

			bool minimized = obj["minimized"].asBool();

			for (auto& i_ufo : *_game->getSavedGame()->getUfos())
			{

				if (i_ufo->_coop_ufo_id == ufo_id && i_ufo->_coop == false)
				{

					// damage
					if (damage > i_ufo->lastPlayerUfoDamage)
					{

						int current_damage = i_ufo->getDamage() + (damage - i_ufo->lastPlayerUfoDamage);

						i_ufo->setDamage(current_damage, _game->getMod());

						i_ufo->lastPlayerUfoDamage = damage;

					}

					i_ufo->setStatusCoop(status);
					i_ufo->setAltitudeCoop(altitude);
					i_ufo->setDetectedCoop(detected);
					i_ufo->setDetected(detected);

					i_ufo->setMissionWaveNumber(wave);
					i_ufo->setCrashId(crash_id);
					i_ufo->setLandId(land_id);

					if (i_ufo->originalCoopSpeed == 0)
					{
						i_ufo->originalCoopSpeed = i_ufo->getSpeed();
					}

					if (i_ufo->getSecondsRemaining() <= 0)
					{
						i_ufo->setSecondsRemaining(86400);
					}
	
					if (end == true || minimized == true)
					{
						i_ufo->_playerShotDownUfo = false;
						i_ufo->setSpeed(i_ufo->originalCoopSpeed);
						i_ufo->originalCoopSpeed = 0;
					}
					else if (minimized == false)
					{
						i_ufo->setSpeed(0);
					}

					if (i_ufo->isCrashed())
					{
						i_ufo->_playerShotDownUfo = true;
						i_ufo->setStatusCoop(Ufo::CRASHED);
						i_ufo->setShotDownByCraftId(std::make_pair(craft_rule, craft_id));
					}

					if (i_ufo->isDestroyed() || survived == false)
					{
						i_ufo->_playerShotDownUfo = true;
						i_ufo->setStatusCoop(Ufo::DESTROYED);
						i_ufo->setShotDownByCraftId(std::make_pair(craft_rule, craft_id));
					}

					break;

				}

			}


		}


	}

	if (stateString == "update_graphs")
	{

		if (_game->getSavedGame() && getServerOwner() == false)
		{
			// income
			// countries
			const Json::Value& countries = obj["countries"];

			for (const Json::Value& c : countries)
			{

				const std::string type = c["type"].asString();
				const Json::Value& fundingJson = c["funding"];
				const Json::Value& activityXcomJson = c["activityXcom"];
				const Json::Value& activityAlienJson = c["activityAlien"];

				Country* local = nullptr;
				for (auto* country : *_game->getSavedGame()->getCountries())
				{
					if (country->getRules()->getType() == type)
					{
						local = country;
						break;
					}
				}
				if (!local)
					continue;

				// funding
				auto& fundingVec = local->getFunding();
				fundingVec.clear();
				fundingVec.reserve(fundingJson.size());

				for (const Json::Value& v : fundingJson)
					fundingVec.push_back(v.asInt());

				// activityXcom
				auto& activityXcomVec = local->getActivityXcom();
				activityXcomVec.clear();
				activityXcomVec.reserve(activityXcomJson.size());

				for (const Json::Value& v : activityXcomJson)
					activityXcomVec.push_back(v.asInt());

				// activityAlien
				auto& activityAlienVec = local->getActivityAlien();
				activityAlienVec.clear();
				activityAlienVec.reserve(activityAlienJson.size());

				for (const Json::Value& v : activityAlienJson)
					activityAlienVec.push_back(v.asInt());
			}

			// regions
			const Json::Value& regions = obj["regions"];

			for (const Json::Value& r : regions)
			{

				const std::string type = r["type"].asString();
				const Json::Value& activityXcomJson = r["activityXcom"];
				const Json::Value& activityAlienJson = r["activityAlien"];

				Region* local = nullptr;
				for (auto* region : *_game->getSavedGame()->getRegions())
				{
					if (region->getRules()->getType() == type)
					{
						local = region;
						break;
					}
				}
				if (!local)
					continue;

				// activityXcom
				auto& activityXcomVec = local->getActivityXcom();
				activityXcomVec.clear();
				activityXcomVec.reserve(activityXcomJson.size());

				for (const Json::Value& v : activityXcomJson)
					activityXcomVec.push_back(v.asInt());

				// activityAlien
				auto& activityAlienVec = local->getActivityAlien();
				activityAlienVec.clear();
				activityAlienVec.reserve(activityAlienJson.size());

				for (const Json::Value& v : activityAlienJson)
					activityAlienVec.push_back(v.asInt());
			}
		}

	}

	if (stateString == "graph_requests")
	{

		if (_game->getSavedGame() && playerInsideCoopBase == false && getServerOwner() == true)
		{

			Json::Value root;

			root["state"] = "update_graphs";

			// countries
			Json::Value countries(Json::arrayValue);

			for (auto* country : *_game->getSavedGame()->getCountries())
			{

				Json::Value c;

				c["type"] = country->getRules()->getType();

				Json::Value funding(Json::arrayValue);

				for (int f : country->getFunding())
					funding.append(f);

				c["funding"] = funding;

				// activityXcom
				Json::Value activityXcom(Json::arrayValue);

				for (int x : country->getActivityXcom())
					activityXcom.append(x);

				c["activityXcom"] = activityXcom;

				// activityAlien
				Json::Value activityAlien(Json::arrayValue);

				for (int a : country->getActivityAlien())
					activityAlien.append(a);

				c["activityAlien"] = activityAlien;

				countries.append(c);
			}

			root["countries"] = countries;

			// regions
			Json::Value regions(Json::arrayValue);

			for (auto* region : *_game->getSavedGame()->getRegions())
			{

				Json::Value r;

				r["type"] = region->getRules()->getType();

				// activityXcom
				Json::Value activityXcom(Json::arrayValue);

				for (int x : region->getActivityXcom())
					activityXcom.append(x);

				r["activityXcom"] = activityXcom;

				// activityAlien
				Json::Value activityAlien(Json::arrayValue);

				for (int a : region->getActivityAlien())
					activityAlien.append(a);

				r["activityAlien"] = activityAlien;

				regions.append(r);
			}

			root["regions"] = regions;

			sendTCPPacketData(root.toStyledString());

		}

	}

	if (stateString == "monthly_report")
	{

		// income
		// countries
		 const Json::Value& countries = obj["countries"];

		 for (const Json::Value& c : countries)
		 {

			 const std::string type = c["type"].asString();
			 const Json::Value& fundingJson = c["funding"];
			 const Json::Value& activityXcomJson = c["activityXcom"];
			 const Json::Value& activityAlienJson = c["activityAlien"];

			 Country* local = nullptr;
			 for (auto* country : *_game->getSavedGame()->getCountries())
			 {
				 if (country->getRules()->getType() == type)
				 {
					 local = country;
					 break;
				 }
			 }
			 if (!local)
				 continue;

			 // funding
			 auto& fundingVec = local->getFunding(); 
			 fundingVec.clear();
			 fundingVec.reserve(fundingJson.size());

			 for (const Json::Value& v : fundingJson)
				 fundingVec.push_back(v.asInt());

			 // activityXcom
			 auto& activityXcomVec = local->getActivityXcom();
			 activityXcomVec.clear();
			 activityXcomVec.reserve(activityXcomJson.size());

			 for (const Json::Value& v : activityXcomJson)
				 activityXcomVec.push_back(v.asInt());

			 // activityAlien
			 auto& activityAlienVec = local->getActivityAlien();
			 activityAlienVec.clear();
			 activityAlienVec.reserve(activityAlienJson.size());

			 for (const Json::Value& v : activityAlienJson)
				 activityAlienVec.push_back(v.asInt());

		 }

		 // regions
		 const Json::Value& regions = obj["regions"];

		 for (const Json::Value& r : regions)
		 {

			 const std::string type = r["type"].asString();
			 const Json::Value& activityXcomJson = r["activityXcom"];
			 const Json::Value& activityAlienJson = r["activityAlien"];

			 Region* local = nullptr;
			 for (auto* region : *_game->getSavedGame()->getRegions())
			 {
				 if (region->getRules()->getType() == type)
				 {
					 local = region;
					 break;
				 }
			 }
			 if (!local)
				 continue;

			  // activityXcom
			 auto& activityXcomVec = local->getActivityXcom();
			 activityXcomVec.clear();
			 activityXcomVec.reserve(activityXcomJson.size());

			 for (const Json::Value& v : activityXcomJson)
				 activityXcomVec.push_back(v.asInt());

			 // activityAlien
			 auto& activityAlienVec = local->getActivityAlien();
			 activityAlienVec.clear();
			 activityAlienVec.reserve(activityAlienJson.size());

			 for (const Json::Value& v : activityAlienJson)
				 activityAlienVec.push_back(v.asInt());

		 }

		 // month
		 int month = obj["month"].asInt();
		 _game->getSavedGame()->getTime()->setMonthCoop(month);

		 // year
		 int year = obj["year"].asInt();
		 _game->getSavedGame()->getTime()->setYearCoop(year);

		 int fundingDiff = obj["fundingDiff"].asInt();
		 fundingDiffCoop = fundingDiff;

		 int ratingTotal = obj["ratingTotal"].asInt();
		 ratingTotalCoop = ratingTotal;

		 int lastMonthsRating = obj["lastMonthsRating"].asInt();
		 lastMonthsRatingCoop = lastMonthsRating;

		 // happyList
		const Json::Value& happyList = obj["happyList"];

		 _happyListCoop.clear();
		 if (happyList.isArray())
		 {
			 _happyListCoop.reserve(happyList.size());
			 for (Json::ArrayIndex i = 0; i < happyList.size(); ++i)
			 {
				 if (happyList[i].isString())
					 _happyListCoop.push_back(happyList[i].asString());
			 }
		 }

		 // sadList
		 const Json::Value& sadList = obj["sadList"];

		 _sadListCoop.clear();
		 if (sadList.isArray())
		 {
			 _sadListCoop.reserve(sadList.size());
			 for (Json::ArrayIndex i = 0; i < sadList.size(); ++i)
			 {
				 if (sadList[i].isString())
					 _sadListCoop.push_back(sadList[i].asString());
			 }
		 }

		 // pactList
		 const Json::Value& pactList = obj["pactList"];

		 _pactListCoop.clear();
		 if (pactList.isArray())
		 {
			 _pactListCoop.reserve(pactList.size());
			 for (Json::ArrayIndex i = 0; i < pactList.size(); ++i)
			 {
				 if (pactList[i].isString())
					 _pactListCoop.push_back(pactList[i].asString());
			 }
		 }

		 // cancelPactList
		 const Json::Value& cancelPactList = obj["cancelPactList"];

		 _cancelPactListCoop.clear();
		 if (cancelPactList.isArray())
		 {
			 _cancelPactListCoop.reserve(cancelPactList.size());
			 for (Json::ArrayIndex i = 0; i < cancelPactList.size(); ++i)
			 {
				 if (cancelPactList[i].isString())
					 _cancelPactListCoop.push_back(cancelPactList[i].asString());
			 }
		 }

		// PRD-J04: authoritative monthly settlement (SHARED). Stored here; applied to
		// the replica's tails in time1MonthCoop after its own monthlyFunding roll.
		if (obj.isMember("sharedFunds"))
		{
			sharedMonthlyFunds = obj["sharedFunds"].asInt64();
			sharedMonthlyMaintenance = obj.get("sharedMaintenance", 0).asInt64();
			sharedMonthlyIncome = obj.get("sharedIncome", 0).asInt64();
			sharedMonthlyExpenditure = obj.get("sharedExpenditure", 0).asInt64();
			sharedMonthlyResearchScore = obj.get("sharedResearchScore", 0).asInt();
			sharedMonthlyPending = true;
		}

		_game->getCoopMod()->show_coop_monthly_report = true;

	}

	// PRD-DF01: per-tick dogfight render frames. df_state rides the
	// SNAP_DOGFIGHT conflation slot as a raw top-level message (last-write-
	// wins, freshest-only, never the reliable FIFO), so it is dispatched
	// here by state string. On a replica it fans out to the render-only
	// DogfightState windows (epoch-guarded); the host ignores its own stream.
	if (stateString == "df_state")
	{
		SharedEcon::applyDogfightState(_game, obj);
	}

	// target positions
	if (stateString == "target_positions")
	{

		// PRD-J04: SHARED position snapshot (`shared:true`). The replica is
		// simulation-frozen, so it applies the HOST's real object positions here
		// (matched by REAL id, not a _coop mirror id) so it still SEES crafts/UFOs
		// move. This is the SHARED counterpart of the SEPARATE mirror below; the two
		// are mutually exclusive (SHARED never sends the SEPARATE snapshot and the
		// SEPARATE block is fenced with !isSharedCampaign()).
		if (obj.get("shared", false).asBool() && isSharedReplica() && _game->getSavedGame())
		{
			SavedGame* sg = _game->getSavedGame();

			// crafts: update the matching real craft (base index + craft id).
			auto& jbases = *sg->getBases();
			for (Json::ArrayIndex i = 0; i < obj["crafts"].size(); i++)
			{
				const Json::Value& jc = obj["crafts"][i];
				int baseId = jc["baseId"].asInt();
				int craftId = jc["id"].asInt();
				if (baseId < 0 || baseId >= (int)jbases.size()) continue;
				for (auto* craft : *jbases[baseId]->getCrafts())
				{
					if (craft->getId() == craftId && craft->getRules()->getType() == jc["rule"].asString())
					{
						craft->setLongitude(jc["lon"].asDouble());
						craft->setLatitude(jc["lat"].asDouble());
						craft->setStatus(jc["status"].asString());
						craft->setSpeed(jc["speed"].asInt());
						// PRD-J08: replica-visible craft condition (host-simulated
						// refuel/repair/rearm progress).
						if (jc.isMember("fuel")) craft->setFuel(jc["fuel"].asInt());
						if (jc.isMember("damage")) craft->setDamage(jc["damage"].asInt());
						if (jc.isMember("shield")) craft->setShield(jc["shield"].asInt());
						break;
					}
				}
			}

			// ufos: create-or-update the matching real UFO (by real id). Track the
			// live id set so despawned UFOs can be hidden afterwards.
			std::unordered_set<int> liveUfoIds;
			for (Json::ArrayIndex i = 0; i < obj["ufos"].size(); i++)
			{
				const Json::Value& ju = obj["ufos"][i];
				int ufoId = ju["id"].asInt();
				liveUfoIds.insert(ufoId);
				int missionId = ju["mission_id"].asInt();

				// find/create the owning AlienMission (needed for Ufo::getMission()).
				AlienMission* mission = nullptr;
				for (auto* m : sg->getAlienMissions())
					if (m->getId() == missionId) { mission = m; break; }
				if (!mission)
				{
					const RuleAlienMission* mrule = _game->getMod()->getAlienMission(ju["mission_rule"].asString(), false);
					if (!mrule) continue;
					mission = new AlienMission(*mrule);
					mission->setRace(ju["race"].asString());
					mission->setId(missionId);
					mission->setRegion(ju["region"].asString(), *_game->getMod());
					sg->getAlienMissions().push_back(mission);
				}

				Ufo* ufo = nullptr;
				for (auto* u : *sg->getUfos())
					if (u->getId() == ufoId) { ufo = u; break; }
				if (!ufo)
				{
					RuleUfo* ufoRule = _game->getMod()->getUfo(ju["ufo_rule"].asString(), false);
					if (!ufoRule) continue;
					const UfoTrajectory& traj = *_game->getMod()->getUfoTrajectory(UfoTrajectory::RETALIATION_ASSAULT_RUN, true);
					ufo = new Ufo(ufoRule, ufoId);
					ufo->setMissionInfo(mission, &traj);
					// PRD-J08 fix: the ctor arg is the UNIQUE id; the DISPLAY id
					// (Target::getId(), which every subsequent snapshot and the
					// dogfight lane match by) must be set explicitly - without it
					// the replica re-created an unmatchable id-0 UFO every tick.
					ufo->setId(ufoId);
					sg->getUfos()->push_back(ufo);
				}
				ufo->setLongitude(ju["lon"].asDouble());
				ufo->setLatitude(ju["lat"].asDouble());
				// PRD-J08: adopt hull damage BEFORE status - setDamage derives
				// CRASHED/DESTROYED from thresholds, setStatus then re-asserts
				// the authoritative value.
				if (ju.isMember("damage")) ufo->setDamage(ju["damage"].asInt(), _game->getMod());
				ufo->setStatus(intToUfostatus(ju["status"].asInt()));
				ufo->setDetected(ju["detected"].asBool());
				ufo->setAltitude(ju["altitude"].asString());
				ufo->setSpeed(ju["speed"].asInt());
				// PRD-J08: crash/land marker identity travels by value.
				if (ju.isMember("crashId") && ju["crashId"].asInt() != 0)
					ufo->setCrashId(ju["crashId"].asInt());
				if (ju.isMember("landId") && ju["landId"].asInt() != 0)
					ufo->setLandId(ju["landId"].asInt());
				ufo->setSecondsRemaining(100000000);
			}
			// despawn: hide replica UFOs no longer in the authoritative set (a frozen
			// replica has no dogfights/followers to unwind; full cleanup is J10).
			for (auto* u : *sg->getUfos())
			{
				if (liveUfoIds.find(u->getId()) == liveUfoIds.end())
				{
					u->setDetected(false);
					u->setStatus(Ufo::DESTROYED);
					u->setSecondsRemaining(0);
				}
			}

			// mission sites: create-or-update the matching real site (by real id).
			{
				std::unordered_set<int> liveSiteIds;
				for (Json::ArrayIndex i = 0; i < obj["missions"].size(); i++)
				{
					const Json::Value& jm = obj["missions"][i];
					int siteId = jm["id"].asInt();
					liveSiteIds.insert(siteId);
					MissionSite* site = nullptr;
					for (auto* s : *sg->getMissionSites())
						if (s->getId() == siteId) { site = s; break; }
					if (!site)
					{
						const RuleAlienMission* srule = _game->getMod()->getAlienMission(jm["rules"].asString(), false);
						AlienDeployment* dep = _game->getMod()->getDeployment(jm["deployment"].asString(), false);
						if (!srule || !dep) continue;
						site = new MissionSite(srule, dep, nullptr);
						site->setId(siteId);
						sg->getMissionSites()->push_back(site);
					}
					site->setLongitude(jm["lon"].asDouble());
					site->setLatitude(jm["lat"].asDouble());
					site->setAlienRace(jm["race"].asString());
					site->setCity(jm["city"].asString());
					// issue #78: mirror the host's detection state and fuse instead of
					// forcing detected + pinning an immortal sentinel. The replica sim
					// is frozen, so both are display-only - but they must match what
					// the host actually shows.
					site->setDetected(jm.isMember("detected") ? jm["detected"].asBool() : true);
					site->setSecondsRemaining(jm.isMember("time")
						? (size_t)jm["time"].asUInt64() : (size_t)100000000);
				}
				// issue #78: publish the authoritative id set; sites absent from it are
				// despawned on the main thread (GeoscapeState::think) - the site analog
				// of the UFO despawn above, deferred so no open popup can be holding a
				// dangling MissionSite*.
				{
					std::lock_guard<std::mutex> lk(sharedLiveSiteIdsMutex);
					sharedLiveSiteIds = std::move(liveSiteIds);
					sharedLiveSiteIdsValid = true;
				}
			}
		}

		// PRD-J02: SEPARATE-only peer economy/craft mirror. A SHARED replica already
		// holds every base/craft/fund as real data in the streamed world, so
		// consuming the mirror snapshot would duplicate them. Fence it off.
		if (_game->getSavedGame() && playerInsideCoopBase == false && openMultipleTargetsMenu == false && !isSharedCampaign())
		{

			// funds
			int64_t funds = obj["funds"].asInt64();
			playersFunds = funds;

			// crafts
			int64_t crafts = obj["craft_count"].asInt64();
			playersCrafts = crafts;

			// bases
			int64_t base_count = obj["base_count"].asInt64();
			playersBases = base_count;

			// crafts
			for (int i = 0; i < obj["crafts"].size(); i++)
			{

				int base_id = obj["crafts"][i]["coopbase_id"].asInt();
				int craft_id = obj["crafts"][i]["craft_id"].asInt();
				std::string rule_id = obj["crafts"][i]["rule"].asString();
				std::string status = obj["crafts"][i]["status"].asString();
				double lat = obj["crafts"][i]["lat"].asDouble();
				double lon = obj["crafts"][i]["lon"].asDouble();

				int fuel = obj["crafts"][i]["fuel"].asInt();
				int damage = obj["crafts"][i]["damage"].asInt();

				int speed = obj["crafts"][i]["speed"].asInt();

				// new!
				int shield = obj["crafts"][i]["shield"].asInt();
				int interceptionOrder = obj["crafts"][i]["interceptionOrder"].asInt();
				std::string craft_name = obj["crafts"][i]["craft_name"].asString();
				int num_total_vehicles = obj["crafts"][i]["num_total_vehicles"].asInt();
				int num_total_soldiers = obj["crafts"][i]["num_total_soldiers"].asInt();

				for (auto* base : *_game->getSavedGame()->getBases())
				{

					if (base->_coopIcon == true && base->_coop_base_id == base_id)
					{

						Craft* craft = 0;

						for (auto& i_craft : *base->getCrafts())
						{

							if (i_craft->getId() == craft_id && i_craft->getRules()->getType() == rule_id)
							{
								craft = i_craft;
								break;
							}
						}

						// If no craft is found, create a new one.
						if (!craft)
						{

							RuleCraft* rule = _game->getMod()->getCraft(rule_id, false);

							if (rule)
							{
								craft = new Craft(rule, base, craft_id);

								// weapons
								auto& weapons = *craft->getWeapons();
								const Json::Value& wj = obj["crafts"][i]["weapons"];

								for (auto* cw : weapons)
									delete cw;
								weapons.clear();

								for (Json::ArrayIndex w = 0; w < wj.size(); ++w)
								{
									const std::string type = wj[w]["type"].asString();
									const int ammo = wj[w]["ammo"].asInt();

									if (type != "" && ammo != -1)
									{
										const RuleCraftWeapon* wRule = _game->getMod()->getCraftWeapon(type);
										if (!wRule)
											continue;

										weapons.push_back(new CraftWeapon(const_cast<RuleCraftWeapon*>(wRule), ammo));
									}
									else
									{
										weapons.push_back(0);
									}

								}

								base->getCrafts()->push_back(craft);
							}
							else
							{
								return;
							}
						}

						craft->coop = true;

						craft->setCoopStatus(status);

						craft->setLongitude(lon);
						craft->setLatitude(lat);

						craft->setFuelCoop(fuel);
						craft->setDamage(damage);

						craft->setSpeed(speed);

						// new!
						craft->setShield(shield);
						craft->setInterceptionOrder(interceptionOrder);
						craft->setName(craft_name);
						craft->coop_total_soldiers = num_total_soldiers;
						craft->coop_total_vehicles = num_total_vehicles;

						// returning-state flags (default false so older/partial
						// packets don't throw); drives the correct status display
						craft->setLowFuel(obj["crafts"][i].get("lowFuel", false).asBool());
						craft->setMissionComplete(obj["crafts"][i].get("mission", false).asBool());
						// pre-localized airborne status string from the owner
						craft->setCoopGeoStatus(obj["crafts"][i].get("geoStatus", "").asString());

						// weapons
						auto& weapons = *craft->getWeapons();
						const Json::Value& wj = obj["crafts"][i]["weapons"];

						const Json::ArrayIndex count =
							std::min<Json::ArrayIndex>((Json::ArrayIndex)weapons.size(), wj.size());

						for (Json::ArrayIndex w = 0; w < count; ++w)
						{

							if (weapons[w])
							{

								int ammo = wj[w]["ammo"].asInt();

								if (ammo != -1)
								{
									weapons[w]->setAmmo(ammo);
								}

							}

						}
					}
				}
			}

			// ufos
			for (int i = 0; i < obj["ufos"].size(); i++)
			{

				int mission_id = obj["ufos"][i]["mission_id"].asInt();
				std::string mission_rule_id = obj["ufos"][i]["mission_rule"].asString();
				std::string race = obj["ufos"][i]["race"].asString();
				std::string region = obj["ufos"][i]["region"].asString();

				int ufo_id = obj["ufos"][i]["ufo_id"].asInt();
				std::string ufo_rule_id = obj["ufos"][i]["ufo_rule"].asString();
				int waveNumber = obj["ufos"][i]["wave"].asInt();
				double d_lat = obj["ufos"][i]["lat"].asDouble();
				double d_lon = obj["ufos"][i]["lon"].asDouble();
				int status_int = obj["ufos"][i]["status"].asInt();
				Ufo::UfoStatus status = intToUfostatus(status_int);
				std::string altitude = obj["ufos"][i]["altitude"].asString();
				bool detected = obj["ufos"][i]["detected"].asBool();

				int crash_id = obj["ufos"][i]["crash_id"].asInt();
				int land_id = obj["ufos"][i]["land_id"].asInt();

				int speed = obj["ufos"][i]["speed"].asInt();

				// new!!!
				bool hyperDetected = obj["ufos"][i]["hyperDetected"].asBool();
				int shield = obj["ufos"][i]["shield"].asInt();
				bool isHunterKiller = obj["ufos"][i]["isHunterKiller"].asBool();
				bool isEscort = obj["ufos"][i]["isEscort"].asBool();

				// alien mission
				AlienMission* alien_mission = 0;

				for (auto& i_alien_mission : _game->getSavedGame()->getAlienMissions())
				{

					if (i_alien_mission->getId() == mission_id && i_alien_mission->_coop == true)
					{
						alien_mission = i_alien_mission;
						break;
					}
				}

				if (!alien_mission)
				{

					const RuleAlienMission* alien_mission_rule = _game->getMod()->getAlienMission(mission_rule_id, false);

					if (alien_mission_rule)
					{

						alien_mission = new AlienMission(*alien_mission_rule, true);

						alien_mission->setCoop(true);
						alien_mission->setRace(race);
						alien_mission->setId(mission_id);

						alien_mission->setRegion(region, *_game->getMod());

						_game->getSavedGame()->getAlienMissions().push_back(alien_mission);
					}
					else
					{
						return;
					}
				}

				// ufo
				Ufo* ufo = 0;

				for (auto& i_ufo : *_game->getSavedGame()->getUfos())
				{

					if (i_ufo->_coop_ufo_id == ufo_id && i_ufo->_coop == true)
					{

						ufo = i_ufo;
						break;
					}
				}

				if (!ufo)
				{

					std::string str_ufo_id = "";

					if (waveNumber < 0)
					{
						str_ufo_id = ufo_rule_id;
					}
					else
					{

						const MissionWave& wave = alien_mission->getRules().getWave(waveNumber);
						str_ufo_id = wave.ufoType;
					}

					RuleUfo* ufoRule = _game->getMod()->getUfo(str_ufo_id, false);

					if (ufoRule)
					{

						const UfoTrajectory& assaultTrajectory = *_game->getMod()->getUfoTrajectory(UfoTrajectory::RETALIATION_ASSAULT_RUN, true);

						ufo = new Ufo(ufoRule, ufo_id);

						ufo->setMissionInfo(alien_mission, &assaultTrajectory);
						ufo->getMission()->setId(mission_id);
						ufo->setCoop(true);
						ufo->_coop_ufo_id = ufo_id;

						_game->getSavedGame()->getUfos()->push_back(ufo);
					}
					else
					{
						return;
					}
				}

				ufo->setCoop(true);

				ufo->setLatitude(d_lat);
				ufo->setLongitude(d_lon);

				// new !!!
				ufo->setShield(shield);
				ufo->setHunterKiller(isHunterKiller);
				ufo->setEscort(isEscort);

				if (getCoopGamemode() == 2 && getHost() == false)
				{
					ufo->setDetectedCoop(true);
					ufo->setHyperDetected(true);
				}
				else if (getCoopGamemode() == 3 && getHost() == true)
				{
					ufo->setDetectedCoop(true);
					ufo->setHyperDetected(true);
				}
				else
				{
					ufo->setHyperDetected(hyperDetected);
					ufo->setDetectedCoop(detected);
				}

				ufo->setStatusCoop(status);
				ufo->setAltitudeCoop(altitude);

				ufo->setLandId(land_id);
				ufo->setCrashId(crash_id);

				ufo->setSpeed(speed);

				ufo->setSecondsRemaining(100000000);
			}

			// mission sites
			for (int i = 0; i < obj["missions"].size(); i++)
			{

				std::string str_deployment = obj["missions"][i]["deployment"].asString();
				std::string str_rules = obj["missions"][i]["rules"].asString();
				std::string str_race = obj["missions"][i]["race"].asString();
				std::string str_city = obj["missions"][i]["city"].asString();
				size_t int_time = obj["missions"][i]["time"].asUInt64();
				double d_lon = obj["missions"][i]["lon"].asDouble();
				double d_lat = obj["missions"][i]["lat"].asDouble();
				int mission_id = obj["missions"][i]["mission_id"].asInt();

				MissionSite* missionSite = 0;

				for (auto* i_mission : *_game->getSavedGame()->getMissionSites())
				{

					if (i_mission->_coop_mission_id == mission_id && i_mission->_coop == true)
					{
						missionSite = i_mission;
						break;
					}
				}

				if (!missionSite)
				{
	
					if (str_deployment == "")
					{

						std::vector<std::string> deployments = _game->getMod()->getDeploymentsList();

						// Initialize the random number generator
						std::srand(std::time(nullptr));

						// Select a random index
						int randomIndex = std::rand() % deployments.size();

						// Select a random deployment
						str_deployment = deployments[randomIndex];
					}

					AlienDeployment* deployment = _game->getMod()->getDeployment(str_deployment, true);

					// MISSION SITE
					missionSite = new MissionSite(_game->getMod()->getAlienMission(str_rules, true), deployment, nullptr);
	
					missionSite->_coop_mission_id = mission_id;
					missionSite->setId(_game->getSavedGame()->getId(deployment->getMarkerName()));

					_game->getSavedGame()->getMissionSites()->push_back(missionSite);
				}

				missionSite->setLongitude(d_lon);
				missionSite->setLatitude(d_lat);

				missionSite->setSecondsRemaining(100000000);
				missionSite->setAlienRace(str_race);
				missionSite->setDetected(true);
				missionSite->setCity(str_city);

				missionSite->setCoop(true);
			}

			// alien bases
			for (int i = 0; i < obj["alienbases"].size(); i++)
			{

				int alienbase_id = obj["alienbases"][i]["alienbase_id"].asInt();
				std::string str_deployment = obj["alienbases"][i]["deployment"].asString();
				std::string str_race = obj["alienbases"][i]["race"].asString();
				double d_lon = obj["alienbases"][i]["lon"].asDouble();
				double d_lat = obj["alienbases"][i]["lat"].asDouble();
				std::string pact = obj["alienbases"][i]["pact"].asString();
				bool discovered = obj["alienbases"][i]["discovered"].asBool();
				int start_month = obj["alienbases"][i]["start_month"].asInt();

				AlienBase *alienBase = 0;

				for (auto* i_alienbase : *_game->getSavedGame()->getAlienBases())
				{

					if (i_alienbase->_coop_alienbase_id == alienbase_id && i_alienbase->_coop == true)
					{
						alienBase = i_alienbase;
						break;
					}
				}

				if (!alienBase)
				{
				
					if (str_deployment == "")
					{

						std::vector<std::string> deployments = _game->getMod()->getDeploymentsList();

						// Initialize the random number generator
						std::srand(std::time(nullptr));

						// Select a random index
						int randomIndex = std::rand() % deployments.size();

						// Select a random deployment
						str_deployment = deployments[randomIndex];
					}

					AlienDeployment* deployment = _game->getMod()->getDeployment(str_deployment, true);

					// ALIEN BASE
					alienBase = new AlienBase(deployment, start_month);
						
					alienBase->_coop_alienbase_id = alienbase_id;
					alienBase->setId(_game->getSavedGame()->getId(deployment->getMarkerName()));

					_game->getSavedGame()->getAlienBases()->push_back(alienBase);

				}

				alienBase->setLongitude(d_lon);
				alienBase->setLatitude(d_lat);

				alienBase->setDiscovered(discovered);
				alienBase->setPactCountry(pact);
				alienBase->setAlienRace(str_race);

				alienBase->_coop = true;

			}


			// remove crafts
			std::unordered_set<std::string> keep_craft;
			for (const auto& jc : obj["crafts"])
			{
				int id = jc["craft_id"].asInt();
				// Use the correct JSON field for the rule/type (support two common names)
				std::string rule = jc["rule"].asString();

				keep_craft.insert(std::to_string(id) + "|" + rule);
			}

			// 2) Remove coop crafts whose (id, rule) pair is NOT in the keep set
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				if (!base->_coopIcon)
					continue;

				auto& v = *base->getCrafts(); // std::vector<Craft*>&
				v.erase(std::remove_if(v.begin(), v.end(),
									   [&](Craft* c)
									   {
										   if (!c->coop)
											   return false;

										   int id = c->getId(); // c->_coop_craft_id
										   std::string rule = c->getRules()->getType();

										   bool remove = (keep_craft.find(std::to_string(id) + "|" + rule) == keep_craft.end());
										   if (remove)
											   delete c; // remove this line if you use smart pointers / different ownership
										   return remove;
									   }),
						v.end());
			}

			// remove ufos
			auto& ufos = *_game->getSavedGame()->getUfos();

			// Collect the UFO IDs from JSON that should be KEPT
			std::unordered_set<int> keep_ufo;
			for (const auto& jufo : obj["ufos"])
			{
				keep_ufo.insert(jufo["ufo_id"].asInt());
			}

			// 2) Remove all coop UFOs whose id is NOT in the keep set
			for (auto it = ufos.begin(); it != ufos.end();)
			{
				Ufo* u = *it;
				if (u->_coop && keep_ufo.find(u->_coop_ufo_id) == keep_ufo.end())
				{
					// If SavedGame owns the UFOs, also free memory:
					delete u; // <- omit if someone else owns them (e.g., smart pointers)
					it = ufos.erase(it);
				}
				else
				{
					++it;
				}
			}

			// remove mission sites
			auto& sites = *_game->getSavedGame()->getMissionSites();

			std::unordered_set<int> keep_mission;
			for (const auto& jmission : obj["missions"])
			{
				keep_mission.insert(jmission["mission_id"].asInt());
			}

			// 2) Remove all coop missions whose id is NOT in the keep set
			for (auto it = sites.begin(); it != sites.end();)
			{
				MissionSite* s = *it;
				if (s->_coop && keep_mission.find(s->_coop_mission_id) == keep_mission.end())
				{
					// If SavedGame owns the UFOs, also free memory:
					delete s; // <- omit if someone else owns them (e.g., smart pointers)
					it = sites.erase(it);
				}
				else
				{
					++it;
				}

			}

			// remove alienbases
			auto& alienbases = *_game->getSavedGame()->getAlienBases();

			std::unordered_set<int> keep_alienbase;
			for (const auto& jalienbase : obj["alienbases"])
			{
				keep_alienbase.insert(jalienbase["alienbase_id"].asInt());
			}

			// 2) Remove all coop alienbases whose id is NOT in the keep set
			for (auto it = alienbases.begin(); it != alienbases.end();)
			{
				AlienBase* ab = *it;
				if (ab->_coop && keep_alienbase.find(ab->_coop_alienbase_id) == keep_alienbase.end())
				{
					// If SavedGame owns the UFOs, also free memory:
					delete ab; // <- omit if someone else owns them (e.g., smart pointers)
					it = alienbases.erase(it);
				}
				else
				{
					++it;
				}
			}

			// coop (issue #28): the coop UFO / mission-site mirrors have just been
			// (re)synced above. Rebind any reloaded own craft whose shared
			// destination was stripped from its world blob back to the live mirror
			// (by cross-instance coop id), so it keeps chasing the REAL target
			// instead of the interim waypoint at the stale saved position.
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				for (auto* craft : *base->getCrafts())
				{
					if (!craft->coop)
						craft->relinkCoopDestination(_game->getSavedGame());
				}
			}

		}



	}

	if (stateString == "click_close")
	{
		_onClickClose = true;
	}

	if (stateString == "endTurn")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				pve2_init = true;

				_battleInit = false;
				_isActivePlayerSync = false;
				_isActiveAISync = true;
				_clientPanicHandle = true;
				_waitBC = false;
				_waitBH = false;

				int side = obj["side"].asInt();

				_game->getSavedGame()->getSavedBattle()->setSideCoop(side);

				if (side == 0)
				{

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();
					battlestate->endTurnCoop();

				}
				else
				{
					_game->getSavedGame()->getSavedBattle()->getBattleGame()->endBattleTurnCoop();
				}

			}

		}

	}

	if (stateString == "endPlayerTurn")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				_coopEnd = 0;

				_AIProgressCoop = 100;

				_game->getSavedGame()->getSavedBattle()->setSideCoop(0);

				// end battle
				_game->getSavedGame()->getSavedBattle()->getBattleState()->EndCoopTurn();

			}

		}

	}

	if (stateString == "update_progress")
	{

		int ret = obj["ret"].asInt();
		_AIProgressCoop = ret;

		int selected_unit_id = obj["selected_unit_id"].asInt();

		bool AISecondMove = obj["AISecondMove"].asInt();

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				_AISecondMoveCoop = AISecondMove;

				if (selected_unit_id != -1)
				{

					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{

						if (unit->getId() == selected_unit_id)
						{

							_game->getSavedGame()->getSavedBattle()->setSelectedUnit(unit);
							break;
						}
					}
				}

			}
		}

	}

	if (stateString == "AIProgress")
	{

		int ret = obj["ret"].asInt();
		_AIProgressCoop = ret;

		int side = obj["side"].asInt();

		int selected_unit_id = obj["selected_unit_id"].asInt();

		bool AISecondMove = obj["AISecondMove"].asInt();

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				_game->getSavedGame()->getSavedBattle()->setSideCoop(side);

				_AISecondMoveCoop = AISecondMove;

				if (selected_unit_id != -1)
				{

					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{

						if (unit->getId() == selected_unit_id)
						{

							_game->getSavedGame()->getSavedBattle()->setSelectedUnit(unit);
							break;
						}
					}
				}
				
				if (ret == 0 && _game->getSavedGame()->getSavedBattle()->getBattleGame())
				{

					if (_coopEnd == 0)
					{
				
						_game->getSavedGame()->getSavedBattle()->setSideCoop(2);
						_coopEnd = 1;
					
					}
					else if (_coopEnd == 1)
					{

						_game->getSavedGame()->getSavedBattle()->setSideCoop(0);
						_coopEnd = 0;

					}
					
					_AISecondMoveCoop = false;

					_game->getSavedGame()->getSavedBattle()->setSelectedUnit(0);

				}

			}

		}


	}

	if (stateString == "DebriefingState")
	{

		// MissionStatistics
		bool isMissionStatistics = obj["isMissionStatistics"].asBool();
		if (isMissionStatistics == true)
		{

			// use later...
			_missionStatisticsCoop = obj["missionStatistics"];

		}

		bool abort = obj["abort"].asBool();
		std::string title = obj["title"].asString();
		bool promotions = obj["promotions"].asBool();

		_soldier_stats = obj["soldier_stats"];

		_battle_stats = obj["battle_stats"];

		_AISecondMoveCoop = false;
		_AIProgressCoop = 100;

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{
				_game->getSavedGame()->getSavedBattle()->setSideCoop(0);
				_game->getSavedGame()->getSavedBattle()->setAborted(abort);

				_game->getSavedGame()->getSavedBattle()->getBattleState()->EndCoopBattle();

				_debriefing_coop_title = title;

				_coop_promotions = promotions;

				for (int i = 0; i < obj["soldiers"].size(); i++)
				{

					std::string coopname = obj["soldiers"][i]["coopname"].asString();
					std::string name = obj["soldiers"][i]["name"].asString();
					int nationality = obj["soldiers"][i]["nationality"].asInt();
					int rank_int = obj["soldiers"][i]["rank"].asInt();
					int promoted = obj["soldiers"][i]["promoted"].asInt();

					int init_tu = obj["soldiers"][i]["init_tu"].asInt();
					int coopbase = obj["soldiers"][i]["coopbase"].asInt();

					for (auto& base : *_game->getSavedGame()->getBases())
					{

						for (auto& soldier : *base->getSoldiers())
						{

							if ((soldier->getCoopName() == coopname && coopname != "") || (soldier->getName() == name && name != "") && soldier->getNationality() == nationality && soldier->getInitStats() && soldier->getInitStats()->tu == init_tu && soldier->getCoopBase() == coopbase)
							{
								soldier->setCoopRank(intToSoldierRank(rank_int));
								soldier->setRecentlyPromotedCoop(promoted);

								if (soldier->getCurrentStats())
								{

									int tu = obj["soldiers"][i]["unit_stats"]["tu"].asInt();
									int stamina = obj["soldiers"][i]["unit_stats"]["stamina"].asInt();
									int health = obj["soldiers"][i]["unit_stats"]["health"].asInt();
									int bravery = obj["soldiers"][i]["unit_stats"]["bravery"].asInt();
									int reactions = obj["soldiers"][i]["unit_stats"]["reactions"].asInt();
									int firing = obj["soldiers"][i]["unit_stats"]["firing"].asInt();
									int throwing = obj["soldiers"][i]["unit_stats"]["throwing"].asInt();
									int strength = obj["soldiers"][i]["unit_stats"]["strength"].asInt();
									int psiStrength = obj["soldiers"][i]["unit_stats"]["psiStrength"].asInt();
									int psiSkill = obj["soldiers"][i]["unit_stats"]["psiSkill"].asInt();
									int melee = obj["soldiers"][i]["unit_stats"]["melee"].asInt();
									int mana = obj["soldiers"][i]["unit_stats"]["mana"].asInt();

									UnitStats stats = UnitStats(tu, stamina, health, bravery, reactions, firing, throwing, strength, psiStrength, psiSkill, melee, mana);

									soldier->setCurrentStatsEditableCoop(stats);

								}

								break;
							}
							else if ((soldier->getName() == name && name != "") && soldier->getNationality() == nationality && soldier->getInitStats() && soldier->getInitStats()->tu == init_tu && soldier->getCoopBase() == coopbase)
							{

								soldier->setCoopRank(intToSoldierRank(rank_int));
								soldier->setRecentlyPromotedCoop(promoted);

								if (soldier->getCurrentStats())
								{

									int tu = obj["soldiers"][i]["unit_stats"]["tu"].asInt();
									int stamina = obj["soldiers"][i]["unit_stats"]["stamina"].asInt();
									int health = obj["soldiers"][i]["unit_stats"]["health"].asInt();
									int bravery = obj["soldiers"][i]["unit_stats"]["bravery"].asInt();
									int reactions = obj["soldiers"][i]["unit_stats"]["reactions"].asInt();
									int firing = obj["soldiers"][i]["unit_stats"]["firing"].asInt();
									int throwing = obj["soldiers"][i]["unit_stats"]["throwing"].asInt();
									int strength = obj["soldiers"][i]["unit_stats"]["strength"].asInt();
									int psiStrength = obj["soldiers"][i]["unit_stats"]["psiStrength"].asInt();
									int psiSkill = obj["soldiers"][i]["unit_stats"]["psiSkill"].asInt();
									int melee = obj["soldiers"][i]["unit_stats"]["melee"].asInt();
									int mana = obj["soldiers"][i]["unit_stats"]["mana"].asInt();

									UnitStats stats = UnitStats(tu, stamina, health, bravery, reactions, firing, throwing, strength, psiStrength, psiSkill, melee, mana);

									soldier->setCurrentStatsEditableCoop(stats);
								}

								break;

							}

						}

					}

				}

				_game->pushState(new DebriefingState);

			}
		}

	}

	if (stateString == "psi_result")
	{

		int unit_id = obj["unit_id"].asInt();

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getId() == unit_id)
					{

						if (unit->getCoop() == 0)
						{
							unit->setCoop(1);
						}
						else if (unit->getCoop() == 1)
						{
							unit->setCoop(0);
						}

						unit->_coop_mindcontrolled = true;

						unit->convertToFaction(FACTION_HOSTILE);
						unit->setOriginalFaction(FACTION_HOSTILE);

						break;

					}

				}
			}
		}


	}

	// BATTLESCAPE
	if (stateString == "PlayerTurnYour")
	{

		if (_chatMenu)
		{
			_chatMenu->setActive(false);
		}

		//  selected unit
		int actor_id = obj["actor_id"].asInt();

		int battle_turn = obj["battle_turn"].asInt();

		uint64_t seed = obj["seed"].asUInt64();

		RNG::setSeed(seed);

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				if (getHost() == false)
				{
					_game->getSavedGame()->getSavedBattle()->setTurnCoop(battle_turn);
				}

				_game->getSavedGame()->getSavedBattle()->abortPathCoop();


				if (getHost() == false)
				{

					// tiles
					auto* savedGame = _game->getSavedGame();
					auto* battle = savedGame->getSavedBattle();
					int mapSize = battle->getMapSizeXYZ();

					// 1) Reset all tiles
					for (int i = 0; i < mapSize; ++i)
					{
						Tile* tile = battle->getTile(i);
						tile->setFireCoop(0, 0);
					}

					// 2) Apply JSON data
					const Json::Value& tiles = obj["tiles"];
					for (Json::ArrayIndex json_id = 0; json_id < tiles.size(); ++json_id)
					{
						int tile_pos_x = tiles[json_id]["tile_pos_x"].asInt();
						int tile_pos_y = tiles[json_id]["tile_pos_y"].asInt();
						int tile_pos_z = tiles[json_id]["tile_pos_z"].asInt();

						bool getDangerous = tiles[json_id]["getDangerous"].asBool();
						int getFire = tiles[json_id]["getFire"].asInt();;

						int animation_offset = tiles[json_id]["animation_offset"].asInt();

						// Direct lookup by coordinates
						Tile* tile = battle->getTile(Position(tile_pos_x, tile_pos_y, tile_pos_z));

						if (!tile)
							continue;

						tile->setDangerous(getDangerous);
						tile->setFireCoop(getFire, animation_offset);
					}

					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{

						if (unit->getId() == actor_id && unit->getFaction() == FACTION_PLAYER)
						{

							_game->getSavedGame()->getSavedBattle()->setSelectedUnit(unit);

							_game->getSavedGame()->getSavedBattle()->getBattleGame()->getCurrentAction()->actor = unit;
						}

						for (int i = 0; i < obj["units"].size(); i++)
						{

							int json_id = obj["units"][i]["unit_id"].asInt();

							// Check if the same unit
							if (unit->getId() == json_id)
							{

								int time = obj["units"][i]["time"].asInt();
								int health = obj["units"][i]["health"].asInt();
								int energy = obj["units"][i]["energy"].asInt();
								int morale = obj["units"][i]["morale"].asInt();
								int mana = obj["units"][i]["mana"].asInt();
								int stunlevel = obj["units"][i]["stunlevel"].asInt();
								bool is_out = obj["units"][i]["is_out"].asBool();
								int motionpoints = obj["units"][i]["motionpoints"].asInt();

								int setDirection = obj["units"][i]["setDirection"].asInt();
								int setFaceDirection = obj["units"][i]["setFaceDirection"].asInt();

								int setTurretDirection = obj["units"][i]["setTurretDirection"].asInt();
								int setTurretToDirection = obj["units"][i]["setTurretToDirection"].asInt();

								bool respawn = obj["units"][i]["respawn"].asBool();

								bool fire = obj["units"][i]["fire"].asInt();
								unit->setFireCoop(fire);

								unit->setDirection(setDirection);
								unit->setFaceDirection(setFaceDirection);

								unit->setDirectionTurretCoop(setTurretDirection);
								unit->setTurretToDirectionCoop(setTurretToDirection);

								unit->setMotionPointsCoop(motionpoints);
								unit->setTimeUnits(time);
								unit->setHealth(health);
								unit->setCoopMorale(morale);
								unit->setCoopEnergy(energy);
								unit->setCoopMana(mana);

								unit->setRespawn(respawn);
								unit->setStunlevelCoop(stunlevel);
					
								const Json::Value& fatalArray = obj["units"][i]["fatalWounds"];

								for (int part = 0; part < BODYPART_MAX && part < fatalArray.size(); ++part)
								{
									unit->setFatalWoundCoop(part, fatalArray[part].asInt());
								}
								

								int pos_x = obj["units"][i]["pos_x"].asInt();
								int pos_y = obj["units"][i]["pos_y"].asInt();
								int pos_z = obj["units"][i]["pos_z"].asInt();

								// Check if positions do not match
								if (unit->getPosition().x != pos_x || unit->getPosition().y != pos_y || unit->getPosition().z != pos_z)
								{

									_game->getSavedGame()->getSavedBattle()->getBattleGame()->teleport(pos_x, pos_y, pos_z, unit);
								}

								break;
							}
						}
					}

				}

				// Reset time units and energy at the start of the alien player's turn
				// PVP
				if (getHost() == false && getCoopGamemode() == 2)
				{
					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{
						if (unit->getCoop() == 1)
						{
							unit->resetTimeUnitsAndEnergy();
						}
					}
				}
				// PVP2
				else if (getHost() == true && getCoopGamemode() == 3)
				{
					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{
						if (unit->getCoop() == 0)
						{
							unit->resetTimeUnitsAndEnergy();
						}
					}
				}

			}

			if (getHost() == false)
			{

				// PVP2 fix
				if (getCoopGamemode() == 3)
				{

					_isActivePlayerSync = false;

					_battleInit = false;
					_isActiveAISync = true;

				}
				else
				{

					_isActivePlayerSync = true;
					_isActiveAISync = false;

					setPlayerTurn(2);

				}

			}
			else
			{

				// PVP2 fix
				if (getCoopGamemode() == 3)
				{
					_isActivePlayerSync = true;
					_isActiveAISync = false;

					setPlayerTurn(2);

				}
				else
				{

					_isActivePlayerSync = true;

					_battleInit = false;
					_isActiveAISync = true;

					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();
					battlestate->endTurnCoop();

				}

	

			}

		}


	}

	// new map packet ready to be loaded
	if (stateString == "WAIT_MAP_SENDER")
	{
		isWaitMap = true;
	}

	if (stateString == "WAIT_BATTLESCAPE_HOST_TRUE" && onTcpHost == true)
	{

		_waitBC = true;
	}

	if (stateString == "WAIT_BATTLESCAPE_CLIENT_TRUE" && onTcpHost == false)
	{
		_waitBH = true;
	}

	if (stateString == "close_save_progress")
	{

		// Closing save progress popup - only if one is actually open (silent
		// background pushes don't show the dialog; popping the screen under
		// it would tear down the geoscape). The campaign wait dialogs
		// (60/62/64/65) manage their own lifetime - never pop those.
		if (!_game->getStates().empty())
		{
			CoopState* top = dynamic_cast<CoopState*>(_game->getStates().back());
			if (top && !top->isCampaignWaitDialog())
			{
				_game->popState();
			}
		}

	}

	if (stateString == "MAP_RESULT_LOAD_PROGRESS")
	{

		writeHostMapLoadProgressFile();

		_isLoadProgress = true;

		CoopState* coop = new CoopState(555);
		coop->loadWorld();

	}

	if (stateString == "close_load_progress" && getServerOwner() == true)
	{

		// P2/F1: a battle resume parks the host behind its resume lobby/wait
		// dialogs - resumeCampaign() closes the lobby but leaves the HostMenu
		// beneath the COOP_DLG_RESUME_ACK_WAIT it pushed, and on the battle path
		// the host never gets a resumeAck (it emits campaign_resume_battle
		// instead), so nothing ever pops them. The client has now finished
		// loading the streamed battle (this packet), so return the host to its
		// own BattlescapeState: pop everything above it so BattlescapeState::
		// think() runs and re-arms the coop-init block (_battleInit / role /
		// turn) once COOP_READY sets coopSession below. Gate strictly on the
		// RESUME_ACK_WAIT dialog actually being on the stack so this fires ONLY
		// on a resume, never on a LIVE battle entry - there the host stacks
		// Briefing/Inventory over a fresh battle and also receives
		// close_load_progress, and popping those would eat the briefing.
		bool inBattleResume = false;
		if (_game->getSavedGame() && _game->getSavedGame()->getSavedBattle() != nullptr)
		{
			for (auto* st : _game->getStates())
			{
				CoopState* cs = dynamic_cast<CoopState*>(st);
				if (cs && cs->getStateCode() == COOP_DLG_RESUME_ACK_WAIT)
				{
					inBattleResume = true;
					break;
				}
			}
		}
		if (inBattleResume)
		{
			int guard = 0;
			while (guard++ < 32 && _game->getStates().size() > 1
				&& dynamic_cast<BattlescapeState*>(_game->getStates().back()) == nullptr)
			{
				_game->popState();
			}
		}

		Json::Value root;
		root["state"] = "COOP_READY_CLIENT_REQUEST"; 

		sendTCPPacketData(root.toStyledString());

	}

	if (stateString == "MAP_RESULT_SAVE_PROGRESS")
	{

		std::string jsonData333 = "{\"state\" : \"close_save_progress\"}";
		sendTCPPacketData(jsonData333);

		// Closing save progress popup - only if one is actually open (silent
		// background pushes don't show the dialog). The campaign wait dialogs
		// (60/62/64/65) manage their own lifetime - never pop those.
		if (!_game->getStates().empty())
		{
			CoopState* top = dynamic_cast<CoopState*>(_game->getStates().back());
			if (top && !top->isCampaignWaitDialog())
			{
				_game->popState();
			}
		}

		// WRITE THE FILE RECEIVED FROM THE CLIENT TO THE HOST
		if (_game->getSavedGame())
		{
			// Install the freshest client blob. On validation failure (PRD-07)
			// the store keeps the last-good blob, so the write below stays safe.
			writeHostMapSaveProgressFile();

			// PRD-06/E1: the SaveGameState funnel deferred its write to here, so
			// this is the single emit of the host .sav for this save cycle - and
			// it embeds the client world that just arrived. (No longer gated on
			// the blob being fresh: even a rejected blob leaves a valid last-good
			// one to embed, and the user asked for a save.)
			writePendingHostSave();
		}

	}

	if (stateString == "MAP_RESULT_HOST" && onTcpHost == true)
	{

		// WRITE THE FILE RECEIVED FROM THE CLIENT TO THE HOST
		writeHostMapFile();

		// ASSIGN CLIENT SOLDIERS TO THE HOST
		// DO NOT ASSIGN SOLDIERS IF THE INVENTORY IS CLOSED DURING BATTLE
		if (inventory_battle_window == true)
		{
			setClientSoldiers();
		}
		else
		{
			CoopState* coop = new CoopState(888);
			coop->loadWorld();

			setHost(false);

			std::string jsonData2 = "{\"state\" : \"changeHost3\"}";
			sendTCPPacketData(jsonData2);
		}

	}

	if (stateString == "MAP_RESULT_CLIENT" && onTcpHost == false)
	{

		DebugLog("MAP_RESULT_CLIENT");

		writeHostMapFile();
		loadHostMap();

		// if not save file
		if (inventory_battle_window == true)
		{

			std::string jsonData2 = "{\"state\" : \"setup_battle\"}";
			sendTCPPacketData(jsonData2);
		}
	}

	if (stateString == "setup_battle")
	{

		DebugLog("setup_battle");

		CoopState* coop = new CoopState(765);

		coop->loadWorld();
	}

	// LOAD MAP
	if (stateString == "map_result_data")
	{
		try
		{

			if (obj.isMember("data") && obj["data"].isString())
			{
				std::string map_data = obj["data"].asString();
				mapData += map_data;

				std::string jsonData2 = "{\"state\" : \"WAIT_MAP_SENDER\"}";
				sendTCPPacketData(jsonData2);
			}
			else
			{
				DebugLog("Error: obj missing 'data' or it is not a string.\n");
			}
		}
		catch (const std::exception& e)
		{
			// Build one message for both DebugLog and crash.log
			std::string msg = "Exception in map loading: " + std::string(e.what());

			DebugLog((msg + "\n").c_str());

			// Write separate crash log file to user/logs/...
			CRASH_LOG(msg);
		}
		catch (...)
		{
			std::string msg = "Unknown exception in map loading.";

			DebugLog((msg + "\n").c_str());
			CRASH_LOG(msg);
		}
	}

	if (stateString == "COOP_READY_SAVE_PROGRESS" && onTcpHost == false)
	{

		// MODS
		std::string str_hash;
		for (Json::Value host_mod : obj["mods"])
		{

			std::string host_mod_name = host_mod["name"].asString();

			str_hash += host_mod_name + ";";
		}

		if (!_game->getCoopMod()->hasRequiredMods(str_hash))
		{

			_game->getCoopMod()->disconnectTCP();

			// refused for a mod mismatch: the attempt is over
			closeConnectingDialog();

			_game->pushState(new ModCheckMenu(str_hash));

			return;
		}

		long long saveID = obj["saveID"].asInt64();
		connectionTCP::saveID = saveID;

		tcpPlayerName = obj.get("playername", tcpPlayerName).asString();

		// campaign lobby (new or resume): sit in the lobby until the host
		// clicks START/RESUME CAMPAIGN (flow-redesign F2/F3). A mid-session
		// rejoin (F4) fetches its world straight away; otherwise the classic
		// path (Profile -> request_load_progress).
		bool campaignStarted = obj.get("campaign_started", true).asBool();
		bool rejoin = obj.get("rejoin", false).asBool();
		// PRD-J01: remember the lobby's economy model for the type label (the
		// client has no save yet; the real adoption happens at campaign_start).
		connectionTCP::_lobbyCampaignType = obj.get("campaignType", 0).asInt();

		// Pop the "Connecting..." wait dialog NOW, before pushing the lobby/load
		// dialog over it. forceCloseCoopStateMenu can't reach it once buried (a
		// non-top state gets no think() tick, and LobbyMenu's ctor clears the
		// flag anyway), so it would linger and resurface as a stale
		// "Connecting..." window when the client later leaves the lobby.
		closeConnectingDialog();

		// Every successful join confirms itself with the "You have joined
		// <host>'s game" popup, on top of whatever the join leads to (lobby or
		// load-wait). It used to be suppressed entirely for campaign lobbies.
		if (!campaignStarted)
		{
			connectionTCP::session.lobbyMode = obj.get("lobby_mode", 1).asInt();
			connectionTCP::forceCloseCoopStateMenu = true;
			connectionTCP::forceClosePasswordCheckMenu = true;
			_game->pushState(new LobbyMenu());
			_game->pushState(new Profile);
		}
		else if (rejoin)
		{
			connectionTCP::session.lobbyMode = obj.get("lobby_mode", 0).asInt();
			connectionTCP::forceCloseCoopStateMenu = true;
			connectionTCP::forceClosePasswordCheckMenu = true;

			_game->pushState(new CoopState(COOP_DLG_CLIENT_LOAD_WAIT));

			Json::Value req;
			req["state"] = "request_load_progress";
			sendTCPPacketData(req.toStyledString());

			_game->pushState(new Profile);
		}
		else
		{
			_game->pushState(new Profile);
		}

	}

	if (stateString == "COOP_READY_CLIENT_REQUEST" && onTcpHost == false)
	{

		Json::Value root;
		root["state"] = "COOP_READY_CLIENT";
		sendTCPPacketData(root.toStyledString());

	}

	if (stateString == "COOP_READY_CLIENT_REQUEST_PROFILE" && onTcpHost == false)
	{

		// MODS
		std::string str_hash;
		for (Json::Value host_mod : obj["mods"])
		{

			std::string host_mod_name = host_mod["name"].asString();

			str_hash += host_mod_name + ";";
		}

		if (!_game->getCoopMod()->hasRequiredMods(str_hash))
		{

			_game->getCoopMod()->disconnectTCP();

			// refused for a mod mismatch: the attempt is over
			closeConnectingDialog();

			_game->pushState(new ModCheckMenu(str_hash));

			return;
		}

		connectionTCP::forceCloseCoopStateMenu = true;
		connectionTCP::forceClosePasswordCheckMenu = true;

		tcpPlayerName = obj.get("playername", tcpPlayerName).asString();

		// joined: retire "Connecting..." before the popup covers it (the
		// flag above only fires while the dialog is still the top state)
		closeConnectingDialog();

		_game->pushState(new Profile);

		Json::Value root;
		root["state"] = "COOP_READY_CLIENT";
		sendTCPPacketData(root.toStyledString());

	}

	if (stateString == "INIT_SERVER" && onTcpHost == true)
	{

		// This runs once...
		if (onceTime == false)
		{

			fixCoopSave();

			j_markers = "";

			_battleInit = false;

			// RESET ALL SOLDIERS OUT OF THE BASES
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				for (auto* soldier : *base->getSoldiers())
				{

					if (soldier->getCraft())
					{
						// if co-op soldiers exceed 50%
						if (soldier->getCraft()->getSpaceAvailable() < 0)
						{
							soldier->setCraftAndMoveEquipment(0, base, _game->getSavedGame()->getMonthsPassed() == -1);
						}
					}
				}
			}

			onceTime = true;

		}

		std::string playername = obj.get("playername", "defaultState").asString();
		std::string servername = obj.get("servername", "defaultState").asString();

		// A joiner may never take a name already in use in this session -
		// the host's own name, or any currently attached client's. (The
		// host's name passes the roster check below and would collapse both
		// players into one identity; a DROPPED client's name stays available
		// on purpose - that is how a rejoin identifies itself. With today's
		// single-client transport the attached-client case is preempted by
		// the server-full close, but the check is written for N clients.)
		bool nameInUse = (playername == _game->getCoopMod()->getHostName());
		if (connectionTCP::session.clientInLobby
			&& playername == _game->getCoopMod()->getCurrentClientName())
		{
			nameInUse = true;
		}
		if (nameInUse)
		{
			Json::Value refuse;
			refuse["state"] = "lobby_join_refused";
			refuse["reason"] = "That player name is already in use.";
			sendTCPPacketData(refuse.toStyledString());
			return;
		}

		// Campaign roster gate (flow-redesign D4/D6): once the player list is
		// locked (non-empty), only registered names may connect - covers the
		// resume lobby, mid-session rejoin, and strangers joining a running
		// campaign. An empty list = pre-START lobby, anyone may join.
		if (_game->getCoopMod()->getCoopCampaign() == true
			&& _game->getSavedGame()
			&& _game->getSavedGame()->isCoopSave())
		{
			const auto& registered = _game->getSavedGame()->getCoopPlayers();
			if (!registered.empty())
			{
				bool known = false;
				for (const auto& p : registered)
				{
					if (p == playername)
					{
						known = true;
					}
				}
				if (!known)
				{
					Json::Value refuse;
					refuse["state"] = "lobby_join_refused";
					refuse["reason"] = "You are not a player in this campaign.";
					sendTCPPacketData(refuse.toStyledString());
					return;
				}
			}
		}

		tcpPlayerName = playername;
		tcpServerName = servername;

		Json::Value root;

		// mod check
		std::vector<std::string> mod_names = _game->getMod()->getCoopModList();

		int index = 0;

		for (auto mod_name : mod_names)
		{

			root["mods"][index]["name"] = mod_name;

			index++;
		}

		// password
		if (connectionTCP::isPasswordRequired == true && !OpenXcom::isConnectionUDPActive())
		{

			std::string tcp_password = obj.get("tcp_password", "").asString();

			if (tcp_password != connectionTCP::password)
			{

				Json::Value rootPassword;
				rootPassword["state"] = "tcp_password";

				sendTCPPacketData(rootPassword.toStyledString());

				return;

			}

		}

		// past every gate: a real client is now attached to the session
		connectionTCP::session.clientAttached();

		if (_game->getCoopMod()->getCoopCampaign() == true)
		{
			root["state"] = "COOP_READY_SAVE_PROGRESS";
			// Kept on the wire for older clients; host-save authority is the only mode.
			root["host_save_progress"] = true;
			// campaign lobbies (new or resume): the client joins the lobby
			// instead of requesting a world (flow-redesign F2/F3). A live
			// session (lobby closed) = mid-session rejoin: fetch directly.
			root["campaign_started"] = (connectionTCP::session.lobbyMode == 0 || connectionTCP::session.lobbyClosed == true);
			root["rejoin"] = (connectionTCP::session.lobbyMode != 0 && connectionTCP::session.lobbyClosed == true);
			root["lobby_mode"] = connectionTCP::session.lobbyMode;
			// PRD-J01: tell the joining client the campaign economy model now,
			// before its save exists, so the lobby type label can render.
			root["campaignType"] = _game->getSavedGame()
				? static_cast<int>(_game->getSavedGame()->getCampaignType()) : 0;

			// Handshake fallback only: a resume carries the loaded saveID (nonzero,
			// so skipped here) and a new campaign re-mints in startCampaign and
			// re-broadcasts via campaign_start (which the client adopts). This
			// mint just guarantees the join reply never sends 0 when a client
			// connects before START CAMPAIGN is clicked.
			if (connectionTCP::saveID == 0)
			{
				connectionTCP::saveID = getDateTimeCoop();
			}

			// saveID
			root["saveID"] = static_cast<Json::Int64>(connectionTCP::saveID);
		}
		else
		{
			root["state"] = "COOP_READY_CLIENT_REQUEST_PROFILE";
		}

		root["playername"] = sendTcpPlayer;
		root["servername"] = sendTcpServerName;

		sendTCPPacketData(root.toStyledString());

		// "<player> has joined the game" - shown for every lobby mode. The
		// host's lobby is already open at this point, so this lands on top of
		// it; campaign lobbies used to suppress the popup entirely.
		_game->pushState(new Profile);

	}

	if (stateString == "COOP_READY_CLIENT" && onTcpHost == true)
	{

		coopSession = true;

		Json::Value root;
		root["state"] = "COOP_READY_HOST";
		root["playername"] = sendTcpPlayer; // Client player ID will be added later...
		root["servername"] = sendTcpServerName; // Client player ID will be added later...
		root["gamemode"] = connectionTCP::_coopGamemode;

		// funds
		int64_t funds = 0;
		if (_game->getSavedGame() && _game->getSavedGame()->getFunds())
		{
			funds = _game->getSavedGame()->getFunds();
		}
		root["funds"] = funds;

		int64_t base_count = 0;
		int64_t craft_count = 0;
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

		root["base_count"] = base_count;
		root["craft_count"] = craft_count;

		// is session locked?
		root["isCoopSessionLocked"] = connectionTCP::session.sessionLocked;
		root["isPlayerReady"] = connectionTCP::isPlayerReady;
		if (connectionTCP::isPlayerReady == true && connectionTCP::isPlayersReady == true && connectionTCP::session.sessionLocked == false)
		{
			connectionTCP::session.campaignStarted();
		}

		// research option
		_enable_research_sync = Options::EnableResearchSync;
		root["enable_research_sync"] = _enable_research_sync;

		// time option
		connectionTCP::_enable_time_sync = Options::EnableTimeSync;
		root["enable_time_sync"] = connectionTCP::_enable_time_sync;

		// reaction shoot option (PVP)
		if (getCoopGamemode() == 2 || getCoopGamemode() == 3)
		{
			connectionTCP::_enable_reaction_shoot = Options::EnableReactionFirePvp;
		}

		// reaction shoot
		root["enable_reaction_shoot"] = connectionTCP::_enable_reaction_shoot;

		// other player footsteps sounds
		connectionTCP::_enable_other_player_footsteps = Options::EnableOtherPlayerFootsteps;
		root["enable_other_player_footsteps"] = connectionTCP::_enable_other_player_footsteps;

		// enable host only time speed
		connectionTCP::_enable_host_only_time_speed = Options::EnableHostOnlyTimeSpeed;
		root["enable_host_only_time_speed"] = connectionTCP::_enable_host_only_time_speed;

		// enable XcomEquipmentAliensPVP
		connectionTCP::_enable_xcom_equipment_aliens_pvp = Options::EnableXcomEquipmentAliensPVP;
		root["enable_xcom_equipment_aliens_pvp"] = _enable_xcom_equipment_aliens_pvp;

		// UnbalancedCraftSoldiersLimit
		connectionTCP::_unbalanced_craft_soldiers_limit = Options::UnbalancedCraftSoldiersLimit;
		root["unbalanced_craft_soldiers_limit"] = _unbalanced_craft_soldiers_limit;

		// campaing check
		root["coop_campaign"] = _coopCampaign;

		// battle  check
		bool inBattle = false;

		if (_game->getSavedGame()->getSavedBattle())
		{
			if (_game->getSavedGame()->getSavedBattle()->getBattleGame())
			{
				inBattle = true;
			}
		}

		if (inBattle == false)
		{
			CoopState* coop = new CoopState(777);
			coop->loadWorld();
		}

		root["battle"] = inBattle;

		sendTCPPacketData(root.toStyledString());

	}

	if (stateString == "COOP_READY_HOST" && onTcpHost == false)
	{

		coopSession = true;

		fixCoopSave();

		// coop fix bases..
		j_markers = "";

		_battleInit = false;

		if (onceTime == true)
		{
			return;
		}

		onceTime = true;

		// is session locked? (value-carrying: mirror the host's flag from the
		// wire, then lock if both are ready) - the raw mirror stays; the derived
		// lock funnels through the transition.
		connectionTCP::session.sessionLocked = obj["isCoopSessionLocked"].asBool();
		connectionTCP::isPlayersReady = obj["isPlayerReady"].asBool();
		if (connectionTCP::isPlayerReady == true && connectionTCP::isPlayersReady == true && connectionTCP::session.sessionLocked == false)
		{
			connectionTCP::session.campaignStarted();
		}

		// set current gamemode
		connectionTCP::_coopGamemode = obj["gamemode"].asInt();

		// funds
		int64_t funds = obj["funds"].asInt64();
		playersFunds = funds;

		// crafts
		int64_t crafts = obj["craft_count"].asInt64();
		playersCrafts = crafts;

		// bases
		int64_t base_count = obj["base_count"].asInt64();
		playersBases = base_count;

		// campaign check
		bool host_coop_campaign = obj["coop_campaign"].asBool();
		bool client_coop_campaign = _coopCampaign;

		if (host_coop_campaign != client_coop_campaign)
		{

			// if campaign: nothing to do - the client's world comes from the
			// host (new_game or streamed progress)
			// if new battle
			if (host_coop_campaign == false)
			{
				_game->pushState(new CoopState(3000));

				return;

			}

		}

		// mod check
		std::vector<std::string> client_mod_names = _game->getMod()->getCoopModList();

		bool mod_found = false;
		int client_mod_count = client_mod_names.size();
		int host_mod_count = obj["mods_count"].asInt();

		// research option
		bool enable_research_sync = obj["enable_research_sync"].asBool();
		_enable_research_sync = enable_research_sync;

		// time option
		bool enable_time_sync = obj["enable_time_sync"].asBool();
		connectionTCP::_enable_time_sync = enable_time_sync;

		// reaction shoot option
		bool enable_reaction_shoot = obj["enable_reaction_shoot"].asBool();
		connectionTCP::_enable_reaction_shoot = enable_reaction_shoot;

		// other player footsteps sounds
		bool enable_other_player_footsteps = obj["enable_other_player_footsteps"].asBool();
		connectionTCP::_enable_other_player_footsteps = enable_other_player_footsteps;

		// enable host only time speed
		bool enable_host_only_time_speed = obj["enable_host_only_time_speed"].asBool();
		connectionTCP::_enable_host_only_time_speed = enable_host_only_time_speed;

		// enable XcomEquipmentAliensPVP
		bool enable_xcom_equipment_aliens_pvp = obj["enable_xcom_equipment_aliens_pvp"].asBool();
		connectionTCP::_enable_xcom_equipment_aliens_pvp = enable_xcom_equipment_aliens_pvp;

		// UnbalancedCraftSoldiersLimit
		bool unbalanced_craft_soldiers_limit = obj["unbalanced_craft_soldiers_limit"].asBool();
		connectionTCP::_unbalanced_craft_soldiers_limit = unbalanced_craft_soldiers_limit;

		// CHECK IF THE CLIENT IS IN BATTLE; IF SO, INCLUDE THE HOST, OTHERWISE DO NOTHING
		// IF BOTH ARE IN BATTLE AT THE SAME TIME, CREATE A SEPARATE SESSION
		bool clientInBattle = false;

		if (_game->getSavedGame()->getSavedBattle())
		{
			if (_game->getSavedGame()->getSavedBattle()->getBattleGame())
			{
				clientInBattle = true;
			}
		}

		std::string playername = obj.get("playername", "defaultState").asString();
		std::string servername = obj.get("servername", "defaultState").asString();

		bool inBattle = obj["battle"].asBool();

		tcpPlayerName = playername;
		tcpServerName = servername;

		if (clientInBattle == false)
		{
			CoopState* coop = new CoopState(777);
			coop->loadWorld();
		}

		initProfile(clientInBattle, inBattle);

		// if neither the client nor the host is in battle, then create base icons

		// BASE
		Json::Value markers;

		markers["state"] = "coopBase";
		markers["battle"] = inBattle;

		// funds
		int64_t funds2 = 0;
		if (_game->getSavedGame() && _game->getSavedGame()->getFunds())
		{
			funds2 = _game->getSavedGame()->getFunds();
		}
		markers["funds"] = funds2;

		// crafts
		int64_t base_count2 = 0;
		int64_t craft_count2 = 0;
		if (_game->getSavedGame() && _game->getSavedGame()->getBases())
		{
			for (auto &base : *_game->getSavedGame()->getBases())
			{
				if (base->_coopBase == false)
				{

					for (auto &craft : *base->getCrafts())
					{
						craft_count2++;
					}

					base_count2++;
				}
			}
		}

		markers["base_count"] = base_count2;
		markers["craft_count"] = craft_count2;

		if (connectionTCP::no_bases == false)
		{

			int index = 0;
			for (auto base : *_game->getSavedGame()->getBases())
			{

				if (base->_coopBase == false && base->_coopIcon == false && (base->getLatitude() != 0 || base->getLongitude() != 0))
				{

					markers["markers"][index]["coopbaseid"] = base->_coop_base_id;

					markers["markers"][index]["base"] = base->getName().c_str();
					markers["markers"][index]["lon"] = base->getLongitude();
					markers["markers"][index]["lan"] = base->getLatitude();

					// new!!!

					// new!!!
					// Facilities synchronization
					// facilities
					int facilities_index = 0;
					double tr_coop = 0;
					double radar_range_coop = 0;
					int completedFacilities = 0;
					int mindShields = 0;
					for (const auto* fac : *base->getFacilities())
					{
						if (fac->getBuildTime() != 0)
						{
							continue;
						}

						if (fac->getRules())
						{
							if (fac->getBuildTime() == 0)
							{
								tr_coop = fac->getRules()->getRadarRange();
								if (tr_coop < 10000 && tr_coop > radar_range_coop)
									radar_range_coop = tr_coop;

								if (_game->getCoopMod()->getServerOwner() == false)
								{
									completedFacilities = fac->getRules()->getSizeX() * fac->getRules()->getSizeY();
									if (fac->getRules()->isMindShield() && !fac->getDisabled())
									{
										mindShields = fac->getRules()->getMindShieldPower();
									}

									markers["markers"][index]["facilities"][facilities_index]["radar_chance_coop"] = fac->getRules()->getRadarChance();
									markers["markers"][index]["facilities"][facilities_index]["hyperwave_coop"] = fac->getRules()->isHyperwave();
									markers["markers"][index]["facilities"][facilities_index]["radar_range_coop"] = fac->getRules()->getRadarRange();
									markers["markers"][index]["facilities"][facilities_index]["completedFacilities"] = completedFacilities;
									markers["markers"][index]["facilities"][facilities_index]["mindShields"] = mindShields;

									facilities_index++;
								}
							}
						}
					}

					markers["markers"][index]["radar_range_coop"] = radar_range_coop;

					markers["markers"][index]["getAvailableEngineers"] = base->getAvailableEngineers();
					markers["markers"][index]["getAvailableHangars"] = base->getAvailableHangars();
					markers["markers"][index]["getAvailableLaboratories"] = base->getAvailableLaboratories();
					markers["markers"][index]["getAvailableQuarters"] = base->getAvailableQuarters();
					markers["markers"][index]["getAvailableScientists"] = base->getAvailableScientists();
					markers["markers"][index]["getAvailableSoldiers"] = base->getAvailableSoldiers();
					markers["markers"][index]["getAvailableStores"] = base->getAvailableStores();
					markers["markers"][index]["getAvailableTraining"] = base->getAvailableTraining();
					markers["markers"][index]["getAvailableWorkshops"] = base->getAvailableWorkshops();

					index++;
				}
			}

		}

		sendTCPPacketData(markers.toStyledString());

		// RESET ALL SOLDIERS OUT OF THE BASES(HAPPENS ONCE IN AN ERROR SITUATION)
		for (auto* base : *_game->getSavedGame()->getBases())
		{
			for (auto* soldier : *base->getSoldiers())
			{

				if (soldier->getCraft())
				{
					// if co-op soldiers exceed 50%
					if (soldier->getCraft()->getSpaceAvailable() < 0)
					{
						soldier->setCraftAndMoveEquipment(0, base, _game->getSavedGame()->getMonthsPassed() == -1);
					}
				}
			}
		}
	}

	// COOP BASE HOST
	if (stateString == "coopBase" && onTcpHost == true)
	{

		// PRD-J02: SEPARATE-only mirror-base machinery (creates _coopIcon peer
		// bases). SHARED has one shared world with real bases - never mirror.
		if (isSharedCampaign())
		{
			return;
		}

		bool inBattle = obj["battle"].asBool();

		// funds
		int64_t funds = obj["funds"].asInt64();
		playersFunds = funds;

		// crafts
		int64_t crafts = obj["crafts"].asInt64();
		playersCrafts = crafts;

		// show host profile
		initProfile(false, inBattle);

		Json::Value m_markers;
		Json::Reader reader;

		if (j_markers.empty())
		{

			j_markers = obj["markers"].toStyledString();
		}

		reader.parse(j_markers, m_markers);

		for (Json::Value marker : m_markers)
		{

			std::string s_lon = marker["lon"].asString();
			std::string s_lan = marker["lan"].asString();

			int coopbaseid = marker["coopbaseid"].asInt();

			int getAvailableEngineers = marker["getAvailableEngineers"].asInt();
			int getAvailableHangars = marker["getAvailableHangars"].asInt();
			int getAvailableLaboratories = marker["getAvailableLaboratories"].asInt();
			int getAvailableQuarters = marker["getAvailableQuarters"].asInt();
			int getAvailableScientists = marker["getAvailableScientists"].asInt();
			int getAvailableSoldiers = marker["getAvailableSoldiers"].asInt();
			int getAvailableStores = marker["getAvailableStores"].asInt();
			int getAvailableTraining = marker["getAvailableTraining"].asInt();
			int getAvailableWorkshops = marker["getAvailableWorkshops"].asInt();
			
			double lon = std::stod(s_lon);
			double lan = std::stod(s_lan);

			Base* CoopBase = new Base(_game->getMod());

			CoopBase->setEngineers(getAvailableEngineers);
			CoopBase->coop_hangar = getAvailableHangars;
			CoopBase->coop_laboratory = getAvailableLaboratories;
			CoopBase->coop_quarters = getAvailableQuarters;
			CoopBase->coop_soldiers = getAvailableSoldiers;
			CoopBase->coop_stores = getAvailableStores;
			CoopBase->coop_training = getAvailableTraining;
			CoopBase->coop_workshop = getAvailableWorkshops;
			CoopBase->setScientists(getAvailableScientists);

			CoopBase->_coop_base_id = coopbaseid;

			// new!!!
			CoopBase->_facilitiesCoop = marker["facilities"];
			double radar_range_coop = marker["radar_range_coop"].asDouble();
			CoopBase->_radar_range_coop = radar_range_coop;

			std::string base_name = marker["base"].asString();
			CoopBase->setName(base_name);

			CoopBase->isCoopBase(true);
			CoopBase->_coopIcon = true;

			CoopBase->setLongitude(lon);
			CoopBase->setLatitude(lan);

			_game->getSavedGame()->getBases()->push_back(CoopBase);
		}

		// HOST
		Json::Value markers;

		markers["state"] = "coopBase2";
		markers["gamemode"] = connectionTCP::_coopGamemode;

		int index = 0;
		for (auto base : *_game->getSavedGame()->getBases())
		{

			if (base->_coopBase == false && base->_coopIcon == false && (base->getLatitude() != 0 || base->getLongitude() != 0))
			{

				markers["markers"][index]["coopbaseid"] = base->_coop_base_id;

				markers["markers"][index]["base"] = base->getName().c_str();
				markers["markers"][index]["lon"] = base->getLongitude();
				markers["markers"][index]["lan"] = base->getLatitude();

				markers["markers"][index]["getAvailableEngineers"] = base->getAvailableEngineers();
				markers["markers"][index]["getAvailableHangars"] = base->getAvailableHangars();
				markers["markers"][index]["getAvailableLaboratories"] = base->getAvailableLaboratories();
				markers["markers"][index]["getAvailableQuarters"] = base->getAvailableQuarters();
				markers["markers"][index]["getAvailableScientists"] = base->getAvailableScientists();
				markers["markers"][index]["getAvailableSoldiers"] = base->getAvailableSoldiers();
				markers["markers"][index]["getAvailableStores"] = base->getAvailableStores();
				markers["markers"][index]["getAvailableTraining"] = base->getAvailableTraining();
				markers["markers"][index]["getAvailableWorkshops"] = base->getAvailableWorkshops();

				// new!!!

				// new!!!
				// Facilities synchronization
				// facilities
				int facilities_index = 0;
				double tr_coop = 0;
				double radar_range_coop = 0;
				int completedFacilities = 0;
				int mindShields = 0;
				for (const auto* fac : *base->getFacilities())
				{
					if (fac->getBuildTime() != 0)
					{
						continue;
					}

					if (fac->getRules())
					{
						if (fac->getBuildTime() == 0)
						{
							tr_coop = fac->getRules()->getRadarRange();
							if (tr_coop < 10000 && tr_coop > radar_range_coop)
								radar_range_coop = tr_coop;

							if (_game->getCoopMod()->getServerOwner() == false)
							{
								completedFacilities = fac->getRules()->getSizeX() * fac->getRules()->getSizeY();
								if (fac->getRules()->isMindShield() && !fac->getDisabled())
								{
									mindShields = fac->getRules()->getMindShieldPower();
								}

								markers["markers"][index]["facilities"][facilities_index]["radar_chance_coop"] = fac->getRules()->getRadarChance();
								markers["markers"][index]["facilities"][facilities_index]["hyperwave_coop"] = fac->getRules()->isHyperwave();
								markers["markers"][index]["facilities"][facilities_index]["radar_range_coop"] = fac->getRules()->getRadarRange();
								markers["markers"][index]["facilities"][facilities_index]["completedFacilities"] = completedFacilities;
								markers["markers"][index]["facilities"][facilities_index]["mindShields"] = mindShields;

								facilities_index++;
							}
						}
					}
				}

				markers["markers"][index]["radar_range_coop"] = radar_range_coop;

				index++;
			}
		}

		sendTCPPacketData(markers.toStyledString());
	}

	// new base icon
	if (stateString == "new_base")
	{
		// PRD-J07 (extending the J02 fence list): SEPARATE-only mirror machinery -
		// creates a _coopIcon marker base. SHARED base creation rides the base_new
		// shared_apply, which appends the REAL base on every machine.
		if (isSharedCampaign())
		{
			return;
		}

		std::string s_lon = obj["markers"]["lon"].asString();
		std::string s_lan = obj["markers"]["lan"].asString();

		int coopbaseid = obj["markers"]["coopbaseid"].asInt();

		int getAvailableEngineers = obj["markers"]["getAvailableEngineers"].asInt();
		int getAvailableHangars = obj["markers"]["getAvailableHangars"].asInt();
		int getAvailableLaboratories = obj["markers"]["getAvailableLaboratories"].asInt();
		int getAvailableQuarters = obj["markers"]["getAvailableQuarters"].asInt();
		int getAvailableScientists = obj["markers"]["getAvailableScientists"].asInt();
		int getAvailableSoldiers = obj["markers"]["getAvailableSoldiers"].asInt();
		int getAvailableStores = obj["markers"]["getAvailableStores"].asInt();
		int getAvailableTraining = obj["markers"]["getAvailableTraining"].asInt();
		int getAvailableWorkshops = obj["markers"]["getAvailableWorkshops"].asInt();

		double lon = std::stod(s_lon);
		double lan = std::stod(s_lan);

		Base* CoopBase = new Base(_game->getMod());

		CoopBase->setEngineers(getAvailableEngineers);
		CoopBase->coop_hangar = getAvailableHangars;
		CoopBase->coop_laboratory = getAvailableLaboratories;
		CoopBase->coop_quarters = getAvailableQuarters;
		CoopBase->coop_soldiers = getAvailableSoldiers;
		CoopBase->coop_stores = getAvailableStores;
		CoopBase->coop_training = getAvailableTraining;
		CoopBase->coop_workshop = getAvailableWorkshops;
		CoopBase->setScientists(getAvailableScientists);

		std::string base_name = obj["markers"]["base"].asString();
		CoopBase->setName(base_name);

		CoopBase->isCoopBase(true);
		CoopBase->_coopIcon = true;

		CoopBase->_coop_base_id = coopbaseid;

		CoopBase->setLongitude(lon);
		CoopBase->setLatitude(lan);

		_game->getSavedGame()->getBases()->push_back(CoopBase);

		// add to the list
		Json::Value m_markers;
		Json::Reader reader;
		reader.parse(j_markers, m_markers);

		m_markers.append(obj["markers"]);

		j_markers = m_markers.toStyledString();
	}

	// NEW COOP BASE REQUEST
	if (stateString == "baseRequest")
	{

		// PRD-J02: SEPARATE-only mirror machinery; never in SHARED. Also guard the
		// save deref: a SHARED replica can receive stray packets before its world
		// exists, and the loop below dereferences the SavedGame.
		if (isSharedCampaign() || !_game->getSavedGame())
		{
			return;
		}

		Json::Value markers;

		int index = 0;
		for (auto base : *_game->getSavedGame()->getBases())
		{

			if (base->_coopBase == false && base->_coopIcon == false && (base->getLongitude() != 0 || base->getLatitude() != 0))
			{

				markers["markers"][index]["coopbaseid"] = base->_coop_base_id;

				markers["markers"][index]["base"] = base->getName().c_str();
				markers["markers"][index]["lon"] = base->getLongitude();
				markers["markers"][index]["lan"] = base->getLatitude();

				markers["markers"][index]["getAvailableEngineers"] = base->getAvailableEngineers();
				markers["markers"][index]["getAvailableHangars"] = base->getAvailableHangars();
				markers["markers"][index]["getAvailableLaboratories"] = base->getAvailableLaboratories();
				markers["markers"][index]["getAvailableQuarters"] = base->getAvailableQuarters();
				markers["markers"][index]["getAvailableScientists"] = base->getAvailableScientists();
				markers["markers"][index]["getAvailableSoldiers"] = base->getAvailableSoldiers();
				markers["markers"][index]["getAvailableStores"] = base->getAvailableStores();
				markers["markers"][index]["getAvailableTraining"] = base->getAvailableTraining();
				markers["markers"][index]["getAvailableWorkshops"] = base->getAvailableWorkshops();

				// new!!!
				// Facilities synchronization
				// facilities
				int facilities_index = 0;
				double tr_coop = 0;
				double radar_range_coop = 0;
				int completedFacilities = 0;
				int mindShields = 0;
				for (const auto* fac : *base->getFacilities())
				{
					if (fac->getBuildTime() != 0)
					{
						continue;
					}

					if (fac->getRules())
					{
						if (fac->getBuildTime() == 0)
						{
							tr_coop = fac->getRules()->getRadarRange();
							if (tr_coop < 10000 && tr_coop > radar_range_coop)
								radar_range_coop = tr_coop;

							if (_game->getCoopMod()->getServerOwner() == false)
							{
								completedFacilities = fac->getRules()->getSizeX() * fac->getRules()->getSizeY();
								if (fac->getRules()->isMindShield() && !fac->getDisabled())
								{
									mindShields = fac->getRules()->getMindShieldPower();
								}

								markers["markers"][index]["facilities"][facilities_index]["radar_chance_coop"] = fac->getRules()->getRadarChance();
								markers["markers"][index]["facilities"][facilities_index]["hyperwave_coop"] = fac->getRules()->isHyperwave();
								markers["markers"][index]["facilities"][facilities_index]["radar_range_coop"] = fac->getRules()->getRadarRange();
								markers["markers"][index]["facilities"][facilities_index]["completedFacilities"] = completedFacilities;
								markers["markers"][index]["facilities"][facilities_index]["mindShields"] = mindShields;

								facilities_index++;
							}
						}
					}
				}

				markers["markers"][index]["radar_range_coop"] = radar_range_coop;

				index++;
			}
		}

		if (getHost() == false && connectionTCP::no_bases == false)
		{

			markers["state"] = "coopBase3";

			sendTCPPacketData(markers.toStyledString());
		}
		else if (getHost() == true)
		{

			markers["state"] = "coopBase2";
			markers["gamemode"] = connectionTCP::_coopGamemode;

			sendTCPPacketData(markers.toStyledString());
		}
	}

	// COOP BASE CLIENT
	if (stateString == "coopBase2" && onTcpHost == false)
	{

		// PRD-J02: SEPARATE-only mirror-base machinery. Never in SHARED.
		if (isSharedCampaign())
		{
			return;
		}

		if (getServerOwner() == false)
		{
			int gamemode = obj["gamemode"].asInt();
			connectionTCP::_coopGamemode = gamemode;
		}

		Json::Value m_markers;
		Json::Reader reader;

		// if there are markers
		if (obj["markers"].empty() == false && obj.isMember("markers") == true)
		{
			j_markers = obj["markers"].toStyledString();
		}

		reader.parse(j_markers, m_markers);

		for (Json::Value marker : m_markers)
		{

			std::string s_lon = marker["lon"].asString();
			std::string s_lan = marker["lan"].asString();

			int coopbaseid = marker["coopbaseid"].asInt();

			int getAvailableEngineers = marker["getAvailableEngineers"].asInt();
			int getAvailableHangars = marker["getAvailableHangars"].asInt();
			int getAvailableLaboratories = marker["getAvailableLaboratories"].asInt();
			int getAvailableQuarters = marker["getAvailableQuarters"].asInt();
			int getAvailableScientists = marker["getAvailableScientists"].asInt();
			int getAvailableSoldiers = marker["getAvailableSoldiers"].asInt();
			int getAvailableStores = marker["getAvailableStores"].asInt();
			int getAvailableTraining = marker["getAvailableTraining"].asInt();
			int getAvailableWorkshops = marker["getAvailableWorkshops"].asInt();

			double lon = std::stod(s_lon);
			double lan = std::stod(s_lan);

			std::string base_name = marker["base"].asString();

			// Check that the base does not already exist
			bool alreadyExists = false;
			for (Base* existingBase : *_game->getSavedGame()->getBases())
			{
				if (existingBase->_coop_base_id == coopbaseid &&
					(existingBase->getLongitude() == lon && existingBase->getLatitude() == lan))
				{

					existingBase->setEngineers(getAvailableEngineers);
					existingBase->coop_hangar = getAvailableHangars;
					existingBase->coop_laboratory = getAvailableLaboratories;
					existingBase->coop_quarters = getAvailableQuarters;
					existingBase->coop_soldiers = getAvailableSoldiers;
					existingBase->coop_stores = getAvailableStores;
					existingBase->coop_training = getAvailableTraining;
					existingBase->coop_workshop = getAvailableWorkshops;
					existingBase->setScientists(getAvailableScientists);

					// new !!!
					existingBase->_facilitiesCoop = marker["facilities"];
					double radar_range_coop = marker["radar_range_coop"].asDouble();
					existingBase->_radar_range_coop = radar_range_coop;

					alreadyExists = true;
					break;
				}
			}

			if (alreadyExists)
				continue;

			Base* CoopBase = new Base(_game->getMod());

			CoopBase->setEngineers(getAvailableEngineers);
			CoopBase->coop_hangar = getAvailableHangars;
			CoopBase->coop_laboratory = getAvailableLaboratories;
			CoopBase->coop_quarters = getAvailableQuarters;
			CoopBase->coop_soldiers = getAvailableSoldiers;
			CoopBase->coop_stores = getAvailableStores;
			CoopBase->coop_training = getAvailableTraining;
			CoopBase->coop_workshop = getAvailableWorkshops;
			CoopBase->setScientists(getAvailableScientists);

			CoopBase->_coop_base_id = coopbaseid;

			// new !!!
			CoopBase->_facilitiesCoop = marker["facilities"];
			double radar_range_coop = marker["radar_range_coop"].asDouble();
			CoopBase->_radar_range_coop = radar_range_coop;

			CoopBase->setName(base_name);

			CoopBase->isCoopBase(true);
			CoopBase->_coopIcon = true;

			CoopBase->setLongitude(lon);
			CoopBase->setLatitude(lan);

			_game->getSavedGame()->getBases()->push_back(CoopBase);
		}
	}

	// COOP BASE HOST
	if (stateString == "coopBase3" && getHost() == true)
	{

		// PRD-J02: SEPARATE-only mirror-base machinery. Never in SHARED.
		if (isSharedCampaign())
		{
			return;
		}

		Json::Value m_markers;
		Json::Reader reader;

		// if there are markers
		if (obj["markers"].empty() == false && obj.isMember("markers") == true)
		{

			j_markers = obj["markers"].toStyledString();
		}

		reader.parse(j_markers, m_markers);

		for (Json::Value marker : m_markers)
		{

			std::string s_lon = marker["lon"].asString();
			std::string s_lan = marker["lan"].asString();

			int coopbaseid = marker["coopbaseid"].asInt();

			int getAvailableEngineers = marker["getAvailableEngineers"].asInt();
			int getAvailableHangars = marker["getAvailableHangars"].asInt();
			int getAvailableLaboratories = marker["getAvailableLaboratories"].asInt();
			int getAvailableQuarters = marker["getAvailableQuarters"].asInt();
			int getAvailableScientists = marker["getAvailableScientists"].asInt();
			int getAvailableSoldiers = marker["getAvailableSoldiers"].asInt();
			int getAvailableStores = marker["getAvailableStores"].asInt();
			int getAvailableTraining = marker["getAvailableTraining"].asInt();
			int getAvailableWorkshops = marker["getAvailableWorkshops"].asInt();

			double lon = std::stod(s_lon);
			double lan = std::stod(s_lan);

			std::string base_name = marker["base"].asString();

			// Check that the base does not already exist
			bool alreadyExists = false;
			for (Base* existingBase : *_game->getSavedGame()->getBases())
			{
				if (existingBase->_coop_base_id == coopbaseid &&
					(existingBase->getLongitude() == lon && existingBase->getLatitude() == lan))
				{

					existingBase->setEngineers(getAvailableEngineers);
					existingBase->coop_hangar = getAvailableHangars;
					existingBase->coop_laboratory = getAvailableLaboratories;
					existingBase->coop_quarters = getAvailableQuarters;
					existingBase->coop_soldiers = getAvailableSoldiers;
					existingBase->coop_stores = getAvailableStores;
					existingBase->coop_training = getAvailableTraining;
					existingBase->coop_workshop = getAvailableWorkshops;
					existingBase->setScientists(getAvailableScientists);

					// new !!!
					existingBase->_facilitiesCoop = marker["facilities"];
					double radar_range_coop = marker["radar_range_coop"].asDouble();
					existingBase->_radar_range_coop = radar_range_coop;

					alreadyExists = true;
					break;
				}
			}

			if (alreadyExists)
				continue;

			Base* CoopBase = new Base(_game->getMod());

			CoopBase->setEngineers(getAvailableEngineers);
			CoopBase->coop_hangar = getAvailableHangars;
			CoopBase->coop_laboratory = getAvailableLaboratories;
			CoopBase->coop_quarters = getAvailableQuarters;
			CoopBase->coop_soldiers = getAvailableSoldiers;
			CoopBase->coop_stores = getAvailableStores;
			CoopBase->coop_training = getAvailableTraining;
			CoopBase->coop_workshop = getAvailableWorkshops;
			CoopBase->setScientists(getAvailableScientists);

			CoopBase->_coop_base_id = coopbaseid;

			// new !!!
			CoopBase->_facilitiesCoop = marker["facilities"];
			double radar_range_coop = marker["radar_range_coop"].asDouble();
			CoopBase->_radar_range_coop = radar_range_coop;

			CoopBase->setName(base_name);

			CoopBase->isCoopBase(true);
			CoopBase->_coopIcon = true;

			CoopBase->setLongitude(lon);
			CoopBase->setLatitude(lan);

			_game->getSavedGame()->getBases()->push_back(CoopBase);
		}
	}

	if (stateString == "craftSoldiers" && onTcpHost == false)
	{

		std::string craftUsed = obj.get("spaceUsed", "0").asString();

		setHostSpaceAvailable(std::stoi(craftUsed));

		generateCraftSoldiers();
	}

	if (stateString == "SEND_FILE_CLIENT_TRUE" && onTcpHost == false)
	{

		_game->getCoopMod()->load_state = "Synchronization finished";

		bool target = obj["target"].asBool();

		if (target == true && _game->getSavedGame())
		{

			bool isUFO = obj["isUFO"].asBool();

			double lat = obj["lat"].asDouble();
			double lon = obj["lon"].asDouble();

			// mission site
			if (isUFO == false)
			{

				auto& missions = *_game->getSavedGame()->getMissionSites();

				for (auto it = missions.begin(); it != missions.end(); ++it)
				{
					if (*it && (*it)->getLatitude() == lat && (*it)->getLongitude() == lon)
					{
						(*it)->setSecondsRemaining(0);
					}
				}

			}
			// ufo
			else
			{

				auto& ufos = *_game->getSavedGame()->getUfos();

				for (auto it = ufos.begin(); it != ufos.end(); ++it)
				{
					if (*it && (*it)->getLatitude() == lat && (*it)->getLongitude() == lon)
					{
						(*it)->setStatusCoop(Ufo::DESTROYED);
					}
				}

			}

			sendBaseFile();

		}

		// Stash the geoscape world in memory so the player can return to it
		// after the mission (never written to disk). Outside the target check
		// so the mission-end reload always has a snapshot.
		if (_game->getSavedGame() && _game->getCoopMod()->getCoopCampaign() == true)
		{
			_game->getSavedGame()->saveCoopToMemory("coop_geoscape_return", _game->getMod(), "coop_geoscape_return");
		}

		CoopState* coopWindow = new CoopState(1);
		_game->pushState(coopWindow);

		std::string jsonData = "{\"state\" : \"SEND_FILE_CLIENT\"}";

		sendTCPPacketData(jsonData);

		_game->getCoopMod()->load_state = "Requesting map data";

	}

	if (stateString == "SEND_FILE_CLIENT_SAVE" && onTcpHost == true)
	{

		CoopState* coopWindow = new CoopState(666);
		coopWindow->loadWorld();

		sendFileClient = true;
	}

	if (stateString == "SEND_FILE_HOST_SAVE" && onTcpHost == true)
	{

		// HERE ON THE HOST, DISPLAY A NOTIFICATION AND SEND THE CLIENT INFORMATION ABOUT THE FILE TRANSFER
		inventory_battle_window = false;

		_game->pushState(new CoopState(1));

		Json::Value root;

		root["state"] = "SEND_FILE_CLIENT_SAVE_TRUE";

		sendTCPPacketData(root.toStyledString().c_str());

		setHost(false);
	}

	if (stateString == "SEND_FILE_CLIENT_SAVE_TRUE" && onTcpHost == false)
	{

		CoopState* coopWindow = new CoopState(666);
		coopWindow->loadWorld();

		sendFileSave = true;
		sendFileClient = true;

		setHost(true);
	}

	if (stateString == "SEND_FILE_CLIENT" && onTcpHost == true)
	{
		sendFileClient = true;
	}

	// INFORMATION FROM HOST TO CLIENT ABOUT MAP LOADING!
	if (stateString == "SEND_FILE_HOST_TRUE" && onTcpHost == true)
	{
		Json::Value root;

		root["state"] = "SEND_FILE_HOST";

		sendTCPPacketData(root.toStyledString());
	}

	// INFORMATION FROM CLIENT TO HOST ABOUT MAP LOADING
	if (stateString == "SEND_FILE_HOST" && onTcpHost == false)
	{

		sendBaseFile();

		_game->getCoopMod()->load_state = "Sending base data";  

		sendFileHost = true;
	}

	if (stateString == "SEND_FILE_HOST_TRUE_SAVE_PROGRESS")
	{

		Json::Value root;

		root["state"] = "SEND_FILE_HOST_SAVE_PROGRESS";

		sendTCPPacketData(root.toStyledString());
	}

	if (stateString == "SEND_FILE_HOST_SAVE_PROGRESS")
	{

		_game->getCoopMod()->load_state = "Saving";

		sendFileHost = true;
		sendProgressSaveFileToHost = true;
	}

	// BASES
	if (stateString == "SEND_FILE_HOST_BASE" && onTcpHost == false)
	{

		sendBaseFile();

		sendFileHost = true;
		sendFileBase = true;
	}

	if (stateString == "SEND_FILE_CLIENT_BASE" && onTcpHost == true)
	{

		sendBaseFile();

		sendFileClient = true;
		sendFileBase = true;
	}

	if (stateString == "MAP_RESULT_CLIENT_BASE" && onTcpHost == false)
	{

		writeHostMapFile2();

		CoopState* coopWindow = new CoopState(55);
		coopWindow->loadWorld();

	}

	if (stateString == "MAP_RESULT_HOST_BASE" && onTcpHost == true)
	{

		writeHostMapFile2();

		CoopState* coopWindow = new CoopState(55);
		coopWindow->loadWorld();

	}
}

void connectionTCP::sendBaseFile()
{

	// saving is not allowed if in battle and inside another player's base!
	if (!_game->getSavedGame()->getSavedBattle() && _game->getCoopMod()->playerInsideCoopBase == false)
	{
		if (_game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->saveCoopToMemory("basehost", _game->getMod(), "basehost");
		}
	}

}

void connectionTCP::setPauseOn()
{

}

void connectionTCP::setPauseOff()
{

}

std::string connectionTCP::getPing()
{
	return current_ping;
}

bool connectionTCP::isCoopSession()
{
	return coopSession;
}

void connectionTCP::setCoopSession(bool session)
{
	coopSession = session;
}

void connectionTCP::setServerOwner(bool owner)
{
	session.setRole(owner ? CoopRole::Host : CoopRole::None);
}

void connectionTCP::setCoopCampaign(bool coop)
{
	_coopCampaign = coop;
}

bool connectionTCP::getCoopCampaign()
{
	return _coopCampaign;
}

// PRD-J01: true only when the active save is a SHARED co-op campaign.
bool connectionTCP::isSharedCampaign()
{
	SavedGame* save = _game ? _game->getSavedGame() : nullptr;
	return save && save->isCoopSave()
		&& save->getCampaignType() == CoopCampaignType::Shared;
}

// Static mirror of isSharedCampaign() for engine-level callers that hold no CoopMod
// instance (e.g. Craft capacity accounting). Reads the same authoritative save via the
// static Game pointer.
bool connectionTCP::isSharedCampaignStatic()
{
	SavedGame* save = _staticGame ? _staticGame->getSavedGame() : nullptr;
	return save && save->isCoopSave()
		&& save->getCampaignType() == CoopCampaignType::Shared;
}

// PRD-J02: a SHARED client holds a replica of the host's single authoritative
// world. Host = seat 0 owns the world; every other seat is a replica.
bool connectionTCP::isSharedReplica()
{
	return isSharedCampaign() && !getHost();
}

// PRD-J02: hand the host's authoritative world to the single-client streamer.
// Serializes FRESH (not a stale stored blob) into a scratch key, then routes it
// through the same resume-blob lane the client already knows how to adopt
// (streamer sendProgressLoadBlob path -> MAP_RESULT_LOAD_PROGRESS ->
// CoopState(555) -> LoadGameState). Reuses the existing file-transfer chunking;
// no second chunk protocol. Host only; caller must ensure the streamer is idle.
void connectionTCP::streamSharedWorldToClient()
{
	if (!getServerOwner() || !_game->getSavedGame())
	{
		return;
	}

	const std::string key = "shared_world";
	connectionTCP::saveError = false;
	_game->getSavedGame()->saveCoopToMemory(key, _game->getMod(), key);

	std::string blob;
	{
		std::lock_guard<std::mutex> lock(coopFilesMutex);
		auto it = coopFilesHost.find(key);
		if (it != coopFilesHost.end())
		{
			blob = it->second;
		}
	}

	if (blob.empty())
	{
		Log(LOG_ERROR) << "[coop-shared] streamSharedWorldToClient: no world blob"
		               << " (saveError=" << connectionTCP::saveError << ")";
		return;
	}

	// snapshot for the streamer thread (same fields request_load_progress sets)
	sendProgressLoadBlob = blob;
	sendProgressLoadFileToClient = key;
	sendFileClient = true;

	Log(LOG_INFO) << "[coop-shared] streaming authoritative world to client ("
	              << blob.size() << " bytes)";
}

// PRD-J10: desync repair. A replica reported a world-checksum mismatch; hand it a
// fresh authoritative world down the same J02 bootstrap lane. The release flag is
// the load-bearing half: LoadGameState parks EVERY client that adopts a streamed
// world in COOP_DLG_CLIENT_RESUME_HOLD until a campaign_begun arrives, and
// mid-session there is no operator BEGIN click to send one (PRD-J09 learned this
// the hard way after battles). resume_ack releases it when this flag is set.
void connectionTCP::sharedResyncStream()
{
	if (!getServerOwner() || !isSharedCampaign() || !_game->getSavedGame())
	{
		return;
	}
	if (sendFileClient)
	{
		// The streamer is single-slot and already busy (bootstrap/resume/post-battle
		// transfer in flight). Drop this request rather than corrupt that transfer -
		// the replica's next mismatching checksum re-asks once its guard expires.
		Log(LOG_WARNING) << "[coop-shared] resync request dropped: streamer busy";
		return;
	}

	sharedResyncRestream = true;
	streamSharedWorldToClient();
	if (!sendFileClient)
	{
		// serialization refused: nothing is in flight, so do not leave the
		// auto-release armed for an unrelated future stream.
		sharedResyncRestream = false;
		Log(LOG_ERROR) << "[coop-shared] resync restream failed to serialize the world";
	}
}

// PRD-J01: this machine's seat. Host is always 0; a client's seat is its
// roster index, carried today by coop_save_owner_player_id (2-player: 1).
// Byte-identical to the historical `getHost() ? 0 : 1` in 2-player play.
int connectionTCP::localSeat()
{
	if (getHost())
		return 0;
	return coop_save_owner_player_id != 0 ? coop_save_owner_player_id : 1;
}

// PRD-J01: active roster size (host + clients). Falls back to the legacy
// 2-player count before the roster locks.
int connectionTCP::seatCount()
{
	if (_staticGame && _staticGame->getSavedGame())
	{
		size_t n = _staticGame->getSavedGame()->getCoopPlayers().size();
		if (n > 0)
			return static_cast<int>(n);
	}
	return 2;
}

// PRD-J01: player name for a seat, or empty if out of range / no roster.
std::string connectionTCP::seatName(int seat)
{
	if (_staticGame && _staticGame->getSavedGame())
	{
		const auto& roster = _staticGame->getSavedGame()->getCoopPlayers();
		if (seat >= 0 && static_cast<size_t>(seat) < roster.size())
			return roster[seat];
	}
	return std::string();
}

int connectionTCP::getCoopGamemode()
{
	return _coopGamemode;
}

void connectionTCP::createCoopMenu()
{

	// If the player has created a server or joined another player's game, create the LobbyMenu
	if (_game->getCoopMod()->isConnected() == 1 || _game->getCoopMod()->getServerOwner() == true)
	{
		_game->pushState(new LobbyMenu());
	}
	else
	{
		_game->pushState(new ServerList());
	}

	if (Options::logPacketMessages == true)
	{
		_game->pushState(new CoopState(942));
	}

}

void connectionTCP::sendTCPPacketStaticData2(std::string data)
{
	enqueueTx(std::move(data));
}

void connectionTCP::writeHostMapFile2()
{
	
	if (mapData.empty())
		return;

	{
		std::lock_guard<std::mutex> lock(coopFilesMutex);
		if (connectionTCP::getServerOwner() == true)
		{
			connectionTCP::coopFilesHost["baseclient"] = std::move(mapData);
		}
		else
		{
			connectionTCP::coopFilesClient["baseclient"] = std::move(mapData);
		}
	}

	// the map data must be reset for the next use (fix)
	mapData = "";

}

void connectionTCP::setHost(bool host)
{
	onTcpHost = host;
}

bool connectionTCP::getHost()
{
	return onTcpHost;
}

int connectionTCP::getHostSpaceAvailable()
{
	return _hostSpace;
}

void connectionTCP::setHostSpaceAvailable(int hostSpace)
{
	_hostSpace = hostSpace;
}

bool connectionTCP::getCoopStatic()
{

	bool coop = false;

	if (onConnect == 1)
	{
		coop = true;
	}

	return coop;
}

void connectionTCP::loadHostMap()
{

	CoopState* coopWindow = new CoopState(2);
	coopWindow->loadWorld();
}

void connectionTCP::sendMissionFile()
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

			CoopState* coopWindow = new CoopState(66);
			_game->pushState(coopWindow);
		}
		else
		{

			// saving files
			if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->saveCoopToMemory("battlehost", _game->getMod(), "battlehost");
			}
			else if (_game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->saveCoopToMemory("battlehost", _game->getMod(), "battlehost");
			}

			_game->getCoopMod()->load_state = "Saving";

			Json::Value obj;
			obj["state"] = "SEND_FILE_HOST_TRUE";

			_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());
		}
	}
	// Host sends the file to the client
	else
	{

		// Save the player ID that owns the co-op save
		if (_game->getCoopMod()->getServerOwner() == false)
		{
			connectionTCP::coop_save_owner_player_id = 1;
		}
		// Ensure the server owner's coop_save_owner_player_id value is set to 0.
		else
		{
			connectionTCP::coop_save_owner_player_id = 0;
		}

		if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->saveCoopToMemory("battlehost", _game->getMod(), "battlehost");
		}
		else if (_game->getCoopMod()->coopMissionEnd == false)
		{
			_game->getSavedGame()->saveCoopToMemory("battlehost", _game->getMod(), "battlehost");
		}

		Json::Value obj;
		obj["state"] = "SEND_FILE_CLIENT_TRUE";
		obj["target"] = false;

		// Delete coop UFOs or missions
		if (getSelectedCraft() && getCoopCampaign() == true)
		{

			Ufo* u = dynamic_cast<Ufo*>(getSelectedCraft()->getDestination());
			MissionSite* m = dynamic_cast<MissionSite*>(getSelectedCraft()->getDestination());

			if (u)
			{
				obj["target"] = true;
				obj["lat"] = u->getLatitude();
				obj["lon"] = u->getLongitude();
				obj["isUFO"] = true;
			}
			else if (m)
			{
				obj["target"] = true;
				obj["lat"] = m->getLatitude();
				obj["lon"] = m->getLongitude();
				obj["isUFO"] = false;
			}


		}

		_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());
	}

}

void connectionTCP::sendSaveProgressFile()
{

	if (_game->getCoopMod()->playerInsideCoopBase == true && _game->getCoopMod()->getCoopCampaign() == true)
	{

		// Go to the Geoscape to begin saving progress.
			
		_game->getCoopMod()->playerInsideCoopBase = false;

		_game->getCoopMod()->ready_coop_save_progress = true;

		CoopState* coopWindow = new CoopState(67);
		_game->pushState(coopWindow);
			
	}
	else
	{

		CoopState* coopWindow = new CoopState(53);
		_game->pushState(coopWindow);

		// saving files
		std::string filename = clientBlobKey(_game->getCoopMod()->getHostName());

		_game->getSavedGame()->saveCoopToMemory(filename, _game->getMod(), filename);
		{
			std::lock_guard<std::mutex> lock(coopFilesMutex);
			eraseStaleBlobEntries(coopFilesClient, "client_", _game->getCoopMod()->getHostName(), filename);
		}

		_game->getCoopMod()->load_state = "Saving";

		Json::Value obj;
		obj["state"] = "SEND_FILE_HOST_TRUE_SAVE_PROGRESS";

		_game->getCoopMod()->sendTCPPacketData(obj.toStyledString());
	}
	
}

int connectionTCP::getCurrentTurn()
{
	if (_game->getSavedGame()->getSavedBattle())
	{

		if (_game->getSavedGame()->getSavedBattle()->getBattleState())
		{
			return _game->getSavedGame()->getSavedBattle()->getBattleState()->getCurrentTurn();
		}
	}

	return -1;
}

ChatMenu* connectionTCP::getChatMenu()
{
	return _chatMenu;
}

void connectionTCP::setChatMenu(ChatMenu* menu)
{
	_chatMenu = menu;
}

int connectionTCP::unitstatusToInt(UnitStatus status)
{
	if (status == STATUS_STANDING)
		return 0;
	if (status == STATUS_WALKING)
		return 1;
	if (status == STATUS_FLYING)
		return 2;
	if (status == STATUS_TURNING)
		return 3;
	if (status == STATUS_AIMING)
		return 4;
	if (status == STATUS_COLLAPSING)
		return 5;
	if (status == STATUS_DEAD)
		return 6;
	if (status == STATUS_UNCONSCIOUS)
		return 7;
	if (status == STATUS_PANICKING)
		return 8;
	if (status == STATUS_BERSERK)
		return 9;
	if (status == STATUS_IGNORE_ME)
		return 10;
	return 10;
}

UnitStatus connectionTCP::intToUnitstatus(int status)
{
	if (status == 0)
		return STATUS_STANDING;
	if (status == 1)
		return STATUS_WALKING;
	if (status == 2)
		return STATUS_FLYING;
	if (status == 3)
		return STATUS_TURNING;
	if (status == 4)
		return STATUS_AIMING;
	if (status == 5)
		return STATUS_COLLAPSING;
	if (status == 6)
		return STATUS_DEAD;
	if (status == 7)
		return STATUS_UNCONSCIOUS;
	if (status == 8)
		return STATUS_PANICKING;
	if (status == 9)
		return STATUS_BERSERK;
	if (status == 10)
		return STATUS_IGNORE_ME;
	return STATUS_IGNORE_ME;
}

int connectionTCP::ufostatusToInt(Ufo::UfoStatus status)
{
	if (status == Ufo::FLYING)
		return 0;
	if (status == Ufo::LANDED)
		return 1;
	if (status == Ufo::CRASHED)
		return 2;
	if (status == Ufo::DESTROYED)
		return 3;
	if (status == Ufo::IGNORE_ME)
		return 4;
	return 4;
}

Ufo::UfoStatus connectionTCP::intToUfostatus(int status)
{
	if (status == 0)
		return Ufo::FLYING;
	if (status == 1)
		return Ufo::LANDED;
	if (status == 2)
		return Ufo::CRASHED;
	if (status == 4)
		return Ufo::DESTROYED;
	if (status == 4)
		return Ufo::IGNORE_ME;
	return Ufo::IGNORE_ME;
}

int connectionTCP::ItemDamageRandomTypeToInt(ItemDamageRandomType type)
{

	if (type == DRT_DEFAULT)
		return 0;
	if (type == DRT_UFO)
		return 1;
	if (type == DRT_TFTD)
		return 2;
	if (type == DRT_FLAT)
		return 3;
	if (type == DRT_FIRE)
		return 4;
	if (type == DRT_NONE)
		return 5;
	if (type == DRT_UFO_WITH_TWO_DICE)
		return 6;
	if (type == DRT_EASY)
		return 7;
	if (type == DRT_STANDARD)
		return 8;
	if (type == DRT_EXPLOSION)
		return 9;

	return 0;
}

ItemDamageRandomType connectionTCP::intToItemDamageRandomType(int type)
{
	if (type == 0)
		return DRT_DEFAULT;
	if (type == 1)
		return DRT_UFO;
	if (type == 2)
		return DRT_TFTD;
	if (type == 3)
		return DRT_FLAT;
	if (type == 4)
		return DRT_FIRE;
	if (type == 5)
		return DRT_NONE;
	if (type == 6)
		return DRT_UFO_WITH_TWO_DICE;
	if (type == 7)
		return DRT_EASY;
	if (type == 8)
		return DRT_STANDARD;
	if (type == 9)
		return DRT_EXPLOSION;

	return DRT_DEFAULT;
}

int connectionTCP::ItemDamageTypeToInt(ItemDamageType type)
{

	if (type == DT_NONE)
		return 0;
	if (type == DT_AP)
		return 1;
	if (type == DT_IN)
		return 2;
	if (type == DT_HE)
		return 3;
	if (type == DT_LASER)
		return 4;
	if (type == DT_PLASMA)
		return 5;
	if (type == DT_STUN)
		return 6;
	if (type == DT_MELEE)
		return 7;
	if (type == DT_ACID)
		return 8;
	if (type == DT_SMOKE)
		return 9;
	if (type == DT_10)
		return 10;
	if (type == DT_11)
		return 11;
	if (type == DT_12)
		return 12;
	if (type == DT_13)
		return 13;
	if (type == DT_14)
		return 14;
	if (type == DT_15)
		return 15;
	if (type == DT_16)
		return 16;
	if (type == DT_17)
		return 17;
	if (type == DT_18)
		return 18;
	if (type == DT_19)
		return 19;
	if (type == DAMAGE_TYPES)
		return 20;

	return 0;
}

ItemDamageType connectionTCP::intToItemDamageType(int type)
{
	if (type == 0)
		return DT_NONE;
	if (type == 1)
		return DT_AP;
	if (type == 2)
		return DT_IN;
	if (type == 3)
		return DT_HE;
	if (type == 4)
		return DT_LASER;
	if (type == 5)
		return DT_PLASMA;
	if (type == 6)
		return DT_STUN;
	if (type == 7)
		return DT_MELEE;
	if (type == 8)
		return DT_ACID;
	if (type == 9)
		return DT_SMOKE;
	if (type == 10)
		return DT_10;
	if (type == 11)
		return DT_11;
	if (type == 12)
		return DT_12;
	if (type == 13)
		return DT_13;
	if (type == 14)
		return DT_14;
	if (type == 15)
		return DT_15;
	if (type == 16)
		return DT_16;
	if (type == 17)
		return DT_17;
	if (type == 18)
		return DT_18;
	if (type == 19)
		return DT_19;
	if (type == 20)
		return DAMAGE_TYPES;

	return DT_NONE;
}

int connectionTCP::InventoryTypeToInt(InventoryType type)
{
	if (type == INV_SLOT)
		return 0;
	if (type == INV_HAND)
		return 1;
	if (type == INV_GROUND)
		return 2;
	return 2;
}

InventoryType connectionTCP::intToInventoryType(int type)
{
	if (type == 0)
		return INV_SLOT;
	if (type == 1)
		return INV_HAND;
	if (type == 2)
		return INV_GROUND;
	return INV_GROUND;
}

int connectionTCP::SoldierRanktoInt(SoldierRank rank)
{
	if (rank == RANK_ROOKIE)
		return 0;
	if (rank == RANK_SQUADDIE)
		return 1;
	if (rank == RANK_SERGEANT)
		return 2;
	if (rank == RANK_CAPTAIN)
		return 3;
	if (rank == RANK_COLONEL)
		return 4;
	if (rank == RANK_COMMANDER)
		return 5;
	return 0;
}

SoldierRank connectionTCP::intToSoldierRank(int rank)
{
	if (rank == 0)
		return RANK_ROOKIE;
	if (rank == 1)
		return RANK_SQUADDIE;
	if (rank == 2)
		return RANK_SERGEANT;
	if (rank == 3)
		return RANK_CAPTAIN;
	if (rank == 4)
		return RANK_COLONEL;
	if (rank == 5)
		return RANK_COMMANDER;
	return RANK_ROOKIE;
}

Json::Value connectionTCP::toJson(const std::map<int, int>& m)
{
	Json::Value j(Json::objectValue);
	for (auto [k, v] : m)
		j[std::to_string(k)] = v;
	return j;
}

std::map<int, int> connectionTCP::fromJson(const Json::Value& j)
{
	std::map<int, int> m;
	for (const auto& key : j.getMemberNames())
		m[std::stoi(key)] = j[key].asInt();
	return m;
}

void connectionTCP::generateCraftSoldiers()
{

	Base* base = _game->getSavedGame()->getBases()->front();
	Craft* craft = base->getCrafts()->front();
	size_t craftID = 0;

	for (auto* soldier : *base->getSoldiers())
	{
		if (soldier->getCraft() == craft)
		{
			soldier->setCraftAndMoveEquipment(0, base, _game->getSavedGame()->getMonthsPassed() == -1);
		}
	}

	_game->pushState(new CraftSoldiersState(base, craftID));
}

bool connectionTCP::getServerOwner()
{
	return session.role == CoopRole::Host;
}

void connectionTCP::setPathLock(int lock)
{
	_pathLock = lock;
}

// assign the client soldiers to the host's craft
void connectionTCP::setClientSoldiers()
{
	// SHARED has ONE shared world: the roster/craft are already shared, so there is no
	// "assign the client's soldiers to the host's craft" merge to do (PRD-J09 skips the
	// SEPARATE two-world dance), and there is no "battleclient" blob to load. Running it
	// anyway re-entered ConfirmLandingState::startCoopMission below, which calls
	// bgen.run() a SECOND time and generates a brand-new RANDOM map on the host - while
	// the client had already loaded the first one. Result: host and client standing on
	// two entirely different maps (soldiers on open ground, no craft). The host already
	// generated and shipped the authoritative battle in btnYesClick; never regenerate.
	if (isSharedCampaign())
	{
		return;
	}

	// STARTING COOP MISSION
	CoopState* coop = new CoopState(111);

	coop->loadWorld();

	// coop campaign base defense
	if (_geo && getCoopCampaign() == true)
	{
		_geo->startCoopMission();
	}
	// coop campaign
	else if (_landing && getCoopCampaign() == true)
	{
		_landing->startCoopMission();
	}
	// coop battle (pve)
	else if (_battleState)
	{
		_battleState->startCoopMission();
	}
}

void connectionTCP::deleteAllCoopBases()
{

	// issue #78: the authoritative SHARED site set dies with the session - a
	// stale one must never prune the next session's world.
	{
		std::lock_guard<std::mutex> lk(sharedLiveSiteIdsMutex);
		sharedLiveSiteIds.clear();
		sharedLiveSiteIdsValid = false;
	}

	if (_game->getSavedGame() && _game->getCoopMod()->getCoopCampaign() == true)
	{

		if (auto* sg = _game->getSavedGame())
		{
			auto& bases = *sg->getBases(); // std::vector<Base*>&

			for (auto it = bases.begin(); it != bases.end();)
			{
				Base* b = *it;
				if (b && b->_coopIcon)
				{
					delete b;             // free memory permanently
					it = bases.erase(it); // remove from the list; returns next iterator
				}
				else
				{
					++it;
				}
			}

			// issue #78 audit: a dropped session also strands the SEPARATE-mode
			// _coop mirror objects; without this they linger as immortal ghosts
			// (pinned secondsRemaining, no sweep) until the player quits to the
			// menu. Order matters: UFOs before their alien missions.
			auto& sites = *sg->getMissionSites();
			for (auto it = sites.begin(); it != sites.end();)
			{
				if (*it && (*it)->_coop) { delete *it; it = sites.erase(it); }
				else ++it;
			}
			auto& ufos = *sg->getUfos();
			for (auto it = ufos.begin(); it != ufos.end();)
			{
				if (*it && (*it)->_coop) { delete *it; it = ufos.erase(it); }
				else ++it;
			}
			auto& abases = *sg->getAlienBases();
			for (auto it = abases.begin(); it != abases.end();)
			{
				if (*it && (*it)->_coop) { delete *it; it = abases.erase(it); }
				else ++it;
			}
			auto& amissions = sg->getAlienMissions();
			for (auto it = amissions.begin(); it != amissions.end();)
			{
				if (*it && (*it)->_coop) { delete *it; it = amissions.erase(it); }
				else ++it;
			}
		}

	}


}

void connectionTCP::updateAllCoopBases()
{

	Json::Value m_markers;
	Json::Reader reader;

	// if markers exist
	if (j_markers == "")
	{
		return;
	}

	reader.parse(j_markers, m_markers);

	for (Json::Value marker : m_markers)
	{

		std::string s_lon = marker["lon"].asString();
		std::string s_lan = marker["lan"].asString();

		int coopbaseid = marker["coopbaseid"].asInt();

		int getAvailableEngineers = marker["getAvailableEngineers"].asInt();
		int getAvailableHangars = marker["getAvailableHangars"].asInt();
		int getAvailableLaboratories = marker["getAvailableLaboratories"].asInt();
		int getAvailableQuarters = marker["getAvailableQuarters"].asInt();
		int getAvailableScientists = marker["getAvailableScientists"].asInt();
		int getAvailableSoldiers = marker["getAvailableSoldiers"].asInt();
		int getAvailableStores = marker["getAvailableStores"].asInt();
		int getAvailableTraining = marker["getAvailableTraining"].asInt();
		int getAvailableWorkshops = marker["getAvailableWorkshops"].asInt();

		double lon = std::stod(s_lon);
		double lan = std::stod(s_lan);

		std::string base_name = marker["base"].asString();

		// Check that the base does not already exist
		bool alreadyExists = false;
		for (Base* existingBase : *_game->getSavedGame()->getBases())
		{
			if (existingBase->_coop_base_id == coopbaseid &&
				(existingBase->getLongitude() == lon && existingBase->getLatitude() == lan))
			{
				alreadyExists = true;
				break;
			}
		}

		if (alreadyExists)
			continue;

		Base* CoopBase = new Base(_game->getMod());

		CoopBase->setEngineers(getAvailableEngineers);
		CoopBase->coop_hangar = getAvailableHangars;
		CoopBase->coop_laboratory = getAvailableLaboratories;
		CoopBase->coop_quarters = getAvailableQuarters;
		CoopBase->coop_soldiers = getAvailableSoldiers;
		CoopBase->coop_stores = getAvailableStores;
		CoopBase->coop_training = getAvailableTraining;
		CoopBase->coop_workshop = getAvailableWorkshops;
		CoopBase->setScientists(getAvailableScientists);

		CoopBase->_coop_base_id = coopbaseid;

		CoopBase->setName(base_name);

		CoopBase->isCoopBase(true);
		CoopBase->_coopIcon = true;

		CoopBase->setLongitude(lon);
		CoopBase->setLatitude(lan);

		_game->getSavedGame()->getBases()->push_back(CoopBase);
	}

}

void connectionTCP::fixCoopSave()
{
	if (_game->getSavedGame() && !_game->getSavedGame()->getSavedBattle() && getCoopCampaign() == true)
	{

		int newID = -1;
		int alienId = 2000000;
		int lastID = 0;

		for (auto& base : *_game->getSavedGame()->getBases())
		{
			auto* soldiers = base->getSoldiers();

			for (auto it = soldiers->begin(); it != soldiers->end();)
			{
				auto* soldier = *it;

				// Check that if the game mode is not PvE2, the soldiers are not aliens.
				if (getCoopGamemode() != 4 && newID == -1)
				{

					// If the soldier ID is greater than 2000000
					if (soldier->getId() >= alienId)
					{
						newID = lastID + 1;
					}
					else
					{
						lastID = soldier->getId();
					}

				}

				if (newID != -1)
				{
					soldier->setId(newID++);
				}

				// For all soldiers where coopbase is not -1, make sure their craft is null.
				if (soldier->getCoopBase() != -1)
				{
					soldier->setCraft(nullptr);
				}

				// If the base coop ID matches the soldier coopbase in the save, delete the soldier.
				if (base->_coop_base_id == soldier->getCoopBase())
				{
					delete soldier;
					it = soldiers->erase(it); // Remove the pointer from the container and continue safely.
					continue;
				}

				++it;
			}
		}
	}
}

bool valid_port(const std::string& s)
{
	if (s.empty())
		return false;

	if (!std::all_of(s.begin(), s.end(), [](unsigned char c)
					 { return std::isdigit(c); }))
	{
		return false;
	}

	int port = std::stoi(s);
	return port >= 0 && port <= 65535;
}

void connectionTCP::hostTCPServer(std::string servername, std::string str_port)
{

	sendTcpServerName = servername;
	gamePaused = 0;
	_waitBC = false;
	_waitBH = false;
	_battleWindow = false;
	_battleInit = false;
	coopInventory = false;
	coopMissionEnd = false;
	inventory_battle_window = true;

	int port = -1;

	if (valid_port(str_port))
	{
		port = std::stoi(str_port);
	}

	if (port == -1)
	{
		tcp_port = 3000;
	}
	else
	{
		tcp_port = port;
	}

	if (_hostThread.joinable())
	{
		_hostStop = true;
		_hostThread.join();
	}

	session.beginHosting();

	_hostStop = false;
	 _hostThread = std::thread(&connectionTCP::startTCPHost, this);

}

void connectionTCP::connectTCPServer(std::string ipaddress, std::string str_port)
{
	ipAddress = ipaddress;
	gamePaused = 0;
	_waitBC = false;
	_waitBH = false;
	_battleWindow = false;
	_battleInit = false;
	coopInventory = false;
	coopMissionEnd = false;
	inventory_battle_window = true;

	int port = -1;

	if (valid_port(str_port))
	{
		port = std::stoi(str_port);
	}

	if (port == -1)
	{
		tcp_port = 3000;
	}
	else
	{
		tcp_port = port;
	}

	if (_clientThread.joinable())
	{
		_clientStop = true;
		_clientThread.join();
	}

	session.beginJoining();

	_clientStop = false;

	_clientThread = std::thread(&connectionTCP::startTCPClient, this);

}

// coop
void connectionTCP::setConfirmLandingState(ConfirmLandingState* landing)
{
	_landing = landing;
}

// coop
void connectionTCP::setNewBattleState(NewBattleState* battlesate)
{
	_battleState = battlesate;
}

void connectionTCP::setGeoscapeState(GeoscapeState* base_geo)
{
	_geo = base_geo;
}

NewBattleState* connectionTCP::getNewBattleState()
{
	return _battleState;
}

bool connectionTCP::getLanding()
{

	if (_landing != nullptr)
	{
		return true;
	}

	return false;
}

void connectionTCP::setSelectedCraft(Craft* selectedCraft)
{
	_selectedCraft = selectedCraft;
}

Craft* connectionTCP::getSelectedCraft()
{
	return _selectedCraft;
}

void connectionTCP::sendTCPPacketData(std::string data)
{
	if (data.empty())
		return;
	if (!g_txQ.push(std::move(data)))
	{
		DebugLog("TX queue full, dropping packet\n");
		++g_txDropCount;
	}
}

void connectionTCP::sendCoopSnapshot(int slot, std::string data)
{
	if (data.empty())
		return;
	// Full-state last-write-wins snapshot -> conflation slot, not the FIFO. The
	// send drain emits the freshest value at link rate; stale copies are elided,
	// so the geoscape flood can never overflow g_txQ.
	enqueueSnapshot(static_cast<CoopSnapSlot>(slot), std::move(data));
}

bool connectionTCP::geoMembershipChanged(const Json::Value& root)
{
	// Compare the set of UFO/mission coop ids in this snapshot to the last one.
	// A change means something spawned or despawned -> the caller must deliver
	// this snapshot reliably (the conflation slot may drop it otherwise).
	std::set<int> ufoIds, missionIds;
	if (root.isMember("ufos"))
	{
		for (const auto& u : root["ufos"])
			ufoIds.insert(u["ufo_id"].asInt());
	}
	if (root.isMember("missions"))
	{
		for (const auto& m : root["missions"])
			missionIds.insert(m["mission_id"].asInt());
	}
	bool changed = (ufoIds != _lastGeoUfoIds) || (missionIds != _lastGeoMissionIds);
	_lastGeoUfoIds.swap(ufoIds);
	_lastGeoMissionIds.swap(missionIds);
	return changed;
}

void connectionTCP::setPlayerTurn(int turn)
{
	_playerTurn = turn;
}

void connectionTCP::sendFile()
{
	sendFileClient = true;
}

int connectionTCP::isConnected()
{
	return onConnect;
}

void connectionTCP::setConnected(int state)
{
	onConnect = state;
}

// disconnect the connection
void connectionTCP::disconnectTCP(bool isMain)
{

		_waitBC = false;
		_waitBH = false;
		coopSession = false;
		// coop: clear the cached teammate geoscape speed/focus so a stale '+' marker
		// doesn't linger after disconnect.
		peerTimeSpeedId = "";
		peerFocusScreen = -1;
		connectionTCP::lobby_timer = -1;
		connectionTCP::isPlayerReady = false;
		connectionTCP::isPlayersReady = false;

		connectionTCP::LobbyFileStatus = -1;
		connectionTCP::_coopGamemode = 0;
		connectionTCP::show_inactive_player_inventory = false;

	    OpenXcom::disconnectRendezvousUdp();

		// Clear all shared TCP/UDP packet queues after the transport is stopped.
		// This prevents stale packets from the previous session from affecting
		// a newly hosted or joined session.
		OpenXcom::clearNetworkSessionQueues();

		deleteAllCoopBases();

		// Capture the machine role ONCE for this teardown - handlers used to
		// mutate server_owner mid-flight and make the cleanup misclassify the
		// machine (the disconnect->cancel bug family).
		const bool teardownAsHost = (session.role == CoopRole::Host);

		// both
		if ((connectionTCP::no_bases == true || !teardownAsHost) && !isMain && connectionTCP::_coopCampaign == true)
		{
			_game->setState(new MainMenuState);
		}

		// host
		if (teardownAsHost && onConnect == -2)
		{

			onConnect = 1;

			if (connectionTCP::session.lobbyMode != 0 && connectionTCP::session.lobbyClosed == true)
			{
				// mid-session client drop: freeze until they reconnect (D5).
				// The dialog sits over the geoscape/battlescape, pausing it.
				// Don't stack a second dialog when a campaign wait dialog that
				// already covers "wait for the player to come back" is present
				// ANYWHERE in the stack, not just on top. A resume-ack wait (62)
				// already shows RESUME once resumeAck arrives, so it covers the
				// freeze dialog's job - stacking a 64 over a buried 62 produces
				// two RESUME dialogs and a double campaign_begun broadcast (C9).
				bool waitDialogPresent = false;
				for (State* st : _game->getStates())
				{
					CoopState* cs = dynamic_cast<CoopState*>(st);
					if (cs && (cs->getStateCode() == COOP_DLG_FREEZE
							|| cs->getStateCode() == COOP_DLG_RESUME_ACK_WAIT))
					{
						waitDialogPresent = true;
						break;
					}
				}
				if (!waitDialogPresent)
				{
					connectionTCP::session.freeze();
					_game->pushState(new CoopState(COOP_DLG_FREEZE));
				}
				else
				{
					Log(LOG_INFO) << "[coop] freeze dialog suppressed: a campaign "
						"wait dialog (freeze/resume-ack) is already on the stack";
				}
			}
			else if (connectionTCP::session.lobbyClosed == true)
			{
				_game->pushState(new LobbyMenu);
			}

		}
		// client
		else
		{

			onConnect = -1;

			// The client's world came from the host and is re-streamed on the
			// next join; drop the session's blobs.
			{
				std::lock_guard<std::mutex> lock(coopFilesMutex);
				connectionTCP::coopFilesClient.clear();
			}

			if (_chatMenu)
			{

				_chatMenu->setActive(false);
				_chatMenu->clearMessages();

				delete getChatMenu();
				setChatMenu(nullptr);
			}

		}

		// Session-state resets: exactly two paths (see CoopSession). The host
		// keeps its campaign/lobby context across a peer drop (D5); a client's
		// session is over. Never clear individual fields here.
		if (teardownAsHost)
		{
			connectionTCP::session.onClientDrop();
		}
		else
		{
			connectionTCP::session.resetSession();
		}

		connectionTCP::no_bases = false;
		connectionTCP::isCoopBaseLoading = false;

		gamePaused = 0;
		playerInsideCoopBase = false;

		_coop_task_completed = true;

		_isActiveAISync = false;

		_isActivePlayerSync = false;

		_clientPanicHandle = false;

		sendFileClient = false;
		sendFileBase = false;
		sendFileHost = false;
		sendFileSave = false;
		onceTime = false;

		isWaitMap = true;
		_hasHitUnit = -1;

		_game->getCoopMod()->pve2_init = false;

		setPlayerTurn(2);

		_battleWindow = false;
		_battleInit = false;

		coopInventory = false;

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{
				if (_game->getSavedGame()->getSavedBattle()->getBattleState())
				{
					_game->getSavedGame()->getSavedBattle()->getBattleState()->setCurrentTurn(2);
				}
			}
		}
}

std::string connectionTCP::getCurrentClientName()
{
	return tcpPlayerName;
}

std::string connectionTCP::getCurrentClientServer()
{
	return tcpServerName;
}

void connectionTCP::setCurrentClientServer(std::string servername)
{
	tcpServerName = servername;
}

std::string connectionTCP::getHostName()
{
	return sendTcpPlayer;
}

std::string connectionTCP::getHostServer()
{
	return sendTcpServerName;
}

void connectionTCP::setHostName(std::string playername)
{
	sendTcpPlayer = playername;
}

void connectionTCP::setHostServer(std::string servername)
{
	sendTcpServerName = servername;
}

void connectionTCP::writeHostMapFile()
{

	if (mapData.empty())
		return;

	if (connectionTCP::getServerOwner() == true)
	{

		std::string filename = "battleclient";

		{
			std::lock_guard<std::mutex> lock(coopFilesMutex);
			connectionTCP::coopFilesHost[filename] = std::move(mapData);
		}

		// RECEIVE CLIENT DATA
		SavedGame* client_save = new SavedGame();

		client_save->loadCoopSaveFromMemory(filename, _game->getMod(), _game->getLanguage(), filename);

		if (client_save && _game->getCoopMod()->getCoopCampaign() == true && _game->getCoopMod()->getServerOwner() == true)
		{

			std::string filename = hostBlobKey(_game->getCoopMod()->getCurrentClientName());

			// served copy lives in memory only; the host .sav embed persists it
			client_save->saveCoopToMemory(filename, _game->getMod(), filename);
			{
				std::lock_guard<std::mutex> lock(coopFilesMutex);
				eraseStaleBlobEntries(coopFilesHost, "host_", _game->getCoopMod()->getCurrentClientName(), filename);
			}

		}

		delete client_save;

	}
	else
	{
		std::lock_guard<std::mutex> lock(coopFilesMutex);
		connectionTCP::coopFilesClient["battleclient"] = std::move(mapData);
	}

	// the map data must be reset for the next use (fix)
	mapData = "";
}

bool connectionTCP::writeHostMapSaveProgressFile()
{

	std::string filename = hostBlobKey(_game->getCoopMod()->getCurrentClientName());

	if (mapData.empty())
		return false;

	// PRD-07 C10: validate the incoming blob BEFORE it touches the store. The
	// previous order installed (and pruned siblings) first, so a blob that
	// failed validation had already displaced the last-good one. Here we parse
	// a throwaway COPY under a scratch key, leaving mapData and the real entry
	// untouched; only a blob that passes every check is installed.
	static const std::string scratchKey = "__validate_save_progress__";
	{
		std::lock_guard<std::mutex> lock(coopFilesMutex);
		connectionTCP::coopFilesHost[scratchKey] = mapData; // copy, not move
	}

	SavedGame* coopFile = new SavedGame();
	coopFile->loadCoopSaveFromMemory(scratchKey, _game->getMod(), _game->getLanguage(), scratchKey);

	bool error = false;
	bool found = false;

	if (coopFile)
	{

		for (auto& base : *coopFile->getBases())
		{

			if (base->_coopBase == false)
			{
				found = true;
			}

			if (base->getName().empty() || (base->getLongitude() == 0 && base->getLatitude() == 0))
			{
				error = true;
				break;
			}

		}

	}
	else
	{
		error = true;
	}

	bool stored = (error == false && coopFile && found == true);

	std::string failReason;
	if (!stored)
	{
		failReason = (coopFile == nullptr) ? "parse failed"
			: error ? "base with empty name or null coords"
			: "no non-coop (own) base present";
	}

	delete coopFile;

	// drop the scratch entry either way - it never becomes the served blob.
	{
		std::lock_guard<std::mutex> lock(coopFilesMutex);
		connectionTCP::coopFilesHost.erase(scratchKey);
	}

	if (!stored)
	{
		// Failure path: leave the store EXACTLY as it was (the last-good blob
		// stays served + embeddable) and surface the error popup.
		Log(LOG_WARNING) << "[coop] rejected client progress blob from '"
						  << _game->getCoopMod()->getCurrentClientName() << "': " << failReason
						  << "; keeping last-good blob";
		_game->pushState(new CoopState(994));

		// the map data must be reset for the next use (fix)
		mapData = "";
		return false;
	}

	// Success: install the validated blob + prune stale siblings. The served
	// copy lives in memory only; persistence is the blob embedded in the host .sav.
	{
		std::lock_guard<std::mutex> lock(coopFilesMutex);
		connectionTCP::coopFilesHost[filename] = std::move(mapData);
		eraseStaleBlobEntries(coopFilesHost, "host_", _game->getCoopMod()->getCurrentClientName(), filename);
	}

	// the map data must be reset for the next use (fix)
	mapData = "";

	return true;
}

void connectionTCP::writeHostMapLoadProgressFile()
{

	std::string filename = clientBlobKey(_game->getCoopMod()->getHostName());

	if (mapData.empty())
		return;

	{
		std::lock_guard<std::mutex> lock(coopFilesMutex);
		connectionTCP::coopFilesClient[filename] = std::move(mapData);
		eraseStaleBlobEntries(coopFilesClient, "client_", _game->getCoopMod()->getHostName(), filename);
	}

	// the map data must be reset for the next use (fix)
	mapData = "";

}

}



