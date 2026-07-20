# JOINT Campaign — Playtest Bug Board

Bugs found playtesting the joint-bases campaign mode. Workflow per bug:
**reproduce in automated harness test → investigate → fix → validate test green.**

Harness: `python tools/coop_test/test_joint_*.py` (in-game TestServer, 2 clients).
Exe: `bin/x64/Release/OpenXcom.exe`. See [[coop-test-harness]].

Status legend: ☐ todo · ◐ in progress · ☑ done (test green)

---

## Round 2 (2026-07-20, second playtest pass) — all ☑

- **R2-1 soldiers still co-owned** ☑ — the B4 creation split didn't migrate an
  EXISTING save (owner 999 → co-owned). Added `SavedGame::migrateJointSoldierOwnership`
  (split 999 by id-parity) on both load paths + creation. Validated END-TO-END:
  `test_joint_soldier_ownership_battle.py` (bootstrap owners → battle `_coop` split on
  both machines) + on-load migration (force 999 → reload → re-split). Commit 754c76e60.
- **R2-2 craft-arrived alerts / dogfight window not shown to all** ☑ — scrapped the
  last-commander gating. All seats get the landing prompt + the dogfight window (full);
  host stays battle authority; any seat answers (first-wins, `land_close` closes the
  rest). `test_joint_landing.py` rewritten to all-seats; dogfight full-for-all in
  `test_joint_dogfight_shared.py`. Commits 62c9fded1, 2f8457d21.
- **R2-3 dogfight window broken for clients (buttons revert)** ☑ — replica buttons are
  now HOST-AUTHORITATIVE (no radio group / no optimistic echo → no revert, no highlight
  desync); window opens full for all. Extensive `test_joint_dogfight_shared.py` (host-
  & client-commanded, host-sync, client cmds stick in host order, conflicts, invariant
  highlight==mode). Commit 2f8457d21.
- **R2-4 coop menu shows wrong host name** ☑ — roster used machine-relative
  `getHostName()` for the host row; now role-relative via `getServerOwner()`.
  `test_joint_ingame_coop_menu.py` asserts host row=HostPlayer on both. Commit 2f8457d21.

---

## B1 — Client facility build not reflected until menu exit ☑
Client builds facility in shared base → construction + debited funds NOT shown
until leaving the "build facilities" menu. Needs live refresh of the open
Basescape/PlaceFacility view on `joint_apply`.
- Repro: extend `test_joint_facilities.py` — after `fac_build` apply lands,
  assert the client's OPEN base view reflects facility + funds without re-query/exit.
- Files: `src/Basescape/PlaceFacilityState.*`, `BasescapeState.*`, `BaseView.*`.

## B2 — Research list not live-synced (double-start race) ☑
"NEW RESEARCH PROJECTS" window must live-sync so two players can't start the
same project → `STR_RESEARCH_NOT_AVAILABLE`. Worse: a client still sees research
it JUST started in the available list, inviting a re-start.
- Repro: extend `test_joint_research.py` — after client starts project T, the
  NewResearchList on BOTH machines must drop T live (no exit/reopen); a second
  start of T must be impossible/no-op, not an error.
- Files: `src/Basescape/NewResearchListState.*`, `ResearchState.*`, `ResearchInfoState.*`.

## B3 — Soldier renames not synchronized ☑
Renaming a soldier is local-only. Must ride joint_cmd like base_rename.
- Repro: `test_joint_soldier_rename.py` (new) — client renames soldier by id →
  both worlds show new name.
- Files: `src/Basescape/SoldierInfoState.*` (name box), J03 joint_cmd protocol.

## B4 — Both players co-own all soldiers (breaks battlescape) ☑
Ownership must match SEPARATE mode, EXCEPT the base's initial starting soldiers
are split evenly between the two players at bootstrap.
- Repro: `test_joint_soldier_ownership.py` (new) — after bootstrap, each soldier
  has exactly one owner; initial roster split ~50/50; battlescape sees correct owner.
- Files: J02 world bootstrap, soldier owner tagging, `test_joint_battle.py`.

## B5 — Only host gets new UFO alerts ☑
UFO-detected alert popup fires host-only; all players must get it.
- Repro: extend `test_ufo_notice.py` / new joint variant — spawn UFO, assert
  BOTH host and client raise the alert.
- Files: `src/Geoscape/GeoscapeState.cpp` (UFO detection popup), joint broadcast.

## B6 — Shared dogfight buttons not auto-synced ☑
Dogfight menu buttons only sync when the OTHER player presses one, which then
desyncs (multiple buttons active at once). Button STATE must auto-replicate.
- Repro: extend `test_joint_dogfight_control.py` — player A toggles stance;
  player B's UI reflects it live; no two-active-button desync.
- Files: `src/Geoscape/DogfightState.*`, df_cmd protocol (PRD-DF02).

## B7 — In-game coop menu is a dead end (no Resume) ☑
Opening coop menu while connected offers only Disconnect. Must show "Resume game"
so players can return to play.
- Repro: `test_lobby_polish.py` / new — while connected, coop menu exposes a
  working resume path back to geoscape.
- Files: coop menu state (in-game), `src/Menu/` coop states.

---

### Log
- 2026-07-20: board created; harness confirmed runnable (exe built today).
- 2026-07-20: ALL 7 FIXED on branch `fix/joint-playtest-bugs`. Each was reproduced
  in a new automated harness test (red), fixed, and re-validated (green); the fix
  was then temporarily disabled + rebuilt to prove the test actually catches it.
  New tests: test_joint_facility_refresh / _research_refresh / _soldier_rename /
  _soldier_ownership / _ufo_alert / _dogfight_highlight / _ingame_coop_menu.
  Regressions green: facilities, research, deploy, battle(mixed+solo), dogfight
  control, ufo_notice(SEPARATE), lobby_polish, world_equal, refresh, purchase.
  Commits (7): B1 254559f64, B2 f43b178db, B3 4d103de91, B4 7fcf16ab9,
  B5 99934578e, B6 49d50bc75, B7 664a50cba.
