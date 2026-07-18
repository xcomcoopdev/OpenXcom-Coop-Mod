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

#include "JointEcon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Engine/Game.h"
#include "../Engine/Language.h"
#include "../Engine/LocalizedText.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"
#include "../Engine/RNG.h"
#include "../Engine/State.h"
#include "../Engine/Yaml.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleCraft.h"
#include "../Mod/RuleSoldier.h"
#include "../Mod/RuleResearch.h"
#include "../Mod/RuleManufacture.h"
#include "../Mod/Unit.h"
#include "../Mod/Armor.h"
#include "../Mod/RuleBaseFacility.h"
#include "../Mod/RuleRegion.h"
#include "../Mod/RuleInterface.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/GameTime.h"
#include "../Savegame/Base.h"
#include "../Savegame/BaseFacility.h"
#include "../Savegame/Region.h"
#include "../Savegame/Craft.h"
#include "../Savegame/ItemContainer.h"
#include "../Savegame/Transfer.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/ResearchProject.h"
#include "../Savegame/Production.h"
#include "../Savegame/Ufo.h"
#include "../Savegame/MissionSite.h"
#include "../Savegame/AlienBase.h"
#include "../Savegame/Waypoint.h"
#include "../Savegame/Target.h"
#include "../Savegame/AlienMission.h"
#include "../Savegame/CraftWeapon.h"
#include "../Savegame/Country.h"
#include "../Savegame/WeightedOptions.h"
#include "../Mod/RuleUfo.h"
#include "../Mod/RuleCraftWeapon.h"
#include "../Mod/RuleCountry.h"
#include "../Mod/RuleAlienMission.h"
#include "../Mod/AlienRace.h"

#include <cmath>
#include "../Basescape/BaseView.h"
#include "../Geoscape/GeoscapeState.h"
#include "../Geoscape/ConfirmLandingState.h"
#include "../Geoscape/Globe.h"
#include "../Geoscape/ResearchCompleteState.h"
#include "../Geoscape/ProductionCompleteState.h"
#include "../Menu/ErrorMessageState.h"

#include "connectionTCP.h"
#include "CoopState.h"

namespace OpenXcom
{
namespace JointEcon
{

namespace {

// ---- Command registry --------------------------------------------------------
struct Handler { CmdValidator validate; CmdApplier apply; };
std::unordered_map<std::string, Handler>& registry()
{
	static std::unordered_map<std::string, Handler> r;
	return r;
}
bool g_inited = false;

// ---- Deferred main-thread work (mirrors the waitedTrades idiom) --------------
// A command queued for host-side validate+apply+broadcast. `remote` = received
// from a client over the wire (reply with joint_ok/joint_fail); !remote = the
// host's own UI (surface a failure locally, no wire reply).
struct PendingCmd
{
	std::string cmd;
	int seq = 0;
	int seat = 0;
	int baseId = -1;
	Json::Value payload;
	bool remote = false;
};

std::mutex g_mx;                     // guards the four queues below
std::deque<PendingCmd>  g_cmdQ;      // host:      to validate+apply+broadcast
std::deque<Json::Value> g_applyQ;    // replica:   joint_apply to apply
std::deque<std::string> g_failQ;     // initiator: joint_fail reasons to surface
int g_resyncServeQ = 0;              // host:      pending joint_resync_requests

// Per-machine monotonic command sequence stamp (protocol `seq`).
std::atomic<int> g_seqCounter{0};

// ---- Diagnostics (harness-observable) ----------------------------------------
std::atomic<uint64_t> g_cmdN{0};     // joint_cmd this host validated
std::atomic<uint64_t> g_okN{0};      // joint_ok sent (host) / received (client)
std::atomic<uint64_t> g_failN{0};    // joint_fail surfaced (initiator) / sent (host)
std::atomic<uint64_t> g_applyN{0};   // joint_apply applied by this machine
std::atomic<uint64_t> g_unknownN{0}; // joint_cmd naming an unregistered cmd
std::mutex g_failMx;
std::string g_lastFail;

// ---- PRD-J10: desync repair bookkeeping --------------------------------------
std::atomic<uint64_t> g_mismatchN{0};  // checksum mismatches seen (replica)
std::atomic<uint64_t> g_resyncReqN{0}; // resyncs asked for (replica) / served (host)
bool g_resyncPending = false;          // replica: a world restream is in flight
bool g_resyncGaveUp = false;           // replica: throttled out, popup already shown
int64_t g_lastResyncGameMin = -1;      // replica: game-minute stamp of the last resync
// The checksum rides the geoscape `time` heartbeat, which the host emits at LINK
// RATE (~2000/s). Log a mismatch ONCE per episode, not once per heartbeat: a
// per-heartbeat log is thousands of fopen/fwrite/fclose per second on the
// replica's main thread, which starves the very world-restream that repairs it -
// the repair then never lands and the desync looks unfixable. (Measured: the
// restream degrades from <1s to >4s per 3KB chunk, then stalls.)
bool g_mismatchLogged = false;
// Wall-clock ms at which the CURRENT mismatch streak started, -1 = in agreement.
// See the debounce in verifyWorldChecksum.
int64_t g_mismatchSinceMs = -1;

bool isHost() { return connectionTCP::getHost(); }

// baseId decision (PRD offered coop_base_id OR index-in-_bases): we use the
// INDEX into SavedGame::getBases(). The JOINT world is a byte-faithful streamed
// replica, so host and every replica hold the same base list in the same order;
// the index is a stable shared key. _coop_base_id is 0/unset for a JOINT
// campaign's own real bases, so it is NOT usable. Base add/remove also rides
// joint_apply (J07), keeping the ordering in lock-step.
Base* resolveBase(Game* game, int baseId)
{
	if (!game || !game->getSavedGame()) return nullptr;
	auto* bases = game->getSavedGame()->getBases();
	if (baseId < 0 || baseId >= (int)bases->size()) return nullptr;
	return (*bases)[baseId];
}

void setLastFail(const std::string& reason)
{
	std::lock_guard<std::mutex> lk(g_failMx);
	g_lastFail = reason;
}

// ---- PRD-J10: apply notification (live screen refresh) -----------------------
// ONE listener, last-registered-wins. `g_listenerOwner` is the identity token that
// makes the "last-registered-wins" rule safe: OXCE defers deleting a popped state
// to the top of the NEXT frame, i.e. AFTER the replacement screen's init() has
// already registered. Without the token the old screen's destructor would clear
// the new screen's listener and refresh would silently die after one rebuild.
const void* g_listenerOwner = nullptr;
ApplyListener g_listener;
std::atomic<int> g_lastApplySeat{-1};

void fireApplyListener(const std::string& cmd, int baseId, int seat)
{
	g_lastApplySeat = seat;
	if (g_listener) g_listener(cmd, baseId);
}

// ---- Shared J05 helpers ------------------------------------------------------

// Serialize a soldier to a YAML string (same wire form giftSoldier uses), so a
// host-generated hire can travel INSIDE joint_apply and be reconstructed on the
// replica without re-rolling RNG (names/stats/nationality would diverge).
std::string serializeSoldier(Game* game, Soldier* soldier)
{
	YAML::YamlRootNodeWriter writer;
	writer.setAsMap();
	soldier->save(writer["soldier"], game->getMod()->getScriptGlobal());
	return writer.emit().yaml;
}

// Reconstruct a soldier from a serialized YAML string. The replica NEVER
// regenerates; it adopts the host's exact soldier (incl. ownerplayerid).
Soldier* deserializeSoldier(Game* game, const std::string& yaml)
{
	YAML::YamlRootNodeReader reader(YAML::YamlString{yaml}, "jointHire");
	auto soldierReader = reader["soldier"];
	std::string type = soldierReader["type"].readVal(game->getMod()->getSoldiersList().front());
	RuleSoldier* rule = game->getMod()->getSoldier(type, false);
	if (!rule) return nullptr;
	Soldier* soldier = new Soldier(rule, nullptr, 0 /*nationality; overwritten by load*/);
	soldier->load(soldierReader, game->getMod(), game->getSavedGame(), game->getMod()->getScriptGlobal());
	soldier->setCraft(0);
	return soldier;
}

// Shortest base-to-base distance, byte-identical to TransferItemsState::getDistance
// (both bases are real shared bases with identical coords on host and replica).
double baseDistance(Base* from, Base* to)
{
	double x[3], y[3], z[3], r = 51.2;
	Base* b = from;
	for (int i = 0; i < 2; ++i)
	{
		x[i] = r * cos(b->getLatitude()) * cos(b->getLongitude());
		y[i] = r * cos(b->getLatitude()) * sin(b->getLongitude());
		z[i] = r * -sin(b->getLatitude());
		b = to;
	}
	x[2] = x[1] - x[0];
	y[2] = y[1] - y[0];
	z[2] = z[1] - z[0];
	return sqrt(x[2] * x[2] + y[2] * y[2] + z[2] * z[2]);
}

// Find a craft at @a base by its per-type id + rule type (Craft::getId() is only
// unique within a type). Matches TransferItemsState / SellState identity.
Craft* findCraft(Base* base, int id, const std::string& type)
{
	for (auto* c : *base->getCrafts())
		if (c->getId() == id && c->getRules()->getType() == type) return c;
	return nullptr;
}

// Find a soldier at @a base by its vanilla unique id (Soldier::getId(), stable
// across the shared world lineage - the same identity sack/sell match on).
Soldier* findSoldier(Base* base, int id)
{
	if (!base) return nullptr;
	for (auto* s : *base->getSoldiers())
		if (s->getId() == id) return s;
	return nullptr;
}

// PRD-J06: a research project / a production is keyed by its ruleset NAME, which
// is UNIQUE per base: SavedGame::getAvailableResearchProjects /
// getAvailableProductions both exclude a rule already running at the base, and
// the res_start / man_start validators re-run that availability check, so a
// second start of the same rule is rejected. No per-entity sequence id needed
// (verified against vanilla; see session notes).
ResearchProject* findResearchProject(Base* base, const std::string& name)
{
	for (auto* p : base->getResearch())
		if (p->getRules()->getName() == name) return p;
	return nullptr;
}
Production* findProduction(Base* base, const std::string& name)
{
	for (auto* p : base->getProductions())
		if (p->getRules()->getName() == name) return p;
	return nullptr;
}

// ---- Reference command: "buy" ------------------------------------------------
// Payload: { items:[ {type:<TransferType int>, rule:"<typeString>", qty:int}, ...],
//            total:int (client estimate, NOT trusted) }.
//
// J03 supports ITEM / SCIENTIST / ENGINEER / CRAFT deterministically (identical
// transfers on host and replica). SOLDIER hire is deferred to J05: a generated
// soldier carries RNG (nationality/name/stats) that would diverge between host
// and replica unless serialized into joint_apply; the validator rejects soldier
// rows so nothing diverges before J05 wires the serialization.

bool buyValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                 int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) { failReason = "no world"; return false; }

	const Json::Value& items = payload["items"];
	if (!items.isArray() || items.empty()) { failReason = "empty purchase"; return false; }

	int64_t total = 0;
	double storeAdd = 0.0;
	int quartersAdd = 0;
	int hangarsAdd = 0;
	std::map<int, int> prisonAdd;

	for (const auto& it : items)
	{
		int type = it.get("type", -1).asInt();
		std::string rule = it.get("rule", "").asString();
		int qty = it.get("qty", 0).asInt();
		if (qty <= 0) continue;
		switch (type)
		{
		case TRANSFER_ITEM:
		{
			RuleItem* r = mod->getItem(rule, false);
			if (!r) { failReason = "unknown item: " + rule; return false; }
			total += (int64_t)qty * r->getBuyCostAdjusted(base, save);
			storeAdd += (double)qty * r->getSize();
			if (r->isAlien()) prisonAdd[r->getPrisonType()] += qty;
			break;
		}
		case TRANSFER_SCIENTIST:
			total += (int64_t)qty * mod->getHireScientistCost();
			quartersAdd += qty;
			break;
		case TRANSFER_ENGINEER:
			total += (int64_t)qty * mod->getHireEngineerCost();
			quartersAdd += qty;
			break;
		case TRANSFER_CRAFT:
		{
			RuleCraft* r = mod->getCraft(rule, false);
			if (!r) { failReason = "unknown craft: " + rule; return false; }
			total += (int64_t)qty * r->getBuyCost();
			hangarsAdd += qty;
			break;
		}
		case TRANSFER_SOLDIER:
		{
			// PRD-J05: hired soldiers spend shared funds; the purchaser owns them
			// (setOwnerPlayerId at apply). The host GENERATES them (RNG) at apply
			// time and serializes each into the joint_apply payload so replicas
			// reconstruct rather than re-roll.
			RuleSoldier* r = mod->getSoldier(rule, false);
			if (!r) { failReason = "unknown soldier: " + rule; return false; }
			total += (int64_t)qty * r->getBuyCost();
			quartersAdd += qty; // a hired soldier occupies living quarters
			break;
		}
		default:
			failReason = "unknown transfer type";
			return false;
		}
	}

	// Funds checked first so an insufficient-funds rejection is unambiguous.
	if (save->getFunds() < total) { failReason = "STR_NOT_ENOUGH_MONEY"; return false; }
	if (base->storesOverfull(storeAdd)) { failReason = "STR_NOT_ENOUGH_STORE_SPACE"; return false; }
	if (quartersAdd > base->getAvailableQuarters() - base->getUsedQuarters())
		{ failReason = "STR_NOT_ENOUGH_LIVING_SPACE"; return false; }
	if (hangarsAdd > base->getAvailableHangars() - base->getUsedHangars())
		{ failReason = "STR_NO_FREE_HANGARS_FOR_PURCHASE"; return false; }
	for (const auto& kv : prisonAdd)
		if (kv.second > base->getAvailableContainment(kv.first) - base->getUsedContainment(kv.first))
			{ failReason = "STR_NOT_ENOUGH_PRISON_SPACE"; return false; }

	cost = total;
	return true;
}

void buyApply(Game* game, Json::Value& payload, Base* base, int seat)
{
	if (!base) return;
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) return;
	auto& limitLog = save->getMonthlyPurchaseLimitLog();
	const bool host = connectionTCP::getHost();

	Json::Value& items = payload["items"];
	if (!items.isArray()) return;

	// Index-based iteration so the host can WRITE resolved soldier YAML back into
	// each soldier row before the payload is broadcast (see buyApply's soldier case).
	for (Json::ArrayIndex i = 0; i < items.size(); ++i)
	{
		Json::Value& it = items[i];
		int type = it.get("type", -1).asInt();
		std::string rule = it.get("rule", "").asString();
		int qty = it.get("qty", 0).asInt();
		if (qty <= 0) continue;
		switch (type)
		{
		case TRANSFER_ITEM:
		{
			RuleItem* r = mod->getItem(rule, false);
			if (!r) break;
			if (r->getMonthlyBuyLimit() > 0) limitLog[r->getType()] += qty;
			Transfer* t = new Transfer(r->getTransferTime());
			t->setItems(r, qty);
			base->getTransfers()->push_back(t);
			break;
		}
		case TRANSFER_SCIENTIST:
		{
			Transfer* t = new Transfer(mod->getPersonnelTime());
			t->setScientists(qty);
			base->getTransfers()->push_back(t);
			break;
		}
		case TRANSFER_ENGINEER:
		{
			Transfer* t = new Transfer(mod->getPersonnelTime());
			t->setEngineers(qty);
			base->getTransfers()->push_back(t);
			break;
		}
		case TRANSFER_CRAFT:
		{
			RuleCraft* r = mod->getCraft(rule, false);
			if (!r) break;
			if (r->getMonthlyBuyLimit() > 0) limitLog[r->getType()] += qty;
			for (int c = 0; c < qty; ++c)
			{
				Transfer* t = new Transfer(r->getTransferTime());
				// getId() advances the per-type counter identically on host and
				// replica (same start + same joint_apply on both), so no id needs
				// to travel in the packet and the counters stay in lock-step.
				Craft* craft = new Craft(r, base, save->getId(r->getType()));
				craft->initFixedWeapons(mod);
				craft->setStatus("STR_REFUELLING");
				t->setCraft(craft);
				base->getTransfers()->push_back(t);
			}
			break;
		}
		case TRANSFER_SOLDIER:
		{
			RuleSoldier* r = mod->getSoldier(rule, false);
			if (!r) break;
			if (r->getMonthlyBuyLimit() > 0) limitLog[r->getType()] += qty;
			int time = r->getTransferTime();
			if (time == 0) time = mod->getPersonnelTime();

			if (host && !it.isMember("soldiers"))
			{
				// HOST first-apply: generate each soldier (RNG), stamp the
				// purchaser as owner, create the in-transit Transfer, and serialize
				// the finished soldier INTO the payload so the broadcast carries it.
				Json::Value serialized(Json::arrayValue);
				for (int s = 0; s < qty; ++s)
				{
					int nationality = save->selectSoldierNationalityByLocation(mod, r, base);
					Soldier* soldier = mod->genSoldier(save, r, nationality);
					if (!r->getSpawnedSoldierTemplate().yaml.empty())
					{
						YAML::YamlRootNodeReader tReader(r->getSpawnedSoldierTemplate(), "(spawned soldier template)");
						int nationalityOrig = soldier->getNationality();
						soldier->load(tReader.toBase(), mod, save, mod->getScriptGlobal(), true);
						if (soldier->getNationality() != nationalityOrig) soldier->genName();
					}
					soldier->setOwnerPlayerId(seat); // PRD-J05: purchaser owns the hire
					Transfer* t = new Transfer(time);
					t->setSoldier(soldier);
					base->getTransfers()->push_back(t);
					serialized.append(serializeSoldier(game, soldier));
				}
				it["soldiers"] = serialized; // travels in joint_apply to the replicas
			}
			else
			{
				// REPLICA (or a re-apply carrying resolved soldiers): reconstruct the
				// host's exact soldiers from the serialized YAML - never re-roll.
				const Json::Value& serialized = it["soldiers"];
				for (Json::ArrayIndex s = 0; s < serialized.size(); ++s)
				{
					Soldier* soldier = deserializeSoldier(game, serialized[s].asString());
					if (!soldier) continue;
					soldier->setOwnerPlayerId(seat); // belt-and-braces (also in YAML)
					Transfer* t = new Transfer(time);
					t->setSoldier(soldier);
					base->getTransfers()->push_back(t);
				}
			}
			break;
		}
		default:
			break;
		}
	}
}

// ---- PRD-J05: "sell" ---------------------------------------------------------
// Payload: { items:[{rule, qty}], soldiers:[id...], crafts:[{id,type}...],
//            scientists:int, engineers:int }. baseId = the base being sold from.
// Vanilla sell is atomic (one OK button, immediate removal + credit). The host
// re-prices against the live world (another player may have sold first); any
// missing quantity rejects the WHOLE command. cost is NEGATIVE (a credit).

bool sellValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                  int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) { failReason = "no world"; return false; }

	int64_t credit = 0;

	const Json::Value& items = payload["items"];
	if (items.isArray())
		for (const auto& it : items)
		{
			std::string rule = it.get("rule", "").asString();
			int qty = it.get("qty", 0).asInt();
			if (qty <= 0) continue;
			RuleItem* r = mod->getItem(rule, false);
			if (!r) { failReason = "unknown item: " + rule; return false; }
			if (base->getStorageItems()->getItem(r) < qty)
				{ failReason = "STR_NOT_ENOUGH_ITEMS_TO_SELL"; return false; }
			credit += (int64_t)qty * r->getSellCostAdjusted(base, save);
		}

	const Json::Value& soldiers = payload["soldiers"];
	if (soldiers.isArray())
		for (const auto& sid : soldiers)
		{
			int id = sid.asInt();
			bool found = false;
			for (auto* s : *base->getSoldiers())
				if (s->getId() == id && s->getCraft() == 0) { found = true; break; }
			if (!found) { failReason = "soldier not sellable"; return false; }
		}

	const Json::Value& crafts = payload["crafts"];
	if (crafts.isArray())
		for (const auto& jc : crafts)
		{
			Craft* c = findCraft(base, jc.get("id", -1).asInt(), jc.get("type", "").asString());
			if (!c || c->getStatus() == "STR_OUT") { failReason = "craft not sellable"; return false; }
			credit += c->getRules()->getSellCost();
		}

	int sci = payload.get("scientists", 0).asInt();
	int eng = payload.get("engineers", 0).asInt();
	if (sci > base->getAvailableScientists()) { failReason = "not enough scientists"; return false; }
	if (eng > base->getAvailableEngineers()) { failReason = "not enough engineers"; return false; }

	cost = -credit; // credit the seller
	return true;
}

void sellApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Mod* mod = game->getMod();
	if (!mod) return;

	// ORDER (items -> soldiers -> crafts -> scientists -> engineers) is fixed and
	// identical on host and replica, so both worlds mutate the same way.
	const Json::Value& items = payload["items"];
	if (items.isArray())
		for (const auto& it : items)
		{
			int qty = it.get("qty", 0).asInt();
			RuleItem* r = mod->getItem(it.get("rule", "").asString(), false);
			if (r && qty > 0) base->getStorageItems()->removeItem(r, qty);
		}

	const Json::Value& soldiers = payload["soldiers"];
	if (soldiers.isArray())
		for (const auto& sid : soldiers)
		{
			int id = sid.asInt();
			for (auto it = base->getSoldiers()->begin(); it != base->getSoldiers()->end(); ++it)
			{
				Soldier* s = *it;
				if (s->getId() == id && s->getCraft() == 0)
				{
					if (s->getArmor()->getStoreItem())
						base->getStorageItems()->addItem(s->getArmor()->getStoreItem());
					base->getSoldiers()->erase(it);
					delete s;
					break;
				}
			}
		}

	const Json::Value& crafts = payload["crafts"];
	if (crafts.isArray())
		for (const auto& jc : crafts)
		{
			Craft* c = findCraft(base, jc.get("id", -1).asInt(), jc.get("type", "").asString());
			if (c) { base->removeCraft(c, true); delete c; }
		}

	int sci = payload.get("scientists", 0).asInt();
	int eng = payload.get("engineers", 0).asInt();
	if (sci > 0) base->setScientists(base->getScientists() - sci);
	if (eng > 0) base->setEngineers(base->getEngineers() - eng);
}

// ---- PRD-J05: "containment" --------------------------------------------------
// Payload: { prisoners:[{rule, qty}], sell:bool }. Mirrors
// ManageAlienContainmentState::dealWithSelectedAliens: remove live aliens from
// storage; if sell -> credit funds (host-authoritative), else (execute) -> add
// the geoscape corpse. Atomic re-price on the host.

bool containmentValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                         int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) { failReason = "no world"; return false; }
	bool sell = payload.get("sell", false).asBool();

	int64_t credit = 0;
	const Json::Value& prisoners = payload["prisoners"];
	if (!prisoners.isArray() || prisoners.empty()) { failReason = "no prisoners"; return false; }
	for (const auto& p : prisoners)
	{
		std::string rule = p.get("rule", "").asString();
		int qty = p.get("qty", 0).asInt();
		if (qty <= 0) continue;
		RuleItem* r = mod->getItem(rule, false);
		if (!r) { failReason = "unknown alien: " + rule; return false; }
		if (base->getStorageItems()->getItem(r) < qty)
			{ failReason = "STR_NOT_ENOUGH_PRISONERS"; return false; }
		if (sell) credit += (int64_t)qty * r->getSellCostAdjusted(base, save);
	}
	cost = sell ? -credit : 0;
	return true;
}

void containmentApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Mod* mod = game->getMod();
	if (!mod) return;
	bool sell = payload.get("sell", false).asBool();

	const Json::Value& prisoners = payload["prisoners"];
	if (!prisoners.isArray()) return;
	for (const auto& p : prisoners)
	{
		std::string rule = p.get("rule", "").asString();
		int qty = p.get("qty", 0).asInt();
		if (qty <= 0) continue;
		RuleItem* r = mod->getItem(rule, false);
		if (!r) continue;
		base->getStorageItems()->removeItem(r, qty);
		if (!sell)
		{
			// Execute: leave the geoscape corpse behind (funds untouched).
			Unit* ruleUnit = mod->getUnit(rule, false);
			if (ruleUnit)
			{
				auto* ruleCorpse = ruleUnit->getArmor()->getCorpseGeoscape();
				if (ruleCorpse && ruleCorpse->isRecoverable() && ruleCorpse->isCorpseRecoverable())
					base->getStorageItems()->addItem(ruleCorpse, qty);
			}
		}
	}
}

// ---- PRD-J05: "transfer" (intra-world base -> base) --------------------------
// Payload: { toBaseId:int, items:[{rule, qty}], soldiers:[id...],
//            crafts:[{id,type}...], scientists:int, engineers:int }.
// baseId = SOURCE base. JOINT transfers are vanilla intra-world moves (cost +
// travel time), NOT the SEPARATE cross-player syncTrade flow. The created
// Transfer objects arrive later via the J04 transfer_arrived broadcast, so host
// and replica must build them in identical order (they run the same applier).

bool transferValidate(Game* game, const Json::Value& payload, Base* fromBase, int /*seat*/,
                      int64_t& cost, std::string& failReason)
{
	if (!fromBase) { failReason = "source base not found"; return false; }
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) { failReason = "no world"; return false; }

	Base* toBase = resolveBase(game, payload.get("toBaseId", -1).asInt());
	if (!toBase || toBase == fromBase) { failReason = "bad destination base"; return false; }

	double distance = baseDistance(fromBase, toBase);
	int itemCost = (int)(1 * distance);
	int soldierCost = (int)(5 * distance);
	int craftCost = (int)(25 * distance);
	int sciCost = (int)(5 * distance);
	int engCost = (int)(5 * distance);

	int64_t total = 0;
	double storeAddTo = 0.0;

	const Json::Value& items = payload["items"];
	if (items.isArray())
		for (const auto& it : items)
		{
			std::string rule = it.get("rule", "").asString();
			int qty = it.get("qty", 0).asInt();
			if (qty <= 0) continue;
			RuleItem* r = mod->getItem(rule, false);
			if (!r) { failReason = "unknown item: " + rule; return false; }
			if (fromBase->getStorageItems()->getItem(r) < qty)
				{ failReason = "STR_NOT_ENOUGH_ITEMS_TO_TRANSFER"; return false; }
			total += (int64_t)qty * itemCost;
			storeAddTo += (double)qty * r->getSize();
		}

	const Json::Value& soldiers = payload["soldiers"];
	if (soldiers.isArray())
		for (const auto& sid : soldiers)
		{
			int id = sid.asInt();
			bool found = false;
			for (auto* s : *fromBase->getSoldiers())
				if (s->getId() == id && s->getCraft() == 0) { found = true; break; }
			if (!found) { failReason = "soldier not transferable"; return false; }
			total += soldierCost;
		}

	const Json::Value& crafts = payload["crafts"];
	if (crafts.isArray())
		for (const auto& jc : crafts)
		{
			Craft* c = findCraft(fromBase, jc.get("id", -1).asInt(), jc.get("type", "").asString());
			if (!c) { failReason = "craft not transferable"; return false; }
			total += craftCost;
		}

	int sci = payload.get("scientists", 0).asInt();
	int eng = payload.get("engineers", 0).asInt();
	if (sci > fromBase->getAvailableScientists()) { failReason = "not enough scientists"; return false; }
	if (eng > fromBase->getAvailableEngineers()) { failReason = "not enough engineers"; return false; }
	total += (int64_t)sci * sciCost;
	total += (int64_t)eng * engCost;

	if (save->getFunds() < total) { failReason = "STR_NOT_ENOUGH_MONEY"; return false; }
	if (Options::storageLimitsEnforced && toBase->storesOverfull(storeAddTo))
		{ failReason = "STR_NOT_ENOUGH_STORE_SPACE"; return false; }

	cost = total; // positive debit
	return true;
}

void transferApply(Game* game, Json::Value& payload, Base* fromBase, int /*seat*/)
{
	if (!fromBase) return;
	Mod* mod = game->getMod();
	if (!mod) return;
	Base* toBase = resolveBase(game, payload.get("toBaseId", -1).asInt());
	if (!toBase) return;

	double distance = baseDistance(fromBase, toBase);
	int time = (int)floor(6 + distance / 10.0);

	const Json::Value& items = payload["items"];
	if (items.isArray())
		for (const auto& it : items)
		{
			int qty = it.get("qty", 0).asInt();
			RuleItem* r = mod->getItem(it.get("rule", "").asString(), false);
			if (!r || qty <= 0) continue;
			fromBase->getStorageItems()->removeItem(r, qty);
			Transfer* t = new Transfer(time);
			t->setItems(r, qty);
			toBase->getTransfers()->push_back(t);
		}

	const Json::Value& soldiers = payload["soldiers"];
	if (soldiers.isArray())
		for (const auto& sid : soldiers)
		{
			int id = sid.asInt();
			for (auto it = fromBase->getSoldiers()->begin(); it != fromBase->getSoldiers()->end(); ++it)
			{
				Soldier* s = *it;
				if (s->getId() == id && s->getCraft() == 0)
				{
					s->setPsiTraining(false);
					if (s->isInTraining()) s->setReturnToTrainingWhenHealed(true);
					s->setTraining(false);
					// Ownership unchanged by a transfer (PRD-J05).
					Transfer* t = new Transfer(time);
					t->setSoldier(s);
					toBase->getTransfers()->push_back(t);
					fromBase->getSoldiers()->erase(it);
					break;
				}
			}
		}

	const Json::Value& crafts = payload["crafts"];
	if (crafts.isArray())
		for (const auto& jc : crafts)
		{
			Craft* craft = findCraft(fromBase, jc.get("id", -1).asInt(), jc.get("type", "").asString());
			if (!craft) continue;
			// Move the craft's assigned soldiers with it (non-airborne path).
			for (auto it = fromBase->getSoldiers()->begin(); it != fromBase->getSoldiers()->end();)
			{
				Soldier* s = *it;
				if (s->getCraft() == craft)
				{
					s->setPsiTraining(false);
					if (s->isInTraining()) s->setReturnToTrainingWhenHealed(true);
					s->setTraining(false);
					Transfer* t = new Transfer(time);
					t->setSoldier(s);
					toBase->getTransfers()->push_back(t);
					it = fromBase->getSoldiers()->erase(it);
				}
				else ++it;
			}
			fromBase->removeCraft(craft, false);
			Transfer* t = new Transfer(time);
			t->setCraft(craft);
			toBase->getTransfers()->push_back(t);
		}

	int sci = payload.get("scientists", 0).asInt();
	int eng = payload.get("engineers", 0).asInt();
	if (sci > 0)
	{
		fromBase->setScientists(fromBase->getScientists() - sci);
		Transfer* t = new Transfer(time);
		t->setScientists(sci);
		toBase->getTransfers()->push_back(t);
	}
	if (eng > 0)
	{
		fromBase->setEngineers(fromBase->getEngineers() - eng);
		Transfer* t = new Transfer(time);
		t->setEngineers(eng);
		toBase->getTransfers()->push_back(t);
	}
}

// ---- PRD-J06: research start / allocate / cancel -----------------------------
// A JOINT research screen mutates NOTHING locally; the OK/Start/Cancel buttons
// emit one of these commands and the host-authoritative world settles via
// joint_apply. Completion stays host-driven (J04 research_done). Research has no
// funds cost, so every validator here sets cost 0 (the apply still carries the
// authoritative funds per the protocol invariant).

// res_start payload: { project:<ruleName> } (+ host-resolved "cost").
bool resStartValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                      int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) { failReason = "no world"; return false; }
	std::string pname = payload.get("project", "").asString();
	RuleResearch* rule = mod->getResearch(pname, false);
	if (!rule) { failReason = "unknown research: " + pname; return false; }
	// Availability = rules + not-already-running-here + needed item + base funcs.
	std::vector<RuleResearch*> avail;
	save->getAvailableResearchProjects(avail, mod, base, false);
	bool ok = false;
	for (auto* r : avail) if (r == rule) { ok = true; break; }
	if (!ok) { failReason = "STR_RESEARCH_NOT_AVAILABLE"; return false; }
	cost = 0;
	return true;
}

void resStartApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) return;
	std::string pname = payload.get("project", "").asString();
	RuleResearch* rule = mod->getResearch(pname, false);
	if (!rule) return;
	if (findResearchProject(base, pname)) return; // idempotent guard

	// Randomised cost is HOST-only RNG (vanilla ResearchInfoState ctor). Resolve it
	// once on the host, write it into the payload, and let replicas adopt it - the
	// buy-soldier pattern - so host and replica hold the same _cost.
	int projCost;
	if (connectionTCP::getHost() && !payload.isMember("cost"))
	{
		int rng = RNG::generate(50, 150);
		projCost = rule->getCost() * rng / 100;
		if (rule->getCost() > 0) projCost = std::max(1, projCost);
		payload["cost"] = projCost;
	}
	else
	{
		projCost = payload.get("cost", rule->getCost()).asInt();
	}
	ResearchProject* proj = new ResearchProject(rule, projCost);
	base->addResearch(proj); // 0 scientists (allocation is a separate res_alloc)
	// Consume the needed item exactly as the vanilla start does (deterministic).
	if (rule->needItem() && rule->destroyItem())
		base->getStorageItems()->removeItem(rule->getNeededItem(), 1);
}

// res_alloc payload: { project:<ruleName>, assigned:<absolute int> }. ABSOLUTE
// target (last-write-wins) so two players adjusting the same project converge
// instead of compounding deltas.
bool resAllocValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                      int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	std::string pname = payload.get("project", "").asString();
	ResearchProject* proj = findResearchProject(base, pname);
	if (!proj) { failReason = "research not running: " + pname; return false; }
	int target = payload.get("assigned", -1).asInt();
	if (target < 0) { failReason = "bad allocation"; return false; }
	int delta = target - proj->getAssigned();
	if (delta > base->getAvailableScientists()) { failReason = "STR_NOT_ENOUGH_SCIENTISTS"; return false; }
	if (delta > base->getFreeLaboratories()) { failReason = "STR_NOT_ENOUGH_LABORATORY_SPACE"; return false; }
	cost = 0;
	return true;
}

void resAllocApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	std::string pname = payload.get("project", "").asString();
	ResearchProject* proj = findResearchProject(base, pname);
	if (!proj) return;
	int target = payload.get("assigned", proj->getAssigned()).asInt();
	int delta = target - proj->getAssigned(); // replica's current matches host (FIFO)
	proj->setAssigned(target);
	base->setScientists(base->getScientists() - delta);
}

// res_cancel payload: { project:<ruleName> }.
bool resCancelValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                       int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	std::string pname = payload.get("project", "").asString();
	if (!findResearchProject(base, pname)) { failReason = "research not running: " + pname; return false; }
	cost = 0;
	return true;
}

void resCancelApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	std::string pname = payload.get("project", "").asString();
	ResearchProject* proj = findResearchProject(base, pname);
	if (!proj) return;
	// removeResearch frees the assigned scientists AND (if unfinished + needItem +
	// destroyItem) refunds the needed item - identical on host and replica because
	// the project's assigned is kept in lock-step and a running project is
	// not-finished on both sides.
	base->removeResearch(proj);
}

// ---- PRD-J06: manufacture start / allocate / cancel --------------------------
// man_start/man_cancel change funds (first-unit debit / refund); man_alloc does
// not but carries funds anyway (protocol invariant). The host re-validates funds,
// materials, engineer pool + workshop space against the live world.

// man_start payload: { item:<ruleName>, engineers, qty, infinite, sell, fallback }.
bool manStartValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                      int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) { failReason = "no world"; return false; }
	std::string item = payload.get("item", "").asString();
	RuleManufacture* rule = mod->getManufacture(item, false);
	if (!rule) { failReason = "unknown manufacture: " + item; return false; }
	// Availability = requirements researched + not-already-producing-here.
	std::vector<RuleManufacture*> avail;
	save->getAvailableProductions(avail, mod, base, MANU_FILTER_DEFAULT);
	bool ok = false;
	for (auto* m : avail) if (m == rule) { ok = true; break; }
	if (!ok) { failReason = "STR_PRODUCTION_NOT_AVAILABLE"; return false; }

	int engineers = payload.get("engineers", 0).asInt();
	if (engineers < 0) { failReason = "bad allocation"; return false; }
	// First-unit gate (vanilla startItem): funds + materials + crafts.
	if (save->getFunds() < rule->getManufactureCost()) { failReason = "STR_NOT_ENOUGH_MONEY"; return false; }
	for (const auto& i : rule->getRequiredItems())
		if (base->getStorageItems()->getItem(i.first) < i.second)
			{ failReason = "STR_NOT_ENOUGH_SPECIAL_MATERIALS"; return false; }
	for (const auto& i : rule->getRequiredCrafts())
		if (base->getCraftCountForProduction(i.first) < i.second)
			{ failReason = "STR_NOT_ENOUGH_SPECIAL_MATERIALS"; return false; }
	// Engineer pool + workshop (flat requiredSpace on activation + one slot each).
	if (engineers > base->getAvailableEngineers()) { failReason = "STR_NOT_ENOUGH_ENGINEERS"; return false; }
	int wsNeed = engineers + (engineers > 0 ? rule->getRequiredSpace() : 0);
	if (wsNeed > base->getFreeWorkshops()) { failReason = "STR_NOT_ENOUGH_WORK_SPACE"; return false; }
	if (rule->getProducedCraft() && base->getAvailableHangars() - base->getUsedHangars() <= 0)
		{ failReason = "STR_NO_FREE_HANGARS_FOR_CRAFT_PRODUCTION"; return false; }
	if (!rule->getSpawnedPersonType().empty() && base->getAvailableQuarters() <= base->getUsedQuarters())
		{ failReason = "STR_NOT_ENOUGH_LIVING_SPACE"; return false; }

	cost = rule->getManufactureCost(); // debit the first unit; protocol adjusts funds
	return true;
}

void manStartApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Mod* mod = game->getMod();
	if (!mod) return;
	std::string item = payload.get("item", "").asString();
	RuleManufacture* rule = mod->getManufacture(item, false);
	if (!rule) return;
	if (findProduction(base, item)) return; // idempotent guard

	int engineers = payload.get("engineers", 0).asInt();
	int qty = payload.get("qty", 1).asInt();
	if (qty < 1) qty = 1;
	Production* p = new Production(rule, qty);
	p->setInfiniteAmount(payload.get("infinite", false).asBool());
	p->setAssignedEngineers(engineers);
	p->setSellItems(payload.get("sell", false).asBool());
	base->addProduction(p);
	base->setEngineers(base->getEngineers() - engineers);
	if (payload.get("fallback", false).asBool())
	{
		for (auto* pp : base->getProductions()) pp->setFallback(false);
		p->setFallback(true);
	}
	// Vanilla startItem's non-funds effects (funds are the protocol's job): remove
	// the first unit's required items + consume one matching required craft each.
	for (const auto& i : rule->getRequiredItems())
		base->getStorageItems()->removeItem(i.first, i.second);
	for (const auto& i : rule->getRequiredCrafts())
	{
		for (auto* c : *base->getCrafts())
		{
			if (c->getRules() == i.first) { base->removeCraft(c, true); delete c; break; }
		}
	}
}

// man_alloc payload: { item, engineers, qty, infinite, sell, fallback } (absolute).
bool manAllocValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                      int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	std::string item = payload.get("item", "").asString();
	Production* p = findProduction(base, item);
	if (!p) { failReason = "production not running: " + item; return false; }
	int engineers = payload.get("engineers", -1).asInt();
	if (engineers < 0) { failReason = "bad allocation"; return false; }
	int delta = engineers - p->getAssignedEngineers();
	if (delta > base->getAvailableEngineers()) { failReason = "STR_NOT_ENOUGH_ENGINEERS"; return false; }
	int wsNeed = delta;
	if (p->isQueuedOnly() && engineers > 0) wsNeed += p->getRules()->getRequiredSpace();
	if (wsNeed > base->getFreeWorkshops()) { failReason = "STR_NOT_ENOUGH_WORK_SPACE"; return false; }
	cost = 0;
	return true;
}

void manAllocApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	std::string item = payload.get("item", "").asString();
	Production* p = findProduction(base, item);
	if (!p) return;
	int engineers = payload.get("engineers", p->getAssignedEngineers()).asInt();
	int delta = engineers - p->getAssignedEngineers();
	p->setAssignedEngineers(engineers);
	base->setEngineers(base->getEngineers() - delta);
	p->setInfiniteAmount(payload.get("infinite", p->getInfiniteAmount()).asBool());
	int qty = payload.get("qty", p->getAmountTotal()).asInt();
	if (qty >= 1) p->setAmountTotal(qty);
	p->setSellItems(payload.get("sell", p->getSellItems()).asBool());
	if (payload.get("fallback", false).asBool())
	{
		for (auto* pp : base->getProductions()) pp->setFallback(false);
		p->setFallback(true);
	}
	else p->setFallback(false);
}

// man_cancel payload: { item, refund } (refund re-derived host-side from the rule).
bool manCancelValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                       int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	std::string item = payload.get("item", "").asString();
	Production* p = findProduction(base, item);
	if (!p) { failReason = "production not running: " + item; return false; }
	// Refund is a property of the rule, not a client choice - re-derive it.
	cost = p->getRules()->getRefund() ? -(int64_t)p->getRules()->getManufactureCost() : 0;
	return true;
}

void manCancelApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	std::string item = payload.get("item", "").asString();
	Production* p = findProduction(base, item);
	if (!p) return;
	const RuleManufacture* rule = p->getRules();
	if (rule->getRefund())
	{
		// refundItem's non-funds effects (funds via the protocol credit above).
		for (const auto& i : rule->getRequiredItems())
			base->getStorageItems()->addItem(i.first, i.second);
	}
	base->removeProduction(p); // frees engineers, deletes the Production
}

// ---- PRD-J07: facilities, rename, sack, new base, base destroyed -------------
// Shared-base construction/management via joint_cmd. baseId = index into
// getBases() (load-bearing: every command routes by it and base add/remove rides
// joint_apply so host and replicas keep the index in lock-step).

// A transient, headless BaseView so the host validates/applies facility placement
// EXACTLY as the interactive PlaceFacilityState does (connectivity/overlap rules,
// build queue), without a mouse. Caller deletes it.
BaseView* makeGridView(Base* base, int x, int y)
{
	BaseView* v = new BaseView(192, 192, 0, 8);
	v->setBase(base);
	v->setGridPosition(x, y);
	return v;
}

// Accumulate the funds refund + item refunds that placing @a rule at (x,y) would
// yield by building over the facilities it intersects - byte-identical to
// PlaceFacilityState::viewClick's removal loop, but WITHOUT mutating (validator
// side). Funds ride the protocol cost; items are re-derived by the applier.
void accumulateBuildOverRefunds(Game* game, Base* base, const RuleBaseFacility* rule,
                                int gridX, int gridY, int64_t& refundValue,
                                std::map<std::string, int>& refundItems)
{
	refundValue = 0;
	const BaseAreaSubset area = BaseAreaSubset(rule->getSizeX(), rule->getSizeY()).offset(gridX, gridY);
	for (int i = (int)base->getFacilities()->size() - 1; i >= 0; --i)
	{
		BaseFacility* over = base->getFacilities()->at(i);
		if (!BaseAreaSubset::intersection(area, over->getPlacement())) continue;
		const auto& itemCost = over->getRules()->getBuildCostItems();
		if (over->getBuildTime() > over->getRules()->getBuildTime())
		{
			refundValue += over->getRules()->getBuildCost();
			for (auto& it : itemCost) refundItems[it.first] += it.second.first;
		}
		else
		{
			refundValue += over->getRules()->getRefundValue();
			for (auto& it : itemCost) refundItems[it.first] += it.second.second;
		}
		if (over->getAmmo() > 0)
			refundItems[over->getRules()->getAmmoItem()->getType()] += over->getAmmo();
	}
	(void)game;
}

// fac_build payload: { facilityType:<ruleName>, x, y }. Client-originated START of
// a facility build (analogous to man_start). Host re-validates placement + funds +
// items against the live world; the vanilla validity re-check IS the tile-conflict
// guard (two players targeting the same tiles -> loser gets joint_fail).
bool facBuildValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                      int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) { failReason = "no world"; return false; }
	RuleBaseFacility* rule = mod->getBaseFacility(payload.get("facilityType", "").asString(), false);
	if (!rule) { failReason = "unknown facility"; return false; }
	// Buildability gates (BuildFacilitiesState/PlaceLiftState list filters).
	if (!save->isResearched(rule->getRequirements()))
		{ failReason = "facility not researched"; return false; }
	if (!rule->isAllowedForBaseType(base->isFakeUnderwater()))
		{ failReason = "facility not allowed for base type"; return false; }
	int x = payload.get("x", -1).asInt();
	int y = payload.get("y", -1).asInt();

	BaseView* v = makeGridView(base, x, y);
	BasePlacementErrors err = v->getPlacementError(rule);
	delete v;
	if (err != BPE_None) { failReason = "STR_CANNOT_BUILD_HERE"; return false; }

	int64_t refundValue = 0;
	std::map<std::string, int> refundItems;
	accumulateBuildOverRefunds(game, base, rule, x, y, refundValue, refundItems);

	int64_t net = (int64_t)rule->getBuildCost() - refundValue;
	if (save->getFunds() < net) { failReason = "STR_NOT_ENOUGH_MONEY"; return false; }
	for (const auto& item : rule->getBuildCostItems())
	{
		int needed = (item.second.first - refundItems[item.first]) - base->getStorageItems()->getItem(item.first);
		if (needed > 0) { failReason = "STR_NOT_ENOUGH_ITEMS"; return false; }
	}
	cost = net; // net debit (build cost minus build-over refunds); funds via protocol
	return true;
}

void facBuildApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Mod* mod = game->getMod();
	if (!mod) return;
	RuleBaseFacility* rule = mod->getBaseFacility(payload.get("facilityType", "").asString(), false);
	if (!rule) return;
	int gridX = payload.get("x", -1).asInt();
	int gridY = payload.get("y", -1).asInt();

	BaseView* view = makeGridView(base, gridX, gridY);

	// Remove any facilities we're building over (refunding items only; funds are the
	// protocol's job, resolved into the command cost). Mirrors PlaceFacilityState.
	double reducedBuildTime = 0.0;
	bool buildingOver = false;
	const BaseAreaSubset areaToBuildOver = BaseAreaSubset(rule->getSizeX(), rule->getSizeY()).offset(gridX, gridY);
	for (int i = (int)base->getFacilities()->size() - 1; i >= 0; --i)
	{
		BaseFacility* checkFacility = base->getFacilities()->at(i);
		if (!BaseAreaSubset::intersection(areaToBuildOver, checkFacility->getPlacement())) continue;
		const auto& itemCost = checkFacility->getRules()->getBuildCostItems();
		if (checkFacility->getBuildTime() > checkFacility->getRules()->getBuildTime())
		{
			for (auto& item : itemCost)
				base->getStorageItems()->addItem(mod->getItem(item.first, true), item.second.first);
		}
		else
		{
			for (auto& item : itemCost)
				base->getStorageItems()->addItem(mod->getItem(item.first, true), item.second.second);
			double oldSizeSquared = (checkFacility->getRules()->getSizeX() * checkFacility->getRules()->getSizeY());
			double newSizeSquared = (rule->getSizeX() * rule->getSizeY());
			reducedBuildTime += (checkFacility->getRules()->getBuildTime() - checkFacility->getBuildTime()) * oldSizeSquared / newSizeSquared;
			if (checkFacility->getBuildTime() == 0) buildingOver = true;
		}
		if (checkFacility->getAmmo() > 0)
		{
			base->getStorageItems()->addItem(checkFacility->getRules()->getAmmoItem(), checkFacility->getAmmo());
			checkFacility->setAmmo(0);
		}
		base->getFacilities()->erase(base->getFacilities()->begin() + i);
		delete checkFacility;
	}

	BaseFacility* fac = new BaseFacility(rule, base);
	fac->setX(gridX);
	fac->setY(gridY);
	fac->setBuildTime(rule->getBuildTime());
	if (buildingOver)
	{
		fac->setIfHadPreviousFacility(true);
		reducedBuildTime = reducedBuildTime * mod->getBuildTimeReductionScaling() / 100.0;
		int reducedBuildTimeRounded = (int)std::round(reducedBuildTime);
		fac->setBuildTime(std::max(1, fac->getBuildTime() - reducedBuildTimeRounded));
	}
	base->getFacilities()->push_back(fac);
	if (Options::allowBuildingQueue)
	{
		if (view->isQueuedBuilding(rule)) fac->setBuildTime(INT_MAX);
		view->reCalcQueuedBuildings();
	}
	// Debit the build-cost items (funds handled by the protocol).
	for (const auto& item : rule->getBuildCostItems())
		base->getStorageItems()->removeItem(item.first, item.second.first);
	delete view;
}

// fac_dismantle payload: { x, y }. Dismantle the facility at (x,y). If it is the
// access lift (last facility), the whole base is removed - both cases ride
// joint_apply so the base-index stays in lock-step. Refund funds ride the cost.
bool facDismantleValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                          int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	int x = payload.get("x", -1).asInt();
	int y = payload.get("y", -1).asInt();
	BaseFacility* fac = nullptr;
	for (auto* f : *base->getFacilities())
		if (f->getX() == x && f->getY() == y) { fac = f; break; }
	if (!fac) { failReason = "facility not found"; return false; }

	if (fac->getRules()->isLift())
	{
		// dismantling the access lift removes the whole base; no refund (vanilla).
		cost = 0;
		return true;
	}
	// Re-run the vanilla dismantle-ability guards (BasescapeState checked them before
	// opening the dialog; re-check on the host in case the world changed).
	if (fac->inUse()) { failReason = "STR_FACILITY_IN_USE"; return false; }
	if (!base->getDisconnectedFacilities(fac).empty() && fac->getRules()->getLeavesBehindOnSell().empty())
		{ failReason = "STR_CANNOT_DISMANTLE_FACILITY"; return false; }
	if (fac->getBuildTime() > 0 && fac->getIfHadPreviousFacility())
		{ failReason = "STR_CANNOT_DISMANTLE_FACILITY_UPGRADING"; return false; }

	// Refund (credit): full if a not-yet-started queued build, else partial.
	int64_t refund = (fac->getBuildTime() > fac->getRules()->getBuildTime())
		? fac->getRules()->getBuildCost() : fac->getRules()->getRefundValue();
	cost = -refund; // negative debit = credit; a negative refund becomes an expense
	return true;
}

void facDismantleApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Mod* mod = game->getMod();
	SavedGame* save = game->getSavedGame();
	if (!mod || !save) return;
	int x = payload.get("x", -1).asInt();
	int y = payload.get("y", -1).asInt();
	BaseFacility* fac = nullptr;
	for (auto* f : *base->getFacilities())
		if (f->getX() == x && f->getY() == y) { fac = f; break; }
	if (!fac) return;

	if (fac->getRules()->isLift())
	{
		// Remove the whole base (index lock-step: both host and replica erase the
		// same index; subsequent bases shift identically).
		auto* bases = save->getBases();
		for (auto it = bases->begin(); it != bases->end(); ++it)
			if (*it == base) { save->stopHuntingXcomCrafts(base); bases->erase(it); delete base; break; }
		return;
	}

	// Item refund (funds ride the protocol credit): full if queued, else partial.
	const auto& itemCost = fac->getRules()->getBuildCostItems();
	if (fac->getBuildTime() > fac->getRules()->getBuildTime())
		for (auto& pair : itemCost) base->getStorageItems()->addItem(mod->getItem(pair.first, true), pair.second.first);
	else
		for (auto& pair : itemCost) base->getStorageItems()->addItem(mod->getItem(pair.first, true), pair.second.second);
	if (fac->getAmmo() > 0)
	{
		base->getStorageItems()->addItem(fac->getRules()->getAmmoItem(), fac->getAmmo());
		fac->setAmmo(0);
	}

	for (auto facIt = base->getFacilities()->begin(); facIt != base->getFacilities()->end(); ++facIt)
	{
		if (*facIt != fac) continue;
		base->getFacilities()->erase(facIt);
		// Leaves-behind facilities (mods): mirror PlaceFacilityState's rules exactly.
		if (fac->getBuildTime() == 0 && !fac->getRules()->getLeavesBehindOnSell().empty())
		{
			const auto& facList = fac->getRules()->getLeavesBehindOnSell();
			if (facList.at(0)->getSizeX() == fac->getRules()->getSizeX() && facList.at(0)->getSizeY() == fac->getRules()->getSizeY())
			{
				BaseFacility* nf = new BaseFacility(facList.at(0), base);
				nf->setX(fac->getX());
				nf->setY(fac->getY());
				nf->setBuildTime(fac->getRules()->getRemovalTime() <= -1 ? nf->getRules()->getBuildTime() : fac->getRules()->getRemovalTime());
				if (nf->getBuildTime() != 0) nf->setIfHadPreviousFacility(true);
				base->getFacilities()->push_back(nf);
			}
			else
			{
				size_t j = 0;
				for (int ny = fac->getY(); ny != fac->getY() + fac->getRules()->getSizeY(); ++ny)
					for (int nx = fac->getX(); nx != fac->getX() + fac->getRules()->getSizeX(); ++nx)
					{
						BaseFacility* nf = new BaseFacility(facList.at(j), base);
						nf->setX(nx);
						nf->setY(ny);
						nf->setBuildTime(fac->getRules()->getRemovalTime() <= -1 ? nf->getRules()->getBuildTime() : fac->getRules()->getRemovalTime());
						if (nf->getBuildTime() != 0) nf->setIfHadPreviousFacility(true);
						base->getFacilities()->push_back(nf);
						if (++j == facList.size()) j = 0;
					}
			}
		}
		delete fac;
		if (Options::allowBuildingQueue)
		{
			BaseView* view = makeGridView(base, x, y);
			view->reCalcQueuedBuildings();
			delete view;
		}
		break;
	}
}

// base_rename payload: { name }. Replaces the SEPARATE changeBaseName packet in
// JOINT; host applies + broadcasts (last-write-wins).
bool baseRenameValidate(Game* /*game*/, const Json::Value& /*payload*/, Base* base, int /*seat*/,
                        int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	cost = 0;
	return true;
}
void baseRenameApply(Game* /*game*/, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	base->setName(payload.get("name", base->getName()).asString());
}

// sack payload: { soldierId }. Policy: ANY player may sack ANY soldier (shared
// roster management, consistent with J05 sell). No refund (vanilla).
bool sackValidate(Game* /*game*/, const Json::Value& payload, Base* base, int /*seat*/,
                  int64_t& cost, std::string& failReason)
{
	if (!base) { failReason = "base not found"; return false; }
	int id = payload.get("soldierId", -1).asInt();
	for (auto* s : *base->getSoldiers())
		if (s->getId() == id) { cost = 0; return true; }
	failReason = "soldier not found";
	return false;
}
void sackApply(Game* /*game*/, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	int id = payload.get("soldierId", -1).asInt();
	for (auto it = base->getSoldiers()->begin(); it != base->getSoldiers()->end(); ++it)
	{
		Soldier* s = *it;
		if (s->getId() != id) continue;
		if (s->getArmor()->getStoreItem())
			base->getStorageItems()->addItem(s->getArmor()->getStoreItem());
		base->getSoldiers()->erase(it);
		delete s;
		break;
	}
}

// base_new payload: { lon, lat, name, liftType, liftX, liftY } (+ host-resolved
// coopbaseid). Client-originated creation of a SUBSEQUENT base (the initial
// campaign base is J02's, host-side pre-stream). baseId is -1 (no existing base);
// the applier appends the new base at the SAME index on host and every replica so
// the index stays in lock-step. Host debits the region base cost once.
bool baseNewValidate(Game* game, const Json::Value& payload, Base* /*base*/, int /*seat*/,
                     int64_t& cost, std::string& failReason)
{
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) { failReason = "no world"; return false; }
	double lon = payload.get("lon", 0.0).asDouble();
	double lat = payload.get("lat", 0.0).asDouble();
	int regionCost = -1;
	for (const auto* region : *save->getRegions())
		if (region->getRules()->insideRegion(lon, lat)) { regionCost = region->getRules()->getBaseCost(); break; }
	if (regionCost < 0) { failReason = "no region for base"; return false; }
	if (!mod->getBaseFacility(payload.get("liftType", "").asString(), false))
		{ failReason = "unknown access lift"; return false; }
	if (save->getFunds() < regionCost) { failReason = "STR_NOT_ENOUGH_MONEY"; return false; }
	cost = regionCost;
	return true;
}

void baseNewApply(Game* game, Json::Value& payload, Base* /*base*/, int /*seat*/)
{
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) return;

	Base* nb = new Base(mod); // ctor random-mints _coop_base_id
	nb->setFakeUnderwater(payload.get("fakeUnderwater", false).asBool());
	nb->setLongitude(payload.get("lon", 0.0).asDouble());
	nb->setLatitude(payload.get("lat", 0.0).asDouble());
	nb->setName(payload.get("name", "").asString());
	nb->calculateServices(save);

	// coopbaseid: host mints (already done by the ctor); serialize into the payload
	// so replicas adopt the SAME id (the buy-soldier host-RNG-into-payload pattern).
	if (connectionTCP::getHost() && !payload.isMember("coopbaseid"))
		payload["coopbaseid"] = nb->_coop_base_id;
	else if (payload.isMember("coopbaseid"))
		nb->_coop_base_id = payload["coopbaseid"].asInt();

	RuleBaseFacility* liftRule = mod->getBaseFacility(payload.get("liftType", "").asString(), false);
	if (liftRule)
	{
		BaseFacility* lift = new BaseFacility(liftRule, nb); // default buildTime 0 = instant
		lift->setX(payload.get("liftX", 0).asInt());
		lift->setY(payload.get("liftY", 0).asInt());
		nb->getFacilities()->push_back(lift);
	}
	nb->calculateServices(save);
	save->getBases()->push_back(nb);
}

// base_destroyed payload: { name } (baseId = index of the destroyed base). Host
// simulates retaliation (J04) and removes the base in BaseDestroyedState; this
// mirrors the removal to replicas (applier runs REPLICA-ONLY: the host already
// erased the base) and pops an informational popup.
void baseDestroyedApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return; // host already removed it in BaseDestroyedState
	if (!base) return;
	SavedGame* save = game->getSavedGame();
	if (!save) return;
	std::string name = payload.get("name", base->getName()).asString();
	auto* bases = save->getBases();
	for (auto it = bases->begin(); it != bases->end(); ++it)
		if (*it == base) { save->stopHuntingXcomCrafts(base); bases->erase(it); delete base; break; }
	auto* itf = game->getMod()->getInterface("geoscape");
	game->pushState(new ErrorMessageState(
		game->getLanguage()->getString("STR_THE_ALIENS_HAVE_DESTROYED_THE_UNDEFENDED_BASE").arg(name),
		game->getScreen()->getPalette(),
		itf->getElement("genericWindow")->color, "BACK01.SCR", itf->getElement("palette")->color));
}

// ---- PRD-J04: host simulation-result commands --------------------------------
// These mirror a host-only completion to replicas. The host has ALREADY applied
// the change via vanilla sim, so:
//   validator : always accept, cost 0 (funds stay host-authoritative; the packet
//               carries getFunds() so replicas re-sync funds for free).
//   applier   : runs on the REPLICA only (early-returns on the host) to avoid a
//               double-apply.

// Locate the live GeoscapeState (for completion popups that need it). Null if the
// replica is currently in a sub-screen/battle -> we simply skip the popup then.
GeoscapeState* findGeoState(Game* game)
{
	if (!game) return nullptr;
	for (auto* st : game->getStates())
	{
		if (auto* gs = dynamic_cast<GeoscapeState*>(st)) return gs;
	}
	return nullptr;
}

// Last wound-recovery value the host broadcast per soldier id, so day_tick only
// carries CHANGED soldiers (process-global; compare-and-set makes stale entries
// self-correct on the next change).
std::unordered_map<int, int> g_soldierRecovery;

bool simAccept(Game* /*game*/, const Json::Value& /*payload*/, Base* /*base*/,
               int /*seat*/, int64_t& cost, std::string& /*failReason*/)
{
	cost = 0; // host-authoritative funds unchanged; broadcast carries getFunds()
	return true;
}

// ---- PRD-J08: shared craft command + dogfight coordination --------------------
// Any player commands any craft. Orders are joint_cmds; the host validates the
// vanilla fuel/crew/status rules against the authoritative world and applies in
// ARRIVAL order (last-command-wins - a later order for the same craft simply
// overrides the destination; no locking). Craft identity across machines =
// rule type + per-type id (the proven findCraft identity).

std::string craftKey(const std::string& type, int id)
{
	return type + "#" + std::to_string(id);
}
std::string craftKey(const Craft* c)
{
	return craftKey(c->getRules()->getType(), c->getId());
}

// Seat of the last APPLIED order per craft. Host and every replica record it
// from the same joint_apply stream, so it is identical everywhere. The host
// routes dogfights by it (the initiating seat flies). -1 / absent = the craft
// was never commanded through the protocol -> treat as host-owned (vanilla).
std::map<std::string, int> g_craftOrderSeat;

// Resolve the ordered craft: prefer the command's base, fall back to a
// world-wide search (the craft may have been transferred since the order).
Craft* resolveOrderCraft(Game* game, const Json::Value& p, Base* base)
{
	int id = p.get("craftId", -1).asInt();
	std::string type = p.get("craftType", "").asString();
	if (base)
		if (Craft* c = findCraft(base, id, type)) return c;
	SavedGame* save = game->getSavedGame();
	if (save)
		for (auto* b : *save->getBases())
			if (Craft* c = findCraft(b, id, type)) return c;
	return nullptr;
}

// Resolve the order's target on THIS machine's world (real shared ids). Null
// for targetType "point" (the applier creates the shared waypoint) or when the
// id does not resolve here (replica snapshot race - harmless, see applier).
Target* resolveOrderTarget(Game* game, const Json::Value& p)
{
	SavedGame* save = game->getSavedGame();
	if (!save) return nullptr;
	std::string tt = p.get("targetType", "").asString();
	int tid = p.get("targetId", -1).asInt();
	if (tt == "ufo")
	{
		for (auto* u : *save->getUfos()) if (u->getId() == tid) return u;
	}
	else if (tt == "site")
	{
		for (auto* s : *save->getMissionSites()) if (s->getId() == tid) return s;
	}
	else if (tt == "abase")
	{
		for (auto* b : *save->getAlienBases()) if (b->getId() == tid) return b;
	}
	else if (tt == "xbase")
	{
		return resolveBase(game, p.get("tBaseId", -1).asInt());
	}
	else if (tt == "xcraft")
	{
		Base* b = resolveBase(game, p.get("tBaseId", -1).asInt());
		if (b) return findCraft(b, p.get("tCraftId", -1).asInt(), p.get("tCraftType", "").asString());
	}
	return nullptr;
}

// craft_launch / craft_retarget payload: { craftId, craftType, targetType:
// "ufo"|"site"|"abase"|"xbase"|"xcraft"|"point", targetId | tBaseId(+tCraftId,
// tCraftType), lon, lat }. baseId = the craft's home-base index. The two cmds
// share semantics (set destination); the distinct names keep intent readable.
bool craftOrderValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                        int64_t& cost, std::string& failReason)
{
	Craft* craft = resolveOrderCraft(game, payload, base);
	if (!craft) { failReason = "craft not found"; return false; }
	// vanilla launch/redirect gate (InterceptState::lstCraftsLeftClick allowStart).
	const std::string status = craft->getStatus();
	bool allow = status == "STR_READY"
		|| ((status == "STR_OUT" || Options::craftLaunchAlways)
			&& !craft->getLowFuel() && !craft->getMissionComplete());
	if (!allow) { failReason = "craft not ready"; return false; }
	// vanilla crew gate (ConfirmDestinationState::btnOkClick).
	if (!craft->arePilotsOnboard(game->getMod())) { failReason = "STR_PILOT_MISSING"; return false; }
	std::string tt = payload.get("targetType", "").asString();
	if (tt != "point" && !resolveOrderTarget(game, payload))
		{ failReason = "target not found"; return false; }
	cost = 0;
	return true;
}

void craftOrderApply(Game* game, Json::Value& payload, Base* base, int seat)
{
	SavedGame* save = game->getSavedGame();
	if (!save) return;
	Craft* craft = resolveOrderCraft(game, payload, base);
	if (!craft) return;
	g_craftOrderSeat[craftKey(craft)] = seat; // initiating seat (dogfight routing)

	std::string tt = payload.get("targetType", "").asString();
	Target* target = nullptr;
	bool resolved = false;
	if (tt == "point")
	{
		// A shared waypoint, created by this applier on the host AND every
		// replica, so the STR_WAY_POINT id counter stays in lock-step.
		Waypoint* w = new Waypoint();
		w->setLongitude(payload.get("lon", 0.0).asDouble());
		w->setLatitude(payload.get("lat", 0.0).asDouble());
		w->setId(save->getId("STR_WAY_POINT"));
		save->getWaypoints()->push_back(w);
		target = w;
		resolved = true;
	}
	else
	{
		target = resolveOrderTarget(game, payload);
		resolved = (target != nullptr);
	}
	if (resolved)
	{
		if (target == craft) target = nullptr; // vanilla: self-target = "patrol here"
		craft->setDestination(target);
	}
	// else: a replica that has not yet seen the target via the position
	// snapshot. Skip the local _dest label; position/status still track the
	// host through the snapshot, and dogfight_start re-asserts _dest if needed.
	if (craft->getRules()->canAutoPatrol())
		craft->setIsAutoPatrolling(false); // vanilla: a new order cancels auto-patrol
	craft->setStatus("STR_OUT");
}

// craft_return / craft_patrol payload: { craftId, craftType } (+ patrol:
// { auto:bool } - true from the geoscape craft dialog, which starts
// auto-patrol on capable craft; false from the "self-target" confirm path).
bool craftExistsValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                         int64_t& cost, std::string& failReason)
{
	if (!resolveOrderCraft(game, payload, base)) { failReason = "craft not found"; return false; }
	cost = 0;
	return true;
}

void craftReturnApply(Game* game, Json::Value& payload, Base* base, int seat)
{
	Craft* craft = resolveOrderCraft(game, payload, base);
	if (!craft) return;
	g_craftOrderSeat[craftKey(craft)] = seat;
	craft->returnToBase();
	if (craft->getRules()->canAutoPatrol())
		craft->setIsAutoPatrolling(false); // vanilla GeoscapeCraftState::btnBaseClick
}

void craftPatrolApply(Game* game, Json::Value& payload, Base* base, int seat)
{
	Craft* craft = resolveOrderCraft(game, payload, base);
	if (!craft) return;
	g_craftOrderSeat[craftKey(craft)] = seat;
	craft->setDestination(0);
	if (payload.get("auto", false).asBool() && craft->getRules()->canAutoPatrol())
	{
		// vanilla GeoscapeCraftState::btnPatrolClick auto-patrol anchor.
		craft->setLatitudeAuto(craft->getLatitude());
		craft->setLongitudeAuto(craft->getLongitude());
		craft->setIsAutoPatrolling(true);
	}
}

// ---- PRD-J09: shared-world squad assembly -----------------------------------
// In JOINT there is ONE base/craft/roster shared by both players, so assigning
// or removing a soldier to/from a craft is a shared-world mutation and rides the
// protocol (never mutated locally on a replica). payload:
//   { craftId, craftType, soldierId, onOff }   baseId = the craft's home-base
// index (the soldier lives at the same base). onOff = the DESIRED final state
// (true = aboard this craft, false = off it) so host and replica converge
// regardless of arrival order (last-write-wins, like the craft orders). Vehicles
// are covered by the same craft space accounting (getSpaceAvailable counts unit
// size); a dedicated vehicle assign command was not needed for the AC.
Soldier* findSoldierAtBase(Base* base, int id)
{
	if (!base) return nullptr;
	for (auto* s : *base->getSoldiers())
		if (s->getId() == id) return s;
	return nullptr;
}

bool craftAssignValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                         int64_t& cost, std::string& failReason)
{
	cost = 0; // no funds effect; broadcast still carries authoritative getFunds()
	if (!base) { failReason = "base not found"; return false; }
	Craft* craft = resolveOrderCraft(game, payload, base);
	if (!craft) { failReason = "craft not found"; return false; }
	Soldier* s = findSoldierAtBase(base, payload.get("soldierId", -1).asInt());
	if (!s) { failReason = "soldier not found"; return false; }
	const bool onOff = payload.get("onOff", false).asBool();
	// A craft already OUT on a mission is locked (vanilla lstSoldiersClick).
	if (s->getCraft() && s->getCraft()->getStatus() == "STR_OUT")
		{ failReason = "craft out on mission"; return false; }
	if (onOff && s->getCraft() != craft)
	{
		// vanilla CraftSoldiersState::lstSoldiersClick add gates.
		if (!s->hasFullHealth()) { failReason = "STR_SOLDIER_NOT_APPROVED"; return false; }
		int space = craft->getSpaceAvailable();
		CraftPlacementErrors err = craft->validateAddingSoldier(space, s);
		if (err != CPE_None) { failReason = "STR_NOT_ENOUGH_CRAFT_SPACE"; return false; }
	}
	return true;
}

void craftAssignApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Craft* craft = resolveOrderCraft(game, payload, base);
	Soldier* s = findSoldierAtBase(base, payload.get("soldierId", -1).asInt());
	if (!craft || !s) return;
	const bool onOff = payload.get("onOff", false).asBool();
	const bool newBattle = game->getSavedGame()->getMonthsPassed() == -1;
	if (onOff)
		s->setCraftAndMoveEquipment(craft, base, newBattle, true);
	else if (s->getCraft() == craft)
		s->setCraftAndMoveEquipment(0, base, newBattle);
}

// ---- PRD-J09 GAP-5: shared-world craft equipment loadout ---------------------
// Equipping a craft at the BASE screen (CraftEquipmentState) moves items between
// the shared base stores and the craft. Those base stores are host-authoritative
// (they are exactly what the GAP-4 checksum sums), so a replica must never mutate
// them locally - it routes the move through this command instead. payload:
//   { craftId, craftType, item, count }   baseId = the craft's home-base index.
// count = the ABSOLUTE desired quantity of `item` loaded on the craft, so host
// and replica converge regardless of arrival order (the J08/J09 last-write-wins
// idiom). Items only; vehicles/ammo are deferred (like craft_assign's vehicle
// variant - CraftEquipmentState still routes non-vehicle items only).
bool craftEquipValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                        int64_t& cost, std::string& failReason)
{
	cost = 0; // no funds effect; broadcast still carries authoritative getFunds()
	if (!base) { failReason = "base not found"; return false; }
	if (!resolveOrderCraft(game, payload, base)) { failReason = "craft not found"; return false; }
	const RuleItem* item = game->getMod()->getItem(payload.get("item", "").asString(), false);
	if (!item) { failReason = "unknown item"; return false; }
	if (item->getVehicleUnit()) { failReason = "vehicles not routed"; return false; }
	return true;
}

void craftEquipApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Craft* craft = resolveOrderCraft(game, payload, base);
	const RuleItem* item = game->getMod()->getItem(payload.get("item", "").asString(), false);
	if (!craft || !item || item->getVehicleUnit()) return;

	ItemContainer* craftItems = craft->getItems();
	ItemContainer* store = base->getStorageItems();
	int current = craftItems->getItem(item);
	int target = payload.get("count", 0).asInt();
	if (target < 0) target = 0;

	// Clamp UP by base availability (never move more than the base holds) and by
	// the craft's capacity, mirroring CraftEquipmentState::moveRightByValue's
	// vanilla gates. Host and replica run this on the one replicated world, so
	// the clamp is identical on both -> they converge with no drift.
	if (target > current + store->getItem(item))
		target = current + store->getItem(item);
	if (target > current)
	{
		int addByCount = craft->getMaxItemsClamped() - craftItems->getTotalQuantity();
		if (addByCount < 0) addByCount = 0;
		if (target - current > addByCount) target = current + addByCount;
		if (item->getSize() > 0.0)
		{
			double freeSpace = craft->getMaxStorageSpaceClamped() + 0.05 - craft->getTotalItemStorageSize();
			int addBySize = (int)std::floor(freeSpace / item->getSize());
			if (addBySize < 0) addBySize = 0;
			if (target - current > addBySize) target = current + addBySize;
		}
	}

	int delta = target - current;
	if (delta > 0)      { craftItems->addItem(item, delta);   store->removeItem(item, delta); }
	else if (delta < 0) { craftItems->removeItem(item, -delta); store->addItem(item, -delta); }
}

// ---- PRD-J09 GAP-5b: shared-world base-screen store mutators ------------------
// Same class as GAP-5 (CraftEquipmentState): a base-screen action moves items in
// and out of the host-authoritative base stores (the exact quantity the GAP-4
// chkItems sums). On a replica those mutations ran ungated and drifted chkItems
// from the host; each now routes an ABSOLUTE end-state command instead. The host
// validates + applies + broadcasts; the applier is pure world-state math run on
// the one replicated world, so host and replica converge with no drift.

// craft_rearm payload: { craftId, craftType, slot, weapon }. weapon="" dismounts
// the slot. baseId = the craft's home-base index. End-state = which craft-weapon
// type is mounted in `slot` (last-write-wins). Mirrors CraftWeaponsState::
// lstWeaponsClick's launcher/clip store moves; clip rearm-over-time stays host-sim
// (J04), as does the deferred re-equip-with-loaded-clips case (checksum backstop).
bool craftRearmValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                        int64_t& cost, std::string& failReason)
{
	cost = 0; // no funds effect; broadcast still carries authoritative getFunds()
	if (!base) { failReason = "base not found"; return false; }
	Craft* craft = resolveOrderCraft(game, payload, base);
	if (!craft) { failReason = "craft not found"; return false; }
	int slot = payload.get("slot", -1).asInt();
	if (slot < 0 || slot >= (int)craft->getWeapons()->size()) { failReason = "bad weapon slot"; return false; }
	std::string wtype = payload.get("weapon", "").asString();
	if (!wtype.empty())
	{
		const RuleCraftWeapon* w = game->getMod()->getCraftWeapon(wtype, false);
		if (!w) { failReason = "unknown craft weapon"; return false; }
		if (!craft->getRules()->isValidWeaponSlot((size_t)slot, w->getWeaponType()))
		{ failReason = "weapon not valid for slot"; return false; }
	}
	return true;
}

void craftRearmApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Craft* craft = resolveOrderCraft(game, payload, base);
	if (!craft) return;
	int slot = payload.get("slot", -1).asInt();
	if (slot < 0 || slot >= (int)craft->getWeapons()->size()) return;
	std::string wtype = payload.get("weapon", "").asString();
	const RuleCraftWeapon* selRule = wtype.empty() ? nullptr
		: game->getMod()->getCraftWeapon(wtype, false);
	if (!wtype.empty() && !selRule) return;

	ItemContainer* store = base->getStorageItems();
	CraftWeapon* current = craft->getWeapons()->at(slot);
	const RuleCraftWeapon* curRule = current ? current->getRules() : nullptr;
	if (curRule == selRule) return; // idempotent: a late/duplicate apply can't double-charge

	// Dismount the current weapon: return the launcher + any loaded clips to the
	// shared stores (mirrors lstWeaponsClick "Remove current weapon").
	if (current)
	{
		store->addItem(current->getRules()->getLauncherItem());
		store->addItem(current->getRules()->getClipItem(), current->getClipsLoaded());
		craft->addCraftStats(-current->getRules()->getBonusStats());
		craft->setShield(craft->getShield()); // exploit protection (as vanilla)
		delete current;
		craft->getWeapons()->at(slot) = 0;
	}

	// Mount the new weapon: consume one launcher from the shared stores. Only if
	// available, so a race can never drive stores negative; deterministic on host
	// + replica -> they converge. Clips load over time via the host sim (vanilla).
	if (selRule && store->getItem(selRule->getLauncherItem()) > 0)
	{
		CraftWeapon* cw = new CraftWeapon(const_cast<RuleCraftWeapon*>(selRule), 0);
		craft->addCraftStats(selRule->getBonusStats());
		store->removeItem(selRule->getLauncherItem());
		craft->getWeapons()->at(slot) = cw;
	}
	craft->checkup();
}

// soldier_armor payload: { soldierId, armor }. baseId = the soldier's base index.
// End-state = which armor the soldier wears (identity swap, last-write-wins - the
// J09 "model the payload to the state, not literally a count" adaptation). Mirrors
// SoldierArmorState / CraftArmorState: return the old armor's store item + consume
// the new one against the shared stores.
bool soldierArmorValidate(Game* game, const Json::Value& payload, Base* base, int /*seat*/,
                          int64_t& cost, std::string& failReason)
{
	cost = 0;
	if (!base) { failReason = "base not found"; return false; }
	if (!findSoldier(base, payload.get("soldierId", -1).asInt())) { failReason = "soldier not found"; return false; }
	if (!game->getMod()->getArmor(payload.get("armor", "").asString(), false)) { failReason = "unknown armor"; return false; }
	return true;
}

void soldierArmorApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	Soldier* s = findSoldier(base, payload.get("soldierId", -1).asInt());
	Armor* next = game->getMod()->getArmor(payload.get("armor", "").asString(), false);
	if (!s || !next) return;
	Armor* prev = s->getArmor();
	if (prev == next) return; // idempotent
	// Store bookkeeping only in a live campaign (monthsPassed != -1), matching the
	// UI screens; pre-game (new battle) never touches stores.
	SavedGame* save = game->getSavedGame();
	if (save && save->getMonthsPassed() != -1)
	{
		if (prev->getStoreItem()) base->getStorageItems()->addItem(prev->getStoreItem());
		if (next->getStoreItem()) base->getStorageItems()->removeItem(next->getStoreItem());
	}
	s->setArmor(next, true);
	if (save) save->setLastSelectedArmor(next->getType());
}

// PRD-DF01: df_open applier (REPLICA only). The host publishes the FULL dogfight
// membership set + epoch on every change; the replica adopts it and reconciles its
// render-only windows (opens new tuples once their craft + UFO are replicated,
// closes departed ones). Payload: { epoch, dogfights:[ {craftId, craftType, ufoId,
// ufoIsAttacking} ] }. This REPLACES the J08 dogfightStartApply (initiator model).
void dfOpenApply(Game* game, Json::Value& payload, Base* /*base*/, int /*seat*/)
{
	if (connectionTCP::getHost()) return; // host owns its own DogfightState set
	GeoscapeState* gs = findGeoState(game);
	if (!gs) return; // no geoscape (sub-screen/battle): reconcile when it returns
	gs->jointApplyDogfightMembership(payload["dogfights"], payload.get("epoch", 0).asInt());
}

// PRD-DF02: df_cmd applier (HOST only). A replica emits df_cmd on the reliable FIFO
// joint_cmd lane when any player presses a stance / weapon / disengage / self-destruct
// button on a replica-view dogfight; the host drives its authoritative DogfightState
// through the SAME lane the local UI uses, arbitrated in g_cmdQ receive-order
// (last-received-wins). Epoch guard (6): the (craftId,ufoId) must still be a live host
// dogfight, else the command is a stale pre-reshuffle order and is dropped (logged
// once). Payload: { craftId, craftType, ufoId, action, arg }. Minimize is NOT sent here
// (per-machine VIEW state, 4). The uniform joint_apply echo to replicas is a no-op
// (this applier early-returns off-host).
void dfCmdApply(Game* game, Json::Value& payload, Base* /*base*/, int /*seat*/)
{
	if (!connectionTCP::getHost()) return; // host applies; a replica ignores the echo
	GeoscapeState* gs = findGeoState(game);
	if (!gs) return;
	int craftId = payload.get("craftId", -1).asInt();
	int ufoId = payload.get("ufoId", -1).asInt();
	std::string craftType = payload.get("craftType", "").asString();
	std::string action = payload.get("action", "").asString();
	int arg = payload.get("arg", -1).asInt();
	if (!gs->jointApplyDogfightCmd(craftId, ufoId, craftType, action, arg))
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			Log(LOG_INFO) << "[JOINT] df_cmd dropped (stale/unknown membership): craft "
				<< craftId << " ufo " << ufoId << " action '" << action << "' (logged once)";
		}
	}
}

// ---- PRD-J10: the landing broker ---------------------------------------------
// land_prompt payload: { craftId, craftType, initiatorSeat, shade }. Host-origin
// (simAccept; applier replica-only + seat-gated), exactly like dogfight_start:
// only the seat that ORDERED the craft is asked whether to land. Battle authority
// is untouched - if the seat says yes, the HOST still generates the battle.
void landPromptApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return; // the host is the one asking
	if (payload.get("initiatorSeat", -1).asInt() != connectionTCP::localSeat()) return;
	Craft* craft = resolveOrderCraft(game, payload, base);
	GeoscapeState* gs = findGeoState(game);
	// The dialog renders the destination's name, so a replica that has not yet
	// replicated the target (or is not on the geoscape) cannot ask the question.
	// Decline immediately rather than leave the host's craft waiting forever -
	// the dogfightStartApply abort pattern.
	if (!craft || !craft->getDestination() || !gs)
	{
		Json::Value p;
		p["craftId"] = payload["craftId"];
		p["craftType"] = payload["craftType"];
		p["yes"] = false;
		p["patrol"] = false;
		submitLocalCmd(game, "land_reply", -1, p);
		return;
	}
	// Textures are null on purpose: this dialog only ASKS. It never runs
	// checkStartingCondition or the battle generator (its broker branch submits
	// land_reply instead), and the only thing it draws from the host's world is
	// the day/night shade, which rides the payload.
	gs->popup(new ConfirmLandingState(craft, nullptr, nullptr,
		payload.get("shade", 0).asInt(), true /*jointBroker*/));
}

// land_reply payload: { craftId, craftType, yes, patrol }. Client -> host; the
// applier is HOST-ONLY (the host owns the consequence, and it is the only machine
// that can generate the authoritative battle).
void landReplyApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (!connectionTCP::getHost()) return;
	Craft* craft = resolveOrderCraft(game, payload, base);
	GeoscapeState* gs = findGeoState(game);
	if (!craft || !gs) return;
	gs->jointLandingReply(craft, payload.get("yes", false).asBool(),
		payload.get("patrol", false).asBool());
}

// PRD-DF01: the J08 host-applies-reported-result path is GONE. The host now
// simulates every JOINT dogfight in its own DogfightState::update, which is the
// SINGLE home for the UFO-downed consequences (country/region score + the
// retaliation roll) - so hostRollRetaliation + dogfightResultApply are deleted
// to avoid a double-roll. The world outcome reaches replicas via the geo
// position snapshot (UFO CRASHED/crashId, craft damage/fuel/ammo).
// research_done payload: { research, bonus, newResearch } (rule names; bonus /
// newResearch may be "").
void researchDoneApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return; // host already applied in time1Day
	if (!base) return;
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) return;

	const std::string rName = payload.get("research", "").asString();
	RuleResearch* research = mod->getResearch(rName, false);
	if (!research) return;
	const std::string bName = payload.get("bonus", "").asString();
	RuleResearch* bonus = bName.empty() ? nullptr : mod->getResearch(bName, false);

	// Remove the base's matching ResearchProject and free its scientists, exactly
	// as the host's time1Day did. Mark it finished first so removeResearch() does
	// not take the "cancelled research" branch and refund the needed item (the
	// replica's frozen project never stepped, so it looks unfinished).
	for (auto* proj : base->getResearch())
	{
		if (proj->getRules()->getName() == rName)
		{
			proj->setSpent(proj->getCost());
			base->removeResearch(proj);
			break;
		}
	}

	// Add the discovered topic(s). The host already selected the getOneFree
	// (RNG) and passed it as `bonus`; addFinishedResearch itself is deterministic,
	// so applying the host's exact choices keeps the replica identical.
	if (bonus)
	{
		save->addFinishedResearch(bonus, mod, base);
		if (!bonus->getLookup().empty())
			save->addFinishedResearch(mod->getResearch(bonus->getLookup(), true), mod, base);
	}
	save->addFinishedResearch(research, mod, base);
	if (!research->getLookup().empty())
		save->addFinishedResearch(mod->getResearch(research->getLookup(), true), mod, base);

	// Mirror the host popup (coop=true -> the ctor does NOT re-broadcast).
	const std::string nrName = payload.get("newResearch", "").asString();
	const RuleResearch* newResearch = nrName.empty() ? nullptr : mod->getResearch(nrName, false);
	game->pushState(new ResearchCompleteState(newResearch, bonus, research, base, true));
}

// fac_done payload: { x, y, type }.
void facDoneApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return;
	if (!base) return;
	int x = payload.get("x", -1).asInt();
	int y = payload.get("y", -1).asInt();
	for (auto* fac : *base->getFacilities())
	{
		if (fac->getX() == x && fac->getY() == y && fac->getBuildTime() > 0)
		{
			fac->setBuildTime(0);
			GeoscapeState* gs = findGeoState(game);
			if (gs)
				game->pushState(new ProductionCompleteState(
					base, game->getLanguage()->getString(payload.get("type", "").asString()),
					gs, PROGRESS_CONSTRUCTION));
			break;
		}
	}
}

// prod_done payload: { manufacture, units, progress, sell }.
void prodDoneApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return;
	if (!base) return;
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) return;

	const std::string mName = payload.get("manufacture", "").asString();
	int units = payload.get("units", 0).asInt();
	// GAP-6b: the host Production's SELL flag. When set, Production::step() credited
	// funds and added NOTHING to stores for the produced items; the authoritative
	// funds ride this joint_apply as usual, so the replica must skip the storage add
	// or its item count drifts ABOVE the host. The sell branch lives inside the
	// NON-craft arm of Production::step(), so a produced CRAFT is never sold - the
	// craft materialization below stays unconditional.
	bool sell = payload.get("sell", false).asBool();
	RuleManufacture* rule = mod->getManufacture(mName, false);
	if (!rule || units <= 0) return;

	// Materialize the deterministic output (items + crafts). Random/spawned-person
	// production is host-RNG and NOT reconstructed here (documented limitation);
	// the next joint_apply / checksum surfaces any resulting drift.
	if (!sell)
		for (const auto& it : rule->getProducedItems())
			base->getStorageItems()->addItem(it.first, it.second * units);
	if (const RuleCraft* craftRule = rule->getProducedCraft())
	{
		for (int c = 0; c < units; ++c)
		{
			// getId(craftType) advances the per-type counter identically to the
			// host (all craft creation rides joint_apply), so ids stay in lockstep.
			Craft* craft = new Craft(const_cast<RuleCraft*>(craftRule), base,
			                         save->getId(craftRule->getType()));
			craft->initFixedWeapons(mod);
			craft->checkup();
			base->getCrafts()->push_back(craft);
		}
	}

	// Remove the matching Production (returns its engineers to the base pool) and
	// mirror the completion popup.
	for (auto* prod : base->getProductions())
	{
		if (prod->getRules()->getName() == mName)
		{
			GeoscapeState* gs = findGeoState(game);
			if (gs)
				game->pushState(new ProductionCompleteState(
					base, game->getLanguage()->getString(mName), gs,
					(productionProgress_e)payload.get("progress", PROGRESS_COMPLETE).asInt(),
					prod));
			base->removeProduction(prod);
			break;
		}
	}
}

// transfer_arrived payload: { arrived: [ {type, rule, qty} ] }.
void transferArrivedApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return;
	if (!base) return;
	const Json::Value& arrived = payload["arrived"];
	if (!arrived.isArray()) return;

	auto& transfers = *base->getTransfers();
	for (const auto& d : arrived)
	{
		int type = d.get("type", -1).asInt();
		std::string rule = d.get("rule", "").asString();
		int qty = d.get("qty", 0).asInt();
		for (auto it = transfers.begin(); it != transfers.end(); ++it)
		{
			Transfer* t = *it;
			bool match = (t->getType() == type);
			if (match && type == TRANSFER_ITEM)
				match = (t->getItems() && t->getItems()->getType() == rule && t->getQuantity() == qty);
			else if (match && (type == TRANSFER_SCIENTIST || type == TRANSFER_ENGINEER))
				match = (t->getQuantity() == qty);
			else if (match && type == TRANSFER_CRAFT)
				match = (t->getCraft() && t->getCraft()->getRules()->getType() == rule);
			else if (match && type == TRANSFER_SOLDIER)
				// PRD-J05: hired/transferred soldiers arrive here too. Match by the
				// soldier's rule type (rule may be "" from an older host -> FIFO
				// falls back to the first pending soldier transfer, still correct
				// because host and replica built them in identical order).
				match = (t->getSoldier() && (rule.empty()
					|| t->getSoldier()->getRules()->getType() == rule));
			if (!match) continue;

			// Force delivery: advance() delivers exactly once when hours reach 0
			// and sets _delivered, so the subsequent delete won't free a craft/
			// soldier now owned by the base.
			while (t->getHours() > 0) t->advance(base);
			transfers.erase(it);
			delete t;
			break;
		}
	}
}

// day_tick payload: { soldiers:[{id,recovery}], productions:[{item,spent}],
// research:[{project,spent}] }. PRD-J06: the replica's timeXxx handlers are
// frozen, so production _timeSpent and research _spent never advance locally;
// the host broadcasts the day's progress so the "days left" / "Progress" columns
// render current on replicas. Display-only (completion is host-driven J04).
void dayTickApply(Game* game, Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return;
	if (!base) return;
	const Json::Value& soldiers = payload["soldiers"];
	if (soldiers.isArray())
		for (const auto& s : soldiers)
		{
			int id = s.get("id", -1).asInt();
			int recovery = s.get("recovery", 0).asInt();
			for (auto* soldier : *base->getSoldiers())
			{
				if (soldier->getId() == id)
				{
					soldier->setWoundRecovery(recovery);
					break;
				}
			}
		}
	const Json::Value& productions = payload["productions"];
	if (productions.isArray())
		for (const auto& pr : productions)
		{
			Production* p = findProduction(base, pr.get("item", "").asString());
			if (p) p->setTimeSpent(pr.get("spent", p->getTimeSpent()).asInt());
		}
	const Json::Value& research = payload["research"];
	if (research.isArray())
		for (const auto& rr : research)
		{
			ResearchProject* rp = findResearchProject(base, rr.get("project", "").asString());
			if (rp) rp->setSpent(rr.get("spent", rp->getSpent()).asInt());
		}
}

// ---- Host-side command processing (main thread) ------------------------------
void rejectHostCmd(Game* game, const PendingCmd& pc, const std::string& reason)
{
	++g_failN;
	setLastFail(reason);
	if (pc.remote)
	{
		Json::Value fail;
		fail["state"] = "joint_fail";
		fail["seq"] = pc.seq;
		fail["reason"] = reason;
		if (game->getCoopMod()) game->getCoopMod()->sendTCPPacketData(fail.toStyledString());
	}
	else
	{
		// The host's own command failed: surface it locally, exactly as a replica
		// surfaces a joint_fail received from the host (PRD-J10: one helper, one
		// dialog, both roles).
		showFail(game, reason);
	}
}

void processHostCmd(Game* game, const PendingCmd& pc)
{
	auto& reg = registry();
	auto hit = reg.find(pc.cmd);
	if (hit == reg.end())
	{
		++g_unknownN;
		rejectHostCmd(game, pc, "unknown command: " + pc.cmd);
		return;
	}

	Base* base = resolveBase(game, pc.baseId);
	int64_t cost = 0;
	std::string failReason;
	++g_cmdN;
	if (!hit->second.validate(game, pc.payload, base, pc.seat, cost, failReason))
	{
		rejectHostCmd(game, pc, failReason.empty() ? "rejected" : failReason);
		return;
	}

	// Passed: debit the authoritative funds, apply the mutation, then broadcast
	// joint_apply (carrying the post-mutation funds) to every peer and joint_ok
	// to the initiator. The applier gets a MUTABLE payload copy so it can resolve
	// host-only RNG into it (e.g. buy serializes generated soldiers); the resolved
	// payload is what we broadcast, so replicas reconstruct instead of re-rolling.
	SavedGame* save = game->getSavedGame();
	save->setFunds(save->getFunds() - cost);
	Json::Value payload = pc.payload;
	hit->second.apply(game, payload, base, pc.seat);
	++g_applyN;

	Json::Value apply;
	apply["state"] = "joint_apply";
	apply["cmd"] = pc.cmd;
	apply["seq"] = pc.seq;
	apply["seat"] = pc.seat;
	apply["baseId"] = pc.baseId;
	apply["payload"] = payload;
	apply["funds"] = Json::Value::Int64(save->getFunds());
	// GAP-9: carry the host's authoritative current-month income/expenditure tails
	// (read AFTER apply, so they include any gross flow the applier booked, e.g. a
	// prod_done that both sells and restarts a unit). The replica adopts these
	// verbatim instead of net-inferring them from setFunds, keeping the Graphs->
	// Finance series exactly the host's. Funds alone are not enough: the host's
	// gross income/expenditure decomposition cannot be reconstructed from the net.
	if (!save->getIncomes().empty())
		apply["incTail"] = Json::Value::Int64(save->getIncomes().back());
	if (!save->getExpenditures().empty())
		apply["expTail"] = Json::Value::Int64(save->getExpenditures().back());
	broadcast(game, apply);

	if (pc.remote && game->getCoopMod())
	{
		Json::Value ok;
		ok["state"] = "joint_ok";
		ok["seq"] = pc.seq;
		game->getCoopMod()->sendTCPPacketData(ok.toStyledString());
		++g_okN;
	}

	// PRD-J10: the host's own open screens are as stale as a replica's after an
	// apply (a client's buy moves the host's funds too), so both roles notify.
	fireApplyListener(pc.cmd, pc.baseId, pc.seat);
}

// ---- Replica-side apply (main thread) ----------------------------------------
void processApply(Game* game, const Json::Value& ap)
{
	SavedGame* save = game->getSavedGame();
	// Funds are host-authoritative: adopt them from the packet no matter what, so
	// the replica cannot drift even if the mutation itself cannot be reconstructed.
	if (save && ap.isMember("funds"))
	{
		if (ap.isMember("incTail") && ap.isMember("expTail"))
		{
			// GAP-9: adopt the host's authoritative funds AND its income/expenditure
			// tails verbatim. setFunds() would net-infer the direction from the delta
			// and drift the Graphs->Finance series (the host reached this value through
			// gross income AND expenditure, e.g. a prod_done that sells + restarts a
			// unit); setFundsRaw() moves only _funds.back(), then we copy the tails.
			save->setFundsRaw(ap["funds"].asInt64());
			if (!save->getIncomes().empty())
				save->getIncomes().back() = ap["incTail"].asInt64();
			if (!save->getExpenditures().empty())
				save->getExpenditures().back() = ap["expTail"].asInt64();
		}
		else
		{
			// Legacy packet without the series tails: keep the old net-inference so
			// funds still stay exact (series may drift, as before the fix).
			save->setFunds(ap["funds"].asInt64());
		}
	}

	std::string cmd = ap.get("cmd", "").asString();
	Base* base = resolveBase(game, ap.get("baseId", -1).asInt());
	auto& reg = registry();
	auto hit = reg.find(cmd);
	// NOTE: no "!base" early-return here. A creation command (base_new) carries
	// baseId=-1 (no existing base) and its applier ignores @a base; every OTHER
	// applier already null-guards @a base itself (if (!base) return;), so passing a
	// null base straight through is safe and keeps base creation working on replicas.
	if (hit == reg.end()) return;

	// Mutable copy for the applier signature; the replica only READS the resolved
	// payload (host already resolved any RNG before broadcasting).
	Json::Value payload = ap["payload"];
	int seat = ap.get("seat", 0).asInt();
	hit->second.apply(game, payload, base, seat);
	++g_applyN;

	// PRD-J10: tell the open screen its world just moved under it.
	fireApplyListener(cmd, ap.get("baseId", -1).asInt(), seat);
}

} // anonymous namespace

// ---- Public API --------------------------------------------------------------
void registerCmd(const std::string& cmd, CmdValidator validate, CmdApplier apply)
{
	registry()[cmd] = Handler{ std::move(validate), std::move(apply) };
}

// ---- PRD-J10: apply notification ---------------------------------------------
void setApplyListener(const void* owner, ApplyListener listener)
{
	g_listenerOwner = owner;
	g_listener = std::move(listener);
}

void clearApplyListener(const void* owner)
{
	// Only the CURRENT owner may clear. A popped screen's destructor runs one
	// frame late - by then its replacement already registered, and an
	// unconditional clear here would kill live refresh for good.
	if (g_listenerOwner != owner) return;
	g_listenerOwner = nullptr;
	g_listener = nullptr;
}

int lastApplySeat()
{
	return g_lastApplySeat.load();
}

int baseIndex(Game* game, const Base* base)
{
	if (!game || !game->getSavedGame() || !base) return -1;
	auto* bases = game->getSavedGame()->getBases();
	for (size_t i = 0; i < bases->size(); ++i)
		if ((*bases)[i] == base) return (int)i;
	return -1;
}

void ScreenRefresh::bind(Game* game, const void* owner, Base* base, bool wantProgress)
{
	if (!game || !game->getCoopMod() || !game->getCoopMod()->isJointCampaign()) return;
	_game = game;
	_base = base;
	_wantProgress = wantProgress;
	_bound = true;
	setApplyListener(owner, [this](const std::string& cmd, int applyBaseId)
	{
		// day_tick is pure progress bookkeeping (wound recovery, research/production
		// "days left"). List views want it; a command screen must NOT throw away the
		// player's half-entered order once per game-day because of it.
		if (!_wantProgress && cmd == "day_tick") return;
		// applyBaseId < 0 = world-scoped (funds-only, base creation, dogfights):
		// always relevant. Otherwise only this screen's own base matters.
		if (applyBaseId >= 0 && _base)
		{
			int mine = baseIndex(_game, _base);
			if (mine >= 0 && mine != applyBaseId) return;
		}
		_dirty = true;
	});
}

void ScreenRefresh::unbind(const void* owner)
{
	clearApplyListener(owner);
	_bound = false;
}

bool ScreenRefresh::consume()
{
	if (!_dirty) return false;
	_dirty = false;
	return true;
}

void showFail(Game* game, const std::string& reason)
{
	if (!game) return;
	// The reason is the host validator's own string: an STR_ id where the vanilla
	// rule already had one (STR_NOT_ENOUGH_MONEY, STR_NOT_ENOUGH_CRAFT_SPACE, ...),
	// a plain sentence otherwise. Language::getString returns the id unchanged when
	// it is not a known key, so one lookup covers both.
	std::string text = "The host rejected your command.";
	if (!reason.empty() && game->getLanguage())
	{
		text = game->getLanguage()->getString(reason);
	}
	connectionTCP::jointFailReason = text;
	game->pushState(new CoopState(COOP_DLG_JOINT_FAIL));
}

void init()
{
	if (g_inited) return;
	g_inited = true;
	registerCmd("buy", &buyValidate, &buyApply);
	// PRD-J05 economy commands (client -> host mutation requests).
	registerCmd("sell",        &sellValidate,        &sellApply);
	registerCmd("containment", &containmentValidate, &containmentApply);
	registerCmd("transfer",    &transferValidate,    &transferApply);
	// PRD-J06 research + manufacture commands (client -> host mutation requests).
	registerCmd("res_start",   &resStartValidate,    &resStartApply);
	registerCmd("res_alloc",   &resAllocValidate,    &resAllocApply);
	registerCmd("res_cancel",  &resCancelValidate,   &resCancelApply);
	registerCmd("man_start",   &manStartValidate,    &manStartApply);
	registerCmd("man_alloc",   &manAllocValidate,    &manAllocApply);
	registerCmd("man_cancel",  &manCancelValidate,   &manCancelApply);
	// PRD-J07 facilities / bases (client -> host mutation requests).
	registerCmd("fac_build",     &facBuildValidate,     &facBuildApply);
	registerCmd("fac_dismantle", &facDismantleValidate, &facDismantleApply);
	registerCmd("base_rename",   &baseRenameValidate,   &baseRenameApply);
	registerCmd("sack",          &sackValidate,         &sackApply);
	registerCmd("base_new",      &baseNewValidate,      &baseNewApply);
	// PRD-J07 base_destroyed: host-originated (retaliation, J04); replica-only apply.
	registerCmd("base_destroyed", &simAccept, &baseDestroyedApply);
	// PRD-J08 craft orders (any player -> host; last-command-wins by arrival order).
	registerCmd("craft_launch",   &craftOrderValidate,  &craftOrderApply);
	registerCmd("craft_retarget", &craftOrderValidate,  &craftOrderApply);
	registerCmd("craft_return",   &craftExistsValidate, &craftReturnApply);
	registerCmd("craft_patrol",   &craftExistsValidate, &craftPatrolApply);

	// PRD-J09: shared-world squad assembly (mixed-owner deployment).
	registerCmd("craft_assign",   &craftAssignValidate, &craftAssignApply);
	// PRD-J09 GAP-5: shared-world craft equipment loadout (base-screen equip).
	registerCmd("craft_equip",    &craftEquipValidate,  &craftEquipApply);
	// PRD-J09 GAP-5b: the sibling base-screen store mutators (arm/rearm a craft
	// weapon; change a soldier's armor - SoldierArmorState + CraftArmorState).
	registerCmd("craft_rearm",    &craftRearmValidate,  &craftRearmApply);
	registerCmd("soldier_armor",  &soldierArmorValidate, &soldierArmorApply);
	// PRD-DF01 shared/replicated dogfights: host-originated membership broadcast
	// (df_open, full set + epoch each change; replica reconciles its render-only
	// windows). df_state (per-tick render frames) rides the SNAP_DOGFIGHT conflation
	// slot, NOT this reliable FIFO lane, so it has no registerCmd entry.
	registerCmd("df_open", &simAccept, &dfOpenApply);
	// PRD-DF02 replicated control: client->host dogfight command (stance / weapon /
	// disengage / self-destruct). Host applies to the authoritative sim in receive-order.
	registerCmd("df_cmd", &simAccept, &dfCmdApply);
	// PRD-J10 landing broker: host-origin prompt (seat-gated replica applier);
	// initiator-reported answer (host-only applier).
	registerCmd("land_prompt", &simAccept, &landPromptApply);
	registerCmd("land_reply",  &simAccept, &landReplyApply);
	// PRD-J04 host simulation-result mirrors (always-accept validator; appliers
	// run replica-side only).
	registerCmd("research_done",    &simAccept, &researchDoneApply);
	registerCmd("fac_done",         &simAccept, &facDoneApply);
	registerCmd("prod_done",        &simAccept, &prodDoneApply);
	registerCmd("transfer_arrived", &simAccept, &transferArrivedApply);
	registerCmd("day_tick",         &simAccept, &dayTickApply);
}

void broadcast(Game* game, const Json::Value& msg)
{
	if (!game || !game->getCoopMod()) return;
	// Transport is strictly 1:1 today (PRD-J01 audit); "broadcast" is a single
	// peer send. When N-player TCP lands, only this body iterates the client set.
	game->getCoopMod()->sendTCPPacketData(msg.toStyledString());
}

// PRD-DF01: df_state router (REPLICA only). df_state rides the SNAP_DOGFIGHT
// conflation slot (a raw top-level message, NOT the joint_apply lane), so
// connectionTCP::onTCPMessage hands it straight here. The GeoscapeState
// epoch-guards it against the reshuffle race and routes each frame to the
// matching render-only window by (craftId, craftType, ufoId).
void applyDogfightState(Game* game, const Json::Value& obj)
{
	if (!game || connectionTCP::getHost()) return; // host renders from its own sim
	GeoscapeState* gs = findGeoState(game);
	if (gs) gs->jointApplyDogfightState(obj);
}

bool onMessage(Game* game, const std::string& state, const Json::Value& obj)
{
	if (state == "joint_cmd")
	{
		// Only the host validates/applies commands; a replica ignores stray cmds.
		if (isHost())
		{
			PendingCmd pc;
			pc.cmd = obj.get("cmd", "").asString();
			pc.seq = obj.get("seq", 0).asInt();
			pc.seat = obj.get("seat", 0).asInt();
			pc.baseId = obj.get("baseId", -1).asInt();
			pc.payload = obj["payload"];
			pc.remote = true;
			std::lock_guard<std::mutex> lk(g_mx);
			g_cmdQ.push_back(std::move(pc));
		}
		return true;
	}
	if (state == "joint_apply")
	{
		// Replicas (and only replicas) adopt applied mutations. The host applied
		// at broadcast time and must never re-apply its own broadcast.
		if (!isHost())
		{
			std::lock_guard<std::mutex> lk(g_mx);
			g_applyQ.push_back(obj);
		}
		return true;
	}
	if (state == "joint_ok")
	{
		++g_okN; // informational: the mutation self-applies from joint_apply
		return true;
	}
	if (state == "joint_fail")
	{
		std::string reason = obj.get("reason", "").asString();
		++g_failN;
		setLastFail(reason);
		std::lock_guard<std::mutex> lk(g_mx);
		g_failQ.push_back(reason);
		return true;
	}
	if (state == "joint_resync_request")
	{
		// PRD-J10: a replica's world checksum diverged from ours. Only the host
		// can answer; queue it for the main-thread pump (the restream serializes
		// the whole SavedGame, which must not race the apply drain).
		if (isHost())
		{
			std::lock_guard<std::mutex> lk(g_mx);
			++g_resyncServeQ;
		}
		return true;
	}
	return false;
}

void update(Game* game)
{
	if (!game) return;

	// 1) Host: drain queued commands -> validate, debit, apply, broadcast.
	for (;;)
	{
		PendingCmd pc;
		{
			std::lock_guard<std::mutex> lk(g_mx);
			if (g_cmdQ.empty()) break;
			pc = std::move(g_cmdQ.front());
			g_cmdQ.pop_front();
		}
		processHostCmd(game, pc);
	}

	// 2) Replica: drain queued applies -> setFunds + apply.
	for (;;)
	{
		Json::Value ap;
		{
			std::lock_guard<std::mutex> lk(g_mx);
			if (g_applyQ.empty()) break;
			ap = std::move(g_applyQ.front());
			g_applyQ.pop_front();
		}
		processApply(game, ap);
	}

	// 3) Initiator: surface queued failures (one dialog per fail).
	for (;;)
	{
		std::string reason;
		bool have = false;
		{
			std::lock_guard<std::mutex> lk(g_mx);
			if (!g_failQ.empty()) { reason = g_failQ.front(); g_failQ.pop_front(); have = true; }
		}
		if (!have) break;
		showFail(game, reason);
	}

	// 4) Host: serve queued resync requests (PRD-J10). Re-stream the authoritative
	// world down the J02 bootstrap lane; the streamer is single-slot, so if it is
	// busy we drop the request - the replica's next mismatching checksum re-asks.
	for (;;)
	{
		bool have = false;
		{
			std::lock_guard<std::mutex> lk(g_mx);
			if (g_resyncServeQ > 0) { --g_resyncServeQ; have = true; }
		}
		if (!have) break;
		connectionTCP* coop = game->getCoopMod();
		if (!coop || !coop->getServerOwner() || !coop->isJointCampaign()) continue;
		++g_resyncReqN;
		Log(LOG_WARNING) << "[JOINT] resync requested by the replica; re-streaming"
			<< " the authoritative world";
		coop->jointResyncStream();
	}
}

void submitLocalCmd(Game* game, const std::string& cmd, int baseId,
                    const Json::Value& payload)
{
	if (!game) return;
	int seq = ++g_seqCounter;
	int seat = connectionTCP::localSeat();

	if (isHost())
	{
		// Host originates: queue for local validate+apply+broadcast (unified with
		// the client-cmd path in update(); host-origin skips the joint_cmd wire).
		PendingCmd pc;
		pc.cmd = cmd;
		pc.seq = seq;
		pc.seat = seat;
		pc.baseId = baseId;
		pc.payload = payload;
		pc.remote = false;
		std::lock_guard<std::mutex> lk(g_mx);
		g_cmdQ.push_back(std::move(pc));
	}
	else
	{
		// Replica: send the command to the host; mutate nothing locally.
		Json::Value msg;
		msg["state"] = "joint_cmd";
		msg["cmd"] = cmd;
		msg["seq"] = seq;
		msg["seat"] = seat;
		msg["baseId"] = baseId;
		msg["payload"] = payload;
		if (game->getCoopMod()) game->getCoopMod()->sendTCPPacketData(msg.toStyledString());
	}
}

Stats stats()
{
	Stats s;
	s.cmd = g_cmdN.load();
	s.ok = g_okN.load();
	s.fail = g_failN.load();
	s.apply = g_applyN.load();
	s.unknown = g_unknownN.load();
	return s;
}

std::string lastFailReason()
{
	std::lock_guard<std::mutex> lk(g_failMx);
	return g_lastFail;
}

void resetStats()
{
	g_cmdN = 0; g_okN = 0; g_failN = 0; g_applyN = 0; g_unknownN = 0;
	std::lock_guard<std::mutex> lk(g_failMx);
	g_lastFail.clear();
}

// ---- PRD-J04 host sim-result broadcasts --------------------------------------
// All gate on isJointCampaign() && host; each rides submitLocalCmd (host-origin),
// which validate(accept)+apply(host no-op)+broadcasts joint_apply to the replica.
namespace {
bool jointHost(Game* game)
{
	return game && game->getCoopMod() && game->getCoopMod()->isJointCampaign()
		&& connectionTCP::getHost();
}
}

void hostResearchDone(Game* game, int baseId, const std::string& research,
                      const std::string& bonus, const std::string& newResearch)
{
	if (!jointHost(game)) return;
	Json::Value p;
	p["research"] = research;
	p["bonus"] = bonus;
	p["newResearch"] = newResearch;
	submitLocalCmd(game, "research_done", baseId, p);
}

void hostFacilityDone(Game* game, int baseId, int x, int y, const std::string& facilityType)
{
	if (!jointHost(game)) return;
	Json::Value p;
	p["x"] = x;
	p["y"] = y;
	p["type"] = facilityType;
	submitLocalCmd(game, "fac_done", baseId, p);
}

void hostProductionDone(Game* game, int baseId, const std::string& manufacture,
                        int units, int progress, bool sell)
{
	if (!jointHost(game)) return;
	Json::Value p;
	p["manufacture"] = manufacture;
	p["units"] = units;
	p["progress"] = progress;
	// GAP-6b: carry the host Production's SELL flag so the replica materializes
	// exactly what the host did (sold -> funds only, nothing to stores).
	p["sell"] = sell;
	submitLocalCmd(game, "prod_done", baseId, p);
}

void hostTransferArrived(Game* game, int baseId, const Json::Value& arrived)
{
	if (!jointHost(game)) return;
	if (!arrived.isArray() || arrived.empty()) return;
	Json::Value p;
	p["arrived"] = arrived;
	submitLocalCmd(game, "transfer_arrived", baseId, p);
}

void hostBaseDestroyed(Game* game, int baseId, const std::string& name)
{
	if (!jointHost(game)) return;
	Json::Value p;
	p["name"] = name;
	submitLocalCmd(game, "base_destroyed", baseId, p);
}

// ---- PRD-J08 public API --------------------------------------------------------

namespace {

// Index of the base holding @a craft (baseId of a craft order), or -1.
int craftBaseIndex(Game* game, const Craft* craft)
{
	SavedGame* save = game ? game->getSavedGame() : nullptr;
	if (!save) return -1;
	auto* bases = save->getBases();
	for (int i = 0; i < (int)bases->size(); ++i)
		for (auto* c : *(*bases)[i]->getCrafts())
			if (c == craft) return i;
	return -1;
}

// Serialize @a target into an order payload (shared real ids; lon/lat always
// carried so a "point" fallback and the UI echo stay possible).
void describeTarget(Game* game, Target* target, Json::Value& p)
{
	p["lon"] = target->getLongitude();
	p["lat"] = target->getLatitude();
	if (auto* u = dynamic_cast<Ufo*>(target))
	{
		p["targetType"] = "ufo";
		p["targetId"] = u->getId();
	}
	else if (auto* s = dynamic_cast<MissionSite*>(target))
	{
		p["targetType"] = "site";
		p["targetId"] = s->getId();
	}
	else if (auto* ab = dynamic_cast<AlienBase*>(target))
	{
		p["targetType"] = "abase";
		p["targetId"] = ab->getId();
	}
	else if (auto* b = dynamic_cast<Base*>(target))
	{
		p["targetType"] = "xbase";
		SavedGame* save = game->getSavedGame();
		auto* bases = save->getBases();
		for (int i = 0; i < (int)bases->size(); ++i)
			if ((*bases)[i] == b) { p["tBaseId"] = i; break; }
	}
	else if (auto* c = dynamic_cast<Craft*>(target))
	{
		p["targetType"] = "xcraft";
		p["tBaseId"] = craftBaseIndex(game, c);
		p["tCraftId"] = c->getId();
		p["tCraftType"] = c->getRules()->getType();
	}
	else
	{
		p["targetType"] = "point"; // waypoint (or unknown) -> a lon/lat point
	}
}

// Launch when the craft is grounded, retarget when airborne (same handler; the
// name keeps the wire readable).
const char* orderCmdFor(const Craft* craft)
{
	return craft->getStatus() == "STR_OUT" ? "craft_retarget" : "craft_launch";
}

} // anonymous namespace

void submitCraftTarget(Game* game, Craft* craft, Target* target)
{
	if (!game || !craft || !target) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	describeTarget(game, target, p);
	submitLocalCmd(game, orderCmdFor(craft), craftBaseIndex(game, craft), p);
}

void submitCraftPoint(Game* game, Craft* craft, double lon, double lat)
{
	if (!game || !craft) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	p["targetType"] = "point";
	p["lon"] = lon;
	p["lat"] = lat;
	submitLocalCmd(game, orderCmdFor(craft), craftBaseIndex(game, craft), p);
}

void submitCraftReturn(Game* game, Craft* craft)
{
	if (!game || !craft) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	submitLocalCmd(game, "craft_return", craftBaseIndex(game, craft), p);
}

void submitCraftPatrol(Game* game, Craft* craft, bool autoPatrol)
{
	if (!game || !craft) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	p["auto"] = autoPatrol;
	submitLocalCmd(game, "craft_patrol", craftBaseIndex(game, craft), p);
}

int lastCraftOrderSeat(const Craft* craft)
{
	if (!craft) return -1;
	auto it = g_craftOrderSeat.find(craftKey(craft));
	return it == g_craftOrderSeat.end() ? -1 : it->second;
}

void submitCraftAssign(Game* game, Craft* craft, Soldier* soldier, bool onOff)
{
	if (!game || !craft || !soldier) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	p["soldierId"] = soldier->getId();
	p["onOff"] = onOff;
	submitLocalCmd(game, "craft_assign", craftBaseIndex(game, craft), p);
}

void submitCraftEquip(Game* game, Craft* craft, const std::string& itemType, int desiredOnCraft)
{
	if (!game || !craft) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	p["item"] = itemType;
	p["count"] = desiredOnCraft;
	submitLocalCmd(game, "craft_equip", craftBaseIndex(game, craft), p);
}

void submitCraftRearm(Game* game, Craft* craft, int slot, const std::string& weaponType)
{
	if (!game || !craft) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	p["slot"] = slot;
	p["weapon"] = weaponType;
	submitLocalCmd(game, "craft_rearm", craftBaseIndex(game, craft), p);
}

void submitSoldierArmor(Game* game, Base* base, Soldier* soldier, const std::string& armorType)
{
	if (!game || !base || !soldier) return;
	Json::Value p;
	p["soldierId"] = soldier->getId();
	p["armor"] = armorType;
	submitLocalCmd(game, "soldier_armor", baseIndex(game, base), p);
}

void hostLandingPrompt(Game* game, Craft* craft, int seat, int shade)
{
	if (!jointHost(game) || !craft) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	p["initiatorSeat"] = seat;
	p["shade"] = shade;
	Log(LOG_INFO) << "[JOINT] landing prompt brokered to seat " << seat
		<< " for " << craftKey(craft);
	submitLocalCmd(game, "land_prompt", craftBaseIndex(game, craft), p);
}

void submitLandReply(Game* game, Craft* craft, bool yes, bool patrol)
{
	if (!game || !craft) return;
	Json::Value p;
	p["craftId"] = craft->getId();
	p["craftType"] = craft->getRules()->getType();
	p["yes"] = yes;
	p["patrol"] = patrol;
	submitLocalCmd(game, "land_reply", craftBaseIndex(game, craft), p);
}

void hostDayTick(Game* game)
{
	if (!jointHost(game)) return;
	SavedGame* save = game->getSavedGame();
	if (!save) return;
	auto* bases = save->getBases();
	for (int bi = 0; bi < (int)bases->size(); ++bi)
	{
		Base* base = (*bases)[bi];
		Json::Value soldiers(Json::arrayValue);
		for (auto* soldier : *base->getSoldiers())
		{
			int id = soldier->getId();
			int rec = soldier->getWoundRecoveryInt();
			auto hit = g_soldierRecovery.find(id);
			if (hit == g_soldierRecovery.end() || hit->second != rec)
			{
				g_soldierRecovery[id] = rec;
				Json::Value js;
				js["id"] = id;
				js["recovery"] = rec;
				soldiers.append(js);
			}
		}
		// PRD-J06: carry each running production's _timeSpent and each research
		// project's _spent so the frozen replica's progress columns stay current
		// (its own step() never runs). Small payload; sent whole each active day.
		Json::Value productions(Json::arrayValue);
		for (auto* prod : base->getProductions())
		{
			Json::Value jp;
			jp["item"] = prod->getRules()->getName();
			jp["spent"] = prod->getTimeSpent();
			productions.append(jp);
		}
		Json::Value research(Json::arrayValue);
		for (auto* rp : base->getResearch())
		{
			Json::Value jr;
			jr["project"] = rp->getRules()->getName();
			jr["spent"] = rp->getSpent();
			research.append(jr);
		}
		if (!soldiers.empty() || !productions.empty() || !research.empty())
		{
			Json::Value p;
			p["soldiers"] = soldiers;
			p["productions"] = productions;
			p["research"] = research;
			submitLocalCmd(game, "day_tick", bi, p);
		}
	}
}

// ---- PRD-J04 detect + PRD-J10 repair: world checksum -------------------------
const int RESYNC_COOLDOWN_MINUTES = 60; // one game hour between auto-resyncs
const int RESYNC_DEBOUNCE_MS = 3000;    // a mismatch must survive this to count

namespace {
// Wall-clock (not game-time) milliseconds: the debounce below measures how long a
// mismatch has SURVIVED, and a paused/slow geoscape must not stretch it.
int64_t steadyMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// A monotone game-minute stamp for the throttle. GameTime has no epoch accessor,
// so compose one from its fields; months are 1-12 and days 1-31, so the ladder is
// strictly increasing even though it skips (nonexistent) day 30/31 of February.
// Only DIFFERENCES matter here, and only against a 60-minute window.
int64_t gameMinutes(SavedGame* save)
{
	if (!save || !save->getTime()) return -1;
	GameTime* t = save->getTime();
	int64_t days = ((int64_t)t->getYear() * 12 + t->getMonth()) * 31 + t->getDay();
	return days * 24 * 60 + t->getHour() * 60 + t->getMinute();
}

// GAP-4: the widened checksum's cheap O(bases) integer aggregates. Computed the
// SAME way on host (stamp) and replica (verify) so they are identical BY
// CONSTRUCTION - both walk the one replicated world - and only real store /
// roster / transfer / production drift moves them. Counts only: no per-item
// hashing and no string building, because this rides the ~2 kHz `time` heartbeat
// (session-notes-10 #1). Deliberately NOT the income/expenditure series: GAP-9
// made those host-authoritative (the joint_apply carries them, the replica adopts
// them verbatim), so they are now equal AT REST - but they still take a discrete
// jump at the monthly roll that host and replica apply a few ticks apart (the same
// transient funds has, only funds already gates the checksum). Adding them would
// double the false-positive surface for that roll transient without catching any
// desync funds/counts don't already catch, so they stay out (GAP-9 decision).
void worldAggregates(SavedGame* save, int64_t& items, int64_t& soldiers,
	int64_t& transfers, int64_t& productions)
{
	items = soldiers = transfers = productions = 0;
	if (!save) return;
	for (Base* base : *save->getBases())
	{
		items += base->getStorageItems()->getTotalQuantity();
		soldiers += (int64_t)base->getSoldiers()->size();
		transfers += (int64_t)base->getTransfers()->size();
		productions += (int64_t)base->getProductions().size();
	}
}
} // namespace

void attachWorldChecksum(Game* game, Json::Value& msg)
{
	if (!game || !game->getSavedGame()) return;
	SavedGame* save = game->getSavedGame();
	msg["chkFunds"] = Json::Value::Int64(save->getFunds());
	msg["chkBases"] = (int)save->getBases()->size();
	msg["chkResearch"] = (int)save->getDiscoveredResearch().size();

	// GAP-4: widen past funds/bases/research so store, roster, transfer and
	// production drift can no longer hide from the auto-repair (funds is the only
	// exact-VALUE field; the four below are counts). Stamp and compare MUST stay
	// in lock-step - both go through worldAggregates().
	int64_t items, soldiers, transfers, productions;
	worldAggregates(save, items, soldiers, transfers, productions);
	msg["chkItems"] = Json::Value::Int64(items);
	msg["chkSoldiers"] = Json::Value::Int64(soldiers);
	msg["chkTransfers"] = Json::Value::Int64(transfers);
	msg["chkProduction"] = Json::Value::Int64(productions);
}

bool requestResync(Game* game, const std::string& why, bool force)
{
	if (!game || !game->getCoopMod() || !game->getCoopMod()->isJointReplica()) return false;
	if (!force && g_resyncPending) return false;

	g_resyncPending = true;
	g_lastResyncGameMin = gameMinutes(game->getSavedGame());
	++g_resyncReqN;
	Log(LOG_WARNING) << "[JOINT] requesting a world resync from the host (" << why
		<< (force ? ", forced)" : ")");

	Json::Value req;
	req["state"] = "joint_resync_request";
	game->getCoopMod()->sendTCPPacketData(req.toStyledString());
	return true;
}

void notifyWorldAdopted()
{
	// A fresh authoritative world just landed: the repair took, so re-arm both the
	// in-flight guard and the "give up" latch. The cooldown stamp stays, so a
	// mismatch that reappears within the window is still treated as unrepairable.
	g_resyncPending = false;
	g_resyncGaveUp = false;
}

void verifyWorldChecksum(Game* game, const Json::Value& msg)
{
	if (!game || !game->getSavedGame()) return;
	if (!msg.isMember("chkFunds")) return; // older/non-JOINT host
	SavedGame* save = game->getSavedGame();
	int64_t hostFunds = msg["chkFunds"].asInt64();
	int hostBases = msg.get("chkBases", -1).asInt();
	int hostResearch = msg.get("chkResearch", -1).asInt();
	// GAP-4 fields. -1 default => a host that predates this change did not stamp
	// them; since every real aggregate is >= 0, a negative host value can ONLY
	// mean "not sent" and must read as agreement, never a cross-version false
	// positive. Absent-or-equal, folded into the condition below.
	int64_t hostItems = msg.get("chkItems", -1).asInt64();
	int64_t hostSoldiers = msg.get("chkSoldiers", -1).asInt64();
	int64_t hostTransfers = msg.get("chkTransfers", -1).asInt64();
	int64_t hostProductions = msg.get("chkProduction", -1).asInt64();
	int64_t myFunds = save->getFunds();
	int myBases = (int)save->getBases()->size();
	int myResearch = (int)save->getDiscoveredResearch().size();
	int64_t myItems, mySoldiers, myTransfers, myProductions;
	worldAggregates(save, myItems, mySoldiers, myTransfers, myProductions);
	if (hostFunds == myFunds && hostBases == myBases && hostResearch == myResearch
		&& (hostItems < 0 || hostItems == myItems)
		&& (hostSoldiers < 0 || hostSoldiers == mySoldiers)
		&& (hostTransfers < 0 || hostTransfers == myTransfers)
		&& (hostProductions < 0 || hostProductions == myProductions))
	{
		// Back in agreement: whatever drifted is gone. Re-arm the repair so a later,
		// unrelated drift gets its own auto-resync instead of the give-up popup.
		if (g_mismatchLogged)
		{
			Log(LOG_INFO) << "[JOINT] world checksum back in agreement with the host";
			g_mismatchLogged = false;
		}
		g_mismatchSinceMs = -1;
		g_resyncGaveUp = false;
		g_lastResyncGameMin = -1;
		return;
	}

	++g_mismatchN;

	// DEBOUNCE. A single mismatching heartbeat does NOT mean the world diverged:
	// the checksum and the joint_apply that moves it are separate packets, so any
	// in-flight mutation shows up here as a brief skew that closes by itself a
	// frame or two later. Only a mismatch that SURVIVES is worth a multi-megabyte
	// world restream (which also replaces the replica's whole state stack). At the
	// heartbeat's ~2 kHz this still detects a real desync in a couple of seconds.
	const int64_t nowMs = steadyMs();
	if (g_mismatchSinceMs < 0) g_mismatchSinceMs = nowMs;
	if (nowMs - g_mismatchSinceMs < RESYNC_DEBOUNCE_MS) return;

	if (!g_mismatchLogged)
	{
		// once per episode - see g_mismatchLogged
		g_mismatchLogged = true;
		Log(LOG_WARNING) << "[JOINT] world checksum mismatch (persisted "
			<< RESYNC_DEBOUNCE_MS << "ms): "
			<< "funds host=" << hostFunds << " replica=" << myFunds
			<< ", bases host=" << hostBases << " replica=" << myBases
			<< ", research host=" << hostResearch << " replica=" << myResearch
			<< ", items host=" << hostItems << " replica=" << myItems
			<< ", soldiers host=" << hostSoldiers << " replica=" << mySoldiers
			<< ", transfers host=" << hostTransfers << " replica=" << myTransfers
			<< ", production host=" << hostProductions << " replica=" << myProductions;
	}

	const int64_t now = gameMinutes(save);
	const bool cooling = (g_lastResyncGameMin >= 0 && now >= 0
		&& now - g_lastResyncGameMin < RESYNC_COOLDOWN_MINUTES);

	if (g_resyncPending)
	{
		// A restream is already on the wire: every heartbeat until it lands still
		// mismatches, and re-asking would queue a second serialization of the whole
		// world on the host. Wait for it - but not forever: if the host dropped the
		// request (its single-slot streamer was busy) the guard expires with the
		// cooldown and the next mismatch re-asks.
		if (cooling) return;
		g_resyncPending = false;
	}

	if (cooling)
	{
		// A resync DID land and we are diverging again inside the cooldown: the
		// auto-repair does not stick, so something is drifting faster than a
		// restream can heal it. Stop looping on multi-megabyte world streams and
		// hand it to the player.
		if (!g_resyncGaveUp)
		{
			g_resyncGaveUp = true;
			Log(LOG_ERROR) << "[JOINT] world desync persisted through an auto-resync"
				<< " (within " << RESYNC_COOLDOWN_MINUTES << " game minutes);"
				<< " automatic repair disabled - advise the host to save and reload";
			showFail(game, "Desync repair failed. Ask the host to save and reload the campaign.");
		}
		return;
	}

	requestResync(game, "world checksum mismatch");
}

ResyncStats resyncStats()
{
	ResyncStats s;
	s.mismatches = g_mismatchN.load();
	s.requests = g_resyncReqN.load();
	s.pending = g_resyncPending;
	s.gaveUp = g_resyncGaveUp;
	s.lastGameMin = g_lastResyncGameMin;
	return s;
}

void resetResyncStats()
{
	g_mismatchN = 0;
	g_resyncReqN = 0;
	g_resyncPending = false;
	g_resyncGaveUp = false;
	g_mismatchLogged = false;
	g_mismatchSinceMs = -1;
	g_lastResyncGameMin = -1;
}

} // namespace JointEcon
} // namespace OpenXcom
