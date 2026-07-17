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

#include <cstdint>
#include <functional>
#include <string>

#include <json/json.h>

namespace OpenXcom
{

class Game;
class Base;

/**
 * PRD-J03: the generic JOINT economy command protocol.
 *
 * In a JOINT campaign there is a single host-authoritative world; clients hold
 * replicas. Every economy mutation (buy/sell/transfer/research/... - only "buy"
 * is wired here, the rest land in J05-J08) rides this protocol:
 *
 *   joint_cmd   client -> host   "please apply this mutation" (client mutates
 *                                 nothing locally)
 *   joint_ok    host   -> client "accepted" (correlated by seq)
 *   joint_fail  host   -> client "rejected: reason"
 *   joint_apply host   -> ALL    "validated + applied; here is the mutation and
 *                                 the new authoritative funds" (replicas apply
 *                                 from this ONLY)
 *
 * Flow (client-originated): client submitLocalCmd() -> joint_cmd -> host
 *   validates on the MAIN thread, debits funds, applies, broadcasts joint_apply
 *   to all + joint_ok to the initiator. The client applies from joint_apply.
 * Flow (host-originated): host submitLocalCmd() -> validate+apply locally +
 *   broadcast joint_apply.
 *
 * Every joint_apply carries the post-mutation authoritative getFunds(); replicas
 * setFunds(funds) on apply, so funds self-heal per mutation. The SEPARATE
 * coopFunds peer-mirror is fenced off in JOINT and stays fenced.
 *
 * Anti-if-chain: joint_* messages are NOT appended to the onTCPMessage if-chain.
 * A single early hook (onMessage) routes them here; per-command behavior lives in
 * this dispatch table. Later PRDs add registerCmd() entries only.
 *
 * Threading: onMessage()/update()/submitLocalCmd() all run on the MAIN thread
 * (onTCPMessage is drained from updateCoopTask, which Game::run calls). The queues
 * below are still mutex-guarded, matching the "network thread queues, main thread
 * applies" idiom, so nothing breaks if a future transport calls onMessage
 * off-thread.
 */
namespace JointEcon
{

/**
 * Host-only pre-flight for a command. Recompute the authoritative cost/space
 * from the mod rules against the live world - never trust client-sent totals.
 * Set @a cost to the funds delta to DEBIT (negative = credit, e.g. a future
 * sale). Return false and fill @a failReason to REJECT (the world stays
 * untouched; the initiator gets joint_fail). @a base is the resolved target, or
 * null if @a baseId did not resolve (reject in that case). Runs on the host,
 * main thread.
 */
using CmdValidator = std::function<bool(Game* game, const Json::Value& payload,
                                        Base* base, int seat,
                                        int64_t& cost, std::string& failReason)>;

/**
 * Mutation applier: create transfers / mutate the shared world. Runs on the HOST
 * (after a passing validate) AND on every REPLICA (from joint_apply). MUST be
 * deterministic and funds-free: the protocol debits the host and setFunds() on
 * replicas from the packet's authoritative funds, so the applier must never
 * touch funds. @a base is the resolved target (never null here).
 *
 * @a payload is passed by mutable reference so the HOST applier may RESOLVE any
 * host-only RNG into it before it is broadcast (e.g. "buy" serializes each hired
 * soldier's generated YAML into the payload; replicas reconstruct from that YAML
 * rather than re-rolling and diverging). The applier must remain deterministic
 * GIVEN the (post-resolution) payload, so host and replica produce identical
 * worlds. Replicas receive the resolved payload and must never re-resolve.
 */
using CmdApplier = std::function<void(Game* game, Json::Value& payload,
                                      Base* base, int seat)>;

/// Register (or overwrite) a command's validate + apply callbacks.
void registerCmd(const std::string& cmd, CmdValidator validate, CmdApplier apply);

/// One-time registration of the built-in commands (currently "buy"). Idempotent;
/// safe to call from every connectionTCP ctor.
void init();

/// Single early hook for connectionTCP::onTCPMessage. Returns true if @a state
/// was a joint_* protocol message (and was consumed). Main thread.
bool onMessage(Game* game, const std::string& state, const Json::Value& obj);

/// Main-thread pump: drains queued commands (host) / applies (replica) / failures
/// (initiator) at a controlled point. Single call from updateCoopTask, next to
/// the waitedTrades drain.
void update(Game* game);

/// UI entry point (main thread). On the HOST: queues the command for local
/// validate+apply+broadcast. On a REPLICA: emits a joint_cmd to the host and
/// mutates nothing locally.
void submitLocalCmd(Game* game, const std::string& cmd, int baseId,
                    const Json::Value& payload);

/// Send a built protocol message to every connected peer. Transport is 1:1 today
/// (PRD-J01 audit) so this degenerates to the single-peer send; N-player only
/// changes this body.
void broadcast(Game* game, const Json::Value& msg);

// ---- PRD-J04: host simulation-result broadcasts ------------------------------
// In JOINT only the host runs world simulation; replicas are frozen (their
// timeXxx handlers early-return). Each host-only completion event is therefore
// mirrored to replicas over the SAME registerCmd/joint_apply channel: the host
// has ALREADY applied the mutation via vanilla code, so it submits a command
// whose validator is always-accept (cost 0, funds carried authoritatively) and
// whose applier runs on REPLICAS ONLY (it early-returns on the host to avoid a
// double-apply). Each helper is a no-op unless isJointCampaign() && the caller is
// the host; call them right where the vanilla sim applied the change.

/// Research finished on the host (time1Day): replica removes the base's matching
/// ResearchProject (freeing its scientists), adds the discovered topic + bonus +
/// lookups deterministically (no RNG re-roll), and pops ResearchCompleteState.
void hostResearchDone(Game* game, int baseId, const std::string& research,
                      const std::string& bonus, const std::string& newResearch);

/// Facility construction finished on the host (time1Day): replica sets the
/// facility at (x,y) buildTime 0 and mirrors the completion popup.
void hostFacilityDone(Game* game, int baseId, int x, int y,
                      const std::string& facilityType);

/// Manufacture batch finished on the host (time1Hour): replica materializes the
/// produced items/crafts, removes the Production (freeing engineers), and mirrors
/// the completion popup.
void hostProductionDone(Game* game, int baseId, const std::string& manufacture,
                        int units, int progress);

/// Transfers delivered on the host (time1Hour): replica delivers the matching
/// pending transfers (items/scientists/engineers/craft) and removes them.
void hostTransferArrived(Game* game, int baseId, const Json::Value& arrived);

/// End-of-day soldier changes on the host (time1Day): replica adopts the
/// per-soldier wound-recovery values for CHANGED soldiers only.
void hostDayTick(Game* game);

/// PRD-J07: a base was destroyed by retaliation on the host (it already removed
/// the base in BaseDestroyedState). Mirror the removal to replicas (they erase the
/// same base index + pop a popup). @a baseId = the destroyed base's index.
void hostBaseDestroyed(Game* game, int baseId, const std::string& name);

// ---- PRD-J04: lightweight world checksum (log-only desync detect) ------------
// Repair is PRD-J10; here the host stamps funds + base count + discovered-tech
// count onto an outgoing snapshot and the replica logs a warning on mismatch.
void attachWorldChecksum(Game* game, Json::Value& msg);
void verifyWorldChecksum(Game* game, const Json::Value& msg);

/// Harness / diagnostics: monotonic counters for the protocol traffic this
/// process has processed, plus the most recent joint_fail reason surfaced here.
struct Stats
{
	uint64_t cmd;     // joint_cmd validated by this host
	uint64_t ok;      // joint_ok received (initiator) or sent (host)
	uint64_t fail;    // joint_fail surfaced (initiator) or sent (host)
	uint64_t apply;   // joint_apply applied by this replica / host
	uint64_t unknown; // joint_cmd with an unregistered cmd string
};
Stats stats();
std::string lastFailReason();
void resetStats();

} // namespace JointEcon

} // namespace OpenXcom
