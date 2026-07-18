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
class Craft;
class Ufo;
class Target;
class Soldier;

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

// ---- PRD-J10: apply notification (live screen refresh) -----------------------
// OXCE Basescape/Geoscape states snapshot the world in their constructors, so a
// joint_apply landing while such a screen is open leaves it stale. There is ONE
// listener (last-registered-wins, matching the "only the top State cares" rule);
// it fires on the MAIN thread right after an apply mutated the world, on the host
// (post-validate) AND on every replica (from joint_apply).
//
// @a baseId is the apply's target base INDEX, or < 0 for a world-scoped mutation
// (funds-only, base creation, dogfights, ...). @a cmd is the protocol command name.
using ApplyListener = std::function<void(const std::string& cmd, int baseId)>;

/// Register the single apply listener. @a owner is an opaque identity token (pass
/// `this`) so a LATE destructor cannot clear a listener the next screen already
/// installed - OXCE deletes popped states one frame after the push that replaced
/// them, so the dtor of the old screen runs AFTER the new screen registered.
void setApplyListener(const void* owner, ApplyListener listener);
/// Drop the apply listener, but ONLY if @a owner still owns it (no-op otherwise).
void clearApplyListener(const void* owner);
/// Seat that originated the most recent apply (for "updated by <seatName>" text).
int lastApplySeat();

/// Index of @a base in SavedGame::getBases() - the JOINT shared base key - or -1.
int baseIndex(Game* game, const Base* base);

/**
 * PRD-J10 screen-refresh binding. A JOINT screen owns one of these, binds it in
 * init() and polls consume() from think(): the listener only raises a flag, so the
 * screen rebuilds itself at a safe point in its own lifecycle rather than from
 * inside the protocol drain (where a pop+push would fight the deferred deletion of
 * popped states, and a second apply in the same drain would pop the replacement).
 *
 * A screen that is not on top never gets think(), so a screen covered by a dialog
 * simply defers its rebuild until the dialog closes and init() runs again - that is
 * the PRD's "dialogs mid-command are exempt" rule, for free.
 */
class ScreenRefresh
{
public:
	/// Bind to the apply stream. No-op unless this is a JOINT campaign, so callers
	/// need no gate. @a base = the screen's base (null = interested in every base).
	/// @a wantProgress = also refresh on day_tick (progress columns); command
	/// screens leave it false so a daily tick cannot wipe an in-progress order.
	void bind(Game* game, const void* owner, Base* base, bool wantProgress = false);
	/// Release the listener (call from the screen's destructor).
	void unbind(const void* owner);
	/// True at most once per apply burst: "an apply for my base landed; rebuild".
	bool consume();
	/// Is this screen bound (i.e. a live JOINT refresh)?
	bool bound() const { return _bound; }
private:
	Game* _game = nullptr;
	Base* _base = nullptr;
	bool _dirty = false;
	bool _bound = false;
	bool _wantProgress = false;
};

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

/// PRD-J10: THE single "the host rejected your command" dialog. Every J05-J08
/// failure path funnels here (the screens never pop their own): the host's
/// machine-readable @a reason - an STR_ id where the vanilla rule had one, a bare
/// sentence otherwise - is shown to the initiator, translated when the language
/// knows the key and verbatim when it does not. Main thread.
void showFail(Game* game, const std::string& reason);

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
                        int units, int progress, bool sell);

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

// ---- PRD-J08: shared craft command + dogfight coordination -------------------
// Any player commands any craft; orders ride joint_cmd (craft_launch /
// craft_retarget / craft_return / craft_patrol), the host validates the vanilla
// fuel/crew/status rules against the authoritative world and applies in ARRIVAL
// order (last-command-wins - a later order for the same craft simply overrides).
// Dogfights follow locked decision (a): the player whose order engaged the UFO
// (the "initiating seat", tracked per craft from the orders) simulates the
// DogfightState on their machine against replica data; the result is reported
// via joint_cmd{dogfight_result}, applied by the host to the authoritative world
// and rebroadcast (joint_apply). While the initiator simulates, the engaged
// craft/UFO are exempt from the joint position snapshot so the local sim isn't
// overwritten mid-fight.

/// UI entry points (initiator side; JOINT only - caller gates). Each builds the
/// payload and submits the matching joint_cmd; nothing is mutated locally.
void submitCraftTarget(Game* game, Craft* craft, Target* target); // launch/retarget
void submitCraftPoint(Game* game, Craft* craft, double lon, double lat); // waypoint
void submitCraftReturn(Game* game, Craft* craft);
void submitCraftPatrol(Game* game, Craft* craft, bool autoPatrol);

/// Seat of the last order applied for @a craft, or -1 if it was never commanded
/// through the protocol (treat as host-owned -> vanilla local dogfight).
int lastCraftOrderSeat(const Craft* craft);

/// PRD-J09: assign/remove @a soldier to/from @a craft in the shared world (JOINT
/// only - caller gates). @a onOff = desired final state (true = aboard). Submits
/// craft_assign; mutates nothing locally (replica applies from joint_apply).
void submitCraftAssign(Game* game, Craft* craft, Soldier* soldier, bool onOff);

/// PRD-J09 GAP-5: set the ABSOLUTE quantity of @a itemType loaded on @a craft in
/// the shared world (JOINT only - caller gates). Submits craft_equip; mutates
/// nothing locally (host validates/clamps + broadcasts, replica applies). Items
/// only - vehicles are deferred (caller must not route a vehicle item).
void submitCraftEquip(Game* game, Craft* craft, const std::string& itemType, int desiredOnCraft);

/// PRD-J09 GAP-5b: mount @a weaponType (empty = dismount) in weapon @a slot of
/// @a craft in the shared world (JOINT only - caller gates). The launcher + loaded
/// clips move against the host-authoritative base stores, so a replica must route
/// this instead of doing it locally. Absolute end-state (which weapon in the slot),
/// last-write-wins. Submits craft_rearm; mutates nothing locally.
void submitCraftRearm(Game* game, Craft* craft, int slot, const std::string& weaponType);

/// PRD-J09 GAP-5b: set @a soldier's armor to @a armorType in the shared world
/// (JOINT only - caller gates). The old armor's store item is returned + the new
/// one consumed against the host-authoritative base stores, so a replica must
/// route this instead of doing it locally. Absolute end-state (which armor the
/// soldier wears), last-write-wins. Submits soldier_armor; mutates nothing locally.
void submitSoldierArmor(Game* game, Base* base, Soldier* soldier, const std::string& armorType);

/// HOST: a craft commanded by a non-host seat reached a flying UFO. Marks the
/// craft in-dogfight + the UFO remotely engaged, and broadcasts
/// joint_apply{dogfight_start} so the initiating seat opens the dogfight UI.
void hostRemoteDogfightStart(Game* game, Craft* craft, Ufo* ufo, int seat);

// ---- PRD-J10: the landing broker (deferred from PRD-J09) --------------------
// The host runs the only geoscape simulation, so ConfirmLandingState pops on the
// HOST even for a craft the CLIENT commanded - J09 shipped it that way and
// flagged it. This is pure UX routing: battle authority does NOT move (the coop
// battle is a lockstep parallel sim - both machines load the same "battlehost"
// blob), only the question "do you want to land?" is re-addressed to the seat
// that gave the order. Same shape as dogfight_start: host-origin broadcast, a
// seat-gated replica applier, and a reply command the host acts on.

/// HOST: a craft commanded by @a seat (> 0) reached a landable target. Broadcast
/// joint_apply{land_prompt} so THAT seat gets the confirm dialog. @a shade is the
/// host's day/night value (the replica's clock may differ by a tick).
void hostLandingPrompt(Game* game, Craft* craft, int seat, int shade);

/// REPLICA: the commanding seat answered its brokered landing dialog. Reports the
/// decision to the host (joint_cmd{land_reply}); the host owns the consequence.
/// @a yes = land; otherwise @a patrol picks "patrol here" over "return to base".
void submitLandReply(Game* game, Craft* craft, bool yes, bool patrol);

/// HOST: true while a remote (client-simulated) engagement is active on @a ufo.
/// Used to SERIALIZE engagements: a second craft reaching the same UFO waits.
bool isUfoRemotelyEngaged(int ufoId);

/// REPLICA: true if this machine is currently simulating a dogfight involving
/// the object - the joint position snapshot must not overwrite it mid-fight.
bool ufoLocallySimulated(int ufoId);
bool craftLocallySimulated(const Craft* craft);
bool clientDogfightActive(const Craft* craft, const Ufo* ufo);

/// REPLICA: the locally-simulated dogfight ended - report the outcome to the
/// host (joint_cmd{dogfight_result}). The local-sim exemption is kept until the
/// host's rebroadcast confirms the result, so the snapshot can't flap the state.
void clientDogfightEnded(Game* game, Craft* craft, Ufo* ufo);

// ---- PRD-J04 detect + PRD-J10 repair: world checksum -------------------------
// The host stamps a lightweight world checksum onto the periodic geoscape `time`
// heartbeat; the replica compares it against its own world. Fields: funds (exact
// value) + base / discovered-tech / total-item / soldier / transfer / production
// counts (GAP-4 widened the original funds+bases+research triple so store, roster,
// transfer and production drift can no longer hide). All are cheap O(bases)
// aggregates, identical host/replica by construction.
// J04 shipped log-only detection; J10 upgrades a mismatch to an AUTOMATIC repair:
// the replica asks for a fresh world (joint_resync_request), the host re-streams
// the authoritative world down the J02 bootstrap lane AND releases the client's
// resume hold, and the replica adopts it whole behind the existing wait dialog.
//
// Throttled: at most one auto-resync per RESYNC_COOLDOWN_MINUTES of GAME time. A
// second mismatch inside that window means the repair did not take, so the replica
// stops trying, logs everything and tells the player to save/reload.
void attachWorldChecksum(Game* game, Json::Value& msg);
void verifyWorldChecksum(Game* game, const Json::Value& msg);

/// Game-minute cooldown between automatic resyncs (see verifyWorldChecksum).
extern const int RESYNC_COOLDOWN_MINUTES;
/// Wall-clock ms a checksum mismatch must SURVIVE before it counts as a desync.
/// The checksum and the joint_apply that moves it are separate packets, so an
/// in-flight mutation is briefly visible here as a skew that heals itself.
extern const int RESYNC_DEBOUNCE_MS;

/// REPLICA: ask the host for a fresh authoritative world. @a why is logged.
/// @a force bypasses the throttle + the in-flight guard (the harness/debug hook).
/// Returns false if the request was throttled or this machine is not a replica.
bool requestResync(Game* game, const std::string& why, bool force = false);

/// A streamed JOINT world was adopted (LoadGameState): clear the in-flight resync
/// guard so a later drift can be repaired again.
void notifyWorldAdopted();

/// Harness/diagnostics: auto-resync bookkeeping on this machine.
struct ResyncStats
{
	uint64_t mismatches;  // checksum mismatches observed (replica)
	uint64_t requests;    // joint_resync_request sent (replica) / served (host)
	bool pending;         // a resync is in flight (replica)
	bool gaveUp;          // throttled out: the "save and reload" popup was shown
	int64_t lastGameMin;  // game-minute stamp of the last auto-resync, -1 = never
};
ResyncStats resyncStats();
void resetResyncStats();

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
