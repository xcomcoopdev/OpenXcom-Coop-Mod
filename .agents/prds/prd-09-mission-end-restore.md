# PRD-09: Mission-end world restore must survive a process restart

**Fixes:** C12 (CONFIRMED: host resuming a coop BATTLE save in a fresh process,
where the client battle-hosts, ends the mission holding the wrong world).

**Files:** `src/Geoscape/GeoscapeState.cpp`, `src/CoopMod/connectionTCP.cpp`,
`src/CoopMod/CoopState.cpp`, `src/Menu/LoadGameState.cpp`.

## Verified mechanics (current behavior)

- On main, the server-owner's mission-end restore read a disk file
  (`coop_geoscape_<saveID>_<host>.sav`, written at mission sync) with
  `_autogeo_.asav` fallback. The branch deleted both disk writers and replaced
  the restore chain (`GeoscapeState.cpp:995-1012`) with, in order:
  1. RAM key `coop_geoscape_return`
  2. RAM key `basehost` (world snapshot from session START)
  3. keep the live in-memory world (which at that point is the streamed,
     client-derived battle world).
- `coop_geoscape_return` is written in exactly one place: the
  `SEND_FILE_CLIENT_TRUE` mission-START handler (`connectionTCP.cpp:8125`).
- `SavedGame::save` embeds only `host_*`-prefixed client blobs
  (`SavedGame.cpp:1516`; restore filter `:817-829`) — so neither
  `coop_geoscape_return` nor `basehost` survives into a fresh process.
- The F3 battle-resume flow regenerates **neither**: phase 1
  (`request_load_progress`, `connectionTCP.cpp:3235-3284`) streams the
  embedded client blob and writes nothing; phase 2
  (`SEND_FILE_CLIENT_SAVE` → CoopState(666)) writes only `battlehost`
  (`CoopState.cpp:1157-1161`); `sendBaseFile` refuses to write `basehost`
  while a battle is loaded (`connectionTCP.cpp:8266`).
- With `coop_save_owner_player_id == 1` (client battle-hosts), the resumed
  server owner takes `setHost(false)` (`BattlescapeState.cpp:1680-1687`) and
  DOES enter this restore path at mission end — hitting the line-1007 guard
  and keeping the client-derived battle world instead of its own campaign
  geoscape. This is the designed quit-and-resume flow (F3), not a crash edge.

## Required behavior

After ANY battle resume — same process or fresh process — the server owner's
mission-end restore has a correct "own geoscape world" source: research,
funds, purchases from before the battle are preserved; the streamed peer world
is never kept as the host's campaign.

## Implementation plan (RAM-only, consistent with the branch design)

1. **Re-stash on battle-resume load.** In the F3 battle-resume path, right
   after the host's own `.sav` finishes loading and BEFORE any battle handoff
   mutates the world (find the spot in `LoadGameState`'s F3 branch /
   the resume orchestration around `LoadGameState.cpp:404-418` — the moment
   the loaded SavedGame is fully constructed), serialize the current
   SavedGame into the `coop_geoscape_return` RAM slot via the same
   `saveCoopToMemory` call the mission-START handler uses
   (`connectionTCP.cpp:8125` — copy its exact key and mechanics).
   Semantics match: the battle save's geoscape portion IS the host's
   pre-battle-continuation geoscape; restoring it at mission end and letting
   Debriefing apply results is exactly what the mission-START stash does.
2. **Guard the degenerate fallback.** In the restore chain
   (`GeoscapeState.cpp:995-1012`):
   - falling back from `coop_geoscape_return` to `basehost` must log a
     warning (`Log(LOG_WARNING)`) — it silently rolls the campaign back to
     session start;
   - the final "keep live world" branch must log an error — it means the host
     keeps a peer-derived world. These logs make any future gap visible in
     harness logs instead of silently corrupting campaigns.
3. **Audit other resume entries.** Grep for every code path that can put the
   server owner into a battle where `setHost(false)` applies
   (`coop_save_owner_player_id` readers) and confirm each has a
   `coop_geoscape_return` stash: (a) live mission start — has it
   (`:8125`); (b) F3 battle resume — added in step 1; (c) mid-battle direct
   rejoin (`test_rejoin_flow` path) — check and stash if missing.

## Regression tests

Battle flows are the least automated part of the harness (geoscape/battle
drivers exist: `battle_state`, `battle_action`, `close_briefing`). If
feasible, add `test_battle_resume_fresh.py`: campaign dance → dispatch craft →
enter battle (client battle-hosts) → host `save_game_ui` → kill BOTH
instances → fresh host+client → resume via lobby → finish/abort battle
(`battle_action abort`) → assert host geoscape world is its own campaign
(e.g. base name/roster from `get_soldiers` matches pre-battle host base, and
funds via a `geo_state`-style introspection differ from the session-start
snapshot). If the battle driver can't reach mission end reliably, implement
steps 1-3, keep the suite green, and record a MANUAL TEST checklist in the
commit body (the flow above, executed by hand) — flag it for the maintainer.

## Acceptance criteria

- Build + `boot_check.py`; full suite green (`test_rejoin_flow.py`,
  `test_resume_flow.py` most relevant).
- The two new log lines exist and fire only on the degenerate paths.
- Commit body: audit table from step 3 (entry point → stash present).

## Out of scope

- Restoring the deleted disk-file fallbacks (design decision already made:
  RAM-only + host .sav is the single artifact); changing Debriefing.
