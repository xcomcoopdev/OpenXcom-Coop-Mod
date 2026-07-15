# PRD-04: Session identity must reset — saveID and blob maps into the session lifecycle

**Fixes (all CONFIRMED, highest severity in the review):**
- **C1** — solo saves written after any coop session are permanently unloadable.
- **C2** — a second coop campaign in the same process reuses the first
  campaign's saveID and serves its stale client world.
- **C3** — an ex-campaign client joining a plain New Battle host gets its
  current game destroyed.

**Files:** `src/CoopMod/connectionTCP.cpp/.h`, `src/Savegame/SavedGame.cpp`,
`src/CoopMod/LobbyMenu.cpp`, `src/CoopMod/Profile.cpp` (read-only check),
`src/Menu/NewGameState.cpp` (read-only check), `tools/coop_test/`.

## Root cause (verified facts — trust these, re-verify anchors)

- `connectionTCP::saveID` (`connectionTCP.cpp:316`) has **exactly one
  zero-write in the entire codebase: its static initializer.** Every other
  write assigns nonzero: `SaveGameState.cpp:215`, `SavedGame.cpp:340/799`
  (`tryRead`), `connectionTCP.cpp:2626/3294/6918/7143`.
- `CoopSession::resetSession` (`connectionTCP.cpp:261-274`), `onClientDrop`
  (`276-292`), `resetCoopState` (`1890-1916`), `disconnectTCP`, and
  `MainMenuState.cpp:344` (calls resetSession) — **none touch `saveID` or the
  blob maps.**
- `coopFilesHost` has no `.clear()` anywhere. Only erasures:
  `SavedGame::load`'s `host_` prefix wipe (`SavedGame.cpp:810-816`, runs only
  inside a disk `load()`) and `eraseStaleBlobEntries` (same-client older-ID
  prune at store time).
- `SavedGame::save` writes `saveID` **unconditionally** (`SavedGame.cpp:1495`)
  but writes the `coop` header marker only `if (_coop)` (`:1482`). Solo
  NewGameState sets `_coop` false (`NewGameState.cpp:181-184`).
- Load-time gate (`SavedGame.cpp:782-789`): `!_coop && oldSaveID != 0` →
  throws `"This save was made on an earlier version of the X-COM Co-op Mod"`.
- Embed guard (`SavedGame.cpp:1504`): `saveID != 0 && !.data` → embeds any
  non-empty `host_*` blob into the save being written (`:1513-1541`).
- `tryRead("saveID", ...)` on a save file **without** a `saveID` key leaves the
  stale global untouched.

### Failure paths (what these facts produce)

- **C1:** coop session → saveID nonzero forever → later solo save carries
  nonzero saveID + no `coop` marker (+ leftover foreign `host_*` blobs if the
  user was the host) → load gate throws → save permanently unloadable.
- **C2:** `connectionTCP.cpp:7141-7144` regenerates saveID **only when it is
  0**; nothing in a second campaign flow regenerates it or clears
  `coopFilesHost` → campaign B reuses A's ID → stale `host_<id>_<client>.data`
  satisfies `hasCoopFile(hostBlobKey(...))` (`CoopState.cpp:766`, the state-60
  wait shows "All players connected" before the new client placed a base),
  gets streamed by `request_load_progress` (`connectionTCP.cpp:3250-3284`),
  and is embedded into campaign B's `_autogeo_.asav` (`LobbyMenu.cpp:580` →
  `SavedGame.cpp:1504-1541`).
- **C3:** main gated Profile's world-fetch on `_host_save_progress` (reset on
  disconnect in main's `disconnectTCP`); the branch's `Profile.cpp:111` gate is
  `saveID != 0` — never reset. Ex-campaign client joins a New Battle host →
  fires `request_load_progress` → host no-blob branch replies `campaign_start`
  → client handler (`connectionTCP.cpp:2621-2666`) unconditionally
  `_game->setSavedGame(save)` (`:2640`) + `new GeoscapeState` (`:2645`) →
  current game destroyed into bogus base placement.

## Required behavior

1. Ending a coop session (disconnect/teardown that calls `resetSession`)
   returns the process to a pristine identity: `saveID == 0`, both blob maps
   empty.
2. Starting a **new** coop campaign always mints a fresh saveID; resuming keeps
   the loaded one.
3. Loading a solo save (no `saveID` key, or `saveID: 0`) leaves the global at 0.
4. A solo save written in any process state contains no `saveID` (or 0) and no
   embedded blobs.
5. With 1-3 in place, `Profile.cpp:111`'s `saveID != 0` gate is correct again —
   no change needed there (verify, don't edit).

## Implementation plan

1. **Reset in the session owner.** In `CoopSession::resetSession()`
   (`connectionTCP.cpp:261-274`) additionally:
   - `connectionTCP::saveID = 0;`
   - clear `coopFilesHost` and `coopFilesClient` **under `coopFilesMutex`**.
   Check every `resetSession()` caller first (`grep -n "resetSession"`): the
   mid-session freeze path must NOT call it (freeze keeps the session alive
   for rejoin — verify the freeze branch in `disconnectTCP` does not call
   resetSession; the review confirmed the freeze branch only pushes
   CoopState(64)). `onClientDrop` must NOT clear (host keeps serving blobs for
   rejoin).
2. **Solo save must not embed or carry coop identity.** In `SavedGame::save`:
   - write `saveID` only `if (_coop)` (same condition as the `coop` marker at
     `:1482`);
   - add `_coop &&` to the embed guard at `:1504`.
3. **Solo/legacy load must zero the global.** In `SavedGame::load`'s disk path
   (the region around the `oldSaveID` gate at `:782-799`): default the global
   to 0 before `tryRead("saveID", ...)` so a save without the key resets it.
   Do NOT touch the in-memory coop blob load path (`loadCoopSaveFromMemory`,
   guard at `:340-343`) — only the disk-file path.
4. **Fresh ID per new campaign.** In `LobbyMenu::startCampaign()`
   (`LobbyMenu.cpp:562-598`): unconditionally regenerate saveID (the datetime
   generator used at `connectionTCP.cpp:2502`/`7141`) before building the
   campaign_start packet. Leave `resumeCampaign()` using the loaded ID. Then
   audit `connectionTCP.cpp:7141-7144` (the `if (saveID == 0)` regenerate at
   join-time): with startCampaign minting IDs, decide whether that site is
   still needed for the lobby-join handshake; keep it as a fallback but add a
   comment. Starting a new campaign should also not see stale blobs — but with
   step 1 clearing on session end and a fresh ID here, stale keys can no longer
   collide. Belt-and-braces: at the top of `startCampaign()`, clear both blob
   maps under the mutex (a brand-new campaign has no world blobs yet by
   definition).
5. **Verify, don't change:** `Profile.cpp:111` gate; `NewGameState` solo path
   (needs no saveID write once 1-4 are in). State in the commit body why each
   is now correct.

## Regression tests (add to `tools/coop_test/`)

Extend `test_session_hardening.py` (it already has the session dance imports):

- **Solo-after-coop:** run the standard 2-instance campaign dance
  (`session.new_campaign`), disconnect both, then on the ex-host instance:
  `open_new_game` (`mode: solo`) → `newgame_ok` → place base → `save_game`
  (named slot) → `load_save` the same file. Assert the load succeeds (no
  "earlier version" refusal) and, via `has_coop_file`, that no `host_*` key
  survives in memory. On failure before this PRD, the load throws — this is
  the C1 repro.
- **Back-to-back campaigns:** after a full campaign dance + disconnect, start a
  second campaign on the same instances. Before the client places its base,
  assert the host's state-60 wait is still waiting (C2 repro: it used to show
  "All players connected" immediately via the stale blob). `save_markers` on
  each side must show different saveIDs across the two campaigns.

## Acceptance criteria

- Build + `boot_check.py`.
- Both new scenarios above pass.
- Full suite passes, especially `test_client_zero_disk.py`,
  `test_resume_flow.py` (resume must still adopt the LOADED saveID — the reset
  work must not break resume), `test_rejoin_flow.py` (freeze path must still
  serve blobs — proves step 1 didn't over-clear).

## Out of scope

- Restructuring the blob maps (PRD-05). Keep the flat maps; just clear them.
