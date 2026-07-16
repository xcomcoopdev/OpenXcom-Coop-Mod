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

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../Engine/Game.h"
#include "../Engine/Language.h"
#include "../Engine/Logger.h"
#include "../Engine/State.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleCraft.h"
#include "../Mod/RuleResearch.h"
#include "../Mod/RuleManufacture.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/Base.h"
#include "../Savegame/BaseFacility.h"
#include "../Savegame/Craft.h"
#include "../Savegame/Transfer.h"
#include "../Savegame/Soldier.h"
#include "../Savegame/ResearchProject.h"
#include "../Savegame/Production.h"
#include "../Geoscape/GeoscapeState.h"
#include "../Geoscape/ResearchCompleteState.h"
#include "../Geoscape/ProductionCompleteState.h"

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

std::mutex g_mx;                     // guards the three queues below
std::deque<PendingCmd>  g_cmdQ;      // host:      to validate+apply+broadcast
std::deque<Json::Value> g_applyQ;    // replica:   joint_apply to apply
std::deque<std::string> g_failQ;     // initiator: joint_fail reasons to surface

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

// Visible failure dialog on the initiator. Reuse the existing purchase-failed
// popup (CoopState 551); the machine-readable reason is exposed via
// lastFailReason() for the harness. (J05 refines the wording per-command.)
void surfaceFail(Game* game)
{
	if (game) game->pushState(new CoopState(551));
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
			failReason = "joint soldier purchase not yet supported (J05)";
			return false;
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

void buyApply(Game* game, const Json::Value& payload, Base* base, int /*seat*/)
{
	if (!base) return;
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) return;
	auto& limitLog = save->getMonthlyPurchaseLimitLog();

	const Json::Value& items = payload["items"];
	if (!items.isArray()) return;

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
		default:
			break; // soldiers rejected at validate; nothing to materialize
		}
	}
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

// research_done payload: { research, bonus, newResearch } (rule names; bonus /
// newResearch may be "").
void researchDoneApply(Game* game, const Json::Value& payload, Base* base, int /*seat*/)
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
void facDoneApply(Game* game, const Json::Value& payload, Base* base, int /*seat*/)
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

// prod_done payload: { manufacture, units, progress }.
void prodDoneApply(Game* game, const Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return;
	if (!base) return;
	SavedGame* save = game->getSavedGame();
	Mod* mod = game->getMod();
	if (!save || !mod) return;

	const std::string mName = payload.get("manufacture", "").asString();
	int units = payload.get("units", 0).asInt();
	RuleManufacture* rule = mod->getManufacture(mName, false);
	if (!rule || units <= 0) return;

	// Materialize the deterministic output (items + crafts). Random/spawned-person
	// production is host-RNG and NOT reconstructed here (documented limitation);
	// the next joint_apply / checksum surfaces any resulting drift.
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
void transferArrivedApply(Game* game, const Json::Value& payload, Base* base, int /*seat*/)
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

// day_tick payload: { soldiers: [ {id, recovery} ] }.
void dayTickApply(Game* game, const Json::Value& payload, Base* base, int /*seat*/)
{
	if (connectionTCP::getHost()) return;
	if (!base) return;
	const Json::Value& soldiers = payload["soldiers"];
	if (!soldiers.isArray()) return;
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
		// surfaces a joint_fail received from the host.
		surfaceFail(game);
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
	// to the initiator.
	SavedGame* save = game->getSavedGame();
	save->setFunds(save->getFunds() - cost);
	hit->second.apply(game, pc.payload, base, pc.seat);
	++g_applyN;

	Json::Value apply;
	apply["state"] = "joint_apply";
	apply["cmd"] = pc.cmd;
	apply["seq"] = pc.seq;
	apply["seat"] = pc.seat;
	apply["baseId"] = pc.baseId;
	apply["payload"] = pc.payload;
	apply["funds"] = Json::Value::Int64(save->getFunds());
	broadcast(game, apply);

	if (pc.remote && game->getCoopMod())
	{
		Json::Value ok;
		ok["state"] = "joint_ok";
		ok["seq"] = pc.seq;
		game->getCoopMod()->sendTCPPacketData(ok.toStyledString());
		++g_okN;
	}
}

// ---- Replica-side apply (main thread) ----------------------------------------
void processApply(Game* game, const Json::Value& ap)
{
	SavedGame* save = game->getSavedGame();
	// Funds are host-authoritative: adopt them from the packet no matter what, so
	// the replica cannot drift even if the mutation itself cannot be reconstructed.
	if (save && ap.isMember("funds")) save->setFunds(ap["funds"].asInt64());

	std::string cmd = ap.get("cmd", "").asString();
	Base* base = resolveBase(game, ap.get("baseId", -1).asInt());
	auto& reg = registry();
	auto hit = reg.find(cmd);
	if (hit == reg.end() || !base) return; // healthy host never broadcasts this

	hit->second.apply(game, ap["payload"], base, ap.get("seat", 0).asInt());
	++g_applyN;
}

} // anonymous namespace

// ---- Public API --------------------------------------------------------------
void registerCmd(const std::string& cmd, CmdValidator validate, CmdApplier apply)
{
	registry()[cmd] = Handler{ std::move(validate), std::move(apply) };
}

void init()
{
	if (g_inited) return;
	g_inited = true;
	registerCmd("buy", &buyValidate, &buyApply);
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
		bool have = false;
		{
			std::lock_guard<std::mutex> lk(g_mx);
			if (!g_failQ.empty()) { g_failQ.pop_front(); have = true; }
		}
		if (!have) break;
		surfaceFail(game);
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
                        int units, int progress)
{
	if (!jointHost(game)) return;
	Json::Value p;
	p["manufacture"] = manufacture;
	p["units"] = units;
	p["progress"] = progress;
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

void hostDayTick(Game* game)
{
	if (!jointHost(game)) return;
	SavedGame* save = game->getSavedGame();
	if (!save) return;
	auto* bases = save->getBases();
	for (int bi = 0; bi < (int)bases->size(); ++bi)
	{
		Json::Value soldiers(Json::arrayValue);
		for (auto* soldier : *(*bases)[bi]->getSoldiers())
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
		if (!soldiers.empty())
		{
			Json::Value p;
			p["soldiers"] = soldiers;
			submitLocalCmd(game, "day_tick", bi, p);
		}
	}
}

// ---- PRD-J04 world checksum (log-only) ---------------------------------------
void attachWorldChecksum(Game* game, Json::Value& msg)
{
	if (!game || !game->getSavedGame()) return;
	SavedGame* save = game->getSavedGame();
	msg["chkFunds"] = Json::Value::Int64(save->getFunds());
	msg["chkBases"] = (int)save->getBases()->size();
	msg["chkResearch"] = (int)save->getDiscoveredResearch().size();
}

void verifyWorldChecksum(Game* game, const Json::Value& msg)
{
	if (!game || !game->getSavedGame()) return;
	if (!msg.isMember("chkFunds")) return; // older/non-JOINT host
	SavedGame* save = game->getSavedGame();
	int64_t hostFunds = msg["chkFunds"].asInt64();
	int hostBases = msg.get("chkBases", -1).asInt();
	int hostResearch = msg.get("chkResearch", -1).asInt();
	int64_t myFunds = save->getFunds();
	int myBases = (int)save->getBases()->size();
	int myResearch = (int)save->getDiscoveredResearch().size();
	if (hostFunds != myFunds || hostBases != myBases || hostResearch != myResearch)
	{
		Log(LOG_WARNING) << "[JOINT] world checksum mismatch (repair is J10): "
			<< "funds host=" << hostFunds << " replica=" << myFunds
			<< ", bases host=" << hostBases << " replica=" << myBases
			<< ", research host=" << hostResearch << " replica=" << myResearch;
	}
}

} // namespace JointEcon
} // namespace OpenXcom
