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
- `test_transfer_fresh.py` - soldier ownership transfer on a fresh campaign.
- `test_bug_fixes.py` - owner resolution, notice display, dialog flicker, etc.
  Exposes `bootstrap_fresh_session()` and `own_base()` reused by other tests.
- `test_transfer_rollback.py` - host-save-is-authority rollback, both directions.
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

`session.py` is the shared campaign dance (`new_campaign` / `resume_campaign`
/ `assert_client_zero_disk`) used by every test.

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
- Transfer / UI: `transfer`, `transfer_targets`, `rename_soldier`,
  `open_soldiers`, `visit_coop_base`, `open_transfer_dialog`, `cancel_dialog`,
  `show_notice`, `get_notices`, `dismiss_notice`, `get_palettes`.
- Geoscape: `geo_state`, `geo_set_speed`, `dismiss_popup`, `craft_dispatch`,
  `confirm_landing`.
- Battlescape: `close_briefing`, `battle_inventory`, `battle_state`,
  `battle_action` (`select` / `move` / `shoot` / `end_turn` / `abort`).
- Server browser: `open_server_browser`, `server_combo`, `combo_open`,
  `screenshot`.

Add a command by extending `TestServer::execute`. Add a test by composing
`GameClient` calls and asserts in a new `test_*.py`.
