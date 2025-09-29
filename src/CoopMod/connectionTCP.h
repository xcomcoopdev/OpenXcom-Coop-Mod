#pragma once
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

#include <iostream>
#include <fstream>
#include <thread>
#include <vector>

#include <filesystem>
#include <cctype> // std::isdigit
#include <mutex>

#include <deque>

#include <json/json.h>
#include <SDL_net.h>

#include "CoopMenu.h"
#include "CoopState.h"
#include "Profile.h"
#include "ChatMenu.h"

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

	// <<< TÄHÄN LISÄTÄÄN >>>
	bool empty() const
	{
		return tail.load(std::memory_order_acquire) ==
			   head.load(std::memory_order_acquire);
	}

	bool full() const
	{
		size_t h = head.load(std::memory_order_relaxed);
		size_t n = (h + 1) % N;
		return n == tail.load(std::memory_order_acquire);
	}
};

namespace OpenXcom
{

extern SPSCQueue<1024> g_txQ;
extern SPSCQueue<1024> g_rxQ;

void sendTCPPacketStaticData(std::string data);

// Single place for enqueue logic.
// Returns false if queue is full (packet dropped).
bool enqueueTx(std::string&& s);

 class Game;

class connectionTCP
{
  private:
	inline static SPSCQueue<1024> txQ{};
	inline static SPSCQueue<1024> rxQ{};
	// chat menu
	ChatMenu* _chatMenu = nullptr;
	Game* _game;
	Uint32 lastRandomClear = 0;
	void generateCraftSoldiers();
	bool _onTCP = false;
  public:
	bool _isMainCampaignBaseDefense = false;
	bool coop_end_turn = false;
	bool allow_cutscene = true;
	static bool _isChatActiveStatic;
	connectionTCP(Game* game);
	void createLoopdataThread();
	void updateCoopTask();
	std::string getCurrentClientName();
	std::string getHostName();
	void setClientSoldiers();
	void deleteAllCoopBases();
	void updateAllCoopBases();
	// coop
	// battle states
	void setConfirmLandingState(ConfirmLandingState* landing);
	void setNewBattleState(NewBattleState* battlesate);
	void setGeoscapeState(GeoscapeState* base_geo);
	NewBattleState* getNewBattleState();
	bool getLanding();
	void setSelectedCraft(Craft* selectedCraft);
	Craft* getSelectedCraft();
	void hostTCPServer(std::string playername, std::string port);
	void connectTCPServer(std::string playername, std::string ipaddress);
	void onTCPMessage(std::string data, Json::Value obj);
	void sendMissionFile();
	int gamePaused = 0; // 0 = no set, 1 = team, 2 = your
	bool cancel_connect = false;
	int getCurrentTurn();
	void loadHostMap();
	static bool getCoopStatic(); // is the player actually connected?
	void sendTCPPacketData(std::string data); // Send TCP packet data
	static bool getHost();
	static int getHostSpaceAvailable();
	static void setHostSpaceAvailable(int _hostSpace);
	void setPauseOn();
	void setPauseOff();
	bool _coop_task_completed = true; // is the co-op task completed (walk, turn, shoot, etc.)?
	size_t _coop_selected_craft_id = 0;
	std::string getPing();
	bool isCoopSession(); // is the co-op session created? (does not consider whether a player has joined)
	void setCoopSession(bool session);
	static bool _coopCampaign;
	void setCoopCampaign(bool coop);
	static int _coopGamemode; // no mode = 0, PVE = 1, PVP = 2, PVP2 = 3, PVE2 = 4,
	bool getCoopCampaign();
	// no mode = 0, PVE = 1, PVP = 2, PVP2 = 3, PVE2 = 4,
	static int getCoopGamemode();
	void createCoopMenu();
	static void sendTCPPacketStaticData2(std::string data);
	void writeHostMapFile2();
	void writeHostMapFile();
	bool inventory_battle_window = true; // Do not use inventory if another player joins a saved game
	bool getServerOwner();
	bool ready_coop_battle = false; // notify the other player that the co-op mission is starting
	std::vector<Soldier*> coopSoldiers;
	std::string current_base_name = "";
	int64_t coopFunds = 0;
	void setHost(bool host);
	bool playerInsideCoopBase = false; // is the player really in another player's base?
	bool coopMissionEnd = false; // is the co-op mission completed?
	Json::Value _jsonTargets, _jsonDamages, _jsonInventory;
	void syncCoopInventory();
	bool coopInventory = false;
	int _pathLock = -1;
	void setPathLock(int lock);
	bool _waitBC = false; // is the client ready in battle?
	bool _waitBH = false; // is the host ready in battle?
	bool _battleWindow = false; // end turn screen
	bool _battleInit = false; // when both have joined and are ready for battle, initialize
	int _playerTurn = 0; // 0 = no one, 1 = team, 2 = your, 3 = waiting, 4 = spectator mode
	void setPlayerTurn(int turn);
	void sendFile();
	// is the player actually connected?
	int isConnected();
	void setConnected(int state);
	void disconnectTCP();
	void syncResearch(std::string);
	void sendResearch();
	ChatMenu* getChatMenu();
	void setChatMenu(ChatMenu* menu);
};

}
