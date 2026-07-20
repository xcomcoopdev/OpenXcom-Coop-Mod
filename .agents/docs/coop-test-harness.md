# Coop test harness (autonomous two-client testing)

> **Provenance note (2026-07-17, PRD-J11):** this file was MISSING from the
> working tree — `.agents/` is kept untracked, and the local copy was lost. It
> was restored from git history (`f86dc5bbb:.agents/docs/coop-test-harness.md`,
> the last tracked version) and the JOINT section below was added to it.
> **Sections above the JOINT one are that old snapshot and are stale in places**
> — they name tests that no longer exist (`test_transfer_legacy.py`,
> `test_transfer_fresh.py`, `test_transfer_rollback.py`, `debug_lobby.py`) and
> predate the flow redesign. `tools/coop_test/README.md` is the tracked,
> maintained harness doc; treat this file as the agent-facing notes.

Run live end-to-end coop tests without a human: an in-game command server
drives real game instances (menus, saves, sessions, transfers, geoscape play,
and full battlescape combat) and reads game state. Built 2026-07-04; used to
validate soldier ownership transfers (see
[`coop-soldier-transfer.md`](coop-soldier-transfer.md)). Extended 2026-07-05
into a full autonomous-play driver (geoscape time driving, craft dispatch,
coop battle entry, tactical combat, extraction) with per-tick host/client
cross-validation — proven on a complete month-long 2-player campaign.

## Architecture

- **In-game server**: `src/CoopMod/TestServer.{h,cpp}` — active only when the
  `OXC_TEST_PORT` env var is set. Listens on that port (localhost only),
  newline-delimited JSON commands, executed on the main thread via a per-frame
  pump in `Game::run()` (so all state access is race-free).
- **Python driver**: `tools/coop_test/harness.py` — `GameClient` (spawn +
  socket + `wait_for` polling), `make_user_dir()` (isolated `-user` folders
  seeded from the real `options.cfg`, with intro cutscene, audio, mouse capture
  and fullscreen all disabled; small windows tucked into corners so the user
  can keep working while tests run).
- **Tests** (run `python tools/coop_test/<file>` — spawns two real game windows):
  - `test_transfer_legacy.py` — Jerzy transfer on the legacy `.sav`.
  - `test_transfer_fresh.py` — transfer on a brand-new campaign, both sides.
  - `test_bug_fixes.py` — visit-window loss, dialog flicker, owner resolution,
    notice display, stacked-notice colors. Exposes `bootstrap_fresh_session()`
    and `own_base()` reused by other tests.
  - `test_transfer_rollback.py` — the host-save-is-authority repro (transfer A,
    save, transfer B, abandon, reload) in BOTH directions + stacked notices.
  - `boot_check.py` — single-instance smoke test for install validation.
  - `play_harness.py` — autonomous 2-player playthrough driver (geoscape +
    battlescape + cross-validation); see its own section below.
- Debug scratchpad: `tools/coop_test/debug_lobby.py` — step-through with full
  state/flag dumps. `[coop-transfer]`/`[testserver]` lines land in each
  instance's `openxcom.log` (`%TEMP%\oxc-coop-test\<host|client>-user\`).

## Commands (TestServer::execute)

Session/introspection: `ping`, `quit`, `get_state` (state-stack typeids),
`get_coop` (all session flags + `insideCoopBase`, `saveID`), `get_soldiers`
(bases + rosters + coop fields), `get_mirror_soldiers {coopBaseId}` (what the
peer-base visit view would list), `has_coop_file {key}`, `set_option
{name,value}` (HostSaveProgress only so far).

Flow drivers (invoke real State handlers, made public where needed —
`Profile::buttonOK`, `NewGameState::btnOkClick`, `BuildNewBaseState::placeAt`,
`BaseNameState::setNameAndConfirm`, `SoldiersState::btnOkClick`,
`BasescapeState::btnGeoscapeClick`, `TransferNoticeState::btnOkClick` — rather
than faking SDL input): `load_save {file}`, `save_game {file}`, `open_new_game`,
`newgame_ok`, `place_first_base {lon,lat,name}`, `profile_ok`, `host_tcp
{server,port,player}`, `join_tcp {ip,port,player}`, `lobby_ready`,
`client_reload_progress` (reconnect: ask host for our world).

Transfer/UI: `transfer {name,owner}`, `transfer_targets {name}` (what the
dialog would offer — validates owner resolution without UI), `rename_soldier
{name,newName}`, `open_soldiers {base}` / `soldiers_ok`, `visit_coop_base
{base}` / `leave_base`, `open_transfer_dialog {name}` / `cancel_dialog`,
`show_notice {message}` / `get_notices` (returns each notice's interface
category) / `dismiss_notice`, `get_palettes` (top states' first colors, for the
flicker check).

Geoscape play (added 2026-07-05):

- `geo_state` — read-only snapshot: `time{year,month,day,hour,minute}`,
  `funds`, `monthsPassed`, per-base `{name, crafts[{type,status}],
  research[{name,spent,cost}], soldiers}`, `ufos[{id,type,detected,status}]`,
  `missionSites[{id,type,race,city}]`. No coop side effects.
- `geo_set_speed {idx}` — select a time-speed button, 0=5s … 5=1day, via the
  new public `GeoscapeState::setTimeSpeedIndex(int)` (synthesizes a real
  radio-group click so the coop speed broadcast behaves as a user click).
  Coop rule: time advances fast only when BOTH players pick the SAME speed
  (see [`geoscape-timescaling.md`](geoscape-timescaling.md)) — the driver sets
  it on host+client together and lets the real timers run.
- `dismiss_popup` — confirm/close the TOP popup; reports the typeid and errors
  on unknown types so new popups surface instead of hanging. Handled:
  `GeoscapeEventState` (btnOkClick), any `ArticleState` subclass (popState —
  btnOkClick is protected), `MonthlyReportState` (btnOkClick),
  `MissionDetectedState` (btnCancelClick = skip; site stays on the globe),
  `NextTurnState` (popState), `AbortMissionState` (btnOkClick = confirm abort),
  `DebriefingState` (btnOkClick).
- `craft_dispatch {site_id, soldiers}` — assign up to N unassigned soldiers to
  the own base's first craft (`Soldier::setCraft`) and
  `Craft::setDestination(site)`; the geoscape flies it as time advances.
- `confirm_landing` — fire `ConfirmLandingState::btnYesClick` (host path pushes
  the CoopState(88) coop-battle init). **Never fire twice** — see gotchas.

Battlescape (added 2026-07-05):

- `close_briefing` — `BriefingState::btnOkClick`.
- `battle_inventory {action}` — pre-battle coop inventory: `autoequip_all`
  (cycles units through `InventoryState::onAutoequip` + `btnNextClick`; uses
  the engine's `BattlescapeGenerator::autoEquip` from the ground pile) and
  `ok` (btnOkClick → starts the tactical turn). Soldiers spawn UNARMED — craft
  items land on the ground tile (`BattlescapeGenerator` `_craftInventoryTile`).
- `battle_state` — `inBattle`, `turn`, `side`, `missionType`,
  `coopTurn` (= `BattlescapeGame::isYourTurn`; **2 = my active turn**, see
  memory battlescape-coop-turn-states), `selectedId`, `spotted[]` (union of
  hostile ids visible to my units via `getVisibleUnits()` — drive targeting
  from this, not omniscience), and `units[]` with `id, faction (0=PLAYER,
  1=HOSTILE, 2=NEUTRAL), status, isOut, health, tu, stun, name, weapon
  (getMainHandWeapon), isPlayerSoldier, murdererId, killedBy, x, y, z`.
- `battle_action {action, ...}` — all main-thread, race-free:
  - `select {unit}` — `setSelectedUnit`.
  - `move {unit,x,y,z}` — BA_WALK: sets `_currentAction`, runs
    `Pathfinding::calculate`, pushes `UnitWalkBState`; errors `no path to target`.
  - `shoot {unit,target,mode}` — mode snap|aimed|auto; sets actor/weapon/type/
    targeting on `getCurrentAction()`, `updateTU()` (returns `tuCost`/`tuHave`),
    pushes `UnitTurnBState` + `ProjectileFlyBState`. Replicates to the peer
    because coop syncs at combat-event level (`unit_fire`/`hit_tile`/
    `hasHitUnit`/`hit_unit` packets fire from inside the BStates).
  - `end_turn` — `requestEndTurn(false)`.
  - `abort` — `BattlescapeState::btnAbortClick` (opens AbortMissionState; the
    driver confirms it via `dismiss_popup`). This is the ONLY abort that ends
    the mission — `setAborted(true)+requestEndTurn` alone never calls
    `finishBattle` and loops forever.

Menu / rendezvous combobox (added 2026-07-07):

- `open_server_browser` — push the coop `ServerList` state (needs a SavedGame;
  bootstrap via `open_new_game`/`newgame_ok`/`place_first_base` first).
- `server_combo` — dump the rendezvous-server `DisableableComboBox`:
  `{visible, selected, options[{label, enabled}]}`. Offline servers read
  `"<name> (offline)"` with `enabled=false`; still-probing ones read
  `"<name> (Wait...)"`. Poll until no label contains `(Wait...)`.
- `screenshot {path}` — save the current frame to a PNG (`Screen::screenshot`)
  for visual inspection; works even on a minimized test window.
- Driver: `tools/coop_test/test_server_browser.py` — one instance, opens the
  browser, waits for probes, asserts combobox visibility + an offline/disabled
  server, and saves `server_browser.png`.

Add commands by extending `TestServer::execute`.

## Autonomous play driver — `tools/coop_test/play_harness.py`

Layers on harness.py; actions: `python tools/coop_test/play_harness.py <action> [budget_s]`

- `bringup` — 2-player XCF coop session to live geoscape + snapshot + backup.
- `advance [s]` — lockstep time advance, auto-dismissing known popups.
- `play_month [s]` — geoscape-only month traversal, skipping all sites,
  enumerating the mission census + daily coop cross-validation.
- `dispatch` / `battle [s]` — advance to the first ENGAGE cult site, dispatch
  both crafts, coop landing, equip, fight, extract.
- `killtest [s]` — aggressive fight (advance-to-contact, up to 10 turns,
  focus-fire) to force kills and stress kill/death replication.
- `campaign [s]` — the full month-1 goal loop: fight every ENGAGE site, skip
  AVOID monsters, backup after each battle + each day, stop at Feb 1 or >2
  losses; prints a final report (missions, roster deltas, bugs).

Key helpers: `activate_xcf(dir)` (flips x-com-files active:true in the isolated
dir only — the real options.cfg stays vanilla), `Session.cross_validate`
(host/client mission-site set equality with a 2.5s re-sample to filter
detection-timing skew), `cross_validate_battle` (per-unit faction+isOut
equality, 1.5s re-sample; `status`/mid-combat HP are transient animation state
— do NOT compare), `validate_kill` (murdererId/killedBy mirror check),
`classify_site` (`AVOID_RACE`/`ENGAGE_HINTS` constants — which mission-site
races/types are safe to engage vs skip), `roster_count` (loss tracking),
`backup_both(label)` → `tools/coop_test/playthrough_backups/`,
`keep_awake()` (SetThreadExecutionState — a screen lock kills the SDL context
of background instances → std::terminate).

## JOINT campaign testing (PRD-J01..J11, 2026-07-17)

A JOINT campaign is ONE host-authoritative world; clients hold replicas whose
simulation is frozen and whose every mutation is a `joint_cmd` to the host.
Testing it needs a different bring-up and a much stronger end assertion than the
SEPARATE suite's "the two mirrors agree".

### `tools/coop_test/joint_fixture.py` — use this, don't hand-roll

```python
js = joint_fixture.bring_up("jbuy", (48670, 48671, 47970))   # host/client/coop ports
host, client = js.host, js.client
try:
    ...
    js.finish()          # world equality + the replica's zero-disk invariant
finally:
    js.shutdown()
```

- `bring_up(tag, ports)` — host creates a JOINT campaign, client joins, the host
  streams the authoritative world, both settle on the geoscape
  (`session.new_campaign(campaign_mode="joint")` + `geo.wait_both_ready`).
  Cleans up its own processes if bring-up throws. `tag` names the two isolated
  user dirs (`jbuy_host` / `jbuy_client`).
- `assert_world_equal(host, client, tag)` / `world_diff` / `world_dump` — the
  JOINT promise asserted: a deep compare of both machines' introspection dumps.
  **Wired into every JOINT test's final state**, so new tests get it free.
  Equality is EVENTUAL (it polls) — an in-flight `joint_apply` is a legitimate
  transient skew, the same reason the J10 desync detector debounces 3s.
- Covers: funds, campaignType, monthsPassed, discovered tech, and per base (IN
  INDEX ORDER — the index is the protocol's routing key) name/coords/coopBaseId,
  facility grid, stores, transfers, research(+assigned), productions(+engineers),
  craft identity+status, free personnel, and the soldier roster with
  `ownerPlayerId`. Deliberately WIDER than the J04/J10 world checksum, which is
  only funds + base count + tech count.
- Known-volatile, excluded **with reasons** (see the module docstring): the
  clock; craft kinematics (lon/lat/fuel/damage/dest — snapshot-driven + the J08
  `_dest`-lag gap); transfer `hours` (a frozen replica never `advance()`s a
  Transfer); research `spent` / production `timeSpent` (day-granular `day_tick`);
  UFOs + mission sites (snapshot best-effort: despawn is hide-not-delete and
  `secondsRemaining` is forced to a sentinel).

`test_joint_world_equal.py` is the helper's own test, including a **negative
control** (a knowingly divergent world MUST raise) — without it every "PASS world
equality" line in the suite would be unfalsifiable.

### JOINT introspection (TestServer)

- `geo_state` gained, for the fixture's dump: top-level `campaignType`,
  `discoveredResearch[]`; per base `coopBaseId`, `items{type:qty}` (stores),
  `transfers[{type,rule,qty,hours}]`. Existing: facilities(+grid+sizes),
  research(+assigned), productions(+engineers/timeSpent), crafts
  (status/fuel/damage/dest/displayStatus), free sci/eng/lab/workshop.
- `get_soldiers` — per-base roster with `owner` (= `ownerPlayerId`), `craftId`.
- `joint_checksum` — this machine's world checksum exactly as the host stamps it
  on the `time` heartbeat. Widened by GAP-4 to 7 fields: `chkFunds`/`chkBases`/
  `chkResearch`/`chkItems`/`chkSoldiers`/`chkTransfers`/`chkProduction` (integer
  aggregates; income/expenditure SERIES deliberately excluded — GAP-9). Makes
  store/roster/production drift visible without waiting for a heartbeat.
- `joint_stats` / `joint_reset_stats` (cmd/ok/fail/apply/unknown + `lastFail`),
  `joint_resync_stats` / `joint_reset_resync_stats`, `force_resync`,
  `joint_cmd {jcmd,baseId,payload}` (raw submission, for host-validator races).
- Command drivers all drive the REAL screens: `buy`, `sell`, `joint_transfer`,
  `containment`, `research_start`, `manufacture_start`, `fac_build`,
  `fac_dismantle`, `sack`, `base_rename`, `build_new_base`, `craft_order`,
  `craft_assign`, `dogfight_action`, `open_screen`/`close_screens`/`screen_state`.
- Per-machine SCAFFOLDING (not production paths) — **call on BOTH machines** or
  you manufacture a desync: `give_items`, `add_base`, `spawn_craft`,
  `set_soldier_owner`, `discover_research`, `set_funds`.

**New TestServer hooks go in the newest shallow sub-dispatcher — now
`TestServer::executeJoint11`** (`executeJoint10` filled up), NOT the main
`execute` if/else chain: each `else if` nests a block and the chain sits at MSVC's
~128-block limit (C1061). The 2026-07-17 origin rebase re-tripped C1061 by stacking
upstream commands on the near-limit chain; fixed by carving `executeJoint11`
(commit `0793c23aa`). When `executeJoint11` nears the limit, carve `executeJoint12`
the same way. See `dev-workflow.md`.

### JOINT traps (each one cost a session)

- **`set_funds` on ONE machine is a real desync.** Funds are a checksum field, so
  J10 auto-repairs it ~3s later by restreaming the world — which wipes any open
  screen and overwrites whatever "unchanged" invariant you were asserting. Same
  for any checksum field (funds / base count / discovered tech).
- **Any automatic restream MUST release the client's resume hold.**
  `LoadGameState` parks every client that adopts a streamed world in
  `COOP_DLG_CLIENT_RESUME_HOLD` (68) until a `campaign_begun` arrives, and
  mid-session nobody clicks BEGIN. Two triggers exist
  (`jointPostBattleRestream`, `jointResyncRestream`) and ONE release site (the
  `resume_ack` handler). A third restream must arm an auto-release flag. This is
  the single most repeated trap in the PRD set.
- **The `time` heartbeat is a ~2 kHz lane** (a conflation slot drained every send
  loop). Anything per-packet there is a main-thread tax — a mismatch log firehose
  once starved the very restream it triggered. Log once per episode.
- **A checksum mismatch alone is NOT desync evidence**: the checksum and the
  `joint_apply` that moves it are separate packets, so a normal buy shows a
  one-frame skew. Hence `RESYNC_DEBOUNCE_MS = 3000`.
- **A popup on EITHER machine stalls the shared clock** — coop only advances
  while both players sit on the geoscape at the same speed. Any test that flies a
  craft after month 1 must drain popups on BOTH sides (see
  `test_joint_month_run._prompt`), but never dismiss `ConfirmLandingState`:
  `dismiss_popup`/`geo_run` auto-DECLINE it.
- **The starting Skyranger is FULL** — a naive "assign 2 soldiers" gets the
  correct `STR_NOT_ENOUGH_CRAFT_SPACE`. Empty the craft first.
- **Funds labels use a non-ASCII thousands separator** (`Unicode::formatFunding`)
  — strip non-digits before comparing.

### Suite map (test → PRD → what it proves)

| test | PRD | proves |
|---|---|---|
| `test_joint_flag.py` | J01 | campaignType JOINT end-to-end + `coopCampaignType` YAML round-trip |
| `test_joint_bootstrap.py` | J02 | client adopts the streamed replica (same base/funds), no mirror bases, replica save refused, no `coopClientSaves` embed |
| `test_joint_resume.py` | J02 | save → reload → rejoin re-streams the world; replica matches whole |
| `test_joint_purchase.py` | J03 | the `joint_cmd`/`joint_apply` protocol: client+host buy, insufficient funds → `joint_fail`, unknown cmd → no crash |
| `test_joint_sim.py` | J04 | host-only sim: research/facility completion mirrors, replica construction frozen, month-end funds/maintenance sync, craft position snapshot |
| `test_joint_commerce.py` | J05 | sell / hire(+owner+identical stats) / cross-base transfer / containment; overlapping sell → atomic `joint_fail` |
| `test_joint_research.py` | J06 | `res_start`/`res_alloc`/`res_cancel` + the two-players-one-project race |
| `test_joint_manufacture.py` | J06 | `man_start`/`man_alloc`/`man_cancel`, funds+material debit, completion |
| `test_joint_facilities.py` | J07 | `fac_build`/`fac_dismantle`, host-validator tile-conflict race, sack |
| `test_joint_newbase.py` | J07 | atomic `base_new`: same coopbaseid/name/lift/index, single debit; index lock-step |
| `test_joint_craft.py` | J08 | shared craft launch/retarget/return/patrol + interception; the host sims the dogfight, both machines spectate |
| `test_joint_deploy.py` | J09 | `craft_assign` both ways (drives the harness cmd, not the screen). NB: J09's "full roster visible to both" was later found to be a MISTAKE — see the 2026-07-20 ownership-parity tests below; each player now sees only their OWN soldiers |
| `test_joint_battle.py` | J09 | mixed + solo-client squads: control split = ownership on both machines; post-battle worlds identical; the SEPARATE guest cleanup must NOT delete client-owned soldiers |
| `test_joint_landing.py` | J10 | the landing broker: the commanding seat gets the prompt; host doesn't re-ask |
| `test_joint_refresh.py` | J10 | 11 screens live-refresh on `joint_apply` (asserted against ctor-time caches) |
| `test_joint_resync.py` | J10 | desync detect → auto-restream → repaired AND released from hold; `force_resync` |
| `test_joint_world_equal.py` | J11 | the equality helper itself + negative control; stores drift is invisible to the checksum; `force_resync` heals past it |
| `test_joint_disconnect.py` | J11 | client hard-killed with a cmd in flight → host self-consistent (no half-apply); rejoin re-streams; world equal; replica live again |
| `test_joint_month_run.py` | J11 | the long run: 2 month ends + a mixed battle in one campaign; equality after every phase; the restream preserves running projects and doesn't break the next settlement |

**Gap-hardening (GAP-1..9, 2026-07-17).** Root-cause fixes found by playtesting
the J01-J11 build; see `joint-campaign.md` for the outcomes.

| test | gap | proves |
|---|---|---|
| `test_joint_base_defense.py` | GAP-1 | base defense stamps `_coop` from `ownerPlayerId` on both machines (unfenced it erased client-owned soldiers via the SEPARATE merge); needs a spare 2nd base so the attacked base's loss isn't game-over |
| `test_joint_checksum.py` | GAP-4 | the widened checksum (chkItems…) catches a replica-only store drift the old 3-field checksum missed → auto-resync heals it |
| `test_joint_equip.py` | GAP-5 | `craft_equip` (items base↔craft) is host-authoritative; ungated it drifted `chkItems` |
| `test_joint_equip2.py` | GAP-5b | the sibling equip screens — `craft_rearm` (weapon mount) + `soldier_armor` (SoldierArmor + CraftArmor) — same fix shape |
| `test_joint_production_sell.py` | GAP-6b | a sell-flagged production credits funds and adds NO items on the replica (the `prod_done` payload carries the `sell` flag) |
| `test_joint_graphs.py` | GAP-9 | income/expenditure SERIES are host-authoritative (`setFundsRaw` + tails in `joint_apply`); funds were always exact, only the Graphs series drifted |

(GAP-6 overshoot is folded into `test_joint_manufacture.py`; GAP-2/7/8 are the
dogfight regressions below.)

**Shared / replicated JOINT dogfights (PRD-DF01..DF03).** The host sims EVERY
dogfight and streams a per-tick `df_state`; every player opens a render-only
`DogfightState` and can command it (`df_cmd`, host arbitrates receive-order);
membership rides `df_open` with a monotonic `membershipEpoch` guarding the
HK-reshuffle race. Dissolves GAP-2, closes GAP-7/8.

| test | PRD | proves |
|---|---|---|
| `test_joint_hk_dogfight.py` | DF01 | GAP-2 dissolved: an HK on a client-commanded craft opens the SAME fight on both machines; outcome mirrored |
| `test_joint_intercept_spectate.py` | DF01 | non-HK intercept spectated on both; ONE authoritative crash (status+crashId), world-equal |
| `test_joint_dogfight_control.py` | DF02 | stance/weapon/disengage via `df_cmd`, host arbitrates last-received-wins; synced `ufoStance` marker; client-local minimize |
| `test_joint_dogfight_present.py` | DF02 | presentation policy: host-commanded fight FULL on host / minimized icon on client; a minimized client still commands |
| `test_joint_dogfight_xp.py` | DF03 | GAP-7: pilot dogfight XP host-authoritative — EQUAL on both, on the host Soldier, riding the roster stream; replica refused the award + never accrues locally |
| `test_joint_dogfight_dest.py` | DF03 | GAP-8: after auto-return the replica craft's `_dest`/status match the host — no lag, no orphan waypoint |
| `test_joint_dogfight_concurrent.py` | DF03 | 4 concurrent intercepts = one membership set on both; HK interrupt-all-and-restart converges both to the new all-HK set atomically (epoch lock-step, no stale window, no old-epoch `df_state`); per-(craft,ufo) targeting |

New/changed harness surface (DF03, all read-only or host-only test hooks):
- `dogfight_state` per-fight fields extended to `{craftId, ufoId, currentDist,
  targetDist, mode, ufoIsAttacking, minimized, ended, isReplicaView, epoch,
  ufoStance, weaponEnabled[], projectileCount}` + the machine's membership epoch;
  `dogfight_action` takes optional `(craft_id, ufo_id)` to target a specific fight
  (its body is now `doDogfightAction()`/`resolveDogfight()` free helpers so the
  deep `execute()` chain stays under the MSVC C1061 nesting limit).
- New hooks: `award_dogfight_xp` (HOST-only; deterministic dogfight-XP award to a
  live fight's crew — the vanilla ruleset defines no craft `pilots`/`dogfight-
  Experience`, so the real RNG path awards nothing, and this exercises the GAP-7
  *propagation* invariant), `assign_crew` (seat a soldier on a craft; call on BOTH
  machines, the `spawn_craft` idiom). `GeoscapeState::harnessDogfightEpoch()`
  returns host `_dfEpoch` / replica `_dfReplicaEpoch`; `DogfightState::
  harnessAwardPilotXp` + `harnessProjectileCount`. No `df_frame` hook was needed —
  the extended `dogfight_state` is enough for frame-agreement assertions.

Gotchas hit (for the next dogfight test author):
- **craft ids are per-TYPE** — a Skyranger and an Avenger can both be id 1. Match
  `(id, type)` everywhere (a soldier seated on "craft 1" landed on the Skyranger,
  not the Avenger, until `assign_crew`/`_craft()` matched the type too).
- **a non-minimized dogfight PAUSES the geoscape clock** (vanilla). To open N
  concurrent fights you must minimize each as it opens so the world keeps
  advancing and the next craft can arrive; once all N are minimized the host clock
  runs (all-minimized) and, via `df_state.hostAnyOpen`, the client's too.

### Playtest-fix tests — 2026-07-20 (branch `fix/joint-playtest-bugs`)

Round-1/2 + soldier-ownership parity. All red-first + red-proofed (temporarily
disable the fix, rebuild, confirm the test goes red, restore).

| test | proves |
| --- | --- |
| `test_joint_facility_refresh.py` | B1: base screen behind the BuildFacilities popup live-refreshes on a peer `fac_build` (reads `BasescapeState::harnessFundsText`) |
| `test_joint_research_refresh.py` | B2: a started topic drops from the open NEW RESEARCH PROJECTS list live (`screen_state.projects`) |
| `test_joint_soldier_rename.py` | B3: `soldier_rename` joint_cmd — client + host renames land on both |
| `test_joint_soldier_ownership.py` | B4: bootstrap owner split even (0/1) + mirrored |
| `test_joint_ufo_alert.py` | B5: host detection alerts ALL seats (`ufo_alert` builds the UfoDetectedState → broadcast) |
| `test_joint_dogfight_highlight.py` | B6: replica stance-button HIGHLIGHT follows the host (`dogfight_state.highlight`) |
| `test_joint_ingame_coop_menu.py` | B7 + host name: in-game coop menu shows RESUME GAME (both seats) and returns to the running geoscape; `lobby_state.hostRowName/clientRowName` role-correct |
| `test_joint_dogfight_shared.py` | R2: dogfight window FULL for all (host- & client-commanded), buttons host-authoritative (no revert), commands apply in host order, `highlight==mode` invariant |
| `test_joint_soldier_visibility.py` | roster (SoldiersState) shows own-only (`screen_state.displayed`) |
| `test_joint_craft_soldiers_visibility.py` | craft-crew assign shows own-only |
| `test_joint_soldier_views_visibility.py` | soldiers + craft_soldiers + craft_armor all own-only |
| `test_joint_gift.py` | `soldier_gift` joint_cmd — gift moves owner on BOTH machines + moves the soldier between the players' lists |
| `test_joint_soldier_ownership_battle.py` | bootstrap split → BattleUnit `_coop` split on both (guards battle req #3); + on-load migration (`reload_save_roundtrip`) heals a 999 save |

New harness surface (2026-07-20):
- `screen_state.displayed` — soldier ids a list screen actually shows (own-only in
  JOINT): SoldiersState/CraftSoldiersState/CraftArmorState `harnessDisplayedSoldierIds`.
- `open_screen` added: `basescape`, `build_facilities`, `new_research`, `craft_armor`.
- `dogfight_state.highlight` (the inverted stance button, 0..4) alongside `mode`.
- `lobby_state.hostRowName/clientRowName` (by player id, unsorted — the roster is
  name-sorted so row order ≠ id order); `open_coop_menu` (pushes LobbyMenu),
  `lobby_action` (clicks the action button).
- `ufo_alert` (host builds a UfoDetectedState → its ctor broadcasts `ufo_popup`).
- `reload_save_roundtrip` (save → load into a throwaway blank SavedGame → report the
  loaded roster's owners; exercises the on-load ownership migration without disturbing
  the live session).
- `fac_build` / `soldier_rename` drive the REAL PlaceFacility / SoldierInfo paths.
- **Build gotcha:** a stray `OpenXcom.exe` (a killed harness game) or `mspdbsrv`
  holds the `.obj`/PDB → `error C1083 ... permission denied` / `D8040` — kill
  `OpenXcom,cl,link,mspdbsrv,vctip` and rebuild (usually the 2nd try links).
- **the HK reshuffle IS real** — `GeoscapeState::time5Seconds`
  `Collections::deleteAll(_dogfights)` + rebuild around the HK (main + escorts in
  `escortRange`, `escortsJoinFightAgainstHK` defaults true). The rebuilt HK fight
  actively resolves, so assert the ATOMIC-CONVERGENCE snapshot (all-HK set equal
  on both, epoch lock-step & advanced), not a frozen set.
- **`force_resync` briefly re-reconciles the replica window** — drive a clean fight
  end from the host (down the UFO) rather than a client `df_cmd` right after it.

**The SEPARATE matrix is the legacy suite itself.** PRD-J11 floated a
`test_joint_separate_matrix.py`; there is no point wrapping it. Every JOINT
behaviour is gated on `isJointCampaign()`, so "SEPARATE still works on a build
with all the JOINT code in it" is exactly what the 24 legacy tests assert when
they run green against the final binary. Run the whole suite, not a wrapper.

### Runtime + CI budget (re-measured 2026-07-17, post gap-hardening + dogfights)

Full suite = `boot_check.py` + every `test_*.py` = **56 tests** (24 legacy + 32
JOINT), serial. Full timed run: **16.1 min total, all pass** (2 flakes auto-retried
once). Distribution is healthy — median ~13 s; most tests are dominated by the
~5-6 s two-process bring-up.

**No test threatens a per-test timeout.** Slowest (all explained, none
pathological):

| ~time | test | why |
|---|---|---|
| 76 s | `test_session_hardening` (legacy) | waits out real reconnect/timeout windows |
| 65 s | `test_joint_dogfight_concurrent` | genuinely spins 4 concurrent dogfights + HK reshuffle |
| 56 s | `test_joint_manufacture` | inflated by the retry (single-pass ~28 s) |
| 35 s | `test_joint_craft` | multi-phase craft command + dogfight |
| 25-29 s | craft/battle/landing/sim/dogfight cluster | multi-phase, real 2-process flows |

The JOINT long-run (`test_joint_month_run`, 2 month ends + a battle) is only ~19 s
— geoscape skips are bounded by IN-GAME time.

Known flakes, **retry once** before digging: `test_ufo_notice` (RNG detection
timing), `test_joint_manufacture` (host production-completion timing),
`test_joint_commerce` (transfer step, under load), and under back-to-back load
`test_joint_disconnect` / `test_joint_resync` (dialog-68 hold-release timing). All
are timing/load-related, not JOINT-logic failures — a stabilization pass someday
would tighten CI budget but nothing blocks.

## The session bootstrap dance (what the tests replicate)

1. host: `load_save` (or `open_new_game` → `newgame_ok` → `place_first_base`) →
   `host_tcp` (pushes LobbyMenu; campaign = countries non-empty)
2. client: `join_tcp` → Profile splash on both → `profile_ok` both
3. client: `newgame_ok` (difficulty) → `place_first_base` (its own linked
   campaign; the name-confirm sends `close_load_progress`) → LobbyMenu
4. both `lobby_ready` (ready toggle) → wait `sessionLocked` → both
   `lobby_ready` again (start) → wait `lobbyClosed && hasSave` on client
5. session live: `coopStatic && coopCampaign && sessionLocked && lobbyClosed`

## Gotchas learned the hard way

- `SDLNet_Init()` must be called in the server thread (nothing else has).
- `SDLNet_ResolveHost(NULL, port)` = listen; a hostname = outbound connect.
- **Fresh saves number soldiers from 1 and can roll fully identical rosters**
  (same RNG seed → same names). Never dedup transfers by name/roster; in tests,
  `rename_soldier` a subject to a unique name before asserting by name/count.
- 30s lobby countdown auto-locks when both ready; a second lobby click starts.
- `client_reload_progress` is how a test simulates the "host reloads a save,
  client reconnects" flow — the client re-fetches its world from the host, so
  after a host `load_save` the client reflects that save's rosters.
- Verifying a *fresh* client-world push landed on the host: `has_coop_file`
  only proves the key exists (a stale blob passes too) — pair it with a short
  sleep, or assert on the resulting roster after a reload.

## Gotchas — geoscape/battle driving (2026-07-05)

- **confirm_landing exactly once per side, only while ConfirmLandingState is the
  TOP state.** After the first confirm, CoopState(88) sits on top but
  ConfirmLandingState is still in the stack — a stack-wide check re-fires
  btnYesClick, coop battle init runs twice, `std::terminate`.
- **CoopState is a WAIT dialog, not a decision** (month save-progress sync, map
  download; int-keyed). Poll for auto-close; never dismiss.
- **Sites despawn in hours-days and IDs recycle** — dispatch promptly; don't
  key logs by id alone.
- Coop battle model: ONE shared squad from the initiating side's craft
  (Civilian Car = 2 soldiers), identical on both machines; enemy ids start at
  1000000. Whichever machine has `coopTurn==2` acts; turns alternate via the
  endTurn packet.
- **Extraction strategy** (kept the month-1 campaign at 0 losses): hold the
  craft/exit zone, fire at spotted hostiles, abort within ~2 turns — living
  units in the exit zone are recovered. Holding longer gets rookies charged
  (battle6 lost one to melee by turn 7); XCF "cult apprehension" can spawn
  hp-150 Shambler Raiders that 2 rookies cannot kill.
- **Screen lock kills background instances** (`std::terminate`, empty
  crashlog) — call `keep_awake()`; shutdown-time terminate pairs in
  `bin/x64/Release/crashlogs/` are benign (unjoined net thread on exit).
- **Windows cp1252 stdout**: XCF agent names contain non-Latin1 chars (`ś`) —
  `sys.stdout.reconfigure(encoding="utf-8")` or redirected runs crash.
- Run long drivers with `python -u` + `tail -f` the log; MSBuild link fails
  with LNK1104 while any OpenXcom.exe instance is alive.
- **Replication gaps found by killtest**: (FIXED, verified) kill attribution
  desynced to the client — `hit_unit` fires mid-hit, before
  `checkForCasualties` assigns `killedBy`, so the client credited murderer 0 /
  faction HOSTILE. Fix: `hit_unit` now carries `murdererId`+`killedBy`
  (`TileEngine.cpp` ~3325) and an authoritative post-death `kill_attrib`
  packet re-applies both from `checkForCasualties`
  (`BattlescapeGame.cpp` ~1679 → receiver `connectionTCP.cpp` ~4340).
  (OPEN) under rapid multi-hit fire the `unit_death` packet can be dropped —
  it is only consumed while `_coop_task_completed` or `_coopInitDeath`
  (a live client `ProjectileFlyBState`) holds, so a death can sit in the hold
  queue forever, leaving a 0-HP-but-alive unit on the client until debrief.
  Doesn't reproduce in cautious play; host save stays authoritative.
