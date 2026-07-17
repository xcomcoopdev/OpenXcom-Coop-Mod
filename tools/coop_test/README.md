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
- `test_coop_transferred_equipment.py` - vanilla-transferring an equipped
  soldier to a peer base must not duplicate its equipment: the world-wide item
  total is unchanged and the client-base equip screen shows the same number of
  item instances to the visiting host as to the client (conservation).
- `test_coop_transfer_equipment_option.py` - the "Alternate craft equipment
  management" option drives transfer behaviour: OFF strips the soldier's gear
  (empty layout at the peer base), ON keeps its loadout. Runs both modes.
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
| `test_joint_craft.py` | shared craft command + a client-simulated dogfight applied host-side |
| `test_joint_deploy.py`, `test_joint_battle.py` | mixed-owner squads: control split follows soldier ownership; post-battle worlds identical |
| `test_joint_landing.py` | the landing broker asks the seat that gave the order |
| `test_joint_refresh.py`, `test_joint_resync.py` | live screen refresh on apply; desync detect -> auto-repair |
| `test_joint_world_equal.py` | the equality helper itself, **including a negative control** |
| `test_joint_disconnect.py` | client killed with a command in flight -> no half-apply; rejoin restores one world |
| `test_joint_month_run.py` | the long run: 2 month ends + a battle in one campaign |

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

Full suite (43 tests, serial) is ~14 min; no test exceeds ~2 min. Known flakes,
retry once: `test_ufo_notice`, `test_joint_manufacture`, `test_joint_commerce`.

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
  `all`=every instance incl. loaded ammo), `base_report` (storage + per-soldier
  reserved layout items; `coop:true` picks the visited base), `give_layout`
  (reserve an `item` on a base's soldiers, optional `slot`=belt|right|left and
  `name` filter), `set_coop_base` (force a soldier's coopBase, e.g. to make it a
  stripped guest), `transfer_to_coop_base` (vanilla base->base transfer of a
  soldier to a peer base, no ownership change), `incoming_transfers` (a base's
  pending incoming item/soldier transfers), `visit_coop_base`, `leave_base`,
  `cancel_dialog`, `show_notice`, `get_notices`, `dismiss_notice`,
  `get_palettes`.
- Geoscape: `geo_state`, `geo_set_speed`, `dismiss_popup`, `craft_dispatch`,
  `confirm_landing`.
- Battlescape: `close_briefing`, `battle_inventory`, `battle_state`,
  `battle_action` (`select` / `move` / `shoot` / `end_turn` / `abort`).
- Server browser: `open_server_browser`, `server_combo`, `combo_open`,
  `screenshot`.

Add a command by extending `TestServer::execute`. Add a test by composing
`GameClient` calls and asserts in a new `test_*.py`.
