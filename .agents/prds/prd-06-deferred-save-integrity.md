# PRD-06: Deferred host save — never write a different world over a named save; write once, not twice

**Fixes:** C5 (CONFIRMED: deferred re-save can serialize the WRONG world over
the user's named save), E1 (CONFIRMED: every host save cycle serializes and
writes the full .sav twice).

**Files:** `src/Menu/SaveGameState.cpp`, `src/CoopMod/connectionTCP.cpp`,
`src/CoopMod/CoopState.cpp`, `src/Menu/LoadGameState.cpp`,
`src/CoopMod/TestServer.cpp`, `tools/coop_test/`.

## Verified mechanics (current behavior)

1. Host saves via the funnel: `SaveGameState::think()`
   (`SaveGameState.cpp:220-233`) sets
   `session.pendingHostSaveName = _filename`, pushes the "Saving..." wait
   dialog (CoopState 54 / `COOP_DLG_HOST_SAVE_WAIT` after PRD-01), then
   **unconditionally** runs the full `save(backup)` + `moveFile` immediately.
2. When the client's progress blob arrives, the `MAP_RESULT_SAVE_PROGRESS`
   handler (`connectionTCP.cpp:6754-6800`) repeats the **identical full
   save + rename** over `pendingHostSaveName`, then clears the name (`:6799`).
   Guards: non-null SavedGame, non-empty name, no live battle (`:6784`). **No
   check that the currently loaded world is still the one that initiated the
   save.**
3. `pendingHostSaveName` is cleared only by that handler, `resetSession()`
   (`connectionTCP.cpp:273`), and `onClientDrop()` (`:283`). **No load path
   clears it.**
4. The wait dialog's CANCEL falls through to a bare `popState()`
   (`CoopState.cpp:854-890` — state 54 absent from both special-case lists),
   leaving `pendingHostSaveName` set.
5. The client defers its blob send until it leaves a base view
   (`connectionTCP.cpp:8511-8523`) — the round-trip window can be long.

**C5 failure:** host saves as `alpha.sav` → cancels the wait dialog (or just
waits) → quickloads `beta.sav` → client blob arrives late → handler serializes
the **beta** world over `alpha.sav`. Named save silently replaced with a
different campaign state.

**E1 cost:** two full YAML emits + two base64 encodes of every embedded
multi-MB blob + two disk writes per save, on the main thread.

## Required behavior

1. A completed save cycle writes `pendingHostSaveName` **exactly once**, with
   the freshest client blob available.
2. Any world switch (loading any save, quickload, returning to menu) aborts a
   pending deferred save.
3. CANCEL on the wait dialog produces a valid save **now** (with last-known
   blobs) instead of leaving a time bomb armed.

## Implementation plan

1. **Single write.** In `SaveGameState::think()`: when the deferred round-trip
   is armed (same condition that currently sets `pendingHostSaveName` — a live
   campaign session with an attached client), **skip the immediate
   save+moveFile**; the `MAP_RESULT_SAVE_PROGRESS` handler performs the one
   write. When the round-trip is NOT armed (no session / no client), keep the
   immediate write and never set `pendingHostSaveName`. Read the surrounding
   code first: the backup/moveFile dance and ironman/error handling must move
   intact into whichever branch performs the write.
2. **CANCEL = save now.** Give the wait dialog's cancel path explicit handling
   (in `CoopState::previous` or a dedicated handler for
   `COOP_DLG_HOST_SAVE_WAIT`): perform the save+moveFile immediately with
   whatever blobs are in the store, clear `pendingHostSaveName`, pop. The user
   asked for a save; they get one — just possibly with a slightly stale client
   blob (same guarantee autosaves already have).
3. **Abort on world switch.** Clear `pendingHostSaveName` when a different
   world is loaded. Chokepoint: the live-session load path runs
   `resetTransferSessionState()` (`connectionTCP.cpp:1220-1228`, called from
   `LoadGameState.cpp:185-191`) — add the clear there, and verify BOTH load
   funnels (menu load and quickload; quickload pushes LoadGameState via the
   quick variant — confirm by reading `GeoscapeState`'s quickload handler) hit
   it. If any load path bypasses `resetTransferSessionState`, add the clear at
   that path too. Log a line when an armed deferred save is aborted.
4. **Belt-and-braces staleness guard.** In the `MAP_RESULT_SAVE_PROGRESS`
   handler, before writing: if `pendingHostSaveName` is empty → skip silently
   (already the case); this plus step 3 is sufficient. Do NOT try to compare
   world pointers.
5. **Introspection for tests.** Expose `pendingHostSaveName` in the TestServer
   `get_coop` (or `coop_stats`) reply.

## Regression tests

Extend `test_session_hardening.py` (or a new `test_deferred_save.py`):

- **Abort-on-load:** live session; host `save_game_ui` (arms deferral, dialog
  54 up); host loads a different save via `load_save_menu` before the client
  leaves its base view; assert `get_coop` shows `pendingHostSaveName == ""`;
  then let the client move; assert the originally named .sav file's mtime/size
  did NOT change after the load (the late blob must not rewrite it).
- **Cancel writes:** host `save_game_ui`, cancel the dialog
  (`coop_dialog_back`), assert the named .sav exists on disk and
  `pendingHostSaveName` is cleared.
- **Single write:** normal completed cycle still produces the save —
  `test_client_zero_disk.py` already drives the full funnel; it must stay
  green.

## Acceptance criteria

- Build + `boot_check.py`; full suite green (`test_client_zero_disk.py` is the
  critical one — it drives aggressive autosaves + host/client saves through
  the real funnel).
- New scenarios above pass.

## Out of scope

- Blob validation ordering (PRD-07). Host quickload policy (PRD-08) — note
  PRD-08 may block host mid-session loads entirely, which shrinks this bug's
  window further; implement this PRD anyway (menu loads and freeze-state loads
  remain).
