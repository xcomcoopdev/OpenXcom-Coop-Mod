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
#include "TestServer.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <typeinfo>

#include <json/json.h>
#include <SDL_net.h>

#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/State.h"
#include "../Geoscape/GeoscapeState.h"
#include "../Geoscape/GeoscapeCraftState.h"
#include "../Geoscape/GeoscapeEventState.h"
#include "../Geoscape/MonthlyReportState.h"
#include "../Geoscape/MissionDetectedState.h"
#include "../Geoscape/ConfirmLandingState.h"
#include "../Ufopaedia/ArticleState.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BriefingState.h"
#include "../Battlescape/InventoryState.h"
#include "../Battlescape/Inventory.h"
#include "../Battlescape/NextTurnState.h"
#include "../Battlescape/AbortMissionState.h"
#include "../Battlescape/DebriefingState.h"
#include "../Battlescape/Pathfinding.h"
#include "../Battlescape/UnitWalkBState.h"
#include "../Battlescape/UnitTurnBState.h"
#include "../Battlescape/ProjectileFlyBState.h"
#include "../Battlescape/Position.h"
#include "../Savegame/BattleItem.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleSoldier.h"
#include "../Mod/Unit.h"
#include "../Savegame/Base.h"
#include "../Savegame/BaseFacility.h"
#include "../Mod/RuleBaseFacility.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/Country.h"
#include "../Mod/RuleCountry.h"
#include "../Savegame/Craft.h"
#include "../Savegame/GameTime.h"
#include "../Savegame/MissionSite.h"
#include "../Savegame/ResearchProject.h"
#include "../Savegame/Production.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/Transfer.h"
#include "../Savegame/EquipmentLayoutItem.h"
#include "../Savegame/ItemContainer.h"
#include "../Savegame/Tile.h"
#include "../Savegame/Ufo.h"
#include "../Savegame/CraftWeapon.h"
#include "../Savegame/Target.h"
#include "../Savegame/Waypoint.h"
#include "../Savegame/AlienMission.h"
#include "../Mod/UfoTrajectory.h"
#include "../Mod/RuleAlienMission.h"
#include "../Mod/AlienDeployment.h"
#include "../Mod/Mod.h"
#include "../Engine/RNG.h"
#include "../Mod/RuleCraftWeapon.h"
#include "../Mod/RuleCraft.h"
#include "../Mod/RuleResearch.h"
#include "../Mod/RuleManufacture.h"
#include "../Mod/RuleUfo.h"
#include "../Menu/NewGameState.h"
#include "../Menu/LoadGameState.h"
#include "../Menu/SaveGameState.h"
#include "../Menu/StartState.h"
#include "../Menu/MainMenuState.h"
#include "../Geoscape/BuildNewBaseState.h"
#include "../Geoscape/BaseNameState.h"
#include "../Geoscape/ConfirmNewBaseState.h"
#include "../Geoscape/UfoDetectedState.h"
#include "../Geoscape/InterceptState.h"
#include "../Geoscape/ConfirmDestinationState.h"
#include "../Geoscape/DogfightState.h"
#include "LobbyMenu.h"
#include "HostMenu.h"
#include "Profile.h"
#include "connectionTCP.h"
#include "ServerList.h"
#include "PasswordCheckMenu.h"
#include "../Engine/Screen.h"
#include "../Basescape/BasescapeState.h"
#include "../Basescape/BuildFacilitiesState.h"
#include "../Basescape/SoldiersState.h"
#include "../Basescape/SoldierInfoState.h"
#include "../Basescape/CraftSoldiersState.h"
#include "../Basescape/TransferItemsState.h"
#include "../Basescape/PurchaseState.h"
#include "../Basescape/SellState.h"
#include "../Basescape/ManageAlienContainmentState.h"
#include "../Basescape/ResearchState.h"
#include "../Basescape/NewResearchListState.h"
#include "../Basescape/ResearchInfoState.h"
#include "../Basescape/StoresState.h"
#include "../Basescape/ManufactureState.h"
#include "../Basescape/ManufactureInfoState.h"
#include "../Basescape/PlaceFacilityState.h"
#include "../Basescape/DismantleFacilityState.h"
#include "../Basescape/SackSoldierState.h"
#include "../Basescape/PlaceLiftState.h"
#include "../Basescape/CraftEquipmentState.h"
#include "../Basescape/CraftWeaponsState.h"
#include "../Basescape/SoldierArmorState.h"
#include "../Basescape/CraftArmorState.h"
#include "../Mod/Armor.h"
#include "JointEcon.h"
#include "CoopState.h"
#include "GiftNoticeState.h"
#include "GiftSoldierMenu.h"
#include "../Interface/DisableableComboBox.h"

namespace OpenXcom
{

namespace {
// PRD-13 S6: the state-stack scan `for (auto* s : game->getStates()) if (auto*
// t = dynamic_cast<T*>(s)) found = t;` was pasted ~20x. These file-local helpers
// replace the pure find-last and top-only variants. Loops with extra
// per-iteration logic (early break, multi-type, counting) are left inline.

/// Last (topmost) instance of T on the state stack, or nullptr.
template <class T> T* findState(Game* game)
{
	T* found = nullptr;
	for (auto* s : game->getStates())
		if (auto* t = dynamic_cast<T*>(s)) found = t;
	return found;
}

/// T only if it is the current top state, else nullptr.
template <class T> T* topState(Game* game)
{
	return game->getStates().empty() ? nullptr
		: dynamic_cast<T*>(game->getStates().back());
}
}

TestServer& TestServer::instance()
{
	static TestServer s;
	return s;
}

void TestServer::startFromEnvironment(Game* game)
{
	if (_running.load())
	{
		return;
	}
	const char* portStr = std::getenv("OXC_TEST_PORT");
	if (!portStr)
	{
		return;
	}
	int port = std::atoi(portStr);
	if (port <= 0 || port > 65535)
	{
		return;
	}
	_game = game;
	_running.store(true);
	_thread = std::thread(&TestServer::ioThread, this, port);
	Log(LOG_INFO) << "[testserver] listening on 127.0.0.1:" << port;
}

void TestServer::stop()
{
	_running.store(false);
	if (_thread.joinable())
	{
		_thread.join();
	}
}

void TestServer::ioThread(int port)
{
	if (SDLNet_Init() != 0)
	{
		Log(LOG_ERROR) << "[testserver] SDLNet_Init failed: " << SDLNet_GetError();
		return;
	}
	IPaddress ip;
	// NULL host = listen (SDL_net semantics; a concrete address would mean
	// an outbound connect). Test-only server, gated by OXC_TEST_PORT.
	if (SDLNet_ResolveHost(&ip, nullptr, (Uint16)port) != 0)
	{
		Log(LOG_ERROR) << "[testserver] resolve failed: " << SDLNet_GetError();
		return;
	}
	TCPsocket listening = SDLNet_TCP_Open(&ip);
	if (!listening)
	{
		Log(LOG_ERROR) << "[testserver] open failed: " << SDLNet_GetError();
		return;
	}
	SDLNet_SocketSet set = SDLNet_AllocSocketSet(2);
	SDLNet_TCP_AddSocket(set, listening);

	TCPsocket client = nullptr;
	std::string recvBuf;

	while (_running.load())
	{
		SDLNet_CheckSockets(set, 50);

		if (TCPsocket fresh = SDLNet_TCP_Accept(listening))
		{
			if (!client)
			{
				client = fresh;
				SDLNet_TCP_AddSocket(set, client);
			}
			else
			{
				SDLNet_TCP_Close(fresh);
			}
		}

		if (client && SDLNet_SocketReady(client))
		{
			char buf[4096];
			int n = SDLNet_TCP_Recv(client, buf, sizeof(buf));
			if (n <= 0)
			{
				SDLNet_TCP_DelSocket(set, client);
				SDLNet_TCP_Close(client);
				client = nullptr;
				recvBuf.clear();
			}
			else
			{
				recvBuf.append(buf, n);
				size_t pos;
				while ((pos = recvBuf.find('\n')) != std::string::npos)
				{
					std::string line = recvBuf.substr(0, pos);
					recvBuf.erase(0, pos + 1);
					if (!line.empty() && line.back() == '\r')
					{
						line.pop_back();
					}
					if (!line.empty())
					{
						std::lock_guard<std::mutex> lock(_mutex);
						_inbox.push_back(line);
					}
				}
			}
		}

		// Flush responses.
		if (client)
		{
			std::deque<std::string> out;
			{
				std::lock_guard<std::mutex> lock(_mutex);
				out.swap(_outbox);
			}
			for (auto& resp : out)
			{
				resp += '\n';
				int sent = 0;
				int len = (int)resp.size();
				while (sent < len)
				{
					int n = SDLNet_TCP_Send(client, resp.data() + sent, len - sent);
					if (n <= 0)
					{
						break;
					}
					sent += n;
				}
			}
		}
	}

	if (client)
	{
		SDLNet_TCP_Close(client);
	}
	SDLNet_TCP_Close(listening);
	SDLNet_FreeSocketSet(set);
}

void TestServer::pump()
{
	if (!_running.load())
	{
		return;
	}
	// While StartState is on the stack the mod is still being loaded on its
	// worker thread; executing commands now races it (e.g. GeoscapeState
	// needs surfaces that modResources() synthesizes at the very end of the
	// load). Leave commands queued until loading finishes.
	// PRD-13: left inline - returns on first match (presence check / early exit), not find-last
	for (auto* s : _game->getStates())
	{
		if (dynamic_cast<StartState*>(s))
		{
			return;
		}
	}
	for (;;)
	{
		std::string line;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_inbox.empty())
			{
				break;
			}
			line = std::move(_inbox.front());
			_inbox.pop_front();
		}
		std::string resp;
		try
		{
			resp = execute(line);
		}
		catch (const std::exception& e)
		{
			Json::Value err;
			err["ok"] = false;
			err["error"] = std::string("exception: ") + e.what();
			Json::FastWriter w;
			resp = w.write(err);
			if (!resp.empty() && resp.back() == '\n') resp.pop_back();
		}
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_outbox.push_back(resp);
		}
	}
}

/**
 * PRD-J11: the rule name a pending Transfer carries, for the geo_state dump. Items
 * and crafts name their rule; a soldier transfer names its RuleSoldier type (the
 * key JointEcon's transferArrivedApply matches on); scientists/engineers are
 * anonymous headcount and have none.
 */
static std::string transferRuleName(Transfer* t)
{
	switch (t->getType())
	{
	case TRANSFER_ITEM:    return t->getItems() ? t->getItems()->getType() : "";
	case TRANSFER_CRAFT:   return t->getCraft() ? t->getCraft()->getRules()->getType() : "";
	case TRANSFER_SOLDIER: return t->getSoldier() ? t->getSoldier()->getRules()->getType() : "";
	default:               return "";
	}
}

// PRD-DF03: resolve the (dogfight_action / award_dogfight_xp) target - the specific
// (craft,ufo) fight when a craft_id/ufo_id is given (the 4-concurrent test drives
// distinct fights), else the first live fight. A free helper so the deep execute()
// if-chain (already at the MSVC C1061 nesting limit) stays flat.
static DogfightState* resolveDogfight(GeoscapeState* geo, int craftId, int ufoId)
{
	if (!geo || geo->getDogfights().empty()) return nullptr;
	if (craftId < 0 && ufoId < 0) return geo->getDogfights().front();
	for (auto* d : geo->getDogfights())
		if ((craftId < 0 || (d->getCraft() && d->getCraft()->getId() == craftId))
			&& (ufoId < 0 || (d->getUfo() && d->getUfo()->getId() == ufoId)))
			return d;
	return nullptr;
}

// PRD-DF03: dogfight_action body, extracted so the deep execute() if-chain (MSVC C1061
// nesting limit) does not carry the stance/weapon dispatch. On a replica the raw Press
// handlers emit df_cmd (+ optimistic echo) regardless of window visibility; on the host
// the Simulate*LeftPress lane. Targets a specific (craft,ufo) fight when given, else front().
static void doDogfightAction(GeoscapeState* geo, const Json::Value& req, Json::Value& resp)
{
	std::string action = req.get("action", "aggressive").asString();
	if (!geo) { resp["error"] = "no geoscape"; return; }
	if (geo->getDogfights().empty()) { resp["error"] = "no dogfight"; return; }
	DogfightState* df = resolveDogfight(geo, req.get("craft_id", -1).asInt(),
	                                    req.get("ufo_id", -1).asInt());
	if (!df) { resp["error"] = "no matching dogfight"; return; }
	int warg = req.get("arg", 0).asInt();
	SDL_Event ev;
	ev.type = SDL_MOUSEBUTTONDOWN;
	ev.button.button = SDL_BUTTON_LEFT;
	Action a = Action(&ev, 0.0, 0.0, 0, 0);
	if (df->isReplicaView())
	{
		if (action == "aggressive") df->btnAggressivePress(&a);
		else if (action == "standard") df->btnStandardPress(&a);
		else if (action == "cautious") df->btnCautiousPress(&a);
		else if (action == "standoff") df->btnStandoffPress(&a);
		else if (action == "disengage") df->btnDisengagePress(&a);
		else if (action == "minimize") df->setMinimized(true);
		else if (action == "weaponToggle") df->harnessToggleWeapon(warg);
		else resp["error"] = "unknown action: " + action;
	}
	else if (action == "aggressive") df->btnAggressiveSimulateLeftPress(&a);
	else if (action == "standard") df->btnStandardSimulateLeftPress(&a);
	else if (action == "cautious") df->btnCautiousSimulateLeftPress(&a);
	else if (action == "standoff") df->btnStandoffSimulateLeftPress(&a);
	else if (action == "disengage") df->btnDisengageSimulateLeftPress(&a);
	else if (action == "minimize") df->setMinimized(true);
	else if (action == "weaponToggle") df->harnessToggleWeapon(warg);
	else resp["error"] = "unknown action: " + action;
	if (!resp.isMember("error")) resp["ok"] = true;
}

static Json::Value soldierToJson(Soldier* s)
{
	Json::Value j;
	j["id"] = s->getId();
	j["name"] = s->getName();
	j["owner"] = s->getOwnerPlayerId();
	j["coop"] = s->getCoop();
	j["coopBase"] = s->getCoopBase();
	j["craft"] = s->getCraft() ? s->getCraft()->getType() : "";
	j["craftId"] = s->getCraft() ? s->getCraft()->getId() : -1; // PRD-J09
	j["armor"] = s->getArmor()->getType(); // PRD-J09 GAP-5b
	j["dead"] = s->getDeath() != nullptr;
	// PRD-DF03 GAP-7: pilot stats + the daily dogfight-XP accumulator, so a test can
	// assert dogfight XP is host-authoritative (equal on both machines, on the
	// authoritative host Soldier) and never accrues replica-locally.
	j["firing"] = s->getCurrentStats()->firing;
	j["reactions"] = s->getCurrentStats()->reactions;
	j["bravery"] = s->getCurrentStats()->bravery;
	j["dogfightXp"] = s->getDailyDogfightExperienceCache()->firing;
	return j;
}

/**
 * PRD-J10 test hooks, in their own dispatcher. The execute() command chain below
 * sits on MSVC's 128-block nesting limit (C1061: "blocks nested too deeply"), so
 * new commands go here rather than deepening it. Returns true if @a cmd was one of
 * ours (and @a resp was filled).
 */
bool TestServer::executeJoint10(const std::string& cmd, const Json::Value& req, Json::Value& resp)
{
	connectionTCP* coop = _game->getCoopMod();

	if (cmd == "joint_resync_stats")
	{
		// PRD-J10: auto-resync bookkeeping (replica: mismatches seen + repairs
		// asked for; host: repairs served).
		JointEcon::ResyncStats rs = JointEcon::resyncStats();
		resp["mismatches"] = Json::Value::UInt64(rs.mismatches);
		resp["requests"] = Json::Value::UInt64(rs.requests);
		resp["pending"] = rs.pending;
		resp["gaveUp"] = rs.gaveUp;
		resp["lastGameMin"] = Json::Value::Int64(rs.lastGameMin);
		resp["ok"] = true;
	}
	else if (cmd == "joint_reset_resync_stats")
	{
		JointEcon::resetResyncStats();
		resp["ok"] = true;
	}
	else if (cmd == "joint_checksum")
	{
		// PRD-J11: this machine's world checksum, exactly as the host stamps it onto
		// the geoscape `time` heartbeat (chkFunds / chkBases / chkResearch plus the
		// GAP-4 chkItems / chkSoldiers / chkTransfers / chkProduction counts). Lets a
		// test read what the desync detector compares WITHOUT waiting for a
		// heartbeat, and makes the checksum's coverage visible to the suite.
		JointEcon::attachWorldChecksum(_game, resp);
		resp["ok"] = true;
	}
	else if (cmd == "force_resync")
	{
		// PRD-J10 debug hook: force the desync repair without waiting for a
		// checksum mismatch. On a REPLICA it sends joint_resync_request past the
		// throttle; on the HOST it pushes the authoritative world down the same
		// lane the request would have triggered.
		if (!coop->isJointCampaign())
			resp["error"] = "not a JOINT campaign";
		else if (connectionTCP::getServerOwner())
		{
			coop->jointResyncStream();
			resp["role"] = "host";
			resp["ok"] = true;
		}
		else
		{
			resp["role"] = "replica";
			resp["sent"] = JointEcon::requestResync(_game, "harness force_resync", true);
			resp["ok"] = true;
		}
	}
	else if (cmd == "open_screen")
	{
		// PRD-J10: push a base screen and LEAVE IT OPEN (unlike buy/sell, which
		// drive the OK handler and pop). This is how the refresh tests put a real
		// screen in front of an incoming joint_apply. <base> optional (default:
		// first real base); <craft_id> for craft_soldiers.
		std::string screen = req.get("screen", "").asString();
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (_game->getSavedGame())
			for (auto* base : *_game->getSavedGame()->getBases())
				if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
				                     : base->getName() == baseName)
				{ target = base; break; }

		if (!target)
			resp["error"] = "base not found";
		else if (screen == "purchase")
		{
			_game->pushState(new PurchaseState(target));
			resp["ok"] = true;
		}
		else if (screen == "sell")
		{
			SellState* ss = new SellState(target, nullptr, OPT_GEOSCAPE);
			_game->pushState(ss);
			ss->delayedInit(); // SellState builds its rows lazily, not in the ctor
			resp["ok"] = true;
		}
		else if (screen == "research")
		{
			_game->pushState(new ResearchState(target));
			resp["ok"] = true;
		}
		else if (screen == "new_research")
		{
			// Playtest B2: the "NEW RESEARCH PROJECTS" startable-topic list.
			_game->pushState(new NewResearchListState(target, false));
			resp["ok"] = true;
		}
		else if (screen == "manufacture")
		{
			_game->pushState(new ManufactureState(target));
			resp["ok"] = true;
		}
		else if (screen == "stores")
		{
			_game->pushState(new StoresState(target));
			resp["ok"] = true;
		}
		else if (screen == "soldiers")
		{
			_game->pushState(new SoldiersState(target));
			resp["ok"] = true;
		}
		else if (screen == "basescape")
		{
			// Playtest B1: the base management screen (funds header + facility grid).
			_game->pushState(new BasescapeState(target, nullptr));
			resp["ok"] = true;
		}
		else if (screen == "build_facilities")
		{
			// Playtest B1: the small "build facilities" popup, ON TOP of a BasescapeState
			// (so the funds/grid behind it are what a live joint_apply must refresh).
			BasescapeState* bs = new BasescapeState(target, nullptr);
			_game->pushState(bs);
			_game->pushState(new BuildFacilitiesState(target, bs));
			resp["ok"] = true;
		}
		else if (screen == "craft_soldiers")
		{
			int craftId = req.get("craft_id", -1).asInt();
			size_t idx = target->getCrafts()->size();
			for (size_t i = 0; i < target->getCrafts()->size(); ++i)
				if (target->getCrafts()->at(i)->getId() == craftId) { idx = i; break; }
			if (idx >= target->getCrafts()->size())
				resp["error"] = "craft id not at base";
			else
			{
				_game->pushState(new CraftSoldiersState(target, idx));
				resp["ok"] = true;
			}
		}
		else
		{
			resp["error"] = "unknown screen: " + screen;
		}
	}
	else if (cmd == "joint_landing_state")
	{
		// PRD-J10 landing broker introspection (HOST): is a landing decision
		// outstanding with another seat? Non-zero means the host brokered the
		// prompt instead of popping its own dialog.
		GeoscapeState* gs = findState<GeoscapeState>(_game);
		if (!gs)
			resp["error"] = "no GeoscapeState";
		else
		{
			resp["pending"] = gs->hasJointLandingPending();
			resp["ok"] = true;
		}
	}
	else if (cmd == "decline_landing")
	{
		// PRD-J10: the NO button of a ConfirmLandingState (brokered or not).
		ConfirmLandingState* cl = findState<ConfirmLandingState>(_game);
		if (!cl)
			resp["error"] = "no ConfirmLandingState on stack";
		else
		{
			cl->btnNoClick(nullptr);
			resp["ok"] = true;
		}
	}
	else if (cmd == "close_screens")
	{
		// PRD-J10: pop everything open_screen pushed, back down to the GeoscapeState,
		// so a test can leave the screens and drive time again. Bounded, so a
		// self-repushing dialog cannot spin the pump forever.
		int popped = 0;
		while (popped < 16 && _game->getStates().size() > 1
			&& !dynamic_cast<GeoscapeState*>(_game->getStates().back()))
		{
			_game->popState();
			++popped;
		}
		resp["popped"] = popped;
		resp["ok"] = true;
	}
	else if (cmd == "screen_state")
	{
		// PRD-J10: introspect the TOP state's constructor-time caches. Those only
		// change when the screen is rebuilt, so they are what proves live refresh
		// happened (rather than the world merely being correct underneath).
		State* top = _game->getStates().empty() ? nullptr : _game->getStates().back();
		resp["top"] = "other";
		if (!top)
		{
			resp["top"] = "none";
		}
		else if (auto* ps = dynamic_cast<PurchaseState*>(top))
		{
			resp["top"] = "purchase";
			resp["funds"] = ps->harnessFundsText();
			std::string item = req.get("item", "").asString();
			if (!item.empty()) resp["stock"] = ps->harnessRowStock(item);
		}
		else if (dynamic_cast<SellState*>(top))        resp["top"] = "sell";
		else if (auto* cs = dynamic_cast<CraftSoldiersState*>(top))
		{
			resp["top"] = "craft_soldiers";
			resp["used"] = cs->harnessUsedText();
		}
		else if (auto* nr = dynamic_cast<NewResearchListState*>(top))
		{
			// Playtest B2: the startable-topic list is the constructor/refresh cache;
			// report it so a test can prove a started topic dropped live.
			resp["top"] = "new_research";
			Json::Value arr(Json::arrayValue);
			for (const auto& n : nr->harnessProjectNames()) arr.append(n);
			resp["projects"] = arr;
		}
		else if (dynamic_cast<ResearchState*>(top))    resp["top"] = "research";
		else if (dynamic_cast<ManufactureState*>(top)) resp["top"] = "manufacture";
		else if (dynamic_cast<StoresState*>(top))      resp["top"] = "stores";
		else if (auto* ss = dynamic_cast<SoldiersState*>(top))
		{
			// Playtest: what the soldier-list SCREEN actually displays. In JOINT each
			// player should see only their own half of the shared roster.
			resp["top"] = "soldiers";
			Json::Value arr(Json::arrayValue);
			for (int id : ss->harnessDisplayedSoldierIds()) arr.append(id);
			resp["displayed"] = arr;
		}
		else if (dynamic_cast<BuildFacilitiesState*>(top))
		{
			// Playtest B1: the popup itself has no funds label - the stale/refreshed
			// header lives on the BasescapeState it covers. Dig for it and report its
			// cached funds string (proves the covered screen rebuilt under the popup).
			resp["top"] = "build_facilities";
			for (auto it = _game->getStates().rbegin(); it != _game->getStates().rend(); ++it)
				if (auto* bs = dynamic_cast<BasescapeState*>(*it))
				{ resp["funds"] = bs->harnessFundsText(); break; }
		}
		else if (auto* bs = dynamic_cast<BasescapeState*>(top))
		{
			resp["top"] = "basescape";
			resp["funds"] = bs->harnessFundsText();
		}
		else if (dynamic_cast<GeoscapeState*>(top))    resp["top"] = "geoscape";
		else if (auto* cd = dynamic_cast<CoopState*>(top))
		{
			resp["top"] = "coop";
			resp["dialog"] = cd->getStateCode();
			resp["title"] = cd->getTitleText();
		}
		resp["ok"] = true;
	}
	else if (cmd == "trigger_base_defense")
	{
		// PRD-J09 GAP-1 repro: drive the coop base-defense entry headlessly.
		// Base defense is a SEPARATE geoscape entry point from the craft-landing
		// flow (GeoscapeState::handleBaseDefense, NOT ConfirmLandingState), so it
		// needs its own hook. Seeds the given UFO (from a prior spawn_ufo) over the
		// first real base and calls the REAL handler, which in JOINT stamps the
		// ownership control split and ships "battlehost" exactly as a live alien
		// retaliation strike would. Host-only (the replica's sim never reaches it).
		GeoscapeState* gs = findState<GeoscapeState>(_game);
		SavedGame* sg = _game->getSavedGame();
		int ufoId = req.get("ufo_id", -1).asInt();
		Base* base = nullptr;
		Ufo* ufo = nullptr;
		if (sg)
		{
			for (auto* b : *sg->getBases())
				if (!b->_coopBase && !b->_coopIcon) { base = b; break; }
			for (auto* u : *sg->getUfos())
				if (u->getId() == ufoId) { ufo = u; break; }
		}
		if (!gs) resp["error"] = "no GeoscapeState";
		else if (!base) resp["error"] = "no own base";
		else if (!ufo) resp["error"] = "ufo id not found";
		else
		{
			// Put the UFO over the base and mark it a non-coop (host-side) strike so
			// handleBaseDefense's coop guard (ufo->getCoop() == false) passes.
			ufo->setLongitude(base->getLongitude());
			ufo->setLatitude(base->getLatitude());
			ufo->setCoop(false);
			ufo->setDetected(true);
			gs->handleBaseDefense(base, ufo);
			resp["base"] = base->getName();
			resp["ok"] = true;
		}
	}
	else if (cmd == "set_ufo_hunt")
	{
		// GAP-2 repro: turn an existing UFO into a hunter-killer actively HUNTING
		// a specific x-craft (isHunterKiller + isHunting, dest = craft, hunt speed).
		// Drives the UFO-INITIATED attack lane (GeoscapeState::time5Seconds, the
		// UFO loop's ufo->reachedDestination() && ufo->isHunting() branch) - distinct
		// from a craft chasing a UFO. Lives in this (shallow) dispatcher, not the
		// main if-chain, which is at the MSVC C1061 nesting limit. Params: ufo_id,
		// craft_id (+ optional craft_type).
		SavedGame* sg = _game->getSavedGame();
		int uid = req.get("ufo_id", -1).asInt();
		int cid = req.get("craft_id", -1).asInt();
		std::string craftType = req.get("craft_type", "").asString();
		Ufo* ufo = nullptr;
		Craft* craft = nullptr;
		if (sg)
		{
			for (auto* u : *sg->getUfos())
				if (u->getId() == uid) { ufo = u; break; }
			for (auto* b : *sg->getBases())
			{
				for (auto* c : *b->getCrafts())
					if (!c->coop && c->getId() == cid
						&& (craftType.empty() || c->getRules()->getType() == craftType))
					{ craft = c; break; }
				if (craft) break;
			}
		}
		if (!sg) resp["error"] = "no saved game";
		else if (!ufo) resp["error"] = "no matching ufo";
		else if (!craft) resp["error"] = "no matching craft";
		else
		{
			ufo->setHunterKiller(true);
			ufo->setTargetedXcomCraft(craft); // dest=craft, _isHunting=true, hunt speed
			resp["isHunterKiller"] = ufo->isHunterKiller();
			resp["isHunting"] = ufo->isHunting();
			resp["ok"] = true;
		}
	}
	else if (cmd == "craft_equip")
	{
		// PRD-J09 GAP-5 repro/driver: move <count> of item <item> onto (count>0)
		// or off (count<0) the base's craft, through the REAL CraftEquipmentState
		// store path a player uses. In JOINT (after the fix) this routes a
		// craft_equip joint_cmd host-side; before it, it mutates THIS machine's
		// base stores locally - the pre-battle store drift GAP-5 closes. Lives in
		// this (shallow) dispatcher, not the main if-chain at the C1061 limit.
		// Params: item, count, optional base + craft_id (default: first craft).
		std::string item = req.get("item", "").asString();
		int count = req.get("count", 0).asInt();
		std::string baseName = req.get("base", "").asString();
		int craftId = req.get("craft_id", -1).asInt();
		Base* target = nullptr;
		if (_game->getSavedGame())
			for (auto* b : *_game->getSavedGame()->getBases())
				if (baseName.empty() ? (b->_coopBase == false && b->_coopIcon == false)
				                     : b->getName() == baseName)
				{ target = b; break; }
		size_t idx = target ? target->getCrafts()->size() : 0;
		if (target)
			for (size_t i = 0; i < target->getCrafts()->size(); ++i)
				if (craftId < 0 || target->getCrafts()->at(i)->getId() == craftId)
				{ idx = i; break; }
		if (!target)
			resp["error"] = "base not found";
		else if (idx >= target->getCrafts()->size())
			resp["error"] = "craft not found";
		else
		{
			CraftEquipmentState* ces = new CraftEquipmentState(target, idx);
			_game->pushState(ces);
			bool moved = ces->harnessMove(item, count);
			_game->popState();
			resp["moved"] = moved;
			resp["ok"] = moved;
			if (!moved) resp["error"] = "item not on craft equipment list: " + item;
		}
	}
	else if (cmd == "craft_rearm")
	{
		// PRD-J09 GAP-5b repro/driver: mount craft-weapon <weapon> ("" = None) in
		// weapon <slot> of the base's craft, through the REAL CraftWeaponsState
		// store path a player uses (arm/rearm moves the launcher + clips against
		// the shared base stores). In JOINT (after the fix) this routes a
		// craft_rearm joint_cmd host-side; before it, it mutates THIS machine's
		// base stores locally - the pre-battle store drift GAP-5b closes.
		// Params: weapon, slot (default 0), optional base + craft_id.
		std::string weapon = req.get("weapon", "").asString();
		int slot = req.get("slot", 0).asInt();
		std::string baseName = req.get("base", "").asString();
		int craftId = req.get("craft_id", -1).asInt();
		Base* target = nullptr;
		if (_game->getSavedGame())
			for (auto* b : *_game->getSavedGame()->getBases())
				if (baseName.empty() ? (b->_coopBase == false && b->_coopIcon == false)
				                     : b->getName() == baseName)
				{ target = b; break; }
		size_t idx = target ? target->getCrafts()->size() : 0;
		if (target)
			for (size_t i = 0; i < target->getCrafts()->size(); ++i)
				if (craftId < 0 || target->getCrafts()->at(i)->getId() == craftId)
				{ idx = i; break; }
		if (!target)
			resp["error"] = "base not found";
		else if (idx >= target->getCrafts()->size())
			resp["error"] = "craft not found";
		else
		{
			CraftWeaponsState* cws = new CraftWeaponsState(target, idx, (size_t)slot);
			bool moved = cws->harnessEquip(weapon);
			delete cws;
			resp["moved"] = moved;
			resp["ok"] = moved;
			if (!moved) resp["error"] = "weapon not on craft armament list: " + weapon;
		}
	}
	else if (cmd == "soldier_armor")
	{
		// PRD-J09 GAP-5b repro/driver: set soldier <soldier_id>'s armor to <armor>
		// through the REAL SoldierArmorState store path (returns the old armor's
		// store item, consumes the new one). In JOINT (after the fix) this routes a
		// soldier_armor joint_cmd; before it, it mutates THIS machine's base stores.
		// Params: soldier_id, armor, optional base.
		std::string armor = req.get("armor", "").asString();
		int soldierId = req.get("soldier_id", -1).asInt();
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (_game->getSavedGame())
			for (auto* b : *_game->getSavedGame()->getBases())
				if (baseName.empty() ? (b->_coopBase == false && b->_coopIcon == false)
				                     : b->getName() == baseName)
				{ target = b; break; }
		size_t sidx = target ? target->getSoldiers()->size() : 0;
		if (target)
			for (size_t i = 0; i < target->getSoldiers()->size(); ++i)
				if (soldierId < 0 || target->getSoldiers()->at(i)->getId() == soldierId)
				{ sidx = i; break; }
		if (!target)
			resp["error"] = "base not found";
		else if (sidx >= target->getSoldiers()->size())
			resp["error"] = "soldier not found";
		else
		{
			SoldierArmorState* sas = new SoldierArmorState(target, sidx, SA_GEOSCAPE);
			bool moved = sas->harnessSetArmor(armor);
			delete sas;
			resp["moved"] = moved;
			resp["ok"] = moved;
			if (!moved) resp["error"] = "armor not on soldier list: " + armor;
		}
	}
	else if (cmd == "craft_deequip_armor")
	{
		// PRD-J09 GAP-5b repro/driver: de-equip ALL base soldiers to their default
		// armor through the REAL CraftArmorState path (btnDeequipAllArmorClick),
		// which returns each replaced armor's store item to the shared base stores.
		// In JOINT (after the fix) this routes one soldier_armor joint_cmd per
		// soldier; before it, it mutates THIS machine's base stores locally.
		// Params: optional base (any craft on the base is used as the screen anchor).
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (_game->getSavedGame())
			for (auto* b : *_game->getSavedGame()->getBases())
				if (baseName.empty() ? (b->_coopBase == false && b->_coopIcon == false)
				                     : b->getName() == baseName)
				{ target = b; break; }
		if (!target)
			resp["error"] = "base not found";
		else if (target->getCrafts()->empty())
			resp["error"] = "base has no craft";
		else
		{
			CraftArmorState* cas = new CraftArmorState(target, 0);
			cas->harnessDeequipAll();
			delete cas;
			resp["ok"] = true;
		}
	}
	else if (cmd == "seed_soldier_armor")
	{
		// PRD-J09 GAP-5b test scaffolding (NOT a drift source): set soldier
		// <soldier_id>'s armor DIRECTLY on THIS machine, no store change, no route.
		// Deterministic - call on host AND client identically to seed an equal
		// shared world (a soldier wearing a store-item armor) before a de-equip
		// drift test. Mirrors give_items' seed-on-both idiom.
		std::string armor = req.get("armor", "").asString();
		int soldierId = req.get("soldier_id", -1).asInt();
		Armor* rule = _game->getMod()->getArmor(armor, false);
		Soldier* found = nullptr;
		if (_game->getSavedGame())
			for (auto* b : *_game->getSavedGame()->getBases())
				for (auto* s : *b->getSoldiers())
					if (s->getId() == soldierId) { found = s; break; }
		if (!rule)
			resp["error"] = "unknown armor: " + armor;
		else if (!found)
			resp["error"] = "soldier not found";
		else
		{
			found->setArmor(rule, true);
			resp["armor"] = found->getArmor()->getType();
			resp["ok"] = true;
		}
	}
	else if (cmd == "assign_crew")
	{
		// PRD-DF03 GAP-7 scaffolding: directly seat soldier <soldier_id> on craft
		// <craft_id> on THIS machine (Soldier::setCraft), no JOINT route - call on BOTH
		// machines identically (the spawn_craft idiom) to seat a pilot lock-step for the
		// dogfight-XP test. Keeps the shared world equal (same soldier on the same craft id).
		SavedGame* sg = _game->getSavedGame();
		int sid = req.get("soldier_id", -1).asInt();
		int cid = req.get("craft_id", -1).asInt();
		std::string ctype = req.get("craft_type", "").asString();
		Craft* craft = nullptr;
		Base* base = nullptr;
		if (sg)
			for (auto* b : *sg->getBases())
			{
				for (auto* c : *b->getCrafts())
					if (!c->coop && c->getId() == cid
						&& (ctype.empty() || c->getRules()->getType() == ctype))
					{ craft = c; base = b; break; }
				if (craft) break;
			}
		Soldier* sol = nullptr;
		if (base)
			for (auto* s : *base->getSoldiers())
				if (s->getId() == sid) { sol = s; break; }
		if (!craft) resp["error"] = "craft not found";
		else if (!sol) resp["error"] = "soldier not on craft base";
		else { sol->setCraft(craft); resp["craftId"] = sol->getCraft()->getId(); resp["ok"] = true; }
	}
	else if (cmd == "award_dogfight_xp")
	{
		// PRD-DF03 GAP-7 (HOST): emulate the host awarding dogfight XP to a live fight's
		// crew. The vanilla harness ruleset defines no craft pilots and no dogfight-
		// Experience, so the real award RNG path yields zero; this applies the SAME
		// authoritative mutation host-side (DogfightState::harnessAwardPilotXp), then the
		// test force_resyncs and asserts the XP rode the roster stream onto the host
		// Soldier. HOST-only: a replica must never award (update() early-returns). Params:
		// craft_id, ufo_id (optional; default first fight), firing/reactions/bravery deltas.
		if (!connectionTCP::getServerOwner())
			resp["error"] = "award_dogfight_xp is host-only (replicas never award XP)";
		else
		{
			GeoscapeState* geo = findState<GeoscapeState>(_game);
			int cid = req.get("craft_id", -1).asInt();
			int uid = req.get("ufo_id", -1).asInt();
			int fd = req.get("firing", 1).asInt();
			int rd = req.get("reactions", 0).asInt();
			int bd = req.get("bravery", 0).asInt();
			DogfightState* df = resolveDogfight(geo, cid, uid);
			if (!geo) resp["error"] = "no geoscape";
			else if (!df) resp["error"] = "no matching dogfight";
			else
			{
				std::vector<int> ids = df->harnessAwardPilotXp(fd, rd, bd);
				Json::Value arr(Json::arrayValue);
				for (int id : ids) arr.append(id);
				resp["pilots"] = arr;
				resp["ok"] = true;
			}
		}
	}
	else
	{
		return false;
	}
	return true;
}

/**
 * PRD-J10/J11 test hooks, second sub-dispatcher. execute()'s command chain hit
 * MSVC's C1061 nested-block limit again after the upstream rebase stacked more
 * commands onto it, so the back half of the chain lives here. Returns true if
 * @a cmd was one of ours (and @a resp was filled).
 */
bool TestServer::executeJoint11(const std::string& cmd, const Json::Value& req, Json::Value& resp)
{
	connectionTCP* coop = _game->getCoopMod();

	if (cmd == "build_new_base")
	{
		// PRD-J07: drive the FULL subsequent-base flow end-to-end through the
		// real states: BuildNewBaseState (globe pick at <lon>,<lat>) ->
		// ConfirmNewBaseState (cost gate; JOINT debits nothing locally) ->
		// BaseNameState (<name>) -> PlaceLiftState (lift at <liftX>,<liftY>).
		// In JOINT the lift click submits ONE base_new joint_cmd; the base
		// materializes on both machines via joint_apply. Response carries the
		// region base <cost> so the test can assert the exact single debit.
		double lon = req.get("lon", 0.0).asDouble();
		double lat = req.get("lat", 0.0).asDouble();
		std::string name = req.get("name", "Joint Base").asString();
		int liftX = req.get("liftX", 2).asInt();
		int liftY = req.get("liftY", 2).asInt();
		GeoscapeState* gs = findState<GeoscapeState>(_game);
		if (!gs || !_game->getSavedGame())
			resp["error"] = "no geoscape";
		else
		{
			Base* b = new Base(_game->getMod());
			BuildNewBaseState* build = new BuildNewBaseState(b, gs->getGlobe(), false);
			_game->pushState(build);
			if (!build->placeAt(lon, lat))
			{
				resp["error"] = "coordinates not on land";
				_game->popState();
				delete b;
			}
			else
			{
				ConfirmNewBaseState* conf = findState<ConfirmNewBaseState>(_game);
				if (!conf)
					resp["error"] = "no ConfirmNewBaseState";
				else
				{
					resp["cost"] = conf->harnessCost();
					resp["affordable"] = conf->harnessConfirm();
					conf->btnOkClick(nullptr); // JOINT: no debit, pushes BaseNameState
					BaseNameState* nameState = findState<BaseNameState>(_game);
					if (!nameState)
						resp["error"] = "no BaseNameState (not enough money?)";
					else
					{
						nameState->setNameAndConfirm(name); // pops name+confirm+build, pushes PlaceLiftState
						PlaceLiftState* lift = findState<PlaceLiftState>(_game);
						if (!lift)
							resp["error"] = "no PlaceLiftState";
						else
						{
							bool ok = lift->harnessPlaceLift(liftX, liftY); // JOINT: base_new + pop
							resp["ok"] = ok;
							if (!ok) resp["error"] = "no access lift available";
						}
					}
				}
			}
		}
	}
	else if (cmd == "joint_cmd")
	{
		// PRD-J03: submit an arbitrary joint_cmd through the protocol (used to
		// exercise the unknown-command path). <jcmd> = command string,
		// <baseId> = base index, <payload> = optional JSON object.
		std::string jcmd = req.get("jcmd", "").asString();
		int baseId = req.get("baseId", 0).asInt();
		Json::Value payload = req.get("payload", Json::Value(Json::objectValue));
		JointEcon::submitLocalCmd(_game, jcmd, baseId, payload);
		resp["ok"] = true;
	}
	else if (cmd == "joint_stats")
	{
		// PRD-J03: read this machine's JointEcon protocol counters + the most
		// recent joint_fail reason surfaced here.
		JointEcon::Stats st = JointEcon::stats();
		resp["cmd"] = Json::Value::UInt64(st.cmd);
		resp["okCount"] = Json::Value::UInt64(st.ok);
		resp["failCount"] = Json::Value::UInt64(st.fail);
		resp["applyCount"] = Json::Value::UInt64(st.apply);
		resp["unknownCount"] = Json::Value::UInt64(st.unknown);
		resp["lastFail"] = JointEcon::lastFailReason();
		resp["ok"] = true;
	}
	else if (cmd == "joint_reset_stats")
	{
		JointEcon::resetStats();
		resp["ok"] = true;
	}
	else if (cmd == "set_funds")
	{
		// PRD-J03 test helper: force this world's funds (host-authoritative in
		// JOINT). Used to set up the insufficient-funds rejection path cleanly
		// without also tripping storage/space limits.
		if (!_game->getSavedGame())
			resp["error"] = "no saved game";
		else
		{
			int64_t value = req.get("value", 0).asInt64();
			_game->getSavedGame()->setFunds(value);
			resp["funds"] = Json::Value::Int64(_game->getSavedGame()->getFunds());
			resp["ok"] = true;
		}
	}
	else if (cmd == "start_research")
	{
		// PRD-J04 test helper: force-start a research project at the first real
		// base on THIS machine (vanilla has no naturally-available research at
		// game start). Low cost -> finishes in ~1 game day. In JOINT the host
		// ticks it to completion and broadcasts research_done; a replica that
		// also started it here gets its project removed + scientists freed on
		// apply, matching the host.
		SavedGame* sg = _game->getSavedGame();
		if (!sg || sg->getBases()->empty())
			resp["error"] = "no base";
		else
		{
			Base* base = nullptr;
			for (auto* b : *sg->getBases())
				if (!b->_coopBase && !b->_coopIcon) { base = b; break; }
			if (!base) base = sg->getBases()->front();
			std::string topic = req.get("topic", "").asString();
			int cost = req.get("cost", 1).asInt();
			int want = req.get("scientists", 1).asInt();
			RuleResearch* rule = nullptr;
			if (!topic.empty())
				rule = _game->getMod()->getResearch(topic, false);
			else
			{
				for (const auto& name : _game->getMod()->getResearchList())
				{
					RuleResearch* r = _game->getMod()->getResearch(name, false);
					if (!r || sg->isResearched(r, false)) continue;
					if (sg->isResearchRuleStatusDisabled(name)) continue;
					bool inProg = false;
					for (auto* p : base->getResearch())
						if (p->getRules() == r) { inProg = true; break; }
					if (!inProg) { rule = r; break; }
				}
			}
			if (!rule)
				resp["error"] = "no research rule";
			else
			{
				int assign = want;
				if (assign > base->getScientists()) assign = base->getScientists();
				if (assign < 1) assign = 1;
				ResearchProject* proj = new ResearchProject(rule, cost);
				proj->setAssigned(assign);
				base->addResearch(proj);
				if (base->getScientists() >= assign)
					base->setScientists(base->getScientists() - assign);
				resp["topic"] = rule->getName();
				resp["assigned"] = assign;
				resp["freeScientists"] = base->getScientists();
				resp["ok"] = true;
			}
		}
	}
	else if (cmd == "is_researched")
	{
		// PRD-J04 test helper: has a research topic been discovered on THIS world?
		SavedGame* sg = _game->getSavedGame();
		RuleResearch* rule = _game->getMod()->getResearch(req.get("topic", "").asString(), false);
		resp["researched"] = (sg && rule) ? sg->isResearched(rule, false) : false;
		resp["ok"] = true;
	}
	else if (cmd == "available_research")
	{
		// PRD-J06 test helper: topics res_start would accept at the base right now
		// (SavedGame::getAvailableResearchProjects). Lets a test pick a real topic.
		SavedGame* sg = _game->getSavedGame();
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (sg)
			for (auto* base : *sg->getBases())
				if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
				                     : base->getName() == baseName)
				{ target = base; break; }
		if (!target)
			resp["error"] = "base not found";
		else
		{
			std::vector<RuleResearch*> avail;
			sg->getAvailableResearchProjects(avail, _game->getMod(), target, false);
			Json::Value topics(Json::arrayValue);
			for (auto* r : avail) topics.append(r->getName());
			resp["topics"] = topics;
			resp["ok"] = true;
		}
	}
	else if (cmd == "discover_research")
	{
		// PRD-J06 test helper: mark <topic> discovered on THIS machine (call on
		// host AND client to keep the shared world equal). Unlocks manufactures /
		// research gated on it so a man_start test has an available production.
		// Deterministic; touches no funds.
		SavedGame* sg = _game->getSavedGame();
		RuleResearch* rule = _game->getMod()->getResearch(req.get("topic", "").asString(), false);
		Base* base = nullptr;
		if (sg)
			for (auto* b : *sg->getBases())
				if (!b->_coopBase && !b->_coopIcon) { base = b; break; }
		if (!sg || !rule)
			resp["error"] = "no world / unknown research";
		else
		{
			sg->addFinishedResearch(rule, _game->getMod(), base);
			resp["researched"] = sg->isResearched(rule, false);
			resp["ok"] = true;
		}
	}
	else if (cmd == "set_research_cost")
	{
		// PRD-J06 test helper: override a running project's _cost on THIS machine
		// (call on the HOST to force a fast completion). Host-authoritative
		// completion then broadcasts research_done regardless of replica cost.
		SavedGame* sg = _game->getSavedGame();
		std::string topic = req.get("topic", "").asString();
		int cost = req.get("cost", 1).asInt();
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (sg)
			for (auto* base : *sg->getBases())
				if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
				                     : base->getName() == baseName)
				{ target = base; break; }
		bool found = false;
		if (target)
			for (auto* p : target->getResearch())
				if (p->getRules()->getName() == topic) { p->setCost(cost); found = true; break; }
		resp["found"] = found;
		resp["ok"] = found;
		if (!found) resp["error"] = "research not running: " + topic;
	}
	else if (cmd == "set_production_progress")
	{
		// PRD-J06 test helper: set a running production's _timeSpent on THIS
		// machine (call on the HOST to drive a deterministic completion on the
		// next hourly step - host-authoritative prod_done then delivers items to
		// both). Avoids the coarse speed-5 overshoot completing early.
		SavedGame* sg = _game->getSavedGame();
		std::string item = req.get("item", "").asString();
		int spent = req.get("timeSpent", 0).asInt();
		// GAP-6 test hook: optionally set the production's assigned engineers too, so a
		// single hourly step can be driven to overshoot _amount*manufactureTime (the
		// prod_done over-materialization repro). Set directly on the Production (NOT via
		// the base pool); removeProduction returns it on completion, so call this on BOTH
		// machines with the SAME value to keep the free-engineer pools in lock-step.
		int engineers = req.get("engineers", -1).asInt();
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (sg)
			for (auto* base : *sg->getBases())
				if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
				                     : base->getName() == baseName)
				{ target = base; break; }
		bool found = false;
		if (target)
			for (auto* p : target->getProductions())
				if (p->getRules()->getName() == item) { p->setTimeSpent(spent); if (engineers >= 0) p->setAssignedEngineers(engineers); found = true; break; }
		resp["found"] = found;
		resp["ok"] = found;
		if (!found) resp["error"] = "production not running: " + item;
	}
	else if (cmd == "research_start")
	{
		// PRD-J06: drive the REAL ResearchInfoState "start project" OK path. In
		// JOINT this emits res_start (+ res_alloc when scientists>0); nothing is
		// applied until the joint_apply round-trip. Proves the UI submit path.
		std::string topic = req.get("topic", "").asString();
		int scientists = req.get("scientists", 0).asInt();
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (_game->getSavedGame())
			for (auto* base : *_game->getSavedGame()->getBases())
				if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
				                     : base->getName() == baseName)
				{ target = base; break; }
		RuleResearch* rule = _game->getMod()->getResearch(topic, false);
		if (!target)
			resp["error"] = "base not found";
		else if (!rule)
			resp["error"] = "unknown research: " + topic;
		else
		{
			ResearchInfoState* st = new ResearchInfoState(target, rule);
			_game->pushState(st);
			bool ok = st->harnessStart(scientists); // btnOkClick -> popState
			resp["sent"] = ok;
			resp["ok"] = ok;
		}
	}
	else if (cmd == "manufacture_start")
	{
		// PRD-J06: drive the REAL ManufactureInfoState "start production" OK path
		// (JOINT -> man_start). A parent ManufactureState is pushed first because
		// ManufactureInfoState::exitState pops TWO states for a new production.
		std::string item = req.get("item", "").asString();
		int engineers = req.get("engineers", 0).asInt();
		int qty = req.get("qty", 1).asInt();
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (_game->getSavedGame())
			for (auto* base : *_game->getSavedGame()->getBases())
				if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
				                     : base->getName() == baseName)
				{ target = base; break; }
		RuleManufacture* rule = _game->getMod()->getManufacture(item, false);
		if (!target)
			resp["error"] = "base not found";
		else if (!rule)
			resp["error"] = "unknown manufacture: " + item;
		else
		{
			_game->pushState(new ManufactureState(target)); // absorbs the 2nd pop
			ManufactureInfoState* st = new ManufactureInfoState(target, rule);
			_game->pushState(st);
			bool ok = st->harnessStart(engineers, qty); // btnOkClick -> exitState
			resp["sent"] = ok;
			resp["ok"] = ok;
		}
	}
	else if (cmd == "set_facility_build_time")
	{
		// PRD-J04 test helper: set a base facility's buildTime on THIS machine.
		// A JOINT replica's time1Day must NOT decrement it; only fac_done (host
		// completion broadcast) may drive it to 0.
		SavedGame* sg = _game->getSavedGame();
		int baseId = req.get("baseId", 0).asInt();
		int index = req.get("index", -1).asInt();
		int t = req.get("time", 10).asInt();
		if (!sg || baseId < 0 || baseId >= (int)sg->getBases()->size())
			resp["error"] = "bad base";
		else
		{
			Base* base = (*sg->getBases())[baseId];
			auto* facs = base->getFacilities();
			BaseFacility* fac = nullptr;
			if (index >= 0 && index < (int)facs->size()) fac = (*facs)[index];
			else if (!facs->empty()) fac = facs->front();
			if (!fac)
				resp["error"] = "no facility";
			else
			{
				fac->setBuildTime(t);
				resp["x"] = fac->getX();
				resp["y"] = fac->getY();
				resp["type"] = fac->getRules()->getType();
				resp["buildTime"] = fac->getBuildTime();
				resp["ok"] = true;
			}
		}
	}
	else if (cmd == "set_geo_day")
	{
		// PRD-J04 test helper: jump the host clock to near month-end so a short
		// advance rolls the month (exercising the monthly settlement sync)
		// without simulating 30 in-game days. Host-authoritative; the client's
		// clock follows via the time packet.
		SavedGame* sg = _game->getSavedGame();
		if (!sg)
			resp["error"] = "no save";
		else
		{
			GameTime* t = sg->getTime();
			int day = req.get("day", 28).asInt();
			int hour = req.get("hour", t->getHour()).asInt();
			GameTime nt(t->getWeekday(), day, t->getMonth(), t->getYear(),
			            hour, t->getMinute(), t->getSecond());
			sg->setTime(nt);
			resp["day"] = sg->getTime()->getDay();
			resp["month"] = sg->getTime()->getMonth();
			resp["ok"] = true;
		}
	}
	else if (cmd == "fly_craft")
	{
		// PRD-J04 test helper: launch the first ready craft at the first real
		// base toward a distant waypoint, so the host moves it and the JOINT
		// position snapshot carries the motion to the (frozen) replica.
		SavedGame* sg = _game->getSavedGame();
		Base* base = nullptr; Craft* craft = nullptr;
		if (sg)
			for (auto* b : *sg->getBases())
				if (!b->_coopBase && !b->_coopIcon && !b->getCrafts()->empty())
				{ base = b; craft = b->getCrafts()->front(); break; }
		if (!craft)
			resp["error"] = "no craft";
		else
		{
			Waypoint* w = new Waypoint();
			// lon/lat are radians; a large longitude offset (wraps, no pole
			// clamp) keeps the craft en route for the whole observation window.
			w->setLongitude(base->getLongitude() + 0.5);
			w->setLatitude(base->getLatitude() + 0.1);
			w->setId(sg->getId("STR_WAY_POINT"));
			sg->getWaypoints()->push_back(w);
			craft->setDestination(w);
			craft->setStatus("STR_OUT");
			resp["craftId"] = craft->getId();
			resp["baseLon"] = base->getLongitude();
			resp["baseLat"] = base->getLatitude();
			resp["ok"] = true;
		}
	}
	else if (cmd == "give_layout")
	{
		// Test helper: give every soldier at a base an equipment-layout entry
		// for <item> (optionally capped at <count> soldiers). Reproduces the
		// state a player creates by equipping soldiers - the layout reserves
		// the item, but (as in the real game) base storage is NOT decremented,
		// so the item is double-counted until a battle actually consumes it.
		std::string layoutBase = req.get("base", "").asString();
		bool wantCoop = req.get("coop", false).asBool();
		std::string itemName = req.get("item", "").asString();
		int count = req.get("count", -1).asInt();
		std::string slotName = req.get("slot", "belt").asString();  // belt|right|left
		std::string onlyName = req.get("name", "").asString();      // limit to soldiers matching this
		Base* target = nullptr;
		if (_game->getSavedGame())
		{
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				bool match;
				if (wantCoop) match = base->_coopBase;
				else if (!layoutBase.empty()) match = (base->getName() == layoutBase);
				else match = (base->_coopBase == false && base->_coopIcon == false);
				if (match) { target = base; break; }
			}
		}
		const RuleItem* rule = itemName.empty() ? nullptr : _game->getMod()->getItem(itemName, false);
		if (!target)
			resp["error"] = "base not found";
		else if (!rule)
			resp["error"] = "unknown item: " + itemName;
		else
		{
			auto* slot = _game->getMod()->getInventoryBelt();
			if (slotName == "right") slot = _game->getMod()->getInventoryRightHand();
			else if (slotName == "left") slot = _game->getMod()->getInventoryLeftHand();
			int dummyId = 0;
			int given = 0;
			for (auto* s : *target->getSoldiers())
			{
				if (!onlyName.empty() && s->getName().find(onlyName) == std::string::npos) continue;
				if (count >= 0 && given >= count) break;
				BattleItem tmp(rule, &dummyId);
				tmp.setSlot(slot);
				tmp.setSlotX(0);
				tmp.setSlotY(0);
				s->getEquipmentLayout()->push_back(new EquipmentLayoutItem(&tmp));
				given++;
			}
			resp["given"] = given;
			resp["ok"] = true;
		}
	}
	else if (cmd == "transfer_to_coop_base")
	{
		// Vanilla base->base transfer of one soldier to a co-op base (no
		// ownership change), driven through the real TransferItemsState /
		// completeTransfer path. This is the "transfer" (distinct from the
		// "gift" ownership change).
		std::string name = req.get("name", "").asString();
		std::string toBaseName = req.get("toBase", "").asString();
		Base* baseFrom = nullptr;
		Soldier* soldier = nullptr;
		Base* baseTo = nullptr;
		if (_game->getSavedGame())
		{
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				if (base->_coopBase == false && base->_coopIcon == false)
				{
					for (auto* s : *base->getSoldiers())
					{
						if (s->getName().find(name) != std::string::npos) { soldier = s; baseFrom = base; break; }
					}
				}
				if (soldier) break;
			}
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				if (base->_coopBase && (toBaseName.empty() || base->getName() == toBaseName)) { baseTo = base; break; }
			}
			if (!baseTo)
			{
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					if (base->_coopIcon && (toBaseName.empty() || base->getName() == toBaseName)) { baseTo = base; break; }
				}
			}
		}
		if (!soldier)
			resp["error"] = "soldier not found in own base: " + name;
		else if (!baseTo)
			resp["error"] = "coop dest base not found: " + toBaseName;
		else
		{
			resp["toBase"] = baseTo->getName();
			resp["toBaseCoopBase"] = baseTo->_coopBase;
			resp["toBaseCoopIcon"] = baseTo->_coopIcon;
			TransferItemsState* st = new TransferItemsState(baseFrom, baseTo, nullptr);
			bool ok = st->transferSoldierNow(soldier);
			delete st;
			resp["transferred"] = ok;
			resp["ok"] = ok;
			if (!ok) resp["error"] = "soldier not a transferable row";
		}
	}
	else if (cmd == "set_coop_base")
	{
		// Test helper: force a soldier's coopBase field. A value != -1 makes
		// the own-base SoldiersState treat it as a foreign/guest soldier and
		// strip it from the editable roster (reproducing the own-base variant
		// of issue #33 without a cross-machine transfer).
		std::string name = req.get("name", "").asString();
		int value = req.get("value", -1).asInt();
		Soldier* found = nullptr;
		if (_game->getSavedGame())
		{
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				for (auto* s : *base->getSoldiers())
				{
					if (s->getName().find(name) != std::string::npos) { found = s; break; }
				}
				if (found) break;
			}
		}
		if (!found)
			resp["error"] = "soldier not found: " + name;
		else
		{
			found->setCoopBase(value);
			resp["ok"] = true;
		}
	}
	else if (cmd == "visit_coop_base")
	{
		std::string baseName = req.get("base", "").asString();
		Base* target = nullptr;
		if (_game->getSavedGame())
		{
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				if (base->_coopBase == true || base->_coopIcon == true)
				{
					if (baseName.empty() || base->getName() == baseName)
					{
						target = base;
						break;
					}
				}
			}
		}
		GeoscapeState* geo = _game->getGeoscapeState();
		if (!target)
		{
			resp["error"] = "coop base not found: " + baseName;
		}
		else if (!geo)
		{
			resp["error"] = "no GeoscapeState";
		}
		else
		{
			// same as clicking the peer base marker (MultipleTargetsState)
			coop->current_base_name = target->getName();
			CoopState* w = new CoopState(50);
			w->setGlobe(geo->getGlobe());
			_game->pushState(w);
			resp["ok"] = true;
		}
	}
	else if (cmd == "leave_base")
	{
		BasescapeState* st = findState<BasescapeState>(_game);
		if (st)
		{
			st->btnGeoscapeClick(nullptr);
			resp["ok"] = true;
		}
		else
		{
			resp["error"] = "no BasescapeState in state stack";
		}
	}
	else if (cmd == "gift_targets")
	{
		// What the transfer dialog would offer for this soldier - lets
		// tests validate owner resolution + button names without UI.
		std::string name = req.get("name", "").asString();
		Soldier* found = nullptr;
		if (_game->getSavedGame())
		{
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				for (auto* s : *base->getSoldiers())
				{
					if (s->getName().find(name) != std::string::npos)
					{
						found = s;
						break;
					}
				}
				if (found) break;
			}
		}
		if (!found)
		{
			resp["error"] = "soldier not found: " + name;
		}
		else
		{
			int currentOwner = GiftSoldierMenu::resolveOwnerId(found);
			int localPlayerId = connectionTCP::localSeat();
			Json::Value targets(Json::arrayValue);
			for (int playerId = 0; playerId <= 1; ++playerId)
			{
				if (playerId != currentOwner)
				{
					Json::Value t;
					t["id"] = playerId;
					t["name"] = (playerId == localPlayerId) ? coop->getHostName() : coop->getCurrentClientName();
					targets.append(t);
				}
			}
			resp["currentOwner"] = currentOwner;
			resp["localPlayer"] = localPlayerId;
			resp["targets"] = targets;
			resp["ok"] = true;
		}
	}
	else if (cmd == "open_gift_dialog")
	{
		std::string name = req.get("name", "").asString();
		Soldier* found = nullptr;
		if (_game->getSavedGame())
		{
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				for (auto* s : *base->getSoldiers())
				{
					if (s->getName().find(name) != std::string::npos)
					{
						found = s;
						break;
					}
				}
				if (found) break;
			}
		}
		if (found)
		{
			_game->pushState(new GiftSoldierMenu(found, GiftSoldierMenu::resolveOwnerId(found)));
			resp["ok"] = true;
		}
		else
		{
			resp["error"] = "soldier not found: " + name;
		}
	}
	else if (cmd == "rename_soldier")
	{
		std::string name = req.get("name", "").asString();
		std::string newName = req.get("newName", "").asString();
		Soldier* found = nullptr;
		if (_game->getSavedGame() && !newName.empty())
		{
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				for (auto* s : *base->getSoldiers())
				{
					if (s->getName().find(name) != std::string::npos)
					{
						found = s;
						break;
					}
				}
				if (found) break;
			}
		}
		if (found)
		{
			found->setName(newName);
			resp["ok"] = true;
		}
		else
		{
			resp["error"] = "soldier not found: " + name;
		}
	}
	else if (cmd == "show_notice")
	{
		_game->pushState(new GiftNoticeState(req.get("message", "test notice").asString()));
		resp["ok"] = true;
	}
	else if (cmd == "get_notices")
	{
		Json::Value notices(Json::arrayValue);
		// PRD-13: left inline - appends every match's category into a container, not a find
		for (auto* s : _game->getStates())
		{
			if (auto* n = dynamic_cast<GiftNoticeState*>(s))
			{
				notices.append(n->getCategory());
			}
		}
		resp["categories"] = notices;
		resp["ok"] = true;
	}
	else if (cmd == "dismiss_notice")
	{
		GiftNoticeState* st = findState<GiftNoticeState>(_game);
		if (st)
		{
			st->btnOkClick(nullptr);
			resp["ok"] = true;
		}
		else
		{
			resp["error"] = "no GiftNoticeState in state stack";
		}
	}
	else if (cmd == "cancel_dialog")
	{
		GiftSoldierMenu* st = findState<GiftSoldierMenu>(_game);
		if (st)
		{
			st->btnCancelClick(nullptr);
			resp["ok"] = true;
		}
		else
		{
			resp["error"] = "no GiftSoldierMenu in state stack";
		}
	}
	else if (cmd == "get_palettes")
	{
		// First N palette entries of the top two states, for asserting
		// that a dialog adopted its parent's palette (flicker check).
		Json::Value states(Json::arrayValue);
		// PRD-13: left inline - reads every state's palette into a container, not a find
		auto& stack = _game->getStates();
		for (auto* s : stack)
		{
			Json::Value e;
			e["state"] = typeid(*s).name();
			Json::Value cols(Json::arrayValue);
			SDL_Color* pal = s->getPalette();
			for (int i = 0; i < 16; ++i)
			{
				cols.append((pal[i].r << 16) | (pal[i].g << 8) | pal[i].b);
			}
			e["colors"] = cols;
			states.append(e);
		}
		resp["states"] = states;
		resp["ok"] = true;
	}
	else if (cmd == "gift")
	{
		std::string name = req.get("name", "").asString();
		int owner = req.get("owner", -1).asInt();
		if (name.empty() || owner < 0)
		{
			resp["error"] = "need name and owner";
		}
		else if (!_game->getSavedGame())
		{
			resp["error"] = "no save loaded";
		}
		else
		{
			Soldier* found = nullptr;
			for (auto* base : *_game->getSavedGame()->getBases())
			{
				for (auto* s : *base->getSoldiers())
				{
					if (s->getName().find(name) != std::string::npos)
					{
						found = s;
						break;
					}
				}
				if (found) break;
			}
			if (!found)
			{
				resp["error"] = "soldier not found: " + name;
			}
			else
			{
				coop->giftSoldier(found, owner, true);
				resp["soldier"] = soldierToJson(found);
				resp["ok"] = true;
			}
		}
	}
	else if (cmd == "save_game")
	{
		std::string file = req.get("file", "").asString();
		if (file.empty() || !_game->getSavedGame())
		{
			resp["error"] = "need file + loaded save";
		}
		else
		{
			_game->getSavedGame()->save(file, _game->getMod());
			resp["ok"] = true;
		}
	}
	else if (cmd == "reload_save_roundtrip")
	{
		// Playtest B4: prove the ON-LOAD ownership migration without disturbing the
		// live coop session. Save the current world, load it into a THROWAWAY blank
		// SavedGame (exactly the path a real resume/load takes -> SavedGame::load ->
		// migrateJointSoldierOwnership) and report the loaded roster's owners.
		SavedGame* sg = _game->getSavedGame();
		if (!sg)
		{
			resp["error"] = "no save";
		}
		else
		{
			std::string f = "harness_migrate_roundtrip.sav";
			sg->save(f, _game->getMod());
			SavedGame* fresh = new SavedGame();
			fresh->load(f, _game->getMod(), _game->getLanguage());
			Json::Value arr(Json::arrayValue);
			for (auto* b : *fresh->getBases())
				for (auto* s : *b->getSoldiers())
				{
					Json::Value j;
					j["id"] = s->getId();
					j["owner"] = s->getOwnerPlayerId();
					arr.append(j);
				}
			resp["soldiers"] = arr;
			delete fresh;
			resp["ok"] = true;
		}
	}
	else if (cmd == "save_game_ui")
	{
		// Save through the real SaveGameState funnel (same path as the
		// in-game autosaves/quicksave), unlike save_game which calls
		// SavedGame::save directly. Exercises the coop save cycle and the
		// client-side save suppression gate. Only the single-pop SaveTypes
		// are exposed: SAVE_DEFAULT pops the save+pause menus it normally
		// sits on, which don't exist when pushed from here.
		std::string type = req.get("type", "").asString();
		if (!_game->getSavedGame())
		{
			resp["error"] = "no loaded save";
		}
		else if (type == "auto_geoscape")
		{
			_game->pushState(new SaveGameState(OPT_GEOSCAPE, SAVE_AUTO_GEOSCAPE, _game->getScreen()->getPalette()));
			resp["ok"] = true;
		}
		else if (type == "quick")
		{
			_game->pushState(new SaveGameState(OPT_GEOSCAPE, SAVE_QUICK, _game->getScreen()->getPalette()));
			resp["ok"] = true;
		}
		else
		{
			resp["error"] = "need type (auto_geoscape|quick)";
		}
	}
	else if (cmd == "client_reload_progress")
	{
		// Reconnect flow: ask the host for our world (same as the client
		// branch of Profile::buttonOK).
		if (connectionTCP::getServerOwner())
		{
			resp["error"] = "host cannot reload progress";
		}
		else if (connectionTCP::saveID == 0)
		{
			resp["error"] = "no saveID";
		}
		else
		{
			Json::Value root;
			root["state"] = "request_load_progress";
			coop->sendTCPPacketData(root.toStyledString());
			resp["ok"] = true;
		}
	}
	else if (cmd == "lobby_state")
	{
		// campaign-lobby introspection (flow-redesign F2)
		LobbyMenu* lobby = findState<LobbyMenu>(_game);
		resp["lobbyOpen"] = (lobby != nullptr);
		resp["lobbyMode"] = connectionTCP::session.lobbyMode;
		resp["sessionLocked"] = connectionTCP::session.sessionLocked;
		resp["startEligible"] = (lobby != nullptr) && lobby->startEligible();
		if (lobby)
		{
			resp["buttonText"] = lobby->actionButtonText();
			resp["buttonVisible"] = lobby->actionButtonVisible();
			resp["detailsText"] = lobby->detailsText();
			resp["hostRowName"] = lobby->rowNameById(1);   // playtest: role-correct names
			resp["clientRowName"] = lobby->rowNameById(2);
			int idx = 0;
			for (const auto& n : lobby->rosterNames())
			{
				resp["players"][idx++] = n;
			}
		}
		resp["ok"] = true;
	}
	else if (cmd == "open_coop_menu")
	{
		// Playtest B7: the in-game coop menu. Reached from the pause screen ->
		// co-op -> (connected) ServerList redirects to the LobbyMenu. Opened over
		// a running campaign it must offer RESUME GAME, not only Disconnect.
		_game->pushState(new LobbyMenu());
		resp["ok"] = true;
	}
	else if (cmd == "lobby_action")
	{
		// Playtest B7: click the lobby's single action button (RESUME GAME when
		// the menu was opened mid-game).
		LobbyMenu* lobby = findState<LobbyMenu>(_game);
		if (!lobby)
			resp["error"] = "no LobbyMenu in state stack";
		else
		{
			lobby->btnCancelClick(nullptr);
			resp["ok"] = true;
		}
	}
	else if (cmd == "load_save_menu")
	{
		// load through the real LoadGameState (runs the co-op routing:
		// coop save -> host window + resume lobby) (flow-redesign F3)
		std::string file = req.get("file", "").asString();
		if (file.empty())
		{
			resp["error"] = "need file";
		}
		else
		{
			_game->pushState(new LoadGameState(OPT_MENU, file, _game->getScreen()->getPalette()));
			resp["ok"] = true;
		}
	}
	else if (cmd == "lobby_resume_campaign")
	{
		// host clicks RESUME CAMPAIGN (flow-redesign F3)
		LobbyMenu* lobby = findState<LobbyMenu>(_game);
		if (!lobby)
		{
			resp["error"] = "no LobbyMenu in state stack";
		}
		else if (connectionTCP::session.lobbyMode != 2)
		{
			resp["error"] = "lobby is not in resume mode";
		}
		else if (!lobby->missingPlayers().empty())
		{
			std::string missing;
			for (const auto& m : lobby->missingPlayers())
			{
				if (!missing.empty()) missing += ", ";
				missing += m;
			}
			resp["error"] = "waiting for: " + missing;
		}
		else
		{
			lobby->resumeCampaign();
			resp["ok"] = true;
		}
	}
	else if (cmd == "lobby_start_campaign")
	{
		// host clicks START CAMPAIGN + confirms (flow-redesign F2)
		LobbyMenu* lobby = findState<LobbyMenu>(_game);
		if (!lobby)
		{
			resp["error"] = "no LobbyMenu in state stack";
		}
		else if (connectionTCP::session.lobbyMode != 1)
		{
			resp["error"] = "lobby is not in new-campaign mode";
		}
		else if (req.get("confirm", "").asString() == "dialog")
		{
			// PRD-10: route through the REAL confirm dialog so the test drives
			// ConfirmStartCampaignState::btnOkClick (the true UI path). Do NOT
			// pre-check startEligible here - the dialog/OK path IS the gate.
			lobby->openStartConfirmDialog();
			resp["ok"] = true;
		}
		else if (!lobby->startEligible())
		{
			resp["error"] = "no client connected";
		}
		else
		{
			lobby->startCampaign();
			resp["ok"] = true;
		}
	}
	else if (cmd == "lobby_confirm_ok")
	{
		// PRD-10: click OK on the START CAMPAIGN confirm dialog (the real
		// ConfirmStartCampaignState::btnOkClick path, which re-checks
		// eligibility before starting).
		LobbyMenu* lobby = findState<LobbyMenu>(_game);
		if (!lobby)
		{
			resp["error"] = "no LobbyMenu in state stack";
		}
		else
		{
			resp["clicked"] = lobby->clickStartConfirmOk();
			resp["ok"] = true;
		}
	}
	else if (cmd == "save_markers")
	{
		// Co-op campaign markers of the live save (flow-redesign F0)
		if (!_game->getSavedGame())
		{
			resp["error"] = "no loaded save";
		}
		else
		{
			resp["coop"] = _game->getSavedGame()->isCoopSave();
			int idx = 0;
			for (const auto& p : _game->getSavedGame()->getCoopPlayers())
			{
				resp["coopPlayers"][idx++] = p;
			}
			// PRD-J01: campaign economy model (0 = Separate, 1 = Joint).
			resp["campaignType"] = static_cast<int>(_game->getSavedGame()->getCampaignType());
			resp["saveID"] = Json::Value::Int64(connectionTCP::saveID);
			resp["ok"] = true;
		}
	}
	else if (cmd == "has_coop_file")
	{
		std::string key = req.get("key", "").asString();
		resp["present"] = connectionTCP::hasCoopFile(key);
		resp["ok"] = true;
	}
	else if (cmd == "dump_coop_file")
	{
		// Test fixture builder: write an in-memory blob to the user dir
		// (used to fabricate legacy sidecar .data files for the
		// v1.8.4-migration test; nothing in normal play calls this).
		std::string key = req.get("key", "").asString();
		std::string blob;
		{
			std::lock_guard<std::mutex> lock(connectionTCP::coopFilesMutex);
			auto it = connectionTCP::coopFilesHost.find(key);
			if (it != connectionTCP::coopFilesHost.end())
				blob = it->second;
			else
			{
				auto cit = connectionTCP::coopFilesClient.find(key);
				if (cit != connectionTCP::coopFilesClient.end())
					blob = cit->second;
			}
		}
		if (blob.empty())
		{
			resp["error"] = "no such blob";
		}
		else
		{
			std::ofstream out(Options::getMasterUserFolder() + key, std::ios::binary);
			out << blob;
			resp["ok"] = true;
		}
	}
	else if (cmd == "set_option")
	{
		std::string name = req.get("name", "").asString();
		if (name == "HostSaveProgress")
		{
			// Removed option (host-save authority is the only mode now);
			// accepted and ignored so older test scripts keep running.
			resp["ok"] = true;
		}
		else if (name == "autosave")
		{
			Options::autosave = req.get("value", false).asBool();
			resp["ok"] = true;
		}
		else if (name == "autosaveFrequency")
		{
			Options::autosaveFrequency = req.get("value", 5).asInt();
			resp["ok"] = true;
		}
		else if (name == "oxceGeoAutosaveFrequency")
		{
			Options::oxceGeoAutosaveFrequency = req.get("value", 0).asInt();
			resp["ok"] = true;
		}
		else if (name == "oxceAlternateCraftEquipmentManagement")
		{
			Options::oxceAlternateCraftEquipmentManagement = req.get("value", false).asBool();
			resp["ok"] = true;
		}
		else
		{
			resp["error"] = "unknown option: " + name;
		}
	}
	else if (cmd == "set_seed")
	{
		// Pin the RNG so a scenario's real-sim outcome is reproducible.
		RNG::setSeed((uint64_t)req.get("seed", 1).asInt64());
		resp["seed"] = Json::Value::Int64((int64_t)RNG::getSeed());
		resp["ok"] = true;
	}
	else if (cmd == "craft_force")
	{
		// Owner-side deterministic state setter for craft-status tests. With
		// craft_id omitted, targets the first own (non-coop) craft. Only sets
		// the fields present in the request; optional checkup() re-derives the
		// base-side _status (READY/REFUELLING/REARMING/REPAIRS) from real logic.
		int craftId = req.get("craft_id", -1).asInt();
		SavedGame* sg = _game->getSavedGame();
		Craft* craft = nullptr; Base* cbase = nullptr;
		if (sg)
		{
			for (auto* b : *sg->getBases())
			{
				for (auto* c : *b->getCrafts())
				{
					if (craftId == -1 ? !c->coop : c->getId() == craftId)
					{
						craft = c; cbase = b; break;
					}
				}
				if (craft) break;
			}
		}
		if (!craft)
		{
			resp["error"] = "no matching craft";
		}
		else
		{
			if (req.isMember("lowFuel")) craft->setLowFuel(req["lowFuel"].asBool());
			if (req.isMember("mission")) craft->setMissionComplete(req["mission"].asBool());
			// Force the geoscape status string directly (e.g. "STR_OUT" to make
			// an own craft "out"/airborne without the takeoff sim). Honors the
			// coop==false guard in Craft::setStatus.
			if (req.isMember("status")) craft->setStatus(req["status"].asString());
			// Teleport the craft (e.g. away from its base so it reads as OUT).
			if (req.isMember("lon")) craft->setLongitude(req["lon"].asDouble());
			if (req.isMember("lat")) craft->setLatitude(req["lat"].asDouble());
			if (req.isMember("fuel")) craft->setFuel(req["fuel"].asInt());
			if (req.isMember("damage")) craft->setDamage(req["damage"].asInt());
			if (req.isMember("dogfight")) craft->setInDogfight(req["dogfight"].asBool());
			if (req.isMember("ammo"))
			{
				int ammo = req["ammo"].asInt();
				for (auto* cw : *craft->getWeapons())
					if (cw) { cw->setAmmo(ammo); cw->setRearming(ammo < cw->getRules()->getAmmoMax()); }
			}
			if (req.isMember("dest"))
			{
				std::string dest = req["dest"].asString();
				if (dest == "base") craft->setDestination(cbase);
				else if (dest == "patrol") craft->setDestination(nullptr);
				else if (dest.rfind("site:", 0) == 0)
				{
					int id = std::atoi(dest.c_str() + 5);
					for (auto* ms : *sg->getMissionSites())
						if (ms->getId() == id) { craft->setDestination(ms); break; }
				}
				else if (dest.rfind("ufo:", 0) == 0)
				{
					int id = std::atoi(dest.c_str() + 4);
					for (auto* u : *sg->getUfos())
						if (u->getId() == id) { craft->setDestination(u); break; }
				}
			}
			if (req.get("checkup", false).asBool()) craft->checkup();
			resp["craft_id"] = craft->getId();
			resp["status"] = craft->getStatus();
			resp["displayStatus"] = craft->getDisplayStatus(_game->getLanguage());
			resp["ok"] = true;
		}
	}
	else if (cmd == "spawn_mission_site")
	{
		// Deterministically place a mission site (no alien-mission RNG), so a
		// craft can be dispatched to it -> "heading to <site>" status. Params:
		// mission (RuleAlienMission id), deployment (AlienDeployment id),
		// lon/lat (radians), race (optional).
		SavedGame* sg = _game->getSavedGame();
		Mod* mod = _game->getMod();
		const RuleAlienMission* mission = mod->getAlienMission(req.get("mission", "").asString(), false);
		const AlienDeployment* deployment = mod->getDeployment(req.get("deployment", "").asString(), false);
		if (!sg) resp["error"] = "no saved game";
		else if (!mission) resp["error"] = "unknown mission rule";
		else if (!deployment) resp["error"] = "unknown deployment rule";
		else
		{
			MissionSite* site = new MissionSite(mission, deployment, nullptr);
			site->setLongitude(req.get("lon", 0.0).asDouble());
			site->setLatitude(req.get("lat", 0.0).asDouble());
			site->setId(sg->getId(deployment->getMarkerName()));
			site->setSecondsRemaining((size_t)req.get("hours", 48).asInt() * 3600);
			site->setAlienRace(req.get("race", "STR_SECTOID").asString());
			site->setDetected(true);
			sg->getMissionSites()->push_back(site);
			resp["site_id"] = site->getId();
			resp["ok"] = true;
		}
	}
	else if (cmd == "spawn_ufo")
	{
		// Deterministically place a UFO so a craft can be dispatched to it
		// (B7 intercepting / B8 destination-crashed / B6 tailing). Attaches a
		// registered throwaway AlienMission so the UFO's lifecycle (race bonus,
		// ~Ufo decreaseLiveUfos) is well-formed and doesn't crash on cleanup.
		// Params: type (RuleUfo), mission (RuleAlienMission), region, race,
		// trajectory (UfoTrajectory id), state (flying|crashed|landed), lon/lat.
		SavedGame* sg = _game->getSavedGame();
		Mod* mod = _game->getMod();
		const RuleUfo* ufoRule = mod->getUfo(req.get("type", "").asString(), false);
		const RuleAlienMission* missionRule = mod->getAlienMission(req.get("mission", "").asString(), false);
		const UfoTrajectory* traj = mod->getUfoTrajectory(req.get("trajectory", "").asString(), false);
		if (!sg) resp["error"] = "no saved game";
		else if (!ufoRule) resp["error"] = "unknown ufo type";
		else if (!missionRule) resp["error"] = "unknown mission rule";
		else if (!traj) resp["error"] = "unknown trajectory";
		else
		{
			AlienMission* m = new AlienMission(*missionRule);
			m->setRace(req.get("race", "STR_SECTOID").asString());
			m->setRegion(req.get("region", "STR_NORTH_AMERICA").asString(), *mod);
			m->setId(sg->getId("STR_ALIEN_MISSIONS"));
			sg->getAlienMissions().push_back(m);

			Ufo* u = new Ufo(ufoRule, sg->getId("STR_UFO_UNIQUE"));
			u->setMissionInfo(m, traj);
			// _uniqueId (ctor) is internal; the display id / craft targeting
			// use Target::getId(), which real UFOs get on detection. Assign it
			// so geo_state reports distinct ids and craft_force can target this
			// exact UFO instead of colliding on id 0.
			u->setId(sg->getId("STR_UFO"));
			u->setLongitude(req.get("lon", 0.0).asDouble());
			u->setLatitude(req.get("lat", 0.0).asDouble());
			u->setAltitude("STR_HIGH_UC");
			u->setDetected(true);
			std::string state = req.get("state", "flying").asString();
			if (state == "crashed")
			{
				u->setStatus(Ufo::CRASHED);
				u->setSecondsRemaining((size_t)req.get("hours", 24).asInt() * 3600);
				u->setSpeed(0);
			}
			else if (state == "landed")
			{
				u->setStatus(Ufo::LANDED);
				u->setSecondsRemaining((size_t)req.get("hours", 24).asInt() * 3600);
				u->setSpeed(0);
			}
			else
			{
				// Flying: give it a destination waypoint so think()->move() is
				// well-formed (a null destination would misbehave).
				u->setStatus(Ufo::FLYING);
				Waypoint* wp = new Waypoint();
				wp->setLongitude(req.get("lon", 0.0).asDouble() + 0.2);
				wp->setLatitude(req.get("lat", 0.0).asDouble());
				u->setDestination(wp);
				u->setSpeed(req.get("speed", 1).asInt());
			}
			sg->getUfos()->push_back(u);
			resp["ufo_id"] = u->getId();
			resp["ok"] = true;
		}
	}
	else if (cmd == "ufo_alert")
	{
		// Playtest B5: reproduce a native UFO detection on THIS machine. Building a
		// UfoDetectedState is exactly what the geoscape sim does when a UFO is first
		// detected, and its ctor broadcasts the reliable "ufo_popup" packet
		// (type+race) to the peer - the JOINT host's one shared sim detecting a UFO
		// must alert every player, not just the host. Picks the first detected UFO
		// (spawn one first). Construct + delete: the ctor's broadcast is the point.
		GeoscapeState* gs = findState<GeoscapeState>(_game);
		SavedGame* sg = _game->getSavedGame();
		Ufo* u = nullptr;
		if (sg)
			for (auto* x : *sg->getUfos())
				if (x->getDetected()) { u = x; break; }
		if (!gs)
			resp["error"] = "no GeoscapeState";
		else if (!u)
			resp["error"] = "no detected ufo (spawn_ufo first)";
		else
		{
			UfoDetectedState* ud = new UfoDetectedState(u, gs, true, false, false);
			delete ud; // ctor already broadcast ufo_popup {type, race}
			resp["type"] = u->getRules()->getType();
			resp["race"] = u->getAlienRace();
			resp["ok"] = true;
		}
	}
	else if (cmd == "spawn_craft")
	{
		// Add a fully-armed craft (e.g. STR_INTERCEPTOR) to the first own base.
		// The coop start base only has an unarmed transport, so REARMING and
		// UFO-interception status tests need a combat craft. Params: type
		// (RuleCraft), weapon (RuleCraftWeapon to mount on every hardpoint).
		SavedGame* sg = _game->getSavedGame();
		Mod* mod = _game->getMod();
		const RuleCraft* rule = mod->getCraft(req.get("type", "").asString(), false);
		Base* base = nullptr;
		if (sg)
			for (auto* b : *sg->getBases())
				if (!b->_coopBase) { base = b; break; }
		if (!sg) resp["error"] = "no saved game";
		else if (!rule) resp["error"] = "unknown craft type";
		else if (!base) resp["error"] = "no own base";
		else
		{
			Craft* craft = new Craft(rule, base, sg->getId(rule->getType()));
			craft->setFuel(craft->getFuelMax());
			RuleCraftWeapon* cwRule = const_cast<RuleCraftWeapon*>(
				mod->getCraftWeapon(req.get("weapon", "STR_STINGRAY").asString(), false));
			if (cwRule)
				// The Craft ctor pre-fills getWeapons() null slots; mount INTO
				// them (a push_back would leave the first slots empty - the
				// dogfight only scans the first rule->getWeapons() slots).
				for (size_t i = 0; i < craft->getWeapons()->size(); ++i)
					if (!craft->getWeapons()->at(i))
						craft->getWeapons()->at(i) = new CraftWeapon(cwRule, cwRule->getAmmoMax());
			craft->checkup(); // -> STR_READY
			base->getCrafts()->push_back(craft);
			resp["craft_id"] = craft->getId();
			resp["weapons"] = (int)craft->getWeapons()->size();
			resp["ok"] = true;
		}
	}
	else if (cmd == "move_ufo")
	{
		// Teleport an existing UFO to lon/lat (radians). Used to prove a peer
		// craft live-tracks a MOVING coop UFO: move the host's UFO and the
		// client mirror (and any craft bound to it) follows via target_positions.
		// Target by cross-instance coop id (coop_id) or local id (ufo_id).
		SavedGame* sg = _game->getSavedGame();
		int coopId = req.get("coop_id", -1).asInt();
		int id = req.get("ufo_id", -1).asInt();
		Ufo* target = nullptr;
		if (sg)
			for (auto* u : *sg->getUfos())
				if ((coopId != -1 && u->getCoopUfoId() == coopId) || (id != -1 && u->getId() == id))
				{ target = u; break; }
		if (!sg) resp["error"] = "no saved game";
		else if (!target) resp["error"] = "no matching ufo";
		else
		{
			target->setLongitude(req.get("lon", target->getLongitude()).asDouble());
			target->setLatitude(req.get("lat", target->getLatitude()).asDouble());
			resp["lon"] = target->getLongitude();
			resp["lat"] = target->getLatitude();
			resp["ok"] = true;
		}
	}
	else if (cmd == "geo_run")
	{
		// Advance geoscape time while auto-draining event popups, until a stop
		// condition or a game-time budget. Requires GeoscapeState on top after
		// draining. Params: speed (0..5 idx), until (one of: minutes:<n>,
		// craft_status:<STR_key>, craft_dest:<kind>, at_base), max_minutes.
		// NOTE: real advancing happens across pump() frames; this handler sets
		// up the run and reports readiness. The driver polls geo_state between
		// geo_run calls. Here we just (a) enable host-only time so the client
		// mirrors, (b) select the requested speed, (c) drain any popup now.
		GeoscapeState* gs = findState<GeoscapeState>(_game);
		// Drain a single blocking popup if present (driver calls repeatedly).
		State* top = topState<State>(_game);
		bool drained = false;
		if (top && !dynamic_cast<GeoscapeState*>(top)
		    && !dynamic_cast<BattlescapeState*>(top))
		{
			if (auto* ev = dynamic_cast<GeoscapeEventState*>(top)) { ev->btnOkClick(nullptr); drained = true; }
			else if (dynamic_cast<ArticleState*>(top)) { _game->popState(); drained = true; }
			else if (auto* mr = dynamic_cast<MonthlyReportState*>(top)) { mr->btnOkClick(nullptr); drained = true; }
			else if (auto* md = dynamic_cast<MissionDetectedState*>(top)) { md->btnCancelClick(nullptr); drained = true; }
			// A craft reaching a site pops ConfirmLanding; decline it so the
			// craft stays airborne (status tests never enter the battle).
			else if (auto* cl = dynamic_cast<ConfirmLandingState*>(top)) { cl->btnNoClick(nullptr); drained = true; }
			else { _game->popState(); drained = true; }
		}
		resp["drained"] = drained;
		resp["topType"] = top ? typeid(*top).name() : "none";
		if (gs)
		{
			// Coop time only advances when BOTH players hold the same speed;
			// the driver calls geo_run on host AND client each tick, so just
			// select the requested speed here (no host-only override, which
			// stalls the peer's clock).
			gs->setTimeSpeedIndex(req.get("speed", 1).asInt());
			resp["ok"] = true;
		}
		else
		{
			resp["error"] = "no GeoscapeState (in popup/battle?)";
		}
	}
	else if (cmd == "geo_craft_buttons")
	{
		// Control-guard check: build the geoscape craft dialog for a craft and
		// report whether the command buttons (Return to base / Select target /
		// Patrol) are shown. A peer's coop craft must NOT show them, so a
		// non-owning player cannot redirect another player's ship. Params:
		// craft_id, coop (which copy to match - own=false, peer mirror=true).
		int craftId = req.get("craft_id", -1).asInt();
		bool wantCoop = req.get("coop", false).asBool();
		SavedGame* sg = _game->getSavedGame();
		Craft* craft = nullptr;
		if (sg)
			for (auto* b : *sg->getBases())
				for (auto* c : *b->getCrafts())
					if (c->getId() == craftId && c->coop == wantCoop) { craft = c; break; }
		GeoscapeState* geo = findState<GeoscapeState>(_game);
		if (!craft) resp["error"] = "no matching craft";
		else if (!geo) resp["error"] = "no geoscape";
		else
		{
			// Built but not pushed: the ctor configures button visibility (the
			// guard), which is all we read; then discard it.
			GeoscapeCraftState* gcs = new GeoscapeCraftState(craft, geo->getGlobe(), nullptr, false);
			resp["coop"] = craft->coop;
			resp["buttons_visible"] = gcs->testControlButtonsVisible();
			delete gcs;
			resp["ok"] = true;
		}
	}
	else if (cmd == "craft_order")
	{
		// PRD-J08: drive the REAL craft-command screens - in a JOINT campaign
		// their branches submit joint_cmds (craft_launch/craft_retarget/
		// craft_return/craft_patrol); SEPARATE/solo take the vanilla paths.
		// Params: order = "target" (with ufo_id | site_id | lon+lat) |
		// "return" | "patrol"; craft_id (+ optional craft_type).
		SavedGame* sg = _game->getSavedGame();
		GeoscapeState* geo = findState<GeoscapeState>(_game);
		int craftId = req.get("craft_id", -1).asInt();
		std::string craftType = req.get("craft_type", "").asString();
		Craft* craft = nullptr;
		if (sg)
		{
			for (auto* b : *sg->getBases())
			{
				for (auto* c : *b->getCrafts())
					if (!c->coop && c->getId() == craftId
						&& (craftType.empty() || c->getRules()->getType() == craftType))
					{ craft = c; break; }
				if (craft) break;
			}
		}
		std::string order = req.get("order", "target").asString();
		if (!sg) resp["error"] = "no saved game";
		else if (!geo) resp["error"] = "no geoscape";
		else if (!craft) resp["error"] = "no matching craft";
		else if (order == "return" || order == "patrol")
		{
			// The REAL geoscape craft dialog: its handler pops itself (net 0).
			GeoscapeCraftState* gcs = new GeoscapeCraftState(craft, geo->getGlobe(), nullptr, false);
			_game->pushState(gcs);
			if (order == "return") gcs->btnBaseClick(nullptr);
			else gcs->btnPatrolClick(nullptr);
			resp["ok"] = true;
		}
		else
		{
			Target* target = nullptr;
			if (req.isMember("ufo_id"))
			{
				int id = req["ufo_id"].asInt();
				for (auto* u : *sg->getUfos()) if (u->getId() == id) { target = u; break; }
			}
			else if (req.isMember("site_id"))
			{
				int id = req["site_id"].asInt();
				for (auto* ms : *sg->getMissionSites()) if (ms->getId() == id) { target = ms; break; }
			}
			else if (req.isMember("lon") && req.isMember("lat"))
			{
				// A fresh waypoint (id 0), exactly what SelectDestinationState
				// hands to ConfirmDestinationState; the confirm handler owns it.
				Waypoint* w = new Waypoint();
				w->setLongitude(req["lon"].asDouble());
				w->setLatitude(req["lat"].asDouble());
				target = w;
			}
			if (!target)
			{
				resp["error"] = "no matching target";
			}
			else
			{
				// Real screens: a filler InterceptState absorbs the SECOND
				// popState of ConfirmDestinationState::btnOkClick (vanilla
				// pops the destination-selection screen underneath it).
				_game->pushState(new InterceptState(geo->getGlobe(), false, nullptr, nullptr));
				ConfirmDestinationState* cds =
					new ConfirmDestinationState(std::vector<Craft*>{ craft }, target);
				_game->pushState(cds);
				cds->btnOkClick(nullptr);
				resp["ok"] = true;
			}
		}
	}
	else if (cmd == "dogfight_state")
	{
		// PRD-J08: introspect the live dogfight list (which machine holds the
		// interactive UI - in JOINT only the initiating seat may).
		GeoscapeState* geo = findState<GeoscapeState>(_game);
		if (!geo) resp["error"] = "no geoscape";
		else
		{
			Json::Value list(Json::arrayValue);
			for (auto* df : geo->getDogfights())
			{
				Json::Value jd;
				jd["ufoId"] = df->getUfo() ? df->getUfo()->getId() : -1;
				jd["craftId"] = df->getCraft() ? df->getCraft()->getId() : -1;
				jd["craftType"] = df->getCraft() ? df->getCraft()->getRules()->getType() : "";
				jd["minimized"] = df->isMinimized();
				jd["ended"] = df->dogfightEnded();
				jd["dist"] = df->harnessCurrentDist();
				jd["targetDist"] = df->harnessTargetDist();
				jd["updates"] = df->harnessUpdateCount();
				jd["disengaging"] = df->harnessEnd();
				// PRD-DF02: synced UFO attack-mode marker + replica flag + weapon states.
				jd["ufoStance"] = df->harnessUfoStance();
				jd["mode"] = df->harnessMode();
				jd["highlight"] = df->harnessHighlight(); // playtest B6
				jd["replica"] = df->isReplicaView();
				// PRD-DF03: full per-machine frame-agreement fields.
				jd["isReplicaView"] = df->isReplicaView();
				jd["currentDist"] = df->harnessCurrentDist();
				jd["ufoIsAttacking"] = df->isUfoAttacking();
				jd["projectileCount"] = df->harnessProjectileCount();
				jd["epoch"] = geo->harnessDogfightEpoch();
				Json::Value we(Json::arrayValue);
				for (int wi = 0; wi < df->harnessWeaponCount(); ++wi)
					we.append(df->harnessWeaponEnabled(wi));
				jd["weaponEnabled"] = we;
				list.append(jd);
			}
			resp["dogfights"] = list;
			resp["count"] = (int)geo->getDogfights().size();
			resp["pending"] = (int)geo->pendingDogfightCount();
			resp["epoch"] = geo->harnessDogfightEpoch();
			resp["ok"] = true;
		}
	}
	else if (cmd == "dogfight_action")
	{
		// PRD-J08/DF02/DF03: drive a live fight's mode/weapon lane. Body extracted to
		// doDogfightAction() so the deep execute() if-chain stays under the C1061 nesting
		// limit; targets a specific (craft,ufo) fight when craft_id/ufo_id is given, else front().
		doDogfightAction(findState<GeoscapeState>(_game), req, resp);
	}
	else if (cmd == "set_ufo_damage")
	{
		// PRD-J08: seed a UFO's damage (e.g. just below the crash threshold,
		// damageMax/2, so the next cannon hit crashes it deterministically).
		SavedGame* sg = _game->getSavedGame();
		int id = req.get("ufo_id", -1).asInt();
		Ufo* ufo = nullptr;
		if (sg)
			for (auto* u : *sg->getUfos())
				if (u->getId() == id) { ufo = u; break; }
		if (!ufo) resp["error"] = "no matching ufo";
		else
		{
			ufo->setDamage(req.get("damage", 0).asInt(), _game->getMod());
			resp["damage"] = ufo->getDamage();
			resp["damageMax"] = ufo->getCraftStats().damageMax;
			resp["status"] = (int)ufo->getStatus();
			resp["ok"] = true;
		}
	}
	else if (cmd == "intercept_list")
	{
		// PRD-J08 AC3: the REAL InterceptState's rows - every shared base's
		// crafts must list for every player in JOINT.
		GeoscapeState* geo = findState<GeoscapeState>(_game);
		if (!geo) resp["error"] = "no geoscape";
		else
		{
			InterceptState* is = new InterceptState(geo->getGlobe(), false, nullptr, nullptr);
			Json::Value rows(Json::arrayValue);
			for (auto* c : is->harnessListedCrafts())
			{
				Json::Value jr;
				jr["craft"] = c->getName(_game->getLanguage());
				jr["craftId"] = c->getId();
				jr["type"] = c->getRules()->getType();
				jr["base"] = c->getBase()->getName();
				jr["status"] = c->getStatus();
				rows.append(jr);
			}
			delete is;
			resp["rows"] = rows;
			resp["ok"] = true;
		}
	}
	else if (cmd == "repro_craft_uaf")
	{
		// Bug #1 shim: drives the REAL Base::removePendingTransfers + the REAL
		// Transfer dtor. Mirrors createPendingTransfers (one soldier transfer
		// for the crew + one craft transfer), then runs exactly the
		// connectionTCP "transfer_completed" removal+delete sequence and checks
		// whether the kept crew's Craft* is left dangling.
		SavedGame* sg = _game->getSavedGame();
		if (!sg) { resp["error"] = "no save loaded"; }
		else {
			Base* baseFrom = nullptr;
			for (auto* b : *sg->getBases()) if (!b->_coopBase && !b->_coopIcon) { baseFrom = b; break; }
			if (!baseFrom) resp["error"] = "no own base";
			else if (baseFrom->getCrafts()->empty()) resp["error"] = "base has no craft";
			else if (baseFrom->getSoldiers()->empty()) resp["error"] = "base has no soldier";
			else {
				Craft* craft = baseFrom->getCrafts()->front();
				Soldier* crew = baseFrom->getSoldiers()->front();
				// SETUP FIX: strip any default crew so the transfer set is complete (else validation rejects).
				for (auto* s : *baseFrom->getSoldiers())
					if (s->getCraft() == craft && s != crew) s->setCraft(nullptr);
				crew->setCraft(craft);

				// Mirror createPendingTransfers: one soldier transfer for the crew + one craft transfer.
				std::vector<Transfer*> pending;
				Transfer* st = new Transfer(6); st->setSoldier(crew); pending.push_back(st);
				Transfer* ct = new Transfer(6); ct->setCraft(craft);  pending.push_back(ct);

				// Exactly the connectionTCP "transfer_completed" sequence:
				bool removeOk = baseFrom->removePendingTransfers(&pending);
				if (removeOk) { for (Transfer* t : pending) delete t; pending.clear(); } // dtor frees _craft

				// Detect the dangling ref WITHOUT dereferencing the freed craft:
				Craft* stored = crew->getCraft();
				bool craftStillLive = false;
				for (auto* c : *baseFrom->getCrafts()) if (c == stored) { craftStillLive = true; break; }

				resp["removeOk"] = removeOk;
				resp["crewName"] = crew->getName();
				resp["crewCraftNonNull"] = (stored != nullptr);
				resp["crewCraftDangling"] = (stored != nullptr && !craftStillLive); // TRUE == bug present
				crew->setCraft(nullptr); // repair the live instance so teardown/save is safe
				resp["ok"] = true;
			}
		}
	}
	else if (cmd == "repro_receiver_ack_gap")
	{
		// Bug #2 shim: drives the REAL onTCPMessage("transfer", ...) + the REAL
		// updateCoopTask() drain. Feeds a transfer whose base_to_id exists on no
		// base and checks the receiver does not accept+queue (and would ACK) it.
		SavedGame* sg = _game->getSavedGame();
		if (!sg || !coop) { resp["error"] = "no save/coop"; }
		else {
			int bogus = 424242;
			for (auto* b : *sg->getBases()) if (b->_coop_base_id == bogus) bogus = 424243;
			int before = 0; for (auto* b : *sg->getBases()) before += (int)b->getTransfers()->size();

			Json::Value obj;
			obj["state"] = "transfer";
			obj["base_to_id"] = bogus; obj["base_from_id"] = bogus; obj["total_funds"] = 0;
			obj["items"][0]["name"] = "STR_PISTOL_CLIP"; obj["items"][0]["amount"] = 3;
			obj["items"][0]["hour"] = 1; obj["items"][0]["type"] = 0; obj["items"][0]["craft_rule"] = "";

			coop->onTCPMessage("transfer", obj);   // real receiver: accepts (items non-empty), would ACK
			coop->updateCoopTask();                // real drain: no base matches -> silently retained

			int afterNoMatch = 0; for (auto* b : *sg->getBases()) afterNoMatch += (int)b->getTransfers()->size();

			// Prove it was accepted+queued (not rejected): give a real base the bogus id, drain again.
			Base* victim = nullptr;
			for (auto* b : *sg->getBases()) if (!b->_coopBase && !b->_coopIcon) { victim = b; break; }
			bool acceptedAndQueued = false;
			if (victim) {
				int vBefore = (int)victim->getTransfers()->size();
				int saved = victim->_coop_base_id; victim->_coop_base_id = bogus;
				coop->updateCoopTask();
				acceptedAndQueued = ((int)victim->getTransfers()->size() > vBefore);
				victim->_coop_base_id = saved;
			}
			resp["bogusBaseId"] = bogus;
			resp["appliedToAnyBaseWhenNoMatch"] = (afterNoMatch > before); // FALSE == silent drop
			resp["receiverAcceptedAndQueued"] = acceptedAndQueued;         // TRUE  == bug present
			resp["ok"] = true;
		}
	}
	else
	{
		return false;
	}
	return true;
}

std::string TestServer::execute(const std::string& line)
{
	Json::Value req;
	Json::Value resp;
	resp["ok"] = false;

	Json::CharReaderBuilder rb;
	std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
	std::string errs;
	if (!reader->parse(line.data(), line.data() + line.size(), &req, &errs))
	{
		resp["error"] = "bad json: " + errs;
	}
	else
	{
		std::string cmd = req.get("cmd", "").asString();
		connectionTCP* coop = _game->getCoopMod();

		if (executeJoint10(cmd, req, resp))
		{
			// handled by the PRD-J10 dispatcher above
		}
		else if (cmd == "ping")
		{
			resp["ok"] = true;
			resp["pong"] = true;
		}
		else if (cmd == "coop_stats")
		{
			// Repro readout: TX-queue drop count (the "TX queue full" bug) plus
			// the last measured coop ping. txDropCount > 0 means the geoscape
			// heartbeat flood overran the send thread on this instance.
			resp["ok"] = true;
			resp["txDropCount"] = Json::UInt64(g_txDropCount.load());
			std::string p = coop ? coop->getPing() : std::string();
			resp["ping"] = p.empty() ? std::string("0") : p;
			resp["coopStatic"] = connectionTCP::getCoopStatic();
		}
		else if (cmd == "quit")
		{
			_game->quit();
			resp["ok"] = true;
		}
		else if (cmd == "get_state")
		{
			Json::Value states(Json::arrayValue);
			// PRD-13: left inline - dumps every state's name into a container, not a find
			for (auto* s : _game->getStates())
			{
				states.append(typeid(*s).name());
			}
			resp["states"] = states;
			resp["ok"] = true;
		}
		else if (cmd == "open_server_browser")
		{
			// Push the coop Server Browser so its rendezvous-server combobox can
			// be inspected. Requires a SavedGame (the ctor reads getCountries());
			// drivers bootstrap one via open_new_game/place_first_base first.
			if (!_game->getSavedGame())
			{
				resp["ok"] = false;
				resp["error"] = "no SavedGame; bootstrap a game before opening the browser";
			}
			else
			{
				_game->pushState(new ServerList());
				resp["ok"] = true;
			}
		}
		else if (cmd == "server_combo")
		{
			// Dump the rendezvous-server combobox state for assertions.
			ServerList* browser = findState<ServerList>(_game);

			if (!browser)
			{
				resp["ok"] = false;
				resp["error"] = "ServerList not on the state stack";
			}
			else
			{
				DisableableComboBox* combo = browser->getServerCombo();
				resp["visible"] = combo->getVisible();
				resp["selected"] = static_cast<Json::UInt>(combo->getSelected());

				Json::Value options(Json::arrayValue);
				for (size_t i = 0; i < combo->getOptionCount(); ++i)
				{
					Json::Value o;
					o["label"] = combo->getOptionLabel(i);
					o["enabled"] = combo->isEnabled(i);
					options.append(o);
				}
				resp["options"] = options;
				resp["ok"] = true;
			}
		}
		else if (cmd == "combo_open")
		{
			// Open the rendezvous-server dropdown so the disabled/greyed rows are
			// visible for a screenshot.
			ServerList* browser = findState<ServerList>(_game);
			if (!browser)
			{
				resp["ok"] = false;
				resp["error"] = "ServerList not on the state stack";
			}
			else
			{
				browser->getServerCombo()->toggle(false, false);
				resp["ok"] = true;
			}
		}
		else if (cmd == "screenshot")
		{
			// Save the current frame to a PNG for visual inspection.
			std::string path = req.get("path", "").asString();
			if (path.empty())
			{
				resp["ok"] = false;
				resp["error"] = "screenshot requires a path";
			}
			else
			{
				_game->getScreen()->screenshot(path);
				resp["ok"] = true;
				resp["path"] = path;
			}
		}
		else if (cmd == "geo_state")
		{
			// Read-only geoscape snapshot for the autonomous play driver:
			// date/funds plus this player's bases, crafts, research, and the
			// UFOs / mission sites visible on the globe. No coop side effects.
			SavedGame* sg = _game->getSavedGame();
			if (!sg)
			{
				resp["error"] = "no saved game";
			}
			else
			{
				GameTime* t = sg->getTime();
				Json::Value time;
				time["year"] = t->getYear();
				time["month"] = t->getMonth();
				time["day"] = t->getDay();
				time["hour"] = t->getHour();
				time["minute"] = t->getMinute();
				resp["time"] = time;
				resp["funds"] = Json::Value::Int64(sg->getFunds());
				resp["monthsPassed"] = sg->getMonthsPassed();
				// PRD-J11: campaign model + discovered tech, so ONE geo_state call is a
				// self-contained world dump for the joint_fixture equality helper (both
				// are world-checksum fields; the helper must not need a second command).
				resp["campaignType"] = static_cast<int>(sg->getCampaignType());
				{
					std::vector<std::string> techs;
					for (auto* r : sg->getDiscoveredResearch())
						techs.push_back(r->getName());
					std::sort(techs.begin(), techs.end());
					Json::Value jt(Json::arrayValue);
					for (const auto& t : techs)
						jt.append(t);
					resp["discoveredResearch"] = jt;
				}
				// PRD-J04: monthly settlement tails, so a JOINT month-end sync test
				// can assert the replica's funds/maintenance equal the host's. The
				// just-ended month's maintenance lives at [size-2] after the roll.
				{
					auto& maint = sg->getMaintenances();
					int64_t maintTail = maint.empty() ? 0
						: (maint.size() >= 2 ? maint[maint.size() - 2] : maint.back());
					resp["maintenanceTail"] = Json::Value::Int64(maintTail);
					resp["incomeTail"] = Json::Value::Int64(sg->getIncomes().empty() ? 0 : sg->getIncomes().back());
					resp["expenditureTail"] = Json::Value::Int64(sg->getExpenditures().empty() ? 0 : sg->getExpenditures().back());
				}

				Json::Value bases(Json::arrayValue);
				for (auto* b : *sg->getBases())
				{
					Json::Value jb;
					jb["name"] = b->getName(_game->getLanguage());
					// PRD-J02: coordinates + mirror flag, so a JOINT bootstrap/resume
					// test can assert the client replica holds the SAME real base
					// (not a _coopBase/_coopIcon mirror).
					jb["lon"] = b->getLongitude();
					jb["lat"] = b->getLatitude();
					jb["coopBase"] = b->_coopBase;
					jb["coopIcon"] = b->_coopIcon;
					// PRD-J11: the base's coop id. In JOINT, base_new mints it host-side
					// and it rides the payload, so it must be EQUAL on every machine -
					// unlike SEPARATE, where each side rolls its own.
					jb["coopBaseId"] = b->_coop_base_id;
					Json::Value crafts(Json::arrayValue);
					for (auto* c : *b->getCrafts())
					{
						Json::Value jc;
						jc["id"] = c->getId();
						jc["coopBaseId"] = b->_coop_base_id;
						jc["coop"] = c->coop;
						jc["type"] = c->getRules()->getType();
						jc["status"] = c->getStatus();
						jc["lon"] = c->getLongitude();
						jc["lat"] = c->getLatitude();
						jc["weapons"] = (int)c->getWeapons()->size();
						// PRD-J09 GAP-5b: the actual mounted craft-weapon types ("" for
						// an empty slot), so a base-screen arm/rearm converges visibly.
						{
							Json::Value wl(Json::arrayValue);
							for (auto* cw : *c->getWeapons())
								wl.append(cw ? cw->getRules()->getType() : std::string(""));
							jc["weaponLoadout"] = wl;
						}
						jc["fuel"] = c->getFuel();       // PRD-J08
						jc["damage"] = c->getDamage();   // PRD-J08
						jc["lowFuel"] = c->getLowFuel();
						jc["mission"] = c->getMissionComplete();
						jc["inDogfight"] = c->isInDogfight();
						// destination classification (owner side only; a peer's
						// craft has no replicated destination)
						std::string destKind = "none";
						int destId = -1;
						if (Target* d = c->getDestination())
						{
							if (d == (Target*)c->getBase()) { destKind = "base"; }
							else if (auto* u = dynamic_cast<Ufo*>(d)) { destKind = "ufo"; destId = u->getId(); }
							else if (auto* ms = dynamic_cast<MissionSite*>(d)) { destKind = "site"; destId = ms->getId(); }
							else { destKind = "other"; }
						}
						jc["destKind"] = destKind;
						jc["destId"] = destId;
						// the exact string the geoscape UI would show (shared code
						// path -> host and client must match)
						jc["displayStatus"] = c->getDisplayStatus(_game->getLanguage());
						crafts.append(jc);
					}
					jb["crafts"] = crafts;
					Json::Value research(Json::arrayValue);
					for (auto* rp : b->getResearch())
					{
						Json::Value jr;
						jr["name"] = rp->getRules()->getName();
						jr["spent"] = rp->getSpent();
						jr["cost"] = rp->getCost();
						jr["assigned"] = rp->getAssigned(); // PRD-J06
						research.append(jr);
					}
					jb["research"] = research;
					// PRD-J06: running productions (engineers/qty/progress) so a JOINT
					// research/manufacture test can assert host and replica agree.
					Json::Value productions(Json::arrayValue);
					for (auto* prod : b->getProductions())
					{
						Json::Value jp;
						jp["item"] = prod->getRules()->getName();
						jp["engineers"] = prod->getAssignedEngineers();
						jp["amount"] = prod->getAmountTotal();
						jp["infinite"] = prod->getInfiniteAmount();
						jp["sell"] = prod->getSellItems();
						jp["timeSpent"] = prod->getTimeSpent();
						jp["produced"] = prod->getAmountProduced();
						productions.append(jp);
					}
					jb["productions"] = productions;
					// PRD-J04: facilities (buildTime + grid position) so a JOINT
					// replica-freeze test can prove construction days-left change only
					// via fac_done, never locally.
					Json::Value facilities(Json::arrayValue);
					for (auto* f : *b->getFacilities())
					{
						Json::Value jf;
						jf["type"] = f->getRules()->getType();
						jf["x"] = f->getX();
						jf["y"] = f->getY();
						jf["sizeX"] = f->getRules()->getSizeX(); // PRD-J07
						jf["sizeY"] = f->getRules()->getSizeY(); // PRD-J07
						jf["buildTime"] = f->getBuildTime();
						facilities.append(jf);
					}
					jb["facilities"] = facilities;
					// PRD-J11: base stores + the full pending-transfer list. Neither is a
					// world-checksum field (the checksum is funds + base count + tech
					// count), so drift here is invisible to the J10 auto-repair - which is
					// exactly why the fixture's equality helper has to read them.
					// `incoming_transfers` only aggregates items by type for one base; the
					// helper needs every base, every transfer kind, with hours left.
					Json::Value items(Json::objectValue);
					for (const auto& pair : *b->getStorageItems()->getContents())
						items[pair.first->getType()] = pair.second;
					jb["items"] = items;
					Json::Value transfers(Json::arrayValue);
					for (auto* t : *b->getTransfers())
					{
						Json::Value jt;
						jt["type"] = (int)t->getType();
						jt["qty"] = t->getQuantity();
						jt["hours"] = t->getHours();
						jt["rule"] = transferRuleName(t);
						transfers.append(jt);
					}
					jb["transfers"] = transfers;
					jb["freeScientists"] = b->getScientists();
					jb["freeEngineers"] = b->getEngineers();     // PRD-J06
					jb["freeLab"] = b->getFreeLaboratories();     // PRD-J06
					jb["freeWorkshop"] = b->getFreeWorkshops();   // PRD-J06
					jb["soldiers"] = (int)b->getSoldiers()->size();
					bases.append(jb);
				}
				resp["bases"] = bases;

				Json::Value ufos(Json::arrayValue);
				for (auto* u : *sg->getUfos())
				{
					Json::Value ju;
					ju["id"] = u->getId();
					ju["coopId"] = u->getCoopUfoId();
					ju["type"] = u->getRules()->getType();
					ju["detected"] = u->getDetected();
					ju["status"] = (int)u->getStatus();
					ju["lon"] = u->getLongitude();
					ju["lat"] = u->getLatitude();
					ju["coop"] = u->getCoop();
					ju["crashId"] = u->getCrashId();   // PRD-J08
					ju["damage"] = u->getDamage();     // PRD-J08
					ufos.append(ju);
				}
				resp["ufos"] = ufos;

				Json::Value sites(Json::arrayValue);
				for (auto* ms : *sg->getMissionSites())
				{
					Json::Value jm;
					jm["id"] = ms->getId();
					jm["coopId"] = ms->getCoopMissionId();
					jm["type"] = ms->getType();
					jm["race"] = ms->getAlienRace();
					jm["city"] = ms->getCity();
					sites.append(jm);
				}
				resp["missionSites"] = sites;
				resp["ok"] = true;
			}
		}
		else if (cmd == "month_report")
		{
			// End-of-month score + per-country funding/activity, read from the
			// SavedGame (valid whether or not MonthlyReportState is on screen).
			// Used to cross-validate that host and client agree on the shared
			// monthly outcome (score, funding changes) even though each player's
			// absolute funds are independent.
			SavedGame* sg = _game->getSavedGame();
			if (!sg)
			{
				resp["error"] = "no saved game";
			}
			else
			{
				int mp = sg->getMonthsPassed();
				resp["monthsPassed"] = mp;
				resp["score"] = sg->getCurrentScore(mp);
				resp["funds"] = Json::Value::Int64(sg->getFunds());
				Json::Value countries(Json::arrayValue);
				for (auto* c : *sg->getCountries())
				{
					Json::Value jc;
					jc["name"] = c->getRules()->getType();
					std::vector<int>& fund = c->getFunding();
					jc["funding"] = fund.empty() ? 0 : fund.back();
					jc["fundingChange"] = fund.size() >= 2 ? (fund.back() - fund[fund.size() - 2]) : 0;
					std::vector<int>& ax = c->getActivityXcom();
					std::vector<int>& aa = c->getActivityAlien();
					jc["activityXcom"] = ax.empty() ? 0 : ax.back();
					jc["activityAlien"] = aa.empty() ? 0 : aa.back();
					countries.append(jc);
				}
				resp["countries"] = countries;
				resp["ok"] = true;
			}
		}
		else if (cmd == "geo_set_speed")
		{
			// Select a geoscape time-speed (0=5s..5=1day). In coop, time only
			// advances fast when BOTH players pick the SAME speed; the driver
			// calls this on host+client together, then lets the real timers run.
			int idx = req.get("idx", 0).asInt();
			GeoscapeState* gs = findState<GeoscapeState>(_game);
			if (!gs)
				resp["error"] = "no GeoscapeState on stack (in a popup/battle?)";
			else
			{
				gs->setTimeSpeedIndex(idx);
				resp["ok"] = true;
			}
		}
		else if (cmd == "craft_dispatch")
		{
			// Assign up to N soldiers to the own base's first ready craft and
			// send it to the mission site with the given id. Geoscape flight +
			// arrival (ConfirmLandingState) happen as time advances.
			int siteId = req.get("site_id", -1).asInt();
			int nsol = req.get("soldiers", 2).asInt();
			SavedGame* sg = _game->getSavedGame();
			Base* base = nullptr; Craft* craft = nullptr;
			if (sg)
			{
				for (auto* b : *sg->getBases())
				{
					if (!b->getCrafts()->empty() && !b->getSoldiers()->empty())
					{
						base = b; craft = b->getCrafts()->front(); break;
					}
				}
			}
			MissionSite* site = nullptr;
			if (sg)
				for (auto* ms : *sg->getMissionSites())
					if (ms->getId() == siteId) site = ms;
			if (!craft)
				resp["error"] = "no base craft with soldiers";
			else if (!site)
				resp["error"] = "no mission site id";
			else
			{
				int assigned = 0;
				for (auto* s : *base->getSoldiers())
				{
					if (assigned >= nsol) break;
					if (s->getCraft() == craft) { assigned++; continue; }
					if (s->getCraft() == nullptr && craft->getSpaceAvailable() > 0)
					{
						s->setCraft(craft);
						assigned++;
					}
				}
				craft->setDestination(site);
				resp["assigned"] = assigned;
				resp["craft"] = craft->getRules()->getType();
				resp["ok"] = true;
			}
		}
		else if (cmd == "set_soldier_owner")
		{
			// PRD-J09 scaffolding: stamp a soldier's ownerPlayerId (seat) by id.
			// Call on BOTH machines to build a deterministic mixed-owner squad
			// (the shared world stays byte-identical), standing in for a real hire
			// until a JOINT battle exercises ownership end to end.
			int sid = req.get("soldier_id", -1).asInt();
			int owner = req.get("owner", 999).asInt();
			bool found = false;
			if (_game->getSavedGame())
				for (auto* b : *_game->getSavedGame()->getBases())
					for (auto* s : *b->getSoldiers())
						if (s->getId() == sid) { s->setOwnerPlayerId(owner); found = true; }
			resp["ok"] = found;
			if (!found) resp["error"] = "soldier id not found";
		}
		else if (cmd == "craft_assign")
		{
			// PRD-J09: mixed-owner squad assembly. Toggle a soldier (host's OR
			// client's, by stable id) on/off a base craft via the REAL
			// CraftSoldiersState path. In JOINT this submits craft_assign (shared
			// world); the caller then verifies both machines agree after apply.
			int soldierId = req.get("soldier_id", -1).asInt();
			int craftId = req.get("craft_id", -1).asInt();
			SavedGame* sg = _game->getSavedGame();
			Base* base = nullptr;
			size_t craftIdx = 0;
			bool found = false;
			if (sg)
			{
				for (auto* b : *sg->getBases())
				{
					for (size_t i = 0; i < b->getCrafts()->size(); ++i)
						if (b->getCrafts()->at(i)->getId() == craftId) { base = b; craftIdx = i; found = true; break; }
					if (found) break;
				}
			}
			Soldier* sol = nullptr;
			if (base)
				for (auto* s : *base->getSoldiers())
					if (s->getId() == soldierId) { sol = s; break; }
			if (!found)
				resp["error"] = "craft id not found";
			else if (!sol)
				resp["error"] = "soldier id not on craft's base";
			else
			{
				// Idempotent: only drive the (toggle) UI path if the current
				// assignment differs from the desired "on" state, so the test is
				// deterministic regardless of the soldier's starting assignment.
				bool want = req.get("on", true).asBool();
				bool cur = (sol->getCraft() == base->getCrafts()->at(craftIdx));
				if (cur != want)
				{
					CraftSoldiersState* st = new CraftSoldiersState(base, craftIdx);
					st->harnessToggle(soldierId);
					delete st;
				}
				resp["ok"] = true;
			}
		}
		else if (cmd == "confirm_landing")
		{
			ConfirmLandingState* cl = findState<ConfirmLandingState>(_game);
			if (!cl)
				resp["error"] = "no ConfirmLandingState on stack";
			else
			{
				cl->btnYesClick(nullptr);
				resp["ok"] = true;
			}
		}
		else if (cmd == "battle_inventory")
		{
			// Pre-battle coop inventory (soldiers spawn unarmed, weapons on the
			// ground). action=autoequip_all cycles units auto-equipping each from
			// the ground pile; action=ok closes it and starts the tactical turn.
			InventoryState* inv = findState<InventoryState>(_game);
			if (!inv)
			{
				resp["error"] = "no InventoryState on stack";
			}
			else
			{
				std::string act = req.get("action", "").asString();
				if (act == "autoequip_all")
				{
					int n = req.get("times", 8).asInt();
					for (int i = 0; i < n; i++)
					{
						inv->onAutoequip(nullptr);
						inv->btnNextClick(nullptr);
					}
					resp["ok"] = true;
				}
				else if (act == "ok")
				{
					inv->btnOkClick(nullptr);
					resp["ok"] = true;
				}
				else
					resp["error"] = "unknown inventory action";
			}
		}
		else if (cmd == "close_briefing")
		{
			BriefingState* br = findState<BriefingState>(_game);
			if (!br)
				resp["error"] = "no BriefingState on stack";
			else
			{
				br->btnOkClick(nullptr);
				resp["ok"] = true;
			}
		}
		else if (cmd == "battle_state")
		{
			SavedGame* sg = _game->getSavedGame();
			SavedBattleGame* bg = sg ? sg->getSavedBattle() : nullptr;
			if (!bg)
			{
				resp["inBattle"] = false;
				resp["ok"] = true;
			}
			else
			{
				resp["inBattle"] = true;
				resp["turn"] = bg->getTurn();
				resp["side"] = (int)bg->getSide();
				resp["missionType"] = bg->getMissionType();
				resp["coopTurn"] = BattlescapeGame::isYourTurn;  // 2 = my active turn
				const BattleUnit* sel = bg->getSelectedUnit();
				resp["selectedId"] = sel ? sel->getId() : -1;
				Json::Value units(Json::arrayValue);
				for (auto* u : *bg->getUnits())
				{
					Json::Value ju;
					ju["id"] = u->getId();
					ju["faction"] = (int)u->getFaction();
					ju["status"] = (int)u->getStatus();
					ju["isOut"] = u->isOut();
					ju["health"] = u->getHealth();
					ju["tu"] = u->getTimeUnits();
					ju["stun"] = u->getStunlevel();
					ju["name"] = u->getName(_game->getLanguage());
					ju["isPlayerSoldier"] = (u->getGeoscapeSoldier() != nullptr);
					// PRD-J09: in-battle control split. _coop 0 = host-controlled,
					// 1 = client-controlled; in JOINT it is derived from the owning
					// soldier's ownerPlayerId (seat) at mission start.
					ju["coop"] = u->getCoop();
					ju["soldierId"] = u->getGeoscapeSoldier() ? u->getGeoscapeSoldier()->getId() : -1;
					ju["owner"] = u->getGeoscapeSoldier() ? u->getGeoscapeSoldier()->getOwnerPlayerId() : -1;
					BattleItem* w = u->getMainHandWeapon(false);
					ju["weapon"] = w ? w->getRules()->getType() : "";
					// kill attribution (for coop outcome cross-validation)
					ju["murdererId"] = u->getMurdererId();
					ju["killedBy"] = (int)u->killedBy();
					Position p = u->getPosition();
					ju["x"] = p.x; ju["y"] = p.y; ju["z"] = p.z;
					units.append(ju);
				}
				resp["units"] = units;
				// Spotted hostiles: union of what all player units currently see.
				// The tactical driver targets only these (no omniscient wall-shots).
				std::set<int> spotted;
				for (auto* u : *bg->getUnits())
				{
					if (u->getFaction() != FACTION_PLAYER || u->isOut()) continue;
					for (auto* v : *u->getVisibleUnits())
						if (v->getFaction() == FACTION_HOSTILE) spotted.insert(v->getId());
				}
				Json::Value sp(Json::arrayValue);
				for (int id : spotted) sp.append(id);
				resp["spotted"] = sp;
				resp["ok"] = true;
			}
		}
		else if (cmd == "battle_action")
		{
			// Unified battlescape action driver. action = select|move|shoot|
			// end_turn|abort. Reaches the BattlescapeGame via the top
			// BattlescapeState. All ops are on the main thread (race-free).
			BattlescapeGame* bg = nullptr;
			BattlescapeState* bstate = nullptr;
			// PRD-13: left inline - assigns two vars + calls getBattleGame() per match, not pure find-last
			for (auto* s : _game->getStates())
				if (auto* bs = dynamic_cast<BattlescapeState*>(s)) { bstate = bs; bg = bs->getBattleGame(); }
			SavedBattleGame* sbg = bg ? bg->getSave() : nullptr;
			std::string act = req.get("action", "").asString();
			if (!bg || !sbg)
			{
				resp["error"] = "not in battlescape";
			}
			else if (act == "end_turn")
			{
				bg->requestEndTurn(false);
				resp["ok"] = true;
			}
			else if (act == "abort")
			{
				// Proper abort: open the confirm dialog (AbortMissionState). The
				// driver then confirms it via dismiss_popup -> btnOkClick ->
				// finishBattle (recovers living units in the exit/craft zone).
				// (setAborted+requestEndTurn alone never ends the mission.)
				if (bstate)
				{
					bstate->btnAbortClick(nullptr);
					resp["ok"] = true;
				}
				else
					resp["error"] = "no BattlescapeState";
			}
			else
			{
				// actions needing a unit
				int uid = req.get("unit", -1).asInt();
				BattleUnit* unit = nullptr;
				for (auto* u : *sbg->getUnits())
					if (u->getId() == uid) unit = u;
				if (!unit)
					resp["error"] = "no unit id";
				else if (act == "select")
				{
					sbg->setSelectedUnit(unit);
					resp["ok"] = true;
				}
				else if (act == "move")
				{
					Position dest(req.get("x", 0).asInt(), req.get("y", 0).asInt(), req.get("z", 0).asInt());
					sbg->setSelectedUnit(unit);
					BattleAction* a = bg->getCurrentAction();
					a->actor = unit; a->target = dest; a->type = BA_WALK;
					a->targeting = false; a->run = false; a->strafe = false;
					bg->getPathfinding()->calculate(a->actor, a->target, a->getMoveType());
					if (bg->getPathfinding()->getStartDirection() == -1)
						resp["error"] = "no path to target";
					else
					{
						bg->statePushBack(new UnitWalkBState(bg, *a));
						resp["ok"] = true;
					}
				}
				else if (act == "shoot")
				{
					int tid = req.get("target", -1).asInt();
					BattleUnit* tgt = nullptr;
					for (auto* u : *sbg->getUnits())
						if (u->getId() == tid) tgt = u;
					BattleItem* w = unit->getMainHandWeapon(false);
					if (!tgt)
						resp["error"] = "no target id";
					else if (!w)
						resp["error"] = "actor has no weapon in hand";
					else
					{
						std::string mode = req.get("mode", "snap").asString();
						BattleActionType bt = BA_SNAPSHOT;
						if (mode == "aimed") bt = BA_AIMEDSHOT;
						else if (mode == "auto") bt = BA_AUTOSHOT;
						sbg->setSelectedUnit(unit);
						BattleAction* a = bg->getCurrentAction();
						a->actor = unit; a->weapon = w; a->type = bt;
						a->targeting = true; a->target = tgt->getPosition();
						a->updateTU();
						resp["tuCost"] = a->Time;
						resp["tuHave"] = unit->getTimeUnits();
						bg->statePushBack(new UnitTurnBState(bg, *a));
						bg->statePushBack(new ProjectileFlyBState(bg, *a));
						resp["ok"] = true;
					}
				}
				else
					resp["error"] = "unknown battle action";
			}
		}
		else if (cmd == "dismiss_popup")
		{
			// Confirm/close the top geoscape popup (event intro, etc.). Handled
			// types grow as the play driver discovers them. Reports the type so
			// unknown popups surface instead of silently hanging.
			State* top = topState<State>(_game);
			resp["type"] = top ? typeid(*top).name() : "none";
			if (auto* ev = dynamic_cast<GeoscapeEventState*>(top))
			{
				ev->btnOkClick(nullptr);
				resp["handled"] = "GeoscapeEventState";
				resp["ok"] = true;
			}
			else if (dynamic_cast<ArticleState*>(top))
			{
				// Ufopaedia article (event reward / intro) — read-only display,
				// btnOkClick is protected, so just pop it (same effect: return
				// to the state underneath, ultimately the geoscape).
				_game->popState();
				resp["handled"] = "ArticleState";
				resp["ok"] = true;
			}
			else if (auto* mr = dynamic_cast<MonthlyReportState*>(top))
			{
				mr->btnOkClick(nullptr);
				resp["handled"] = "MonthlyReportState";
				resp["ok"] = true;
			}
			else if (auto* md = dynamic_cast<MissionDetectedState*>(top))
			{
				// New mission-site alert. Default action = skip (cancel); the
				// site stays on the globe. The play driver decides per site type
				// whether to instead dispatch a craft (engage) separately.
				md->btnCancelClick(nullptr);
				resp["handled"] = "MissionDetectedState";
				resp["ok"] = true;
			}
			else if (auto* ud = dynamic_cast<UfoDetectedState*>(top))
			{
				// New UFO alert. Cancel (not Intercept) = acknowledge and close;
				// the UFO stays on the globe. btnCancelClick sets the acknowledged
				// flag so it does not immediately re-alert.
				ud->btnCancelClick(nullptr);
				resp["handled"] = "UfoDetectedState";
				resp["ok"] = true;
			}
			else if (dynamic_cast<NextTurnState*>(top))
			{
				// Transient "Turn N" screen — the turn already advanced when it
				// was created; just pop it to reach the tactical map.
				_game->popState();
				resp["handled"] = "NextTurnState";
				resp["ok"] = true;
			}
			else if (auto* ab = dynamic_cast<AbortMissionState*>(top))
			{
				ab->btnOkClick(nullptr);
				resp["handled"] = "AbortMissionState";
				resp["ok"] = true;
			}
			else if (auto* db = dynamic_cast<DebriefingState*>(top))
			{
				db->btnOkClick(nullptr);
				resp["handled"] = "DebriefingState";
				resp["ok"] = true;
			}
			else if (!top || dynamic_cast<GeoscapeState*>(top))
			{
				// Geoscape itself on top: nothing to dismiss.
				resp["handled"] = "none";
				resp["ok"] = true;
			}
			else if (dynamic_cast<CoopState*>(top))
			{
				// Coop WAIT dialog (map download / month save-progress sync). It
				// auto-closes; never pop it. Caller should wait.
				resp["wait"] = true;
				resp["error"] = "coop wait dialog (auto-closes; not dismissable)";
			}
			else
			{
				// Unknown geoscape popup: generically close it so "skip all
				// dialogs" stays robust as new popups appear. Reported as
				// "generic" + the typeid so new dialog types can be reviewed and
				// given explicit handling if the raw pop is ever wrong.
				_game->popState();
				resp["handled"] = "generic";
				resp["ok"] = true;
			}
		}
		else if (cmd == "get_coop")
		{
			resp["coopStatic"] = coop->getCoopStatic();
			resp["coopCampaign"] = coop->getCoopCampaign();
			resp["host"] = connectionTCP::getHost();
			resp["serverOwner"] = connectionTCP::getServerOwner();
			resp["onConnect"] = coop->isConnected();
			resp["sessionLocked"] = connectionTCP::session.sessionLocked;
			resp["playerReady"] = connectionTCP::isPlayerReady;
			resp["playersReady"] = connectionTCP::isPlayersReady;
			resp["lobbyClosed"] = connectionTCP::session.lobbyClosed;
			resp["lobbyFileStatus"] = connectionTCP::LobbyFileStatus;
			resp["lobbyMode"] = connectionTCP::session.lobbyMode;
			resp["resumeAck"] = connectionTCP::session.resumeAck;
			resp["coopSession"] = coop->isCoopSession();
			resp["hasSave"] = _game->getSavedGame() != nullptr;
			resp["inBattle"] = _game->getSavedGame() && _game->getSavedGame()->getSavedBattle();
			resp["hostName"] = coop->getHostName();
			resp["clientName"] = coop->getCurrentClientName();
			resp["insideCoopBase"] = coop->playerInsideCoopBase;
			resp["saveID"] = Json::Value::Int64(connectionTCP::saveID);
			resp["pendingHostSaveName"] = connectionTCP::session.pendingHostSaveName;
			// PRD-J09: post-battle / mission-lifecycle introspection.
			resp["coopMissionEnd"] = coop->coopMissionEnd;
			resp["readyCoopBattle"] = coop->ready_coop_battle;
			resp["isLoadProgress"] = coop->_isLoadProgress;
			resp["joint"] = coop->isJointCampaign();
			{
				CoopState* top = findState<CoopState>(_game);
				resp["coopDialog"] = top ? top->getStateCode() : -1;
			}
			resp["ok"] = true;
		}
		else if (cmd == "load_save")
		{
			std::string file = req.get("file", "").asString();
			SavedGame* s = new SavedGame();
			s->load(file, _game->getMod(), _game->getLanguage());
			coop->resetGiftSessionState();
			_game->setSavedGame(s);
			_game->setState(new GeoscapeState);
			resp["ok"] = true;
		}
		else if (cmd == "host_tcp")
		{
			std::string server = req.get("server", "TestServer").asString();
			std::string port = req.get("port", "3000").asString();
			std::string player = req.get("player", "HostPlayer").asString();

			// campaign when a real campaign save is loaded (same check as HostMenu)
			bool campaign = _game->getSavedGame() && !_game->getSavedGame()->getCountries()->empty();

			// Legacy-save migration: an empty host slot is unclaimed - the
			// re-hosting player claims it (mirrors HostMenu::hostTCPGame)
			if (campaign
				&& !_game->getSavedGame()->getCoopPlayers().empty()
				&& _game->getSavedGame()->getCoopPlayers()[0].empty())
			{
				std::vector<std::string> players = _game->getSavedGame()->getCoopPlayers();
				players[0] = player;
				_game->getSavedGame()->setCoopPlayers(players);
				Log(LOG_INFO) << "[coop-migrate] locked legacy roster host slot to '" << player << "'";
			}

			// flow-redesign D1: solo campaigns can never be hosted as co-op
			if (campaign && !_game->getSavedGame()->isCoopSave())
			{
				resp["error"] = "solo campaign cannot be hosted (D1)";
			}
			// D4: the host identity is locked to the save
			else if (campaign
				&& !_game->getSavedGame()->getCoopPlayers().empty()
				&& _game->getSavedGame()->getCoopPlayers()[0] != player)
			{
				resp["error"] = "campaign can only be hosted by " + _game->getSavedGame()->getCoopPlayers()[0] + " (D4)";
			}
			else
			{
				// same lobby-mode derivation as HostMenu::hostTCPGame
				if (campaign)
				{
					connectionTCP::session.lobbyMode = _game->getSavedGame()->getCoopPlayers().empty() ? 1 : 2;
				}
				// optional room password (default: open server, like an empty UI box)
				connectionTCP::password = req.get("password", "").asString();
				connectionTCP::isPasswordRequired = !connectionTCP::password.empty();
				connectionTCP::_coopGamemode = 1; // PVE
				coop->setCoopSession(false);
				coop->setPlayerTurn(3);
				coop->setHostName(player);
				coop->setCoopCampaign(campaign);
				coop->hostTCPServer(server, port);
				coop->setServerOwner(true);
				if (campaign)
				{
					// replace an open HostMenu with the lobby (the UI path
					// does this via hostTCPGame)
					if (topState<HostMenu>(_game))
					{
						_game->popState();
					}
					_game->pushState(new LobbyMenu());
				}
				resp["campaign"] = campaign;
				resp["ok"] = true;
			}
		}
		else if (cmd == "host_menu_host")
		{
			// drive the real host window: pick a connection type
			// (0 TCP, 1 UDP private, 2 UDP public, 3 hotseat) and START HOST
			HostMenu* hm = topState<HostMenu>(_game);
			if (!hm)
			{
				resp["error"] = "no HostMenu on top";
			}
			else
			{
				hm->testHostWithVisibility(req.get("visibility", 0).asInt());
				resp["ok"] = true;
			}
		}
		else if (cmd == "host_menu_state")
		{
			HostMenu* hm = findState<HostMenu>(_game);
			resp["open"] = (hm != nullptr);
			resp["controlsVisible"] = hm ? hm->hostControlsVisible() : false;
			resp["ok"] = true;
		}
		else if (cmd == "host_menu_cancel")
		{
			HostMenu* hm = topState<HostMenu>(_game);
			if (!hm)
			{
				resp["error"] = "no HostMenu on top";
			}
			else
			{
				hm->btnCancelClick(nullptr);
				resp["ok"] = true;
			}
		}
		else if (cmd == "lobby_disconnect")
		{
			LobbyMenu* lobby = topState<LobbyMenu>(_game);
			if (!lobby)
			{
				resp["error"] = "no LobbyMenu on top";
			}
			else
			{
				lobby->btnDisconnectClick(nullptr);
				resp["ok"] = true;
			}
		}
		else if (cmd == "disconnect_to_menu")
		{
			// Return this instance to the main menu the way the in-game "abandon
			// to main menu" path does: drop the transport and reset the session.
			// MainMenuState::init() also calls resetSession(), so this exercises
			// the real teardown that must return the process to a pristine coop
			// identity (saveID 0, blob maps empty) for a second campaign.
			coop->disconnectTCP(true);
			coop->setServerOwner(false);
			connectionTCP::session.resetSession();
			_game->setState(new MainMenuState);
			resp["ok"] = true;
		}
		else if (cmd == "coop_dialog_info")
		{
			// introspect the topmost CoopState dialog anywhere on the stack:
			// its code, title text, back-button text + visibility. Used by the
			// harness to assert dialog wording/scaling and to catch a lingering
			// "Connecting..." (code 15) buried under the lobby.
			CoopState* cs = findState<CoopState>(_game);
			if (!cs)
			{
				resp["present"] = false;
			}
			else
			{
				resp["present"] = true;
				resp["code"] = cs->getStateCode();
				resp["title"] = cs->getTitleText();
				resp["backText"] = cs->getBackText();
				resp["backVisible"] = cs->isBackVisible();
				resp["windowHeight"] = cs->getWindowHeight();
			}
			resp["ok"] = true;
		}
		else if (cmd == "coop_push_connecting")
		{
			// Mirror the real join UI (ServerList/DirectConnect), which pushes a
			// "Connecting..." wait dialog (CoopState 15) before kicking off the
			// async connect. join_tcp bypasses that UI, so tests that need the
			// connecting-dialog scenario (e.g. the lingering-window bug) push it
			// explicitly right before join_tcp.
			_game->pushState(new CoopState(15));
			resp["ok"] = true;
		}
		else if (cmd == "password_join")
		{
			// fill the PasswordCheckMenu password box and click JOIN, like a
			// player answering the host's password challenge
			PasswordCheckMenu* pw = topState<PasswordCheckMenu>(_game);
			if (!pw)
			{
				resp["error"] = "no PasswordCheckMenu on top";
			}
			else
			{
				pw->submitPassword(req.get("password", "").asString());
				resp["ok"] = true;
			}
		}
		else if (cmd == "coop_dialog_back")
		{
			// click the back/RESUME/OK button of the top CoopState dialog
			CoopState* cs = topState<CoopState>(_game);
			if (!cs)
			{
				resp["error"] = "no CoopState on top";
			}
			else
			{
				cs->previous(nullptr);
				resp["ok"] = true;
			}
		}
		else if (cmd == "join_tcp")
		{
			std::string ipaddr = req.get("ip", "127.0.0.1").asString();
			std::string port = req.get("port", "3000").asString();
			std::string player = req.get("player", "ClientPlayer").asString();

			coop->setCoopSession(false);
			coop->setPlayerTurn(3);
			coop->setHostName(player);
			bool campaign = _game->getSavedGame() && !_game->getSavedGame()->getCountries()->empty();
			coop->setCoopCampaign(campaign);
			coop->connectTCPServer(ipaddr, port);
			resp["ok"] = true;
		}
		else if (cmd == "profile_ok")
		{
			Profile* profile = findState<Profile>(_game);
			if (profile)
			{
				profile->buttonOK(nullptr);
				resp["ok"] = true;
			}
			else
			{
				resp["error"] = "no Profile in state stack";
			}
		}
		else if (cmd == "open_new_game")
		{
			// mode: "solo" (default), "coop"/"coop_separate" or
			// "coop_joint"/"joint" - mirrors the New Game dropdown (D1; PRD-J01).
			std::string mode = req.get("mode", "solo").asString();
			bool coop = (mode == "coop" || mode == "coop_separate"
				|| mode == "coop_joint" || mode == "joint");
			CoopCampaignType ct = (mode == "coop_joint" || mode == "joint")
				? CoopCampaignType::Joint : CoopCampaignType::Separate;
			_game->pushState(new NewGameState(coop, ct));
			resp["ok"] = true;
		}
		else if (cmd == "newgame_ok")
		{
			NewGameState* ng = findState<NewGameState>(_game);
			if (ng)
			{
				ng->btnOkClick(nullptr);
				resp["ok"] = true;
			}
			else
			{
				resp["error"] = "no NewGameState in state stack";
			}
		}
		else if (cmd == "place_first_base")
		{
			double lon = req.get("lon", 0.0).asDouble();
			double lat = req.get("lat", 0.0).asDouble();
			std::string name = req.get("name", "TestBase").asString();

			BuildNewBaseState* build = findState<BuildNewBaseState>(_game);
			if (!build)
			{
				resp["error"] = "no BuildNewBaseState in state stack";
			}
			else if (!build->placeAt(lon, lat))
			{
				resp["error"] = "coordinates not on land";
			}
			else
			{
				// placeAt pushed BaseNameState (first base); confirm the name.
				BaseNameState* nameState = findState<BaseNameState>(_game);
				if (nameState)
				{
					nameState->setNameAndConfirm(name);
					resp["ok"] = true;
				}
				else
				{
					resp["error"] = "BaseNameState not pushed after placement";
				}
			}
		}
		else if (cmd == "lobby_ready")
		{
			LobbyMenu* lobby = findState<LobbyMenu>(_game);
			if (lobby)
			{
				lobby->btnCancelClick(nullptr);
				resp["ok"] = true;
			}
			else
			{
				resp["error"] = "no LobbyMenu in state stack";
			}
		}
		else if (cmd == "get_soldiers")
		{
			if (!_game->getSavedGame())
			{
				resp["error"] = "no save loaded";
			}
			else
			{
				Json::Value bases(Json::arrayValue);
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					Json::Value b;
					b["name"] = base->getName();
					b["coopBaseFlag"] = base->_coopBase;
					b["coopIcon"] = base->_coopIcon;
					b["coopBaseId"] = base->_coop_base_id;
					Json::Value soldiers(Json::arrayValue);
					for (auto* s : *base->getSoldiers())
					{
						soldiers.append(soldierToJson(s));
					}
					b["soldiers"] = soldiers;
					bases.append(b);
				}
				resp["bases"] = bases;
				resp["ok"] = true;
			}
		}
		else if (cmd == "get_mirror_soldiers")
		{
			// What the mirror-base visit view would list: soldiers in THIS
			// machine's save stationed at the given coop base id (the exact
			// source set CoopState(55) deep-copies into the visited base).
			int coopBaseId = req.get("coopBaseId", -1).asInt();
			if (!_game->getSavedGame())
			{
				resp["error"] = "no save loaded";
			}
			else
			{
				Json::Value soldiers(Json::arrayValue);
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					for (auto* s : *base->getSoldiers())
					{
						if (s->getCoopBase() == coopBaseId)
						{
							soldiers.append(soldierToJson(s));
						}
					}
				}
				resp["soldiers"] = soldiers;
				resp["ok"] = true;
			}
		}
		else if (cmd == "open_soldiers")
		{
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
			{
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false) : base->getName() == baseName)
					{
						target = base;
						break;
					}
				}
			}
			if (target)
			{
				_game->pushState(new SoldiersState(target));
				resp["ok"] = true;
			}
			else
			{
				resp["error"] = "base not found: " + baseName;
			}
		}
		else if (cmd == "soldiers_ok")
		{
			SoldiersState* st = findState<SoldiersState>(_game);
			if (st)
			{
				st->btnOkClick(nullptr);
				resp["ok"] = true;
			}
			else
			{
				resp["error"] = "no SoldiersState in state stack";
			}
		}
		else if (cmd == "base_report")
		{
			// Read-only snapshot of a base's equipment pool + its soldiers'
			// reserved (equipment-layout) items. Proves issue #33: a visited
			// coop base must not expose items reserved by the peer's soldiers'
			// loadouts. Match: coop=true -> first visited coop base; else base
			// name if given; else the local own base.
			std::string reportBase = req.get("base", "").asString();
			bool wantCoop = req.get("coop", false).asBool();
			Base* target = nullptr;
			if (_game->getSavedGame())
			{
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					bool match;
					if (wantCoop)
						match = base->_coopBase;
					else if (!reportBase.empty())
						match = (base->getName() == reportBase);
					else
						match = (base->_coopBase == false && base->_coopIcon == false);
					if (match) { target = base; break; }
				}
			}
			if (!target)
			{
				resp["error"] = "base not found";
			}
			else
			{
				resp["name"] = target->getName();
				resp["coopBaseFlag"] = target->_coopBase;
				resp["coopBaseId"] = target->_coop_base_id;

				Json::Value storage(Json::objectValue);
				for (const auto& pair : *target->getStorageItems()->getContents())
					storage[pair.first->getType()] = pair.second;
				resp["storage"] = storage;

				Json::Value reserved(Json::objectValue);
				Json::Value soldiers(Json::arrayValue);
				for (auto* s : *target->getSoldiers())
				{
					Json::Value js;
					js["name"] = s->getName();
					js["owner"] = s->getOwnerPlayerId();
					js["coopBase"] = s->getCoopBase();
					js["id"] = s->getId();
					js["type"] = s->getRules()->getType();
					// PRD-J05: a compact stats fingerprint so a hire test can assert
					// the host-generated soldier is byte-identical on the replica
					// (reconstructed from the serialized YAML, never re-rolled).
					{
						const UnitStats* st = s->getCurrentStats();
						Json::Value stats(Json::objectValue);
						stats["tu"] = st->tu;
						stats["stamina"] = st->stamina;
						stats["health"] = st->health;
						stats["bravery"] = st->bravery;
						stats["reactions"] = st->reactions;
						stats["firing"] = st->firing;
						stats["throwing"] = st->throwing;
						stats["strength"] = st->strength;
						stats["psiStrength"] = st->psiStrength;
						stats["psiSkill"] = st->psiSkill;
						stats["melee"] = st->melee;
						js["stats"] = stats;
					}
					Json::Value layout(Json::arrayValue);
					for (auto* eli : *s->getEquipmentLayout())
					{
						const RuleItem* it = eli->getItemType();
						if (it)
						{
							layout.append(it->getType());
							reserved[it->getType()] = reserved.get(it->getType(), 0).asInt() + 1;
						}
						for (int a = 0; a < RuleItem::AmmoSlotMax; ++a)
						{
							const RuleItem* am = eli->getAmmoItemForSlot(a);
							if (am)
								reserved[am->getType()] = reserved.get(am->getType(), 0).asInt() + 1;
						}
					}
					js["layout"] = layout;
					soldiers.append(js);
				}
				resp["soldiers"] = soldiers;
				resp["reserved"] = reserved;
				resp["ok"] = true;
			}
		}
		else if (cmd == "soldiers_inventory")
		{
			// Replicate the right-click "equip soldier at base" action: opens the
			// base inventory (runInventory) for the top SoldiersState's base. The
			// InventoryState ground then holds the items the player sees in the
			// bottom pane (read via inventory_ground).
			SoldiersState* st = findState<SoldiersState>(_game);
			if (!st)
			{
				resp["error"] = "no SoldiersState in state stack";
			}
			else
			{
				st->btnInventoryClick(nullptr);
				resp["opened"] = (findState<InventoryState>(_game) != nullptr);
				resp["ok"] = true;
			}
		}
		else if (cmd == "inventory_ground")
		{
			// Read-only: the items on the base-inventory ground tile = the
			// "bottom pane" the player sees. Requires an open base inventory
			// (soldiers_inventory first).
			SavedGame* invSg = _game->getSavedGame();
			SavedBattleGame* bg = invSg ? invSg->getSavedBattle() : nullptr;
			if (!bg)
			{
				resp["error"] = "no battle/inventory active";
			}
			else
			{
				Tile* ground = bg->getTile(0);
				Json::Value items(Json::objectValue);
				int total = 0;
				if (ground)
				{
					for (auto* bi : *ground->getInventory())
					{
						std::string t = bi->getRules()->getType();
						items[t] = items.get(t, 0).asInt() + 1;
						total++;
					}
				}
				// items carried by player units (on soldiers), to check
				// conservation: ground + carried should equal what storage had.
				Json::Value carried(Json::objectValue);
				int carriedTotal = 0, units = 0;
				for (auto* u : *bg->getUnits())
				{
					if (u->getFaction() != FACTION_PLAYER) continue;
					units++;
					for (auto* bi : *u->getInventory())
					{
						std::string t = bi->getRules()->getType();
						carried[t] = carried.get(t, 0).asInt() + 1;
						carriedTotal++;
					}
				}
				// Every BattleItem instance in the inventory battle (ground +
				// carried + loaded-into-weapon ammo), to detect true duplication
				// independent of how items are distributed across soldiers.
				Json::Value all(Json::objectValue);
				int allTotal = 0;
				for (auto* bi : *bg->getItems())
				{
					std::string t = bi->getRules()->getType();
					all[t] = all.get(t, 0).asInt() + 1;
					allTotal++;
				}
				resp["items"] = items;
				resp["total"] = total;
				resp["carried"] = carried;
				resp["carriedTotal"] = carriedTotal;
				resp["units"] = units;
				resp["all"] = all;
				resp["allTotal"] = allTotal;
				resp["ok"] = true;
			}
		}
		else if (cmd == "inventory_unload")
		{
			// Reproduce issue #29: unloading a loaded weapon from the base soldier
			// equip screen. Drives Inventory::unload() exactly like btnUnloadClick,
			// which is where the co-op moveItem() path deref'd the just-unloaded
			// ammo's (null) inventory slot and crashed (0xC0000005).
			//
			// Requires an open base inventory (call soldiers_inventory first). If the
			// currently selected soldier has no loaded firearm, one is built and
			// loaded so the unload path is always exercised.
			InventoryState* inv = findState<InventoryState>(_game);
			SavedGame* sg = _game->getSavedGame();
			SavedBattleGame* bg = sg ? sg->getSavedBattle() : nullptr;
			if (!inv || !bg)
			{
				resp["error"] = "no InventoryState/battle active (call soldiers_inventory first)";
			}
			else
			{
				Inventory* inventory = inv->getInventoryForTest();
				BattleUnit* unit = inventory ? inventory->getSelectedUnit() : nullptr;
				Tile* ground = bg->getTile(0);
				if (!unit)
				{
					resp["error"] = "no selected unit in inventory";
				}
				else if (!ground)
				{
					resp["error"] = "no ground tile in inventory battle";
				}
				else
				{
					// Deterministic setup: clear the selected soldier's hands to the
					// ground so unload(false) always has the free hand it needs and
					// actually reaches the (previously crashing) moveItem() path -
					// independent of any state a prior unload left on this soldier.
					RuleInventory* groundRule = _game->getMod()->getInventoryGround();
					auto* uinv = unit->getInventory();
					for (auto it = uinv->begin(); it != uinv->end(); )
					{
						BattleItem* bi = *it;
						if (bi->getSlot() && bi->getSlot()->getType() == INV_HAND)
						{
							it = uinv->erase(it);
							ground->addItem(bi, groundRule);
						}
						else
						{
							++it;
						}
					}

					// Build + load a firearm on the (now empty-handed) soldier.
					const RuleItem* wRule = nullptr;
					const RuleItem* aRule = nullptr;
					for (auto& name : _game->getMod()->getItemsList())
					{
						const RuleItem* r = _game->getMod()->getItem(name, false);
						if (!r || r->getBattleType() != BT_FIREARM) continue;
						if (r->isFixed()) continue;  // skip tank/vehicle-mounted weapons - not hand-holdable
						if (r->getInventoryWidth() == 0 || r->getInventoryHeight() == 0) continue;
						auto* ammos = r->getPrimaryCompatibleAmmo();
						if (ammos && !ammos->empty()) { wRule = r; aRule = ammos->front(); break; }
					}
					if (!wRule)
					{
						resp["error"] = "no firearm+ammo rule available in mod";
					}
					else
					{
						// Place the weapon straight into the (now free) right hand,
						// bypassing addItem()'s weight/placement heuristics which can
						// refuse an off-craft base soldier.
						BattleItem* weapon = bg->createItemForTile(wRule, ground);
						weapon->moveToOwner(unit);
						weapon->setSlot(_game->getMod()->getInventoryRightHand());
						weapon->setSlotX(0);
						weapon->setSlotY(0);

						BattleItem* ammo = bg->createItemForTile(aRule, ground);
						ground->removeItem(ammo);
						if (!weapon->setAmmoPreMission(ammo))
						{
							resp["error"] = "could not load ammo into weapon";
						}
						else
						{
							resp["weapon"] = weapon->getRules()->getType();
							inventory->setSelectedItem(weapon);
							// This is the call that crashed pre-fix (issue #29).
							bool unloaded = inventory->unload(false);
							resp["unloaded"] = unloaded;
							resp["ok"] = true;
						}
					}
				}
			}
		}
		else if (cmd == "incoming_transfers")
		{
			// Read a base's pending incoming transfers (item type -> total qty and
			// soldier count), so a transfer can be checked as "en route" without
			// advancing game time.
			std::string tbase = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
			{
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					if (tbase.empty() ? (base->_coopBase == false && base->_coopIcon == false) : base->getName() == tbase)
					{ target = base; break; }
				}
			}
			if (!target)
				resp["error"] = "base not found";
			else
			{
				Json::Value items(Json::objectValue);
				int soldiers = 0;
				for (auto* t : *target->getTransfers())
				{
					if (t->getType() == TRANSFER_ITEM && t->getItems())
						items[t->getItems()->getType()] = items.get(t->getItems()->getType(), 0).asInt() + t->getQuantity();
					else if (t->getType() == TRANSFER_SOLDIER)
						soldiers++;
				}
				resp["items"] = items;
				resp["soldiers"] = soldiers;
				resp["ok"] = true;
			}
		}
		else if (cmd == "buy")
		{
			// PRD-J03: drive a purchase through the real PurchaseState OK path.
			// <item> = item ruleset type, <count> = quantity, <base> = target base
			// name (default: first real, non-mirror base). In a JOINT campaign this
			// emits a joint_cmd (client) or queues a local command (host); nothing
			// is applied until the joint_apply round-trip completes.
			std::string itemType = req.get("item", "").asString();
			int count = req.get("count", 1).asInt();
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
			{
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
					                     : base->getName() == baseName)
					{ target = base; break; }
				}
			}
			// <kind> "item" (default) or "soldier": PRD-J05 hires spend shared funds
			// and the host generates + serializes the soldier into joint_apply.
			std::string kind = req.get("kind", "item").asString();
			if (!target)
				resp["error"] = "base not found";
			else
			{
				PurchaseState* ps = new PurchaseState(target);
				_game->pushState(ps);
				bool ok = (kind == "soldier")
					? ps->harnessBuySoldier(itemType, count)
					: ps->harnessBuyItem(itemType, count); // calls btnOkClick -> popState
				resp["sent"] = ok;
				resp["ok"] = ok;
				if (!ok) resp["error"] = "no purchasable row for " + kind + ": " + itemType;
			}
		}
		else if (cmd == "sell")
		{
			// PRD-J05: drive a SELL of <count> of ITEM <item> through the real
			// SellState OK path. In JOINT this emits a "sell" joint_cmd (nothing is
			// removed until the joint_apply round-trip). <base> optional.
			std::string itemType = req.get("item", "").asString();
			int count = req.get("count", 1).asInt();
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
					                     : base->getName() == baseName)
					{ target = base; break; }
			if (!target)
				resp["error"] = "base not found";
			else
			{
				SellState* ss = new SellState(target, nullptr, OPT_GEOSCAPE);
				_game->pushState(ss);
				bool ok = ss->harnessSellItem(itemType, count); // btnOkClick -> popState
				resp["sent"] = ok;
				resp["ok"] = ok;
				if (!ok) resp["error"] = "no sellable row for item: " + itemType;
			}
		}
		else if (cmd == "joint_transfer")
		{
			// PRD-J05: drive a base->base transfer of <count> of ITEM <item> from
			// <fromBase> to <toBase> through TransferItemsState -> submitJointTransfer
			// (the JOINT "transfer" joint_cmd). Bases matched by name.
			std::string itemType = req.get("item", "").asString();
			int count = req.get("count", 1).asInt();
			std::string fromName = req.get("fromBase", "").asString();
			std::string toName = req.get("toBase", "").asString();
			Base* from = nullptr; Base* to = nullptr;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					if (from == nullptr && (fromName.empty()
						? (base->_coopBase == false && base->_coopIcon == false)
						: base->getName() == fromName)) from = base;
					else if (!toName.empty() && base->getName() == toName) to = base;
				}
			if (!from || !to)
				resp["error"] = "from/to base not found";
			else
			{
				TransferItemsState* st = new TransferItemsState(from, to, nullptr);
				bool ok = st->harnessTransferItem(itemType, count); // submitJointTransfer
				delete st;
				resp["sent"] = ok;
				resp["ok"] = ok;
				if (!ok) resp["error"] = "no transferable row for item: " + itemType;
			}
		}
		else if (cmd == "containment")
		{
			// PRD-J05: remove <count> live aliens of type <item> from a base's
			// containment via the real ManageAlienContainmentState path (JOINT ->
			// "containment" joint_cmd). <sell>=true sells, false executes.
			std::string alienType = req.get("item", "").asString();
			int count = req.get("count", 1).asInt();
			bool sell = req.get("sell", true).asBool();
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
					                     : base->getName() == baseName)
					{ target = base; break; }
			RuleItem* alien = _game->getMod()->getItem(alienType, false);
			if (!target)
				resp["error"] = "base not found";
			else if (!alien)
				resp["error"] = "unknown alien item: " + alienType;
			else
			{
				ManageAlienContainmentState* ms =
					new ManageAlienContainmentState(target, alien->getPrisonType(), OPT_GEOSCAPE);
				_game->pushState(ms);
				bool ok = ms->harnessRemovePrisoner(alienType, count, sell); // dealWith -> popState
				resp["sent"] = ok;
				resp["ok"] = ok;
				if (!ok) resp["error"] = "no containment row for alien: " + alienType;
			}
		}
		else if (cmd == "give_items")
		{
			// PRD-J05 test helper: add <count> of ITEM <item> directly to a base's
			// stores on THIS machine. Deterministic (no RNG); call on host AND
			// client identically to set up an equal shared world for a sell /
			// containment test.
			std::string itemType = req.get("item", "").asString();
			int count = req.get("count", 1).asInt();
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
					                     : base->getName() == baseName)
					{ target = base; break; }
			RuleItem* rule = _game->getMod()->getItem(itemType, false);
			if (!target)
				resp["error"] = "base not found";
			else if (!rule)
				resp["error"] = "unknown item: " + itemType;
			else
			{
				target->getStorageItems()->addItem(rule, count);
				resp["stored"] = target->getStorageItems()->getItem(rule);
				resp["ok"] = true;
			}
		}
		else if (cmd == "add_base")
		{
			// PRD-J05 test helper: append a minimal empty base (name + coords only,
			// no facilities) to THIS machine's world. Call on host AND client
			// identically so both hold the same base list in the same order
			// (baseId = index resolves equally). Stands in for J07's proper
			// joint_apply base creation - test scaffolding only; a JOINT test that
			// wants a REAL shared base must use `build_new_base` (base_new).
			//
			// PRD-J11: <coopbaseid> is forced to a fixed default because Base's ctor
			// ROLLS A RANDOM _coop_base_id when it is 0 (Base.cpp ~:71-80). Calling
			// this on two machines therefore produced two DIFFERENT coop ids - the
			// one thing about this helper that was never deterministic, despite the
			// comment that used to claim it was. Harmless until the J11 world-equality
			// helper started comparing coopBaseId, which it must: base_new's whole
			// point is that the host MINTS the id and it rides the payload.
			SavedGame* sg = _game->getSavedGame();
			if (!sg)
				resp["error"] = "no save";
			else
			{
				Base* b = new Base(_game->getMod());
				b->setName(req.get("name", "Base B").asString());
				b->setLongitude(req.get("lon", 1.0).asDouble());
				b->setLatitude(req.get("lat", 0.3).asDouble());
				b->_coop_base_id = req.get("coopbaseid", 424242).asInt();
				sg->getBases()->push_back(b);
				resp["coopBaseId"] = b->_coop_base_id;
				resp["baseCount"] = (int)sg->getBases()->size();
				resp["ok"] = true;
			}
		}
		else if (cmd == "fac_build")
		{
			// PRD-J07: drive a facility build through the REAL PlaceFacilityState
			// viewClick path at grid (<x>,<y>). In JOINT this emits a fac_build
			// joint_cmd (nothing applied until joint_apply); solo/SEPARATE builds
			// locally. <facility> = ruleset type, <base> = name (default first real).
			std::string facType = req.get("facility", "").asString();
			int x = req.get("x", -1).asInt();
			int y = req.get("y", -1).asInt();
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
					                     : base->getName() == baseName)
					{ target = base; break; }
			RuleBaseFacility* rule = _game->getMod()->getBaseFacility(facType, false);
			if (!target)
				resp["error"] = "base not found";
			else if (!rule)
				resp["error"] = "unknown facility: " + facType;
			else
			{
				PlaceFacilityState* pf = new PlaceFacilityState(target, rule);
				_game->pushState(pf);
				pf->harnessBuild(x, y); // real viewClick -> JOINT fac_build + popState
				resp["ok"] = true;
			}
		}
		else if (cmd == "fac_dismantle")
		{
			// PRD-J07: drive a dismantle through the REAL DismantleFacilityState OK
			// path for the facility at grid (<x>,<y>). JOINT-only harness lane: the
			// JOINT branch emits fac_dismantle + pops before ever touching the view
			// (passed null here; the SEPARATE/solo path needs a live BaseView).
			int x = req.get("x", -1).asInt();
			int y = req.get("y", -1).asInt();
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
					                     : base->getName() == baseName)
					{ target = base; break; }
			BaseFacility* fac = nullptr;
			if (target)
				for (auto* f : *target->getFacilities())
					if (f->getX() == x && f->getY() == y) { fac = f; break; }
			if (!target)
				resp["error"] = "base not found";
			else if (!fac)
				resp["error"] = "no facility at grid";
			else if (!_game->getCoopMod()->isJointCampaign())
				resp["error"] = "fac_dismantle hook is JOINT-only";
			else
			{
				DismantleFacilityState* ds = new DismantleFacilityState(target, nullptr, fac);
				_game->pushState(ds);
				ds->harnessDismantle(); // real btnOkClick -> JOINT fac_dismantle + popState
				resp["ok"] = true;
			}
		}
		else if (cmd == "sack")
		{
			// PRD-J07: drive a sack through the REAL SackSoldierState OK path.
			// <soldierId> = the soldier's stable getId() (resolved to a roster index
			// here). In JOINT this emits a sack joint_cmd (any player may sack any
			// soldier); solo/SEPARATE runs the vanilla local path.
			int soldierId = req.get("soldierId", -1).asInt();
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
					                     : base->getName() == baseName)
					{ target = base; break; }
			int idx = -1;
			if (target)
				for (size_t i = 0; i < target->getSoldiers()->size(); ++i)
					if (target->getSoldiers()->at(i)->getId() == soldierId) { idx = (int)i; break; }
			if (!target)
				resp["error"] = "base not found";
			else if (idx < 0)
				resp["error"] = "soldier not found";
			else
			{
				SackSoldierState* ss = new SackSoldierState(target, (size_t)idx);
				_game->pushState(ss);
				ss->harnessSack(); // real btnOkClick -> JOINT sack + popState
				resp["ok"] = true;
			}
		}
		else if (cmd == "base_rename")
		{
			// PRD-J07: rename a base through the REAL BasescapeState edtBaseChange
			// path (JOINT -> base_rename joint_cmd; SEPARATE -> changeBaseName).
			// The state is constructed transiently (never pushed; the handler does
			// not pop), matching the TransferItemsState harness idiom.
			std::string baseName = req.get("base", "").asString();
			std::string newName = req.get("name", "").asString();
			Base* target = nullptr;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
					if (baseName.empty() ? (base->_coopBase == false && base->_coopIcon == false)
					                     : base->getName() == baseName)
					{ target = base; break; }
			if (!target)
				resp["error"] = "base not found";
			else if (newName.empty())
				resp["error"] = "empty name";
			else
			{
				BasescapeState* bs = new BasescapeState(target, nullptr);
				bs->harnessRename(newName);
				delete bs;
				resp["ok"] = true;
			}
		}
		else if (cmd == "soldier_rename")
		{
			// Playtest B3: drive a soldier rename through the REAL SoldierInfoState
			// edtSoldierChange path. In JOINT this emits soldier_rename (nothing
			// authoritative until joint_apply); solo/SEPARATE renames locally.
			// <soldierId> = Soldier::getId(), <name> = new name, <base> optional.
			int id = req.get("soldierId", -1).asInt();
			std::string newName = req.get("name", "").asString();
			std::string baseName = req.get("base", "").asString();
			Base* target = nullptr;
			size_t idx = 0;
			if (_game->getSavedGame())
				for (auto* base : *_game->getSavedGame()->getBases())
				{
					if (base->_coopBase || base->_coopIcon) continue;
					if (!baseName.empty() && base->getName() != baseName) continue;
					auto* sols = base->getSoldiers();
					for (size_t i = 0; i < sols->size(); ++i)
						if (sols->at(i)->getId() == id) { target = base; idx = i; break; }
					if (target) break;
				}
			if (!target)
				resp["error"] = "soldier not found";
			else if (newName.empty())
				resp["error"] = "empty name";
			else
			{
				SoldierInfoState* si = new SoldierInfoState(target, idx);
				si->init();               // sets _soldier + _edtSoldier from the roster
				si->harnessRename(newName); // real edtSoldierChange -> JOINT soldier_rename
				delete si;
				resp["ok"] = true;
			}
		}
		else if (executeJoint11(cmd, req, resp))
		{
			// handled by the second sub-dispatcher (executeJoint11), split off to
			// stay under MSVC's C1061 nested-block limit (same reason as executeJoint10).
		}
		else
		{
			resp["error"] = "unknown cmd: " + cmd;
		}
	}

	Json::FastWriter w;
	std::string out = w.write(resp);
	if (!out.empty() && out.back() == '\n')
	{
		out.pop_back();
	}
	return out;
}

}
