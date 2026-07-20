# JOINT campaign — implemented feature reference (2026-07-17)

Post-implementation reference for the JOINT co-op campaign mode. For the
pre-implementation analysis see `joint-campaign-shared-bases-analysis.md`; for the
PRDs/session-notes see `.agents/prds/joint/**` (uncommitted). For tests see
`coop-test-harness.md`.

## Status

- Branch `feat/joint-campaign` on `origin` (`OpenXcom-Coop/OpenXcom-Coop-Mod`),
  rebased on `origin/main`; ~24 commits; suite **all-green** (55 JOINT+legacy tests
  + boot_check, ~16 min serial). No PR opened yet. **Checked out in the main repo
  root** (worktree removed 2026-07-20); builds green there — see "Selecting &
  running it".
- Built across 3 waves: **J01-J11** (shared-economy campaign), **GAP-1..9**
  (hardening), **DF01-DF03** (shared/replicated dogfights).
- **Only open item: GAP-3** — N-player (>2) transport is 1:1 (issue
  `OpenXcom-Coop/OpenXcom-Coop-Mod#63`). The seat/command/broadcast layers are
  N-ready; only the wire isn't.

## Selecting & running it (playtest)

- **Working copy:** `feat/joint-campaign` is checked out in the **main repo root**
  `C:\Users\bentl\OpenXcom-Coop-Mod` (the dedicated `joint-campaign` worktree was
  removed 2026-07-20 to consolidate). Build/run from there.
- **Exe to run:** `C:\Users\bentl\OpenXcom-Coop-Mod\bin\x64\Release\OpenXcom.exe`.
  **Gotcha:** several stale `OpenXcom.exe` copies exist under other worktrees +
  the old main-repo build — running one of those shows NO JOINT option. Confirm
  you're on the right build: Main Menu → NEW GAME must show three buttons.
- **Selecting JOINT vs SEPARATE:** Main Menu → **NEW GAME** opens the mode picker
  (`NewGameModeState`, file-local in `src/Menu/MainMenuState.cpp`) with three
  buttons: **`SOLO`**, **`CO-OP (SEPARATE)`**, **`CO-OP (JOINT)`**. Literal labels
  (no `STR_COOP_*` keys). `CO-OP (JOINT)` → `NewGameState(true,
  CoopCampaignType::Joint)` → coop lobby. The choice is immutable for the campaign.
  Both players see the type in the `HostMenu`/`LobbyMenu` before roster lock.

## What it is

A campaign is **JOINT** or **SEPARATE**, chosen at creation, immutable. SEPARATE =
the pre-existing behavior (two mirrored economies). JOINT = **one host-authoritative
world** shared by all players (bases, funds, research, manufacture, crafts,
geoscape objects), with each player keeping sole control of their own soldiers in
the battlescape. `SavedGame::_campaignType`; gate everything on
`connectionTCP::isJointCampaign()` (and `isJointReplica()` = joint && !host).
SEPARATE must be byte-identical — the pre-existing mirror machinery
(`Base::_coopBase`, `coopBase*` marker packets, cross-player `transfer` commerce,
`playersFunds`) is SEPARATE-only and fenced off in JOINT.

## Architecture (the load-bearing model)

- **One authoritative world (host).** Clients hold a **replica** streamed at
  campaign start / resume via the existing blob/file-transfer machinery
  (`streamJointWorldToClient()` → resume-blob → `MAP_RESULT_LOAD_PROGRESS` →
  `CoopState(555)` → `LoadGameState(loadCoopProgress=true)`). Replicas never save
  to disk.
- **Command protocol (`src/CoopMod/JointEcon.cpp`, ~pure-ASCII).** Every mutation
  is a `joint_cmd` to the host; host re-validates against the authoritative world
  (never trusts client totals), applies, and broadcasts `joint_apply`. Dispatch:
  `registerCmd(cmd, validate, apply)` in `init()`; the single hook is one
  `if (JointEcon::onMessage(...)) return;` at the top of
  `connectionTCP::onTCPMessage`; one `JointEcon::update(game)` in `updateCoopTask`.
  Host applies commands from `g_cmdQ` in **receive-order** (= arbitration order).
  `CmdApplier` takes a **mutable** `Json::Value&` so the host resolves RNG /
  generated entities INTO the broadcast payload; replicas reconstruct, never
  regenerate (the buy-soldier pattern — avoids RNG divergence).
- **Every `joint_apply` carries authoritative funds** → funds self-heal per
  mutation. Replica `_funds` is written ONLY by: bootstrap/restream stream,
  `joint_apply`, `monthly_report`, post-battle restream. Do not add another funds
  channel.
- **Host-sim authority.** Replicas' mutating `timeXxx` handlers early-return
  (`time5Seconds/10Minutes/30Minutes/1Hour/1Day/1Month`); the host simulates and
  broadcasts results (research_done, prod_done, fac_done, transfer_arrived,
  day_tick, monthly_report). Replica-visible geoscape MOVEMENT rides the
  `SNAP_GEO_POSITIONS` conflation snapshot's `joint:true` variant.
- **Seats.** `connectionTCP::localSeat()/seatCount()/seatName()`, host = seat 0,
  indexed into `SavedGame::_coopPlayers`. No `host?0:1` hardcodes in JOINT paths.
- **Transport is 1:1** (`startTCPHost` holds a single `clientSock`; 3rd client
  dropped silently). `broadcast()` is a seam that degenerates to the single peer —
  N-player only changes its body (GAP-3 / #63).
- **Desync repair.** A world checksum rides the `time` heartbeat
  (`attachWorldChecksum`/`verifyWorldChecksum` in JointEcon): `chkFunds`,
  `chkBases`, `chkResearch`, `chkItems`, `chkSoldiers`, `chkTransfers`,
  `chkProduction` (integer aggregates; income/expenditure SERIES deliberately
  EXCLUDED — see GAP-9). Mismatch → 3s debounce → `joint_resync_request` → host
  re-streams the world → replica reloads. Log once per episode (the heartbeat is
  ~2 kHz — per-packet logging starves the repair it triggers).

## Command inventory (all JOINT-gated, host-authoritative)

Economy: `buy` (items/soldiers/scientists/engineers/crafts — soldiers generated
host-side + serialized), `sell`, `containment`, `transfer` (intra-world base→base),
`res_start/res_alloc/res_cancel`, `man_start/man_alloc/man_cancel` (allocations
carry ABSOLUTE targets = last-write-wins). Bases/facilities: `fac_build`,
`fac_dismantle`, `base_new`, `base_rename`, `base_destroyed`, `sack`. Soldiers:
`soldier_rename`, `soldier_gift` (host-authoritative ownership move; give-unit).
Equip: `craft_equip` (items base↔craft), `craft_rearm` (weapon mount),
`soldier_armor` (armor swap — used by SoldierArmorState AND CraftArmorState).
Craft/geoscape: `craft_launch/craft_retarget/craft_return/craft_patrol`. Landing
broker (all seats prompted, first-answer-wins): `land_prompt`, `land_reply`,
`land_close`. Dogfights: `df_open`, `df_state`, `df_close`, `df_cmd` (see below).
Sim results: `research_done`,
`prod_done` (carries `sell` flag + capped count), `fac_done`, `transfer_arrived`,
`day_tick`, `monthly_report`.

## Shared/replicated dogfights (DF01-DF03)

Replaces the OLD J08 "initiator client simulates, reports one result" model
(hostRemoteDogfightStart / dogfightResultApply / clientDogfight* / g_localSim* /
g_remoteEngagedUfo — all **REMOVED**; the duplicated retaliation/score roll was
deleted, host-side `DogfightState::update` is the single home).

- **Host simulates EVERY JOINT dogfight** (Lane 1 UFO-hunts-craft + vanilla Lane 2
  craft-chases-UFO both open the host's own `DogfightState`).
- **Render-only replicas** via `bool DogfightState::_isReplicaView`
  (`= isJointCampaign() && !getServerOwner()`): ONE early gate in `update()`
  (apply latest frame → `animate()` → return; skip the entire sim body, all RNG +
  mutation + projectile-move + end-detection). `applyFrame()` populates members +
  rebuilds `_projectiles`; draw helpers reused verbatim.
- **`df_state`** (per-tick frame, 20-50 Hz) rides a NEW conflation slot
  `SNAP_DOGFIGHT` — last-write-wins, one combined full-set frame per tick, drains
  at link rate. **NEVER the FIFO** (the TX-flood-sensitive lane). Carries
  `epoch`, `hostAnyOpen`, and per-fight dist/mode/projectiles/ufoStance/etc.
- **`df_open`/`df_close`/`df_cmd`** ride the reliable FIFO `joint_apply`/`joint_cmd`
  lane. `df_open` = the full membership set + a monotonic `membershipEpoch`; the
  HK interrupt-all-and-restart reshuffle is ONE `df_open`. `df_state` frames older
  than the replica's current epoch are dropped (kills the reshuffle race between
  the two lanes).
- **`df_cmd`** {stance/weapon/minimize/disengage/selfDestruct}: replica buttons
  emit it (with optimistic local echo) instead of mutating; host applies via the
  same `Simulate*LeftPress`/`weaponClick`/`setMinimized` lane in `g_cmdQ`
  receive-order = **last-received-wins arbitration**.
- **Synced attack marker** = `ufoStance` (`Ufo::getHuntBehavior()`) in `df_state`.
- **Presentation policy (owner decision):** the commanding seat
  (`lastCraftOrderSeat(craft)`; host for HK/host-commanded) opens the FULL window;
  other players get a minimized icon (`df_open` carries `commandingSeat`; replica
  opens `_minimized = mySeat != commandingSeat`). Local minimize is view-only; the
  host's `hostAnyOpen` drives the world clock.
- `df_close` was NOT implemented (the `df_open` full-set diff subsumes open+close).

## Load-bearing invariants / traps (each cost a debugging session)

1. **`baseId` = INDEX into `SavedGame::getBases()`** for every command + day_tick +
   fac_done. Base add/remove MUST ride `joint_apply` or every index-keyed command
   misroutes.
2. **`GeoscapeState::init` post-battle soldier-delete loop** must be fenced
   `!isJointCampaign()` — unfenced it ERASES every client-owned soldier in JOINT
   (they carry `_coop=1`, `coopBase==-1`). Same for the SEPARATE dual-world blob
   dance + guest cleanup.
3. **Paired-fence trap:** fencing a ctor filter (e.g. `SoldiersState`) without its
   paired restore wipes the roster. Fence both halves.
4. **Coop battle is LOCKSTEP PARALLEL SIM** — both machines load the same
   `battlehost` blob and both run debriefing, so JOINT worlds are already identical
   post-battle; the whole-world restream is belt-and-braces + re-establishes the
   frozen-replica relationship.
5. **Restream ⇒ hold-release.** Any automatic world restream to a replica MUST
   release the client's resume hold (`resume_ack` auto-sends `campaign_begun`), or
   the client hangs forever in `COOP_DLG_CLIENT_RESUME_HOLD` (dialog 68). Two
   triggers today (post-battle, resync); a third must arm an auto-release flag.
6. **`time` heartbeat is ~2 kHz** — never per-packet log/allocate on it; a checksum
   skew is not desync (in-flight `joint_apply`), hence the 3s debounce.
7. **Detection ≠ prevention:** the widened checksum makes store/roster/production
   drift SELF-HEAL via resync, but root-cause fixes still belong in their own place
   (recurring resyncs otherwise).
8. **Latin-1 + TestServer C1061** — see `dev-workflow.md`.

## Gap-hardening outcomes (GAP-1..9)

GAP-1 base-defense control split (stamps `_coop` from ownership; was erasing client
soldiers). GAP-2 HK-dogfight routing = DESIGN, dissolved by the dogfight feature.
GAP-4 widened checksum. GAP-5/5b equip-class store drift on 5 base screens
(`craft_equip`/`craft_rearm`/`soldier_armor`). GAP-6 prod_done overshoot count-cap.
GAP-6b sell-flagged production. GAP-7 pilot XP (host-authoritative, closed by
dogfight feature). GAP-8 `_dest`/waypoints (host-authoritative, closed by dogfight
feature). GAP-9 income/expenditure graph series (host-authoritative via
`setFundsRaw` + tails in `joint_apply`; funds were always exact). **GAP-3 remains**
(N-player transport, #63).

## Playtest fixes — 2026-07-20 (branch `fix/joint-playtest-bugs`)

Two playtest passes + a design correction. Board: `.agents/prds/joint/PLAYTEST-BUGS.md`.
All red-first harness tests, red-proofed.

**Round 1 (B1-B7):** live-refresh the base screen under the BuildFacilities popup
(covered state never `think()`s → bind ScreenRefresh in BuildFacilitiesState);
live-sync the NEW RESEARCH PROJECTS list (NewResearchListState ScreenRefresh);
`soldier_rename` joint_cmd; split JOINT starting soldiers between seats; UFO alerts
to ALL seats (client matcher required `getCoop()==true` — JOINT replica UFOs are
`false`); dogfight stance-button highlight sync (B6); RESUME GAME in the in-game coop
menu (LobbyMenu gated on `sessionLocked` + a GeoscapeState underneath).

**Round 2:** dogfight window opens FULL for every player (dropped the
`commandingSeat` minimize); replica stance buttons are HOST-AUTHORITATIVE (no radio
group, no optimistic echo → no click-revert, no highlight desync — `mousePress`
moved the ImageButton invert behind the tracker); craft-arrived / landing alerts go
to ALL seats (broker to all + host pops its own + `land_close` closes the losers,
first-answer-wins via the pending-erase guard); coop-menu host NAME is role-relative
(`getServerOwner() ? getHostName() : getCurrentClientName()` — the getters are
machine-relative, see [[coop-name-getters-machine-relative]]).

## Soldier ownership = SEPARATE (undoes the J09 shared-roster mistake)

**J09 was wrong:** it deliberately showed the whole shared roster to everyone
("every player must see ALL soldiers (mixed-owner squads)") and fenced the SEPARATE
guest-filter off in JOINT. Corrected 2026-07-20: JOINT soldiers behave like SEPARATE.

- **Data was already correct** (only the VIEWS leaked): `getOwnerPlayerId()` 0=host,
  1=client, serialized, split at creation AND on load
  (`SavedGame::migrateJointSoldierOwnership`, id-parity, heals pre-split saves), hire
  owns the buyer.
- **Views filter NON-destructively.** New `JointEcon::visibleSoldiers(game,base)` /
  `ownsSoldier(game,s)` (JOINT: `getOwnerPlayerId()==localSeat()`). Each list view
  builds a LOCAL filtered copy and routes display + row-index + click + reorder
  through it. **NEVER mutate `_base->getSoldiers()`** in JOINT: its COUNT is in the
  desync checksum (`chkSoldiers`) so a destructive swap (the SEPARATE mechanism)
  drops the count → false resync; and a `joint_apply` can land mid-screen (J10). So
  reorder/sort are disabled in JOINT (would reorder the shared roster).
  - Trap: a filtered display list + an `init()` loop that still iterates the FULL
    roster to set cell text → out-of-bounds → crash (hit in CraftArmorState). Iterate
    the same filtered `_viewSoldiers` everywhere.
- **Views done:** SoldiersState, CraftSoldiersState (assign own → mixed crew; Space
  Used still counts the whole craft), SellState, TransferItemsState,
  CraftPilotSelectState, SoldierMemorialState, SoldierTransformationListState,
  CraftArmorState, SoldierInfoState prev/next (skips the peer's soldiers).
- **Counts/capacity stay SHARED** (living quarters, base info) — physically one base.
- **Gifting (give-unit) is now host-authoritative:** the SEPARATE `giftSoldier`
  local+broadcast never reached the JOINT replica → new `soldier_gift` joint_cmd
  (host sets owner + broadcasts joint_apply); the soldier moves between both players'
  lists. Battle-time gifts keep the live-control path.
- **Battlescape needed NO change:** zero `isJointCampaign()` branches in
  `src/Battlescape/` — JOINT reuses the SEPARATE lockstep, gated by `BattleUnit._coop`
  (0/1 from owner at entry). Each player controls only their own units on their own
  turn already; verified by the bootstrap-split → battle coop-split test.

New tests: `test_joint_soldier_visibility` / `_craft_soldiers_visibility` /
`_soldier_views_visibility` / `_gift` / `_soldier_ownership_battle` (+ the round-1/2
tests). Plan: `.agents/prds/joint/PLAN-soldier-ownership-parity.md`.

## Residual limits (documented, non-blocking)

WAN dogfight command-latency feel untested (localhost harness); `df_close`
subsumed; `Ufo` huntBehavior read-only (marker asserts parity, not runtime flip);
modded-ruleset spawned/random production not reconstructed (base xcom1 has none);
CraftEquipment vehicles/HWP + loaded-clip re-equip deferred (checksum backstops).

## Key files

`src/CoopMod/JointEcon.{h,cpp}` (command protocol + checksum + dogfight apply),
`src/CoopMod/connectionTCP.{h,cpp}` (transport, snapshot slots, onTCPMessage hook,
seats), `src/Geoscape/GeoscapeState.{h,cpp}` (sim authority, dogfight orchestration,
membership reconcile), `src/Geoscape/DogfightState.{h,cpp}` (`_isReplicaView`,
buildStateFrame/applyFrame, df_cmd handlers), `src/CoopMod/TestServer.cpp`
(harness hooks — `executeJoint10`/`executeJoint11`), `src/Savegame/SavedGame.cpp`
(`_campaignType`, `setFundsRaw`, checksum aggregates).
