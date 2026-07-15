# PRD-08: Host mid-session load policy — block it (no silent world fork)

**Fixes:** C7 (CONFIRMED: host quickload mid-live-session rolls back the served
world, the client is never told, and the client's next progress push clobbers
the rolled-back blob → permanent divergence).

**Files:** `src/Geoscape/GeoscapeState.cpp`, `src/Battlescape/BattlescapeState.cpp`,
`src/CoopMod/connectionTCP.cpp` (the `localSavesAllowed` predicate from PRD-03),
`src/Menu/LoadGameState.cpp`, `tools/coop_test/test_session_hardening.py`.

## Verified mechanics (current behavior)

- The quickload gate at `GeoscapeState.cpp:772` contains
  `... || getServerOwner() == true` — the host may quickload (F9) with a live
  client attached.
- `SavedGame::load` then: overwrites `connectionTCP::saveID`
  (`SavedGame.cpp:799`), erases all `host_*` blobs and restores the
  quicksave's embedded ones (`:808-830`).
- Nothing informs or disconnects the attached client: the live-session load
  path runs only `s->load()` + `resetTransferSessionState()`
  (`LoadGameState.cpp:185-191`; `resetTransferSessionState` clears transfer
  bookkeeping only, `connectionTCP.cpp:1220-1228`). The F3 lobby-regate branch
  is explicitly skipped during a live session (`LoadGameState.cpp:404-418`,
  `getCoopStatic() == false` condition).
- The client keeps playing its live (now-future) world; its next progress push
  is stored via `writeHostMapSaveProgressFile` under the restored saveID's key
  (`connectionTCP.cpp:9534-9546`), and even on a saveID mismatch the embed
  picks the newest blob per client (`SavedGame.cpp:1504-1539`) — the host's
  rolled-back save absorbs the client's future world either way.

## Decision (made): block, don't resync

Two candidate policies were considered:
- **(a) Block** host mid-session local loads, same refusal UX the client gets.
- (b) Allow + full resync (rebroadcast campaign state + re-serve blobs).

**(a) is chosen.** (b) is a large protocol feature with its own failure modes;
(a) closes a corruption path with a few lines and matches the branch's design
philosophy (the lobby/freeze flows are the sanctioned way to change worlds).
The host can still: save any time (funnel), and load after ending the session
(disconnect/freeze paths land outside a live session, where loads are
allowed). If real play finds a need for mid-session rollback, implement (b)
later behind the lobby (host loads → session re-gates through the resume
lobby).

## Implementation plan

1. In the PRD-03 predicate home (`connectionTCP`): split load policy from save
   policy if not already split:
   ```cpp
   static bool localSavesAllowed();  // may this machine WRITE local saves (host: yes, client: no)
   static bool localLoadsAllowed();  // may this machine LOAD local saves NOW (live session: NO for everyone)
   ```
   `localLoadsAllowed()` = false whenever a live coop session is attached
   (host or client), true otherwise. "Live session" = the same condition the
   quickload gates already probe (`getCoopStatic()` / session attached) — read
   the existing gate's terms and reuse them; do not invent a new liveness
   definition.
2. Point the quickload gates (`GeoscapeState.cpp:772`,
   `BattlescapeState.cpp:5307`) and the LoadGameState chokepoint gate
   (from PRD-03) at `localLoadsAllowed()`. Remove the
   `|| getServerOwner() == true` escape. Show the same refusal popup the
   client currently gets (reuse that string/state — grep for the client
   refusal the existing gate raises).
3. PauseState Load button visibility (PRD-03's single assignment) now derives
   from `localLoadsAllowed()`.
4. Confirm the F3 resume interposition still works: it runs when NO live
   session exists, so `localLoadsAllowed()` is true there. Run
   `test_resume_flow.py` to prove it.

## Regression tests

- `test_session_hardening.py` currently asserts the HOST quickload path is
  permitted (it was a deliberate branch behavior). Find that assertion and
  flip it: host quickload during a live session must now refuse (top state
  unchanged / refusal popup present). Client-side assertions stay as-is.
- Add: host attempts `load_save_menu` mid-session → refused; after
  disconnect → allowed.

## Acceptance criteria

- Build + `boot_check.py`; full suite green with the updated expectations;
  `test_resume_flow.py` and `test_rejoin_flow.py` untouched and green.
- Commit body records the policy decision and the alternative (b) for the
  future.

## Out of scope

- Implementing resync/rebroadcast; touching autosave behavior.
