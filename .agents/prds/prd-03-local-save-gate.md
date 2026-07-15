# PRD-03: One predicate for "may this machine touch local saves", gate LoadGameState, fix PauseState buttons

**Fixes:** S9 (local-saves rule copy-pasted into 3 engine states + divergent 4th;
LoadGameState unguarded; PauseState visibility logic re-exposes Load/Save to a
coop client).

**Files:** `src/CoopMod/connectionTCP.cpp/.h`, `src/Battlescape/BattlescapeState.cpp`,
`src/Geoscape/GeoscapeState.cpp`, `src/Menu/SaveGameState.cpp`,
`src/Menu/LoadGameState.cpp`, `src/Menu/PauseState.cpp`.

## Verified defects

1. The "coop client must not touch local saves" predicate is copy-pasted
   verbatim at `BattlescapeState.cpp:5307` (quickload) and
   `GeoscapeState.cpp:772` (quickload) — both literally commented
   "same predicate as the SaveGameState client gate" — with its De Morgan
   complement at `SaveGameState.cpp:192` (the swallow gate), plus a **fourth,
   divergent** variant at `PauseState.cpp:226`
   (`getCoopStatic()==false && isCoopSession()==true` → error popup).
2. `LoadGameState` itself has **no** gate (its lines 224/393 are lobbyMode
   resume-ack checks, not save gates). All save-side entry points funnel into
   the SaveGameState swallow, so saving is covered — loading is not.
3. Unguarded load path: `PauseState::btnLoadClick` (`PauseState.cpp:213`) →
   `ListLoadState.cpp:106` → `ConfirmLoadState.cpp:91` push `LoadGameState`
   with no predicate. It is "protected" only by button visibility whose logic
   is contradictory: `PauseState.cpp:149/154` hide Load, then line 181 and
   `init()` line 276 **re-show Load+Save** whenever
   `isCoopSession()==false && getServerOwner()==false` — which re-exposes local
   load to a coop-static client (a state where `getCoopStatic()` is true but
   `isCoopSession()` momentarily false).

## Required behavior

- Exactly one function answers "may this machine read/write local saves right
  now"; every gate and every button-visibility decision calls it.
- A coop client cannot reach a local-disk load through ANY path: quickload
  keys, pause menu, list/confirm states, or a directly pushed LoadGameState.
- Host behavior is unchanged by this PRD (host quickload policy changes later
  in PRD-08 — keep the predicate factored so PRD-08 edits one function).

## Implementation plan

1. Read the three identical predicate sites and confirm the common form (it is
   built from `getCoopStatic()` / `getServerOwner()` / session checks). Extract
   it verbatim into `connectionTCP`:
   ```cpp
   // single authority: may this machine use local .sav files right now?
   static bool localSavesAllowed();
   ```
   Add a short comment enumerating the intended truth table (solo play: yes;
   coop host: yes; coop client: no).
2. Replace the predicate at `BattlescapeState.cpp:5307`,
   `GeoscapeState.cpp:772`, `SaveGameState.cpp:192` (mind the negation), and
   `PauseState.cpp:226` (keep its error-popup UX, key it off
   `!localSavesAllowed()`).
3. **Gate LoadGameState at the chokepoint.** In `LoadGameState`'s
   init/think entry (mirror how `SaveGameState.cpp:192` swallows): if
   `!localSavesAllowed()` and this LoadGameState was reached as a plain local
   load (i.e. NOT one of the coop-orchestrated flows already special-cased in
   the file — read `LoadGameState.cpp:404-418`, the F3 resume interposition,
   before writing this condition), pop self immediately. This makes
   ListLoadState/ConfirmLoadState safe without touching them.
4. **Fix PauseState visibility.** Make Load/Save visibility a single
   assignment derived from `localSavesAllowed()` (Save additionally per its
   existing host rules), applied consistently at construction AND `init()`
   (lines 149/154/181/276). Delete the contradictory re-show branch.
5. Search for other pushers of `LoadGameState` (`grep -n "new LoadGameState"
   src/`) and confirm each is either host-side, solo, or a coop-orchestrated
   flow. List them in the commit body.

## Acceptance criteria

- Build + `boot_check.py`.
- `python tools/coop_test/test_session_hardening.py` (contains the existing
  save/load gate scenarios) passes.
- `python tools/coop_test/test_client_zero_disk.py` passes — this is the
  standing invariant test for "client writes zero save data".
- `python tools/coop_test/test_resume_flow.py` passes (proves the F3 load
  interposition still works — the new gate must not swallow it).
- New targeted check: extend `test_session_hardening.py` — with a live
  session, drive the client's pause menu (`get_state`/menu commands) and
  assert the Load path refuses (top state does not become LoadGameState, or
  the swallow returns to pause) for the client.

## Out of scope

- Changing host quickload policy (PRD-08). Do not alter what the predicate
  returns for the host.
