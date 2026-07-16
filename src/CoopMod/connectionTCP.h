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
#include <set>
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

// Bounded ring buffer of std::string slots. NOTE: despite the name, this is NOT
// single-producer/single-consumer in this codebase. g_txQ/g_rxQ each have 3+
// producers and 2+ consumers (main thread, network thread, loopData thread, UDP
// thread, plus clearNetworkSessionQueues). Concurrent buf[] std::string moves on
// the same slot double-free the heap (the crash mis-symbolized as SDL_FreeRW). So
// every operation is serialized by an internal mutex that covers the buf[] move,
// not just the cursor. The name is kept to avoid churn at ~40 call sites.
template <size_t N>
struct SPSCQueue
{
	std::array<std::string, N> buf{};
	size_t head{0}; // producer writes (guarded by m)
	size_t tail{0}; // consumer reads  (guarded by m)
	mutable std::mutex m;

	bool push(std::string&& s)
	{
		std::lock_guard<std::mutex> lk(m);
		size_t n = (head + 1) % N;
		if (n == tail)
			return false; // full
		buf[head] = std::move(s);
		head = n;
		return true;
	}

	bool pop(std::string& out)
	{
		std::lock_guard<std::mutex> lk(m);
		if (tail == head)
			return false; // empty
		out = std::move(buf[tail]);
		tail = (tail + 1) % N;
		return true;
	}

	bool empty() const
	{
		std::lock_guard<std::mutex> lk(m);
		return tail == head;
	}

	bool full() const
	{
		std::lock_guard<std::mutex> lk(m);
		return ((head + 1) % N) == tail;
	}
};

namespace OpenXcom
{

// Shared network queues used by both connectionTCP and connectionUDP.
// Definitions must exist exactly once in a .cpp file, normally connectionTCP.cpp:
extern SPSCQueue<1024> g_txQ;
extern SPSCQueue<1024> g_rxQ;
extern int tcp_port;

// Count of packets dropped because the TX queue was full (test harness reads
// this via the coop_stats command to detect the "TX queue full" backlog bug).
extern std::atomic<uint64_t> g_txDropCount;

// ===== Geoscape sync conflation slot =====
// The two GeoscapeState::think() heartbeats are full-state, last-write-wins
// snapshots. Instead of FIFO-queuing every per-frame copy onto g_txQ (which
// overflows on a slow link), each channel keeps a single overwrite slot; the
// send thread emits the freshest one at whatever rate the link drains. Preserves
// update rate (no throttle) while eliminating the backlog.
enum CoopSnapSlot { SNAP_GEO_POSITIONS = 0, SNAP_GEO_TIME = 1, SNAP_COUNT };

// Overwrite the conflation slot with the newest snapshot (thread-safe).
void enqueueSnapshot(CoopSnapSlot slot, std::string&& s);

// True if any conflation slot has an unsent snapshot (send thread wake check).
bool anySnapshotDirty();

// Pop one dirty conflation slot as a raw (unframed) payload, clearing its dirty
// flag; returns false if none pending. Used by the UDP transport, whose datagram
// path sends whole messages (the TCP path uses drainSnapshotsInto, which frames).
bool popSnapshot(std::string& out);

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
class SavedGame;

// ===== Coop session lifecycle state =====

enum class CoopRole { None, Host, Client };

/**
 * Single owner of session-lifecycle state, replacing the scattered statics
 * that produced three flavors of the same missed/mis-ordered-reset bug.
 * Rules:
 *  - exactly TWO reset paths (resetSession / onClientDrop) - never clear
 *    fields ad hoc;
 *  - mutate through the named transitions where one fits, so every lifecycle
 *    change is searchable and logged (PRD-12 S4: the encoding is the mirrored
 *    booleans below; every multi-field or cross-file write funnels through a
 *    named, logged transition - there is no separate phase enum to drift from);
 *  - mutate on the main thread. The network threads keep signaling through
 *    onConnect and the pump/teardown translate. (The thread-side role writes
 *    and the transport-glue mirrors that predate this struct are kept raw and
 *    commented - see the residual list in the PRD-12 commit.)
 */
struct CoopSession
{
	CoopRole role = CoopRole::None;

	// what the lobby is for: 0 = legacy/new-battle (ready dance),
	// 1 = new co-op campaign (START CAMPAIGN), 2 = resuming a co-op save
	int lobbyMode = 0;
	// a client has passed every join gate (roster, password) and is attached;
	// onConnect==1 only means "listening", so this is the real presence signal
	bool clientInLobby = false;
	// players/teams locked (campaign started, resume began, or legacy ready
	// dance completed)
	bool sessionLocked = false;
	// the lobby UI has been dismissed - the session is considered live.
	// Defaults TRUE (historical isLobbyMenuClosed semantics: "no lobby open");
	// LobbyMenu's constructor clears it.
	bool lobbyClosed = true;
	// resume handshake: a client reported its world loaded (CoopState 62/64)
	bool resumeAck = false;
	// battle-save resume is two-phase: geoscape world first, then the battle
	// stream; set while phase two is still owed
	bool resumeBattlePending = false;
	// PRD-11 C8: names of clients that were actually SERVED a resume world blob
	// this resume cycle. Only an eligible acker gets the battle stream; a
	// registered-but-no-blob client (routed through fresh base building) must
	// not. Cleared together with resumeBattlePending.
	std::set<std::string> resumeBattleEligible;
	// set on clients when the host begins/resumes the campaign; releases the
	// "waiting for players" hold (CoopState 65)
	bool campaignBegun = false;
	// host .sav awaiting a re-save once the fresh client blob arrives
	// (stale-embed race fix)
	std::string pendingHostSaveName;

	// --- named transitions (each logs; the log line is the lifecycle trace) ---
	void beginHosting();     // main menu/new game -> hosting a lobby
	void beginJoining();     // client connecting to a host
	void clientAttached();   // a client passed every join gate
	void campaignStarted();  // players/teams locked (START / campaign_start / ready-dance done)
	void sessionLive();      // waiting dialogs released - play begins/resumes
	void freeze();           // a registered player dropped mid-session (D5)
	void setRole(CoopRole r);// main-thread role change (setServerOwner)

	// --- multi-field / cross-file lifecycle writes funnelled here (PRD-12) ---
	void adoptResumeSave();          // a co-op save is loaded for resume (lobbyMode=2, unlock, clear ack)
	void armResumeHandshake(bool hasBattle); // resume/rejoin: clear ack, arm battle phase-two if a battle is loaded
	void markLobbyOpen();            // the lobby UI opened (lobbyClosed=false)
	void markLobbyClosed();          // the lobby UI dismissed (lobbyClosed=true)
	void armDeferredSave(const std::string& name); // host save deferred until the fresh client blob arrives
	void clearDeferredSave();        // deferred host save consumed/cleared
	void signalCampaignBegun();      // host began/resumed: release the client hold (campaignBegun=true)
	void consumeCampaignBegun();     // client consumed the release / cleared a stale one (campaignBegun=false)

	// --- the ONLY reset paths ---
	void resetSession();     // full teardown / back to main menu
	void onClientDrop();     // host side: the campaign/lobby context survives
};

class connectionTCP
{
  private:
	std::thread _loopThread;
	std::thread _clientThread;
	std::thread _hostThread;
	// chat menu
	ChatMenu* _chatMenu = nullptr;
	Game* _game;
	// PRD-J01: process-single Game handle so the static seat accessors can
	// read the active roster (SavedGame::_coopPlayers). Set once in the ctor.
	static Game* _staticGame;
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
	// Send a full-state geoscape snapshot via the conflation slot (last-write-wins,
	// never queued FIFO). slot is a CoopSnapSlot. Used by GeoscapeState::think().
	void sendCoopSnapshot(int slot, std::string data);
	// Reliable geoscape lifecycle: returns true (and updates the tracked set) when
	// the UFO/mission membership in the snapshot changed since the last call. The
	// conflation slot silently drops transient spawns/despawns, so the caller also
	// sends the snapshot on the reliable FIFO lane whenever this returns true. The
	// set rarely changes, so the extra reliable sends do not reintroduce the flood.
	bool geoMembershipChanged(const Json::Value& root);
	std::set<int> _lastGeoUfoIds;
	std::set<int> _lastGeoMissionIds;
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
	// PRD-J01: campaign economy model carried to a joining client during the
	// lobby handshake (before its save exists) so the type label can render.
	// 0 = Separate, 1 = Joint. Mirrors SavedGame::getCampaignType().
	static int _lobbyCampaignType;
	bool getCoopCampaign();
	// PRD-J01: true when the ACTIVE save is a JOINT co-op campaign. Every later
	// JOINT-gated behavior tests this; SEPARATE/solo return false.
	bool isJointCampaign();
	// PRD-J02: true for a JOINT client - a world replica the host streams. A
	// replica never builds its own world, never saves to disk, and never runs
	// the SEPARATE mirror machinery. (isJointCampaign() && !host)
	bool isJointReplica();
	// PRD-J02: serialize the host's authoritative world fresh and hand it to the
	// streamer (single-client resume-blob lane) so the connected client adopts
	// it as its replica. Host only; used at JOINT campaign start and resume.
	void streamJointWorldToClient();
	// Seat = index into SavedGame::_coopPlayers (host = 0). N-player safe.
	static int localSeat();                 // this machine's seat
	static int seatCount();                 // active roster size
	static std::string seatName(int seat);  // player name for a seat
	// no mode = 0, PVE = 1, PVP = 2, PVP2 = 3, PVE2 = 4,
	static int getCoopGamemode();
	void createCoopMenu();
	static void sendTCPPacketStaticData2(std::string data);
	void writeHostMapFile2();
	void writeHostMapFile();
	bool writeHostMapSaveProgressFile();
	void writeHostMapLoadProgressFile();
	// PRD-06: write the armed deferred host save exactly once (embedding the
	// current client blob) and disarm. Used by both the completed round-trip
	// and the wait-dialog CANCEL path. No-op if nothing armed / a battle is live.
	void writePendingHostSave();
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

	// Transport scratch only: peer base/battlescape payloads plus the served
	// client-world blobs for the CURRENT session. Never a permanent save
	// store - the host .sav embed (coopClientSaves) is the durable copy, and
	// SavedGame::load redefines the served set from it on every load. (Same
	// intent as the fixes branch: temp data and permanent saves stay
	// strictly separate.)
	// Stores coop files in a hash map instead of separate files in the host folders
	static std::unordered_map<std::string, std::string> coopFilesHost;
	// Stores coop files in a hash map instead of separate files in the client folders
	static std::unordered_map<std::string, std::string> coopFilesClient;
	// Guards both blob maps: the loopData streamer thread reads them while the
	// main thread stores/erases entries. Hold only around map access; copy the
	// blob out before any long work.
	static std::mutex coopFilesMutex;
	static bool hasCoopFile(const std::string& key);
	// Canonical world-blob keys, scoped by the current saveID:
	// host_<saveID>_<clientName>.data / client_<saveID>_<hostName>.data
	static std::string hostBlobKey(const std::string& clientName);
	static std::string clientBlobKey(const std::string& hostName);
	// Newest stored world blob for a given client, matched by EXACT player-name
	// field across any saveID (the host re-mints saveID on every save, so the
	// stored key's id can lag the current one). Returns nullptr if none. Blob
	// identity comes from the caller's roster, never from reverse-parsing keys.
	// CALLER MUST HOLD coopFilesMutex; the returned pointer is valid only while
	// that lock is held.
	static const std::string* findHostClientBlob(const std::string& clientName);
	// Single authority: may this machine read/write local .sav files right now?
	// Truth table: solo play -> yes; coop host -> yes; coop client -> no.
	// Every local save/load gate and Load/Save button-visibility decision must
	// route through this so the rule lives in one place (PRD-08 tunes the host
	// case later by editing only this function).
	static bool localSavesAllowed();
	// PRD-08 C7: may this machine LOAD a local save RIGHT NOW? False whenever a
	// live coop session is attached (host OR client) - loading mid-session forks
	// the served world silently. True when solo / after the session ends (the
	// lobby resume/rejoin flows are the sanctioned way to change worlds).
	static bool localLoadsAllowed();
	// One authority for the packet that creates/refreshes a client world
	// (fields: state=campaign_start, difficulty, gamemode, saveID, players[]).
	// Built identically by host lobby start, resume-no-blob, and the
	// request_load_progress no-blob fallback.
	static Json::Value buildCampaignStartPacket(const SavedGame* save);
	// STATELESS campaign-context check derived from the live save (a co-op
	// campaign world is loaded). Prefer this over session.lobbyMode for
	// host-side routing decisions: lobby mode is transport-lifecycle state.
	bool inCoopCampaignContext() const;

	// The single owner of session-lifecycle state: see CoopSession above the
	// class. Mutate via its named transitions / the two reset methods.
	static CoopSession session;

	// Reason string from the last lobby_join_refused, shown by the refusal
	// dialog (CoopState 63).
	static std::string joinRefusalReason;

	// save
	static bool saveError;
	static long long saveID;

	// password
	static bool isPasswordRequired;
	static std::string password;

	// lobby menu (legacy ready-dance fields; campaign lobbies don't use them)
	static bool isPlayerReady;
	static bool isPlayersReady;
	static int LobbyFileStatus;
	static int lobby_timer;
	// PRD-11 C13: one-shot signal from the network thread that the host replied
	// "busy" to a request_load_progress. The client's load-wait dialog (CoopState
	// 52) consumes it and schedules a retry.
	static bool loadProgressBusy;
	static bool forceCloseCoopStateMenu;
	static bool forceClosePasswordCheckMenu;

	// other
	static int manuallyAddedServerRemoveID;
	static bool canRemoveManuallyAddedServer;
	static bool isInfoboxClosed;

	// Permanently gifts a soldier to another player (0 = host, 1 = client).
	// Follows the guest-soldier model: a soldier's object lives in its OWNER's
	// save, tagged with coopBase = the station base's coop id when that base
	// belongs to the other player (-1 when stationed at one of the owner's own
	// bases). The soldier is serialized, removed from the giver's save and
	// recreated in the receiver's save, keeping the same station base - so it
	// stays "in" the base it was in, and shows up when the new owner views
	// that base. During battle only the control flags flip immediately; the
	// physical move is queued and runs after the mission ends. Gifts
	// overwrite unconditionally, so soldiers can be gifted back and forth.
	void giftSoldier(Soldier* soldier, int newOwnerId, bool broadcast);
	// Completes queued in-battle gifts once no battle is active. Must run
	// before the post-battle coop cleanup (GeoscapeState calls it first).
	void processPendingSoldierGifts();

  private:
	// Serializes the soldier (with its station base id) and sends the
	// physical-gift packet to the peer.
	void sendSoldierGiftPacket(Soldier* soldier, int newOwnerId);
	// Erases the soldier pointer from every base roster (including the
	// SoldiersState/CraftSoldiersState base_oldsoldiers snapshots).
	void removeSoldierFromLocalBases(Soldier* soldier);
	// In-battle gifts waiting for the mission to end: soldier + new owner.
	std::vector<std::pair<Soldier*, int> > _pendingSoldierGifts;
	// Soldiers gifted away are parked here instead of deleted: UI states
	// (sort snapshots, open dialogs) may still hold pointers to them.
	std::vector<Soldier*> _giftedSoldiers;
	// Ids of soldiers gifted away this session. A stale copy of one of
	// these can resurrect when the pre-visit "basehost" snapshot is restored;
	// the sweep in processPendingSoldierGifts() parks exactly those (and
	// nothing else - legacy saves carry unrelated ownerPlayerId values).
	std::unordered_set<int> _giftedAwaySoldierIds;
	// Counter feeding the unique per-packet gift id, plus the in-memory
	// duplicate-delivery guard (sufficient now: the host's save is the single
	// authority, so packets are never re-sent across sessions).
	int _giftSendCounter = 0;
	std::unordered_set<long long> _seenGiftPacketIds;
	// Incoming physical gifts received while our SavedGame is swapped out
	// (viewing the peer's base, playerInsideCoopBase). Applying them then
	// would mutate the temporary peer world and be discarded on exit - the
	// soldier would vanish on both machines. Replayed once our world is back.
	std::vector<Json::Value> _pendingIncomingGifts;
  public:
	// Single-authority model: the HOST's .sav embeds the latest client-world
	// blob (see SavedGame::save/load), so loading a host save atomically
	// restores BOTH players' rosters; the client re-fetches its world from
	// the host on reconnect. To keep the embedded blob fresh, the client
	// silently pushes its progress to the host after every soldier gift.
	void pushProgressToHostSilently();
	// Fix B (Bug 1): when the client assigns/unassigns its guest soldiers to a
	// host craft via the mirror-base UI, the assignment is written only into the
	// "basehost" blob (the client's copy of the HOST world). The client's OWN
	// world blob (client_<saveID>_<host>.data) - which GeoscapeState reloads at
	// mission end - is never updated, so the guest's CoopCraft reverts to its
	// stale value (unassigned) after a battle. This durably mirrors the
	// per-guest CoopCraft/CoopCraftType into the own-world blob (and pushes it
	// to the host) so the assignment survives the mission-end reload.
	// assignments maps a guest's CoopName to {CoopCraft, CoopCraftType}.
	void syncOwnWorldGuestCraft(int coopBaseId, const std::map<std::string, std::pair<int, std::string>>& assignments);
	// Clears session gift state (pending queues, dedup ids, away-ids)
	// after a save load - stale in-memory state must never outlive the save
	// that is now the authority.
	void resetGiftSessionState();
};

}
