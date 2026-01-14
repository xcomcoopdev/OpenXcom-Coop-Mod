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

#include "../Engine/Game.h"

#include "../Basescape/CraftSoldiersState.h"
#include "../Mod/AlienDeployment.h"
#include "../Menu/CutsceneState.h"

#include "../Savegame/AlienMission.h"
#include "../Mod/UfoTrajectory.h"
#include "../Savegame/Ufo.h"
#include "../Battlescape/DebriefingState.h"

#include "../Savegame/Country.h"
#include "../Mod/RuleCountry.h"
#include "../Mod/RuleRegion.h"
#include "../Savegame/Region.h"

#include "../Mod/RuleCraftWeapon.h"
#include "../Savegame/CraftWeapon.h"

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

// are we receiving map data? If not, wait.
int gettingData = 0;

// ip address
std::string ipAddress = "";

// port (default: 3000)
int tcp_port = 3000;

// is it the host?
bool onTcpHost = false;

// is the server owner the one who creates the server?
bool server_owner = false;

// the local player's name
std::string sendTcpPlayer = "Player";

// the recipient player's name
std::string tcpPlayerName = "Player";

int onConnect = -1; // -1 = connect lost, 0 = client cant connect, -2 = disconnect, 1 = connected, -3 = server error, 2 = waiting for player

// trigger the event once
bool onceTime = false;

// base markers
std::string j_markers;

// has the map data arrived?
bool isWaitMap = true;

// trading
Json::Value waitedTrades;

int connectionTCP::_coopGamemode = 0; 

bool connectionTCP::_isChatActiveStatic = false;

bool connectionTCP::_isActiveAISync = false;

bool connectionTCP::_isActivePlayerSync = false; 

bool connectionTCP::_enable_time_sync = true;

bool connectionTCP::_enable_reaction_shoot = true;

bool connectionTCP::_enable_other_player_footsteps = true;

bool connectionTCP::_enable_host_only_time_speed = false;

bool connectionTCP::_enable_xcom_equipment_aliens_pvp = true;

bool connectionTCP::_reset_timeunits_onturnchange_pvp = true;

bool connectionTCP::_coopCampaign = false;

bool connectionTCP::_battleInit = false;

bool connectionTCP::playerInsideCoopBase = false;

bool connectionTCP::coopInventory = false;

std::string current_ping = "";

connectionTCP::connectionTCP(Game* game) : _game(game)
{

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
	return g_txQ.push(std::move(s));
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
	if (data.empty())
		return;
	if (!g_txQ.push(std::move(data)))
	{
		DebugLog("TX queue full, dropping packet\n");
	}
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

// in the loop, load the map file data between host and client
void connectionTCP::loopData()
{
	while (!_stop)
	{
		try
		{
			if (sendFileClient)
			{

				int fileindex = 0;
				std::string side = server_owner ? "host" : "client";
				std::string filename = sendFileSave ? side + "/battlehost.data" : (sendFileBase ? side + "/basehost.data" : side + "/battlehost.data");

				std::string filepath = Options::getMasterUserFolder() + filename;

				std::ifstream myfile(filepath);
				if (!myfile.is_open())
					throw std::runtime_error("Failed to open file: " + filepath);

				std::string line;
				std::string result;

				while (getline(myfile, line))
				{
					while (!isWaitMap)
						SDL_Delay(20);

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
							while (!isWaitMap)
								SDL_Delay(20);

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

				while (!isWaitMap)
					SDL_Delay(20);

				std::string jsonData = sendFileBase
										   ? "{\"state\" : \"MAP_RESULT_CLIENT_BASE\"}"
										   : "{\"state\" : \"MAP_RESULT_CLIENT\"}";

				sendTCPPacketStaticData(jsonData);
				sendFileBase = false;
				sendFileClient = false;
				sendFileSave = false;
			}
			else if (sendFileHost)
			{

				int fileindex = 0;
				std::string side = server_owner ? "host" : "client";
				std::string filename = sendFileSave ? side + "/battlehost.data" : (sendFileBase ? side + "/basehost.data" : (connectionTCP::_coopCampaign ? side + "/basehost.data" : side + "/battlehost.data"));

				std::string filepath = Options::getMasterUserFolder() + filename;

				std::ifstream myfile(filepath);
				if (!myfile.is_open())
					throw std::runtime_error("Failed to open file: " + filepath);

				std::string line;
				std::string result;

				while (getline(myfile, line))
				{
					while (!isWaitMap)
						SDL_Delay(20);

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
							while (!isWaitMap)
								SDL_Delay(20);

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

				while (!isWaitMap)
					SDL_Delay(20);

				std::string jsonData = sendFileBase
										   ? "{\"state\" : \"MAP_RESULT_HOST_BASE\"}"
										   : "{\"state\" : \"MAP_RESULT_HOST\"}";

				sendTCPPacketStaticData(jsonData);
				sendFileBase = false;
				sendFileHost = false;
				sendFileSave = false;
			}
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

void connectionTCP::createLoopdataThread()
{

	_loopThread = std::thread(&connectionTCP::loopData, this);

}

// an endless loop that processes the sync-packet data: tasks, remove targets, research, trading, disconnect, errors.
void connectionTCP::updateCoopTask()
{

	// time
	if (connectionTCP::getCoopStatic() == true && connectionTCP::getServerOwner() == false && connectionTCP::_enable_time_sync == true && _year != 0)
	{

		if (_game->getSavedGame())
		{

			GameTime new_time(_weekday, _day, _month, _year, _hour, _minute, _second);

			_game->getSavedGame()->setTime(new_time);

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

			std::string base_name = waitedTrades[i]["base"].asString();

			for (auto base : *_game->getSavedGame()->getBases())
			{
				if (base->getName() == base_name)
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

	// coop
	// server error!
	if (onConnect == -3)
	{

		// Make sure it calls disconnectTCP, otherwise it may get stuck.
		_game->pushState(new CoopState(440));

	}

	// disconnect from server!
	if (onConnect == -2)
	{

		if (allow_cutscene == true)
		{
			// Make sure it calls disconnectTCP, otherwise it may get stuck.
			if (server_owner == true)
			{
				_game->pushState(new CoopState(20));
			}
			else if (server_owner == false)
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
	static std::deque<std::string> rxHold; // local, game-thread only

	// Pull everything currently available from the lock-free queue into our local hold.
	{
		std::string msg;
		while (g_rxQ.pop(msg))
		{
			rxHold.emplace_back(std::move(msg));
		}
	}

	for (;;)
	{
		if (rxHold.empty())
			break;

		const size_t passCount = rxHold.size();
		size_t consumedThisPass = 0;

		for (size_t i = 0; i < passCount; ++i)
		{
			std::string jsonStr = std::move(rxHold.front());
			rxHold.pop_front();

			try
			{
				Json::CharReaderBuilder rb;
				Json::Value obj;
				std::string errs;
				std::istringstream ss(jsonStr);
				if (!Json::parseFromStream(rb, ss, &obj, &errs))
				{
					DebugLog(("JSON parse error: " + errs + "\n").c_str());
					continue; // drop malformed
				}

				const std::string stateString = obj.get("state", "defaultState").asString();
				const int fromId = obj.get("from", -1).asInt();

				// Make operator precedence explicit:
				const bool consumeNow =
						 (_coop_task_completed || (
						 (stateString == "abortPath" && _coopWalkInit) ||
						 (stateString == "unit_death" && _coopInitDeath) ||
						 (stateString == "after_unit_death" && _coopInitDeath)) ||
					 stateString == "close_event" || stateString == "click_close" || stateString == "AIProgress" || stateString == "update_progress" || stateString == "DebriefingState" || stateString == "endTurn" || stateString == "hit_tile" || stateString == "destroy_tile" || stateString == "set_fire_tile" || stateString == "set_smoke_tile" || stateString == "unit_fire" || stateString == "calc_explode_fov") &&
					!(stateString == "endPlayerTurn" && (_coopEnd == 1 || (_game->getSavedGame() && !_game->getSavedGame()->getSavedBattle())));

				if (consumeNow)
				{
					onTCPMessage(stateString, obj);
					++consumedThisPass;
				}
				else
				{
					// Rotate to the back so we can try the next message
					rxHold.emplace_back(std::move(jsonStr));
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

				// Put back to the *back* to avoid pinning the head
				rxHold.emplace_back(std::move(jsonStr));
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

		// if client cancels the action
		if (cancel_connect == false)
		{
			_game->pushState(new CoopState(16));
		}

		cancel_connect = false;
	}


}


void connectionTCP::syncCoopInventory()
{

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

		int slot_ammo = _jsonInventory[i]["slot_ammo"].asInt();

		int sel_item_id = _jsonInventory[i]["sel_item_id"].asInt();

		std::string item_name = _jsonInventory[i]["item_name"].asString();

		std::string sel_item_name = _jsonInventory[i]["sel_item_name"].asString();

		int tile_x = _jsonInventory[i]["tile_x"].asInt();
		int tile_y = _jsonInventory[i]["tile_y"].asInt();
		int tile_z = _jsonInventory[i]["tile_z"].asInt();

		if (_game->getSavedGame()->getSavedBattle()->getBattleState())
		{
			_game->getSavedGame()->getSavedBattle()->getBattleState()->moveCoopInventory(sel_item_name, item_name, inv_id, inv_x, inv_y, unit_id, item_id, move_cost, slot_x, slot_y, getHealQuantity, getPainKillerQuantity, getStimulantQuantity, getFuseTimer, getXCOMProperty, isAmmo, isWeaponWithAmmo, isFuseEnabled, getAmmoQuantity, slot_ammo, sel_item_id, tile_x, tile_y, tile_z);
		}

		resetCoopInventory = true;

		_jsonInventory[i] = {};
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
	sendFileBase = false;
	sendFileHost = false;
	sendFileSave = false;
	gettingData = 0;

	mapData.clear();

	onTcpHost = isHost;
	server_owner = isHost;
	onConnect = -1;
}

// SERVER SETUP


// ===== Constants =====
static constexpr uint32_t kMaxMsgLen = 4u * 1024u * 1024u; // Safety cap: 4 MB per message

// ===== Lock-free SPSC ring buffers (single-producer, single-consumer) =====
// Producer: game thread, Consumer: network thread (TX queue)
// Producer: network thread, Consumer: game thread (RX queue)
template <size_t N>
struct SPSCQueue
{
	std::array<std::string, N> buf{};
	std::atomic<size_t> head{0}; // producer writes
	std::atomic<size_t> tail{0}; // consumer reads

	bool push(std::string&& s)
	{
		size_t h = head.load(std::memory_order_relaxed);
		size_t n = (h + 1) % N;
		if (n == tail.load(std::memory_order_acquire))
			return false; // full
		buf[h] = std::move(s);
		head.store(n, std::memory_order_release);
		return true;
	}
	bool pop(std::string& out)
	{
		size_t t = tail.load(std::memory_order_relaxed);
		if (t == head.load(std::memory_order_acquire))
			return false; // empty
		out = std::move(buf[t]);
		tail.store((t + 1) % N, std::memory_order_release);
		return true;
	}
	bool empty() const
	{
		return tail.load(std::memory_order_acquire) == head.load(std::memory_order_acquire);
	}
};



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
			if (!out.empty())
			{
				if (!sendAll(sock, out.data(), (int)out.size()))
				{
					DebugLog("DISCONNECT CLIENT: SEND\n");
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
				uint32_t msgLen;
				std::memcpy(&msgLen, recvBuffer.data(), 4);
				msgLen = SDL_SwapBE32(msgLen);
				if (msgLen > kMaxMsgLen)
				{
					DebugLog("Client: message too large, disconnecting\n");
					onConnect = -3;
					onceTime = false;
					goto client_cleanup;
				}
				if (recvBuffer.size() < 4 + msgLen)
					break;

				std::string message(recvBuffer.begin() + 4, recvBuffer.begin() + 4 + msgLen);
				recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + 4 + msgLen);

				if (!message.empty())
				{
					// Handle PONG internally, push others to RX queue for the game thread
					Json::CharReaderBuilder rb;
					Json::Value obj;
					std::string errs;
					std::istringstream ss(message);
					if (Json::parseFromStream(rb, ss, &obj, &errs))
					{
						if (maybeHandlePingOnClient(obj))
							continue;
						if (maybeHandlePongOnClient(obj))
							continue;
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
			hello["state"] = "COOP_READY_CLIENT";
			hello["playername"] = sendTcpPlayer;
			sendJSONNoLock(hello);
		}

		// ---- Client RTT ping ----
		clientMaybeSendPing();

		// ---- Gentle yield only if nothing happened ----
		if (ready == 0 && g_txQ.empty())
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
	server_owner = true;

	for (;;)
	{
		if (onConnect == -1)
			break;

		if (_hostStop)
			break;

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
				uint32_t msgLen;
				std::memcpy(&msgLen, recvBuffer.data(), 4);
				msgLen = SDL_SwapBE32(msgLen);
				if (msgLen > kMaxMsgLen)
				{
					DebugLog("Host: message too large, drop client\n");
					onConnect = -3;
					SDLNet_TCP_DelSocket(socketSet, clientSock);
					SDLNet_TCP_Close(clientSock);
					clientSock = nullptr;
					recvBuffer.clear();
					break;
				}
				if (recvBuffer.size() < 4 + msgLen)
					break;

				std::string message(recvBuffer.begin() + 4, recvBuffer.begin() + 4 + msgLen);
				recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + 4 + msgLen);

				if (!message.empty())
				{
					Json::CharReaderBuilder rb;
					Json::Value obj;
					std::string errs;
					std::istringstream ss(message);
					if (Json::parseFromStream(rb, ss, &obj, &errs))
					{
						// 1) Jos tuli PING clientiltä → vastaa heti PONGilla (sinulla oli tämä jo)
						if (maybeHandlePingOnHost(obj))
							continue;

						// 2) Jos tuli PONG (vastauksena hostin omaan PINGiin) → laske ja loggaa
						if (maybeHandlePongOnHost(obj))
							continue;
					}
					if (!g_rxQ.push(std::move(message)))
					{
						DebugLog("RX queue full, dropping message\n");
					}
				}
			}
		}

		if (clientSock)
			hostMaybeSendPing();

		// ---- Gentle yield if nothing to do ----
		if (ready == 0 && OpenXcom::g_txQ.empty())
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

	server_owner = false;
	onConnect = -1;
	return;
}

// TCP
void connectionTCP::onTCPMessage(std::string stateString, Json::Value obj)
{

	coopSession = true;

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

		show_coop_ufo_popup_type = str_type;
		show_coop_ufo_popup_race = str_race;

	}

	if (stateString == "mission_popup")
	{

		int mission_id = obj["mission_id"].asInt();

		show_coop_mission_popup = mission_id;

	}

	// delete_base
	if (stateString == "delete_base")
	{

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
		int ending = obj["ending"].asInt();

		if (_game->getSavedGame())
		{

			_game->getSavedGame()->setEnding((GameEnding)ending);

			_game->getSavedGame()->setMonthsPassed(monthsPassed);
		}

		allow_cutscene = false;

		_game->pushState(new CutsceneState(cutsceneId));
	}

	// server full!
	if (stateString == "server_full")
	{

		onConnect = -1;
		_game->pushState(new CoopState(444));
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

		std::string filename = "";

		if (getServerOwner() == true)
		{

			filename = "host/basehost.data";
		}
		else
		{

			filename = "client/basehost.data";
		}

		SavedGame* file_units = new SavedGame();
		std::string filepath = Options::getMasterUserFolder() + filename;

		if (OpenXcom::CrossPlatform::fileExists(filepath))
		{

			bool save = false;

			file_units->load(filename, _game->getMod(), _game->getLanguage());

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

				file_units->save(filename, _game->getMod());
			}
		}
	}

	// TRADING AND EXPORTING GOODS
	if (stateString == "trade" || stateString == "transfer")
	{

		waitedTrades.append(obj);
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

			_weekday = weekday;
			_day = day;
			_month = month;
			_year = year;
			_hour = hour;
			_minute = minute;
			_second = second;
		}

		std::string time_speed = obj["time_speed"].asString();
		other_time_speed_coop = time_speed;
	
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

		int slot_ammo = obj["slot_ammo"].asInt();

		std::string item_name = obj["item_name"].asString();

		std::string sel_item_name = obj["sel_item_name"].asString();

		int sel_item_id = obj["sel_item_id"].asInt();

		int tile_x = obj["tile_x"].asInt();
		int tile_y = obj["tile_y"].asInt();
		int tile_z = obj["tile_z"].asInt();

		if (coopInventory == true && _game->getSavedGame()->getSavedBattle())
		{

			_game->getSavedGame()->getSavedBattle()->getBattleState()->moveCoopInventory(sel_item_name, item_name, inv_id, inv_x, inv_y, unit_id, item_id, move_cost, slot_x, slot_y, getHealQuantity, getPainKillerQuantity, getStimulantQuantity, getFuseTimer, getXCOMProperty, isAmmo, isWeaponWithAmmo, isFuseEnabled, getAmmoQuantity, slot_ammo, sel_item_id, tile_x, tile_y, tile_z);

			resetCoopInventory = true;
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

		if (playerInsideCoopBase == true)
		{

			_coopFacility.append(obj);

		}

	}

	if (stateString == "dismantle_facility")
	{

		if (playerInsideCoopBase == true)
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

	// hit unit
	if (stateString == "hit_unit")
	{

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

				int unit_id = obj["unit_id"].asInt();
				int health = obj["health"].asInt();

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					if (unit->getId() == unit_id)
					{

						unit->setHealth(health);
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

	// RANDOM SEED
	if (stateString == "current_seed")
	{

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

							int motionpoints = obj["motionpoints"].asInt();

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
							unit->setTimeUnits(time);
							unit->setHealth(health);
							unit->setCoopMorale(morale);
							unit->setCoopEnergy(energy);
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
	
					if (end == true)
					{
						i_ufo->_playerShotDownUfo = false;
						i_ufo->setSpeed(i_ufo->originalCoopSpeed);
						i_ufo->originalCoopSpeed = 0;
					}
					else
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

		_game->getCoopMod()->show_coop_monthly_report = true;

	}

	// target positions
	if (stateString == "target_positions")
	{

		if (_game->getSavedGame() && playerInsideCoopBase == false)
		{

			// bases
			for (int i = 0; i < obj["bases"].size(); i++)
			{

				int coopbase_id = obj["bases"][i]["coopbase_id"].asInt();
				int range_coop = obj["bases"][i]["range_coop"].asInt();
				int range_detection = obj["bases"][i]["range_detection"].asInt();

				for (auto &temp_base : *_game->getSavedGame()->getBases())
				{

					if (temp_base->_coopIcon == true && temp_base->_coop_base_id == coopbase_id)
					{

						temp_base->_range_coop = range_coop;
						temp_base->_range_detection = range_detection;

						break;
					}

				}

			}

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

									const RuleCraftWeapon* wRule = _game->getMod()->getCraftWeapon(type);
									if (!wRule)
										continue;

									weapons.push_back(new CraftWeapon(const_cast<RuleCraftWeapon*>(wRule), ammo));
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

						// weapons
						auto& weapons = *craft->getWeapons();
						const Json::Value& wj = obj["crafts"][i]["weapons"];

						const Json::ArrayIndex count =
							std::min<Json::ArrayIndex>((Json::ArrayIndex)weapons.size(), wj.size());

						for (Json::ArrayIndex w = 0; w < count; ++w)
						{
							weapons[w]->setAmmo(wj[w]["ammo"].asInt());
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

						alien_mission = new AlienMission(*alien_mission_rule);

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

				if (getCoopGamemode() == 2 && getHost() == false)
				{
					ufo->setDetectedCoop(true);
				}
				else if (getCoopGamemode() == 3 && getHost() == true)
				{
					ufo->setDetectedCoop(true);
				}
				else
				{
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

					}
					
					_AISecondMoveCoop = false;

					_game->getSavedGame()->getSavedBattle()->setSelectedUnit(0);

				}

			}

		}


	}

	if (stateString == "DebriefingState")
	{

		bool abort = obj["abort"].asBool();
		std::string title = obj["title"].asString();

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
								int motionpoints = obj["motionpoints"].asInt();

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
				// PVP2
				else if (getHost() == true && getCoopGamemode() == 3 && connectionTCP::_reset_timeunits_onturnchange_pvp == true)
				{

					for (auto &unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
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

					// Auto save before (only HOST)
					SavedGame* newsave = new SavedGame(*_game->getSavedGame());

					newsave->setName("coop_mission_2");

					newsave->save("coop_mission_2.sav", _game->getMod());

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

		gettingData = 0;
	}

	if (stateString == "MAP_RESULT_CLIENT" && onTcpHost == false)
	{

		DebugLog("MAP_RESULT_CLIENT");

		writeHostMapFile();
		loadHostMap();

		gettingData = 0;

		// if not save file
		if (inventory_battle_window == true)
		{

			std::string jsonData2 = "{\"state\" : \"setup_battle\"}";
			sendTCPPacketData(jsonData2);
		}
	}

	if (stateString == "setup_battle" && onTcpHost == true)
	{

		CoopState* coop = new CoopState(765);

		coop->loadWorld();
	}

	// LOAD MAP
	if (stateString == "map_result_data")
	{
		try
		{

			gettingData = 1;

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

	if (stateString == "COOP_READY_CLIENT" && onTcpHost == true)
	{

		fixCoopSave();

		// coop fix bases..
		j_markers = "";

		if (onceTime == true)
		{
			return;
		}

		onceTime = true;

		_battleInit = false;

		// Define the file path and values to write
		std::string filename = Options::getMasterUserFolder() + "/ip_address.json";

		// Create JSON object
		Json::Value root133;
		root133["ip"] = ipAddress;
		root133["name"] = sendTcpPlayer;

		// Write JSON to file
		std::ofstream file(filename);
		if (file.is_open())
		{
			Json::StreamWriterBuilder writer;
			file << Json::writeString(writer, root133);
			file.close();

			std::cout << "IP address and player name written to " << filename << std::endl;
		}
		else
		{
			std::cerr << "Failed to open file for writing." << std::endl;
		}

		// file (host)
		if (std::filesystem::create_directory(Options::getMasterUserFolder() + "/host"))
		{
			DebugLog("host folder created!");
		}
		else
		{
			DebugLog("host folder failed created!");
		}

		// file (client)
		if (std::filesystem::create_directory(Options::getMasterUserFolder() + "/client"))
		{
			DebugLog("client folder created!");
		}
		else
		{
			DebugLog("client folder failed created!");
		}

		// IF THE HOST IS IN BATTLE, INCLUDE JOINERS; OTHERWISE DO NOTHING
		// DISPLAY THE CLIENT PLAYER'S BASE
		std::string playername = obj.get("playername", "defaultState").asString();

		tcpPlayerName = playername;

		Json::Value root;
		root["state"] = "COOP_READY_HOST";
		root["playername"] = sendTcpPlayer;
		root["gamemode"] = connectionTCP::_coopGamemode;

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

		root["enable_reaction_shoot"] = connectionTCP::_enable_time_sync;

		// other player footsteps sounds
		connectionTCP::_enable_other_player_footsteps = Options::EnableOtherPlayerFootsteps;
		root["enable_other_player_footsteps"] = connectionTCP::_enable_other_player_footsteps;

		// enable host only time speed
		connectionTCP::_enable_host_only_time_speed = Options::EnableHostOnlyTimeSpeed;
		root["enable_host_only_time_speed"] = connectionTCP::_enable_host_only_time_speed;

		// enable XcomEquipmentAliensPVP
		connectionTCP::_enable_xcom_equipment_aliens_pvp = Options::EnableXcomEquipmentAliensPVP;
		root["enable_xcom_equipment_aliens_pvp"] = _enable_xcom_equipment_aliens_pvp;

		// resetTimeUnitsOnTurnChangePVP
		connectionTCP::_reset_timeunits_onturnchange_pvp = Options::resetTimeUnitsOnTurnChangePVP;
		root["reset_timeunits_onturnchange_pvp"] = _reset_timeunits_onturnchange_pvp;

		// campaing check
		root["coop_campaign"] = _coopCampaign;

		// mod check
		std::vector<std::string> mod_names = _game->getMod()->getCoopModList();

		int index = 0;

		for (auto mod_name : mod_names)
		{

			root["mods"][index]["name"] = mod_name;

			index++;
		}

		// mod count
		root["mods_count"] = Json::UInt(mod_names.size());

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

		// RESET ALL SOLDIERS OUT OF THE BASES (HAPPENS ONCE IN AN ERROR SITUATION)
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

	if (stateString == "COOP_READY_HOST" && onTcpHost == false)
	{

		fixCoopSave();

		// coop fix bases..
		j_markers = "";

		_battleInit = false;

		if (onceTime == true)
		{
			return;
		}

		onceTime = true;

		// set current gamemode
		connectionTCP::_coopGamemode = obj["gamemode"].asInt();

		// Define the file path and values to write
		std::string filename = Options::getMasterUserFolder() + "/ip_address.json";

		// Create JSON object
		Json::Value root135;
		root135["ip"] = ipAddress;
		root135["name"] = sendTcpPlayer;

		// Write JSON to file
		std::ofstream file(filename);
		if (file.is_open())
		{
			Json::StreamWriterBuilder writer;
			file << Json::writeString(writer, root135);
			file.close();

			std::cout << "IP address and player name written to " << filename << std::endl;
		}
		else
		{
			std::cerr << "Failed to open file for writing." << std::endl;
		}

		// file (host)
		if (std::filesystem::create_directory(Options::getMasterUserFolder() + "/host"))
		{
			DebugLog("host folder created!");
		}
		else
		{
			DebugLog("host folder failed created!");
		}

		// file (client)
		if (std::filesystem::create_directory(Options::getMasterUserFolder() + "/client"))
		{
			DebugLog("client folder created!");
		}
		else
		{
			DebugLog("client folder failed created!");
		}

		// campaign check
		bool host_coop_campaign = obj["coop_campaign"].asBool();
		bool client_coop_campaign = _coopCampaign;

		if (host_coop_campaign != client_coop_campaign)
		{

			// if campaign
			if (host_coop_campaign == true)
			{
				_game->pushState(new CoopState(2000));
			}
			// if new battle
			else
			{
				_game->pushState(new CoopState(3000));
			}

			return;
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

		// resetTimeUnitsOnTurnChangePVP
		bool reset_timeunits_onturnchange_pvp = obj["reset_timeunits_onturnchange_pvp"].asBool();
		connectionTCP::_reset_timeunits_onturnchange_pvp = reset_timeunits_onturnchange_pvp;


		for (Json::Value host_mod : obj["mods"])
		{

			mod_found = false;

			std::string host_mod_name = host_mod["name"].asString();

			for (std::string client_mod_name : client_mod_names)
			{

				//  if mod  found
				if (client_mod_name == host_mod_name)
				{

					mod_found = true;
				}
			}

			if (mod_found == false)
			{
				break;
			}
		}

		// If the mod is not found, handle the compatibility error.
		if (mod_found == false || (host_mod_count != client_mod_count))
		{

			_game->pushState(new CoopState(1000));

			return;
		}

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

		bool inBattle = obj["battle"].asBool();

		tcpPlayerName = playername;

		if (clientInBattle == false)
		{
			CoopState* coop = new CoopState(777);
			coop->loadWorld();
		}

		// DISPLAY THE CLIENT PLAYER'S BASE
		_game->popState();

		_game->pushState(new Profile(clientInBattle, 0, inBattle));

		// if neither the client nor the host is in battle, then create base icons

		// BASE
		Json::Value markers;

		markers["state"] = "coopBase";
		markers["battle"] = inBattle;

		int index = 0;
		for (auto base : *_game->getSavedGame()->getBases())
		{

			if (base->_coopBase == false && base->_coopIcon == false)
			{

				markers["markers"][index]["coopbaseid"] = base->_coop_base_id;

				markers["markers"][index]["base"] = base->getName().c_str();
				markers["markers"][index]["lon"] = base->getLongitude();
				markers["markers"][index]["lan"] = base->getLatitude();

				// new!!!
				markers["markers"][index]["range_coop"] = base->_range_coop;

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

		bool inBattle = obj["battle"].asBool();

		// show host profile
		_game->pushState(new Profile(false, 0, inBattle));

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

			// new!!!
			int range_coop = marker["range_coop"].asInt();
			
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
			CoopBase->_range_coop = range_coop;

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

		int index = 0;
		for (auto base : *_game->getSavedGame()->getBases())
		{

			if (base->_coopBase == false && base->_coopIcon == false)
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
				markers["markers"][index]["range_coop"] = base->_range_coop;

				index++;
			}
		}

		sendTCPPacketData(markers.toStyledString());
	}

	// new base icon
	if (stateString == "new_base")
	{

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

		// new!!!
		int range_coop = obj["markers"]["range_coop"].asInt();

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

		CoopBase->_range_coop = range_coop;

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

		Json::Value markers;

		int index = 0;
		for (auto base : *_game->getSavedGame()->getBases())
		{

			if (base->_coopBase == false && base->_coopIcon == false)
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
				markers["markers"][index]["range_coop"] = base->_range_coop;

				index++;
			}
		}

		if (getHost() == false)
		{

			markers["state"] = "coopBase3";

			sendTCPPacketData(markers.toStyledString());
		}
		else
		{

			markers["state"] = "coopBase2";

			sendTCPPacketData(markers.toStyledString());
		}
	}

	// COOP BASE CLIENT
	if (stateString == "coopBase2" && onTcpHost == false)
	{

		Json::Value m_markers;
		Json::Reader reader;

		// jos markkereita
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

			// new!!!
			int range_coop = marker["range_coop"].asInt();

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

			CoopBase->_range_coop = range_coop;

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

			// new!!!
			int range_coop = marker["range_coop"].asInt();

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

			CoopBase->_range_coop = range_coop;

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

				for (auto it = missions.begin(); it != missions.end();)
				{
					if (*it && (*it)->getLatitude() == lat && (*it)->getLongitude() == lon)
					{
						delete *it;
						it = missions.erase(it);
					}
					else
					{
						++it;
					}
				}

			}
			// ufo
			else
			{

				auto& ufos = *_game->getSavedGame()->getUfos();

				for (auto it = ufos.begin(); it != ufos.end();)
				{
					if (*it && (*it)->getLatitude() == lat && (*it)->getLongitude() == lon)
					{
						delete *it;
						it = ufos.erase(it);
					}
					else
					{
						++it;
					}
				}

			}

			sendBaseFile();

			// Save the geospace file so the player can return to it later
			if (_game->getCoopMod()->getCoopCampaign() == true)
			{
				_game->getSavedGame()->setName("coop_geoscape");
				_game->getSavedGame()->save("coop_geoscape.sav", _game->getMod());
			}


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

		_game->getCoopMod()->load_state = "Sending base data";  

		sendFileHost = true;
	}

	// BASES
	if (stateString == "SEND_FILE_HOST_BASE" && onTcpHost == false)
	{

		if (gettingData == 1)
		{
			gettingData = 2;
			return;
		}

		sendBaseFile();

		sendFileHost = true;
		sendFileBase = true;
	}

	if (stateString == "SEND_FILE_CLIENT_BASE" && onTcpHost == true)
	{

		if (gettingData == 1)
		{
			gettingData = 2;
			return;
		}

		sendBaseFile();

		sendFileClient = true;
		sendFileBase = true;
	}

	if (stateString == "MAP_RESULT_CLIENT_BASE" && onTcpHost == false)
	{

		writeHostMapFile2();

		CoopState* coopWindow = new CoopState(55);
		coopWindow->loadWorld();

		// send the file immediately if needed...
		if (gettingData == 2)
		{

			sendFileHost = true;
			sendFileBase = true;
		}

		gettingData = 0;
	}

	if (stateString == "MAP_RESULT_HOST_BASE" && onTcpHost == true)
	{

		writeHostMapFile2();

		CoopState* coopWindow = new CoopState(55);
		coopWindow->loadWorld();

		// lahetetaan samantien tiedosto jos tarvetta...
		if (gettingData == 2)
		{

			sendFileClient = true;
			sendFileBase = true;
		}

		gettingData = 0;
	}
}

void connectionTCP::sendBaseFile()
{

	if (_game->getCoopMod()->getHost() == false)
	{
		// saving is not allowed if in battle and inside another player's base!
		if (!_game->getSavedGame()->getSavedBattle() && _game->getCoopMod()->playerInsideCoopBase == false)
		{

			if (_game->getCoopMod()->getServerOwner() == true && _game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->save("host/basehost.data", _game->getMod());
			}
			else if (_game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->save("client/basehost.data", _game->getMod());
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
			}
			else if (_game->getCoopMod()->coopMissionEnd == false)
			{
				_game->getSavedGame()->save("client/basehost.data", _game->getMod());
			}
		}
	}

}

void connectionTCP::setPauseOn()
{

	// coop
	if (getCoopStatic() == true && _isActivePlayerSync == false && gamePaused == 0 && _battleWindow == false && _isActiveAISync == false && _battleInit == true)
	{

		Json::Value root;

		root["state"] = "GamePausedON";

		sendTCPPacketData(root.toStyledString().c_str());
	}
}

void connectionTCP::setPauseOff()
{

	// coop
	if (getCoopStatic() == true && _isActivePlayerSync == false && gamePaused == 0 && _battleWindow == false && _isActiveAISync == false && _battleInit == true)
	{

		Json::Value root;

		root["state"] = "GamePausedOFF";

		sendTCPPacketData(root.toStyledString().c_str());
	}
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

void connectionTCP::setCoopCampaign(bool coop)
{
	_coopCampaign = coop;
}

bool connectionTCP::getCoopCampaign()
{
	return _coopCampaign;
}

int connectionTCP::getCoopGamemode()
{
	return _coopGamemode;
}

void connectionTCP::createCoopMenu()
{

	CoopMenu* state = new CoopMenu();

	state->showGamemode();

	_game->pushState(state);
}

void connectionTCP::sendTCPPacketStaticData2(std::string data)
{
	if (data.empty())
		return;
	if (!enqueueTx(std::move(data)))
	{ // fastest path
		DebugLog("TX queue full, dropping packet\n");
	}
}

void connectionTCP::writeHostMapFile2()
{
	std::string filename = "";

	if (getServerOwner() == true)
	{
		filename = "host/baseclient.data";
	}
	else
	{
		filename = "client/baseclient.data";
	}

	std::string filepath = Options::getMasterUserFolder() + filename;
	std::ofstream file(filepath);
	std::string my_string = mapData;
	file << my_string;

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
				_game->getSavedGame()->save("host/battlehost.data", _game->getMod());

			}
			else if (_game->getCoopMod()->coopMissionEnd == false)
			{

				_game->getSavedGame()->save("client/battlehost.data", _game->getMod());

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
	return server_owner;
}

void connectionTCP::setPathLock(int lock)
{
	_pathLock = lock;
}

// assign the client soldiers to the host's craft
void connectionTCP::setClientSoldiers()
{

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
		for (auto& base : *_game->getSavedGame()->getBases())
		{
			auto* soldiers = base->getSoldiers();

			for (auto it = soldiers->begin(); it != soldiers->end();)
			{
				auto* soldier = *it;

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

void connectionTCP::hostTCPServer(std::string playername, std::string ipaddress)
{

	gamePaused = 0;
	_waitBC = false;
	_waitBH = false;
	_battleWindow = false;
	_battleInit = false;
	coopInventory = false;
	coopMissionEnd = false;
	inventory_battle_window = true;

	int port = getPortFromAddress(ipaddress);

	if (port == -1)
	{
		tcp_port = 3000;
	}
	else
	{
		tcp_port = port;
	}

	if (playername != "")
	{
		sendTcpPlayer = playername;
	}

	if (_hostThread.joinable())
	{
		_hostStop = true;
		_hostThread.join();
	}

	_hostStop = false;
	 _hostThread = std::thread(&connectionTCP::startTCPHost, this);

}

void connectionTCP::connectTCPServer(std::string playername, std::string ipaddress)
{
	ipAddress = ipaddress;
	sendTcpPlayer = playername;
	gamePaused = 0;
	_waitBC = false;
	_waitBH = false;
	_battleWindow = false;
	_battleInit = false;
	coopInventory = false;
	coopMissionEnd = false;
	inventory_battle_window = true;

	int port = getPortFromAddress(ipaddress);

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
	}
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
void connectionTCP::disconnectTCP()
{

		deleteAllCoopBases();

		// host
		if (server_owner == true && onConnect == -2)
		{

			coopSession = true;
			onConnect = 2;
			_waitBC = false;

		}
		// client
		else
		{

			onConnect = -1;
			coopSession = false;
			_waitBH = false;

			if (_chatMenu)
			{

				_chatMenu->setActive(false);
				_chatMenu->clearMessages();

				delete getChatMenu();
				setChatMenu(nullptr);
			}

		}

		// both
		connectionTCP::_coopGamemode = 0;
		gamePaused = 0;
		playerInsideCoopBase = false;

		_coop_task_completed = true;

		_isActiveAISync = false;

		_isActivePlayerSync = false;

		_clientPanicHandle = false;

		// new values
		gettingData = 0;
		sendFileClient = false;
		sendFileBase = false;
		sendFileHost = false;
		sendFileSave = false;
		onceTime = false;

		isWaitMap = true;

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

std::string connectionTCP::getHostName()
{
	return sendTcpPlayer;
}

void connectionTCP::writeHostMapFile()
{
	std::string filename = "";

	if (mapData.empty())
		return;

	if (getServerOwner() == true)
	{
		filename = "host/battleclient.data";
	}
	else
	{
		filename = "client/battleclient.data";
	}

	std::string filepath = Options::getMasterUserFolder() + filename;
	std::ofstream file(filepath);
	std::string my_string = mapData;
	file << my_string;

	// the map data must be reset for the next use (fix)
	mapData = "";
}




}



