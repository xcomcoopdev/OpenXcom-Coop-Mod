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
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <filesystem>
#include <cctype> // std::isdigit
#include <mutex>

#include <deque>

#include <json/json.h>

#include <SDL_net.h>

#include "ServerList.h"
#include "LobbyMenu.h"

#include "CoopMenu.h"
#include "CoopState.h"
#include "Profile.h"
#include "ChatMenu.h"

#include "../Engine/Options.h"

#include "../Savegame/Ufo.h"
#include "../Mod/RuleInventory.h"

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

// Shared network queues used by both connectionTCP and connectionUDP.
// Definitions must exist exactly once in a .cpp file, normally connectionTCP.cpp:
extern SPSCQueue<1024> g_txQ;
extern SPSCQueue<1024> g_rxQ;
extern int tcp_port;

// Existing name kept for compatibility: this only enqueues to g_txQ.
// It does not have to mean that the active transport is TCP.
void sendTCPPacketStaticData(std::string data);

// Single place for enqueue logic.
// Returns false if queue is full, so caller may log/drop/retry.
bool enqueueTx(std::string&& s);

// Clears shared TCP/UDP transport queues and the updateCoopTask hold queue.
// Call this when leaving a multiplayer session before starting a new one.
void clearNetworkSessionQueues();

class Game;
class Ufo;

class connectionTCP
{
  private:
	std::thread _loopThread;
	std::thread _clientThread;
	std::thread _hostThread;
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
	void initProfile(bool clientInBattle, bool inBattle);
	long long getDateTimeCoop() const;
	void clearAllReceivedTCPPackets();
	void createLoopdataThread();
	void updateCoopTask();
	std::vector<std::string> splitVectorMod(std::string s, std::string delimiter);
	bool hasRequiredMods(const std::string& mod_hash);
	std::string getCurrentClientName();
	std::string getCurrentClientServer();
	void setCurrentClientServer(std::string servername);
	std::string getHostName();
	std::string getHostServer();
	void setHostName(std::string playername);
	void setHostServer(std::string servername);
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
	void hostTCPServer(std::string servername, std::string port);
	void connectTCPServer(std::string ipaddress, std::string port);
	void onTCPMessage(std::string data, Json::Value obj);
	void sendBaseFile();
	void sendMissionFile();
	void sendSaveProgressFile();
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
	void setServerOwner(bool owner);
	static bool _coopCampaign;
	void setCoopCampaign(bool coop);
	static int _coopGamemode; // no mode = 0, PVE = 1, PVP = 2, PVP2 = 3, PVE2 = 4,
	static int coop_save_owner_player_id; // ID of the player who owns the co-op save 
	bool getCoopCampaign();
	// no mode = 0, PVE = 1, PVP = 2, PVP2 = 3, PVE2 = 4,
	static int getCoopGamemode();
	void createCoopMenu();
	static void sendTCPPacketStaticData2(std::string data);
	void writeHostMapFile2();
	void writeHostMapFile();
	void writeHostMapSaveProgressFile();
	void writeHostMapLoadProgressFile();
	bool inventory_battle_window = true; // Do not use inventory if another player joins a saved game
	static bool getServerOwner();
	bool ready_coop_battle = false; // notify the other player that the co-op mission is starting
	bool ready_coop_save_progress = false; // Notify the other player that progress saving is starting
	std::vector<Soldier*> coopSoldiers;
	std::string current_base_name = "";
	int64_t coopFunds = 0; // Stores the current player’s funds
	int64_t playersFunds = 0; // Stores the funds of all players
	int64_t playersCrafts = 0;  // Stores the crafts of all players
	int64_t playersBases = 0; // Stores the bases of all players
	void setHost(bool host);
	static bool playerInsideCoopBase; // is the player really in another player's base?
	bool coopMissionEnd = false; // is the co-op mission completed?
	Json::Value _jsonTargets, _jsonDamages, _jsonInventory, jsonAddedCoopItems;
	void syncCoopInventory();
	static bool coopInventory;
	int _pathLock = -1;
	void setPathLock(int lock);
	bool _waitBC = false; // is the client ready in battle?
	bool _waitBH = false; // is the host ready in battle?
	bool _battleWindow = false; // end turn screen
	static bool _battleInit; // when both have joined and are ready for battle, initialize
	int _playerTurn = 0; // 0 = no one, 1 = team, 2 = your, 3 = waiting, 4 = spectator mode
	void setPlayerTurn(int turn);
	void sendFile();
	// is the player actually connected?
	int isConnected();
	void setConnected(int state);
	void disconnectTCP(bool isMain = false);
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
	int InventoryTypeToInt(InventoryType type);
	InventoryType intToInventoryType(int type);
	int SoldierRanktoInt(SoldierRank rank);
	SoldierRank intToSoldierRank(int rank);

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

	static bool _unbalanced_craft_soldiers_limit;

	static bool _host_save_progress;

	int walk_end_unit_id = -1;

	bool AbortCoopWalk = false;

	// time
	static int _weekday;
	static int _day;
	static int _month;
	static int _year;
	static int _hour;
	static int _minute;
	static int _second;

	static int monthsPassed;
	static int daysPassed;

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
	// coop: teammate's last-reported geoscape time-speed id ("_btn5Secs".."_btn1Day"),
	// "" if unknown. Unlike other_time_speed_coop (consumed/cleared every timeAdvance),
	// this persists so the geoscape UI can show which speed the ally has selected.
	std::string peerTimeSpeedId = "";
	// coop: wall-clock ms of the last "time" heartbeat received from the peer,
	// updated on both sides. A "time" packet is emitted every GeoscapeState::think()
	// the peer spends on the geoscape; if it goes stale the peer is away
	// (base/options/popup/etc.). The host uses this to freeze the shared clock, and
	// both sides use it to dim the ally marker to yellow. Written on the packet-handler
	// thread, read on the main thread, hence atomic.
	std::atomic<Uint32> lastPeerTimePacketMs{0};
	// coop: which geoscape location the teammate is looking at, for the ally marker.
	// -1 = on the geoscape (use peerTimeSpeedId); 0..5 = a toolbar sub-screen index
	// (Intercept/Bases/Graphs/Ufopaedia/Options/Funding). Reset to -1 whenever a
	// "time" packet arrives (those are only sent from the geoscape).
	std::atomic<int> peerFocusScreen{-1};

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

	Json::Value _soldier_stats;

	Json::Value _battle_stats;

	bool show_briefing_state = false;

	std::vector<Position> _trajectoryCoop;

	std::string _debriefing_coop_title = "";

	std::string load_state = "Please wait";

	static bool moveCoopItems;

	int _selectedItemID = -1;
	std::string _selectedItemType = "";

	bool _coop_promotions = false;

	int _hasHitUnit = -1;

	bool openMultipleTargetsMenu = false;

	static bool no_bases;
	static bool isCoopBaseLoading;

	// hotseat
	static bool _isHotseatActive;
	static bool _isHotseatReactionFireEnabled;
	bool _changeHotseatTurn = false;
	bool _isHotseatAlienTurn = false;
	Json::Value _discoveredTilesAlienTurn;
	Json::Value _discoveredTilesXComTurn;
	bool _firstAlienInit = false;

	// MissionStatistics
	Json::Value toJson(const std::map<int, int>& m);
	std::map<int, int> fromJson(const Json::Value& j);
	Json::Value _missionStatisticsCoop = Json::nullValue;

	// inventory
	static bool show_inactive_player_inventory;

	// pause
	static bool pauseSound;

	// LOAD_PROGRESS
	bool _isLoadProgress = false;

	// Stores coop files in a hash map instead of separate files in the host folders
	static std::unordered_map<std::string, std::string> coopFilesHost;
	// Stores coop files in a hash map instead of separate files in the client folders
	static std::unordered_map<std::string, std::string> coopFilesClient;
	static bool hasCoopFile(const std::string& key);

	// save
	static bool saveError;
	static long long saveID;

	// password
	static bool isPasswordRequired;
	static std::string password;

	// lobby menu
	static bool isCoopSessionLocked;
	static bool isPlayerReady;
	static bool isPlayersReady;
	static int LobbyFileStatus;
	static int lobby_timer;
	static bool forceCloseCoopStateMenu;
	static bool forceClosePasswordCheckMenu;
	static bool isLobbyMenuClosed;

	// other
	static int manuallyAddedServerRemoveID;
	static bool canRemoveManuallyAddedServer;
	static bool isInfoboxClosed;

	// Permanently transfers a soldier to another player (0 = host, 1 = client).
	// Follows the guest-soldier model: a soldier's object lives in its OWNER's
	// save, tagged with coopBase = the station base's coop id when that base
	// belongs to the other player (-1 when stationed at one of the owner's own
	// bases). The soldier is serialized, removed from the giver's save and
	// recreated in the receiver's save, keeping the same station base - so it
	// stays "in" the base it was in, and shows up when the new owner views
	// that base. During battle only the control flags flip immediately; the
	// physical move is queued and runs after the mission ends. Transfers
	// overwrite unconditionally, so soldiers can be traded back and forth.
	void transferSoldierOwnership(Soldier* soldier, int newOwnerId, bool broadcast);
	// Completes queued in-battle transfers once no battle is active. Must run
	// before the post-battle coop cleanup (GeoscapeState calls it first).
	void processPendingSoldierTransfers();

  private:
	// Serializes the soldier (with its station base id) and sends the
	// physical-transfer packet to the peer.
	void sendSoldierTransferPacket(Soldier* soldier, int newOwnerId);
	// Erases the soldier pointer from every base roster (including the
	// SoldiersState/CraftSoldiersState base_oldsoldiers snapshots).
	void removeSoldierFromLocalBases(Soldier* soldier);
	// In-battle transfers waiting for the mission to end: soldier + new owner.
	std::vector<std::pair<Soldier*, int> > _pendingSoldierTransfers;
	// Soldiers transferred away are parked here instead of deleted: UI states
	// (sort snapshots, open dialogs) may still hold pointers to them.
	std::vector<Soldier*> _transferredSoldiers;
	// Ids of soldiers transferred away this session. A stale copy of one of
	// these can resurrect when the pre-visit "basehost" snapshot is restored;
	// the sweep in processPendingSoldierTransfers() parks exactly those (and
	// nothing else - legacy saves carry unrelated ownerPlayerId values).
	std::unordered_set<int> _transferredAwaySoldierIds;
	// Counter feeding the unique per-packet transfer id, plus the in-memory
	// duplicate-delivery guard (sufficient now: the host's save is the single
	// authority, so packets are never re-sent across sessions).
	int _transferSendCounter = 0;
	std::unordered_set<long long> _seenTransferPacketIds;
	// Incoming physical transfers received while our SavedGame is swapped out
	// (viewing the peer's base, playerInsideCoopBase). Applying them then
	// would mutate the temporary peer world and be discarded on exit - the
	// soldier would vanish on both machines. Replayed once our world is back.
	std::vector<Json::Value> _pendingIncomingTransfers;
  public:
	// Single-authority model: the HOST's .sav embeds the latest client-world
	// blob (see SavedGame::save/load), so loading a host save atomically
	// restores BOTH players' rosters; the client re-fetches its world from
	// the host on reconnect. To keep the embedded blob fresh, the client
	// silently pushes its progress to the host after every soldier transfer.
	void pushProgressToHostSilently();
	// Clears session transfer state (pending queues, dedup ids, away-ids)
	// after a save load - stale in-memory state must never outlive the save
	// that is now the authority.
	void resetTransferSessionState();
};

}
