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

#include "../Engine/Options.h"

#include "../Savegame/Ufo.h"

#include "CrashHandler.h" // coop

#include <algorithm> // clamp, minmax
#include <cmath>     // round
#include <optional>

#ifdef _WIN32
#include <windows.h>
#endif

inline void DebugLog(const std::string& msg)
{
#ifdef _WIN32
	OutputDebugStringA(msg.c_str());
#else
	std::fprintf(stderr, "%s\n", msg.c_str());
#endif

	if (OpenXcom::Options::logInfoToFile && OpenXcom::Options::debugMode)
	{
		CrashHandler::log(msg);
	}

}

inline void DebugLog(const char* msg)
{
	DebugLog(std::string(msg));
}

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
 class Ufo;

class connectionTCP
{
  private:
	std::thread _loopThread;
	std::thread _clientThread;
	std::thread _hostThread;
	inline static SPSCQueue<1024> txQ{};
	inline static SPSCQueue<1024> rxQ{};
	// chat menu
	ChatMenu* _chatMenu = nullptr;
	Game* _game;
	Uint32 lastRandomClear = 0;
	void generateCraftSoldiers();
	bool _onTCP = false;
	bool _stop = false;
	bool _hostStop = false;
	bool _clientStop = false;
	void loopData();
	void startTCPClient();
	void startTCPHost();
  public:
	// coop
	connectionTCP(Game* game);
	~connectionTCP();  
	bool _isMainCampaignBaseDefense = false;
	bool coop_end_turn = false;
	bool allow_cutscene = true;
	// research
	Json::Value waitedResearch;
	static bool _isChatActiveStatic;
	void createLoopdataThread();
	void updateCoopTask();
	std::string getCurrentClientName();
	std::string getHostName();
	void setClientSoldiers();
	void deleteAllCoopBases();
	void updateAllCoopBases();
	void fixCoopSave();
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
	void sendBaseFile();
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
	static bool getServerOwner();
	bool ready_coop_battle = false; // notify the other player that the co-op mission is starting
	std::vector<Soldier*> coopSoldiers;
	std::string current_base_name = "";
	int64_t coopFunds = 0;
	void setHost(bool host);
	static bool playerInsideCoopBase; // is the player really in another player's base?
	bool coopMissionEnd = false; // is the co-op mission completed?
	Json::Value _jsonTargets, _jsonDamages, _jsonInventory;
	void syncCoopInventory();
	static bool coopInventory;
	int _pathLock = -1;
	void setPathLock(int lock);
	bool _waitBC = false; // is the client ready in battle?
	bool _waitBH = false; // is the host ready in battle?
	bool _battleWindow = false; // end turn screen
	static bool _battleInit; // when both have joined and are ready for battle, initialize
	bool resetCoopInventory = false;
	int _playerTurn = 0; // 0 = no one, 1 = team, 2 = your, 3 = waiting, 4 = spectator mode
	void setPlayerTurn(int turn);
	void sendFile();
	// is the player actually connected?
	int isConnected();
	void setConnected(int state);
	void disconnectTCP();
	ChatMenu* getChatMenu();
	void setChatMenu(ChatMenu* menu);

	int unitstatusToInt(UnitStatus status);
	UnitStatus intToUnitstatus(int status);
	int ufostatusToInt(Ufo::UfoStatus status);
	Ufo::UfoStatus intToUfostatus(int status);
	int ItemDamageRandomTypeToInt(ItemDamageRandomType type);
	ItemDamageRandomType intToItemDamageRandomType(int type);
	int ItemDamageTypeToInt(ItemDamageType type);
	ItemDamageType intToItemDamageType(int type);

	// coop projectiles
	Json::Value _coopProjectilesClient;
	Json::Value _coopProjectilesHost;

	Json::Value _coopEndPath = Json::nullValue;

	bool _coopInitDeath = false;
	bool _coopWalkInit = false;
	bool _coopAllow = true;

	int _coopPVPwin = 0; // 0 = not set, 1 = xcom, 2 = ufo

	bool _clientPanicHandle = false;

	static bool _isActiveAISync;

	static bool _isActivePlayerSync;

	bool _onClickClose = false;

	int _currentAmmoID = -1;
	std::string currentAmmoType = "";

	bool _enable_research_sync = true;

	static bool _enable_time_sync;

	static bool _enable_reaction_shoot;

	static bool _enable_other_player_footsteps;

	static bool _enable_host_only_time_speed;

	static bool _enable_xcom_equipment_aliens_pvp;

	static bool _reset_timeunits_onturnchange_pvp;

	int walk_end_unit_id = -1;

	bool AbortCoopWalk = false;

	// time
	int _weekday = 0;
	int _day = 0;
	int _month = 0;
	int _year = 0;
	int _hour = 0;
	int _minute = 0;
	int _second = 0;

	int _AIProgressCoop = -1;
	bool _AISecondMoveCoop = false;
	int _coopEnd = 0;
	int _psi_target_id = -1;

	int _melee_target_id = -1;
	int _melee_hit_number = -1;

	std::vector<BattleActionAttack> _battleActions;

	std::vector <int> _smokeRNGs;

	bool pve2_init = false;

	std::string other_time_speed_coop = "";

	int show_coop_mission_popup = -1;

	std::string show_coop_ufo_popup_type = "";
	std::string show_coop_ufo_popup_race = "";
	std::string show_coop_ufo_popup_altitude = "";

	bool show_coop_monthly_report = false;

	int fundingDiffCoop = -1;
	int ratingTotalCoop = -1;
	int lastMonthsRatingCoop = -1;

	std::vector<std::string> _happyListCoop, _sadListCoop, _pactListCoop, _cancelPactListCoop;

	Json::Value _coopFacility;

	Json::Value _deleteCoopFacility;

	bool show_briefing_state = false;

	std::vector<Position> _trajectoryCoop;

	std::string _debriefing_coop_title = "";

	std::string load_state = "Please wait";

};

}
