# Coop test harness

Autonomous, two-client testing for the coop mod. A small command server built
into the game lets Python drivers spawn real game instances, drive them through
menus / saves / lobby / geoscape / battle, read live state, and assert that host
and client stay in sync. No human input, no external mod, no pre-existing save.

## Requirements

- A built `bin/x64/Release/OpenXcom.exe` (see the top-level build docs).
- Python 3.8+ on Windows (the driver uses `subprocess` window placement and
  spawns the Windows exe; the in-game server itself is portable).

## Quick start

```
python tools/coop_test/boot_check.py          # single-instance smoke test
python tools/coop_test/test_geoscape_sync.py  # two instances, geoscape sync check
```

Each driver spawns its own game window(s), runs, and tears them down. Windows
are small (640x400) and never grab the mouse, so you can keep working while a
test runs.

## How it works

- In-game server: `src/CoopMod/TestServer.{h,cpp}`. It starts only when the
  `OXC_TEST_PORT` env var is set, so it is inert in normal play. It listens on
  `127.0.0.1:<port>` and speaks newline-delimited JSON.
- Threading: a socket IO thread does I/O only and queues commands into a
  mutex-guarded inbox. A per-frame pump in `Game::run()` drains the inbox on the
  main thread, runs the command against the real game objects, and queues the
  JSON reply. All game-state access is therefore race-free.
- Handlers invoke the real `State` methods (`Profile::buttonOK`,
  `BuildNewBaseState::placeAt`, `UnitWalkBState`, ...) rather than faking SDL
  input, so a command exercises the same path a human click would.
- Driver side: `harness.py` provides `GameClient` (spawn + socket +
  `cmd`/`ok`/`wait_for`) and `make_user_dir` (a hermetic, isolated user folder).

```
Python driver  --JSON+newline-->  TestServer IO thread  --inbox(mutex)-->
  pump() (main thread)  -->  execute()  -->  real game objects  -->  outbox  --reply-->
```

Two game instances also connect to each other over the real coop link
(`connectionTCP`); that link is what the tests actually exercise.

## Isolation (runs on any machine)

`make_user_dir()` writes a fresh minimal `options.cfg` (see `HERMETIC_OPTIONS`)
that pins the stock `xcom1` master with no external mods, intro/audio/mouse
capture off, and a 640x400 window. OpenXcom defaults every other key and
resolves data (`UFO`/`TFTD`/`standard`/`common`) from the exe's own directory.
No local config is read and no save is needed: the tests bootstrap a brand-new
campaign each run.

## Tests

- `boot_check.py` - single-instance install smoke test.
- `test_geoscape_sync.py` - two instances; geoscape host/client sync check.
- `test_gift_fresh.py` - gifting a soldier (ownership change) on a fresh campaign.
- `test_bug_fixes.py` - owner resolution, notice display, dialog flicker, etc.
  Exposes `bootstrap_fresh_session()` and `own_base()` reused by other tests.
- `test_gift_rollback.py` - host-save-is-authority rollback of a gift, both directions.
- `test_server_browser.py` - rendezvous-server combobox state (offline path).
- `test_ufo_notice.py` - when one player detects a UFO, the peer gets the notice
  too (both `UfoDetectedState` popups); checks both directions.
- `test_client_zero_disk.py` - host-save authority: after a full session with
  aggressive autosaves and host/client saves through the real save funnel, the
  client's user dir contains zero `.sav`/`.asav`/`.data` files (and the host
  dir no `.data` sidecars - world blobs live in memory + the host .sav embed).
- `test_new_campaign_flow.py` - redesigned campaign flow e2e: Solo/Co-op New
  Game split (D1 solo-hosting refusal), host-driven lobby + START CAMPAIGN,
  parallel base building, coop markers + locked player list.
- `test_resume_flow.py` - resume e2e: menu load -> host window -> resume
  lobby, wrong-name refusal, registered client with an empty user dir gets its
  world back (roster + markers intact).
- `test_rejoin_flow.py` - mid-session hard-kill -> host freeze dialog ->
  direct rejoin (no lobby) -> RESUME; roster intact, zero-disk.
- `test_coop_base_equip_visibility.py` - issue #33: items a player reserved by
  equipping soldiers at a base must not appear as free/available equipment to a
  peer visiting that base, nor to the owner when the reserving soldier is a
  stripped co-op guest; checks all four host/client directions.
- `test_coop_transferred_equipment.py` - two scenarios for a vanilla-transferred
  equipped soldier. (1) conservation: the transfer must not duplicate the gear -
  the world-wide item total is unchanged and the client-base equip screen shows
  the same number of item instances to the visiting host as to the client.
  (2) the two equip screens must agree: with "Alternate craft equipment
  management" ON, a soldier equipped with a weapon in each hand + rockets in the
  backpack is transferred to the peer base and seated on the peer's craft; the
  co-op trade confirmation must not fail (see below), and
  `Bases > Soldiers > Inventory` and `Bases > Equip craft > ... > Inventory` must
  then show the same items in the same slots on the soldier AND the same free
  items on the ground pane.

  The confirmation check is the regression guard for the double-book bug:
  `TransferItemsState::completeTransfer()` (the immediate, non-ACK-gated path)
  applies the funds debit, the source-store removal and the co-op limit
  decrements locally, then sends the same `"transfer"` packet the ACK-gated
  `createPendingTransfers()` sends. The peer's `transfer_completed` used to
  re-apply all of it via `Base::removePendingTransfers()`, whose store
  re-validation then found nothing left to remove and failed outright - CoopState
  552 "Failed to remove items from your base." on the sender while the receiver
  had already added the goods, with the pending-transfer list never cleared. The
  packet now carries `already_applied`, echoed back in the ACK, and the sender
  only clears its pending list for that path.
- `test_coop_transfer_equipment_option.py` - the "Alternate craft equipment
  management" option drives transfer behaviour: OFF strips the soldier's gear
  (empty layout at the peer base), ON keeps its loadout. Runs both modes.
- `test_coop_quarters_accounting.py` - the base that HOUSES a soldier is the one
  that pays for it. A transfer keeps the soldier in the SENDER's roster (tagged
  `coopBase` = the destination) and gives the receiver nothing at all
  (`Base::syncTrade` drops an incoming TRANSFER_SOLDIER because `soldier_rule`
  is never written), so the guest used to occupy nobody's living quarters.
  `Base::getUsedQuarters()` now skips soldiers with `coopBase != -1` on a real
  own base and adds `coop_guests`, which `connectionTCP::sendGuestCensus()`
  reports to the peer. Moves THREE soldiers each way - one transfer would not
  have caught the roster being pruned between moves.
- `test_coop_peer_equip_screens.py` - a transferred soldier must keep its full
  loadout on BOTH equip screens at a peer's co-op base. `deployXCOM` numbered
  item `coopID`s only in its craft branch, so base-inventory items all kept
  `coopID 0`; the craft's `coopItems` manifest (written from that same screen)
  was all-zero too, and `BattleUnit::hasCoopItem` collapsed into a TYPE-only
  match - `placeItemByLayout` then refused a visitor's OWN weapon whenever the
  peer had ever equipped that item type on that craft. Asserts (a) no deployed
  soldier's layout items are left on the ground, (b) both screens put the same
  items in the same slots, (c) both hold the same item instances (`all`), and
  checks all of it at a plain own base first, so a coop-only divergence fails
  while stock behaviour is left alone. Deliberately does NOT assert ground-pane
  equality - the two screens deploy different unit sets (all base soldiers vs.
  the craft crew) and loose weapons auto-chamber ammo, both of which reproduce
  with no coop base in sight.
- `test_coop_guest_craft_seat.py` - taking a guest soldier OFF a craft at a
  peer's co-op base must stick across a trip to the geoscape. `Soldier::
  _coopCraft` is the persisted guest seat and a co-op base is rebuilt from it on
  every entry, but `SoldiersState`/`CraftInfoState`/`CoopState` only ever wrote
  it inside `if (soldier->getCraft())` - so an unassign left a stale craft id and
  the next rebuild re-seated the soldier. Runs both directions (host visiting the
  client's base and vice versa).
- `test_coop_transfer_equipment_counts.py` - with the option ON the gear's item
  counts actually move, immediately (0-hour transfer, matching the instant
  soldier move): the sending base's stored count drops and the receiving base's
  stored count rises by the same amount, world total conserved. OFF moves
  nothing.

### JOINT campaign tests (PRD-J01..J11)

A JOINT campaign is ONE host-authoritative world shared by both players (bases,
funds, research, manufacture, crafts), each player keeping control of their own
soldiers in battle. SEPARATE (the classic two-mirrored-economies mode) is
unchanged and is what every test above still exercises.

**Use `joint_fixture.py`** - do not hand-roll the bring-up:

```python
js = joint_fixture.bring_up("jbuy", (48670, 48671, 47970))  # host/client/coop ports
host, client = js.host, js.client
try:
    ...
    js.finish()          # world equality + the replica's zero-disk invariant
finally:
    js.shutdown()
```

- `bring_up(tag, ports)` - host creates a JOINT campaign, client joins, the host
  streams the authoritative world, both settle on the geoscape. Cleans up its own
  processes if bring-up throws.
- `assert_world_equal(host, client, tag)` - **the JOINT promise, asserted**: a
  deep compare of both machines' introspection dumps (funds, tech, and per base
  in INDEX order: coords/coopBaseId/facilities/stores/transfers/research/
  productions/craft identity+status/free personnel/roster with `ownerPlayerId`).
  Wired into every JOINT test's final state. Polls, because an in-flight
  `joint_apply` is a legitimate transient skew. Known-volatile fields are excluded
  **with reasons** - see the module docstring before adding to it.

| test | proves |
|---|---|
| `test_joint_flag.py` | campaignType JOINT end-to-end + save YAML round-trip |
| `test_joint_bootstrap.py` | the client adopts the streamed replica; no mirror bases; replica saves refused |
| `test_joint_resume.py` | save -> reload -> rejoin re-streams the world |
| `test_joint_purchase.py` | the `joint_cmd`/`joint_apply` protocol + funds authority |
| `test_joint_sim.py` | host-only simulation; the replica's sim is frozen; month-end sync |
| `test_joint_commerce.py` | sell / hire / cross-base transfer / containment |
| `test_joint_research.py`, `test_joint_manufacture.py` | research + manufacture start/allocate/cancel, incl. the two-players-one-project race |
| `test_joint_facilities.py`, `test_joint_newbase.py` | facilities, dismantle, sack; atomic new-base creation + base-index lock-step |
| `test_joint_craft.py` | shared craft command + interception; the host sims the dogfight, both machines spectate the same fight |
| `test_joint_deploy.py`, `test_joint_battle.py` | mixed-owner squads: control split follows soldier ownership; post-battle worlds identical |
| `test_joint_landing.py` | the landing broker asks the seat that gave the order |
| `test_joint_refresh.py`, `test_joint_resync.py` | live screen refresh on apply; desync detect -> auto-repair |
| `test_shared_equip_transfer.py` | equipping + moving soldiers around a SHARED world: real drag-drop equipping, both equip screens agree, a craft seat sticks when cleared, a second base built, then an intra-world `transfer` shared_cmd - the soldier arrives at the right base **unassigned from any craft**, with its layout intact and its **owner unchanged** (only a gift changes ownership), identically on both machines |
| `test_joint_world_equal.py` | the equality helper itself, **including a negative control** |
| `test_joint_disconnect.py` | client killed with a command in flight -> no half-apply; rejoin restores one world |
| `test_joint_month_run.py` | the long run: 2 month ends + a battle in one campaign |

#### Shared / replicated JOINT dogfights (PRD-DF01..DF03)

The host simulates EVERY JOINT dogfight and streams a per-tick state frame
(`df_state`); every player opens a render-only `DogfightState` and can issue
stance / weapon / disengage commands (`df_cmd`, arbitrated host-side in
receive-order). Membership rides `df_open` with a monotonic `membershipEpoch`
that guards `df_state` against the HK-reshuffle out-of-order race. This dissolves
GAP-2 (no exclusive "flyer" to route an HK to) and closes GAP-7 / GAP-8 (XP and
`_dest`/waypoints are host-authoritative, since a replica's `update()`
early-returns before any sim/award code).

| test | proves |
|---|---|
| `test_joint_hk_dogfight.py` | GAP-2: an HK attack on a client-commanded craft opens the SAME fight on both machines (no host-only routing); outcome mirrored |
| `test_joint_intercept_spectate.py` | a regular (non-HK) intercept is spectated on both machines; ONE authoritative crash (status + crashId), world-equal |
| `test_joint_dogfight_control.py` | any player commands a shared fight: stance/weapon/disengage via `df_cmd`, host arbitrates receive-order (last-received-wins); synced UFO-stance marker; client-local minimize |
| `test_joint_dogfight_present.py` | presentation policy: a host-commanded fight opens FULL on the host / a minimized icon on the client, and a minimized client still commands the host |
| `test_joint_dogfight_xp.py` | GAP-7: pilot dogfight XP is host-authoritative - EQUAL on both machines, lands on the host `Soldier`, rides the roster stream; a replica is refused the award and never accrues XP locally |
| `test_joint_dogfight_dest.py` | GAP-8: after a dogfight ends in auto-return, the replica craft's `_dest`/status match the host - no lag, no orphan waypoint to the downed UFO (the replica never sets `_dest` locally) |
| `test_joint_dogfight_concurrent.py` | up to 4 concurrent interceptions hold ONE membership set on both machines; an HK interrupt-all-and-restart converges both to the new set atomically (epoch lock-step, no stale window, no old-epoch `df_state`); exercises per-(craft,ufo) command targeting |

Traps worth knowing before you write a JOINT test:

- Per-machine scaffolding (`give_items`, `add_base`, `spawn_craft`,
  `set_soldier_owner`, `discover_research`, `set_funds`) must be called on **both**
  machines or you manufacture a desync. `set_funds` on one machine is a *real*
  desync - funds are a checksum field, so it auto-repairs ~3 s later by
  restreaming the world, wiping any open screen.
- A popup on **either** machine stalls the shared clock (co-op only advances while
  both players are on the geoscape at the same speed). Drain both sides - but
  never dismiss `ConfirmLandingState`: `dismiss_popup`/`geo_run` auto-decline it.
- New TestServer hooks go in `TestServer::executeJoint10`; the old `execute`
  if/else chain is at MSVC's 128-block nesting limit (C1061).

Full suite (serial) is ~20 min; no test exceeds ~2 min. Known flakes, retry once:
`test_ufo_notice`, `test_joint_manufacture`, `test_joint_commerce`,
`test_joint_disconnect`, `test_joint_resync`.

`session.py` is the shared campaign dance (`new_campaign` / `resume_campaign`
/ `assert_client_zero_disk`) used by every test; `joint_fixture.py` builds the
JOINT bring-up + world equality on top of it.

`harness.py` is the shared library (not a test).

## Command catalog (TestServer::execute)

- Introspection: `ping`, `get_state`, `get_coop`, `get_soldiers`,
  `get_mirror_soldiers`, `has_coop_file`, `coop_stats`, `set_option`.
- Session flow: `load_save`, `load_save_menu` (real LoadGameState routing),
  `save_game`, `save_game_ui` (through the real SaveGameState funnel: `type` =
  `quick` | `auto_geoscape`), `open_new_game` (`mode`: `solo` | `coop`),
  `newgame_ok`, `place_first_base`, `profile_ok`, `host_tcp`, `join_tcp`,
  `lobby_state`, `lobby_start_campaign`, `lobby_resume_campaign`,
  `coop_dialog_back`, `save_markers`, `lobby_ready` (legacy),
  `client_reload_progress`, `quit`.
- Gift / UI: `gift` (change a soldier's owner), `gift_targets`,
  `open_gift_dialog`, `rename_soldier`, `open_soldiers`, `soldiers_ok`,
  `soldiers_inventory` (right-click-equip a base's soldiers), `inventory_ground`
  (read the open inventory's ground pane: `items`=ground, `carried`=on units,
  `all`=every instance incl. loaded ammo, `soldiers`=per-unit per-slot loadout
  with x/y and loaded ammo - use this to compare two equip screens item for
  item), `open_craft_equipment` / `craft_inventory` / `craft_equipment_ok` (the
  craft-side twins of open_soldiers/soldiers_inventory/soldiers_ok: the real
  `Bases > Equip craft > <craft> > Equipment > Inventory` path; select the base
  with `base`=name or `coop:true`, and the craft with `craft_id`), `base_report`
  (storage + `crafts` + per-soldier reserved layout items; `coop:true` picks the
  visited base), `give_layout` (reserve an `item` on a base's soldiers, optional
  `slot`=belt|right|left|backpack, `qty` entries per soldier, and a `name`
  filter), `inventory_move` (drag one item inside an OPEN inventory screen via
  the real `Inventory::fitItem`/`moveItem`, which is what writes the craft's
  `coopItems` manifest: `name`=soldier, `item`, `slot`=right|left|backpack|belt|
  ground, `from`=ground|unit; reports `landedSlot`/`landedOnUnit` because
  `moveItem` silently no-ops under several co-op guards),
  `set_coop_base` (force a soldier's coopBase, e.g. to make it a
  stripped guest), `transfer_to_coop_base` (vanilla base->base transfer of a
  soldier to a peer base, no ownership change), `incoming_transfers` (a base's
  pending incoming item/soldier transfers), `visit_coop_base`, `leave_base`,
  `cancel_dialog`, `show_notice`, `get_notices`, `dismiss_notice`,
  `get_palettes`.
- Geoscape: `geo_state`, `geo_set_speed`, `dismiss_popup`, `craft_dispatch`,
  `confirm_landing`, `craft_order` (`target`/`return`/`patrol`), `intercept_list`.
- Dogfights (shared JOINT): `dogfight_state` (per-open-fight introspection on each
  machine: `craftId/ufoId/currentDist/targetDist/mode/ufoIsAttacking/minimized/
  ended/isReplicaView/epoch/ufoStance/weaponEnabled[]/projectileCount`, plus the
  machine's membership `epoch`), `dogfight_action` (drive a fight's stance / weapon
  toggle / minimize / disengage; targets a specific `(craft_id,ufo_id)` fight or
  the first live one), `spawn_ufo`, `set_ufo_damage`, `set_ufo_hunt` (make a UFO an
  HK hunting a craft), `assign_crew` (seat a soldier on a craft, call on both), and
  `award_dogfight_xp` (HOST-only: award deterministic dogfight XP to a live fight's
  crew, for the GAP-7 propagation test).
- Battlescape: `close_briefing`, `battle_inventory`, `battle_state`,
  `battle_action` (`select` / `move` / `shoot` / `end_turn` / `abort`).
- Server browser: `open_server_browser`, `server_combo`, `combo_open`,
  `screenshot`.

Add a command by extending `TestServer::execute`. Add a test by composing
`GameClient` calls and asserts in a new `test_*.py`.
