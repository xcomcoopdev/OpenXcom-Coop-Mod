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

// research
Json::Value waitedResearch;

// missions
Json::Value pendingMissions;

// Clear Targets
Json::Value pendingRemoveTargets;

int connectionTCP::_coopGamemode = 0; 

bool connectionTCP::_isChatActiveStatic = false; 

bool connectionTCP::_coopCampaign = false;

std::string current_ping = "";

connectionTCP::connectionTCP(Game* game) : _game(game)
{

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
		OutputDebugStringA("TX queue full, dropping packet\n");
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
	OutputDebugStringA((msg + "\n").c_str());
}

// in the loop, load the map file data between host and client
DWORD __stdcall loopData(LPVOID param)
{
	while (true)
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
						Sleep(20);

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
								Sleep(20);
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
					Sleep(20);

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
						Sleep(20);

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
								Sleep(20);
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
					Sleep(20);

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
			logError("Error in loopData: " + std::string(e.what()));
		}
		catch (...)
		{
			logError("Unknown error in loopData!");
		}

		Sleep(10); // Prevent 100% CPU usage when idle
	}
}

void connectionTCP::createLoopdataThread()
{

	HANDLE hClientThread2 = CreateThread(NULL, 0, loopData, NULL, 0, 0);
	CloseHandle(hClientThread2);

}

// an endless loop that processes the sync-packet data: tasks, remove targets, research, trading, disconnect, errors.
void connectionTCP::updateCoopTask()
{

	// remove targets
	// coop
	if (getCoopStatic() && !pendingRemoveTargets.empty() && _game->getSavedGame())
	{

			if (playerInsideCoopBase == false && !_game->getSavedGame()->getSavedBattle())
			{

				for (Json::Value::ArrayIndex i = 0; i < pendingRemoveTargets.size(); ++i)
				{

					double d_lon = pendingRemoveTargets[i]["lon"].asDouble();
					double d_lan = pendingRemoveTargets[i]["lan"].asDouble();

					// mission sites
					auto& missionSites = *_game->getSavedGame()->getMissionSites();

					for (auto it = missionSites.begin(); it != missionSites.end();)
					{
						if ((*it)->getLongitude() == d_lon && (*it)->getLatitude() == d_lan)
						{
							it = missionSites.erase(it); // Removes and returns the next iterator
						}
						else
						{
							++it;
						}
					}

					// UFOS
					auto& ufos = *_game->getSavedGame()->getUfos();

					for (auto it = ufos.begin(); it != ufos.end();)
					{
						if ((*it)->getLongitude() == d_lon && (*it)->getLatitude() == d_lan)
						{
							it = ufos.erase(it);
						}
						else
						{
							++it;
						}
					}
				}

				pendingRemoveTargets.clear();
			}
		
	}

	// missions
	// coop
	if (getCoopStatic() && !pendingMissions.empty() && _game->getSavedGame())
	{

			if (playerInsideCoopBase == false && !_game->getSavedGame()->getSavedBattle())
			{

				for (Json::Value::ArrayIndex i = 0; i < pendingMissions.size(); ++i)
				{

					std::string str_deployment = pendingMissions[i]["deployment"].asString();
					std::string str_rules = pendingMissions[i]["rules"].asString();
					std::string str_race = pendingMissions[i]["race"].asString();
					std::string str_city = pendingMissions[i]["city"].asString();
					size_t int_time = pendingMissions[i]["time"].asUInt64();
					double d_lon = pendingMissions[i]["lon"].asDouble();
					double d_lat = pendingMissions[i]["lat"].asDouble();

					bool isDuplicate = false;

					// Check if the same coordinates already exist in the missionSite list
					for (const auto& existingSite : *_game->getSavedGame()->getMissionSites())
					{
						if (existingSite->getLongitude() == d_lon && existingSite->getLatitude() == d_lat)
						{
							isDuplicate = true;
							break;
						}
					}

					// Add a new MissionSite only if no duplicate is found
					if (!isDuplicate)
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

						MissionSite* missionSite = new MissionSite(_game->getMod()->getAlienMission(str_rules, true), deployment, nullptr);

						missionSite->setLongitude(d_lon);
						missionSite->setLatitude(d_lat);
						missionSite->setId(_game->getSavedGame()->getId(deployment->getMarkerName()));
						missionSite->setSecondsRemaining(100000000);
						missionSite->setAlienRace(str_race);
						missionSite->setDetected(true);
						missionSite->setCity(str_city);

						_game->getSavedGame()->getMissionSites()->push_back(missionSite);
					}
				}

				pendingMissions.clear();
			}
		
	}

	// coop
	// research
	if (getCoopStatic() && !waitedResearch.empty())
	{

		if (_game->getSavedGame()->getSelectedBase())
		{

			if (_game->getSavedGame()->getSelectedBase()->_coopBase == false)
			{

				for (Json::Value::ArrayIndex i = 0; i < waitedResearch.size(); ++i)
				{

					std::string jsonString = waitedResearch[i].toStyledString();

					syncResearch(jsonString);
				}

				waitedResearch.clear();
			}
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

		// Oldest message first (equivalent to taking from back in your old vtcpData model)
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
				OutputDebugStringA(("JSON parse error: " + errs + "\n").c_str());
				continue; // drop malformed
			}

			const std::string stateString = obj.get("state", "defaultState").asString();
			const int fromId = obj.get("from", -1).asInt(); // -1 if field missing

			// Decide if we handle it now or delay to preserve ordering
			const bool consumeNow = (_coop_task_completed || stateString == "abortPath");
			if (consumeNow)
			{
				// Use JSON directly as before
				onTCPMessage(stateString, obj);
				// continue to next item
			}
			else
			{
				// Not ready to consume -> put it back to the *front-hold* tail
				// to keep global ordering (oldest stays oldest).
				rxHold.emplace_front(std::move(jsonStr));
				break; // stop processing now to strictly preserve order
			}
		}
		catch (const std::exception& e)
		{
			OutputDebugStringA((std::string("Exception in processNetworkLoopNoLocks: ") + e.what() + "\n").c_str());
			// Put the item back to avoid losing it
			rxHold.emplace_front(std::move(jsonStr));
			onConnect = -3;
			break;
		}
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

		_game->getSavedGame()->getSavedBattle()->getBattleState()->moveCoopInventory(sel_item_name, item_name, inv_id, inv_x, inv_y, unit_id, item_id, move_cost, slot_x, slot_y, getHealQuantity, getPainKillerQuantity, getStimulantQuantity, getFuseTimer, getXCOMProperty, isAmmo, isWeaponWithAmmo, isFuseEnabled, getAmmoQuantity, slot_ammo, sel_item_id);

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
extern "C" unsigned long __stdcall startTCPClient(void* /*param*/)
{
	OutputDebugStringA("startTCPClient\n");
	resetCoopState(false); // client

#ifdef _WIN32
	Sleep(100); // tiny stagger; avoid long sleeps
#endif

	if (SDLNet_Init() == -1)
	{
		OutputDebugStringA("SDLNet init failed\n");
		onConnect = -3;
		return (unsigned long)-1;
	}

	IPaddress ip;
	if (SDLNet_ResolveHost(&ip, ipAddress.c_str(), tcp_port) == -1)
	{
		OutputDebugStringA("Can't resolve host\n");
		SDLNet_Quit();
		onConnect = 0;
		return (unsigned long)-1;
	}

	TCPsocket sock = SDLNet_TCP_Open(&ip);
	if (!sock)
	{
		OutputDebugStringA("Can't connect to server\n");
		SDLNet_Quit();
		onConnect = 0;
		return (unsigned long)-1;
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
					OutputDebugStringA("DISCONNECT CLIENT: SEND\n");
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
					OutputDebugStringA("DISCONNECT CLIENT: RECV\n");
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
					OutputDebugStringA("Client: message too large, disconnecting\n");
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
							continue; // <-- lisää tämä
						if (maybeHandlePongOnClient(obj))
							continue; // tämä oli jo olemassa
					}
					if (!g_rxQ.push(std::move(message)))
					{
						OutputDebugStringA("RX queue full, dropping message\n");
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
			Sleep(0);
#endif
		}
	}

client_cleanup:
	SDLNet_FreeSocketSet(socketSet);
	SDLNet_TCP_Close(sock);
	SDLNet_Quit();
	return 0;
}

// ===== Host thread (single client) =====
// Simplified for exactly two players: host + one client.
// If another client tries to connect, close it silently (no "server_full" message),
// because we only use sendTCPPacketStaticData for outbound traffic.
extern "C" unsigned long __stdcall startTCPHost(void* /*param*/)
{
	OutputDebugStringA("startTCPHost\n");
	resetCoopState(true); // host

	if (SDLNet_Init() == -1)
	{
		onConnect = -3;
		OutputDebugStringA("SDLNet init failed\n");
		return (unsigned long)-1;
	}

	IPaddress ip;
	if (SDLNet_ResolveHost(&ip, nullptr, tcp_port) == -1)
	{
		OutputDebugStringA("Can't resolve host\n");
		SDLNet_Quit();
		onConnect = -3;
		return (unsigned long)-1;
	}

	TCPsocket listening = SDLNet_TCP_Open(&ip);
	if (!listening)
	{
		OutputDebugStringA("Can't open TCP socket\n");
		SDLNet_Quit();
		onConnect = -3;
		return (unsigned long)-1;
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

		// ---- Accept new client if we don't have one ----
		if (TCPsocket newClient = SDLNet_TCP_Accept(listening))
		{
			if (!clientSock)
			{
				clientSock = newClient;
				SDLNet_TCP_AddSocket(socketSet, clientSock);
				OutputDebugStringA("Host: client connected\n");
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
					OutputDebugStringA("Host: send failed, drop client\n");
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
					OutputDebugStringA("Host: client disconnected\n");
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
					OutputDebugStringA("Host: message too large, drop client\n");
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
						OutputDebugStringA("RX queue full, dropping message\n");
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
			Sleep(0);
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
	return 0;
}

// TCP
void connectionTCP::onTCPMessage(std::string stateString, Json::Value obj)
{

	coopSession = true;

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

		connectionTCP::_coopGamemode = 1;

		CoopState* coopWindow = new CoopState(4);
		_game->pushState(coopWindow);

		sendMissionFile();
	}

	if (stateString == "craft_list")
	{

		size_t id = obj["selected_craft_id"].asUInt();

		_coop_selected_craft_id = id;
	}

	// abort path
	if (stateString == "abortPath")
	{

		int x = obj["x"].asInt();
		int y = obj["y"].asInt();
		int z = obj["z"].asInt();

		if (_game->getSavedGame())
		{
			if (_game->getSavedGame()->getSavedBattle())
			{

				if (_game->getSavedGame()->getSavedBattle()->getBattleGame())
				{

					_game->getSavedGame()->getSavedBattle()->getBattleGame()->abortCoopPath(x, y, z);

				}

			}
		}

	}

	// Removes the target UFO or mission sites
	if (stateString == "remove_target")
	{
		// Check directly in the loop
		bool exists = false;
		for (const auto& item : pendingRemoveTargets)
		{
			if (item == obj)
			{
				exists = true;
				break; // Stop checking if found
			}
		}
		if (!exists)
		{
			pendingRemoveTargets.append(obj);
		}
	}

	// Adds mission sites or UFOs
	if (stateString == "mission")
	{

		// Check directly in the loop
		bool exists = false;
		for (const auto& item : pendingMissions)
		{
			if (item == obj)
			{
				exists = true;
				break; // Stop checking if found
			}
		}
		if (!exists)
		{
			pendingMissions.append(obj);
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

		// set random seed
		uint64_t current_seed = obj["seed"].asUInt64();
		RNG::setCoopSeed(current_seed);

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

		int weekday = obj["weekday"].asInt();
		int day = obj["day"].asInt();
		int month = obj["month"].asInt();
		int year = obj["year"].asInt();
		int hour = obj["hour"].asInt();
		int minute = obj["minute"].asInt();
		int second = obj["second"].asInt();

		GameTime new_time(weekday, day, month, year, hour, minute, second);

		if (_game->getSavedGame())
		{
			_game->getSavedGame()->setTime(new_time);
		}

	}

	if (stateString == "changeHost")
	{

		setHost(false);

		connectionTCP::_coopGamemode = 1;
	}

	if (stateString == "changeHost3")
	{

		setPlayerTurn(1);
		setHost(true);
		connectionTCP::_coopGamemode = 1;
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

		if (coopInventory == true)
		{
			_game->getSavedGame()->getSavedBattle()->getBattleState()->moveCoopInventory(sel_item_name, item_name, inv_id, inv_x, inv_y, unit_id, item_id, move_cost, slot_x, slot_y, getHealQuantity, getPainKillerQuantity, getStimulantQuantity, getFuseTimer, getXCOMProperty, isAmmo, isWeaponWithAmmo, isFuseEnabled, getAmmoQuantity, slot_ammo, sel_item_id);
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

		//set random seed
		uint64_t current_seed = obj["seed"].asUInt64();
		RNG::setCoopSeed(current_seed);

		BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

		std::string jsonString = obj.toStyledString(); // Muuntaa koko JSON-objektin

		battlestate->movePlayerTarget(jsonString);
	}

	if (stateString == "BattleScapeTurn")
	{

		BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

		std::string jsonString = obj.toStyledString(); // Muuntaa koko JSON-objektin

		battlestate->turnPlayerTarget(jsonString);
	}

	if (stateString == "cancel_action")
	{
		_game->getSavedGame()->getSavedBattle()->getBattleState()->coopCancelAction();
	}

	if (stateString == "launch_press")
	{

		_game->getSavedGame()->getSavedBattle()->getBattleState()->coopLaunchPress();
	}

	if (stateString == "psi_press")
	{

		_game->getSavedGame()->getSavedBattle()->getBattleState()->coopPsiButtonAction();
	}

	if (stateString == "BattleScapeShoot")
	{

		int actor_id = obj["actor_id"].asInt();
		int actor_tu = obj["actor_tu"].asInt();
		int type = obj["type"].asInt();
		bool targeting = obj["targeting"].asBool();
		std::string hand = obj["hand"].asString();
		int fusetimer = obj["fusetimer"].asInt();
		bool fuse = obj["fuse"].asBool();
		int shoot_type = obj["shoot_type"].asInt();

		std::string weapon_type = obj["weapon_type"].asString();

		int target_x = obj["coords"]["target"]["x"].asInt();
		int target_y = obj["coords"]["target"]["y"].asInt();
		int target_z = obj["coords"]["target"]["z"].asInt();

		int start_x = obj["coords"]["start"]["x"].asInt();
		int start_y = obj["coords"]["start"]["y"].asInt();
		int start_z = obj["coords"]["start"]["z"].asInt();

		Position target = Position(target_x, target_y, target_z);

		Position start = Position(start_x, start_y, start_z);

		BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();

		battlestate->shootPlayerTarget(actor_id, target, type, hand, targeting, fuse, fusetimer, shoot_type, weapon_type, actor_tu, start);
	}

	if (stateString == "active_grenade")
	{

		// set random seed
		//uint64_t current_seed = obj["seed"].asUInt64();
		//RNG::setCoopSeed(current_seed);

		int actor_id = obj["actor_id"].asInt();
		int type = obj["type"].asInt();
		std::string hand = obj["hand"].asString();

		bool fusetimer = obj["fusetimer"].asInt();

		int item_id = obj["item_id"].asInt();

		_game->getSavedGame()->getSavedBattle()->getBattleState()->coopActiveGranade(actor_id, type, hand, fusetimer, item_id);
	}

	if (stateString == "action_click")
	{

		// set random seed
		//uint64_t current_seed = obj["seed"].asUInt64();
		//RNG::setCoopSeed(current_seed);

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

	if (stateString == "unit_action")
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

	// medkit
	if (stateString == "medkit")
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

	// RANDOM SEED
	if (stateString == "current_seed")
	{

		// set random seed
		uint64_t current_seed = obj["seed"].asUInt64();
		RNG::setCoopSeed(current_seed);


		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

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
							int stun = obj["units"][i]["stun"].asInt();
							bool is_out = obj["units"][i]["is_out"].asBool();
							int motionpoints = obj["motionpoints"].asInt();

							int setDirection = obj["units"][i]["setDirection"].asInt();
							int setFaceDirection = obj["units"][i]["setFaceDirection"].asInt();

							unit->setDirection(setDirection);
							unit->setFaceDirection(setFaceDirection);

							unit->setMotionPointsCoop(motionpoints);
							unit->setTimeUnits(time);
							unit->setHealth(health);
							unit->setCoopMorale(morale);
							unit->setCoopEnergy(energy);
							unit->setCoopMana(mana);

							if (is_out == true)
							{

								if (!unit->isOut())
								{
									unit->kill();
								}

								continue;
							}

							int pos_x = obj["units"][i]["pos_x"].asInt();
							int pos_y = obj["units"][i]["pos_y"].asInt();
							int pos_z = obj["units"][i]["pos_z"].asInt();

							// Check if positions do not match
							if (unit->getPosition().x != pos_x || unit->getPosition().y != pos_y || unit->getPosition().z != pos_z)
							{

								_game->getSavedGame()->getSavedBattle()->getBattleGame()->teleport(pos_x, pos_y, pos_z, unit);

							}

			

						}
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

		bool battle_over = obj["battle"].asBool();

		bool error = false;

		//  selected unit
		int actor_id = obj["actor_id"].asInt();

		// set random seed
		uint64_t current_seed = obj["seed"].asUInt64();
		RNG::setCoopSeed(current_seed);

		if (_game->getSavedGame())
		{

			if (_game->getSavedGame()->getSavedBattle())
			{

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
							int stun = obj["units"][i]["stun"].asInt();
							bool is_out = obj["units"][i]["is_out"].asBool();
							int motionpoints = obj["motionpoints"].asInt();

							int setDirection = obj["units"][i]["setDirection"].asInt();
							int setFaceDirection = obj["units"][i]["setFaceDirection"].asInt();

							unit->setDirection(setDirection);
							unit->setFaceDirection(setFaceDirection);

							unit->setMotionPointsCoop(motionpoints);
							unit->setTimeUnits(time);
							unit->setHealth(health);
							unit->setCoopMorale(morale);
							unit->setCoopEnergy(energy);
							unit->setCoopMana(mana);

							if (is_out == true)
							{

								if (!unit->isOut())
								{
									unit->kill();
								}

								continue;
							}

							int pos_x = obj["units"][i]["pos_x"].asInt();
							int pos_y = obj["units"][i]["pos_y"].asInt();
							int pos_z = obj["units"][i]["pos_z"].asInt();

							// Check if positions do not match
							if (unit->getPosition().x != pos_x || unit->getPosition().y != pos_y || unit->getPosition().z != pos_z)
							{

								error = true;

								_game->getSavedGame()->getSavedBattle()->getBattleGame()->teleport(pos_x, pos_y, pos_z, unit);

								if (getHost() == false)
								{

									_game->getSavedGame()->getSavedBattle()->_endturnCoop = true;
								}

							}

							/*
							if (getHost() == true)
							{

								// Check if positions do not match
								if (unit->getPosition().x != pos_x || unit->getPosition().y != pos_y || unit->getPosition().z != pos_z)
								{

									error = true;
									OutputDebugStringA("Out of sync!!!");
								}
							}
							else
							{

						

							}
							*/

							break;
						}
					}
				}


			}


			if (battle_over == true)
			{

				for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					unit->kill();
				}

				if (error == false)
				{
					BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();
					battlestate->endTurnCoop();
				}


				return;
			}

			if (getHost() == false && battle_over == false)
			{

				// if not pvp
				if (connectionTCP::_coopGamemode != 2 && connectionTCP::_coopGamemode != 3)
				{
					if (error == false)
					{
						BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();
						battlestate->endTurnCoop();
					}

					setPlayerTurn(1);

				}
				// pvp or pvp2
				else
				{

					// reset timeunits
					for (auto& unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
					{

						if (unit->_coop_mindcontrolled == true)
						{

							if (unit->getCoop() == 0)
							{
								unit->setCoop(1);
							}
							else if (unit->getCoop() == 1)
							{
								unit->setCoop(0);
							}

							unit->_coop_mindcontrolled = false;
						}

						unit->resetTimeUnitsAndEnergy();
					}

					setPlayerTurn(2);

					if (error == false)
					{
						BattlescapeState* battlestate = _game->getSavedGame()->getSavedBattle()->getBattleState();
						battlestate->endTurnCoop();
					}
				}
			}
			else if (battle_over == false)
			{

				// Auto save before (only HOST)
				SavedGame* newsave = new SavedGame(*_game->getSavedGame());

				newsave->setName("coop_mission_2");

				newsave->save("coop_mission_2.sav", _game->getMod());

				// check if out of sync with host
				for (auto unit : *_game->getSavedGame()->getSavedBattle()->getUnits())
				{

					// if pvp
					if (connectionTCP::_coopGamemode == 2 || connectionTCP::_coopGamemode == 3)
					{

						unit->resetTimeUnitsAndEnergy();
					}
				}

				setPlayerTurn(2);

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

		OutputDebugStringA("MAP_RESULT_CLIENT");

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
				OutputDebugStringA("Error: obj missing 'data' or it is not a string.\n");
			}
		}
		catch (const std::exception& e)
		{
			OutputDebugStringA(("Exception in map loading: " + std::string(e.what()) + "\n").c_str());
		}
		catch (...)
		{
			OutputDebugStringA("Unknown exception in map loading.\n");
		}
	}

	if (stateString == "COOP_READY_CLIENT" && onTcpHost == true)
	{

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
			OutputDebugStringA("host folder created!");
		}
		else
		{
			OutputDebugStringA("host folder failed created!");
		}

		// file (client)
		if (std::filesystem::create_directory(Options::getMasterUserFolder() + "/client"))
		{
			OutputDebugStringA("client folder created!");
		}
		else
		{
			OutputDebugStringA("client folder failed created!");
		}

		// IF THE HOST IS IN BATTLE, INCLUDE JOINERS; OTHERWISE DO NOTHING
		// DISPLAY THE CLIENT PLAYER'S BASE
		std::string playername = obj.get("playername", "defaultState").asString();

		tcpPlayerName = playername;

		// CHECK GAMEMODE
		if (getCoopCampaign() == true)
		{
			connectionTCP::_coopGamemode = 1;
		}

		Json::Value root;
		root["state"] = "COOP_READY_HOST";
		root["playername"] = sendTcpPlayer;
		root["gamemode"] = connectionTCP::_coopGamemode;

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
		root["mods_count"] = mod_names.size();

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

		sendTCPPacketData(root.toStyledString().c_str());

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

		// CHECK GAMEMODE
		if (getCoopCampaign() == true)
		{
			connectionTCP::_coopGamemode = 1;
		}

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
			OutputDebugStringA("host folder created!");
		}
		else
		{
			OutputDebugStringA("host folder failed created!");
		}

		// file (client)
		if (std::filesystem::create_directory(Options::getMasterUserFolder() + "/client"))
		{
			OutputDebugStringA("client folder created!");
		}
		else
		{
			OutputDebugStringA("client folder failed created!");
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

			if (base->_coopBase == false)
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

			if (base->_coopBase == false)
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

		Json::Value markers;

		int index = 0;
		for (auto base : *_game->getSavedGame()->getBases())
		{

			if (base->_coopBase == false)
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

	if (stateString == "craftSoldiers" && onTcpHost == false)
	{

		std::string craftUsed = obj.get("spaceUsed", "0").asString();

		setHostSpaceAvailable(std::stoi(craftUsed));

		OutputDebugStringA(std::to_string(getHostSpaceAvailable()).c_str());

		generateCraftSoldiers();
	}

	if (stateString == "SEND_FILE_CLIENT_TRUE" && onTcpHost == false)
	{

		OutputDebugStringA("SEND_FILE_CLIENT_TRUE");

		CoopState* coopWindow = new CoopState(1);
		_game->pushState(coopWindow);

		std::string jsonData = "{\"state\" : \"SEND_FILE_CLIENT\"}";

		sendTCPPacketData(jsonData);
	}

	if (stateString == "SEND_FILE_CLIENT_SAVE" && onTcpHost == true)
	{

		CoopState* coopWindow = new CoopState(666);
		coopWindow->loadWorld();

		OutputDebugStringA("SEND_FILE_CLIENT_SAVE");
		sendFileClient = true;
	}

	if (stateString == "SEND_FILE_HOST_SAVE" && onTcpHost == true)
	{

		// HERE ON THE HOST, DISPLAY A NOTIFICATION AND SEND THE CLIENT INFORMATION ABOUT THE FILE TRANSFER
		inventory_battle_window = false;

		_game->pushState(new CoopState(1));

		OutputDebugStringA("SEND_FILE_HOST_SAVE");

		Json::Value root;

		root["state"] = "SEND_FILE_CLIENT_SAVE_TRUE";

		sendTCPPacketData(root.toStyledString().c_str());

		setHost(false);
	}

	if (stateString == "SEND_FILE_CLIENT_SAVE_TRUE" && onTcpHost == false)
	{

		CoopState* coopWindow = new CoopState(666);
		coopWindow->loadWorld();

		OutputDebugStringA("SEND_FILE_CLIENT_SAVE_TRUE");

		sendFileSave = true;
		sendFileClient = true;

		setHost(true);
	}

	if (stateString == "SEND_FILE_CLIENT" && onTcpHost == true)
	{
		OutputDebugStringA("SEND_FILE_CLIENT");
		sendFileClient = true;
	}

	// INFORMATION FROM HOST TO CLIENT ABOUT MAP LOADING!
	if (stateString == "SEND_FILE_HOST_TRUE" && onTcpHost == true)
	{

		OutputDebugStringA("SEND_FILE_HOST_TRUE");

		Json::Value root;

		root["state"] = "SEND_FILE_HOST";

		sendTCPPacketData(root.toStyledString());
	}

	// INFORMATION FROM CLIENT TO HOST ABOUT MAP LOADING
	if (stateString == "SEND_FILE_HOST" && onTcpHost == false)
	{
		OutputDebugStringA("SEND_FILE_HOST");
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

		_game->getSavedGame()->getCoop()->sendFileBase(1);

		OutputDebugStringA("SEND_FILE_HOST_BASE");
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

		_game->getSavedGame()->getCoop()->sendFileBase(1);

		OutputDebugStringA("SEND_FILE_CLIENT_BASE");
		sendFileClient = true;
		sendFileBase = true;
	}

	if (stateString == "MAP_RESULT_CLIENT_BASE" && onTcpHost == false)
	{

		OutputDebugStringA("MAP_RESULT_CLIENT_BASE");

		writeHostMapFile2();

		CoopState* coopWindow = new CoopState(55);
		coopWindow->loadWorld();

		// send the file immediately if needed...
		if (gettingData == 2)
		{

			OutputDebugStringA("SEND_FILE_HOST_BASE");
			sendFileHost = true;
			sendFileBase = true;
		}

		gettingData = 0;
	}

	if (stateString == "MAP_RESULT_HOST_BASE" && onTcpHost == true)
	{

		OutputDebugStringA("MAP_RESULT_HOST_BASE");

		writeHostMapFile2();

		CoopState* coopWindow = new CoopState(55);
		coopWindow->loadWorld();

		// lahetetaan samantien tiedosto jos tarvetta...
		if (gettingData == 2)
		{

			OutputDebugStringA("SEND_FILE_CLIENT_BASE");
			sendFileClient = true;
			sendFileBase = true;
		}

		gettingData = 0;
	}
}



void connectionTCP::setPauseOn()
{

	// coop
	if (getCoopStatic() == true && (getCurrentTurn() == 1 || getCurrentTurn() == 3) && gamePaused == 0 && _battleWindow == false)
	{

		Json::Value root;

		root["state"] = "GamePausedON";

		sendTCPPacketData(root.toStyledString().c_str());
	}
}

void connectionTCP::setPauseOff()
{

	// coop
	if (getCoopStatic() == true && (getCurrentTurn() == 1 || getCurrentTurn() == 3) && gamePaused == 0 && _battleWindow == false)
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
		OutputDebugStringA("TX queue full, dropping packet\n");
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
	_game->getSavedGame()->getCoop()->sendFile();
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

void connectionTCP::syncResearch(std::string research)
{

	CoopState* window = new CoopState(99);
	_game->pushState(window);

	_game->getSavedGame()->syncResearch(research);
}

void connectionTCP::sendResearch()
{
	std::string json_string = _game->getSavedGame()->sendResearch();

	sendTCPPacketData(json_string);

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

	HANDLE hostThread = CreateThread(NULL, 0, startTCPHost, NULL, 0, 0);
	CloseHandle(hostThread);
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

	HANDLE clientThread = CreateThread(NULL, 0, startTCPClient, NULL, 0, 0);
	CloseHandle(clientThread);
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
		OutputDebugStringA("TX queue full, dropping packet\n");
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

		}
		// client
		else
		{

			onConnect = -1;
			coopSession = false;


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

		// new values
		gettingData = 0;
		sendFileClient = false;
		sendFileBase = false;
		sendFileHost = false;
		sendFileSave = false;
		onceTime = false;

		isWaitMap = true;

		setPlayerTurn(2);

		_battleWindow = false;
		_battleInit = false;

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



