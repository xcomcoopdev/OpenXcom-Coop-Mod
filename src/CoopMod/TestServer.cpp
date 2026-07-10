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

#include <cstdlib>
#include <set>
#include <typeinfo>

#include <json/json.h>
#include <SDL_net.h>

#include "../Engine/Game.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/State.h"
#include "../Geoscape/GeoscapeState.h"
#include "../Geoscape/GeoscapeEventState.h"
#include "../Geoscape/MonthlyReportState.h"
#include "../Geoscape/MissionDetectedState.h"
#include "../Geoscape/ConfirmLandingState.h"
#include "../Ufopaedia/ArticleState.h"
#include "../Battlescape/BattlescapeState.h"
#include "../Battlescape/BattlescapeGame.h"
#include "../Battlescape/BriefingState.h"
#include "../Battlescape/InventoryState.h"
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
#include "../Mod/Unit.h"
#include "../Savegame/Base.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/Country.h"
#include "../Mod/RuleCountry.h"
#include "../Savegame/Craft.h"
#include "../Savegame/GameTime.h"
#include "../Savegame/MissionSite.h"
#include "../Savegame/ResearchProject.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/Ufo.h"
#include "../Mod/RuleCraft.h"
#include "../Mod/RuleResearch.h"
#include "../Mod/RuleUfo.h"
#include "../Menu/NewGameState.h"
#include "../Menu/StartState.h"
#include "../Geoscape/BuildNewBaseState.h"
#include "../Geoscape/BaseNameState.h"
#include "../Geoscape/UfoDetectedState.h"
#include "LobbyMenu.h"
#include "Profile.h"
#include "connectionTCP.h"
#include "ServerList.h"
#include "../Engine/Screen.h"
#include "../Basescape/BasescapeState.h"
#include "../Basescape/SoldiersState.h"
#include "CoopState.h"
#include "TransferNoticeState.h"
#include "TransferSoldierMenu.h"
#include "../Interface/DisableableComboBox.h"

namespace OpenXcom
{

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

static Json::Value soldierToJson(Soldier* s)
{
	Json::Value j;
	j["id"] = s->getId();
	j["name"] = s->getName();
	j["owner"] = s->getOwnerPlayerId();
	j["coop"] = s->getCoop();
	j["coopBase"] = s->getCoopBase();
	j["craft"] = s->getCraft() ? s->getCraft()->getType() : "";
	j["dead"] = s->getDeath() != nullptr;
	return j;
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

		if (cmd == "ping")
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
			ServerList* browser = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* sl = dynamic_cast<ServerList*>(s))
					browser = sl;
			}

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
			ServerList* browser = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* sl = dynamic_cast<ServerList*>(s))
					browser = sl;
			}
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

				Json::Value bases(Json::arrayValue);
				for (auto* b : *sg->getBases())
				{
					Json::Value jb;
					jb["name"] = b->getName(_game->getLanguage());
					Json::Value crafts(Json::arrayValue);
					for (auto* c : *b->getCrafts())
					{
						Json::Value jc;
						jc["type"] = c->getRules()->getType();
						jc["status"] = c->getStatus();
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
						research.append(jr);
					}
					jb["research"] = research;
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
			GeoscapeState* gs = nullptr;
			for (auto* s : _game->getStates())
				if (auto* g = dynamic_cast<GeoscapeState*>(s))
					gs = g;
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
		else if (cmd == "confirm_landing")
		{
			ConfirmLandingState* cl = nullptr;
			for (auto* s : _game->getStates())
				if (auto* c = dynamic_cast<ConfirmLandingState*>(s)) cl = c;
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
			InventoryState* inv = nullptr;
			for (auto* s : _game->getStates())
				if (auto* i = dynamic_cast<InventoryState*>(s)) inv = i;
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
			BriefingState* br = nullptr;
			for (auto* s : _game->getStates())
				if (auto* b = dynamic_cast<BriefingState*>(s)) br = b;
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
			State* top = _game->getStates().empty() ? nullptr : _game->getStates().back();
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
			resp["sessionLocked"] = connectionTCP::isCoopSessionLocked;
			resp["playerReady"] = connectionTCP::isPlayerReady;
			resp["playersReady"] = connectionTCP::isPlayersReady;
			resp["lobbyClosed"] = connectionTCP::isLobbyMenuClosed;
			resp["lobbyFileStatus"] = connectionTCP::LobbyFileStatus;
			resp["coopSession"] = coop->isCoopSession();
			resp["hasSave"] = _game->getSavedGame() != nullptr;
			resp["inBattle"] = _game->getSavedGame() && _game->getSavedGame()->getSavedBattle();
			resp["hostName"] = coop->getHostName();
			resp["clientName"] = coop->getCurrentClientName();
			resp["insideCoopBase"] = coop->playerInsideCoopBase;
			resp["saveID"] = Json::Value::Int64(connectionTCP::saveID);
			resp["ok"] = true;
		}
		else if (cmd == "load_save")
		{
			std::string file = req.get("file", "").asString();
			SavedGame* s = new SavedGame();
			s->load(file, _game->getMod(), _game->getLanguage());
			coop->resetTransferSessionState();
			_game->setSavedGame(s);
			_game->setState(new GeoscapeState);
			resp["ok"] = true;
		}
		else if (cmd == "host_tcp")
		{
			std::string server = req.get("server", "TestServer").asString();
			std::string port = req.get("port", "3000").asString();
			std::string player = req.get("player", "HostPlayer").asString();

			connectionTCP::password = "";
			connectionTCP::isPasswordRequired = false;
			connectionTCP::_coopGamemode = 1; // PVE
			coop->setCoopSession(false);
			coop->setPlayerTurn(3);
			coop->setHostName(player);
			// campaign when a real campaign save is loaded (same check as HostMenu)
			bool campaign = _game->getSavedGame() && !_game->getSavedGame()->getCountries()->empty();
			coop->setCoopCampaign(campaign);
			coop->hostTCPServer(server, port);
			coop->setServerOwner(true);
			if (Options::HostSaveProgress && campaign)
			{
				_game->pushState(new LobbyMenu());
			}
			resp["campaign"] = campaign;
			resp["ok"] = true;
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
			Profile* profile = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* p = dynamic_cast<Profile*>(s))
				{
					profile = p;
				}
			}
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
			_game->pushState(new NewGameState);
			resp["ok"] = true;
		}
		else if (cmd == "newgame_ok")
		{
			NewGameState* ng = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* n = dynamic_cast<NewGameState*>(s))
				{
					ng = n;
				}
			}
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

			BuildNewBaseState* build = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* b = dynamic_cast<BuildNewBaseState*>(s))
				{
					build = b;
				}
			}
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
				BaseNameState* nameState = nullptr;
				for (auto* s : _game->getStates())
				{
					if (auto* n = dynamic_cast<BaseNameState*>(s))
					{
						nameState = n;
					}
				}
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
			LobbyMenu* lobby = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* l = dynamic_cast<LobbyMenu*>(s))
				{
					lobby = l;
				}
			}
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
			SoldiersState* st = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* x = dynamic_cast<SoldiersState*>(s))
				{
					st = x;
				}
			}
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
			BasescapeState* st = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* x = dynamic_cast<BasescapeState*>(s))
				{
					st = x;
				}
			}
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
		else if (cmd == "transfer_targets")
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
				int currentOwner = TransferSoldierMenu::resolveOwnerId(found);
				int localPlayerId = connectionTCP::getHost() ? 0 : 1;
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
		else if (cmd == "open_transfer_dialog")
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
				_game->pushState(new TransferSoldierMenu(found, TransferSoldierMenu::resolveOwnerId(found)));
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
			_game->pushState(new TransferNoticeState(req.get("message", "test notice").asString()));
			resp["ok"] = true;
		}
		else if (cmd == "get_notices")
		{
			Json::Value notices(Json::arrayValue);
			for (auto* s : _game->getStates())
			{
				if (auto* n = dynamic_cast<TransferNoticeState*>(s))
				{
					notices.append(n->getCategory());
				}
			}
			resp["categories"] = notices;
			resp["ok"] = true;
		}
		else if (cmd == "dismiss_notice")
		{
			TransferNoticeState* st = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* x = dynamic_cast<TransferNoticeState*>(s))
				{
					st = x;
				}
			}
			if (st)
			{
				st->btnOkClick(nullptr);
				resp["ok"] = true;
			}
			else
			{
				resp["error"] = "no TransferNoticeState in state stack";
			}
		}
		else if (cmd == "cancel_dialog")
		{
			TransferSoldierMenu* st = nullptr;
			for (auto* s : _game->getStates())
			{
				if (auto* x = dynamic_cast<TransferSoldierMenu*>(s))
				{
					st = x;
				}
			}
			if (st)
			{
				st->btnCancelClick(nullptr);
				resp["ok"] = true;
			}
			else
			{
				resp["error"] = "no TransferSoldierMenu in state stack";
			}
		}
		else if (cmd == "get_palettes")
		{
			// First N palette entries of the top two states, for asserting
			// that a dialog adopted its parent's palette (flicker check).
			Json::Value states(Json::arrayValue);
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
		else if (cmd == "transfer")
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
					coop->transferSoldierOwnership(found, owner, true);
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
		else if (cmd == "has_coop_file")
		{
			std::string key = req.get("key", "").asString();
			resp["present"] = connectionTCP::hasCoopFile(key);
			resp["ok"] = true;
		}
		else if (cmd == "set_option")
		{
			std::string name = req.get("name", "").asString();
			if (name == "HostSaveProgress")
			{
				Options::HostSaveProgress = req.get("value", false).asBool();
				resp["ok"] = true;
			}
			else
			{
				resp["error"] = "unknown option: " + name;
			}
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
